/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ZoneProgressionPolicy.h"
#include "TransportCrossingPolicy.h"

#include <limits>

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

// soak-s10-zoneprog-r1 regression: ordinary walk ticks must not spend the reissue budget.
TEST(ZoneProgression, OnlyStuckWalkTicksSpendReissues)
{
    Params p;  // maxReissues 8, timeout 1 h
    BotState s;
    s.mode = Mode::Walk;
    for (int tick = 0; tick < 1000; ++tick)
        NoteWalkTick(s, false);
    EXPECT_EQ(s.reissues, 0U);
    EXPECT_EQ(s.mode, Mode::Walk);
    EXPECT_FALSE(TravelExhausted(p, s, 37251));
    NoteWalkTick(s, true);
    EXPECT_EQ(s.reissues, 1U);
    EXPECT_EQ(s.mode, Mode::Unreachable);  // next tick re-chooses flight / walk
    for (int k = 0; k < 8; ++k)
        NoteWalkTick(s, true);
    EXPECT_TRUE(TravelExhausted(p, s, 37251));
}

// soak-s11: Corwick (Elwynn -> Westfall, from (-9306,-319)) walked straight and died in Duskwood Raven Hill.
TEST(ZoneProgression, RoadJoinsNearestAndAdvances)
{
    std::vector<RoadPoint> const road = RoadFor(DefaultRoads(), 12, 40);
    ASSERT_EQ(road.size(), 3U);
    std::uint32_t wp = JoinRoad(road, -9306, -319);
    EXPECT_EQ(wp, 0U);  // Goldshire first, not the hub across Duskwood
    EXPECT_EQ(AdvanceRoad(road, wp, -9306, -319, 20), 0U);
    EXPECT_EQ(AdvanceRoad(road, wp, -9470, 60, 20), 1U);    // at Goldshire -> Westbrook
    EXPECT_EQ(AdvanceRoad(road, 2, -9789, 990, 20), 3U);    // past the last point -> hub
    EXPECT_EQ(JoinRoad(road, -9700, 800), 1U);              // mid-route bot joins mid-road
    EXPECT_TRUE(RoadFor(DefaultRoads(), 141, 148).empty());
    EXPECT_EQ(RoadFor(DefaultRoads(), 85, 130).back().x, 919);
}

TEST(ZoneProgression, AzuremystHubIsBloodWatch)
{
    Route const* r = PickRoute(DefaultRoutes(), 1, 3524, 10, 1);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->inn, 17553U);  // Caregiver Topher Loaal, not the Outland innkeeper at (-174,5529)
    EXPECT_EQ(r->y, -11897);
}

TEST(ZoneProgression, PortalFallbackDecision)
{
    EXPECT_FALSE(PortalFallback(true, Mode::Walk, false, 7, 8, 1199999, 1200000));
    EXPECT_TRUE(PortalFallback(true, Mode::Walk, false, 8, 8, 0, 1200000));        // stuck budget spent
    EXPECT_TRUE(PortalFallback(true, Mode::Walk, false, 0, 8, 1200000, 1200000));  // K minutes passed
    EXPECT_TRUE(PortalFallback(true, Mode::Unreachable, false, 8, 8, 0, 1200000));
    EXPECT_FALSE(PortalFallback(true, Mode::Walk, false, 0, 8, 99999999, 0));      // time rule off
    EXPECT_FALSE(PortalFallback(false, Mode::Walk, false, 99, 8, 99999999, 1));    // Transports off / mode real
    EXPECT_FALSE(PortalFallback(true, Mode::Walk, true, 99, 8, 99999999, 1));      // already in the zone
    EXPECT_FALSE(PortalFallback(true, Mode::Flight, false, 99, 8, 99999999, 1));   // flight / chain own legs
    EXPECT_FALSE(PortalFallback(true, Mode::Chain, false, 99, 8, 99999999, 1));
    EXPECT_STREQ(ModeName(Mode::Portal), "portal");
}

// soak-s13-full-r1: bot 62957 (alliance L13) died 61 times at Duskwood (-10603,292); no route leaves zone 10.
TEST(ZoneProgression, EscapeHubIsNearestLevelFitOnTheBotsMap)
{
    std::vector<Route> const routes = DefaultRoutes();
    Route const* r = PickEscapeRoute(routes, 1, 13, 10, 0, -10603, 292);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 40U);  // Westfall, Sentinel Hill
    // Standing in Westfall already: the next nearest level fit on map 0 (Loch Modan), never its own zone.
    r = PickEscapeRoute(routes, 1, 13, 40, 0, -10653, 1166);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 38U);
    // L16: Westfall's band (9-14) no longer fits; Redridge (band 15-20) is nearer than Loch Modan.
    r = PickEscapeRoute(routes, 1, 16, 10, 0, -10603, 292);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 44U);
    // Horde L12 on Kalimdor: the Barrens (Crossroads).
    r = PickEscapeRoute(routes, 2, 12, 406, 1, 900, 900);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 17U);
    // No hub on the bot's map: first level fit in table order (a flight/portal leg must carry it).
    r = PickEscapeRoute(routes, 1, 13, 65, 571, 0, 0);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 40U);
    // No band holds the level: none (caller keeps the flight relocation).
    EXPECT_EQ(PickEscapeRoute(routes, 1, 31, 10, 0, -10603, 292), nullptr);
}

// AutoWow.DeathLoop.V2: lowest fitting band first. soak-s14-full-r1: 62964 (alliance L10) died 3 times in
// one circle at Loch Modan (-5347,-2850) with relocate=false.
TEST(ZoneProgression, LowEscapeHubPrefersTheLowestFittingBand)
{
    std::vector<Route> const routes = DefaultRoutes();
    // L10 in Loch Modan: Westfall (9-14) beats Darkshore / Bloodmyst (9-18); never Loch Modan itself.
    Route const* r = PickLowEscapeRoute(routes, 1, 10, 38, 0, -5347, -2850);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 40U);
    // L16: Westfall no longer fits; Redridge (15-20) before Loch Modan / Darkshore / Bloodmyst (9-18).
    r = PickLowEscapeRoute(routes, 1, 16, 10, 0, -10603, 292);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 38U);  // 9-18 bands start lower than Redridge's 15: Loch Modan, nearest of them on map 0
    // Same band, same map: the nearer hub (PickEscapeRoute's order).
    std::vector<Route> const two = {{1, 0, 501, 9, 14, 0, 1000, 0, 0, 1, false},
                                    {1, 0, 502, 9, 14, 0, 100, 0, 0, 2, false}};
    r = PickLowEscapeRoute(two, 1, 10, 7, 0, 0, 0);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 502U);
    // Horde L11 in the Barrens: no other hub on Kalimdor fits; Silverpine (9-18) before Ghostlands (9-20).
    r = PickLowEscapeRoute(routes, 2, 11, 17, 1, -727, -2542);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 130U);
    EXPECT_EQ(PickLowEscapeRoute(routes, 1, 31, 10, 0, -10603, 292), nullptr);
}

