#include "OracleQuestExecutor.h"
#include "AutoWowOracleZoneTravelAssistPolicy.h"

#include "gtest/gtest.h"

#include <type_traits>

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleExecutor;
using namespace AutoWowOracleQuestExecutor;

constexpr Guid kBot = 42;
constexpr SquadId kSquad = 77;
constexpr QuestId kQuest = 9001;
constexpr Guid kTarget = 0xabc;
constexpr std::uint32_t kMap = 530;
constexpr std::uint32_t kEntry = 1234;
constexpr ItemId kItem = 5678;
constexpr DecisionId kDecision = 101;
constexpr IntentId kIntent = 102;
constexpr Epoch kEpoch = 7;
constexpr FactVersion kFrame = 11;
constexpr Tick kTick = 100;
constexpr Tick kExpiry = 110;

ActiveLeaseSnapshot Ownership(DecisionId decision = kDecision, IntentId intent = kIntent,
                              Epoch epoch = kEpoch, Tick expires = kExpiry)
{
    ActiveLeaseSnapshot proof;
    proof.active = true;
    proof.botGuid = kBot;
    proof.decisionId = decision;
    proof.intentId = intent;
    proof.epoch = epoch;
    proof.expiresTick = expires;
    proof.scope = {ScopeKind::LabFixture, kSquad, kBot};
    proof.operation = OperationCode::QuestObjective;
    return proof;
}

ExecutorRequest MakeRequest(QuestObjectiveFamily family = QuestObjectiveFamily::NpcOrGameObject)
{
    ExecutorRequest request;
    request.valid = true;
    request.requestId = 90001;
    request.operation = OperationCode::QuestObjective;
    request.domain = Domain::Quest;
    request.intent = IntentCode::QuestObjective;
    request.resource = LeaseResource::QuestGather;
    request.scope = {ScopeKind::LabFixture, kSquad, kBot};
    request.decisionId = kDecision;
    request.intentId = kIntent;
    request.actorGuid = kBot;
    request.targetGuid = kTarget;
    request.itemId = family == QuestObjectiveFamily::Item ? kItem : 0;
    request.objectiveId = 0;
    request.executorAvailable = true;
    request.quest = {true, kQuest, family, 2,
                     family == QuestObjectiveFamily::NpcOrGameObject ? kEntry : 0,
                     family == QuestObjectiveFamily::Item ? kItem : 0};
    request.epoch = kEpoch;
    request.frameVersion = kFrame;
    request.issuedTick = kTick;
    request.expiresTick = kExpiry;
    request.ttlTicks = 10;
    request.ownershipProof = Ownership();
    return request;
}

QuestObjectiveObservation MakeObservation(ExecutorRequest const& request,
                                          QuestObjectiveFamily family =
                                              QuestObjectiveFamily::NpcOrGameObject)
{
    QuestObjectiveObservation observation;
    observation.world.onWorldThread = true;
    observation.world.botAlive = true;
    observation.world.botGuid = kBot;
    observation.world.mapId = kMap;
    observation.world.instanceId = 1;
    observation.world.tick = kTick + 1;
    observation.world.frameVersion = kFrame;
    observation.world.epoch = kEpoch;
    observation.world.ownership = Ownership();

    observation.objective.valid = true;
    observation.objective.botGuid = kBot;
    observation.objective.questId = kQuest;
    observation.objective.family = family;
    observation.objective.slot = 2;
    observation.objective.requiredEntry =
        family == QuestObjectiveFamily::NpcOrGameObject ? kEntry : 0;
    observation.objective.requiredItemId = family == QuestObjectiveFamily::Item ? kItem : 0;
    observation.objective.targetGuid = request.targetGuid;
    observation.objective.targetMapId = kMap;
    observation.objective.targetMapApplicable = true;
    observation.objective.currentCount = 1;
    observation.objective.requiredCount = 2;
    observation.objective.phase = ObjectiveStepPhase::EngageTarget;
    return observation;
}

