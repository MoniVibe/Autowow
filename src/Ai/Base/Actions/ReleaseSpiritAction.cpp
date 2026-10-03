/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReleaseSpiritAction.h"
#include "DungeonDeathRecoveryPolicy.h"
#include "ReleasedCorpseApproachPolicy.h"
#include "ServerFacade.h"
#include "Corpse.h"
#include "Event.h"
#include "DBCStores.h"
#include "GameGraveyard.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "Log.h"
#include "NearestNpcsValue.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Group.h"
#include "../../World/Gathering/GatheringWorkerState.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_map>
#include <vector>

namespace
{
struct DungeonCorpseRunState
{
    uint32 corpseMap = 0;
    time_t startedAt = 0;
    uint8 entranceAttempts = 0;
    time_t retryAfter = 0;
    bool exhaustedLogged = false;
};

std::unordered_map<uint32, DungeonCorpseRunState> dungeonCorpseRuns;

constexpr uint32 IngressRejectedPathTypes =
    PATHFIND_SHORTCUT | PATHFIND_NOPATH |
    PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY;
constexpr float IngressHeightSearchPadding = 2.0f;

bool IsGroupedInstanceDeath(Player const* player)
{
    return player && player->GetGroup() && player->GetMap() && player->GetMap()->IsDungeon();
}

bool IsReleasedGroupedInstanceCorpse(Player const* player)
{
    if (!player || !player->GetGroup() || !player->HasCorpse() ||
        !player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return false;

    MapEntry const* corpseMap = sMapStore.LookupEntry(player->GetCorpseLocation().GetMapId());
    return corpseMap && corpseMap->IsDungeon();
}

DungeonCorpseRunState& GetDungeonCorpseRunState(Player const* player, uint32 corpseMap, time_t now)
{
    DungeonCorpseRunState& state = dungeonCorpseRuns[player->GetGUID().GetCounter()];
    if (!state.startedAt || state.corpseMap != corpseMap)
    {
        state = {};
        state.corpseMap = corpseMap;
        state.startedAt = now;
    }
    return state;
}

DungeonCorpseRunState& ResetDungeonCorpseRunState(Player const* player, uint32 corpseMap, time_t now)
{
    DungeonCorpseRunState& state = dungeonCorpseRuns[player->GetGUID().GetCounter()];
    state = {};
    state.corpseMap = corpseMap;
    state.startedAt = now;
    return state;
}

uint32 CorpseRunElapsedSeconds(DungeonCorpseRunState const& state, time_t now)
{
    return now > state.startedAt ? static_cast<uint32>(now - state.startedAt) : 0;
}

void RecordEntranceAttempt(DungeonCorpseRunState& state, time_t now)
{
    state.entranceAttempts = std::min<uint8>(
        static_cast<uint8>(state.entranceAttempts + 1),
        DungeonDeathRecovery::MaxEntranceAttempts);
    state.retryAfter = now + DungeonDeathRecovery::RetryBackoffSeconds;
}

ReleasedCorpseApproachPolicy::IngressTriggerVolume MakeIngressTriggerVolume(
    AreaTrigger const& trigger)
{
    return {trigger.x, trigger.y, trigger.z, trigger.radius, trigger.length, trigger.width,
            trigger.height, trigger.orientation};
}

bool IsUsableIngressPath(PathGenerator const& path)
{
    uint32 const pathType = static_cast<uint32>(path.GetPathType());
    // The probe is only a bounded point inside the trigger volume, not a mandatory navmesh
    // endpoint. A partial path is usable when its actual endpoint is inside the trigger; the
    // endpointInside check below is the final admission gate. Unsafe/ambiguous path types remain
    // rejected so this cannot turn into a shortcut or a blind cross-map attempt.
    return (pathType & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE)) != 0 &&
        !(pathType & IngressRejectedPathTypes);
}

ReleasedCorpseApproachPolicy::IngressApproachSelection FindReachableIngressApproach(
    Player const* player, AreaTrigger const& trigger)
{
    using namespace ReleasedCorpseApproachPolicy;

    IngressTriggerVolume const volume = MakeIngressTriggerVolume(trigger);
    std::vector<IngressApproachCandidate> const candidates =
        BuildIngressApproachCandidates(volume);
    Map* map = player ? player->GetMap() : nullptr;
    uint32 const triggerMap = trigger.map;
    float const verticalExtent = trigger.radius > 0.0f ? trigger.radius : trigger.height * 0.5f;
    return SelectIngressApproach(candidates,
        [player, map, &volume, triggerMap, verticalExtent](IngressApproachCandidate const& candidate)
        {
            IngressApproachEvaluation evaluation;
            if (!player || !map || map->GetId() != player->GetMapId() ||
                map->GetId() != triggerMap ||
                !std::isfinite(verticalExtent) || verticalExtent <= 0.0f)
            {
                return evaluation;
            }

            float const searchTop = candidate.z + verticalExtent + IngressHeightSearchPadding;
            float const searchDistance = verticalExtent * 2.0f +
                IngressHeightSearchPadding * 2.0f;
            float const ground = map->GetHeight(player->GetPhaseMask(), candidate.x, candidate.y,
                                                searchTop, true, searchDistance);
            evaluation.floorValid = ground > INVALID_HEIGHT && std::isfinite(ground) &&
                IsInsideIngressTrigger(volume, candidate.x, candidate.y, ground);
            if (!evaluation.floorValid)
                return evaluation;

            PathGenerator path(player);
            bool const calculated = path.CalculatePath(candidate.x, candidate.y, ground, false);
            evaluation.pathReachable = calculated && IsUsableIngressPath(path);
            if (!evaluation.pathReachable)
                return evaluation;

            G3D::Vector3 const& endpoint = path.GetActualEndPosition();
            evaluation.endpointX = endpoint.x;
            evaluation.endpointY = endpoint.y;
            evaluation.endpointZ = endpoint.z;
            evaluation.endpointInside = IsInsideIngressTrigger(
                volume, endpoint.x, endpoint.y, endpoint.z);
            evaluation.pathDistance = path.getPathLength();
            return evaluation;
        });
}

ReleasedCorpseApproachPolicy::IngressPortalSelection FindIngressPortal(
    Player const* player, uint32 corpseMap)
{
    std::vector<ReleasedCorpseApproachPolicy::IngressPortalCandidate> candidates;
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        float distanceSquared = 0.0f;
        if (trigger)
        {
            float const dx = player->GetPositionX() - trigger->x;
            float const dy = player->GetPositionY() - trigger->y;
            float const dz = player->GetPositionZ() - trigger->z;
            distanceSquared = dx * dx + dy * dy + dz * dz;
        }
        candidates.push_back({triggerId, trigger ? trigger->map : 0, teleport.target_mapId,
                              distanceSquared, trigger != nullptr});
    }
    return ReleasedCorpseApproachPolicy::SelectIngressPortal(
        candidates, player->GetMapId(), corpseMap);
}
}

