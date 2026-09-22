/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidEncounterTriage.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Creature.h"
#include "Group.h"
#include "Pet.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RaidAddWavePolicy.h"
#include "RaidFearResponsePolicy.h"
#include "RaidFearSignal.h"
#include "RaidInterruptResponsePolicy.h"
#include "RaidPriorityAddPolicy.h"
#include "RaidTargetClaimValue.h"
#include "RtiTargetValue.h"
#include "Timer.h"
#include "ThreatManager.h"

namespace
{
using PolicyGuid = RaidPriorityAddPolicy::Guid;

struct RaidAddSnapshot
{
    std::vector<RaidPriorityAddPolicy::RaidMember> members;
    std::vector<RaidPriorityAddPolicy::AddCandidate> adds;
    std::map<PolicyGuid, Creature*> addUnits;
    std::map<PolicyGuid, Creature*> bossUnits;
    std::map<PolicyGuid, Player*> memberUnits;
    std::size_t reportedAddCount = 0;
    bool bossEngaged = false;
    bool bossHasVictim = false;
};

struct PriorityAddSelection
{
    Creature* unit = nullptr;
    PolicyGuid targetGuid = 0;
    RaidPriorityAddPolicy::AssignmentKind kind = RaidPriorityAddPolicy::AssignmentKind::DpsFocus;
    std::uint8_t interruptRank = 0;
    bool allowSameOwnerRetarget = true;
};

struct RaidAddWaveEvaluation
{
    RaidAddSnapshot snapshot;
    RaidPriorityAddPolicy::Plan plan;
    RaidAddWavePolicy::Memory previous;
    RaidAddWavePolicy::Decision wave;
};

std::vector<Player*> GetAliveRaidMembers(Player* bot)
{
    std::vector<Player*> members;
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
    {
        if (bot && bot->IsAlive() && bot->IsInWorld())
            members.push_back(bot);
        return members;
    }

    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId() ||
            member->GetInstanceId() != bot->GetInstanceId())
        {
            continue;
        }
        members.push_back(member);
    }

    std::sort(members.begin(), members.end(),
        [](Player const* left, Player const* right) {
            return left->GetGUID().GetRawValue() < right->GetGUID().GetRawValue();
        });
    return members;
}

bool IsObservedRaidThreat(Creature* creature, std::vector<Player*> const& members)
{
    for (Player* member : members)
    {
        if (creature->GetVictim() == member || creature->GetThreatMgr().IsThreatenedBy(member, true))
            return true;
    }
    return false;
}

bool IsValidObservedCreature(Creature* creature, Player* bot)
{
    return creature && creature->IsAlive() && creature->IsInWorld() && !creature->IsDuringRemoveFromWorld() &&
        creature->GetMap() == bot->GetMap() && !creature->IsPet() && !creature->IsTotem() &&
        creature->GetCreatureType() != CREATURE_TYPE_CRITTER;
}

void AddObservedCreature(std::map<PolicyGuid, Creature*>& observed, Unit* unit, Player* bot)
{
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    if (IsValidObservedCreature(creature, bot))
        observed[creature->GetGUID().GetRawValue()] = creature;
}

bool HasKnownRaidInterrupt(Player* member, PlayerbotAI* memberAI)
{
    if (!member || !memberAI)
        return false;

    RaidInterruptResponsePolicy::ActionNames const actions =
        RaidInterruptResponsePolicy::GetActions(member->getClass());
    for (char const* action : {actions.primary, actions.fallback})
    {
        if (!action)
            continue;

        uint32 const spellId = memberAI->GetAiObjectContext()->GetValue<uint32>("spell id", action)->Get();
        if (!spellId)
            continue;
        if (member->HasSpell(spellId))
            return true;
        if (Pet* pet = member->GetPet(); pet && pet->HasSpell(spellId))
            return true;
    }
    return false;
}

