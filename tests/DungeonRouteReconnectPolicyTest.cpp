/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonRouteReconnectPolicy.h"
#include "DungeonNavigatorCombatPolicy.h"
#include "DungeonNavigatorConvoyPolicy.h"

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
std::string ReadDungeonNavigatorSource()
{
    std::filesystem::path const root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream input(root / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}
}

TEST(DungeonRouteReconnectPolicy, SkipsUnsafeStoredPrefixForIndependentlyReachablePoint)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {
        {false, false, 0.0f}, {true, true, 18.0f}, {true, true, 39.0f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f), 2u);
}

TEST(DungeonRouteReconnectPolicy, RejectsSafePointOutsideBoundedMovementWindow)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {
        {false, false, 0.0f}, {true, true, 46.0f}, {true, true, 80.0f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, DoesNotTreatZeroLengthProbeAsReconnect)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {{true, true, 0.0f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, RejectsSafeIncompleteProbeThatMissedDestination)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {{true, false, 44.675f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, DirectReachabilityRejectsSafeNormalIncompleteProgress)
{
    EXPECT_FALSE(DungeonRouteReconnect::DestinationReached(true, false, true, true));
    EXPECT_TRUE(DungeonRouteReconnect::DestinationReached(true, false, true, false));
    EXPECT_TRUE(DungeonRouteReconnect::DestinationReached(true, true, false, false));
    EXPECT_FALSE(DungeonRouteReconnect::DestinationReached(false, true, true, false));
}

TEST(DungeonRouteReconnectPolicy, CompleteEarlierCandidateWinsOverIncompleteLaterCandidate)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {
        {true, true, 31.0f}, {true, false, 44.675f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f), 0u);
}

TEST(DungeonRouteReconnectPolicy, AcceptsExactMovementBoundary)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates = {{true, true, 45.0f}};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f), 0u);
}

TEST(DungeonRouteReconnectPolicy, EmptyAndUnsafeCandidatesFailClosed)
{
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable({}, 45.0f),
              DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable({{false, true, 20.0f}}, 45.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, NoProgressRetriesThenReplansOnceThenBlocks)
{
    using DungeonRouteReconnect::NoProgressDecision;

    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(0, 3, 0, 1),
        NoProgressDecision::Retry);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(2, 3, 0, 1),
        NoProgressDecision::Retry);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(3, 3, 0, 1),
        NoProgressDecision::Replan);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(3, 3, 1, 1),
        NoProgressDecision::Block);
}

TEST(DungeonRouteReconnectPolicy, NoProgressInvalidLimitsFailClosed)
{
    using DungeonRouteReconnect::NoProgressDecision;

    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(0, 0, 0, 1),
        NoProgressDecision::Block);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateNoProgress(3, 3, 0, 0),
        NoProgressDecision::Block);
}

TEST(DungeonRouteReconnectPolicy, SharedAnchorPrefixWidensToLaterEstablishedAnchor)
{
    DungeonRouteReconnect::SharedAnchorSearchBounds const bounds = {5, 16, 128};
    EXPECT_EQ(DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(1, 64, bounds), 5u);
    EXPECT_EQ(DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(2, 64, bounds), 16u);
    EXPECT_EQ(DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(3, 64, bounds), 64u);
    EXPECT_EQ(DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(4, 7, bounds), 7u);

    std::vector<DungeonNavigatorConvoy::SharedCandidate> candidates(9);
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        DungeonNavigatorConvoy::SharedCandidate& candidate = candidates[index];
        candidate.routeIndex = 12 - index;
        candidate.sameMap = true;
        candidate.ordinaryWalk = index >= 5;
        candidate.followerSafe = candidate.ordinaryWalk;
        candidate.followerDestinationReached = candidate.ordinaryWalk;
        candidate.followerPathLength = 10.0f;
        candidate.followerPhysicalProgress = 10.0f;
        candidate.leaderSafe = candidate.ordinaryWalk;
        candidate.leaderDestinationReached = candidate.ordinaryWalk;
        candidate.leaderPathLength = 10.0f;
        candidate.leaderPhysicalProgress = 10.0f;
    }

    DungeonNavigatorConvoy::SharedRegroupBounds search = {
        DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(1, candidates.size(), bounds),
        1.5f, 3.0f, 128.0f};
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
                  candidates, 12, 11, search), DungeonNavigatorConvoy::NoSelection);

    search.maximumCandidates = DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(
        2, candidates.size(), bounds);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
                  candidates, 12, 11, search), 5u);
    EXPECT_EQ(candidates[5].routeIndex, 7u);
}

TEST(DungeonRouteReconnectPolicy,
     SharedRegroupTerminalDeduplicatesAndResetsOnContextChangeOrRejoin)
{
    DungeonRouteReconnect::SharedRegroupContext const original = {600, 17, 1, 127424, 42};
    DungeonRouteReconnect::SharedRegroupTerminalState state;

    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, original, false),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::Continue);
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, original, true),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::EnterTerminal);
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, original, true),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::NoOp);
    EXPECT_TRUE(DungeonRouteReconnect::IsSharedRegroupTerminal(state, original));

    DungeonRouteReconnect::SharedRegroupContext changed = original;
    changed.encounterId = 2;
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, changed, false),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::Continue);
    EXPECT_FALSE(DungeonRouteReconnect::IsSharedRegroupTerminal(state, changed));
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, changed, true),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::EnterTerminal);
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, changed, true),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::NoOp);

    DungeonRouteReconnect::ResetSharedRegroupEpoch(state);
    EXPECT_EQ(DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                  state, changed, false),
        DungeonRouteReconnect::SharedRegroupTerminalDecision::Continue);
    EXPECT_FALSE(DungeonRouteReconnect::IsSharedRegroupTerminal(state, changed));
}

