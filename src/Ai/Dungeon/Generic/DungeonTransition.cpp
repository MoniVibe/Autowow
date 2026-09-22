/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonTransition.h"

#include "Corpse.h"
#include "DBCStores.h"
#include "DungeonTransitionPolicy.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Server/Protocol/Opcodes.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
constexpr uint32 TransitionPollBackoffMs = 2 * IN_MILLISECONDS;

struct PortalCandidate
{
    uint32 triggerId = 0;
    AreaTrigger const* trigger = nullptr;
    AreaTriggerTeleport const* teleport = nullptr;
};

bool IsReadyMember(Player* member)
{
    return member && PlayerbotsMgr::instance().GetPlayerbotAI(member) && member->IsAlive() &&
        member->IsInWorld() && !member->IsInCombat() && !member->IsInFlight() &&
        !member->IsBeingTeleported();
}

DungeonTransitionPolicy::MemberReadiness ClassifyMemberReadiness(
    Player* member, DungeonTransitionPolicy::MemberReadinessContext const& context,
    Map* targetInstance = nullptr)
{
    Corpse* corpse = member && targetInstance ?
        targetInstance->GetCorpseByPlayer(member->GetGUID()) : nullptr;
    bool const corpseValid = corpse && corpse->IsInWorld() && corpse->GetType() != CORPSE_BONES &&
        corpse->GetOwnerGUID() == member->GetGUID();

    DungeonTransitionPolicy::MemberReadinessFacts facts;
    if (member)
    {
        facts.online = PlayerbotsMgr::instance().GetPlayerbotAI(member) && member->IsInWorld();
        facts.alive = member->IsAlive();
        facts.ready = IsReadyMember(member);
        facts.released = member->HasPlayerFlag(PLAYER_FLAGS_GHOST);
        facts.inFlight = member->IsInFlight();
        facts.hasActualCorpse = targetInstance ? corpseValid : member->HasCorpse();
        if (member->HasCorpse())
            facts.corpseMapId = member->GetCorpseLocation().GetMapId();
        if (corpseValid)
            facts.corpseInstanceId = corpse->GetInstanceId();
    }
    return DungeonTransitionPolicy::EvaluateMemberReadiness(facts, context);
}

void ActivatePortal(Player* member, uint32 triggerId)
{
    member->StopMoving();
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    packet.rpos(0);
    member->GetSession()->HandleAreaTriggerOpcode(packet);

    // Exterior staging may temporarily pause followers so formation AI cannot walk them out of a
    // narrow trigger before their serial admission turn. Once the authoritative area-trigger
    // packet has been handled, normal bot activity can resume in the shared instance.
    if (PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member))
        memberAI->SetAutoWowPaused(false);
}

PortalCandidate FindExteriorPortal(std::vector<Player*> const& members, uint32 exteriorMap)
{
    PortalCandidate best;
    float bestDistance = std::numeric_limits<float>::infinity();
    if (members.empty())
        return best;

    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        MapEntry const* targetMap = sMapStore.LookupEntry(teleport.target_mapId);
        if (!trigger || trigger->map != exteriorMap || !targetMap || !targetMap->IsNonRaidDungeon())
            continue;

        bool allInsideTrigger = true;
        for (Player* member : members)
        {
            if (member->GetMapId() != exteriorMap || member->GetInstanceId() ||
                !member->IsInAreaTriggerRadius(trigger, 0.0f))
            {
                allInsideTrigger = false;
                break;
            }
        }
        if (!allInsideTrigger)
            continue;

        float const distance = members.front()->GetExactDist(trigger->x, trigger->y, trigger->z);
        if (distance < bestDistance || (distance == bestDistance && triggerId < best.triggerId))
        {
            bestDistance = distance;
            best = {triggerId, trigger, &teleport};
        }
    }
    return best;
}

PortalCandidate FindIngressPortal(std::vector<Player*> const& outsideMembers, uint32 targetMap)
{
    PortalCandidate best;
    float bestDistance = std::numeric_limits<float>::infinity();
    if (outsideMembers.empty())
        return best;

    uint32 const exteriorMap = outsideMembers.front()->GetMapId();
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        if (teleport.target_mapId != targetMap)
            continue;

        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        if (!trigger || trigger->map != exteriorMap)
            continue;

        bool allInsideTrigger = true;
        for (Player* member : outsideMembers)
        {
            if (member->GetMapId() != exteriorMap || member->GetInstanceId() ||
                !member->IsInAreaTriggerRadius(trigger, 0.0f))
            {
                allInsideTrigger = false;
                break;
            }
        }
        if (!allInsideTrigger)
            continue;

        float const distance = outsideMembers.front()->GetExactDist(trigger->x, trigger->y, trigger->z);
        if (distance < bestDistance || (distance == bestDistance && triggerId < best.triggerId))
        {
            bestDistance = distance;
            best = {triggerId, trigger, &teleport};
        }
    }
    return best;
}
}

void DungeonTransitionStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Higher than route discovery: finish party admission before the leader advances farther.
    triggers.push_back(new TriggerNode("timer", {NextAction("dungeon party transition", 2.0f)}));
}

bool DungeonPartyTransitionAction::Execute(Event /*event*/)
{
    uint32 const now = getMSTime();
    if (nextCheckTime && int32(nextCheckTime - now) > 0)
        return false;
    nextCheckTime = now + TransitionPollBackoffMs;

    if (!bot || !IsReadyMember(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group || !group->IsLeader(bot->GetGUID()))
        return false;
    if (group->GetDifficulty(false) != DUNGEON_DIFFICULTY_NORMAL)
        return false;

    std::vector<Player*> groupMembers;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || !PlayerbotsMgr::instance().GetPlayerbotAI(member))
            return false;
        if (member->GetDifficulty(false) != DUNGEON_DIFFICULTY_NORMAL)
            return false;
        groupMembers.push_back(member);
    }
    if (groupMembers.size() < 2)
        return false;

    Map* leaderMap = bot->GetMap();
    bool const leaderInside = leaderMap && leaderMap->IsNonRaidDungeon() && bot->GetInstanceId();
    if (!leaderInside)
    {
        std::vector<Player*> livingMembers;
        std::vector<Player*> releasedMembers;
        for (Player* member : groupMembers)
        {
            if (IsReadyMember(member))
                livingMembers.push_back(member);
            else if (!member->IsAlive() && member->HasPlayerFlag(PLAYER_FLAGS_GHOST) &&
                     member->HasCorpse())
                releasedMembers.push_back(member);
            else
                return false;
        }
        if (livingMembers.empty())
            return false;

        for (Player* member : livingMembers)
        {
            if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
                return false;
            if (member->isMoving())
                return false;
        }

        PortalCandidate const portal = FindExteriorPortal(livingMembers, bot->GetMapId());
        if (!portal.triggerId)
            return false;
        DungeonTransitionPolicy::MemberReadinessContext const readinessContext{
            DungeonTransitionPolicy::PartyReadinessStage::ExteriorPortal,
            portal.teleport->target_mapId, 0, 0};
        for (Player* member : releasedMembers)
            if (ClassifyMemberReadiness(member, readinessContext) !=
                DungeonTransitionPolicy::MemberReadiness::OmitReleasedCorpse)
                return false;

        uint32 const oldMap = bot->GetMapId();
        ActivatePortal(bot, portal.triggerId);
        LOG_INFO("playerbots",
            "[DungeonTransition] leader={} trigger={} stage=leader exterior_map={} target_map={} released_waiting={} activated=true",
            bot->GetName(), portal.triggerId, oldMap, portal.teleport->target_mapId,
            releasedMembers.size());
        return true;
    }

    std::vector<Player*> members;
    uint32 releasedWaiting = 0;
    DungeonTransitionPolicy::MemberReadinessContext const readinessContext{
        DungeonTransitionPolicy::PartyReadinessStage::LeaderInside,
        0, bot->GetMapId(), bot->GetInstanceId()};
    for (Player* member : groupMembers)
    {
        DungeonTransitionPolicy::MemberReadiness const readiness =
            ClassifyMemberReadiness(member, readinessContext, leaderMap);
        if (readiness == DungeonTransitionPolicy::MemberReadiness::Proceed)
        {
            members.push_back(member);
            continue;
        }
        if (readiness == DungeonTransitionPolicy::MemberReadiness::OmitReleasedCorpse)
        {
            ++releasedWaiting;
            continue;
        }
        return false;
    }

    std::vector<Player*> outsideMembers;
    uint32 insideMembers = 0;
    for (Player* member : members)
    {
        if (member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId())
        {
            ++insideMembers;
            continue;
        }
        if (member->GetMapId() == bot->GetMapId() || member->GetInstanceId())
        {
            LOG_INFO("playerbots",
                "[DungeonTransition] leader={} stage=blocked reason=split_instance member={} map={} instance={}",
                bot->GetName(), member->GetName(), member->GetMapId(), member->GetInstanceId());
            return false;
        }
        if (member->isMoving())
            return false;
        outsideMembers.push_back(member);
    }
    if (outsideMembers.empty())
        return false;

    std::sort(outsideMembers.begin(), outsideMembers.end(), [](Player* left, Player* right)
    {
        return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
    });

    PortalCandidate const portal = FindIngressPortal(outsideMembers, bot->GetMapId());
    if (!portal.triggerId)
        return false;

    Player* nextMember = outsideMembers.front();
    ActivatePortal(nextMember, portal.triggerId);
    LOG_INFO("playerbots",
        "[DungeonTransition] leader={} trigger={} stage=follower member={} inside_before={} total={} released_waiting={} target_map={} target_instance={} activated=true",
        bot->GetName(), portal.triggerId, nextMember->GetName(), insideMembers, members.size(),
        releasedWaiting, bot->GetMapId(), bot->GetInstanceId());
    return true;
}
