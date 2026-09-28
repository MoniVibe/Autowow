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
// Offline Detour replay over the p1data mmaps: every consecutive walk pair is a complete path without the slope
// check and no point below the pair's lower endpoint by more than 1 yd; only the Wailing Caverns lower-cave
// pairs (leg 7) touch water polys, the shallow floor pool the earlier encounters already cross.
// A direct point is an unmeshed step or drop the navmesh does not connect (Wailing Caverns: the Serpentis /
// Verdan ledge is a 126-poly island). Each mover (leader or follower) reaches it by a straight MoveTo without
// pathfinding, only from within DirectStartRadius of the previous point and only for a step of at most
// DirectMaximumHorizontal across, DirectMaximumDrop down and DirectMaximumRise up (DirectStepShape).
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
    bool direct = false;  // reached from the previous point by a straight unpathed move
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
    // Wailing Caverns, Lord Serpentis (idx5, spawn 38148): from Skum north through the western tunnels to the
    // lip above the ledge trench, the 5.5 yd / 5.9 yd down step onto the Serpentis / Verdan island (S60: no
    // travel-node or navmesh link, blocked=unsupported_transition), then up the ledge to Serpentis.
    {43, 5, 0, -285.58f, -312.97f, -69.19f},
    {43, 5, 1, -269.9f, -300.3f, -67.96f},
    {43, 5, 2, -272.0f, -271.2f, -62.33f},
    {43, 5, 3, -274.1f, -242.1f, -60.49f},
    {43, 5, 4, -274.0f, -221.7f, -63.29f},
    {43, 5, 5, -273.9f, -201.3f, -60.23f},
    {43, 5, 6, -287.6f, -168.5f, -62.59f},
    {43, 5, 7, -301.3f, -135.7f, -59.69f},
    {43, 5, 8, -305.1f, -123.2f, -61.83f},
    {43, 5, 9, -309.2f, -95.75f, -64.17f},
    {43, 5, 10, -313.3f, -68.3f, -63.43f},
    {43, 5, 11, -311.7f, -57.6f, -62.09f},
    {43, 5, 12, -301.65f, -29.5f, -60.6f},
    {43, 5, 13, -291.6f, -1.4f, -58.05f},
    {43, 5, 14, -289.1f, 3.5f, -63.96f, true},
    {43, 5, 15, -275.2f, 23.1f, -60.27f},
    {43, 5, 16, -261.3f, 42.7f, -53.29f},
    {43, 5, 17, -256.0f, 47.5f, -52.23f},
    {43, 5, 18, -232.5f, 55.5f, -49.03f},
    {43, 5, 19, -201.6f, 53.6f, -48.78f},
    {43, 5, 20, -170.7f, 51.7f, -41.03f},
    {43, 5, 21, -161.3f, 42.7f, -36.76f},
    {43, 5, 22, -147.59f, 20.26f, -28.31f},
    {43, 5, 23, -133.87f, -2.18f, -28.08f},
    {43, 5, 24, -120.16f, -24.62f, -28.58f},
    // Wailing Caverns, Verdan the Everliving (idx6, spawn 33974): along the ledge top.
    {43, 6, 0, -102.73f, -1.66f, -29.46f},
    {43, 6, 1, -85.3f, 21.3f, -30.89f},
    {43, 6, 2, -81.86f, 32.26f, -30.99f},
    // Wailing Caverns, Disciple of Naralex (idx7 gate rows, spawn 18675): back down the ledge (its island points
    // repeat leg 5's; NearestPoint resumes on this leg), the 28 yd drop off the west lip, then the lower cave
    // floor round to the entrance. The ledge island joins the Disciple's floor only through the lower cave.
    {43, 7, 0, -108.53f, 29.98f, -30.69f},
    {43, 7, 1, -135.2f, 27.7f, -27.96f},
    {43, 7, 2, -140.8f, 31.2f, -27.96f},
    {43, 7, 3, -155.75f, 41.45f, -34.34f},
    {43, 7, 4, -170.7f, 51.7f, -41.03f},
    {43, 7, 5, -201.6f, 53.6f, -48.78f},
    {43, 7, 6, -232.5f, 55.5f, -49.03f},
    {43, 7, 7, -256.0f, 47.5f, -52.23f},
    {43, 7, 8, -261.3f, 42.7f, -53.29f},
    {43, 7, 9, -278.8f, 23.0f, -60.7f},
    {43, 7, 10, -280.7f, 22.4f, -88.7f, true},
    {43, 7, 11, -282.1f, 24.5f, -88.23f},
    {43, 7, 12, -298.7f, 42.7f, -91.96f},
    {43, 7, 13, -310.4f, 44.5f, -93.29f},
    {43, 7, 14, -320.8f, 42.7f, -96.23f},
    {43, 7, 15, -325.1f, 37.9f, -99.16f},
    {43, 7, 16, -322.1f, 30.9f, -100.23f},
    {43, 7, 17, -295.7f, 16.5f, -104.49f},
    {43, 7, 18, -287.7f, 12.5f, -105.29f},
    {43, 7, 19, -273.3f, 12.8f, -105.56f},
    {43, 7, 20, -256.0f, 18.7f, -104.76f},
    {43, 7, 21, -236.3f, 13.3f, -104.23f},
    {43, 7, 22, -221.3f, 18.1f, -103.96f},
    {43, 7, 23, -213.3f, 21.3f, -105.56f},
    {43, 7, 24, -192.0f, 42.7f, -105.83f},
    {43, 7, 25, -174.1f, 53.6f, -103.16f},
    {43, 7, 26, -157.3f, 64.0f, -105.29f},
    {43, 7, 27, -137.9f, 65.6f, -105.83f},
    {43, 7, 28, -124.8f, 66.1f, -103.96f},
    {43, 7, 29, -112.8f, 64.0f, -105.03f},
    {43, 7, 30, -87.7f, 69.6f, -105.56f},
    {43, 7, 31, -76.5f, 79.5f, -104.76f},
    {43, 7, 32, -71.2f, 85.3f, -104.76f},
    {43, 7, 33, -64.0f, 95.2f, -105.03f},
    {43, 7, 34, -52.8f, 106.7f, -105.29f},
    {43, 7, 35, -45.3f, 120.5f, -105.29f},
    {43, 7, 36, -38.9f, 128.0f, -105.29f},
    {43, 7, 37, -29.3f, 144.5f, -104.49f},
    {43, 7, 38, -24.5f, 165.9f, -104.23f},
    {43, 7, 39, -25.6f, 190.4f, -101.83f},
    {43, 7, 40, -28.3f, 196.8f, -97.83f},
    {43, 7, 41, -45.35f, 202.95f, -95.93f},
    {43, 7, 42, -62.4f, 209.1f, -93.29f},
    {43, 7, 43, -87.2f, 225.6f, -92.23f},
    {43, 7, 44, -96.8f, 226.9f, -90.09f},
    {43, 7, 45, -102.1f, 225.3f, -88.49f},
    {43, 7, 46, -103.7f, 217.1f, -82.36f},
    {43, 7, 47, -105.2f, 183.2f, -78.79f},
    {43, 7, 48, -106.7f, 149.3f, -80.49f},
    {43, 7, 49, -128.0f, 128.0f, -78.63f},
    {43, 7, 50, -134.965f, 125.402f, -78.0945f},
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