TEST(DungeonRouteReconnectPolicy, FirstNoProofPendsSecondTerminatesAndLaterTicksAreNoOps)
{
    using DungeonRouteReconnect::WalkRevalidationDecision;

    DungeonRouteReconnect::WalkRevalidationState state;
    DungeonRouteReconnect::WalkRevalidationKey const key = {1, 2, 3, 4, 1};

    EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
        WalkRevalidationDecision::EnterPending);
    EXPECT_TRUE(DungeonRouteReconnect::IsWalkRevalidationPending(state, key));

    EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
        WalkRevalidationDecision::EnterTerminal);
    EXPECT_TRUE(DungeonRouteReconnect::IsWalkRevalidationTerminal(state, key));

    EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
        WalkRevalidationDecision::NoOp);
    EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
        WalkRevalidationDecision::NoOp);
}

TEST(DungeonRouteReconnectPolicy, EveryChangedRevalidationKeyGetsOneNewPendingCycle)
{
    using DungeonRouteReconnect::WalkRevalidationDecision;

    DungeonRouteReconnect::WalkRevalidationKey const original = {1, 2, 3, 4, 1};
    std::vector<DungeonRouteReconnect::WalkRevalidationKey> const changedKeys = {
        {5, 2, 3, 4, 1},
        {1, 5, 3, 4, 1},
        {1, 2, 5, 4, 1},
        {1, 2, 3, 5, 1},
        {1, 2, 3, 4, 2},
    };

    for (DungeonRouteReconnect::WalkRevalidationKey const& changed : changedKeys)
    {
        DungeonRouteReconnect::WalkRevalidationState state;
        EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, original),
            WalkRevalidationDecision::EnterPending);
        EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, original),
            WalkRevalidationDecision::EnterTerminal);
        EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, changed),
            WalkRevalidationDecision::EnterPending);
        EXPECT_TRUE(DungeonRouteReconnect::IsWalkRevalidationPending(state, changed));
    }
}

TEST(DungeonRouteReconnectPolicy, PendingRevalidationAcceptsOnlyFreshOrdinaryWalkEvidence)
{
    DungeonRouteReconnect::WalkRevalidationKey const key = {1, 2, 3, 4, 1};
    DungeonRouteReconnect::WalkRevalidationKey const wrongKey = {1, 2, 3, 5, 1};
    DungeonRouteReconnect::FreshOrdinaryWalkEvidence const accepted = {
        true, true, true, true};

    DungeonRouteReconnect::WalkRevalidationState idle;
    EXPECT_FALSE(DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
        idle, key, accepted));

    for (DungeonRouteReconnect::FreshOrdinaryWalkEvidence const& rejected : {
             DungeonRouteReconnect::FreshOrdinaryWalkEvidence{false, true, true, true},
             DungeonRouteReconnect::FreshOrdinaryWalkEvidence{true, false, true, true},
             DungeonRouteReconnect::FreshOrdinaryWalkEvidence{true, true, false, true},
             DungeonRouteReconnect::FreshOrdinaryWalkEvidence{true, true, true, false},
         })
    {
        DungeonRouteReconnect::WalkRevalidationState state;
        DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key);
        EXPECT_FALSE(DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
            state, key, rejected));
        EXPECT_TRUE(DungeonRouteReconnect::IsWalkRevalidationPending(state, key));
        EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
            DungeonRouteReconnect::WalkRevalidationDecision::EnterTerminal);
    }

    DungeonRouteReconnect::WalkRevalidationState state;
    DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key);
    EXPECT_FALSE(DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
        state, wrongKey, accepted));
    EXPECT_TRUE(DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
        state, key, accepted));
    EXPECT_FALSE(DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
        state, key, accepted));
    EXPECT_EQ(DungeonRouteReconnect::ObserveWalkRevalidationNoProof(state, key),
        DungeonRouteReconnect::WalkRevalidationDecision::NoOp);
}

TEST(DungeonRouteReconnectPolicy, ReplanWalkAdmissionIsTableDrivenAndFailClosed)
{
    struct AdmissionCase
    {
        char const* name;
        DungeonRouteReconnect::ReplanWalkCandidateFacts facts;
        bool expected;
    };

    std::vector<AdmissionCase> const cases = {
        {"accepted fully proven walk candidate", {0, 8, true, true, true, true, true, true}, true},
        {"incomplete attachment", {0, 8, true, true, true, true, false, true}, false},
        {"unsafe attachment", {0, 8, true, true, true, false, true, true}, false},
        {"cross map candidate", {0, 8, true, false, true, true, true, true}, false},
        {"special candidate", {0, 8, true, true, true, true, true, false}, false},
        {"candidate at limit", {8, 8, true, true, true, true, true, true}, false},
        {"zero candidate limit", {0, 0, true, true, true, true, true, true}, false},
    };

    for (AdmissionCase const& testCase : cases)
    {
        EXPECT_EQ(DungeonRouteReconnect::AdmitReplanWalkCandidate(testCase.facts),
            testCase.expected) << testCase.name;
    }
}

TEST(DungeonRouteReconnectPolicy, StoredWalkAttachmentSkipsUnsafePointZeroForReachablePointOne)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates = {
        {true, false, false}, {true, true, true}, {true, true, true}};
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 32), 1u);
}

TEST(DungeonRouteReconnectPolicy, StoredWalkAttachmentChoosesEarliestReachablePoint)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates = {
        {true, false, false}, {true, true, true}, {true, true, true}};
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 32), 1u);
}

TEST(DungeonRouteReconnectPolicy, StoredWalkAttachmentCanUseLargerBoundedRecoveryPrefix)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates(97);
    candidates[96] = {true, true, true};

    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 32), DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 128), 96u);
}

TEST(DungeonRouteReconnectPolicy, StoredWalkAttachmentRejectsSafeIncompleteCandidate)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates = {
        {true, true, false}, {true, false, false}};
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 32), DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, TransitionSuffixCannotUseStoredWalkPrefixRecovery)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates = {
        {true, true, true}};
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, true, 32), DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, false, false, 32), DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, StoredWalkAttachmentIsBoundedAndFailsClosed)
{
    std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate> candidates = {
        {true, false, false}, {true, true, true}};
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  candidates, true, false, 1), DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                  {}, true, false, 32), DungeonRouteReconnect::NoSelection);
}

