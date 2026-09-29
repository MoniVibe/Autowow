/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonRoutePolicy.h"

#include <cmath>
#include <iterator>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"

using namespace DungeonRoute;

namespace
{
// Longest replayed consecutive pair (Deadmines cove dock, leg 3 point 1 -> 2) with margin.
constexpr float MaximumPointSpacing = 50.0f;
}

TEST(DungeonRoutePolicy, TableIsOrderedContiguousAndFinite)
{
    for (std::size_t i = 0; i < std::size(Points); ++i)
    {
        Point const& point = Points[i];
        EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) << i;
        if (!i || Points[i - 1].mapId != point.mapId || Points[i - 1].encounterIdx != point.encounterIdx)
        {
            EXPECT_EQ(point.pointOrder, 0u) << i;  // every leg starts at point 0
        }
        else
        {
            EXPECT_EQ(point.pointOrder, Points[i - 1].pointOrder + 1) << i;
        }
        if (i)
        {
            Point const& previous = Points[i - 1];
            EXPECT_LT(std::tie(previous.mapId, previous.encounterIdx, previous.pointOrder),
                std::tie(point.mapId, point.encounterIdx, point.pointOrder)) << i;
            if (previous.mapId == point.mapId && previous.encounterIdx == point.encounterIdx)
            {
                EXPECT_LE(Distance(previous, point.x, point.y, point.z), MaximumPointSpacing) << i;
            }
            else if (previous.mapId == point.mapId)
            {
                // A leg starts where another leg of the map ends (the walk order need not be the index order:
                // Sunken Temple walks idx3 before idx1).
                bool joined = false;
                for (std::size_t j = 0; j + 1 < std::size(Points); ++j)
                {
                    Point const& end = Points[j];
                    bool const legEnd = Points[j + 1].mapId != end.mapId || Points[j + 1].encounterIdx != end.encounterIdx;
                    if (end.mapId == point.mapId && end.encounterIdx != point.encounterIdx && legEnd &&
                        Distance(end, point.x, point.y, point.z) <= MaximumPointSpacing)
                    {
                        joined = true;
                    }
                }
                EXPECT_TRUE(joined) << i;
            }
        }
        if (point.direct)
        {
            // A direct point has a walk point of the same map before it and fits the step bounds.
            ASSERT_GT(i, 0u);
            EXPECT_EQ(Points[i - 1].mapId, point.mapId) << i;
            EXPECT_FALSE(Points[i - 1].direct) << i;
            EXPECT_TRUE(DirectStepShape(Points[i - 1], point)) << i;
        }
    }
}

TEST(DungeonRoutePolicy, DeadminesLegsEndAtBossSpawns)
{
    // creature spawns 79337 Mr. Smite, 79344 Cookie, 79333 Captain Greenskin, 79336 Edwin VanCleef
    struct Boss { std::uint32_t encounter; float x, y, z; std::size_t points; };
    Boss const bosses[] = {
        {3, -22.8471f, -797.283f, 20.3745f, 10},
        {4, -67.5844f, -853.749f, 17.075f, 9},
        {5, -59.62f, -820.132f, 41.6134f, 17},
        {6, -87.369f, -819.895f, 39.3004f, 1},
    };
    std::size_t total = 0;
    for (Boss const& boss : bosses)
    {
        total += boss.points;
        std::vector<std::size_t> const rows = RouteFor(36, boss.encounter);
        ASSERT_EQ(rows.size(), total) << boss.encounter;
        EXPECT_LT(Distance(Points[rows.back()], boss.x, boss.y, boss.z), 0.01f) << boss.encounter;
        EXPECT_EQ(Points[rows.back()].encounterIdx, boss.encounter);
        // The route is one contiguous table run (the navigator maps route index i to row rows[0] + i).
        for (std::size_t i = 0; i < rows.size(); ++i)
            EXPECT_EQ(rows[i], rows.front() + i) << boss.encounter;
    }
    // The route to Mr. Smite starts on the cannon side of the Iron Clad Door (30534 at -100.5,-668.8).
    std::vector<std::size_t> const smite = RouteFor(36, 3);
    EXPECT_LT(Distance(Points[smite.front()], -100.5f, -668.8f, 7.4f), 10.0f);
}

