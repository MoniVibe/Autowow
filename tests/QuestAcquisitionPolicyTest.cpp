/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "gtest/gtest.h"

#include "../src/AutoWow/QuestAcquisitionPolicy.h"
#include "../src/AutoWow/QuestGiverTravelPolicy.h"
#include "../src/AutoWow/QuestGiverTravelLifecycle.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string BridgeSource()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/AutoWowBridge.cpp");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string AcquireBody()
{
    std::string const source = BridgeSource();
    std::size_t const start = source.find("bool AcquireQuestParty()");
    std::size_t const end = source.find("bool QuestParty()", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}

std::string MovementCaptureBody()
{
    std::string const source = BridgeSource();
    std::size_t const start = source.find("void CaptureQuestAcquisitionMovement(");
    std::size_t const end = source.find(
        "AutoWowQuestAcquisition::MovementOrderFacts PassiveQuestAcquisitionOrderFacts", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}

AutoWowQuestAcquisition::Candidate SafeCandidate(std::size_t sourceIndex, uint32_t questId,
                                                  int32_t giverEntry, double distance)
{
    return {sourceIndex, questId, giverEntry, 1, 10.0, 20.0, 30.0, distance,
            false, true, true, true, 9001};
}

AutoWowQuestAcquisition::MovementDiagnosticsFacts ReadyMovementFacts()
{
    using namespace AutoWowQuestAcquisition;
    MovementDiagnosticsFacts facts;
    facts.order = {true, true, true, true, true};
    facts.canMove = true;
    facts.canMoveAround = true;
    facts.travelActivityAllowed = true;
    facts.detailedMoveAllowed = true;
    return facts;
}
}

TEST(QuestAcquisitionParserTest, AcceptsOnlyExactAcquireGrammar)
{
    using namespace AutoWowQuestAcquisition;
    WireRequest request;
    std::string error;
    ASSERT_TRUE(ParseWireRequest("quest 7 acquire", request, error));
    EXPECT_EQ(request.mode, WireMode::Acquire);
    EXPECT_EQ(request.leaderGuid, 7u);
    EXPECT_EQ(request.questId, 0u);

    ASSERT_TRUE(ParseWireRequest("QUEST 10 ACQUIRE", request, error));
    EXPECT_EQ(request.mode, WireMode::Acquire);
    EXPECT_EQ(request.leaderGuid, 10u);
}

TEST(QuestAcquisitionPolicyTest, EventBoundSpawnEligibilityTracksSignedBindings)
{
    using AutoWowQuestAcquisition::IsEventBoundSpawnEligible;
    std::vector<std::uint16_t> const active{12, 51};

    EXPECT_TRUE(IsEventBoundSpawnEligible({}, active));
    EXPECT_FALSE(IsEventBoundSpawnEligible({50}, active));
    EXPECT_TRUE(IsEventBoundSpawnEligible({51}, active));
    EXPECT_TRUE(IsEventBoundSpawnEligible({-50}, active));
    EXPECT_FALSE(IsEventBoundSpawnEligible({-51}, active));
    EXPECT_TRUE(IsEventBoundSpawnEligible({50, -49}, active));
    EXPECT_FALSE(IsEventBoundSpawnEligible({50, -51}, active));
}

TEST(QuestAcquisitionParserTest, PreservesExactQuestAndLegacyAutomaticForms)
{
    using namespace AutoWowQuestAcquisition;
    WireRequest request;
    std::string error;
    ASSERT_TRUE(ParseWireRequest("quest 7 3521", request, error));
    EXPECT_EQ(request.mode, WireMode::ExplicitQuest);
    EXPECT_EQ(request.questId, 3521u);

    ASSERT_TRUE(ParseWireRequest("quest 7", request, error));
    EXPECT_EQ(request.mode, WireMode::Automatic);
}

TEST(QuestAcquisitionParserTest, RejectsAmbiguousMissingAndExtraTokens)
{
    using namespace AutoWowQuestAcquisition;
    for (std::string const& text : {
             "quest", "quest 0 acquire", "quest seven acquire", "quest 7 acquire extra",
             "quest 7 3521 extra", "quest 7 acquire3521", "quests 7 acquire"})
    {
        WireRequest request;
        std::string error;
        EXPECT_FALSE(ParseWireRequest(text, request, error)) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(QuestAcquisitionSelectionTest, SkipsActiveBackedQuestShapesAndUnsafeCandidates)
{
    using namespace AutoWowQuestAcquisition;
    Candidate active = SafeCandidate(0, 3521, 100, 1.0);
    active.activeQuest = true;
    Candidate unsupported = SafeCandidate(1, 10, 101, 2.0);
    unsupported.supported = false;
    Candidate ineligible = SafeCandidate(2, 11, 102, 3.0);
    ineligible.coreEligible = false;
    Candidate unsuitable = SafeCandidate(3, 12, 103, 4.0);
    unsuitable.partySuitable = false;
    Candidate selected = SafeCandidate(4, 13, 104, 5.0);

    std::vector<Candidate> candidates{active, unsupported, ineligible, unsuitable, selected};
    auto const result = SelectCandidate(candidates);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(candidates[*result].questId, 13u);
}

TEST(QuestAcquisitionSelectionTest, RanksNearestThenStableQuestGiverAndCoordinates)
{
    using namespace AutoWowQuestAcquisition;
    Candidate far = SafeCandidate(0, 1, 100, 20.0);
    Candidate tieHighQuest = SafeCandidate(1, 20, 100, 10.0);
    Candidate tieLowQuest = SafeCandidate(2, 10, 100, 10.0);
    Candidate exactTieLaterSource = tieLowQuest;
    exactTieLaterSource.sourceIndex = 3;

    std::vector<Candidate> candidates{far, tieHighQuest, exactTieLaterSource, tieLowQuest};
    auto const result = SelectCandidate(candidates);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(candidates[*result].questId, 10u);
    EXPECT_EQ(candidates[*result].sourceIndex, 2u);
}

TEST(QuestAcquisitionSelectionTest, RanksReachableCrossMapCandidatesByRouteDistance)
{
    using namespace AutoWowQuestAcquisition;
    Candidate local = SafeCandidate(0, 20, 100, 900.0);
    local.mapId = 1;
    Candidate crossMap = SafeCandidate(1, 10, 101, 700.0);
    crossMap.mapId = 530;

    std::vector<Candidate> candidates{local, crossMap};
    auto const result = SelectCandidate(candidates);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(candidates[*result].questId, 10u);
    EXPECT_EQ(candidates[*result].mapId, 530u);
}

TEST(QuestAcquisitionSelectionTest, ReturnsNoCandidateWhenEveryCandidateIsFiltered)
{
    using namespace AutoWowQuestAcquisition;
    Candidate candidate = SafeCandidate(0, 1, 100, 1.0);
    candidate.supported = false;
    EXPECT_FALSE(SelectCandidate({candidate}).has_value());
}

TEST(QuestAcquisitionSelectionTest, SelectsAuthoritativePersistentSpawnWithoutLiveBinding)
{
    using namespace AutoWowQuestAcquisition;
    Candidate candidate = SafeCandidate(0, 827, 3208, 386.181);
    candidate.liveGiverGuid = 0;
    candidate.persistentSpawnGuid = 77123;

    EXPECT_TRUE(IsEligible(candidate));
    std::optional<std::size_t> const selected = SelectCandidate({candidate});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 0u);
}

TEST(QuestAcquisitionSelectionTest, RejectsCandidateWithoutLiveGiverBinding)
{
    using namespace AutoWowQuestAcquisition;
    Candidate candidate = SafeCandidate(0, 1, 100, 1.0);
    candidate.liveGiverGuid = 0;
    EXPECT_FALSE(IsEligible(candidate));
    EXPECT_FALSE(SelectCandidate({candidate}).has_value());
}

TEST(QuestAcquisitionGiverResolutionTest, RequiresExactLiveQuestGiverFacts)
{
    using namespace AutoWowQuestAcquisition;
    LiveGiverFacts resolved{9001, 15732, 15732, true, false, true, true, true};
    EXPECT_EQ(ClassifyGiverResolution(resolved), GiverResolutionFailure::None);
    EXPECT_TRUE(IsEligibleLiveGiver(resolved));
    EXPECT_EQ(GiverResolutionFailureName(GiverResolutionFailure::None), "resolved");

    LiveGiverFacts resolvedGameObject{9002, -15895, 15895, false, true, true, true, true};
    EXPECT_EQ(ClassifyGiverResolution(resolvedGameObject), GiverResolutionFailure::None);
    EXPECT_TRUE(IsEligibleLiveGiver(resolvedGameObject));

    LiveGiverFacts ambiguousType = resolved;
    ambiguousType.isGameObject = true;
    EXPECT_EQ(ClassifyGiverResolution(ambiguousType), GiverResolutionFailure::TypeOrEntryMismatch);

    LiveGiverFacts missing{};
    EXPECT_EQ(ClassifyGiverResolution(missing), GiverResolutionFailure::Missing);

    LiveGiverFacts wrongEntry = resolved;
    wrongEntry.liveEntry = 15895;
    EXPECT_EQ(ClassifyGiverResolution(wrongEntry), GiverResolutionFailure::TypeOrEntryMismatch);

    LiveGiverFacts unloaded = resolved;
    unloaded.inWorld = false;
    EXPECT_EQ(ClassifyGiverResolution(unloaded), GiverResolutionFailure::NotInWorld);

    LiveGiverFacts dead = resolved;
    dead.alive = false;
    EXPECT_EQ(ClassifyGiverResolution(dead), GiverResolutionFailure::Dead);

    LiveGiverFacts noQuest = resolved;
    noQuest.offersQuest = false;
    EXPECT_EQ(ClassifyGiverResolution(noQuest), GiverResolutionFailure::QuestUnavailable);
}

TEST(QuestAcquisitionGiverResolutionTest, BindsOnlyTheExactPersistentSpawnRuntimeIdentity)
{
    using namespace AutoWowQuestAcquisition;
    PersistentGiverBindingFacts unresolvedAfterLoad{
        77123, 0, 9001, 77123, 1, 1, 3208, 3208, true, false};
    EXPECT_TRUE(IsMatchingPersistentGiver(unresolvedAfterLoad));

    PersistentGiverBindingFacts wrongRuntime = unresolvedAfterLoad;
    wrongRuntime.expectedRuntimeGiverGuid = 9002;
    EXPECT_FALSE(IsMatchingPersistentGiver(wrongRuntime));

    PersistentGiverBindingFacts wrongMap = unresolvedAfterLoad;
    wrongMap.runtimeMapId = 530;
    EXPECT_FALSE(IsMatchingPersistentGiver(wrongMap));
}

TEST(QuestAcquisitionGiverResolutionTest, RejectsDuplicateSameEntryMapWithDifferentSpawn)
{
    using namespace AutoWowQuestAcquisition;
    PersistentGiverBindingFacts duplicateSameEntryMap{
        77123, 0, 9002, 77124, 1, 1, 3208, 3208, true, false};
    EXPECT_FALSE(IsMatchingPersistentGiver(duplicateSameEntryMap));
}

TEST(QuestAcquisitionMovementTest, DoesNotTreatInactiveTravelActionAsTraveling)
{
    using namespace AutoWowQuestAcquisition;
    MovementOrderFacts facts{true, true, true, true, false};
    EXPECT_FALSE(IsMovementOrderActive(facts));
    EXPECT_EQ(ClassifyMovementActivation(facts), MovementActivationFailure::ActionNotActivated);
    EXPECT_EQ(MovementActivationFailureName(MovementActivationFailure::ActionNotActivated),
              "action_not_activated");
}

TEST(QuestAcquisitionMovementTest, RequiresEveryMovementInvariantBeforeReportingTravel)
{
    using namespace AutoWowQuestAcquisition;
    MovementOrderFacts facts{true, true, true, true, true};
    ASSERT_TRUE(IsMovementOrderActive(facts));

    for (bool* fact : {&facts.targetInstalled, &facts.travelStatus, &facts.travelStrategy,
                       &facts.actionActivated})
    {
        *fact = false;
        EXPECT_FALSE(IsMovementOrderActive(facts));
        *fact = true;
    }
}

TEST(QuestAcquisitionMovementTest, ReportsTheFirstMissingActivationInvariant)
{
    using namespace AutoWowQuestAcquisition;
    MovementOrderFacts facts{true, true, true, true, true};
    for (auto const expected : {MovementActivationFailure::TargetNotInstalled,
                                MovementActivationFailure::TravelStatusNotActive,
                                MovementActivationFailure::TravelStrategyMissing,
                                MovementActivationFailure::ActionNotActivated})
    {
        facts = {true, true, true, true, true};
        switch (expected)
        {
            case MovementActivationFailure::TargetNotInstalled: facts.targetInstalled = false; break;
            case MovementActivationFailure::TravelStatusNotActive: facts.travelStatus = false; break;
            case MovementActivationFailure::TravelStrategyMissing: facts.travelStrategy = false; break;
            case MovementActivationFailure::ActionNotActivated: facts.actionActivated = false; break;
            default: break;
        }
        EXPECT_EQ(ClassifyMovementActivation(facts), expected);
    }
}

TEST(QuestAcquisitionMovementDiagnosticsTest, PreservesAllActionResultCategories)
{
    using namespace AutoWowQuestAcquisition;
    EXPECT_EQ(MovementActionResultName(MovementActionResult::NotAttempted), "not_attempted");
    EXPECT_EQ(MovementActionResultName(MovementActionResult::Accepted), "accepted");
    EXPECT_EQ(MovementActionResultName(MovementActionResult::Rejected), "rejected");
}

TEST(QuestAcquisitionMovementDiagnosticsTest, ClassifiesConcreteActionGates)
{
    using namespace AutoWowQuestAcquisition;
    using GateSetter = void (*)(MovementDiagnosticsFacts&);
    std::array<GateSetter, 8> const gates = {
             [](MovementDiagnosticsFacts& facts) { facts.inCombat = true; },
             [](MovementDiagnosticsFacts& facts) { facts.canMove = false; },
             [](MovementDiagnosticsFacts& facts) { facts.canMoveAround = false; },
             [](MovementDiagnosticsFacts& facts) { facts.travelActivityAllowed = false; },
             [](MovementDiagnosticsFacts& facts) { facts.inFlight = true; },
             [](MovementDiagnosticsFacts& facts) { facts.controlled = true; },
             [](MovementDiagnosticsFacts& facts) { facts.beingTeleported = true; },
             [](MovementDiagnosticsFacts& facts) { facts.canLoot = true; },
         };
    for (GateSetter const setGate : gates)
    {
        MovementDiagnosticsFacts facts = ReadyMovementFacts();
        facts.actionResult = MovementActionResult::Rejected;
        setGate(facts);
        EXPECT_EQ(ClassifyMovementDiagnostics(facts), MovementDiagnosticCategory::ActionGate);
        EXPECT_EQ(MovementDiagnosticCategoryName(ClassifyMovementDiagnostics(facts)), "action_gate");
    }
}

TEST(QuestAcquisitionMovementDiagnosticsTest, SeparatesInactiveTargetFromAnUnattemptedAction)
{
    using namespace AutoWowQuestAcquisition;
    MovementDiagnosticsFacts inactive = ReadyMovementFacts();
    inactive.order.targetInstalled = false;
    inactive.actionResult = MovementActionResult::NotAttempted;
    EXPECT_EQ(ClassifyMovementDiagnostics(inactive), MovementDiagnosticCategory::TargetNotActive);

    MovementDiagnosticsFacts active = ReadyMovementFacts();
    active.order = {true, true, true, true, true};
    EXPECT_EQ(ClassifyMovementDiagnostics(active), MovementDiagnosticCategory::NotAttempted);
    EXPECT_EQ(MovementDiagnosticCategoryName(ClassifyMovementDiagnostics(active)), "not_attempted");
}

TEST(QuestAcquisitionMovementDiagnosticsTest, ReportsUnsafePathTypeAsPathOrGround)
{
    using namespace AutoWowQuestAcquisition;
    MovementDiagnosticsFacts facts = ReadyMovementFacts();
    facts.actionResult = MovementActionResult::Rejected;
    facts.pathProbe = {true, false, false, 4, 6, 1004.006, 0.0};

    EXPECT_EQ(ClassifyPathProbe(facts.pathProbe), PathProbeDisposition::Unsafe);
    EXPECT_FALSE(IsPathEndpointComplete(facts.pathProbe));
    EXPECT_EQ(ClassifyMovementDiagnostics(facts), MovementDiagnosticCategory::PathOrGround);
    EXPECT_EQ(PathProbeDispositionName(ClassifyPathProbe(facts.pathProbe)), "unsafe");
}

TEST(QuestAcquisitionMovementDiagnosticsTest, RejectsSafeButEndpointIncompleteNormalHybrid)
{
    using namespace AutoWowQuestAcquisition;
    MovementDiagnosticsFacts facts = ReadyMovementFacts();
    facts.actionResult = MovementActionResult::Rejected;
    // q827/Margoz evidence: PATHFIND_NORMAL|PATHFIND_INCOMPLETE (5), a short partial path,
    // and a final point still 386 yards from the requested giver.
    facts.pathProbe = {true, true, true, 5, 6, 386.181, 20.026};

    EXPECT_EQ(ClassifyPathProbe(facts.pathProbe), PathProbeDisposition::Incomplete);
    EXPECT_FALSE(IsPathEndpointComplete(facts.pathProbe));
    EXPECT_EQ(ClassifyMovementDiagnostics(facts), MovementDiagnosticCategory::PathOrGround);
    EXPECT_EQ(PathProbeDispositionName(ClassifyPathProbe(facts.pathProbe)), "incomplete");
}

TEST(QuestAcquisitionMovementDiagnosticsTest, KeepsSafeCompletePathAsPendingOrdinaryTravel)
{
    using namespace AutoWowQuestAcquisition;
    MovementDiagnosticsFacts facts = ReadyMovementFacts();
    facts.actionResult = MovementActionResult::Rejected;
    facts.pathProbe = {true, true, true, 1, 6, 0.5, 386.0};

    EXPECT_EQ(ClassifyPathProbe(facts.pathProbe), PathProbeDisposition::Complete);
    EXPECT_TRUE(IsPathEndpointComplete(facts.pathProbe));
    EXPECT_EQ(ClassifyMovementDiagnostics(facts), MovementDiagnosticCategory::PendingOrdinaryTravel);
    EXPECT_EQ(MovementDiagnosticCategoryName(ClassifyMovementDiagnostics(facts)),
              "pending_ordinary_travel");
}

TEST(QuestAcquisitionMovementDiagnosticsTest, CaptureSurfaceHasNoBehaviorMutation)
{
    std::string const captureBody = MovementCaptureBody();
    ASSERT_FALSE(captureBody.empty());
    EXPECT_NE(captureBody.find("AutoWowDungeonPath::Probe"), std::string::npos);
    EXPECT_EQ(captureBody.find("DoSpecificAction"), std::string::npos);
    EXPECT_EQ(captureBody.find("setTarget("), std::string::npos);
    EXPECT_EQ(captureBody.find("setNullTravelTarget"), std::string::npos);
    EXPECT_EQ(captureBody.find("TeleportTo("), std::string::npos);
}

TEST(QuestAcquisitionLifecycleTest, LongRouteUsesElapsedDeadlineAndFailureOnlyBudgets)
{
    using namespace AutoWowQuestGiverTravel;

    LifecycleFacts facts;
    facts.routeDeadlineMs = AcquisitionDeadlineMs(3878.58, 7.0f);
    facts.movementAttempts = 26;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::Continue);

    facts.movementRejects = kMaxMovementRejects;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedMovement);
}

