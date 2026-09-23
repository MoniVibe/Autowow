/*
 * Pure committed-travel-intent tests (TravelIntentPolicy.h, AutoWow.TravelIntent.Enable). No world
 * access: MoveFarToIntent supplies the live facts; these tests pin the decisions.
 */

#include "TravelIntentPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace TravelIntentPolicy;

Params const kParams{5, 90 * 1000, 20};
// Coldridge soak-s5-cohort-r2 bounce toward Felix's Box (-6374,773).
Point const kGoal = MakePoint(0, -6374.81f, 773.70f);
Point const kA = MakePoint(0, -6189.0f, 391.0f);
Point const kB = MakePoint(0, -6315.0f, 470.0f);

TEST(TravelIntentPolicyTest, PointsAreIntegerYards)
{
    EXPECT_EQ(kGoal.x, -6375);
    EXPECT_EQ(kGoal.y, 773);
    EXPECT_EQ(DistanceYards(MakePoint(0, 0, 0), MakePoint(0, 3, 4)), 5u);
    EXPECT_EQ(DistanceYards(MakePoint(0, 0, 0), MakePoint(1, 0, 0)), 0xFFFFFFFFu);
}

TEST(TravelIntentPolicyTest, CommitHoldsWhileFollowing)
{
    Intent intent;
    EXPECT_EQ(Observe(intent, kGoal, DistanceYards(kA, kGoal), 1000, kParams), Verdict::Progress);
    EXPECT_EQ(ClassifySegment(intent, kA, false, false), SegmentState::None);
    ASSERT_TRUE(Admit(intent, kA, kB, DistanceYards(kB, kGoal), false, kParams));
    Commit(intent, kA, kB, DistanceYards(kB, kGoal));
    Point const midway = MakePoint(0, -6250.0f, 430.0f);
    EXPECT_EQ(ClassifySegment(intent, midway, true, false), SegmentState::Follow);
    EXPECT_EQ(ClassifySegment(intent, midway, false, false), SegmentState::Interrupted);
    EXPECT_EQ(ClassifySegment(intent, MakePoint(0, -6313.0f, 468.0f), false, false), SegmentState::Reached);
    EXPECT_EQ(ClassifySegment(intent, midway, true, true), SegmentState::Unsafe);
}

TEST(TravelIntentPolicyTest, HysteresisProtectsLiveSegment)
{
    Intent intent;
    (void)Observe(intent, kGoal, 400, 1000, kParams);
    Commit(intent, kA, kB, 300);  // committed plan leaves the bot 300 yd from the goal
    Point const here = MakePoint(0, -6250.0f, 430.0f);
    Point const slightlyBetter = MakePoint(0, -6300.0f, 560.0f);
    Point const muchBetter = MakePoint(0, -6340.0f, 700.0f);
    EXPECT_FALSE(Admit(intent, here, slightlyBetter, 250, false, kParams));  // 250 > 300 * 0.8
    EXPECT_TRUE(Admit(intent, here, muchBetter, 240, false, kParams));       // exactly the margin
    EXPECT_FALSE(Admit(intent, here, muchBetter, 100, true, kParams));       // dangerous endpoint
    DropSegment(intent);
    EXPECT_TRUE(Admit(intent, here, slightlyBetter, 250, false, kParams));   // no live plan to beat
}

TEST(TravelIntentPolicyTest, NonStepIsRefused)
{
    Intent intent;
    (void)Observe(intent, kGoal, 400, 1000, kParams);
    EXPECT_FALSE(Admit(intent, kA, MakePoint(0, -6187.0f, 393.0f), 390, false, kParams));
}

// The STARTER_STALLS root cause: from A the selector yields B, from B it yields A. Legacy re-armed the
// stuck counter on every accepted segment and bounced forever. The intent must refuse B -> A, then
// exhaust its replan budget and give up with a typed reason.
TEST(TravelIntentPolicyTest, AlternatingSegmentsDoNotFlipFlop)
{
    Intent intent;
    std::uint32_t now = 1000;
    Point here = kA;
    std::uint32_t commits = 0;
    bool gaveUp = false;
    for (int tick = 0; tick < 200 && !gaveUp; ++tick, now += 1000)
    {
        ASSERT_NE(Observe(intent, kGoal, DistanceYards(here, kGoal), now, kParams), Verdict::NoProgress);
        SegmentState const state = ClassifySegment(intent, here, false, false);
        if (state == SegmentState::Reached)
            CompleteSegment(intent);
        if (ReplanCoolingDown(intent, now))
            continue;
        Point const candidate = (here.x == kA.x && here.y == kA.y) ? kB : kA;  // the bouncing selector
        if (Admit(intent, here, candidate, DistanceYards(candidate, kGoal), false, kParams))
        {
            Commit(intent, here, candidate, DistanceYards(candidate, kGoal));
            ++commits;
            here = candidate;  // the walk arrives
            continue;
        }
        if (NoteFailure(intent, now, kParams))
        {
            Abandon(intent, GiveUp::ReplanExhausted);
            gaveUp = true;
        }
    }
    EXPECT_EQ(commits, 1u);  // A -> B only; B -> A is refused
    EXPECT_TRUE(gaveUp);
    EXPECT_EQ(TakeGiveUp(intent), GiveUp::ReplanExhausted);
    EXPECT_EQ(TakeGiveUp(intent), GiveUp::None);
}

