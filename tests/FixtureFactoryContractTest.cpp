/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/FixtureFactoryControl.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
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

TEST(FixtureFactoryContract, DefaultClosedAndAllowlistIsExact)
{
    EXPECT_FALSE(AutoWowFixture::IsFixtureGuidAllowlisted("", 42));
    EXPECT_FALSE(AutoWowFixture::IsFixtureGuidAllowlisted("   ", 42));
    EXPECT_FALSE(AutoWowFixture::IsFixtureGuidAllowlisted("41,43", 42));
    EXPECT_TRUE(AutoWowFixture::IsFixtureGuidAllowlisted("41, 42", 42));
    EXPECT_TRUE(AutoWowFixture::IsFixtureGuidAllowlisted("41 42", 42));
    EXPECT_FALSE(AutoWowFixture::IsFixtureGuidAllowlisted("not-a-guid-42", 42));
}

TEST(FixtureFactoryContract, RequestBoundsAreClosed)
{
    EXPECT_TRUE(AutoWowFixture::IsValidLevel(1, 80));
    EXPECT_TRUE(AutoWowFixture::IsValidLevel(80, 80));
    EXPECT_FALSE(AutoWowFixture::IsValidLevel(0, 80));
    EXPECT_FALSE(AutoWowFixture::IsValidLevel(81, 80));

    EXPECT_TRUE(AutoWowFixture::IsValidSpecIndex(0));
    EXPECT_TRUE(AutoWowFixture::IsValidSpecIndex(AutoWowFixture::kMaxSpecIndexExclusive - 1));
    EXPECT_FALSE(AutoWowFixture::IsValidSpecIndex(AutoWowFixture::kMaxSpecIndexExclusive));

    for (uint32 quality = AutoWowFixture::kMinQuality; quality <= AutoWowFixture::kMaxQuality; ++quality)
        EXPECT_TRUE(AutoWowFixture::IsValidQuality(quality));
    EXPECT_FALSE(AutoWowFixture::IsValidQuality(AutoWowFixture::kMaxQuality + 1));
}

TEST(FixtureFactoryContract, ConfigAndBridgeExposeOnlyTheBoundedFixtureOrders)
{
    std::string const config = ReadSource(ModuleRoot() / "conf/playerbots.conf.dist");
    ASSERT_FALSE(config.empty());
    EXPECT_NE(config.find("AutoWow.FixtureGuids = \"\""), std::string::npos);

    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    EXPECT_NE(bridge.find("command == \"fixture\""), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowFixture::Init"), std::string::npos);
    EXPECT_NE(bridge.find("AutoWowFixture::Status"), std::string::npos);
    EXPECT_NE(bridge.find("IsValidSpecIndex"), std::string::npos);
    EXPECT_NE(bridge.find("IsValidQuality"), std::string::npos);
}

TEST(FixtureFactoryContract, ExactSpecIsPassedThroughTheFactoryAndSaved)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    ASSERT_FALSE(control.empty());
    EXPECT_NE(control.find("PlayerbotFactory factory(target.player, level, quality)"), std::string::npos);
    EXPECT_NE(control.find("factory.InitializeFixture(specIndex, quality)"), std::string::npos);
    std::size_t const initializePosition = control.find("factory.InitializeFixture(specIndex, quality)");
    std::size_t const resetPosition = control.find("target.ai->ResetStrategies(false)", initializePosition);
    EXPECT_NE(resetPosition, std::string::npos)
        << "fixture-init must rebuild AI strategies after changing talents so live roles are not stale";

    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(factory.empty());
    std::string_view const body = FunctionBody(factory, "bool PlayerbotFactory::InitializeFixture",
                                               "void PlayerbotFactory::Refresh");
    ASSERT_FALSE(body.empty());

    std::array<std::string_view, 8> const orderedCalls = {
        "bot->GiveLevel(level)",
        "bot->LearnDefaultSkills()",
        "InitSkills()",
        "InitClassSpells()",
        "InitAvailableSpells()",
        "InitTalentsBySpecNo(bot, static_cast<int>(specIndex), true)",
        "InitEquipment(false, false)",
        "bot->SaveToDB(false, false)",
    };

    std::size_t previous = 0;
    for (std::string_view const call : orderedCalls)
    {
        std::size_t const position = body.find(call, previous);
        ASSERT_NE(position, std::string_view::npos) << call;
        previous = position + call.size();
    }
}