TEST(QuestAcquisitionLifecycleTest, TargetExpiryAndWaitStatesHaveExplicitTerminalContracts)
{
    using namespace AutoWowQuestGiverTravel;

    LifecycleFacts facts;
    facts.staleGiverWaitMs = kMaxStaleGiverWaitMs;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedStaleGiver);

    facts = {};
    facts.partyArrivalWaitMs = kMaxPartyArrivalWaitMs;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedPartyArrival);
}

TEST(QuestAcquisitionContractTest, ForcedTargetBypassIsRemovedAndLiveLifecycleIsPresent)
{
    std::string const body = AcquireBody();
    std::string const source = BridgeSource();
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(body.find("setForced(true)"), std::string::npos);
    EXPECT_NE(body.find("ClearMovementFeedback"), std::string::npos);
    EXPECT_NE(body.find("ApplyLiveMovementFeedback"), std::string::npos);
    EXPECT_NE(source.find("movement_feedback"), std::string::npos);
    EXPECT_NE(body.find("expectedPartyMembers"), std::string::npos);
    EXPECT_NE(source.find("Poll(bot, false)"), std::string::npos);
    EXPECT_NE(source.find("travel_target_lost"), std::string::npos);
    EXPECT_EQ(AutoWowQuestGiverTravel::LifecycleDecisionName(
                  AutoWowQuestGiverTravel::LifecycleDecision::BlockedPartyArrival),
              "party_arrival_timeout");
    EXPECT_NE(source.find("stale_giver"), std::string::npos);
    EXPECT_NE(source.find("partial_party_acceptance"), std::string::npos);
    EXPECT_NE(source.find("leader_parked_waiting_followers"), std::string::npos);
    EXPECT_NE(source.find("movement_state"), std::string::npos);
    EXPECT_NE(source.find("ClearTerminalMovementFeedback"), std::string::npos);
}