TEST(DungeonRoutePolicy, EncountersWithoutLegHaveNoRoute)
{
    EXPECT_TRUE(RouteFor(36, 0).empty());
    EXPECT_TRUE(RouteFor(36, 2).empty());
    EXPECT_TRUE(RouteFor(36, 7).empty());
    EXPECT_TRUE(RouteFor(43, 3).empty());
    EXPECT_TRUE(RouteFor(48, 2).empty());
}

TEST(DungeonRoutePolicy, EntersAtNearestPoint)
{
    std::vector<std::size_t> const rows = RouteFor(36, 5);
    // Leader at the Defias Cannon after the door opens: first point.
    EXPECT_EQ(NearestPoint(rows, -107.562f, -659.674f, 7.21211f), 0u);
    // Leader at Mr. Smite after the kill, route to Captain Greenskin: Smite's point, the end of leg 3.
    std::size_t const atSmite = NearestPoint(rows, -22.0f, -797.0f, 20.4f);
    ASSERT_NE(atSmite, NoPoint);
    EXPECT_EQ(Points[rows[atSmite]].encounterIdx, 3u);
    EXPECT_EQ(Points[rows[atSmite]].pointOrder, 9u);
    // Leader at Cookie, route to Captain Greenskin: Cookie's point, then leg 5 from its point 0.
    std::size_t const atCookie = NearestPoint(rows, -67.0f, -853.0f, 17.1f);
    ASSERT_NE(atCookie, NoPoint);
    EXPECT_EQ(Points[rows[atCookie]].encounterIdx, 4u);
    EXPECT_EQ(Points[rows[atCookie + 1]].encounterIdx, 5u);
    EXPECT_EQ(Points[rows[atCookie + 1]].pointOrder, 0u);
    EXPECT_EQ(NearestPoint({}, 0.0f, 0.0f, 0.0f), NoPoint);
}

TEST(DungeonRoutePolicy, NearestPointTieKeepsLaterPoint)
{
    std::vector<std::size_t> const rows = RouteFor(36, 3);
    Point const& a = Points[rows[3]];
    Point const& b = Points[rows[4]];
    float const midX = (a.x + b.x) * 0.5f;
    float const midY = (a.y + b.y) * 0.5f;
    float const midZ = (a.z + b.z) * 0.5f;
    float const da = Distance(a, midX, midY, midZ);
    float const db = Distance(b, midX, midY, midZ);
    std::size_t const nearest = NearestPoint(rows, midX, midY, midZ);
    EXPECT_EQ(nearest, da < db ? 3u : 4u);
    EXPECT_EQ(NearestPoint({rows[3], rows[3]}, a.x, a.y, a.z), 1u);

    // Wailing Caverns: leg 7 repeats leg 5's ledge points on the way back; a leader on one resumes on leg 7.
    std::vector<std::size_t> const wc = RouteFor(43, 7);
    std::size_t const onLedge = NearestPoint(wc, -232.5f, 55.5f, -49.03f);
    ASSERT_NE(onLedge, NoPoint);
    EXPECT_EQ(Points[wc[onLedge]].encounterIdx, 7u);
}

