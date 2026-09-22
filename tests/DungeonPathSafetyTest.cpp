/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "../src/AutoWow/DungeonPathSafety.h"

#include "gtest/gtest.h"

namespace
{
AutoWowDungeonPath::ValidationFacts SafeFacts()
{
    return {true, PATHFIND_NORMAL, 4, 0.25f, 1.0f, 2.0f, true, 0.5f, 100.0f};
}
}

TEST(DungeonPathSafety, AcceptsOnlyCompleteGroundedNormalPath)
{
    EXPECT_TRUE(AutoWowDungeonPath::Validate(SafeFacts()).empty());
}

TEST(DungeonPathSafety, RejectsEveryCoreFallbackPathType)
{
    uint32 const rejected[] = {PATHFIND_SHORTCUT, PATHFIND_NOPATH, PATHFIND_NOT_USING_PATH, PATHFIND_SHORT,
                               PATHFIND_FARFROMPOLY_START, PATHFIND_FARFROMPOLY_END};
    for (uint32 flag : rejected)
    {
        auto facts = SafeFacts();
        facts.pathType |= flag;
        EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "unsafe_path_type") << flag;
    }
}

TEST(DungeonPathSafety, AcceptsOnlyGroundedNearEndpointNormalIncompleteHybrid)
{
    auto facts = SafeFacts();
    facts.pathType = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    facts.pointCount = 42;
    facts.endpointDistance = 6.264f;
    facts.sourceDistance = 178.0f;
    EXPECT_TRUE(AutoWowDungeonPath::Validate(facts).empty());

    facts.pointCount = 39;
    facts.endpointDistance = 71.2f;
    facts.sourceDistance = 178.19f;
    EXPECT_TRUE(AutoWowDungeonPath::Validate(facts).empty());

    facts.pathType = PATHFIND_INCOMPLETE;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "path_not_normal");

    facts.pathType = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    facts.pointCount = 2;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "unsafe_path_type");

    facts.pointCount = 42;
    facts.endpointDistance = 71.2f;
    facts.sourceDistance = 72.0f;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "incomplete_path_no_progress");
}

TEST(DungeonPathSafety, RejectsVerticalVoidShortcutEvenWhenCoreCallsItNormal)
{
    auto facts = SafeFacts();
    facts.maxSegmentVerticalDelta = 18.0f;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "vertical_segment_too_large");

    facts = SafeFacts();
    facts.excursionBelowEndpoints = 18.0f;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "path_dips_below_corridor");
}

TEST(DungeonPathSafety, RejectsOffGroundOrMissingCollisionSamples)
{
    auto facts = SafeFacts();
    facts.allGroundSamplesValid = false;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "ground_sample_missing");

    facts = SafeFacts();
    facts.maxGroundDelta = 8.0f;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "path_point_off_ground");
}

TEST(DungeonPathSafety, RejectsPartialEndpointAndDegeneratePaths)
{
    auto facts = SafeFacts();
    facts.endpointDistance = 9.0f;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "endpoint_not_reached");

    facts = SafeFacts();
    facts.pointCount = 1;
    EXPECT_EQ(AutoWowDungeonPath::Validate(facts), "path_too_short");
}

TEST(DungeonPathSafety, GroundLineRequiresDenseGroundedVisibleGentleSamples)
{
    AutoWowDungeonPath::GroundLineFacts facts{true, 12, true, 0.5f, true, 0.4f};
    EXPECT_TRUE(AutoWowDungeonPath::ValidateGroundLine(facts).empty());

    facts.allGroundSamplesValid = false;
    EXPECT_EQ(AutoWowDungeonPath::ValidateGroundLine(facts), "ground_line_missing_floor");
    facts = {true, 12, true, 8.0f, true, 0.4f};
    EXPECT_EQ(AutoWowDungeonPath::ValidateGroundLine(facts), "ground_line_floor_mismatch");
    facts = {true, 12, true, 0.5f, false, 0.4f};
    EXPECT_EQ(AutoWowDungeonPath::ValidateGroundLine(facts), "ground_line_blocked");
    facts = {true, 12, true, 0.5f, true, 8.0f};
    EXPECT_EQ(AutoWowDungeonPath::ValidateGroundLine(facts), "ground_line_too_steep");
}
