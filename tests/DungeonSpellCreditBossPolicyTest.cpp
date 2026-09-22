/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonSpellCreditBossPolicy.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace DungeonSpellCreditBoss;

EncounterGroupFacts Encounter(std::uint32_t mapId, std::uint32_t encounterId,
    bool complete = false, bool hasKillCredit = false)
{
    return {mapId, encounterId, complete, hasKillCredit};
}

StaticSpawnFacts Spawn(std::uint32_t mapId, std::uint32_t spawnMask,
    std::uint32_t spawnId, std::uint32_t entry, std::string scriptName,
    bool mappedByKillCredit = false)
{
    return {mapId, spawnMask, spawnId, entry, std::move(scriptName), mappedByKillCredit};
}

void ExpectBinding(Binding const& binding, std::uint32_t encounterId,
    std::uint32_t spawnId, std::uint32_t entry, std::string_view scriptName)
{
    ASSERT_TRUE(binding.selected);
    EXPECT_EQ(binding.encounterId, encounterId);
    EXPECT_EQ(binding.spawnId, spawnId);
    EXPECT_EQ(binding.entry, entry);
    EXPECT_EQ(binding.scriptName, std::string(scriptName));
}

void ExpectEquivalent(Binding const& left, Binding const& right)
{
    EXPECT_EQ(left.selected, right.selected);
    EXPECT_EQ(left.encounterId, right.encounterId);
    EXPECT_EQ(left.spawnId, right.spawnId);
    EXPECT_EQ(left.entry, right.entry);
    EXPECT_EQ(left.scriptName, right.scriptName);
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

std::string_view FunctionBody(std::string const& source, std::string_view start,
    std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};
    std::size_t const finish = source.find(end, begin + start.size());
    if (finish == std::string::npos)
        return std::string_view(source).substr(begin);
    return std::string_view(source).substr(begin, finish - begin);
}
}

TEST(DungeonSpellCreditBossPolicy, UniqueIncompleteNonKillEncounterBindsUniqueUnmappedBoss)
{
    Binding const binding = Select(41, 2,
        {Encounter(41, 3), Encounter(41, 1, true), Encounter(41, 2, false, true)},
        {Spawn(41, 2, 701, 801, "boss_final_keeper")});

    ExpectBinding(binding, 3, 701, 801, "boss_final_keeper");
}

TEST(DungeonSpellCreditBossPolicy, TwoUnmatchedBossSpawnsAreAmbiguous)
{
    Binding const binding = Select(42, 1, {Encounter(42, 4)},
        {Spawn(42, 1, 710, 810, "boss_north"),
            Spawn(42, 1, 711, 811, "boss_south")});

    EXPECT_FALSE(binding.selected);
}

TEST(DungeonSpellCreditBossPolicy, TwoIncompleteNonKillEncountersAreAmbiguous)
{
    Binding const binding = Select(43, 1, {Encounter(43, 4), Encounter(43, 5)},
        {Spawn(43, 1, 720, 820, "boss_only")});

    EXPECT_FALSE(binding.selected);
}

TEST(DungeonSpellCreditBossPolicy, KillCreditMappedBossesAreExcludedRegardlessOfCompletion)
{
    Binding const binding = Select(44, 1,
        {Encounter(44, 1, true, true), Encounter(44, 2, false, true), Encounter(44, 3)},
        {Spawn(44, 1, 730, 830, "boss_known_complete", true),
            Spawn(44, 1, 731, 831, "boss_known_incomplete", true),
            Spawn(44, 1, 732, 832, "boss_unmapped")});

    ExpectBinding(binding, 3, 732, 832, "boss_unmapped");
}

TEST(DungeonSpellCreditBossPolicy, NonBossAndUnscriptedSpawnsNeverQualify)
{
    std::vector<StaticSpawnFacts> const rejected = {
        Spawn(45, 1, 740, 840, "npc_elite_keeper"),
        Spawn(45, 1, 741, 841, "npc_boss_impostor"),
        Spawn(45, 1, 742, 842, "Boss_wrong_case"),
        Spawn(45, 1, 743, 843, ""),
    };
    EXPECT_FALSE(Select(45, 1, {Encounter(45, 3)}, rejected).selected);

    std::vector<StaticSpawnFacts> withBoss = rejected;
    withBoss.push_back(Spawn(45, 1, 744, 844, "boss_exact_prefix"));
    ExpectBinding(Select(45, 1, {Encounter(45, 3)}, withBoss),
        3, 744, 844, "boss_exact_prefix");
}

