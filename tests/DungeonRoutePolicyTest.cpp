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
            // A leg continues where the previous leg of the map ends.
            if (previous.mapId == point.mapId)
                EXPECT_LE(Distance(previous, point.x, point.y, point.z), MaximumPointSpacing) << i;
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

TEST(DungeonRoutePolicy, NearestPointTieKeepsEarlierPoint)
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
    EXPECT_EQ(nearest, da <= db ? 3u : 4u);
    EXPECT_EQ(NearestPoint({rows[3], rows[3]}, a.x, a.y, a.z), 0u);
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