RaidAddSnapshot BuildRaidAddSnapshot(Player* bot)
{
    RaidAddSnapshot snapshot;
    std::vector<Player*> const raidMembers = GetAliveRaidMembers(bot);
    std::map<PolicyGuid, Creature*> observed;

    for (Player* member : raidMembers)
    {
        RaidPriorityAddPolicy::RaidMember policyMember;
        policyMember.guid = member->GetGUID().GetRawValue();
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        policyMember.canRespond = memberAI != nullptr;
        if (memberAI)
        {
            RaidTargetClaim const& memberClaim =
                memberAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();
            bool const liveEmergencyClaim = memberClaim.target &&
                !RaidTargetClaimPolicy::IsExpired(getMSTime(), memberClaim.expiresAtMs) &&
                memberClaim.authority == RaidTargetAuthority::EncounterEmergency;
            policyMember.canRespond = !liveEmergencyClaim;
        }
        policyMember.isHealer = PlayerbotAI::IsHeal(member);
        policyMember.isTank = PlayerbotAI::IsTank(member);
        policyMember.isMainTank = PlayerbotAI::IsMainTank(member);
        policyMember.isDps = PlayerbotAI::IsDps(member);
        policyMember.isRanged = PlayerbotAI::IsRanged(member);
        // Class contexts expose talent- and pet-dependent actions even when this concrete bot does
        // not know the underlying spell. Only consume an interrupt slot for a spell actually known
        // by the player or its active pet; the normal action still enforces range, cooldown, facing,
        // immunity, power, and GCD at execution time.
        policyMember.canInterrupt = HasKnownRaidInterrupt(member, memberAI);
        snapshot.members.push_back(policyMember);
        snapshot.memberUnits[policyMember.guid] = member;

        for (auto const& [guid, reference] : member->GetThreatMgr().GetThreatenedByMeList())
        {
            if (reference)
                AddObservedCreature(observed, reference->GetOwner(), bot);
        }
        for (Unit* attacker : member->getAttackers())
            AddObservedCreature(observed, attacker, bot);
    }

    for (auto const& [guid, creature] : observed)
    {
        if (!IsObservedRaidThreat(creature, raidMembers))
            continue;

        bool hostileToRaid = false;
        for (Player* member : raidMembers)
        {
            if (member->IsValidAttackTarget(creature))
            {
                hostileToRaid = true;
                break;
            }
        }
        if (!hostileToRaid)
            continue;

        if (creature->IsDungeonBoss() || creature->isWorldBoss())
        {
            snapshot.bossUnits[guid] = creature;
            // Reaching this branch means a live hostile raid boss is present in a raid member's
            // threat/attacker observations. Airborne and transition bosses can legitimately have
            // no current victim, so engagement must not be inferred from GetVictim().
            snapshot.bossEngaged = true;
            snapshot.bossHasVictim = snapshot.bossHasVictim || creature->GetVictim() != nullptr;
            if (Unit* victim = creature->GetVictim())
            {
                PolicyGuid const victimGuid = victim->GetGUID().GetRawValue();
                for (RaidPriorityAddPolicy::RaidMember& member : snapshot.members)
                {
                    if (member.guid == victimGuid)
                    {
                        member.ownsActiveBoss = true;
                        break;
                    }
                }
            }
            continue;
        }

        CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
        bool const elite = creatureTemplate &&
            (creatureTemplate->rank == CREATURE_ELITE_ELITE || creatureTemplate->rank == CREATURE_ELITE_RAREELITE ||
                creatureTemplate->rank == CREATURE_ELITE_RARE);
        RaidPriorityAddPolicy::AddCandidate add;
        add.guid = guid;
        add.victimGuid = creature->GetVictim() ? creature->GetVictim()->GetGUID().GetRawValue() : 0;
        add.threatBacked = true;
        add.isCasting = creature->IsNonMeleeSpellCast(true);
        add.isElite = elite;
        snapshot.adds.push_back(add);
        snapshot.addUnits[guid] = creature;
    }
    snapshot.reportedAddCount = snapshot.adds.size();
    return snapshot;
}

