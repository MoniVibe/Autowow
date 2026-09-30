/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToHeal.h"
#include "Action.h"
#include "CriticalMemberRecoveryPolicy.h"
#include "Playerbots.h"
#include "RaidTargetClaimValue.h"
#include "ServerFacade.h"
#include "Timer.h"

class IsTargetOfHealingSpell : public SpellEntryPredicate
{
public:
    bool Check(SpellInfo const* spellInfo) override
    {
        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MECHANICAL)
                return true;
        }

        return false;
    }
};

namespace
{
using CriticalMemberRecoveryPolicy::Healer;
using CriticalMemberRecoveryPolicy::Request;

bool HasActiveBlockingRaidClaim(PlayerbotAI* memberAI)
{
    if (!memberAI)
        return false;

    RaidTargetClaim const& claim =
        memberAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();
    return claim.target && !RaidTargetClaimPolicy::IsExpired(getMSTime(), claim.expiresAtMs) &&
           static_cast<uint8>(claim.authority) >=
               static_cast<uint8>(RaidTargetAuthority::BossOwnership);
}

bool HasActiveAvoidance(PlayerbotAI* memberAI)
{
    if (!memberAI)
        return false;

    Action* avoidance = memberAI->GetAiObjectContext()->GetAction("avoid aoe");
    return avoidance && avoidance->isUseful();
}

std::vector<Healer> BuildRecoveryHealers(Group* group, Unit* target)
{
    std::vector<Healer> healers;
    if (!group || !target)
        return healers;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
        if (!member || !memberAI || !member->IsInWorld() || !member->IsAlive() ||
            member->GetMapId() != target->GetMapId() || !PlayerbotAI::IsHeal(member))
        {
            continue;
        }

        Healer healer;
        healer.guid = member->GetGUID().GetRawValue();
        healer.alive = true;
        healer.withinBoundedRecoveryRange =
            CriticalMemberRecoveryPolicy::IsWithinBoundedRecoveryRange(
                member->GetDistance2d(target), sPlayerbotAIConfig.healDistance);
        healer.inCastRangeAndLos =
            member->GetDistance2d(target) <= sPlayerbotAIConfig.healDistance &&
            member->IsWithinLOSInMap(target);
        // A tank is conservatively treated as owning a possible tank-pickup movement.  This keeps
        // recovery from competing with boss ownership or any ordinary taunt/pickup assignment.
        healer.hasTankPickup = PlayerbotAI::IsTank(member);
        healer.ownsBoss = HasActiveBlockingRaidClaim(memberAI);
        healer.emergencyAvoidance = HasActiveAvoidance(memberAI);
        healers.push_back(healer);
    }
    return healers;
}

Request BuildRecoveryRequest(PlayerbotAI* currentAI, Group* group, Unit* target, bool incomingHeal)
{
    Request request;
    if (!currentAI || !target)
        return request;

    Player* currentBot = currentAI->GetBot();
    request.target.guid = target->GetGUID().GetRawValue();
    request.target.healthPct = target->GetHealthPct();
    request.target.distanceToCurrentHealer = currentBot ? currentBot->GetDistance2d(target) : 0.0f;
    request.target.alive = target->IsAlive() && target->IsInWorld();
    request.target.inCastRangeAndLos = currentBot &&
        request.target.distanceToCurrentHealer <= sPlayerbotAIConfig.healDistance &&
        currentBot->IsWithinLOSInMap(target);
    request.target.incomingHeal = incomingHeal;
    request.target.superseded = !currentBot || PlayerbotAI::IsTank(currentBot) ||
        HasActiveBlockingRaidClaim(currentAI) || HasActiveAvoidance(currentAI);
    request.target.isTank = target->ToPlayer() && PlayerbotAI::IsTank(target->ToPlayer());
    request.healers = BuildRecoveryHealers(group, target);
    request.currentHealerGuid = currentBot ? currentBot->GetGUID().GetRawValue() : 0;
    request.healDistance = sPlayerbotAIConfig.healDistance;
    request.criticalHealth = sPlayerbotAIConfig.criticalHealth;
    request.nowMs = getMSTime();
    return request;
}
}

inline bool compareByHealth(Unit const* u1, Unit const* u2) { return u1->GetHealthPct() < u2->GetHealthPct(); }