TEST(DungeonRoutePolicy, WailingCavernsLegsEndAtGoals)
{
    // creature spawns 38148 Lord Serpentis, 33974 Verdan the Everliving; idx7 ends at the Disciple of Naralex
    // gossip gate row (spawn 18675).
    struct Goal { std::uint32_t encounter; float x, y, z; std::size_t points; };
    Goal const goals[] = {
        {5, -120.16f, -24.62f, -28.58f, 25},
        {6, -81.86f, 32.26f, -30.99f, 3},
        {7, -134.965f, 125.402f, -78.0945f, 51},
    };
    std::size_t total = 0;
    for (Goal const& goal : goals)
    {
        total += goal.points;
        std::vector<std::size_t> const rows = RouteFor(43, goal.encounter);
        ASSERT_EQ(rows.size(), total) << goal.encounter;
        EXPECT_TRUE(EndsAt(rows, goal.x, goal.y, goal.z)) << goal.encounter;
    }
    // Starts at Skum (spawn 87131), where S60 parties stalled with Serpentis next.
    EXPECT_LT(Distance(Points[RouteFor(43, 5).front()], -285.58f, -312.97f, -69.19f), 0.01f);

    // Two direct points: the step onto the ledge island and the drop off it.
    std::vector<std::size_t> direct;
    for (std::size_t row : RouteFor(43, 7))
        if (Points[row].direct)
            direct.push_back(row);
    ASSERT_EQ(direct.size(), 2u);
    EXPECT_EQ(Points[direct[0]].encounterIdx, 5u);
    EXPECT_EQ(Points[direct[0]].pointOrder, 14u);
    EXPECT_LT(Distance(Points[direct[0] - 1], -291.6f, -1.4f, -58.1f), 0.1f);
    EXPECT_LT(Distance(Points[direct[0]], -289.1f, 3.5f, -64.0f), 0.1f);
    EXPECT_EQ(Points[direct[1]].encounterIdx, 7u);
    EXPECT_EQ(Points[direct[1]].pointOrder, 10u);
    EXPECT_NEAR(Points[direct[1]].z - Points[direct[1] - 1].z, -28.0f, 0.1f);
}

TEST(DungeonRoutePolicy, RouteServesOnlyTheGoalItEndsAt)
{
    // Deadmines encounter 3's gate rows (Defias Gunpowder chest, cannon) never take the Mr. Smite leg.
    std::vector<std::size_t> const smite = RouteFor(36, 3);
    EXPECT_TRUE(EndsAt(smite, -22.8471f, -797.283f, 20.3745f));
    EXPECT_FALSE(EndsAt(smite, -106.409f, -617.284f, 13.8495f));
    EXPECT_FALSE(EndsAt(smite, -107.562f, -659.674f, 7.21211f));
    EXPECT_FALSE(EndsAt({}, 0.0f, 0.0f, 0.0f));
}

