/*
 * Pure implementation of the typed AutoWow Oracle executor boundary.
 */
#include "OracleExecutorTypes.h"

#include <limits>

namespace AutoWowOracleExecutor
{
namespace
{
constexpr std::uint64_t kRequestIdDomain = 0x41574f4558454352ULL;

constexpr std::uint64_t Mix(std::uint64_t seed, std::uint64_t left,
    std::uint64_t right) noexcept
{
    std::uint64_t value = seed ^ (left + 0x9e3779b97f4a7c15ULL) ^
        (right + 0xbf58476d1ce4e5b9ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

constexpr std::uint64_t QuestReferenceKey(QuestReference const& quest) noexcept
{
    std::uint64_t key = static_cast<std::uint64_t>(quest.valid);
    key = Mix(key, quest.questId, static_cast<std::uint8_t>(quest.objectiveFamily));
    key = Mix(key, quest.objectiveSlot, quest.requiredEntry);
    return Mix(key, quest.requiredItemId, 0);
}

constexpr std::uint64_t GatherReferenceKey(GatherSourceReference const& gather) noexcept
{
    std::uint64_t key = static_cast<std::uint64_t>(gather.valid);
    key = Mix(key, gather.spawnId, gather.entry);
    key = Mix(key, gather.mapId, gather.instanceId);
    return Mix(key, gather.materialItemId, static_cast<std::uint8_t>(gather.goal));
}

constexpr ExecutorRequestId MakeRequestId(Decision const& decision) noexcept
{
    std::uint64_t key = Mix(kRequestIdDomain, decision.decisionId, decision.intentId);
    key = Mix(key, decision.epoch, static_cast<std::uint8_t>(decision.operation));
    key = Mix(key, decision.scope.squadId, decision.scope.botGuid);
    key = Mix(key, decision.targetGuid, decision.itemId);
    key = Mix(key, decision.objectiveId, decision.rallyPointId);
    key = Mix(key, QuestReferenceKey(decision.quest), GatherReferenceKey(decision.gather));
    return key == kInvalidExecutorRequestId ? 1 : key;
}

constexpr bool SameEvidence(EvidenceCounters const& left,
    EvidenceCounters const& right) noexcept
{
    return left.objective == right.objective && left.progress == right.progress &&
        left.resource == right.resource && left.combat == right.combat &&
        left.healing == right.healing && left.deaths == right.deaths && left.pvp == right.pvp &&
        left.failures == right.failures;
}

constexpr bool SameOwnershipProof(ActiveLeaseSnapshot const& left,
    ActiveLeaseSnapshot const& right) noexcept
{
    return left.active == right.active && left.botGuid == right.botGuid &&
        left.decisionId == right.decisionId && left.intentId == right.intentId &&
        left.epoch == right.epoch && left.expiresTick == right.expiresTick &&
        AutoWowOracle::SameScope(left.scope, right.scope) && left.operation == right.operation;
}

ValidationResult Reject(ExecutorStatus status, ExecutorReason reason) noexcept
{
    return {false, status, reason};
}

ValidationResult Accept(ExecutorStatus status = ExecutorStatus::Accepted,
    ExecutorReason reason = ExecutorReason::None) noexcept
{
    return {true, status, reason};
}

constexpr bool IsOperationBlockedReason(ExecutorReason reason) noexcept
{
    return reason == ExecutorReason::OperationBlocked ||
        reason == ExecutorReason::InvalidOperationPayload ||
        reason == ExecutorReason::InvalidQuestReference ||
        reason == ExecutorReason::InvalidGatherSourceReference;
}

constexpr ExecutorStatus StatusForReason(ExecutorReason reason) noexcept
{
    return IsOperationBlockedReason(reason) ? ExecutorStatus::Blocked : ExecutorStatus::Rejected;
}

ExecutorReceipt ReceiptForDecision(Decision const& decision, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& before, EvidenceCounters const& after,
    ProofSequence sequence = 0) noexcept
{
    ExecutorReceipt receipt;
    receipt.valid = ExecutorStatusWithinBounds(status) && status != ExecutorStatus::Invalid &&
        ExecutorReasonWithinBounds(reason) && ProofSequenceWithinBounds(sequence);
    receipt.status = status;
    receipt.reason = reason;
    receipt.decisionId = decision.decisionId;
    receipt.intentId = decision.intentId;
    receipt.operation = decision.operation;
    receipt.scope = decision.scope;
    receipt.actorGuid = decision.actorGuid;
    receipt.quest = decision.quest;
    receipt.gather = decision.gather;
    receipt.ownershipProof = ownershipProof;
    receipt.observedTick = now;
    receipt.frameVersion = decision.preconditions.frameVersion;
    receipt.sequence = sequence;
    receipt.proof.before = before;
    receipt.proof.after = after;
    receipt.proof.delta = MakeProofDelta(before, after);
    return receipt;
}

ExecutorReceipt ReceiptForRequest(ExecutorRequest const& request, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& after, ProofSequence sequence = 0) noexcept
{
    ExecutorReceipt receipt;
    receipt.valid = request.valid && ExecutorStatusWithinBounds(status) &&
        status != ExecutorStatus::Invalid && ExecutorReasonWithinBounds(reason) &&
        ProofSequenceWithinBounds(sequence);
    receipt.status = status;
    receipt.reason = reason;
    receipt.requestId = request.requestId;
    receipt.decisionId = request.decisionId;
    receipt.intentId = request.intentId;
    receipt.operation = request.operation;
    receipt.scope = request.scope;
    receipt.actorGuid = request.actorGuid;
    receipt.quest = request.quest;
    receipt.gather = request.gather;
    receipt.gatherGoal = request.gatherGoal;
    receipt.ownershipProof = ownershipProof;
    receipt.observedTick = now;
    receipt.frameVersion = request.frameVersion;
    receipt.sequence = sequence;
    receipt.proof.before = request.proofBaseline;
    receipt.proof.after = after;
    receipt.proof.delta = MakeProofDelta(receipt.proof.before, receipt.proof.after);
    return receipt;
}

ExecutorReason ValidateDecisionShape(Decision const& decision) noexcept
{
    if (!decision.valid || decision.decisionId == 0 || decision.intentId == 0 ||
        decision.scope.botGuid == 0 || decision.actorGuid == 0 ||
        decision.actorGuid != decision.scope.botGuid || decision.ttlTicks == 0 ||
        decision.expiresTick <= decision.issuedTick)
        return ExecutorReason::InvalidDecision;
    return ExecutorReason::None;
}

ExecutorReason ValidateDecisionOperation(Decision const& decision) noexcept
{
    if (!OperationCodeWithinBounds(decision.operation) ||
        decision.operation == OperationCode::Unknown)
        return ExecutorReason::UnknownOperation;
    if (!QuestGatherAdapterOperation(decision.operation))
        return ExecutorReason::OperationBlocked;

    switch (decision.operation)
    {
        case OperationCode::QuestObjective:
            if (!AutoWowOracle::ValidQuestReference(decision.quest) ||
                !AutoWowOracle::CurrentQuestObjectivePayloadConsistent(decision.targetGuid,
                    decision.itemId, decision.objectiveId, decision.quest, decision.gather))
                return ExecutorReason::InvalidQuestReference;
            break;
        case OperationCode::GatherRoute:
            if (!AutoWowOracle::ValidGatherRouteReference(decision.gather))
                return ExecutorReason::InvalidGatherSourceReference;
            break;
        case OperationCode::GatherSource:
            if (!AutoWowOracle::ValidGatherSourceReference(decision.gather))
                return ExecutorReason::InvalidGatherSourceReference;
            break;
        default:
            return ExecutorReason::OperationBlocked;
    }

    // The shared contract verifies only the deliberately enabled QuestObjective and exact
    // GatherSource tuples. All other operations remain fail-closed.
    if (!AutoWowOracle::IsCurrentVerifiedOperation(decision))
        return ExecutorReason::OperationBlocked;
    return ExecutorReason::None;
}

ValidationResult ValidateLease(ExecutorRequest const& request,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    if (!request.valid)
        return Reject(ExecutorStatus::Invalid, ExecutorReason::InvalidRequest);
    if (now >= request.expiresTick)
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeaseExpired);
    if (!request.ownershipProof.active || !ownershipProof.active)
        return Reject(ExecutorStatus::Rejected, ExecutorReason::MissingOwnershipProof);
    if (request.scope.botGuid == 0 || request.actorGuid != request.scope.botGuid ||
        ownershipProof.botGuid != request.scope.botGuid ||
        !AutoWowOracle::SameScope(ownershipProof.scope, request.scope))
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeaseMismatch);
    if (ownershipProof.decisionId != request.decisionId ||
        ownershipProof.intentId != request.intentId)
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeasePreempted);
    if (ownershipProof.epoch != request.epoch ||
        ownershipProof.expiresTick != request.expiresTick ||
        ownershipProof.operation != request.operation)
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeaseMismatch);
    if (!SameOwnershipProof(request.ownershipProof, ownershipProof))
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeaseMismatch);
    return Accept();
}