TEST(ZoneProgression, BeginEscapeStartsADeathLoopTrip)
{
    std::vector<Route> const routes = DefaultRoutes();
    Route const* hub = PickEscapeRoute(routes, 1, 13, 10, 0, -10603, 292);
    ASSERT_NE(hub, nullptr);
    BotState s;
    s.nextCheckMs = 777;
    s.cooldownUntilMs = 888;
    s.reissues = 5;
    s.roadJoined = true;
    s.wp = 2;
    BeginEscape(s, *hub, 10, 5000, true);
    EXPECT_EQ(s.phase, Phase::Travel);
    EXPECT_EQ(s.trigger, Trigger::DeathLoop);
    EXPECT_EQ(s.fromZone, 10U);
    EXPECT_EQ(s.route.from, 10U);  // no Elwynn road: straight to the hub
    EXPECT_EQ(s.route.to, 40U);
    EXPECT_EQ(s.startMs, 5000U);
    EXPECT_EQ(s.reissues, 0U);
    EXPECT_EQ(s.mode, Mode::Portal);  // the Travel phase portals at once (reissues <= MaxReissues)
    EXPECT_FALSE(s.roadJoined);
    EXPECT_EQ(s.wp, 0U);
    EXPECT_EQ(s.nextCheckMs, 777U);
    EXPECT_EQ(s.cooldownUntilMs, 888U);
    EXPECT_TRUE(RoadFor(DefaultRoads(), s.route.from, s.route.to).empty());
    BeginEscape(s, *hub, 10, 6000, false);
    EXPECT_EQ(s.mode, Mode::Unreachable);  // flight / walk choice, then the ordinary portal fallback
    // Walk leg of an escape still falls back to the portal after PortalAfterMs.
    EXPECT_TRUE(PortalFallback(true, Mode::Walk, false, 0, 8, 1200000, 1200000));
    EXPECT_STREQ(TriggerName(Trigger::DeathLoop), "death_loop");
    EXPECT_EQ(LedgerFields(10, 40, 1654, true, Mode::Portal),
              ",\"from\":10,\"to\":40,\"travel_ms\":1654,\"arrived\":true,\"mode\":\"portal\"");
}

TEST(ZoneProgression, LedgerFieldsAreStable)
{
    EXPECT_EQ(LedgerFields(12, 40, 123456, true, Mode::Walk),
              ",\"from\":12,\"to\":40,\"travel_ms\":123456,\"arrived\":true,\"mode\":\"walk\"");
    EXPECT_STREQ(TriggerName(Trigger::NoQuests), "no_quests");
}

// AutoWow.ZoneProgression.HighRoutes: the L20-60 hub table.
std::vector<Route> HighTable()
{
    std::vector<Route> routes = DefaultRoutes();
    for (Route const& r : HubRoutes(DefaultHubs(), DefaultHubSources()))
        routes.push_back(r);
    return routes;
}

TEST(ZoneProgression, HubRoutesAreWalkableSameContinentAndStable)
{
    std::vector<Route> const high = HubRoutes(DefaultHubs(), DefaultHubSources());
    ASSERT_FALSE(high.empty());
    for (Route const& r : high)
    {
        EXPECT_TRUE(r.team == 1 || r.team == 2);
        EXPECT_TRUE(r.map == 0 || r.map == 1);
        EXPECT_NE(r.from, r.to);
        EXPECT_FALSE(r.crossing);
        EXPECT_GE(r.minLevel, 20U);
        EXPECT_LE(r.maxLevel, 60U);
        EXPECT_LE(r.minLevel, r.maxLevel);
    }
    // Same input, same table (deterministic order), and the base table is untouched.
    std::vector<Route> const again = HubRoutes(DefaultHubs(), DefaultHubSources());
    ASSERT_EQ(again.size(), high.size());
    for (std::size_t k = 0; k < high.size(); ++k)
        EXPECT_TRUE(again[k].from == high[k].from && again[k].to == high[k].to && again[k].x == high[k].x);
    EXPECT_EQ(DefaultRoutes().size(), 15U);
}

// Every level 20-60 has a hub on the bot's own continent, from a starter zone of each faction.
TEST(ZoneProgression, HubRoutesCoverLevels20To60PerFactionAndContinent)
{
    std::vector<Route> const routes = HighTable();
    struct Case { std::uint32_t team, map, zone; };
    for (Case c : {Case{1, 0, 12}, Case{1, 0, 1}, Case{1, 1, 148}, Case{2, 0, 130}, Case{2, 1, 17}, Case{2, 1, 215}})
        for (std::uint32_t level = 20; level <= 60; ++level)
            for (std::uint32_t guid : {1U, 2U, 3U, 7U, 62955U})
            {
                Route const* r = PickRoute(routes, c.team, c.zone, level, guid);
                ASSERT_NE(r, nullptr) << c.team << " " << c.zone << " L" << level;
                EXPECT_EQ(r->map, c.map) << c.zone << " L" << level;
                EXPECT_TRUE(r->team == 0 || r->team == c.team);
                EXPECT_TRUE(level >= r->minLevel && level <= r->maxLevel);
            }
}

// A bot that outgrows any hub zone (its band max up to 57) always has a way on, same continent.
TEST(ZoneProgression, EveryHubZoneHasAWayOnUntilLevel57)
{
    std::vector<Route> const routes = HighTable();
    for (Hub const& h : DefaultHubs())
        for (std::uint32_t level = h.maxLevel; level <= 57; ++level)
        {
            Route const* r = PickRoute(routes, h.team, h.zone, level, h.npc);
            ASSERT_NE(r, nullptr) << h.team << " " << h.zone << " L" << level;
            EXPECT_NE(r->to, h.zone);
            EXPECT_EQ(r->map, h.map);
        }
}

