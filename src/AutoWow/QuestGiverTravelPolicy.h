/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_GIVER_TRAVEL_POLICY_H
#define AUTOWOW_QUEST_GIVER_TRAVEL_POLICY_H

#include "PathGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

// Pure decision logic for the quest-giver movement adapter. The runtime adapter is responsible for
// probing the live map and asking TravelNodeMap for a segmented route; this header only decides which
// already-probed movement source is admissible and which deterministic partial point to use.
namespace AutoWowQuestGiverTravel
{
// The acquisition deadline is route-aware. Successful staged segments are progress and must not
// consume a retry budget; the absolute cap is only a final fail-closed guard for a route that never
// converges.
constexpr std::uint32_t kBaseAcquisitionLifetimeMs = 5U * 60U * 1000U;
constexpr std::uint32_t kMaxAcquisitionLifetimeMs = 30U * 60U * 1000U;
constexpr std::uint32_t kMaxMovementRejects = 6U;
constexpr std::uint32_t kMaxNoProgressChecks = 6U;
constexpr std::uint32_t kMaxStaleGiverWaitMs = 60U * 1000U;
constexpr std::uint32_t kMaxPartyArrivalWaitMs = 90U * 1000U;

enum class GiverLookupState : std::uint8_t
{
    Resolved,
    GridUnloaded,
    ConfirmedMissing
};

inline GiverLookupState ClassifyGiverLookup(bool spawnDataExists, bool gridLoaded,
                                            bool liveObjectResolved)
{
    if (!spawnDataExists)
        return GiverLookupState::ConfirmedMissing;
    if (!gridLoaded)
        return GiverLookupState::GridUnloaded;
    return liveObjectResolved ? GiverLookupState::Resolved : GiverLookupState::ConfirmedMissing;
}

enum class LifecycleDecision : std::uint8_t
{
    Continue,
    BlockedTimeout,
    BlockedMovement,
    BlockedNoProgress,
    BlockedStaleGiver,
    BlockedPartyArrival
};

struct LifecycleFacts
{
    std::uint32_t elapsedMs = 0;
    std::uint32_t routeDeadlineMs = kBaseAcquisitionLifetimeMs;
    std::uint32_t movementAttempts = 0;
    std::uint32_t movementRejects = 0;
    std::uint32_t noProgressChecks = 0;
    std::uint32_t staleGiverWaitMs = 0;
    std::uint32_t partyArrivalWaitMs = 0;
};

inline std::uint32_t AcquisitionDeadlineMs(double routeDistance, float movementSpeed)
{
    if (!std::isfinite(routeDistance) || routeDistance <= 0.0 ||
        !std::isfinite(movementSpeed) || movementSpeed <= 0.0f)
        return kBaseAcquisitionLifetimeMs;

    // Three expected route durations plus a fixed startup/repath allowance keeps long but healthy
    // staged routes alive while retaining a bounded upper limit for a permanently stalled target.
    double const expectedMs = routeDistance / static_cast<double>(movementSpeed) * 1000.0;
    double const deadlineMs = expectedMs * 3.0 + 60.0 * 1000.0;
    if (!std::isfinite(deadlineMs))
        return kMaxAcquisitionLifetimeMs;
    if (deadlineMs <= static_cast<double>(kBaseAcquisitionLifetimeMs))
        return kBaseAcquisitionLifetimeMs;
    if (deadlineMs >= static_cast<double>(kMaxAcquisitionLifetimeMs))
        return kMaxAcquisitionLifetimeMs;
    return static_cast<std::uint32_t>(deadlineMs);
}

// Creation-time journey clock.  The route and speed are sampled exactly once; after construction
// neither a mount nor a slow can shorten or extend this bounded absolute deadline.
class AbsoluteSessionDeadline
{
public:
    static AbsoluteSessionDeadline Create(std::uint64_t nowMs, double routeDistance,
                                          float movementSpeed)
    {
        return AbsoluteSessionDeadline(nowMs, AcquisitionDeadlineMs(routeDistance, movementSpeed));
    }

    std::uint64_t CreatedAtMs() const { return createdAtMs_; }
    std::uint64_t DeadlineAtMs() const { return deadlineAtMs_; }
    std::uint32_t DurationMs() const { return durationMs_; }
    std::uint32_t ElapsedMs(std::uint64_t nowMs) const
    {
        if (nowMs <= createdAtMs_)
            return 0;
        std::uint64_t const elapsed = nowMs - createdAtMs_;
        return elapsed >= durationMs_ ? durationMs_ : static_cast<std::uint32_t>(elapsed);
    }
    std::uint32_t RemainingMs(std::uint64_t nowMs) const
    {
        std::uint32_t const elapsed = ElapsedMs(nowMs);
        return elapsed < durationMs_ ? durationMs_ - elapsed : 0;
    }
    bool IsExpired(std::uint64_t nowMs) const { return nowMs >= deadlineAtMs_; }

private:
    AbsoluteSessionDeadline(std::uint64_t createdAtMs, std::uint32_t durationMs)
        : createdAtMs_(createdAtMs), deadlineAtMs_(createdAtMs + durationMs),
          durationMs_(durationMs)
    {
    }