// ReleaseSpiritAction implementation
bool ReleaseSpiritAction::Execute(Event event)
{
    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()))
    {
        AutoWowGather::DeathRecoveryTransition const recovery = AutoWowGather::PlanDeathRecovery(
            bot->GetGUID().GetCounter(), bot->IsAlive(), bot->HasPlayerFlag(PLAYER_FLAGS_GHOST),
            bot->HasCorpse(), false, bot->isMoving());
        if (recovery.action == AutoWowGather::DeathRecoveryAction::ReleaseSpirit)
        {
            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);
            botAI->SetNextCheckDelay(1000);
            return true;
        }

        if (recovery.action == AutoWowGather::DeathRecoveryAction::Wait)
        {
            botAI->SetNextCheckDelay(std::max<uint32>(1, recovery.delaySeconds) * 1000);
            return true;
        }

        return false;
    }

    if (bot->IsAlive())
    {
        if (!bot->InBattleground())
        {
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "release_spirit_not_dead_wait", "I am not dead, will wait here", {}));
            // -follow in bg is overwriten each tick with +follow
            // +stay in bg causes stuttering effect as bot is cycled between +stay and +follow each tick
            botAI->ChangeStrategy("-follow,+stay", BOT_STATE_NON_COMBAT);
        }

        return false;
    }

    if (bot->GetCorpse() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "release_spirit_already_spirit", "I am already a spirit", {}));
        return false;
    }

    WorldPacket const& packet = event.getPacket();
    const std::string message = !packet.empty() && packet.GetOpcode() == CMSG_REPOP_REQUEST
        ? PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_releasing", "Releasing...", {})
        : PlayerbotTextMgr::instance().GetBotTextOrDefault("release_spirit_meet_graveyard", "Meet me at the graveyard", {});
    botAI->TellMasterNoFacing(message);

    IncrementDeathCount();
    bot->DurabilityRepairAll(false, 1.0f, false);
    LogRelease("released");

    WorldPacket releasePacket(CMSG_REPOP_REQUEST);
    releasePacket << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(releasePacket);

    return true;
}

