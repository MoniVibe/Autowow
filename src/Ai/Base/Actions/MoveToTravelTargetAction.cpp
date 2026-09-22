/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MoveToTravelTargetAction.h"

#include "ChooseRpgTargetAction.h"
#include "DungeonPathSafety.h"
#include "DungeonPathWalkAction.h"
#include "LootObjectStack.h"
#include "Playerbots.h"
#include "QuestGiverTravelFeedback.h"
#include "QuestGiverTravelPolicy.h"
#include "QuestTravelWalk.h"
#include "TravelNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
bool IsQuestGiverTravelTarget(TravelTarget* target)
{
    return target && target->getDestination() &&
        target->getDestination()->getName() == "AutoWowQuestAcquisitionDestination";
}

AutoWowQuestGiverTravel::PathFacts MakePathFacts(AutoWowDungeonPath::ProbeResult const& probe)
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

std::optional<AutoWowDungeonPath::ProbeResult> SelectCompleteWalkProbe(
    Player* bot, WorldPosition const& source, WorldPosition const& point)
{
    if (!bot || !bot->IsInWorld() || source.GetMapId() != point.GetMapId() ||
        bot->GetMapId() != point.GetMapId())
        return std::nullopt;

    AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::ProbeFrom(
        bot, source.GetPositionX(), source.GetPositionY(), source.GetPositionZ(),
        point.GetPositionX(), point.GetPositionY(), point.GetPositionZ());
    if (!AutoWowQuestGiverTravel::IsCompleteReprobe(MakePathFacts(probe)))
        return std::nullopt;

    // Return the complete probe itself. The caller must pass this exact path to WalkPrepared;
    // returning only the requested endpoint would allow the next movement layer to repath it.
    return probe;
}

constexpr float kLocalDetourPi = 3.14159265358979323846f;
constexpr std::array<float, 3> kLocalDetourRadii{8.0f, 16.0f, 24.0f};
// Offset order is intentionally centered on the exact giver bearing, then expands symmetrically.
constexpr std::array<float, 9> kLocalDetourAngleOffsets{
    0.0f,
    kLocalDetourPi / 6.0f,
    -kLocalDetourPi / 6.0f,
    kLocalDetourPi / 3.0f,
    -kLocalDetourPi / 3.0f,
    kLocalDetourPi / 2.0f,
    -kLocalDetourPi / 2.0f,
    2.0f * kLocalDetourPi / 3.0f,
    -2.0f * kLocalDetourPi / 3.0f};

struct QuantizedCellHash
{
    std::size_t operator()(AutoWowQuestGiverTravel::QuantizedCell const& cell) const noexcept
    {
        std::size_t hash = static_cast<std::size_t>(cell.mapId);
        hash = hash * 31U + static_cast<std::size_t>(cell.x);
        hash = hash * 31U + static_cast<std::size_t>(cell.y);
        hash = hash * 31U + static_cast<std::size_t>(cell.z);
        return hash;
    }
};

struct QuestGiverEscapeSession
{
    bool initialized = false;
    TravelTarget* targetIdentity = nullptr;
    AutoWowQuestGiverTravel::Waypoint targetPoint{};
    AutoWowQuestGiverTravel::QuantizedCell targetCell{};
    std::unordered_set<AutoWowQuestGiverTravel::QuantizedCell, QuantizedCellHash> visited;
    AutoWowQuestGiverTravel::EscapeBudget budget{};
};

std::unordered_map<std::uint32_t, QuestGiverEscapeSession> questGiverEscapeSessions;

void ClearQuestGiverEscapeSession(Player* bot)
{
    if (bot)
        questGiverEscapeSessions.erase(bot->GetGUID().GetCounter());
}