bool IsCcMarked(Group* group, ObjectGuid guid)
{
    if (!group || !guid)
        return false;
    // Preserve the conventional hard-CC marks while leaving kill/assist marks (skull/cross) alone.
    for (int8 const icon : {RtiTargetValue::diamondIndex, RtiTargetValue::triangleIndex,
             RtiTargetValue::moonIndex, RtiTargetValue::squareIndex})
    {
        if (group->GetTargetIcon(icon) == guid)
            return true;
    }
    return false;
}

RaidAddWavePolicy::Memory ReadWaveMemory(RaidTargetClaim const& claim)
{
    RaidAddWavePolicy::Memory memory;
    memory.waveId = claim.addWaveId;
    memory.state = claim.addWaveState <= static_cast<uint8>(RaidAddWavePolicy::State::Burn) ?
        static_cast<RaidAddWavePolicy::State>(claim.addWaveState) : RaidAddWavePolicy::State::Inactive;
    memory.anchorGuid = claim.addWaveAnchorGuid;
    memory.focusGuid = claim.addWaveFocusGuid;
    memory.preemptGuid = claim.addWavePreemptGuid;
    memory.replacementFromGuid = claim.addWaveReplacementFromGuid;
    return memory;
}

void WriteWaveMemory(RaidTargetClaim& claim, RaidAddWavePolicy::Memory const& memory)
{
    claim.addWaveId = memory.waveId;
    claim.addWaveState = static_cast<uint8>(memory.state);
    claim.addWaveAnchorGuid = memory.anchorGuid;
    claim.addWaveFocusGuid = memory.focusGuid;
    claim.addWavePreemptGuid = memory.preemptGuid;
    claim.addWaveReplacementFromGuid = memory.replacementFromGuid;
}

