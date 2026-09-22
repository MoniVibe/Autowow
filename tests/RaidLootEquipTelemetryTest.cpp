/*
 * Focused source-contract tests for behavior-neutral, exact-instance raid-loot telemetry.
 */

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

void ExpectNoBehaviorMutation(std::string_view body)
{
    for (std::string_view const mutation : {"StoreNewItem(", "SwapItem(", "DestroyItem(", "SetAmmo(",
                                                  "HandleAutoEquipItemSlotOpcode(", "EquipItem("})
        EXPECT_EQ(body.find(mutation), std::string_view::npos) << mutation;
}
}  // namespace

TEST(RaidLootEquipTelemetry, AwardContainsSyntheticRollAndAwardedInstanceIdentities)
{
    std::string const scripts = ReadSource(ModuleRoot() / "src/Script/Playerbots.cpp");
    ASSERT_FALSE(scripts.empty());
    std::string_view const body = FunctionBody(scripts, "void OnPlayerGroupRollRewardItem", "\n    }\n};");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("[RaidLoot] event=award"), std::string_view::npos);
    for (std::string_view const field : {"winner={}", "winner_guid={}", "winner_guid_counter={}", "entry={}",
                                         "roll_guid={}", "roll_guid_counter={}", "item_guid={}",
                                         "item_guid_counter={}", "count={}", "vote={}"})
        EXPECT_NE(body.find(field), std::string_view::npos) << field;
    EXPECT_NE(body.find("roll->itemGUID.GetRawValue()"), std::string_view::npos);
    EXPECT_NE(body.find("roll->itemGUID.GetCounter()"), std::string_view::npos);
    ExpectNoBehaviorMutation(body);
}

TEST(RaidLootEquipTelemetry, EvaluationReportsTheExistingPerInstanceUsageResultWithoutMutation)
{
    std::string const equip = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/EquipAction.cpp");
    ASSERT_FALSE(equip.empty());
    std::string_view const scan = FunctionBody(equip, "ItemIds EquipAction::SelectInventoryItemsToEquip",
                                               "bool EquipUpgradesPacketAction::Execute");
    ASSERT_FALSE(scan.empty());

    std::size_t const usageRead = scan.find("AI_VALUE2(ItemUsage, \"item upgrade\"");
    ASSERT_NE(usageRead, std::string_view::npos);
    EXPECT_EQ(scan.find("AI_VALUE2(ItemUsage", usageRead + 1), std::string_view::npos)
        << "telemetry must report the ItemUsage value already computed by the inventory scan";
    EXPECT_NE(scan.find("[RaidLoot] event=evaluate"), std::string_view::npos);
    for (std::string_view const field : {"trigger=inventory_upgrade_scan", "source={}", "bot_guid={}",
                                         "bot_guid_counter={}", "entry={}", "item_guid={}",
                                         "item_guid_counter={}", "usage={}", "selected=true"})
        EXPECT_NE(scan.find(field), std::string_view::npos) << field;
    EXPECT_NE(scan.find("item->GetGUID().GetRawValue()"), std::string_view::npos);
    EXPECT_NE(scan.find("item->GetGUID().GetCounter()"), std::string_view::npos);
    ExpectNoBehaviorMutation(scan);
}

TEST(RaidLootEquipTelemetry, EquipSuccessRemainsBehindTheExactSlotInstancePostcondition)
{
    std::string const equip = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/EquipAction.cpp");
    ASSERT_FALSE(equip.empty());
    std::string_view const body = FunctionBody(equip, "void LogRaidLootEquip", "\n}\n}");
    ASSERT_FALSE(body.empty());

    std::size_t const postcondition = body.find("equipped->GetGUID() != expectedItem->GetGUID()");
    std::size_t const logCall = body.find("LOG_INFO");
    ASSERT_NE(postcondition, std::string_view::npos);
    ASSERT_NE(logCall, std::string_view::npos);
    EXPECT_LT(postcondition, logCall);
    for (std::string_view const field : {"item_guid_counter={}", "slot={}", "previous_item={}",
                                         "previous_item_guid_counter={}", "result=equipped"})
        EXPECT_NE(body.find(field), std::string_view::npos) << field;
    ExpectNoBehaviorMutation(body);
}