TEST(DungeonRoutePolicy, DirectStepGuards)
{
    Point const top = {43, 5, 13, 0.0f, 0.0f, 0.0f};
    Point const step = {43, 5, 14, 5.0f, 0.0f, -6.0f, true};
    Point const walk = {43, 5, 14, 5.0f, 0.0f, -6.0f};
    float const stepDistance = Distance(step, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(CanDirectStep(0.5f, stepDistance, top, step));
    EXPECT_TRUE(CanDirectStep(DirectStartRadius, DirectStartRadius + 1.0f, top, step));
    EXPECT_FALSE(CanDirectStep(DirectStartRadius + 0.1f, 20.0f, top, step));  // not at the step's start
    EXPECT_FALSE(CanDirectStep(7.0f, 1.0f, top, step));  // already at the bottom
    EXPECT_FALSE(CanDirectStep(0.5f, stepDistance, top, walk));  // walk points are never stepped
    EXPECT_FALSE(CanDirectStep(std::nanf(""), stepDistance, top, step));

    EXPECT_TRUE(DirectStepShape(top, {43, 5, 14, 8.0f, 0.0f, -30.0f, true}));
    EXPECT_FALSE(DirectStepShape(top, {43, 5, 14, 8.1f, 0.0f, -1.0f, true}));  // too far across
    EXPECT_FALSE(DirectStepShape(top, {43, 5, 14, 1.0f, 0.0f, -30.1f, true}));  // too far down
    EXPECT_TRUE(DirectStepShape(top, {43, 5, 14, 1.0f, 0.0f, 6.0f, true}));
    EXPECT_FALSE(DirectStepShape(top, {43, 5, 14, 1.0f, 0.0f, 6.1f, true}));  // too far up
    EXPECT_FALSE(DirectStepShape(top, {36, 5, 14, 1.0f, 0.0f, -1.0f, true}));  // other map

    // Level crossing (a swim across a water surface): farther across, but only level.
    EXPECT_TRUE(DirectStepShape(top, {43, 5, 14, DirectMaximumLevelCrossing, 0.0f, 0.0f, true}));
    EXPECT_TRUE(DirectStepShape(top, {43, 5, 14, 12.0f, 0.0f, -DirectLevelTolerance, true}));
    EXPECT_FALSE(DirectStepShape(top, {43, 5, 14, 12.0f, 0.0f, -DirectLevelTolerance - 0.1f, true}));
    EXPECT_FALSE(DirectStepShape(top, {43, 5, 14, DirectMaximumLevelCrossing + 0.1f, 0.0f, 0.0f, true}));
    EXPECT_FALSE(DirectStepShape(top, {36, 5, 14, 12.0f, 0.0f, 0.0f, true}));  // other map

    // Level crossings move on a raw spline; ledge steps and drops keep MovePoint.
    EXPECT_TRUE(IsLevelCrossing(top, {43, 5, 14, 12.0f, 0.0f, 0.0f, true}));
    EXPECT_FALSE(IsLevelCrossing(top, {43, 5, 14, 5.0f, 0.0f, -6.0f, true}));
    EXPECT_FALSE(IsLevelCrossing(top, {43, 5, 14, DirectMaximumLevelCrossing + 0.1f, 0.0f, 0.0f, true}));
    EXPECT_FALSE(IsLevelCrossing(top, {36, 5, 14, 12.0f, 0.0f, 0.0f, true}));
}

TEST(DungeonRoutePolicy, RazorfenDownsAmnennarLegClimbsTheSpiral)
{
    // Amnennar the Coldbringer, idx3 (spawn 87209); no earlier Razorfen Downs leg.
    EXPECT_TRUE(RouteFor(129, 2).empty());
    std::vector<std::size_t> const rows = RouteFor(129, 3);
    ASSERT_EQ(rows.size(), 23u);
    EXPECT_TRUE(EndsAt(rows, 2403.37f, 960.93f, 55.1437f));
    // Starts at Glutton's spawn (8567); a party at the S68 kill point enters there, not mid-spiral.
    EXPECT_LT(Distance(Points[rows.front()], 2468.7f, 1006.8f, 23.8f), 0.01f);
    EXPECT_EQ(NearestPoint(rows, 2463.0f, 1019.0f, 24.0f), 0u);
    for (std::size_t row : rows)
        EXPECT_FALSE(Points[row].direct) << row;
}

TEST(DungeonRoutePolicy, RouteToCutsALegAtItsGoal)
{
    // A goal at a leg's end: the same rows as RouteFor (every earlier leg).
    std::vector<std::size_t> const smite = RouteFor(36, 3);
    EXPECT_EQ(RouteTo(36, 3, -22.8471f, -797.283f, 20.3745f), smite);
    // A goal on no point of the encounter's own leg: none (the Gunpowder chest).
    EXPECT_TRUE(RouteTo(36, 3, -106.409f, -617.284f, 13.8495f).empty());
    // A point of an earlier leg is not a goal of this one (Mr. Smite's point on the route to Cookie).
    EXPECT_TRUE(RouteTo(36, 4, -22.8471f, -797.283f, 20.3745f).empty());
}

TEST(DungeonRoutePolicy, SunkenTempleLegsWalkTheGateRowsInOrder)
{
    // Statues 148830..148835 (the idx0 gate rows, in use order), then Atal'alarion (34521) at leg 0's end.
    struct Goal { float x, y, z; };
    Goal const statues[] = {{-515.553f, 95.2582f, -148.74f}, {-419.849f, 94.4837f, -148.74f},
        {-491.4f, 135.97f, -148.74f}, {-491.491f, 53.4818f, -148.74f}, {-443.855f, 136.101f, -148.74f},
        {-443.417f, 53.8312f, -148.74f}, {-480.4f, 96.5663f, -189.73f}};
    std::size_t last = 0;
    for (Goal const& goal : statues)
    {
        std::vector<std::size_t> const rows = RouteTo(109, 0, goal.x, goal.y, goal.z);
        ASSERT_FALSE(rows.empty()) << goal.x;
        EXPECT_GT(rows.size(), last) << goal.x;  // each goal lies further along the leg
        EXPECT_EQ(Points[rows.back()].encounterIdx, 0u);
        last = rows.size();
    }
    EXPECT_EQ(last, RouteFor(109, 0).size());
    // Starts at the instance entrance (areatrigger_teleport 446), where S69 parties entered.
    EXPECT_LT(Distance(Points[RouteFor(109, 0).front()], -319.2f, 99.9f, -131.9f), 0.01f);

    // Balcony trolls (idx3 gate rows, in row order), then Jammal'an (39737) at leg 3's end.
    Goal const trolls[] = {{-406.189f, 131.068f, -66.9138f}, {-467.396f, 165.997f, -66.7027f},
        {-528.646f, 130.163f, -66.7533f}, {-527.969f, 59.4516f, -66.7188f}, {-466.655f, 24.4261f, -66.7908f},
        {-405.506f, 60.4569f, -67.0678f}, {-425.894f, -86.0747f, -88.224f}};
    last = 0;
    for (Goal const& goal : trolls)
    {
        std::vector<std::size_t> const rows = RouteTo(109, 3, goal.x, goal.y, goal.z);
        ASSERT_FALSE(rows.empty()) << goal.x;
        EXPECT_GT(rows.size(), last) << goal.x;
        last = rows.size();
    }
    EXPECT_EQ(last, RouteFor(109, 3).size());
    // The leader at Atal'alarion enters leg 3 at its first point (the tie with leg 0's end keeps the later point).
    std::vector<std::size_t> const toMijan = RouteTo(109, 3, -406.189f, 131.068f, -66.9138f);
    std::size_t const entry = NearestPoint(toMijan, -480.4f, 96.5663f, -189.73f);
    ASSERT_NE(entry, NoPoint);
    EXPECT_EQ(Points[toMijan[entry]].encounterIdx, 3u);
    EXPECT_EQ(Points[toMijan[entry]].pointOrder, 0u);

    // After Jammal'an: Dreamscythe, Weaver, Morphaz, Hazzas, Shade of Eranikus each end their leg.
    struct Boss { std::uint32_t encounter; float x, y, z; };
    Boss const bosses[] = {{1, -453.45f, 137.17f, -90.75f}, {2, -458.84f, 127.7f, -91.57f},
        {5, -667.59f, 103.111f, -90.8313f}, {6, -667.359f, 80.803f, -90.8326f}, {8, -658.379f, -35.7623f, -90.8352f}};
    for (Boss const& boss : bosses)
        EXPECT_TRUE(EndsAt(RouteFor(109, boss.encounter), boss.x, boss.y, boss.z)) << boss.encounter;
    for (std::size_t row : RouteFor(109, 8))
        EXPECT_FALSE(Points[row].direct) << row;
}

TEST(DungeonRoutePolicy, BlackfathomSarevessLegSwimsThePool)
{
    // Lady Sarevess, idx1 (spawn 26129); no leg for Ghamoo-ra (idx0), so the route is leg 1 alone.
    EXPECT_TRUE(RouteFor(48, 0).empty());
    std::vector<std::size_t> const rows = RouteFor(48, 1);
    ASSERT_EQ(rows.size(), 15u);
    EXPECT_TRUE(EndsAt(rows, -299.917f, 413.755f, -57.123f));
    // Starts at Ghamoo-ra's spawn (25732), where S62 parties stood with Sarevess next.
    EXPECT_LT(Distance(Points[rows.front()], -442.424f, 211.822f, -52.6367f), 0.01f);
    EXPECT_EQ(NearestPoint(rows, -443.0f, 207.0f, -52.6f), 0u);

    // One direct point, the level swim across the unlinked water surface.
    std::vector<std::size_t> direct;
    for (std::size_t row : rows)
        if (Points[row].direct)
            direct.push_back(row);
    ASSERT_EQ(direct.size(), 1u);
    Point const& from = Points[direct[0] - 1];
    Point const& to = Points[direct[0]];
    EXPECT_EQ(to.pointOrder, 6u);
    float const across = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
    EXPECT_GT(across, DirectMaximumHorizontal);
    EXPECT_NEAR(to.z, from.z, 0.01f);
    EXPECT_TRUE(DirectStepShape(from, to));
    EXPECT_TRUE(IsLevelCrossing(from, to));
    // The Wailing Caverns steps are not level crossings.
    for (std::size_t row : RouteFor(43, 7))
        if (Points[row].direct)
            EXPECT_FALSE(IsLevelCrossing(Points[row - 1], Points[row])) << row;
}

TEST(DungeonRoutePolicy, DirectStepTimesOutOnce)
{
    EXPECT_EQ(EvaluateDirectStep(DirectArrivalRadius, 0), DirectWait::Arrived);
    EXPECT_EQ(EvaluateDirectStep(1.0f, DirectTimeoutMs + 5000), DirectWait::Arrived);
    EXPECT_EQ(EvaluateDirectStep(20.0f, DirectTimeoutMs - 1), DirectWait::Pending);
    EXPECT_EQ(EvaluateDirectStep(20.0f, DirectTimeoutMs), DirectWait::Failed);
    EXPECT_EQ(EvaluateDirectStep(std::nanf(""), DirectTimeoutMs), DirectWait::Failed);
    EXPECT_EQ(DirectTimeoutMs, 10000u);

    std::vector<std::size_t> const rows = RouteFor(43, 7);
    std::size_t const stepRow = rows[14];  // leg 5 point 14, the step onto the island
    ASSERT_TRUE(Points[stepRow].direct);
    auto failedStep = [&](std::size_t row) { return row == stepRow; };
    EXPECT_TRUE(RouteHasFailedStep(rows, 0, failedStep));
    EXPECT_FALSE(RouteHasFailedStep(rows, 15, failedStep));  // already past it
    EXPECT_FALSE(RouteHasFailedStep(rows, 0, [](std::size_t) { return false; }));
}

TEST(DungeonRoutePolicy, EntryNeverStartsOnADirectPoint)
{
    std::vector<std::size_t> const rows = RouteFor(43, 5);
    EXPECT_EQ(EntryStart(rows, 14), 13u);  // entered at the step: start from its top
    EXPECT_EQ(EntryStart(rows, 15), 15u);
    EXPECT_EQ(EntryStart(rows, 0), 0u);
    EXPECT_EQ(EntryStart(rows, NoPoint), NoPoint);
}

TEST(DungeonRoutePolicy, UseRouteBetweenEntryAndHandoff)
{
    EXPECT_TRUE(UseRoute(0.0f, 100.0f));
    EXPECT_TRUE(UseRoute(EntryRadius, HandoffRadius + 0.1f));
    EXPECT_FALSE(UseRoute(EntryRadius + 0.1f, 100.0f));  // far away: ordinary travel nodes bring the party in
    EXPECT_FALSE(UseRoute(0.0f, HandoffRadius));  // at the boss: ordinary direct approach and activation
    EXPECT_FALSE(UseRoute(std::nanf(""), 100.0f));
    EXPECT_FALSE(UseRoute(0.0f, std::nanf("")));
    // Handoff exceeds the navigator's 3 yd arrival radius, so an exhausted route releases for activation
    // instead of rebuilding from its last point.
    EXPECT_GT(HandoffRadius, 3.0f);
    EXPECT_EQ(LookaheadPointLimit, 1u);
}
