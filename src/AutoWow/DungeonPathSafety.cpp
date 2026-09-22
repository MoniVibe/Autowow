/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonPathSafety.h"

#include "Map.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace AutoWowDungeonPath
{
namespace
{
constexpr uint32 RejectedPathTypes = PATHFIND_SHORTCUT | PATHFIND_NOPATH |
                                     PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY;
constexpr float MaxEndpointDistance = 2.5f;
// Detour can preserve a complete normal corridor while adding PATHFIND_INCOMPLETE when a requested
// formation tangent ends outside the final polygon or behind a closed encounter door. Accept that
// hybrid only when the grounded route has real intermediate points and makes measurable forward
// progress; actual shortcuts stay rejected and the director separately detects repeated stalls.
constexpr float MinimumIncompletePathProgress = 2.0f;
constexpr float MaxSegmentVerticalDelta = 5.5f;
constexpr float MaxExcursionBelowEndpoints = 8.0f;
constexpr float MaxGroundDelta = 3.5f;
constexpr float GroundLineSampleSpacing = 1.5f;
constexpr float MaxGroundLineLength = 400.0f;
constexpr float MaxGroundLineExpectedDelta = 3.5f;
constexpr float MaxGroundLineSegmentVerticalDelta = 3.0f;
// Smaller than half a WoW map grid (533.333 yards), so every grid intersected by a
// straight source/destination corridor is prepared before Detour or VMap is queried.
constexpr float CollisionPreparationSpacing = 200.0f;

float Distance(G3D::Vector3 const& left, G3D::Vector3 const& right)
{
    float const dx = right.x - left.x;
    float const dy = right.y - left.y;
    float const dz = right.z - left.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void PrepareCollisionCorridor(Player const* player, float sourceX, float sourceY,
                              float destinationX, float destinationY)
{
    if (!player || !player->GetMap())
        return;

    float const dx = destinationX - sourceX;
    float const dy = destinationY - sourceY;
    float const horizontalLength = std::sqrt(dx * dx + dy * dy);
    std::size_t const segmentCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(horizontalLength / CollisionPreparationSpacing)));

    // GetGridTerrainData creates terrain/collision data but does not call LoadGrid, so this
    // loads the parent map's map/vmap/mmap tiles without spawning distant creatures or
    // relocating the probing player. Instance maps share those parent collision objects.
    Map* map = player->GetMap();
    for (std::size_t index = 0; index <= segmentCount; ++index)
    {
        float const ratio = static_cast<float>(index) / static_cast<float>(segmentCount);
        map->GetGridTerrainData(sourceX + dx * ratio, sourceY + dy * ratio);
    }
}
}

std::string ValidateGroundLine(GroundLineFacts const& facts)
{
    if (!facts.withinLengthLimit)
        return "ground_line_too_long";
    if (facts.pointCount < 2)
        return "ground_line_too_short";
    if (!facts.allGroundSamplesValid)
        return "ground_line_missing_floor";
    if (!std::isfinite(facts.maxExpectedGroundDelta) ||
        facts.maxExpectedGroundDelta > MaxGroundLineExpectedDelta)
        return "ground_line_floor_mismatch";
    if (!facts.allSegmentsInLineOfSight)
        return "ground_line_blocked";
    if (!std::isfinite(facts.maxSegmentVerticalDelta) ||
        facts.maxSegmentVerticalDelta > MaxGroundLineSegmentVerticalDelta)
        return "ground_line_too_steep";
    return {};
}

std::string Validate(ValidationFacts const& facts)
{
    if (!facts.calculated)
        return "path_calculation_failed";
    if (!(facts.pathType & PATHFIND_NORMAL))
        return "path_not_normal";
    if (facts.pathType & RejectedPathTypes)
        return "unsafe_path_type";
    bool const hybridNormalIncomplete = (facts.pathType & PATHFIND_INCOMPLETE) != 0;
    if (hybridNormalIncomplete &&
        (facts.pathType != (PATHFIND_NORMAL | PATHFIND_INCOMPLETE) || facts.pointCount < 3))
        return "unsafe_path_type";
    if (facts.pointCount < 2)
        return "path_too_short";
    if (!std::isfinite(facts.endpointDistance))
        return "endpoint_not_reached";
    if (hybridNormalIncomplete)
    {
        if (!std::isfinite(facts.sourceDistance) ||
            facts.sourceDistance - facts.endpointDistance < MinimumIncompletePathProgress)
            return "incomplete_path_no_progress";
    }
    else if (facts.endpointDistance > MaxEndpointDistance)
        return "endpoint_not_reached";
    if (!std::isfinite(facts.maxSegmentVerticalDelta) ||
        facts.maxSegmentVerticalDelta > MaxSegmentVerticalDelta)
        return "vertical_segment_too_large";
    if (!std::isfinite(facts.excursionBelowEndpoints) ||
        facts.excursionBelowEndpoints > MaxExcursionBelowEndpoints)
        return "path_dips_below_corridor";
    if (!facts.allGroundSamplesValid)
        return "ground_sample_missing";
    if (!std::isfinite(facts.maxGroundDelta) || facts.maxGroundDelta > MaxGroundDelta)
        return "path_point_off_ground";
    return {};
}

