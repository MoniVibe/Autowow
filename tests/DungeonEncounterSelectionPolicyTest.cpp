/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonEncounterSelectionPolicy.h"

#include <algorithm>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace DungeonEncounterSelection;

CandidateFacts Candidate(uint32 mapId, uint32 encounterId, uint32 goalId, uint32 routeOrder,
    int32 score, Completion completion = Completion::Incomplete,
    GateState prerequisites = GateState::Satisfied,
    Reachability reachability = Reachability::Reachable)
{
    return {{mapId, encounterId}, goalId, routeOrder, score, completion, prerequisites, reachability};
}

void ExpectKey(EncounterKey const& actual, uint32 mapId, uint32 encounterId)
{
    EXPECT_EQ(actual.mapId, mapId);
    EXPECT_EQ(actual.encounterId, encounterId);
}

void ExpectEquivalent(Selection const& left, Selection const& right)
{
    EXPECT_EQ(left.status, right.status);
    ExpectKey(left.selected, right.selected.mapId, right.selected.encounterId);
    EXPECT_EQ(left.selectedGoalId, right.selectedGoalId);
    ASSERT_EQ(left.decisions.size(), right.decisions.size());
    for (std::size_t index = 0; index < left.decisions.size(); ++index)
    {
        ExpectKey(left.decisions[index].key, right.decisions[index].key.mapId,
            right.decisions[index].key.encounterId);
        EXPECT_EQ(left.decisions[index].goalId, right.decisions[index].goalId);
        EXPECT_EQ(left.decisions[index].reason, right.decisions[index].reason);
    }
}
}

TEST(DungeonEncounterSelectionPolicy, EmptyInputHasNoCandidates)
{
    Selection const result = Select({});
    EXPECT_EQ(result.status, Status::NoCandidates);
    ExpectKey(result.selected, 0, 0);
    EXPECT_EQ(result.selectedGoalId, 0u);
    EXPECT_TRUE(result.decisions.empty());
}

TEST(DungeonEncounterSelectionPolicy, SelectsEligibleCandidateAndReportsEveryDecision)
{
    Selection const result = Select({
        Candidate(33, 2, 102, 2, 40, Completion::Complete),
        Candidate(33, 1, 101, 1, 50),
        Candidate(33, 3, 103, 3, 100, Completion::Incomplete, GateState::Blocked)
    });

    ASSERT_EQ(result.status, Status::Selected);
    ExpectKey(result.selected, 33, 1);
    EXPECT_EQ(result.selectedGoalId, 101u);
    ASSERT_EQ(result.decisions.size(), 3u);
    EXPECT_EQ(result.decisions[0].reason, BlockedReason::None);
    EXPECT_EQ(result.decisions[1].reason, BlockedReason::AlreadyComplete);
    EXPECT_EQ(result.decisions[2].reason, BlockedReason::PrerequisitesBlocked);
}

TEST(DungeonEncounterSelectionPolicy, CompletingSelectedEncounterAdvancesDeterministically)
{
    std::vector<CandidateFacts> candidates = {
        Candidate(44, 1, 201, 1, 80), Candidate(44, 2, 202, 2, 70)};

    Selection const first = Select(candidates);
    ExpectKey(first.selected, 44, 1);
    candidates[0].completion = Completion::Complete;
    Selection const advanced = Select(candidates);
    ASSERT_EQ(advanced.status, Status::Selected);
    ExpectKey(advanced.selected, 44, 2);
    EXPECT_EQ(advanced.selectedGoalId, 202u);
}

TEST(DungeonEncounterSelectionPolicy, UnknownAndFailedGatesFailClosedWithDistinctReasons)
{
    Selection const result = Select({
        Candidate(1, 1, 11, 1, 1, Completion::Unknown),
        Candidate(1, 2, 12, 2, 1, Completion::Incomplete, GateState::Unknown),
        Candidate(1, 3, 13, 3, 1, Completion::Incomplete, GateState::Blocked),
        Candidate(1, 4, 14, 4, 1, Completion::Incomplete, GateState::Satisfied, Reachability::Unknown),
        Candidate(1, 5, 15, 5, 1, Completion::Incomplete, GateState::Satisfied, Reachability::Unreachable)
    });

    ASSERT_EQ(result.status, Status::Blocked);
    ASSERT_EQ(result.decisions.size(), 5u);
    EXPECT_EQ(result.decisions[0].reason, BlockedReason::CompletionUnknown);
    EXPECT_EQ(result.decisions[1].reason, BlockedReason::PrerequisitesUnknown);
    EXPECT_EQ(result.decisions[2].reason, BlockedReason::PrerequisitesBlocked);
    EXPECT_EQ(result.decisions[3].reason, BlockedReason::ReachabilityUnknown);
    EXPECT_EQ(result.decisions[4].reason, BlockedReason::Unreachable);
}