TEST(FixtureFactoryContract, FixturePathHasNoRawSqlQuestOrCombatSurface)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    ASSERT_FALSE(control.empty());

    for (std::string_view const forbidden : {"SELECT ", "Database", "Query(", "Execute(", "InitQuests",
                                               "ResetQuests", "CombatStop", "DoSpecificAction", "Randomize(",
                                               "AddPlayerBot", "LogoutPlayerBot"})
    {
        EXPECT_EQ(control.find(forbidden), std::string::npos) << forbidden;
    }

    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(factory.empty());
    std::string_view const body = FunctionBody(factory, "bool PlayerbotFactory::InitializeFixture",
                                               "void PlayerbotFactory::Refresh");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(body.find("Quest"), std::string_view::npos);
    EXPECT_EQ(body.find("Combat"), std::string_view::npos);
}

TEST(FixtureFactoryContract, RequestedGearQualityIsExactWhenAValidCandidateExists)
{
    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(factory.empty());

    std::string_view const fixtureBody = FunctionBody(factory, "bool PlayerbotFactory::InitializeFixture",
                                                      "void PlayerbotFactory::Refresh");
    ASSERT_FALSE(fixtureBody.empty());
    EXPECT_NE(fixtureBody.find("fixtureExactItemQuality = true"), std::string_view::npos);
    EXPECT_NE(fixtureBody.find("fixtureExactItemQuality = false"), std::string_view::npos);
    EXPECT_NE(fixtureBody.find("bot->SetHealth(bot->GetMaxHealth())"), std::string_view::npos);
    EXPECT_NE(fixtureBody.find("bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA))"), std::string_view::npos);

    std::string_view const equipmentBody = FunctionBody(factory, "void PlayerbotFactory::InitEquipment",
                                                        "void PlayerbotFactory::InitPet");
    ASSERT_FALSE(equipmentBody.empty());
    EXPECT_NE(equipmentBody.find("!fixtureExactItemQuality && items[slot].size() < 25"), std::string_view::npos);
    EXPECT_NE(equipmentBody.find("!fixtureExactItemQuality && urand(1, 100) <= skipProb"), std::string_view::npos);
    EXPECT_NE(equipmentBody.find("if (fixtureExactItemQuality)"), std::string_view::npos);
    EXPECT_NE(equipmentBody.find("bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true)"),
              std::string_view::npos);
}

TEST(FixtureFactoryContract, ExactQualityAndDownlevelFailuresAreReportedHonestly)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(control.empty());
    ASSERT_FALSE(factory.empty());

    EXPECT_NE(control.find("target.player->GetLevel() > level"), std::string::npos);
    EXPECT_NE(control.find("fixture_downlevel_reuse_refused"), std::string::npos);
    EXPECT_NE(control.find("if (!factory.InitializeFixture(specIndex, quality))"), std::string::npos);
    EXPECT_NE(control.find("fixture_exact_quality_unavailable"), std::string::npos);

    std::string_view const fixtureBody = FunctionBody(factory, "bool PlayerbotFactory::InitializeFixture",
                                                      "void PlayerbotFactory::Refresh");
    ASSERT_FALSE(fixtureBody.empty());
    EXPECT_NE(fixtureBody.find("itemTemplate->Quality != requestedQuality"), std::string_view::npos);
    EXPECT_NE(fixtureBody.find("return false"), std::string_view::npos);
    EXPECT_LT(fixtureBody.find("return false"), fixtureBody.find("bot->SaveToDB(false, false)"));
    EXPECT_NE(fixtureBody.find("return true"), std::string_view::npos);
}