void ReleaseSpiritAction::IncrementDeathCount() const
{
    // BG deaths don't count, matching PlayerbotAI::DoNextAction.
    if (bot->InBattleground())
        return;

    // Death Count to prevent skeleton piles
    Player* master = botAI->GetMaster();
    if (!master || GET_PLAYERBOT_AI(master))
    {
        uint32 deathCount = AI_VALUE(uint32, "death count");
        context->GetValue<uint32>("death count")->Set(deathCount + 1);
    }
}

void ReleaseSpiritAction::LogRelease(std::string const& releaseMsg) const
{
    const std::string teamPrefix = bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H";

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> {}",
        bot->GetGUID().ToString().c_str(),
        teamPrefix,
        bot->GetLevel(),
        bot->GetName().c_str(),
        releaseMsg.c_str());
}

// AutoReleaseSpiritAction implementation
bool AutoReleaseSpiritAction::Execute(Event /*event*/)
{
    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()) &&
        !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        // Persistent gatherers use the same CMSG_REPOP_REQUEST a real client sends, but do not
        // inherit the generic autonomous-bot durability repair side effect. The policy reserves
        // this once per death and supplies the retry backoff, so repeated dead-engine ticks cannot
        // spam release packets.
        AutoWowGather::DeathRecoveryTransition const recovery = AutoWowGather::PlanDeathRecovery(
            bot->GetGUID().GetCounter(), bot->IsAlive(), bot->HasPlayerFlag(PLAYER_FLAGS_GHOST),
            bot->HasCorpse(), false, bot->isMoving());
        if (recovery.action == AutoWowGather::DeathRecoveryAction::ReleaseSpirit)
        {
            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);
            botAI->SetNextCheckDelay(1000);
            return true;
        }

        if (recovery.action == AutoWowGather::DeathRecoveryAction::Wait)
        {
            botAI->SetNextCheckDelay(std::max<uint32>(1, recovery.delaySeconds) * 1000);
            return true;
        }

        return false;
    }

    if (IsReleasedGroupedInstanceCorpse(bot) &&
        bot->GetMapId() != bot->GetCorpseLocation().GetMapId())
    {
        // The dead engine's default strategy owns AreaTriggerAction. RepopAction arms that normal
        // client-equivalent ingress path and never teleports or resurrects the ghost directly.
        return botAI->DoSpecificAction("repop", Event("dungeon corpse run"), true);
    }

    bool const groupedInstanceDeath = IsGroupedInstanceDeath(bot);
    IncrementDeathCount();
    if (groupedInstanceDeath)
    {
        // FindCorpseAction's legacy five-death branch instant-revives autonomous bots. A grouped
        // dungeon/raid corpse is instead governed by the finite entrance-attempt policy below.
        context->GetValue<uint32>("death count")->Set(0);
    }
    bot->DurabilityRepairAll(false, 1.0f, false);
    LogRelease("auto released");

    WorldPacket packet(CMSG_REPOP_REQUEST);
    packet << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(packet);

    LogRelease("releases spirit");

    if (bot->InBattleground())
    {
        return HandleBattlegroundSpiritHealer();
    }

    if (groupedInstanceDeath)
    {
        uint32 const corpseMap = bot->GetCorpseLocation().GetMapId();
        if (IsReleasedGroupedInstanceCorpse(bot) && bot->GetMapId() != corpseMap)
        {
            ResetDungeonCorpseRunState(bot, corpseMap, time(nullptr));
            if (botAI->DoSpecificAction("repop", Event("dungeon corpse run"), true))
                return true;
        }
    }

    botAI->SetNextCheckDelay(1000);
    return true;
}