RaidAddWaveEvaluation EvaluateRaidAddWave(PlayerbotAI* botAI, Player* bot)
{
    RaidAddWaveEvaluation evaluation;
    evaluation.snapshot = BuildRaidAddSnapshot(bot);
    evaluation.plan = RaidPriorityAddPolicy::BuildPlan(
        evaluation.snapshot.members, evaluation.snapshot.adds,
        evaluation.snapshot.bossEngaged, evaluation.snapshot.bossHasVictim);

    RaidTargetClaim& claim = botAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();
    evaluation.previous = ReadWaveMemory(claim);
    std::vector<RaidAddWavePolicy::Member> waveMembers;
    for (RaidPriorityAddPolicy::RaidMember const& member : evaluation.snapshot.members)
    {
        RaidAddWavePolicy::Member waveMember;
        waveMember.guid = member.guid;
        waveMember.canRespond = member.canRespond;
        waveMember.isDps = member.isDps;
        waveMember.isTank = member.isTank;
        waveMember.isHealer = member.isHealer;
        waveMember.ownsBoss = member.ownsActiveBoss;
        waveMember.reservedForBossPressure = member.guid == evaluation.plan.bossPressureReserveGuid;
        waveMember.x = evaluation.snapshot.memberUnits.at(member.guid)->GetPositionX();
        waveMember.y = evaluation.snapshot.memberUnits.at(member.guid)->GetPositionY();
        waveMember.z = evaluation.snapshot.memberUnits.at(member.guid)->GetPositionZ();
        waveMember.safeForAdds = !member.isMainTank || !evaluation.snapshot.bossHasVictim;
        waveMember.currentAddLoad = static_cast<std::size_t>(std::count_if(
            evaluation.snapshot.adds.begin(), evaluation.snapshot.adds.end(),
            [&member](RaidPriorityAddPolicy::AddCandidate const& add) {
                return add.victimGuid == member.guid;
            }));
        waveMember.hasLooseAddTarget = std::any_of(
            evaluation.snapshot.adds.begin(), evaluation.snapshot.adds.end(),
            [&member](RaidPriorityAddPolicy::AddCandidate const& add) {
                return add.victimGuid == member.guid && !member.isTank;
            });
        auto const player = evaluation.snapshot.memberUnits.find(member.guid);
        if (player != evaluation.snapshot.memberUnits.end())
        {
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(player->second);
            if (memberAI)
            {
                RaidTargetClaim const& memberClaim =
                    memberAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();
                waveMember.hasEmergencyClaim = memberClaim.target &&
                    !RaidTargetClaimPolicy::IsExpired(getMSTime(), memberClaim.expiresAtMs) &&
                    memberClaim.authority == RaidTargetAuthority::EncounterEmergency;
                if (member.isTank && memberClaim.target &&
                    memberClaim.authority == RaidTargetAuthority::GenericTriage)
                {
                    waveMember.stickyAddGuid = memberClaim.target.GetRawValue();
                }
            }
        }
        waveMembers.push_back(waveMember);
    }

    std::vector<RaidAddWavePolicy::Add> waveAdds;
    for (RaidPriorityAddPolicy::AddCandidate const& add : evaluation.snapshot.adds)
    {
        auto const unitEntry = evaluation.snapshot.addUnits.find(add.guid);
        if (unitEntry == evaluation.snapshot.addUnits.end())
            continue;
        Creature* unit = unitEntry->second;
        RaidAddWavePolicy::Add waveAdd;
        waveAdd.guid = add.guid;
        waveAdd.x = unit->GetPositionX();
        waveAdd.y = unit->GetPositionY();
        waveAdd.z = unit->GetPositionZ();
        waveAdd.ccMarked = IsCcMarked(bot->GetGroup(), unit->GetGUID());
        waveAdd.eligibleForTankAssignment = true;

        RaidPriorityAddPolicy::RaidMember const* victim = nullptr;
        for (RaidPriorityAddPolicy::RaidMember const& member : evaluation.snapshot.members)
            if (member.guid == add.victimGuid)
                victim = &member;
        auto const waveVictim = std::find_if(waveMembers.begin(), waveMembers.end(),
            [victim](RaidAddWavePolicy::Member const& member) {
                return victim && member.guid == victim->guid;
            });
        bool const safeTankVictim = waveVictim != waveMembers.end() && waveVictim->isTank &&
            waveVictim->canRespond && waveVictim->safeForAdds && !waveVictim->ownsBoss &&
            !waveVictim->isHealer && !waveVictim->reservedForBossPressure && !waveVictim->hasEmergencyClaim;
        waveAdd.currentTankGuid = safeTankVictim ? victim->guid : 0;
        waveAdd.controlledBySafeTank = safeTankVictim;
        bool const healerVictim = victim && victim->isHealer;
        bool const exposedDpsVictim = victim && !victim->isTank && !victim->isHealer;
        waveAdd.priority = healerVictim ? 4 : exposedDpsVictim ? 3 : add.isCasting ? 2 : add.isElite ? 1 : 0;
        waveAdd.dangerousCast = add.isCasting;
        waveAdd.dangerPriority = healerVictim ? 4 : exposedDpsVictim ? 3 : add.isElite ? 2 : 1;
        waveAdds.push_back(waveAdd);
    }

    evaluation.wave = RaidAddWavePolicy::Evaluate(
        evaluation.previous, std::move(waveMembers), std::move(waveAdds), evaluation.snapshot.reportedAddCount);
    if (evaluation.wave.enteredNewEpoch)
        evaluation.wave.memory.replacementFromGuid = claim.target.GetRawValue();
    if (!evaluation.wave.temporaryPreemption && claim.target.GetRawValue() == evaluation.wave.memory.focusGuid)
    {
        evaluation.wave.memory.preemptGuid = 0;
        evaluation.wave.memory.replacementFromGuid = 0;
    }
    bool const waveTelemetryChanged = evaluation.wave.memory.state != RaidAddWavePolicy::State::Inactive &&
        (evaluation.wave.memory.waveId != evaluation.previous.waveId ||
            evaluation.wave.memory.state != evaluation.previous.state ||
            evaluation.wave.memory.anchorGuid != evaluation.previous.anchorGuid ||
            evaluation.wave.memory.focusGuid != evaluation.previous.focusGuid ||
            evaluation.wave.memory.preemptGuid != evaluation.previous.preemptGuid);
    if (waveTelemetryChanged)
    {
        LOG_INFO("playerbots",
            "[RaidAddWave] wave_id={} state={} anchor={} total={} clustered={} controlled={} stack_responders={} "
            "burn_responders={} focus={} bot_guid={}",
            evaluation.wave.memory.waveId, RaidAddWavePolicy::StateName(evaluation.wave.memory.state),
            evaluation.wave.memory.anchorGuid, evaluation.wave.total, evaluation.wave.clustered,
            evaluation.wave.controlled, evaluation.wave.stackResponders.size(), evaluation.wave.responders.size(),
            evaluation.wave.memory.focusGuid, bot->GetGUID().GetRawValue());
    }
    WriteWaveMemory(claim, evaluation.wave.memory);
    return evaluation;
}

