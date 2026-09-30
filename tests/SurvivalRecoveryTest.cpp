/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurvivalRecovery.h"

#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace
{
using TravelIntentPolicy::DistanceYards;
using TravelIntentPolicy::Point;
using WalkingV2Policy::Mob;

Point P(std::int32_t x, std::int32_t y) { return Point{0, x, y, 0}; }

Mob M(std::int32_t x, std::int32_t y, std::uint32_t level, bool elite = false, std::uint32_t aggro = 10)
{
    Mob m;
    m.x = x;
    m.y = y;
    m.level = level;
    m.elite = elite;
    m.aggroYards = aggro;
    return m;
}

std::vector<AutoWowSafeRevive::Spot> AllRevive(Point const& corpse)
{
    std::vector<AutoWowSafeRevive::Spot> spots;
    for (std::size_t k = 0; k < AutoWowSafeRevive::kReviveCandidates; ++k)
        spots.push_back({AutoWowSafeRevive::ReviveCandidate(corpse, k), true});
    return spots;
}

// ---- SafeRevive ----------------------------------------------------------------------------------------

TEST(SafeRevive, CandidatesStayWithinReclaimReach)
{
    using namespace AutoWowSafeRevive;
    Point const corpse = P(1000, -2000);
    EXPECT_EQ(kReviveCandidates, 17u);
    EXPECT_EQ(ReviveCandidate(corpse, 0).x, 1000);
    EXPECT_EQ(ReviveCandidate(corpse, 1).x, 1000 + kInnerRingYards);   // E, inner
    EXPECT_EQ(ReviveCandidate(corpse, 3).y, -2000 + kInnerRingYards);  // N, inner
    EXPECT_EQ(ReviveCandidate(corpse, 9).x, 1000 + kReachYards);       // E, outer
    EXPECT_EQ(ReviveCandidate(corpse, 13).x, 1000 - kReachYards);      // W, outer
    Point const ne = ReviveCandidate(corpse, 2);                        // 724 * 17 / 1024 = 12
    EXPECT_EQ(ne.x, 1012);
    EXPECT_EQ(ne.y, -1988);
    for (std::size_t k = 0; k < kReviveCandidates; ++k)
        EXPECT_LE(DistanceYards(corpse, ReviveCandidate(corpse, k)), std::uint32_t(kReachYards)) << k;
    EXPECT_EQ(ReviveCandidate(corpse, kReviveCandidates).x, corpse.x);  // out of range: the corpse
}

TEST(SafeRevive, ThreatWeighsElitesAndLevelInsideAggroPlusMargin)
{
    using namespace AutoWowSafeRevive;
    std::uint32_t const lvl = 10;
    EXPECT_EQ(SpotThreat(P(0, 0), {M(15, 0, 9)}, lvl), 1u);        // lower level, 10 + 5 reach: 1
    EXPECT_EQ(SpotThreat(P(0, 0), {M(16, 0, 9)}, lvl), 0u);        // one yard out
    EXPECT_EQ(SpotThreat(P(0, 0), {M(0, 12, 10)}, lvl), 3u);       // at level
    EXPECT_EQ(SpotThreat(P(0, 0), {M(0, -12, 8, true)}, lvl), 6u); // elite
    EXPECT_EQ(SpotThreat(P(0, 0), {M(15, 0, 9), M(0, 12, 10), M(0, -12, 8, true), M(90, 0, 60, true)}, lvl), 10u);
}

// soak-s16-full-r1: the corpse lies in the pack that killed it (killer east, two more at the corpse).
// Legacy reclaims at the corpse among them; the plan takes a clear outer spot on the far side.
TEST(SafeRevive, PicksAClearSpotAwayFromThePack)
{
    using namespace AutoWowSafeRevive;
    Point const corpse = P(0, 0);
    std::vector<Mob> const pack{M(8, 0, 12), M(-3, 6, 11), M(2, -7, 11)};
    std::uint32_t const lvl = 10;
    std::vector<Spot> const spots = AllRevive(corpse);
    EXPECT_GT(SpotThreat(corpse, pack, lvl), 0u);  // the legacy reclaim spot
    Anchors const a{P(-400, 0), P(8, 0), std::nullopt};  // graveyard west, killer east
    Pick const best = PickSpot(spots, pack, lvl, a);
    ASSERT_NE(best.index, kNoSpot);
    EXPECT_EQ(best.threat, 0u);
    EXPECT_EQ(best.index, 13u);  // outer ring, due west: toward the graveyard, away from the killer
    EXPECT_EQ(Decide(best, 2, Params{}), Plan::ReviveAt);  // a safe spot beats the spirit healer
}

TEST(SafeRevive, UnreachableSpotsAreSkippedAndTiesTakeTheLowestIndex)
{
    using namespace AutoWowSafeRevive;
    std::vector<Spot> spots = AllRevive(P(0, 0));
    for (Spot& s : spots)
        s.reachable = false;
    EXPECT_EQ(PickSpot(spots, {}, 10, {}).index, kNoSpot);
    spots[5].reachable = true;
    spots[11].reachable = true;
    EXPECT_EQ(PickSpot(spots, {}, 10, {}).index, 5u);  // no anchors, no mobs: first reachable
    // Replay: same inputs, same pick.
    EXPECT_EQ(PickSpot(spots, {}, 10, {}).index, PickSpot(spots, {}, 10, {}).index);
}

TEST(SafeRevive, SurroundedTakesTheLeastThreatOrTheSpiritHealerOnARepeatDeath)
{
    using namespace AutoWowSafeRevive;
    // Every candidate inside some aggro radius: a ring of at-level mobs with 40 yd reach around the corpse.
    std::vector<Mob> ring;
    for (std::size_t k = 0; k < kBearings.size(); ++k)
    {
        Point const q = Offset(P(0, 0), k, 20);
        ring.push_back(M(q.x, q.y, 10, false, 35));
    }
    std::vector<Spot> const spots = AllRevive(P(0, 0));
    Pick const best = PickSpot(spots, ring, 10, {});
    ASSERT_NE(best.index, kNoSpot);
    EXPECT_GT(best.threat, 0u);
    Params p;  // spirit healer from the 2nd death in the window
    EXPECT_EQ(Decide(best, 1, p), Plan::ReviveAt);
    EXPECT_EQ(Decide(best, 2, p), Plan::SpiritHealer);
    EXPECT_EQ(Decide(Pick{}, 1, p), Plan::None);          // nothing reachable, first death: legacy run
    EXPECT_EQ(Decide(Pick{}, 3, p), Plan::SpiritHealer);  // nothing reachable, repeat death
    p.spiritDeaths = 0;
    EXPECT_EQ(Decide(best, 9, p), Plan::ReviveAt);
    EXPECT_STREQ(PlanName(Plan::SpiritHealer), "spirit_healer");
}

TEST(SafeRevive, RetreatLeavesTheDeathSpotTowardTheGraveyard)
{
    using namespace AutoWowSafeRevive;
    Point const spot = P(0, 0);
    std::vector<Spot> spots;
    for (std::size_t k = 0; k < kRetreatCandidates; ++k)
        spots.push_back({RetreatCandidate(spot, k), true});
    EXPECT_EQ(DistanceYards(spot, spots[1].p), std::uint32_t(kRetreatYards));
    EXPECT_EQ(DistanceYards(spot, spots[0].p), 0u);  // stay
    // Death spot 20 yd east, graveyard far south, one clear mob north-west of the path.
    Anchors const a{P(0, -600), std::nullopt, P(20, 0)};
    Pick const best = PickSpot(spots, {M(-32, 32, 10)}, 10, a);
    EXPECT_EQ(best.threat, 0u);
    EXPECT_EQ(best.index, 7u);  // bearing S (kBearings[6]) -> candidate 7
    // Staying wins only on strictly less threat: every ring point in a camp, the spot itself clear.
    std::vector<Mob> camp;
    for (std::size_t k = 1; k < kRetreatCandidates; ++k)
        camp.push_back(M(spots[k].p.x, spots[k].p.y, 10));
    EXPECT_EQ(PickSpot(spots, camp, 10, a).index, 0u);
}

// ---- Unstick -------------------------------------------------------------------------------------------

TEST(Unstick, FrozenAfterFrozenMsWithinRadius)
{
    using namespace AutoWowUnstick;
    Params p;  // 10 yd, 5 min
    BotState s;
    EXPECT_EQ(s.version, kPolicyVersion);
    std::uint64_t t = 1000;
    for (; t < 1000 + p.frozenMs; t += 1000)
        ASSERT_EQ(Observe(p, s, P(100 + (t / 1000) % 3, 50), t), Trigger::None) << t;  // jitters 0-2 yd
    EXPECT_EQ(Observe(p, s, P(101, 50), t), Trigger::Frozen);
    EXPECT_STREQ(TriggerName(Trigger::Frozen), "frozen");
}

TEST(Unstick, WalkingAwayOrAPausedGoalRestartsTheClock)
{
    using namespace AutoWowUnstick;
    Params p;
    // 200 s still, then 11 yd away: the clock restarts there (from the old anchor it would fire at 300 s).
    BotState s;
    std::uint64_t t = 0;
    for (; t < 200000; t += 10000)
        EXPECT_EQ(Observe(p, s, P(0, 0), t), Trigger::None);
    std::uint64_t const moved = t;
    for (; t < moved + p.frozenMs; t += 10000)
        EXPECT_EQ(Observe(p, s, P(11, 0), t), Trigger::None) << t;
    EXPECT_EQ(Observe(p, s, P(11, 0), t), Trigger::Frozen);

    // Goal ticks every 30 s; one gap over kMaxGapMs (combat, rest, another status) restarts the clock.
    BotState g;
    for (t = 0; t <= 150000; t += 30000)
        EXPECT_EQ(Observe(p, g, P(5, 5), t), Trigger::None);
    std::uint64_t const restart = 150000 + kMaxGapMs + 1000;
    for (t = restart; t < restart + p.frozenMs; t += 30000)
        EXPECT_EQ(Observe(p, g, P(5, 5), t), Trigger::None) << t;
    EXPECT_EQ(Observe(p, g, P(5, 5), t), Trigger::Frozen);
    p.frozenMs = 0;  // rule off
    EXPECT_EQ(Observe(p, g, P(5, 5), t + 30000), Trigger::None);
}

// soak-s16-full-r1: every walk from the frozen spot ended intent_replan_exhausted.
TEST(Unstick, ReplanExhaustedGiveUpsWithinTheWindow)
{
    using namespace AutoWowUnstick;
    Params p;  // 5 within 15 min
    BotState s;
    std::uint64_t t = 10000;
    for (std::uint32_t i = 0; i < p.replanExhausted - 1; ++i, t += 14000)
    {
        NoteReplanExhausted(p, s, t);
        EXPECT_EQ(Observe(p, s, P(0, 0), t), Trigger::None);
    }
    NoteReplanExhausted(p, s, t);
    EXPECT_EQ(Observe(p, s, P(0, 0), t), Trigger::ReplanExhausted);

    // Give-ups spread wider than the window never add up.
    BotState w;
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        std::uint64_t const at = 1000 + i * (p.windowMs / 4 + 1000);
        NoteReplanExhausted(p, w, at);
        EXPECT_LT(w.exhausted, p.replanExhausted);
        EXPECT_EQ(Observe(p, w, P(std::int32_t(i) * 50, 0), at), Trigger::None);
    }
}

