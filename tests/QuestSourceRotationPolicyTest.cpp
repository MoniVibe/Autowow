/*
 * Pure source-rotation policy tests. These tests intentionally do not require a loaded world;
 * live target validity, combat, loot, and quest credit remain the native executor's authority.
 */

#include "QuestSourceRotationPolicy.h"

#include "gtest/gtest.h"

namespace
{
using QuestSourceRotationPolicy::Candidate;
using QuestSourceRotationPolicy::Decision;
using QuestSourceRotationPolicy::Request;

Request BaseRequest()
{
    Request request;
    request.botMapId = 1;
    request.currentSignedEntry = 3195;
    request.currentSpawnKey = 100;
    request.maxRotations = 16;
    request.objectiveEntries = {3195, 3196};
    request.candidates = {
        Candidate{3195, 1, 100, 5.0f},
        Candidate{3195, 1, 101, 12.0f},
        Candidate{3196, 1, 200, 8.0f},
        Candidate{3195, 2, 300, 1.0f},
        Candidate{1988, 1, 400, 0.5f},
    };
    return request;
}
}  // namespace

TEST(QuestSourceRotationPolicy, ActiveTargetPreservesCurrentSource)
{
    Request request = BaseRequest();
    request.activeTarget = true;

    auto const result = QuestSourceRotationPolicy::Decide(request);

    EXPECT_EQ(result.decision, Decision::PreserveActiveTarget);
    EXPECT_EQ(result.candidate.spawnKey, 0u);
}

TEST(QuestSourceRotationPolicy, SelectsNearestLegalDifferentSourceDeterministically)
{
    Request request = BaseRequest();

    auto const result = QuestSourceRotationPolicy::Decide(request);

    ASSERT_EQ(result.decision, Decision::RotateToNextSource);
    EXPECT_EQ(result.candidate.signedEntry, 3196);
    EXPECT_EQ(result.candidate.spawnKey, 200u);
}

TEST(QuestSourceRotationPolicy, FiltersCrossMapUnlistedCurrentAndExhaustedSources)
{
    Request request = BaseRequest();
    request.exhaustedSpawnKeys = {101, 200};
    request.candidates.push_back(Candidate{3195, 1, 500, 0.25f});

    auto const result = QuestSourceRotationPolicy::Decide(request);

    ASSERT_EQ(result.decision, Decision::RotateToNextSource);
    EXPECT_EQ(result.candidate.signedEntry, 3195);
    EXPECT_EQ(result.candidate.spawnKey, 500u);
}

TEST(QuestSourceRotationPolicy, BoundedFailureStopsRotationAfterCapOrWhenEmpty)
{
    Request capped = BaseRequest();
    capped.rotationCount = capped.maxRotations;
    EXPECT_EQ(QuestSourceRotationPolicy::Decide(capped).decision, Decision::BoundedFailure);

    Request empty = BaseRequest();
    empty.candidates.clear();
    EXPECT_EQ(QuestSourceRotationPolicy::Decide(empty).decision, Decision::BoundedFailure);
}

TEST(QuestSourceRotationPolicy, FailedQuestItemGoRotatesToDifferentResolvedSpawn)
{
    Request request;
    request.botMapId = 1;
    request.currentSignedEntry = -152094;
    request.currentSpawnKey = 100;
    request.maxRotations = 16;
    request.rotationCount = 1;
    request.objectiveEntries = {-152094};
    request.exhaustedSpawnKeys = {100}; // The first GO gave zero core item delta.
    request.candidates = {
        Candidate{-152094, 1, 100, 1.0f},
        Candidate{-152094, 1, 101, 10.0f},
        Candidate{-152094, 2, 102, 2.0f}};

    auto const rotated = QuestSourceRotationPolicy::Decide(request);
    ASSERT_EQ(rotated.decision, Decision::RotateToNextSource);
    EXPECT_EQ(rotated.candidate.spawnKey, 101u);

    request.exhaustedSpawnKeys.push_back(101);
    EXPECT_EQ(QuestSourceRotationPolicy::Decide(request).decision, Decision::BoundedFailure);
}
