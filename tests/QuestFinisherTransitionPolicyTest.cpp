/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify it under the terms of the
 * GNU General Public License version 2 or later.
 */

#include "QuestFinisherTransitionPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gtest/gtest.h"

namespace
{
std::string ReadModuleSource(std::string const& relativePath)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(QuestFinisherTransitionPolicyTest, IncompleteCoreCompletableQuestRequestsNormalCompletion)
{
    using namespace AutoWowQuestFinisher;
    EXPECT_EQ(SelectDirective({QUEST_STATUS_INCOMPLETE, false, true, true}), Directive::RequestCompletion);
    EXPECT_TRUE(IsFinisherReady({QUEST_STATUS_INCOMPLETE, false, true, true}));
}

TEST(QuestFinisherTransitionPolicyTest, CompleteQuestUsesTurnInDirective)
{
    using namespace AutoWowQuestFinisher;
    EXPECT_EQ(SelectDirective({QUEST_STATUS_COMPLETE, false, false, true}), Directive::TurnIn);
}

TEST(QuestFinisherTransitionPolicyTest, IncompleteQuestWithOutstandingWorkStaysBlocked)
{
    using namespace AutoWowQuestFinisher;
    EXPECT_EQ(SelectDirective({QUEST_STATUS_INCOMPLETE, false, false, true}), Directive::None);
    EXPECT_EQ(SelectDirective({QUEST_STATUS_INCOMPLETE, false, true, false}), Directive::None);
}

TEST(QuestFinisherTransitionPolicyTest, RewardedQuestCannotReenterFinisher)
{
    using namespace AutoWowQuestFinisher;
    EXPECT_EQ(SelectDirective({QUEST_STATUS_COMPLETE, true, true, true}), Directive::None);
    EXPECT_FALSE(IsFinisherReady({QUEST_STATUS_INCOMPLETE, true, true, true}));
}

TEST(QuestFinisherTransitionContractTest, ProductionUsesCoreCompletionAndExactPacketPath)
{
    std::string const values = ReadModuleSource("src/Ai/Base/Value/QuestValues.cpp");
    std::string const action = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::string const bridge = ReadModuleSource("src/AutoWow/AutoWowBridge.cpp");

    EXPECT_NE(values.find("bot->CanCompleteQuest(questId)"), std::string::npos);
    EXPECT_NE(action.find("HandleQuestgiverRequestRewardOpcode"), std::string::npos);
    EXPECT_NE(action.find("fobj->hasInvolvedQuest(questId)"), std::string::npos);
    EXPECT_NE(action.find("AutoWowQuestFinisher::IsFinisherReady"), std::string::npos);
    EXPECT_NE(bridge.find("result_quest"), std::string::npos);
    EXPECT_EQ(action.find("bot->CompleteQuest(questId)"), std::string::npos);
    EXPECT_EQ(action.find("bot->RewardQuest(quest"), std::string::npos);
}