IntentLease MakeLease()
{
    Decision decision;
    decision.valid = true;
    decision.decisionId = kDecision;
    decision.intentId = kIntent;
    decision.domain = Domain::Quest;
    decision.intent = IntentCode::QuestObjective;
    decision.resource = LeaseResource::QuestGather;
    decision.scope = {ScopeKind::LabFixture, kSquad, kBot};
    decision.ttlTicks = 10;
    decision.issuedTick = kTick;
    decision.expiresTick = kExpiry;
    decision.epoch = kEpoch;
    decision.targetGuid = kTarget;
    decision.executorAvailable = true;
    decision.operation = OperationCode::QuestObjective;
    decision.quest = {true, kQuest, QuestObjectiveFamily::NpcOrGameObject, 2, kEntry, 0};
    decision.preconditions.frameVersion = kFrame;
    decision.actorGuid = kBot;
    decision.evidence = {};
    return {true, decision};
}

NativeStepObservation SuccessfulStep(void*, NativeObjectiveStepRequest const& request)
{
    NativeStepObservation result;
    result.dispatchAccepted = true;
    result.stepsInvoked = 1;
    result.after.world.onWorldThread = true;
    result.after.world.botAlive = true;
    result.after.world.botGuid = kBot;
    result.after.world.mapId = kMap;
    result.after.world.instanceId = 1;
    result.after.world.tick = kTick + 2;
    result.after.world.frameVersion = kFrame + 1;
    result.after.world.epoch = kEpoch;
    result.after.world.ownership = request.request.ownershipProof;
    result.after.objective = request.objectiveBefore;
    result.after.objective.currentCount++;
    return result;
}

TEST(OracleQuestExecutorTest, UsesCurrentSharedRequestAndExactObjectiveIdentity)
{
    static_assert(std::is_same_v<decltype(ExecutorRequest{}.operation), OperationCode>);
    static_assert(std::is_same_v<decltype(IntentLease{}.decision), Decision>);

    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation const observation = MakeObservation(request);

    AutoWowOracleQuestExecutor::ValidationResult const validation =
        OracleQuestExecutor::Validate(request, observation);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(validation.reason, QuestObjectiveReason::None);

    PreparedObjective const prepared = OracleQuestExecutor::Prepare(request, observation);
    ASSERT_TRUE(prepared.valid);
    ASSERT_TRUE(prepared.accepted);
    EXPECT_EQ(prepared.request.operation, OperationCode::QuestObjective);
    EXPECT_EQ(prepared.request.quest.questId, kQuest);
    EXPECT_EQ(prepared.request.quest.objectiveFamily, QuestObjectiveFamily::NpcOrGameObject);
    EXPECT_EQ(prepared.request.quest.objectiveSlot, 2);
    EXPECT_EQ(prepared.objectiveRuntime.objective.requiredEntry, kEntry);
    EXPECT_EQ(prepared.objectiveRuntime.targetGuid, kTarget);
    EXPECT_EQ(prepared.objectiveRuntime.targetMapId, kMap);
    EXPECT_EQ(prepared.objectiveRuntime.maxSteps, 1);
    EXPECT_TRUE(prepared.objectiveRuntime.stopAfterStep);
}

TEST(OracleQuestExecutorTest, AcceptsItemObjectiveWithExactItemIdentity)
{
    ExecutorRequest const request = MakeRequest(QuestObjectiveFamily::Item);
    QuestObjectiveObservation const observation = MakeObservation(request,
                                                                  QuestObjectiveFamily::Item);

    AutoWowOracleQuestExecutor::ValidationResult const validation =
        OracleQuestExecutor::Validate(request, observation);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(observation.objective.requiredItemId, kItem);
    EXPECT_EQ(observation.objective.requiredEntry, 0u);
}

