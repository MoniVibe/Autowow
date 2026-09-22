/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_GIVER_TRAVEL_FEEDBACK_H
#define AUTOWOW_QUEST_GIVER_TRAVEL_FEEDBACK_H

#include "QuestGiverTravelPolicy.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace AutoWowQuestGiverTravel
{
enum class MovementFeedbackResult : std::uint8_t
{
    Accepted,
    // MoveTo can reject a refresh while the previous accepted normal-priority move is still
    // reserved. This is an expected wait state, not a failed movement attempt.
    Deferred,
    // The current path/route probe produced no safe immediate segment. This is bounded
    // no-progress observation, not a movement API rejection.
    NoProgress,
    Rejected,
    Parked
};

enum class TravelMgrDiagnosticReason : std::uint8_t
{
    None,
    RouteEmpty,
    PathEmpty,
    TransitionPrefix,
    ScanWindowExhausted,
    FreshProbeNonNormal,
    FreshProbeIncomplete,
    FreshProbeUngrounded,
    NoCandidate,
    Accepted,
    AcceptedSafeIncomplete
};

enum class QuestWalkPreparedRejectReason : std::uint8_t
{
    None,
    UnsafeProbe,
    PathTooShort,
    MovementNotAllowed,
    AlreadyAtEndpoint,
    DuplicateMove,
    WaitingForLastMove,
    MotionMasterMissing,
    Accepted
};

inline std::string_view QuestWalkPreparedRejectReasonName(QuestWalkPreparedRejectReason reason)
{
    switch (reason)
    {
        case QuestWalkPreparedRejectReason::UnsafeProbe: return "unsafe_probe";
        case QuestWalkPreparedRejectReason::PathTooShort: return "path_too_short";
        case QuestWalkPreparedRejectReason::MovementNotAllowed: return "movement_not_allowed";
        case QuestWalkPreparedRejectReason::AlreadyAtEndpoint: return "already_at_endpoint";
        case QuestWalkPreparedRejectReason::DuplicateMove: return "duplicate_move";
        case QuestWalkPreparedRejectReason::WaitingForLastMove: return "waiting_for_last_move";
        case QuestWalkPreparedRejectReason::MotionMasterMissing: return "motion_master_missing";
        case QuestWalkPreparedRejectReason::Accepted: return "accepted";
        case QuestWalkPreparedRejectReason::None: default: return "none";
    }
}

inline std::string_view StagedEntryDecisionName(StagedEntryDecision decision)
{
    switch (decision)
    {
        case StagedEntryDecision::Execute: return "execute";
        case StagedEntryDecision::Deferred: return "deferred";
        case StagedEntryDecision::NoProgress: return "no_progress";
        default: return "unknown";
    }
}

// Keep only the latest policy observation per bot. This is read-only telemetry and deliberately
// does not retain history or participate in the movement decision.
struct StagedEntryDiagnostics
{
    StagedEntryFacts facts{};
    StagedEntryDecision decision = StagedEntryDecision::NoProgress;
};

inline std::string_view TravelMgrDiagnosticReasonName(TravelMgrDiagnosticReason reason)
{
    switch (reason)
    {
        case TravelMgrDiagnosticReason::RouteEmpty: return "route_empty";
        case TravelMgrDiagnosticReason::PathEmpty: return "path_empty";
        case TravelMgrDiagnosticReason::TransitionPrefix: return "transition_prefix";
        case TravelMgrDiagnosticReason::ScanWindowExhausted: return "scan_window_exhausted";
        case TravelMgrDiagnosticReason::FreshProbeNonNormal: return "fresh_probe_non_normal";
        case TravelMgrDiagnosticReason::FreshProbeIncomplete: return "fresh_probe_incomplete";
        case TravelMgrDiagnosticReason::FreshProbeUngrounded: return "fresh_probe_ungrounded";
        case TravelMgrDiagnosticReason::NoCandidate: return "no_candidate";
        case TravelMgrDiagnosticReason::Accepted: return "accepted";
        case TravelMgrDiagnosticReason::AcceptedSafeIncomplete:
            return "accepted_safe_incomplete";
        case TravelMgrDiagnosticReason::None: default: return "none";
    }
}

struct TravelMgrDiagnostics
{
    std::uint32_t pathPointCount = 0;
    std::uint32_t scanPointsConsidered = 0;
    std::uint32_t sameMapWalkPoints = 0;
    std::uint32_t distanceEligiblePoints = 0;
    std::uint32_t freshProbeAttempts = 0;
    std::uint32_t freshProbeAccepts = 0;
    std::uint32_t selectedRouteIndex = 0;
    float selectedDistance = -1.0f;
    float nearestRouteDistance = -1.0f;
    float farthestRouteDistance = -1.0f;
    bool unattachedStartAllowed = false;
    bool selected = false;
    TravelMgrDiagnosticReason reason = TravelMgrDiagnosticReason::None;
};

// A normal navmesh probe can end at a grounded, forward-moving frontier when the requested
// endpoint is not reachable from the current map state. DungeonPathSafety marks that result safe
// only when it is exactly PATHFIND_NORMAL|PATHFIND_INCOMPLETE, has real intermediate points, and
// passes all vertical/ground/progress checks. It is therefore safe to execute as a bounded segment,
// but it must never be treated as completion of the requested destination.
inline bool IsSafeIncompleteTravelMgrProbe(PathFacts const& facts)
{
    constexpr std::uint32_t safeIncompletePath = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    return facts.calculated && facts.safe && facts.normalPathType &&
        facts.pathType == safeIncompletePath && facts.pointCount >= 3 && facts.validHeight &&
        std::isfinite(facts.endpointDistance) && std::isfinite(facts.pathLength) &&
        facts.pathLength > 0.0f;
}

// This is the segment-level counterpart to IsUsableTravelMgrReanchorCandidate. It keeps the
// original complete-probe contract and adds only the explicitly safe incomplete frontier form.
// Route type, map identity, finite coordinates, distance bounds, and destination progress remain
// hard requirements, so this helper cannot authorize a transition, teleport, or zero-progress
// step. Destination progress may be straight-line or measured along the same selected route when
// terrain requires a bounded detour.
inline bool IsUsableTravelMgrSegmentCandidate(
    TravelMgrReanchorCandidateFacts const& candidate,
    float maximumDirectDistance = kTravelMgrReanchorMaximumDirectDistance)
{
    bool const straightLineProgress = std::isfinite(candidate.targetDistanceReduction) &&
        candidate.targetDistanceReduction >= kPartialTargetProgressEpsilon;
    bool const routedProgress = std::isfinite(candidate.routeDistanceReduction) &&
        candidate.routeDistanceReduction >= kPartialTargetProgressEpsilon;
    if (candidate.nodeKind != TravelMgrRouteNodeKind::Walk || !candidate.sameMap ||
        !IsFinite(candidate.point) || !std::isfinite(maximumDirectDistance) ||
        maximumDirectDistance < kTravelMgrReanchorMinimumDirectDistance ||
        !std::isfinite(candidate.directDistance) ||
        candidate.directDistance < kTravelMgrReanchorMinimumDirectDistance ||
        candidate.directDistance > maximumDirectDistance ||
        (!straightLineProgress && !routedProgress) ||
        !std::isfinite(candidate.freshProbe.pathLength) || candidate.freshProbe.pathLength <= 0.0f)
        return false;

    return IsCompleteReprobe(candidate.freshProbe) ||
        IsSafeIncompleteTravelMgrProbe(candidate.freshProbe);
}

// Deterministically choose the most advanced safe segment before the first route transition. The
// existing tie-breaker remains authoritative for route index, direct distance, and path length.
inline std::optional<std::size_t> SelectTravelMgrSegmentCandidate(
    std::vector<TravelMgrReanchorCandidateFacts> const& candidates,
    float maximumDirectDistance = kTravelMgrReanchorMaximumDirectDistance,
    std::size_t maximumScanPoints = kMaxTravelMgrReanchorScanPoints)
{
    if (maximumScanPoints == 0U)
        return std::nullopt;

    std::optional<std::size_t> selected;
    std::size_t scanned = 0U;
    for (std::size_t index = 0; index < candidates.size() && scanned < maximumScanPoints; ++index)
    {
        TravelMgrReanchorCandidateFacts const& candidate = candidates[index];
        if (candidate.nodeKind != TravelMgrRouteNodeKind::Walk)
            break;
        ++scanned;

        if (!IsUsableTravelMgrSegmentCandidate(candidate, maximumDirectDistance))
            continue;
        if (!selected || IsBetterTravelMgrReanchorCandidate(candidate, candidates[*selected]))
            selected = index;
    }
    return selected;
}

// One bounded, last-observation record for the autonomous quest walk selector. It deliberately
// keeps only scalar probe facts and the final rejection class; it does not retain route points,
// history, or a second movement policy. This is telemetry only and never authorizes movement.
struct QuestWalkProbeDiagnostics
{
    bool attempted = false;
    std::string selectionSource = "none";
    bool directProbeAttempted = false;
    bool directProbeSafe = false;
    std::uint32_t directPathType = PATHFIND_BLANK;
    std::uint32_t directPointCount = 0;
    float directEndpointDistance = -1.0f;
    float directPathLength = 0.0f;
    std::string directReason;
    std::string directNavmeshReason;
    std::string directGroundLineReason;
    bool travelMgrAttempted = false;
    TravelMgrDiagnostics travelMgr{};
    bool walkPreparedCalled = false;
    bool walkPreparedAccepted = false;
    QuestWalkPreparedRejectReason walkRejectReason = QuestWalkPreparedRejectReason::None;
};

inline std::string_view MovementFeedbackResultName(MovementFeedbackResult result)
{
    switch (result)
    {
        case MovementFeedbackResult::Accepted: return "accepted";
        case MovementFeedbackResult::Deferred: return "deferred";
        case MovementFeedbackResult::NoProgress: return "no_progress";
        case MovementFeedbackResult::Rejected: return "rejected";
        case MovementFeedbackResult::Parked: return "parked";
        default: return "unknown";
    }
}

inline bool IsMovementRefreshDeferred(bool waitingForLastMove, bool duplicateMove)
{
    return waitingForLastMove || duplicateMove;
}

struct MovementFeedback
{
    std::uint64_t sequence = 0;
    std::uint32_t observations = 0;
    std::uint32_t movementAttempts = 0;
    std::uint32_t rejectedAttempts = 0;
    std::uint32_t noProgressChecks = 0;
    StepKind step = StepKind::Blocked;
    MovementFeedbackResult result = MovementFeedbackResult::Rejected;
    bool moving = false;
    float remainingDistance = -1.0f;
};

inline std::unordered_map<std::uint32_t, MovementFeedback> movementFeedbackByBot;
inline std::unordered_map<std::uint32_t, TravelMgrDiagnostics> travelMgrDiagnosticsByBot;
inline std::unordered_map<std::uint32_t, StagedEntryDiagnostics> stagedEntryDiagnosticsByBot;
inline std::unordered_map<std::uint32_t, QuestWalkProbeDiagnostics> questWalkDiagnosticsByBot;

inline void ClearMovementFeedback(std::uint32_t botGuid)
{
    movementFeedbackByBot.erase(botGuid);
    travelMgrDiagnosticsByBot.erase(botGuid);
    stagedEntryDiagnosticsByBot.erase(botGuid);
    questWalkDiagnosticsByBot.erase(botGuid);
}

inline void RecordTravelMgrDiagnostics(std::uint32_t botGuid, TravelMgrDiagnostics diagnostics)
{
    travelMgrDiagnosticsByBot[botGuid] = diagnostics;
}

inline TravelMgrDiagnostics const* ReadTravelMgrDiagnostics(std::uint32_t botGuid)
{
    auto const it = travelMgrDiagnosticsByBot.find(botGuid);
    return it == travelMgrDiagnosticsByBot.end() ? nullptr : &it->second;
}

inline void RecordQuestWalkDiagnostics(std::uint32_t botGuid, QuestWalkProbeDiagnostics diagnostics)
{
    questWalkDiagnosticsByBot[botGuid] = std::move(diagnostics);
}

inline QuestWalkProbeDiagnostics const* ReadQuestWalkDiagnostics(std::uint32_t botGuid)
{
    auto const it = questWalkDiagnosticsByBot.find(botGuid);
    return it == questWalkDiagnosticsByBot.end() ? nullptr : &it->second;
}

inline void RecordStagedEntryDiagnostics(std::uint32_t botGuid, StagedEntryFacts const& facts,
                                         StagedEntryDecision decision)
{
    stagedEntryDiagnosticsByBot[botGuid] = StagedEntryDiagnostics{facts, decision};
}

inline StagedEntryDiagnostics const* ReadStagedEntryDiagnostics(std::uint32_t botGuid)
{
    auto const it = stagedEntryDiagnosticsByBot.find(botGuid);
    return it == stagedEntryDiagnosticsByBot.end() ? nullptr : &it->second;
}

inline void RecordMovementFeedback(std::uint32_t botGuid, StepKind step,
                                   MovementFeedbackResult result, bool moving,
                                   float remainingDistance)
{
    MovementFeedback& feedback = movementFeedbackByBot[botGuid];
    ++feedback.sequence;
    ++feedback.observations;
    if (result == MovementFeedbackResult::Accepted || result == MovementFeedbackResult::Rejected)
        ++feedback.movementAttempts;
    if (result == MovementFeedbackResult::Accepted)
        feedback.noProgressChecks = 0;
    else if (result == MovementFeedbackResult::NoProgress)
        ++feedback.noProgressChecks;
    if (result == MovementFeedbackResult::Rejected && !moving)
        ++feedback.rejectedAttempts;
    feedback.step = step;
    feedback.result = result;
    feedback.moving = moving;
    feedback.remainingDistance = remainingDistance;
}

inline MovementFeedback const* ReadMovementFeedback(std::uint32_t botGuid)
{
    auto const it = movementFeedbackByBot.find(botGuid);
    return it == movementFeedbackByBot.end() ? nullptr : &it->second;
}
}

#endif  // AUTOWOW_QUEST_GIVER_TRAVEL_FEEDBACK_H
