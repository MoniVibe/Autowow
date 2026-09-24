/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"

#include "AutoWowBridge.h"
#include "AutoWowQuestLedger.h"
#include "DeathLoopBreaker.h"
#include "DungeonPathSafety.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "MapMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PersistentCorpseApproachPolicy.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "SurvivalRecovery.h"
#include "Corpse.h"
#include "../../World/Gathering/GatheringWorkerState.h"

#include <cmath>

namespace
{
constexpr uint32 PersistentCorpseRejectedPathTypes =
    PATHFIND_SHORTCUT | PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH |
    PATHFIND_SHORT | PATHFIND_FARFROMPOLY;

CorpseRouteRetryPolicy::RouteEndpoint ToRouteEndpoint(GraveyardStruct const* grave)
{
    if (!grave)
        return {};

    return {grave->ID, grave->Map, grave->x, grave->y, grave->z};
}
}

bool ReviveFromCorpseAction::Execute(Event event)
{
    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.farDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()))
    {
        bool const corpseNear = corpse->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS - 5.0f, true);
        AutoWowGather::DeathRecoveryTransition const recovery = AutoWowGather::PlanDeathRecovery(
            bot->GetGUID().GetCounter(), false, true, true, corpseNear, bot->isMoving());
        if (recovery.action != AutoWowGather::DeathRecoveryAction::ReclaimCorpse)
        {
            if (recovery.action == AutoWowGather::DeathRecoveryAction::Wait)
                botAI->SetNextCheckDelay((recovery.delaySeconds ? recovery.delaySeconds : 1) * 1000);
            return false;
        }

        // This is the same reclaim opcode emitted by the normal client. The worker policy has
        // already verified the corpse-near phase and reserved this attempt, preventing repeated
        // reclaim packets while the core's corpse timer is still active.
        LOG_DEBUG("playerbots", "[GatherWorkerRecovery] bot={} reclaiming corpse",
                  bot->GetGUID().ToString().c_str());
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        WorldPacket packet(CMSG_RECLAIM_CORPSE);
        packet << bot->GetGUID();
        bot->GetSession()->HandleReclaimCorpseOpcode(packet);
        return true;
    }

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.farDistance))
            return false;
    }

    // AutoWow.Survival.SafeRevive: a planned death reclaims only on its chosen spot (FindCorpseAction walks
    // the ghost there) and a spirit-healer plan never reclaims. No plan = the legacy rules below.
    if (AutoWowSafeRevive::Enabled())
    {
        AutoWowSafeRevive::Gate const gate = AutoWowSafeRevive::ReclaimGate(bot, corpse);
        if (gate == AutoWowSafeRevive::Gate::Hold)
            return false;
        if (gate == AutoWowSafeRevive::Gate::Spirit)
            return botAI->DoSpecificAction("spirit healer", Event("safe revive"), true);
        if (gate == AutoWowSafeRevive::Gate::Reclaim)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            WorldPacket packet(CMSG_RECLAIM_CORPSE);
            packet << bot->GetGUID();
            bot->GetSession()->HandleReclaimCorpseOpcode(packet);
            return true;
        }
    }

    if (!botAI->HasRealPlayerMaster())
    {
        uint32 dCount = AI_VALUE(uint32, "death count");

        if (dCount >= 5)
        {
            return botAI->DoSpecificAction("spirit healer");
        }
    }

    // AutoWow.DeathLoop: an escalated death never reclaims the body next to its killer. Falls through to
    // the ordinary reclaim only when the spirit-healer route cannot act at all.
    if (AutoWowDeathLoop::Enabled() && AutoWowDeathLoop::WantsSpiritHealer(bot->GetGUID().GetCounter()) &&
        botAI->DoSpecificAction("spirit healer", Event("death loop"), true))
        return true;

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()))
    {
        bool const corpseNear = corpse &&
            corpse->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS - 5.0f, true);
        AutoWowGather::DeathRecoveryTransition const recovery = AutoWowGather::PlanDeathRecovery(
            bot->GetGUID().GetCounter(), bot->IsAlive(), bot->HasPlayerFlag(PLAYER_FLAGS_GHOST),
            bot->HasCorpse(), corpseNear, bot->isMoving());

        if (recovery.action == AutoWowGather::DeathRecoveryAction::ReleaseSpirit)
        {
            // Find-corpse can win the dead trigger before the generic auto-release trigger. Keep
            // the worker path ordinary and repair-free in that ordering as well.
            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);
            botAI->SetNextCheckDelay(1000);
            return true;
        }

        if (recovery.action == AutoWowGather::DeathRecoveryAction::ReclaimCorpse)
        {
            LOG_DEBUG("playerbots", "[GatherWorkerRecovery] bot={} reclaiming corpse",
                      bot->GetGUID().ToString().c_str());
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            WorldPacket packet(CMSG_RECLAIM_CORPSE);
            packet << bot->GetGUID();
            bot->GetSession()->HandleReclaimCorpseOpcode(packet);
            return true;
        }

        if (recovery.action == AutoWowGather::DeathRecoveryAction::Wait)
        {
            botAI->SetNextCheckDelay((recovery.delaySeconds ? recovery.delaySeconds : 1) * 1000);
            return true;
        }
    }

    bool const persistentNoTeleport = AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter());
    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()) && !persistentNoTeleport)
    {
        // A worker that was not deployed through the persistent lifecycle has no authority to
        // enter the legacy corpse branch. Stop and expose the missing lifecycle guard rather than
        // permitting its ordinary-bot teleport fallback.
        AutoWowGather::SetRouteStatus(bot->GetGUID().GetCounter(), "blocked",
                                      "worker_death_recovery_blocked", "no_teleport_policy_missing");
        return false;
    }

    // AutoWow.DeathLoop: an escalated death (same-spot loop, or a killer far above the bot's level) takes
    // the spirit healer instead of the corpse run - ahead of the legacy five-death revive-in-place, which
    // is itself a loop at a hostile spawn. Falls through to the ordinary corpse run if the spirit-healer
    // route cannot act at all.
    if (AutoWowDeathLoop::Enabled() && bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) &&
        AutoWowDeathLoop::WantsSpiritHealer(bot->GetGUID().GetCounter()) &&
        botAI->DoSpecificAction("spirit healer", Event("death loop"), true))
        return true;

    // Player::GetCorpse() only searches the player's current Map.  A released ghost whose body is
    // on another continent or inside an unloaded instance still has an authoritative persisted
    // corpse location, so hand that state to the ordinary local spirit-healer route.
    if (!corpse && persistentNoTeleport && bot->HasCorpse() &&
        bot->GetCorpseLocation().GetMapId() != bot->GetMapId())
    {
        LOG_WARN("playerbots",
            "[PersistentCorpseRecovery] bot={} phase=cross_map_corpse_location current_map={} "
            "corpse_map={} teleport_fallback=false",
            bot->GetName(), bot->GetMapId(), bot->GetCorpseLocation().GetMapId());
        return botAI->DoSpecificAction(
            "spirit healer", Event("persistent cross-map corpse-location recovery"), true);
    }

    if (!corpse)
        return false;

    if (persistentNoTeleport && corpse->GetMapId() != bot->GetMapId())
    {
        // A released player cannot walk directly to a corpse on another map.  Hand the bot to the
        // bounded local spirit-healer route immediately; never let the legacy corpse timeout move
        // it across a continent or instance boundary.
        LOG_WARN("playerbots",
            "[PersistentCorpseRecovery] bot={} phase=cross_map_corpse current_map={} corpse_map={} "
            "teleport_fallback=false",
            bot->GetName(), bot->GetMapId(), corpse->GetMapId());
        return botAI->DoSpecificAction("spirit healer", Event("persistent cross-map corpse recovery"), true);
    }

    // AutoWow.Survival.SafeRevive (default 0): the ghost walks to the least-threatened reachable spot within
    // reclaim reach, or takes the spirit healer after a repeat death with no safe spot. No plan = legacy.
    if (AutoWowSafeRevive::Enabled())
        if (std::optional<bool> const safe = SafeReviveApproach(corpse))
            return *safe;

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.farDistance)) return false;
    // }

    uint32 dCount = AI_VALUE(uint32, "death count");

    if (!botAI->HasRealPlayerMaster())
    {
        if (dCount >= 5)
        {
            // Persistent AutoWow bots must escape a corpse camp as a player would: walk as a
            // ghost to a spirit healer. The legacy random-bot path resurrects in place and resets
            // the counter, which can create an endless die/revive loop at the same hostile spawn.
            if (persistentNoTeleport)
                return botAI->DoSpecificAction("spirit healer", Event("persistent corpse recovery"), true);

            // LOG_INFO("playerbots", "Bot {} {}:{} <{}>: died too many times, was revived and teleported",
            //     bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
            //     bot->GetName().c_str());
            context->GetValue<uint32>("death count")->Set(0);
            // sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
            sRandomPlayerbotMgr.Revive(bot);
            return true;
        }
    }

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool moveToLeader = groupLeader && groupLeader != bot && leaderPos.fDist(corpsePos) < reclaimDist;

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        if (moveToLeader)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.spellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE)  // We have walked too long already.
            return false;
        else
        {
            GuidVector units = AI_VALUE(GuidVector, "possible targets no los");

            if (botPos.getUnitsAggro(units, bot) == 0)  // There are no mobs near.
                return false;
        }
    }

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.reactDistance)
    {
        if (moveToLeader)
            moveToPos = leaderPos;
        else if (!persistentNoTeleport)
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    bool moved = false;

    if (!botAI->AllowActivity(ALL_ACTIVITY))
    {
        uint32 delay = ServerFacade::instance().GetDistance2d(bot, corpse) /
                       bot->GetSpeed(MOVE_RUN);        // Time a bot would take to travel to it's corpse.
        delay = std::min(delay, uint32(10 * MINUTE));  // Cap time to get to corpse at 10 minutes.

        if (deadTime > delay)
        {
            if (persistentNoTeleport)
                return botAI->DoSpecificAction("spirit healer", Event("persistent corpse route timeout"), true);

            bot->GetMotionMaster()->Clear();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "corpse_run_teleport");
            bot->TeleportTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), 0);
        }

        moved = true;
    }
    else
    {
        if (bot->isMoving())
            moved = true;
        else
        {
            if (deadTime < 10 * MINUTE && dCount < 5)  // Look for corpse up to 30 minutes.
            {
                if (persistentNoTeleport && moveToPos.GetMapId() == bot->GetMapId())
                {
                    AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
                        bot, moveToPos.GetPositionX(), moveToPos.GetPositionY(),
                        moveToPos.GetPositionZ());
                    bool const hasEndpoint = probe.path.size() >= 2;
                    G3D::Vector3 endpoint;
                    if (hasEndpoint)
                        endpoint = probe.path.back();

                    float const sourceToEndpoint = hasEndpoint ? bot->GetExactDist(
                        endpoint.x, endpoint.y, endpoint.z) : 0.0f;
                    float const endpointToDestination = hasEndpoint ? std::sqrt(
                        std::pow(endpoint.x - moveToPos.GetPositionX(), 2.0f) +
                        std::pow(endpoint.y - moveToPos.GetPositionY(), 2.0f) +
                        std::pow(endpoint.z - moveToPos.GetPositionZ(), 2.0f)) : 0.0f;
                    bool const partialPathTypeAllowed =
                        (probe.pathType & PATHFIND_INCOMPLETE) != 0 &&
                        (probe.pathType & PersistentCorpseRejectedPathTypes) == 0;
                    PersistentCorpseApproachPolicy::Decision const decision =
                        PersistentCorpseApproachPolicy::Select({
                            true, probe.safe, partialPathTypeAllowed, hasEndpoint,
                            probe.allGroundSamplesValid, bot->GetExactDist(
                                moveToPos.GetPositionX(), moveToPos.GetPositionY(),
                                moveToPos.GetPositionZ()),
                            sourceToEndpoint, endpointToDestination});

                    if (decision == PersistentCorpseApproachPolicy::Decision::DirectDestination)
                    {
                        moved = MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(),
                            moveToPos.GetPositionY(), moveToPos.GetPositionZ(), false, false, true);
                    }
                    else if (decision == PersistentCorpseApproachPolicy::Decision::PartialEndpoint)
                    {
                        moved = MoveTo(bot->GetMapId(), endpoint.x, endpoint.y, endpoint.z,
                            false, false, true);
                        if (moved)
                        {
                            LOG_DEBUG("playerbots",
                                "[PersistentCorpseRecovery] bot={} phase=corpse_walk_segment "
                                "segment_distance={} remaining_distance={} teleport_fallback=false",
                                bot->GetName(), sourceToEndpoint, endpointToDestination);
                        }
                    }
                }
                else
                {
                    moved = MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(),
                        moveToPos.GetPositionY(), moveToPos.GetPositionZ(), false, false);
                }
            }

            if (!moved)
            {
                moved = botAI->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    return moved;
}

