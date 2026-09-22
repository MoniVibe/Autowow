/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONPULLREADINESSGUARD_H
#define PLAYERBOTS_DUNGEONPULLREADINESSGUARD_H

#include <string>

#include "DungeonPullReadinessPolicy.h"
#include "Group.h"
#include "GroupReference.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ThreatManager.h"
#include "Unit.h"

namespace DungeonPullReadiness
{
inline bool HasCurrentCombatOrThreat(Unit* unit)
{
    if (!unit)
        return false;
    if (unit->IsInCombat())
        return true;

    for (auto const& threat : unit->GetThreatMgr().GetThreatenedByMeList())
    {
        Unit* attacker = threat.second->GetOwner();
        if (attacker && attacker->IsAlive() && attacker->IsInWorld() && attacker->GetMap() == unit->GetMap())
            return true;
    }
    return false;
}

inline bool TargetThreatens(Unit* target, Unit* member)
{
    return target && member &&
        (target->GetVictim() == member || target->GetThreatMgr().IsThreatenedBy(member, true));
}

inline bool IsPlayerDirectedAttack(std::string const& actionName)
{
    // The normal chat "attack" command dispatches this distinct action. Explicit pull commands
    // use PullRequestAction and never enter this path.
    return actionName == "attack my target" || actionName == "attack duel opponent";
}

// Deliberate dungeon pulls are held until the group is in support. A real unit-combat state still
// wins immediately so retaliation and survival combat are never blocked by this pull gate.
inline bool IsReady(PlayerbotAI* botAI, Player* bot, Unit* target, std::string const& actionName,
    bool deliberatePull = false)
{
    if (!botAI || !bot || !target || IsPlayerDirectedAttack(actionName) || !bot->IsAlive() ||
        bot->IsBeingTeleported() || bot->InBattleground())
    {
        return true;
    }

    if (!deliberatePull && bot->IsInCombat())
        return true;

    // PullRequestAction changes the engine before PullAction performs the actual spell/attack.
    // Do not let that pre-cast engine state bypass the deliberate-pull check. Ordinary attacks keep
    // the existing non-combat-only behavior.
    if (!deliberatePull && botAI->GetState() != BOT_STATE_NON_COMBAT)
        return true;

    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !group->IsLeader(bot->GetGUID()) || !map || !map->IsDungeon() ||
        !botAI->HasStrategy("dungeon navigator", BOT_STATE_NON_COMBAT))
    {
        return true;
    }

    Facts facts;
    facts.initiatorIsTank = PlayerbotAI::IsTank(bot);
    facts.targetDistance = bot->GetExactDist(target);

    bool groupHasCurrentCombatOrThreat = false;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        MemberFacts memberFacts;
        memberFacts.online = member != nullptr;
        if (member)
        {
            memberFacts.alive = member->IsAlive();
            memberFacts.inWorld = member->IsInWorld();
            memberFacts.sameMap = member->GetMapId() == bot->GetMapId();
            memberFacts.sameInstance = member->GetInstanceId() == bot->GetInstanceId();
            memberFacts.teleporting = member->IsBeingTeleported();
            memberFacts.tank = PlayerbotAI::IsTank(member);
            memberFacts.healer = PlayerbotAI::IsHeal(member);
            memberFacts.dps = PlayerbotAI::IsDps(member);
            if (memberFacts.sameMap && memberFacts.sameInstance)
            {
                memberFacts.distanceToInitiator = bot->GetExactDist(member);
                groupHasCurrentCombatOrThreat = groupHasCurrentCombatOrThreat || HasCurrentCombatOrThreat(member);
                facts.targetThreatensGroupOrPet =
                    facts.targetThreatensGroupOrPet || TargetThreatens(target, member);

                if (Pet* pet = member->GetPet())
                {
                    groupHasCurrentCombatOrThreat =
                        groupHasCurrentCombatOrThreat || HasCurrentCombatOrThreat(pet);
                    facts.targetThreatensGroupOrPet =
                        facts.targetThreatensGroupOrPet || TargetThreatens(target, pet);
                }
            }
        }
        facts.members.push_back(memberFacts);
    }

    // Existing unrelated combat is not a reason to block ordinary survival attacks, but it is a
    // deliberate-pull boundary: do not add a new target while the party is already engaged. A
    // threat from this target is still evaluated so the pure policy records explicit SelfDefense.
    facts.existingCombatOrThreat = groupHasCurrentCombatOrThreat;
    if (groupHasCurrentCombatOrThreat && !facts.targetThreatensGroupOrPet && !deliberatePull)
        return true;

    Requirements const requirements;
    Result const readiness = Evaluate(facts, requirements);
    if (readiness.decision == Decision::Ready || readiness.decision == Decision::SelfDefense)
        return true;

    LOG_INFO("playerbots",
        "[DungeonPullReadiness] blocked bot={} target={} reason={} target_distance={} radius={} "
        "healers={}/{} dps={}/{} required_dps={}",
        bot->GetName(), target->GetName(), ToString(readiness.decision), facts.targetDistance,
        requirements.supportRadius, readiness.readyHealers, readiness.availableHealers,
        readiness.readyDps, readiness.availableDps, readiness.requiredReadyDps);
    return false;
}
}

#endif
