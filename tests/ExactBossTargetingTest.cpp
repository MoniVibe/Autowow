/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/ExactBossTargetControl.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

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

std::string_view FunctionBody(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};
    std::size_t const finish = source.find(end, begin + start.size());
    if (finish == std::string::npos)
        return std::string_view(source).substr(begin);
    return std::string_view(source).substr(begin, finish - begin);
}
}  // namespace

TEST(ExactBossTargeting, LegacyBroadWireAndExactEntryWireAreDistinct)
{
    AutoWowExactBossTarget::WireRequest request;
    std::string error;

    EXPECT_TRUE(AutoWowExactBossTarget::ParseWireRequest("engage 42", request, error));
    EXPECT_EQ(request.mode, AutoWowExactBossTarget::EngageMode::Nearby);
    EXPECT_EQ(request.leaderGuid, 42u);
    EXPECT_EQ(request.creatureEntry, 0u);
    EXPECT_TRUE(error.empty());

    EXPECT_TRUE(AutoWowExactBossTarget::ParseWireRequest("engage 42 entry 36597", request, error));
    EXPECT_EQ(request.mode, AutoWowExactBossTarget::EngageMode::CreatureEntry);
    EXPECT_EQ(request.leaderGuid, 42u);
    EXPECT_EQ(request.creatureEntry, 36597u);
    EXPECT_TRUE(error.empty());

    EXPECT_TRUE(AutoWowExactBossTarget::ParseWireRequest("engage 42 player 77", request, error));
    EXPECT_EQ(request.mode, AutoWowExactBossTarget::EngageMode::PlayerGuid);
    EXPECT_EQ(request.leaderGuid, 42u);
    EXPECT_EQ(request.playerGuid, 77u);
    EXPECT_TRUE(error.empty());
}

