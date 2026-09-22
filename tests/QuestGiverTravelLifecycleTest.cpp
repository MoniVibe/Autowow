/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2.
 */

#include "../src/AutoWow/QuestGiverTravelFeedback.h"
#include "../src/AutoWow/QuestGiverTravelLifecycle.h"
#include "../src/AutoWow/QuestAcquisitionPolicy.h"
#include "../src/AutoWow/AutoWowBridge.h"
#include "../src/AutoWow/AutoWowIndependentActivityPolicy.h"
#include "../src/Bot/PlayerbotAI.h"
#include "../src/Mgr/Travel/TravelMgr.h"

#include <gtest/gtest.h>

TEST(QuestGiverTravelLifecycleTest, BridgeNativeExpiryWinsOverTravelTargetLost)
{
    AutoWowQuestAcquisitionBridgeTest::LifecycleScenarioResult const result =
        AutoWowQuestAcquisitionBridgeTest::RunNativeExpiryScenario();
    EXPECT_TRUE(result.targetInactive);
    EXPECT_TRUE(result.unloadedGiverDidNotStartStaleWait);
    EXPECT_TRUE(result.confirmedMissingGiverReachedBoundedTerminal);
    for (std::string const* json : {&result.response, &result.snapshot})
    {
        EXPECT_NE(json->find("\"state\":\"blocked\""), std::string::npos);
        EXPECT_NE(json->find("\"reason\":\"acquisition_timeout\""), std::string::npos);
        EXPECT_EQ(json->find("travel_target_lost"), std::string::npos);
        EXPECT_NE(json->find("\"movement_attempts\":27"), std::string::npos);
        EXPECT_NE(json->find("\"native_status_deadline_elapsed_ms\":"), std::string::npos);
        EXPECT_NE(json->find("\"native_aligned_deadline_at_ms\":"), std::string::npos);
    }
    EXPECT_NE(result.response.find("\"movement_kick_accepted\":false"), std::string::npos);
    EXPECT_NE(result.snapshot.find("\"latest_accepted\":false"), std::string::npos);
    EXPECT_NE(result.snapshot.find("\"target_installed\":true"), std::string::npos);
    EXPECT_NE(result.snapshot.find("\"travel_strategy\":true"), std::string::npos);
}

TEST(QuestGiverTravelLifecycleTest, UnloadedBackedGiverDoesNotBecomeConfirmedMissing)
{
    using namespace AutoWowQuestGiverTravel;
    // A loaded coarse grid is not authoritative while the spawn remains outside the player's
    // runtime object-resolution range.
    EXPECT_EQ(ClassifyGiverLookup(true, true, false, false), GiverLookupState::GridUnloaded);
    EXPECT_EQ(ClassifyGiverLookup(true, true, true, false), GiverLookupState::ConfirmedMissing);
    EXPECT_EQ(ClassifyGiverLookup(true, true, true, true), GiverLookupState::Resolved);
    EXPECT_EQ(ClassifyGiverLookup(false, true, true, false), GiverLookupState::ConfirmedMissing);

    AutoWowQuestAcquisition::PersistentGiverBindingFacts loaded{
        77123, 0, 9001, 77123, 1, 1, 3208, 3208, true, false};
    EXPECT_TRUE(AutoWowQuestAcquisition::IsMatchingPersistentGiver(loaded));
    loaded.expectedRuntimeGiverGuid = 9002;
    EXPECT_FALSE(AutoWowQuestAcquisition::IsMatchingPersistentGiver(loaded));
}

