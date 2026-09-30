/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_TRAVEL_WALK_H
#define AUTOWOW_QUEST_TRAVEL_WALK_H

#include "DungeonPathSafety.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QuestGiverTravelFeedback.h"
#include "TravelMgr.h"
#include "TravelNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AutoWowQuestGiverTravel
{
struct QuestWalkProbeSelection
{
    std::optional<AutoWowDungeonPath::ProbeResult> probe;
    QuestWalkProbeDiagnostics diagnostics{};
};

// Errands may use a TravelMgr segment only when the route graph reaches the exact same-map town
// without a portal, transport, taxi, teleport, or map boundary. This certifies graph topology; the
// returned ProbeResult separately certifies the one live-source segment that will actually execute.
inline constexpr std::size_t kMaxErrandsWalkRoutePoints = 2048U;
inline constexpr float kErrandsWalkRouteEndpointTolerance = 2.5f;

enum class ErrandsWalkProofKind : std::uint8_t
{
    None,
    Direct,
    TravelMgr
};

enum class ErrandsWalkTopology : std::uint8_t
{
    None,
    Direct,
    AllWalk,
    Empty,
    TooLong,
    MapChange,
    Transition,
    InvalidCoordinates,
    WrongEndpoint
};

inline char const* ErrandsWalkProofKindName(ErrandsWalkProofKind kind)
{
    switch (kind)
    {
        case ErrandsWalkProofKind::Direct: return "direct";
        case ErrandsWalkProofKind::TravelMgr: return "travel_mgr";
        case ErrandsWalkProofKind::None: default: return "none";
    }
}

inline char const* ErrandsWalkTopologyName(ErrandsWalkTopology topology)
{
    switch (topology)
    {
        case ErrandsWalkTopology::Direct: return "direct";
        case ErrandsWalkTopology::AllWalk: return "all_walk";
        case ErrandsWalkTopology::Empty: return "empty";
        case ErrandsWalkTopology::TooLong: return "too_long";
        case ErrandsWalkTopology::MapChange: return "map_change";
        case ErrandsWalkTopology::Transition: return "transition";
        case ErrandsWalkTopology::InvalidCoordinates: return "invalid_coordinates";
        case ErrandsWalkTopology::WrongEndpoint: return "wrong_endpoint";
        case ErrandsWalkTopology::None: default: return "none";
    }
}

inline ErrandsWalkTopology ClassifyErrandsWalkTopology(
    std::vector<PathNodePoint> const& route, WorldPosition const& destination,
    std::size_t maximumPoints = kMaxErrandsWalkRoutePoints)
{
    if (route.empty())
        return ErrandsWalkTopology::Empty;
    if (maximumPoints == 0U || route.size() > maximumPoints)
        return ErrandsWalkTopology::TooLong;

    auto finite = [](WorldPosition const& point)
    {
        return std::isfinite(point.GetPositionX()) && std::isfinite(point.GetPositionY()) &&
               std::isfinite(point.GetPositionZ());
    };
    if (!finite(destination))
        return ErrandsWalkTopology::InvalidCoordinates;

    for (PathNodePoint const& node : route)
    {
        if (!finite(node.point))
            return ErrandsWalkTopology::InvalidCoordinates;
        if (node.point.GetMapId() != destination.GetMapId())
            return ErrandsWalkTopology::MapChange;
        if (node.type != NODE_PREPATH && node.type != NODE_PATH && node.type != NODE_NODE)
            return ErrandsWalkTopology::Transition;
    }

    WorldPosition const& endpoint = route.back().point;
    float const dx = endpoint.GetPositionX() - destination.GetPositionX();
    float const dy = endpoint.GetPositionY() - destination.GetPositionY();
    float const dz = endpoint.GetPositionZ() - destination.GetPositionZ();
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kErrandsWalkRouteEndpointTolerance)
        return ErrandsWalkTopology::WrongEndpoint;
    return ErrandsWalkTopology::AllWalk;
}

struct ErrandsWalkProbeSelection
{
    std::optional<AutoWowDungeonPath::ProbeResult> probe;
    ErrandsWalkProofKind kind = ErrandsWalkProofKind::None;
    ErrandsWalkTopology topology = ErrandsWalkTopology::None;
    std::uint32_t routePointCount = 0;
};

