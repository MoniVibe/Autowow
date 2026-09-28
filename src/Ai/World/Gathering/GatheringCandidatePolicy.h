/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGCANDIDATEPOLICY_H
#define PLAYERBOTS_GATHERINGCANDIDATEPOLICY_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AutoWowGather
{
enum class Profession : std::uint8_t
{
    None = 0,
    Herbalism,
    Mining,
    Skinning
};

struct Candidate
{
    std::uint64_t spawnId = 0;
    std::uint32_t entry = 0;
    std::uint32_t mapId = 0;
    std::uint32_t phaseMask = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    Profession profession = Profession::None;
    std::uint32_t requiredSkill = 0;
    std::uint32_t nodeLevel = 0;
    std::string name;
};

struct Profile
{
    std::uint32_t mapId = 0;
    std::uint32_t phaseMask = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint32_t level = 0;
    std::uint32_t herbalismSkill = 0;
    std::uint32_t miningSkill = 0;
    std::uint32_t skinningSkill = 0;
    bool hasMiningTool = false;
    float maxDistance = 0.0f;
    std::uint32_t maxNodeLevelAboveBot = 0;
    bool anySkill = false;  // AutoWow.Gather.AnySkill: a known profession works nodes above its skill
};

using CooldownMap = std::unordered_map<std::uint64_t, std::uint64_t>;

inline char const* ProfessionName(Profession profession)
{
    switch (profession)
    {
        case Profession::Herbalism: return "herbalism";
        case Profession::Mining: return "mining";
        case Profession::Skinning: return "skinning";
        default: return "none";
    }
}

inline float DistanceSquared(Profile const& profile, Candidate const& candidate)
{
    float const dx = candidate.x - profile.x;
    float const dy = candidate.y - profile.y;
    float const dz = candidate.z - profile.z;
    return dx * dx + dy * dy + dz * dz;
}

inline bool IsEligible(Candidate const& candidate, Profile const& profile, CooldownMap const& cooldowns,
                       std::uint64_t nowSeconds)
{
    if (!candidate.spawnId || candidate.mapId != profile.mapId)
        return false;

    if (candidate.phaseMask && profile.phaseMask && !(candidate.phaseMask & profile.phaseMask))
        return false;

    std::uint32_t skill = 0;
    switch (candidate.profession)
    {
        case Profession::Herbalism: skill = profile.herbalismSkill; break;
        case Profession::Mining:
            if (!profile.hasMiningTool)
                return false;
            skill = profile.miningSkill;
            break;
        case Profession::Skinning: skill = profile.skinningSkill; break;
        default: return false;
    }

    if (!skill || (skill < candidate.requiredSkill && !profile.anySkill))  // AutoWow.Gather.AnySkill
        return false;

    if (candidate.nodeLevel && candidate.nodeLevel > profile.level + profile.maxNodeLevelAboveBot)
        return false;

    if (profile.maxDistance > 0.0f && DistanceSquared(profile, candidate) > profile.maxDistance * profile.maxDistance)
        return false;

    auto const cooldown = cooldowns.find(candidate.spawnId);
    return cooldown == cooldowns.end() || cooldown->second <= nowSeconds;
}

// Selection is intentionally deterministic even though ObjectMgr's source container is unordered:
// nearest 3-D distance wins, followed by entry and spawn id as stable tie breakers.
inline std::optional<std::size_t> SelectCandidate(std::vector<Candidate> const& candidates, Profile const& profile,
                                                  CooldownMap const& cooldowns, std::uint64_t nowSeconds)
{
    std::optional<std::size_t> best;
    float bestDistance = std::numeric_limits<float>::max();

    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        if (!IsEligible(candidate, profile, cooldowns, nowSeconds))
            continue;

        float const distance = DistanceSquared(profile, candidate);
        if (!best || distance < bestDistance ||
            (std::fabs(distance - bestDistance) <= 0.001f &&
             (candidate.entry < candidates[*best].entry ||
              (candidate.entry == candidates[*best].entry && candidate.spawnId < candidates[*best].spawnId))))
        {
            best = index;
            bestDistance = distance;
        }
    }

    return best;
}
}

#endif