ExecutorReason ValidateRequestOperation(ExecutorRequest const& request) noexcept
{
    if (!OperationCodeWithinBounds(request.operation) ||
        request.operation == OperationCode::Unknown)
        return ExecutorReason::UnknownOperation;
    if (!QuestGatherAdapterOperation(request.operation))
        return ExecutorReason::OperationBlocked;

    switch (request.operation)
    {
        case OperationCode::QuestObjective:
            if (!AutoWowOracle::ValidQuestReference(request.quest) ||
                !AutoWowOracle::CurrentQuestObjectivePayloadConsistent(request.targetGuid,
                    request.itemId, request.objectiveId, request.quest, request.gather))
                return ExecutorReason::InvalidQuestReference;
            break;
        case OperationCode::GatherRoute:
            if (!AutoWowOracle::ValidGatherRouteReference(request.gather))
                return ExecutorReason::InvalidGatherSourceReference;
            break;
        case OperationCode::GatherSource:
            if (!AutoWowOracle::ValidGatherSourceReference(request.gather) ||
                request.gatherGoal != request.gather.goal)
                return ExecutorReason::InvalidGatherSourceReference;
            break;
        default:
            return ExecutorReason::OperationBlocked;
    }

    if (!AutoWowOracle::IsVerifiedOperationTuple(request.executorAvailable, request.domain,
            request.intent, request.resource, request.operation, request.quest, request.gather,
            request.itemId, request.objectiveId))
        return ExecutorReason::OperationBlocked;
    return ExecutorReason::None;
}