// Estimate the remaining distance to the exact destination along the already selected TravelMgr
// walk prefix. This is deliberately a polyline measure, not a new path search: it lets a route
// that initially bends around terrain prove forward progress while retaining the route's exact
// destination and transition boundary. The caller supplies the freshly probed endpoint, so a
// partial probe cannot claim the progress of an unreachable stored waypoint.
inline float TravelMgrRouteRemainingDistance(
    Waypoint const& from, std::vector<PathNodePoint> const& route, std::size_t firstIndex,
    std::size_t walkPrefixEnd, Waypoint const& destination)
{
    if (firstIndex >= walkPrefixEnd || walkPrefixEnd > route.size() ||
        from.mapId != destination.mapId || !IsFinite(from) || !IsFinite(destination))
        return std::numeric_limits<float>::quiet_NaN();

    auto waypointAt = [&](std::size_t index) -> Waypoint
    {
        PathNodePoint const& node = route[index];
        return {node.point.GetMapId(), node.point.GetPositionX(), node.point.GetPositionY(),
                node.point.GetPositionZ()};
    };

    Waypoint const first = waypointAt(firstIndex);
    Waypoint const last = waypointAt(walkPrefixEnd - 1U);
    if (first.mapId != from.mapId || last.mapId != from.mapId || !IsFinite(first) || !IsFinite(last))
        return std::numeric_limits<float>::quiet_NaN();

    float remaining = Distance2d(from, first);
    for (std::size_t index = firstIndex; index + 1U < walkPrefixEnd; ++index)
    {
        Waypoint const current = waypointAt(index);
        Waypoint const next = waypointAt(index + 1U);
        if (current.mapId != from.mapId || next.mapId != from.mapId || !IsFinite(current) ||
            !IsFinite(next))
            return std::numeric_limits<float>::quiet_NaN();
        remaining += Distance2d(current, next);
    }
    remaining += Distance2d(last, destination);
    return std::isfinite(remaining) ? remaining : std::numeric_limits<float>::quiet_NaN();
}

inline PathFacts BuildPathFacts(AutoWowDungeonPath::ProbeResult const& probe)
{
    return {
        true,
        probe.safe,
        (probe.pathType & PATHFIND_NORMAL) != 0,
        probe.pathType,
        probe.path.size(),
        probe.endpointDistance,
        probe.pathLength,
        probe.allGroundSamplesValid,
        probe.mode == "ground_line" && probe.safe && probe.allGroundSamplesValid};
}

// Return the complete probe itself. Callers must pass this exact path to WalkPrepared; returning
// only the requested endpoint would discard the safety proof and ask a second movement layer to
// re-plan it.
inline std::optional<AutoWowDungeonPath::ProbeResult> SelectCompleteWalkProbe(
    Player* bot, WorldPosition const& destination)
{
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
        bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    if (!IsCompleteReprobe(BuildPathFacts(probe)))
        return std::nullopt;
    return probe;
}

