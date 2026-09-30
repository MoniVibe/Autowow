/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "GatheringCandidatePolicy.h"
#include "GatheringReceiptPolicy.h"
#include "GatheringSafetyPolicy.h"
#include "WorkerGatherWatchdogPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using AutoWowGather::Candidate;
using AutoWowGather::CandidateLease;
using AutoWowGather::CandidateLeaseEvent;
using AutoWowGather::CandidateLeasePolicy;
using AutoWowGather::CandidateLeaseSample;
using AutoWowGather::CooldownMap;
using AutoWowGather::GatherCreditDecision;
using AutoWowGather::GatherCreditObservation;
using AutoWowGather::GatherNoDurableCreditDetail;
using AutoWowGather::GatherNoDurableCreditObservation;
using AutoWowGather::LiquidContact;
using AutoWowGather::LiquidEndpointClass;
using AutoWowGather::LiquidEndpointObservation;
using AutoWowGather::LiquidKind;
using AutoWowGather::LiquidSafetyDecision;
using AutoWowGather::Profession;
using AutoWowGather::Profile;
using AutoWowGather::SwimmingRouteProof;
using AutoWowGather::WorkerGatherWatchdogDecision;
using AutoWowGather::WorkerGatherWatchdogObservation;

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

CandidateLeaseSample LeaseSample(std::uint64_t candidateId, std::uint64_t nowSeconds, float x,
                                 float distance)
{
    CandidateLeaseSample sample;
    sample.candidateId = candidateId;
    sample.nowSeconds = nowSeconds;
    sample.position = {x, 0.0f, 0.0f};
    sample.distanceToCandidate = distance;
    return sample;
}

Candidate MakeCandidate(std::uint64_t spawnId, Profession profession, float x)
{
    Candidate candidate;
    candidate.spawnId = spawnId;
    candidate.entry = static_cast<std::uint32_t>(1000 + spawnId);
    candidate.mapId = 0;
    candidate.phaseMask = 1;
    candidate.x = x;
    candidate.profession = profession;
    candidate.requiredSkill = 1;
    candidate.nodeLevel = 1;
    return candidate;
}

Profile SkilledProfile()
{
    Profile profile;
    profile.mapId = 0;
    profile.phaseMask = 1;
    profile.level = 20;
    profile.herbalismSkill = 100;
    profile.miningSkill = 100;
    profile.hasMiningTool = true;
    profile.maxDistance = 1000.0f;
    profile.maxNodeLevelAboveBot = 5;
    return profile;
}

WorkerGatherWatchdogDecision WatchdogDecision(CandidateLeaseEvent leaseEvent, bool hasCandidate = true,
                                              std::uint64_t lootTargetCandidateId = 200766)
{
    WorkerGatherWatchdogObservation observation;
    observation.explicitWorker = true;
    observation.hasCandidate = hasCandidate;
    observation.candidateId = 200766;
    observation.leaseCandidateId = 200766;
    observation.leaseEvent = leaseEvent;
    observation.lootTargetCandidateId = lootTargetCandidateId;
    return AutoWowGather::EvaluateWorkerGatherWatchdog(observation);
}
}

TEST(WorkerGatherWatchdogPolicy, RepeatedHigherPriorityChecksAtDistanceZeroStayNonBlockingBeforeLease)
{
    auto transition = AutoWowGather::ObserveCandidateLease(
        CandidateLease{}, LeaseSample(200766, 100, 0.0f, 0.0f), AutoWowGather::DefaultCandidateLeasePolicy);
    ASSERT_EQ(transition.event, CandidateLeaseEvent::Acquired);

    for (std::uint64_t now = 101; now < 130; ++now)
    {
        transition = AutoWowGather::ObserveCandidateLease(
            transition.lease, LeaseSample(200766, now, 0.0f, 0.0f),
            AutoWowGather::DefaultCandidateLeasePolicy);
        WorkerGatherWatchdogDecision const decision = WatchdogDecision(transition.event);
        EXPECT_FALSE(decision.actionSucceeded) << now;
        EXPECT_FALSE(decision.expireCandidate) << now;
        EXPECT_EQ(decision.eventSequenceAdvance, 0u) << now;
    }
}

