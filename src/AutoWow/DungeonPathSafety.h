/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOW_DUNGEON_PATH_SAFETY_H
#define _PLAYERBOT_AUTOWOW_DUNGEON_PATH_SAFETY_H

#include "PathGenerator.h"

#include <cstddef>
#include <string>

class Player;

namespace AutoWowDungeonPath
{
struct ValidationFacts
{
    bool calculated = false;
    uint32 pathType = PATHFIND_BLANK;
    std::size_t pointCount = 0;
    float endpointDistance = 0.0f;
    float maxSegmentVerticalDelta = 0.0f;
    float excursionBelowEndpoints = 0.0f;
    bool allGroundSamplesValid = false;
    float maxGroundDelta = 0.0f;
    float sourceDistance = 0.0f;
};

struct GroundLineFacts
{
    bool withinLengthLimit = false;
    std::size_t pointCount = 0;
    bool allGroundSamplesValid = false;
    float maxExpectedGroundDelta = 0.0f;
    bool allSegmentsInLineOfSight = false;
    float maxSegmentVerticalDelta = 0.0f;
};

struct ProbeResult
{
    bool safe = false;
    std::string mode = "navmesh";
    std::string reason;
    std::string navmeshReason;
    std::string groundLineReason;
    uint32 pathType = PATHFIND_BLANK;
    Movement::PointsArray path;
    float destinationX = 0.0f;
    float destinationY = 0.0f;
    float destinationZ = 0.0f;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceZ = 0.0f;
    float endpointDistance = 0.0f;
    float pathLength = 0.0f;
    float maxSegmentVerticalDelta = 0.0f;
    float excursionBelowEndpoints = 0.0f;
    bool allGroundSamplesValid = false;
    float maxGroundDelta = 0.0f;
};

// Pure policy seam used by unit tests. An empty string means the path is safe.
std::string Validate(ValidationFacts const& facts);
std::string ValidateGroundLine(GroundLineFacts const& facts);

// Navmesh/vmap probe. It prepares only the required terrain collision cache and never changes
// player position, object data, or MotionMaster state.
ProbeResult Probe(Player const* player, float destinationX, float destinationY, float destinationZ);

// Same read-only probe with an explicit virtual source. The player supplies map, phase, and
// movement capabilities only; its live position and MotionMaster remain untouched.
ProbeResult ProbeFrom(Player const* player, float sourceX, float sourceY, float sourceZ,
                      float destinationX, float destinationY, float destinationZ);

std::string Json(ProbeResult const& result, bool includePoints = true);
}

#endif