TEST(ExactBossTargeting, MalformedExactSelectorNeverFallsBackToBroadEngage)
{
    for (std::string const& text : {
             "engage", "engage 0", "engage -1", "engage 42 entry", "engage 42 entry 0",
             "engage 42 entry -1", "engage 42 entry 36597 extra", "engage 42 guid 36597",
             "engage 42 36597", "engage 42 entry 4294967296", "engage 42 player",
             "engage 42 player 0", "engage 42 player -1", "engage 42 player 77 extra"})
    {
        AutoWowExactBossTarget::WireRequest request;
        std::string error;
        EXPECT_FALSE(AutoWowExactBossTarget::ParseWireRequest(text, request, error)) << text;
        EXPECT_EQ(request.mode, AutoWowExactBossTarget::EngageMode::Nearby) << text;
        EXPECT_EQ(request.creatureEntry, 0u) << text;
        EXPECT_EQ(request.playerGuid, 0u) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(ExactBossTargeting, CandidateMustPassEveryExactScopeAndHostilityGate)
{
    AutoWowExactBossTarget::TargetFacts valid = {
        true, true, true, true, true, true, AutoWowExactBossTarget::kMaxTargetDistance,
    };
    std::string error;
    EXPECT_TRUE(AutoWowExactBossTarget::ValidateTargetFacts(valid, error));
    EXPECT_TRUE(error.empty());

    auto expectRejected = [](AutoWowExactBossTarget::TargetFacts const& facts, std::string_view expectedError)
    {
        std::string error;
        EXPECT_FALSE(AutoWowExactBossTarget::ValidateTargetFacts(facts, error));
        EXPECT_EQ(error, expectedError);
    };

    AutoWowExactBossTarget::TargetFacts changed = valid;
    changed.inWorld = false;
    expectRejected(changed, "exact_engage_target_not_in_world");
    changed = valid;
    changed.sameMap = false;
    expectRejected(changed, "exact_engage_target_wrong_map");
    changed = valid;
    changed.sameInstance = false;
    expectRejected(changed, "exact_engage_target_wrong_instance");
    changed = valid;
    changed.alive = false;
    expectRejected(changed, "exact_engage_target_dead");
    changed = valid;
    changed.hostile = false;
    expectRejected(changed, "exact_engage_target_not_hostile");
    changed = valid;
    changed.attackable = false;
    expectRejected(changed, "exact_engage_target_not_attackable");
    changed = valid;
    changed.distance = AutoWowExactBossTarget::kMaxTargetDistance + 0.01f;
    expectRejected(changed, "exact_engage_target_out_of_range");
    changed = valid;
    changed.distance = -0.01f;
    expectRejected(changed, "exact_engage_target_out_of_range");
    changed = valid;
    changed.distance = std::numeric_limits<float>::quiet_NaN();
    expectRejected(changed, "exact_engage_target_out_of_range");
}

TEST(ExactBossTargeting, BridgeKeepsBroadEngageAndIssuesExactIntentThroughNormalPartyAttack)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());

    EXPECT_NE(bridge.find("DoSpecificAction(\"attack anything\", Event(), true)"), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowExactBossTarget::ParseWireRequest"), std::string::npos);
    EXPECT_NE(bridge.find("exact_engage_requires_grouped_leader"), std::string::npos);
    EXPECT_NE(bridge.find("IsLeagueMember(m_request.botGuid)"), std::string::npos);

    std::string_view const engage = FunctionBody(bridge, "bool EngageNearby()", "bool ScoutNearby()");
    ASSERT_FALSE(engage.empty());
    std::size_t const allowlistGate = engage.find("IsLeagueMember(m_request.botGuid)");
    std::size_t const exactModeGate = engage.find("if (m_request.exactCreatureEntry || m_request.exactPlayerGuid)");
    std::size_t const creatureDispatch = engage.find("return EngageExactCreature(bot, group)");
    std::size_t const playerDispatch = engage.find("return EngageExactPlayer(bot, group)");
    std::size_t const broadDispatch = engage.find("DoSpecificAction(\"attack anything\", Event(), true)");
    ASSERT_NE(allowlistGate, std::string_view::npos);
    ASSERT_NE(exactModeGate, std::string_view::npos);
    ASSERT_NE(creatureDispatch, std::string_view::npos);
    ASSERT_NE(playerDispatch, std::string_view::npos);
    ASSERT_NE(broadDispatch, std::string_view::npos);
    EXPECT_LT(allowlistGate, exactModeGate);
    EXPECT_LT(exactModeGate, creatureDispatch);
    EXPECT_LT(creatureDispatch, playerDispatch);
    EXPECT_LT(playerDispatch, broadDispatch);

    std::string_view const exact = FunctionBody(
        bridge, "bool EngageExactUnit(Player* leader, Group* group", "bool EngageExactCreature(");
    ASSERT_FALSE(exact.empty());
    for (std::string_view const required : {
             "target->GetMapId()", "target->GetInstanceId()",
             "target->IsAlive()", "target->IsHostileTo", "IsValidAttackTarget(target)", "GetDistance(target)",
             "ValidateTargetFacts", "group->GetFirstMember()", "AttackExact(target)"})
    {
        EXPECT_NE(exact.find(required), std::string_view::npos) << required;
    }

    std::string_view const creature = FunctionBody(
        bridge, "bool EngageExactCreature(", "bool EngageExactPlayer(");
    EXPECT_NE(creature.find("FindNearestCreature"), std::string_view::npos);
    EXPECT_NE(creature.find("target->GetEntry()"), std::string_view::npos);
    std::string_view const player = FunctionBody(
        bridge, "bool EngageExactPlayer(", "bool EngageNearby()");
    EXPECT_NE(player.find("ObjectAccessor::FindConnectedPlayer"), std::string_view::npos);
    EXPECT_NE(player.find("m_request.exactPlayerGuid"), std::string_view::npos);
    EXPECT_NE(player.find("EngageExactUnit"), std::string_view::npos);
}

TEST(ExactBossTargeting, WholePartyPreflightCompletesBeforeAnyAttackIntent)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    std::string_view const exact = FunctionBody(
        bridge, "bool EngageExactUnit(Player* leader, Group* group", "bool EngageExactCreature(");
    ASSERT_FALSE(exact.empty());

    std::size_t const preflight = exact.find("Preflight the complete roster");
    std::size_t const intent = exact.find("prioritized targets");
    ASSERT_NE(preflight, std::string_view::npos);
    ASSERT_NE(intent, std::string_view::npos);
    EXPECT_LT(preflight, intent);

    for (std::string_view const gate : {
             "exact_engage_requires_online_playerbot_party", "exact_engage_party_member_paused",
             "exact_engage_requires_alive_party", "exact_target_not_attackable_for_entire_party",
             "exact_target_not_in_entire_party_los", "exact_engage_invalid_party_roster"})
    {
        EXPECT_NE(exact.find(gate), std::string_view::npos) << gate;
    }
}

TEST(ExactBossTargeting, ExactPathContainsNoWorldOrPersistenceShortcuts)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    std::string_view const exact = FunctionBody(
        bridge, "bool EngageExactUnit(Player* leader, Group* group", "bool EngageExactCreature(");
    ASSERT_FALSE(exact.empty());

    for (std::string_view const forbidden : {
             "TeleportTo", "NearTeleportTo", "SummonCreature", "CreateCreature", "KillPlayer",
             "DealDamage", "SaveToDB", "Execute(", "CommitTransaction", "PlayerbotsDatabase"})
    {
        EXPECT_EQ(exact.find(forbidden), std::string_view::npos) << forbidden;
    }
}
