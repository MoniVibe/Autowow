#include "../src/AutoWow/OracleGatherExecutor.h"
#include "../src/Ai/World/Gathering/WorkerGatherAction.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleExecutor;
using namespace AutoWowOracleGatherExecutor;

constexpr Guid kBot = 77;
constexpr SquadId kSquad = 8;
constexpr NodeId kSpawn = 9001;
constexpr std::uint32_t kEntry = 7001;
constexpr std::uint32_t kMap = 1;
constexpr std::uint32_t kInstance = 9;
constexpr ItemId kMaterial = 500;
constexpr DecisionId kDecision = 1001;
constexpr IntentId kIntent = 2001;
constexpr Epoch kEpoch = 4;
constexpr FactVersion kFrame = 12;
constexpr Tick kIssued = 20;
constexpr Tick kExpiry = 40;

Scope TestScope()
{
    return {ScopeKind::LabFixture, kSquad, kBot};
}

ActiveLeaseSnapshot Ownership(DecisionId decisionId = kDecision, IntentId intentId = kIntent,
                              Epoch epoch = kEpoch, Tick expiresTick = kExpiry,
                              OperationCode operation = OperationCode::GatherSource)
{
    ActiveLeaseSnapshot proof;
    proof.active = true;
    proof.botGuid = kBot;
    proof.decisionId = decisionId;
    proof.intentId = intentId;
    proof.epoch = epoch;
    proof.expiresTick = expiresTick;
    proof.scope = TestScope();
    proof.operation = operation;
    return proof;
}

GatherSourceReference GatherReference(NodeId spawnId = kSpawn, std::uint32_t entry = kEntry,
                                      std::uint32_t mapId = kMap, ItemId material = kMaterial,
                                      std::uint32_t instanceId = kInstance,
                                      GatherGoal goal = GatherGoal::HarvestNode)
{
    return {true, spawnId, entry, mapId, material, instanceId, goal};
}

ExecutorRequest Request(GatherGoal goal = GatherGoal::HarvestNode)
{
    ExecutorRequest request;
    request.valid = true;
    request.requestId = 70001;
    request.operation = OperationCode::GatherSource;
    request.domain = Domain::Gathering;
    request.intent = IntentCode::GatherSource;
    request.resource = LeaseResource::QuestGather;
    request.scope = TestScope();
    request.decisionId = kDecision;
    request.intentId = kIntent;
    request.actorGuid = kBot;
    request.targetGuid = kSpawn;
    request.itemId = kMaterial;
    request.executorAvailable = true;
    request.gather = GatherReference(kSpawn, kEntry, kMap, kMaterial, kInstance, goal);
    request.gatherGoal = goal;
    request.epoch = kEpoch;
    request.frameVersion = kFrame;
    request.issuedTick = kIssued;
    request.expiresTick = kExpiry;
    request.ttlTicks = kExpiry - kIssued;
    request.ownershipProof = Ownership();
    return request;
}

IntentLease Lease(GatherGoal goal = GatherGoal::HarvestNode)
{
    Decision decision;
    decision.valid = true;
    decision.decisionId = kDecision;
    decision.intentId = kIntent;
    decision.domain = Domain::Gathering;
    decision.intent = IntentCode::GatherSource;
    decision.resource = LeaseResource::QuestGather;
    decision.scope = TestScope();
    decision.actorGuid = kBot;
    decision.targetGuid = kSpawn;
    decision.itemId = kMaterial;
    decision.executorAvailable = true;
    decision.operation = OperationCode::GatherSource;
    decision.gather = GatherReference(kSpawn, kEntry, kMap, kMaterial, kInstance, goal);
    decision.epoch = kEpoch;
    decision.issuedTick = kIssued;
    decision.expiresTick = kExpiry;
    decision.ttlTicks = kExpiry - kIssued;
    decision.preconditions.frameVersion = kFrame;
    decision.preconditions.epoch = kEpoch;
    decision.preconditions.botGuid = kBot;
    decision.preconditions.squadId = kSquad;
    decision.preconditions.mapId = kMap;
    decision.preconditions.instanceId = kInstance;
    return {true, decision};
}

