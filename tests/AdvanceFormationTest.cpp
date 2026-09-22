/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "../src/AutoWow/AdvanceFormation.h"

#include <cmath>
#include <set>
#include <utility>

#include "gtest/gtest.h"

TEST(AdvanceFormation, LeaderOwnsTheNamedWaypoint)
{
    auto const destination = AutoWowAdvanceFormation::ForSlot(0, 0.0f, 0.0f, 100.0f, 20.0f, 0.0f);
    EXPECT_FLOAT_EQ(destination.x, 100.0f);
    EXPECT_FLOAT_EQ(destination.y, 20.0f);
    EXPECT_FLOAT_EQ(destination.trailing, 0.0f);
    EXPECT_FLOAT_EQ(destination.lateral, 0.0f);
}

TEST(AdvanceFormation, TenPlayerRaidFormsTwoNarrowRowsBehindEastboundLeader)
{
    auto const left = AutoWowAdvanceFormation::ForSlot(1, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);
    auto const center = AutoWowAdvanceFormation::ForSlot(3, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);
    auto const right = AutoWowAdvanceFormation::ForSlot(5, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);
    auto const secondRow = AutoWowAdvanceFormation::ForSlot(9, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(left.x, 97.5f);
    EXPECT_FLOAT_EQ(left.y, -3.0f);
    EXPECT_FLOAT_EQ(center.x, 97.5f);
    EXPECT_FLOAT_EQ(center.y, 0.0f);
    EXPECT_FLOAT_EQ(right.y, 3.0f);
    EXPECT_FLOAT_EQ(secondRow.x, 95.0f);
    EXPECT_FLOAT_EQ(secondRow.y, 1.5f);
}

TEST(AdvanceFormation, GridRotatesWithTheApproachPath)
{
    auto const destination = AutoWowAdvanceFormation::ForSlot(1, 0.0f, 0.0f, 0.0f, 100.0f, 0.0f);
    EXPECT_FLOAT_EQ(destination.x, 3.0f);
    EXPECT_FLOAT_EQ(destination.y, 97.5f);
}

TEST(AdvanceFormation, CoincidentOriginUsesLeaderOrientation)
{
    float constexpr halfPi = 1.57079632679f;
    auto const destination = AutoWowAdvanceFormation::ForSlot(3, 10.0f, 10.0f, 10.0f, 10.0f, halfPi);
    EXPECT_NEAR(destination.x, 10.0f, 0.0001f);
    EXPECT_NEAR(destination.y, 7.5f, 0.0001f);
}

TEST(AdvanceFormation, FortyPlayerGridIsDeterministicUniqueAndCorridorBounded)
{
    std::set<std::pair<int, int>> occupied;
    for (std::uint32_t slot = 0; slot < 40; ++slot)
    {
        auto const first = AutoWowAdvanceFormation::ForSlot(slot, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);
        auto const second = AutoWowAdvanceFormation::ForSlot(slot, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f);
        EXPECT_FLOAT_EQ(first.x, second.x);
        EXPECT_FLOAT_EQ(first.y, second.y);
        EXPECT_LE(std::fabs(first.lateral), 3.0f);
        EXPECT_LE(first.trailing, 20.0f);
        occupied.emplace(static_cast<int>(std::lround(first.x * 10.0f)),
                         static_cast<int>(std::lround(first.y * 10.0f)));
    }
    EXPECT_EQ(occupied.size(), 40u);
}

TEST(AdvanceFormation, ZeroLateralScaleProducesDeterministicSingleFileRows)
{
    for (std::uint32_t slot = 1; slot < 10; ++slot)
    {
        AutoWowAdvanceFormation::Destination const point = AutoWowAdvanceFormation::ForSlot(
            slot, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f);
        EXPECT_NEAR(point.y, 0.0f, 0.001f);
        EXPECT_NEAR(point.lateral, 0.0f, 0.001f);
        EXPECT_LT(point.x, 20.0f);
    }
}
