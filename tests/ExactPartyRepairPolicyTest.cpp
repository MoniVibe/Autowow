/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "gtest/gtest.h"

#include "../src/AutoWow/ExactPartyRepairPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
AutoWowExactParty::ExactPartySignals SafeFiveBotParty()
{
    AutoWowExactParty::ExactPartySignals state;
    state.requestedCount = 5;
    state.requestedLeaderPresent = true;
    state.allRequestedOnline = true;
    state.allRequestedPlayerbots = true;
    state.sameFaction = true;
    state.allSubsetGroupsOrdinary = true;
    state.allSubsetGroupMembersRequested = true;
    state.allSubsetGroupMembersOnline = true;
    state.allSubsetGroupMembersPlayerbots = true;
    state.allGroupedRequestedMembersPresent = true;
    return state;
}

std::string CreatePartyBody()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/AutoWowBridge.cpp");
    std::string const source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    std::size_t const start = source.find("bool CreateParty()");
    std::size_t const end = source.find("bool RallyParty()", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}
}

TEST(ExactPartyRepairPolicyTest, CreatesOrReconfiguresWithoutRepairWhenAlreadySafe)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Create);

    state.leaderHasGroup = true;
    state.leaderPresentInCurrentGroup = true;
    state.currentCount = 5;
    state.subsetGroupCount = 1;
    state.groupedRequestedCount = 5;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Reconfigure);
}

TEST(ExactPartyRepairPolicyTest, RepairsOnlyAProperRequestedSubset)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    state.leaderHasGroup = true;
    state.leaderPresentInCurrentGroup = true;
    state.currentCount = 4;
    state.subsetGroupCount = 1;
    state.groupedRequestedCount = 4;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::RepairSubset);

    state.subsetGroupCount = 2;
    state.groupedRequestedCount = 5;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::RepairSubset);
}

TEST(ExactPartyRepairPolicyTest, RepairsFragmentedSubsetsWhenIntendedLeaderIsUngrouped)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    state.subsetGroupCount = 2;
    state.groupedRequestedCount = 4;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::RepairSubset);

    state.subsetGroupCount = 1;
    state.groupedRequestedCount = 3;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::RepairSubset);
}

TEST(ExactPartyRepairPolicyTest, RefusesConflictsSpecialGroupsAndMissingLeader)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    state.subsetGroupCount = 2;
    state.groupedRequestedCount = 4;

    state.allSubsetGroupsOrdinary = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allSubsetGroupsOrdinary = true;

    state.allSubsetGroupMembersRequested = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allSubsetGroupMembersRequested = true;

    state.allGroupedRequestedMembersPresent = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allGroupedRequestedMembersPresent = true;

    state.requestedLeaderPresent = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.requestedLeaderPresent = true;

    state.sameFaction = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
}

TEST(ExactPartyRepairPolicyTest, RefusesHumanOrOfflineRequestedAndCurrentMembers)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    state.allRequestedPlayerbots = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allRequestedPlayerbots = true;

    state.allRequestedOnline = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allRequestedOnline = true;

    state.subsetGroupCount = 1;
    state.groupedRequestedCount = 4;
    state.allSubsetGroupMembersPlayerbots = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
    state.allSubsetGroupMembersPlayerbots = true;

    state.allSubsetGroupMembersOnline = false;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
}

TEST(ExactPartyRepairPolicyTest, RefusesInconsistentLeaderOrGroupedCounts)
{
    using namespace AutoWowExactParty;
    ExactPartySignals state = SafeFiveBotParty();
    state.leaderHasGroup = true;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);

    state.leaderHasGroup = false;
    state.subsetGroupCount = 1;
    state.groupedRequestedCount = 6;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);

    state.subsetGroupCount = 0;
    state.groupedRequestedCount = 1;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);

    state.subsetGroupCount = 2;
    state.groupedRequestedCount = 1;
    EXPECT_EQ(ClassifyExactParty(state), ExactPartyAction::Refuse);
}

TEST(ExactPartyRepairPolicyTest, BridgeUsesBoundedCoreGroupRepairAndDistinctTelemetry)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("ClassifyExactParty"), std::string::npos);
    EXPECT_NE(body.find("std::set<Group*> subsetGroups"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->GetMemberSlots()"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->isRaidGroup()"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->isBGGroup()"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->isBFGroup()"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->isLFGGroup()"), std::string::npos);
    EXPECT_NE(body.find("ExactPartyAction::RepairSubset"), std::string::npos);
    EXPECT_NE(body.find("std::vector<Group*> groupsToDisband"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->Disband()"), std::string::npos);
    EXPECT_NE(body.find("group->Create(leader)"), std::string::npos);
    EXPECT_NE(body.find("group->AddMember(member)"), std::string::npos);
    EXPECT_NE(body.find("repaired_subset"), std::string::npos);
    EXPECT_EQ(body.find("TeleportTo("), std::string::npos);
    EXPECT_EQ(body.find("CharacterDatabase"), std::string::npos);
    EXPECT_EQ(body.find("WorldDatabase"), std::string::npos);
    EXPECT_EQ(body.find("SaveToDB("), std::string::npos);
    EXPECT_EQ(body.find("AddQuest("), std::string::npos);
    EXPECT_EQ(body.find("RewardQuest("), std::string::npos);
}