TEST(DungeonNavigatorStoredWalkAttachmentSourceContract,
     PreparedAttachmentPrecedesStoredSuffixWithoutMetadataCursorAdvance)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const recovery = source.find("recovery=stored_walk_prefix_attachment");
    std::size_t const prepared = source.find(
        "for (G3D::Vector3 const& point : storedWalkAttachmentProbe.path)", recovery);
    std::size_t const suffix = source.find(
        "storedWalkAttachment + 1", prepared);
    ASSERT_NE(recovery, std::string::npos);
    ASSERT_NE(prepared, std::string::npos);
    ASSERT_NE(suffix, std::string::npos);
    EXPECT_LT(prepared, suffix);

    std::string_view const attachment(source.data() + recovery, suffix - recovery);
    EXPECT_EQ(attachment.find("TeleportTo("), std::string::npos);
    EXPECT_EQ(attachment.find("NearTeleportTo("), std::string::npos);
    EXPECT_EQ(attachment.find("travelRouteNextIndex = storedWalkAttachment"),
        std::string::npos);
    EXPECT_NE(source.find("walk_prefix_unreachable"), std::string::npos);
}

TEST(DungeonNavigatorReplanRecoverySourceContract,
     WalkRecoveryIsGatedBeforeOpaqueFullPathAndHasNoTransitionExecutor)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const gate = source.find("bool const replanOnlyWalkRecovery =");
    std::size_t const graph = source.find("FindStoredWalkGraphRoute(start, finish, bot", gate);
    std::size_t const replanPass = source.find("result.replanAttempted = true");
    std::size_t const recoveryGate = source.find(
        "if (replanOnlyWalkRecovery && storedWalkGraphResult.replanAttempted)");
    std::size_t const fullPath = source.find("TravelNodeMap::getFullPath", gate);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(graph, std::string::npos);
    ASSERT_NE(replanPass, std::string::npos);
    ASSERT_NE(recoveryGate, std::string::npos);
    ASSERT_NE(fullPath, std::string::npos);
    EXPECT_LT(gate, graph);
    EXPECT_LT(replanPass, fullPath);
    EXPECT_LT(recoveryGate, fullPath);
    EXPECT_LT(graph, fullPath);

    std::string_view const replanPassWindow(source.data() + replanPass,
        fullPath - replanPass);
    EXPECT_NE(replanPassWindow.find("TravelNodeReplanAttachmentRadius"), std::string::npos);
    EXPECT_NE(replanPassWindow.find("TravelNodeReplanCandidateLimit"), std::string::npos);
    EXPECT_NE(replanPassWindow.find("ProbeReachedStoredDestination"), std::string::npos);
    EXPECT_NE(replanPassWindow.find("stored_walk_graph_replan"), std::string::npos);
    EXPECT_NE(replanPassWindow.find("no_proven_walk_recovery"), std::string::npos);

    std::string_view const recovery(source.data() + recoveryGate, fullPath - recoveryGate);
    EXPECT_EQ(recovery.find("TravelNodeMap::getFullPath"), std::string::npos);
    EXPECT_EQ(recovery.find("TravelNodePathType::portal"), std::string::npos);
    EXPECT_EQ(recovery.find("TravelNodePathType::transport"), std::string::npos);
    EXPECT_EQ(recovery.find("TravelNodePathType::flightPath"), std::string::npos);
    EXPECT_EQ(recovery.find("TravelNodePathType::teleportSpell"), std::string::npos);
    EXPECT_EQ(recovery.find("HandleGameObjectUseOpcode"), std::string::npos);
    EXPECT_EQ(recovery.find("CMSG_GAMEOBJ_USE"), std::string::npos);
    EXPECT_EQ(recovery.find("NearTeleportTo"), std::string::npos);
}

TEST(DungeonNavigatorReplanRecoverySourceContract,
     NoProofRevalidationIsDelayedKeyedTerminalOnceAndMovementFree)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const key = source.find(
        "DungeonRouteReconnect::WalkRevalidationKey const walkRevalidationKey");
    std::size_t const noProof = source.find(
        "if (replanOnlyWalkRecovery && route.empty())", key);
    std::size_t const fallback = source.find(
        "TravelNodeMap::getFullPath", noProof);
    std::size_t const terminalGate = source.find(
        "IsWalkRevalidationTerminal", fallback);
    ASSERT_NE(key, std::string::npos);
    ASSERT_NE(noProof, std::string::npos);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(terminalGate, std::string::npos);
    EXPECT_LT(key, noProof);
    EXPECT_LT(noProof, fallback);
    EXPECT_LT(fallback, terminalGate);

    std::string_view const noProofWindow(source.data() + noProof, fallback - noProof);
    EXPECT_NE(noProofWindow.find("ObserveWalkRevalidationNoProof"), std::string::npos);
    EXPECT_NE(noProofWindow.find("recovery=pending_revalidation"), std::string::npos);
    EXPECT_NE(noProofWindow.find("blocked=no_proven_walk_recovery revalidation=terminal"),
        std::string::npos);
    EXPECT_NE(noProofWindow.find("return false;"), std::string::npos);
    for (std::string_view forbidden : {"MoveTo(", "WalkPrepared(", "TeleportTo(",
             "NearTeleportTo(", "getFullPath", "++travelRouteNextIndex"})
    {
        EXPECT_EQ(noProofWindow.find(forbidden), std::string::npos) << forbidden;
    }

    EXPECT_NE(source.find("AcceptFreshOrdinaryWalkEvidence", key), std::string::npos);
    EXPECT_NE(source.find("evidence=fresh_ordinary_walk", key), std::string::npos);
    EXPECT_NE(source.find("SelectSharedBackwardReanchor"), std::string::npos);
}

