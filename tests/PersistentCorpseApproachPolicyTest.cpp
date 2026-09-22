/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "PersistentCorpseApproachPolicy.h"

#include "gtest/gtest.h"

#include <limits>

using namespace PersistentCorpseApproachPolicy;

TEST(PersistentCorpseApproachPolicy, SafeFullPathUsesDestination)
{
    EXPECT_EQ(Select({true, true, false, false, false, 500.0f, 0.0f, 0.0f}),
        Decision::DirectDestination);
}

TEST(PersistentCorpseApproachPolicy, GroundedIncompletePathUsesMonotonicEndpoint)
{
    EXPECT_EQ(Select({true, false, true, true, true, 500.0f, 110.0f, 390.0f}),
        Decision::PartialEndpoint);
}

TEST(PersistentCorpseApproachPolicy, RejectsNoProgressAndWrongMap)
{
    EXPECT_EQ(Select({true, false, true, true, true, 500.0f, 110.0f, 499.0f}),
        Decision::None);
    EXPECT_EQ(Select({false, true, true, true, true, 500.0f, 110.0f, 390.0f}),
        Decision::None);
}

TEST(PersistentCorpseApproachPolicy, RejectsUnsafeMissingOrUngroundedEndpoint)
{
    EXPECT_EQ(Select({true, false, false, true, true, 500.0f, 110.0f, 390.0f}),
        Decision::None);
    EXPECT_EQ(Select({true, false, true, false, true, 500.0f, 110.0f, 390.0f}),
        Decision::None);
    EXPECT_EQ(Select({true, false, true, true, false, 500.0f, 110.0f, 390.0f}),
        Decision::None);
}

TEST(PersistentCorpseApproachPolicy, RejectsNonFiniteDistances)
{
    float const infinity = std::numeric_limits<float>::infinity();
    EXPECT_EQ(Select({true, false, true, true, true, infinity, 110.0f, 390.0f}),
        Decision::None);
    EXPECT_EQ(Select({true, false, true, true, true, 500.0f, infinity, 390.0f}),
        Decision::None);
    EXPECT_EQ(Select({true, false, true, true, true, 500.0f, 110.0f, infinity}),
        Decision::None);
}
