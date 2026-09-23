/*
 * Pure starter-stall recovery policy tests (QuestStallRecoveryPolicy.h). No world access: the
 * executor's live travel, Blocked hold and item-use gates remain the native authority.
 */

#include "QuestStallRecoveryPolicy.h"

#include <limits>

#include "gtest/gtest.h"

namespace
{
using namespace QuestStallRecoveryPolicy;

TravelKey const kBox = MakeTravelKey(0, -6374.81f, 773.704f);

TEST(QuestStallRecoveryPolicyTest, TravelKeyIsIntegerYards)
{
    EXPECT_EQ(kBox.mapId, 0u);
    EXPECT_EQ(kBox.x, -6375);
    EXPECT_EQ(kBox.y, 773);
    EXPECT_EQ(QuantizeYards(309.9f), 309u);
    EXPECT_EQ(QuantizeYards(-1.0f), 0u);
    EXPECT_EQ(QuantizeYards(std::numeric_limits<float>::infinity()), 0u);
}

TEST(QuestStallRecoveryPolicyTest, SteadyApproachNeverExpires)
{
    TravelWatch watch;
    std::uint32_t now = 1000;
    EXPECT_EQ(ObserveTravel(watch, kBox, 900, now), TravelVerdict::Progress);
    for (std::uint32_t distance = 890; distance > 10; distance -= 10)
    {
        now += 5000;  // 2 yd/s, much slower than a walking bot
        EXPECT_EQ(ObserveTravel(watch, kBox, distance, now), TravelVerdict::Progress);
    }
}

// Live soak-s5-cohort-r2 shape: the bot alternates between two walk segments whose distances to the
// Felix's Box spawn are ~309 and ~427 yd. MoveFarTo re-arms on each segment; the watch must expire.
TEST(QuestStallRecoveryPolicyTest, PingPongBetweenTwoSegmentsExpires)
{
    TravelWatch watch;
    std::uint32_t now = 5000;
    ASSERT_EQ(ObserveTravel(watch, kBox, 380, now), TravelVerdict::Progress);
    ASSERT_EQ(ObserveTravel(watch, kBox, 309, now += 1000), TravelVerdict::Progress);  // first leg
    TravelVerdict verdict = TravelVerdict::Travelling;
    std::uint32_t ticks = 0;
    for (; ticks < 1000 && verdict != TravelVerdict::Expired; ++ticks)
    {
        std::uint32_t const distance = (ticks / 12) % 2 ? 309 : 427;  // 12 s per leg
        verdict = ObserveTravel(watch, kBox, distance, now += 1000);
    }
    EXPECT_EQ(verdict, TravelVerdict::Expired);
    EXPECT_EQ(ticks, kTravelNoProgressBudgetMs / 1000);
}

TEST(QuestStallRecoveryPolicyTest, SubThresholdGainsDoNotResetBudget)
{
    TravelWatch watch;
    std::uint32_t now = 1;
    ASSERT_EQ(ObserveTravel(watch, kBox, 400, now), TravelVerdict::Progress);
    // 9 yd total gain is below kTravelMinImprovementYards: jitter, not approach.
    EXPECT_EQ(ObserveTravel(watch, kBox, 391, now += 5000), TravelVerdict::Travelling);
    EXPECT_EQ(ObserveTravel(watch, kBox, 390, now += 5000), TravelVerdict::Progress);
    EXPECT_EQ(watch.bestYards, 390u);
    EXPECT_EQ(watch.chargedMs, 0u);
}

TEST(QuestStallRecoveryPolicyTest, ObservationGapsAreNotCharged)
{
    TravelWatch watch;
    std::uint32_t now = 100;
    ASSERT_EQ(ObserveTravel(watch, kBox, 400, now), TravelVerdict::Progress);
    // A 10-minute combat/death gap without observation is not charged.
    EXPECT_EQ(ObserveTravel(watch, kBox, 400, now += 10 * 60 * 1000), TravelVerdict::Travelling);
    EXPECT_EQ(watch.chargedMs, 0u);
    EXPECT_EQ(ObserveTravel(watch, kBox, 400, now += kTravelMaxObservationGapMs), TravelVerdict::Travelling);
    EXPECT_EQ(watch.chargedMs, kTravelMaxObservationGapMs);
}

TEST(QuestStallRecoveryPolicyTest, NewDestinationRearms)
{
    TravelWatch watch;
    std::uint32_t now = 1;
    ASSERT_EQ(ObserveTravel(watch, kBox, 400, now), TravelVerdict::Progress);
    for (int i = 0; i < 149; ++i)
        ASSERT_NE(ObserveTravel(watch, kBox, 400, now += 1000), TravelVerdict::Expired);
    TravelKey const chest = MakeTravelKey(0, -6503.89f, 680.367f);
    EXPECT_EQ(ObserveTravel(watch, chest, 500, now += 1000), TravelVerdict::Progress);
    EXPECT_EQ(watch.chargedMs, 0u);
    EXPECT_EQ(ObserveTravel(watch, chest, 500, now += 1000), TravelVerdict::Travelling);
    ResetTravel(watch);
    EXPECT_FALSE(watch.armed);
}

TEST(QuestStallRecoveryPolicyTest, ClockWrapIsCharged)
{
    TravelWatch watch;
    std::uint32_t now = 0xFFFFF000u;
    ASSERT_EQ(ObserveTravel(watch, kBox, 400, now), TravelVerdict::Progress);
    now += 5000;  // wraps
    EXPECT_EQ(ObserveTravel(watch, kBox, 400, now), TravelVerdict::Travelling);
    EXPECT_EQ(watch.chargedMs, 5000u);
}

TEST(QuestStallRecoveryPolicyTest, BlockedDeferralRequiresDwellAndNonOracle)
{
    EXPECT_FALSE(ShouldDeferBlocked(false, 0, 100000));                     // never blocked
    EXPECT_FALSE(ShouldDeferBlocked(false, 1000, 1000 + kBlockedDwellMs - 1));
    EXPECT_TRUE(ShouldDeferBlocked(false, 1000, 1000 + kBlockedDwellMs));
    EXPECT_FALSE(ShouldDeferBlocked(true, 1000, 1000 + 10 * kBlockedDwellMs));  // Oracle keeps its hold
    EXPECT_TRUE(ShouldDeferBlocked(false, 0xFFFFFF00u, 0xFFFFFF00u + kBlockedDwellMs));  // wraps
}

TEST(QuestStallRecoveryPolicyTest, DeferralBookExpiresAndReturns)
{
    DeferralBook book;
    book.Defer(3361, 1000);
    EXPECT_TRUE(book.IsDeferred(3361, 1000));
    EXPECT_FALSE(book.IsDeferred(170, 1000));
    EXPECT_TRUE(book.IsDeferred(3361, 1000 + kDeferMs - 1));
    EXPECT_FALSE(book.IsDeferred(3361, 1000 + kDeferMs));  // eligible again
    EXPECT_TRUE(book.entries.empty());
}

TEST(QuestStallRecoveryPolicyTest, DeferralBookRedeferExtendsAndIsBounded)
{
    DeferralBook book;
    book.Defer(233, 1000);
    book.Defer(233, 5000);
    ASSERT_EQ(book.entries.size(), 1u);
    EXPECT_TRUE(book.IsDeferred(233, 1000 + kDeferMs));  // extended by the second deferral

    DeferralBook full;
    for (std::uint32_t quest = 1; quest <= kMaxDeferredQuests + 3; ++quest)
        full.Defer(quest, 10);
    EXPECT_EQ(full.entries.size(), kMaxDeferredQuests);
    EXPECT_FALSE(full.IsDeferred(1, 10));  // oldest evicted first
    EXPECT_TRUE(full.IsDeferred(kMaxDeferredQuests + 3, 10));
}

TEST(QuestStallRecoveryPolicyTest, SpellTargetEntriesComeOnlyFromPositiveUnitEntryConditions)
{
    // q9303 Inoculation: spell 29528 conditions (17, 29528, type 31, target 1, value1 3, value2 16518).
    std::vector<SpellTargetConditionFact> const facts{
        {kConditionObjectEntryGuid, 1, kTypeIdUnit, 16518, false},
        {kConditionObjectEntryGuid, 1, kTypeIdUnit, 16518, false},   // duplicate else-group
        {kConditionObjectEntryGuid, 1, kTypeIdUnit, 700, false},
        {kConditionObjectEntryGuid, 0, kTypeIdUnit, 800, false},     // caster condition
        {kConditionObjectEntryGuid, 1, kTypeIdUnit, 900, true},      // negative
        {kConditionObjectEntryGuid, 1, 5, 181283, false},            // gameobject type
        {kConditionObjectEntryGuid, 1, kTypeIdUnit, 0, false},       // any entry
        {1, 1, 17743, 0, false}};                                    // aura (q5441), not an entry
    EXPECT_EQ(SpellTargetCreatureEntries(facts), (std::vector<std::uint32_t>{700, 16518}));
    EXPECT_TRUE(SpellTargetCreatureEntries({}).empty());
}
}  // namespace