TEST(DungeonRouteReconnectPolicy, BackwardAnchorRequiresSameMapAndIncludesExactRadius)
{
    std::vector<DungeonRouteReconnect::StoredPoint> points(8);
    points[4] = {true, 3.0f};
    points[5] = {false, 1.0f};
    EXPECT_EQ(DungeonRouteReconnect::SelectBackwardAnchor(points, 7, 4, 3.0f), 4u);
}

TEST(DungeonRouteReconnectPolicy, BackwardAnchorIsBoundedAndFailsWithoutPhysicalEvidence)
{
    std::vector<DungeonRouteReconnect::StoredPoint> points(10);
    points[2] = {true, 0.0f};
    EXPECT_EQ(DungeonRouteReconnect::SelectBackwardAnchor(points, 9, 4, 3.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, BackwardAnchorAcceptsImmediatePredecessor)
{
    std::vector<DungeonRouteReconnect::StoredPoint> points(6);
    points[4] = {true, 2.0f};
    EXPECT_EQ(DungeonRouteReconnect::SelectBackwardAnchor(points, 5, 4, 3.0f), 4u);
}

TEST(DungeonRouteReconnectPolicy, NexusIncompleteProbeCannotJump39To71)
{
    std::vector<DungeonRouteReconnect::Candidate> candidates(32);
    candidates.back() = {true, false, 44.675f};
    EXPECT_EQ(DungeonRouteReconnect::SelectFarthestReachable(candidates, 45.0f),
              DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, NexusOvershot71RecoversToHighestPhysicallyReachedPoint)
{
    std::vector<DungeonRouteReconnect::StoredPoint> points(72);
    points[55] = {true, 1.2f};
    points[54] = {true, 2.0f};
    EXPECT_EQ(DungeonRouteReconnect::SelectBackwardAnchor(points, 71, 32, 3.0f), 55u);
}

TEST(DungeonRouteReconnectPolicy, GroundReattachAcceptsSmallProvenUpwardCorrection)
{
    EXPECT_TRUE(DungeonRouteReconnect::CanGroundReattach(true, 0.432f, true, 0.1f, 2.0f));
    EXPECT_TRUE(DungeonRouteReconnect::CanGroundReattach(true, 2.0f, true, 0.1f, 2.0f));
}

TEST(DungeonRouteReconnectPolicy, GroundReattachFailsClosedWithoutEveryGuard)
{
    EXPECT_FALSE(DungeonRouteReconnect::CanGroundReattach(false, 0.432f, true, 0.1f, 2.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanGroundReattach(true, 0.432f, false, 0.1f, 2.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanGroundReattach(true, 0.05f, true, 0.1f, 2.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanGroundReattach(true, 2.01f, true, 0.1f, 2.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanGroundReattach(
        true, std::numeric_limits<float>::infinity(), true, 0.1f, 2.0f));
}

TEST(DungeonRouteReconnectPolicy, SafeIncompleteProbeProvidesBoundedProgressWithoutArrival)
{
    EXPECT_TRUE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 12.0f, 12.0f, 1.5f, 45.0f));
    EXPECT_TRUE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 12.0001f, 12.0f, 1.5f, 12.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 12.5001f, 12.0f, 1.5f, 12.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        true, true, 12.0f, 12.0f, 1.5f, 45.0f));
}

TEST(DungeonRouteReconnectPolicy, PartialProgressRejectsUnsafeDegenerateAndUnboundedMotion)
{
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        false, false, 12.0f, 12.0f, 1.5f, 45.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 0.0f, 0.0f, 1.5f, 45.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 46.0f, 12.0f, 1.5f, 45.0f));
    EXPECT_FALSE(DungeonRouteReconnect::CanUsePartialProgress(
        true, false, 12.0f, 1.49f, 1.5f, 45.0f));
}

TEST(DungeonRouteReconnectPolicy, LongCompleteSafePathSelectsFurthestCautiousPrefix)
{
    std::vector<float> const cumulative = {0.0f, 4.0f, 8.0f, 12.0f, 15.2f};
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(cumulative, 1.5f, 12.0f), 3u);
}

TEST(DungeonRouteReconnectPolicy, CompleteSafePrefixAcceptsExactMovementBoundary)
{
    std::vector<float> const cumulative = {0.0f, 1.5f, 12.0f, 12.1f};
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(cumulative, 1.5f, 12.0f), 2u);
}

TEST(DungeonRouteReconnectPolicy, CompleteSafePrefixFailsWhenPathHasNoBoundedVertex)
{
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(
                  {0.0f, 1.49f, 12.01f}, 1.5f, 12.0f),
        DungeonRouteReconnect::NoSelection);
}

TEST(DungeonRouteReconnectPolicy, CompleteSafePrefixRejectsMalformedDistanceSeries)
{
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(
                  {0.0f, 8.0f, 7.0f}, 1.5f, 12.0f),
        DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(
                  {0.0f, std::numeric_limits<float>::infinity()}, 1.5f, 12.0f),
        DungeonRouteReconnect::NoSelection);
    EXPECT_EQ(DungeonRouteReconnect::SelectPreparedPathPrefix(
                  {1.0f, 8.0f}, 1.5f, 12.0f),
        DungeonRouteReconnect::NoSelection);
}

TEST(DungeonNavigatorPreparedCompletePrefixSourceContract,
     ConsumesValidatedPrefixWithoutAdvancingStoredCursorOrTeleporting)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const branchBegin = source.find(
        "if (!partialFrontier && probe.safe && destinationReached");
    std::size_t const branchEnd = source.find(
        "if (!partialFrontier && !probe.path.empty())", branchBegin);
    ASSERT_NE(branchBegin, std::string::npos);
    ASSERT_NE(branchEnd, std::string::npos);

    std::string_view const branch(source.data() + branchBegin, branchEnd - branchBegin);
    EXPECT_NE(branch.find("SelectPreparedPathPrefix"), std::string::npos);
    EXPECT_NE(branch.find("prefixProbe.path.resize(prefixIndex + 1)"), std::string::npos);
    EXPECT_EQ(branch.find("travelRouteNextIndex ="), std::string::npos);
    EXPECT_EQ(branch.find("TeleportTo("), std::string::npos);
    EXPECT_EQ(branch.find("NearTeleportTo("), std::string::npos);
    EXPECT_NE(source.find("prepared_complete_prefix"), std::string::npos);
}

