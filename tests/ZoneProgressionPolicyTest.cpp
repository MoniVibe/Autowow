/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ZoneProgressionPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowZoneProgression;

// soak-s9-baseline-r1: L9-10 cohort bots stalled in their 5-12 starter zones (Elwynn bracket 5,12).
TEST(ZoneProgression, LevelTriggerAtBracketMaxMinusMargin)
{
    Params p;  // margin 2
    EXPECT_EQ(Evaluate(p, 9, 12, 0, true), Trigger::None);
    EXPECT_EQ(Evaluate(p, 10, 12, 0, true), Trigger::Level);
    EXPECT_EQ(Evaluate(p, 11, 12, 0, true), Trigger::Level);
    // Unbracketed zone: level rule off. No route: never.
    EXPECT_EQ(Evaluate(p, 60, 0, 0, true), Trigger::None);
    EXPECT_EQ(Evaluate(p, 11, 12, 99, false), Trigger::None);
}

TEST(ZoneProgression, NoQuestTriggerNeedsConsecutiveStalls)
{
    Params p;
    p.stallChecks = 3;
    std::uint32_t stall = 0;
    stall = NextStall(stall, false);
    stall = NextStall(stall, false);
    EXPECT_EQ(Evaluate(p, 9, 12, stall, true), Trigger::None);
    stall = NextStall(stall, true);  // an actionable quest resets the run
    EXPECT_EQ(stall, 0U);
    for (int k = 0; k < 3; ++k)
        stall = NextStall(stall, false);
    EXPECT_EQ(Evaluate(p, 9, 12, stall, true), Trigger::NoQuests);
    p.stallChecks = 0;  // rule off
    EXPECT_EQ(Evaluate(p, 9, 12, 100, true), Trigger::None);
}

TEST(ZoneProgression, DefaultTableCoversEveryStarterZone)
{
    std::vector<Route> const routes = DefaultRoutes();
    struct Case { std::uint32_t team, from, to; };
    for (Case c : {Case{1, 12, 40}, Case{1, 1, 38}, Case{1, 141, 148}, Case{1, 3524, 3525}, Case{2, 14, 17},
                   Case{2, 215, 17}, Case{2, 85, 130}, Case{2, 3430, 3433}})
    {
        Route const* r = PickRoute(routes, c.team, c.from, 10, 62955);
        ASSERT_NE(r, nullptr) << c.from;
        EXPECT_EQ(r->to, c.to) << c.from;
        EXPECT_NE(r->inn, 0U);
    }
    // Wrong faction and a level below every band get nothing.
    EXPECT_EQ(PickRoute(routes, 2, 12, 10, 1), nullptr);
    EXPECT_EQ(PickRoute(routes, 1, 12, 5, 1), nullptr);
    // Elwynn at 16 goes to Redridge; Westfall at 18 onward to Duskwood.
    EXPECT_EQ(PickRoute(routes, 1, 12, 16, 1)->to, 44U);
    EXPECT_EQ(PickRoute(routes, 1, 40, 18, 1)->to, 10U);
    // Only Teldrassil -> Darkshore needs a sea crossing.
    EXPECT_TRUE(PickRoute(routes, 1, 141, 10, 1)->crossing);
}

TEST(ZoneProgression, TiedRoutesSpreadByGuid)
{
    std::vector<Route> routes;
    ASSERT_TRUE(ParseRoutes("1,12,40,9,14,0,1,2,3,10,0; 1,12,44,9,14,0,4,5,6,11,0", routes));
    EXPECT_EQ(PickRoute(routes, 1, 12, 10, 100)->to, 40U);
    EXPECT_EQ(PickRoute(routes, 1, 12, 10, 101)->to, 44U);
    EXPECT_EQ(PickRoute(routes, 1, 12, 10, 102)->to, 40U);
}

TEST(ZoneProgression, ParseRoutesRejectsMalformedAndKeepsOutput)
{
    std::vector<Route> routes = DefaultRoutes();
    std::size_t const n = routes.size();
    EXPECT_FALSE(ParseRoutes("1,12,40,9,14,0,1,2,3,10", routes));        // 10 fields
    EXPECT_FALSE(ParseRoutes("1,12,40,9,14,0,1,2,3,10,0,7", routes));    // 12 fields
    EXPECT_FALSE(ParseRoutes("3,12,40,9,14,0,1,2,3,10,0", routes));      // team
    EXPECT_FALSE(ParseRoutes("1,12,40,15,14,0,1,2,3,10,0", routes));     // band
    EXPECT_FALSE(ParseRoutes("1,12,40,9,14,0,1,x,3,10,0", routes));
    EXPECT_EQ(routes.size(), n);
    ASSERT_TRUE(ParseRoutes("0,85,130,9,18,0,510,1636,126,6739,1;", routes));
    ASSERT_EQ(routes.size(), 1U);
    EXPECT_EQ(routes[0].x, 510);
    EXPECT_TRUE(routes[0].crossing);
    ASSERT_TRUE(ParseRoutes("2,3430,3433,9,20,530,7553,-6898,96,16542,0", routes));
    EXPECT_EQ(routes[0].y, -6898);
}

TEST(ZoneProgression, TravelModeSelection)
{
    EXPECT_EQ(SelectMode(true, true, false), Mode::Flight);
    EXPECT_EQ(SelectMode(true, false, true), Mode::Flight);   // Teldrassil with Auberdine known
    EXPECT_EQ(SelectMode(false, true, false), Mode::Walk);
    EXPECT_EQ(SelectMode(false, true, true), Mode::Unreachable);  // Teldrassil, Auberdine unknown
    EXPECT_EQ(SelectMode(false, false, false), Mode::Unreachable);
}

TEST(ZoneProgression, TravelExhaustedByTimeoutOrReissues)
{
    Params p;
    p.travelTimeoutMs = 1000;
    p.maxReissues = 2;
    BotState s;
    s.startMs = 5000;
    EXPECT_FALSE(TravelExhausted(p, s, 6000));
    EXPECT_TRUE(TravelExhausted(p, s, 6001));
    s.reissues = 3;
    EXPECT_TRUE(TravelExhausted(p, s, 5000));
}

TEST(ZoneProgression, LedgerFieldsAreStable)
{
    EXPECT_EQ(LedgerFields(12, 40, 123456, true, Mode::Walk),
              ",\"from\":12,\"to\":40,\"travel_ms\":123456,\"arrived\":true,\"mode\":\"walk\"");
    EXPECT_STREQ(TriggerName(Trigger::NoQuests), "no_quests");
}
}  // namespace