TEST(OracleQuestExecutorTest, RejectsEveryObjectiveIdentityMismatch)
{
    ExecutorRequest const request = MakeRequest();

    QuestObjectiveObservation wrongQuest = MakeObservation(request);
    wrongQuest.objective.questId++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongQuest).reason,
              QuestObjectiveReason::QuestIdMismatch);

    QuestObjectiveObservation wrongFamily = MakeObservation(request);
    wrongFamily.objective.family = QuestObjectiveFamily::Item;
    wrongFamily.objective.requiredEntry = 0;
    wrongFamily.objective.requiredItemId = kItem;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongFamily).reason,
              QuestObjectiveReason::FamilyMismatch);

    QuestObjectiveObservation wrongSlot = MakeObservation(request);
    wrongSlot.objective.slot++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongSlot).reason,
              QuestObjectiveReason::SlotMismatch);

    QuestObjectiveObservation wrongEntry = MakeObservation(request);
    wrongEntry.objective.requiredEntry++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongEntry).reason,
              QuestObjectiveReason::RequiredEntryMismatch);

    ExecutorRequest itemRequest = MakeRequest(QuestObjectiveFamily::Item);
    QuestObjectiveObservation wrongItem = MakeObservation(itemRequest,
                                                           QuestObjectiveFamily::Item);
    wrongItem.objective.requiredItemId++;
    EXPECT_EQ(OracleQuestExecutor::Validate(itemRequest, wrongItem).reason,
              QuestObjectiveReason::RequiredItemMismatch);

    QuestObjectiveObservation wrongTarget = MakeObservation(request);
    wrongTarget.objective.targetGuid++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongTarget).reason,
              QuestObjectiveReason::TargetGuidMismatch);

    QuestObjectiveObservation wrongMap = MakeObservation(request);
    wrongMap.objective.targetMapId++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, wrongMap).reason,
              QuestObjectiveReason::TargetMapMismatch);
}

TEST(OracleQuestExecutorTest, RejectsAcquireStartAcceptTurnInFinisherAndRecovery)
{
    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation observation = MakeObservation(request);

    for (OperationCode operation : {OperationCode::QuestAcquire, OperationCode::QuestAccept,
                                    OperationCode::QuestTurnIn, OperationCode::Recover})
    {
        ExecutorRequest rejected = request;
        rejected.operation = operation;
        QuestObjectiveReason const reason = OracleQuestExecutor::Validate(rejected, observation).reason;
        if (operation == OperationCode::QuestAcquire)
            EXPECT_EQ(reason, QuestObjectiveReason::QuestAcquireRejected);
        else if (operation == OperationCode::QuestAccept)
            EXPECT_EQ(reason, QuestObjectiveReason::QuestAcceptRejected);
        else if (operation == OperationCode::QuestTurnIn)
            EXPECT_EQ(reason, QuestObjectiveReason::QuestTurnInRejected);
        else
            EXPECT_EQ(reason, QuestObjectiveReason::RecoveryRejected);
    }

    observation.objective.phase = ObjectiveStepPhase::StartQuest;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::QuestStartRejected);
    observation.objective.phase = ObjectiveStepPhase::Finisher;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::FinisherRejected);
    observation.objective.phase = ObjectiveStepPhase::Recovery;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::RecoveryRejected);
}

TEST(OracleQuestExecutorTest, RejectsMissingExpiredStaleAndPreemptedOwnership)
{
    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation observation = MakeObservation(request);

    observation.world.ownership.active = false;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::MissingOwnership);

    observation = MakeObservation(request);
    observation.world.tick = request.expiresTick;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::LeaseExpired);

    observation = MakeObservation(request);
    observation.world.frameVersion++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::StaleFrame);

    observation = MakeObservation(request);
    observation.world.epoch++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::StaleEpoch);

    observation = MakeObservation(request);
    observation.world.ownership.decisionId++;
    EXPECT_EQ(OracleQuestExecutor::Validate(request, observation).reason,
              QuestObjectiveReason::LeasePreempted);
}

TEST(OracleQuestExecutorTest, PrepareFromIntentLeaseUsesSharedPreparationGate)
{
    IntentLease const lease = MakeLease();
    ExecutorRequest request = MakeRequest();
    QuestObjectiveObservation observation = MakeObservation(request);

    PreparedObjective const prepared = OracleQuestExecutor::Prepare(lease, observation);
    ASSERT_TRUE(prepared.valid);
    ASSERT_TRUE(prepared.accepted);
    EXPECT_EQ(prepared.request.operation, OperationCode::QuestObjective);

    IntentLease preempted = lease;
    preempted.decision.decisionId++;
    PreparedObjective const rejected = OracleQuestExecutor::Prepare(preempted, observation);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.reason, QuestObjectiveReason::LeasePreempted);
}