TEST(QuestGiverTravelLifecycleTest, ProductionJourneyInstallsMovesAndEmitsTerminalTelemetry)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 954;
    constexpr std::uint64_t createdAt = 1000000;

    PlayerbotAI ai;
    TravelDestination destination(1.0f, 3.0f);
    WorldPosition point(1, 100.0f, 200.0f, 30.0f, 0.0f);
    destination.addPoint(&point);
    TravelTarget target(&ai);
    target.setTarget(&destination, &point);

    QuestAcquisitionJourneyRuntime journey(createdAt, 3878.58, 7.0f);
    ASSERT_TRUE(journey.InstallTarget(target, createdAt + 10));
    EXPECT_EQ(journey.NativeDeadlineAtMs(), journey.Deadline().DeadlineAtMs());
    EXPECT_GE(target.getTimeLeft() + 1U, journey.Deadline().RemainingMs(createdAt + 10));
    EXPECT_FALSE(journey.InstallTarget(target, createdAt + 20)); // immutable one-shot binding

    ClearMovementFeedback(botGuid);
    for (std::uint32_t segment = 0; segment < 27; ++segment)
    {
        RecordMovementFeedback(botGuid, StepKind::TravelMgrSegment,
                               MovementFeedbackResult::Accepted, true,
                               3878.58f - static_cast<float>(segment + 1) * 140.0f);
        MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
        ASSERT_NE(feedback, nullptr);
        journey.ApplyStagedMoverOutcome(*feedback);
        EXPECT_FALSE(journey.IsTerminal());
    }
    EXPECT_EQ(journey.MovementAttempts(), 27U);

    EXPECT_FALSE(journey.ExpireIfDue(journey.Deadline().DeadlineAtMs() - 1));
    EXPECT_TRUE(journey.ExpireIfDue(journey.Deadline().DeadlineAtMs()));
    std::string const response = journey.TelemetryJson("response");
    std::string const snapshot = journey.TelemetryJson("snapshot");
    EXPECT_NE(response.find("\"state\":\"blocked\""), std::string::npos);
    EXPECT_NE(response.find("\"reason\":\"acquisition_timeout\""), std::string::npos);
    EXPECT_NE(response.find("\"movement_attempts\":27"), std::string::npos);
    EXPECT_NE(response.find("\"movement_kick_accepted\":false"), std::string::npos);
    EXPECT_NE(response.find("\"movement_action_result\":\"not_attempted\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"surface\":\"snapshot\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"state\":\"blocked\""), std::string::npos);
    ClearMovementFeedback(botGuid);
}

TEST(QuestGiverTravelLifecycleTest, SpeedChangesCannotMoveInstalledOrLogicalDeadline)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint64_t createdAt = 2000000;
    constexpr double distance = 3878.58;

    PlayerbotAI ai;
    TravelDestination destination(1.0f, 3.0f);
    WorldPosition point(1, 100.0f, 200.0f, 30.0f, 0.0f);
    destination.addPoint(&point);
    TravelTarget target(&ai);
    target.setTarget(&destination, &point);
    QuestAcquisitionJourneyRuntime journey(createdAt, distance, 7.0f);
    ASSERT_TRUE(journey.InstallTarget(target, createdAt));

    std::uint64_t const logicalDeadline = journey.Deadline().DeadlineAtMs();
    std::uint32_t const nativeDeadline = target.getStatusDeadline();
    // These are the deadlines a later mount or slow would have produced under the old polling-time
    // recomputation. They are deliberately never applied to the already-created journey.
    EXPECT_NE(AcquisitionDeadlineMs(distance, 14.0f), journey.Deadline().DurationMs());
    EXPECT_NE(AcquisitionDeadlineMs(distance, 3.5f), journey.Deadline().DurationMs());
    EXPECT_EQ(journey.Deadline().DeadlineAtMs(), logicalDeadline);
    EXPECT_EQ(target.getStatusDeadline(), nativeDeadline);
    EXPECT_FALSE(journey.ExpireIfDue(logicalDeadline - 1));
    EXPECT_EQ(target.getStatusDeadline(), nativeDeadline);
}

TEST(QuestGiverTravelLifecycleTest, ParkedLeaderStaleGiverAndPollingRemainFailClosed)
{
    using namespace AutoWowQuestGiverTravel;
    SessionLifecycle lifecycle;
    SessionDecision parked = lifecycle.Observe({1000, true, true, 2, 1, true});
    EXPECT_EQ(parked.state, SessionMovementState::LeaderParkedWaitingFollowers);
    EXPECT_FALSE(parked.moveLeader);
    EXPECT_FALSE(parked.issueAcceptance);
    EXPECT_EQ(lifecycle.PartyArrivalWaitMs(4000), 3000U);

    SessionDecision ready = lifecycle.Observe({4000, true, true, 2, 2, false});
    EXPECT_EQ(ready.state, SessionMovementState::CohesiveAtGiver);
    EXPECT_FALSE(ready.issueAcceptance);
    EXPECT_FALSE(ready.replaceTarget);
    for (std::uint32_t poll = 0; poll < 32; ++poll)
    {
        SessionDecision observed = lifecycle.Observe({4001 + poll, true, true, 2, 2, false});
        EXPECT_FALSE(observed.issueAcceptance);
        EXPECT_FALSE(observed.replaceTarget);
    }

    SessionDecision stale = lifecycle.Observe({5000, false, false, 2, 0, false});
    EXPECT_EQ(stale.state, SessionMovementState::StaleGiver);
    LifecycleFacts facts;
    facts.staleGiverWaitMs = lifecycle.StaleGiverWaitMs(5000 + kMaxStaleGiverWaitMs);
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedStaleGiver);
}