TEST(FixtureFactoryContract, FixtureInventoryResetPreservesEquipmentAndReservesLootSpace)
{
    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(factory.empty());

    std::string_view const fixtureBody = FunctionBody(factory, "bool PlayerbotFactory::InitializeFixture",
                                                      "void PlayerbotFactory::Refresh");
    ASSERT_FALSE(fixtureBody.empty());

    std::array<std::string_view, 7> const orderedCalls = {
        "InitEquipment(false, false)",
        "ClearFixtureCarriedItems()",
        "InitBags(true)",
        "InitAmmo()",
        "InitReagents()",
        "InitConsumables()",
        "EnsureFixtureLootSlotReserve(kFixtureLootSlotReserve)",
    };

    std::size_t previous = 0;
    for (std::string_view const call : orderedCalls)
    {
        std::size_t const position = fixtureBody.find(call, previous);
        ASSERT_NE(position, std::string_view::npos) << call;
        previous = position + call.size();
    }

    EXPECT_EQ(fixtureBody.find("ClearInventory()"), std::string_view::npos);
    EXPECT_EQ(fixtureBody.find("ClearAllItems()"), std::string_view::npos);
    EXPECT_EQ(fixtureBody.find("InitFood()"), std::string_view::npos);
    EXPECT_EQ(fixtureBody.find("InitPotions()"), std::string_view::npos);

    std::string_view const cleanupBody = FunctionBody(factory, "void PlayerbotFactory::ClearFixtureCarriedItems",
                                                      "namespace\n{");
    ASSERT_FALSE(cleanupBody.empty());
    EXPECT_NE(cleanupBody.find("INVENTORY_SLOT_ITEM_START"), std::string_view::npos);
    EXPECT_NE(cleanupBody.find("INVENTORY_SLOT_BAG_START"), std::string_view::npos);
    EXPECT_NE(cleanupBody.find("bot->DestroyItem"), std::string_view::npos);
    EXPECT_EQ(cleanupBody.find("EQUIPMENT_SLOT_START"), std::string_view::npos);
    EXPECT_EQ(cleanupBody.find("ITERATE_ALL_ITEMS"), std::string_view::npos);

    std::string const header = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.h");
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("kFixtureLootSlotReserve = 8"), std::string::npos);
}

TEST(FixtureFactoryContract, EvidenceReportsLootCapacityAndReserveVerdict)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    ASSERT_FALSE(control.empty());

    for (std::string_view const field : {"\\\"backpack_capacity\\\"", "\\\"equipped_bag_count\\\"",
                                         "\\\"bag_capacity\\\"", "\\\"total_capacity\\\"",
                                         "\\\"free_slots\\\"", "\\\"carried_item_count\\\"",
                                         "\\\"loot_slot_reserve\\\"", "\\\"loot_slot_reserve_met\\\""})
    {
        EXPECT_NE(control.find(field), std::string::npos) << field;
    }
}

TEST(FixtureFactoryContract, EvidenceExposesExactIdentityAndRoleReadinessForOfflineOracleChecks)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    ASSERT_FALSE(control.empty());

    for (std::string_view const field : {"\\\"identity\\\"", "account_id", "race_id", "faction",
                                         "\\\"talents\\\"", "GetTalentMap", "\\\"spells\\\"",
                                         "GetSpellMap", "role_critical_ready", "\\\"weapons\\\"",
                                         "EQUIPMENT_SLOT_MAINHAND", "EQUIPMENT_SLOT_RANGED"})
    {
        EXPECT_NE(control.find(field), std::string::npos) << field;
    }
}

TEST(FixtureFactoryContract, SuccessfulGroupRollAwardsHaveRaidLootTelemetry)
{
    std::string const scripts = ReadSource(ModuleRoot() / "src/Script/Playerbots.cpp");
    ASSERT_FALSE(scripts.empty());

    EXPECT_NE(scripts.find("PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM"), std::string::npos);
    std::string_view const body = FunctionBody(scripts, "void OnPlayerGroupRollRewardItem",
                                               "\n    }\n};");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("[RaidLoot] event=award"), std::string_view::npos);
    for (std::string_view const field : {"winner={}", "item={}", "item_guid={}", "vote={}"})
        EXPECT_NE(body.find(field), std::string_view::npos) << field;
}

TEST(FixtureFactoryContract, PlayerbotAndLeagueRoleGatesAreExplicit)
{
    std::string const control = ReadSource(ModuleRoot() / "src/AutoWow/FixtureFactoryControl.cpp");
    ASSERT_FALSE(control.empty());
    EXPECT_NE(control.find("FindConnectedPlayer"), std::string::npos);
    EXPECT_NE(control.find("GetPlayerbotAI"), std::string::npos);
    EXPECT_NE(control.find("IsRealPlayer"), std::string::npos);
    EXPECT_NE(control.find("racer"), std::string::npos);
    EXPECT_NE(control.find("captain"), std::string::npos);
}