TEST(OracleQuestExecutorTest, DispatchCallsOneBoundedStepAndRevalidatesProgress)
{
    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation const before = MakeObservation(request);
    PreparedObjective const prepared = OracleQuestExecutor::Prepare(request, before);
    ASSERT_TRUE(prepared.accepted);

    DispatchResult const dispatched = OracleQuestExecutor::Dispatch(
        prepared, before, &SuccessfulStep);
    ASSERT_TRUE(dispatched.valid);
    ASSERT_TRUE(dispatched.accepted);
    EXPECT_EQ(dispatched.status, ExecutorStatus::Completed);
    EXPECT_EQ(dispatched.stepsInvoked, 1);
    EXPECT_TRUE(dispatched.nativeCalled);
    EXPECT_TRUE(dispatched.beforeValidated);
    EXPECT_TRUE(dispatched.afterValidated);
    EXPECT_EQ(dispatched.receipt.operation, OperationCode::QuestObjective);
}

TEST(OracleQuestExecutorTest, RejectsPostDispatchMismatchPreemptionAndTransitions)
{
    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation const before = MakeObservation(request);
    PreparedObjective const prepared = OracleQuestExecutor::Prepare(request, before);
    ASSERT_TRUE(prepared.accepted);

    auto changedObjective = [](void*, NativeObjectiveStepRequest const& nativeRequest)
    {
        NativeStepObservation result = SuccessfulStep(nullptr, nativeRequest);
        result.after.objective.questId++;
        return result;
    };
    DispatchResult changed = OracleQuestExecutor::Dispatch(prepared, before, changedObjective);
    EXPECT_FALSE(changed.accepted);
    EXPECT_FALSE(changed.afterValidated);
    EXPECT_EQ(changed.reason, QuestObjectiveReason::QuestIdMismatch);

    auto preempted = [](void*, NativeObjectiveStepRequest const& nativeRequest)
    {
        NativeStepObservation result = SuccessfulStep(nullptr, nativeRequest);
        result.after.world.ownership.decisionId++;
        return result;
    };
    DispatchResult lost = OracleQuestExecutor::Dispatch(prepared, before, preempted);
    EXPECT_FALSE(lost.accepted);
    EXPECT_EQ(lost.reason, QuestObjectiveReason::LeasePreempted);

    auto transitioned = [](void*, NativeObjectiveStepRequest const& nativeRequest)
    {
        NativeStepObservation result = SuccessfulStep(nullptr, nativeRequest);
        result.transitionedToAnotherObjective = true;
        return result;
    };
    DispatchResult next = OracleQuestExecutor::Dispatch(prepared, before, transitioned);
    EXPECT_FALSE(next.accepted);
    EXPECT_EQ(next.reason, QuestObjectiveReason::AutomaticTransitionRejected);

    auto finisher = [](void*, NativeObjectiveStepRequest const& nativeRequest)
    {
        NativeStepObservation result = SuccessfulStep(nullptr, nativeRequest);
        result.after.objective.phase = ObjectiveStepPhase::Finisher;
        return result;
    };
    DispatchResult turnIn = OracleQuestExecutor::Dispatch(prepared, before, finisher);
    EXPECT_FALSE(turnIn.accepted);
    EXPECT_EQ(turnIn.reason, QuestObjectiveReason::AutomaticTransitionRejected);
}

TEST(OracleQuestExecutorTest, RejectsUnavailableAndOverBudgetNativeDispatch)
{
    ExecutorRequest const request = MakeRequest();
    QuestObjectiveObservation const before = MakeObservation(request);
    PreparedObjective const prepared = OracleQuestExecutor::Prepare(request, before);
    ASSERT_TRUE(prepared.accepted);

    EXPECT_EQ(OracleQuestExecutor::Dispatch(prepared, before, nullptr).reason,
              QuestObjectiveReason::NativeUnavailable);

    auto overBudget = [](void*, NativeObjectiveStepRequest const& nativeRequest)
    {
        NativeStepObservation result = SuccessfulStep(nullptr, nativeRequest);
        result.stepsInvoked = 2;
        return result;
    };
    DispatchResult result = OracleQuestExecutor::Dispatch(prepared, before, overBudget);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, QuestObjectiveReason::StepBudgetExceeded);
}