NodeReservationProof NodeReservation(GatherGoal goal = GatherGoal::HarvestNode)
{
    Receipt receipt;
    receipt.status = ReceiptStatus::Accepted;
    receipt.reason = ReceiptReason::LeaseAcquired;
    receipt.decisionId = kDecision;
    receipt.intentId = kIntent;
    receipt.domain = Domain::Gathering;
    receipt.resource = LeaseResource::QuestGather;
    receipt.scope = TestScope();
    receipt.frameVersion = kFrame;
    receipt.epoch = kEpoch;
    receipt.actorGuid = kBot;
    receipt.operation = OperationCode::GatherSource;
    receipt.gather = GatherReference(kSpawn, kEntry, kMap, kMaterial, kInstance, goal);
    return {receipt, kExpiry};
}

FixedTargetCandidate Candidate(GatherGoal goal = GatherGoal::HarvestNode)
{
    FixedTargetCandidate candidate;
    candidate.reference = GatherReference(kSpawn, kEntry, kMap, kMaterial, kInstance, goal);
    candidate.facts.alive = true;
    candidate.facts.available = true;
    candidate.facts.reachable = true;
    candidate.facts.profession = AutoWowGather::Profession::Mining;
    candidate.facts.toolRequired = true;
    candidate.facts.toolAvailable = true;
    candidate.facts.sourceYieldsRequestedMaterial = true;
    return candidate;
}

FixedTargetObservation Observation(GatherGoal goal = GatherGoal::HarvestNode)
{
    FixedTargetObservation observation;
    observation.world.onWorldThread = true;
    observation.world.botAlive = true;
    observation.world.botGuid = kBot;
    observation.world.mapId = kMap;
    observation.world.instanceId = kInstance;
    observation.world.tick = kIssued + 1;
    observation.world.frameVersion = kFrame;
    observation.world.epoch = kEpoch;
    observation.world.ownershipProof = Ownership();
    observation.nodeReservation = NodeReservation(goal);
    observation.candidate = Candidate(goal);
    return observation;
}

FixedTargetObservation PostObservation(GatherGoal goal = GatherGoal::HarvestNode)
{
    FixedTargetObservation observation = Observation(goal);
    observation.world.tick = kIssued + 2;
    observation.world.frameVersion = kFrame + 1;
    return observation;
}

struct NativeProbe
{
    int calls = 0;
    NativeStepPhase phase = NativeStepPhase::Unknown;
    std::uint8_t maxSteps = 0;
    bool stopAfterStep = false;
    FixedTargetCandidate candidate;
    FixedTargetObservation after = PostObservation();
    bool accept = true;
    bool selectedAnotherCandidate = false;
    AutoWowGather::GatherCreditObservation credit;
};

NativeStepObservation NativeStep(void* context, NativeStepRequest const& request)
{
    NativeProbe* probe = static_cast<NativeProbe*>(context);
    ++probe->calls;
    probe->phase = request.phase;
    probe->maxSteps = request.maxSteps;
    probe->stopAfterStep = request.stopAfterStep;
    probe->candidate = request.candidate;

    NativeStepObservation observation;
    observation.dispatchAccepted = probe->accept;
    observation.stepsInvoked = 1;
    observation.selectedAnotherCandidate = probe->selectedAnotherCandidate;
    observation.credit = probe->credit;
    observation.after = probe->after;
    return observation;
}

// This adapter drives the production WorkerGatherAction receipt loop. It supplies raw controlled
// source/inventory/skill snapshots; ObserveFixedTargetReceipt derives GatherCreditObservation and
// the material/skill deltas. No finished credit object is injected into the executor.
struct CanonicalReceiptLoopProbe
{
    WorkerGatherAction::FixedTargetNativeContext context;
    AutoWowGather::GatherReceiptSnapshot afterSnapshot;
    int polls = 0;
    int nativeCalls = 0;