// Select one freshly probed segment from this exact TravelMgr route. Keeping the route as an
// argument lets errands certify the complete graph topology and derive its executable segment from
// the same immutable route snapshot. Quest callers retain their existing public selector below.
inline std::optional<AutoWowDungeonPath::ProbeResult> SelectTravelMgrWalkProbeFromRoute(
    Player* bot, WorldPosition const& destination, WorldPosition const& start,
    std::vector<PathNodePoint> const& route)
{
    TravelMgrDiagnostics diagnostics;
    diagnostics.unattachedStartAllowed = true;
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    // Quest travel is already gated by a fresh complete probe and WalkPrepared. It may therefore
    // use a longer bounded graph segment than the generic re-anchor selector's 150 yd default;
    // otherwise a legitimate route whose first graph point is 170-235 yd away is never even
    // probed, leaving a no-teleport quest finisher parked at a safe but distant frontier.
    constexpr float questTravelMaximumStep = 300.0f;
    float const preferredMaximumStep = questTravelMaximumStep;
    Waypoint const startPoint{
        start.GetMapId(), start.GetPositionX(), start.GetPositionY(), start.GetPositionZ()};
    Waypoint const destinationPoint{
        destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ()};
    float const currentTargetDistance = Distance2d(startPoint, destinationPoint);

    std::vector<TravelMgrReanchorCandidateFacts> candidates;
    std::vector<std::optional<AutoWowDungeonPath::ProbeResult>> preparedProbes;
    diagnostics.pathPointCount = static_cast<std::uint32_t>(route.size());
    if (route.empty())
    {
        diagnostics.reason = TravelMgrDiagnosticReason::PathEmpty;
        RecordTravelMgrDiagnostics(bot->GetGUID().GetCounter(), diagnostics);
        return std::nullopt;
    }

    // The route prefix is a hard same-map/walk boundary. Do not let a later transition or a
    // cross-map tail participate in the routed-progress proof.
    std::size_t walkPrefixEnd = 0;
    for (; walkPrefixEnd < route.size(); ++walkPrefixEnd)
    {
        PathNodePoint const& node = route[walkPrefixEnd];
        if ((node.type != NODE_PREPATH && node.type != NODE_PATH && node.type != NODE_NODE) ||
            node.point.GetMapId() != startPoint.mapId)
            break;
    }
    float const currentRouteRemaining = TravelMgrRouteRemainingDistance(
        startPoint, route, 0U, walkPrefixEnd, destinationPoint);

    candidates.reserve(std::min(route.size(), kMaxTravelMgrReanchorScanPoints));
    preparedProbes.reserve(candidates.capacity());

    for (std::size_t routeIndex = 0;
         routeIndex < route.size() && routeIndex < kMaxTravelMgrReanchorScanPoints;
         ++routeIndex)
    {
        PathNodePoint const& node = route[routeIndex];
        // NODE_NODE is a normal graph waypoint. It is not a transition and is safe to consider
        // when the caller freshly validates the segment from the live bot position.
        if (node.type != NODE_PREPATH && node.type != NODE_PATH && node.type != NODE_NODE)
        {
            if (routeIndex == 0)
                diagnostics.reason = TravelMgrDiagnosticReason::TransitionPrefix;
            break;
        }

        Waypoint const point{
            node.point.GetMapId(), node.point.GetPositionX(), node.point.GetPositionY(),
            node.point.GetPositionZ()};
        if (point.mapId != startPoint.mapId)
            break;

        ++diagnostics.scanPointsConsidered;

        TravelMgrReanchorCandidateFacts candidate;
        candidate.routeIndex = routeIndex;
        candidate.nodeKind = TravelMgrRouteNodeKind::Walk;
        candidate.point = point;
        candidate.sameMap = point.mapId == startPoint.mapId;
        candidate.directDistance = Distance2d(startPoint, point);
        candidate.targetDistanceReduction =
            currentTargetDistance - Distance2d(point, destinationPoint);

        if (candidate.sameMap && std::isfinite(candidate.directDistance))
        {
            ++diagnostics.sameMapWalkPoints;
            if (diagnostics.nearestRouteDistance < 0.0f ||
                candidate.directDistance < diagnostics.nearestRouteDistance)
                diagnostics.nearestRouteDistance = candidate.directDistance;
            if (candidate.directDistance > diagnostics.farthestRouteDistance)
                diagnostics.farthestRouteDistance = candidate.directDistance;
        }

        std::optional<AutoWowDungeonPath::ProbeResult> prepared;
        if (candidate.sameMap && IsFinite(point) && std::isfinite(candidate.directDistance) &&
            candidate.directDistance >= kTravelMgrReanchorMinimumDirectDistance &&
            candidate.directDistance <= preferredMaximumStep)
        {
            ++diagnostics.distanceEligiblePoints;
            ++diagnostics.freshProbeAttempts;
            AutoWowDungeonPath::ProbeResult const fresh = AutoWowDungeonPath::ProbeFrom(
                bot, start.GetPositionX(), start.GetPositionY(), start.GetPositionZ(),
                node.point.GetPositionX(), node.point.GetPositionY(), node.point.GetPositionZ());
            PathFacts const freshFacts = BuildPathFacts(fresh);
            bool const complete = IsCompleteReprobe(freshFacts);
            bool const safeIncomplete = IsSafeIncompleteTravelMgrProbe(freshFacts);
            if (!freshFacts.validHeight)
                diagnostics.reason = TravelMgrDiagnosticReason::FreshProbeUngrounded;
            else if (!complete && !safeIncomplete)
                diagnostics.reason = !freshFacts.normalPathType && !freshFacts.validatedGroundLine
                    ? TravelMgrDiagnosticReason::FreshProbeNonNormal
                    : TravelMgrDiagnosticReason::FreshProbeIncomplete;
            else
            {
                // Rank the actual prepared endpoint, not merely the stored graph point. For a safe
                // incomplete probe this preserves the no-regression guarantee when the path ends
                // at a valid frontier before the requested graph point.
                if (!fresh.path.empty())
                {
                    auto const& endpoint = fresh.path.back();
                    Waypoint const endpointPoint{
                        start.GetMapId(), endpoint.x, endpoint.y, endpoint.z};
                    if (!IsFinite(endpointPoint))
                    {
                        diagnostics.reason = TravelMgrDiagnosticReason::FreshProbeIncomplete;
                        candidates.push_back(candidate);
                        preparedProbes.push_back(std::nullopt);
                        continue;
                    }
                    candidate.point = endpointPoint;
                    candidate.directDistance = Distance2d(startPoint, endpointPoint);
                    candidate.targetDistanceReduction =
                        currentTargetDistance - Distance2d(endpointPoint, destinationPoint);
                    float const candidateRouteRemaining = TravelMgrRouteRemainingDistance(
                        endpointPoint, route, routeIndex, walkPrefixEnd, destinationPoint);
                    if (std::isfinite(currentRouteRemaining) && std::isfinite(candidateRouteRemaining))
                        candidate.routeDistanceReduction = currentRouteRemaining - candidateRouteRemaining;
                }
                prepared = fresh;
                candidate.freshProbe = freshFacts;
                ++diagnostics.freshProbeAccepts;
            }
        }

        candidates.push_back(candidate);
        preparedProbes.push_back(std::move(prepared));
    }

    std::optional<std::size_t> const selected =
        SelectTravelMgrSegmentCandidate(candidates, preferredMaximumStep);
    if (!selected || *selected >= preparedProbes.size() || !preparedProbes[*selected])
    {
        if (diagnostics.reason == TravelMgrDiagnosticReason::None)
            diagnostics.reason = route.size() > kMaxTravelMgrReanchorScanPoints &&
                    diagnostics.scanPointsConsidered == kMaxTravelMgrReanchorScanPoints
                ? TravelMgrDiagnosticReason::ScanWindowExhausted
                : TravelMgrDiagnosticReason::NoCandidate;
        RecordTravelMgrDiagnostics(bot->GetGUID().GetCounter(), diagnostics);
        return std::nullopt;
    }

    diagnostics.selected = true;
    diagnostics.selectedRouteIndex = static_cast<std::uint32_t>(candidates[*selected].routeIndex);
    diagnostics.selectedDistance = candidates[*selected].directDistance;
    diagnostics.reason = IsSafeIncompleteTravelMgrProbe(candidates[*selected].freshProbe)
        ? TravelMgrDiagnosticReason::AcceptedSafeIncomplete
        : TravelMgrDiagnosticReason::Accepted;
    RecordTravelMgrDiagnostics(bot->GetGUID().GetCounter(), diagnostics);
    return std::move(preparedProbes[*selected]);
}