std::optional<bool> FindCorpseAction::SafeReviveApproach(Corpse* corpse)
{
    std::optional<AutoWowSafeRevive::Target> const t = AutoWowSafeRevive::PlanFor(bot, corpse);
    if (!t)
        return std::nullopt;
    if (t->plan == AutoWowSafeRevive::Plan::SpiritHealer)
    {
        if (botAI->DoSpecificAction("spirit healer", Event("safe revive"), true))
            return true;
        AutoWowSafeRevive::Abandon(bot->GetGUID().GetCounter());
        return std::nullopt;
    }
    if (bot->GetExactDist2d(t->x, t->y) <= float(AutoWowSafeRevive::kAtSpotYards))
    {
        if (bot->isMoving())
            bot->StopMoving();
        return false;  // on the spot: "corpse near" -> revive from corpse reclaims here
    }
    LastMovement& last = AI_VALUE(LastMovement&, "last movement");
    bool const ours = last.lastMoveToMapId == bot->GetMapId() &&
                      std::fabs(last.lastMoveToX - t->x) < 1.0f && std::fabs(last.lastMoveToY - t->y) < 1.0f;
    if (ours && (bot->isMoving() || IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL)))
        return true;
    // A refusal (a previous move still pending) is retried next tick; PlanFor's approach timeout hands a
    // hopeless approach back to the legacy corpse run.
    MoveTo(bot->GetMapId(), t->x, t->y, t->z, false, false, false, true);
    return true;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()))
        return bot->isDead();

    return bot->GetCorpse() ||
        (AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter()) && bot->HasCorpse());
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    else if (startZone && AI_VALUE(uint8, "durability"))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