bool IsSameQuestGiverTarget(AutoWowQuestGiverTravel::Waypoint const& left,
                            AutoWowQuestGiverTravel::Waypoint const& right)
{
    constexpr float positionEpsilon = 0.01f;
    return left.mapId == right.mapId && std::fabs(left.x - right.x) <= positionEpsilon &&
        std::fabs(left.y - right.y) <= positionEpsilon &&
        std::fabs(left.z - right.z) <= positionEpsilon;
}

QuestGiverEscapeSession* GetQuestGiverEscapeSession(Player* bot, TravelTarget* target)
{
    if (!bot || !target || !target->getPosition())
        return nullptr;

    WorldPosition const& destination = *target->getPosition();
    AutoWowQuestGiverTravel::Waypoint const targetPoint{
        destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ()};
    std::optional<AutoWowQuestGiverTravel::QuantizedCell> const targetCell =
        AutoWowQuestGiverTravel::QuantizeCell(targetPoint);
    if (!targetCell)
        return nullptr;

    QuestGiverEscapeSession& session =
        questGiverEscapeSessions[bot->GetGUID().GetCounter()];
    if (!session.initialized || session.targetIdentity != target ||
        !IsSameQuestGiverTarget(session.targetPoint, targetPoint))
    {
        session = {};
        session.initialized = true;
        session.targetIdentity = target;
        session.targetPoint = targetPoint;
        session.targetCell = *targetCell;

        WorldPosition const start(bot);
        AutoWowQuestGiverTravel::Waypoint const startPoint{
            start.GetMapId(), start.GetPositionX(), start.GetPositionY(), start.GetPositionZ()};
        if (std::optional<AutoWowQuestGiverTravel::QuantizedCell> const startCell =
                AutoWowQuestGiverTravel::QuantizeCell(startPoint))
            session.visited.insert(*startCell);

        AutoWowQuestGiverTravel::BeginInitialEscapeEpoch(
            session.budget, AutoWowQuestGiverTravel::Distance2d(startPoint, targetPoint));
    }
    return &session;
}

void MarkQuestGiverCell(QuestGiverEscapeSession& session,
                        AutoWowQuestGiverTravel::Waypoint const& point)
{
    if (std::optional<AutoWowQuestGiverTravel::QuantizedCell> const cell =
            AutoWowQuestGiverTravel::QuantizeCell(point))
    {
        if (session.visited.find(*cell) == session.visited.end() &&
            session.visited.size() >= AutoWowQuestGiverTravel::kMaxEscapeVisitedCells)
            return;
        session.visited.insert(*cell);
    }
}

bool MarkQuestGiverCell(QuestGiverEscapeSession& session,
                        AutoWowQuestGiverTravel::QuantizedCell const& cell)
{
    if (session.visited.find(cell) != session.visited.end())
        return false;
    if (session.visited.size() >= AutoWowQuestGiverTravel::kMaxEscapeVisitedCells)
        return false;
    session.visited.insert(cell);
    return true;
}

void MarkPreparedEndpoint(QuestGiverEscapeSession& session,
                          AutoWowDungeonPath::ProbeResult const& probe, std::uint32_t mapId)
{
    if (probe.path.empty())
        return;

    G3D::Vector3 const& endpoint = probe.path.back();
    MarkQuestGiverCell(session, AutoWowQuestGiverTravel::Waypoint{
        mapId, endpoint.x, endpoint.y, endpoint.z});
}