TEST(Unstick, CooldownAfterADecision)
{
    using namespace AutoWowUnstick;
    Params p;
    BotState s;
    for (std::uint32_t i = 0; i < p.replanExhausted; ++i)
        NoteReplanExhausted(p, s, 5000);
    ASSERT_EQ(Observe(p, s, P(0, 0), 5000), Trigger::ReplanExhausted);
    Acted(p, s, 5000);
    EXPECT_EQ(s.exhausted, 0u);
    for (std::uint32_t i = 0; i < p.replanExhausted; ++i)
        NoteReplanExhausted(p, s, 6000);
    EXPECT_EQ(Observe(p, s, P(0, 0), 6000), Trigger::None);                    // cooling down
    EXPECT_EQ(Observe(p, s, P(0, 0), 5000 + p.cooldownMs), Trigger::ReplanExhausted);
}

TEST(Unstick, NavmeshHoleAndDecision)
{
    using namespace AutoWowUnstick;
    std::array<std::uint32_t, kProbes> escape{};
    EXPECT_TRUE(NavmeshHole(escape, 8));
    escape[3] = 7;
    EXPECT_TRUE(NavmeshHole(escape, 8));
    escape[6] = 8;
    EXPECT_FALSE(NavmeshHole(escape, 8));

    EXPECT_EQ(Decide(true, true, true), Action::Hearth);   // hearth first
    EXPECT_EQ(Decide(true, false, true), Action::Portal);  // bound here: hearth is no escape
    EXPECT_EQ(Decide(false, true, true), Action::Portal);  // on cooldown / no stone, in a hole
    EXPECT_EQ(Decide(false, true, false), Action::None);   // walkable: nothing
    EXPECT_STREQ(ActionName(Action::Portal), "portal");
}

