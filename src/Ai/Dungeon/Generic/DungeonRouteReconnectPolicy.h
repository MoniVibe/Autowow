/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef _PLAYERBOT_DUNGEON_ROUTE_RECONNECT_POLICY_H
#define _PLAYERBOT_DUNGEON_ROUTE_RECONNECT_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace DungeonRouteReconnect
{
constexpr std::size_t NoSelection = std::numeric_limits<std::size_t>::max();

// Shared regroup recovery widens its established-route probe prefix in a few deterministic
// stages. The navigator supplies the existing hard cap; this policy only chooses the current
// bounded prefix and never authorizes a route transition or a position change.
struct SharedAnchorSearchBounds
{
    std::size_t smallPrefix = 0;
    std::size_t mediumPrefix = 0;
    std::size_t hardPrefix = 0;
};

inline bool HasValidSharedAnchorSearchBounds(SharedAnchorSearchBounds const& bounds)
{
    return bounds.smallPrefix > 0 && bounds.smallPrefix <= bounds.mediumPrefix &&
        bounds.mediumPrefix <= bounds.hardPrefix;
}

inline std::size_t SelectSharedAnchorSearchPrefix(
    std::uint8_t oneBasedAttempt,
    std::size_t routePointCount,
    SharedAnchorSearchBounds const& bounds)
{
    if (!oneBasedAttempt || !routePointCount || !HasValidSharedAnchorSearchBounds(bounds))
        return 0;

    std::size_t requested = oneBasedAttempt == 1 ? bounds.smallPrefix :
        (oneBasedAttempt == 2 ? bounds.mediumPrefix : bounds.hardPrefix);
    return std::min(routePointCount, requested);
}

struct SharedRegroupContext
{
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t encounterId = 0;
    std::uint32_t spawnId = 0;
    std::uint32_t memberGuid = 0;
};

inline bool SameSharedRegroupContext(SharedRegroupContext const& left,
                                     SharedRegroupContext const& right)
{
    return left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.encounterId == right.encounterId && left.spawnId == right.spawnId &&
        left.memberGuid == right.memberGuid;
}

enum class SharedRegroupTerminalDecision
{
    Continue,
    EnterTerminal,
    NoOp,
};

struct SharedRegroupTerminalState
{
    bool hasContext = false;
    SharedRegroupContext context;
    bool terminal = false;
};

inline bool IsSharedRegroupTerminal(SharedRegroupTerminalState const& state,
                                    SharedRegroupContext const& context)
{
    return state.hasContext && state.terminal && SameSharedRegroupContext(state.context,
                                                                            context);
}

// A no-candidate result is retryable until the caller has consumed the final bounded prefix.
// Once that prefix is exhausted, latch exactly one terminal for this route/member context.
// Changing the route context creates a new epoch; a physical rejoin uses ResetSharedRegroupEpoch.
inline SharedRegroupTerminalDecision ObserveSharedRegroupNoCandidate(
    SharedRegroupTerminalState& state,
    SharedRegroupContext const& context,
    bool fullyExhausted)
{
    if (!state.hasContext || !SameSharedRegroupContext(state.context, context))
    {
        state = {};
        state.hasContext = true;
        state.context = context;
    }

    if (state.terminal)
        return SharedRegroupTerminalDecision::NoOp;
    if (!fullyExhausted)
        return SharedRegroupTerminalDecision::Continue;

    state.terminal = true;
    return SharedRegroupTerminalDecision::EnterTerminal;
}

inline void ResetSharedRegroupEpoch(SharedRegroupTerminalState& state)
{
    state = {};
}

struct Candidate
{
    bool safe = false;
    bool destinationReached = false;
    float pathLength = 0.0f;
};

struct StoredWalkAttachmentCandidate
{
    bool sameMapWalk = false;
    bool safe = false;
    bool destinationReached = false;
};

struct ReplanWalkCandidateFacts
{
    std::size_t candidateRank = 0;
    std::size_t candidateLimit = 0;
    bool withinDistance = false;
    bool sameMap = false;
    bool completeWalkGraph = false;
    bool attachmentSafe = false;
    bool attachmentReached = false;
    bool noSpecialPoint = false;
};

struct WalkRevalidationKey
{
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t encounterId = 0;
    std::uint32_t goalId = 0;
    std::uint8_t noProgressEpoch = 0;
};

inline bool SameWalkRevalidationKey(WalkRevalidationKey const& left,
                                    WalkRevalidationKey const& right)
{
    return left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.encounterId == right.encounterId && left.goalId == right.goalId &&
        left.noProgressEpoch == right.noProgressEpoch;
}

enum class WalkRevalidationPhase
{
    None,
    Pending,
    Accepted,
    Terminal,
};

struct WalkRevalidationState
{
    bool hasKey = false;
    WalkRevalidationKey key;
    WalkRevalidationPhase phase = WalkRevalidationPhase::None;
};

enum class WalkRevalidationDecision
{
    EnterPending,
    EnterTerminal,
    NoOp,
};

struct FreshOrdinaryWalkEvidence
{
    bool fresh = false;
    bool routePresent = false;
    bool sameMap = false;
    bool completeOrdinaryWalk = false;
};

inline bool IsWalkRevalidationPending(WalkRevalidationState const& state,
                                      WalkRevalidationKey const& key)
{
    return state.hasKey && SameWalkRevalidationKey(state.key, key) &&
        state.phase == WalkRevalidationPhase::Pending;
}

inline bool IsWalkRevalidationTerminal(WalkRevalidationState const& state,
                                       WalkRevalidationKey const& key)
{
    return state.hasKey && SameWalkRevalidationKey(state.key, key) &&
        state.phase == WalkRevalidationPhase::Terminal;
}

// The first no-proof observation owns one delayed revalidation for its exact goal, world, and
// no-progress epoch. The second no-proof observation consumes that allowance and becomes terminal.
// Further identical observations are no-ops; changing any key field starts one new bounded cycle.
inline WalkRevalidationDecision ObserveWalkRevalidationNoProof(
    WalkRevalidationState& state, WalkRevalidationKey const& key)
{
    if (!state.hasKey || !SameWalkRevalidationKey(state.key, key) ||
        state.phase == WalkRevalidationPhase::None)
    {
        state.hasKey = true;
        state.key = key;
        state.phase = WalkRevalidationPhase::Pending;
        return WalkRevalidationDecision::EnterPending;
    }

    if (state.phase == WalkRevalidationPhase::Pending)
    {
        state.phase = WalkRevalidationPhase::Terminal;
        return WalkRevalidationDecision::EnterTerminal;
    }

    return WalkRevalidationDecision::NoOp;
}

// A pending cycle can be completed only by evidence produced by its later scan, and only when the
// result is a complete ordinary same-map route. Invalid, stale, special, cross-map, or missing
// evidence leaves the cycle pending so the caller can consume it as terminal no-proof.
inline bool AcceptFreshOrdinaryWalkEvidence(WalkRevalidationState& state,
                                            WalkRevalidationKey const& key,
                                            FreshOrdinaryWalkEvidence const& evidence)
{
    if (!IsWalkRevalidationPending(state, key) || !evidence.fresh ||
        !evidence.routePresent || !evidence.sameMap || !evidence.completeOrdinaryWalk)
    {
        return false;
    }

    state.phase = WalkRevalidationPhase::Accepted;
    return true;
}

// Replan recovery admits only a bounded candidate with a complete ordinary walking proof. This
// pure gate deliberately has no transition or executor semantics; the caller must cache only the
// proven walk points after admission.
inline bool AdmitReplanWalkCandidate(ReplanWalkCandidateFacts const& facts)
{
    return facts.candidateLimit && facts.candidateRank < facts.candidateLimit &&
        facts.withinDistance && facts.sameMap && facts.completeWalkGraph && facts.attachmentSafe &&
        facts.attachmentReached && facts.noSpecialPoint;
}

// A persisted complete walking route may begin with a stale or awkward attachment point even
// though a nearby later point is independently reachable. Select only the earliest physically
// proven same-map walking point from the caller's bounded prefix. Transition-bearing and
// non-persisted routes remain fail-closed: this policy never authorizes skipping a portal, jump,
// elevator, or other transition.
inline std::size_t SelectEarliestStoredWalkAttachment(
    std::vector<StoredWalkAttachmentCandidate> const& candidates,
    bool completeStoredWalk,
    bool skippedTransition,
    std::size_t maximumCandidates)
{
    if (!completeStoredWalk || skippedTransition || maximumCandidates == 0)
        return NoSelection;

    std::size_t const end = candidates.size() < maximumCandidates ?
        candidates.size() : maximumCandidates;
    for (std::size_t index = 0; index < end; ++index)
    {
        StoredWalkAttachmentCandidate const& candidate = candidates[index];
        if (candidate.sameMapWalk && candidate.safe && candidate.destinationReached)
            return index;
    }
    return NoSelection;
}

// A path may be safe as bounded progress without reaching its requested destination. Only a
// validated ground line or a complete normal navmesh path is direct reachability evidence.
inline bool DestinationReached(bool safe, bool groundLine, bool normalPath, bool incompletePath)
{
    return safe && (groundLine || (normalPath && !incompletePath));
}

// Select the furthest route-ordered point that is independently reachable from the bot within
// the bounded movement window. A safe partial path is progress evidence only: it cannot advance the
// stored cursor because it did not reach the requested route point.
inline std::size_t SelectFarthestReachable(std::vector<Candidate> const& candidates,
                                           float maximumPathLength)
{
    std::size_t selected = NoSelection;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        if (candidate.safe && candidate.destinationReached && candidate.pathLength > 0.0f &&
            candidate.pathLength <= maximumPathLength)
        {
            selected = index;
        }
    }
    return selected;
}

