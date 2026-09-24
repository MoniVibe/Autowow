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
}  // namespace