TEST(DungeonSpellCreditBossPolicy, MapAndSpawnMaskFilteringRemainFailClosed)
{
    Binding const binding = Select(46, 4,
        {Encounter(99, 1), Encounter(46, 6)},
        {Spawn(99, 4, 750, 850, "boss_other_map"),
            Spawn(46, 2, 751, 851, "boss_inactive_mask"),
            Spawn(46, 4, 752, 852, "boss_active")});

    ExpectBinding(binding, 6, 752, 852, "boss_active");
    EXPECT_FALSE(Select(46, 0, {Encounter(46, 6)},
        {Spawn(46, 4, 752, 852, "boss_active")}).selected);
}

TEST(DungeonSpellCreditBossPolicy, BindingIsDeterministicAcrossInputPermutations)
{
    std::vector<EncounterGroupFacts> encounters = {
        Encounter(47, 3, true), Encounter(47, 4), Encounter(47, 5, false, true)};
    std::vector<StaticSpawnFacts> spawns = {
        Spawn(47, 1, 760, 860, "boss_reserved", true),
        Spawn(47, 1, 761, 861, "npc_not_a_boss"),
        Spawn(47, 1, 762, 862, "boss_unique")};
    Binding const expected = Select(47, 1, encounters, spawns);
    ExpectBinding(expected, 4, 762, 862, "boss_unique");

    std::reverse(encounters.begin(), encounters.end());
    std::reverse(spawns.begin(), spawns.end());
    ExpectEquivalent(expected, Select(47, 1, encounters, spawns));
    std::rotate(encounters.begin(), encounters.begin() + 1, encounters.end());
    std::rotate(spawns.begin(), spawns.begin() + 1, spawns.end());
    ExpectEquivalent(expected, Select(47, 1, encounters, spawns));
}

TEST(DungeonSpellCreditBossSourceContract, AdapterIsGenericAndOnlyFeedsOrdinaryNavigation)
{
    std::filesystem::path const root = ModuleRoot();
    std::string const navigator =
        ReadSource(root / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    std::string const policy =
        ReadSource(root / "src/Ai/Dungeon/Generic/DungeonSpellCreditBossPolicy.h");
    ASSERT_FALSE(navigator.empty());
    ASSERT_FALSE(policy.empty());

    std::string_view const adapter = FunctionBody(navigator,
        "SpellCreditBossFallbackGoal BuildSpellCreditBossFallback(",
        "bool ShouldLogSpellCreditBossFallback(");
    std::string_view const probe = FunctionBody(navigator,
        "if (!complete && spellCreditFallback.binding.selected &&",
        "if (!complete && !bestGoal.spawnId && fallbackGoal.spawnId)");
    ASSERT_FALSE(adapter.empty());
    ASSERT_FALSE(probe.empty());

    EXPECT_NE(adapter.find("ENCOUNTER_CREDIT_KILL_CREATURE"), std::string_view::npos);
    EXPECT_NE(adapter.find("GetCreatureTemplate(spawn.id)"), std::string_view::npos);
    EXPECT_NE(adapter.find("GetScriptName(creatureTemplate->ScriptID)"), std::string_view::npos);
    EXPECT_NE(adapter.find("killMappedSpawnIds"), std::string_view::npos);
    EXPECT_NE(adapter.find("killMappedEntries"), std::string_view::npos);
    EXPECT_NE(probe.find("AutoWowDungeonPath::Probe"), std::string_view::npos);
    EXPECT_NE(navigator.find("reason=unique_unmapped_spell_credit_boss"), std::string::npos);
    EXPECT_NE(navigator.find("bool const attacked = Attack(creature);"), std::string::npos);
    EXPECT_EQ(probe.find("Attack("), std::string_view::npos);

    std::string const controlledSource = policy + std::string(adapter) + std::string(probe);
    for (std::string_view const forbiddenApi : {
             "SetBossState(", "SetData(", "SetCompletedEncounterMask(",
             "UpdateEncounterState(", "CompleteEncounter(", "CastSpell(",
             "TeleportTo(", "NearTeleportTo("})
    {
        EXPECT_EQ(controlledSource.find(forbiddenApi), std::string::npos) << forbiddenApi;
    }

    std::array<std::string, 4> const forbiddenContentIds = {
        std::to_string(6u * 10u * 10u),
        std::to_string(26u * 1000u + 632u),
        std::to_string(2u * 100000u + 143u),
        std::to_string(61u * 1000u + 863u),
    };
    std::string const ownedProductionSource = policy + navigator;
    for (std::string const& id : forbiddenContentIds)
        EXPECT_EQ(ownedProductionSource.find(id), std::string::npos) << id;
}