TEST(OracleQuestExecutorTest, ExactLiveBindFailureLeavesFiveMinuteWorkWindow)
{
    using namespace AutoWowOracleRoute;
    RouteLedgerKey key;
    key.actor = 123;
    key.questId = 8327;
    key.stableSpawn = 99;

    LedgerUpdate first = RecordBlocked({}, key, 100, RouteFailure::ExactLiveBindRequired);
    ASSERT_TRUE(first.accepted);
    EXPECT_EQ(first.nextRetryAt, 400U);
    EXPECT_FALSE(RetryAllowed(first.ledger, key, 399, false));
    EXPECT_TRUE(RetryAllowed(first.ledger, key, 400, false));

    LedgerUpdate second = RecordBlocked(first.ledger, key, 400,
                                        RouteFailure::ExactLiveBindRequired);
    ASSERT_TRUE(second.accepted);
    EXPECT_EQ(second.nextRetryAt, 700U);
    EXPECT_FALSE(second.requiresReentry);

    LedgerUpdate third = RecordBlocked(second.ledger, key, 700,
                                       RouteFailure::ExactLiveBindRequired);
    ASSERT_TRUE(third.accepted);
    EXPECT_EQ(third.nextRetryAt, 1300U);
    EXPECT_TRUE(third.requiresReentry);

    LedgerUpdate unrelated = RecordBlocked({}, key, 100, RouteFailure::Stalled);
    ASSERT_TRUE(unrelated.accepted);
    EXPECT_EQ(unrelated.nextRetryAt, 130U);
}

TEST(AutoWowOracleZoneTravelAssistPolicyTest, RequiresRecoverableFailureAndCurrentZone)
{
    using namespace AutoWowOracleZoneTravelAssist;
    Facts facts;
    facts.enabled = facts.managed = facts.taggedLease = facts.exactSpawn = true;
    facts.alive = facts.sameMapAndInstance = facts.sameZone = true;
    facts.validDestination = facts.outsideArrivalRadius = facts.cooldownReady = true;
    facts.failure = AutoWowOracleRoute::RouteFailure::Stalled;
    EXPECT_TRUE(ShouldAssist(facts));
    facts.sameZone = false; // Same map, different zone must still be walked.
    EXPECT_FALSE(ShouldAssist(facts));
    facts.sameZone = true;
    facts.sameMapAndInstance = false;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.sameMapAndInstance = true;
    facts.failure = AutoWowOracleRoute::RouteFailure::ExactLiveBindRequired;
    EXPECT_TRUE(ShouldAssist(facts));
    facts.failure = AutoWowOracleRoute::RouteFailure::IdentityDrift;
    EXPECT_FALSE(ShouldAssist(facts));
}

TEST(AutoWowOracleZoneTravelAssistPolicyTest, RejectsOrdinaryBotsAndActiveMovement)
{
    using namespace AutoWowOracleZoneTravelAssist;
    Facts facts;
    facts.enabled = facts.managed = facts.taggedLease = facts.exactSpawn = true;
    facts.alive = facts.sameMapAndInstance = facts.sameZone = true;
    facts.validDestination = facts.outsideArrivalRadius = facts.cooldownReady = true;
    facts.failure = AutoWowOracleRoute::RouteFailure::Stalled;
    facts.enabled = false;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.enabled = true;
    facts.managed = false; // Includes ordinary bots and the observer.
    EXPECT_FALSE(ShouldAssist(facts));
    facts.managed = true;
    facts.taggedLease = false;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.taggedLease = true;
    facts.moving = true;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.moving = false;
    facts.cooldownReady = false;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.cooldownReady = true;
    facts.inCombat = true;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.inCombat = false;
    facts.inFlight = true;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.inFlight = false;
    facts.onTransport = true;
    EXPECT_FALSE(ShouldAssist(facts));
    facts.onTransport = false;
    facts.teleporting = true;
    EXPECT_FALSE(ShouldAssist(facts));
}

TEST(AutoWowOracleZoneTravelAssistPolicyTest, TeleportRequiresExactLiveBindBeforeNativeWork)
{
    using namespace AutoWowOracleZoneTravelAssist;
    EXPECT_EQ(DecidePostTeleportBind(true, false, false, 0), BindDecision::Wait);
    EXPECT_EQ(DecidePostTeleportBind(false, false, false, 2), BindDecision::Wait);
    EXPECT_EQ(DecidePostTeleportBind(false, true, false, 2), BindDecision::Wait);
    EXPECT_EQ(DecidePostTeleportBind(false, true, true, 2), BindDecision::EnterNativePhase);
    EXPECT_EQ(DecidePostTeleportBind(false, false, false, kBindWaitSeconds), BindDecision::Block);
    EXPECT_EQ(DecidePostTeleportBind(true, false, false, kBindWaitSeconds), BindDecision::Block);
}

}  // namespace