bool AutoReleaseSpiritAction::isUseful()
{
    if (!bot->isDead() || bot->InArena())
        return false;

    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()) &&
        !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return true;

    if (bot->InBattleground())
        return ShouldDelayBattlegroundRelease();

    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        // Keep the bounded instance recovery action authoritative while the ghost is outside.
        // This prevents the legacy cross-map FindCorpseAction from treating corpse coordinates on
        // another map as local coordinates or falling back to an instant autonomous revive.
        return IsReleasedGroupedInstanceCorpse(bot) &&
            bot->GetMapId() != bot->GetCorpseLocation().GetMapId();
    }

    return ShouldAutoRelease();
}

bool AutoReleaseSpiritAction::HandleBattlegroundSpiritHealer()
{
    constexpr uint32_t RESURRECT_DELAY = 15;
    const time_t now = time(nullptr);

    if ((now - m_bgGossipTime < RESURRECT_DELAY) &&
        bot->HasAura(SPELL_WAITING_FOR_RESURRECT))
    {
        return false;
    }

    float bgRange = 2000.0f;
    GuidVector npcs = NearestNpcsValue(botAI, bgRange);
    Unit* spiritHealer = nullptr;

    for (auto const& guid : npcs)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (unit && unit->IsFriendlyTo(bot) && unit->IsSpiritService())
        {
            spiritHealer = unit;
            break;
        }
    }

    if (!spiritHealer)
        return false;

    if (bot->GetDistance(spiritHealer) >= INTERACTION_DISTANCE)
    {
        // Bot needs to actually click spirit-healer in BG to get res timer going
        // and in IOC it's not within clicking range when they res in own base

        // Teleport to nearest friendly Spirit Healer when not currently in range of one.
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(bot->GetMapId(), spiritHealer->GetPositionX(), spiritHealer->GetPositionY(), spiritHealer->GetPositionZ(), 0.f);
        RESET_AI_VALUE(bool, "combat::self target");
        RESET_AI_VALUE(WorldPosition, "current position");
    }
    else if (!IsSelfBot(bot))
    {
        m_bgGossipTime = now;
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << spiritHealer->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);
    }

    return true;
}

bool AutoReleaseSpiritAction::ShouldAutoRelease() const
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    bool const sameInstanceDungeon = bot->GetMap() && bot->GetMap()->IsDungeon();
    bool aliveHealerPresent = false;
    if (sameInstanceDungeon)
    {
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            if (member && member != bot && member->IsAlive() && member->IsInWorld() &&
                member->GetMapId() == bot->GetMapId() &&
                member->GetInstanceId() == bot->GetInstanceId() &&
                PlayerbotAI::IsHeal(member, true))
            {
                aliveHealerPresent = true;
                break;
            }
        }
    }

    if (DungeonDeathRecovery::ShouldWaitForHealer(true, sameInstanceDungeon, aliveHealerPresent))
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    if (!groupLeader || groupLeader == bot)
        return true;

    if (!IsRealPlayer(botAI->GetMaster()))
        return true;

    if (IsRealPlayer(botAI->GetMaster()) &&
        groupLeader->GetMapId() == bot->GetMapId() &&
        bot->GetMap() &&
        (bot->GetMap()->IsRaid() || bot->GetMap()->IsDungeon()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(
        AI_VALUE2(float, "distance", "group leader"),
        sPlayerbotAIConfig.sightDistance);
}

bool AutoReleaseSpiritAction::ShouldDelayBattlegroundRelease() const
{
    // The below delays release to spirit with 6 seconds.
    // This prevents currently casted (ranged) spells to be re-directed to the died bot's ghost.

    // If the bot already is a spirit, reset release time and return true
    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        botAI->bgReleaseAttemptTime = 0;
        return true;
    }

    // Delay release to spirit.
    const time_t now = time(nullptr);
    constexpr time_t RELEASE_DELAY = 6;

    if (botAI->bgReleaseAttemptTime == 0)
        botAI->bgReleaseAttemptTime = now;

    if (now - botAI->bgReleaseAttemptTime < RELEASE_DELAY)
        return false;

    botAI->bgReleaseAttemptTime = 0;
    return true;
}

