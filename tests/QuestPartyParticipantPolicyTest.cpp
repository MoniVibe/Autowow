/*
 * Focused participant-set contract tests for explicit quest-party execution.
 * These tests exercise the pure production policy and a small source-wiring seam; they do not
 * boot a world, send a bridge command, alter a quest, or touch a database.
 */

#include "gtest/gtest.h"

#include "../src/AutoWow/QuestPartyParticipantPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using AutoWowQuestParty::Decision;
using AutoWowQuestParty::MemberFacts;

MemberFacts Active(bool leader)
{
    MemberFacts facts;
    facts.leader = leader;
    facts.present = true;
    facts.online = true;
    facts.playerbot = true;
    facts.alive = true;
    facts.exactQuestInLog = true;
    facts.statusAllowed = true;
    return facts;
}

MemberFacts FollowerWithoutQuest()
{
    MemberFacts facts = Active(false);
    facts.exactQuestInLog = false;
    facts.statusAllowed = false;
    return facts;
}

std::string BridgeSource()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/AutoWowBridge.cpp");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST(QuestPartyParticipantPolicyTest, LeaderOnlyTurnInUsesLeaderAsSoleParticipant)
{
    using namespace AutoWowQuestParty;
    std::vector<MemberFacts> const members{Active(true), FollowerWithoutQuest()};

    ParticipantPlan const plan = BuildPlan(members);

    EXPECT_EQ(plan.decision, Decision::ExecuteParticipants);
    EXPECT_EQ(plan.participantCount, 1U);
    EXPECT_EQ(plan.nonParticipantCount, 1U);
    EXPECT_EQ(plan.invalidRosterCount, 0U);
    EXPECT_TRUE(plan.leaderParticipates);
    EXPECT_TRUE(IsParticipant(members.front()));
    EXPECT_FALSE(IsParticipant(members.back()));
}

TEST(QuestPartyParticipantPolicyTest, LeaderOnlyObjectiveDoesNotRequireFollowerQuestOwnership)
{
    using namespace AutoWowQuestParty;
    MemberFacts leader = Active(true);
    MemberFacts follower = FollowerWithoutQuest();
    follower.alive = false;

    ParticipantPlan const plan = BuildPlan({leader, follower});

    EXPECT_EQ(plan.decision, Decision::ExecuteParticipants);
    EXPECT_EQ(plan.participantCount, 1U);
    EXPECT_EQ(plan.nonParticipantCount, 1U);
}

TEST(QuestPartyParticipantPolicyTest, ExactQuestFollowerParticipatesAndMissingQuestFollowerDoesNot)
{
    using namespace AutoWowQuestParty;
    MemberFacts followerWithQuest = Active(false);
    MemberFacts followerWithoutQuest = FollowerWithoutQuest();

    ParticipantPlan const plan = BuildPlan({Active(true), followerWithQuest, followerWithoutQuest});

    EXPECT_EQ(plan.decision, Decision::ExecuteParticipants);
    EXPECT_EQ(plan.participantCount, 2U);
    EXPECT_EQ(plan.nonParticipantCount, 1U);
    EXPECT_TRUE(IsParticipant(followerWithQuest));
    EXPECT_FALSE(IsParticipant(followerWithoutQuest));
}

TEST(QuestPartyParticipantPolicyTest, LeaderWithoutExactQuestIsRejected)
{
    using namespace AutoWowQuestParty;
    MemberFacts leader = FollowerWithoutQuest();
    leader.leader = true;

    ParticipantPlan const plan = BuildPlan({leader, FollowerWithoutQuest()});

    EXPECT_EQ(plan.decision, Decision::RejectLeader);
    EXPECT_EQ(plan.participantCount, 0U);
    EXPECT_TRUE(!plan.leaderParticipates);
}

TEST(QuestPartyParticipantPolicyTest, NonPlayerbotRosterMemberStillFailsClosed)
{
    using namespace AutoWowQuestParty;
    MemberFacts human = FollowerWithoutQuest();
    human.playerbot = false;

    ParticipantPlan const plan = BuildPlan({Active(true), human});

    EXPECT_EQ(plan.decision, Decision::RejectRoster);
    EXPECT_EQ(plan.invalidRosterCount, 1U);
}

TEST(QuestPartyParticipantPolicyTest, RewardedLeaderIsIdempotentAndDoesNotPrimeFollowers)
{
    using namespace AutoWowQuestParty;
    MemberFacts leader = Active(true);
    leader.rewarded = true;
    leader.exactQuestInLog = false;

    ParticipantPlan const plan = BuildPlan({leader, Active(false)});

    EXPECT_EQ(plan.decision, Decision::AlreadyRewarded);
    EXPECT_EQ(plan.participantCount, 1U);
    EXPECT_TRUE(plan.leaderRewarded);
    EXPECT_FALSE(plan.leaderParticipates);
}

TEST(QuestPartyParticipantPolicyTest, BridgeUsesParticipantAggregatesAndIndependentParticipantStrategy)
{
    std::string const source = BridgeSource();
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("QuestPartyParticipantPolicy.h"), std::string::npos);
    EXPECT_NE(source.find("AutoWowQuestParty::BuildPlan"), std::string::npos);
    EXPECT_NE(source.find("AutoWowQuestParty::IsParticipant"), std::string::npos);
    EXPECT_NE(source.find("+follow,-grind,-move random,-new rpg,-travel"), std::string::npos);
    EXPECT_NE(source.find("+grind,+new rpg,-follow,-move random,-travel"), std::string::npos);
    EXPECT_NE(source.find("Every participant retains its own objective, travel, combat, loot, and"),
              std::string::npos);
    EXPECT_NE(source.find(
                  "memberAI->SetAutoWowIndependentParty(true);\n                        memberAI->ChangeStrategy(\n                            \"+grind,-travel,-move random,-follow,+new rpg\""),
              std::string::npos);
    EXPECT_NE(source.find("all_participant_core_reward_confirmed"), std::string::npos);
    EXPECT_EQ(source.find("Every member must\n             // own the exact quest"), std::string::npos);
    EXPECT_EQ(source.find("bool exactPartyReady = true"), std::string::npos);
}

TEST(QuestPartyParticipantPolicyTest, QuestDispatchRearmsIndependentModeAfterRestart)
{
    std::string const source = BridgeSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("Re-arm this in-memory mode on every quest dispatch"), std::string::npos);
    EXPECT_NE(source.find("The quest order is the idempotent boundary for the persistent campaign"),
              std::string::npos);
}

TEST(QuestPartyParticipantPolicyTest, NonparticipantsKeepAnIndependentNativeLoop)
{
    std::string const source = BridgeSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("Not owning the leader's requested quest is not a command to become an"),
              std::string::npos);
    EXPECT_NE(source.find("+grind,+new rpg,-follow,-move random,-travel"), std::string::npos);
    EXPECT_NE(source.find("a later party\n                        // request can enroll it"), std::string::npos);
}
