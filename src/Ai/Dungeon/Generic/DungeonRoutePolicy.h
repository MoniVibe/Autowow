/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONROUTEPOLICY_H
#define PLAYERBOTS_DUNGEONROUTEPOLICY_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

// Curated dungeon legs (AutoWow.DungeonNav.ConvoyV2): the walk players take where the navmesh shortest path
// does not (Deadmines: S59 probe parties stalled behind the Iron Clad Door, the only corridor the navigator
// proposed swam the cove and path safety rejected it, path_dips_below_corridor). One compiled-in row per
// point; a leg is the rows of one (mapId, encounterIdx) and ends at that encounter's boss spawn. A leg starts
// where the previous leg of the map ends, so the route to an encounter walks the map's legs up to it.
// Offline Detour replay over the p1data mmaps: every consecutive pair is a complete path without the slope
// check, with no water poly on it and no point below the leg's lower endpoint by more than 1 yd.
namespace DungeonRoute
{
struct Point
{
    std::uint32_t mapId;
    std::uint32_t encounterIdx;  // leg id: the encounter this leg walks to
    std::uint32_t pointOrder;
    float x;
    float y;
    float z;
};

// Rows sorted by (mapId, encounterIdx, pointOrder), pointOrder 0.. per leg; DungeonRoutePolicyTest enforces it.
inline constexpr Point Points[] = {
    // Deadmines, Mr. Smite (idx3, spawn 79337): cannon side of the Iron Clad Door, through it, along the dock
    // walkway above the cove (z 8.4-9.2), up the gangplank from Smite's chest landing to the bow deck.
    {36, 3, 0, -107.2f, -662.8f, 7.3f},
    {36, 3, 1, -92.7f, -678.4f, 7.42f},
    {36, 3, 2, -88.3f, -721.3f, 8.67f},
    {36, 3, 3, -85.3f, -725.3f, 8.94f},
    {36, 3, 4, -71.7f, -727.2f, 8.67f},
    {36, 3, 5, -37.3f, -731.2f, 9.2f},
    {36, 3, 6, -28.8f, -733.1f, 8.4f},
    {36, 3, 7, -10.4f, -754.7f, 9.2f},
    {36, 3, 8, -4.5f, -781.1f, 10.0f},
    {36, 3, 9, -22.8471f, -797.283f, 20.3745f},
    // Deadmines, Cookie (idx4, spawn 79344): the ship's lower deck from the bow round to the galley.
    {36, 4, 0, -21.3f, -810.7f, 19.6f},
    {36, 4, 1, -18.9f, -815.2f, 21.47f},
    {36, 4, 2, -18.9f, -820.5f, 19.87f},
    {36, 4, 3, -25.9f, -833.9f, 19.6f},
    {36, 4, 4, -33.1f, -842.1f, 19.07f},
    {36, 4, 5, -39.2f, -847.2f, 18.8f},
    {36, 4, 6, -50.1f, -852.5f, 18.54f},
    {36, 4, 7, -59.5f, -853.9f, 17.47f},
    {36, 4, 8, -67.5844f, -853.749f, 17.075f},
    // Deadmines, Captain Greenskin (idx5, spawn 79333): lower deck to the stern, up the stairs through the
    // middle deck (z 22-32) to the main deck (z 38-43).
    {36, 5, 0, -85.3f, -853.3f, 17.47f},
    {36, 5, 1, -128.0f, -832.0f, 16.94f},
    {36, 5, 2, -128.5f, -813.3f, 17.2f},
    {36, 5, 3, -122.9f, -811.7f, 18.54f},
    {36, 5, 4, -119.5f, -808.0f, 18.54f},
    {36, 5, 5, -107.5f, -787.5f, 18.8f},
    {36, 5, 6, -102.4f, -783.2f, 22.0f},
    {36, 5, 7, -96.0f, -781.1f, 22.27f},
    {36, 5, 8, -90.4f, -779.7f, 25.47f},
    {36, 5, 9, -88.8f, -781.9f, 27.07f},
    {36, 5, 10, -100.5f, -794.4f, 28.14f},
    {36, 5, 11, -96.8f, -800.3f, 32.14f},
    {36, 5, 12, -64.0f, -789.3f, 39.6f},
    {36, 5, 13, -53.1f, -794.1f, 38.54f},
    {36, 5, 14, -45.1f, -797.9f, 39.34f},
    {36, 5, 15, -48.5f, -804.0f, 42.8f},
    {36, 5, 16, -59.62f, -820.132f, 41.6134f},
    // Deadmines, Edwin VanCleef (idx6, spawn 79336): across the main deck to the stern.
    {36, 6, 0, -87.369f, -819.895f, 39.3004f},
};

constexpr std::size_t NoPoint = std::numeric_limits<std::size_t>::max();
// A curated route is taken only from within EntryRadius of one of its points (farther away the ordinary
// travel-node route brings the party in) and is handed back to the ordinary direct approach inside
// HandoffRadius of the boss spawn (the leg's last point), so an exhausted route never rebuilds itself.
constexpr float EntryRadius = 40.0f;
constexpr float HandoffRadius = 8.0f;

// Table indices of the route to (mapId, encounterIdx): every leg of the map up to and including the
// encounter's, in table order. Empty when the encounter has no leg.
inline std::vector<std::size_t> RouteFor(std::uint32_t mapId, std::uint32_t encounterIdx)
{
    std::vector<std::size_t> rows;
    bool hasLeg = false;
    for (std::size_t index = 0; index < std::size(Points); ++index)
    {
        if (Points[index].mapId != mapId || Points[index].encounterIdx > encounterIdx)
            continue;
        rows.push_back(index);
        hasLeg = hasLeg || Points[index].encounterIdx == encounterIdx;
    }
    if (!hasLeg)
        rows.clear();
    return rows;
}

inline float Distance(Point const& point, float x, float y, float z)
{
    float const dx = point.x - x;
    float const dy = point.y - y;
    float const dz = point.z - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Position in rows of the point nearest (x, y, z); ties keep the earlier point. NoPoint for no rows.
inline std::size_t NearestPoint(std::vector<std::size_t> const& rows, float x, float y, float z)
{
    std::size_t best = NoPoint;
    float bestDistance = 0.0f;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        float const distance = Distance(Points[rows[index]], x, y, z);
        if (best == NoPoint || distance < bestDistance)
        {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

inline bool UseRoute(float entryDistance, float finalDistance)
{
    return std::isfinite(entryDistance) && std::isfinite(finalDistance) &&
        entryDistance <= EntryRadius && finalDistance > HandoffRadius;
}

// The leader walks a curated route one point at a time: only the consecutive pairs were replayed, a
// farther point's shortest path may leave the walkway (the cove swim).
constexpr std::uint32_t LookaheadPointLimit = 1;
}

#endif