Unit* PartyMemberToHeal::Calculate()
{
    IsTargetOfHealingSpell predicate;

    Group* group = bot->GetGroup();
    if (!group)
        return bot;

    bool isRaid = bot->GetGroup()->isRaidGroup();
    PartyMemberToHealPolicy::CandidatePriority selectedPriority;
    Unit* selectedTarget = nullptr;

    auto considerTarget = [&](Unit* candidate, PartyMemberToHealPolicy::CandidatePriority const& priority)
    {
        if (!PartyMemberToHealPolicy::IsSelectable(priority) ||
            (selectedTarget && !PartyMemberToHealPolicy::ShouldReplace(priority, selectedPriority)))
            return;

        if (!Check(candidate))
            return;

        selectedPriority = priority;
        selectedTarget = candidate;
    };

    auto canSelectPlayer = [&](Player* player, float health)
    {
        bool const canIgnoreReservation =
            health < sPlayerbotAIConfig.criticalHealth ||
            (!isRaid && health < sPlayerbotAIConfig.mediumHealth);
        bool const hasIncomingHeal = !canIgnoreReservation && IsTargetOfSpellCast(player, predicate);
        return PartyMemberToHealPolicy::CanSelectWithIncomingHeal(
            health, isRaid, hasIncomingHeal, sPlayerbotAIConfig.criticalHealth,
            sPlayerbotAIConfig.mediumHealth);
    };

    auto playerPriority = [&](Player* player, float health)
    {
        return PartyMemberToHealPolicy::CalculatePriority(
            health, player->GetDistance2d(bot), sPlayerbotAIConfig.healDistance,
            sPlayerbotAIConfig.criticalHealth, sPlayerbotAIConfig.lowHealth,
            sPlayerbotAIConfig.mediumHealth, PlayerbotAI::IsExplicitMainTank(player),
            botAI->IsTank(player));
    };

    // If focus heal targets strategy is active, only heal those targets
    if (botAI->HasStrategy("focus heal targets", BOT_STATE_COMBAT))
    {
        std::list<ObjectGuid> const focusHealTargets =
            AI_VALUE(std::list<ObjectGuid>, "focus heal targets");

        for (ObjectGuid const& focusHealTarget : focusHealTargets)
        {
            Player* player = ObjectAccessor::FindPlayer(focusHealTarget);
            if (!player || !player->IsInWorld() || !player->IsAlive() || !player->IsInSameGroupWith(bot))
                continue;

            float health = player->GetHealthPct();
            if (!canSelectPlayer(player, health))
                continue;

            considerTarget(player, playerPriority(player, health));
        }

        return selectedTarget;
    }

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (player->IsGameMaster())
            continue;
        if (player && player->IsAlive())
        {
            float health = player->GetHealthPct();
            if (canSelectPlayer(player, health))
            {
                // delay Check player to here for better performance
                considerTarget(player, playerPriority(player, health));
            }
        }

        Pet* pet = player->GetPet();
        if (pet && pet->IsAlive())
        {
            float health = ((Unit*)pet)->GetHealthPct();
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth)
            {
                // delay Check pet to here for better performance
                considerTarget(pet, PartyMemberToHealPolicy::CalculateCompanionPriority(
                                        health, sPlayerbotAIConfig.criticalHealth,
                                        sPlayerbotAIConfig.lowHealth, sPlayerbotAIConfig.mediumHealth));
            }
        }

        Unit* charm = player->GetCharm();
        if (charm && charm->IsAlive())
        {
            float health = charm->GetHealthPct();
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth)
            {
                // delay Check charm to here for better performance
                considerTarget(charm, PartyMemberToHealPolicy::CalculateCompanionPriority(
                                          health, sPlayerbotAIConfig.criticalHealth,
                                          sPlayerbotAIConfig.lowHealth, sPlayerbotAIConfig.mediumHealth));
            }
        }
    }
    return selectedTarget;
}

bool PartyMemberToHeal::Check(Unit* player)
{
    // return player && player != bot && player->GetMapId() == bot->GetMapId() && player->IsInWorld() &&
    //     ServerFacade::instance().GetDistance2d(bot, player) < (player->IsPlayer() && botAI->IsTank((Player*)player) ? 50.0f
    //     : 40.0f);
    if (!player || player->GetMapId() != bot->GetMapId() || player->IsCharmed())
        return false;

    if (bot->GetDistance2d(player) < sPlayerbotAIConfig.healDistance * 2 &&
        bot->IsWithinLOSInMap(player))
    {
        return true;
    }

    // The old two-range gate made a critical raid member at 91.4 yd invisible when HealDistance
    // was 38.5.  Only live player targets in the pure policy's bounded critical window can use
    // the derived one-healer recovery claimant; pets/charmed units and over-bound targets still
    // fail closed.
    if (!player->IsPlayer())
        return false;

    IsTargetOfHealingSpell predicate;
    bool const incomingHeal = IsTargetOfSpellCast(player->ToPlayer(), predicate);
    Request const request = BuildRecoveryRequest(botAI, bot->GetGroup(), player, incomingHeal);
    if (!CriticalMemberRecoveryPolicy::IsRecoveryCandidate(
            request.target, request.healDistance, request.criticalHealth))
    {
        return false;
    }

    return CriticalMemberRecoveryPolicy::Evaluate(request).action ==
        CriticalMemberRecoveryPolicy::Action::HealerApproach;
}

Unit* HealerLowMana::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    MinValueCalculator calc(100);

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player == bot)
            continue;
        if (player->IsGameMaster() || !player->IsAlive())
            continue;
        if (!botAI->IsHeal(player))
            continue;

        float mana = player->GetPowerPct(POWER_MANA);
        if (mana < calc.minValue)
            calc.probe(mana, player);
    }

    return (Unit*)calc.param;
}

Unit* PartyMemberToProtect::Calculate()
{
    return nullptr;
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::vector<Unit*> needProtect;

    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (GuidVector::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit)
            continue;

        Unit* pVictim = unit->GetVictim();
        if (!pVictim || !pVictim->IsPlayer())
            continue;

        if (pVictim == bot)
            continue;

        float attackDistance = 30.0f;
        if (ServerFacade::instance().GetDistance2d(pVictim, unit) > attackDistance)
            continue;

        if (botAI->IsTank((Player*)pVictim) && pVictim->GetHealthPct() > 10)
            continue;
        else if (pVictim->GetHealthPct() > 30)
            continue;

        if (find(needProtect.begin(), needProtect.end(), pVictim) == needProtect.end())
            needProtect.push_back(pVictim);
    }

    if (needProtect.empty())
        return nullptr;

    sort(needProtect.begin(), needProtect.end(), compareByHealth);

    return needProtect[0];
}
