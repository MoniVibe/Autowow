/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATORCONVOYPOLICY_H
#define PLAYERBOTS_DUNGEONNAVIGATORCONVOYPOLICY_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace DungeonNavigatorConvoy
{
constexpr std::size_t NoSelection = std::numeric_limits<std::size_t>::max();

inline bool IsSettledAtAssignedPoint(bool sameMap, float distance, float arrivalRadius)
{
    return sameMap && std::isfinite(distance) && std::isfinite(arrivalRadius) &&
        arrivalRadius >= 0.0f && distance <= arrivalRadius;
}

inline bool CanHoldWithinCohesion(bool sameMap, float leaderDistance,
                                  float convoyAdvanceDistance, float cohesionRadius)
{
    return sameMap && std::isfinite(leaderDistance) &&
        std::isfinite(convoyAdvanceDistance) && std::isfinite(cohesionRadius) &&
        convoyAdvanceDistance >= 0.0f && cohesionRadius >= convoyAdvanceDistance &&
        leaderDistance > convoyAdvanceDistance && leaderDistance <= cohesionRadius;
}

// A clientless follower can occasionally be advanced below WMO collision while its XY remains on
// the established route. Recovery is deliberately stricter than ordinary cohesion: the caller
// must prove the intended local floor and a complete continuation path before correcting only Z.
inline bool CanGroundReattach(bool hasRoute, bool sameContext, bool alive, bool inCombat,
                              bool beingTeleported, bool onTransport, bool floorValid,
                              float upwardCorrection, bool continuationReached,
                              float minimumCorrection, float maximumCorrection)
{
    return hasRoute && sameContext && alive && !inCombat && !beingTeleported && !onTransport &&
        floorValid && continuationReached && std::isfinite(upwardCorrection) &&
        std::isfinite(minimumCorrection) && std::isfinite(maximumCorrection) &&
        minimumCorrection > 0.0f && maximumCorrection >= minimumCorrection &&
        upwardCorrection >= minimumCorrection && upwardCorrection <= maximumCorrection;
}

// When the current XY itself is inside a collision hole, same-XY floor recovery cannot be proven.
// A follower may instead be restored to its own already-established trailing route slot, but never
// to or beyond the leader frontier. The correction remains local and requires a complete ordinary
// continuation path from the restored slot to the next route point.
inline bool CanRouteSlotReattach(bool hasRoute, bool sameContext, bool alive, bool inCombat,
                                 bool beingTeleported, bool onTransport, bool targetFloorValid,
                                 bool targetSameMap, std::size_t targetRouteIndex,
                                 std::size_t leaderFrontier, float horizontalDistance,
                                 float upwardCorrection, bool continuationReached,
                                 float maximumHorizontalDistance, float minimumCorrection,
                                 float maximumCorrection)
{
    return hasRoute && sameContext && alive && !inCombat && !beingTeleported && !onTransport &&
        targetFloorValid && targetSameMap && targetRouteIndex < leaderFrontier &&
        continuationReached && std::isfinite(horizontalDistance) &&
        std::isfinite(upwardCorrection) && std::isfinite(maximumHorizontalDistance) &&
        std::isfinite(minimumCorrection) && std::isfinite(maximumCorrection) &&
        maximumHorizontalDistance > 0.0f && minimumCorrection > 0.0f &&
        maximumCorrection >= minimumCorrection &&
        horizontalDistance <= maximumHorizontalDistance &&
        upwardCorrection >= minimumCorrection && upwardCorrection <= maximumCorrection;
}

// Once the active encounter is complete, its cached route is no longer a valid convoy frontier.
// A stationary follower may first rejoin the stationary leader through a newly proven ordinary
// path; the caller must still execute that prepared path through the normal movement action.
inline bool CanPostCombatRejoin(bool completedCachedRoute, bool sameContext, bool alive,
                                bool inCombat, bool beingTeleported, bool onTransport,
                                bool leaderPathReached, float leaderPathLength,
                                float leaderDistance, float minimumRejoinDistance,
                                float maximumRejoinPathLength)
{
    return completedCachedRoute && sameContext && alive && !inCombat && !beingTeleported &&
        !onTransport && leaderPathReached && std::isfinite(leaderPathLength) &&
        leaderPathLength > 0.0f && std::isfinite(maximumRejoinPathLength) &&
        maximumRejoinPathLength > 0.0f && leaderPathLength <= maximumRejoinPathLength &&
        std::isfinite(leaderDistance) && std::isfinite(minimumRejoinDistance) &&
        minimumRejoinDistance >= 0.0f && leaderDistance > minimumRejoinDistance;
}

struct Candidate
{
    std::size_t routeIndex = 0;
    bool sameMap = false;
    bool safe = false;
    bool destinationReached = false;
    float pathLength = 0.0f;
    float physicalProgress = 0.0f;
};

inline bool IsOrdinarilyReachable(Candidate const& candidate, float minimumProgress)
{
    return candidate.sameMap && candidate.safe && candidate.destinationReached &&
        std::isfinite(candidate.pathLength) && candidate.pathLength > 0.0f &&
        std::isfinite(candidate.physicalProgress) &&
        candidate.physicalProgress >= minimumProgress;
}

// A shared regroup anchor is admitted only when the stranded follower and the current leader can
// both reach the same already-established route point through bounded ordinary walking. Keeping
// this evidence separate from Candidate preserves the normal follower-slot policy while making a
// leader retreat impossible to authorize from follower-only or special-transition evidence.
struct SharedCandidate
{
    std::size_t routeIndex = 0;
    bool sameMap = false;
    bool ordinaryWalk = false;
    bool followerSafe = false;
    bool followerDestinationReached = false;
    float followerPathLength = 0.0f;
    float followerPhysicalProgress = 0.0f;
    bool leaderSafe = false;
    bool leaderDestinationReached = false;
    float leaderPathLength = 0.0f;
    float leaderPhysicalProgress = 0.0f;
};

struct SharedRegroupBounds
{
    std::size_t maximumCandidates = 0;
    float minimumFollowerProgress = 0.0f;
    float minimumLeaderRetreat = 0.0f;
    float maximumPathLength = 0.0f;
};

inline bool HasValidSharedRegroupBounds(SharedRegroupBounds const& bounds)
{
    return bounds.maximumCandidates &&
        std::isfinite(bounds.minimumFollowerProgress) &&
        std::isfinite(bounds.minimumLeaderRetreat) &&
        std::isfinite(bounds.maximumPathLength) &&
        bounds.minimumFollowerProgress > 0.0f && bounds.minimumLeaderRetreat > 0.0f &&
        bounds.maximumPathLength >= bounds.minimumFollowerProgress &&
        bounds.maximumPathLength >= bounds.minimumLeaderRetreat;
}

inline bool IsSharedOrdinarilyReachable(SharedCandidate const& candidate,
                                        SharedRegroupBounds const& bounds)
{
    return HasValidSharedRegroupBounds(bounds) && candidate.sameMap && candidate.ordinaryWalk &&
        candidate.followerSafe && candidate.followerDestinationReached && candidate.leaderSafe &&
        candidate.leaderDestinationReached && std::isfinite(candidate.followerPathLength) &&
        candidate.followerPathLength > 0.0f &&
        candidate.followerPathLength <= bounds.maximumPathLength &&
        std::isfinite(candidate.leaderPathLength) && candidate.leaderPathLength > 0.0f &&
        candidate.leaderPathLength <= bounds.maximumPathLength &&
        std::isfinite(candidate.followerPhysicalProgress) &&
        candidate.followerPhysicalProgress >= bounds.minimumFollowerProgress &&
        std::isfinite(candidate.leaderPhysicalProgress) &&
        candidate.leaderPhysicalProgress >= bounds.minimumLeaderRetreat;
}

// Select the furthest route-ordered shared anchor without ever moving the leader forward or
// allowing a follower to cross its normal slot. The caller bounds the number of route points it
// probes; the remembered floor prevents a failed movement attempt from oscillating farther back.
inline std::size_t SelectSharedBackwardReanchor(
    std::vector<SharedCandidate> const& candidates,
    std::size_t leaderFrontier,
    std::size_t normalSlotIndex,
    SharedRegroupBounds const& bounds,
    std::size_t rememberedRouteFloor = NoSelection)
{
    if (!HasValidSharedRegroupBounds(bounds) || !leaderFrontier ||
        normalSlotIndex > leaderFrontier)
    {
        return NoSelection;
    }

    std::size_t const end = std::min(candidates.size(), bounds.maximumCandidates);
    std::size_t selected = NoSelection;
    std::size_t selectedRouteIndex = 0;
    for (std::size_t index = 0; index < end; ++index)
    {
        SharedCandidate const& candidate = candidates[index];
        if (candidate.routeIndex >= leaderFrontier || candidate.routeIndex > normalSlotIndex ||
            (rememberedRouteFloor != NoSelection &&
                candidate.routeIndex < rememberedRouteFloor) ||
            !IsSharedOrdinarilyReachable(candidate, bounds))
        {
            continue;
        }

        if (selected == NoSelection || candidate.routeIndex > selectedRouteIndex)
        {
            selected = index;
            selectedRouteIndex = candidate.routeIndex;
        }
    }
    return selected;
}

// Every follower owns a distinct route-ordered ceiling behind the leader. Selection remains on
// exact stored points and accepts only independently proven, bounded physical movement.
inline std::size_t SelectTarget(std::vector<Candidate> const& candidates,
                                std::size_t leaderFrontier,
                                std::size_t followerOrdinal,
                                float minimumProgress,
                                float maximumPathLength)
{
    if (!followerOrdinal ||
        !std::isfinite(minimumProgress) || minimumProgress < 0.0f ||
        !std::isfinite(maximumPathLength) || maximumPathLength <= 0.0f)
    {
        return NoSelection;
    }

    // A large raid can have more follower slots than the leader has traversed
    // points during startup. Those rear slots remain at the route origin rather
    // than underflowing or blocking the whole convoy.
    std::size_t const maximumRouteIndex = leaderFrontier > followerOrdinal ?
        leaderFrontier - followerOrdinal : 0;
    std::size_t selected = NoSelection;
    std::size_t selectedRouteIndex = 0;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        if (candidate.routeIndex > maximumRouteIndex ||
            !IsOrdinarilyReachable(candidate, minimumProgress) ||
            candidate.pathLength > maximumPathLength)
        {
            continue;
        }

        if (selected == NoSelection || candidate.routeIndex > selectedRouteIndex)
        {
            selected = index;
            selectedRouteIndex = candidate.routeIndex;
        }
    }
    return selected;
}