TEST(DungeonRouteReconnectPolicy, SafePreparedCorridorMayReplaceOpaqueTransitionMetadata)
{
    EXPECT_TRUE(DungeonRouteReconnect::CanCachePreparedCorridor(
        true, 18, 44.675f, 39.0f, 1.5f));
}

TEST(DungeonRouteReconnectPolicy, PreparedCorridorRequiresValidatedMeaningfulPhysicalProgress)
{
    EXPECT_FALSE(DungeonRouteReconnect::CanCachePreparedCorridor(
        false, 18, 44.675f, 39.0f, 1.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanCachePreparedCorridor(
        true, 1, 44.675f, 39.0f, 1.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanCachePreparedCorridor(
        true, 18, 0.0f, 39.0f, 1.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanCachePreparedCorridor(
        true, 18, 44.675f, 1.49f, 1.5f));
    EXPECT_FALSE(DungeonRouteReconnect::CanCachePreparedCorridor(
        true, 18, std::numeric_limits<float>::infinity(), 39.0f, 1.5f));
}

TEST(DungeonRouteReconnectPolicy, PartyProvenLowerFloorAllowsReachableRouteReanchor)
{
    DungeonRouteReconnect::PartyRouteReanchorFacts facts;
    facts.anchorReachable = true;
    facts.sourceZ = -2.107f;
    facts.anchorZ = -16.125f;
    facts.members = {
        {true, 3.0f, -16.125f},
        {true, 4.0f, -16.125f},
        {true, 5.0f, -16.125f},
        {true, 6.0f, -16.125f},
    };

    DungeonRouteReconnect::PartyRouteReanchorDecision const decision =
        DungeonRouteReconnect::EvaluatePartyRouteReanchor(facts);
    EXPECT_TRUE(decision.allowed);
    EXPECT_EQ(decision.eligibleMembers, 4u);
    EXPECT_EQ(decision.supportingMembers, 4u);
}

TEST(DungeonRouteReconnectPolicy, PartyRouteReanchorRequiresStrictMajorityAndTwoWitnesses)
{
    DungeonRouteReconnect::PartyRouteReanchorFacts split;
    split.anchorReachable = true;
    split.sourceZ = 14.0f;
    split.anchorZ = 0.0f;
    split.members = {
        {true, 3.0f, 0.0f},
        {true, 4.0f, 0.0f},
        {true, 3.0f, 14.0f},
        {true, 4.0f, 14.0f},
    };
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(split).allowed);

    split.members = {{true, 3.0f, 0.0f}};
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(split).allowed);
}

TEST(DungeonRouteReconnectPolicy, PartyRouteReanchorFailsClosedWithoutPhysicalProof)
{
    DungeonRouteReconnect::PartyRouteReanchorFacts facts;
    facts.anchorReachable = true;
    facts.sourceZ = 14.0f;
    facts.anchorZ = 0.0f;
    facts.members = {{true, 3.0f, 0.0f}, {true, 4.0f, 0.0f}, {true, 5.0f, 0.0f}};

    auto unreachable = facts;
    unreachable.anchorReachable = false;
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(unreachable).allowed);

    auto upward = facts;
    upward.sourceZ = -2.0f;
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(upward).allowed);

    auto excessiveDrop = facts;
    excessiveDrop.sourceZ = 24.01f;
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(excessiveDrop).allowed);

    auto nonFiniteMember = facts;
    nonFiniteMember.members[0].z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(DungeonRouteReconnect::EvaluatePartyRouteReanchor(nonFiniteMember).allowed);
}

TEST(DungeonRouteReconnectPolicy, PhysicalStoredRouteArrivalReleasesCacheForTerminalActivation)
{
    using DungeonRouteReconnect::ExhaustedRouteDecision;
    EXPECT_EQ(DungeonRouteReconnect::EvaluateExhaustedRoute(72, 72, true, 3.0f, 3.0f),
              ExhaustedRouteDecision::ReleaseForTerminalActivation);
}

TEST(DungeonRouteReconnectPolicy, ExhaustedCursorWithoutFinalPointArrivalFailsClosed)
{
    using DungeonRouteReconnect::ExhaustedRouteDecision;
    EXPECT_EQ(DungeonRouteReconnect::EvaluateExhaustedRoute(72, 72, true, 3.01f, 3.0f),
              ExhaustedRouteDecision::Block);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateExhaustedRoute(72, 72, false, 0.0f, 3.0f),
              ExhaustedRouteDecision::Block);
    EXPECT_EQ(DungeonRouteReconnect::EvaluateExhaustedRoute(0, 0, true, 0.0f, 3.0f),
              ExhaustedRouteDecision::Block);
}

TEST(DungeonNavigatorConvoyPolicy, AdvancesOriginFollowerToFarthestBoundedExactRoutePoint)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {0, true, true, true, 0.0f, 0.0f},
        {10, true, true, true, 18.0f, 18.0f},
        {20, true, true, true, 39.0f, 39.0f},
        {31, true, true, true, 46.0f, 46.0f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 32, 1, 1.5f, 45.0f), 2u);
}

TEST(DungeonNavigatorConvoyPolicy, StaggersFollowersBehindLeaderByRouteOrder)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {27, true, true, true, 20.0f, 20.0f},
        {28, true, true, true, 21.0f, 21.0f},
        {29, true, true, true, 22.0f, 22.0f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 30, 1, 1.5f, 45.0f), 2u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 30, 2, 1.5f, 45.0f), 1u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 30, 3, 1.5f, 45.0f), 0u);
}

