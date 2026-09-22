/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadBridge()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/AutoWowBridge.cpp");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string ReadPlayerbotAI()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/Bot/PlayerbotAI.cpp");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string CreatePartyBody()
{
    std::string const source = ReadBridge();
    std::size_t const start = source.find("bool CreateParty()");
    std::size_t const end = source.find("bool RallyParty()", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}

std::string RecoverQuestPartyBody()
{
    std::string const source = ReadBridge();
    std::size_t const start = source.find("bool RecoverQuestParty()");
    std::size_t const end = source.find("std::string OrderResponse", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}
}

TEST(PartyControlContract, ExactRosterCanBeReconfiguredWithoutDisbanding)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("party_existing_group_not_exact_roster"), std::string::npos);
    EXPECT_NE(body.find("group->GetMembersCount() != members.size() + 1"), std::string::npos);
    EXPECT_NE(body.find("group->isRaidGroup()"), std::string::npos);
    EXPECT_NE(body.find("group->isBGGroup()"), std::string::npos);
    EXPECT_NE(body.find("group->isBFGroup()"), std::string::npos);
    EXPECT_NE(body.find("group->isLFGGroup()"), std::string::npos);
    EXPECT_NE(body.find("group->ChangeLeader(leader->GetGUID())"), std::string::npos);
    EXPECT_NE(body.find("reconfigured ? \"reconfigured\" : \"created\""), std::string::npos);
}

TEST(PartyControlContract, ReconfigurationStillRequiresEveryRequestedPlayerInOneGroup)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("party_roster_group_conflict"), std::string::npos);
    EXPECT_NE(body.find("leader->GetGroup() != group"), std::string::npos);
    EXPECT_NE(body.find("member->GetGroup() != group || !group->IsMember(member->GetGUID())"),
              std::string::npos);
}

TEST(PartyControlContract, FragmentedRequestedSubsetsAreAllPreflightedBeforeDisband)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("std::set<Group*> subsetGroups"), std::string::npos);
    EXPECT_NE(body.find("for (Group* subsetGroup : subsetGroups)"), std::string::npos);
    EXPECT_NE(body.find("allSubsetGroupsOrdinary"), std::string::npos);
    EXPECT_NE(body.find("allSubsetGroupMembersRequested"), std::string::npos);
    EXPECT_NE(body.find("allSubsetGroupMembersOnline"), std::string::npos);
    EXPECT_NE(body.find("allSubsetGroupMembersPlayerbots"), std::string::npos);
    EXPECT_NE(body.find("allGroupedRequestedMembersPresent"), std::string::npos);

    std::size_t const classify = body.find("ClassifyExactParty(signals)");
    std::size_t const disband = body.find("subsetGroup->Disband()");
    ASSERT_NE(classify, std::string::npos);
    ASSERT_NE(disband, std::string::npos);
    EXPECT_LT(classify, disband);
}

TEST(PartyControlContract, FragmentedRepairDisbandsEverySafeGroupThenRebuildsUnderIntendedLeader)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("std::vector<Group*> groupsToDisband(subsetGroups.begin(), subsetGroups.end())"),
              std::string::npos);
    EXPECT_NE(body.find("for (Group* subsetGroup : groupsToDisband)"), std::string::npos);
    EXPECT_NE(body.find("subsetGroup->Disband()"), std::string::npos);
    EXPECT_NE(body.find("group->Create(leader)"), std::string::npos);
    EXPECT_NE(body.find("\"repaired_subset\""), std::string::npos);
}

TEST(PartyControlContract, PartyMembersStartIndependentWhileExplicitOrdersMayCoordinate)
{
    std::string const body = CreatePartyBody();
    ASSERT_FALSE(body.empty());

    std::size_t const leaderSetup = body.find("leaderAI->SetMaster(nullptr)");
    std::size_t const leaderFollowRemoval = body.find("leaderAI->ChangeStrategy(\"-follow\"",
                                                        leaderSetup);
    std::size_t const followerSetup = body.find("memberAI->SetMaster(leader)");
    std::size_t const followerIndependentLoop = body.find(
        "memberAI->ChangeStrategy(\n                    \"+grind,+new rpg,-follow,-move random,-travel\"",
        followerSetup);
    ASSERT_NE(leaderSetup, std::string::npos);
    ASSERT_NE(leaderFollowRemoval, std::string::npos);
    ASSERT_NE(followerSetup, std::string::npos);
    ASSERT_NE(followerIndependentLoop, std::string::npos);
    EXPECT_NE(body.find("leaderAI->SetAutoWowIndependentParty(true)"), std::string::npos);
    EXPECT_NE(body.find("memberAI->SetAutoWowIndependentParty(true)"), std::string::npos);
    EXPECT_LT(leaderSetup, leaderFollowRemoval);
    EXPECT_LT(followerSetup, followerIndependentLoop);
}

TEST(PartyControlContract, RecoveryReturnsFollowersToIndependentNativeLoops)
{
    std::string const body = RecoverQuestPartyBody();
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("memberAI->ChangeStrategy(\"+grind,+new rpg,-follow,-move random,-travel\""),
              std::string::npos);
    EXPECT_EQ(body.find("memberAI->ChangeStrategy(\"+follow,-move random,-grind,-travel,-new rpg\""),
              std::string::npos);
}

TEST(PartyControlContract, NativeGroupMaintenanceRespectsIndependentPartyMode)
{
    std::string const source = ReadPlayerbotAI();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("autoWowIndependentParty = false"), std::string::npos);
    EXPECT_NE(source.find("if (autoWowIndependentParty && !bot->InBattleground())"),
              std::string::npos);
    EXPECT_NE(source.find("re-add +follow every tick"), std::string::npos);
}
