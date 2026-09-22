/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AdvanceFormation.h"

#include <cmath>

namespace AutoWowAdvanceFormation
{
Destination ForSlot(std::uint32_t slot, float originX, float originY,
                    float waypointX, float waypointY, float fallbackOrientation, float lateralScale)
{
    if (!slot)
        return {waypointX, waypointY, 0.0f, 0.0f};

    float forwardX = waypointX - originX;
    float forwardY = waypointY - originY;
    float const length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (length > 0.01f)
    {
        forwardX /= length;
        forwardY /= length;
    }
    else
    {
        forwardX = std::cos(fallbackOrientation);
        forwardY = std::sin(fallbackOrientation);
    }

    std::uint32_t const row = ((slot - 1u) / 5u) + 1u;
    std::uint32_t const column = (slot - 1u) % 5u;
    float const trailing = 2.5f * static_cast<float>(row);
    float const lateral = 1.5f * (static_cast<float>(column) - 2.0f) * lateralScale;

    // Left is the 90-degree counter-clockwise normal of forward. Rows trail back along the
    // known approach vector rather than radiating into walls or open void beside the corridor.
    float const leftX = -forwardY;
    float const leftY = forwardX;
    return {
        waypointX - forwardX * trailing + leftX * lateral,
        waypointY - forwardY * trailing + leftY * lateral,
        trailing,
        lateral,
    };
}
}