TEST(QuestGiverTravelLifecycleTest, NativeTickOwnsEligibleTargetWithoutBridgePoll)
{
    using namespace AutoWowQuestAcquisitionBridge;
    TickFacts facts;
    facts.hasPendingSession = true;
    facts.botAlive = true;
    facts.canMove = true;
    facts.travelActivityAllowed = true;
    facts.detailedMoveAllowed = true;
    facts.targetInstalled = true;
    facts.targetActive = true;
    facts.cadenceDue = true;

    // The native contract is directly evaluable from an AI tick; no bridge-poll fact or call is
    // required to grant ownership or authorize the first staged dispatch.
    EXPECT_EQ(DecideTick(facts), TickResult::Owned);
    EXPECT_TRUE(ShouldDispatchStagedMover(facts));
}

TEST(QuestGiverTravelLifecycleTest, NativeTickCadencePreventsDuplicateDispatch)
{
    using namespace AutoWowQuestAcquisitionBridge;
    EXPECT_EQ(SaturatingMonotonicDeltaMs(1000, 1000), 0U);
    EXPECT_EQ(SaturatingMonotonicDeltaMs(2000, 1000), 0U);
    EXPECT_EQ(SaturatingMonotonicDeltaMs(1000, 2125), 1125U);
    EXPECT_EQ(SaturatingMonotonicDeltaMs(
                  1, 1 + static_cast<uint64>(std::numeric_limits<uint32>::max()) + 1),
              std::numeric_limits<uint32>::max());

    TickCadence cadence;
    EXPECT_TRUE(cadence.IsDue());
    cadence.Arm();
    EXPECT_FALSE(cadence.IsDue());
    cadence.Advance(kNativeTickCadenceMs - 1);
    EXPECT_FALSE(cadence.IsDue());
    cadence.Advance(1);
    EXPECT_TRUE(cadence.IsDue());

    TickFacts facts;
    facts.hasPendingSession = true;
    facts.botAlive = true;
    facts.canMove = true;
    facts.travelActivityAllowed = true;
    facts.detailedMoveAllowed = true;
    facts.targetInstalled = true;
    facts.targetActive = true;
    facts.cadenceDue = true;
    facts.movementInFlight = true;
    EXPECT_EQ(DecideTick(facts), TickResult::Owned);
    EXPECT_FALSE(ShouldDispatchStagedMover(facts));

    // A prior accepted feedback record may still report moving, but it is intentionally absent
    // from TickFacts authority. Once live motion stops and cadence is due, dispatch resumes.
    facts.movementInFlight = false;
    EXPECT_TRUE(ShouldDispatchStagedMover(facts));
}

TEST(QuestGiverTravelLifecycleTest, NativeTickRelinquishesToCampaignTravelAndWorkHandoff)
{
    using namespace AutoWowQuestAcquisitionBridge;
    TickFacts facts;
    facts.hasPendingSession = true;
    facts.botAlive = true;
    facts.canMove = true;
    facts.travelActivityAllowed = true;
    facts.detailedMoveAllowed = true;
    facts.targetInstalled = true;
    facts.targetActive = true;
    facts.cadenceDue = true;

    facts.campaignTravelOwned = true;
    EXPECT_EQ(DecideTick(facts), TickResult::NoWork);

    facts.campaignTravelOwned = false;
    facts.targetWorkReady = true;
    EXPECT_EQ(DecideTick(facts), TickResult::NoWork);
}