ValidationResult ValidateRequestShape(ExecutorRequest const& request) noexcept
{
    if (!request.valid)
        return Reject(ExecutorStatus::Invalid, ExecutorReason::InvalidRequest);
    if (!ExecutorRequestIdWithinBounds(request.requestId) || request.decisionId == 0 ||
        request.intentId == 0 || request.scope.botGuid == 0 || request.actorGuid == 0 ||
        request.actorGuid != request.scope.botGuid || request.ttlTicks == 0 ||
        request.expiresTick <= request.issuedTick)
        return Reject(ExecutorStatus::Invalid, ExecutorReason::InvalidRequest);
    return Accept();
}

ValidationResult ValidateRequestOwnership(ExecutorRequest const& request,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    ValidationResult const shape = ValidateRequestShape(request);
    if (!shape.valid)
        return shape;
    ValidationResult const lease = ValidateLease(request, ownershipProof, now);
    if (!lease.valid)
        return lease;
    return Accept();
}

void CopyDecisionToRequest(ExecutorRequest& request, Decision const& decision,
    ActiveLeaseSnapshot const& ownershipProof) noexcept
{
    request.valid = true;
    request.requestId = MakeRequestId(decision);
    request.operation = decision.operation;
    request.domain = decision.domain;
    request.intent = decision.intent;
    request.resource = decision.resource;
    request.scope = decision.scope;
    request.decisionId = decision.decisionId;
    request.intentId = decision.intentId;
    request.actorGuid = decision.actorGuid;
    request.targetGuid = decision.targetGuid;
    request.itemId = decision.itemId;
    request.objectiveId = decision.objectiveId;
    request.rallyPointId = decision.rallyPointId;
    request.role = decision.role;
    request.executorAvailable = decision.executorAvailable;
    request.quest = decision.quest;
    request.gather = decision.gather;
    request.gatherGoal = decision.gather.goal;
    request.epoch = decision.epoch;
    request.frameVersion = decision.preconditions.frameVersion;
    request.issuedTick = decision.issuedTick;
    request.expiresTick = decision.expiresTick;
    request.ttlTicks = decision.ttlTicks;
    request.proofBaseline = decision.evidence;
    request.ownershipProof = ownershipProof;
}