TEST(WorkerGatherWatchdogPolicy, ExactThirtySecondThresholdExpiresMatchingCandidateAndLootTarget)
{
    EXPECT_EQ(AutoWowGather::DefaultCandidateLeasePolicy.noProgressWindowSeconds, 30u);
    EXPECT_EQ(AutoWowGather::DefaultCandidateLeasePolicy.maxLeaseSeconds, 300u);

    auto transition = AutoWowGather::ObserveCandidateLease(
        CandidateLease{}, LeaseSample(200766, 100, 0.0f, 0.0f), AutoWowGather::DefaultCandidateLeasePolicy);

    transition = AutoWowGather::ObserveCandidateLease(
        transition.lease, LeaseSample(200766, 129, 0.0f, 0.0f),
        AutoWowGather::DefaultCandidateLeasePolicy);
    EXPECT_FALSE(WatchdogDecision(transition.event).actionSucceeded);

    transition = AutoWowGather::ObserveCandidateLease(
        transition.lease, LeaseSample(200766, 130, 0.0f, 0.0f),
        AutoWowGather::DefaultCandidateLeasePolicy);
    ASSERT_EQ(transition.event, CandidateLeaseEvent::ExpiredNoProgress);

    WorkerGatherWatchdogDecision const decision = WatchdogDecision(transition.event);
    EXPECT_TRUE(decision.actionSucceeded);
    EXPECT_TRUE(decision.expireCandidate);
    EXPECT_TRUE(decision.clearMatchingLootTarget);
    EXPECT_EQ(decision.eventSequenceAdvance, 1u);

    EXPECT_FALSE(WatchdogDecision(transition.event, true, 200767).clearMatchingLootTarget);
}

TEST(WorkerGatherWatchdogPolicy, ExpiryAdvancesSequenceExactlyOnceAfterCandidateClear)
{
    std::uint64_t eventSequence = 12;
    WorkerGatherWatchdogDecision decision =
        WatchdogDecision(CandidateLeaseEvent::ExpiredNoProgress, true);
    ASSERT_TRUE(decision.expireCandidate);
    eventSequence += decision.eventSequenceAdvance;

    decision = WatchdogDecision(CandidateLeaseEvent::ExpiredNoProgress, false);
    EXPECT_FALSE(decision.actionSucceeded);
    eventSequence += decision.eventSequenceAdvance;
    EXPECT_EQ(eventSequence, 13u);
}

