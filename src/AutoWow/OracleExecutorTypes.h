/*
 * Typed, side-effect-free boundary for future AutoWow quest/gather adapters.
 *
 * OperationCode is owned by AutoWowOracleContract.h and is the only operation selector here.
 * IntentLease and ActiveLeaseSnapshot are also contract-owned: this layer records the snapshot it
 * was given and revalidates it later, but never creates, stores, renews, or preempts a lease.
 * Nothing in this file dispatches a Playerbot action.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_EXECUTOR_TYPES_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_EXECUTOR_TYPES_H

#include "AutoWowOracleContract.h"

#include <cstdint>

namespace AutoWowOracleExecutor
{
using Guid = AutoWowOracle::Guid;
using ItemId = AutoWowOracle::ItemId;
using StableId = AutoWowOracle::StableId;
using Tick = AutoWowOracle::Tick;
using FactVersion = AutoWowOracle::FactVersion;
using Epoch = AutoWowOracle::Epoch;
using DecisionId = AutoWowOracle::DecisionId;
using IntentId = AutoWowOracle::IntentId;
using EvidenceCounters = AutoWowOracle::EvidenceCounters;
using OperationCode = AutoWowOracle::OperationCode;
using Decision = AutoWowOracle::Decision;
using IntentLease = AutoWowOracle::IntentLease;
using QuestReference = AutoWowOracle::QuestReference;
using GatherSourceReference = AutoWowOracle::GatherSourceReference;
using ActiveLeaseSnapshot = AutoWowOracle::ActiveLeaseSnapshot;

using ExecutorRequestId = std::uint64_t;
using ProofSequence = std::uint16_t;

inline constexpr ExecutorRequestId kInvalidExecutorRequestId = 0;
inline constexpr ProofSequence kMaxProofSequence = 1024;

inline constexpr bool ExecutorRequestIdWithinBounds(ExecutorRequestId value)
{
    return value != kInvalidExecutorRequestId;
}

inline constexpr bool ProofSequenceWithinBounds(ProofSequence value)
{
    return value <= kMaxProofSequence;
}

// This checks the bounds of the shared enum, not a second operation vocabulary.
inline constexpr bool OperationCodeWithinBounds(OperationCode operation)
{
    return static_cast<std::uint8_t>(operation) <=
        static_cast<std::uint8_t>(OperationCode::Disengage);
}

inline constexpr bool KnownOperationCode(OperationCode operation)
{
    return OperationCodeWithinBounds(operation) && operation != OperationCode::Unknown;
}

// These are the future adapter families represented by this narrow lane. The contract may plan
// other operations, but the current runtime authority deliberately blocks them.
inline constexpr bool QuestGatherAdapterOperation(OperationCode operation)
{
    switch (operation)
    {
        case OperationCode::QuestObjective:
        case OperationCode::GatherRoute:
        case OperationCode::GatherSource:
            return true;
        default:
            return false;
    }
}

enum class ExecutorStatus : std::uint8_t
{
    Invalid = 0,
    Accepted,
    Progressing,
    Progress = Progressing,
    Completed,
    Blocked,
    Failed,
    Rejected
};

inline constexpr bool ExecutorStatusWithinBounds(ExecutorStatus status)
{
    return static_cast<std::uint8_t>(status) <=
        static_cast<std::uint8_t>(ExecutorStatus::Rejected);
}

// Reasons are bounded and typed. They describe the adapter boundary without introducing another
// string or enum for the operation itself.
enum class ExecutorReason : std::uint8_t
{
    None = 0,
    InvalidRequest,
    InvalidDecision,
    LeaseMissing,
    InvalidLease = LeaseMissing,
    MissingOwnershipProof,
    LeaseExpired,
    LeaseMismatch,
    LeasePreempted,
    UnknownOperation,
    OperationBlocked,
    ExecutorBlocked = OperationBlocked,
    InvalidOperationPayload,
    InvalidQuestReference,
    InvalidGatherSourceReference,
    StaleFrame,
    StaleEpoch,
    ProofMismatch,
    ReceiptMismatch,
    InvalidReceipt,
    OracleRejected,
    ExecutorFailed
};

inline constexpr bool ExecutorReasonWithinBounds(ExecutorReason reason)
{
    return static_cast<std::uint8_t>(reason) <=
        static_cast<std::uint8_t>(ExecutorReason::ExecutorFailed);
}

struct ProofDelta
{
    std::int64_t objective = 0;
    std::int64_t progress = 0;
    std::int64_t resource = 0;
    std::int64_t combat = 0;
    std::int64_t healing = 0;
    std::int64_t deaths = 0;
    std::int64_t pvp = 0;
    std::int64_t failures = 0;
};

inline constexpr bool operator==(ProofDelta const& left, ProofDelta const& right)
{
    return left.objective == right.objective && left.progress == right.progress &&
        left.resource == right.resource && left.combat == right.combat &&
        left.healing == right.healing && left.deaths == right.deaths && left.pvp == right.pvp &&
        left.failures == right.failures;
}

inline constexpr bool operator!=(ProofDelta const& left, ProofDelta const& right)
{
    return !(left == right);
}

struct ProofSnapshot
{
    EvidenceCounters before;
    EvidenceCounters after;
    ProofDelta delta;
};

// This request is a typed adapter payload, not an action command. In particular, it has no
// Decision::action string and no Playerbot/native object. The ownership proof is a snapshot copied
// from OracleArbiter::QueryActiveLease; it is not authoritative by itself.
struct ExecutorRequest
{
    bool valid = false;
    ExecutorRequestId requestId = kInvalidExecutorRequestId;

    OperationCode operation = OperationCode::Unknown;
    AutoWowOracle::Domain domain = AutoWowOracle::Domain::Combat;
    AutoWowOracle::IntentCode intent = AutoWowOracle::IntentCode::CombatAction;
    AutoWowOracle::LeaseResource resource = AutoWowOracle::LeaseResource::Idle;
    AutoWowOracle::Scope scope;

    DecisionId decisionId = 0;
    IntentId intentId = 0;
    Guid actorGuid = 0;
    Guid targetGuid = 0;
    ItemId itemId = 0;
    StableId objectiveId = 0;
    StableId rallyPointId = 0;
    AutoWowOracle::RoleCode role = AutoWowOracle::RoleCode::Unknown;
    bool executorAvailable = false;

    QuestReference quest;
    GatherSourceReference gather;
    AutoWowOracle::GatherGoal gatherGoal = AutoWowOracle::GatherGoal::Unknown;

    Epoch epoch = 0;
    FactVersion frameVersion = 0;
    Tick issuedTick = 0;
    Tick expiresTick = 0;
    std::uint32_t ttlTicks = 0;
    EvidenceCounters proofBaseline;
    ActiveLeaseSnapshot ownershipProof;
};

// An adapter can return one of these bounded receipts after it has observed the world. It carries
// the operation and typed references so a future quest/gather adapter need not recover them from a
// string label. The receipt itself does not execute anything.
struct ExecutorReceipt
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    ExecutorReason reason = ExecutorReason::InvalidReceipt;

    ExecutorRequestId requestId = kInvalidExecutorRequestId;
    DecisionId decisionId = 0;
    IntentId intentId = 0;
    OperationCode operation = OperationCode::Unknown;
    AutoWowOracle::Scope scope;
    Guid actorGuid = 0;
    QuestReference quest;
    GatherSourceReference gather;
    AutoWowOracle::GatherGoal gatherGoal = AutoWowOracle::GatherGoal::Unknown;
    ActiveLeaseSnapshot ownershipProof;

    Tick observedTick = 0;
    FactVersion frameVersion = 0;
    ProofSequence sequence = 0;
    ProofSnapshot proof;
};

// valid means the result has a structurally valid bounded receipt. accepted says whether a request
// is ready for a future adapter; blocked/rejected results remain useful typed receipts.
struct ExecutorResult
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    ExecutorReason reason = ExecutorReason::InvalidRequest;
    ExecutorRequest request;
    ExecutorReceipt receipt;
};

struct ValidationResult
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    ExecutorReason reason = ExecutorReason::InvalidRequest;
};

// PrepareRequest is the only construction path that can mark an ExecutorRequest valid. It uses a
// snapshot supplied by the contract arbiter and never creates or stores lease authority.
ExecutorResult PrepareRequest(IntentLease const& lease, ActiveLeaseSnapshot const& ownershipProof,
    Tick now) noexcept;

ExecutorRequest MakeRequest(IntentLease const& lease, ActiveLeaseSnapshot const& ownershipProof,
    Tick now) noexcept;

ProofDelta MakeProofDelta(EvidenceCounters const& before,
    EvidenceCounters const& after) noexcept;
bool ProofDeltaMatches(ProofSnapshot const& proof) noexcept;

ExecutorReceipt MakeReceipt(ExecutorRequest const& request, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& after, ProofSequence sequence = 0) noexcept;

ExecutorResult MakeResult(ExecutorRequest const& request, ExecutorStatus status,
    ExecutorReason reason, ActiveLeaseSnapshot const& ownershipProof, Tick now,
    EvidenceCounters const& after, ProofSequence sequence = 0) noexcept;

// Validation is side-effect-free and depends only on the request, the current arbiter snapshot,
// and the observed tick. A fresh snapshot is required so an unexpired preempted lease is rejected.
ValidationResult ValidateRequest(ExecutorRequest const& request,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept;

ValidationResult ValidateReceipt(ExecutorRequest const& request, ExecutorReceipt const& receipt,
    ActiveLeaseSnapshot const& ownershipProof, Tick now) noexcept;

ExecutorStatus ToExecutorStatus(AutoWowOracle::Receipt const& receipt) noexcept;
ExecutorReason ToExecutorReason(AutoWowOracle::Receipt const& receipt) noexcept;

} // namespace AutoWowOracleExecutor

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_EXECUTOR_TYPES_H
