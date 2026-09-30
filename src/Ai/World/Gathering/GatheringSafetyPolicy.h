/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGSAFETYPOLICY_H
#define PLAYERBOTS_GATHERINGSAFETYPOLICY_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace AutoWowGather
{
struct PositionSample
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct CandidateLeasePolicy
{
    std::uint64_t noProgressWindowSeconds = 30;
    std::uint64_t maxLeaseSeconds = 300;
    float minDistanceReduction = 2.0f;
    float minPositionChange = 2.0f;
};

inline constexpr CandidateLeasePolicy DefaultCandidateLeasePolicy{};

struct CandidateLease
{
    bool active = false;
    std::uint64_t candidateId = 0;
    std::uint64_t acquiredAtSeconds = 0;
    std::uint64_t lastProgressAtSeconds = 0;
    PositionSample lastProgressPosition;
    float bestDistance = std::numeric_limits<float>::infinity();
};

struct CandidateLeaseSample
{
    std::uint64_t candidateId = 0;
    std::uint64_t nowSeconds = 0;
    PositionSample position;
    float distanceToCandidate = std::numeric_limits<float>::infinity();
};

enum class CandidateLeaseEvent : std::uint8_t
{
    None = 0,
    Acquired,
    Pending,
    Progress,
    ExpiredNoProgress,
    ExpiredMaxLease
};

struct CandidateLeaseTransition
{
    CandidateLease lease;
    CandidateLeaseEvent event = CandidateLeaseEvent::None;
};

inline std::uint64_t ElapsedSeconds(std::uint64_t nowSeconds, std::uint64_t sinceSeconds)
{
    return nowSeconds >= sinceSeconds ? nowSeconds - sinceSeconds : 0;
}

inline float PositionDistanceSquared(PositionSample const& left, PositionSample const& right)
{
    float const dx = left.x - right.x;
    float const dy = left.y - right.y;
    float const dz = left.z - right.z;
    return dx * dx + dy * dy + dz * dz;
}

inline CandidateLeaseTransition ObserveCandidateLease(CandidateLease const& current,
                                                       CandidateLeaseSample const& sample,
                                                       CandidateLeasePolicy const& policy)
{
    CandidateLeaseTransition transition;
    if (!sample.candidateId)
        return transition;

    if (!current.active || current.candidateId != sample.candidateId)
    {
        transition.lease.active = true;
        transition.lease.candidateId = sample.candidateId;
        transition.lease.acquiredAtSeconds = sample.nowSeconds;
        transition.lease.lastProgressAtSeconds = sample.nowSeconds;
        transition.lease.lastProgressPosition = sample.position;
        transition.lease.bestDistance = sample.distanceToCandidate;
        transition.event = CandidateLeaseEvent::Acquired;
        return transition;
    }

    transition.lease = current;

    std::uint64_t const maxLeaseSeconds = policy.maxLeaseSeconds ? policy.maxLeaseSeconds : 1;
    if (ElapsedSeconds(sample.nowSeconds, current.acquiredAtSeconds) >= maxLeaseSeconds)
    {
        transition.event = CandidateLeaseEvent::ExpiredMaxLease;
        return transition;
    }

    float const minDistanceReduction = policy.minDistanceReduction > 0.0f ? policy.minDistanceReduction : 0.0f;
    bool const distanceReduced = std::isfinite(sample.distanceToCandidate) &&
                                 (!std::isfinite(current.bestDistance) ||
                                  sample.distanceToCandidate + minDistanceReduction <= current.bestDistance);

    float const minPositionChange = policy.minPositionChange > 0.0f ? policy.minPositionChange : 0.0f;
    bool const positionChanged =
        PositionDistanceSquared(sample.position, current.lastProgressPosition) >=
        minPositionChange * minPositionChange;

    if (distanceReduced || positionChanged)
    {
        transition.lease.lastProgressAtSeconds = sample.nowSeconds;
        transition.lease.lastProgressPosition = sample.position;
        if (distanceReduced)
            transition.lease.bestDistance = sample.distanceToCandidate;
        transition.event = CandidateLeaseEvent::Progress;
        return transition;
    }

    std::uint64_t const noProgressWindowSeconds =
        policy.noProgressWindowSeconds ? policy.noProgressWindowSeconds : 1;
    transition.event = ElapsedSeconds(sample.nowSeconds, current.lastProgressAtSeconds) >= noProgressWindowSeconds
                           ? CandidateLeaseEvent::ExpiredNoProgress
                           : CandidateLeaseEvent::Pending;
    return transition;
}

inline char const* CandidateLeaseReason(CandidateLeaseEvent event)
{
    switch (event)
    {
        case CandidateLeaseEvent::ExpiredNoProgress: return "candidate_lease_no_progress";
        case CandidateLeaseEvent::ExpiredMaxLease: return "candidate_lease_max_age";
        default: return "none";
    }
}

enum class GatherBagAdmission : std::uint8_t
{
    Admit = 0,
    CheckCapacity
};

inline GatherBagAdmission EvaluateGatherBagAdmission(bool gatheringSource, bool hasGameClientMaster)
{
    return gatheringSource && !hasGameClientMaster
               ? GatherBagAdmission::CheckCapacity
               : GatherBagAdmission::Admit;
}

inline bool HasGatherBagCapacity(std::uint8_t bagUsagePercent)
{
    return bagUsagePercent <= 80;
}

enum class LiquidContact : std::uint8_t
{
    NoWater = 0,
    AboveWater,
    WaterWalk,
    InWater,
    UnderWater,
    Unknown
};

enum class LiquidKind : std::uint8_t
{
    None = 0,
    Water,
    Hazardous,
    Unknown
};

enum class LiquidEndpointClass : std::uint8_t
{
    Dry = 0,
    ShallowWater,
    DeepWater,
    Hazardous,
    Unknown
};

struct LiquidEndpointObservation
{
    LiquidContact contact = LiquidContact::Unknown;
    LiquidKind kind = LiquidKind::Unknown;
};

inline LiquidEndpointClass ClassifyLiquidEndpoint(LiquidEndpointObservation const& observation)
{
    if (observation.contact == LiquidContact::NoWater || observation.contact == LiquidContact::AboveWater)
        return LiquidEndpointClass::Dry;

    if (observation.kind == LiquidKind::Hazardous)
        return LiquidEndpointClass::Hazardous;

    if (observation.kind != LiquidKind::Water)
        return LiquidEndpointClass::Unknown;

    switch (observation.contact)
    {
        case LiquidContact::WaterWalk:
        case LiquidContact::InWater: return LiquidEndpointClass::ShallowWater;
        case LiquidContact::UnderWater: return LiquidEndpointClass::DeepWater;
        default: return LiquidEndpointClass::Unknown;
    }
}

struct SwimmingRouteProof
{
    bool canSwim = false;
    bool pathCalculated = false;
    bool pathComplete = false;
    bool allPathPointsInWater = false;
};

enum class LiquidSafetyDecision : std::uint8_t
{
    AllowDry = 0,
    AllowShallowWater,
    AllowSupportedSwimmingRoute,
    RejectHazardousLiquid,
    RejectUnsupportedDeepWater,
    RejectUnknownLiquid
};

inline LiquidSafetyDecision EvaluateLiquidSafety(LiquidEndpointClass endpoint,
                                                 SwimmingRouteProof const& route)
{
    switch (endpoint)
    {
        case LiquidEndpointClass::Dry: return LiquidSafetyDecision::AllowDry;
        case LiquidEndpointClass::ShallowWater: return LiquidSafetyDecision::AllowShallowWater;
        case LiquidEndpointClass::DeepWater:
            return route.canSwim && route.pathCalculated && route.pathComplete && route.allPathPointsInWater
                       ? LiquidSafetyDecision::AllowSupportedSwimmingRoute
                       : LiquidSafetyDecision::RejectUnsupportedDeepWater;
        case LiquidEndpointClass::Hazardous: return LiquidSafetyDecision::RejectHazardousLiquid;
        default: return LiquidSafetyDecision::RejectUnknownLiquid;
    }
}

inline bool IsLiquidSafetyAllowed(LiquidSafetyDecision decision)
{
    return decision == LiquidSafetyDecision::AllowDry ||
           decision == LiquidSafetyDecision::AllowShallowWater ||
           decision == LiquidSafetyDecision::AllowSupportedSwimmingRoute;
}

inline char const* LiquidSafetyReason(LiquidSafetyDecision decision)
{
    switch (decision)
    {
        case LiquidSafetyDecision::RejectHazardousLiquid: return "hazardous_liquid_endpoint";
        case LiquidSafetyDecision::RejectUnsupportedDeepWater: return "deep_water_route_unsupported";
        case LiquidSafetyDecision::RejectUnknownLiquid: return "unknown_liquid_endpoint";
        default: return "none";
    }
}
}

#endif
