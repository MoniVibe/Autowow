/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONPROGRESSIONINTERACTIONPOLICY_H
#define PLAYERBOTS_DUNGEONPROGRESSIONINTERACTIONPOLICY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace DungeonProgressionInteraction
{
constexpr float MaximumLeaderDistance = 80.0f;
constexpr float MaximumBossDistance = 80.0f;

struct GateFacts
{
    bool arrived = false;
    bool bossAlive = false;
    bool bossInWorld = false;
    bool bossNotTargetable = false;
    bool partyOutOfCombat = false;
};

struct CandidateFacts
{
    std::uint64_t spawnId = 0;
    std::uint32_t entry = 0;
    float leaderDistance = 0.0f;
    float bossDistance = 0.0f;
    bool inWorld = false;
    bool spawned = false;
    bool ready = false;
    bool selectable = false;
    bool notInUse = false;
    bool supportedType = false;
};

inline bool GateActive(GateFacts const& facts)
{
    return facts.arrived && facts.bossAlive && facts.bossInWorld && facts.bossNotTargetable &&
           facts.partyOutOfCombat;
}

inline bool Eligible(CandidateFacts const& candidate)
{
    return candidate.spawnId && candidate.entry && candidate.inWorld && candidate.spawned &&
           candidate.ready && candidate.selectable && candidate.notInUse && candidate.supportedType &&
           candidate.leaderDistance <= MaximumLeaderDistance && candidate.bossDistance <= MaximumBossDistance;
}

inline std::optional<std::size_t> Select(GateFacts const& gate,
                                         std::vector<CandidateFacts> const& candidates)
{
    if (!GateActive(gate))
        return std::nullopt;

    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        CandidateFacts const& candidate = candidates[index];
        if (!Eligible(candidate))
            continue;

        if (!selected)
        {
            selected = index;
            continue;
        }

        CandidateFacts const& current = candidates[*selected];
        if (candidate.leaderDistance < current.leaderDistance ||
            (candidate.leaderDistance == current.leaderDistance && candidate.bossDistance < current.bossDistance) ||
            (candidate.leaderDistance == current.leaderDistance && candidate.bossDistance == current.bossDistance &&
             candidate.spawnId < current.spawnId))
        {
            selected = index;
        }
    }
    return selected;
}
}

#endif
