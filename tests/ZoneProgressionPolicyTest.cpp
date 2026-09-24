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
}  // namespace