// ---- SafeRevive.V2 --------------------------------------------------------------------------------------
// soak-s21-full-r1: 13 revive_at plans at deaths 3-4 (past SpiritDeaths 2), all at threat 0.
TEST(SafeReviveV2, SpiritDeathsForceTheSpiritHealerWhateverTheSpot)
{
    AutoWowSafeRevive::Params const p;  // spiritDeaths 2
    AutoWowSafeRevive::Pick clear;
    clear.index = 3;
    clear.threat = 0;
    EXPECT_EQ(AutoWowSafeRevive::Decide(clear, 3, p), AutoWowSafeRevive::Plan::ReviveAt);  // V1: the bug
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(clear, 3, p), AutoWowSafeRevive::Plan::SpiritHealer);
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(clear, 2, p), AutoWowSafeRevive::Plan::SpiritHealer);
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(clear, 1, p), AutoWowSafeRevive::Plan::ReviveAt);
    AutoWowSafeRevive::Pick hot;
    hot.index = 5;
    hot.threat = 4;
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(hot, 1, p), AutoWowSafeRevive::Plan::ReviveAt);  // least threat
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(AutoWowSafeRevive::Pick{}, 1, p), AutoWowSafeRevive::Plan::None);
    AutoWowSafeRevive::Params never;
    never.spiritDeaths = 0;
    EXPECT_EQ(AutoWowSafeRevive::DecideV2(clear, 8, never), AutoWowSafeRevive::Plan::ReviveAt);
}

