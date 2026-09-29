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
// DirectMaximumHorizontal across, DirectMaximumDrop down and DirectMaximumRise up, or a level crossing (a swim
// across a water surface the navmesh leaves unlinked) of at most DirectMaximumLevelCrossing (DirectStepShape).
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
    // Blackfathom Deeps, Lady Sarevess (idx1, spawn 26129): from Ghamoo-ra (spawn 25732) north-east down into
    // the pool, the 15.8 yd swim across its surface (z -55.96) where the navmesh water polys are unlinked islands
    // (S62: both probe parties blocked=unsupported_transition after Ghamoo-ra; the main mesh region and
    // Sarevess's 88-poly region share no link), then up the far shore to Sarevess.
    {48, 1, 0, -442.424f, 211.822f, -52.637f},
    {48, 1, 1, -428.0f, 225.7f, -52.57f},
    {48, 1, 2, -413.6f, 239.6f, -54.13f},
    {48, 1, 3, -399.2f, 253.4f, -55.38f},
    {48, 1, 4, -384.8f, 267.3f, -55.46f},
    {48, 1, 5, -366.9f, 284.5f, -55.96f},
    {48, 1, 6, -355.5f, 295.5f, -55.96f, true},
    {48, 1, 7, -351.1f, 323.2f, -54.77f},
    {48, 1, 8, -347.3f, 346.9f, -52.7f},
    {48, 1, 9, -344.1f, 366.6f, -53.15f},
    {48, 1, 10, -341.6f, 382.4f, -53.24f},
    {48, 1, 11, -330.7f, 392.6f, -53.33f},
    {48, 1, 12, -317.5f, 401.6f, -54.21f},
    {48, 1, 13, -307.6f, 408.5f, -55.82f},
    {48, 1, 14, -299.917f, 413.755f, -57.123f},
    // Sunken Temple (109): the Atal'ai statues and the balcony trolls are gate rows walked in order on one leg each
    // (RouteTo cuts a leg at its goal). S69: both probe parties stopped at the travel-node end on the entrance
    // ledge (-371,54, z -129) logging blocked=unsupported_transition, 20 yd above the statue ring; the ring, pit and
    // balconies are one navmesh region, but every goal-to-goal path is steep for the slope check or past
    // PathGenerator's 74 points. Detour ground (pit: ground|water) corridor corners, thinned to the farthest pair
    // that replays complete without the slope check and dips at most 1 yd. Walk order: statues 148830..148835 and
    // Atal'alarion (idx0, leg 0); the six defenders then Jammal'an (idx3, leg 3); after Jammal'an (the dragons
    // and Eranikus wait for his death, gate prerequisite rows) Dreamscythe (1), Weaver (2), Morphaz (5), Hazzas (6),
    // Shade of Eranikus (8). Each leg starts at the previous goal of that walk.
    {109, 0, 0, -319.2f, 99.9f, -131.9f},  // entrance
    {109, 0, 1, -349.5f, 89.7f, -130.94f},
    {109, 0, 2, -370.9f, 70.7f, -130.8f},
    {109, 0, 3, -369.8f, 38.7f, -137.57f},
    {109, 0, 4, -405.3f, 21.3f, -148.66f},
    {109, 0, 5, -423.7f, 14.7f, -148.66f},
    {109, 0, 6, -465.6f, 3.7f, -147.33f},
    {109, 0, 7, -496.6f, 10.6f, -148.11f},
    {109, 0, 8, -516.0f, 17.1f, -148.66f},
    {109, 0, 9, -546.7f, 47.5f, -148.66f},
    {109, 0, 10, -554.7f, 74.1f, -148.66f},
    {109, 0, 11, -515.553f, 95.2582f, -148.74f},  // statue 148830
    {109, 0, 12, -554.1f, 116.3f, -147.33f},
    {109, 0, 13, -545.9f, 143.7f, -148.66f},
    {109, 0, 14, -516.0f, 173.6f, -148.66f},
    {109, 0, 15, -481.0f, 184.2f, -147.99f},
    {109, 0, 16, -438.4f, 180.5f, -147.46f},
    {109, 0, 17, -405.9f, 160.3f, -148.66f},
    {109, 0, 18, -384.0f, 128.0f, -148.66f},
    {109, 0, 19, -384.0f, 85.3f, -148.4f},
    {109, 0, 20, -419.849f, 94.4837f, -148.74f},  // statue 148831
    {109, 0, 21, -380.8f, 116.5f, -148.66f},
    {109, 0, 22, -389.1f, 143.5f, -148.66f},
    {109, 0, 23, -419.5f, 173.9f, -148.66f},
    {109, 0, 24, -454.1f, 184.6f, -148.16f},
    {109, 0, 25, -493.3f, 180.8f, -148.66f},
    {109, 0, 26, -491.4f, 135.97f, -148.74f},  // statue 148832
    {109, 0, 27, -505.1f, 176.0f, -148.66f},
    {109, 0, 28, -538.6f, 151.0f, -148.16f},
    {109, 0, 29, -548.5f, 139.5f, -148.66f},
    {109, 0, 30, -559.5f, 97.6f, -147.33f},
    {109, 0, 31, -554.7f, 74.1f, -148.66f},
    {109, 0, 32, -546.7f, 47.5f, -148.66f},
    {109, 0, 33, -520.5f, 44.3f, -148.66f},
    {109, 0, 34, -491.491f, 53.4818f, -148.74f},  // statue 148833
    {109, 0, 35, -529.6f, 31.2f, -147.33f},
    {109, 0, 36, -548.5f, 51.5f, -148.66f},
    {109, 0, 37, -559.5f, 93.3f, -147.33f},
    {109, 0, 38, -551.4f, 128.6f, -148.16f},
    {109, 0, 39, -545.9f, 143.7f, -148.66f},
    {109, 0, 40, -516.0f, 173.6f, -148.66f},
    {109, 0, 41, -481.0f, 184.2f, -147.99f},
    {109, 0, 42, -438.4f, 180.5f, -147.46f},
    {109, 0, 43, -443.855f, 136.101f, -148.74f},  // statue 148834
    {109, 0, 44, -412.3f, 164.3f, -148.66f},
    {109, 0, 45, -386.9f, 139.5f, -148.66f},
    {109, 0, 46, -376.0f, 97.6f, -147.33f},
    {109, 0, 47, -384.0f, 64.0f, -148.66f},
    {109, 0, 48, -388.8f, 47.5f, -148.66f},
    {109, 0, 49, -419.7f, 16.8f, -148.66f},
    {109, 0, 50, -443.417f, 53.8312f, -148.74f},  // statue 148835
    {109, 0, 51, -423.7f, 14.7f, -148.66f},
    {109, 0, 52, -390.9f, 25.6f, -148.66f},
    {109, 0, 53, -370.3f, 52.3f, -128.41f},
    {109, 0, 54, -362.7f, 85.3f, -131.6f},
    {109, 0, 55, -341.3f, 106.7f, -131.6f},
    {109, 0, 56, -329.8f, 136.6f, -130.83f},
    {109, 0, 57, -321.6f, 132.3f, -153.73f},
    {109, 0, 58, -299.7f, 100.8f, -172.93f},
    {109, 0, 59, -315.2f, 100.3f, -172.93f},
    {109, 0, 60, -347.2f, 99.6f, -171.97f},
    {109, 0, 61, -379.2f, 98.9f, -172.43f},
    {109, 0, 62, -411.2f, 98.1f, -172.34f},
    {109, 0, 63, -439.2f, 97.5f, -183.44f},
    {109, 0, 64, -480.4f, 96.5663f, -189.73f},  // Atal'alarion 34521
    {109, 1, 0, -425.894f, -86.0747f, -88.224f},  // Jammal'an 39737
    {109, 1, 1, -457.9f, -85.5f, -90.3f},
    {109, 1, 2, -489.3f, -80.6f, -90.3f},
    {109, 1, 3, -520.4f, -73.0f, -90.3f},
    {109, 1, 4, -527.3f, -29.1f, -90.3f},
    {109, 1, 5, -514.8f, 0.4f, -90.3f},
    {109, 1, 6, -502.2f, 29.8f, -90.4f},
    {109, 1, 7, -489.6f, 59.2f, -89.97f},
    {109, 1, 8, -477.0f, 88.6f, -93.81f},
    {109, 1, 9, -469.3f, 106.7f, -94.53f},
    {109, 1, 10, -453.45f, 137.17f, -90.75f},  // Dreamscythe 239020
    {109, 2, 0, -453.45f, 137.17f, -90.75f},  // Dreamscythe 239020
    {109, 2, 1, -458.84f, 127.7f, -91.57f},  // Weaver 239021
    {109, 3, 0, -480.4f, 96.5663f, -189.73f},  // Atal'alarion 34521
    {109, 3, 1, -448.4f, 97.3f, -188.16f},
    {109, 3, 2, -420.4f, 97.9f, -172.99f},
    {109, 3, 3, -388.4f, 98.7f, -172.25f},
    {109, 3, 4, -356.4f, 99.4f, -171.83f},
    {109, 3, 5, -315.2f, 100.3f, -172.93f},
    {109, 3, 6, -297.9f, 120.8f, -172.13f},
    {109, 3, 7, -315.2f, 129.6f, -151.33f},
    {109, 3, 8, -351.2f, 119.7f, -131.6f},
    {109, 3, 9, -381.2f, 141.8f, -131.08f},
    {109, 3, 10, -362.7f, 154.9f, -100.93f},
    {109, 3, 11, -367.7f, 130.9f, -69.46f},
    {109, 3, 12, -406.189f, 131.068f, -66.9138f},  // Mijan 39847
    {109, 3, 13, -367.7f, 130.9f, -69.46f},
    {109, 3, 14, -388.0f, 142.9f, -53.73f},
    {109, 3, 15, -424.8f, 164.3f, -53.73f},
    {109, 3, 16, -467.396f, 165.997f, -66.7027f},  // Zul'Lor 34522
    {109, 3, 17, -424.8f, 164.3f, -53.73f},
    {109, 3, 18, -453.3f, 183.8f, -53.23f},
    {109, 3, 19, -468.8f, 186.7f, -52.66f},
    {109, 3, 20, -500.5f, 182.3f, -53.15f},
    {109, 3, 21, -528.3f, 178.4f, -54.0f},
    {109, 3, 22, -536.0f, 152.0f, -69.2f},
    {109, 3, 23, -528.646f, 130.163f, -66.7533f},  // Zolo 39843
    {109, 3, 24, -547.2f, 162.4f, -69.2f},
    {109, 3, 25, -549.7f, 138.9f, -53.23f},
    {109, 3, 26, -561.1f, 109.1f, -53.23f},
    {109, 3, 27, -570.1f, 82.7f, -53.73f},
    {109, 3, 28, -551.7f, 62.9f, -69.2f},
    {109, 3, 29, -527.969f, 59.4516f, -66.7188f},  // Gasher 39844
    {109, 3, 30, -566.1f, 58.4f, -69.2f},
    {109, 3, 31, -545.1f, 46.7f, -53.73f},
    {109, 3, 32, -508.8f, 25.9f, -53.73f},
    {109, 3, 33, -466.655f, 24.4261f, -66.7908f},  // Loro 39845
    {109, 3, 34, -508.8f, 25.9f, -53.73f},
    {109, 3, 35, -479.0f, 5.9f, -53.04f},
    {109, 3, 36, -464.5f, 3.2f, -52.66f},
    {109, 3, 37, -432.8f, 7.4f, -53.1f},
    {109, 3, 38, -404.0f, 11.2f, -53.73f},
    {109, 3, 39, -397.9f, 38.7f, -69.2f},
    {109, 3, 40, -405.506f, 60.4569f, -67.0678f},  // Hukku 39846
    {109, 3, 41, -386.4f, 28.3f, -69.2f},
    {109, 3, 42, -355.5f, 42.7f, -76.66f},
    {109, 3, 43, -355.5f, 42.7f, -116.13f},
    {109, 3, 44, -328.0f, 42.7f, -130.26f},
    {109, 3, 45, -325.6f, 53.9f, -105.2f},
    {109, 3, 46, -298.7f, 71.5f, -90.53f},
    {109, 3, 47, -336.3f, 91.5f, -90.8f},
    {109, 3, 48, -354.1f, 90.7f, -90.8f},
    {109, 3, 49, -395.2f, 81.6f, -90.8f},
    {109, 3, 50, -405.3f, 64.0f, -90.8f},
    {109, 3, 51, -426.7f, 42.7f, -90.8f},
    {109, 3, 52, -457.7f, 34.7f, -90.56f},
    {109, 3, 53, -490.7f, 26.1f, -90.8f},
    {109, 3, 54, -512.0f, 0.0f, -90.8f},
    {109, 3, 55, -526.3f, -28.6f, -90.3f},
    {109, 3, 56, -533.9f, -69.6f, -90.8f},
    {109, 3, 57, -503.9f, -80.6f, -90.22f},
    {109, 3, 58, -490.7f, -85.3f, -90.8f},
    {109, 3, 59, -458.7f, -85.5f, -90.3f},
    {109, 3, 60, -425.894f, -86.0747f, -88.224f},  // Jammal'an 39737
    {109, 5, 0, -458.84f, 127.7f, -91.57f},  // Weaver 239021
    {109, 5, 1, -490.0f, 120.4f, -90.98f},
    {109, 5, 2, -521.1f, 113.2f, -90.45f},
    {109, 5, 3, -538.7f, 109.1f, -90.8f},
    {109, 5, 4, -570.7f, 108.1f, -90.3f},
    {109, 5, 5, -602.7f, 107.2f, -90.3f},
    {109, 5, 6, -618.7f, 106.7f, -90.8f},
    {109, 5, 7, -650.6f, 104.4f, -90.3f},
    {109, 5, 8, -667.59f, 103.111f, -90.8313f},  // Morphaz 33657
    {109, 6, 0, -667.59f, 103.111f, -90.8313f},  // Morphaz 33657
    {109, 6, 1, -667.359f, 80.803f, -90.8326f},  // Hazzas 33658
    {109, 8, 0, -667.359f, 80.803f, -90.8326f},  // Hazzas 33658
    {109, 8, 1, -664.9f, 48.9f, -90.3f},
    {109, 8, 2, -662.5f, 17.0f, -90.3f},
    {109, 8, 3, -660.0f, -14.9f, -90.3f},
    {109, 8, 4, -658.379f, -35.7623f, -90.8352f},  // Shade of Eranikus 39842
    // Razorfen Downs, Mordresh Fire Eye (idx1, spawn 87187) from Tuten'kash's gong, then Glutton (idx2, spawn 87242).
    // S69/S70: after Tuten'kash the leader's route to Mordresh was either a short prepared corridor (the runs that
    // completed) or, from a few yards over, a 62-point travel-node route whose first node lies on the lower level
    // (2470,914, z 27; incomplete from the gong ledge): the leader looped on route_index=0 prefixes at (2500,836) and
    // the followers' slot was unreachable (convoy_shared_regroup_terminal). Detour ground corridor, thinned; every
    // pair replays complete without the slope check. Leg 2 ends where leg 3 starts.
    {129, 1, 0, 2552.44f, 856.98f, 51.49f},  // Gong 148917 (Tuten'kash)
    {129, 1, 1, 2522.8f, 844.9f, 48.8f},
    {129, 1, 2, 2492.3f, 817.1f, 45.58f},
    {129, 1, 3, 2496.0f, 789.3f, 39.71f},
    {129, 1, 4, 2508.3f, 759.7f, 40.39f},
    {129, 1, 5, 2515.2f, 726.7f, 41.58f},
    {129, 1, 6, 2502.4f, 697.1f, 51.98f},
    {129, 1, 7, 2471.5f, 680.5f, 59.98f},
    {129, 1, 8, 2466.62f, 671.44f, 63.47f},  // Mordresh Fire Eye 87187
    {129, 2, 0, 2466.62f, 671.44f, 63.47f},  // Mordresh Fire Eye 87187
    {129, 2, 1, 2471.5f, 680.5f, 59.98f},
    {129, 2, 2, 2502.4f, 697.1f, 51.98f},
    {129, 2, 3, 2517.3f, 736.3f, 41.31f},
    {129, 2, 4, 2487.5f, 749.3f, 45.04f},
    {129, 2, 5, 2457.3f, 781.3f, 46.11f},
    {129, 2, 6, 2447.7f, 824.0f, 44.51f},
    {129, 2, 7, 2451.2f, 855.8f, 39.68f},
    {129, 2, 8, 2460.8f, 886.1f, 30.11f},
    {129, 2, 9, 2464.5f, 903.5f, 27.98f},
    {129, 2, 10, 2467.3f, 935.4f, 24.91f},
    {129, 2, 11, 2469.1f, 956.0f, 25.58f},
    {129, 2, 12, 2468.8f, 988.0f, 25.21f},
    {129, 2, 13, 2468.7f, 1006.8f, 23.8f},  // Glutton 87242, leg 3 starts here
    // Razorfen Downs, Amnennar the Coldbringer (idx3, spawn 87209): from Glutton's spawn (8567) south, up onto
    // the bramble spiral and round it to Amnennar's top (z 55). S68: both probe parties stalled at the travel-node
    // end below it (2364,904, z 29) with blocked=unsupported_transition; the full path is a 95-poly corridor whose
    // smoothed point path runs past PathGenerator's 74-point limit. Corners of the Detour ground corridor.
    {129, 3, 0, 2468.7f, 1006.8f, 23.8f},
    {129, 3, 1, 2468.9f, 981.4f, 24.85f},
    {129, 3, 2, 2469.1f, 956.0f, 25.58f},
    {129, 3, 3, 2463.2f, 937.6f, 25.84f},
    {129, 3, 4, 2457.6f, 929.9f, 30.64f},
    {129, 3, 5, 2452.5f, 933.3f, 35.18f},
    {129, 3, 6, 2447.5f, 950.7f, 35.98f},
    {129, 3, 7, 2446.9f, 991.5f, 37.84f},
    {129, 3, 8, 2436.5f, 1007.7f, 37.84f},
    {129, 3, 9, 2427.5f, 1012.5f, 37.31f},
    {129, 3, 10, 2377.9f, 1008.0f, 39.98f},
    {129, 3, 11, 2364.0f, 996.0f, 39.71f},
    {129, 3, 12, 2353.3f, 974.1f, 39.98f},
    {129, 3, 13, 2346.7f, 960.0f, 43.44f},
    {129, 3, 14, 2370.9f, 941.3f, 42.38f},
    {129, 3, 15, 2389.3f, 938.7f, 43.71f},
    {129, 3, 16, 2412.0f, 934.7f, 44.78f},
    {129, 3, 17, 2421.9f, 950.7f, 46.11f},
    {129, 3, 18, 2423.7f, 971.7f, 48.78f},
    {129, 3, 19, 2422.1f, 989.9f, 50.38f},
    {129, 3, 20, 2396.5f, 989.6f, 53.31f},
    {129, 3, 21, 2392.8f, 988.0f, 54.64f},
    {129, 3, 22, 2403.37f, 960.93f, 55.1437f},
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

