/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_RELEASED_DUNGEON_CORPSE_APPROACH_POLICY_H
#define PLAYERBOTS_RELEASED_DUNGEON_CORPSE_APPROACH_POLICY_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ReleasedCorpseApproachPolicy
{
constexpr std::size_t IngressSphereRingCount = 4;
constexpr std::size_t IngressSpherePointsPerRing = 8;
constexpr std::size_t MaxIngressApproachCandidates =
    1 + IngressSphereRingCount * IngressSpherePointsPerRing;
constexpr float IngressTriggerSafetyMargin = 0.1f;

struct IngressTriggerVolume
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    float length = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float orientation = 0.0f;
};

struct IngressApproachCandidate
{
    std::size_t candidateIndex = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct IngressApproachEvaluation
{
    bool floorValid = false;
    bool pathReachable = false;
    bool endpointInside = false;
    float endpointX = 0.0f;
    float endpointY = 0.0f;
    float endpointZ = 0.0f;
    float pathDistance = std::numeric_limits<float>::infinity();
};

struct IngressApproachSelection
{
    bool found = false;
    IngressApproachCandidate candidate;
    float endpointX = 0.0f;
    float endpointY = 0.0f;
    float endpointZ = 0.0f;
    float pathDistance = 0.0f;
    std::size_t candidateCount = 0;
    std::size_t probesEvaluated = 0;
};

inline bool IsFiniteIngressTriggerVolume(IngressTriggerVolume const& volume)
{
    return std::isfinite(volume.x) && std::isfinite(volume.y) &&
        std::isfinite(volume.z) && std::isfinite(volume.radius) &&
        std::isfinite(volume.length) && std::isfinite(volume.width) &&
        std::isfinite(volume.height) && std::isfinite(volume.orientation);
}

// Equivalent to Player::IsInAreaTriggerRadius for an arbitrary point. A positive margin shrinks
// the volume and is used only while generating probes; runtime endpoint admission uses margin 0.
inline bool IsInsideIngressTrigger(IngressTriggerVolume const& volume,
                                   float x, float y, float z, float margin = 0.0f)
{
    if (!IsFiniteIngressTriggerVolume(volume) || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(margin) || margin < 0.0f)
    {
        return false;
    }

    float const dx = x - volume.x;
    float const dy = y - volume.y;
    float const dz = z - volume.z;
    if (volume.radius > 0.0f)
    {
        float const usableRadius = volume.radius - margin;
        return usableRadius >= 0.0f &&
            dx * dx + dy * dy + dz * dz <= usableRadius * usableRadius;
    }

    float const xRadius = volume.length * 0.5f - margin;
    float const yRadius = volume.width * 0.5f - margin;
    float const zRadius = volume.height * 0.5f - margin;
    if (xRadius < 0.0f || yRadius < 0.0f || zRadius < 0.0f)
        return false;

    // Position::IsWithinBox rotates the point by -orientation before its axis-aligned check.
    float const cosine = std::cos(volume.orientation);
    float const sine = std::sin(volume.orientation);
    float const localX = dx * cosine + dy * sine;
    float const localY = dy * cosine - dx * sine;
    return std::fabs(localX) <= xRadius && std::fabs(localY) <= yRadius &&
        std::fabs(dz) <= zRadius;
}

inline std::vector<IngressApproachCandidate> BuildIngressApproachCandidates(
    IngressTriggerVolume const& volume)
{
    std::vector<IngressApproachCandidate> candidates;
    candidates.reserve(MaxIngressApproachCandidates);
    if (!IsInsideIngressTrigger(volume, volume.x, volume.y, volume.z,
                                IngressTriggerSafetyMargin))
    {
        return candidates;
    }

    candidates.push_back({0, volume.x, volume.y, volume.z});
    if (volume.radius > 0.0f)
    {
        constexpr float pi = 3.14159265358979323846f;
        constexpr float angleStep = 2.0f * pi /
            static_cast<float>(IngressSpherePointsPerRing);
        constexpr float boundaryInset = 0.95f;
        float const usableRadius = (volume.radius - IngressTriggerSafetyMargin) * boundaryInset;
        for (std::size_t ring = 1; ring <= IngressSphereRingCount; ++ring)
        {
            float const ringRadius = usableRadius * static_cast<float>(ring) /
                static_cast<float>(IngressSphereRingCount);
            for (std::size_t angleIndex = 0;
                 angleIndex < IngressSpherePointsPerRing; ++angleIndex)
            {
                if (candidates.size() >= MaxIngressApproachCandidates)
                    return candidates;
                float const angle = static_cast<float>(angleIndex) * angleStep;
                candidates.push_back({candidates.size(),
                    volume.x + std::cos(angle) * ringRadius,
                    volume.y + std::sin(angle) * ringRadius,
                    volume.z});
            }
        }
        return candidates;
    }

    // A bounded 5x5 local grid covers oriented boxes. Center is already first; row-major local
    // ordering is stable and the outer row/columns remain inset from the real trigger faces.
    constexpr std::array<float, 5> gridFractions = {-0.95f, -0.475f, 0.0f, 0.475f, 0.95f};
    float const usableXRadius = volume.length * 0.5f - IngressTriggerSafetyMargin;
    float const usableYRadius = volume.width * 0.5f - IngressTriggerSafetyMargin;
    float const cosine = std::cos(volume.orientation);
    float const sine = std::sin(volume.orientation);
    for (float const yFraction : gridFractions)
    {
        for (float const xFraction : gridFractions)
        {
            if (xFraction == 0.0f && yFraction == 0.0f)
                continue;
            if (candidates.size() >= MaxIngressApproachCandidates)
                return candidates;

            float const localX = xFraction * usableXRadius;
            float const localY = yFraction * usableYRadius;
            candidates.push_back({candidates.size(),
                volume.x + localX * cosine - localY * sine,
                volume.y + localX * sine + localY * cosine,
                volume.z});
        }
    }
    return candidates;
}

inline bool IsAcceptableIngressApproach(IngressApproachEvaluation const& evaluation)
{
    return evaluation.floorValid && evaluation.pathReachable && evaluation.endpointInside &&
        std::isfinite(evaluation.endpointX) && std::isfinite(evaluation.endpointY) &&
        std::isfinite(evaluation.endpointZ) && std::isfinite(evaluation.pathDistance) &&
        evaluation.pathDistance >= 0.0f;
}

template <typename CandidateRange, typename Evaluator>
IngressApproachSelection SelectIngressApproach(CandidateRange const& candidates,
                                                Evaluator&& evaluator)
{
    IngressApproachSelection selection;
    selection.candidateCount = candidates.size();
    for (IngressApproachCandidate const& candidate : candidates)
    {
        IngressApproachEvaluation const evaluation = evaluator(candidate);
        ++selection.probesEvaluated;
        if (!IsAcceptableIngressApproach(evaluation))
            continue;

        bool const better = !selection.found ||
            evaluation.pathDistance < selection.pathDistance ||
            (evaluation.pathDistance == selection.pathDistance &&
             candidate.candidateIndex < selection.candidate.candidateIndex);
        if (!better)
            continue;

        selection.found = true;
        selection.candidate = candidate;
        selection.endpointX = evaluation.endpointX;
        selection.endpointY = evaluation.endpointY;
        selection.endpointZ = evaluation.endpointZ;
        selection.pathDistance = evaluation.pathDistance;
    }
    return selection;
}

struct IngressPortalCandidate
{
    std::uint32_t triggerId = 0;
    std::uint32_t sourceMap = 0;
    std::uint32_t targetMap = 0;
    float distanceSquared = 0.0f;
    bool triggerAvailable = false;
};

struct IngressPortalSelection
{
    bool found = false;
    IngressPortalCandidate candidate;
};

constexpr bool IsMatchingIngress(IngressPortalCandidate const& candidate,
                                 std::uint32_t currentMap, std::uint32_t corpseMap)
{
    return candidate.triggerAvailable && candidate.triggerId != 0 &&
        candidate.sourceMap == currentMap && candidate.targetMap == corpseMap &&
        candidate.distanceSquared >= 0.0f;
}

constexpr bool IsBetterIngress(IngressPortalCandidate const& candidate,
                               IngressPortalCandidate const& current)
{
    return candidate.distanceSquared < current.distanceSquared ||
        (candidate.distanceSquared == current.distanceSquared &&
         candidate.triggerId < current.triggerId);
}

template <typename CandidateRange>
IngressPortalSelection SelectIngressPortal(CandidateRange const& candidates,
                                           std::uint32_t currentMap,
                                           std::uint32_t corpseMap)
{
    IngressPortalSelection selection;
    for (IngressPortalCandidate const& candidate : candidates)
    {
        if (!IsMatchingIngress(candidate, currentMap, corpseMap))
            continue;

        if (!selection.found || IsBetterIngress(candidate, selection.candidate))
        {
            selection.found = true;
            selection.candidate = candidate;
        }
    }
    return selection;
}
}

#endif