TEST(SafeReviveV2, ThreatCountsEveryHostileWithinSixtyYards)
{
    // A mob 40 yd off with a 10 yd aggro radius: invisible to V1, counted by V2.
    std::vector<Mob> const mobs = {M(40, 0, 20), M(0, 70, 20)};
    EXPECT_EQ(AutoWowSafeRevive::SpotThreat(P(0, 0), mobs, 20), 0U);
    EXPECT_EQ(AutoWowSafeRevive::SpotThreat(P(0, 0), mobs, 20, AutoWowSafeRevive::kThreatYardsV2), 3U);
    // The wider radius never lowers a threat the aggro radius already sees.
    std::vector<Mob> const big = {M(0, 70, 20, false, 80)};
    EXPECT_EQ(AutoWowSafeRevive::SpotThreat(P(0, 0), big, 20, AutoWowSafeRevive::kThreatYardsV2), 3U);
    // PickSpot with the V2 radius prefers the spot farther from the 40 yd mob.
    std::vector<AutoWowSafeRevive::Spot> const spots = {{P(0, 0), true}, {P(-34, 0), true}};
    AutoWowSafeRevive::Pick const v1 = AutoWowSafeRevive::PickSpot(spots, {M(40, 0, 20)}, 20, {});
    AutoWowSafeRevive::Pick const v2 =
        AutoWowSafeRevive::PickSpot(spots, {M(40, 0, 20)}, 20, {}, AutoWowSafeRevive::kThreatYardsV2);
    EXPECT_EQ(v1.index, 0U);
    EXPECT_EQ(v1.threat, 0U);
    EXPECT_EQ(v2.index, 1U);
    EXPECT_EQ(v2.threat, 0U);
}

TEST(SafeReviveV2, RespawnSoonWindow)
{
    EXPECT_TRUE(AutoWowSafeRevive::RespawnSoon(1000, 1000));
    EXPECT_TRUE(AutoWowSafeRevive::RespawnSoon(1120, 1000));   // 120 s
    EXPECT_FALSE(AutoWowSafeRevive::RespawnSoon(1121, 1000));
    EXPECT_TRUE(AutoWowSafeRevive::RespawnSoon(900, 1000));    // overdue: counts
    EXPECT_FALSE(AutoWowSafeRevive::RespawnSoon(0, 1000));     // none scheduled
}