std::optional<AutoWowDungeonPath::ProbeResult> SelectQuestGiverLocalDetour(
    Player* bot, WorldPosition const& destination, QuestGiverEscapeSession& session)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    WorldPosition const start(bot);
    float const dx = destination.GetPositionX() - start.GetPositionX();
    float const dy = destination.GetPositionY() - start.GetPositionY();
    float const giverDistance = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(giverDistance) || giverDistance <= 0.0f)
        return std::nullopt;

    float const giverBearing = std::atan2(dy, dx);
    std::vector<WorldPosition> points;
    std::vector<AutoWowDungeonPath::ProbeResult> preparedProbes;
    std::vector<AutoWowQuestGiverTravel::LocalDetourCandidateFacts> candidates;
    points.reserve(kLocalDetourRadii.size() * kLocalDetourAngleOffsets.size());
    preparedProbes.reserve(points.capacity());
    candidates.reserve(points.capacity());

    for (float const radius : kLocalDetourRadii)
    {
        for (float const offset : kLocalDetourAngleOffsets)
        {
            float const angle = giverBearing + offset;
            float const x = start.GetPositionX() + std::cos(angle) * radius;
            float const y = start.GetPositionY() + std::sin(angle) * radius;
            if (!std::isfinite(x) || !std::isfinite(y))
                continue;

            // Use the live start height only to reserve the XY cell before probing. If the actual
            // floor lookup fails, this still prevents the same invalid local cell from repeating.
            AutoWowQuestGiverTravel::Waypoint const rawPoint{
                bot->GetMapId(), x, y, start.GetPositionZ()};
            std::optional<AutoWowQuestGiverTravel::QuantizedCell> const cell =
                AutoWowQuestGiverTravel::QuantizeCell(rawPoint);
            if (!cell || session.visited.find(*cell) != session.visited.end())
                continue;
            // A ring/fan cell is considered visited once it has been probed, including a failed
            // probe. This prevents the same local topology query from repeating on every AI tick.
            if (!MarkQuestGiverCell(session, *cell))
                continue;

            float const z = bot->GetMap()->GetHeight(
                bot->GetPhaseMask(), x, y, start.GetPositionZ() + 2.0f, true, 10.0f);
            if (!std::isfinite(z) || z <= INVALID_HEIGHT)
                continue;

            WorldPosition const point(bot->GetMapId(), x, y, z, bot->GetOrientation());
            AutoWowQuestGiverTravel::Waypoint const policyPoint{
                point.GetMapId(), point.GetPositionX(), point.GetPositionY(), point.GetPositionZ()};

            AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::ProbeFrom(
                bot, start.GetPositionX(), start.GetPositionY(), start.GetPositionZ(), x, y, z);
            AutoWowQuestGiverTravel::PathFacts const pathFacts = MakePathFacts(probe);
            float const candidateDistance = std::sqrt(
                (destination.GetPositionX() - x) * (destination.GetPositionX() - x) +
                (destination.GetPositionY() - y) * (destination.GetPositionY() - y));

            points.push_back(point);
            preparedProbes.push_back(probe);
            AutoWowQuestGiverTravel::LocalDetourCandidateFacts candidate;
            candidate.sameMap = point.GetMapId() == bot->GetMapId();
            candidate.finite = AutoWowQuestGiverTravel::IsFinite(policyPoint);
            bool const completeAuthorization = AutoWowQuestGiverTravel::IsComplete(pathFacts);
            candidate.completeNormalPath = completeAuthorization && !pathFacts.validatedGroundLine;
            candidate.routeLength = probe.pathLength;
            candidate.distanceReduction = giverDistance - candidateDistance;
            candidate.pathType = probe.pathType;
            candidate.validHeight = probe.allGroundSamplesValid;
            candidate.validatedGroundLine =
                completeAuthorization && pathFacts.validatedGroundLine;
            candidates.push_back(candidate);
        }
    }

    std::optional<std::size_t> const selected =
        AutoWowQuestGiverTravel::SelectLocalDetour(candidates);
    return selected ? std::optional<AutoWowDungeonPath::ProbeResult>(preparedProbes[*selected])
                    : std::nullopt;
}