    std::uint64_t const createdAtMs_;
    std::uint64_t const deadlineAtMs_;
    std::uint32_t const durationMs_;
};

inline LifecycleDecision EvaluateLifecycle(LifecycleFacts const& facts)
{
    // These are elapsed waits, not polling-loop counts. A despawned giver and a permanently late
    // follower therefore become explicit terminal states promptly even when the poll cadence
    // changes.
    if (facts.staleGiverWaitMs >= kMaxStaleGiverWaitMs)
        return LifecycleDecision::BlockedStaleGiver;
    if (facts.partyArrivalWaitMs >= kMaxPartyArrivalWaitMs)
        return LifecycleDecision::BlockedPartyArrival;
    // Successful movementAttempts are telemetry only. Rejections/no-progress are the bounded
    // failure budget; a healthy q954-length route may require more than 24 successful segments.
    if (facts.noProgressChecks >= kMaxNoProgressChecks)
        return LifecycleDecision::BlockedNoProgress;
    if (facts.movementRejects >= kMaxMovementRejects)
        return LifecycleDecision::BlockedMovement;
    if (facts.elapsedMs >= facts.routeDeadlineMs)
        return LifecycleDecision::BlockedTimeout;
    return LifecycleDecision::Continue;
}

inline std::string_view LifecycleDecisionName(LifecycleDecision decision)
{
    switch (decision)
    {
        case LifecycleDecision::Continue: return "continue";
        case LifecycleDecision::BlockedTimeout: return "acquisition_timeout";
        case LifecycleDecision::BlockedMovement: return "movement_retry_limit";
        case LifecycleDecision::BlockedNoProgress: return "movement_no_progress_timeout";
        case LifecycleDecision::BlockedStaleGiver: return "stale_giver";
        case LifecycleDecision::BlockedPartyArrival: return "party_arrival_timeout";
        default: return "unknown";
    }
}

enum class StepKind : std::uint8_t
{
    Direct,
    PartialPath,
    TravelMgrSegment,
    LocalDetour,
    EscapeRing,
    Blocked
};

enum class StagedEntryDecision : std::uint8_t
{
    Execute,
    Deferred,
    NoProgress
};

// A hearthstone is an emergency recovery route for one very narrow failure mode. It is not a
// generic no-path escape: the runtime must first prove that the bounded quest-giver local/escape
// search ended at an off-navmesh start. Keep this evidence separate from ordinary movement
// diagnostics so a door, a missing route edge, or a transient path failure cannot authorize it.
struct OffNavmeshProbeFacts
{
    bool attempted = false;
    std::uint32_t pathType = PATHFIND_BLANK;
    std::string_view groundLineReason;
    std::uint32_t consecutiveStrongProbes = 0;
};

constexpr std::uint32_t kRequiredStrongOffNavmeshProbes = 2U;

inline bool IsStrongOffNavmeshProbe(OffNavmeshProbeFacts const& facts)
{
    if (!facts.attempted || (facts.pathType & PATHFIND_FARFROMPOLY) == 0)
        return false;

    // These are the two ground-line failures observed when the source itself is not attached to
    // the local navigation/terrain surface. Other ground-line failures remain ordinary blocked
    // terrain and must not trigger hearth recovery.
    return facts.groundLineReason == "ground_line_too_short" ||
        facts.groundLineReason == "ground_line_missing_floor";
}

inline bool HasStrongOffNavmeshEvidence(OffNavmeshProbeFacts const& facts)
{
    return IsStrongOffNavmeshProbe(facts) &&
        facts.consecutiveStrongProbes >= kRequiredStrongOffNavmeshProbes;
}

inline bool IsBoundedLocalEscapeRecoveryExhausted(std::string_view lastStep,
                                                  std::string_view lastResult)
{
    // MoveToTravelTargetAction emits this exact pair only from its terminal no-progress branch,
    // after TravelMgr/local detour selection and the finite escape-ring search have both failed.
    // A generic blocked/no-progress observation is deliberately insufficient.
    return lastStep == "escape_ring" && lastResult == "no_progress";
}

enum class HearthRecoveryDecision : std::uint8_t
{
    NotEligible,
    Requested,
    Unavailable,
    Cooldown
};

constexpr std::uint32_t kMaxHearthRecoveryAttempts = 1U;

struct HearthRecoveryFacts
{
    bool sessionPendingOrJustExhausted = false;
    bool botAlive = false;
    bool inCombat = false;
    bool inFlight = false;
    bool beingTeleported = false;
    bool campaignTravelOwned = false;
    bool boundedLocalEscapeRecoveryExhausted = false;
    bool strongOffNavmeshEvidence = false;
    bool hearthstoneUseful = false;
    bool hearthstoneAvailable = false;
    std::uint32_t hearthstoneAttempts = 0;
};

inline HearthRecoveryDecision EvaluateHearthRecovery(HearthRecoveryFacts const& facts)
{
    if (facts.hearthstoneAttempts >= kMaxHearthRecoveryAttempts)
        return HearthRecoveryDecision::Cooldown;

    if (!facts.sessionPendingOrJustExhausted || !facts.botAlive || facts.inCombat ||
        facts.inFlight || facts.beingTeleported || facts.campaignTravelOwned ||
        !facts.boundedLocalEscapeRecoveryExhausted || !facts.strongOffNavmeshEvidence)
        return HearthRecoveryDecision::NotEligible;

    if (!facts.hearthstoneUseful || !facts.hearthstoneAvailable)
        return HearthRecoveryDecision::Unavailable;

    return HearthRecoveryDecision::Requested;
}

inline std::string_view HearthRecoveryDecisionName(HearthRecoveryDecision decision)
{
    switch (decision)
    {
        case HearthRecoveryDecision::NotEligible: return "not_eligible";
        case HearthRecoveryDecision::Requested: return "requested";
        case HearthRecoveryDecision::Unavailable: return "unavailable";
        case HearthRecoveryDecision::Cooldown: return "cooldown";
        default: return "unknown";
    }
}

enum class QuestAcquisitionStrategyRole : std::uint8_t
{
    Leader,
    Follower
};

enum class QuestAcquisitionStrategyDecision : std::uint8_t
{
    Yield,
    AlreadyOwned,
    Restore
};

struct QuestAcquisitionStrategyFacts
{
    bool sessionPending = false;
    bool botAlive = false;
    bool inCombat = false;
    bool inFlight = false;
    bool beingTeleported = false;
    bool campaignTravelOwned = false;
    bool paused = false;
    bool nonCombatGatesClear = false;
    QuestAcquisitionStrategyRole role = QuestAcquisitionStrategyRole::Leader;
    bool hasTravel = false;
    bool hasFollow = false;
    bool hasGrind = false;
    bool hasRandomMove = false;
    bool hasNewRpg = false;
};

inline QuestAcquisitionStrategyDecision EvaluateQuestAcquisitionStrategy(
    QuestAcquisitionStrategyFacts const& facts)
{
    if (!facts.sessionPending || !facts.botAlive || facts.inCombat || facts.inFlight ||
        facts.beingTeleported || facts.campaignTravelOwned || facts.paused ||
        !facts.nonCombatGatesClear)
        return QuestAcquisitionStrategyDecision::Yield;

    bool const owned = facts.role == QuestAcquisitionStrategyRole::Leader
        ? facts.hasTravel && !facts.hasFollow && !facts.hasGrind && !facts.hasRandomMove &&
              !facts.hasNewRpg
        : facts.hasFollow && !facts.hasGrind && !facts.hasRandomMove && !facts.hasNewRpg &&
              !facts.hasTravel;
    return owned ? QuestAcquisitionStrategyDecision::AlreadyOwned
                 : QuestAcquisitionStrategyDecision::Restore;
}

// Cohesive quest acquisition is stricter than escorting an already-questing leader. Before a
// route is selected, every alive expected party member must pass the ordinary core acquisition
// checks that are currently knowable. An unresolved giver may defer only the exact object check;
// it may not waive status, prerequisite, or quest-log capacity checks.
struct PartyQuestEligibilityFacts
{
    bool alive = false;
    bool questStatusNone = false;
    bool canTake = false;
    bool canAdd = false;
    bool exactGiverEligibilityKnown = false;
    bool exactGiverEligible = false;
};

inline bool IsCommonPartyQuestEligible(
    std::vector<PartyQuestEligibilityFacts> const& members)
{
    bool foundAliveMember = false;
    for (PartyQuestEligibilityFacts const& member : members)
    {
        if (!member.alive)
            continue;
        foundAliveMember = true;
        if (!member.questStatusNone || !member.canTake || !member.canAdd ||
            (member.exactGiverEligibilityKnown && !member.exactGiverEligible))
            return false;
    }
    return foundAliveMember;
}

struct StagedEntryFacts
{
    bool activityAllowed = false;
    bool exactQuestAcquisitionTarget = false;
    bool travelStatusActive = false;
    bool inFlight = false;
    bool flying = false;
    bool moving = false;
    bool canMoveAround = false;
    bool lootPossible = false;
};

inline StagedEntryDecision EvaluateStagedEntry(StagedEntryFacts const& facts)
{
    if (!facts.activityAllowed || !facts.exactQuestAcquisitionTarget ||
        !facts.travelStatusActive || facts.inFlight || facts.flying || !facts.canMoveAround ||
        facts.lootPossible)
        return StagedEntryDecision::NoProgress;
    if (facts.moving)
        return StagedEntryDecision::Deferred;
    return StagedEntryDecision::Execute;
}

struct PathFacts
{
    bool calculated = false;
    bool safe = false;
    bool normalPathType = false;
    std::uint32_t pathType = PATHFIND_BLANK;
    std::size_t pointCount = 0;
    float endpointDistance = 0.0f;
    float pathLength = 0.0f;
    // Existing policy-only callers omit this field and retain the historical "known valid"
    // default. Production probes must copy ProbeResult::allGroundSamplesValid into it.
    bool validHeight = true;
    // This bit is explicit proof from the validated ground-line probe mode. It must never be
    // inferred from a blank/non-normal path type.
    bool validatedGroundLine = false;
};

struct Waypoint
{
    std::uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// A partial frontier is allowed to advance the journey only when it is measurably closer to the
// actual quest-giver target. This small deterministic margin rejects sideways/regressive noise;
// bounded regressions remain the exclusive responsibility of the escape-ring policy below.
constexpr float kPartialTargetProgressEpsilon = 0.5f;

constexpr float kLocalDetourMinimumRouteLength = 1.0f;
constexpr float kLocalDetourMaximumRouteLength = 32.0f;
constexpr float kLocalDetourMinimumDistanceReduction = 0.5f;

// The escape is deliberately finite and repeatable. Sixteen evenly-spaced angles cover the full
// circle without a per-tick random seed; short probes are tried before the wider rings so an
// unattached start can find a nearby topology cell without turning this into an unbounded search.
constexpr float kEscapeRingPi = 3.14159265358979323846f;
constexpr std::array<float, 5> kEscapeRingRadii{4.0f, 8.0f, 16.0f, 32.0f, 48.0f};
constexpr std::array<float, 16> kEscapeRingAngles{
    0.0f,
    kEscapeRingPi / 8.0f,
    2.0f * kEscapeRingPi / 8.0f,
    3.0f * kEscapeRingPi / 8.0f,
    4.0f * kEscapeRingPi / 8.0f,
    5.0f * kEscapeRingPi / 8.0f,
    6.0f * kEscapeRingPi / 8.0f,
    7.0f * kEscapeRingPi / 8.0f,
    8.0f * kEscapeRingPi / 8.0f,
    9.0f * kEscapeRingPi / 8.0f,
    10.0f * kEscapeRingPi / 8.0f,
    11.0f * kEscapeRingPi / 8.0f,
    12.0f * kEscapeRingPi / 8.0f,
    13.0f * kEscapeRingPi / 8.0f,
    14.0f * kEscapeRingPi / 8.0f,
    15.0f * kEscapeRingPi / 8.0f};
constexpr float kEscapeCellSize = 8.0f;
constexpr std::uint32_t kMaxEscapeAttempts = 8U;
constexpr std::size_t kMaxEscapeVisitedCells = 512U;
constexpr float kMaxEscapeTotalDistance = 192.0f;
constexpr float kMaxEscapeTotalRegression = 64.0f;
constexpr float kEscapeMaximumRouteLength = 96.0f;
constexpr float kEscapeMinimumRouteLength = 1.0f;
constexpr float kEscapeMinimumImprovement = 0.5f;
// An epoch is a bounded local topology search. A later epoch is allowed only after the bot has
// made meaningful net progress toward the same target; this prevents a failed ring from becoming
// an unbounded oscillation loop just because the search state was reset.
constexpr std::uint32_t kMaxEscapeEpochs = 3U;
constexpr std::uint32_t kMaxEscapeJourneyAttempts =
    kMaxEscapeEpochs * kMaxEscapeAttempts;
constexpr float kEscapeMinimumEpochProgress = 4.0f;

struct LocalDetourCandidateFacts
{
    bool sameMap = false;
    bool finite = false;
    bool completeNormalPath = false;
    float routeLength = 0.0f;
    float distanceReduction = 0.0f;
    // Appended fields keep the existing policy-test aggregate shape source-compatible while
    // allowing production selection to enforce the same strict path contract as escape rings.
    std::uint32_t pathType = PATHFIND_NORMAL;
    bool validHeight = true;
    bool validatedGroundLine = false;
};

inline bool IsFinite(Waypoint const& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

inline float Distance2d(Waypoint const& left, Waypoint const& right)
{
    float const dx = right.x - left.x;
    float const dy = right.y - left.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline bool HasTargetAwarePartialProgress(
    Waypoint const& start, Waypoint const& target, Waypoint const& candidate,
    float minimumProgress = kPartialTargetProgressEpsilon)
{
    if (start.mapId != target.mapId || candidate.mapId != start.mapId ||
        !IsFinite(start) || !IsFinite(target) || !IsFinite(candidate) ||
        !std::isfinite(minimumProgress) || minimumProgress < 0.0f)
        return false;

    float const startDistance = Distance2d(start, target);
    float const candidateDistance = Distance2d(candidate, target);
    return std::isfinite(startDistance) && std::isfinite(candidateDistance) &&
        candidateDistance + minimumProgress < startDistance;
}

inline bool IsComplete(PathFacts const& facts, float endpointTolerance = 2.5f)
{
    // Navmesh authorization remains exact PATHFIND_NORMAL. A ground-line result is authorized only
    // by its explicit validation bit and the exact blank path type produced by that probe mode;
    // arbitrary blank/non-normal paths and every flagged navmesh result remain rejected.
    bool const exactNormalNavmesh =
        facts.normalPathType && facts.pathType == PATHFIND_NORMAL;
    bool const explicitValidatedGroundLine =
        facts.validatedGroundLine && facts.pathType == PATHFIND_BLANK;
    return facts.calculated && facts.safe &&
        (exactNormalNavmesh || explicitValidatedGroundLine) && facts.validHeight &&
        facts.pointCount >= 2 &&
        std::isfinite(facts.endpointDistance) && facts.endpointDistance <= endpointTolerance;
}

// A partial-frontier point is executable only when a fresh probe of that exact segment satisfies
// the complete-path invariant. The initial quest-giver probe may be incomplete; it is never itself
// permission to issue MoveTo.
inline bool IsCompleteReprobe(PathFacts const& facts, float endpointTolerance = 2.5f)
{
    return IsComplete(facts, endpointTolerance);
}

// TravelMgr routes can contain a stale or unreachable prefix before a usable walk point. Re-anchor
// only within this finite walk-only prefix; the runtime must still obtain every `freshProbe` from the
// same live start before asking this selector to choose one.
constexpr std::size_t kMaxTravelMgrReanchorScanPoints = 32U;
constexpr float kTravelMgrReanchorMinimumDirectDistance = 2.0f;
constexpr float kTravelMgrReanchorMaximumDirectDistance = 150.0f;

enum class TravelMgrRouteNodeKind : std::uint8_t
{
    Walk,
    Transition
};

struct TravelMgrReanchorCandidateFacts
{
    std::size_t routeIndex = 0;
    TravelMgrRouteNodeKind nodeKind = TravelMgrRouteNodeKind::Transition;
    Waypoint point{};
    bool sameMap = false;
    float directDistance = 0.0f;
    float targetDistanceReduction = 0.0f;
    // A TravelMgr route can make a legitimate local detour before converging on the exact
    // destination. This is the reduction in remaining distance measured along that same,
    // destination-directed route. It is optional for callers that already prove straight-line
    // progress; production segmented-route callers populate it from the live route and fresh
    // endpoint, never from an arbitrary waypoint.
    float routeDistanceReduction = 0.0f;
    PathFacts freshProbe{};
};

inline bool IsUsableTravelMgrReanchorCandidate(
    TravelMgrReanchorCandidateFacts const& candidate,
    float maximumDirectDistance = kTravelMgrReanchorMaximumDirectDistance)
{
    if (candidate.nodeKind != TravelMgrRouteNodeKind::Walk || !candidate.sameMap ||
        !IsFinite(candidate.point) || !std::isfinite(maximumDirectDistance) ||
        maximumDirectDistance < kTravelMgrReanchorMinimumDirectDistance ||
        !std::isfinite(candidate.directDistance) ||
        candidate.directDistance < kTravelMgrReanchorMinimumDirectDistance ||
        candidate.directDistance > maximumDirectDistance ||
        !std::isfinite(candidate.targetDistanceReduction) ||
        candidate.targetDistanceReduction < kPartialTargetProgressEpsilon ||
        !IsCompleteReprobe(candidate.freshProbe) ||
        !std::isfinite(candidate.freshProbe.pathLength) || candidate.freshProbe.pathLength <= 0.0f)
        return false;
    return true;
}

inline bool IsBetterTravelMgrReanchorCandidate(TravelMgrReanchorCandidateFacts const& candidate,
                                                TravelMgrReanchorCandidateFacts const& current)
{
    constexpr float scoreEpsilon = 0.001f;
    if (candidate.routeIndex != current.routeIndex)
        return candidate.routeIndex > current.routeIndex;
    if (candidate.directDistance < current.directDistance - scoreEpsilon)
        return true;
    if (std::fabs(candidate.directDistance - current.directDistance) > scoreEpsilon)
        return false;
    if (candidate.freshProbe.pathLength < current.freshProbe.pathLength - scoreEpsilon)
        return true;
    return false;
}

// Returns the index into `candidates`, not the route index. The scan is intentionally prefix-only:
// a transition is a hard boundary, while an invalid walk point is merely a failed candidate and does
// not prevent a later walk point in the bounded prefix from being considered.
inline std::optional<std::size_t> SelectTravelMgrReanchorCandidate(
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

        if (!IsUsableTravelMgrReanchorCandidate(candidate, maximumDirectDistance))
            continue;
        if (!selected || IsBetterTravelMgrReanchorCandidate(candidate, candidates[*selected]))
            selected = index;
    }
    return selected;
}

inline bool IsBoundedPartial(PathFacts const& facts, float minimumPathLength = 2.0f)
{
    // The initial probe is only a bounded frontier nomination. It may be an incomplete-only
    // corridor, so normal/safe are deliberately deferred to IsCompleteReprobe for the exact
    // candidate segment. Reject path flags that would make even that frontier unsafe.
    constexpr std::uint32_t allowedPathTypes = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    return facts.calculated && (facts.pathType & PATHFIND_INCOMPLETE) != 0 &&
        (facts.pathType & ~allowedPathTypes) == 0 && facts.pointCount >= 2 &&
        std::isfinite(facts.pathLength) && facts.pathLength >= minimumPathLength;
}

inline StepKind SelectStep(PathFacts const& facts)
{
    if (IsComplete(facts))
        return StepKind::Direct;
    if (IsBoundedPartial(facts))
        return StepKind::PartialPath;

    // Anything not proven complete or safe is delegated to the normal TravelMgr graph. The caller
    // must still find an ordinary walk segment; otherwise it returns Blocked without a fallback move.
    if (facts.calculated)
        return StepKind::TravelMgrSegment;
    return StepKind::Blocked;
}

inline std::string_view StepKindName(StepKind kind)
{
    switch (kind)
    {
        case StepKind::Direct: return "direct";
        case StepKind::PartialPath: return "partial_path";
        case StepKind::TravelMgrSegment: return "travel_mgr_segment";
        case StepKind::LocalDetour: return "local_detour";
        case StepKind::EscapeRing: return "escape_ring";
        case StepKind::Blocked: return "blocked";
        default: return "unknown";
    }
}

inline bool IsUsableLocalDetour(LocalDetourCandidateFacts const& facts,
                                float maximumRouteLength = kLocalDetourMaximumRouteLength,
                                float minimumDistanceReduction = kLocalDetourMinimumDistanceReduction)
{
    bool const exactNormalNavmesh =
        facts.completeNormalPath && facts.pathType == PATHFIND_NORMAL;
    bool const explicitValidatedGroundLine =
        facts.validatedGroundLine && facts.pathType == PATHFIND_BLANK;
    return facts.sameMap && facts.finite &&
        (exactNormalNavmesh || explicitValidatedGroundLine) && facts.validHeight &&
        std::isfinite(facts.routeLength) && facts.routeLength > kLocalDetourMinimumRouteLength &&
        facts.routeLength <= maximumRouteLength && std::isfinite(facts.distanceReduction) &&
        facts.distanceReduction >= minimumDistanceReduction;
}

inline std::optional<std::size_t> SelectLocalDetour(
    std::vector<LocalDetourCandidateFacts> const& candidates,
    float maximumRouteLength = kLocalDetourMaximumRouteLength,
    float minimumDistanceReduction = kLocalDetourMinimumDistanceReduction)
{
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        if (!IsUsableLocalDetour(candidates[index], maximumRouteLength, minimumDistanceReduction))
            continue;
        if (!selected)
        {
            selected = index;
            continue;
        }

        LocalDetourCandidateFacts const& current = candidates[*selected];
        LocalDetourCandidateFacts const& candidate = candidates[index];
        constexpr float scoreEpsilon = 0.001f;
        if (candidate.distanceReduction > current.distanceReduction + scoreEpsilon ||
            (std::fabs(candidate.distanceReduction - current.distanceReduction) <= scoreEpsilon &&
             candidate.routeLength < current.routeLength - scoreEpsilon))
            selected = index;
        // Equal reduction and route length retain the earlier fan-order candidate.
    }
    return selected;
}

inline std::vector<std::size_t> SelectPartialWaypointCandidates(
    std::vector<Waypoint> const& path, Waypoint const& start, float minimumStep = 2.0f,
    float preferredMaximumStep = 120.0f)
{
    std::vector<std::size_t> candidates;
    if (path.size() < 2 || !IsFinite(start))
        return candidates;

    // PathGenerator returns points in travel order. Keep every admissible point so the runtime can
    // re-probe them from farthest progress toward the bot. A failed far candidate must not discard
    // a nearer candidate that is actually walkable.
    for (std::size_t index = 1; index < path.size(); ++index)
    {
        Waypoint const& point = path[index];
        if (point.mapId != start.mapId || !IsFinite(point))
            continue;

        float const distance = Distance2d(start, point);
        if (!std::isfinite(distance) || distance < minimumStep)
            continue;
        if (distance <= preferredMaximumStep)
            candidates.push_back(index);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [&](std::size_t left,
                                                               std::size_t right)
    {
        float const leftDistance = Distance2d(start, path[left]);
        float const rightDistance = Distance2d(start, path[right]);
        if (leftDistance != rightDistance)
            return leftDistance > rightDistance;
        return left < right;
    });
    return candidates;
}

// Keep the path-relative farthest-to-nearest order, but admit only candidates that make finite,
// same-map target-aware progress. This is intentionally separate from SelectPartialWaypointCandidates
// so existing non-target policy callers retain their source-compatible behavior; quest-giver
// production movement must use this target-aware selector.
inline std::vector<std::size_t> SelectTargetAwarePartialWaypointCandidates(
    std::vector<Waypoint> const& path, Waypoint const& start, Waypoint const& target,
    float minimumStep = 2.0f, float preferredMaximumStep = 120.0f,
    float minimumTargetProgress = kPartialTargetProgressEpsilon)
{
    std::vector<std::size_t> candidates =
        SelectPartialWaypointCandidates(path, start, minimumStep, preferredMaximumStep);
    if (!IsFinite(target) || target.mapId != start.mapId ||
        !std::isfinite(minimumTargetProgress) || minimumTargetProgress < 0.0f)
        return {};

    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](std::size_t index)
    {
        return index >= path.size() ||
            !HasTargetAwarePartialProgress(start, target, path[index], minimumTargetProgress);
    }), candidates.end());
    return candidates;
}