// Horde never lands on an Alliance hub and vice versa. Faction template of each hub npc (world DB
// creature_template.faction) and its FactionTemplate.dbc enemy group (2 alliance, 4 horde, 0 neutral).
TEST(ZoneProgression, NoHostileFactionHubs)
{
    struct Npc { std::uint32_t entry, factionTemplate, enemyGroup; };
    std::vector<Npc> const npcs = {
        {6790, 12, 4},   {1464, 55, 4},     {2352, 12, 4},    {24366, 1732, 4}, {2835, 12, 4},
        {6807, 120, 0},  {7744, 694, 4},    {2941, 55, 4},    {8609, 12, 4},    {2299, 12, 4},
        {12596, 12, 4},  {16256, 794, 0},   {6738, 80, 4},    {16458, 80, 4},   {11103, 80, 4},
        {6272, 894, 4},  {7733, 474, 0},    {4319, 80, 4},    {12577, 80, 4},   {10583, 474, 0},
        {12578, 80, 4},  {11118, 855, 0},   {15174, 994, 0},  {2388, 68, 2},    {9501, 29, 2},
        {5814, 29, 2},   {6930, 29, 2},     {9356, 29, 2},    {14731, 1494, 2}, {3305, 29, 2},
        {13177, 29, 2},  {7731, 29, 2},     {12196, 29, 2},   {11116, 104, 2},  {11106, 104, 2},
        {24208, 29, 2},  {7737, 29, 2},     {8610, 29, 2},    {11900, 29, 2},
    };
    for (Hub const& h : DefaultHubs())
    {
        auto const it = std::find_if(npcs.begin(), npcs.end(), [&](Npc const& n) { return n.entry == h.npc; });
        ASSERT_NE(it, npcs.end()) << h.npc;
        std::uint32_t const teamGroup = h.team == 1 ? 2 : 4;
        EXPECT_EQ(it->enemyGroup & teamGroup, 0U) << "hub npc " << h.npc << " hostile to team " << h.team;
        EXPECT_TRUE(h.inn == 0 || h.inn == h.npc);
    }
    for (Route const& r : HubRoutes(DefaultHubs(), DefaultHubSources()))
        if (r.inn)
            EXPECT_NE(std::find_if(npcs.begin(), npcs.end(), [&](Npc const& n) { return n.entry == r.inn; }), npcs.end());
}

TEST(ZoneProgression, HubPickSpreadsByGuidDeterministically)
{
    std::vector<Route> const routes = HighTable();
    // Alliance L32 in Duskwood: Stranglethorn (Rebel Camp) or Arathi (Refuge Pointe), by guid.
    Route const* a = PickRoute(routes, 1, 10, 32, 0);
    Route const* b = PickRoute(routes, 1, 10, 32, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->to, b->to);
    EXPECT_EQ(PickRoute(routes, 1, 10, 32, 0), a);
    for (std::uint32_t guid = 0; guid < 8; ++guid)
    {
        Route const* r = PickRoute(routes, 1, 10, 32, guid);
        EXPECT_TRUE(r->to == 33 || r->to == 45) << r->to;
    }
    // Horde L46 in the Hinterlands: never back to the Hinterlands, never Kalimdor.
    for (std::uint32_t guid = 0; guid < 8; ++guid)
    {
        Route const* r = PickRoute(routes, 2, 47, 46, guid);
        ASSERT_NE(r, nullptr);
        EXPECT_NE(r->to, 47U);
        EXPECT_EQ(r->map, 0U);
    }
    // Wrong team: an Alliance-only source zone has nothing for the Horde.
    EXPECT_EQ(PickRoute(routes, 2, 12, 30, 1), nullptr);
}

// ---- AutoWow.Survival.HardEscape (1) --------------------------------------------------------------------
// Coarse Eastern Kingdoms zone boxes (x north, y west; first match wins) with the conf.dist bracket lows.
std::uint32_t EkZone(std::int32_t x, std::int32_t y)
{
    struct Box
    {
        std::uint32_t zone;
        std::int32_t x0, x1, y0, y1;
    };
    static Box const boxes[] = {
        {46, -8400, -7500, -3200, -500},    // Burning Steppes (51-60)
        {51, -7500, -6300, -2000, -500},    // Searing Gorge (45-51)
        {38, -6300, -4700, -4200, -2100},   // Loch Modan (10-20)
        {1, -6300, -5000, -2100, 1000},     // Dun Morogh (5-12)
        {44, -9800, -8400, -3500, -1300},   // Redridge (16-28)
        {10, -11200, -10000, -1500, 700},   // Duskwood (19-33)
        {12, -10000, -8400, -1300, 1000},   // Elwynn (5-12)
        {40, -11500, -10000, 700, 2000},    // Westfall (10-21)
    };
    for (Box const& b : boxes)
        if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1)
            return b.zone;
    return 0;
}

std::uint32_t EkLow(std::uint32_t zone)
{
    switch (zone)
    {
        case 46: return 51;
        case 51: return 45;
        case 38: return 10;
        case 1: return 5;
        case 44: return 16;
        case 10: return 19;
        case 12: return 5;
        case 40: return 10;
    }
    return 0;
}

// Danger = bracket low more than 5 above the bot (AutoWowDeathLoop::Overshoot with WalkZoneMargin 5).
auto DangerFor(std::uint32_t level)
{
    return [level](std::uint32_t zone) { return EkLow(zone) > level + 5; };
}

TEST(ZoneProgression, SegmentCrossesDangerSkipsTheBotsOwnZone)
{
    auto const danger = DangerFor(18);
    // Westfall -> Thelsamar runs through Burning Steppes.
    EXPECT_TRUE(SegmentCrossesDanger(-10547, 1197, -5378, -2974, kDangerStepYards, 40, EkZone, danger));
    // Westfall -> Darkshire stays in Westfall / Duskwood.
    EXPECT_FALSE(SegmentCrossesDanger(-10547, 1197, -10516, -1161, kDangerStepYards, 40, EkZone, danger));
    // From inside Burning Steppes the bot's own zone does not count; Lakeshire is next door.
    EXPECT_FALSE(SegmentCrossesDanger(-7924, -1354, -9224, -2158, kDangerStepYards, 46, EkZone, danger));
    // ... but the same line counts for a bot standing elsewhere.
    EXPECT_TRUE(SegmentCrossesDanger(-7924, -1354, -9224, -2158, kDangerStepYards, 44, EkZone, danger));
    // A L50 may cross Burning Steppes (51 <= 55). Zero-length and step 0 are safe.
    EXPECT_FALSE(SegmentCrossesDanger(-10547, 1197, -5378, -2974, kDangerStepYards, 40, EkZone, DangerFor(50)));
    EXPECT_FALSE(SegmentCrossesDanger(-10547, 1197, -10547, 1197, 0, 40, EkZone, danger));
}

