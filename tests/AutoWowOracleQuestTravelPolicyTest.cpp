#include "../src/AutoWow/AutoWowOracleQuestTravelPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleQuestTravel;

Endpoint At(std::uint32_t map, double x = 1.0)
{
    return {map, 0, x, 2.0, 3.0, true};
}

ActorFacts Actor(Guid guid, std::uint32_t entry, Endpoint endpoint, bool ready)
{
    return {guid, entry, endpoint, true, true, true, true, true, ready};
}

QuestTravelFrame MakeFrame()
{
    QuestTravelFrame frame;
    frame.version = 10;
    frame.tick = 100;
    frame.epoch = 3;
    frame.bot = {42, true, false, 1000, At(1)};
    frame.quest.questId = 792;
    frame.quest.known = true;
    frame.quest.available = true;
    frame.quest.giver = Actor(7001, 3001, At(1, 4.0), true);
    frame.quest.finisher = Actor(7002, 3002, At(1, 5.0), true);
    frame.quest.capability.hasNpcObjective = true;
    frame.objective = {true, true, {792, ObjectiveFamily::NpcOrGameObject, 0},
        ObjectiveKind::CreatureCredit, 3101, 0, 0, 3, 0, 0};
    return frame;
}

ObjectiveSourceFact Source(std::uint32_t entry, Guid guid, Endpoint endpoint, bool inRange)
{
    return {entry, guid, endpoint, true, true, true, true, true, inRange, true};
}

TravelTransitionFact Walk(Endpoint source, Endpoint destination, std::uint64_t id)
{
    return {id, TravelKind::WalkingApproach, source, destination, 1, 0, true, false,
        false, false, false, false, false};
}

TravelTransitionFact CrossMap(Endpoint source, Endpoint destination, std::uint64_t id)
{
    return {id, TravelKind::AreaTrigger, source, destination, 1, 0, true, true,
        true, false, false, false, false};
}

WorldReadFrame Shared(QuestTravelFrame const& frame)
{
    WorldReadFrame shared;
    shared.version = frame.version;
    shared.tick = frame.tick;
    shared.epoch = frame.epoch;
    shared.scope = {ScopeKind::LabFixture, 77, frame.bot.botGuid};
    shared.bot = {frame.bot.botGuid, frame.bot.alive, frame.bot.inCombat,
        frame.bot.position.mapId, frame.bot.position.instanceId};
    return shared;
}