// Route to a goal on (mapId, encounterIdx)'s own leg: RouteFor cut after the leg's first point at the goal, so one leg
// serves the encounter's gate rows walked in order (Sunken Temple statues, balcony trolls) and the boss at its end.
// Empty when the goal is not a point of that leg. A goal at a leg's end gives exactly RouteFor.
inline std::vector<std::size_t> RouteTo(std::uint32_t mapId, std::uint32_t encounterIdx, float x, float y, float z)
{
    std::vector<std::size_t> rows = RouteFor(mapId, encounterIdx);
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        if (Points[rows[index]].encounterIdx == encounterIdx && Distance(Points[rows[index]], x, y, z) <= GoalMatchRadius)
        {
            rows.resize(index + 1);
            return rows;
        }
    }
    rows.clear();
    return rows;
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
constexpr float DirectMaximumLevelCrossing = 18.0f;
constexpr float DirectLevelTolerance = 0.5f;
constexpr float DirectArrivalRadius = 3.0f;
constexpr std::uint32_t DirectTimeoutMs = 10000;

// A level crossing (a swim across an unlinked water surface) is moved on a raw straight spline at the mover's own
// height: MovePoint without path generation still runs MoveSplineInit::MoveTo(generatePath = true), and across
// unlinked navmesh that path is the incomplete stub at the start (S64 Blackfathom: the leader never left point 5,
// direct step failed after DirectTimeoutMs). Ledge steps and drops keep MovePoint.
inline bool IsLevelCrossing(Point const& from, Point const& to)
{
    float const dx = to.x - from.x;
    float const dy = to.y - from.y;
    return from.mapId == to.mapId && std::fabs(to.z - from.z) <= DirectLevelTolerance &&
        std::sqrt(dx * dx + dy * dy) <= DirectMaximumLevelCrossing;
}

inline bool DirectStepShape(Point const& from, Point const& to)
{
    float const dx = to.x - from.x;
    float const dy = to.y - from.y;
    float const dz = to.z - from.z;
    if (from.mapId != to.mapId)
        return false;
    if (IsLevelCrossing(from, to))
        return true;
    return std::sqrt(dx * dx + dy * dy) <= DirectMaximumHorizontal && dz >= -DirectMaximumDrop &&
        dz <= DirectMaximumRise;
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