// soak-s22-full-r1 replay: Taelorin (alliance L18) looped at the Westfall grave (-10547,1197); the V2 escape
// took Loch Modan (lowest band) and the walk died 183 times in Burning Steppes (zone 46).
TEST(ZoneProgression, SafeEscapeHubReplaysTaelorin)
{
    std::vector<Route> routes = DefaultRoutes();
    for (Route const& r : HubRoutes(DefaultHubs(), DefaultHubSources()))
        routes.push_back(r);
    auto const danger = DangerFor(18);
    auto crossesFrom = [&](std::int32_t x, std::int32_t y, std::uint32_t zone)
    {
        return [=, &danger](Route const& r)
        { return SegmentCrossesDanger(x, y, r.x, r.y, kDangerStepYards, zone, EkZone, danger); };
    };

    // Before: the lowest-band pick crosses zone 46.
    Route const* old = PickLowEscapeRoute(routes, 1, 18, 40, 0, -10547, 1197);
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->to, 38U);
    EXPECT_TRUE(SegmentCrossesDanger(-10547, 1197, old->x, old->y, kDangerStepYards, 40, EkZone,
                                     [](std::uint32_t z) { return z == 46; }));

    // After: Duskwood (Darkshire), whose line never enters zone 46.
    Route const* r = PickSafeEscapeRoute(routes, 1, 18, 40, 0, -10547, 1197, crossesFrom(-10547, 1197, 40));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 10U);
    EXPECT_FALSE(SegmentCrossesDanger(-10547, 1197, r->x, r->y, kDangerStepYards, 40, EkZone,
                                      [](std::uint32_t z) { return z == 46; }));

    // Already in Burning Steppes at (-7924,-1354): out the short way to Redridge (Lakeshire).
    r = PickSafeEscapeRoute(routes, 1, 18, 46, 0, -7924, -1354, crossesFrom(-7924, -1354, 46));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 44U);
}

TEST(ZoneProgression, SafeEscapeHubSkipsANearerHubBehindDanger)
{
    // Bot at (0,0) on map 0. Hub 501 is near but its line crosses a danger strip (x in [100,200]).
    std::vector<Route> const routes = {{1, 0, 501, 9, 20, 0, 300, 0, 0, 1, false},
                                       {1, 0, 502, 9, 20, 0, -900, 0, 0, 2, false},
                                       {1, 0, 503, 9, 20, 1, 5, 5, 0, 3, false}};
    auto zoneAt = [](std::int32_t x, std::int32_t) -> std::uint32_t { return x >= 100 && x <= 200 ? 77 : 7; };
    auto danger = [](std::uint32_t z) { return z == 77; };
    auto crosses = [&](Route const& r) { return SegmentCrossesDanger(0, 0, r.x, r.y, 50, 7, zoneAt, danger); };
    Route const* r = PickSafeEscapeRoute(routes, 1, 15, 7, 0, 0, 0, crosses);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 502U);
    // Every same-map line crosses: the other-map hub (its leg is a flight / portal, never this walk).
    std::vector<Route> const blocked = {routes[0], routes[2]};
    r = PickSafeEscapeRoute(blocked, 1, 15, 7, 0, 0, 0, crosses);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 503U);
    // Nothing else: the nearest anyway (the walk's own zone check and portal fallback take over).
    std::vector<Route> const only = {routes[0]};
    r = PickSafeEscapeRoute(only, 1, 15, 7, 0, 0, 0, crosses);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->to, 501U);
    // Level outside every band / own zone: none.
    EXPECT_EQ(PickSafeEscapeRoute(routes, 1, 30, 7, 0, 0, 0, crosses), nullptr);
    EXPECT_EQ(PickSafeEscapeRoute(only, 1, 15, 501, 0, 0, 0, crosses), nullptr);
}

// AutoWow.ZoneProgression.Outland: HighRoutes + the Outland ladder.
std::vector<Route> OutlandTable()
{
    std::vector<Route> routes = HighTable();
    AddOutland(routes);
    return routes;
}

// soak-s85: an L58/L59 already in Hellfire has no eligible cross-zone hub yet. Reuse the faction's
// existing Hellfire inn as a death-loop-only escape target instead of reporting no_route forever.
TEST(ZoneProgression, SameZoneEscapeFallbackUsesExistingHellfireHubsAt58And59)
{
    std::vector<Route> const routes = OutlandTable();
    auto const safe = [](Route const&) { return false; };

    Route const* crossZone = PickSafeEscapeRoute(routes, 1, 58, kHellfireZone, kOutlandMap, -2570, 3917, safe);
    EXPECT_EQ(crossZone, nullptr);
    Route const* hub = PickEscapeOrSafeSameZoneHub(crossZone, routes, 1, 58, kHellfireZone, kOutlandMap, -2570,
                                                   3917, safe);
    ASSERT_NE(hub, nullptr);
    EXPECT_EQ(hub->inn, 16826U);
    EXPECT_EQ(hub->x, -709);
    EXPECT_EQ(hub->y, 2739);

    crossZone = PickSafeEscapeRoute(routes, 2, 59, kHellfireZone, kOutlandMap, -1243, 5866, safe);
    EXPECT_EQ(crossZone, nullptr);
    hub = PickEscapeOrSafeSameZoneHub(crossZone, routes, 2, 59, kHellfireZone, kOutlandMap, -1243, 5866, safe);
    ASSERT_NE(hub, nullptr);
    EXPECT_EQ(hub->inn, 16602U);
    EXPECT_EQ(hub->x, 191);
    EXPECT_EQ(hub->y, 2611);
}

TEST(ZoneProgression, SameZoneEscapeFallbackHonorsArrivalRadiusAtTheRealHub)
{
    std::vector<Route> const routes = OutlandTable();
    auto const safe = [](Route const&) { return false; };
    Route const* hub = PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 58, kHellfireZone, kOutlandMap, -2570, 3917,
                                                   safe);
    ASSERT_NE(hub, nullptr);

    EXPECT_TRUE(AtRouteHub(*hub, kOutlandMap, -709.0f, 2739.0f));
    EXPECT_TRUE(AtRouteHub(*hub, kOutlandMap, -695.0f, 2739.0f));
    EXPECT_FALSE(AtRouteHub(*hub, kOutlandMap, -694.0f, 2739.0f));
    EXPECT_FALSE(AtRouteHub(*hub, 0, -709.0f, 2739.0f));
    EXPECT_EQ(PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 58, kHellfireZone, kOutlandMap, -709, 2739, safe),
              nullptr);
    EXPECT_EQ(PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 58, kHellfireZone, kOutlandMap, -695, 2739, safe),
              nullptr);
}

