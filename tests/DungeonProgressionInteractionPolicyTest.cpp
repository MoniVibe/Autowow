/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonProgressionInteractionPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace
{
DungeonProgressionInteraction::GateFacts ActiveGate()
{
    return {true, true, true, true, true};
}

DungeonProgressionInteraction::CandidateFacts Candidate(std::uint64_t spawnId, float leaderDistance)
{
    return {spawnId, static_cast<std::uint32_t>(1000 + spawnId), leaderDistance, 20.0f,
            true, true, true, true, true, true};
}

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
}

TEST(DungeonProgressionInteractionPolicy, RequiresReachedLivingLockedBossAndNoCombat)
{
    std::vector<DungeonProgressionInteraction::CandidateFacts> const candidates = {Candidate(10, 5.0f)};
    auto gate = ActiveGate();
    ASSERT_TRUE(DungeonProgressionInteraction::Select(gate, candidates).has_value());

    gate.arrived = false;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(gate, candidates).has_value());
    gate = ActiveGate(); gate.bossAlive = false;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(gate, candidates).has_value());
    gate = ActiveGate(); gate.bossInWorld = false;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(gate, candidates).has_value());
    gate = ActiveGate(); gate.bossNotTargetable = false;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(gate, candidates).has_value());
    gate = ActiveGate(); gate.partyOutOfCombat = false;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(gate, candidates).has_value());
}

TEST(DungeonProgressionInteractionPolicy, RejectsUnsafeUnreadyOrUnrelatedObjects)
{
    auto candidate = Candidate(10, 5.0f);
    for (int field = 0; field < 6; ++field)
    {
        auto rejected = candidate;
        if (field == 0) rejected.inWorld = false;
        if (field == 1) rejected.spawned = false;
        if (field == 2) rejected.ready = false;
        if (field == 3) rejected.selectable = false;
        if (field == 4) rejected.notInUse = false;
        if (field == 5) rejected.supportedType = false;
        EXPECT_FALSE(DungeonProgressionInteraction::Select(ActiveGate(), {rejected}).has_value()) << field;
    }

    candidate.leaderDistance = DungeonProgressionInteraction::MaximumLeaderDistance + 0.1f;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(ActiveGate(), {candidate}).has_value());
    candidate = Candidate(10, 5.0f);
    candidate.bossDistance = DungeonProgressionInteraction::MaximumBossDistance + 0.1f;
    EXPECT_FALSE(DungeonProgressionInteraction::Select(ActiveGate(), {candidate}).has_value());
}

TEST(DungeonProgressionInteractionPolicy, SelectsNearestThenStableSpawnAndAdvancesAfterDespawn)
{
    std::vector<DungeonProgressionInteraction::CandidateFacts> candidates = {
        Candidate(30, 12.0f), Candidate(20, 8.0f), Candidate(10, 8.0f)};
    auto selected = DungeonProgressionInteraction::Select(ActiveGate(), candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);

    candidates[*selected].spawned = false;
    selected = DungeonProgressionInteraction::Select(ActiveGate(), candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 20u);
}

TEST(DungeonProgressionInteractionPolicy, RuntimeUsesNormalOpcodeWithoutInstanceHardcodes)
{
    std::string const source =
        ReadSource(ModuleRoot() / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("DungeonProgressionInteraction::Select"), std::string::npos);
    EXPECT_NE(source.find("CMSG_GAMEOBJ_USE"), std::string::npos);
    EXPECT_NE(source.find("GAMEOBJECT_TYPE_GOOBER"), std::string::npos);
    EXPECT_NE(source.find("GAMEOBJECT_TYPE_BUTTON"), std::string::npos);
    EXPECT_EQ(source.find("188526"), std::string::npos);
    EXPECT_EQ(source.find("188527"), std::string::npos);
    EXPECT_EQ(source.find("188528"), std::string::npos);
}