TEST(WorkerGatherSchedulerContract, CandidateSelectionIsNotMaskedByQualifiedWatchdog)
{
    WorkerGatherWatchdogDecision const pending = WatchdogDecision(CandidateLeaseEvent::Pending);
    EXPECT_FALSE(pending.actionSucceeded);
    EXPECT_FALSE(pending.clearMatchingLootTarget);

    std::string const strategy =
        ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherStrategy.cpp");
    std::string const actionHeader =
        ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.h");
    std::string const action =
        ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    std::string const queue = ReadSource(ModuleRoot() / "src/Script/WorldThr/Queue.cpp");
    std::string const loot = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/LootAction.cpp");
    ASSERT_FALSE(strategy.empty());
    ASSERT_FALSE(actionHeader.empty());
    ASSERT_FALSE(action.empty());
    ASSERT_FALSE(queue.empty());
    ASSERT_FALSE(loot.empty());
    std::size_t const scheduledSeek = strategy.find("NextAction(\"worker gather seek\", 5.5f)");
    ASSERT_NE(scheduledSeek, std::string::npos);
    EXPECT_EQ(strategy.find("worker gather seek::watchdog"), std::string::npos);
    EXPECT_NE(actionHeader.find("NewRpgBaseAction(botAI, \"worker gather seek\")"), std::string::npos);
    EXPECT_NE(queue.find("action->getAction()->getName() == basket->getAction()->getName()"),
              std::string::npos);

    std::size_t const workerExecute = action.find("bool WorkerGatherAction::Execute(Event /*event*/)");
    std::size_t const noCandidate = action.find("if (!candidate)", workerExecute);
    std::size_t const selection = action.find("candidate = AutoWowGather::SelectCandidateFor(bot, botAI)",
                                               noCandidate);
    ASSERT_NE(workerExecute, std::string::npos);
    ASSERT_NE(noCandidate, std::string::npos);
    ASSERT_NE(selection, std::string::npos);
    EXPECT_LT(workerExecute, noCandidate);
    EXPECT_LT(noCandidate, selection);

    std::string const canonicalGather =
        ReadSource(ModuleRoot() / "src/Ai/Base/Strategy/LootNonCombatStrategy.cpp");
    ASSERT_FALSE(canonicalGather.empty());
    EXPECT_NE(canonicalGather.find("NextAction(\"add gathering loot\", 5.0f)"), std::string::npos);
    EXPECT_GT(5.5f, 5.0f);

    std::size_t const lootExecute = loot.find("bool OpenLootAction::Execute");
    std::size_t const doLoot = loot.find("bool OpenLootAction::DoLoot", lootExecute);
    ASSERT_NE(lootExecute, std::string::npos);
    ASSERT_NE(doLoot, std::string::npos);
    std::string const ordinarySuccess = loot.substr(lootExecute, doLoot - lootExecute);
    EXPECT_NE(ordinarySuccess.find("if (result)"), std::string::npos);
    std::size_t const successBranch = ordinarySuccess.find("if (result)");
    std::size_t const completion = ordinarySuccess.find("MarkCandidateGathered", successBranch);
    ASSERT_NE(completion, std::string::npos);
    EXPECT_NE(ordinarySuccess.find("liveSpawnId", completion), std::string::npos);
    EXPECT_NE(ordinarySuccess.find("liveGameObject->GetSpawnId()"), std::string::npos);
    EXPECT_NE(ordinarySuccess.find("available loot\")->Remove(lootObject.guid)"), std::string::npos);
    EXPECT_NE(ordinarySuccess.find("loot target\")->Set(LootObject())"), std::string::npos);

    std::string const state =
        ReadSource(ModuleRoot() / "src/Ai/World/Gathering/GatheringWorkerState.cpp");
    ASSERT_FALSE(state.empty());
    std::size_t const marker = state.find("bool MarkCandidateGathered");
    std::size_t const miss = state.find("void MarkCandidateMiss", marker);
    ASSERT_NE(marker, std::string::npos);
    ASSERT_NE(miss, std::string::npos);
    std::string const completionState = state.substr(marker, miss - marker);
    EXPECT_NE(completionState.find("state->second.candidate->spawnId != spawnId"), std::string::npos);
    EXPECT_NE(completionState.find("gatherReceipt.awaitingCredit"), std::string::npos);
    EXPECT_NE(completionState.find("gather_interaction_attempted"), std::string::npos);
    EXPECT_EQ(completionState.find("ClearCandidateTracking(state->second)"), std::string::npos);
    EXPECT_EQ(completionState.find("candidate_completed"), std::string::npos);
    EXPECT_EQ(completionState.find("Increment(state->second.missCount)"), std::string::npos);
}

TEST(WorkerGatherReceiptPolicy, HandoffCannotCompleteWithoutDurableEvidence)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::AwaitingCredit);
}

TEST(WorkerGatherReceiptPolicy, PreparedExactSourceAdmissionAllowsStoreBeforeCreditFlags)
{
    // No pre-generation source-item or yield flag is an input: true here models an empty prepared
    // receipt followed by the requested item appearing in the exact live generated GO loot.
    EXPECT_TRUE(AutoWowGather::IsPreparedExactGatherLootAllowed(
        true, true, true, true, true));

    EXPECT_FALSE(AutoWowGather::IsPreparedExactGatherLootAllowed(
        true, true, false, true, true)) << "mismatched live GameObject";
    EXPECT_FALSE(AutoWowGather::IsPreparedExactGatherLootAllowed(
        true, true, true, false, true)) << "item absent from exact live generated loot";

    // The pre-generation receipt may have no source items. The item proof comes from the exact
    // GameObject's live generated loot during synchronous StoreLootAction instead. Every live
    // exact-source proof remains mandatory, so a mismatched GO or item remains rejected.
    for (std::size_t rejected = 0; rejected < 5; ++rejected)
    {
        bool facts[] = {true, true, true, true, true};
        facts[rejected] = false;
        EXPECT_FALSE(AutoWowGather::IsPreparedExactGatherLootAllowed(
            facts[0], facts[1], facts[2], facts[3], facts[4]))
            << "rejected proof index " << rejected;
    }
}

TEST(WorkerGatherReceiptPolicy, LootOpenedWithoutCreditBecomesExplicitNoCredit)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.lootResponseObserved = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, MissingLootResponseIsDistinctFromNoDurableCredit)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoLootResponse);
}

TEST(WorkerGatherReceiptPolicy, PostLootConsumptionConfirmsGenericGathering)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.lootResponseObserved = true;
    observation.lootProcessingObserved = true;
    observation.sourceLootConsumed = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedLootConsumed);
}