TEST(DungeonEncounterSelectionPolicy, ClassificationStopsAtCompletionThenPrerequisitesThenReachability)
{
    Selection const result = Select({
        Candidate(2, 1, 21, 1, 1, Completion::Unknown, GateState::Blocked, Reachability::Unreachable),
        Candidate(2, 2, 22, 2, 1, Completion::Complete, GateState::Unknown, Reachability::Unknown),
        Candidate(2, 3, 23, 3, 1, Completion::Incomplete, GateState::Blocked, Reachability::Unreachable)
    });

    ASSERT_EQ(result.decisions.size(), 3u);
    EXPECT_EQ(result.decisions[0].reason, BlockedReason::CompletionUnknown);
    EXPECT_EQ(result.decisions[1].reason, BlockedReason::AlreadyComplete);
    EXPECT_EQ(result.decisions[2].reason, BlockedReason::PrerequisitesBlocked);
}

TEST(DungeonEncounterSelectionPolicy, RankingUsesEveryDeclaredTieBreaker)
{
    EXPECT_EQ(Select({Candidate(9, 9, 99, 0, 9), Candidate(1, 1, 11, 99, 10)}).selectedGoalId, 11u);
    EXPECT_EQ(Select({Candidate(9, 9, 99, 2, 10), Candidate(1, 1, 11, 1, 10)}).selectedGoalId, 11u);
    EXPECT_EQ(Select({Candidate(9, 1, 99, 1, 10), Candidate(8, 9, 11, 1, 10)}).selectedGoalId, 11u);
    EXPECT_EQ(Select({Candidate(8, 2, 99, 1, 10), Candidate(8, 1, 11, 1, 10)}).selectedGoalId, 11u);
    EXPECT_EQ(Select({Candidate(8, 1, 99, 1, 10), Candidate(8, 2, 11, 1, 10)}).selectedGoalId, 99u);
}

TEST(DungeonEncounterSelectionPolicy, NegativeScoresRemainEligibleAndRankNormally)
{
    Selection const result = Select({Candidate(3, 1, 31, 1, -20), Candidate(3, 2, 32, 1, -5)});
    ASSERT_EQ(result.status, Status::Selected);
    EXPECT_EQ(result.selectedGoalId, 32u);
}

TEST(DungeonEncounterSelectionPolicy, ZeroEncounterIdIsValidWhenMapAndGoalArePresent)
{
    Selection const result = Select({Candidate(3, 0, 30, 1, 5)});
    ASSERT_EQ(result.status, Status::Selected);
    ExpectKey(result.selected, 3, 0);
    EXPECT_EQ(result.selectedGoalId, 30u);
}

TEST(DungeonEncounterSelectionPolicy, SelectionAndDiagnosticsArePermutationStableAndIdempotent)
{
    std::vector<CandidateFacts> candidates = {
        Candidate(7, 4, 74, 3, 10, Completion::Incomplete, GateState::Satisfied, Reachability::Unknown),
        Candidate(7, 2, 72, 2, 20),
        Candidate(6, 9, 69, 1, 20),
        Candidate(7, 1, 71, 1, 100, Completion::Complete)
    };
    Selection const expected = Select(candidates);
    ExpectEquivalent(expected, Select(candidates));

    std::reverse(candidates.begin(), candidates.end());
    ExpectEquivalent(expected, Select(candidates));
    std::rotate(candidates.begin(), candidates.begin() + 1, candidates.end());
    ExpectEquivalent(expected, Select(candidates));
}

TEST(DungeonEncounterSelectionPolicy, AllValidCompleteCandidatesReportAllComplete)
{
    Selection const result = Select({
        Candidate(5, 2, 52, 2, 10, Completion::Complete),
        Candidate(5, 1, 51, 1, 20, Completion::Complete)});
    EXPECT_EQ(result.status, Status::AllComplete);
    EXPECT_EQ(result.decisions[0].reason, BlockedReason::AlreadyComplete);
    EXPECT_EQ(result.decisions[1].reason, BlockedReason::AlreadyComplete);
}

TEST(DungeonEncounterSelectionPolicy, ZeroMapOrGoalAndDuplicateEncounterKeysAreInvalid)
{
    Selection const result = Select({
        Candidate(0, 1, 10, 1, 1),
        Candidate(1, 2, 0, 2, 1),
        Candidate(2, 3, 20, 3, 1),
        Candidate(2, 3, 21, 4, 2)});

    ASSERT_EQ(result.status, Status::InvalidInput);
    ExpectKey(result.selected, 0, 0);
    ASSERT_EQ(result.decisions.size(), 4u);
    EXPECT_EQ(result.decisions[0].reason, BlockedReason::InvalidKey);
    EXPECT_EQ(result.decisions[1].reason, BlockedReason::InvalidKey);
    EXPECT_EQ(result.decisions[2].reason, BlockedReason::DuplicateKey);
    EXPECT_EQ(result.decisions[3].reason, BlockedReason::DuplicateKey);
}

TEST(DungeonEncounterSelectionPolicy, InvalidDiagnosticsAreDeterministicUnderPermutation)
{
    std::vector<CandidateFacts> candidates = {
        Candidate(9, 4, 94, 2, 10), Candidate(9, 4, 91, 1, 20), Candidate(0, 8, 88, 3, 30)};
    Selection const expected = Select(candidates);
    std::reverse(candidates.begin(), candidates.end());
    ExpectEquivalent(expected, Select(candidates));
}