TEST(TravelIntentPolicyTest, ForwardCorridorKeepsCommitting)
{
    Intent intent;
    std::uint32_t now = 1000;
    Point here = MakePoint(0, 0, 0);
    Point const goal = MakePoint(0, 1000, 0);
    for (int step = 1; step <= 20; ++step, now += 5000)
    {
        ASSERT_EQ(Observe(intent, goal, DistanceYards(here, goal), now, kParams), Verdict::Progress);
        if (ClassifySegment(intent, here, false, false) == SegmentState::Reached)
            CompleteSegment(intent);
        Point const next = MakePoint(0, static_cast<float>(step * 40), 0);
        ASSERT_TRUE(Admit(intent, here, next, DistanceYards(next, goal), false, kParams));
        Commit(intent, here, next, DistanceYards(next, goal));
        here = next;
    }
    EXPECT_EQ(intent.segmentsCommitted, 20u);
}

TEST(TravelIntentPolicyTest, ProgressWindowMeasuresGoalNotSegments)
{
    Intent intent;
    std::uint32_t now = 1000;
    ASSERT_EQ(Observe(intent, kGoal, 400, now, kParams), Verdict::Progress);
    // Segments keep committing (as legacy walks did) but the goal distance never improves by 10 yd.
    Verdict verdict = Verdict::Travelling;
    std::uint32_t ticks = 0;
    while (verdict != Verdict::NoProgress && ticks < 1000)
    {
        now += 1000;
        ++ticks;
        Commit(intent, MakePoint(0, static_cast<float>(ticks), 0), MakePoint(0, static_cast<float>(ticks + 50), 0), 395);
        verdict = Observe(intent, kGoal, ticks % 2 ? 395 : 405, now, kParams);
    }
    EXPECT_EQ(verdict, Verdict::NoProgress);
    EXPECT_EQ(ticks, kParams.progressWindowMs / 1000);
}

TEST(TravelIntentPolicyTest, ObservationGapsAreNotCharged)
{
    Intent intent;
    std::uint32_t now = 1000;
    (void)Observe(intent, kGoal, 400, now, kParams);
    for (int i = 0; i < 10; ++i)
    {
        now += 30 * 1000;  // combat / another status owned the bot in between (inside kStaleIntentMs)
        EXPECT_EQ(Observe(intent, kGoal, 400, now, kParams), Verdict::Travelling);
    }
    EXPECT_EQ(intent.chargedMs, 0u);
}

TEST(TravelIntentPolicyTest, GoalMoveAndStaleIntentRearm)
{
    Intent intent;
    (void)Observe(intent, kGoal, 400, 1000, kParams);
    Commit(intent, kA, kB, 300);
    // small target drift keeps the commitment
    EXPECT_EQ(Observe(intent, MakePoint(0, -6365.0f, 780.0f), 400, 2000, kParams), Verdict::Travelling);
    EXPECT_TRUE(intent.hasSegment);
    // target moved beyond kGoalMoveYards: a fresh intent
    EXPECT_EQ(Observe(intent, MakePoint(0, -6300.0f, 800.0f), 400, 3000, kParams), Verdict::Progress);
    EXPECT_FALSE(intent.hasSegment);
    EXPECT_EQ(intent.recentCount, 0u);
    Commit(intent, kA, kB, 300);
    // unobserved past kStaleIntentMs: history, re-armed
    EXPECT_EQ(Observe(intent, MakePoint(0, -6300.0f, 800.0f), 400, 3000 + kStaleIntentMs + 1, kParams),
              Verdict::Progress);
    EXPECT_FALSE(intent.hasSegment);
}

TEST(TravelIntentPolicyTest, ReplanTriggersAndBudget)
{
    Intent intent;
    (void)Observe(intent, kGoal, 400, 1000, kParams);
    Commit(intent, kA, kB, 300);
    for (std::uint32_t i = 1; i < kParams.replanFailCount; ++i)
        EXPECT_FALSE(NoteFailure(intent, 1000 + i * kReplanCooldownMs, kParams));
    EXPECT_TRUE(ReplanCoolingDown(intent, 1000 + (kParams.replanFailCount - 1) * kReplanCooldownMs + 1));
    EXPECT_TRUE(NoteFailure(intent, 20000, kParams));
    // reaching a segment clears the streak
    CompleteSegment(intent);
    EXPECT_EQ(intent.failures, 0u);
    EXPECT_FALSE(ReplanCoolingDown(intent, 20001));
    // a zero budget still gives up on the first failure (never infinite)
    Params const zero{0, 1000, 20};
    EXPECT_TRUE(NoteFailure(intent, 30000, zero));
}

TEST(TravelIntentPolicyTest, RecentRingIsBounded)
{
    Intent intent;
    for (int i = 0; i < 20; ++i)
        Remember(intent, MakePoint(0, static_cast<float>(i * 100), 0));
    EXPECT_EQ(intent.recentCount, kRecentPoints);
    EXPECT_FALSE(IsRecent(intent, MakePoint(0, 0, 0)));      // evicted
    EXPECT_TRUE(IsRecent(intent, MakePoint(0, 1905, 0)));    // last remembered, within kRevisitYards
}
}  // namespace