    explicit CanonicalReceiptLoopProbe(GatherGoal goal = GatherGoal::HarvestNode)
    {
        this->context.before = Observation(goal);
        this->context.candidateValid = true;
        this->context.controlledReceiptSnapshot = &Snapshot;
        this->context.controlledReceiptSnapshotContext = this;
    }

    static bool Snapshot(void* rawContext, AutoWowGather::GatherReceiptSnapshot* snapshot) noexcept
    {
        CanonicalReceiptLoopProbe* probe = static_cast<CanonicalReceiptLoopProbe*>(rawContext);
        ++probe->polls;
        if (probe->polls == 1)
            return false;
        *snapshot = probe->afterSnapshot;
        return true;
    }

    static bool Native(void* rawContext, NativeStepRequest const&) noexcept
    {
        WorkerGatherAction::FixedTargetNativeContext* context =
            static_cast<WorkerGatherAction::FixedTargetNativeContext*>(rawContext);
        CanonicalReceiptLoopProbe* probe = static_cast<CanonicalReceiptLoopProbe*>(
            context->controlledReceiptSnapshotContext);
        ++probe->nativeCalls;
        return true;
    }

    static void Capture(void* rawContext, std::uint32_t, EvidenceCounters& evidence) noexcept
    {
        WorkerGatherAction::FixedTargetNativeContext* context =
            static_cast<WorkerGatherAction::FixedTargetNativeContext*>(rawContext);
        CanonicalReceiptLoopProbe* probe = static_cast<CanonicalReceiptLoopProbe*>(
            context->controlledReceiptSnapshotContext);
        evidence.resource = probe->afterSnapshot.materialAfter;
        evidence.progress = probe->afterSnapshot.skillAfter;
    }

    static NativeStepObservation Step(void* rawContext, NativeStepRequest const& request)
    {
        WorkerGatherAction::FixedTargetNativeContext* context =
            static_cast<WorkerGatherAction::FixedTargetNativeContext*>(rawContext);
        return WorkerGatherAction::ExecuteProductionReceiptLoop(
            context, request, context->before, &Native, &Capture);
    }
};

struct FakeWorkerNativeAdapter
{
    FixedTargetCandidate requested;
    FixedTargetCandidate fallback;
    int calls = 0;
    bool exactHandoff = false;
    bool fallbackUsed = false;
    std::uint8_t maxSteps = 0;

    static NativeStepObservation Step(void* context, NativeStepRequest const& request)
    {
        FakeWorkerNativeAdapter* adapter = static_cast<FakeWorkerNativeAdapter*>(context);
        ++adapter->calls;
        adapter->requested = request.candidate;
        adapter->maxSteps = request.maxSteps;
        adapter->exactHandoff = ExactCandidateMatches(request.candidate, Candidate());
        adapter->fallbackUsed = ExactCandidateMatches(request.candidate, adapter->fallback);

        NativeStepObservation observation;
        observation.dispatchAccepted = true;
        observation.stepsInvoked = 1;
        observation.after = PostObservation();
        observation.after.world.ownershipProof = request.request.ownershipProof;
        observation.after.nodeReservation = request.nodeReservation;
        observation.after.candidate = request.candidate;
        return observation;
    }
};

DispatchResult Dispatch(NativeProbe& probe,
                        FixedTargetObservation const& before = Observation(),
                        FixedTargetCandidate const& exactTarget = Candidate())
{
    return OracleGatherExecutor::Dispatch(Request(), Lease(), exactTarget, before, &NativeStep,
                                          &probe);
}

