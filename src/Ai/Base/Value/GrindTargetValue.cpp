/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindTargetValue.h"

#include <algorithm>

#include "AttackersValue.h"
#include "DungeonNavigatorAmbientPolicy.h"
#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "AutoWowAcceptance.h"
#include "QuestObjectiveContext.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "TacticalRuntime.h"

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = nullptr;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(uint32 assistCount)
{
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    QuestObjectiveSpec objectiveSpec = AI_VALUE(QuestObjectiveSpec, "active quest objective");
    bool const questDirectiveActive = std::holds_alternative<NewRpgInfo::DoQuest>(botAI->rpgInfo.data);

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        // Self-defense is always allowed. When the attacker is also an exact
        // source for the locked objective, retain its GUID so a fast kill can
        // flow into objective-locked corpse looting. Unrelated attackers remain
        // pure self-defense and are never adopted as quest sources.
        if (objectiveSpec.hasLock() && unit->ToCreature() && objectiveSpec.acceptsCreatureEntry(unit->GetEntry()))
            if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
                doQuest && doQuest->objectiveRuntime.phase != QuestActionPhase::LootSource &&
                doQuest->objectiveRuntime.phase != QuestActionPhase::VerifyProgress)
                doQuest->objectiveRuntime.selectedTargetGuid = unit->GetGUID();

        return unit;
    }

    // AutoWow.Tactics.Enable (default 0), treatment-arm solo priests: no proactive pull below the pull
    // hp/mana thresholds (the "tactical nc" rest triggers drink/eat instead). Self-defence returned above.
    if (AutoWowTactics::Enabled() && AutoWowTactics::HoldProactivePull(botAI))
        return nullptr;
    // Same arm: pack-risk pull choice (hard reject + risk band sort key / distance penalty).
    bool const packRisk = AutoWowTactics::Enabled() && AutoWowTactics::PullRiskActive(botAI);
    std::uint32_t const packCapacity = packRisk ? AutoWowTactics::PullCapacity(botAI) : 0;

    // A completed/transitioning/blocked Director quest has no objective lock, but it still owns the
    // bot. Do not let legacy grind proactively acquire a fresh mob while the phase machine is walking
    // to or interacting with the finisher. Direct attackers were returned above and remain legal
    // self-defense. Incomplete supported objectives continue into the strict whitelist branch below.
    if (ShouldSuppressLegacyQuestGrind(questDirectiveActive, objectiveSpec))
        return nullptr;

    // CAST/source-item and scripted-event objectives name creatures but do not
    // ask the player to kill them. Direct attackers above remain legal
    // self-defense; proactive target selection must stay empty while their
    // dedicated phases interact with or protect the exact creature.
    if (objectiveSpec.hasLock() && (objectiveSpec.kind == QuestObjectiveKind::UseQuestItem ||
                                    objectiveSpec.kind == QuestObjectiveKind::ScriptedEvent))
        return nullptr;

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");

    // Strict quest-objective targeting. When a supported, incomplete objective is locked, a bot may
    // only pick an offensive grind target that actually satisfies that objective - regardless of
    // distance or aggro range. Direct attackers were already handled above as a separate self-defense
    // case (which does not clear the objective). This branch never falls through to legacy grind
    // selection: if nothing valid is found it returns nullptr.
    if (objectiveSpec.hasLock())
    {
        if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
        {
            QuestObjectiveRuntime& runtime = doQuest->objectiveRuntime;

            // Once objective loot is being resolved, the selected GUID is a
            // corpse identity, not an invitation to pick the next living mob.
            // Returning no grind target lets the higher-priority loot/new-RPG
            // actions consume or verify that corpse without this value racing
            // ahead and overwriting it.
            if (runtime.phase == QuestActionPhase::LootSource || runtime.phase == QuestActionPhase::VerifyProgress)
                return nullptr;

            // A five-player party can kill a low-level source between New-RPG
            // ticks. Hold the dead source until AcquireTarget/EngageTarget sees
            // it and enters LootSource; do not replace it with the next live
            // whitelisted source merely because one is nearby.
            if (!runtime.selectedTargetGuid.IsEmpty())
                if (Unit* selected = botAI->GetUnit(runtime.selectedTargetGuid); selected && !selected->IsAlive())
                    return nullptr;
        }

        // The generic possible-target cache starts from a broad "unfriendly"
        // grid query. That cache can omit a legitimate quest source even when
        // the creature is loaded a few yards away (q789 exposed this with a
        // Scorpid Worker 12.5yd from the bot). Seed one live candidate per exact
        // whitelisted creature entry, then run every candidate through the same
        // attackability/visibility/XP gates below. This broadens discovery only;
        // it never broadens objective legality.
        for (QuestObjectiveSource const& source : objectiveSpec.sources)
        {
            if (source.type != QuestObjectiveSource::Type::Creature)
                continue;

            Creature* nearest = bot->FindNearestCreature(source.entry, sPlayerbotAIConfig.sightDistance, true);
            if (!nearest)
                continue;

            ObjectGuid const guid = nearest->GetGUID();
            if (std::find(targets.begin(), targets.end(), guid) == targets.end())
                targets.push_back(guid);
        }

        Unit* bestTarget = nullptr;
        // Deterministic lexicographic score (smaller is better): party-shared target, path distance,
        // distance to the objective's source spawns, non-elite preference, targeting-player count, and
        // finally a GUID counter tiebreak so selection is fully deterministic (no random tiebreak).
        bool bestShared = false;
        float bestPathDist = 0.0f;
        float bestSpawnDist = 0.0f;
        bool bestElite = false;
        uint32 bestTargeting = 0;
        uint32 bestGuidCounter = 0;
        uint32 bestBand = 0;

        for (ObjectGuid const guid : targets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit)
                continue;

            if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
                continue;

            if (!unit->IsAlive())
                continue;

            Creature* creature = unit->ToCreature();
            if (!creature)
                continue;

            // Directly seeded source candidates did not pass through
            // PossibleTargetsValue, so apply its authoritative safety gate here
            // (also harmless/idempotent for cached candidates).
            if (!AttackersValue::IsPossibleTarget(unit, bot))
                continue;

            // Only creatures whose entry satisfies the locked objective are valid.
            if (!objectiveSpec.acceptsCreatureEntry(creature->GetEntry()))
                continue;

            // The objective must still be incomplete.
            if (objectiveSpec.currentCount >= objectiveSpec.requiredCount)
                continue;

            // Hostile / attackable, mirroring the legacy checks.
            if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
                continue;

            if (!bot->isHonorOrXPTarget(unit))
                continue;

            // Group contention (relaxes as assistCount grows across Calculate()'s retries).
            if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
                continue;

            if (!bot->InBattleground() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 &&
                !unit->GetGUID().IsPlayer())
                continue;

            bool isElite = false;
            if (CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate())
                if (creatureTemplate->rank > CREATURE_ELITE_NORMAL)
                {
                    if (!AI_VALUE(bool, "can fight elite"))
                        continue;

                    isElite = true;
                }

            if (!bot->IsWithinLOSInMap(unit))
                continue;

            uint32 band = 0;
            if (packRisk)
            {
                AutoWowPackRisk::Verdict const risk = AutoWowTactics::ScorePull(botAI, unit, targets, packCapacity);
                if (risk.reject)
                    continue;
                band = risk.band;
            }

            // Deterministic score components.
            uint32 targetingCount = GetTargetingPlayerCount(unit);
            bool shared = targetingCount > 0;
            float pathDist = bot->GetDistance(unit);

            float spawnDist = 0.0f;
            bool haveSpawn = false;
            for (auto const& source : objectiveSpec.sources)
            {
                for (auto const& spawn : source.spawns)
                {
                    if (spawn.GetMapId() != unit->GetMapId())
                        continue;

                    float d = unit->GetDistance(spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ());
                    if (!haveSpawn || d < spawnDist)
                    {
                        spawnDist = d;
                        haveSpawn = true;
                    }
                }
            }

            uint32 guidCounter = guid.GetCounter();

            bool takeCandidate = false;
            if (!bestTarget)
            {
                takeCandidate = true;
            }
            else if (shared != bestShared)
            {
                takeCandidate = shared;  // prefer a target a party member is already engaging
            }
            else if (packRisk && band != bestBand)
            {
                takeCandidate = band < bestBand;  // AutoWow.Tactics: a lone mob before a pack
            }
            else if (pathDist != bestPathDist)
            {
                takeCandidate = pathDist < bestPathDist;
            }
            else if (spawnDist != bestSpawnDist)
            {
                takeCandidate = spawnDist < bestSpawnDist;
            }
            else if (isElite != bestElite)
            {
                takeCandidate = !isElite;  // prefer non-elite
            }
            else if (targetingCount != bestTargeting)
            {
                takeCandidate = targetingCount < bestTargeting;
            }
            else
            {
                takeCandidate = guidCounter < bestGuidCounter;
            }

            if (takeCandidate)
            {
                bestTarget = unit;
                bestShared = shared;
                bestPathDist = pathDist;
                bestSpawnDist = spawnDist;
                bestElite = isElite;
                bestTargeting = targetingCount;
                bestGuidCounter = guidCounter;
                bestBand = band;
            }
        }

        if (packRisk && bestTarget)
            AutoWowTactics::NotePullChoice(bot, bestTarget, bestBand);

        // Violation guard only: strict selection above admits objective-whitelisted creature entries
        // exclusively. Self-defense returned before this branch and is deliberately not counted.
        if (bestTarget && !objectiveSpec.acceptsCreatureEntry(bestTarget->GetEntry()))
            AutoWowAcceptance::NoteUnrelatedOffensivePull();

        // Preserve the last selected GUID after the target dies. The quest phase
        // machine uses that exact corpse identity to enter objective-locked loot;
        // ResolveObjective / a completed loot attempt clears it deliberately.
        if (bestTarget)
            if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
                doQuest->objectiveRuntime.selectedTargetGuid = bestTarget->GetGUID();

        // Locked objective: never fall through to legacy grind selection.
        return bestTarget;
    }

    // The dungeon navigator owns proactive pulls in instances. Keep retaliation and exact
    // objective selection above intact, but do not enter the legacy generic candidate selector.
    if (DungeonNavigatorAmbientPolicy::ShouldSuppress(bot, botAI, BOT_STATE_NON_COMBAT))
        return nullptr;

    if (targets.empty())
        return nullptr;

    // Acceptance guard: reaching legacy grind selection while an objective lock is active would be a
    // random-grind-fallback violation (the strict branch returns above). Stays 0 unless bypassed.
    if (questDirectiveActive)
        AutoWowAcceptance::NoteRandomGrindFallback();

    float distance = 0;
    Unit* result = nullptr;
    uint32 resultBand = 0;
    std::unordered_map<uint32, bool> needForQuestMap;

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
            continue;

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            continue;

        if (!bot->isHonorOrXPTarget(unit))
            continue;

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
            continue;

        if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
            continue;

        // if (!bot->InBattleground() && master && master->GetDistance(unit) >= sPlayerbotAIConfig.grindDistance &&
        // !sRandomPlayerbotMgr.IsRandomBot(bot)) continue;

        // Bots in bot-groups no have a more limited range to look for grind target
        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > sPlayerbotAIConfig.lootDistance)
        {
            if (botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                botAI->TellMaster(chat->FormatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        if (!bot->InBattleground() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 && !unit->GetGUID().IsPlayer())
            continue;

        if (Creature* creature = unit->ToCreature())
            if (CreatureTemplate const* CreatureTemplate = creature->GetCreatureTemplate())
                if (CreatureTemplate->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
                    continue;

        if (!bot->IsWithinLOSInMap(unit))
        {
            continue;
        }

        bool inactiveGrindStatus = botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM && botAI->rpgInfo.GetStatus() != RPG_IDLE;

        float aggroRange = 30.0f;
        if (unit->ToCreature())
            aggroRange = std::min(30.0f, unit->ToCreature()->GetAggroRange(bot) + 10.0f);
        bool outOfAggro = unit->ToCreature() && bot->GetDistance(unit) > aggroRange;
        if (inactiveGrindStatus && outOfAggro)
        {
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);

            if (!needForQuestMap[unit->GetEntry()])
                continue;
        }

        uint32 band = 0;
        if (packRisk)
        {
            AutoWowPackRisk::Verdict const risk = AutoWowTactics::ScorePull(botAI, unit, targets, packCapacity);
            if (risk.reject)
                continue;
            band = risk.band;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                if (!member || !member->IsAlive())
                    continue;

                float d = member->GetDistance(unit);
                if (packRisk)
                    d += float(AutoWowTactics::RiskYd() * band);
                if (!result || d < distance)
                {
                    distance = d;
                    result = unit;
                    resultBand = band;
                }
            }
        }
        else
        {
            float newdistance = bot->GetDistance(unit);
            if (packRisk)
                newdistance += float(AutoWowTactics::RiskYd() * band);
            if (!result || (newdistance < distance))
            {
                distance = newdistance;
                result = unit;
                resultBand = band;
            }
        }
    }

    if (packRisk && result)
        AutoWowTactics::NotePullChoice(bot, result, resultBand);

    return result;
}

