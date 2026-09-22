#include "FixtureAccelerationControl.h"

#include <gtest/gtest.h>

TEST(FixtureAccelerationControlTest, AcceptsOnlyTheExplicitFixturePacing)
{
    EXPECT_TRUE(AutoWowFixtureAcceleration::IsValidPacingPercent(1000));
    EXPECT_FALSE(AutoWowFixtureAcceleration::IsValidPacingPercent(100));
    EXPECT_FALSE(AutoWowFixtureAcceleration::IsValidPacingPercent(1001));
}

TEST(FixtureAccelerationControlTest, RequiresExactEntryAndStableSpawn)
{
    EXPECT_TRUE(AutoWowFixtureAcceleration::IsValidExactTarget(324, 201104));
    EXPECT_FALSE(AutoWowFixtureAcceleration::IsValidExactTarget(0, 201104));
    EXPECT_FALSE(AutoWowFixtureAcceleration::IsValidExactTarget(324, 0));
}
