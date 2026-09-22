/*
 * Bounded fixed-target AutoWow GatherSource executor.
 *
 * This lane consumes the authoritative AutoWowOracleExecutor::ExecutorRequest and
 * AutoWowOracle::IntentLease types. GatherSource is enabled only through the deliberate exact
 * source tuple and a live pre-probed frame; this executor never creates or renews authority,
 * selects a candidate, teleports, or invokes the ordinary worker selector.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_GATHER_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_GATHER_EXECUTOR_H

#include "OracleExecutorTypes.h"
#include "OracleRouteExecutor.h"
#include "../Ai/World/Gathering/GatheringCandidatePolicy.h"
#include "../Ai/World/Gathering/GatheringReceiptPolicy.h"

#include <cstdint>
#include <string_view>

namespace AutoWowOracleGatherExecutor
{
using AutoWowOracle::ActiveLeaseSnapshot;
using AutoWowOracle::Epoch;
using AutoWowOracle::FactVersion;
using AutoWowOracle::GatherSourceReference;
using AutoWowOracle::Guid;
using AutoWowOracle::IntentLease;
using AutoWowOracle::ItemId;
using AutoWowOracle::OperationCode;
using AutoWowOracle::ReceiptReason;
using AutoWowOracle::ReceiptStatus;
using AutoWowOracle::Scope;
using AutoWowOracle::Tick;
using AutoWowOracleExecutor::ExecutorReason;
using AutoWowOracleExecutor::ExecutorRequest;
using AutoWowOracleExecutor::ExecutorStatus;
using AutoWowOracleRoute::RouteFailure;
using AutoWowOracleRoute::RouteKey;
using AutoWowOracleRoute::RouteLedgerKey;
using AutoWowOracleRoute::RouteObservation;
using AutoWowOracleRoute::RouteResult;
using AutoWowOracleRoute::RouteSession;
using AutoWowOracleRoute::SessionLedger;

inline constexpr std::string_view kWorkerGatherAction = "worker gather seek";
inline constexpr std::string_view kGatherQualifier = "gather";

// These are native facts, not planner text. A fixed-target call carries one complete fact set and
// the post-step observation must carry the same set; changing any field is target substitution.
struct CandidateFacts
{
    bool alive = false;
    bool available = false;
    bool reachable = false;
    AutoWowGather::Profession profession = AutoWowGather::Profession::None;
    bool toolRequired = false;
    bool toolAvailable = false;
    // The native source table/generated loot observation independently proves that this exact
    // source can produce the requested material. A request's materialItemId alone is not proof.
    bool sourceYieldsRequestedMaterial = false;
};

// The reference is the authoritative full source identity. It is deliberately not flattened to
// a coordinate, vector index, or runtime GUID.
struct FixedTargetCandidate
{
    GatherSourceReference reference;
    CandidateFacts facts;
};

// This is an accepted arbiter receipt copied into the native seam. The executor never constructs
// an accepted proof and never calls ReserveNode; a caller-invented boolean is not an authority.
struct NodeReservationProof
{
    AutoWowOracle::Receipt arbiterReceipt;
    Tick expiresTick = 0;
};

struct FixedTargetWorldSnapshot
{
    bool onWorldThread = false;
    bool botAlive = false;
    Guid botGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    Tick tick = 0;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    bool withinInteractRange = false;
    bool lootReady = false;
    ActiveLeaseSnapshot ownershipProof;
    AutoWowOracle::EvidenceCounters evidence;
};

struct FixedTargetObservation
{
    FixedTargetWorldSnapshot world;
    NodeReservationProof nodeReservation;
    FixedTargetCandidate candidate;
};

enum class NativeStepPhase : std::uint8_t
{
    Unknown = 0,
    Seek,
    Interact,
    Loot
};

inline constexpr std::string_view NativeStepPhaseName(NativeStepPhase phase) noexcept
{
    switch (phase)
    {
        case NativeStepPhase::Unknown: return "unknown";
        case NativeStepPhase::Seek: return "seek";
        case NativeStepPhase::Interact: return "interact";
        case NativeStepPhase::Loot: return "loot";
    }
    return "unknown";
}

enum class FixedTargetReason : std::uint8_t
{
    None = 0,
    InvalidRequest,
    NotWorldThread,
    WrongOperation,
    WrongDomain,
    WrongIntent,
    WrongResource,
    InvalidGatherSourceReference,
    WrongBot,
    StaleFrame,
    StaleEpoch,
    LeaseExpired,
    MissingOwnershipProof,
    LeaseMismatch,
    LeasePreempted,
    ReservationMissing,
    ReservationExpired,
    ReservationMismatch,
    CandidateMismatch,
    CandidateFactsMismatch,
    NativeUnavailable,
    NativeRejected,
    StepBudgetExceeded,
    PostconditionMismatch,
    NoFallback,
    RouteIntentMismatch,
    RouteNotReady,
    RouteBlocked
};

inline constexpr std::string_view FixedTargetReasonName(FixedTargetReason reason) noexcept
{
    switch (reason)
    {
        case FixedTargetReason::None: return "none";
        case FixedTargetReason::InvalidRequest: return "invalid_request";
        case FixedTargetReason::NotWorldThread: return "not_world_thread";
        case FixedTargetReason::WrongOperation: return "wrong_operation";
        case FixedTargetReason::WrongDomain: return "wrong_domain";
        case FixedTargetReason::WrongIntent: return "wrong_intent";
        case FixedTargetReason::WrongResource: return "wrong_resource";
        case FixedTargetReason::InvalidGatherSourceReference: return "invalid_gather_source_reference";
        case FixedTargetReason::WrongBot: return "wrong_bot";
        case FixedTargetReason::StaleFrame: return "stale_frame";
        case FixedTargetReason::StaleEpoch: return "stale_epoch";
        case FixedTargetReason::LeaseExpired: return "lease_expired";
        case FixedTargetReason::MissingOwnershipProof: return "missing_ownership_proof";
        case FixedTargetReason::LeaseMismatch: return "lease_mismatch";
        case FixedTargetReason::LeasePreempted: return "lease_preempted";
        case FixedTargetReason::ReservationMissing: return "reservation_missing";
        case FixedTargetReason::ReservationExpired: return "reservation_expired";
        case FixedTargetReason::ReservationMismatch: return "reservation_mismatch";
        case FixedTargetReason::CandidateMismatch: return "candidate_mismatch";
        case FixedTargetReason::CandidateFactsMismatch: return "candidate_facts_mismatch";
        case FixedTargetReason::NativeUnavailable: return "native_unavailable";
        case FixedTargetReason::NativeRejected: return "native_rejected";
        case FixedTargetReason::StepBudgetExceeded: return "step_budget_exceeded";
        case FixedTargetReason::PostconditionMismatch: return "postcondition_mismatch";
        case FixedTargetReason::NoFallback: return "no_fallback";
        case FixedTargetReason::RouteIntentMismatch: return "route_intent_mismatch";
        case FixedTargetReason::RouteNotReady: return "route_not_ready";
        case FixedTargetReason::RouteBlocked: return "route_blocked";
    }
    return "invalid_request";
}

struct ValidationResult
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    FixedTargetReason reason = FixedTargetReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
};

struct NativeStepRequest
{
    ExecutorRequest request;
    IntentLease lease;
    NodeReservationProof nodeReservation;
    FixedTargetCandidate candidate;
    NativeStepPhase phase = NativeStepPhase::Unknown;
    std::uint8_t maxSteps = 1;
    bool stopAfterStep = true;
};

struct NativeStepObservation
{
    bool dispatchAccepted = false;
    std::uint8_t stepsInvoked = 0;
    bool selectedAnotherCandidate = false;
    // A native acceptance is only an interaction attempt. Completion is derived from this
    // canonical, source-linked evidence by the pure receipt policy.
    AutoWowGather::GatherCreditObservation credit;
    FixedTargetObservation after;
};

using NativeStepFunction = NativeStepObservation (*)(void* context,
                                                      NativeStepRequest const& request);

// Value-only join between the exact DB route descriptor and the reserved T1 gather source. The
// route key retains map/instance/entry/stable-spawn and full per-attempt lease identity; source
// retains material and gather-goal identity, which are not fields in the generic T2 RouteKey.
struct GatherRouteIntent
{
    AutoWowOracleRoute::RouteIntent route;
    GatherSourceReference source;
};

// Route evidence is receipt-shaped but explicitly nullable by field. Unknown evidence remains
// unavailable and false; target visibility or movement never manufactures an inventory delta.
struct GatherRouteEvidence
{
    bool distanceAvailable = false;
    double initialRemainingDistance = 0.0;
    double remainingDistance = 0.0;
    double reducedDistance = 0.0;

    bool segmentIndexAvailable = false;
    AutoWowOracleRoute::SegmentIndex segmentIndex = 0;

    bool objectiveDeltaAvailable = false;
    bool objectiveDelta = false;
    bool questDeltaAvailable = false;
    bool questDelta = false;
    bool inventoryDeltaAvailable = false;
    bool inventoryDelta = false;
    bool interactionDeltaAvailable = false;
    bool interactionDelta = false;
};

struct GatherRouteObservation
{
    RouteObservation route;

    // Only explicitly available receipt observations are forwarded as progress evidence. This
    // prevents a movement/grid-load observation from being reinterpreted as material credit.
    bool objectiveDeltaAvailable = false;
    bool objectiveDelta = false;
    bool questDeltaAvailable = false;
    bool questDelta = false;
    bool inventoryDeltaAvailable = false;
    bool inventoryDelta = false;
    bool interactionDeltaAvailable = false;
    bool interactionDelta = false;
};

struct GatherRouteStep
{
    bool valid = false;
    GatherRouteIntent intent;
    RouteSession session;
    RouteResult result;
    GatherRouteEvidence evidence;

    bool targetLoadKnown = false;
    bool targetLoaded = false;
    bool liveIdentityKnown = false;
    RouteKey liveIdentity;
    bool exactCurrentIdentity = false;
    bool exactLiveBound = false;
    bool interactionReady = false;

    // Caller-owned lease/backoff controls copied from the pure policy result and durable ledger.
    bool releaseLease = false;
    bool backoffAvailable = false;
    bool requiresReentry = false;
    Tick nextRetryAt = 0;
};

[[nodiscard]] GatherRouteIntent MakeGatherRouteIntent(
    AutoWowOracleRoute::RouteIntent const& route,
    GatherSourceReference const& source) noexcept;

[[nodiscard]] GatherRouteIntent MakeGatherRouteIntent(
    ExecutorRequest const& request,
    AutoWowOracleRoute::RouteIntent const& route) noexcept;

[[nodiscard]] RouteFailure ValidateGatherRouteIntent(
    GatherRouteIntent const& intent) noexcept;

[[nodiscard]] bool ExactGatherRouteMatches(
    ExecutorRequest const& request,
    GatherRouteIntent const& intent) noexcept;

// These adapters invoke only the pure T2 transition functions. The caller supplies DB
// confirmation, anchors, plans and reprobes, then executes returned normal movement commands.
[[nodiscard]] GatherRouteStep StartGatherRoute(
    GatherRouteIntent const& intent,
    double estimatedTravelSeconds,
    Tick now,
    SessionLedger const& ledger,
    bool explicitReentrySignal = false) noexcept;

[[nodiscard]] GatherRouteStep AdvanceGatherRoute(
    GatherRouteIntent const& intent,
    RouteSession const& session,
    GatherRouteObservation const& observation,
    Tick now) noexcept;

[[nodiscard]] GatherRouteStep ResetGatherRoute(
    GatherRouteIntent const& intent,
    RouteSession const& session,
    Tick now,
    bool explicitReentrySignal = false) noexcept;

[[nodiscard]] bool IsExactLiveBound(
    GatherRouteIntent const& intent,
    RouteSession const& session) noexcept;

struct DispatchResult
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    FixedTargetReason reason = FixedTargetReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
    NativeStepPhase phase = NativeStepPhase::Unknown;
    std::uint8_t stepsInvoked = 0;
    bool nativeCalled = false;
    bool beforeValidated = false;
    bool afterValidated = false;
    AutoWowGather::GatherCreditObservation credit;
    AutoWowGather::GatherCreditDecision creditDecision = AutoWowGather::GatherCreditDecision::AwaitingInteraction;
    ExecutorRequest request;
    FixedTargetObservation before;
    FixedTargetObservation after;

    // Available only on DispatchAfterRoute. Pending/blocked routes preserve their typed T2
    // failure and receipt evidence without invoking the native gather callback.
    bool routeValidated = false;
    bool routeExactLiveBound = false;
    bool routeInteractionReady = false;
    RouteFailure routeFailure = RouteFailure::None;
    RouteResult routeResult;
    GatherRouteEvidence routeEvidence;
};

// Pure field-for-field comparison. It covers spawnId, entry, map/instance, material, goal, and every
// alive/available/reachable/profession/tool fact. No distance or candidate ordering is consulted.
[[nodiscard]] bool ExactCandidateMatches(FixedTargetCandidate const& expected,
                                         FixedTargetCandidate const& observed) noexcept;

// Request-facing identity/fact check used before the native call.
[[nodiscard]] bool ExactCandidateMatches(ExecutorRequest const& request,
                                         FixedTargetCandidate const& candidate) noexcept;

class OracleGatherExecutor final
{
public:
    // This validates a typed request that was prepared by an upstream contract/arbiter seam. It
    // intentionally does not call PrepareRequest(); the fresh proof in observation.world is
    // authoritative;
    // ExecutorRequest::ownershipProof is checked only as a copied consistency field.
    [[nodiscard]] static ValidationResult Validate(ExecutorRequest const& request,
                                                    IntentLease const& lease,
                                                    FixedTargetCandidate const& exactTarget,
                                                    FixedTargetObservation const& observation,
                                                    bool allowNewerFrame = false) noexcept;

    // The sole native boundary: one exact candidate, one seek/interact/loot phase, max one step,
    // and immediate post-step revalidation. There is no selector, retry loop, teleport, or
    // random fallback in this function.
    [[nodiscard]] static DispatchResult Dispatch(ExecutorRequest const& request,
                                                  IntentLease const& lease,
                                                  FixedTargetCandidate const& exactTarget,
                                                  FixedTargetObservation const& before,
                                                  NativeStepFunction nativeStep,
                                                  void* context = nullptr) noexcept;

    // Route-gated native boundary. A completed Verify observation must still identify the exact
    // currently loaded source before the existing T1 safety/tool/skill/bag/credit checks can run.
    [[nodiscard]] static DispatchResult DispatchAfterRoute(
        ExecutorRequest const& request,
        IntentLease const& lease,
        FixedTargetCandidate const& exactTarget,
        FixedTargetObservation const& before,
        GatherRouteIntent const& routeIntent,
        GatherRouteStep const& routeStep,
        NativeStepFunction nativeStep,
        void* context = nullptr) noexcept;
};

} // namespace AutoWowOracleGatherExecutor

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_GATHER_EXECUTOR_H
