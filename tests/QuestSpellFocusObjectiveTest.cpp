/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "AiObjectContext.h"
#include "QuestValues.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
std::string ReadModuleSource(std::string const& relativePath)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string SourceBetween(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};

    std::size_t const finish = source.find(end, begin + start.size());
    return source.substr(begin, finish == std::string::npos ? std::string::npos : finish - begin);
}
}  // namespace

TEST(QuestSpellFocusObjective, DetectsSingleOnUseFocusSpellCreatingRequiredItem)
{
    uint32 const requiredItemId = 70001;
    QuestItemUseSpellFact const fact{/*spellId*/ 80001, /*onUse*/ true,
                                     /*requiredSpellFocusId*/ 90001, {requiredItemId}};

    QuestItemSpellFocusMatch const match = ResolveQuestItemSpellFocus(requiredItemId, {fact});

    ASSERT_TRUE(match.isResolved());
    EXPECT_EQ(match.spellId, fact.spellId);
    EXPECT_EQ(match.spellFocusId, fact.requiredSpellFocusId);
}

TEST(QuestSpellFocusObjective, RejectsSpellCreatingWrongOutputItem)
{
    QuestItemUseSpellFact const wrongOutput{/*spellId*/ 80002, /*onUse*/ true,
                                            /*requiredSpellFocusId*/ 90002, {/*other item*/ 70002}};

    EXPECT_FALSE(ResolveQuestItemSpellFocus(/*requiredItemId*/ 70003, {wrongOutput}).isResolved());
}

TEST(QuestSpellFocusObjective, RejectsMatchingOutputWithoutSpellFocus)
{
    uint32 const requiredItemId = 70004;
    QuestItemUseSpellFact const noFocus{/*spellId*/ 80003, /*onUse*/ true,
                                        /*requiredSpellFocusId*/ 0, {requiredItemId}};

    EXPECT_FALSE(ResolveQuestItemSpellFocus(requiredItemId, {noFocus}).isResolved());
}

TEST(QuestSpellFocusObjective, RejectsNonOnUseAndAmbiguousMatches)
{
    uint32 const requiredItemId = 70005;
    QuestItemUseSpellFact const nonOnUse{/*spellId*/ 80004, /*onUse*/ false,
                                         /*requiredSpellFocusId*/ 90003, {requiredItemId}};
    QuestItemUseSpellFact const first{/*spellId*/ 80005, /*onUse*/ true,
                                      /*requiredSpellFocusId*/ 90003, {requiredItemId}};
    QuestItemUseSpellFact const second{/*spellId*/ 80006, /*onUse*/ true,
                                       /*requiredSpellFocusId*/ 90004, {requiredItemId}};

    EXPECT_FALSE(ResolveQuestItemSpellFocus(requiredItemId, {nonOnUse}).isResolved());
    EXPECT_FALSE(ResolveQuestItemSpellFocus(requiredItemId, {first, second}).isResolved());
}

TEST(QuestSpellFocusObjective, GameObjectSourceEntryUsesSignedRuntimeConvention)
{
    uint32 const gameObjectEntry = 12001;
    int32 const signedEntry = SignedGameObjectObjectiveEntry(gameObjectEntry);

    EXPECT_EQ(signedEntry, -static_cast<int32>(gameObjectEntry));
    EXPECT_LT(signedEntry, 0);
    EXPECT_EQ(SignedGameObjectObjectiveEntry(0), 0);
    EXPECT_EQ(SignedGameObjectObjectiveEntry(
                  static_cast<uint32>(std::numeric_limits<int32>::max()) + 1u),
              0);
}

TEST(QuestSpellFocusObjective, ResolverAndExecutorPreserveCoreAuthority)
{
    std::string const resolver = ReadModuleSource("src/Ai/Base/Value/QuestValues.cpp");
    ASSERT_FALSE(resolver.empty());

    for (std::string_view const required : {"quest->GetSrcItemId()", "ITEM_SPELLTRIGGER_ON_USE",
                                             "SPELL_EFFECT_CREATE_ITEM", "SPELL_EFFECT_CREATE_ITEM_2",
                                             "GAMEOBJECT_TYPE_SPELL_FOCUS", "spellFocus.focusId",
                                             "GetGameObjectTemplates()", "GetAllGOData()",
                                             "source->second.spawns.emplace_back(goData)",
                                             "if (!source.spawns.empty())", "if (!s.sources.empty())",
                                             "s.kind = QuestObjectiveKind::Unsupported",
                                             "s.kind = QuestObjectiveKind::UseQuestItem"})
        EXPECT_NE(resolver.find(required), std::string::npos) << required;

    std::string const action = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::string const usePhase =
        SourceBetween(action, "case QuestActionPhase::UseQuestItem:", "case QuestActionPhase::EscortEvent:");
    std::string const focusUse =
        SourceBetween(usePhase, "if (rt.selectedSourceEntry < 0)", "if (!BindQuestItemTarget");
    ASSERT_FALSE(focusUse.empty());

    for (std::string_view const required : {"BindSourceGameObject", "acceptsGameObjectEntry",
                                             "TargetlessQuestItemUseAction", "UseItemAuto(questItem)",
                                             "RecordAutoWowQuestItemUsePacket()",
                                             "ObjectiveCurrentCount(questId, objectiveIdx)",
                                             "MoveWorldObjectTo(focus->GetGUID(), CONTACT_DISTANCE)"})
        EXPECT_NE(focusUse.find(required), std::string::npos) << required;

    for (std::string_view const forbidden : {"AddItem(", "CompleteQuest(", "RewardQuest(",
                                              "KilledMonsterCredit(", "CastedCreatureOrGO(", "TeleportTo("})
        EXPECT_EQ(focusUse.find(forbidden), std::string::npos) << forbidden;

    // The established creature-targeted item-use path remains intact for CAST objectives.
    EXPECT_NE(usePhase.find("use.UseItemOnUnit(questItem, target)"), std::string::npos);
}
