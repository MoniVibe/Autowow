/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATORPACINGPOLICY_H
#define PLAYERBOTS_DUNGEONNAVIGATORPACINGPOLICY_H

#include <algorithm>
#include <cstdint>

namespace DungeonNavigatorPacing
{
constexpr std::uint32_t SuccessfulMoveBackoffCeilingMs = 5000u;
constexpr std::uint32_t SuccessfulMoveBackoffFloorMs = 100u;

// Successful navigator scans may be accelerated by MaxWaitForMove, but never exceed the
// established five-second pacing or fall below a safe pathfinding-rescan floor.
constexpr std::uint32_t GetSuccessfulMoveBackoffMs(std::uint32_t maxWaitForMoveMs)
{
    return std::clamp(maxWaitForMoveMs, SuccessfulMoveBackoffFloorMs,
        SuccessfulMoveBackoffCeilingMs);
}
}

#endif