TEST(WorkerGatherReceiptPolicy, ExactSourceConsumptionLatchSurvivesTerminalDisappearance)
{
    bool consumed = AutoWowGather::LatchSourceLootConsumption(false, true, true, true, true, true);
    consumed = AutoWowGather::LatchSourceLootConsumption(consumed, true, true, false, true, false);

    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.sourceLootConsumed = consumed;
    observation.lootProcessingObserved = true;

    EXPECT_TRUE(consumed);
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedLootConsumed);
}

TEST(WorkerGatherReceiptPolicy, SourceDisappearanceAloneNeverLatchesConsumption)
{
    bool const consumed = AutoWowGather::LatchSourceLootConsumption(false, true, true, false, true, true);
    bool const consumedWithoutResponse =
        AutoWowGather::LatchSourceLootConsumption(false, true, true, true, false, true);

    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.sourceLootConsumed = consumed;
    observation.lootProcessingObserved = true;
    observation.lootResponseObserved = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_FALSE(consumed);
    EXPECT_FALSE(consumedWithoutResponse);
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, NoAutostoreWithLiveUnlootedSourceReportsBagUsageBoundary)
{
    GatherNoDurableCreditObservation observation;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.sourcePresent = true;
    observation.sourceUnlootedCount = 1;
    observation.bagSpacePercent = 81;
    observation.lootResponseDelta = 1;
    observation.storeLootExecutionDelta = 1;

    EXPECT_EQ(AutoWowGather::DiagnoseNoDurableCredit(observation),
              GatherNoDurableCreditDetail::LootNotQueuedHighBagUsage);

    observation.bagSpacePercent = 80;
    EXPECT_EQ(AutoWowGather::DiagnoseNoDurableCredit(observation),
              GatherNoDurableCreditDetail::LootFilteredOrIneligible);
}

TEST(WorkerGatherReceiptPolicy, AutostoreWithoutConsumptionIdentifiesCoreStorageRejection)
{
    GatherNoDurableCreditObservation observation;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.sourcePresent = true;
    observation.sourceUnlootedCount = 1;
    observation.lootResponseDelta = 1;
    observation.storeLootExecutionDelta = 1;
    observation.autostoreLootPacketDelta = 1;

    EXPECT_EQ(AutoWowGather::DiagnoseNoDurableCredit(observation),
              GatherNoDurableCreditDetail::AutostoreRejected);
}

TEST(WorkerGatherReceiptPolicy, MissingGeneratedSourceIdentifiesTerminalRaceWithoutClaimingCredit)
{
    GatherNoDurableCreditObservation observation;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.sourcePresent = false;
    observation.lootResponseDelta = 1;
    observation.storeLootExecutionDelta = 1;

    EXPECT_EQ(AutoWowGather::DiagnoseNoDurableCredit(observation),
              GatherNoDurableCreditDetail::SourceTerminalStateUnobserved);
}

TEST(WorkerGatherReceiptPolicy, CoreSkillCreditConfirmsWhenNoItemEvidenceExists)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.matchingSkillDelta = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedSkillCredit);
}

TEST(WorkerGatherReceiptPolicy, EventSequenceDoesNotCompleteAtInteractionOrLootOpen)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::AwaitingCredit);

    observation.lootResponseObserved = true;
    observation.elapsedSeconds = 1;
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::AwaitingCredit);

    observation.sourceYieldsRequestedMaterial = true;
    observation.lootProcessingObserved = true;
    observation.sourceLootConsumed = true;
    observation.elapsedSeconds = 2;
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedLootConsumed);
}

TEST(WorkerGatherReceiptPolicy, DelayedMatchingSkillCreditCompletesBeforeBoundedTimeout)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.elapsedSeconds = observation.timeoutSeconds - 1;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::AwaitingCredit);

    observation.matchingSkillDelta = true;
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedSkillCredit);
}

TEST(WorkerGatherReceiptPolicy, MatchingMaterialDeltaIsDurableCredit)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.matchingMaterialDelta = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::ConfirmedMaterialCredit);
}

TEST(WorkerGatherReceiptPolicy, ObtainMaterialRequiresInventoryDeltaBeyondHarvestNodeProof)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.materialFulfillmentRequired = true;
    observation.sourceLootConsumed = true;
    observation.lootProcessingObserved = true;
    observation.matchingSkillDelta = true;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::AwaitingCredit);

    observation.matchingMaterialDelta = true;
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation),
              GatherCreditDecision::ConfirmedMaterialCredit);
}