// Re-probe facts are aligned with path indices. This pure selector is the contract used by the
// action: only the farthest candidate whose exact segment proves complete is executable.
inline std::optional<std::size_t> SelectCompletePartialWaypoint(
    std::vector<Waypoint> const& path, Waypoint const& start,
    std::vector<PathFacts> const& reprobeFacts, float minimumStep = 2.0f,
    float preferredMaximumStep = 120.0f, float endpointTolerance = 2.5f)
{
    std::vector<std::size_t> const candidates =
        SelectPartialWaypointCandidates(path, start, minimumStep, preferredMaximumStep);
    for (std::size_t const index : candidates)
    {
        if (index < reprobeFacts.size() && IsCompleteReprobe(reprobeFacts[index], endpointTolerance))
            return index;
    }
    return std::nullopt;
}

// Target-aware counterpart used by quest-giver movement. Exact complete-path validation remains
// the final gate, and candidates are still tried from farthest to nearest among eligible points.
inline std::optional<std::size_t> SelectTargetAwareCompletePartialWaypoint(
    std::vector<Waypoint> const& path, Waypoint const& start, Waypoint const& target,
    std::vector<PathFacts> const& reprobeFacts, float minimumStep = 2.0f,
    float preferredMaximumStep = 120.0f, float endpointTolerance = 2.5f,
    float minimumTargetProgress = kPartialTargetProgressEpsilon)
{
    std::vector<std::size_t> const candidates = SelectTargetAwarePartialWaypointCandidates(
        path, start, target, minimumStep, preferredMaximumStep, minimumTargetProgress);
    for (std::size_t const index : candidates)
    {
        if (index < reprobeFacts.size() && IsCompleteReprobe(reprobeFacts[index], endpointTolerance))
            return index;
    }
    return std::nullopt;
}