bool SpiritHealerAction::Execute(Event /*event*/)
{
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        if (AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter()) && bot->HasCorpse() &&
            bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            return ExecuteNoTeleportCorpseRecovery(nullptr);
        }

        corpseRouteState_.Reset();
        botAI->TellError("I am not a spirit");
        return false;
    }

    if (AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter()))
    {
        // teleport_fallback=false: league-party and persistent-worker bots stay on the bounded
        // walking route and can never reach the legacy teleport branch below.
        return ExecuteNoTeleportCorpseRecovery(corpse);
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    GraveyardStruct const* ClosestGrave =
        GetGrave(dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10);

    if (!ClosestGrave)
        return false;

    CorpseRouteRetryPolicy::RouteEndpoint const legacyGrave = ToRouteEndpoint(ClosestGrave);
    if (TrySpiritHealerInteraction(legacyGrave))
        return true;

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)
        return true;

    // if (!botAI->HasActivePlayerMaster())
    // {
    context->GetValue<uint32>("death count")->Set(dCount + 1);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "ghost_graveyard_teleport");
    return bot->TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.f);
    // }

    // LOG_INFO("playerbots", "Bot {} {}:{} <{}> can't find a spirit healer", bot->GetGUID().ToString().c_str(),
    //          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    // botAI->TellError("Cannot find any spirit healer nearby");
    return false;
}