std::optional<AutoWowDungeonPath::ProbeResult> SelectQuestGiverEscapeProbe(
    Player* bot, WorldPosition const& destination, QuestGiverEscapeSession& session)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap() || bot->GetMapId() != destination.GetMapId())
        return std::nullopt;

    WorldPosition const start(bot);
    AutoWowQuestGiverTravel::Waypoint const startPoint{
        start.GetMapId(), start.GetPositionX(), start.GetPositionY(), start.GetPositionZ()};
    AutoWowQuestGiverTravel::Waypoint const targetPoint{
        destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ()};
    float const distanceToGiver = AutoWowQuestGiverTravel::Distance2d(startPoint, targetPoint);
    if (!std::isfinite(distanceToGiver))
    {
        AutoWowQuestGiverTravel::ExhaustEscapeEpoch(session.budget);
        return std::nullopt;
    }

    // Exhausting one local search does not immediately terminalize the journey. A fresh epoch is
    // permitted only when the same target has become meaningfully closer since the prior epoch
    // began; journey-wide counters and visited cells remain intact across this reset.
    AutoWowQuestGiverTravel::ObserveEscapeProgress(session.budget, distanceToGiver);
    if (AutoWowQuestGiverTravel::IsEscapeExhausted(session.budget) &&
        !AutoWowQuestGiverTravel::BeginEscapeReplan(session.budget, distanceToGiver))
        return std::nullopt;

    std::vector<AutoWowQuestGiverTravel::Waypoint> const ring =
        AutoWowQuestGiverTravel::BuildEscapeRing(startPoint, targetPoint);
    if (ring.empty())
    {
        AutoWowQuestGiverTravel::ExhaustEscapeEpoch(session.budget);
        return std::nullopt;
    }

    std::vector<AutoWowQuestGiverTravel::EscapeCandidateFacts> candidates;
    std::vector<std::optional<AutoWowDungeonPath::ProbeResult>> preparedProbes;
    candidates.reserve(ring.size());
    preparedProbes.reserve(ring.size());

    for (std::size_t ringOrder = 0; ringOrder < ring.size(); ++ringOrder)
    {
        AutoWowQuestGiverTravel::Waypoint const& candidatePoint = ring[ringOrder];
        std::optional<AutoWowQuestGiverTravel::QuantizedCell> const cell =
            AutoWowQuestGiverTravel::QuantizeCell(candidatePoint);
        if (!cell || session.visited.find(*cell) != session.visited.end())
            continue;
        // Mark every generated cell before probing it. Invalid-height and unsafe cells therefore
        // cannot be selected again by the same route/session.
        if (!MarkQuestGiverCell(session, *cell))
            continue;

        AutoWowQuestGiverTravel::EscapeCandidateFacts candidate;
        candidate.point = candidatePoint;
        candidate.cell = *cell;
        candidate.quantized = true;
        candidate.sameMap = candidatePoint.mapId == bot->GetMapId();
        candidate.finite = AutoWowQuestGiverTravel::IsFinite(candidatePoint);
        candidate.ringOrder = ringOrder;
        candidate.distanceReduction = distanceToGiver -
            AutoWowQuestGiverTravel::Distance2d(candidatePoint, targetPoint);

        std::optional<AutoWowDungeonPath::ProbeResult> prepared;
        if (candidate.sameMap && candidate.finite)
        {
            float const height = bot->GetMap()->GetHeight(
                bot->GetPhaseMask(), candidatePoint.x, candidatePoint.y,
                start.GetPositionZ() + 2.0f, true, 10.0f);
            candidate.validHeight = std::isfinite(height) && height > INVALID_HEIGHT;
            if (candidate.validHeight)
            {
                WorldPosition const groundedPoint(
                    bot->GetMapId(), candidatePoint.x, candidatePoint.y, height,
                    bot->GetOrientation());
                prepared = SelectCompleteWalkProbe(bot, start, groundedPoint);
                if (prepared)
                {
                    AutoWowQuestGiverTravel::PathFacts const facts =
                        MakePathFacts(*prepared);
                    candidate.freshlyStrictComplete =
                        AutoWowQuestGiverTravel::IsCompleteReprobe(facts);
                    candidate.routeLength = prepared->pathLength;
                    candidate.pathType = prepared->pathType;
                    candidate.validatedGroundLine =
                        candidate.freshlyStrictComplete && facts.validatedGroundLine;
                }
            }
        }

        candidates.push_back(candidate);
        preparedProbes.push_back(std::move(prepared));
    }

    AutoWowQuestGiverTravel::EscapeSelection const selection =
        AutoWowQuestGiverTravel::SelectEscapeCandidate(candidates, session.budget);
    if (!selection.index || *selection.index >= preparedProbes.size() ||
        !preparedProbes[*selection.index] ||
        !AutoWowQuestGiverTravel::ConsumeEscapeCandidate(
            session.budget, candidates[*selection.index]))
    {
        AutoWowQuestGiverTravel::ExhaustEscapeEpoch(session.budget);
        return std::nullopt;
    }

    return std::move(preparedProbes[*selection.index]);
}