struct StoredPoint
{
    bool sameMap = false;
    float distance = 0.0f;
};

// Recover a legacy/incorrectly overshot cursor only from physical evidence. The highest preceding
// same-map stored point inside the arrival radius wins, and the scan never exceeds the lookahead
// window. The caller must still rerun ordinary forward probes from the returned anchor.
inline std::size_t SelectBackwardAnchor(std::vector<StoredPoint> const& points,
                                        std::size_t cursor,
                                        std::size_t maximumLookback,
                                        float arrivalRadius)
{
    if (points.empty() || cursor == 0 || maximumLookback == 0)
        return NoSelection;

    std::size_t const end = cursor < points.size() ? cursor : points.size();
    std::size_t const begin = end > maximumLookback ? end - maximumLookback : 0;
    for (std::size_t index = end; index > begin;)
    {
        --index;
        StoredPoint const& point = points[index];
        if (point.sameMap && point.distance <= arrivalRadius)
            return index;
    }
    return NoSelection;
}

// Recover only a small vertical detachment from otherwise valid dungeon ground. The caller must
// keep XY unchanged and independently prove that the ordinary continuation is reachable from the
// corrected height. This is a collision reattachment, never a route or encounter skip.
inline bool CanGroundReattach(bool groundValid,
                              float upwardCorrection,
                              bool continuationReached,
                              float minimumCorrection,
                              float maximumCorrection)
{
    return groundValid && std::isfinite(upwardCorrection) && continuationReached &&
        upwardCorrection >= minimumCorrection && upwardCorrection <= maximumCorrection;
}