// Recovery is deliberately less local than normal slot advancement: a complete safe path may
// traverse a staircase whose walking length exceeds the normal convoy lookahead. The caller still
// bounds how many already-established route points it probes. Selection itself is route-monotonic,
// never permits a point beyond the follower's normal slot (and therefore never beyond the leader),
// and honors a remembered floor so repeated recovery cannot oscillate to an earlier route point.
inline std::size_t SelectBackwardReanchor(std::vector<Candidate> const& candidates,
                                          std::size_t leaderFrontier,
                                          std::size_t normalSlotIndex,
                                          float minimumProgress,
                                          std::size_t rememberedRouteFloor = NoSelection)
{
    if (normalSlotIndex > leaderFrontier ||
        !std::isfinite(minimumProgress) || minimumProgress < 0.0f)
    {
        return NoSelection;
    }

    std::size_t selected = NoSelection;
    std::size_t selectedRouteIndex = 0;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        if (candidate.routeIndex > normalSlotIndex || candidate.routeIndex > leaderFrontier ||
            (rememberedRouteFloor != NoSelection &&
                candidate.routeIndex < rememberedRouteFloor) ||
            !IsOrdinarilyReachable(candidate, minimumProgress))
        {
            continue;
        }

        if (selected == NoSelection || candidate.routeIndex > selectedRouteIndex)
        {
            selected = index;
            selectedRouteIndex = candidate.routeIndex;
        }
    }
    return selected;
}
}

#endif