TEST(QuestAcquisitionContractTest, GiverIsRevalidatedWhileMovingAndTerminalTelemetryIsCleared)
{
    using namespace AutoWowQuestGiverTravel;
    EXPECT_EQ(ClassifyGiverLookup(true, false, false), GiverLookupState::GridUnloaded);
    EXPECT_EQ(ClassifyGiverLookup(true, true, false), GiverLookupState::ConfirmedMissing);
    LifecycleFacts facts;
    facts.staleGiverWaitMs = kMaxStaleGiverWaitMs - 1U;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::Continue);
    facts.staleGiverWaitMs = kMaxStaleGiverWaitMs;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedStaleGiver);
}

TEST(QuestAcquisitionContractTest, ObservationalPollGuardsQuestOpcodeAndTargetMutation)
{
    using namespace AutoWowQuestGiverTravel;
    SessionLifecycle lifecycle;
    SessionDecision observed = lifecycle.Observe({1000, true, true, 2, 2, false});
    EXPECT_EQ(observed.state, SessionMovementState::CohesiveAtGiver);
    EXPECT_FALSE(observed.issueAcceptance);
    EXPECT_FALSE(observed.replaceTarget);
    SessionDecision active = lifecycle.Observe({1001, true, true, 2, 2, true});
    EXPECT_TRUE(active.issueAcceptance);
    EXPECT_FALSE(active.replaceTarget);
}

