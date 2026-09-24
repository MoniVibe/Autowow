/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RestGate.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowRestGate;

// soak-s14-full-r1: 15 of 15 priest deaths started below 50% mana; wins started at a median 89%.
TEST(RestGate, HoldsBelowThresholdsAndWhileSick)
{
    Params p;  // hp 70, mana 60
    EXPECT_FALSE(Hold(p, 100, 100, true, false));
    EXPECT_FALSE(Hold(p, 70, 60, true, false));   // at the thresholds: pull
    EXPECT_TRUE(Hold(p, 69, 100, true, false));   // hurt
    EXPECT_TRUE(Hold(p, 100, 59, true, false));   // low mana (mana user)
    EXPECT_FALSE(Hold(p, 100, 0, false, false));  // rage / energy: mana ignored
    EXPECT_TRUE(Hold(p, 100, 100, true, true));   // resurrection sickness
    EXPECT_TRUE(Hold(p, 100, 100, false, true));
}

TEST(RestGate, ZeroDisablesAThreshold)
{
    Params p;
    p.minHpPct = 0;
    p.minManaPct = 0;
    EXPECT_FALSE(Hold(p, 1, 1, true, false));
    EXPECT_TRUE(Hold(p, 1, 1, true, true));  // sickness still holds
    EXPECT_FALSE(NeedHealth(p, 1));
    EXPECT_FALSE(NeedMana(p, 1, true));
}

TEST(RestGate, EatDrinkTriggersMatchThePullThresholds)
{
    Params p;
    EXPECT_TRUE(NeedHealth(p, 69));
    EXPECT_FALSE(NeedHealth(p, 70));
    EXPECT_TRUE(NeedMana(p, 59, true));
    EXPECT_FALSE(NeedMana(p, 60, true));
    EXPECT_FALSE(NeedMana(p, 0, false));  // no mana bar: never drinks
}

TEST(RestGate, DefaultsAndInertFlag)
{
    Params const p;
    EXPECT_EQ(p.minHpPct, 70U);
    EXPECT_EQ(p.minManaPct, 60U);
    EXPECT_EQ(kResurrectionSicknessAura, 15007U);
    EXPECT_FALSE(Enabled());
}
}  // namespace