Creature* FindOwnedRaidBoss(Player* bot)
{
    if (!bot || !bot->IsAlive() || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsRaid())
        return nullptr;

    RaidAddSnapshot const snapshot = BuildRaidAddSnapshot(bot);
    for (auto const& bossEntry : snapshot.bossUnits)
    {
        Creature* boss = bossEntry.second;
        if (boss && boss->GetVictim() == bot)
            return boss;
    }
    return nullptr;
}

PriorityAddSelection FindPriorityRaidAdd(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsRaid())
        return {};

    RaidAddWaveEvaluation evaluation = EvaluateRaidAddWave(botAI, bot);
    RaidAddSnapshot const& snapshot = evaluation.snapshot;
    RaidPriorityAddPolicy::Plan const& plan = evaluation.plan;
    RaidAddWavePolicy::Memory const& previousWave = evaluation.previous;
    RaidAddWavePolicy::Decision const& wave = evaluation.wave;
    RaidTargetClaim& claim = botAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();

    PolicyGuid const botGuid = bot->GetGUID().GetRawValue();
    RaidPriorityAddPolicy::Assignment const* assignment = plan.FindAssignment(botGuid);
    PolicyGuid selectedGuid = 0;
    RaidPriorityAddPolicy::AssignmentKind selectedKind = RaidPriorityAddPolicy::AssignmentKind::DpsFocus;
    std::uint8_t selectedInterruptRank = 0;
    bool allowSameOwnerRetarget = true;
    Unit* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    PolicyGuid const currentTargetGuid = currentTarget ? currentTarget->GetGUID().GetRawValue() : 0;

    if (wave.memory.state == RaidAddWavePolicy::State::Inactive)
    {
        if (!assignment)
            return {};
        selectedGuid = assignment->addGuid;
        selectedKind = assignment->kind;
        selectedInterruptRank = assignment->interruptRank;
    }
    else
    {
        if (wave.temporaryPreemption)
        {
            // Redirect exactly one already-eligible interrupt slot to the materially dangerous cast.
            auto const responder = std::find_if(plan.assignments.begin(), plan.assignments.end(),
                [](RaidPriorityAddPolicy::Assignment const& candidate) {
                    return candidate.kind == RaidPriorityAddPolicy::AssignmentKind::Interrupt;
                });
            if (responder != plan.assignments.end() && responder->memberGuid == botGuid)
            {
                selectedGuid = wave.memory.preemptGuid;
                selectedKind = RaidPriorityAddPolicy::AssignmentKind::Interrupt;
                selectedInterruptRank = responder->interruptRank;
            }
        }
        if (!selectedGuid && (wave.memory.state == RaidAddWavePolicy::State::Gather ||
                wave.memory.state == RaidAddWavePolicy::State::Burn))
        {
            selectedGuid = RaidAddWavePolicy::FindTankAssignment(wave, botGuid, currentTargetGuid);
            if (selectedGuid)
            {
                selectedKind = RaidPriorityAddPolicy::AssignmentKind::TankPickup;
                allowSameOwnerRetarget = RaidAddWavePolicy::MayReplaceTankTarget(
                    wave, botGuid, currentTargetGuid, selectedGuid);
            }
        }
        if (!selectedGuid && assignment && assignment->kind == RaidPriorityAddPolicy::AssignmentKind::TankPickup)
        {
            selectedGuid = assignment->addGuid;
            selectedKind = assignment->kind;
        }
        if (!selectedGuid && wave.memory.state == RaidAddWavePolicy::State::Burn &&
            RaidAddWavePolicy::IsResponder(wave, botGuid))
        {
            selectedGuid = wave.memory.focusGuid;
            selectedKind = RaidPriorityAddPolicy::AssignmentKind::DpsFocus;
            allowSameOwnerRetarget = RaidAddWavePolicy::MayReplaceStickyTarget(
                previousWave, wave, claim.target.GetRawValue(), selectedGuid);
        }
        if (!selectedGuid)
            return {};
    }

    auto const found = snapshot.addUnits.find(selectedGuid);
    if (found == snapshot.addUnits.end())
        return {};
    return {found->second, selectedGuid, selectedKind, selectedInterruptRank, allowSameOwnerRetarget};
}

