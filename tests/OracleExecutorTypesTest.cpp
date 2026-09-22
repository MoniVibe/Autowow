#include "../src/AutoWow/OracleExecutorTypes.h"

#include "gtest/gtest.h"

#include <type_traits>

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleExecutor;

WorldReadFrame MakeFrame(Guid botGuid = 42)
{
    WorldReadFrame frame;
    frame.version = 10;
    frame.tick = 100;
    frame.epoch = 3;
    frame.scope = {ScopeKind::LabFixture, 77, botGuid};
    frame.bot = {botGuid, true, false, 1, 2};
    frame.evidence = {2, 3, 4, 5, 6, 0, 7, 1};
    frame.combat.decisionKey = 1000;
    frame.combat.self = {botGuid, 1000, false};
    return frame;
}

OracleCandidate MakeQuestCandidate(Guid questId = 792, std::uint16_t priority = 100)
{
    OracleCandidate candidate;
    candidate.domain = Domain::Quest;
    candidate.intent = IntentCode::QuestObjective;
    candidate.resource = LeaseResource::QuestGather;
    candidate.priority = priority;
    candidate.ttlTicks = 5;
    candidate.targetGuid = questId;
    candidate.action = "quest.objective";
    candidate.executorAvailable = true;
    candidate.operation = OperationCode::QuestObjective;
    candidate.quest = {true, static_cast<QuestId>(questId), QuestObjectiveFamily::NpcOrGameObject,
        0, 1, 0};
    return candidate;
}

Decision PlanQuest(WorldReadFrame& frame, Guid questId = 792, std::uint16_t priority = 100)
{
    bool const added = AddCandidate(frame, MakeQuestCandidate(questId, priority));
    EXPECT_TRUE(added);
    if (!added)
        return {};
    PlanResult const plan = Plan(frame);
    EXPECT_TRUE(plan.hasDecision);
    if (!plan.hasDecision)
        return {};
    return plan.decision;
}
}

TEST(OracleExecutorTypesTest, UsesTheSharedTypedOperationAndReferences)
{
    static_assert(std::is_same_v<decltype(ExecutorRequest{}.operation), OperationCode>);
    static_assert(std::is_trivially_copyable_v<ExecutorRequest>);

    OracleArbiter<> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = PlanQuest(frame);
    LeaseResult const acquired = arbiter.Acquire(frame, decision);
    ASSERT_TRUE(acquired.hasLease);

    ActiveLeaseSnapshot const proof = arbiter.QueryActiveLease(frame.bot.guid, frame.tick);
    ExecutorResult const prepared = PrepareRequest(acquired.lease, proof, frame.tick);
    ASSERT_TRUE(prepared.valid);
    ASSERT_TRUE(prepared.accepted);
    ASSERT_TRUE(prepared.request.valid);
    EXPECT_EQ(prepared.request.operation, OperationCode::QuestObjective);
    EXPECT_EQ(prepared.request.quest.questId, 792U);
    EXPECT_EQ(prepared.request.quest.objectiveFamily, QuestObjectiveFamily::NpcOrGameObject);
    EXPECT_FALSE(prepared.request.gather.valid);
    EXPECT_EQ(prepared.request.ownershipProof.decisionId, decision.decisionId);
    EXPECT_EQ(prepared.request.ownershipProof.operation, OperationCode::QuestObjective);

    // The contract action label is diagnostic metadata only and cannot alter typed identity.
    Decision relabeled = decision;
    relabeled.action = "not-a-second-operation-vocabulary";
    ExecutorRequest const relabeledRequest = MakeRequest(
        IntentLease{true, relabeled}, proof, frame.tick);
    ASSERT_TRUE(relabeledRequest.valid);
    EXPECT_EQ(relabeledRequest.requestId, prepared.request.requestId);
    EXPECT_EQ(relabeledRequest.operation, prepared.request.operation);
}