inline std::optional<std::size_t> SelectPartialWaypoint(
    std::vector<Waypoint> const& path, Waypoint const& start, float minimumStep = 2.0f,
    float preferredMaximumStep = 120.0f)
{
    std::vector<std::size_t> const candidates =
        SelectPartialWaypointCandidates(path, start, minimumStep, preferredMaximumStep);
    if (candidates.empty())
        return std::nullopt;
    return candidates.front();
}

struct QuantizedCell
{
    std::uint32_t mapId = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

inline bool operator==(QuantizedCell const& left, QuantizedCell const& right)
{
    return left.mapId == right.mapId && left.x == right.x && left.y == right.y &&
        left.z == right.z;
}

inline std::optional<QuantizedCell> QuantizeCell(Waypoint const& point,
                                                 float cellSize = kEscapeCellSize)
{
    if (!IsFinite(point) || !std::isfinite(cellSize) || cellSize <= 0.0f)
        return std::nullopt;

    auto quantize = [cellSize](float value) -> std::optional<std::int32_t>
    {
        double const cell = std::floor(static_cast<double>(value) / cellSize);
        if (!std::isfinite(cell) || cell < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            cell > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            return std::nullopt;
        return static_cast<std::int32_t>(cell);
    };

    std::optional<std::int32_t> const x = quantize(point.x);
    std::optional<std::int32_t> const y = quantize(point.y);
    std::optional<std::int32_t> const z = quantize(point.z);
    if (!x || !y || !z)
        return std::nullopt;
    return QuantizedCell{point.mapId, *x, *y, *z};
}

inline bool IsVisitedCell(QuantizedCell const& cell, std::vector<QuantizedCell> const& visited)
{
    return std::find(visited.begin(), visited.end(), cell) != visited.end();
}

inline std::vector<Waypoint> BuildEscapeRing(Waypoint const& start, Waypoint const& target)
{
    std::vector<Waypoint> ring;
    if (start.mapId != target.mapId || !IsFinite(start) || !IsFinite(target))
        return ring;

    float const dx = target.x - start.x;
    float const dy = target.y - start.y;
    float const bearing = std::atan2(dy, dx);
    if (!std::isfinite(bearing))
        return ring;

    ring.reserve(kEscapeRingRadii.size() * kEscapeRingAngles.size());
    for (float const radius : kEscapeRingRadii)
    {
        for (float const angle : kEscapeRingAngles)
        {
            float const absoluteAngle = bearing + angle;
            Waypoint const candidate{
                start.mapId,
                start.x + std::cos(absoluteAngle) * radius,
                start.y + std::sin(absoluteAngle) * radius,
                start.z};
            if (IsFinite(candidate))
                ring.push_back(candidate);
        }
    }
    return ring;
}

struct EscapeCandidateFacts
{
    Waypoint point{};
    QuantizedCell cell{};
    bool quantized = false;
    bool sameMap = false;
    bool finite = false;
    bool validHeight = false;
    bool freshlyStrictComplete = false;
    bool visited = false;
    float routeLength = 0.0f;
    float distanceReduction = 0.0f;
    std::uint32_t pathType = PATHFIND_BLANK;
    std::size_t ringOrder = 0;
    bool validatedGroundLine = false;
};

enum class EscapeSelectionKind : std::uint8_t
{
    Improving,
    Sideways,
    Regressive,
    Exhausted
};

struct EscapeBudget
{
    // `attempts` remains the current epoch's attempt count for compatibility with the original
    // policy contract. The total fields below are journey-wide and are never reset by a replan.
    std::uint32_t attempts = 0;
    float totalDistance = 0.0f;
    float totalRegression = 0.0f;

    std::uint32_t epochCount = 0;
    std::uint32_t journeyAttempts = 0;
    float epochDistance = 0.0f;
    float epochRegression = 0.0f;
    float epochStartRemainingDistance = std::numeric_limits<float>::infinity();
    float lastRemainingDistance = std::numeric_limits<float>::infinity();
    float bestRemainingDistance = std::numeric_limits<float>::infinity();
    bool exhausted = false;
};

inline bool IsFiniteRemainingDistance(float remainingDistance)
{
    return std::isfinite(remainingDistance) && remainingDistance >= 0.0f;
}

inline void ResetEscapeEpochCounters(EscapeBudget& budget, float remainingDistance)
{
    budget.attempts = 0;
    budget.epochDistance = 0.0f;
    budget.epochRegression = 0.0f;
    budget.epochStartRemainingDistance = remainingDistance;
    budget.lastRemainingDistance = remainingDistance;
    budget.bestRemainingDistance = remainingDistance;
    budget.exhausted = false;
}

// Starts the first bounded local search for a journey. Runtime callers should invoke this exactly
// once when a target session is created; tests may also use it to make epoch semantics explicit.
inline bool BeginInitialEscapeEpoch(EscapeBudget& budget, float remainingDistance)
{
    if (budget.epochCount != 0 || !IsFiniteRemainingDistance(remainingDistance) ||
        budget.journeyAttempts >= kMaxEscapeJourneyAttempts)
        return false;

    budget.epochCount = 1;
    ResetEscapeEpochCounters(budget, remainingDistance);
    return true;
}

inline void ObserveEscapeProgress(EscapeBudget& budget, float remainingDistance)
{
    if (budget.epochCount == 0 || !IsFiniteRemainingDistance(remainingDistance))
        return;
    budget.lastRemainingDistance = remainingDistance;
    if (!IsFiniteRemainingDistance(budget.bestRemainingDistance) ||
        remainingDistance < budget.bestRemainingDistance)
        budget.bestRemainingDistance = remainingDistance;
}

inline bool HasMeaningfulEscapeProgress(EscapeBudget const& budget)
{
    return budget.epochCount != 0 && IsFiniteRemainingDistance(budget.epochStartRemainingDistance) &&
        IsFiniteRemainingDistance(budget.lastRemainingDistance) &&
        budget.epochStartRemainingDistance - budget.lastRemainingDistance >=
            kEscapeMinimumEpochProgress;
}

// Forward declarations keep the replan predicate next to the epoch state while the legacy
// availability/exhaustion predicates remain below the candidate-selection contract.
inline bool IsEscapeBudgetAvailable(EscapeBudget const& budget);
inline bool IsEscapeExhausted(EscapeBudget const& budget);

inline bool CanBeginEscapeReplan(EscapeBudget const& budget, float remainingDistance)
{
    if (!IsFiniteRemainingDistance(remainingDistance) || budget.epochCount == 0 ||
        budget.epochCount >= kMaxEscapeEpochs || budget.journeyAttempts >= kMaxEscapeJourneyAttempts ||
        !std::isfinite(budget.totalDistance) || budget.totalDistance >= kMaxEscapeTotalDistance ||
        !std::isfinite(budget.totalRegression) ||
        budget.totalRegression >= kMaxEscapeTotalRegression)
        return false;

    // Observe the candidate distance without mutating the budget so this predicate is pure from a
    // caller's perspective. A replan is valid only after the previous epoch was exhausted and the
    // journey's current position is meaningfully closer to this same target.
    return IsEscapeExhausted(budget) && IsFiniteRemainingDistance(remainingDistance) &&
        IsFiniteRemainingDistance(budget.epochStartRemainingDistance) &&
        budget.epochStartRemainingDistance - remainingDistance >=
            kEscapeMinimumEpochProgress;
}

inline bool BeginEscapeReplan(EscapeBudget& budget, float remainingDistance)
{
    if (!CanBeginEscapeReplan(budget, remainingDistance))
        return false;

    ++budget.epochCount;
    ResetEscapeEpochCounters(budget, remainingDistance);
    return true;
}

inline void ExhaustEscapeEpoch(EscapeBudget& budget)
{
    budget.exhausted = true;
}

struct EscapeSelection
{
    std::optional<std::size_t> index;
    EscapeSelectionKind kind = EscapeSelectionKind::Exhausted;
};

inline bool IsEscapeBudgetAvailable(EscapeBudget const& budget)
{
    return !budget.exhausted && budget.attempts < kMaxEscapeAttempts &&
        budget.journeyAttempts < kMaxEscapeJourneyAttempts &&
        std::isfinite(budget.totalDistance) && budget.totalDistance < kMaxEscapeTotalDistance &&
        std::isfinite(budget.totalRegression) &&
        budget.totalRegression < kMaxEscapeTotalRegression;
}

inline bool IsUsableEscapeCandidate(EscapeCandidateFacts const& candidate,
                                    EscapeBudget const& budget)
{
    bool const exactNormalNavmesh = candidate.pathType == PATHFIND_NORMAL;
    bool const explicitValidatedGroundLine =
        candidate.validatedGroundLine && candidate.pathType == PATHFIND_BLANK;
    if (!IsEscapeBudgetAvailable(budget) || !candidate.quantized || candidate.visited ||
        !candidate.sameMap || !candidate.finite || !candidate.validHeight ||
        !candidate.freshlyStrictComplete ||
        (!exactNormalNavmesh && !explicitValidatedGroundLine))
        return false;
    if (!std::isfinite(candidate.routeLength) || candidate.routeLength <= kEscapeMinimumRouteLength ||
        candidate.routeLength > kEscapeMaximumRouteLength ||
        !std::isfinite(candidate.distanceReduction))
        return false;

    float const regression = std::max(0.0f, -candidate.distanceReduction);
    return budget.totalDistance + candidate.routeLength <= kMaxEscapeTotalDistance &&
        budget.totalRegression + regression <= kMaxEscapeTotalRegression;
}

inline bool IsBetterEscapeCandidate(EscapeCandidateFacts const& candidate,
                                    EscapeCandidateFacts const& current)
{
    constexpr float scoreEpsilon = 0.001f;
    if (candidate.distanceReduction > current.distanceReduction + scoreEpsilon)
        return true;
    if (std::fabs(candidate.distanceReduction - current.distanceReduction) > scoreEpsilon)
        return false;
    if (candidate.routeLength < current.routeLength - scoreEpsilon)
        return true;
    if (std::fabs(candidate.routeLength - current.routeLength) > scoreEpsilon)
        return false;
    return candidate.ringOrder < current.ringOrder;
}

inline EscapeSelection SelectEscapeCandidate(
    std::vector<EscapeCandidateFacts> const& candidates, EscapeBudget const& budget,
    std::vector<QuantizedCell> const& visited = {})
{
    if (!IsEscapeBudgetAvailable(budget))
        return {};

    std::optional<std::size_t> improving;
    std::optional<std::size_t> fallback;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        EscapeCandidateFacts const& candidate = candidates[index];
        if (candidate.quantized && IsVisitedCell(candidate.cell, visited))
            continue;
        if (!IsUsableEscapeCandidate(candidate, budget))
            continue;

        std::optional<std::size_t>& selected =
            candidate.distanceReduction >= kEscapeMinimumImprovement ? improving : fallback;
        if (!selected || IsBetterEscapeCandidate(candidate, candidates[*selected]))
            selected = index;
    }

    if (improving)
        return {improving, EscapeSelectionKind::Improving};
    if (fallback)
    {
        return {fallback, candidates[*fallback].distanceReduction < 0.0f
                              ? EscapeSelectionKind::Regressive
                              : EscapeSelectionKind::Sideways};
    }
    return {};
}

inline bool ConsumeEscapeCandidate(EscapeBudget& budget, EscapeCandidateFacts const& candidate)
{
    if (!IsUsableEscapeCandidate(candidate, budget))
        return false;

    float const regression = std::max(0.0f, -candidate.distanceReduction);
    ++budget.attempts;
    ++budget.journeyAttempts;
    budget.totalDistance += candidate.routeLength;
    budget.totalRegression += regression;
    budget.epochDistance += candidate.routeLength;
    budget.epochRegression += regression;
    return true;
}

inline bool IsEscapeExhausted(EscapeBudget const& budget)
{
    return budget.exhausted || !IsEscapeBudgetAvailable(budget);
}
}

#endif