ExecutorResult PreparationOutcome(Decision const& decision, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    ExecutorResult result;
    result.status = status;
    result.reason = reason;
    result.receipt = ReceiptForDecision(decision, status, reason, ownershipProof, now,
        decision.evidence, decision.evidence);
    result.valid = result.receipt.valid;
    result.accepted = false;
    return result;
}

} // namespace

ExecutorResult PrepareRequest(IntentLease const& lease,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    if (!lease.valid)
        return PreparationOutcome(lease.decision, ExecutorStatus::Rejected,
            ExecutorReason::LeaseMissing, ownershipProof, now);

    Decision const& decision = lease.decision;
    ExecutorReason reason = ValidateDecisionShape(decision);
    if (reason != ExecutorReason::None)
        return PreparationOutcome(decision, ExecutorStatus::Rejected, reason, ownershipProof, now);

    reason = ValidateDecisionOperation(decision);
    if (reason != ExecutorReason::None)
        return PreparationOutcome(decision, StatusForReason(reason), reason, ownershipProof, now);

    if (now >= decision.expiresTick)
        return PreparationOutcome(decision, ExecutorStatus::Rejected,
            ExecutorReason::LeaseExpired, ownershipProof, now);
    if (!ownershipProof.active)
        return PreparationOutcome(decision, ExecutorStatus::Rejected,
            ExecutorReason::MissingOwnershipProof, ownershipProof, now);
    if (ownershipProof.botGuid != decision.scope.botGuid ||
        !AutoWowOracle::SameScope(ownershipProof.scope, decision.scope))
        return PreparationOutcome(decision, ExecutorStatus::Rejected,
            ExecutorReason::LeaseMismatch, ownershipProof, now);
    if (ownershipProof.decisionId != decision.decisionId ||
        ownershipProof.intentId != decision.intentId)
        return PreparationOutcome(decision, ExecutorStatus::Rejected,
            ExecutorReason::LeasePreempted, ownershipProof, now);
    if (ownershipProof.epoch != decision.epoch ||
        ownershipProof.expiresTick != decision.expiresTick ||
        ownershipProof.operation != decision.operation)
        return PreparationOutcome(decision, ExecutorStatus::Rejected,
            ExecutorReason::LeaseMismatch, ownershipProof, now);

    ExecutorResult result;
    CopyDecisionToRequest(result.request, decision, ownershipProof);
    result.receipt = ReceiptForRequest(result.request, ExecutorStatus::Accepted,
        ExecutorReason::None, ownershipProof, now, decision.evidence);
    result.valid = result.receipt.valid;
    result.accepted = result.request.valid && result.valid;
    result.status = ExecutorStatus::Accepted;
    result.reason = ExecutorReason::None;
    return result;
}

ExecutorRequest MakeRequest(IntentLease const& lease,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    return PrepareRequest(lease, ownershipProof, now).request;
}

