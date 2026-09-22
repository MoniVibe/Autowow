/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_RELEASEDCORPSEAPPROACHPOLICY_H
#define PLAYERBOTS_RELEASEDCORPSEAPPROACHPOLICY_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ReleasedCorpseApproachPolicy
{
constexpr std::size_t RingCount = 4;
constexpr std::size_t AnglesPerRing = 8;
constexpr std::size_t MaxProbeCount = 1 + RingCount * AnglesPerRing;
constexpr std::size_t FirstFallbackProbe = 1;
constexpr float MaxVerticalDelta = 5.0f;

struct Candidate
{
    std::size_t probeIndex = 0;
    float radius = 0.0f;
    std::uint8_t angleIndex = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct Evaluation
{
    bool floorValid = false;
    bool navmeshReachable = false;
    bool corpseLineOfSight = false;
    float endpointZ = 0.0f;
};

struct Selection
{
    bool found = false;
    Candidate candidate;
    float endpointZ = 0.0f;
    std::size_t probesEvaluated = 0;
};

inline std::array<Candidate, MaxProbeCount> BuildCandidates(float corpseX, float corpseY)
{
    constexpr std::array<float, RingCount> radii = {2.0f, 4.0f, 8.0f, 12.0f};
    constexpr float pi = 3.14159265358979323846f;
    constexpr float angleStep = 2.0f * pi / static_cast<float>(AnglesPerRing);

    std::array<Candidate, MaxProbeCount> candidates{};
    candidates[0] = {0, 0.0f, 0, corpseX, corpseY};

    std::size_t probeIndex = 1;
    for (float radius : radii)
    {
        for (std::size_t angleIndex = 0; angleIndex < AnglesPerRing; ++angleIndex)
        {
            float const angle = static_cast<float>(angleIndex) * angleStep;
            candidates[probeIndex] = {
                probeIndex,
                radius,
                static_cast<std::uint8_t>(angleIndex),
                corpseX + std::cos(angle) * radius,
                corpseY + std::sin(angle) * radius,
            };
            ++probeIndex;
        }
    }

    return candidates;
}

inline bool IsSameVerticalLevel(float corpseZ, float endpointZ)
{
    return std::isfinite(corpseZ) && std::isfinite(endpointZ) &&
           std::fabs(endpointZ - corpseZ) <= MaxVerticalDelta;
}

inline bool IsAcceptable(float corpseZ, Evaluation const& evaluation)
{
    return evaluation.floorValid && evaluation.navmeshReachable &&
           evaluation.corpseLineOfSight && IsSameVerticalLevel(corpseZ, evaluation.endpointZ);
}

template <typename Evaluator>
Selection Select(std::array<Candidate, MaxProbeCount> const& candidates, float corpseZ,
                 std::size_t firstProbeIndex, Evaluator&& evaluator)
{
    Selection selection;
    if (firstProbeIndex >= candidates.size())
        return selection;

    for (std::size_t index = firstProbeIndex; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        Evaluation const evaluation = evaluator(candidate);
        ++selection.probesEvaluated;
        if (!IsAcceptable(corpseZ, evaluation))
            continue;

        selection.found = true;
        selection.candidate = candidate;
        selection.endpointZ = evaluation.endpointZ;
        return selection;
    }

    return selection;
}
}

#endif