void RecordQuestGiverMovement(Player* bot, TravelTarget* target,
                              AutoWowQuestGiverTravel::StepKind step,
                              AutoWowQuestGiverTravel::MovementFeedbackResult result)
{
    if (!bot)
        return;

    float remainingDistance = -1.0f;
    if (target && target->getPosition() && bot->GetMapId() == target->getPosition()->GetMapId())
    {
        WorldPosition current(bot);
        remainingDistance = current.distance(*target->getPosition());
    }

    AutoWowQuestGiverTravel::RecordMovementFeedback(
        bot->GetGUID().GetCounter(), step, result,
        result != AutoWowQuestGiverTravel::MovementFeedbackResult::Parked && bot->isMoving(),
        remainingDistance);
}

bool IsAtQuestGiver(Player* bot, TravelTarget* target)
{
    if (!bot || !target || !target->getPosition() || bot->GetMapId() != target->getPosition()->GetMapId())
        return false;
    WorldPosition current(bot);
    return current.distance(*target->getPosition()) <= INTERACTION_DISTANCE * 1.5f;
}
}

bool MoveToTravelTargetAction::MoveQuestGiverStaged(TravelTarget* target)
{
    if (!bot)
        return false;
    if (!target || !target->getPosition())
    {
        ClearQuestGiverEscapeSession(bot);
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::Rejected);
        return false;
    }

    if (IsAtQuestGiver(bot, target))
    {
        // The leader has arrived. Keep the travel action alive for the session's party-cohesion
        // poll, but do not issue another MovePoint or charge this parked state as a failure.
        ClearQuestGiverEscapeSession(bot);
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::Parked);
        return true;
    }

    QuestGiverEscapeSession* session = GetQuestGiverEscapeSession(bot, target);
    if (!session)
    {
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::NoProgress);
        return true;
    }

    // MoveTo deliberately rejects a normal-priority refresh while the previous staged point is
    // still reserved. Treat that expected cadence gate as deferred observation; only a genuine
    // move attempt that fails should consume the journey's rejection budget.
    if (AutoWowQuestGiverTravel::IsMovementRefreshDeferred(
            IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL), false))
    {
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::Deferred);
        return true;
    }

    WorldPosition const destination = *target->getPosition();
    AutoWowQuestGiverTravel::Waypoint const currentPoint{
        bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    AutoWowQuestGiverTravel::Waypoint const targetPoint{
        destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ()};
    AutoWowQuestGiverTravel::ObserveEscapeProgress(
        session->budget, AutoWowQuestGiverTravel::Distance2d(currentPoint, targetPoint));
    AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
        bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    AutoWowQuestGiverTravel::PathFacts const facts = MakePathFacts(probe);
    AutoWowQuestGiverTravel::StepKind const step = AutoWowQuestGiverTravel::SelectStep(facts);

    auto preparedMoveDeferred = [&](AutoWowDungeonPath::ProbeResult const& prepared) -> bool
    {
        if (prepared.path.empty())
            return false;
        G3D::Vector3 const& endpoint = prepared.path.back();
        return AutoWowQuestGiverTravel::IsMovementRefreshDeferred(
            IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL),
            IsDuplicateMove(endpoint.x, endpoint.y, endpoint.z));
    };

    auto executePrepared = [&](AutoWowQuestGiverTravel::StepKind moveStep,
                               AutoWowDungeonPath::ProbeResult const& prepared) -> bool
    {
        if (preparedMoveDeferred(prepared))
        {
            RecordQuestGiverMovement(
                bot, target, moveStep, AutoWowQuestGiverTravel::MovementFeedbackResult::Deferred);
            return true;
        }

        // WalkPrepared consumes the exact path that was just validated. Do not pass its endpoint to
        // MoveTo: that would discard the verified spline and ask a second layer to repath it.
        AutoWowDungeonWalkAction walk(botAI);
        bool const accepted = walk.WalkPrepared(prepared);
        if (accepted)
            MarkPreparedEndpoint(*session, prepared, bot->GetMapId());
        RecordQuestGiverMovement(
            bot, target, moveStep,
            accepted ? AutoWowQuestGiverTravel::MovementFeedbackResult::Accepted
                     : AutoWowQuestGiverTravel::MovementFeedbackResult::Rejected);
        return accepted;
    };

    auto terminalNoProgress = [&]() -> bool
    {
        AutoWowQuestGiverTravel::ExhaustEscapeEpoch(session->budget);
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::EscapeRing,
            AutoWowQuestGiverTravel::MovementFeedbackResult::NoProgress);
        // The exact quest route has reached a terminal bounded blocker. Clear local topology state
        // so a later non-quest/session target cannot inherit its cells or budgets.
        if (target->getStatus() == TRAVEL_STATUS_TRAVEL)
            target->setStatus(TRAVEL_STATUS_COOLDOWN);
        ClearQuestGiverEscapeSession(bot);
        return true;
    };

    auto moveCompleteFallback = [&]() -> bool
    {
        // The order is intentional: partial frontier, then a verified TravelMgr walk segment, then
        // an improving local detour, and only then the bounded full-circle topology escape.
        if (std::optional<AutoWowDungeonPath::ProbeResult> const travelMgrProbe =
                AutoWowQuestGiverTravel::SelectTravelMgrWalkProbe(bot, destination))
            return executePrepared(AutoWowQuestGiverTravel::StepKind::TravelMgrSegment,
                                   *travelMgrProbe);

        if (std::optional<AutoWowDungeonPath::ProbeResult> const detourProbe =
                SelectQuestGiverLocalDetour(bot, destination, *session))
            return executePrepared(AutoWowQuestGiverTravel::StepKind::LocalDetour, *detourProbe);

        if (std::optional<AutoWowDungeonPath::ProbeResult> const escapeProbe =
                SelectQuestGiverEscapeProbe(bot, destination, *session))
            return executePrepared(AutoWowQuestGiverTravel::StepKind::EscapeRing, *escapeProbe);

        return terminalNoProgress();
    };

    if (step == AutoWowQuestGiverTravel::StepKind::Direct)
        return executePrepared(step, probe);

    if (step == AutoWowQuestGiverTravel::StepKind::PartialPath)
    {
        WorldPosition const start(bot);
        std::vector<AutoWowQuestGiverTravel::Waypoint> points;
        points.reserve(probe.path.size());
        for (G3D::Vector3 const& point : probe.path)
            points.push_back({bot->GetMapId(), point.x, point.y, point.z});

        AutoWowQuestGiverTravel::Waypoint const startPoint{
            start.GetMapId(), start.GetPositionX(), start.GetPositionY(), start.GetPositionZ()};
        std::vector<std::size_t> const candidateOrder =
            AutoWowQuestGiverTravel::SelectTargetAwarePartialWaypointCandidates(
                points, startPoint, targetPoint);

        if (candidateOrder.empty())
            return moveCompleteFallback();

        // The partial path supplies only finite, same-map, target-progressing, bounded candidates.
        // They are not move authorizations: try them in farthest-progress order and re-probe each
        // exact live-start segment until one proves complete/safe/normal/grounded.
        for (std::size_t const candidateIndex : candidateOrder)
        {
            AutoWowQuestGiverTravel::Waypoint const& candidate = points[candidateIndex];
            std::optional<AutoWowQuestGiverTravel::QuantizedCell> const cell =
                AutoWowQuestGiverTravel::QuantizeCell(candidate);
            if (!cell || session->visited.find(*cell) != session->visited.end())
                continue;
            if (!MarkQuestGiverCell(*session, *cell))
                continue;

            WorldPosition const candidatePosition(candidate.mapId, candidate.x, candidate.y,
                                                  candidate.z, bot->GetOrientation());
            std::optional<AutoWowDungeonPath::ProbeResult> const completeCandidate =
                SelectCompleteWalkProbe(bot, start, candidatePosition);
            if (!completeCandidate)
                continue;

            return executePrepared(step, *completeCandidate);
        }

        return moveCompleteFallback();
    }

    if (step == AutoWowQuestGiverTravel::StepKind::TravelMgrSegment)
        return moveCompleteFallback();

    RecordQuestGiverMovement(
        bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
        AutoWowQuestGiverTravel::MovementFeedbackResult::NoProgress);
    return true;
}

