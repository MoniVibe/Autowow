/*
 * Bounded AutoWow QuestObjective adapter.
 *
 * This lane consumes the authoritative AutoWowOracleExecutor request types. It does not create,
 * renew, release, or arbitrate ownership. The caller must pass a fresh ActiveLeaseSnapshot from
 * the arbiter in every observation; a copied ownershipProof inside an ExecutorRequest is never
 * treated as current by itself.
 *
 * Prepare() and Validate() are pure policy/data checks. Dispatch() is the only native boundary:
 * it gives one fixed objective step to the supplied callback, with a one-step budget and a locked
 * objectiveRuntime, then validates the immediately returned world/objective snapshot before it
 * can report success. The callback must be the narrowly scoped fixed-objective native entry point;
 * it must not call the general "new rpg do quest" driver, select another objective, or turn in a
 * quest.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_EXECUTOR_H

#include "OracleExecutorTypes.h"
#include "OracleRouteExecutor.h"

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace AutoWowOracleQuestExecutor
{
using AutoWowOracle::ActiveLeaseSnapshot;
using AutoWowOracle::Epoch;
using AutoWowOracle::FactVersion;
using AutoWowOracle::Guid;
using AutoWowOracle::IntentLease;
using AutoWowOracle::ItemId;
using AutoWowOracle::OperationCode;
using AutoWowOracle::QuestId;
using AutoWowOracle::QuestObjectiveFamily;
using AutoWowOracle::QuestReference;
using AutoWowOracle::Tick;

using AutoWowOracleExecutor::ExecutorReason;
using AutoWowOracleExecutor::ExecutorRequest;
using AutoWowOracleExecutor::ExecutorReceipt;
using AutoWowOracleExecutor::ExecutorStatus;

// The first group is the only phase set that this adapter may dispatch. The remaining values are
// explicit rejection inputs so a caller cannot smuggle quest acquisition, finisher, or recovery
// work through an objective callback.
enum class ObjectiveStepPhase : std::uint8_t
{
    Unknown = 0,
    ResolveObjective,
    TravelToSource,
    AcquireTarget,
    SelfDefense,
    EngageTarget,
    InteractSource,
    UseQuestItem,
    EscortEvent,
    LootSource,
    VerifyProgress,
    WaitForRespawn,
    StartQuest,
    AcquireQuest,
    AcceptQuest,
    TurnInQuest,
    Finisher,
    Recovery
};

[[nodiscard]] constexpr bool IsBoundedObjectivePhase(ObjectiveStepPhase phase) noexcept
{
    switch (phase)
    {
        case ObjectiveStepPhase::ResolveObjective:
        case ObjectiveStepPhase::TravelToSource:
        case ObjectiveStepPhase::AcquireTarget:
        case ObjectiveStepPhase::SelfDefense:
        case ObjectiveStepPhase::EngageTarget:
        case ObjectiveStepPhase::InteractSource:
        case ObjectiveStepPhase::UseQuestItem:
        case ObjectiveStepPhase::EscortEvent:
        case ObjectiveStepPhase::LootSource:
        case ObjectiveStepPhase::VerifyProgress:
        case ObjectiveStepPhase::WaitForRespawn:
            return true;
        case ObjectiveStepPhase::Unknown:
        case ObjectiveStepPhase::StartQuest:
        case ObjectiveStepPhase::AcquireQuest:
        case ObjectiveStepPhase::AcceptQuest:
        case ObjectiveStepPhase::TurnInQuest:
        case ObjectiveStepPhase::Finisher:
        case ObjectiveStepPhase::Recovery:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr std::string_view ObjectiveStepPhaseName(ObjectiveStepPhase phase) noexcept
{
    switch (phase)
    {
        case ObjectiveStepPhase::Unknown: return "unknown";
        case ObjectiveStepPhase::ResolveObjective: return "resolve_objective";
        case ObjectiveStepPhase::TravelToSource: return "travel_to_source";
        case ObjectiveStepPhase::AcquireTarget: return "acquire_target";
        case ObjectiveStepPhase::SelfDefense: return "self_defense";
        case ObjectiveStepPhase::EngageTarget: return "engage_target";
        case ObjectiveStepPhase::InteractSource: return "interact_source";
        case ObjectiveStepPhase::UseQuestItem: return "use_quest_item";
        case ObjectiveStepPhase::EscortEvent: return "escort_event";
        case ObjectiveStepPhase::LootSource: return "loot_source";
        case ObjectiveStepPhase::VerifyProgress: return "verify_progress";
        case ObjectiveStepPhase::WaitForRespawn: return "wait_for_respawn";
        case ObjectiveStepPhase::StartQuest: return "start_quest";
        case ObjectiveStepPhase::AcquireQuest: return "acquire_quest";
        case ObjectiveStepPhase::AcceptQuest: return "accept_quest";
        case ObjectiveStepPhase::TurnInQuest: return "turn_in_quest";
        case ObjectiveStepPhase::Finisher: return "finisher";
        case ObjectiveStepPhase::Recovery: return "recovery";
    }
    return "unknown";
}

enum class QuestObjectiveReason : std::uint8_t
{
    None = 0,
    InvalidRequest,
    NotWorldThread,
    MissingOwnership,
    LeaseExpired,
    LeaseMismatch,
    LeasePreempted,
    StaleFrame,
    StaleEpoch,
    WrongOperation,
    QuestAcquireRejected,
    QuestStartRejected,
    QuestAcceptRejected,
    QuestTurnInRejected,
    FinisherRejected,
    RecoveryRejected,
    WrongDomain,
    WrongIntent,
    WrongResource,
    ExecutorUnavailable,
    InvalidQuestReference,
    InvalidObjectiveSnapshot,
    WrongBot,
    QuestIdMismatch,
    FamilyMismatch,
    SlotMismatch,
    RequiredEntryMismatch,
    RequiredItemMismatch,
    TargetGuidMismatch,
    TargetMapMismatch,
    ObjectiveAlreadyComplete,
    ObjectivePhaseRejected,
    NativeUnavailable,
    NativeRejected,
    StepBudgetExceeded,
    AutomaticTransitionRejected,
    PostconditionMismatch,
    PostconditionInvalid
};

[[nodiscard]] constexpr std::string_view QuestObjectiveReasonName(
    QuestObjectiveReason reason) noexcept
{
    switch (reason)
    {
        case QuestObjectiveReason::None: return "none";
        case QuestObjectiveReason::InvalidRequest: return "invalid_request";
        case QuestObjectiveReason::NotWorldThread: return "not_world_thread";
        case QuestObjectiveReason::MissingOwnership: return "missing_ownership";
        case QuestObjectiveReason::LeaseExpired: return "lease_expired";
        case QuestObjectiveReason::LeaseMismatch: return "lease_mismatch";
        case QuestObjectiveReason::LeasePreempted: return "lease_preempted";
        case QuestObjectiveReason::StaleFrame: return "stale_frame";
        case QuestObjectiveReason::StaleEpoch: return "stale_epoch";
        case QuestObjectiveReason::WrongOperation: return "wrong_operation";
        case QuestObjectiveReason::QuestAcquireRejected: return "quest_acquire_rejected";
        case QuestObjectiveReason::QuestStartRejected: return "quest_start_rejected";
        case QuestObjectiveReason::QuestAcceptRejected: return "quest_accept_rejected";
        case QuestObjectiveReason::QuestTurnInRejected: return "quest_turn_in_rejected";
        case QuestObjectiveReason::FinisherRejected: return "finisher_rejected";
        case QuestObjectiveReason::RecoveryRejected: return "recovery_rejected";
        case QuestObjectiveReason::WrongDomain: return "wrong_domain";
        case QuestObjectiveReason::WrongIntent: return "wrong_intent";
        case QuestObjectiveReason::WrongResource: return "wrong_resource";
        case QuestObjectiveReason::ExecutorUnavailable: return "executor_unavailable";
        case QuestObjectiveReason::InvalidQuestReference: return "invalid_quest_reference";
        case QuestObjectiveReason::InvalidObjectiveSnapshot: return "invalid_objective_snapshot";
        case QuestObjectiveReason::WrongBot: return "wrong_bot";
        case QuestObjectiveReason::QuestIdMismatch: return "quest_id_mismatch";
        case QuestObjectiveReason::FamilyMismatch: return "family_mismatch";
        case QuestObjectiveReason::SlotMismatch: return "slot_mismatch";
        case QuestObjectiveReason::RequiredEntryMismatch: return "required_entry_mismatch";
        case QuestObjectiveReason::RequiredItemMismatch: return "required_item_mismatch";
        case QuestObjectiveReason::TargetGuidMismatch: return "target_guid_mismatch";
        case QuestObjectiveReason::TargetMapMismatch: return "target_map_mismatch";
        case QuestObjectiveReason::ObjectiveAlreadyComplete: return "objective_already_complete";
        case QuestObjectiveReason::ObjectivePhaseRejected: return "objective_phase_rejected";
        case QuestObjectiveReason::NativeUnavailable: return "native_unavailable";
        case QuestObjectiveReason::NativeRejected: return "native_rejected";
        case QuestObjectiveReason::StepBudgetExceeded: return "step_budget_exceeded";
        case QuestObjectiveReason::AutomaticTransitionRejected:
            return "automatic_transition_rejected";
        case QuestObjectiveReason::PostconditionMismatch: return "postcondition_mismatch";
        case QuestObjectiveReason::PostconditionInvalid: return "postcondition_invalid";
    }
    return "invalid_request";
}

// This is the native/world-thread state that accompanies a fresh objective snapshot. It is a
// snapshot, not a thread token: the caller owns the invariant that the callback runs on the world
// thread and must provide a new ownership proof for every before/after observation.
struct ObjectiveWorldSnapshot
{
    bool onWorldThread = false;
    bool botAlive = true;
    Guid botGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    Tick tick = 0;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    ActiveLeaseSnapshot ownership;
};

// The fields here are deliberately the full identity, not a flattened POI/index. Counts may
// change after one native step; every identity field must remain equal to the typed request.
struct QuestObjectiveSnapshot
{
    bool valid = false;
    Guid botGuid = 0;
    QuestId questId = 0;
    QuestObjectiveFamily family = QuestObjectiveFamily::None;
    std::uint8_t slot = 0;
    std::uint32_t requiredEntry = 0;
    ItemId requiredItemId = 0;
    Guid targetGuid = 0;
    std::uint32_t targetMapId = 0;
    bool targetMapApplicable = true;
    std::uint32_t currentCount = 0;
    std::uint32_t requiredCount = 0;
    ObjectiveStepPhase phase = ObjectiveStepPhase::Unknown;
};

struct QuestObjectiveObservation
{
    ObjectiveWorldSnapshot world;
    QuestObjectiveSnapshot objective;
};

// This record is handed to the native callback as `objectiveRuntime`. It is the fixed-objective
// action entry point's lock: the callback gets one step, one identity, and an explicit stop bit.
struct ObjectiveRuntimeLock
{
    bool locked = false;
    QuestReference objective;
    Guid targetGuid = 0;
    std::uint32_t targetMapId = 0;
    bool targetMapApplicable = true;
    std::uint32_t baselineCount = 0;
    std::uint32_t requiredCount = 0;
    std::uint8_t maxSteps = 1;
    bool stopAfterStep = true;
};

struct ValidationResult
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    QuestObjectiveReason reason = QuestObjectiveReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
};

struct PreparedObjective
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    QuestObjectiveReason reason = QuestObjectiveReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
    ExecutorRequest request;
    QuestObjectiveSnapshot objective;
    ObjectiveRuntimeLock objectiveRuntime;
    ExecutorReceipt receipt;
};

struct NativeObjectiveStepRequest
{
    ExecutorRequest request;
    QuestObjectiveSnapshot objectiveBefore;
    ObjectiveRuntimeLock objectiveRuntime;
    std::uint8_t maxSteps = 1;
    bool stopAfterStep = true;
};

struct NativeStepObservation
{
    bool dispatchAccepted = false;
    std::uint8_t stepsInvoked = 0;
    bool transitionedToAnotherObjective = false;
    bool enteredFinisher = false;
    QuestObjectiveObservation after;
};

using NativeStepFunction = NativeStepObservation (*)(void* context,
                                                      NativeObjectiveStepRequest const& request);

struct DispatchResult
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    QuestObjectiveReason reason = QuestObjectiveReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
    ExecutorRequest request;
    ObjectiveRuntimeLock objectiveRuntime;
    std::uint8_t stepsInvoked = 0;
    bool nativeCalled = false;
    bool beforeValidated = false;
    bool afterValidated = false;
    ExecutorReceipt receipt;
};

// Pure admission wrapper for T2 route sessions.  Native movement remains owned by the caller;
// this adapter only ensures that the route policy cannot be entered by an ordinary RPG action.
struct QuestRouteRequest
{
    bool oracleManaged = false;
    bool oracleTagged = false;
    bool enabled = false;
    AutoWowOracleRoute::RouteIntent intent;
    double estimatedTravelSeconds = 0.0;
    AutoWowOracleRoute::Tick now = 0;
    AutoWowOracleRoute::SessionLedger ledger;
    bool explicitReentrySignal = false;
    AutoWowOracleRoute::WorldChangeId currentWorldChangeId = 0;
};

struct QuestRoutePolicyStep
{
    bool applicable = false;
    AutoWowOracleRoute::RouteStep route;
};

enum class QuestRouteReentrySignal : std::uint8_t
{
    None = 0,
    TargetNewlyLiveLoaded,
    MovedOver100Yards,
    ServerSession
};

struct QuestRouteReentryFacts
{
    bool targetLoadKnown = false;
    bool targetLoaded = false;
    bool positionKnown = false;
    AutoWowOracleRoute::MapId mapId = 0;
    double x = 0.0;
    double y = 0.0;
    std::uint64_t serverSessionId = 0;
};

struct QuestRouteBackoffSet
{
    std::array<AutoWowOracleRoute::QuestId,
               AutoWowOracleRoute::kMaxSessionLedgerEntries> questIds{};
    std::uint8_t count = 0;
};

struct QuestRouteRuntimeHandoff
{
    bool occupied = false;
    AutoWowOracleRoute::ActorId actor = 0;
    AutoWowOracleRoute::QuestId questId = 0;
    AutoWowOracleRoute::ObjectiveFamilyId objectiveFamily = 0;
    AutoWowOracleRoute::ObjectiveSlotId objectiveSlot = 0;
    AutoWowOracleRoute::RouteCatalogVersion routeCatalogVersion = 0;
    AutoWowOracleRoute::LeaseId lease = 0;
    AutoWowOracleRoute::RouteLedgerKey blockKey;
    AutoWowOracleRoute::RouteFailure failure = AutoWowOracleRoute::RouteFailure::None;
    AutoWowOracleRoute::Tick nextRetryAt = 0;
    AutoWowOracleRoute::WorldChangeId blockedWorldChangeId = 0;
    QuestRouteReentryFacts blockedReentryFacts;
    QuestRouteReentrySignal qualifyingReentrySignal = QuestRouteReentrySignal::None;
    bool requiresWorldChange = false;
    bool blocked = false;
    bool arrived = false;
    bool interactionPending = false;
    bool nativeInteractionObserved = false;
    bool leaseReleaseRequested = false;
    bool leaseReleaseAuthoritative = false;
    AutoWowOracleRoute::RouteResult result;
    AutoWowOracleRoute::SessionLedger ledger;
};

[[nodiscard]] QuestRouteReentrySignal EvaluateQuestRouteReentry(
    QuestRouteReentryFacts const& blocked, QuestRouteReentryFacts const& current) noexcept;
[[nodiscard]] std::uint64_t MakeQuestRouteIdentity(
    AutoWowOracleRoute::RouteKey const& routeKey) noexcept;
[[nodiscard]] QuestRouteBackoffSet CollectActiveQuestRouteBackoffs(
    AutoWowOracleRoute::ActorId actor, AutoWowOracleRoute::Tick now,
    AutoWowOracleRoute::QuestId reentryEligibleQuestId = 0) noexcept;
[[nodiscard]] AutoWowOracleRoute::SessionLedger LoadQuestRouteLedger(
    AutoWowOracleRoute::ActorId actor) noexcept;
[[nodiscard]] bool StoreQuestRouteLedger(
    AutoWowOracleRoute::ActorId actor,
    AutoWowOracleRoute::SessionLedger const& ledger) noexcept;
[[nodiscard]] bool PublishQuestRouteHandoff(
    AutoWowOracleRoute::ActorId actor, AutoWowOracleRoute::RouteStep const& step,
    QuestRouteReentryFacts const& blockedFacts) noexcept;
[[nodiscard]] bool GetQuestRouteHandoff(AutoWowOracleRoute::ActorId actor,
                                        QuestRouteRuntimeHandoff& out) noexcept;
[[nodiscard]] bool SetQuestRouteServerSession(
    AutoWowOracleRoute::ActorId actor, std::uint64_t serverSessionId) noexcept;
[[nodiscard]] bool RecordQuestRouteReentrySignal(
    AutoWowOracleRoute::ActorId actor, QuestRouteReentrySignal signal) noexcept;
void RecordQuestRouteNativeInteraction(AutoWowOracleRoute::ActorId actor,
                                       AutoWowOracleRoute::LeaseId lease) noexcept;
[[nodiscard]] bool CompleteQuestRouteLeaseRelease(
    AutoWowOracleRoute::ActorId actor, AutoWowOracleRoute::LeaseId lease,
    AutoWowOracleRoute::Tick now, bool arbiterReleaseSucceeded,
    QuestRouteRuntimeHandoff& out) noexcept;

class OracleQuestExecutor final
{
public:
    // Pure validation of a current ExecutorRequest and a caller-supplied fresh ownership/world
    // snapshot. allowNewerFrame is false for the pre-dispatch observation and true only for the
    // immediately returned post-dispatch observation.
    [[nodiscard]] static ValidationResult Validate(ExecutorRequest const& request,
                                                    QuestObjectiveObservation const& observation,
                                                    bool allowNewerFrame = false) noexcept;

    // Pure construction from an already prepared ExecutorRequest.
    [[nodiscard]] static PreparedObjective Prepare(ExecutorRequest const& request,
                                                   QuestObjectiveObservation const& observation) noexcept;

    // Pure construction from the authoritative shared IntentLease. PrepareRequest performs the
    // shared operation/ownership gate; this adapter only accepts its QuestObjective result.
    [[nodiscard]] static PreparedObjective Prepare(IntentLease const& lease,
                                                   QuestObjectiveObservation const& observation) noexcept;

    // The sole native boundary. The callback is invoked at most once, with maxSteps == 1 and a
    // locked objectiveRuntime. Its returned observation is validated immediately before success
    // is reported; objective/finisher transitions are explicit rejection paths.
    [[nodiscard]] static DispatchResult Dispatch(PreparedObjective const& prepared,
                                                  QuestObjectiveObservation const& before,
                                                  NativeStepFunction nativeStep,
                                                  void* context = nullptr) noexcept;

    [[nodiscard]] static QuestRoutePolicyStep StartQuestRoute(
        QuestRouteRequest const& request) noexcept;
    [[nodiscard]] static QuestRoutePolicyStep AdvanceQuestRoute(
        bool oracleManaged, bool oracleTagged, bool enabled,
        AutoWowOracleRoute::RouteSession const& session,
        AutoWowOracleRoute::RouteObservation const& observation,
        AutoWowOracleRoute::Tick now) noexcept;
    [[nodiscard]] static QuestRoutePolicyStep ResetQuestRoute(
        bool oracleManaged, bool oracleTagged, bool enabled,
        AutoWowOracleRoute::RouteSession const& session,
        AutoWowOracleRoute::Tick now, bool explicitReentrySignal = false,
        AutoWowOracleRoute::WorldChangeId currentWorldChangeId = 0) noexcept;
};

}  // namespace AutoWowOracleQuestExecutor

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_EXECUTOR_H