TEST(SafeReviveV2, RelocationInspectionIsInertWhenDisabled)
{
    EXPECT_FALSE(AutoWowSafeRevive::RelocationPending(62989));
    EXPECT_FALSE(AutoWowSafeRevive::TakeRelocation(62989));
}

TEST(RestSafe, CandidatesRingAtThirtyThenSixtyYards)
{
    Point const here = P(-9000, 400);
    EXPECT_EQ(AutoWowRestSafe::Candidate(here, 0).x, here.x);
    EXPECT_EQ(AutoWowRestSafe::Candidate(here, AutoWowRestSafe::kCandidates).y, here.y);
    for (std::size_t k = 1; k < AutoWowRestSafe::kCandidates; ++k)
    {
        std::uint32_t const d = DistanceYards(here, AutoWowRestSafe::Candidate(here, k));
        std::uint32_t const ring = k <= 8 ? AutoWowRestSafe::kInnerYards : AutoWowRestSafe::kOuterYards;
        EXPECT_LE(d, ring) << k;
        EXPECT_GE(d + 1, ring) << k;
    }
}

TEST(RestSafe, AClearSpotStaysPut)
{
    Point const here = P(0, 0);
    EXPECT_TRUE(AutoWowRestSafe::MoveOrder(here, {}, 20).empty());
    EXPECT_TRUE(AutoWowRestSafe::MoveOrder(here, {M(100, 0, 20)}, 20).empty());  // out of aggro range
}

TEST(RestSafe, MoveOrderLeavesTheAggroRadiusNearestRingFirst)
{
    // Aggro 15 + margin 5 covers here and the inner E point (30, 0) only.
    std::vector<std::size_t> const order = AutoWowRestSafe::MoveOrder(P(0, 0), {M(10, 0, 20, false, 15)}, 20);
    ASSERT_EQ(order.size(), AutoWowRestSafe::kCandidates - 2);  // not stay, not inner E
    EXPECT_EQ(order.front(), 2u);                                // inner NE, lowest clear index
    EXPECT_EQ(std::count(order.begin(), order.end(), std::size_t(1)), 0);
}

TEST(RestSafe, LeastThreatFirstThenLowestIndex)
{
    // Elite on the bot (6); a normal mob covering only the outer W point (3 < 6: still better than here).
    std::vector<Mob> const mobs{M(0, 0, 20, true, 10), M(-60, 0, 20, false, 10)};
    std::vector<std::size_t> const order = AutoWowRestSafe::MoveOrder(P(0, 0), mobs, 20);
    ASSERT_EQ(order.size(), AutoWowRestSafe::kCandidates - 1);
    EXPECT_EQ(order.front(), 1u);
    EXPECT_EQ(order.back(), 13u);  // outer W, threat 3
}

TEST(RestSafe, HostilePlayerThreatensFortyFiveYardsAtEliteWeight)
{
    Point const here = P(0, 0);
    EXPECT_EQ(AutoWowSafeRevive::SpotThreat(here, {AutoWowRestSafe::PlayerThreat(44, 0, 1)}, 70), 6u);
    EXPECT_EQ(AutoWowSafeRevive::SpotThreat(here, {AutoWowRestSafe::PlayerThreat(46, 0, 80)}, 70), 0u);
    // A player on the bot covers the whole inner ring: only the 60 yd ring helps.
    std::vector<std::size_t> const order =
        AutoWowRestSafe::MoveOrder(here, {AutoWowRestSafe::PlayerThreat(0, 0, 20)}, 20);
    ASSERT_EQ(order.size(), 8u);
    EXPECT_EQ(order.front(), 9u);
}

TEST(RestSafe, PvpPlanRevivesOnlyOnAZeroThreatSpot)
{
    using AutoWowSafeRevive::Plan;
    EXPECT_EQ(AutoWowRestSafe::PvpPlan(Plan::ReviveAt, {3, 0}), Plan::ReviveAt);
    EXPECT_EQ(AutoWowRestSafe::PvpPlan(Plan::ReviveAt, {3, 6}), Plan::SpiritHealer);
    EXPECT_EQ(AutoWowRestSafe::PvpPlan(Plan::None, {}), Plan::SpiritHealer);
    EXPECT_EQ(AutoWowRestSafe::PvpPlan(Plan::SpiritHealer, {0, 0}), Plan::SpiritHealer);
}
}  // namespace