// A validated incomplete probe may be consumed as bounded physical progress, but it must never
// count as arrival at the requested stored point. The caller retains the cursor and moves only to
// the probe's actual endpoint.
inline bool CanUsePartialProgress(bool safe,
                                  bool destinationReached,
                                  float pathLength,
                                  float endpointMovement,
                                  float minimumMovement,
                                  float maximumPathLength)
{
    return safe && !destinationReached && std::isfinite(pathLength) &&
        std::isfinite(endpointMovement) && pathLength > 0.0f &&
        pathLength <= maximumPathLength && endpointMovement >= minimumMovement;
}

// A complete safe path can still exceed the cautious movement window. Select the furthest
// already-validated path vertex inside that window so the caller can consume the path in bounded
// strides without treating the stored destination as reached. Cumulative distances must describe
// one finite, non-decreasing path beginning at zero; malformed or too-sparse paths fail closed.
inline std::size_t SelectPreparedPathPrefix(
    std::vector<float> const& cumulativeDistances,
    float minimumMovement,
    float maximumMovement)
{
    if (cumulativeDistances.size() < 2 || !std::isfinite(minimumMovement) ||
        !std::isfinite(maximumMovement) || minimumMovement <= 0.0f ||
        maximumMovement < minimumMovement || !std::isfinite(cumulativeDistances.front()) ||
        cumulativeDistances.front() != 0.0f)
    {
        return NoSelection;
    }

    std::size_t selected = NoSelection;
    float previous = cumulativeDistances.front();
    for (std::size_t index = 1; index < cumulativeDistances.size(); ++index)
    {
        float const distance = cumulativeDistances[index];
        if (!std::isfinite(distance) || distance < previous)
            return NoSelection;
        previous = distance;

        if (distance >= minimumMovement && distance <= maximumMovement)
            selected = index;
    }
    return selected;
}

// A fully safety-validated probe may be cached as an ordinary local walking corridor. This is
// intentionally independent of travel-node transition metadata: the caller stores only the
// probe's actual grounded points and must still consume them through normal bounded movement.
inline bool CanCachePreparedCorridor(bool safe,
                                     std::size_t pointCount,
                                     float pathLength,
                                     float endpointMovement,
                                     float minimumMovement)
{
    return safe && pointCount >= 2 && std::isfinite(pathLength) &&
        std::isfinite(endpointMovement) && std::isfinite(minimumMovement) &&
        pathLength > 0.0f && minimumMovement > 0.0f &&
        endpointMovement >= minimumMovement;
}

