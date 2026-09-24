/*
 * Multi-floor travel for the Walking V2 mover (AutoWow.Travel.VerticalSnap, default off; needs
 * AutoWow.TravelIntent.Enable and AutoWow.Walking.V2).
 *
 * Root cause (soak-s17-full-r1: 136 intent_replan_exhausted blocks -> 160 of 178 deferrals, and the
 * flight-master give-ups, all at segments=0): the mover's PathGenerator ran with SetSlopeCheck(true).
 * FindSmoothPath then checks each 4 yd step's rise (+0.5 yd lift, against a steer point held at the
 * previous step's height) against the collision height scaled down by the slope angle, and fails the
 * whole path with DT_SLOPE_TOO_STEEP at the first step that exceeds it - so a bot at the foot of a hill,
 * ramp or stair outside the 70 yd direct-MoveTo radius gets a 0-1 point path (NOPATH / no progress) for
 * every candidate, 5 replans in a row, forever. The navmesh itself connects these goals: the offline
 * replay over the extracted mmaps (tests/offline/VerticalSnapOfflineCheck.cpp, the 31 S17 give-up spots
 * with >= 15 repeats) reproduces the freeze with the slope check on, and with this flag on reaches the
 * Thelsamar, Tranquillien, Sepulcher, Thunder Bluff, Booty Bay and Ironforge flight masters and the
 * Crossroads / Barrens / Ratchet quest goals. Each goal poly was found within 1.2 yd, so goal z was not
 * the fault; the mmaps were built with a 60 degree walkable slope, and a bot moves by server spline, so a
 * navmesh-walkable slope is walkable for it. Not fixed here: Silvermoon's elevated approach, whose only
 * route to Skymistress Gloaming exceeds the 74-poly corridor and first leads away from her (goal-monotone
 * admission refuses it) - AutoWow.Travel.FlightTrapFix bounds that one.
 *
 * With the flag on the V2 mover (a) computes its paths without the slope check and (b) snaps the goal to
 * the nearest navmesh point within kSnapRadiusYards horizontally and kSnapVerticalYards vertically
 * (Detour picks the nearest in 3D, i.e. the goal's own floor when one is in reach), so an off-mesh spawn
 * point (counter, prop, ledge) is approached to within interaction range instead of being unreachable.
 */
#ifndef PLAYERBOTS_NAVMESH_SNAP_H
#define PLAYERBOTS_NAVMESH_SNAP_H

#include "DetourNavMeshQuery.h"

#include <cstdint>

namespace AutoWowVerticalSnap
{
inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr float kSnapRadiusYards = 4.0f;     // < INTERACTION_DISTANCE (5.5): a snapped goal stays in reach
inline constexpr float kSnapVerticalYards = 30.0f;  // one or two floors of a tower / inn
// PathGenerator::CreateFilter for a Player: NAV_GROUND | NAV_MAGMA | NAV_WATER.
inline constexpr unsigned short kIncludeFlags = 0x01 | 0x02 | 0x08;

// Nearest navmesh point to (x, y, z) inside the snap box; false when the box holds no walkable poly
// (tile not loaded, no mmap). Detour coordinates are (y, z, x).
inline bool Snap(dtNavMeshQuery const& query, float x, float y, float z, float& outX, float& outY, float& outZ)
{
    dtQueryFilter filter;
    filter.setIncludeFlags(kIncludeFlags);
    filter.setExcludeFlags(0);
    float const center[3] = {y, z, x};
    float const extents[3] = {kSnapRadiusYards, kSnapVerticalYards, kSnapRadiusYards};
    dtPolyRef ref = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query.findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
        return false;
    outX = nearest[2];
    outY = nearest[0];
    outZ = nearest[1];
    return true;
}

namespace detail
{
inline bool gEnabled = false;  // AutoWow.Travel.VerticalSnap
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
}  // namespace AutoWowVerticalSnap

#endif  // PLAYERBOTS_NAVMESH_SNAP_H