TEST(DungeonNavigatorConvoyPolicy, RejectsUnsafeIncompleteOffMapAndUnboundedCandidates)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {1, true, false, true, 10.0f, 10.0f},
        {2, true, true, false, 10.0f, 10.0f},
        {3, false, true, true, 10.0f, 10.0f},
        {4, true, true, true, 45.01f, 20.0f},
        {5, true, true, true, 20.0f, 1.49f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 10, 1, 1.5f, 45.0f),
        DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, FailsClosedWithoutFollowerSlotOrValidBounds)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {1, true, true, true, 10.0f, 10.0f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 10, 0, 1.5f, 45.0f),
        DungeonNavigatorConvoy::NoSelection);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 10, 1, 1.5f, 0.0f),
        DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, ClampsRearRaidSlotsToRouteOriginDuringStartup)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {0, true, true, true, 8.0f, 8.0f},
        {1, true, true, true, 12.0f, 12.0f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 8, 9, 1.5f, 45.0f), 0u);
}

TEST(DungeonNavigatorConvoyPolicy, AlreadyAtAssignedTrailingPointIsSettled)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(true, 0.0f, 3.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(true, 3.0f, 3.0f));
}

TEST(DungeonNavigatorConvoyPolicy, SettledCheckFailsClosedOffMapOrOutsideRadius)
{
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(false, 0.0f, 3.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(true, 3.01f, 3.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(
        true, std::numeric_limits<float>::infinity(), 3.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(true, 0.0f, -1.0f));
}

TEST(DungeonNavigatorConvoyPolicy, UnreachableSlotMayHoldInsideNormalCohesionEnvelope)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::CanHoldWithinCohesion(true, 17.74f, 15.0f, 45.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::CanHoldWithinCohesion(true, 45.0f, 15.0f, 45.0f));
}

TEST(DungeonNavigatorConvoyPolicy, CohesionHoldFailsClosedForSeparatedOrInvalidMember)
{
    EXPECT_FALSE(DungeonNavigatorConvoy::CanHoldWithinCohesion(false, 17.74f, 15.0f, 45.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanHoldWithinCohesion(true, 15.0f, 15.0f, 45.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanHoldWithinCohesion(true, 45.01f, 15.0f, 45.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanHoldWithinCohesion(
        true, std::numeric_limits<float>::infinity(), 15.0f, 45.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanHoldWithinCohesion(true, 17.74f, 45.0f, 15.0f));
}

TEST(DungeonNavigatorConvoyPolicy, GroundReattachAcceptsProvenOutOfCombatTerrainSplit)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::CanGroundReattach(
        true, true, true, false, false, false, true, 52.2f, true, 8.0f, 64.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::CanGroundReattach(
        true, true, true, false, false, false, true, 8.0f, true, 8.0f, 64.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::CanGroundReattach(
        true, true, true, false, false, false, true, 64.0f, true, 8.0f, 64.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::CanGroundReattach(
        true, true, true, false, false, false, true, 119.0f, true, 8.0f, 192.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanGroundReattach(
        true, true, true, false, false, false, true, 192.01f, true, 8.0f, 192.0f));
}

TEST(DungeonNavigatorConvoyPolicy, GroundReattachFailsClosedWithoutEverySafetyProof)
{
    auto allowed = [](bool hasRoute, bool sameContext, bool alive, bool inCombat,
                       bool teleporting, bool onTransport, bool floorValid,
                       float correction, bool continuation)
    {
        return DungeonNavigatorConvoy::CanGroundReattach(hasRoute, sameContext, alive,
            inCombat, teleporting, onTransport, floorValid, correction, continuation,
            8.0f, 64.0f);
    };

    EXPECT_FALSE(allowed(false, true, true, false, false, false, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, false, true, false, false, false, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, false, false, false, false, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, true, true, false, false, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, true, false, true, false, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, true, false, false, true, true, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, false, 52.2f, true));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true, 52.2f, false));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true, 7.99f, true));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true, 64.01f, true));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true,
        std::numeric_limits<float>::infinity(), true));
}

TEST(DungeonNavigatorConvoyPolicy, RouteSlotReattachAcceptsLocalProvenTrailingSlot)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::CanRouteSlotReattach(
        true, true, true, false, false, false, true, true,
        45, 48, 17.8f, 49.0f, true, 20.0f, 8.0f, 192.0f));
}

TEST(DungeonNavigatorConvoyPolicy, RouteSlotReattachFailsClosedAtOrBeyondLeader)
{
    EXPECT_FALSE(DungeonNavigatorConvoy::CanRouteSlotReattach(
        true, true, true, false, false, false, true, true,
        48, 48, 9.6f, 49.0f, true, 12.0f, 8.0f, 192.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanRouteSlotReattach(
        true, true, true, false, false, false, true, true,
        49, 48, 9.6f, 49.0f, true, 12.0f, 8.0f, 192.0f));
}

TEST(DungeonNavigatorConvoyPolicy, RouteSlotReattachRequiresEveryPhysicalProof)
{
    auto allowed = [](bool sameContext, bool targetFloorValid, bool targetSameMap,
                       float horizontal, float correction, bool continuation)
    {
        return DungeonNavigatorConvoy::CanRouteSlotReattach(
            true, sameContext, true, false, false, false, targetFloorValid,
            targetSameMap, 47, 48, horizontal, correction, continuation,
            12.0f, 8.0f, 192.0f);
    };
    EXPECT_FALSE(allowed(false, true, true, 9.6f, 49.0f, true));
    EXPECT_FALSE(allowed(true, false, true, 9.6f, 49.0f, true));
    EXPECT_FALSE(allowed(true, true, false, 9.6f, 49.0f, true));
    EXPECT_FALSE(DungeonNavigatorConvoy::CanRouteSlotReattach(
        true, true, true, false, false, false, true, true,
        47, 48, 20.01f, 49.0f, true, 20.0f, 8.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, 9.6f, 7.99f, true));
    EXPECT_FALSE(allowed(true, true, true, 9.6f, 192.01f, true));
    EXPECT_FALSE(allowed(true, true, true, 9.6f, 49.0f, false));
}

TEST(DungeonNavigatorConvoyPolicy, PostCombatRejoinAcceptsSeparatedFollowerOnProvenPath)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::CanPostCombatRejoin(
        true, true, true, false, false, false, true, 78.0f, 69.49247f, 15.0f, 192.0f));
}