TEST(WorkerGatherReceiptPolicy, ControlledSnapshotDerivesMaterialAndSkillDeltas)
{
    AutoWowGather::GatherReceiptSnapshot snapshot;
    snapshot.interactionAttempted = true;
    snapshot.sourceMatched = true;
    snapshot.sourceLootGenerationObserved = true;
    snapshot.sourceYieldsRequestedMaterial = true;
    snapshot.materialBefore = 4;
    snapshot.materialAfter = 5;
    snapshot.skillBefore = 125;
    snapshot.skillAfter = 126;

    GatherCreditObservation const observation =
        AutoWowGather::MakeGatherCreditObservation(snapshot, true);
    EXPECT_TRUE(observation.materialFulfillmentRequired);
    EXPECT_TRUE(observation.matchingMaterialDelta);
    EXPECT_TRUE(observation.matchingSkillDelta);
    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation),
              GatherCreditDecision::ConfirmedMaterialCredit);
}

TEST(WorkerGatherReceiptPolicy, SourceMismatchCannotCompleteFromAnotherLootSession)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.lootResponseObserved = true;
    observation.lootProcessingObserved = true;
    observation.sourceLootConsumed = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, LootConsumptionWithoutProcessingIsNotSuccess)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourceYieldsRequestedMaterial = true;
    observation.lootResponseObserved = true;
    observation.sourceLootConsumed = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, UnrelatedSkillIncreaseIsNotAFalsePositive)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.lootResponseObserved = true;
    observation.skillIncreased = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, MatchingDeltaWithoutSourceGenerationIsNotDurable)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.sourceMatched = true;
    observation.matchingSkillDelta = true;
    observation.matchingMaterialDelta = true;
    observation.lootResponseObserved = true;
    observation.elapsedSeconds = observation.timeoutSeconds;

    EXPECT_EQ(AutoWowGather::EvaluateGatherCredit(observation), GatherCreditDecision::FailedNoDurableCredit);
}

TEST(WorkerGatherReceiptPolicy, SourceMaterialMustBeProvenIndependently)
{
    GatherNoDurableCreditObservation observation;
    observation.sourceMatched = true;
    observation.sourceLootGenerationObserved = true;
    observation.sourcePresent = true;
    observation.sourceUnlootedCount = 1;
    observation.lootResponseDelta = 1;
    observation.storeLootExecutionDelta = 1;

    EXPECT_EQ(AutoWowGather::DiagnoseNoDurableCredit(observation),
              GatherNoDurableCreditDetail::SourceMaterialUnproven);
}

TEST(WorkerGatherLeasePolicy, TracksDistanceOrPositionProgressThenExpiresAtNoProgressBoundary)
{
    CandidateLeasePolicy policy;
    policy.noProgressWindowSeconds = 10;
    policy.maxLeaseSeconds = 100;
    policy.minDistanceReduction = 2.0f;
    policy.minPositionChange = 3.0f;

    auto transition = AutoWowGather::ObserveCandidateLease(CandidateLease{}, LeaseSample(7, 100, 0.0f, 100.0f), policy);
    ASSERT_EQ(transition.event, CandidateLeaseEvent::Acquired);

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(7, 105, 1.0f, 99.0f), policy);
    EXPECT_EQ(transition.event, CandidateLeaseEvent::Pending);

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(7, 109, 3.0f, 99.0f), policy);
    ASSERT_EQ(transition.event, CandidateLeaseEvent::Progress);
    EXPECT_EQ(transition.lease.lastProgressAtSeconds, 109u);

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(7, 118, 3.5f, 98.5f), policy);
    EXPECT_EQ(transition.event, CandidateLeaseEvent::Pending);

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(7, 119, 3.5f, 98.5f), policy);
    EXPECT_EQ(transition.event, CandidateLeaseEvent::ExpiredNoProgress);
    EXPECT_STREQ(AutoWowGather::CandidateLeaseReason(transition.event), "candidate_lease_no_progress");
}