Creature* FindRaidAddWaveGatherAnchor(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsRaid())
        return nullptr;

    RaidAddWaveEvaluation evaluation = EvaluateRaidAddWave(botAI, bot);
    RaidAddWavePolicy::Decision const& wave = evaluation.wave;
    PolicyGuid const botGuid = bot->GetGUID().GetRawValue();
    if (!RaidAddWavePolicy::IsStackResponder(wave, botGuid))
        return nullptr;

    auto const found = evaluation.snapshot.addUnits.find(wave.gatherAnchorGuid);
    if (found == evaluation.snapshot.addUnits.end() ||
        !IsValidObservedCreature(found->second, bot))
    {
        return nullptr;
    }
    return found->second;
}

char const* GetClassTauntAction(Player* bot)
{
    if (!bot)
        return nullptr;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: return "taunt";
        case CLASS_PALADIN: return "hand of reckoning";
        case CLASS_DRUID: return "growl";
        case CLASS_DEATH_KNIGHT: return "dark command";
        default: return nullptr;
    }
}
}

bool RaidPriorityAddAvailableTrigger::IsActive()
{
    return FindPriorityRaidAdd(botAI, bot).unit != nullptr;
}

bool RaidAddWaveGatherTrigger::IsActive()
{
    return FindRaidAddWaveGatherAnchor(botAI, bot) != nullptr;
}

bool RaidOwnedBossTargetAvailableTrigger::IsActive()
{
    // Stay active while ownership is live so the cooperative claim lease is refreshed even when
    // the bot is already on the correct target.
    return FindOwnedRaidBoss(bot) != nullptr;
}

bool RaidHoldOwnedBossTargetAction::Execute(Event /*event*/)
{
    Creature* boss = FindOwnedRaidBoss(bot);
    if (!boss)
        return false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    bool const changedTarget = currentTarget != boss;
    bool const attacked = AttackWithRaidTargetClaim(
        boss, "raid boss ownership", RaidTargetAuthority::BossOwnership);
    if (changedTarget && attacked)
    {
        LOG_INFO("playerbots",
            "[RaidTriage] bot={} bot_guid={} target={} target_guid={} entry={} assignment=boss_owner",
            bot->GetName(), bot->GetGUID().GetRawValue(), boss->GetName(), boss->GetGUID().GetRawValue(),
            boss->GetEntry());
    }
    return changedTarget && attacked;
}

