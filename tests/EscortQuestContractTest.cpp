/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under the terms of Version 2
 * of the License.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path CoreRoot()
{
    return std::filesystem::path(__FILE__)
        .parent_path() // tests
        .parent_path() // mod-playerbots
        .parent_path() // modules
        .parent_path(); // core checkout
}
}

TEST(EscortQuestContract, Q435PassesAcceptedQuestToNativeEscortStart)
{
    const std::filesystem::path scriptPath =
        CoreRoot() / "src/server/scripts/EasternKingdoms/zone_silverpine_forest.cpp";
    std::ifstream script(scriptPath, std::ios::in | std::ios::binary);
    ASSERT_TRUE(script.is_open()) << "Could not open " << scriptPath.string();

    const std::string source((std::istreambuf_iterator<char>(script)), std::istreambuf_iterator<char>());
    const std::size_t erlandStart = source.find("class npc_deathstalker_erland");
    ASSERT_NE(erlandStart, std::string::npos);

    const std::size_t nextSection = source.find("/*######", erlandStart);
    ASSERT_NE(nextSection, std::string::npos);
    const std::string_view erland(source.data() + erlandStart, nextSection - erlandStart);

    EXPECT_NE(erland.find("if (quest->GetQuestId() == QUEST_ESCORTING)"), std::string_view::npos);
    EXPECT_NE(erland.find("pEscortAI->Start(true, player->GetGUID(), quest);"), std::string_view::npos);
    EXPECT_EQ(erland.find("pEscortAI->Start(true, player->GetGUID());"), std::string_view::npos);
}