namespace
{
ProbeResult ProbeNavmeshFrom(Player const* player, float sourceX, float sourceY, float sourceZ,
                             float destinationX, float destinationY, float destinationZ)
{
    ProbeResult result;
    result.sourceX = sourceX;
    result.sourceY = sourceY;
    result.sourceZ = sourceZ;
    result.destinationX = destinationX;
    result.destinationY = destinationY;
    result.destinationZ = destinationZ;

    if (!player || !player->IsInWorld() || !player->GetMap())
    {
        result.reason = "player_not_in_world";
        return result;
    }

    if (!std::isfinite(sourceX) || !std::isfinite(sourceY) || !std::isfinite(sourceZ) ||
        !std::isfinite(destinationX) || !std::isfinite(destinationY) || !std::isfinite(destinationZ))
    {
        result.reason = "destination_not_finite";
        return result;
    }

    PrepareCollisionCorridor(player, sourceX, sourceY, destinationX, destinationY);

    PathGenerator generator(player);
    generator.SetSlopeCheck(true);
    bool const calculated = generator.CalculatePath(
        sourceX, sourceY, sourceZ, destinationX, destinationY, destinationZ, false);
    result.pathType = static_cast<uint32>(generator.GetPathType());
    result.path = generator.GetPath();

    G3D::Vector3 const destination(destinationX, destinationY, destinationZ);
    if (!result.path.empty())
        result.endpointDistance = Distance(result.path.back(), destination);
    else
        result.endpointDistance = std::numeric_limits<float>::infinity();

    float minimumPathZ = std::min(sourceZ, destinationZ);
    result.allGroundSamplesValid = !result.path.empty();
    for (std::size_t index = 0; index < result.path.size(); ++index)
    {
        G3D::Vector3 const& point = result.path[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        {
            result.allGroundSamplesValid = false;
            continue;
        }

        minimumPathZ = std::min(minimumPathZ, point.z);
        if (index)
        {
            result.pathLength += Distance(result.path[index - 1], point);
            result.maxSegmentVerticalDelta = std::max(
                result.maxSegmentVerticalDelta, std::fabs(point.z - result.path[index - 1].z));
        }

        float const ground = player->GetMap()->GetHeight(
            player->GetPhaseMask(), point.x, point.y, point.z + 2.0f, true, 10.0f);
        if (ground <= INVALID_HEIGHT || !std::isfinite(ground))
        {
            result.allGroundSamplesValid = false;
            continue;
        }
        result.maxGroundDelta = std::max(result.maxGroundDelta, std::fabs(point.z - ground));
    }

    float const endpointFloor = std::min(sourceZ, destinationZ);
    result.excursionBelowEndpoints = std::max(0.0f, endpointFloor - minimumPathZ);

    ValidationFacts const facts = {
        calculated,
        result.pathType,
        result.path.size(),
        result.endpointDistance,
        result.maxSegmentVerticalDelta,
        result.excursionBelowEndpoints,
        result.allGroundSamplesValid,
        result.maxGroundDelta,
        Distance(G3D::Vector3(sourceX, sourceY, sourceZ), destination),
    };
    result.reason = Validate(facts);
    result.safe = result.reason.empty();
    return result;
}

ProbeResult ProbeGroundLineFrom(Player const* player, float sourceX, float sourceY, float sourceZ,
                                float destinationX, float destinationY, float destinationZ)
{
    ProbeResult result;
    result.mode = "ground_line";
    result.sourceX = sourceX;
    result.sourceY = sourceY;
    result.sourceZ = sourceZ;
    result.destinationX = destinationX;
    result.destinationY = destinationY;
    result.destinationZ = destinationZ;

    if (!player || !player->IsInWorld() || !player->GetMap())
    {
        result.reason = "player_not_in_world";
        return result;
    }

    float const dx = destinationX - sourceX;
    float const dy = destinationY - sourceY;
    float const horizontalLength = std::sqrt(dx * dx + dy * dy);
    std::size_t const segmentCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(horizontalLength / GroundLineSampleSpacing)));
    bool const withinLengthLimit = horizontalLength <= MaxGroundLineLength;
    bool allGroundSamplesValid = true;
    bool allSegmentsInLineOfSight = true;
    float maxExpectedGroundDelta = 0.0f;

