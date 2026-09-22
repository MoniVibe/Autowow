/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}
}

TEST(MageCounterspellStrategyContract, WiresCurrentTargetAndPreservesEnemyHealer)
{
    std::filesystem::path const strategyPath =
        ModuleRoot() / "src/Ai/Class/Mage/Strategy/GenericMageStrategy.cpp";
    std::ifstream strategy(strategyPath, std::ios::in | std::ios::binary);
    ASSERT_TRUE(strategy.is_open()) << "Could not open " << strategyPath.string();

    std::string const source((std::istreambuf_iterator<char>(strategy)), std::istreambuf_iterator<char>());
    std::size_t const mageStart = source.find("void GenericMageStrategy::InitTriggers");
    ASSERT_NE(mageStart, std::string::npos);

    std::size_t const mageEnd = source.find("void MageCureStrategy::InitTriggers", mageStart);
    ASSERT_NE(mageEnd, std::string::npos);
    std::string_view const mageTriggers(source.data() + mageStart, mageEnd - mageStart);

    EXPECT_NE(mageTriggers.find(
                  "TriggerNode(\"counterspell\", { NextAction(\"counterspell\", 40.0f) })"),
              std::string_view::npos);
    EXPECT_NE(mageTriggers.find("TriggerNode(\"counterspell on enemy healer\", { NextAction(\"counterspell on "
                                "enemy healer\", 40.0f) })"),
              std::string_view::npos);
}
