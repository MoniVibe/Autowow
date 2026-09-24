/*
 * Pure chunked-walk tests (WalkingV2Policy.h, AutoWow.Walking.V2) plus the V2 road join
 * (ZoneProgressionPolicy.h JoinRoadAhead). No world access: an ideal-navmesh oracle stands in for
 * PathGenerator, so a deterministic walk over a recorded road can be replayed step by step.
 */

#include "TravelIntentPolicy.h"
#include "WalkingV2Policy.h"
#include "ZoneProgressionPolicy.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

namespace
{
using TravelIntentPolicy::DistanceYards;
using TravelIntentPolicy::Point;
using namespace WalkingV2Policy;

Point P(std::int32_t x, std::int32_t y) { return Point{0, x, y, 0}; }

TEST(WalkingV2PolicyTest, ChunkProbesAreFixedIntegerFan)
{
    Point const here = P(0, 0);
    Point const goal = P(1000, 0);
    EXPECT_EQ(ChunkProbe(here, goal, 0).x, 120);
    EXPECT_EQ(ChunkProbe(here, goal, 0).y, 0);
    EXPECT_EQ(ChunkProbe(here, goal, 1).x, 103);  // +30 deg, truncated
    EXPECT_EQ(ChunkProbe(here, goal, 1).y, 60);
    EXPECT_EQ(ChunkProbe(here, goal, 2).y, -60);  // -30 deg
    EXPECT_EQ(ChunkProbe(here, goal, 5).x, 0);    // +90 deg
    EXPECT_EQ(ChunkProbe(here, goal, 5).y, 120);
    EXPECT_EQ(ChunkProbe(here, goal, 7).x, 60);   // short ring
    EXPECT_EQ(ChunkProbe(here, goal, 7).y, 0);
    // Negative bearings truncate toward zero, symmetric with the positive case.
    EXPECT_EQ(ChunkProbe(here, P(-1000, 0), 1).x, -103);
    EXPECT_EQ(ChunkProbe(here, P(-1000, 0), 1).y, -60);
    // Degenerate inputs stay put.
    EXPECT_EQ(ChunkProbe(here, here, 0).x, 0);
    EXPECT_EQ(ChunkProbe(here, Point{1, 50, 50, 0}, 0).x, 0);
    EXPECT_EQ(ChunkProbe(here, goal, kChunkProbes).x, 0);
}

TEST(WalkingV2PolicyTest, ChunksOnlyBeyondTheFirstRing)
{
    EXPECT_FALSE(UseChunks(P(0, 0), P(120, 0)));
    EXPECT_TRUE(UseChunks(P(0, 0), P(121, 0)));
    EXPECT_FALSE(UseChunks(P(0, 0), Point{1, 500, 0, 0}));
}

TEST(WalkingV2PolicyTest, AdmitChunkIsGoalMonotone)
{
    Point const here = P(0, 0);
    Point const goal = P(1000, 0);
    EXPECT_TRUE(AdmitChunk(here, P(120, 0), goal, false));
    EXPECT_TRUE(AdmitChunk(here, P(10, 0), goal, false));    // exactly kMinGainYards
    EXPECT_FALSE(AdmitChunk(here, P(9, 0), goal, false));    // too little gain
    EXPECT_FALSE(AdmitChunk(here, P(3, 0), goal, false));    // not a step
    EXPECT_FALSE(AdmitChunk(here, P(-120, 0), goal, false)); // backwards
    EXPECT_FALSE(AdmitChunk(here, P(0, 120), goal, false));  // sideways: no gain
    EXPECT_FALSE(AdmitChunk(here, P(120, 0), goal, true));   // dangerous
}

// The legacy revisit refusal (15 yd of any of the last 8 segment points) rejects a chunk that
// returns near an earlier commit even when it is real progress; goal-monotone admission accepts it.
TEST(WalkingV2PolicyTest, NoRevisitRefusalForGoalProgress)
{
    using namespace TravelIntentPolicy;
    Params const params{5, 90 * 1000, 20};
    Point const goal = P(1000, 0);
    Intent intent;
    (void)Observe(intent, goal, 1000, 1000, params);
    Commit(intent, P(0, 0), P(110, 0), DistanceYards(P(110, 0), goal));
    CompleteSegment(intent);
    // Pushed back by a pull to (100, 0); the next chunk ends 12 yd past the previous endpoint.
    Point const here = P(100, 0);
    Point const candidate = P(122, 0);
    EXPECT_FALSE(Admit(intent, here, candidate, DistanceYards(candidate, goal), false, params));
    EXPECT_TRUE(AdmitChunk(here, candidate, goal, false));
}

TEST(WalkingV2PolicyTest, JoinRoadAheadSkipsAPointBehind)
{
    using namespace AutoWowZoneProgression;
    std::vector<RoadPoint> const dunMorogh = RoadFor(DefaultRoads(), 1, 38);
    ASSERT_EQ(dunMorogh.size(), 8u);
    // soak-s14 Heddabrand at (-6080,389): the nearest point is Coldridge Valley, behind him.
    EXPECT_EQ(JoinRoad(dunMorogh, -6080, 389), 0u);
    EXPECT_EQ(JoinRoadAhead(dunMorogh, -6080, 389), 1u);
    // Deep in the valley the first point is still ahead.
    EXPECT_EQ(JoinRoadAhead(dunMorogh, -6400, 550), 0u);
    // Elwynn mid-route past Westbrook joins Jansen Stead.
    std::vector<RoadPoint> const elwynn = RoadFor(DefaultRoads(), 12, 40);
    EXPECT_EQ(JoinRoad(elwynn, -9700, 800), 1u);
    EXPECT_EQ(JoinRoadAhead(elwynn, -9700, 800), 2u);
    // The last point never steps past the end.
    EXPECT_EQ(JoinRoadAhead(elwynn, -9789, 990), 2u);
    EXPECT_EQ(JoinRoadAhead({}, 0, 0), 0u);
}

// Deterministic replay: the zone-progression walk target (JoinRoadAhead + AdvanceRoad 20 yd) driven by
// the V2 chunk planner against an ideal navmesh that refuses probes whose straight segment crosses a
// blocked disc. Arrival, per-step goal monotonicity and a bounded step count are pinned.
struct Disc
{
    std::int64_t x, y, r;
};

bool Crosses(Point const& a, Point const& b, Disc const& d)
{
    // Closest point of segment a-b to the disc centre (long double: test oracle only, not policy).
    std::int64_t const vx = std::int64_t(b.x) - a.x, vy = std::int64_t(b.y) - a.y;
    std::int64_t const wx = d.x - a.x, wy = d.y - a.y;
    std::int64_t const vv = vx * vx + vy * vy;
    std::int64_t t = vv == 0 ? 0 : (wx * vx + wy * vy);
    if (t < 0)
        t = 0;
    if (t > vv)
        t = vv;
    long double const cx = static_cast<long double>(vx) * t, cy = static_cast<long double>(vy) * t;
    long double const ex = static_cast<long double>(wx) * vv - cx, ey = static_cast<long double>(wy) * vv - cy;
    long double const lhs = ex * ex + ey * ey;
    long double const rhs = static_cast<long double>(d.r) * d.r * vv * vv;
    return lhs <= rhs;
}

struct WalkResult
{
    bool arrived = false;
    std::uint32_t segments = 0;
    std::uint32_t firstTarget = 0;
    bool monotone = true;
};

WalkResult ReplayRoadWalk(std::vector<AutoWowZoneProgression::RoadPoint> const& road, Point start, Point hub,
                          std::vector<Disc> const& blocked)
{
    using namespace AutoWowZoneProgression;
    WalkResult out;
    Point here = start;
    std::uint32_t wp = JoinRoadAhead(road, here.x, here.y);
    out.firstTarget = wp;
    for (int step = 0; step < 400; ++step)
    {
        wp = AdvanceRoad(road, wp, here.x, here.y, 20);
        Point const target = wp >= road.size() ? hub : P(road[wp].x, road[wp].y);
        if (wp >= road.size() && DistanceYards(here, hub) < 15)
        {
            out.arrived = true;
            return out;
        }
        if (!UseChunks(here, target) || DistanceYards(here, target) < 70)
        {
            here = target;  // final approach (pathFinderDis): the direct MoveTo
            ++out.segments;
            continue;
        }
        bool moved = false;
        for (std::size_t k = 0; k < kChunkProbes && !moved; ++k)
        {
            Point const probe = ChunkProbe(here, target, k);
            bool clear = true;
            for (Disc const& d : blocked)
                clear = clear && !Crosses(here, probe, d);
            if (!clear || !AdmitChunk(here, probe, target, false))
                continue;
            out.monotone = out.monotone && DistanceYards(probe, target) < DistanceYards(here, target);
            here = probe;
            moved = true;
            ++out.segments;
        }
        if (!moved)
            return out;  // a failed replan: the live mover would charge the replan budget
    }
    return out;
}

TEST(WalkingV2PolicyTest, ReplayDunMoroghRoadToThelsamar)
{
    using namespace AutoWowZoneProgression;
    std::vector<RoadPoint> const road = RoadFor(DefaultRoads(), 1, 38);
    // soak-s14 Heddabrand graduation start; hub = Thelsamar inn (route 1 -> 38).
    WalkResult const r = ReplayRoadWalk(road, P(-6080, 389), P(-5378, -2974), {});
    EXPECT_TRUE(r.arrived);
    EXPECT_TRUE(r.monotone);
    EXPECT_EQ(r.firstTarget, 1u);
    EXPECT_LE(r.segments, 40u);  // ~3700 yd of road in <= 120 yd chunks plus final approaches
}

TEST(WalkingV2PolicyTest, ReplayFansAroundABlockedBearing)
{
    // A straight 1000 yd errand walk with a 40 yd rock 100 yd ahead of the start on the bearing.
    WalkResult const r = ReplayRoadWalk({}, P(0, 0), P(1000, 0), {Disc{100, 0, 40}});
    EXPECT_TRUE(r.arrived);
    EXPECT_TRUE(r.monotone);
    EXPECT_LE(r.segments, 12u);
    // With the whole forward half-plane walled off at 30 yd the planner reports a failed replan.
    WalkResult const walled = ReplayRoadWalk({}, P(0, 0), P(1000, 0), {Disc{60, 0, 59}});
    EXPECT_FALSE(walled.arrived);
    EXPECT_EQ(walled.segments, 0u);
}
// ---- AutoWow.Travel.Safe: aggro-aware chunk choice ------------------------------------------------
TEST(WalkingV2PolicyTest, Dist2ToSegmentIsIntegerAndClamped)
{
    EXPECT_EQ(Dist2ToSegment(5, 3, P(0, 0), P(10, 0)), 9);     // beside the segment
    EXPECT_EQ(Dist2ToSegment(-4, 3, P(0, 0), P(10, 0)), 25);   // before a: distance to a
    EXPECT_EQ(Dist2ToSegment(13, 4, P(0, 0), P(10, 0)), 25);   // past b: distance to b
    EXPECT_EQ(Dist2ToSegment(3, 4, P(0, 0), P(0, 0)), 25);     // degenerate segment
    EXPECT_EQ(Dist2ToSegment(1, 2, P(0, 0), P(3, 3)), 0);      // 1/sqrt2 truncated: 0.5 -> 0
    // World-scale coordinates do not overflow.
    EXPECT_EQ(Dist2ToSegment(0, 20000, P(-17000, 0), P(17000, 0)), 400000000);
}

TEST(WalkingV2PolicyTest, PathThreatCountsThreateningMobsOnce)
{
    std::vector<Point> const path{P(0, 0), P(50, 0), P(100, 0)};
    std::vector<Mob> const mobs{
        {50, 20, 12, false, 15},   // at level, 20 yd off, reach 15+5: counted once (two segments touch)
        {50, 21, 12, false, 15},   // 21 yd off: just out of reach
        {80, 5, 10, false, 20},    // below the bot's level: ignored
        {80, 5, 8, true, 20},      // elite of any level: counted
        {-30, 0, 20, false, 24},   // before the path start: reach 29 < 30, out
        {-29, 0, 20, false, 24}};  // reach 29 covers the start point
    EXPECT_EQ(PathThreat(path, mobs, 12), 3u);
    EXPECT_EQ(PathThreat({}, mobs, 12), 0u);
    EXPECT_EQ(PathThreat({P(50, 0)}, mobs, 12), 1u);  // single point: only the first reaches it
    EXPECT_TRUE(Threatens(Mob{0, 0, 12, false, 0}, 12));
    EXPECT_FALSE(Threatens(Mob{0, 0, 11, false, 0}, 12));
    EXPECT_TRUE(Threatens(Mob{0, 0, 1, true, 0}, 12));
}

TEST(WalkingV2PolicyTest, PickChunkPrefersFirstClearThenLeastThreat)
{
    EXPECT_EQ(PickChunk({}), kNoChunk);
    EXPECT_EQ(PickChunk({{false, 0}, {false, 0}}), kNoChunk);
    EXPECT_EQ(PickChunk({{true, 2}, {false, 0}, {true, 0}, {true, 0}}), 2u);  // first clear, fan order
    EXPECT_EQ(PickChunk({{true, 3}, {true, 1}, {true, 1}, {false, 0}}), 1u); // least threat, earlier on tie
    EXPECT_EQ(PickChunk({{true, 0}}), 0u);
}

// The V2 chunk planner on the ideal navmesh (straight probe paths), with or without the safe choice.
// `touches` = committed chunks whose straight path came within aggro reach of a threatening mob.
struct SafeWalkResult
{
    bool arrived = false;
    std::uint32_t segments = 0;
    std::uint32_t touches = 0;
};

SafeWalkResult ReplaySafeWalk(Point start, Point goal, std::vector<Mob> const& mobs, std::uint32_t botLevel, bool safe)
{
    SafeWalkResult out;
    Point here = start;
    for (int step = 0; step < 100; ++step)
    {
        if (!UseChunks(here, goal))
        {
            out.touches += PathThreat({here, goal}, mobs, botLevel) ? 1 : 0;
            ++out.segments;
            out.arrived = true;
            return out;
        }
        std::vector<ChunkChoice> choices;
        std::vector<Point> ends;
        for (std::size_t k = 0; k < kChunkProbes; ++k)
        {
            Point const probe = ChunkProbe(here, goal, k);
            ends.push_back(probe);
            choices.push_back({AdmitChunk(here, probe, goal, false), 0});
            if (!choices.back().admissible)
                continue;
            if (!safe)
                break;  // step 0: the first admissible chunk
            choices.back().threat = PathThreat({here, probe}, mobs, botLevel);
            if (choices.back().threat == 0)
                break;
        }
        std::size_t const pick = PickChunk(choices);
        if (pick == kNoChunk)
            return out;
        out.touches += choices[pick].threat || (!safe && PathThreat({here, ends[pick]}, mobs, botLevel)) ? 1 : 0;
        here = ends[pick];
        ++out.segments;
    }
    return out;
}

TEST(WalkingV2PolicyTest, SafeReplayWalksAroundACampOnTheBearing)
{
    // soak-s16 shape: a 1000 yd errand walk whose straight bearing crosses a camp of three at-level mobs
    // (aggro 20) 180 yd out; a lower-level mob sits on the detour and is ignored.
    std::vector<Mob> const camp{{180, 0, 12, false, 20}, {190, 10, 12, false, 20}, {185, -10, 13, false, 20},
                                {230, 55, 9, false, 20}};
    SafeWalkResult const plain = ReplaySafeWalk(P(0, 0), P(1000, 0), camp, 12, false);
    SafeWalkResult const safe = ReplaySafeWalk(P(0, 0), P(1000, 0), camp, 12, true);
    EXPECT_TRUE(plain.arrived);
    EXPECT_GE(plain.touches, 1u);
    EXPECT_TRUE(safe.arrived);
    EXPECT_EQ(safe.touches, 0u);
    EXPECT_LE(safe.segments, plain.segments + 2);
    // Deterministic: the same inputs replay the same walk.
    SafeWalkResult const again = ReplaySafeWalk(P(0, 0), P(1000, 0), camp, 12, true);
    EXPECT_EQ(again.segments, safe.segments);
    // A wall of mobs across the whole route: no clear chunk crosses it, so the least threatened one is
    // taken (the trip never stalls on mobs).
    std::vector<Mob> wall;
    for (std::int32_t y = -1000; y <= 1000; y += 10)
        wall.push_back({125, y, 12, false, 20});
    SafeWalkResult const forced = ReplaySafeWalk(P(0, 0), P(1000, 0), wall, 12, true);
    EXPECT_TRUE(forced.arrived);
    EXPECT_GE(forced.touches, 1u);
}

// ---- AutoWow.Survival.HardEscape (2) --------------------------------------------------------------------
TEST(WalkingV2PolicyTest, ZoneDangerIsAHighZoneOtherThanTheBotsOwn)
{
    // soak-s22-full-r1: Taelorin, L18, Redridge (44) -> Burning Steppes (46, bracket low 51).
    EXPECT_TRUE(ZoneDanger(46, 44, 51, 18, 5));
    EXPECT_FALSE(ZoneDanger(46, 46, 51, 18, 5));  // already inside: walking out is allowed
    EXPECT_FALSE(ZoneDanger(46, 44, 51, 50, 5));  // a L50 may go (51 <= 55)
    EXPECT_FALSE(ZoneDanger(10, 44, 19, 18, 5));  // Duskwood 19 <= 23
    EXPECT_TRUE(ZoneDanger(10, 44, 24, 18, 5));   // strictly above level + margin
    EXPECT_FALSE(ZoneDanger(0, 44, 60, 18, 5));   // no zone
    EXPECT_FALSE(ZoneDanger(41, 44, 0, 18, 5));   // unknown bracket: never
}

// Walks a chunk replay (ideal navmesh: each probe is its own endpoint; endpoint and chunk midpoint are
// zone-checked as the Travel.Safe path sampling does) with zone 46 (low 51) in a box, zone 44 elsewhere.
struct ZoneWalkResult
{
    bool arrived = false;
    bool enteredDanger = false;
    std::uint32_t segments = 0;
};

ZoneWalkResult ReplayZoneWalk(Point from, Point goal, std::int32_t boxY0, std::int32_t boxY1)
{
    auto zoneOf = [&](Point const& p) -> std::uint32_t
    { return p.x >= 800 && p.x <= 1200 && p.y >= boxY0 && p.y <= boxY1 ? 46 : 44; };
    auto danger = [&](Point const& p, Point const& here)
    { return ZoneDanger(zoneOf(p), zoneOf(here), zoneOf(p) == 46 ? 51 : 16, 18, 5); };
    ZoneWalkResult out;
    Point here = from;
    for (std::uint32_t step = 0; step < 200; ++step)
    {
        if (DistanceYards(here, goal) <= kChunkYards)
        {
            out.arrived = true;
            return out;
        }
        bool moved = false;
        for (std::size_t k = 0; k < kChunkProbes && !moved; ++k)
        {
            Point const c = ChunkProbe(here, goal, k);
            Point const mid = P((here.x + c.x) / 2, (here.y + c.y) / 2);
            if (!AdmitChunk(here, c, goal, danger(c, here) || danger(mid, here)))
                continue;
            here = c;
            ++out.segments;
            out.enteredDanger = out.enteredDanger || zoneOf(here) == 46;
            moved = true;
        }
        if (!moved)
            return out;  // no admissible chunk: the leg stalls (reissue / portal fallback), never beelines
    }
    return out;
}

TEST(WalkingV2PolicyTest, ZoneDangerChunksSkirtOrStallButNeverEnter)
{
    // The box covers the straight line but leaves the north side open: the fan skirts it.
    ZoneWalkResult const skirt = ReplayZoneWalk(P(0, 0), P(2000, 0), -300, 60);
    EXPECT_TRUE(skirt.arrived);
    EXPECT_FALSE(skirt.enteredDanger);
    // A wall across the whole route: no admissible chunk - the walk stops short, it does not cross.
    ZoneWalkResult const wall = ReplayZoneWalk(P(0, 0), P(2000, 0), -5000, 5000);
    EXPECT_FALSE(wall.arrived);
    EXPECT_FALSE(wall.enteredDanger);
    EXPECT_GE(wall.segments, 1u);
    // Deterministic replay.
    EXPECT_EQ(ReplayZoneWalk(P(0, 0), P(2000, 0), -300, 60).segments, skirt.segments);
}
}  // namespace