    if (withinLengthLimit)
    {
        result.path.reserve(segmentCount + 1);
        for (std::size_t index = 0; index <= segmentCount; ++index)
        {
            float const ratio = static_cast<float>(index) / static_cast<float>(segmentCount);
            float const x = sourceX + dx * ratio;
            float const y = sourceY + dy * ratio;
            float const expectedZ = sourceZ + (destinationZ - sourceZ) * ratio;
            float const ground = player->GetMap()->GetHeight(
                player->GetPhaseMask(), x, y, expectedZ + 2.0f, true, 6.0f);
            if (ground <= INVALID_HEIGHT || !std::isfinite(ground))
            {
                allGroundSamplesValid = false;
                break;
            }

            maxExpectedGroundDelta = std::max(maxExpectedGroundDelta, std::fabs(ground - expectedZ));
            result.path.emplace_back(x, y, ground);
            if (result.path.size() < 2)
                continue;

            G3D::Vector3 const& previous = result.path[result.path.size() - 2];
            G3D::Vector3 const& current = result.path.back();
            result.pathLength += Distance(previous, current);
            result.maxSegmentVerticalDelta = std::max(
                result.maxSegmentVerticalDelta, std::fabs(current.z - previous.z));
            float const sightHeight = std::min(1.0f, player->GetCollisionHeight());
            if (!player->GetMap()->isInLineOfSight(
                    previous.x, previous.y, previous.z + sightHeight,
                    current.x, current.y, current.z + sightHeight,
                    player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                allSegmentsInLineOfSight = false;
        }
    }

    result.allGroundSamplesValid = allGroundSamplesValid;
    result.maxGroundDelta = maxExpectedGroundDelta;
    if (!result.path.empty())
        result.endpointDistance = Distance(result.path.back(), G3D::Vector3(destinationX, destinationY, destinationZ));
    else
        result.endpointDistance = std::numeric_limits<float>::infinity();

    GroundLineFacts const facts = {
        withinLengthLimit,
        result.path.size(),
        allGroundSamplesValid,
        maxExpectedGroundDelta,
        allSegmentsInLineOfSight,
        result.maxSegmentVerticalDelta,
    };
    result.reason = ValidateGroundLine(facts);
    result.safe = result.reason.empty();
    return result;
}
}

ProbeResult ProbeFrom(Player const* player, float sourceX, float sourceY, float sourceZ,
                      float destinationX, float destinationY, float destinationZ)
{
    ProbeResult navmesh = ProbeNavmeshFrom(
        player, sourceX, sourceY, sourceZ, destinationX, destinationY, destinationZ);
    if (navmesh.safe)
        return navmesh;

    ProbeResult groundLine = ProbeGroundLineFrom(
        player, sourceX, sourceY, sourceZ, destinationX, destinationY, destinationZ);
    groundLine.navmeshReason = navmesh.reason;
    if (groundLine.safe)
        return groundLine;

    // When neither mode is safe, preserve the Detour path for diagnosis. Its XY points are
    // useful for locating missing/incorrect corridor tiles, while safe remains false so no
    // caller can execute it. The rejected ground-line reason remains visible alongside it.
    navmesh.groundLineReason = groundLine.reason;
    return navmesh;
}

ProbeResult Probe(Player const* player, float destinationX, float destinationY, float destinationZ)
{
    if (!player)
        return ProbeFrom(nullptr, 0.0f, 0.0f, 0.0f, destinationX, destinationY, destinationZ);
    return ProbeFrom(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                     destinationX, destinationY, destinationZ);
}

std::string Json(ProbeResult const& result, bool includePoints)
{
    std::ostringstream out;
    auto appendNumber = [&out](float value)
    {
        if (std::isfinite(value))
            out << value;
        else
            out << "null";
    };

    out << std::fixed << std::setprecision(3)
        << "{\"safe\":" << (result.safe ? "true" : "false")
        << ",\"mode\":\"" << result.mode << "\""
        << ",\"reason\":\"" << result.reason << "\""
        << ",\"navmesh_reason\":\"" << result.navmeshReason << "\""
        << ",\"ground_line_reason\":\"" << result.groundLineReason << "\""
        << ",\"path_type\":" << result.pathType
        << ",\"point_count\":" << result.path.size()
        << ",\"source\":{\"x\":" << result.sourceX
        << ",\"y\":" << result.sourceY << ",\"z\":" << result.sourceZ << "}"
        << ",\"destination\":{\"x\":" << result.destinationX
        << ",\"y\":" << result.destinationY << ",\"z\":" << result.destinationZ << "}"
        << ",\"endpoint_distance\":";
    appendNumber(result.endpointDistance);
    out << ",\"path_length\":";
    appendNumber(result.pathLength);
    out << ",\"max_segment_vertical_delta\":";
    appendNumber(result.maxSegmentVerticalDelta);
    out << ",\"excursion_below_endpoints\":";
    appendNumber(result.excursionBelowEndpoints);
    out << ",\"ground_samples_valid\":" << (result.allGroundSamplesValid ? "true" : "false")
        << ",\"max_ground_delta\":";
    appendNumber(result.maxGroundDelta);

    if (includePoints)
    {
        out << ",\"points\":[";
        for (std::size_t index = 0; index < result.path.size(); ++index)
        {
            if (index)
                out << ',';
            out << "{\"x\":" << result.path[index].x << ",\"y\":" << result.path[index].y
                << ",\"z\":" << result.path[index].z << "}";
        }
        out << ']';
    }
    out << '}';
    return out.str();
}
}