// TravelMgr can retain a route whose first stored point is stale or unreachable from the bot's
// current position. Re-anchor only within the bounded same-map walk prefix. Every candidate is
// freshly probed from the live start, and the returned path is the exact fresh path that the caller
// must execute. Transition nodes and map changes remain hard boundaries.
inline std::optional<AutoWowDungeonPath::ProbeResult> SelectTravelMgrWalkProbe(
    Player* bot, WorldPosition const& destination)
{
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    WorldPosition const start(bot);
    TravelPath segmented = TravelNodeMap::getFullPath(start, destination, bot, true);
    if (segmented.empty())
    {
        TravelMgrDiagnostics diagnostics;
        diagnostics.unattachedStartAllowed = true;
        diagnostics.reason = TravelMgrDiagnosticReason::RouteEmpty;
        RecordTravelMgrDiagnostics(bot->GetGUID().GetCounter(), diagnostics);
        return std::nullopt;
    }
    return SelectTravelMgrWalkProbeFromRoute(bot, destination, start, segmented.getPath());
}

// Errands-specific admission. Existing quest selection intentionally remains unchanged: errands
// additionally require the complete TravelMgr graph to be same-map/all-walk, and return the exact
// fresh probe that the caller must hand directly to WalkPrepared.
inline ErrandsWalkProbeSelection SelectErrandsWalkProbe(Player* bot, WorldPosition const& destination)
{
    ErrandsWalkProbeSelection selection;
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
        return selection;

    selection.probe = SelectCompleteWalkProbe(bot, destination);
    if (selection.probe)
    {
        selection.kind = ErrandsWalkProofKind::Direct;
        selection.topology = ErrandsWalkTopology::Direct;
        return selection;
    }

    WorldPosition const start(bot);
    TravelPath segmented = TravelNodeMap::getFullPath(start, destination, bot, true);
    if (segmented.empty())
    {
        selection.topology = ErrandsWalkTopology::Empty;
        return selection;
    }

    std::vector<PathNodePoint> const route = segmented.getPath();
    selection.routePointCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(route.size(), std::numeric_limits<std::uint32_t>::max()));
    selection.topology = ClassifyErrandsWalkTopology(route, destination);
    if (selection.topology != ErrandsWalkTopology::AllWalk)
        return selection;

    selection.probe = SelectTravelMgrWalkProbeFromRoute(bot, destination, start, route);
    if (selection.probe)
        selection.kind = ErrandsWalkProofKind::TravelMgr;
    return selection;
}

