/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/WarsongFixtureControl.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<uint32> ExactRoster()
{
    std::vector<uint32> roster(AutoWowWarsong::kRosterSize);
    std::iota(roster.begin(), roster.end(), 1001u);
    return roster;
}

std::string CanonicalWire(std::string_view subcommand, std::vector<uint32> const& roster)
{
    std::ostringstream wire;
    wire << "wsg " << subcommand;
    for (uint32 guid : roster)
        wire << ' ' << guid;
    return wire.str();
}
}  // namespace

TEST(WarsongFixtureControl, RosterShapeIsExactlyTwentyPositiveUniqueGuids)
{
    std::string error;
    std::vector<uint32> roster = ExactRoster();
    EXPECT_TRUE(AutoWowWarsong::ValidateRosterShape(roster, error));
    EXPECT_TRUE(error.empty());

    roster.pop_back();
    EXPECT_FALSE(AutoWowWarsong::ValidateRosterShape(roster, error));
    EXPECT_EQ(error, "wsg_requires_exactly_20_guids");

    roster = ExactRoster();
    roster.back() = roster.front();
    EXPECT_FALSE(AutoWowWarsong::ValidateRosterShape(roster, error));
    EXPECT_EQ(error, "wsg_guids_must_be_unique");

    roster = ExactRoster();
    roster.back() = 0;
    EXPECT_FALSE(AutoWowWarsong::ValidateRosterShape(roster, error));
    EXPECT_EQ(error, "wsg_guids_must_be_positive");
}

TEST(WarsongFixtureControl, SameRosterIsOrderIndependentButExact)
{
    std::vector<uint32> roster = ExactRoster();
    std::vector<uint32> reversed = roster;
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_TRUE(AutoWowWarsong::IsSameRoster(roster, reversed));

    reversed.back() = 999999u;
    EXPECT_FALSE(AutoWowWarsong::IsSameRoster(roster, reversed));
}

TEST(WarsongFixtureControl, BridgeAcceptsCanonicalPowerShellWsgWire)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    EXPECT_NE(bridge.find("command == \"wsg\""), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowWarsong::ParseWireRequest(requestText"), std::string::npos);
    EXPECT_NE(bridge.find("operation == AutoWowWarsong::WireOperation::Queue"), std::string::npos);
    EXPECT_NE(bridge.find("operation == AutoWowWarsong::WireOperation::Status"), std::string::npos);

    // Dashed spellings remain compatibility aliases; the canonical wire above is what PowerShell emits.
    EXPECT_NE(bridge.find("command == \"wsg-queue\""), std::string::npos);
    EXPECT_NE(bridge.find("command == \"wsg-status\""), std::string::npos);
    EXPECT_NE(bridge.find("command == \"wsg-leave\""), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowWarsong::Queue"), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowWarsong::Status"), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowWarsong::Leave"), std::string::npos);
}

TEST(WarsongFixtureControl, CanonicalPowerShellWireParsesAllThreeOperations)
{
    std::vector<uint32> const expectedRoster = ExactRoster();
    struct Case
    {
        std::string_view subcommand;
        AutoWowWarsong::WireOperation operation;
    };

    for (Case const testCase : {
             Case{"queue", AutoWowWarsong::WireOperation::Queue},
             Case{"status", AutoWowWarsong::WireOperation::Status},
             Case{"leave", AutoWowWarsong::WireOperation::Leave}})
    {
        AutoWowWarsong::WireOperation parsedOperation = AutoWowWarsong::WireOperation::Status;
        std::vector<uint32> parsedRoster;
        std::string error;
        EXPECT_TRUE(AutoWowWarsong::ParseWireRequest(
            CanonicalWire(testCase.subcommand, expectedRoster), parsedOperation, parsedRoster, error));
        EXPECT_EQ(parsedOperation, testCase.operation);
        EXPECT_EQ(parsedRoster, expectedRoster);
        EXPECT_TRUE(error.empty());
    }
}

TEST(WarsongFixtureControl, CanonicalPowerShellStatusFieldNamesAreEmitted)
{
    constexpr std::array<std::string_view, 7> expectedRoot = {
        "ok", "instance_id", "battleground_state", "roster", "score", "flags", "winner"};
    constexpr std::array<std::string_view, 14> expectedPlayer = {
        "guid", "faction", "team", "instance_id", "queue_state", "battleground_state", "kills",
        "deaths", "honorable_kills", "damage", "healing", "flag_state", "flag_captures", "flag_returns"};
    EXPECT_EQ(AutoWowWarsong::kCanonicalStatusRootFields, expectedRoot);
    EXPECT_EQ(AutoWowWarsong::kCanonicalStatusPlayerFields, expectedPlayer);

    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/WarsongFixtureControl.cpp");
    ASSERT_FALSE(control.empty());
    for (std::string_view const field : expectedRoot)
        EXPECT_NE(control.find("\\\"" + std::string(field) + "\\\""), std::string::npos) << field;
    for (std::string_view const field : expectedPlayer)
        EXPECT_NE(control.find("\\\"" + std::string(field) + "\\\""), std::string::npos) << field;

    EXPECT_NE(control.find("case STATUS_WAIT_LEAVE: return \"complete\""), std::string::npos);
    EXPECT_NE(control.find("\\\"captures\\\":"), std::string::npos);
    EXPECT_NE(control.find("\\\"returns\\\":"), std::string::npos);
}

TEST(WarsongFixtureControl, QueueUsesCanonicalSoloJoinAndCanonicalLeaveOnly)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/WarsongFixtureControl.cpp");
    ASSERT_FALSE(control.empty());

    EXPECT_NE(control.find("uint8 const joinAsGroup = 0"), std::string::npos);
    EXPECT_NE(control.find("HandleBattlemasterJoinOpcode"), std::string::npos);
    EXPECT_NE(control.find("HandleBattleFieldPortOpcode"), std::string::npos);
    EXPECT_NE(control.find("HandleBattlefieldLeaveOpcode"), std::string::npos);

    for (std::string_view const forbidden : {".AddGroup(", "AddBattlegroundQueueId(",
                                             "RemoveBattlegroundQueueId(", "SetBattlegroundId(",
                                             "TeleportTo("})
    {
        EXPECT_EQ(control.find(forbidden), std::string::npos) << forbidden;
    }
}