TEST(OracleExecutorTypesTest, RejectsAbsentExpiredAndMissingOwnershipProofs)
{
    ExecutorResult const absent = PrepareRequest(IntentLease{}, ActiveLeaseSnapshot{}, 100);
    ASSERT_TRUE(absent.valid);
    EXPECT_FALSE(absent.accepted);
    EXPECT_EQ(absent.status, ExecutorStatus::Rejected);
    EXPECT_EQ(absent.reason, ExecutorReason::LeaseMissing);
    EXPECT_TRUE(absent.receipt.valid);

    OracleArbiter<> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision shortDecision = PlanQuest(frame, 793);
    shortDecision.ttlTicks = 1;
    shortDecision.expiresTick = frame.tick + 1;
    LeaseResult const shortLease = arbiter.Acquire(frame, shortDecision);
    ASSERT_TRUE(shortLease.hasLease);

    ActiveLeaseSnapshot const current = arbiter.QueryActiveLease(frame.bot.guid, frame.tick);
    ExecutorResult const missingProof = PrepareRequest(shortLease.lease,
        ActiveLeaseSnapshot{}, frame.tick);
    ASSERT_TRUE(missingProof.valid);
    EXPECT_FALSE(missingProof.accepted);
    EXPECT_EQ(missingProof.reason, ExecutorReason::MissingOwnershipProof);

    ExecutorResult const expired = PrepareRequest(shortLease.lease, current,
        shortDecision.expiresTick);
    ASSERT_TRUE(expired.valid);
    EXPECT_FALSE(expired.accepted);
    EXPECT_EQ(expired.status, ExecutorStatus::Rejected);
    EXPECT_EQ(expired.reason, ExecutorReason::LeaseExpired);
}

TEST(OracleExecutorTypesTest, RejectsMismatchedAndPreemptedOwnershipProofs)
{
    OracleArbiter<> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = PlanQuest(frame, 794, 100);
    LeaseResult const acquired = arbiter.Acquire(frame, decision);
    ASSERT_TRUE(acquired.hasLease);
    ActiveLeaseSnapshot const proof = arbiter.QueryActiveLease(frame.bot.guid, frame.tick);

    ActiveLeaseSnapshot mismatched = proof;
    mismatched.botGuid = 999;
    mismatched.scope.botGuid = 999;
    ExecutorResult const mismatch = PrepareRequest(acquired.lease, mismatched, frame.tick);
    ASSERT_TRUE(mismatch.valid);
    EXPECT_FALSE(mismatch.accepted);
    EXPECT_EQ(mismatch.status, ExecutorStatus::Rejected);
    EXPECT_EQ(mismatch.reason, ExecutorReason::LeaseMismatch);

    WorldReadFrame highFrame = MakeFrame();
    Decision const highDecision = PlanQuest(highFrame, 795, 500);
    LeaseResult const high = arbiter.Acquire(highFrame, highDecision);
    ASSERT_TRUE(high.hasLease);
    ActiveLeaseSnapshot const highProof = arbiter.QueryActiveLease(
        highFrame.bot.guid, highFrame.tick);
    ASSERT_TRUE(highProof.active);
    EXPECT_FALSE(arbiter.OwnsActiveLease(acquired.lease, highFrame.tick));
    EXPECT_TRUE(arbiter.OwnsActiveLease(high.lease, highFrame.tick));

    ExecutorResult const preempted = PrepareRequest(acquired.lease, highProof, frame.tick);
    ASSERT_TRUE(preempted.valid);
    EXPECT_FALSE(preempted.accepted);
    EXPECT_EQ(preempted.status, ExecutorStatus::Rejected);
    EXPECT_EQ(preempted.reason, ExecutorReason::LeasePreempted);
}