ProofDelta MakeProofDelta(EvidenceCounters const& before,
    EvidenceCounters const& after) noexcept
{
    auto counterDelta = [](std::uint64_t left, std::uint64_t right) noexcept
    {
        constexpr std::uint64_t signedMaximum =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (right >= left)
        {
            std::uint64_t const difference = right - left;
            return difference > signedMaximum ? std::numeric_limits<std::int64_t>::max() :
                                                 static_cast<std::int64_t>(difference);
        }
        std::uint64_t const difference = left - right;
        return difference > signedMaximum ? std::numeric_limits<std::int64_t>::min() :
                                             -static_cast<std::int64_t>(difference);
    };

    return {counterDelta(before.objective, after.objective),
        counterDelta(before.progress, after.progress), counterDelta(before.resource, after.resource),
        counterDelta(before.combat, after.combat), counterDelta(before.healing, after.healing),
        counterDelta(before.deaths, after.deaths), counterDelta(before.pvp, after.pvp),
        counterDelta(before.failures, after.failures)};
}

bool ProofDeltaMatches(ProofSnapshot const& proof) noexcept
{
    return proof.delta == MakeProofDelta(proof.before, proof.after);
}

ExecutorReceipt MakeReceipt(ExecutorRequest const& request, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& after, ProofSequence sequence) noexcept
{
    ValidationResult const validation = ValidateRequest(request, ownershipProof, now);
    if (!validation.valid)
        return ReceiptForRequest(request, validation.status == ExecutorStatus::Invalid ?
                ExecutorStatus::Rejected : validation.status,
            validation.reason, ownershipProof, now, after, sequence);
    return ReceiptForRequest(request, status, reason, ownershipProof, now, after, sequence);
}

ExecutorResult MakeResult(ExecutorRequest const& request, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& after, ProofSequence sequence) noexcept
{
    ExecutorResult result;
    result.request = request;
    result.receipt = MakeReceipt(request, status, reason, ownershipProof, now, after, sequence);
    result.valid = result.receipt.valid;
    result.status = result.receipt.status;
    result.reason = result.receipt.reason;
    result.accepted = result.valid &&
        (result.status == ExecutorStatus::Accepted ||
            result.status == ExecutorStatus::Progressing ||
            result.status == ExecutorStatus::Completed);
    return result;
}

ValidationResult ValidateRequest(ExecutorRequest const& request,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    ValidationResult const shape = ValidateRequestShape(request);
    if (!shape.valid)
        return shape;

    ExecutorReason const operation = ValidateRequestOperation(request);
    if (operation != ExecutorReason::None)
        return Reject(StatusForReason(operation), operation);

    return ValidateRequestOwnership(request, ownershipProof, now);
}

