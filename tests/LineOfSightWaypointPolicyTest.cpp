/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/Ai/Base/LineOfSightWaypointPolicy.h"

#include "gtest/gtest.h"

TEST(LineOfSightWaypointPolicy, RejectsProjectedStartAndAcceptsRealMovement)
{
    using LineOfSightWaypointPolicy::IsMeaningfulHorizontalStep;
    EXPECT_FALSE(IsMeaningfulHorizontalStep(0.0f));
    EXPECT_FALSE(IsMeaningfulHorizontalStep(0.75f));
    EXPECT_FALSE(IsMeaningfulHorizontalStep(1.0f));
    EXPECT_TRUE(IsMeaningfulHorizontalStep(1.01f));
    EXPECT_TRUE(IsMeaningfulHorizontalStep(8.0f));
}

TEST(LineOfSightWaypointPolicy, SupportsAConservativeCallerThreshold)
{
    using LineOfSightWaypointPolicy::IsMeaningfulHorizontalStep;
    EXPECT_FALSE(IsMeaningfulHorizontalStep(2.0f, 3.0f));
    EXPECT_TRUE(IsMeaningfulHorizontalStep(3.1f, 3.0f));
}