// TravelMgr may return a valid graph route whose first usable graph point is behind a local
// obstacle relative to the bot's current position. Reuse the same finite, deterministic fan as
// the staged quest-giver mover: each candidate is grounded, freshly probed, and accepted only when
// it measurably reduces distance to the exact destination. This is an intermediate segment, never
// a replacement destination and never a random stepping stone.
inline std::optional<AutoWowDungeonPath::ProbeResult> SelectDeterministicLocalDetourWalkProbe(
    Player* bot, WorldPosition const& destination)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    WorldPosition const start(bot);
    float const dx = destination.GetPositionX() - start.GetPositionX();
    float const dy = destination.GetPositionY() - start.GetPositionY();
    float const targetDistance = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(targetDistance) || targetDistance <= 0.0f)
        return std::nullopt;

    constexpr float detourPi = 3.14159265358979323846f;
    constexpr std::array<float, 3> detourRadii{8.0f, 16.0f, 24.0f};
    constexpr std::array<float, 9> detourAngleOffsets{
        0.0f,
        detourPi / 6.0f,
        -detourPi / 6.0f,
        detourPi / 3.0f,
        -detourPi / 3.0f,
        detourPi / 2.0f,
        -detourPi / 2.0f,
        2.0f * detourPi / 3.0f,
        -2.0f * detourPi / 3.0f};

    float const bearing = std::atan2(dy, dx);
    if (!std::isfinite(bearing))
        return std::nullopt;

    Waypoint const startPoint{
        start.GetMapId(), start.GetPositionX(), start.GetPositionY(), start.GetPositionZ()};
    Waypoint const targetPoint{
        destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ()};
    std::vector<AutoWowDungeonPath::ProbeResult> preparedProbes;
    std::vector<LocalDetourCandidateFacts> candidates;
    preparedProbes.reserve(detourRadii.size() * detourAngleOffsets.size());
    candidates.reserve(preparedProbes.capacity());

    for (float const radius : detourRadii)
    {
        for (float const offset : detourAngleOffsets)
        {
            float const angle = bearing + offset;
            float const x = start.GetPositionX() + std::cos(angle) * radius;
            float const y = start.GetPositionY() + std::sin(angle) * radius;
            if (!std::isfinite(x) || !std::isfinite(y))
                continue;

            float const z = bot->GetMap()->GetHeight(
                bot->GetPhaseMask(), x, y, start.GetPositionZ() + 2.0f, true, 10.0f);
            if (!std::isfinite(z) || z <= INVALID_HEIGHT)
                continue;

            AutoWowDungeonPath::ProbeResult probe = AutoWowDungeonPath::ProbeFrom(
                bot, start.GetPositionX(), start.GetPositionY(), start.GetPositionZ(), x, y, z);
            PathFacts const facts = BuildPathFacts(probe);
            Waypoint const candidatePoint{bot->GetMapId(), x, y, z};
            float const candidateDistance = Distance2d(candidatePoint, targetPoint);

            LocalDetourCandidateFacts candidate;
            candidate.sameMap = candidatePoint.mapId == startPoint.mapId;
            candidate.finite = IsFinite(candidatePoint);
            bool const completeAuthorization = IsComplete(facts);
            candidate.completeNormalPath = completeAuthorization && !facts.validatedGroundLine;
            candidate.routeLength = probe.pathLength;
            candidate.distanceReduction = targetDistance - candidateDistance;
            candidate.pathType = probe.pathType;
            candidate.validHeight = probe.allGroundSamplesValid;
            candidate.validatedGroundLine = completeAuthorization && facts.validatedGroundLine;

            preparedProbes.push_back(std::move(probe));
            candidates.push_back(candidate);
        }
    }

    std::optional<std::size_t> const selected = SelectLocalDetour(candidates);
    if (!selected || *selected >= preparedProbes.size())
        return std::nullopt;
    return std::move(preparedProbes[*selected]);
}