ValidationResult ValidateReceipt(ExecutorRequest const& request, ExecutorReceipt const& receipt,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept
{
    ValidationResult const requestValidation = ValidateRequest(request, ownershipProof, now);
    if (!requestValidation.valid)
        return requestValidation;
    if (!receipt.valid || !ExecutorStatusWithinBounds(receipt.status) ||
        receipt.status == ExecutorStatus::Invalid || !ExecutorReasonWithinBounds(receipt.reason) ||
        !OperationCodeWithinBounds(receipt.operation) || !ProofSequenceWithinBounds(receipt.sequence))
        return Reject(ExecutorStatus::Invalid, ExecutorReason::InvalidReceipt);
    if (receipt.requestId != request.requestId || receipt.decisionId != request.decisionId ||
        receipt.intentId != request.intentId || receipt.operation != request.operation ||
        !AutoWowOracle::SameScope(receipt.scope, request.scope) ||
        receipt.actorGuid != request.actorGuid || receipt.quest.valid != request.quest.valid ||
        receipt.quest.questId != request.quest.questId ||
        receipt.gather.valid != request.gather.valid ||
        receipt.gather.spawnId != request.gather.spawnId ||
        receipt.gatherGoal != request.gatherGoal ||
        !AutoWowOracle::SameGatherSourceReference(receipt.gather, request.gather))
        return Reject(ExecutorStatus::Invalid, ExecutorReason::ReceiptMismatch);
    if (receipt.observedTick > now || !SameOwnershipProof(receipt.ownershipProof, ownershipProof))
        return Reject(ExecutorStatus::Rejected, ExecutorReason::LeaseMismatch);
    if (!SameEvidence(receipt.proof.before, request.proofBaseline) ||
        !ProofDeltaMatches(receipt.proof))
        return Reject(ExecutorStatus::Invalid, ExecutorReason::ProofMismatch);
    return Accept(receipt.status, receipt.reason);
}

ExecutorStatus ToExecutorStatus(AutoWowOracle::Receipt const& receipt) noexcept
{
    switch (receipt.status)
    {
        case AutoWowOracle::ReceiptStatus::Accepted:
            return ExecutorStatus::Accepted;
        case AutoWowOracle::ReceiptStatus::Progressing:
            return ExecutorStatus::Progressing;
        case AutoWowOracle::ReceiptStatus::Completed:
            return ExecutorStatus::Completed;
        case AutoWowOracle::ReceiptStatus::Blocked:
            return ExecutorStatus::Blocked;
        case AutoWowOracle::ReceiptStatus::Failed:
            return ExecutorStatus::Failed;
        case AutoWowOracle::ReceiptStatus::Rejected:
            return ExecutorStatus::Rejected;
    }
    return ExecutorStatus::Invalid;
}

ExecutorReason ToExecutorReason(AutoWowOracle::Receipt const& receipt) noexcept
{
    switch (receipt.reason)
    {
        case AutoWowOracle::ReceiptReason::None:
        case AutoWowOracle::ReceiptReason::Planned:
        case AutoWowOracle::ReceiptReason::LeaseAcquired:
        case AutoWowOracle::ReceiptReason::LeaseAlreadyOwned:
        case AutoWowOracle::ReceiptReason::LeaseRenewed:
        case AutoWowOracle::ReceiptReason::LeaseReleased:
            return ExecutorReason::None;
        case AutoWowOracle::ReceiptReason::InvalidDecision:
            return ExecutorReason::InvalidDecision;
        case AutoWowOracle::ReceiptReason::InvalidScope:
            return ExecutorReason::LeaseMismatch;
        case AutoWowOracle::ReceiptReason::InvalidFrame:
        case AutoWowOracle::ReceiptReason::StaleFrame:
            return ExecutorReason::StaleFrame;
        case AutoWowOracle::ReceiptReason::StaleEpoch:
            return ExecutorReason::StaleEpoch;
        case AutoWowOracle::ReceiptReason::Expired:
            return ExecutorReason::LeaseExpired;
        case AutoWowOracle::ReceiptReason::PreconditionFailed:
            return ExecutorReason::InvalidOperationPayload;
        case AutoWowOracle::ReceiptReason::LabOnlyPersistentScope:
            return ExecutorReason::OracleRejected;
        case AutoWowOracle::ReceiptReason::BotLeaseConflict:
            return ExecutorReason::LeasePreempted;
        case AutoWowOracle::ReceiptReason::SquadOwnerConflict:
        case AutoWowOracle::ReceiptReason::ItemReservationConflict:
        case AutoWowOracle::ReceiptReason::NodeReservationConflict:
            return ExecutorReason::OperationBlocked;
        case AutoWowOracle::ReceiptReason::CapacityExceeded:
            return ExecutorReason::OracleRejected;
        case AutoWowOracle::ReceiptReason::NotOwner:
            return ExecutorReason::LeaseMismatch;
        case AutoWowOracle::ReceiptReason::ExecutorBlocked:
            return ExecutorReason::OperationBlocked;
        case AutoWowOracle::ReceiptReason::ExecutorFailed:
            return ExecutorReason::ExecutorFailed;
        case AutoWowOracle::ReceiptReason::NoEligibleIntent:
            return ExecutorReason::OracleRejected;
    }
    return ExecutorReason::OracleRejected;
}

} // namespace AutoWowOracleExecutor