TEST(OracleExecutorTypesTest, RejectsUnknownAndContractBlockedOperations)
{
    OracleArbiter<> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = PlanQuest(frame, 796);
    LeaseResult const acquired = arbiter.Acquire(frame, decision);
    ASSERT_TRUE(acquired.hasLease);
    ActiveLeaseSnapshot const proof = arbiter.QueryActiveLease(frame.bot.guid, frame.tick);

    Decision unknown = decision;
    unknown.operation = static_cast<OperationCode>(0xff);
    ExecutorResult const unknownResult = PrepareRequest(IntentLease{true, unknown}, proof,
        frame.tick);
    ASSERT_TRUE(unknownResult.valid);
    EXPECT_FALSE(unknownResult.accepted);
    EXPECT_EQ(unknownResult.status, ExecutorStatus::Rejected);
    EXPECT_EQ(unknownResult.reason, ExecutorReason::UnknownOperation);

    Decision gather = decision;
    gather.operation = OperationCode::GatherSource;
    gather.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};
    gather.itemId = 600;
    ExecutorResult const blocked = PrepareRequest(IntentLease{true, gather}, proof, frame.tick);
    ASSERT_TRUE(blocked.valid);
    EXPECT_FALSE(blocked.accepted);
    EXPECT_EQ(blocked.status, ExecutorStatus::Blocked);
    EXPECT_EQ(blocked.reason, ExecutorReason::OperationBlocked);
    EXPECT_EQ(blocked.receipt.operation, OperationCode::GatherSource);
    EXPECT_EQ(blocked.receipt.gather.spawnId, 9001U);
    EXPECT_EQ(blocked.request.gatherGoal, GatherGoal::ObtainMaterial);
}

TEST(OracleExecutorTypesTest, ReceiptsCarryTypedIdentityAndEvidenceProof)
{
    OracleArbiter<> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = PlanQuest(frame, 797);
    LeaseResult const acquired = arbiter.Acquire(frame, decision);
    ASSERT_TRUE(acquired.hasLease);
    ActiveLeaseSnapshot const proof = arbiter.QueryActiveLease(frame.bot.guid, frame.tick);
    ExecutorRequest const request = MakeRequest(acquired.lease, proof, frame.tick);
    ASSERT_TRUE(request.valid);

    EvidenceCounters after = request.proofBaseline;
    after.progress += 2;
    ExecutorResult const progressing = MakeResult(request, ExecutorStatus::Progressing,
        ExecutorReason::None, proof, frame.tick, after, 1);
    ASSERT_TRUE(progressing.valid);
    ASSERT_TRUE(progressing.accepted);
    EXPECT_EQ(progressing.receipt.operation, OperationCode::QuestObjective);
    EXPECT_EQ(progressing.receipt.quest.questId, 797U);
    EXPECT_EQ(progressing.receipt.proof.delta.progress, 2);
    EXPECT_TRUE(ProofDeltaMatches(progressing.receipt.proof));
    EXPECT_TRUE(ValidateReceipt(request, progressing.receipt, proof, frame.tick).valid);

    ExecutorReceipt tampered = progressing.receipt;
    tampered.proof.delta.progress = 99;
    ValidationResult const invalid = ValidateReceipt(request, tampered, proof, frame.tick);
    EXPECT_FALSE(invalid.valid);
    EXPECT_EQ(invalid.reason, ExecutorReason::ProofMismatch);

    WorldReadFrame highFrame = MakeFrame();
    Decision const highDecision = PlanQuest(highFrame, 798, 500);
    ASSERT_TRUE(arbiter.Acquire(highFrame, highDecision).hasLease);
    ActiveLeaseSnapshot const highProof = arbiter.QueryActiveLease(
        highFrame.bot.guid, highFrame.tick);
    ValidationResult const lost = ValidateReceipt(request, progressing.receipt, highProof,
        highFrame.tick);
    EXPECT_FALSE(lost.valid);
    EXPECT_EQ(lost.reason, ExecutorReason::LeasePreempted);
}

TEST(OracleExecutorTypesTest, MapsSharedContractReceiptVocabulary)
{
    Receipt receipt;
    receipt.status = ReceiptStatus::Progressing;
    EXPECT_EQ(ToExecutorStatus(receipt), ExecutorStatus::Progressing);

    receipt.status = ReceiptStatus::Blocked;
    receipt.reason = ReceiptReason::ExecutorBlocked;
    EXPECT_EQ(ToExecutorStatus(receipt), ExecutorStatus::Blocked);
    EXPECT_EQ(ToExecutorReason(receipt), ExecutorReason::OperationBlocked);

    receipt.status = ReceiptStatus::Rejected;
    receipt.reason = ReceiptReason::Expired;
    EXPECT_EQ(ToExecutorStatus(receipt), ExecutorStatus::Rejected);
    EXPECT_EQ(ToExecutorReason(receipt), ExecutorReason::LeaseExpired);
}
