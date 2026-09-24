/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PullLevelCap.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowPullCap;

// soak-s13-full-r1: L9-13 bots died in their new zones to mobs +1..+3 above them.
TEST(PullLevelCap, OverCapAboveMaxLevelAboveWithElitePenalty)
{
    Params p;  // max +2, elite +3
    EXPECT_FALSE(OverCap(p, 11, 11, false));
    EXPECT_FALSE(OverCap(p, 11, 13, false));  // +2: allowed
    EXPECT_TRUE(OverCap(p, 11, 14, false));   // +3: capped
    EXPECT_FALSE(OverCap(p, 11, 5, false));
    // Elite: level + 3 counts, so an elite at the bot's own level is already capped.
    EXPECT_FALSE(OverCap(p, 11, 10, true));   // 13
    EXPECT_TRUE(OverCap(p, 11, 11, true));    // 14
    p.maxLevelAbove = 0;
    p.elitePenalty = 0;
    EXPECT_TRUE(OverCap(p, 11, 12, false));
    EXPECT_FALSE(OverCap(p, 11, 11, true));
}

TEST(PullLevelCap, QuestExceptionNeedsObjectiveHealthAndNoPack)
{
    Params p;  // healthy >= 80
    EXPECT_TRUE(QuestException(p, true, 100, 100, true, 0));
    EXPECT_TRUE(QuestException(p, true, 80, 80, true, 0));
    EXPECT_FALSE(QuestException(p, false, 100, 100, true, 0));  // not an objective
    EXPECT_FALSE(QuestException(p, true, 79, 100, true, 0));    // hurt
    EXPECT_FALSE(QuestException(p, true, 100, 79, true, 0));    // low mana (mana user)
    EXPECT_TRUE(QuestException(p, true, 100, 0, false, 0));     // rage/energy: mana ignored
    EXPECT_FALSE(QuestException(p, true, 100, 100, true, 1));   // a linked neighbour: not alone
}

TEST(PullLevelCap, DefaultsAndInertFlag)
{
    Params const p;
    EXPECT_EQ(p.maxLevelAbove, 2U);
    EXPECT_EQ(p.elitePenalty, 3U);
    EXPECT_EQ(p.healthyPct, 80U);
    EXPECT_EQ(p.aloneYards, 15U);
    EXPECT_FALSE(Enabled());
}
}  // namespace