TEST(WorkerGatherLeasePolicy, HardMaxLeaseExpiresDespiteRecentProgressAndCandidateSwitchAcquiresFreshLease)
{
    CandidateLeasePolicy policy;
    policy.noProgressWindowSeconds = 10;
    policy.maxLeaseSeconds = 20;
    policy.minDistanceReduction = 1.0f;
    policy.minPositionChange = 100.0f;

    auto transition = AutoWowGather::ObserveCandidateLease(CandidateLease{}, LeaseSample(11, 50, 0.0f, 50.0f), policy);
    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(11, 69, 0.0f, 48.0f), policy);
    ASSERT_EQ(transition.event, CandidateLeaseEvent::Progress);

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(11, 70, 0.0f, 47.0f), policy);
    EXPECT_EQ(transition.event, CandidateLeaseEvent::ExpiredMaxLease);
    EXPECT_STREQ(AutoWowGather::CandidateLeaseReason(transition.event), "candidate_lease_max_age");

    transition = AutoWowGather::ObserveCandidateLease(transition.lease, LeaseSample(12, 71, 0.0f, 30.0f), policy);
    EXPECT_EQ(transition.event, CandidateLeaseEvent::Acquired);
    EXPECT_EQ(transition.lease.candidateId, 12u);
    EXPECT_EQ(transition.lease.acquiredAtSeconds, 71u);
}

TEST(WorkerGatherLeasePolicy, ExpiredCandidateCooldownSelectsAnotherCandidateDeterministically)
{
    Profile const profile = SkilledProfile();
    std::vector<Candidate> const candidates = {
        MakeCandidate(10, Profession::Herbalism, 10.0f),
        MakeCandidate(20, Profession::Mining, 20.0f),
    };
    CooldownMap cooldowns;

    auto selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1000);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);

    cooldowns[10] = 1100;
    selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1001);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 20u);

    selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1100);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);
}

TEST(WorkerGatherLiquidPolicy, PreservesDryAndShallowNodesButRequiresProofForDeepWater)
{
    SwimmingRouteProof noProof;
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(
                  AutoWowGather::ClassifyLiquidEndpoint({LiquidContact::NoWater, LiquidKind::None}), noProof),
              LiquidSafetyDecision::AllowDry);
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(
                  AutoWowGather::ClassifyLiquidEndpoint({LiquidContact::InWater, LiquidKind::Water}), noProof),
              LiquidSafetyDecision::AllowShallowWater);

    LiquidEndpointClass const deepWater =
        AutoWowGather::ClassifyLiquidEndpoint({LiquidContact::UnderWater, LiquidKind::Water});
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(deepWater, noProof),
              LiquidSafetyDecision::RejectUnsupportedDeepWater);

    SwimmingRouteProof proof;
    proof.canSwim = true;
    proof.pathCalculated = true;
    proof.pathComplete = true;
    proof.allPathPointsInWater = true;
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(deepWater, proof),
              LiquidSafetyDecision::AllowSupportedSwimmingRoute);

    LiquidEndpointObservation const hazardous{LiquidContact::UnderWater, LiquidKind::Hazardous};
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(AutoWowGather::ClassifyLiquidEndpoint(hazardous), proof),
              LiquidSafetyDecision::RejectHazardousLiquid);
    EXPECT_EQ(AutoWowGather::EvaluateLiquidSafety(LiquidEndpointClass::Unknown, proof),
              LiquidSafetyDecision::RejectUnknownLiquid);
}