bool RepopAction::Execute(Event /*event*/)
{
    if (IsReleasedGroupedInstanceCorpse(bot))
    {
        time_t const now = time(nullptr);
        uint32 const corpseMap = bot->GetCorpseLocation().GetMapId();
        DungeonCorpseRunState& state = GetDungeonCorpseRunState(bot, corpseMap, now);
        ReleasedCorpseApproachPolicy::IngressPortalSelection const portalSelection =
            FindIngressPortal(bot, corpseMap);
        AreaTrigger const* portal = portalSelection.found ?
            sObjectMgr->GetAreaTrigger(portalSelection.candidate.triggerId) : nullptr;
        bool const insidePortal = portal && bot->IsInAreaTriggerRadius(portal, 0.0f);

        DungeonDeathRecovery::ReleasedRecoveryFacts facts;
        facts.grouped = true;
        facts.releasedGhost = true;
        facts.corpseInDungeonOrRaid = true;
        facts.onCorpseMap = bot->GetMapId() == corpseMap;
        facts.ingressPortalAvailable = portal != nullptr;
        facts.insideIngressPortal = insidePortal;
        facts.movementOrTransferInProgress = bot->isMoving() || bot->IsBeingTeleported();
        facts.retryBackoffActive = state.retryAfter > now;
        facts.entranceAttempts = state.entranceAttempts;
        facts.elapsedSeconds = CorpseRunElapsedSeconds(state, now);

        DungeonDeathRecovery::ReleasedRecoveryStep const step =
            DungeonDeathRecovery::EvaluateReleasedRecovery(facts);
        LastMovement& portalMovement = context->GetValue<LastMovement&>("last area trigger")->Get();

        switch (step)
        {
            case DungeonDeathRecovery::ReleasedRecoveryStep::ResumeCorpseApproach:
                portalMovement.lastAreaTrigger = 0;
                return false;
            case DungeonDeathRecovery::ReleasedRecoveryStep::EntranceInProgress:
                return true;
            case DungeonDeathRecovery::ReleasedRecoveryStep::RetryBackoff:
                botAI->SetNextCheckDelay(static_cast<uint32>((state.retryAfter - now) * IN_MILLISECONDS));
                return true;
            case DungeonDeathRecovery::ReleasedRecoveryStep::Exhausted:
                portalMovement.lastAreaTrigger = 0;
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                if (!state.exhaustedLogged)
                {
                    state.exhaustedLogged = true;
                    LOG_ERROR("playerbots",
                        "[DungeonDeathRecovery] bot={} phase=exhausted corpse_map={} current_map={} attempts={} elapsed={} action=idle",
                        bot->GetName(), corpseMap, bot->GetMapId(),
                        static_cast<uint32>(state.entranceAttempts),
                        facts.elapsedSeconds);
                }
                botAI->SetNextCheckDelay(30 * IN_MILLISECONDS);
                return true;
            case DungeonDeathRecovery::ReleasedRecoveryStep::MissingEntrance:
                portalMovement.lastAreaTrigger = 0;
                RecordEntranceAttempt(state, now);
                LOG_ERROR("playerbots",
                    "[DungeonDeathRecovery] bot={} phase=ingress_missing corpse_map={} current_map={} attempt={} action=backoff",
                    bot->GetName(), corpseMap, bot->GetMapId(),
                    static_cast<uint32>(state.entranceAttempts));
                return true;
            case DungeonDeathRecovery::ReleasedRecoveryStep::ApproachEntrance:
            {
                ReleasedCorpseApproachPolicy::IngressApproachSelection const approach =
                    FindReachableIngressApproach(bot, *portal);
                if (!approach.found)
                {
                    portalMovement.lastAreaTrigger = 0;
                    RecordEntranceAttempt(state, now);
                    LOG_ERROR("playerbots",
                        "[DungeonDeathRecovery] bot={} phase=approach_failed reason=no_reachable_inside_candidate trigger={} corpse_map={} current_map={} attempt={} candidate_count={} probe_count={} action=backoff",
                        bot->GetName(), portalSelection.candidate.triggerId, corpseMap,
                        bot->GetMapId(), static_cast<uint32>(state.entranceAttempts),
                        approach.candidateCount, approach.probesEvaluated);
                    return true;
                }

                portalMovement.lastAreaTrigger = portalSelection.candidate.triggerId;
                float const distance = bot->GetExactDist(
                    approach.endpointX, approach.endpointY, approach.endpointZ);
                bool const moved = MoveTo(bot->GetMapId(), approach.endpointX,
                                          approach.endpointY, approach.endpointZ,
                                          false, false, true);
                if (moved)
                {
                    WaitForReach(distance);
                    LOG_INFO("playerbots",
                        "[DungeonDeathRecovery] bot={} phase=approach trigger={} corpse_map={} current_map={} distance={} path_distance={} candidate={} candidate_count={} probe_count={} attempt={} moved=true",
                        bot->GetName(), portalSelection.candidate.triggerId, corpseMap,
                        bot->GetMapId(), distance, approach.pathDistance,
                        approach.candidate.candidateIndex, approach.candidateCount,
                        approach.probesEvaluated,
                        static_cast<uint32>(state.entranceAttempts) + 1);
                    return true;
                }

                portalMovement.lastAreaTrigger = 0;
                RecordEntranceAttempt(state, now);
                LOG_ERROR("playerbots",
                    "[DungeonDeathRecovery] bot={} phase=approach_failed reason=move_to_rejected trigger={} corpse_map={} current_map={} attempt={} candidate={} candidate_count={} probe_count={} action=backoff",
                    bot->GetName(), portalSelection.candidate.triggerId, corpseMap,
                    bot->GetMapId(), static_cast<uint32>(state.entranceAttempts),
                    approach.candidate.candidateIndex, approach.candidateCount,
                    approach.probesEvaluated);
                return true;
            }
            case DungeonDeathRecovery::ReleasedRecoveryStep::ActivateEntrance:
            {
                portalMovement.lastAreaTrigger = portalSelection.candidate.triggerId;
                RecordEntranceAttempt(state, now);
                bool const activated = botAI->DoSpecificAction("area trigger", Event("dungeon corpse run"), true);
                if (!activated)
                    portalMovement.lastAreaTrigger = 0;
                LOG_INFO("playerbots",
                    "[DungeonDeathRecovery] bot={} phase=activate trigger={} corpse_map={} map_after={} attempt={} packet={} transfer={}",
                    bot->GetName(), portalSelection.candidate.triggerId, corpseMap,
                    bot->GetMapId(), static_cast<uint32>(state.entranceAttempts), activated,
                    bot->IsBeingTeleported());
                return true;
            }
            case DungeonDeathRecovery::ReleasedRecoveryStep::OrdinaryDeath:
                break;
        }
    }

    GraveyardStruct const* graveyard = GetGrave(
        AI_VALUE(uint32, "death count") > 10 ||
        CalculateDeadTime() > 30 * MINUTE
    );

    if (!graveyard)
        return false;

    PerformGraveyardTeleport(graveyard);
    return true;
}

bool RepopAction::isUseful()
{
    return !bot->InBattleground() && !bot->IsAlive();
}

int64 RepopAction::CalculateDeadTime() const
{
    if (Corpse* corpse = bot->GetCorpse())
        return time(nullptr) - corpse->GetGhostTime();

    return bot->isDead() ? 0 : 60 * MINUTE;
}

void RepopAction::PerformGraveyardTeleport(GraveyardStruct const* graveyard) const
{
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(graveyard->Map, graveyard->x, graveyard->y, graveyard->z, 0.f);
    RESET_AI_VALUE(bool, "combat::self target");
    RESET_AI_VALUE(WorldPosition, "current position");
}

// SelfResurrectAction implementation for Warlock's Soulstone Resurrection/Shaman's Reincarnation
bool SelfResurrectAction::Execute(Event /*event*/)
{
    if (!bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        WorldPacket packet(CMSG_SELF_RES);
        bot->GetSession()->HandleSelfResOpcode(packet);
        return true;
    }
    return false;
}
bool SelfResurrectAction::isUseful()
{
    return !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
}