TEST(OracleGatherExecutorTest, UsesAuthoritativeTypedGatherRequestAndLease)
{
    static_assert(std::is_same_v<decltype(ExecutorRequest{}.operation), OperationCode>);
    static_assert(std::is_same_v<decltype(IntentLease{}.decision), Decision>);

    ExecutorRequest const request = Request();
    EXPECT_EQ(request.operation, OperationCode::GatherSource);
    EXPECT_EQ(request.gather.spawnId, kSpawn);
    EXPECT_EQ(request.gather.entry, kEntry);
    EXPECT_EQ(request.gather.mapId, kMap);
    EXPECT_EQ(request.gather.materialItemId, kMaterial);
    EXPECT_EQ(request.gatherGoal, GatherGoal::HarvestNode);

    // GatherSource is deliberately admitted only as the exact, source-material-proven tuple.
    EXPECT_TRUE(IsVerifiedOperationTuple(request.executorAvailable, request.domain,
                                          request.intent, request.resource, request.operation,
                                          request.quest, request.gather, request.itemId,
                                          request.objectiveId));
}

TEST(OracleGatherExecutorTest, RejectsRequestGoalThatDoesNotMatchExactReference)
{
    ExecutorRequest request = Request(GatherGoal::HarvestNode);
    request.gatherGoal = GatherGoal::ObtainMaterial;
    NativeProbe probe;
    DispatchResult const result = OracleGatherExecutor::Dispatch(
        request, Lease(GatherGoal::HarvestNode), Candidate(GatherGoal::HarvestNode),
        Observation(GatherGoal::HarvestNode), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::InvalidGatherSourceReference);
}

TEST(OracleGatherExecutorTest, ExactCandidateMatcherRejectsEveryIdentityAndFactMismatch)
{
    FixedTargetCandidate const expected = Candidate();
    FixedTargetCandidate observed = expected;
    ASSERT_TRUE(ExactCandidateMatches(expected, observed));

    observed.reference.spawnId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.entry++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.mapId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.instanceId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.materialItemId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));

    observed = expected;
    observed.facts.alive = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.available = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.reachable = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.profession = AutoWowGather::Profession::Herbalism;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.toolRequired = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.toolAvailable = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.facts.sourceYieldsRequestedMaterial = false;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
}

TEST(OracleGatherExecutorTest, DispatchesOneExactSeekInteractOrLootStep)
{
    NativeProbe seek;
    DispatchResult result = Dispatch(seek);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Progressing);
    EXPECT_EQ(result.phase, NativeStepPhase::Seek);
    EXPECT_TRUE(result.beforeValidated);
    EXPECT_TRUE(result.afterValidated);
    EXPECT_EQ(seek.calls, 1);
    EXPECT_EQ(seek.phase, NativeStepPhase::Seek);
    EXPECT_EQ(seek.maxSteps, 1);
    EXPECT_TRUE(seek.stopAfterStep);
    EXPECT_TRUE(ExactCandidateMatches(Candidate(), seek.candidate));

    FixedTargetObservation interactBefore = Observation();
    interactBefore.world.withinInteractRange = true;
    NativeProbe interact;
    result = Dispatch(interact, interactBefore);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.phase, NativeStepPhase::Interact);
    EXPECT_EQ(interact.calls, 1);

    FixedTargetObservation lootBefore = Observation();
    lootBefore.world.lootReady = true;
    NativeProbe loot;
    result = Dispatch(loot, lootBefore);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Progressing);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::AwaitingCredit);
    EXPECT_EQ(result.phase, NativeStepPhase::Loot);
    EXPECT_EQ(loot.calls, 1);

    loot.credit.interactionAttempted = true;
    loot.credit.lootResponseObserved = true;
    loot.credit.lootProcessingObserved = true;
    loot.credit.sourceMatched = true;
    loot.credit.sourceLootGenerationObserved = true;
    loot.credit.sourceYieldsRequestedMaterial = true;
    loot.credit.sourceLootConsumed = true;
    result = Dispatch(loot, lootBefore);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::ConfirmedLootConsumed);
}

