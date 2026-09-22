#ifndef PLAYERBOTS_AUTONOMOUS_RPG_TRAVEL_POLICY_H
#define PLAYERBOTS_AUTONOMOUS_RPG_TRAVEL_POLICY_H

#include <cmath>
#include <cstdint>

namespace AutonomousRpgTravelPolicy
{
constexpr float kMeaningfulProgressYards = 5.0f;
constexpr uint32_t kStuckTimeMs = 90 * 1000;
constexpr uint32_t kObservationGapMs = 30 * 1000;
constexpr uint32_t kFailedDestinationCooldownMs = 5 * 60 * 1000;

// Observe on every autonomous travel tick, including while a long spline is active. The nearest
// distance is a high-water mark for progress; oscillation within five yards cannot reset time.
inline bool ObserveProgress(float distance, float& nearestDistance, uint32_t& progressAt,
                            uint32_t& attempts, uint32_t& lastObservedAt, uint32_t now)
{
    if (lastObservedAt != 0 && static_cast<uint32_t>(now - lastObservedAt) > kObservationGapMs)
    {
        // Combat, loading, or another movement owner paused this RPG action. Start a fresh
        // observation window rather than charging the pause against the destination.
        nearestDistance = distance;
        progressAt = now;
        attempts = 0;
        lastObservedAt = now;
        return false;
    }
    lastObservedAt = now;
    if (distance + kMeaningfulProgressYards < nearestDistance)
    {
        nearestDistance = distance;
        progressAt = now;
        attempts = 0;
        return false;
    }
    ++attempts;
    if (attempts < 5 || static_cast<uint32_t>(now - progressAt) < kStuckTimeMs)
        return false;
    progressAt = now;
    attempts = 0;
    return true;
}

inline bool IsCoolingDown(uint32_t failedAt, uint32_t now)
{
    return static_cast<uint32_t>(now - failedAt) < kFailedDestinationCooldownMs;
}

inline bool IsCompleteLocalRoute(bool normal, bool incomplete, bool noPath, bool farFromPoly,
                                 bool shortcut, bool notUsingPath, float endpointDistance)
{
    return normal && !incomplete && !noPath && !farFromPoly && !shortcut && !notUsingPath &&
        endpointDistance <= 10.0f;
}

struct LocalPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct PartialSegmentFacts
{
    bool safeNavmesh = false;
    bool normalIncompleteOnly = false;
    bool sameZone = false;
    bool coolingDown = false;
    float pathLength = 0.0f;
    float distanceToCoarse = 0.0f;
    float endpointToCoarse = 0.0f;
    float distanceToEndpoint = 0.0f;
};

struct PartialSegmentDecision
{
    bool accepted = false;
    LocalPoint destination{};
    float progress = 0.0f;
};

// An incomplete path can be useful as one grounded local leg, but its far coarse target
// must never become the committed GO_GRIND/GO_CAMP destination.
inline PartialSegmentDecision SelectPartialSegmentEndpoint(PartialSegmentFacts const& facts,
                                                           LocalPoint endpoint)
{
    if (!facts.safeNavmesh || !facts.normalIncompleteOnly || !facts.sameZone ||
        facts.coolingDown || !std::isfinite(facts.pathLength) ||
        !std::isfinite(facts.distanceToCoarse) || !std::isfinite(facts.endpointToCoarse) ||
        !std::isfinite(facts.distanceToEndpoint) || !std::isfinite(endpoint.x) ||
        !std::isfinite(endpoint.y) || !std::isfinite(endpoint.z) ||
        facts.pathLength < 15.0f || facts.pathLength > 250.0f ||
        facts.distanceToEndpoint < 15.0f || facts.distanceToEndpoint > 250.0f)
        return {};

    float const progress = facts.distanceToCoarse - facts.endpointToCoarse;
    if (progress < kMeaningfulProgressYards)
        return {};
    return {true, endpoint, progress};
}
}

#endif