TEST(WorkerGatherSafetySourceContract, UsesCoreLiquidAndPathApisWithoutTeleportRandomOrCandidateHardcodes)
{
    std::string const action = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    std::string const state = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/GatheringWorkerState.cpp");
    ASSERT_FALSE(action.empty());
    ASSERT_FALSE(state.empty());

    for (std::string_view required : {"GetLiquidData", "MAP_LIQUID_TYPE_WATER", "PathGenerator",
                                      "CanSwim", "IsWaterPath", "MoveFarTo(destination, /*questNoTeleport*/ true"})
        EXPECT_NE(action.find(required), std::string::npos) << required;

    EXPECT_NE(state.find("std::chrono::steady_clock"), std::string::npos);
    EXPECT_NE(state.find("candidate_lease_expired"), std::string::npos);
    EXPECT_NE(state.find("candidate_liquid_rejected"), std::string::npos);
    EXPECT_NE(state.find("SKILL_SKINNING"), std::string::npos);
    EXPECT_NE(state.find("EvaluateGatherCredit"), std::string::npos);
    EXPECT_NE(state.find("PrepareReceiptBeforeAttempt"), std::string::npos);
    EXPECT_NE(state.find("GetLootGenerationTime"), std::string::npos);
    EXPECT_NE(state.find("sourceMatched"), std::string::npos);
    EXPECT_NE(state.find("sourceLootGenerationObserved"), std::string::npos);
    EXPECT_NE(state.find("LatchSourceLootConsumption"), std::string::npos);
    EXPECT_NE(state.find("DiagnoseNoDurableCredit"), std::string::npos);
    EXPECT_NE(state.find("bagSpacePercent"), std::string::npos);
    EXPECT_NE(state.find("itemCountsBefore"), std::string::npos);
    EXPECT_NE(state.find("GetItemCount"), std::string::npos);
    EXPECT_EQ(state.find("GO_JUST_DEACTIVATED"), std::string::npos);

    std::size_t const receiptHandoff = action.find("SetCandidateRuntimeGuid");
    std::size_t const lootHandoff = action.find("DoSpecificAction(\"add gathering loot\"");
    ASSERT_NE(receiptHandoff, std::string::npos);
    ASSERT_NE(lootHandoff, std::string::npos);
    EXPECT_LT(receiptHandoff, lootHandoff);

    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    for (std::string_view field : {"\\\"awaiting_credit\\\"", "\\\"credit_evidence\\\"",
                                   "\\\"loot_response_delta\\\"", "\\\"store_loot_execution_delta\\\"",
                                   "\\\"autostore_loot_packet_delta\\\"", "\\\"loot_packet_item_delta\\\"",
                                   "\\\"loot_allowed_owner_slot_delta\\\"", "\\\"loot_slot_type_rejected_delta\\\"",
                                   "\\\"loot_policy_rejected_delta\\\"", "\\\"loot_missing_template_delta\\\"",
                                   "\\\"loot_bag_reserve_rejected_delta\\\"",
                                   "\\\"loot_admission_failure_mask\\\"", "\\\"loot_admission_failure\\\"",
                                   "\\\"skill_before\\\"",
                                   "\\\"skill_after\\\""})
        EXPECT_NE(bridge.find(field), std::string::npos) << field;

    std::string const combined = action + state;
    for (std::string_view forbidden : {"TeleportTo(", "MoveRandom", "rand_norm", "urand(", "frand(",
                                       "Stranglekelp"})
        EXPECT_EQ(combined.find(forbidden), std::string::npos) << forbidden;

    std::regex const entryLiteral(R"((GetEntry\(\)|\.entry|->entry)\s*(==|!=)\s*[0-9]+)");
    std::regex const nameComparison(R"((\.name|->name|GetName\(\))\s*(==|!=))");
    EXPECT_FALSE(std::regex_search(combined, entryLiteral));
    EXPECT_FALSE(std::regex_search(combined, nameComparison));
}

TEST(WorkerGatherSafetyRegression, ExistingSkillToolAndNearestSelectionBehaviorRemainsIntact)
{
    Profile profile = SkilledProfile();
    CooldownMap cooldowns;
    std::vector<Candidate> candidates = {
        MakeCandidate(30, Profession::Herbalism, 30.0f),
        MakeCandidate(10, Profession::Herbalism, 10.0f),
        MakeCandidate(20, Profession::Mining, 20.0f),
    };

    auto selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1000);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);

    profile.hasMiningTool = false;
    EXPECT_FALSE(AutoWowGather::IsEligible(candidates[2], profile, cooldowns, 1000));
    profile.herbalismSkill = 0;
    EXPECT_FALSE(AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1000).has_value());
}

TEST(GatherBagAdmissionPolicy, ChecksOnlyAutonomousGatheringAndRejectsAboveEightyPercent)
{
    EXPECT_EQ(AutoWowGather::EvaluateGatherBagAdmission(true, false),
              AutoWowGather::GatherBagAdmission::CheckCapacity);
    EXPECT_TRUE(AutoWowGather::HasGatherBagCapacity(80));
    EXPECT_FALSE(AutoWowGather::HasGatherBagCapacity(81));

    EXPECT_EQ(AutoWowGather::EvaluateGatherBagAdmission(true, true),
              AutoWowGather::GatherBagAdmission::Admit);
    EXPECT_EQ(AutoWowGather::EvaluateGatherBagAdmission(false, false),
              AutoWowGather::GatherBagAdmission::Admit);
}
