/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassQuestPolicy.h"

#include <set>

#include "AutoWowQuestLedger.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowClassQuests;

// Races / classes (3.3.5 ids).
constexpr std::uint32_t kHuman = 1, kOrc = 2, kDwarf = 3, kNightElf = 4, kTauren = 6, kBloodElf = 10;
constexpr std::uint32_t kWarrior = 1, kPaladin = 2, kHunter = 3, kShaman = 7, kMage = 8, kWarlock = 9, kDruid = 11;

std::vector<Pick> Owed(std::uint32_t cls, std::uint32_t race, std::uint32_t level, bool mounts = false,
                      std::set<std::uint32_t> const& known = {}, std::set<std::uint32_t> const& owned = {})
{
    return Select(cls, race, level, mounts, [&](std::uint32_t s) { return known.count(s) > 0; },
                  [&](std::uint32_t i) { return owned.count(i) > 0; });
}

std::vector<std::uint32_t> Spells(std::vector<Pick> const& picks)
{
    std::vector<std::uint32_t> out;
    for (Pick const& p : picks)
        if (p.spell)
            out.push_back(p.spell);
    return out;
}

std::vector<std::uint32_t> Items(std::vector<Pick> const& picks)
{
    std::vector<std::uint32_t> out;
    for (Pick const& p : picks)
        if (p.item)
            out.push_back(p.item);
    return out;
}

using V = std::vector<std::uint32_t>;

TEST(ClassQuestPolicy, TableIsOneClassPerRowAndOrdered)
{
    for (std::size_t i = 0; i < kRewards.size(); ++i)
    {
        Reward const& g = kRewards[i];
        EXPECT_TRUE(g.quest && g.classMask && (g.classMask & (g.classMask - 1)) == 0) << g.quest;
        EXPECT_TRUE(g.spells[0] || g.item) << g.quest;  // every row grants something
        if (i)
        {
            Reward const& p = kRewards[i - 1];
            bool const ordered = p.classMask < g.classMask ||
                                 (p.classMask == g.classMask &&
                                  (p.level < g.level || (p.level == g.level && p.quest < g.quest)));
            EXPECT_TRUE(ordered) << g.quest;
        }
    }
}

TEST(ClassQuestPolicy, LevelGate)
{
    EXPECT_TRUE(Owed(kWarrior, kHuman, 9).empty());
    std::vector<Pick> const at10 = Owed(kWarrior, kHuman, 10);
    EXPECT_EQ(Spells(at10), (V{71, 7386, 355}));
    EXPECT_EQ(at10[0].quest, 1498u);
    EXPECT_EQ(at10[0].level, 10u);
    EXPECT_EQ(Spells(Owed(kWarrior, kOrc, 30)), (V{71, 7386, 355, 2458, 20252}));
}

TEST(ClassQuestPolicy, KnownSpellsAndOtherClassesAreSkipped)
{
    EXPECT_EQ(Spells(Owed(kWarrior, kHuman, 30, false, {71, 2458})), (V{7386, 355, 20252}));
    EXPECT_TRUE(Owed(kWarrior, kHuman, 30, false, {71, 7386, 355, 2458, 20252}).empty());
    EXPECT_EQ(Spells(Owed(kMage, kHuman, 70)), (V{10140, 28272}));
    EXPECT_EQ(Spells(Owed(kMage, kHuman, 71)), (V{10140, 28272, 53140}));
    EXPECT_TRUE(Owed(0, kHuman, 80).empty());
}

TEST(ClassQuestPolicy, RaceGate)
{
    // Druid Bear Form / Aquatic Form chains exist for Night Elf and Tauren only.
    EXPECT_EQ(Spells(Owed(kDruid, kTauren, 16)), (V{5487, 6795, 6807, 8946, 1066}));
    EXPECT_EQ(Spells(Owed(kDruid, kNightElf, 16)), (V{5487, 6795, 6807, 8946, 1066}));
    EXPECT_EQ(Spells(Owed(kDruid, kHuman, 16)), (V{8946}));
    // Blood elf paladins get the Thalassian mounts, not the Warhorse / Charger.
    EXPECT_EQ(Spells(Owed(kPaladin, kBloodElf, 60, true)), (V{7328, 5502, 34769, 33388, 34767, 33391}));
    EXPECT_EQ(Spells(Owed(kPaladin, kDwarf, 60, true)), (V{7328, 5502, 13819, 33388, 23214, 33391}));
}

TEST(ClassQuestPolicy, MountsNeedTheirFlag)
{
    EXPECT_EQ(Spells(Owed(kPaladin, kDwarf, 60)), (V{7328, 5502}));
    EXPECT_EQ(Spells(Owed(kWarlock, kOrc, 40)), (V{688, 697, 712, 691}));
    EXPECT_EQ(Spells(Owed(kWarlock, kOrc, 40, true)), (V{688, 697, 712, 691, 5784, 33388}));
    // Riding already known (e.g. bought) is not granted again.
    EXPECT_EQ(Spells(Owed(kWarlock, kOrc, 40, true, {33388})), (V{688, 697, 712, 691, 5784}));
}

TEST(ClassQuestPolicy, HunterPetSpellsAtTen)
{
    EXPECT_TRUE(Owed(kHunter, kOrc, 9).empty());
    std::vector<Pick> const picks = Owed(kHunter, kOrc, 10);
    EXPECT_EQ(Spells(picks), (V{6991, 982, 1515, 883, 2641}));
    EXPECT_EQ(picks[0].quest, 6081u);
    EXPECT_EQ(picks[2].quest, 6082u);
}

TEST(ClassQuestPolicy, ShamanTotemsOnceAndMasterSupersedes)
{
    std::vector<Pick> const l20 = Owed(kShaman, kOrc, 20);
    EXPECT_EQ(Spells(l20), (V{8071, 3599, 5394}));
    EXPECT_EQ(Items(l20), (V{5175, 5176, 5177}));
    EXPECT_EQ(Items(Owed(kShaman, kOrc, 20, false, {}, {5175})), (V{5176, 5177}));
    // At 30 the Totem of the Earthen Ring covers all four: the basic totems are not granted.
    EXPECT_EQ(Items(Owed(kShaman, kOrc, 30)), (V{kMasterTotem}));
    EXPECT_TRUE(Items(Owed(kShaman, kOrc, 30, false, {}, {kMasterTotem})).empty());
    // A master owned below 30 also covers them.
    EXPECT_TRUE(Items(Owed(kShaman, kOrc, 20, false, {}, {kMasterTotem})).empty());
    EXPECT_TRUE(Owed(kShaman, kOrc, 3).empty());
}

TEST(ClassQuestPolicy, LedgerLine)
{
    Pick const p{1518, 0, 5175, 4};
    EXPECT_EQ(LedgerFields(p), ",\"spell\":0,\"item\":5175,\"level\":4");
    AutoWowQuestLedger::Row row;
    row.ev = AutoWowQuestLedger::Event::ClassQuest;
    row.quest = p.quest;
    row.reason = "grant";
    std::string const fields = LedgerFields(p);
    row.extra = fields;
    std::string const line = AutoWowQuestLedger::FormatLine("r", row);
    EXPECT_NE(line.find("\"ev\":\"classquest\""), std::string::npos);
    EXPECT_NE(line.find("\"quest\":1518"), std::string::npos);
    EXPECT_NE(line.find("\"reason\":\"grant\",\"phase\":\"\",\"spell\":0,\"item\":5175,\"level\":4}"), std::string::npos);
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::ClassQuest), 23);
}
}  // namespace