bool MoveToTravelTargetAction::ExecuteQuestGiverStagedEntry(TravelTarget* target)
{
    if (!bot || !target || !IsQuestGiverTravelTarget(target))
    {
        ClearQuestGiverEscapeSession(bot);
        return false;
    }

    LootObject loot = AI_VALUE(LootObject, "loot target");
    AutoWowQuestGiverTravel::StagedEntryFacts const facts{
        botAI->AllowActivity(TRAVEL_ACTIVITY, true),
        true,
        target->getStatus() == TRAVEL_STATUS_TRAVEL,
        bot->IsInFlight() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT),
        bot->IsFlying(),
        bot->isMoving(),
        AI_VALUE(bool, "can move around"),
        loot.IsLootPossible(bot)};
    AutoWowQuestGiverTravel::StagedEntryDecision const decision =
        AutoWowQuestGiverTravel::EvaluateStagedEntry(facts);
    AutoWowQuestGiverTravel::RecordStagedEntryDiagnostics(
        bot->GetGUID().GetCounter(), facts, decision);
    if (decision == AutoWowQuestGiverTravel::StagedEntryDecision::NoProgress)
    {
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::NoProgress);
        return true;
    }
    if (decision == AutoWowQuestGiverTravel::StagedEntryDecision::Deferred)
    {
        RecordQuestGiverMovement(
            bot, target, AutoWowQuestGiverTravel::StepKind::Blocked,
            AutoWowQuestGiverTravel::MovementFeedbackResult::Deferred);
        return true;
    }

    return MoveQuestGiverStaged(target);
}