void ExpectPlanningOnlyBlocked(QuestTravelFrame const& frame, IntentKind kind, Domain domain,
    IntentCode intent, LeaseResource resource, AutoWowOracle::OperationCode operation,
    Guid targetGuid)
{
    QuestTravelPlanResult planned = PlanQuestTravel(frame);
    ASSERT_TRUE(planned.hasCandidate);
    EXPECT_EQ(planned.kind, kind);
    EXPECT_EQ(planned.candidate.domain, domain);
    EXPECT_EQ(planned.candidate.intent, intent);
    EXPECT_EQ(planned.candidate.resource, resource);
    EXPECT_EQ(planned.candidate.operation, operation);
    EXPECT_EQ(planned.candidate.targetGuid, targetGuid);
    EXPECT_FALSE(planned.candidate.executorAvailable);

    WorldReadFrame shared = Shared(frame);
    ASSERT_TRUE(AddQuestTravelCandidate(shared, frame));
    PlanResult const oraclePlan = Plan(shared);
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(oraclePlan.receipt.domain, domain);
    EXPECT_EQ(oraclePlan.receipt.resource, resource);
    EXPECT_EQ(oraclePlan.receipt.operation, operation);
    EXPECT_FALSE(oraclePlan.receipt.executorAvailable);
    EXPECT_EQ(oraclePlan.receipt.quest.questId, frame.quest.questId);

    Decision const blockedDecision = MakeDecision(shared, planned.candidate);
    ASSERT_TRUE(blockedDecision.valid);
    OracleArbiter<> arbiter;
    LeaseResult const blockedLease = arbiter.Acquire(shared, blockedDecision);
    EXPECT_FALSE(blockedLease.hasLease);
    EXPECT_EQ(blockedLease.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
}

TEST(AutoWowOracleQuestTravelPolicyTest, PublishesSharedQuestPlanStagesAndArbiterLease)
{
    QuestTravelFrame frame = MakeFrame();
    ExpectPlanningOnlyBlocked(frame, IntentKind::AcquireQuest, Domain::Quest,
        IntentCode::QuestObjective, LeaseResource::QuestGather,
        AutoWowOracle::OperationCode::QuestAcquire, frame.quest.giver.guid);

    frame.quest.offerSelected = true;
    frame.quest.canAccept = true;
    ExpectPlanningOnlyBlocked(frame, IntentKind::AcceptQuest, Domain::Quest,
        IntentCode::QuestObjective, LeaseResource::QuestGather,
        AutoWowOracle::OperationCode::QuestAccept, frame.quest.giver.guid);

    frame.quest.active = true;
    ASSERT_TRUE(AddObjectiveSource(frame, Source(9999, 8100, At(1, 2.0), true)));
    ASSERT_TRUE(AddObjectiveSource(frame, Source(3101, 8102, At(1, 6.0), true)));
    QuestTravelPlanResult objective = PlanQuestTravel(frame);
    ASSERT_TRUE(objective.hasCandidate);
    EXPECT_EQ(objective.kind, IntentKind::Objective);
    EXPECT_EQ(objective.candidate.targetGuid, 8102U);
    EXPECT_EQ(objective.candidate.itemId, 0U);
    EXPECT_EQ(objective.candidate.operation, AutoWowOracle::OperationCode::QuestObjective);
    EXPECT_TRUE(objective.candidate.executorAvailable);
    EXPECT_EQ(objective.candidate.quest.objectiveFamily,
        AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject);
    EXPECT_EQ(objective.progress.referenceCount, 0U);

    WorldReadFrame objectiveShared = Shared(frame);
    ASSERT_TRUE(AddQuestTravelCandidate(objectiveShared, frame));
    PlanResult const objectivePlan = Plan(objectiveShared);
    ASSERT_TRUE(objectivePlan.hasDecision);
    EXPECT_EQ(objectivePlan.decision.operation, AutoWowOracle::OperationCode::QuestObjective);
    EXPECT_TRUE(objectivePlan.decision.executorAvailable);
    OracleArbiter<> objectiveArbiter;
    LeaseResult const objectiveLease = objectiveArbiter.Acquire(
        objectiveShared, objectivePlan.decision);
    ASSERT_TRUE(objectiveLease.hasLease);
    EXPECT_EQ(objectiveLease.receipt.status, AutoWowOracle::ReceiptStatus::Accepted);
    EXPECT_EQ(objectiveLease.receipt.reason, AutoWowOracle::ReceiptReason::LeaseAcquired);
    EXPECT_EQ(objectiveArbiter.ActiveBotLeaseCount(), 1U);
    EXPECT_EQ(objectiveArbiter.Release(objectiveShared, objectiveLease.lease).status,
        AutoWowOracle::ReceiptStatus::Completed);
    EXPECT_EQ(objectiveArbiter.ActiveBotLeaseCount(), 0U);

    frame.objective.currentCount = 3;
    frame.objective.lastObservedCount = 1;
    frame.quest.complete = true;
    frame.quest.canTurnIn = true;
    QuestTravelPlanResult turnIn = PlanQuestTravel(frame);
    ASSERT_TRUE(turnIn.hasCandidate);
    EXPECT_TRUE(turnIn.progress.complete);
    ExpectPlanningOnlyBlocked(frame, IntentKind::TurnInQuest, Domain::Quest,
        IntentCode::QuestObjective, LeaseResource::QuestGather,
        AutoWowOracle::OperationCode::QuestTurnIn, frame.quest.finisher.guid);

    frame = MakeFrame();
    frame.quest.giver.interactionReady = false;
    ASSERT_TRUE(AddTravelTransition(frame, Walk(frame.bot.position,
        frame.quest.giver.endpoint, 88)));
    ExpectPlanningOnlyBlocked(frame, IntentKind::Travel, Domain::Navigation,
        IntentCode::Navigate, LeaseResource::Transition,
        AutoWowOracle::OperationCode::Navigate, frame.quest.giver.guid);

    frame = MakeFrame();
    frame.quest.active = true;
    frame.quest.capability.hasGameObjectObjective = true;
    frame.quest.capability.hasNpcObjective = false;
    ExpectPlanningOnlyBlocked(frame, IntentKind::Recover, Domain::Recovery,
        IntentCode::Recover, LeaseResource::Recovery,
        AutoWowOracle::OperationCode::Recover, 0U);
}

TEST(AutoWowOracleQuestTravelPolicyTest, HighWaterExactMatchingAndBoundedBackoffAreStable)
{
    ObjectiveFacts objective;
    objective.present = true;
    objective.supported = true;
    objective.key = {792, ObjectiveFamily::Item, 1};
    objective.requiredCount = 5;
    objective.baselineCount = 4;
    objective.lastObservedCount = 4;
    objective.currentCount = 3;
    ObjectiveProgress regressed = EvaluateObjectiveProgress(objective);
    EXPECT_TRUE(regressed.regressed);
    EXPECT_FALSE(regressed.progressed);
    objective.currentCount = 6;
    ObjectiveProgress advanced = EvaluateObjectiveProgress(objective);
    EXPECT_TRUE(advanced.progressed);
    EXPECT_EQ(advanced.positiveDelta, 2U);

    QuestTravelFrame frame = MakeFrame();
    frame.quest.active = true;
    frame.objective.key = {792, ObjectiveFamily::Item, 1};
    frame.objective.kind = ObjectiveKind::CollectItem;
    frame.objective.requiredEntry = 0;
    frame.objective.requiredItemId = 444;
    ASSERT_TRUE(AddObjectiveSource(frame, Source(443, 8101, At(1), true)));
    ASSERT_TRUE(AddObjectiveSource(frame, Source(444, 8102, At(1), true)));
    QuestTravelPlanResult exact = PlanQuestTravel(frame);
    ASSERT_TRUE(exact.hasCandidate);
    EXPECT_EQ(exact.candidate.targetGuid, 8102U);
    EXPECT_EQ(exact.candidate.itemId, 444U);

    EXPECT_EQ(BackoffTicks(0), 2U);
    EXPECT_EQ(BackoffTicks(99), kMaximumBackoffTicks);
    frame.retry = {true, FailureKind::ObjectiveNoProgress, 2, 100, 0};
    frame.tick = 107;
    QuestTravelPlanResult waiting = PlanQuestTravel(frame);
    ASSERT_TRUE(waiting.hasCandidate);
    EXPECT_EQ(waiting.kind, IntentKind::Recover);
    EXPECT_EQ(waiting.recovery, RecoveryReason::Backoff);
    EXPECT_EQ(waiting.retryAfterTick, 108U);
}

TEST(AutoWowOracleQuestTravelPolicyTest, RecoveryAndCapabilityStatesRemainValueOnly)
{
    QuestTravelFrame frame = MakeFrame();
    frame.quest.active = true;
    frame.quest.capability.hasGameObjectObjective = true;
    frame.quest.capability.hasNpcObjective = false;
    QuestTravelPlanResult unsupported = PlanQuestTravel(frame);
    ASSERT_TRUE(unsupported.hasCandidate);
    EXPECT_EQ(unsupported.kind, IntentKind::Recover);
    EXPECT_EQ(unsupported.recovery, RecoveryReason::CapabilityUnsupported);
    EXPECT_EQ(unsupported.candidate.domain, Domain::Recovery);
    EXPECT_EQ(unsupported.candidate.resource, LeaseResource::Recovery);
    EXPECT_FALSE(unsupported.candidate.executorAvailable);

    frame = MakeFrame();
    frame.quest.giver.interactionReady = false;
    frame.quest.active = false;
    ASSERT_TRUE(AddTravelTransition(frame, Walk(frame.bot.position, frame.quest.giver.endpoint, 88)));
    QuestTravelPlanResult sameMap = PlanQuestTravel(frame);
    ASSERT_TRUE(sameMap.hasCandidate);
    EXPECT_EQ(sameMap.kind, IntentKind::Travel);
    EXPECT_EQ(sameMap.candidate.domain, Domain::Navigation);
    EXPECT_EQ(sameMap.candidate.resource, LeaseResource::Transition);
    EXPECT_FALSE(sameMap.candidate.executorAvailable);
    EXPECT_TRUE(sameMap.candidate.requiresSameMap);
    EXPECT_FALSE(sameMap.candidate.transition.required);

    frame.quest.giver.endpoint = At(2, 4.0);
    frame.transitionCount = 0;
    ASSERT_TRUE(AddTravelTransition(frame, CrossMap(frame.bot.position,
        frame.quest.giver.endpoint, 77)));
    QuestTravelPlanResult crossMap = PlanQuestTravel(frame);
    ASSERT_TRUE(crossMap.hasCandidate);
    EXPECT_EQ(crossMap.kind, IntentKind::Travel);
    EXPECT_FALSE(crossMap.candidate.requiresSameMap);
    EXPECT_TRUE(crossMap.candidate.transition.required);
    EXPECT_EQ(crossMap.candidate.transition.stableId, 77U);
    EXPECT_TRUE(crossMap.candidate.transition.directedProof);
    EXPECT_FALSE(crossMap.candidate.executorAvailable);
    EXPECT_EQ(crossMap.capability.kind, CapabilityClass::Kill);
    EXPECT_TRUE(crossMap.capability.supported);
    EXPECT_TRUE(crossMap.progress.present);
    EXPECT_EQ(crossMap.progress.requiredCount, 3U);

    WorldReadFrame shared = Shared(frame);
    ASSERT_TRUE(AddQuestTravelCandidate(shared, frame));
    PlanResult plan = Plan(shared);
    EXPECT_FALSE(plan.hasDecision);
    EXPECT_EQ(plan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(plan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(plan.receipt.operation, AutoWowOracle::OperationCode::Navigate);
    OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);

    frame.transitions[0].directedProof = false;
    QuestTravelPlanResult blocked = PlanQuestTravel(frame);
    ASSERT_TRUE(blocked.hasCandidate);
    EXPECT_EQ(blocked.kind, IntentKind::Recover);
    EXPECT_EQ(blocked.recovery, RecoveryReason::CrossMapRouteUnavailable);
    EXPECT_EQ(blocked.capability.kind, CapabilityClass::Kill);
    EXPECT_TRUE(blocked.capability.supported);
    EXPECT_TRUE(blocked.progress.present);
    EXPECT_EQ(blocked.progress.requiredCount, 3U);

}
}  // namespace