TEST(ZoneProgression, SameZoneEscapeTripWalksAndPortalsToTheExactHub)
{
    std::vector<Route> const routes = OutlandTable();
    Route const* hub = PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 58, kHellfireZone, kOutlandMap, -2570, 3917,
                                                   [](Route const&) { return false; });
    ASSERT_NE(hub, nullptr);
    ASSERT_TRUE(hub->crossing);  // the existing hub comes from an Outland entry route row

    BotState trip;
    BeginEscape(trip, *hub, kHellfireZone, 1000, false);
    EXPECT_EQ(trip.route.from, kHellfireZone);
    EXPECT_EQ(trip.route.to, kHellfireZone);
    EXPECT_FALSE(trip.route.crossing);
    EXPECT_FALSE(IsOutlandEntry(trip.route));
    EXPECT_EQ(trip.route.x, -709);
    EXPECT_EQ(trip.route.y, 2739);

    std::vector<AutoWowTransports::Crossing> crossings = AutoWowTransports::DefaultCrossings();
    for (AutoWowTransports::Crossing const& crossing : AutoWowTransports::OutlandCrossings())
        crossings.push_back(crossing);
    std::vector<AutoWowTransports::Crossing> const chain =
        AutoWowTransports::ChainFor(crossings, 1, trip.route.from, trip.route.to);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(AutoWowTransports::SelectMode(false, true, trip.route.crossing, !chain.empty()), Mode::Walk);

    bool const far = PortalFallbackDestinationReached(trip, kHellfireZone, kOutlandMap, -2570.0f, 3917.0f);
    EXPECT_FALSE(far);
    EXPECT_TRUE(PortalFallback(true, Mode::Walk, far, 8, 8, 0, 1200000));
    bool const atHub = PortalFallbackDestinationReached(trip, kHellfireZone, kOutlandMap, -709.0f, 2739.0f);
    EXPECT_TRUE(atHub);
    EXPECT_FALSE(PortalFallback(true, Mode::Walk, atHub, 8, 8, 0, 1200000));
}

TEST(ZoneProgression, SameZoneEscapeFallbackDeduplicatesAndRejectsUnsafeHubs)
{
    std::vector<Route> const routes = {
        {2, 1, 7, 10, 20, 530, 100, 0, 0, 1001, false},  // opposing faction
        {1, 1, 7, 30, 40, 530, 200, 0, 0, 1002, false},  // wrong level
        {1, 1, 7, 10, 20, 1, 50, 0, 0, 1006, false},     // wrong map
        {1, 1, 7, 10, 20, 530, 300, 0, 0, 1003, false},
        {1, 2, 7, 10, 20, 530, 300, 0, 0, 9999, false},  // duplicate coordinates
        {1, 3, 7, 10, 20, 530, 310, 0, 0, 1003, false},  // duplicate hub identity
        {1, 4, 7, 10, 20, 530, -600, 0, 0, 1004, false},
        {1, 5, 7, 10, 20, 530, -900, 0, 0, 1005, false},
    };
    auto const zoneAt = [](std::int32_t x, std::int32_t) -> std::uint32_t
    { return x >= 100 && x <= 200 ? 77 : 7; };
    int checked = 0;
    auto const unsafe = [&](Route const& r)
    {
        ++checked;
        bool const crosses = SegmentCrossesDanger(0, 0, r.x, r.y, 50, 7, zoneAt,
                                                   [](std::uint32_t z) { return z == 77; });
        bool const activePointDanger = r.map == 530 && r.x == -600 && r.y == 0;
        return crosses || activePointDanger;
    };
    Route const* hub = PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 15, 7, 530, 0, 0, unsafe);
    ASSERT_NE(hub, nullptr);
    EXPECT_EQ(hub->inn, 1005U);
    EXPECT_EQ(checked, 3);  // three unique eligible hubs, despite repeated route rows
    EXPECT_EQ(PickEscapeOrSafeSameZoneHub(nullptr, routes, 1, 15, 7, 530, 0, 0,
                                          [](Route const&) { return true; }),
              nullptr);
}

TEST(ZoneProgression, CrossZoneAndOrdinaryRoutingStillWinAtHellfireLevel60)
{
    std::vector<Route> const routes = OutlandTable();
    Route const* escape = PickSafeEscapeRoute(routes, 1, 60, kHellfireZone, kOutlandMap, -2570, 3917,
                                               [](Route const&) { return false; });
    ASSERT_NE(escape, nullptr);
    EXPECT_EQ(escape->to, 3521U);
    int fallbackChecks = 0;
    Route const* chosen = PickEscapeOrSafeSameZoneHub(escape, routes, 1, 60, kHellfireZone, kOutlandMap, -2570,
                                                      3917,
                                                      [&](Route const&)
                                                      {
                                                          ++fallbackChecks;
                                                          return false;
                                                      });
    EXPECT_EQ(chosen, escape);
    EXPECT_EQ(fallbackChecks, 0);

    Route const* ordinary = PickRoute(routes, 1, kHellfireZone, 60, 0);
    ASSERT_NE(ordinary, nullptr);
    EXPECT_EQ(ordinary->to, 3521U);
}

// soak-s76: the L58-67 cohort bots stood in Silithus (1377), Winterspring (618), Feralas (357), Un'Goro (490)
// and Eastern Plaguelands (139). Every one of them now has exactly one kind of way on: its Hellfire hub.
TEST(ZoneProgression, OutlandEntryFromEveryAzerothHubZoneAt58To70)
{
    std::vector<Route> const routes = OutlandTable();
    for (std::uint32_t team : {1U, 2U})
        for (HubSource const& s : OutlandEntryZones(team))
            for (std::uint32_t level = kOutlandMinLevel; level <= kOutlandMaxLevel; ++level)
                for (std::uint32_t guid : {62955U, 62961U, 62980U})
                {
                    Route const* r = PickRoute(routes, team, s.zone, level, guid);
                    ASSERT_NE(r, nullptr) << team << " " << s.zone << " L" << level;
                    EXPECT_EQ(r->to, kHellfireZone);
                    EXPECT_EQ(r->map, kOutlandMap);
                    EXPECT_TRUE(r->crossing);
                    EXPECT_TRUE(IsOutlandEntry(*r));
                    EXPECT_EQ(r->inn, team == 1 ? 16826U : 16602U);  // Honor Hold / Thrallmar
                }
    // The soak-s76 bots by name: 62961 (dwarf L67, Feralas), 62980 (orc L65, Silithus), 62955 (human L64, EPL).
    EXPECT_EQ(PickRoute(routes, 1, 357, 67, 62961)->inn, 16826U);
    EXPECT_EQ(PickRoute(routes, 2, 1377, 65, 62980)->inn, 16602U);
    EXPECT_EQ(PickRoute(routes, 1, 139, 64, 62955)->inn, 16826U);
    // Below 58 the Azeroth ladder is unchanged in kind: same continent, no crossing.
    Route const* r = PickRoute(routes, 1, 139, 57, 62955);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->map, 0U);
    EXPECT_FALSE(IsOutlandEntry(*r));
}