bool MoveToTravelTargetAction::Execute(Event /*event*/)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    if (!IsQuestGiverTravelTarget(target))
        ClearQuestGiverEscapeSession(bot);

    WorldPosition botLocation(bot);
    WorldLocation location = *target->getPosition();

    // Quest acquisition is a persistent party journey. Its target is a precise giver spawn, so a
    // random offset plus one final-point PathGenerator attempt can reject forever on a normal but
    // incomplete long path. Stage only this destination through proven path points or TravelMgr's
    // ordinary walk graph; followers remain on their existing follow strategy.
    if (IsQuestGiverTravelTarget(target))
        return MoveQuestGiverStaged(target);

    Group* group = bot->GetGroup();
    if (group && !urand(0, 1) && bot == botAI->GetGroupLeader() && !bot->IsInCombat())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member == bot)
                continue;

            if (!member->IsAlive())
                continue;

            if (!member->isMoving())
                continue;

            PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);
            if (memberBotAI && !memberBotAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                continue;

            WorldPosition memberPos(member);
            WorldPosition targetPos = *target->getPosition();

            float memberDistance = botLocation.distance(memberPos);

            if (memberDistance < 50.0f)
                continue;
            if (memberDistance > sPlayerbotAIConfig.reactDistance * 20)
                continue;

            // float memberAngle = botLocation.getAngleBetween(targetPos, memberPos);

            // if (botLocation.getMapId() == targetPos.getMapId() && botLocation.getMapId() == memberPos.getMapId() &&
            // memberAngle < static_cast<float>(M_PI) / 2) //We are heading that direction anyway.
            //     continue;

            if (!urand(0, 5))
            {
                std::ostringstream out;
                if (botAI->GetMaster() && !bot->GetGroup()->IsMember(botAI->GetMaster()->GetGUID()))
                    out << "Waiting a bit for ";
                else
                    out << "Please hurry up ";

                out << member->GetName();

                botAI->TellMasterNoFacing(out);
            }

            target->setExpireIn(target->getTimeLeft() + sPlayerbotAIConfig.maxWaitForMove);

            botAI->SetNextCheckDelay(sPlayerbotAIConfig.maxWaitForMove);

            return true;
        }
    }

    float maxDistance = target->getDestination()->getRadiusMin();

    // Spread bots around the target but keep the offset stable per
    // (bot, destination) pair. Previously the angle and radius were
    // re-rolled every time the action re-entered (i.e. every tick the
    // bot wasn't already moving), which made bots oscillate between
    // two random points around the same quest POI instead of
    // committing to one approach.
    uint32 botLow = bot->GetGUID().GetCounter();
    int32 destSeed = static_cast<int32>(location.GetPositionX()) * 73856093 ^
                     static_cast<int32>(location.GetPositionY()) * 19349663;
    uint32 seed = botLow ^ static_cast<uint32>(destSeed);
    float angle = 2.0f * static_cast<float>(M_PI) * static_cast<float>(seed % 1000) / 1000.0f;
    float mod = 0.5f + static_cast<float>((seed / 1000) % 1000) / 2000.0f;  // [0.5, 1.0]

    if (target->getMaxTravelTime() > target->getTimeLeft())  // The bot is late. Speed it up.
    {
        // distance = sPlayerbotAIConfig.fleeDistance;
        // angle = bot->GetAngle(location.GetPositionX(), location.GetPositionY());
        // location = botLocation.getLocation();
    }

    float x = location.GetPositionX();
    float y = location.GetPositionY();
    float z = location.GetPositionZ();
    float mapId = location.GetMapId();

    x += cos(angle) * maxDistance * mod;
    y += sin(angle) * maxDistance * mod;

    bool canMove = false;

    if (bot->IsWithinLOS(x, y, z))
        canMove = MoveNear(mapId, x, y, z, 0);
    else
        canMove = MoveTo(mapId, x, y, z, false, false);

    if (!canMove && !target->isForced())
    {
        target->incRetry(true);

        if (target->isMaxRetry(true))
            target->setStatus(TRAVEL_STATUS_COOLDOWN);
    }
    else
        target->setRetry(true);

    return canMove;
}

bool MoveToTravelTargetAction::isUseful()
{
    TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();

    if (!botAI->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (!target->isTraveling())
        return false;

    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    if (bot->IsFlying())
        return false;

    if (bot->isMoving())
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (loot.IsLootPossible(bot))
        return false;

    // Quest acquisition owns an exact giver point and its staged mover performs the safety probe
    // itself. The generic follow-validity gate is for ordinary travel/follow positioning; it can
    // reject the direct kick before MoveQuestGiverStaged records the TravelMgr/no-progress result.
    if (!IsQuestGiverTravelTarget(target) &&
        !ChooseRpgTargetAction::isFollowValid(bot, *target->getPosition()))
        return false;

    return true;
}