TEST(DungeonNavigatorConvoyPolicy, PostCombatRejoinFailsClosedWithoutEveryRecoveryGuard)
{
    auto allowed = [](bool completedRoute, bool sameContext, bool alive, bool inCombat,
                       bool teleporting, bool onTransport, bool pathReached,
                       float pathLength, float distance, float minimumDistance,
                       float maximumPathLength)
    {
        return DungeonNavigatorConvoy::CanPostCombatRejoin(
            completedRoute, sameContext, alive, inCombat, teleporting, onTransport,
            pathReached, pathLength, distance, minimumDistance, maximumPathLength);
    };

    EXPECT_FALSE(allowed(false, true, true, false, false, false, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, false, true, false, false, false, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, false, false, false, false, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, true, false, false, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, true, false, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, false, true, true,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, false,
        78.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true,
        78.0f, 15.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true,
        193.0f, 69.0f, 15.0f, 192.0f));
    EXPECT_FALSE(allowed(true, true, true, false, false, false, true,
        std::numeric_limits<float>::infinity(), 69.0f, 15.0f, 192.0f));
}

TEST(DungeonNavigatorConvoyPolicy, VerticalSplitFindsEarlierReachableReanchor)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {78, true, false, false, 0.0f, 89.0f},
        {77, true, false, false, 0.0f, 88.0f},
        {46, true, true, true, 112.0f, 91.0f},
        {45, true, true, true, 109.0f, 88.0f},
    };

    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        candidates, 79, 78, 1.5f), 2u);
}

TEST(DungeonNavigatorConvoyPolicy, BackwardReanchorDeterministicallySelectsFurthestReachable)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {12, true, true, true, 20.0f, 18.0f},
        {40, true, true, true, 70.0f, 64.0f},
        {23, true, true, true, 42.0f, 39.0f},
        {39, true, true, true, 68.0f, 62.0f},
    };

    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        candidates, 79, 78, 1.5f), 1u);
}

TEST(DungeonNavigatorConvoyPolicy, BackwardReanchorFailsClosedWhenNoPointIsReachable)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {46, true, false, true, 70.0f, 64.0f},
        {45, true, true, false, 68.0f, 62.0f},
        {44, false, true, true, 65.0f, 59.0f},
        {43, true, true, true, 0.0f, 0.0f},
    };

    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        candidates, 79, 78, 1.5f), DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, BackwardReanchorNeverSelectsAheadOfLeaderOrNormalSlot)
{
    std::vector<DungeonNavigatorConvoy::Candidate> candidates = {
        {80, true, true, true, 10.0f, 10.0f},
        {79, true, true, true, 10.0f, 10.0f},
        {78, true, true, true, 10.0f, 10.0f},
    };

    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        candidates, 79, 78, 1.5f), 2u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        candidates, 78, 79, 1.5f), DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, RememberedReanchorFloorPreventsBackwardOscillation)
{
    std::vector<DungeonNavigatorConvoy::Candidate> stalledAtRememberedPoint = {
        {42, true, true, true, 0.0f, 0.0f},
        {41, true, true, true, 9.0f, 9.0f},
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        stalledAtRememberedPoint, 79, 78, 1.5f, 42),
        DungeonNavigatorConvoy::NoSelection);

    stalledAtRememberedPoint.push_back({43, true, true, true, 8.0f, 8.0f});
    EXPECT_EQ(DungeonNavigatorConvoy::SelectBackwardReanchor(
        stalledAtRememberedPoint, 79, 78, 1.5f, 42), 2u);
}

TEST(DungeonNavigatorConvoySourceContract,
     RecoveryUsesPreparedMovementAndBoundedSameXyGroundReattach)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const convoyStart = source.find("auto combatGateAllowsConvoy");
    std::size_t const recovery = source.find("SelectBackwardReanchor");
    std::size_t const execution = source.find("if (!convoyPlans.empty())", recovery);
    std::size_t const convoyEnd = source.find("DungeonEncounterList const* encounters", execution);
    ASSERT_NE(convoyStart, std::string::npos);
    ASSERT_NE(recovery, std::string::npos);
    ASSERT_NE(execution, std::string::npos);
    ASSERT_NE(convoyEnd, std::string::npos);

    std::string_view const convoy(source.data() + convoyStart, convoyEnd - convoyStart);
    std::size_t const memberCombatGate =
        convoy.find("combatGateAllowsConvoy(member, leaderDistance)");
    std::size_t const cohesionGate = convoy.find("CanHoldWithinCohesion");
    std::size_t const recoveryPolicy = convoy.find("SelectBackwardReanchor");
    ASSERT_NE(memberCombatGate, std::string::npos);
    ASSERT_NE(cohesionGate, std::string::npos);
    ASSERT_NE(recoveryPolicy, std::string::npos);
    EXPECT_LT(memberCombatGate, recoveryPolicy);
    EXPECT_LT(cohesionGate, recoveryPolicy);
    EXPECT_NE(convoy.find("recovery=convoy_backward_reanchor"), std::string::npos);
    EXPECT_NE(convoy.find("ConvoyBackwardReanchorPointLimit"), std::string::npos);
    EXPECT_NE(convoy.find("ConvoyBackwardReanchorBackoffMs"), std::string::npos);
    EXPECT_NE(convoy.find("ProbeLeg(\n                        member"),
        std::string::npos);
    EXPECT_NE(convoy.find("AutoWowDungeonWalkAction walk(plan.memberAI)"), std::string::npos);
    EXPECT_EQ(convoy.find("AutoWowDungeonWalkAction walk(botAI)"), std::string::npos);
    EXPECT_EQ(convoy.find("MoveTo("), std::string::npos);
    EXPECT_EQ(convoy.find("bot->TeleportTo("), std::string::npos);
    EXPECT_NE(convoy.find(
        "member->NearTeleportTo(member->GetPositionX(), member->GetPositionY(),"),
        std::string::npos);
    EXPECT_NE(convoy.find("CanGroundReattach"), std::string::npos);
    EXPECT_NE(convoy.find("continuationReached"), std::string::npos);
    EXPECT_NE(convoy.find("recovery=convoy_ground_reattach"), std::string::npos);
    EXPECT_NE(convoy.find("SelectSharedAnchorSearchPrefix"), std::string::npos);
    EXPECT_NE(convoy.find("ConvoySharedRegroupSmallPrefix"), std::string::npos);
    EXPECT_NE(convoy.find("IsConvoySharedRegroupTerminal"), std::string::npos);
    EXPECT_NE(convoy.find("terminal=latched"), std::string::npos);
    EXPECT_EQ(convoy.find("member->TeleportTo("), std::string::npos);
    EXPECT_EQ(convoy.find("ordinary formation movement catch up"), std::string::npos);
    EXPECT_EQ(convoy.find("map=600"), std::string::npos);
}

