/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AvoidAoeStrategyPolicy.h"

#include "gtest/gtest.h"

using AiFactoryPolicy::ShouldEnableAvoidAoe;

TEST(AvoidAoeStrategyPolicy, DisabledConfigKeepsStrategyDisabled)
{
    EXPECT_FALSE(ShouldEnableAvoidAoe(false, true, false, false, false));
    EXPECT_FALSE(ShouldEnableAvoidAoe(false, false, false, true, false));
    EXPECT_FALSE(ShouldEnableAvoidAoe(false, false, false, false, true));
}

TEST(AvoidAoeStrategyPolicy, RealPlayerMasterPreservesCanonicalBehaviorForEveryRole)
{
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, true, true, false, false));
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, true, false, true, false));
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, true, false, false, true));
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, true, false, false, false));
}

TEST(AvoidAoeStrategyPolicy, MasterlessNonTankHealerAndDpsEnableStrategy)
{
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, false, false, true, false));
    EXPECT_TRUE(ShouldEnableAvoidAoe(true, false, false, false, true));
}

TEST(AvoidAoeStrategyPolicy, MasterlessTankIsExcludedEvenWhenAlsoDpsOrHealer)
{
    EXPECT_FALSE(ShouldEnableAvoidAoe(true, false, true, false, false));
    EXPECT_FALSE(ShouldEnableAvoidAoe(true, false, true, false, true));
    EXPECT_FALSE(ShouldEnableAvoidAoe(true, false, true, true, false));
}

TEST(AvoidAoeStrategyPolicy, MasterlessUnknownRoleStaysDisabled)
{
    EXPECT_FALSE(ShouldEnableAvoidAoe(true, false, false, false, false));
}