// Position in rows of the point nearest (x, y, z); ties keep the later point, so a point a later leg repeats
// resumes on that leg. NoPoint for no rows.
inline std::size_t NearestPoint(std::vector<std::size_t> const& rows, float x, float y, float z)
{
    std::size_t best = NoPoint;
    float bestDistance = 0.0f;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        float const distance = Distance(Points[rows[index]], x, y, z);
        if (best == NoPoint || distance <= bestDistance)
        {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

// A route serves a navigator goal (boss spawn or gate row position) only when its last point is that goal, so
// an encounter's gate rows elsewhere (the Deadmines Gunpowder chest and cannon) never walk its boss leg.
constexpr float GoalMatchRadius = 1.0f;

inline bool EndsAt(std::vector<std::size_t> const& rows, float x, float y, float z)
{
    return !rows.empty() && Distance(Points[rows.back()], x, y, z) <= GoalMatchRadius;
}

inline bool UseRoute(float entryDistance, float finalDistance)
{
    return std::isfinite(entryDistance) && std::isfinite(finalDistance) &&
        entryDistance <= EntryRadius && finalDistance > HandoffRadius;
}

// The leader walks a curated route one point at a time: only the consecutive pairs were replayed, a
// farther point's shortest path may leave the walkway (the cove swim).
constexpr std::uint32_t LookaheadPointLimit = 1;

// Direct points: authored step bounds, the mover's start radius around the previous point, and the time it
// has to arrive within DirectArrivalRadius before the step counts as failed (logged once, never retried by
// that mover in that instance; the leader's curated route is then blocked and the ordinary handling resumes).
constexpr float DirectStartRadius = 8.0f;
constexpr float DirectMaximumHorizontal = 8.0f;
constexpr float DirectMaximumDrop = 30.0f;
constexpr float DirectMaximumRise = 6.0f;
constexpr float DirectArrivalRadius = 3.0f;
constexpr std::uint32_t DirectTimeoutMs = 10000;

inline bool DirectStepShape(Point const& from, Point const& to)
{
    float const dx = to.x - from.x;
    float const dy = to.y - from.y;
    float const dz = to.z - from.z;
    return from.mapId == to.mapId && std::sqrt(dx * dx + dy * dy) <= DirectMaximumHorizontal &&
        dz >= -DirectMaximumDrop && dz <= DirectMaximumRise;
}

// The mover stands within DirectStartRadius of the step's previous point and nearer it than the step
// point (a mover already at the bottom of a short step is never sent again).
inline bool CanDirectStep(float fromDistance, float toDistance, Point const& from, Point const& to)
{
    return to.direct && std::isfinite(fromDistance) && std::isfinite(toDistance) &&
        fromDistance <= DirectStartRadius && fromDistance < toDistance && DirectStepShape(from, to);
}

enum class DirectWait : std::uint8_t
{
    Arrived,  // within DirectArrivalRadius of the step point
    Pending,  // still inside DirectTimeoutMs
    Failed,   // timed out short of the point
};

inline DirectWait EvaluateDirectStep(float toDistance, std::uint32_t elapsedMs)
{
    if (std::isfinite(toDistance) && toDistance <= DirectArrivalRadius)
        return DirectWait::Arrived;
    return elapsedMs < DirectTimeoutMs ? DirectWait::Pending : DirectWait::Failed;
}

// First route position a route entered at rows[entry] may start from: a direct point is entered from its
// previous point, which the step's start radius is measured against.
inline std::size_t EntryStart(std::vector<std::size_t> const& rows, std::size_t entry)
{
    return entry != NoPoint && entry && entry < rows.size() && Points[rows[entry]].direct ? entry - 1 : entry;
}

// Route rows [begin, end) contain a direct point in failed.
template <typename Failed>
bool RouteHasFailedStep(std::vector<std::size_t> const& rows, std::size_t begin, Failed&& failed)
{
    for (std::size_t index = begin; index < rows.size(); ++index)
        if (Points[rows[index]].direct && failed(rows[index]))
            return true;
    return false;
}
}

#endif
