/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_PLAYERBOTS_AUTOWOW_ADVANCE_FORMATION_H
#define MOD_PLAYERBOTS_AUTOWOW_ADVANCE_FORMATION_H

#include <cstdint>

namespace AutoWowAdvanceFormation
{
struct Destination
{
    float x = 0.0f;
    float y = 0.0f;
    float trailing = 0.0f;
    float lateral = 0.0f;
};

// Slot zero is the leader at the named waypoint. Remaining slots form deterministic five-wide
// rows behind the waypoint, aligned with the path used to reach it. This avoids collapsing an
// entire raid onto one XYZ while keeping the formation narrow enough for instance corridors.
Destination ForSlot(std::uint32_t slot, float originX, float originY,
                    float waypointX, float waypointY, float fallbackOrientation,
                    float lateralScale = 1.0f);
}

#endif