// The New RPG quest source/finisher path uses the same ordered proof as the staged quest-giver
// mover: exact complete route first, then a fresh bounded TravelMgr segment. No random stepping
// stone or teleport is authorized by this selector.
inline QuestWalkProbeSelection SelectQuestWalkProbeDetailed(
    Player* bot, WorldPosition const& destination, bool recordDiagnostics = true);

inline std::optional<AutoWowDungeonPath::ProbeResult> SelectQuestWalkProbe(
    Player* bot, WorldPosition const& destination)
{
    return SelectQuestWalkProbeDetailed(bot, destination).probe;
}

// Detailed form used by the autonomous quest path. The selector still has exactly the same
// no-teleport behavior as SelectQuestWalkProbe; this adds only scalar evidence about each bounded
// attempt so questobjective can explain why a bot is parked.
inline QuestWalkProbeSelection SelectQuestWalkProbeDetailed(
    Player* bot, WorldPosition const& destination, bool recordDiagnostics)
{
    QuestWalkProbeSelection selection;
    selection.diagnostics.attempted = true;

    auto record = [&selection, recordDiagnostics](std::uint32_t botGuid)
    {
        if (recordDiagnostics)
            RecordQuestWalkDiagnostics(botGuid, selection.diagnostics);
    };

    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
    {
        selection.diagnostics.directProbeAttempted = false;
        selection.diagnostics.directReason = "bot_not_in_same_map";
        record(bot ? bot->GetGUID().GetCounter() : 0);
        return selection;
    }

    uint32 const botGuid = bot->GetGUID().GetCounter();
    AutoWowDungeonPath::ProbeResult const direct = AutoWowDungeonPath::Probe(
        bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    selection.diagnostics.directProbeAttempted = true;
    selection.diagnostics.directProbeSafe = direct.safe;
    selection.diagnostics.directPathType = direct.pathType;
    selection.diagnostics.directPointCount = static_cast<std::uint32_t>(direct.path.size());
    selection.diagnostics.directEndpointDistance = direct.endpointDistance;
    selection.diagnostics.directPathLength = direct.pathLength;
    selection.diagnostics.directReason = direct.reason;
    selection.diagnostics.directNavmeshReason = direct.navmeshReason;
    selection.diagnostics.directGroundLineReason = direct.groundLineReason;

    if (IsCompleteReprobe(BuildPathFacts(direct)))
    {
        selection.probe = direct;
        selection.diagnostics.selectionSource = "direct";
        selection.diagnostics.directReason = "accepted";
        selection.diagnostics.walkPreparedCalled = false;
        record(botGuid);
        return selection;
    }

    selection.diagnostics.travelMgrAttempted = true;
    selection.probe = SelectTravelMgrWalkProbe(bot, destination);
    if (TravelMgrDiagnostics const* travelMgr = ReadTravelMgrDiagnostics(botGuid))
        selection.diagnostics.travelMgr = *travelMgr;

    if (selection.probe)
        selection.diagnostics.selectionSource = "travel_mgr";
    else
    {
        selection.probe = SelectDeterministicLocalDetourWalkProbe(bot, destination);
        if (selection.probe)
            selection.diagnostics.selectionSource = "local_detour";
    }

    record(botGuid);
    return selection;
}
}

#endif