TEST(WarsongFixtureControl, LeaveUsesCanonicalClientPathEvenDuringCombat)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/WarsongFixtureControl.cpp");
    ASSERT_FALSE(control.empty());

    EXPECT_NE(control.find("HandleBattlefieldLeaveOpcode"), std::string::npos);
    EXPECT_NE(control.find("wsg_leave_queue_target_charmed"), std::string::npos);
    EXPECT_EQ(control.find("wsg_leave_target_in_combat"), std::string::npos);
    EXPECT_EQ(control.find("wsg_leave_queue_target_in_combat_or_charmed"), std::string::npos);
}

TEST(WarsongFixtureControl, FailClosedAndExactRosterProofsAreExplicit)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/WarsongFixtureControl.cpp");
    ASSERT_FALSE(control.empty());

    for (std::string_view const required : {
             "randomBotJoinBG", "randomBotAutoJoinBG", "isTesting()", "isArenaTesting()",
             "GetPlayerbotAI", "IsRealPlayer", "autowow_league_member", "wsg_roster_levels_differ",
             "wsg_roster_requires_10_alliance_10_horde", "wsg_foreign_queue_players_in_bracket",
             "wsg_foreign_active_players_in_bracket", "m_QueuedPlayers", "exact_queue_roster",
             "exact_roster", "same_instance", "ten_v_ten", "wsg_queue_rollback_failed"})
    {
        EXPECT_NE(control.find(required), std::string::npos) << required;
    }
}

TEST(WarsongFixtureControl, StatusContainsWsgObjectiveAndPerPlayerEvidence)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/WarsongFixtureControl.cpp");
    ASSERT_FALSE(control.empty());

    for (std::string_view const required : {
             "GetTeamScore", "GetFlagState", "GetFlagPickerGUID", "BuildPvPLogDataPacket",
             "kills", "deaths", "honorable_kills", "damage", "healing", "captures", "returns",
             "winner", "world_map", "queue", "invite", "battleground", "bracket", "instance", "team"})
    {
        EXPECT_NE(control.find(required), std::string::npos) << required;
    }
}
