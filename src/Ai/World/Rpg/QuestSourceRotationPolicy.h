/*
 * Pure policy for rotating an incomplete quest objective across resolved source spawns.
 *
 * This policy has no world access and never invents a target.  The caller supplies only
 * objective-resolved candidates; the native targeter remains responsible for live-unit,
 * attackability, LOS, loot, and credit checks.
 */
#ifndef PLAYERBOTS_QUEST_SOURCE_ROTATION_POLICY_H
#define PLAYERBOTS_QUEST_SOURCE_ROTATION_POLICY_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace QuestSourceRotationPolicy
{
struct Candidate
{
    std::int32_t signedEntry = 0;
    std::uint32_t mapId = 0;
    std::uint64_t spawnKey = 0;
    float distance = 0.0f;
};

enum class Decision : std::uint8_t
{
    PreserveActiveTarget,
    RotateToNextSource,
    BoundedFailure
};

struct Request
{
    std::uint32_t botMapId = 0;
    std::int32_t currentSignedEntry = 0;
    std::uint64_t currentSpawnKey = 0;
    bool activeTarget = false;
    std::uint32_t rotationCount = 0;
    std::uint32_t maxRotations = 0;
    std::vector<std::int32_t> objectiveEntries;
    std::vector<std::uint64_t> exhaustedSpawnKeys;
    std::vector<Candidate> candidates;
};

struct Result
{
    Decision decision = Decision::BoundedFailure;
    Candidate candidate{};
};

[[nodiscard]] inline bool Contains(std::vector<std::int32_t> const& values, std::int32_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] inline bool Contains(std::vector<std::uint64_t> const& values, std::uint64_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] inline Result Decide(Request const& request)
{
    // A live target owns the current source. Rotation is never allowed to steal an active pull.
    if (request.activeTarget)
        return {Decision::PreserveActiveTarget, {}};

    if (request.rotationCount >= request.maxRotations)
        return {Decision::BoundedFailure, {}};

    Candidate const* best = nullptr;
    for (Candidate const& candidate : request.candidates)
    {
        // A zero key cannot be remembered as exhausted and would make a retry loop possible.
        if (candidate.spawnKey == 0 || candidate.mapId != request.botMapId ||
            !Contains(request.objectiveEntries, candidate.signedEntry) ||
            candidate.signedEntry == request.currentSignedEntry &&
                candidate.spawnKey == request.currentSpawnKey ||
            Contains(request.exhaustedSpawnKeys, candidate.spawnKey))
            continue;

        bool const better = !best || candidate.distance < best->distance ||
                            (candidate.distance == best->distance &&
                             (candidate.signedEntry < best->signedEntry ||
                              (candidate.signedEntry == best->signedEntry &&
                               candidate.spawnKey < best->spawnKey)));
        if (better)
            best = &candidate;
    }

    return best ? Result{Decision::RotateToNextSource, *best}
                : Result{Decision::BoundedFailure, {}};
}
}  // namespace QuestSourceRotationPolicy

#endif  // PLAYERBOTS_QUEST_SOURCE_ROTATION_POLICY_H