bool GrindTargetValue::needForQuest(Unit* target)
{
    QuestStatusMap& questMap = bot->getQuestStatusMap();
    for (auto& quest : questMap)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest.first);
        if (!questTemplate)
            continue;

        uint32 questId = questTemplate->GetQuestId();
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);

        if (status == QUEST_STATUS_INCOMPLETE)
        {
            const QuestStatusData* questStatus = &bot->getQuestStatusMap()[questId];

            if (questTemplate->GetQuestLevel() > bot->GetLevel() + 5)
                continue;

            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; j++)
            {
                int32 entry = questTemplate->RequiredNpcOrGo[j];

                if (entry && entry > 0)
                {
                    int required = questTemplate->RequiredNpcOrGoCount[j];
                    int available = questStatus->CreatureOrGOCount[j];

                    if (required && available < required && target->GetEntry() == uint32(entry))
                        return true;
                }
            }
        }
    }

    if (CreatureTemplate const* data = sObjectMgr->GetCreatureTemplate(target->GetEntry()))
    {
        if (uint32 lootId = data->lootid)
        {
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot))
            {
                return true;
            }
        }
    }

    return false;
}

uint32 GrindTargetValue::GetTargetingPlayerCount(Unit* unit)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    uint32 count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if ((botAI && *botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!botAI && member->GetTarget() == unit->GetGUID()))
            ++count;
    }

    return count;
}
