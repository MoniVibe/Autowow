#include "../src/Ai/World/Rpg/QuestPartyCohesionPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowQuestPartyCohesion;

Facts FollowerFacts()
{
    Facts facts;
    facts.inParty = true;
    facts.follower = true;
    facts.leaderAvailable = true;
    facts.leaderAlive = true;
    facts.sameMap = true;
    facts.selfAlive = true;
    facts.distanceToLeader = kCampaignCohesionRadius;
    return facts;
}
}

TEST(QuestPartyCohesionPolicyTest, LeaderAndSoloBotsAreNotRejoined)
{
    Facts solo;
    EXPECT_EQ(Evaluate(solo).action, Action::NotApplicable);

    Facts leader = FollowerFacts();
    leader.follower = false;
    EXPECT_EQ(Evaluate(leader).action, Action::NotApplicable);
    EXPECT_EQ(Evaluate(leader).reason, Reason::Leader);
}

TEST(QuestPartyCohesionPolicyTest, FollowersWithinRadiusMayWorkTheirObjective)
{
    Facts facts = FollowerFacts();
    EXPECT_EQ(Evaluate(facts).action, Action::AllowObjective);
    EXPECT_EQ(Evaluate(facts).reason, Reason::WithinRadius);

    facts.distanceToLeader = kCampaignCohesionRadius + 0.01f;
    EXPECT_EQ(Evaluate(facts).action, Action::RejoinLeader);
    EXPECT_EQ(Evaluate(facts).reason, Reason::OutsideRadius);
}

TEST(QuestPartyCohesionPolicyTest, CombatAndWorkWindowsRemainIndependent)
{
    Facts facts = FollowerFacts();
    facts.distanceToLeader = 200.0f;
    facts.selfInCombat = true;
    EXPECT_EQ(Evaluate(facts).action, Action::AllowException);
    EXPECT_EQ(Evaluate(facts).reason, Reason::SelfCombat);

    facts.selfInCombat = false;
    facts.objectiveWorkWindow = true;
    EXPECT_EQ(Evaluate(facts).action, Action::AllowException);
    EXPECT_EQ(Evaluate(facts).reason, Reason::ObjectiveWorkWindow);
}

TEST(QuestPartyCohesionPolicyTest, CrossMapAndMissingLeaderRemainIndependentWithoutTeleport)
{
    Facts facts = FollowerFacts();
    facts.sameMap = false;
    EXPECT_EQ(Evaluate(facts).action, Action::AllowIndependent);
    EXPECT_EQ(Evaluate(facts).reason, Reason::CrossMap);

    facts.sameMap = true;
    facts.leaderAvailable = false;
    EXPECT_EQ(Evaluate(facts).action, Action::AllowIndependent);
    EXPECT_EQ(Evaluate(facts).reason, Reason::LeaderUnavailable);
}

TEST(QuestPartyCohesionPolicyTest, CorpseRecoveryIsNotSuppressedByCohesion)
{
    Facts facts = FollowerFacts();
    facts.selfAlive = false;
    facts.distanceToLeader = 200.0f;
    EXPECT_EQ(Evaluate(facts).action, Action::AllowException);
    EXPECT_EQ(Evaluate(facts).reason, Reason::SelfDead);
}