TEST(ZoneProgression, OutlandLadderClampsAzerothBandsOnly)
{
    std::vector<Route> const high = HighTable();
    std::vector<Route> const routes = OutlandTable();
    ASSERT_GT(routes.size(), high.size());
    for (std::size_t k = 0; k < high.size(); ++k)
    {
        EXPECT_EQ(routes[k].from, high[k].from);
        EXPECT_EQ(routes[k].to, high[k].to);
        EXPECT_EQ(routes[k].minLevel, high[k].minLevel);
        EXPECT_EQ(routes[k].maxLevel, std::min(high[k].maxLevel, kOutlandMinLevel - 1));
    }
    for (std::size_t k = high.size(); k < routes.size(); ++k)
    {
        EXPECT_EQ(routes[k].map, kOutlandMap);
        EXPECT_GE(routes[k].minLevel, kOutlandMinLevel);
        EXPECT_LE(routes[k].maxLevel, kOutlandMaxLevel);
        EXPECT_NE(routes[k].from, routes[k].to);
    }
    // Deterministic: same table twice.
    std::vector<Route> const again = OutlandTable();
    ASSERT_EQ(again.size(), routes.size());
    for (std::size_t k = 0; k < routes.size(); ++k)
        EXPECT_TRUE(again[k].from == routes[k].from && again[k].to == routes[k].to && again[k].x == routes[k].x);
}

// Inside Outland: every hub zone (and Shattrath) has a way on from its band max until 69, on map 530, walkable.
TEST(ZoneProgression, OutlandLadderClimbsTo70)
{
    std::vector<Route> const routes = OutlandTable();
    for (Hub const& h : OutlandHubs())
        for (std::uint32_t level = h.maxLevel; level < kOutlandMaxLevel; ++level)
        {
            Route const* r = PickRoute(routes, h.team, h.zone, level, h.npc);
            ASSERT_NE(r, nullptr) << h.team << " " << h.zone << " L" << level;
            EXPECT_NE(r->to, h.zone);
            EXPECT_EQ(r->map, kOutlandMap);
            EXPECT_FALSE(r->crossing);
            EXPECT_TRUE(level >= r->minLevel && level <= r->maxLevel);
        }
    // The brief's ladder: Hellfire L61 -> Zangarmarsh only; Shattrath L58 -> Hellfire.
    EXPECT_EQ(PickRoute(routes, 1, 3483, 61, 0)->to, 3521U);
    EXPECT_EQ(PickRoute(routes, 2, 3483, 61, 1)->to, 3521U);
    EXPECT_EQ(PickRoute(routes, 2, 3703, 58, 0)->to, kHellfireZone);
    // No band past 70 (no Northrend ladder).
    for (Route const& x : routes)
        EXPECT_LE(x.maxLevel, kOutlandMaxLevel);
}

TEST(ZoneProgression, NorthrendAdmissionCoversCapitalsAndWholeOutlandFrontier)
{
    std::vector<Route> const admissions = NorthrendEntryRoutes(80, 68);
    for (std::uint32_t team : {1U, 2U})
        for (HubSource const& source : NorthrendEntryZones(team))
        {
            Route const* r = PickRoute(admissions, team, source.zone, 74, 62955);
            ASSERT_NE(r, nullptr) << team << " " << source.zone;
            EXPECT_TRUE(IsNorthrendEntry(*r));
            EXPECT_EQ(r->to, kBoreanZone);
            EXPECT_EQ(r->map, kNorthrendMap);
            EXPECT_EQ(r->minLevel, 68U);
            EXPECT_EQ(r->maxLevel, 80U);
        }
    EXPECT_NE(PickRoute(admissions, 1, kStormwindZone, 68, 1), nullptr);
    EXPECT_NE(PickRoute(admissions, 2, kOrgrimmarZone, 80, 1), nullptr);
    EXPECT_EQ(PickRoute(admissions, 1, kStormwindZone, 67, 1), nullptr);
    EXPECT_EQ(PickRoute(admissions, 2, kOrgrimmarZone, 81, 1), nullptr);
}

TEST(ZoneProgression, NorthrendAdmissionBoundsAreSeparateFromBoreanQuestBand)
{
    Hub const alliance = NorthrendArrivalHub(1);
    Hub const horde = NorthrendArrivalHub(2);
    EXPECT_EQ(alliance.minLevel, 68U);
    EXPECT_EQ(alliance.maxLevel, 75U);
    EXPECT_EQ(horde.minLevel, 68U);
    EXPECT_EQ(horde.maxLevel, 75U);

    std::vector<Route> const baseline = OutlandTable();
    std::vector<Route> routes = baseline;
    AddNorthrend(routes, 80, 68);
    ASSERT_GE(routes.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i)
    {
        EXPECT_EQ(routes[i].minLevel, baseline[i].minLevel);
        EXPECT_EQ(routes[i].maxLevel, baseline[i].maxLevel);
    }
    for (std::uint32_t team : {1U, 2U})
        for (HubSource const& source : NorthrendEntryZones(team))
            for (std::uint32_t level = 68; level <= 80; ++level)
                for (std::uint32_t guid : {0U, 1U, 62961U})
                {
                    Route const* selected = PickRoute(routes, team, source.zone, level, guid);
                    ASSERT_NE(selected, nullptr) << team << " " << source.zone << " " << level << " " << guid;
                    EXPECT_TRUE(IsNorthrendEntry(*selected));
                }
}

TEST(ZoneProgression, NorthrendAdmissionNeverEntersAnyEscapePicker)
{
    std::vector<Route> const baseline = OutlandTable();
    std::vector<Route> enabled = baseline;
    AddNorthrend(enabled, 80, 68);
    auto same = [](Route const* a, Route const* b)
    {
        return a && b && a->team == b->team && a->to == b->to && a->map == b->map &&
               a->x == b->x && a->y == b->y && a->z == b->z && a->inn == b->inn;
    };
    auto safe = [](Route const&) { return false; };
    for (std::uint32_t team : {1U, 2U})
        for (HubSource const& source : NorthrendEntryZones(team))
        {
            if (source.map != kOutlandMap)
                continue;
            for (std::uint32_t level = 68; level <= 70; ++level)
            {
                Route const* baseNearest = PickEscapeRoute(baseline, team, level, source.zone, source.map, 0, 0);
                Route const* newNearest = PickEscapeRoute(enabled, team, level, source.zone, source.map, 0, 0);
                ASSERT_TRUE(same(baseNearest, newNearest)) << team << " " << source.zone << " " << level;
                EXPECT_FALSE(IsNorthrendEntry(*newNearest));
                EXPECT_EQ(newNearest->map, kOutlandMap);

                Route const* baseLow = PickLowEscapeRoute(baseline, team, level, source.zone, source.map, 0, 0);
                Route const* newLow = PickLowEscapeRoute(enabled, team, level, source.zone, source.map, 0, 0);
                ASSERT_TRUE(same(baseLow, newLow));
                EXPECT_FALSE(IsNorthrendEntry(*newLow));

                Route const* baseSafe =
                    PickSafeEscapeRoute(baseline, team, level, source.zone, source.map, 0, 0, safe);
                Route const* newSafe =
                    PickSafeEscapeRoute(enabled, team, level, source.zone, source.map, 0, 0, safe);
                ASSERT_TRUE(same(baseSafe, newSafe));
                EXPECT_FALSE(IsNorthrendEntry(*newSafe));
            }
        }
    Route const* north = PickRoute(enabled, 1, kStormwindZone, 68, 0);
    ASSERT_NE(north, nullptr);
    ASSERT_TRUE(IsNorthrendEntry(*north));
    EXPECT_EQ(PickEscapeOrSafeSameZoneHub(north, enabled, 1, 68, kBoreanZone, kNorthrendMap,
                                           2200, 5100, safe),
              nullptr);
}