bool SpiritHealerAction::ExecuteNoTeleportCorpseRecovery(Corpse* corpse)
{
    using namespace CorpseRouteRetryPolicy;

    if (!corpse && !bot->HasCorpse())
        return false;

    // A remote corpse object is not loaded into the ghost's current Map.  The player-owned corpse
    // location remains authoritative.  Quantize that persisted location into a stable per-body key
    // so repeated action ticks share one retry budget without querying or mutating the database.
    WorldLocation const persistedCorpse = bot->GetCorpseLocation();
    auto const quantizedCoordinate = [](float coordinate)
    {
        return static_cast<std::uint64_t>(std::llround(coordinate * 4.0f)) & 0x1FFFFFu;
    };
    std::uint64_t const persistedCorpseKey =
        ((static_cast<std::uint64_t>(persistedCorpse.GetMapId()) & 0x3FFu) << 42u) |
        (quantizedCoordinate(persistedCorpse.GetPositionX()) << 21u) |
        quantizedCoordinate(persistedCorpse.GetPositionY());
    DeathIdentity const death = corpse ?
        DeathIdentity{corpse->GetGUID().GetCounter(), static_cast<std::int64_t>(corpse->GetGhostTime())} :
        DeathIdentity{bot->GetGUID().GetCounter(), static_cast<std::int64_t>(persistedCorpseKey)};
    GraveyardStruct const* localGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    RouteEndpoint localEndpoint = ToRouteEndpoint(localGrave);
    if (!localEndpoint.IsLocalTo(bot->GetMapId()))
        localEndpoint = {};
    corpseRouteState_.BeginDeath(death, localEndpoint);

    auto recordRouteFailure = [this](RouteEndpoint const& grave)
    {
        Transition const failure = corpseRouteState_.Observe(MovementObservation::RouteFailure);
        if (failure.emitTerminalReceipt)
        {
            LOG_ERROR("playerbots",
                "[PersistentCorpseRecovery] bot={} receipt=corpse_recovery_blocked_no_path grave_id={} "
                "route_attempts={} route_failures={} teleport_fallback=false",
                bot->GetName(), grave.graveId, corpseRouteState_.RouteAttempts(), corpseRouteState_.RouteFailures());
        }

        if (failure.blocked)
        {
            bot->StopMoving();
            botAI->SetNextCheckDelay(BlockedRetryDelayMs);
            return;
        }

        LOG_WARN("playerbots",
            "[PersistentCorpseRecovery] bot={} phase=spirit_healer_walk grave_id={} route_attempt={} "
            "route_failures={} teleport_fallback=false",
            bot->GetName(), grave.graveId, corpseRouteState_.RouteAttempts(), corpseRouteState_.RouteFailures());
        botAI->SetNextCheckDelay(RetryDelayMs(corpseRouteState_.RouteFailures()));
    };

    // A terminal state is checked before NPC interaction or movement. Once exhausted, this death
    // remains idle and fail closed even if later action ticks keep selecting spirit healer.
    if (corpseRouteState_.IsBlocked())
    {
        bot->StopMoving();
        botAI->SetNextCheckDelay(BlockedRetryDelayMs);
        return false;
    }

    RouteEndpoint const& grave = corpseRouteState_.Endpoint();
    if (grave.IsValid() && bot->GetMapId() == grave.mapId &&
        bot->GetDistance2d(grave.x, grave.y) < sPlayerbotAIConfig.sightDistance)
    {
        if (TrySpiritHealerInteraction(grave))
        {
            corpseRouteState_.Observe(MovementObservation::RecoverySucceeded);
            return true;
        }

        corpseRouteState_.Observe(MovementObservation::Waiting);
        botAI->SetNextCheckDelay(WaitingRetryDelayMs);
        return true;
    }

    if (grave.IsValid())
    {
        UpdateMovementState();
        MovementObservation const preflight =
            ClassifyPreflight(IsMovingAllowed(), IsDuplicateMove(grave.x, grave.y, grave.z),
                              IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL), bot->isMoving());
        if (preflight != MovementObservation::Ready)
        {
            corpseRouteState_.Observe(preflight);
            botAI->SetNextCheckDelay(WaitingRetryDelayMs);
            return true;
        }

        // The previous accepted route is a genuine failure only after its duplicate/wait/moving
        // gates have all cleared and the ghost is still not at the pinned grave.
        if (corpseRouteState_.IsRouteInProgress())
        {
            recordRouteFailure(grave);
            return false;
        }

        if (MoveTo(grave.mapId, grave.x, grave.y, grave.z, false, false))
        {
            corpseRouteState_.Observe(MovementObservation::RouteStarted);
            LOG_DEBUG("playerbots",
                "[PersistentCorpseRecovery] bot={} phase=spirit_healer_walk grave_id={} route_attempt={} "
                "route_failures={} teleport_fallback=false",
                bot->GetName(), grave.graveId, corpseRouteState_.RouteAttempts(),
                corpseRouteState_.RouteFailures());
            return true;
        }
    }

    recordRouteFailure(grave);
    return false;
}

bool SpiritHealerAction::TrySpiritHealerInteraction(CorpseRouteRetryPolicy::RouteEndpoint const& grave)
{
    if (!grave.IsValid() || bot->GetMapId() != grave.mapId ||
        bot->GetDistance2d(grave.x, grave.y) >= sPlayerbotAIConfig.sightDistance)
        return false;

    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit || !unit->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER))
            continue;

        LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                  bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
        PlayerbotChatHandler ch(bot);
        bot->ResurrectPlayer(0.5f);
        bot->SpawnCorpseBones();
        context->GetValue<Unit*>("current target")->Set(nullptr);
        bot->SetTarget();
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

        uint32 const deathCount = AI_VALUE(uint32, "death count");
        if (deathCount > 20)
            context->GetValue<uint32>("death count")->Set(0);

        return true;
    }

    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