TEST(QuestGiverTravelLifecycleTest, NativeTickGatesUnsafeMovementAndCleansTerminalOnce)
{
    using namespace AutoWowQuestAcquisitionBridge;
    TickFacts facts;
    facts.hasPendingSession = true;
    facts.botAlive = true;
    facts.canMove = true;
    facts.travelActivityAllowed = true;
    facts.detailedMoveAllowed = true;
    facts.targetInstalled = true;
    facts.targetActive = true;
    facts.cadenceDue = true;

    for (bool* gate : {&facts.botPaused, &facts.inCombat, &facts.inFlight,
                       &facts.beingTeleported})
    {
        *gate = true;
        EXPECT_EQ(DecideTick(facts), TickResult::NoWork);
        *gate = false;
    }
    facts.botAlive = false;
    EXPECT_EQ(DecideTick(facts), TickResult::NoWork);
    facts.botAlive = true;
    facts.canMove = false;
    EXPECT_EQ(DecideTick(facts), TickResult::NoWork);

    TickFacts terminal;
    terminal.hasPendingSession = true;
    terminal.sessionTerminal = true;
    terminal.terminalCleanupNeeded = true;
    EXPECT_EQ(DecideTick(terminal), TickResult::Terminal);
    terminal.terminalCleanupNeeded = false;
    EXPECT_EQ(DecideTick(terminal), TickResult::NoWork);
}

TEST(QuestGiverTravelLifecycleTest, SafeIncompleteFrontierCountsAsBoundedMovementProgress)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 0xA4402U;
    ClearMovementFeedback(botGuid);

    TravelMgrReanchorCandidateFacts candidate;
    candidate.routeIndex = 4U;
    candidate.nodeKind = TravelMgrRouteNodeKind::Walk;
    candidate.point = {1, 145.0f, 20.0f, 0.0f};
    candidate.sameMap = true;
    candidate.directDistance = 145.0f;
    candidate.targetDistanceReduction = 12.0f;
    candidate.freshProbe = {true,
                            true,
                            true,
                            PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                            3,
                            4.0f,
                            70.0f,
                            true,
                            false};

    EXPECT_TRUE(IsSafeIncompleteTravelMgrProbe(candidate.freshProbe));
    EXPECT_TRUE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));

    QuestAcquisitionJourneyRuntime journey(1000, 386.181, 7.0f);
    RecordMovementFeedback(botGuid, StepKind::TravelMgrSegment,
                           MovementFeedbackResult::Accepted, true, 240.0f);
    MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    journey.ApplyStagedMoverOutcome(*feedback);

    EXPECT_EQ(feedback->movementAttempts, 1U);
    EXPECT_EQ(feedback->rejectedAttempts, 0U);
    EXPECT_EQ(feedback->noProgressChecks, 0U);
    EXPECT_TRUE(journey.Lifecycle().MovementAccepted());
    EXPECT_EQ(EvaluateLifecycle({0, kBaseAcquisitionLifetimeMs, 1, 0, 0, 0, 0}),
              LifecycleDecision::Continue);
    ClearMovementFeedback(botGuid);
}

TEST(QuestGiverTravelLifecycleTest, IndependentAutoWowActivityKeepsSoloBotResponsive)
{
    using namespace AutoWowIndependentActivityPolicy;
    EXPECT_TRUE(ShouldForceActivity(true, false));
    EXPECT_FALSE(ShouldForceActivity(false, false));
    EXPECT_FALSE(ShouldForceActivity(true, true));
    EXPECT_TRUE(ShouldUseFastReaction(true, false, false, false));
    EXPECT_FALSE(ShouldUseFastReaction(false, false, false, false));
    EXPECT_FALSE(ShouldUseFastReaction(true, true, false, false));
    EXPECT_FALSE(ShouldUseFastReaction(true, false, true, false));
    EXPECT_FALSE(ShouldUseFastReaction(true, false, false, true));
}

TEST(QuestGiverTravelLifecycleTest, IndependentNonCombatReactionHasShortBoundedDelay)
{
    using namespace AutoWowIndependentActivityPolicy;
    EXPECT_EQ(NonCombatReactDelay(100), 100U);
    EXPECT_EQ(NonCombatReactDelay(200), 200U);
    EXPECT_EQ(NonCombatReactDelay(1000), 250U);
    EXPECT_EQ(NonCombatReactDelay(0), 100U);
}