TEST(OracleGatherExecutorTest, OpenLootAcceptanceNeverCompletesWithoutCanonicalCredit)
{
    NativeProbe probe;
    FixedTargetObservation lootBefore = Observation();
    lootBefore.world.lootReady = true;

    DispatchResult const result = Dispatch(probe, lootBefore);
    ASSERT_TRUE(result.accepted);
    EXPECT_NE(result.status, ExecutorStatus::Completed);
    EXPECT_FALSE(AutoWowGather::IsConfirmedGatherCredit(result.creditDecision));
}

TEST(OracleGatherExecutorTest, HarvestNodeCompletesFromCanonicalSkillCredit)
{
    NativeProbe probe;
    probe.credit.interactionAttempted = true;
    probe.credit.sourceMatched = true;
    probe.credit.sourceLootGenerationObserved = true;
    probe.credit.sourceYieldsRequestedMaterial = true;
    probe.credit.matchingSkillDelta = true;

    DispatchResult const result = Dispatch(probe);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.request.gatherGoal, GatherGoal::HarvestNode);
    EXPECT_EQ(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::ConfirmedSkillCredit);
    EXPECT_FALSE(result.credit.materialFulfillmentRequired);
}

TEST(OracleGatherExecutorTest, ProductionReceiptLoopPollsBeforeAndAfterExactNativeStep)
{
    CanonicalReceiptLoopProbe probe(GatherGoal::ObtainMaterial);
    probe.afterSnapshot.interactionAttempted = true;
    probe.afterSnapshot.lootResponseObserved = true;
    probe.afterSnapshot.lootProcessingObserved = true;
    probe.afterSnapshot.sourceMatched = true;
    probe.afterSnapshot.sourceLootGenerationObserved = true;
    probe.afterSnapshot.sourceYieldsRequestedMaterial = true;
    probe.afterSnapshot.sourceLootConsumed = true;
    probe.afterSnapshot.materialBefore = 7;
    probe.afterSnapshot.materialAfter = 10;
    probe.afterSnapshot.skillBefore = 125;
    probe.afterSnapshot.skillAfter = 126;

    DispatchResult const result = OracleGatherExecutor::Dispatch(
        Request(GatherGoal::ObtainMaterial), Lease(GatherGoal::ObtainMaterial),
        Candidate(GatherGoal::ObtainMaterial), probe.context.before, &CanonicalReceiptLoopProbe::Step,
        &probe.context);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::ConfirmedMaterialCredit);
    EXPECT_EQ(probe.polls, 2);
    EXPECT_EQ(probe.nativeCalls, 1);
    EXPECT_TRUE(result.after.world.evidence.resource == 10U);
    EXPECT_TRUE(result.after.world.evidence.progress == 126U);
    EXPECT_TRUE(result.credit.materialFulfillmentRequired);
    EXPECT_TRUE(result.credit.matchingMaterialDelta);
    EXPECT_TRUE(result.credit.matchingSkillDelta);
}

TEST(OracleGatherExecutorTest, ProductionReceiptLoopDoesNotTreatConsumedNodeAsMaterial)
{
    CanonicalReceiptLoopProbe probe(GatherGoal::ObtainMaterial);
    probe.afterSnapshot.interactionAttempted = true;
    probe.afterSnapshot.lootResponseObserved = true;
    probe.afterSnapshot.lootProcessingObserved = true;
    probe.afterSnapshot.sourceMatched = true;
    probe.afterSnapshot.sourceLootGenerationObserved = true;
    probe.afterSnapshot.sourceYieldsRequestedMaterial = true;
    probe.afterSnapshot.sourceLootConsumed = true;
    probe.afterSnapshot.materialBefore = 7;
    probe.afterSnapshot.materialAfter = 7;
    probe.afterSnapshot.skillBefore = 125;
    probe.afterSnapshot.skillAfter = 126;

    DispatchResult const result = OracleGatherExecutor::Dispatch(
        Request(GatherGoal::ObtainMaterial), Lease(GatherGoal::ObtainMaterial),
        Candidate(GatherGoal::ObtainMaterial), probe.context.before, &CanonicalReceiptLoopProbe::Step,
        &probe.context);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Progressing);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::AwaitingCredit);
    EXPECT_TRUE(result.credit.materialFulfillmentRequired);
    EXPECT_FALSE(result.credit.matchingMaterialDelta);
    EXPECT_TRUE(result.credit.matchingSkillDelta);
}

