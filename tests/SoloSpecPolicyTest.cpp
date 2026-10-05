/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SoloSpecPolicy.h"

#include "gtest/gtest.h"

using namespace AutoWowSoloSpec;

// S109-S113 off-spec cohort bots (Sub rogue, Arcane mages, Resto druid, Disc priests) are re-talented; the solo
// trees they would be compared with (Combat, Frost, Balance) are kept.
TEST(SoloSpecPolicy, ForensicOffSpecsResetSoloSpecsKept)
{
    EXPECT_EQ(SpecToApply(4, 2, 40), 1);   // Subtlety rogue -> combat pve
    EXPECT_EQ(SpecToApply(8, 0, 40), 2);   // Arcane mage -> frost pve
    EXPECT_EQ(SpecToApply(11, 2, 40), 3);  // Restoration druid -> cat pve
    EXPECT_EQ(SpecToApply(5, 0, 40), 2);   // Discipline priest -> shadow pve
    EXPECT_EQ(SpecToApply(5, 1, 40), 2);   // Holy priest -> shadow pve
    EXPECT_EQ(SpecToApply(4, 1, 40), kKeep);   // Combat
    EXPECT_EQ(SpecToApply(8, 2, 40), kKeep);   // Frost
    EXPECT_EQ(SpecToApply(8, 1, 40), kKeep);   // Fire
    EXPECT_EQ(SpecToApply(11, 0, 40), kKeep);  // Balance
    EXPECT_EQ(SpecToApply(11, 1, 40), kKeep);  // Feral
    EXPECT_EQ(SpecToApply(5, 2, 40), kKeep);   // Shadow
}

TEST(SoloSpecPolicy, EveryPlayableClassHasASoloTree)
{
    struct Case
    {
        std::uint32_t cls;
        std::uint8_t allowed;
        std::int32_t spec;
    };
    Case const cases[] = {{1, 0b011, 0}, {2, 0b100, 2}, {3, 0b001, 0}, {4, 0b010, 1}, {5, 0b100, 2},
                          {6, 0b101, 2}, {7, 0b010, 1}, {8, 0b110, 2}, {9, 0b011, 0}, {11, 0b011, 3}};
    for (Case const& c : cases)
    {
        SCOPED_TRACE(c.cls);
        EXPECT_EQ(RowOf(c.cls).allowedTabs, c.allowed);
        for (std::uint32_t tab = 0; tab < 3; ++tab)
            EXPECT_EQ(SpecToApply(c.cls, tab, 11), ((c.allowed >> tab) & 1u) ? kKeep : c.spec);
        // The applied template lands in an allowed tree (spec index = tab for 0-2; druid 3 = cat, feral tab 1).
        std::uint32_t const appliedTab = c.spec == 3 ? 1u : static_cast<std::uint32_t>(c.spec);
        EXPECT_TRUE((c.allowed >> appliedTab) & 1u);
    }
}

TEST(SoloSpecPolicy, NothingToResetOrUnknownKeeps)
{
    EXPECT_EQ(SpecToApply(4, 2, 0), kKeep);   // no talent points spent yet (level < 10)
    EXPECT_EQ(SpecToApply(10, 0, 40), kKeep);  // unused class id
    EXPECT_EQ(SpecToApply(0, 0, 40), kKeep);
    EXPECT_EQ(SpecToApply(4, 3, 40), kKeep);   // out-of-range tab
}
