#include "gtest/gtest.h"

#include "../src/Ai/World/Rpg/Action/ExactQuestAttackRecoveryPolicy.h"
#include "../src/Ai/World/Rpg/QuestSourceStallPolicy.h"

namespace
{
using namespace ExactQuestAttackRecoveryPolicy;

Facts ValidVisibleSource()
{
    Facts facts;
    facts.sourceMatched = true;
    facts.targetAvailable = true;
    facts.raidClaimAllows = true;
    facts.validAttackTarget = true;
    facts.vehicleAllows = true;
    facts.dungeonPullReady = true;
    facts.lineOfSight = true;
    return facts;
}

TEST(ExactQuestAttackRecoveryPolicyTest, VisibleRangedTargetNeverTriggersApproachAfterAttackRejection)
{
    Facts const facts = ValidVisibleSource();
    EXPECT_EQ(Classify(facts), Rejection::OtherAttackGuard);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
}

TEST(ExactQuestAttackRecoveryPolicyTest, OnlyValidHostileBlockedLineOfSightMayApproach)
{
    Facts facts = ValidVisibleSource();
    facts.lineOfSight = false;
    EXPECT_EQ(Classify(facts), Rejection::BlockedLineOfSight);
    EXPECT_TRUE(ShouldApproach(Classify(facts)));

    facts.friendly = true;
    EXPECT_EQ(Classify(facts), Rejection::Friendly);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.friendly = false;
    facts.validAttackTarget = false;
    EXPECT_EQ(Classify(facts), Rejection::InvalidAttackTarget);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.validAttackTarget = true;
    facts.flight = true;
    EXPECT_EQ(Classify(facts), Rejection::Flight);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.flight = false;
    facts.raidClaimAllows = false;
    EXPECT_EQ(Classify(facts), Rejection::RaidClaim);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.raidClaimAllows = true;
    facts.dungeonPullReady = false;
    EXPECT_EQ(Classify(facts), Rejection::DungeonPull);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.dungeonPullReady = true;
    facts.vehicleAllows = false;
    EXPECT_EQ(Classify(facts), Rejection::Vehicle);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
    facts.vehicleAllows = true;
    facts.sourceMatched = false;
    EXPECT_EQ(Classify(facts), Rejection::SourceMismatch);
    EXPECT_FALSE(ShouldApproach(Classify(facts)));
}

TEST(ExactQuestAttackRecoveryPolicyTest, UnavailableSourceClockSurvivesMovementAndRebind)
{
    std::uint32_t firstFailureMs = 0;
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(
        firstFailureMs, 1000, QuestSourceStallPolicy::kUnavailableSourceBudgetMs));
    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(firstFailureMs, false, false);
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(
        firstFailureMs, 60000, QuestSourceStallPolicy::kUnavailableSourceBudgetMs));
    EXPECT_TRUE(QuestSourceStallPolicy::RecordUnavailableSource(
        firstFailureMs, 91000, QuestSourceStallPolicy::kUnavailableSourceBudgetMs));
    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(firstFailureMs, false, true);
    EXPECT_EQ(firstFailureMs, 0U);
}
}