TEST(OracleGatherExecutorTest, SourceMaterialProofIsRequiredEvenWithInventoryDelta)
{
    NativeProbe probe;
    probe.credit.interactionAttempted = true;
    probe.credit.sourceMatched = true;
    probe.credit.sourceLootGenerationObserved = true;
    probe.credit.matchingMaterialDelta = true;

    DispatchResult const result = Dispatch(probe);
    ASSERT_TRUE(result.accepted);
    EXPECT_NE(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.creditDecision, AutoWowGather::GatherCreditDecision::AwaitingCredit);
}

TEST(OracleGatherExecutorTest, WorkerAdapterReceivesOnlyExactCandidateWithoutFallback)
{
    FakeWorkerNativeAdapter adapter;
    adapter.fallback = Candidate();
    adapter.fallback.reference = GatherReference(kSpawn + 1);

    DispatchResult const result = WorkerGatherAction::ExecuteFixedTargetGather(
        Request(), Lease(), Observation().world, Observation().nodeReservation, Candidate(),
        &FakeWorkerNativeAdapter::Step, &adapter);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(adapter.calls, 1);
    EXPECT_EQ(adapter.maxSteps, 1);
    EXPECT_TRUE(adapter.exactHandoff);
    EXPECT_FALSE(adapter.fallbackUsed);
    EXPECT_TRUE(ExactCandidateMatches(Candidate(), adapter.requested));
    EXPECT_EQ(result.after.world.ownershipProof.decisionId, kDecision);
    EXPECT_EQ(result.after.nodeReservation.arbiterReceipt.gather.spawnId, kSpawn);
    EXPECT_TRUE(ExactCandidateMatches(Candidate(), result.after.candidate));
}

TEST(OracleGatherExecutorTest, RejectsEveryCandidateMismatchBeforeNative)
{
    auto expectRejected = [](FixedTargetObservation const& observation,
                             FixedTargetReason reason)
    {
        NativeProbe probe;
        DispatchResult const result = Dispatch(probe, observation);
        EXPECT_FALSE(result.accepted);
        EXPECT_EQ(result.reason, reason);
        EXPECT_EQ(probe.calls, 0);
    };

    FixedTargetObservation observation = Observation();
    observation.candidate.reference.spawnId++;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);
    observation = Observation();
    observation.candidate.reference.entry++;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);
    observation = Observation();
    observation.candidate.reference.mapId++;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);
    observation = Observation();
    observation.candidate.reference.materialItemId++;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);

    observation = Observation();
    observation.candidate.facts.alive = false;
    expectRejected(observation, FixedTargetReason::CandidateFactsMismatch);
    observation = Observation();
    observation.candidate.facts.available = false;
    expectRejected(observation, FixedTargetReason::CandidateFactsMismatch);
    observation = Observation();
    observation.candidate.facts.reachable = false;
    expectRejected(observation, FixedTargetReason::CandidateFactsMismatch);
    observation = Observation();
    observation.candidate.facts.profession = AutoWowGather::Profession::Herbalism;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);
    observation = Observation();
    observation.candidate.facts.toolRequired = false;
    expectRejected(observation, FixedTargetReason::CandidateMismatch);
    observation = Observation();
    observation.candidate.facts.toolAvailable = false;
    expectRejected(observation, FixedTargetReason::CandidateFactsMismatch);
    observation = Observation();
    observation.candidate.facts.sourceYieldsRequestedMaterial = false;
    expectRejected(observation, FixedTargetReason::CandidateFactsMismatch);
}