TEST(ZoneProgression, PhysicalNorthrendHubRejectsCustodyAndInvalidThreeDimensionalArrival)
{
    Route const route = NorthrendEntryRoutes(80).front();
    PhysicalHubFacts good{false, false, false, route.map, float(route.x), float(route.y), float(route.z)};
    EXPECT_TRUE(AtPhysicalNorthrendHub(route, good));
    auto bad = good; bad.anyTransport = true; EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; bad.inFlight = true; EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; bad.teleporting = true; EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; ++bad.map; EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; bad.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; bad.z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
    bad = good; bad.z += 3.01f; EXPECT_FALSE(AtPhysicalNorthrendHub(route, bad));
}

// Outland hub npcs: creature_template.faction and FactionTemplate.dbc enemy group (2 alliance, 4 horde).
TEST(ZoneProgression, NoHostileOutlandHubs)
{
    struct Npc { std::uint32_t entry, factionTemplate, enemyGroup; };
    std::vector<Npc> const npcs = {
        {16826, 1667, 4}, {16602, 68, 2},   {18251, 1638, 4}, {18245, 126, 2},  {19296, 1732, 4},
        {18957, 1735, 2}, {18914, 1722, 4}, {18913, 1652, 2}, {19495, 80, 4},   {19470, 1735, 2},
        {19571, 35, 0},   {19352, 1732, 4}, {19319, 1735, 2},
    };
    for (Hub const& h : OutlandHubs())
    {
        auto const it = std::find_if(npcs.begin(), npcs.end(), [&](Npc const& n) { return n.entry == h.npc; });
        ASSERT_NE(it, npcs.end()) << h.npc;
        EXPECT_EQ(it->enemyGroup & (h.team == 1 ? 2U : 4U), 0U) << "hub npc " << h.npc;
        EXPECT_EQ(h.inn, h.npc);
        EXPECT_EQ(h.map, kOutlandMap);
    }
}

TEST(ZoneProgression, OutlandChainPortalsLikeAWalk)
{
    EXPECT_EQ(FallbackMode(Mode::Chain, true), Mode::Walk);
    EXPECT_EQ(FallbackMode(Mode::Chain, false), Mode::Chain);
    EXPECT_EQ(FallbackMode(Mode::Flight, true), Mode::Flight);
    // An entry chain past its clock portals; any other chain never takes the fallback.
    EXPECT_TRUE(PortalFallback(true, FallbackMode(Mode::Chain, true), false, 0, 8, 2400000, 2400000));
    EXPECT_FALSE(PortalFallback(true, FallbackMode(Mode::Chain, true), false, 0, 8, 2399999, 2400000));
    EXPECT_FALSE(PortalFallback(true, FallbackMode(Mode::Chain, false), false, 8, 8, 9999999, 1));
    // Already in Hellfire: no portal.
    EXPECT_FALSE(PortalFallback(true, FallbackMode(Mode::Chain, true), true, 8, 8, 9999999, 1));
}

// ---- AutoWow.ZoneProgression.Northrend2 ----------------------------------------------------------------
std::vector<Route> Northrend2Table()
{
    std::vector<Route> routes = DefaultRoutes();
    for (Route const& r : HubRoutes(DefaultHubs(), DefaultHubSources()))
        routes.push_back(r);
    AddOutland(routes);
    AddNorthrend(routes, 80);
    AddNorthrend2(routes, 80);
    return routes;
}

// soak-s107b: L70+ bots stranded in STV / Duskwood / Deadwind / Westfall / Elwynn / Blasted Lands. Each stages
// to its team's chain start on the same continent; Alliance on Kalimdor crosses to Stormwind.
TEST(ZoneProgression, Northrend2StagesStrandedAzerothBotsToTheirChainStart)
{
    Route r;
    for (std::uint32_t zone : {33U, 10U, 41U, 40U, 12U, 4U})
    {
        ASSERT_TRUE(NorthrendStagingRoute(1, 0, zone, 70, 80, 0, r)) << zone;
        EXPECT_EQ(r.from, zone);
        EXPECT_EQ(r.to, kStormwindZone);
        EXPECT_EQ(r.map, 0U);
        EXPECT_EQ(r.inn, 6740U);
        EXPECT_FALSE(r.crossing);
        EXPECT_TRUE(IsNorthrendStaging(r));
        EXPECT_FALSE(IsNorthrendEntry(r));
    }
    for (std::uint32_t zone : {10U, 41U, 12U, 4U})
    {
        ASSERT_TRUE(NorthrendStagingRoute(2, 0, zone, 72, 80, 0, r)) << zone;
        EXPECT_EQ(r.to, kStranglethornZone);
        EXPECT_EQ(r.inn, 5814U);
        EXPECT_FALSE(r.crossing);
    }
    ASSERT_TRUE(NorthrendStagingRoute(2, 1, 440, 70, 80, 0, r));  // Tanaris
    EXPECT_EQ(r.to, kOrgrimmarZone);
    EXPECT_FALSE(r.crossing);
    ASSERT_TRUE(NorthrendStagingRoute(1, 1, 440, 70, 80, 0, r));
    EXPECT_EQ(r.to, kStormwindZone);
    EXPECT_TRUE(r.crossing);
    EXPECT_EQ(r.minLevel, kNorthrendMinLevel);
    EXPECT_EQ(r.maxLevel, 80U);
}

TEST(ZoneProgression, Northrend2StagingPrefersAReadyHearthBoundInAStagingZone)
{
    Route r;
    // Horde in Duskwood bound in Orgrimmar: hearth there (crossing, the hearth leg crosses it).
    ASSERT_TRUE(NorthrendStagingRoute(2, 0, 10, 70, 80, kOrgrimmarZone, r));
    EXPECT_EQ(r.to, kOrgrimmarZone);
    EXPECT_EQ(r.map, 1U);
    EXPECT_TRUE(r.crossing);
    // A hearth bound elsewhere (Booty Bay, or the other team's capital) changes nothing.
    ASSERT_TRUE(NorthrendStagingRoute(2, 0, 10, 70, 80, 33, r));
    EXPECT_EQ(r.to, kStranglethornZone);
    ASSERT_TRUE(NorthrendStagingRoute(2, 0, 10, 70, 80, kStormwindZone, r));
    EXPECT_EQ(r.to, kStranglethornZone);
    ASSERT_TRUE(NorthrendStagingRoute(1, 1, 440, 70, 80, 1537, r));
    EXPECT_EQ(r.to, kStormwindZone);
}

