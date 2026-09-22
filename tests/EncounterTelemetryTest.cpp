/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EncounterTelemetry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using AutoWowEncounterTelemetry::CandidateSignal;
using AutoWowEncounterTelemetry::MemberEngagementSignal;

std::string ReadModuleSource(std::string const& relativePath)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

CandidateSignal Candidate(std::uint64_t guid, bool boss, bool engaged, float distance,
                          bool hostile = true)
{
    return {guid, true, true, hostile, boss, engaged, distance};
}
}  // namespace

TEST(EncounterTelemetry, ActiveEngagedAddIsIncluded)
{
    auto const selected = AutoWowEncounterTelemetry::SelectCandidates({Candidate(91, false, true, 42.0f)});
    ASSERT_EQ(selected.candidates.size(), 1u);
    EXPECT_EQ(selected.candidates.front().guid, 91u);
    EXPECT_TRUE(selected.candidates.front().engaged);
}

TEST(EncounterTelemetry, IrrelevantTrashIsExcluded)
{
    auto const selected = AutoWowEncounterTelemetry::SelectCandidates({Candidate(92, false, false, 12.0f)});
    EXPECT_TRUE(selected.candidates.empty());
    EXPECT_EQ(selected.eligibleBeforeCap, 0u);
    EXPECT_FALSE(selected.truncated);
}

TEST(EncounterTelemetry, HostileBossIsIncludedWithoutCurrentEngagement)
{
    auto const selected = AutoWowEncounterTelemetry::SelectCandidates({Candidate(93, true, false, 249.0f)});
    ASSERT_EQ(selected.candidates.size(), 1u);
    EXPECT_EQ(selected.candidates.front().guid, 93u);
    EXPECT_TRUE(selected.candidates.front().boss);
}

TEST(EncounterTelemetry, FriendlyBossAndOutOfRangeOrInvalidUnitsFailClosed)
{
    std::vector<CandidateSignal> candidates = {
        Candidate(1, true, false, 10.0f, false),
        Candidate(2, true, false, 250.01f),
        Candidate(3, false, true, std::numeric_limits<float>::quiet_NaN()),
        {4, false, true, true, true, true, 1.0f},
        {5, true, false, true, true, true, 1.0f},
    };
    EXPECT_TRUE(AutoWowEncounterTelemetry::SelectCandidates(std::move(candidates)).candidates.empty());
}

TEST(EncounterTelemetry, HardCapIsDeterministicBossFirstThenDistanceThenGuid)
{
    std::vector<CandidateSignal> candidates = {
        Candidate(50, false, true, 3.0f), Candidate(40, true, false, 20.0f),
        Candidate(30, true, true, 20.0f), Candidate(20, true, true, 10.0f),
        Candidate(10, false, true, 1.0f), Candidate(20, false, false, 30.0f),
    };

    auto const first = AutoWowEncounterTelemetry::SelectCandidates(candidates, 3);
    std::reverse(candidates.begin(), candidates.end());
    auto const reversed = AutoWowEncounterTelemetry::SelectCandidates(candidates, 3);

    ASSERT_EQ(first.candidates.size(), 3u);
    ASSERT_EQ(reversed.candidates.size(), 3u);
    EXPECT_TRUE(first.truncated);
    EXPECT_EQ(first.eligibleBeforeCap, 5u);
    EXPECT_EQ(first.candidates[0].guid, 20u);
    EXPECT_EQ(first.candidates[1].guid, 30u);
    EXPECT_EQ(first.candidates[2].guid, 40u);
    for (std::size_t index = 0; index < first.candidates.size(); ++index)
        EXPECT_EQ(first.candidates[index].guid, reversed.candidates[index].guid);
}

TEST(EncounterTelemetry, ThreatAggregationCountsUniqueMembersAndUnion)
{
    std::vector<MemberEngagementSignal> signals = {
        {7, true, false, false, true},
        {7, false, true, false, false},
        {8, false, true, true, true},
        {9, false, false, false, false},
    };
    auto const aggregate = AutoWowEncounterTelemetry::AggregateThreat(std::move(signals));
    EXPECT_EQ(aggregate.threateningMembers, 1u);
    EXPECT_EQ(aggregate.targetingMembers, 2u);
    EXPECT_EQ(aggregate.threateningOrTargetingMembers, 2u);
    EXPECT_EQ(aggregate.victimLinks, 1u);
    EXPECT_EQ(aggregate.combatLinks, 2u);
    EXPECT_EQ(aggregate.involvedMembers, 2u);
}

TEST(EncounterTelemetry, PercentIsBoundedAndZeroSafe)
{
    EXPECT_FLOAT_EQ(AutoWowEncounterTelemetry::Percent(5, 0), 0.0f);
    EXPECT_FLOAT_EQ(AutoWowEncounterTelemetry::Percent(25, 100), 25.0f);
    EXPECT_FLOAT_EQ(AutoWowEncounterTelemetry::Percent(125, 100), 100.0f);
}

TEST(EncounterTelemetrySourceContract, ProductionCollectorIsReadOnlyBoundedAndWorldStateBased)
{
    std::string const source = ReadModuleSource("src/AutoWow/EncounterTelemetry.cpp");
    ASSERT_FALSE(source.empty());

    for (std::string_view const forbidden : {
             "TeleportTo(", "NearTeleportTo(", "CastSpell(", "SetHealth(", "SetQuestStatus(",
             "CharacterDatabase", "WorldDatabase", "PlayerbotsDatabase", "SaveToDB(", "AddMember(",
             "RemoveMember(", "ChangeStrategy(", "HandlePacket(", "AreaExploredOrEventHappens(",
             "KilledMonsterCredit(", "RewardPlayerAndGroupAtEvent("})
        EXPECT_EQ(source.find(forbidden), std::string::npos) << forbidden;

    for (std::string_view const required : {
             "Cell::VisitObjects", "kEncounterRadius", "kMaxEncounterUnits", "IsDungeonBoss()",
             "isWorldBoss()", "IsThreatenedBy(member)", "IsInCombatWith(member)",
             "PlayerbotAI::IsTank", "PlayerbotAI::IsHeal", "PLAYER_FLAGS_GHOST",
             "GameTime::GetGameTimeMS", "eligible_before_cap", "truncated"})
        EXPECT_NE(source.find(required), std::string::npos) << required;

    for (std::string_view const jsonField : {
             "leader_guid", "map_id", "instance_id", "difficulty", "timestamp_unix",
             "death_state", "released", "in_combat", "roles", "health", "mana", "power",
             "position", "target", "victim", "active_spell_id", "threat", "encounter_units",
             "rank", "distance", "group_links", "threatening_or_targeting_members"})
        EXPECT_NE(source.find(jsonField), std::string::npos) << jsonField;
}

TEST(EncounterTelemetrySourceContract, BridgeWiringIsSurgicalAndUsesWorldThreadOperation)
{
    std::string const bridge = ReadModuleSource("src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    EXPECT_NE(bridge.find("#include \"EncounterTelemetry.h\""), std::string::npos);
    EXPECT_NE(bridge.find("case AutoWowRequestType::EncounterLog:"), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowEncounterTelemetry::Build(bot, botAI)"), std::string::npos);
    EXPECT_NE(bridge.find("command == \"encounterlog\""), std::string::npos);
    EXPECT_NE(bridge.find("PlayerbotWorldThreadProcessor::instance().QueueOperation"), std::string::npos);
}