TEST(OracleGatherExecutorTest, RequiresFreshOwnershipAndAcceptedNodeReservation)
{
    FixedTargetObservation missingOwnership = Observation();
    missingOwnership.world.ownershipProof.active = false;
    NativeProbe probe;
    DispatchResult result = Dispatch(probe, missingOwnership);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::MissingOwnershipProof);

    FixedTargetObservation preempted = Observation();
    preempted.world.ownershipProof.decisionId++;
    result = Dispatch(probe, preempted);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::LeasePreempted);

    FixedTargetObservation expired = Observation();
    expired.world.tick = kExpiry;
    result = Dispatch(probe, expired);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::LeaseExpired);

    FixedTargetObservation missingReservation = Observation();
    missingReservation.nodeReservation.arbiterReceipt.status = ReceiptStatus::Rejected;
    result = Dispatch(probe, missingReservation);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::ReservationMissing);

    FixedTargetObservation wrongReservation = Observation();
    wrongReservation.nodeReservation.arbiterReceipt.gather.entry++;
    result = Dispatch(probe, wrongReservation);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::ReservationMismatch);

    FixedTargetObservation expiredReservation = Observation();
    expiredReservation.nodeReservation.expiresTick = expiredReservation.world.tick;
    result = Dispatch(probe, expiredReservation);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::ReservationExpired);
    EXPECT_EQ(probe.calls, 0);
}

TEST(OracleGatherExecutorTest, RevalidatesPreemptionExpiryAndReservationLossAfterNativeStep)
{
    NativeProbe probe;
    probe.after.world.ownershipProof.decisionId++;
    DispatchResult result = Dispatch(probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::LeasePreempted);
    EXPECT_TRUE(result.nativeCalled);

    probe = NativeProbe{};
    probe.after.world.tick = kExpiry;
    result = Dispatch(probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::LeaseExpired);

    probe = NativeProbe{};
    probe.after.nodeReservation.arbiterReceipt.status = ReceiptStatus::Rejected;
    result = Dispatch(probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::ReservationMissing);
}

TEST(OracleGatherExecutorTest, RefusesFallbackOrPostStepCandidateSubstitution)
{
    NativeProbe fallback;
    fallback.selectedAnotherCandidate = true;
    DispatchResult result = Dispatch(fallback);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::NoFallback);

    NativeProbe substituted;
    substituted.after.candidate.reference.spawnId++;
    result = Dispatch(substituted);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::CandidateMismatch);
    EXPECT_EQ(substituted.calls, 1);
}

TEST(OracleGatherExecutorTest, NativeFailureAndOverBudgetNeverBecomeSuccess)
{
    NativeProbe rejected;
    rejected.accept = false;
    DispatchResult result = Dispatch(rejected);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, FixedTargetReason::NativeRejected);

    NativeProbe overBudget;
    overBudget.after = PostObservation();
    auto overBudgetStep = [](void*, NativeStepRequest const& request)
    {
        NativeStepObservation observation;
        observation.dispatchAccepted = true;
        observation.stepsInvoked = 2;
        observation.after.world = Observation().world;
        observation.after.nodeReservation = request.nodeReservation;
        observation.after.candidate = request.candidate;
        observation.after.world.tick = kIssued + 2;
        observation.after.world.frameVersion = kFrame + 1;
        return observation;
    };
    DispatchResult overBudgetResult = OracleGatherExecutor::Dispatch(
        Request(), Lease(), Candidate(), Observation(), overBudgetStep);
    EXPECT_FALSE(overBudgetResult.accepted);
    EXPECT_EQ(overBudgetResult.reason, FixedTargetReason::StepBudgetExceeded);
}

} // namespace