bool RaidAttackPriorityAddAction::Execute(Event /*event*/)
{
    PriorityAddSelection const selection = FindPriorityRaidAdd(botAI, bot);
    Creature* target = selection.unit;
    if (!target)
        return false;
    if (selection.kind == RaidPriorityAddPolicy::AssignmentKind::Interrupt &&
        !target->IsNonMeleeSpellCast(true))
    {
        return false;
    }

    Unit* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    RaidPriorityAddPolicy::TargetDisposition const disposition =
        RaidPriorityAddPolicy::ResolveTargetDisposition(selection.targetGuid,
            currentTarget ? currentTarget->GetGUID().GetRawValue() : 0);
    bool const changedTarget = disposition == RaidPriorityAddPolicy::TargetDisposition::AcquireAssignedTarget;

    bool const attacked = AttackWithRaidTargetClaim(
        target, "raid triage", RaidTargetAuthority::GenericTriage, selection.allowSameOwnerRetarget);
    bool const assignmentHeld = !changedTarget || attacked;
    if (!assignmentHeld)
        return false;

    char const* assignmentKind = RaidPriorityAddPolicy::AssignmentKindName(selection.kind);
    char const* interruptSlot = RaidPriorityAddPolicy::InterruptAssignmentRankName(selection.interruptRank);
    if (changedTarget && attacked)
    {
        LOG_INFO("playerbots",
            "[RaidTriage] bot={} bot_guid={} target={} target_guid={} entry={} assignment={} interrupt_rank={} "
            "interrupt_slot={} casting={} victim={}",
            bot->GetName(), bot->GetGUID().GetRawValue(), target->GetName(), selection.targetGuid,
            target->GetEntry(), assignmentKind, selection.interruptRank, interruptSlot,
            target->IsNonMeleeSpellCast(true),
            target->GetVictim() ? target->GetVictim()->GetName() : "none");
    }

    // A short dangerous cast can complete before the next ordinary class-strategy tick after an
    // assigned retarget. Try only this cast's designated responder's legitimate class interrupt.
    // Cooldowns, range, facing, known spells, immunity, power, and the GCD remain enforced by the
    // existing action implementation.
    if (selection.kind == RaidPriorityAddPolicy::AssignmentKind::Interrupt &&
        target->IsNonMeleeSpellCast(true) && (currentTarget == target || attacked))
    {
        RaidInterruptResponsePolicy::ActionNames const actions =
            RaidInterruptResponsePolicy::GetActions(bot->getClass());
        bool interrupted = actions.primary && botAI->DoSpecificAction(
            actions.primary, Event("raid priority add interrupt"), true);
        if (!interrupted && actions.fallback)
        {
            interrupted = botAI->DoSpecificAction(
                actions.fallback, Event("raid priority add interrupt"), true);
        }
        if (interrupted)
        {
            LOG_INFO("playerbots",
                "[RaidInterrupt] bot={} bot_guid={} target={} target_guid={} entry={} assignment={} "
                "interrupt_rank={} interrupt_slot={}",
                bot->GetName(), bot->GetGUID().GetRawValue(), target->GetName(), selection.targetGuid,
                target->GetEntry(), assignmentKind, selection.interruptRank, interruptSlot);
            return true;
        }
    }

    // Tank pickups use only the class action already registered by Playerbots. Its normal spell,
    // cooldown, range, form, immunity, and power checks remain authoritative; no threat is injected.
    if (selection.kind == RaidPriorityAddPolicy::AssignmentKind::TankPickup && target->GetVictim() != bot)
    {
        char const* tauntAction = GetClassTauntAction(bot);
        if (tauntAction && botAI->DoSpecificAction(
                tauntAction, Event("raid priority add tank pickup"), true))
        {
            LOG_INFO("playerbots",
                "[RaidTriage] response=taunt bot={} bot_guid={} target={} target_guid={} entry={} assignment={}",
                bot->GetName(), bot->GetGUID().GetRawValue(), target->GetName(), selection.targetGuid,
                target->GetEntry(), assignmentKind);
            return true;
        }
    }

    // The cooperative claim lease spans multiple ordinary combat decisions. On a hold tick,
    // yielding lets the legitimate class rotation run while lower-authority target selectors remain
    // unable to replace this assignment. Same-owner reassignment and emergency movement remain
    // immediately available.
    return changedTarget && attacked;
}

