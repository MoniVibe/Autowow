/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATOR_H
#define PLAYERBOTS_DUNGEONNAVIGATOR_H

#include "AttackAction.h"
#include "DungeonGatePolicy.h"
#include "DungeonRoutePolicy.h"
#include "DungeonRouteReconnectPolicy.h"
#include "Strategy.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct DungeonNavigatorRoutePoint
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Encounter index the bot's dungeon navigator last selected in this map instance, or -1. Any thread.
int32 GetDungeonNavigatorTargetEncounter(uint32 botGuid, uint32 mapId, uint32 instanceId);
// Mask of the encounters the bot's navigator last set aside (gate_unavailable) in this map instance, or 0. Any thread.
uint32 GetDungeonNavigatorUnavailableMask(uint32 botGuid, uint32 mapId, uint32 instanceId);

class DungeonNavigatorStrategy : public Strategy
{
public:
    explicit DungeonNavigatorStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dungeon navigator"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
};

class DungeonNavigateNextEncounterAction : public AttackAction
{
public:
    explicit DungeonNavigateNextEncounterAction(PlayerbotAI* botAI)
        : AttackAction(botAI, "dungeon navigate next encounter") {}

    bool Execute(Event event) override;

private:
    void ResetTravelRoute();
    void ReplanTravelRoute();
    void BlockTravelRoute(char const* reason);

    uint32 nextScanTime = 0;
    std::vector<DungeonNavigatorRoutePoint> travelRoute;
    uint32 travelRouteEncounterId = 0;
    uint32 travelRouteSpawnId = 0;
    uint32 travelRouteMapId = 0;
    uint32 travelRouteInstanceId = 0;
    std::size_t travelRouteNextIndex = 0;
    bool travelRouteInitialized = false;
    bool travelRouteBlocked = false;
    char const* travelRouteBlockedReason = "none";
    // ConvoyV2: DungeonRoute::Points row of travelRoute[0] when the route is a curated leg, else NoPoint.
    std::size_t travelRouteCuratedRow = DungeonRoute::NoPoint;
    // ConvoyV2 curated direct points: the leader's pending step, followers' pending steps (member guid ->
    // (row, start ms)), and failed (mover guid, row) steps, which are never retried in directStepInstanceId.
    std::size_t directStepRow = DungeonRoute::NoPoint;
    uint32 directStepStartMs = 0;
    std::map<uint32, std::pair<std::size_t, uint32>> directStepFollowers;
    std::set<std::pair<uint32, std::size_t>> directStepFailed;
    uint32 directStepMapId = 0;
    uint32 directStepInstanceId = 0;
    uint32 lastTravelEncounterId = 0;
    uint32 lastTravelSpawnId = 0;
    std::size_t lastTravelWaypointIndex = 0;
    float lastTravelWaypointX = 0.0f;
    float lastTravelWaypointY = 0.0f;
    float lastTravelWaypointZ = 0.0f;
    float lastTravelBotX = 0.0f;
    float lastTravelBotY = 0.0f;
    float lastTravelBotZ = 0.0f;
    uint8 travelNoProgressRetries = 0;
    uint8 travelNoProgressReplans = 0;
    DungeonRouteReconnect::WalkRevalidationState walkRevalidationState;
    // Active only while a follower is using backward reanchor recovery on this cached route.
    // The remembered route floor prevents a later failed scan from selecting an older point.
    std::map<uint32, std::size_t> convoyBackwardReanchorFloors;
    // ConvoyV2: member guid -> (leader frontier, route index) the follower last settled on.
    std::map<uint32, std::pair<std::size_t, std::size_t>> convoySettledRouteFloors;
    // ConvoyV2: route index of the leader's current navmesh-gap direct hop and its attempts.
    std::size_t directHopRouteIndex = DungeonRouteReconnect::NoSelection;
    uint8 directHopAttempts = 0;
    // AutoWow.DungeonNav.Gates: DungeonGate::Steps row index -> runtime, for gateMapId/gateInstanceId.
    std::map<std::size_t, DungeonGate::StepRuntime> gateRuntime;
    // Last INFO line time per (row, per-scan gate result), DungeonGate::ShouldLog.
    std::map<std::pair<std::size_t, std::string>, uint32> gateLogLast;
    uint32 gateMapId = 0;
    uint32 gateInstanceId = 0;
    // Instance of the last selection when it was a gate row, else 0: the party-cohesion straggler walk.
    uint32 gateGoalInstanceId = 0;
    // Encounters already logged as set aside (gate_unavailable) in gateMapId/gateInstanceId.
    std::set<uint32> gateUnavailableLogged;
};

#endif