TEST(QuestAcquisitionContractTest, PartyArrivalIsElapsedAndLeaderParkingIsObservable)
{
    std::string const source = BridgeSource();
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("journey.Lifecycle().ObserveParty"), std::string::npos);
    EXPECT_NE(source.find("PartyArrivalWaitMs()"), std::string::npos);
    EXPECT_NE(source.find("partyCohesionReady = true"), std::string::npos);
    EXPECT_NE(source.find("partyCohesionReady && TravelDestination::isIn"), std::string::npos);
    EXPECT_NE(source.find("leader_parked_waiting_followers"), std::string::npos);
}

TEST(QuestAcquisitionContractTest, BridgeUsesExactPersistentTravelAndNormalOpcode)
{
    std::string const source = BridgeSource();
    std::string const body = AcquireBody();
    ASSERT_FALSE(body.empty());
    EXPECT_NE(source.find("AutoWowQuestAcquisition::ParseWireRequest(requestText"), std::string::npos);
    EXPECT_NE(source.find("AutoWowRequestType::QuestAcquire"), std::string::npos);
    EXPECT_NE(body.find("getGlobalValue<questGuidpMap>(\"quest guidp map\")"), std::string::npos);
    EXPECT_NE(body.find("HasQuestRelationFlag(relationMask, QuestRelationFlag::questGiver)"), std::string::npos);
    EXPECT_NE(body.find("ClassifyCapability"), std::string::npos);
    EXPECT_NE(body.find("bool const gridLoaded = leaderMap && leaderMap->IsGridLoaded"), std::string::npos);
    EXPECT_NE(body.find("liveGiver = gridLoaded ? liveSpawn.GetWorldObject() : nullptr"), std::string::npos);
    EXPECT_NE(body.find("if (persistentSpawn && !gridLoaded)"), std::string::npos);
    EXPECT_NE(body.find("persistentSpawnGuid"), std::string::npos);
    EXPECT_NE(source.find("GetResolvedGiverSpawnId(giver)"), std::string::npos);
    EXPECT_NE(source.find("GetResolvedGiverSpawnId(chosenGiver)"), std::string::npos);
    EXPECT_NE(source.find("return creature->GetSpawnId();"), std::string::npos);
    EXPECT_NE(source.find("return gameObject->GetSpawnId();"), std::string::npos);
    EXPECT_NE(body.find("ClassifyGiverResolution"), std::string::npos);
    EXPECT_NE(body.find("liveGiverGuid"), std::string::npos);
    EXPECT_NE(body.find("giver_resolution_failed"), std::string::npos);
    EXPECT_NE(body.find("AutoWowQuestAcquisition::SelectCandidate"), std::string::npos);
    EXPECT_NE(body.find("EnableLeagueNoTeleport(group)"), std::string::npos);
    EXPECT_NE(body.find("+travel,-grind,-move random,-new rpg,-follow"), std::string::npos);
    EXPECT_NE(body.find("+follow,-grind,-move random,-new rpg,-travel"), std::string::npos);
    EXPECT_NE(body.find("ExecuteQuestGiverStagedEntry(travelTarget)"), std::string::npos);
    EXPECT_EQ(body.find("DoSpecificAction(\"move to travel target\", Event(), true)"), std::string::npos);
    EXPECT_NE(body.find("bool const actionActivated"), std::string::npos);
    EXPECT_NE(body.find("travelTarget->isTraveling()"), std::string::npos);
    EXPECT_NE(body.find("movementKickAccepted"), std::string::npos);
    EXPECT_NE(body.find("targetInstalled, travelStatus, forced, travelStrategy, actionActivated"),
              std::string::npos);
    EXPECT_NE(body.find("ClassifyMovementActivation"), std::string::npos);
    EXPECT_NE(body.find("movementActivation"), std::string::npos);
    EXPECT_NE(body.find("IsMovementOrderActive"), std::string::npos);
    EXPECT_NE(body.find("movement_order_not_active"), std::string::npos);
    EXPECT_NE(source.find("AutoWowDungeonPath::Probe"), std::string::npos);
    EXPECT_NE(source.find("movement_action_result"), std::string::npos);
    EXPECT_NE(source.find("movement_category"), std::string::npos);
    EXPECT_NE(source.find("endpoint_complete"), std::string::npos);
    EXPECT_NE(source.find("path_type"), std::string::npos);
    EXPECT_NE(source.find("QuestAcquisitionSnapshotJson(bot, botAI, travelTarget)"),
              std::string::npos);
    EXPECT_NE(body.find("setNullTravelTarget(leader)"), std::string::npos);
    EXPECT_NE(source.find("HandleQuestgiverAcceptQuestOpcode(packet)"), std::string::npos);
    EXPECT_NE(source.find("previousLeaderStatus == QUEST_STATUS_NONE"), std::string::npos);
    EXPECT_NE(source.find("observedLeaderStatus == QUEST_STATUS_INCOMPLETE"), std::string::npos);
    EXPECT_NE(source.find("\"issued\""), std::string::npos);
    EXPECT_NE(source.find("\"accepted\""), std::string::npos);
    EXPECT_NE(source.find("\"blocked\""), std::string::npos);
    EXPECT_NE(body.find("leaderPosition.distance(&point)"), std::string::npos);
    EXPECT_NE(body.find("distance >= 200000.0"), std::string::npos);
    EXPECT_NE(body.find("point.GetMapId() != leader->GetMapId()"), std::string::npos);
    EXPECT_NE(body.find("cross_map_route_unavailable"), std::string::npos);
    EXPECT_EQ(body.find("getQuestTravelDestinations(leader, -1"), std::string::npos);
    EXPECT_EQ(body.find("distance > 5000.0"), std::string::npos);
}

TEST(QuestAcquisitionContractTest, AcquireBodyContainsNoBypassOrBroadFallback)
{
    std::string const body = AcquireBody();
    ASSERT_FALSE(body.empty());
    for (std::string_view const forbidden : {
             ".AddQuest(", "CompleteQuest(", "RewardQuest(", "KilledMonsterCredit(",
             "CastedCreatureOrGO(", "AcceptAllQuestsAction", "accept all quests", "TeleportTo(",
             "+move random", "+grind", "urand(", "frand(", "rand("})
        EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
}