bool RaidGatherAddWaveAction::Execute(Event /*event*/)
{
    Creature* anchor = FindRaidAddWaveGatherAnchor(botAI, bot);
    if (!anchor)
        return false;

    float const stackDistance = RaidAddWavePolicy::kGatherStackDistance + anchor->GetCombatReach();
    if (bot->GetDistance(anchor) <= stackDistance)
        return false;

    // This is an ordinary pathfinding move. Any loose add already attacking the responder follows
    // naturally; no position, threat, victim, or movement state is injected into either unit.
    bool const moved = MoveNear(
        anchor, RaidAddWavePolicy::kGatherStackDistance, MovementPriority::MOVEMENT_COMBAT);
    if (moved)
    {
        LOG_INFO("playerbots",
            "[RaidAddWave] response=stack bot={} bot_guid={} anchor={} anchor_guid={} distance={:.2f}",
            bot->GetName(), bot->GetGUID().GetRawValue(), anchor->GetName(),
            anchor->GetGUID().GetRawValue(), bot->GetDistance(anchor));
    }
    return moved;
}

bool RaidIncomingFearTrigger::IsActive()
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap())
        return false;

    bool warningActive = RaidFearSignal::IsActive(bot->GetInstanceId());
    if (!warningActive)
    {
        Unit* currentTarget = AI_VALUE(Unit*, "current target");
        warningActive = currentTarget && RaidFearSignal::IsKnownFearCaster(currentTarget->GetEntry());
    }

    return RaidFearResponsePolicy::ShouldPrepare(bot->GetMap()->IsRaid(), bot->IsAlive(), warningActive,
        bot->getClass() == CLASS_PRIEST, bot->getClass() == CLASS_SHAMAN);
}

bool RaidPrepareForFearAction::Execute(Event /*event*/)
{
    if (bot->getClass() == CLASS_SHAMAN)
    {
        bool const cast = botAI->DoSpecificAction("tremor totem", Event("raid incoming fear"), true);
        if (cast)
        {
            LOG_INFO("playerbots", "[RaidFear] response=tremor bot={} guid={} instance={}", bot->GetName(),
                bot->GetGUID().GetCounter(), bot->GetInstanceId());
        }
        return cast;
    }

    if (bot->getClass() != CLASS_PRIEST || !bot->GetGroup())
        return false;

    Player* mainTank = nullptr;
    for (GroupReference* reference = bot->GetGroup()->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (member && member->IsAlive() && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
            botAI->IsMainTank(member))
        {
            mainTank = member;
            break;
        }
    }
    if (!mainTank || !botAI->CanCastSpell("fear ward", mainTank))
        return false;

    bool const cast = botAI->CastSpell("fear ward", mainTank);
    if (cast)
    {
        LOG_INFO("playerbots", "[RaidFear] response=fear_ward bot={} guid={} target={} target_guid={} instance={}",
            bot->GetName(), bot->GetGUID().GetCounter(), mainTank->GetName(), mainTank->GetGUID().GetCounter(),
            bot->GetInstanceId());
    }
    return cast;
}

void RaidEncounterTriageStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Encounter-specific sensors publish into the shared raid-survival layer. Keeping emergency
    // movement here means it remains available while a bot is peeling an add and even if a
    // legacy encounter strategy was not restored after a map transition.
    triggers.push_back(new TriggerNode(
        "ony deep breath warning", {NextAction("ony move to safe zone", ACTION_EMERGENCY + 10)}));
    triggers.push_back(new TriggerNode(
        "raid incoming fear", {NextAction("raid prepare for fear", ACTION_EMERGENCY + 4)}));
    triggers.push_back(new TriggerNode(
        "raid owned boss target available", {NextAction("raid hold owned boss target", ACTION_RAID + 6)}));
    triggers.push_back(new TriggerNode(
        "raid priority add available", {NextAction("raid attack priority add", ACTION_RAID + 4)}));
    // Interrupt/target responses at RAID+4 temporarily preempt stacking; after the cast clears the
    // deterministic focus remains unchanged and this lower-priority ordinary movement resumes.
    triggers.push_back(new TriggerNode(
        "raid add wave gather", {NextAction("raid gather add wave", ACTION_RAID + 3)}));
}