struct PartyAnchorSample
{
    bool eligible = false;
    float horizontalDistance = 0.0f;
    float z = 0.0f;
};

struct PartyRouteReanchorFacts
{
    bool anchorReachable = false;
    float sourceZ = 0.0f;
    float anchorZ = 0.0f;
    std::vector<PartyAnchorSample> members;
};

struct PartyRouteReanchorPolicy
{
    float minimumDrop = 3.0f;
    float maximumDrop = 24.0f;
    float maximumHorizontalDistance = 12.0f;
    float maximumGroundDelta = 1.5f;
    std::size_t minimumSupportingMembers = 2;
};

struct PartyRouteReanchorDecision
{
    bool allowed = false;
    std::size_t eligibleMembers = 0;
    std::size_t supportingMembers = 0;
};

// Re-route a vertically displaced leader only to a lower-floor anchor occupied by a strict
// majority of the eligible party. The caller must independently prove a complete safe walking
// path to that anchor. This policy authorizes ordinary route caching only: never relocation,
// transition execution, or route skipping.
inline PartyRouteReanchorDecision EvaluatePartyRouteReanchor(
    PartyRouteReanchorFacts const& facts,
    PartyRouteReanchorPolicy const& policy = {})
{
    PartyRouteReanchorDecision decision;
    if (!facts.anchorReachable || !std::isfinite(facts.sourceZ) ||
        !std::isfinite(facts.anchorZ) ||
        !std::isfinite(policy.minimumDrop) || !std::isfinite(policy.maximumDrop) ||
        !std::isfinite(policy.maximumHorizontalDistance) ||
        !std::isfinite(policy.maximumGroundDelta) || policy.minimumDrop <= 0.0f ||
        policy.maximumDrop < policy.minimumDrop || policy.maximumHorizontalDistance <= 0.0f ||
        policy.maximumGroundDelta < 0.0f || !policy.minimumSupportingMembers)
    {
        return decision;
    }

    float const drop = facts.sourceZ - facts.anchorZ;
    if (drop < policy.minimumDrop || drop > policy.maximumDrop)
        return decision;

    for (PartyAnchorSample const& member : facts.members)
    {
        if (!member.eligible)
            continue;
        if (!std::isfinite(member.horizontalDistance) || !std::isfinite(member.z))
            return {};

        ++decision.eligibleMembers;
        if (member.horizontalDistance <= policy.maximumHorizontalDistance &&
            std::fabs(member.z - facts.anchorZ) <= policy.maximumGroundDelta)
        {
            ++decision.supportingMembers;
        }
    }

    decision.allowed = decision.supportingMembers >= policy.minimumSupportingMembers &&
        decision.supportingMembers * 2 > decision.eligibleMembers;
    return decision;
}

enum class ExhaustedRouteDecision
{
    Continue,
    ReleaseForTerminalActivation,
    Block,
};

enum class NoProgressDecision
{
    Retry,
    Replan,
    Block,
};

// A cached route is allowed one or more explicitly bounded replans after repeated physical
// no-progress. This recovers from ordinary combat displacement without turning a bad route into
// an infinite rebuild loop. Invalid zero limits fail closed.
inline NoProgressDecision EvaluateNoProgress(uint8_t retries,
                                             uint8_t retryLimit,
                                             uint8_t replans,
                                             uint8_t replanLimit)
{
    if (!retryLimit || retries < retryLimit)
        return retryLimit ? NoProgressDecision::Retry : NoProgressDecision::Block;
    if (replanLimit && replans < replanLimit)
        return NoProgressDecision::Replan;
    return NoProgressDecision::Block;
}

// A consumed cursor releases the cached route only when the bot is physically at the final stored
// point on the same map. The next navigator scan can then evaluate the exact encounter spawn without
// preserveCachedRoute forcing it back into an endless exhausted-route branch.
inline ExhaustedRouteDecision EvaluateExhaustedRoute(std::size_t cursor,
                                                      std::size_t pointCount,
                                                      bool finalPointSameMap,
                                                      float finalPointDistance,
                                                      float arrivalRadius)
{
    if (cursor < pointCount)
        return ExhaustedRouteDecision::Continue;
    if (pointCount && finalPointSameMap && finalPointDistance <= arrivalRadius)
        return ExhaustedRouteDecision::ReleaseForTerminalActivation;
    return ExhaustedRouteDecision::Block;
}
}

#endif