TEST(ZoneProgression, Northrend2NoStagingAtAChainStartBelow68OrOffAzeroth)
{
    Route r;
    r.from = 777;
    EXPECT_FALSE(NorthrendStagingRoute(1, 0, kStormwindZone, 70, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(2, 1, kOrgrimmarZone, 70, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(2, 0, kStranglethornZone, 70, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(2, 1, kDurotarZone, 70, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(1, 0, 10, 67, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(1, 530, 3483, 70, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(1, kNorthrendMap, kBoreanZone, 72, 80, 0, r));
    EXPECT_FALSE(NorthrendStagingRoute(1, 0, 10, 70, 69, 0, r));  // server cap below the bot
    EXPECT_EQ(r.from, 777U);  // untouched
    // Alliance Stranglethorn is no Alliance chain start: it stages.
    EXPECT_TRUE(NorthrendStagingRoute(1, 0, kStranglethornZone, 70, 80, 0, r));
    // Ordinary low hub routes are never staging trips.
    for (Route const& h : HubRoutes(DefaultHubs(), DefaultHubSources()))
        EXPECT_FALSE(IsNorthrendStaging(h));
}

// Every staging destination is a chain start: the admission route leaves it next (PickRoute priority).
TEST(ZoneProgression, Northrend2StagingDestinationsAdmitNext)
{
    std::vector<Route> const routes = Northrend2Table();
    for (Hub const& h : NorthrendStagingHubs())
    {
        Route const* next = PickRoute(routes, h.team, h.zone, 70, 1);
        ASSERT_NE(next, nullptr) << h.zone;
        EXPECT_TRUE(IsNorthrendEntry(*next)) << h.zone;
    }
    Route const* durotar = PickRoute(routes, 2, kDurotarZone, 68, 3);
    ASSERT_NE(durotar, nullptr);
    EXPECT_TRUE(IsNorthrendEntry(*durotar));
    // Alliance gains no Stranglethorn / Durotar admission.
    Route const* stv = PickRoute(routes, 1, kStranglethornZone, 70, 3);
    EXPECT_TRUE(!stv || !IsNorthrendEntry(*stv));
}

// The Northrend ladder: every level 68-80 has an own-faction hub reachable from Borean on map 571.
TEST(ZoneProgression, Northrend2LadderCoversLevels68To80PerFaction)
{
    std::vector<Route> const routes = Northrend2Table();
    for (std::uint32_t team : {1U, 2U})
        for (std::uint32_t level = 68; level <= 80; ++level)
        {
            bool any = false;
            for (Route const& r : routes)
                if (r.team == team && r.map == kNorthrendMap && !IsNorthrendEntry(r) && level >= r.minLevel &&
                    level <= r.maxLevel)
                    any = true;
            EXPECT_TRUE(any) << team << " L" << level;
        }
    // Borean leads on up the ladder; a top stage never leads back down.
    Route const* up = PickRoute(routes, 1, kBoreanZone, 73, 0);
    ASSERT_NE(up, nullptr);
    EXPECT_NE(up->to, kBoreanZone);
    EXPECT_EQ(up->map, kNorthrendMap);
    for (Route const& r : HubRoutes(NorthrendHubs(), {}))
        EXPECT_FALSE(r.from == 210 && r.to == kBoreanZone);
}

// Northrend hub npcs: creature_template.faction and FactionTemplate.dbc enemy group (2 alliance, 4 horde, 0 neutral);
// zone = map-grid area of the spawn (p1data maps + AreaTable.dbc).
TEST(ZoneProgression, NoHostileNorthrendHubs)
{
    struct Npc { std::uint32_t entry, factionTemplate, enemyGroup; };
    std::vector<Npc> const npcs = {
        {25245, 1973, 4}, {25278, 1978, 2}, {23731, 1892, 4}, {24342, 1929, 2}, {27052, 1892, 4},
        {26985, 1980, 2}, {27066, 1892, 4}, {27125, 1981, 2}, {28791, 2070, 0}, {28038, 35, 0},
        {29926, 1926, 4}, {29944, 1978, 2}, {33970, 2025, 4}, {33971, 2123, 2},
    };
    for (Hub const& h : NorthrendHubs())
    {
        auto const it = std::find_if(npcs.begin(), npcs.end(), [&](Npc const& n) { return n.entry == h.npc; });
        ASSERT_NE(it, npcs.end()) << h.npc;
        EXPECT_EQ(it->enemyGroup & (h.team == 1 ? 2U : 4U), 0U) << "hub npc " << h.npc;
        EXPECT_EQ(h.inn, h.npc);
        EXPECT_EQ(h.map, kNorthrendMap);
    }
    // Staging inns: Allison (12, alliance), Gryshka / Thulbek (29, horde).
    for (Hub const& h : NorthrendStagingHubs())
        EXPECT_EQ(h.inn, h.team == 1 ? 6740U : (h.zone == kOrgrimmarZone ? 6929U : 5814U));
}

// soak-s107: the lowest-band death-loop escape sent L68-70 Northrend bots back to Hellfire. Northrend2 keeps the
// escape on the bot's own map.
TEST(ZoneProgression, Northrend2DeathLoopEscapeStaysOnTheBotsMap)
{
    std::vector<Route> const routes = Northrend2Table();
    Route const* legacy = PickLowEscapeRoute(routes, 1, 69, 3537, kNorthrendMap, 2282, 5210);
    ASSERT_NE(legacy, nullptr);
    EXPECT_NE(legacy->map, kNorthrendMap);  // the defect this flag fixes
    std::vector<Route> const same = SameMapRoutes(routes, kNorthrendMap);
    Route const* hub = PickLowEscapeRoute(same, 1, 69, 3537, kNorthrendMap, 2282, 5210);
    ASSERT_NE(hub, nullptr);
    EXPECT_EQ(hub->map, kNorthrendMap);
    EXPECT_EQ(hub->to, 495U);  // Howling Fjord, the other 68-72 hub
    for (Route const& r : same)
        EXPECT_EQ(r.map, kNorthrendMap);
}

TEST(ZoneProgression, Northrend2HearthModeIsWireAppended)
{
    EXPECT_EQ(static_cast<std::uint32_t>(Mode::Hearth), 5U);
    EXPECT_STREQ(ModeName(Mode::Hearth), "hearth");
    EXPECT_FALSE(PortalFallback(true, Mode::Hearth, false, 99, 8, 9999999, 1));
}
}  // namespace