TEST(DungeonNavigatorConvoySourceContract,
     CompletedCachedRouteRejoinsFollowersBeforeSelectingNextEncounter)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    std::size_t const completionCheck = source.find(
        "IsEncounterComplete(script, travelRouteEncounterId)");
    std::size_t const convoyGate = source.find("auto combatGateAllowsConvoy");
    ASSERT_NE(completionCheck, std::string::npos);
    ASSERT_NE(convoyGate, std::string::npos);
    EXPECT_LT(completionCheck, convoyGate);
    EXPECT_NE(source.find("recovery=convoy_post_combat_route_reset"), std::string::npos);
    EXPECT_NE(source.find("CanPostCombatRejoin"), std::string::npos);
    EXPECT_NE(source.find("recovery=convoy_post_combat_rejoin"), std::string::npos);
    EXPECT_NE(source.find("ProbeLeg(\n                member, bot->GetPositionX()"),
        std::string::npos);
    EXPECT_EQ(source.find("member->TeleportTo("), std::string::npos);
}

TEST(DungeonNavigatorPullPacingSourceContract, CapsEachLeaderSplineToCautiousCombatStride)
{
    std::string const source = ReadDungeonNavigatorSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("constexpr float TravelLookaheadDistance = 12.0f;"),
        std::string::npos);
    EXPECT_NE(source.find(
        "DungeonRouteReconnect::SelectFarthestReachable(\n                reconnectCandidates, TravelLookaheadDistance)"),
        std::string::npos);
    EXPECT_NE(source.find(
        "TravelLookaheadDistance + TravelPartialPathTolerance"), std::string::npos);
}

TEST(DungeonNavigatorCombatPolicy, GenuineCombatSignalsRemainHardBlocksAfterTimeout)
{
    using DungeonNavigatorCombat::Decision;
    using DungeonNavigatorCombat::Signals;

    for (Signals signals : {
             Signals{false, false, true, false, false, false, false},
             Signals{false, false, false, true, false, false, false},
             Signals{false, false, false, false, false, true, false},
             Signals{false, false, false, false, false, false, true},
         })
    {
        EXPECT_EQ(DungeonNavigatorCombat::Evaluate(signals, 300'000, true),
            Decision::BlockActive);
    }
}

TEST(DungeonNavigatorCombatPolicy, RelationshipOnlyCombatUsesAmbiguityTimeout)
{
    using DungeonNavigatorCombat::Decision;
    using DungeonNavigatorCombat::Signals;

    Signals const relationships{false, false, false, false, true, false, false};
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(relationships, 0, false),
        Decision::BlockAmbiguous);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(relationships, 29'999, false),
        Decision::BlockAmbiguous);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(relationships, 30'000, false),
        Decision::RecoverAndAllow);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(relationships, 35'000, true),
        Decision::AllowRecovered);
}

TEST(DungeonNavigatorCombatPolicy, CombatFlagWithRelationshipsUsesAmbiguityTimeout)
{
    using DungeonNavigatorCombat::Decision;
    using DungeonNavigatorCombat::Signals;

    Signals const flaggedRelationships{true, false, false, false, true, false, false};
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(flaggedRelationships, 29'999, false),
        Decision::BlockAmbiguous);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(flaggedRelationships, 30'000, false),
        Decision::RecoverAndAllow);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(flaggedRelationships, 35'000, true),
        Decision::AllowRecovered);
}

TEST(DungeonNavigatorCombatPolicy, TransientFlagOnlyAmbiguityBlocks)
{
    using DungeonNavigatorCombat::Decision;
    using DungeonNavigatorCombat::Signals;

    Signals const memberFlag{true, false, false, false, false, false, false};
    Signals const petFlag{false, true, false, false, false, false, false};
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(memberFlag, 0, false),
        Decision::BlockAmbiguous);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(memberFlag, 29'999, false),
        Decision::BlockAmbiguous);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(petFlag, 29'999, false),
        Decision::BlockAmbiguous);
}

TEST(DungeonNavigatorCombatPolicy, ProlongedFlagOnlyAmbiguityRecoversThenAllowsConvoy)
{
    using DungeonNavigatorCombat::Decision;
    using DungeonNavigatorCombat::Signals;

    Signals const stale{true, false, false, false, false, false, false};
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(stale, 30'000, false),
        Decision::RecoverAndAllow);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate(stale, 35'000, true),
        Decision::AllowRecovered);
    EXPECT_EQ(DungeonNavigatorCombat::Evaluate({}, 0, false), Decision::Allow);
}
