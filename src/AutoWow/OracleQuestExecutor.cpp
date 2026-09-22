#include "OracleQuestExecutor.h"

#include <cmath>
#include <cstring>

namespace AutoWowOracleQuestExecutor
{
namespace
{
using AutoWowOracleExecutor::MakeReceipt;
using AutoWowOracleExecutor::PrepareRequest;
using AutoWowOracleExecutor::ValidateRequest;

inline constexpr std::size_t kQuestRouteHandoffCapacity = AutoWowOracle::kMaxBotLeases;
static_assert(kQuestRouteHandoffCapacity == 256);
std::array<QuestRouteRuntimeHandoff, kQuestRouteHandoffCapacity> questRouteHandoffs{};

QuestRouteRuntimeHandoff* FindQuestRouteHandoffSlot(AutoWowOracleRoute::ActorId actor,
                                                    bool create) noexcept
{
    if (actor == 0)
        return nullptr;
    for (QuestRouteRuntimeHandoff& slot : questRouteHandoffs)
        if (slot.occupied && slot.actor == actor)
            return &slot;
    if (!create)
        return nullptr;
    for (QuestRouteRuntimeHandoff& slot : questRouteHandoffs)
    {
        if (slot.occupied)
            continue;
        slot = {};
        slot.occupied = true;
        slot.actor = actor;
        return &slot;
    }
    return nullptr;
}

bool SameDurableRouteIdentity(AutoWowOracleRoute::RouteLedgerKey const& left,
                              AutoWowOracleRoute::RouteLedgerKey const& right) noexcept
{
    AutoWowOracleRoute::RouteLedgerKey leftBase = left;
    AutoWowOracleRoute::RouteLedgerKey rightBase = right;
    leftBase.failureClass = AutoWowOracleRoute::RouteFailureClass::None;
    rightBase.failureClass = AutoWowOracleRoute::RouteFailureClass::None;
    return leftBase == rightBase;
}

ValidationResult Reject(ExecutorStatus status, QuestObjectiveReason reason,
                        ExecutorReason sharedReason = ExecutorReason::InvalidRequest) noexcept
{
    return {false, status, reason, sharedReason};
}

ValidationResult Accept() noexcept
{
    return {true, ExecutorStatus::Accepted, QuestObjectiveReason::None, ExecutorReason::None};
}

QuestObjectiveReason MapSharedReason(ExecutorReason reason) noexcept
{
    switch (reason)
    {
        case ExecutorReason::None:
            return QuestObjectiveReason::None;
        case ExecutorReason::InvalidRequest:
        case ExecutorReason::InvalidDecision:
            return QuestObjectiveReason::InvalidRequest;
        case ExecutorReason::LeaseMissing:
        case ExecutorReason::MissingOwnershipProof:
            return QuestObjectiveReason::MissingOwnership;
        case ExecutorReason::LeaseExpired:
            return QuestObjectiveReason::LeaseExpired;
        case ExecutorReason::LeaseMismatch:
            return QuestObjectiveReason::LeaseMismatch;
        case ExecutorReason::LeasePreempted:
            return QuestObjectiveReason::LeasePreempted;
        case ExecutorReason::UnknownOperation:
            return QuestObjectiveReason::WrongOperation;
        case ExecutorReason::OperationBlocked:
            return QuestObjectiveReason::WrongOperation;
        case ExecutorReason::InvalidOperationPayload:
        case ExecutorReason::InvalidQuestReference:
            return QuestObjectiveReason::InvalidQuestReference;
        case ExecutorReason::StaleFrame:
            return QuestObjectiveReason::StaleFrame;
        case ExecutorReason::StaleEpoch:
            return QuestObjectiveReason::StaleEpoch;
        case ExecutorReason::ProofMismatch:
        case ExecutorReason::ReceiptMismatch:
        case ExecutorReason::InvalidReceipt:
            return QuestObjectiveReason::PostconditionInvalid;
        case ExecutorReason::OracleRejected:
            return QuestObjectiveReason::InvalidRequest;
        case ExecutorReason::ExecutorFailed:
            return QuestObjectiveReason::NativeRejected;
    }
    return QuestObjectiveReason::InvalidRequest;
}

ValidationResult FromShared(AutoWowOracleExecutor::ValidationResult const& shared) noexcept
{
    return {shared.valid, shared.status, MapSharedReason(shared.reason), shared.reason};
}

QuestObjectiveReason OperationRejection(OperationCode operation) noexcept
{
    switch (operation)
    {
        case OperationCode::QuestAcquire:
            return QuestObjectiveReason::QuestAcquireRejected;
        case OperationCode::QuestAccept:
            return QuestObjectiveReason::QuestAcceptRejected;
        case OperationCode::QuestTurnIn:
            return QuestObjectiveReason::QuestTurnInRejected;
        case OperationCode::Recover:
            return QuestObjectiveReason::RecoveryRejected;
        case OperationCode::QuestObjective:
            return QuestObjectiveReason::None;
        default:
            return QuestObjectiveReason::WrongOperation;
    }
}

ValidationResult RejectOperation(ExecutorRequest const& request) noexcept
{
    QuestObjectiveReason const reason = OperationRejection(request.operation);
    return Reject(ExecutorStatus::Rejected, reason, ExecutorReason::OperationBlocked);
}

ValidationResult ValidateObjectiveShape(QuestObjectiveSnapshot const& objective) noexcept
{
    if (!objective.valid || objective.botGuid == 0 || objective.questId == 0 ||
        objective.family == QuestObjectiveFamily::None || objective.targetGuid == 0 ||
        objective.requiredCount == 0 || objective.currentCount > objective.requiredCount)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::InvalidObjectiveSnapshot,
                      ExecutorReason::InvalidQuestReference);

    if (!objective.targetMapApplicable && objective.targetMapId != 0)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::InvalidObjectiveSnapshot,
                      ExecutorReason::InvalidQuestReference);

    switch (objective.family)
    {
        case QuestObjectiveFamily::NpcOrGameObject:
            if (objective.requiredEntry == 0 || objective.requiredItemId != 0)
                return Reject(ExecutorStatus::Rejected,
                              QuestObjectiveReason::InvalidObjectiveSnapshot,
                              ExecutorReason::InvalidQuestReference);
            break;
        case QuestObjectiveFamily::Item:
            if (objective.requiredEntry != 0 || objective.requiredItemId == 0)
                return Reject(ExecutorStatus::Rejected,
                              QuestObjectiveReason::InvalidObjectiveSnapshot,
                              ExecutorReason::InvalidQuestReference);
            break;
        case QuestObjectiveFamily::None:
            return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::InvalidObjectiveSnapshot,
                          ExecutorReason::InvalidQuestReference);
    }

    if (!IsBoundedObjectivePhase(objective.phase))
    {
        QuestObjectiveReason reason = QuestObjectiveReason::ObjectivePhaseRejected;
        switch (objective.phase)
        {
            case ObjectiveStepPhase::Finisher:
                reason = QuestObjectiveReason::FinisherRejected;
                break;
            case ObjectiveStepPhase::Recovery:
                reason = QuestObjectiveReason::RecoveryRejected;
                break;
            case ObjectiveStepPhase::StartQuest:
                reason = QuestObjectiveReason::QuestStartRejected;
                break;
            case ObjectiveStepPhase::AcquireQuest:
                reason = QuestObjectiveReason::QuestAcquireRejected;
                break;
            case ObjectiveStepPhase::AcceptQuest:
                reason = QuestObjectiveReason::QuestAcceptRejected;
                break;
            case ObjectiveStepPhase::TurnInQuest:
                reason = QuestObjectiveReason::QuestTurnInRejected;
                break;
            case ObjectiveStepPhase::Unknown:
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
                break;
        }
        return Reject(ExecutorStatus::Rejected, reason, ExecutorReason::OperationBlocked);
    }

    return Accept();
}

ValidationResult ValidateRequestContext(ExecutorRequest const& request,
                                        QuestObjectiveObservation const& observation,
                                        bool allowNewerFrame) noexcept
{
    ObjectiveWorldSnapshot const& world = observation.world;
    if (!request.valid)
        return Reject(ExecutorStatus::Invalid, QuestObjectiveReason::InvalidRequest,
                      ExecutorReason::InvalidRequest);
    if (!world.onWorldThread)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::NotWorldThread,
                      ExecutorReason::InvalidRequest);
    if (!world.botAlive)
        return Reject(ExecutorStatus::Blocked, QuestObjectiveReason::NativeUnavailable,
                      ExecutorReason::OperationBlocked);
    QuestObjectiveReason const operationReason = OperationRejection(request.operation);
    if (operationReason != QuestObjectiveReason::None)
        return RejectOperation(request);
    if (request.domain != AutoWowOracle::Domain::Quest)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::WrongDomain,
                      ExecutorReason::OperationBlocked);
    if (request.intent != AutoWowOracle::IntentCode::QuestObjective)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::WrongIntent,
                      ExecutorReason::OperationBlocked);
    if (request.resource != AutoWowOracle::LeaseResource::QuestGather)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::WrongResource,
                      ExecutorReason::OperationBlocked);
    if (!request.executorAvailable)
        return Reject(ExecutorStatus::Blocked, QuestObjectiveReason::ExecutorUnavailable,
                      ExecutorReason::OperationBlocked);

    AutoWowOracleExecutor::ValidationResult const shared =
        ValidateRequest(request, world.ownership, world.tick);
    if (!shared.valid)
        return FromShared(shared);

    if (world.botGuid == 0 || world.botGuid != request.actorGuid ||
        world.botGuid != request.scope.botGuid || world.epoch != request.epoch)
        return Reject(ExecutorStatus::Rejected,
                      world.epoch != request.epoch ? QuestObjectiveReason::StaleEpoch :
                                                     QuestObjectiveReason::WrongBot,
                      world.epoch != request.epoch ? ExecutorReason::StaleEpoch :
                                                      ExecutorReason::LeaseMismatch);

    if (allowNewerFrame ? world.frameVersion < request.frameVersion
                        : world.frameVersion != request.frameVersion)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::StaleFrame,
                      ExecutorReason::StaleFrame);

    ValidationResult const objectiveShape = ValidateObjectiveShape(observation.objective);
    if (!objectiveShape.valid)
        return objectiveShape;

    QuestObjectiveSnapshot const& objective = observation.objective;
    if (objective.botGuid != request.actorGuid)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::WrongBot,
                      ExecutorReason::LeaseMismatch);
    if (objective.questId != request.quest.questId)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::QuestIdMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.family != request.quest.objectiveFamily)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::FamilyMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.slot != request.quest.objectiveSlot)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::SlotMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.requiredEntry != request.quest.requiredEntry)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::RequiredEntryMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.requiredItemId != request.quest.requiredItemId)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::RequiredItemMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.targetGuid != request.targetGuid)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::TargetGuidMismatch,
                      ExecutorReason::InvalidQuestReference);
    if (objective.targetMapApplicable && objective.targetMapId != world.mapId)
        return Reject(ExecutorStatus::Rejected, QuestObjectiveReason::TargetMapMismatch,
                      ExecutorReason::InvalidQuestReference);

    return Accept();
}

void CopyValidation(PreparedObjective& result, ValidationResult const& validation) noexcept
{
    result.valid = validation.valid;
    result.status = validation.status;
    result.reason = validation.reason;
    result.sharedReason = validation.sharedReason;
}

void CopyValidation(DispatchResult& result, ValidationResult const& validation) noexcept
{
    result.valid = validation.valid;
    result.status = validation.status;
    result.reason = validation.reason;
    result.sharedReason = validation.sharedReason;
}

ObjectiveRuntimeLock MakeRuntimeLock(ExecutorRequest const& request,
                                     QuestObjectiveSnapshot const& objective) noexcept
{
    ObjectiveRuntimeLock runtime;
    runtime.locked = true;
    runtime.objective = request.quest;
    runtime.targetGuid = objective.targetGuid;
    runtime.targetMapId = objective.targetMapId;
    runtime.targetMapApplicable = objective.targetMapApplicable;
    runtime.baselineCount = objective.currentCount;
    runtime.requiredCount = objective.requiredCount;
    runtime.maxSteps = 1;
    runtime.stopAfterStep = true;
    return runtime;
}

PreparedObjective MakePreparationFailure(ExecutorRequest const& request,
                                         QuestObjectiveObservation const& observation,
                                         ValidationResult const& validation) noexcept
{
    PreparedObjective result;
    result.request = request;
    result.objective = observation.objective;
    CopyValidation(result, validation);
    return result;
}

DispatchResult MakeDispatchFailure(PreparedObjective const& prepared,
                                   QuestObjectiveReason reason, ExecutorReason sharedReason,
                                   ExecutorStatus status = ExecutorStatus::Rejected) noexcept
{
    DispatchResult result;
    result.request = prepared.request;
    result.objectiveRuntime = prepared.objectiveRuntime;
    result.valid = false;
    result.accepted = false;
    result.status = status;
    result.reason = reason;
    result.sharedReason = sharedReason;
    return result;
}

}  // namespace

QuestRouteReentrySignal EvaluateQuestRouteReentry(
    QuestRouteReentryFacts const& blocked, QuestRouteReentryFacts const& current) noexcept
{
    if (blocked.targetLoadKnown && current.targetLoadKnown && !blocked.targetLoaded &&
        current.targetLoaded)
        return QuestRouteReentrySignal::TargetNewlyLiveLoaded;
    if (blocked.positionKnown && current.positionKnown)
    {
        if (blocked.mapId != current.mapId)
            return QuestRouteReentrySignal::MovedOver100Yards;
        if (std::isfinite(blocked.x) && std::isfinite(blocked.y) &&
            std::isfinite(current.x) && std::isfinite(current.y))
        {
            double const dx = current.x - blocked.x;
            double const dy = current.y - blocked.y;
            if (dx * dx + dy * dy > 100.0 * 100.0)
                return QuestRouteReentrySignal::MovedOver100Yards;
        }
    }
    if (blocked.serverSessionId != 0 && current.serverSessionId != 0 &&
        blocked.serverSessionId != current.serverSessionId)
        return QuestRouteReentrySignal::ServerSession;
    return QuestRouteReentrySignal::None;
}

std::uint64_t MakeQuestRouteIdentity(AutoWowOracleRoute::RouteKey const& key) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](std::uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    auto mixDouble = [&mix](double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    mix(key.actor);
    mix(key.purpose);
    mix(key.mapId);
    mix(key.instanceId);
    mix(static_cast<std::uint64_t>(key.targetKind));
    mix(key.entry);
    mix(key.stableSpawn);
    mixDouble(key.dbX);
    mixDouble(key.dbY);
    mixDouble(key.dbZ);
    mixDouble(key.dbO);
    mixDouble(key.radius);
    mix(static_cast<std::uint64_t>(key.segment));
    mix(key.questId);
    mix(key.objectiveFamily);
    mix(key.objectiveSlot);
    mix(key.routeCatalogVersion);
    return hash == 0 ? 1 : hash;
}

QuestRouteBackoffSet CollectActiveQuestRouteBackoffs(
    AutoWowOracleRoute::ActorId actor, AutoWowOracleRoute::Tick now,
    AutoWowOracleRoute::QuestId reentryEligibleQuestId) noexcept
{
    QuestRouteBackoffSet result;
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (!slot)
        return result;
    for (std::uint8_t index = 0;
         index < slot->ledger.entryCount &&
         index < AutoWowOracleRoute::kMaxSessionLedgerEntries; ++index)
    {
        AutoWowOracleRoute::SessionLedgerEntry const& entry = slot->ledger.entries[index];
        AutoWowOracleRoute::QuestId const questId = entry.ledgerKey.questId;
        bool const active = entry.occupied && questId != 0 &&
            (now < entry.nextRetryAt ||
             (entry.requiresReentry && questId != reentryEligibleQuestId));
        if (!active)
            continue;
        bool duplicate = false;
        for (std::uint8_t existing = 0; existing < result.count; ++existing)
            duplicate = duplicate || result.questIds[existing] == questId;
        if (!duplicate && result.count < result.questIds.size())
            result.questIds[result.count++] = questId;
    }
    return result;
}

AutoWowOracleRoute::SessionLedger LoadQuestRouteLedger(
    AutoWowOracleRoute::ActorId actor) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    return slot ? slot->ledger : AutoWowOracleRoute::SessionLedger{};
}

bool StoreQuestRouteLedger(AutoWowOracleRoute::ActorId actor,
                           AutoWowOracleRoute::SessionLedger const& ledger) noexcept
{
    if (QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, true))
    {
        slot->ledger = ledger;
        return true;
    }
    return false;
}

bool PublishQuestRouteHandoff(AutoWowOracleRoute::ActorId actor,
                              AutoWowOracleRoute::RouteStep const& step,
                              QuestRouteReentryFacts const& blockedFacts) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, true);
    if (!slot)
        return false;
    slot->actor = actor;
    slot->questId = step.result.routeKey.questId;
    slot->objectiveFamily = step.result.routeKey.objectiveFamily;
    slot->objectiveSlot = step.result.routeKey.objectiveSlot;
    slot->routeCatalogVersion = step.result.routeKey.routeCatalogVersion;
    slot->lease = step.result.routeKey.lease;
    slot->blockKey = step.result.ledgerKey;
    slot->failure = step.result.failure;
    slot->nextRetryAt = step.result.nextRetryAt;
    slot->blockedWorldChangeId = 0;
    slot->blockedReentryFacts = blockedFacts;
    slot->qualifyingReentrySignal = QuestRouteReentrySignal::None;
    slot->requiresWorldChange = false;
    for (std::uint8_t index = 0;
         index < step.session.ledger.entryCount &&
         index < AutoWowOracleRoute::kMaxSessionLedgerEntries; ++index)
    {
        AutoWowOracleRoute::SessionLedgerEntry const& entry = step.session.ledger.entries[index];
        if (entry.occupied &&
            SameDurableRouteIdentity(entry.ledgerKey, step.result.ledgerKey))
        {
            if (entry.nextRetryAt >= slot->nextRetryAt)
            {
                slot->nextRetryAt = entry.nextRetryAt;
                slot->requiresWorldChange = entry.requiresReentry;
                slot->blockedWorldChangeId = entry.blockedWorldChangeId;
                slot->blockKey = entry.ledgerKey;
            }
        }
    }
    slot->blocked = step.result.blocked;
    slot->arrived = step.result.arrived;
    slot->interactionPending = step.result.arrived;
    slot->nativeInteractionObserved = false;
    slot->leaseReleaseRequested = step.result.releaseLease;
    slot->leaseReleaseAuthoritative = step.result.leaseReleaseAuthoritative;
    slot->result = step.result;
    slot->ledger = step.session.ledger;
    return true;
}

bool GetQuestRouteHandoff(AutoWowOracleRoute::ActorId actor,
                          QuestRouteRuntimeHandoff& out) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (!slot)
        return false;
    out = *slot;
    return true;
}

bool SetQuestRouteServerSession(AutoWowOracleRoute::ActorId actor,
                                std::uint64_t serverSessionId) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (!slot || serverSessionId == 0)
        return false;
    if (slot->blockedReentryFacts.serverSessionId == 0)
        slot->blockedReentryFacts.serverSessionId = serverSessionId;
    return true;
}

bool RecordQuestRouteReentrySignal(AutoWowOracleRoute::ActorId actor,
                                   QuestRouteReentrySignal signal) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (!slot || signal == QuestRouteReentrySignal::None)
        return false;
    slot->qualifyingReentrySignal = signal;
    return true;
}

void RecordQuestRouteNativeInteraction(AutoWowOracleRoute::ActorId actor,
                                       AutoWowOracleRoute::LeaseId lease) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (slot && slot->arrived && slot->interactionPending && lease != 0 &&
        slot->lease == lease)
        slot->nativeInteractionObserved = true;
}

bool CompleteQuestRouteLeaseRelease(AutoWowOracleRoute::ActorId actor,
                                    AutoWowOracleRoute::LeaseId lease,
                                    AutoWowOracleRoute::Tick now,
                                    bool arbiterReleaseSucceeded,
                                    QuestRouteRuntimeHandoff& out) noexcept
{
    QuestRouteRuntimeHandoff* slot = FindQuestRouteHandoffSlot(actor, false);
    if (!slot || lease == 0 || slot->lease != lease ||
        (!slot->blocked && !slot->arrived))
        return false;
    slot->leaseReleaseRequested = true;
    slot->leaseReleaseAuthoritative = arbiterReleaseSucceeded;
    slot->interactionPending = false;
    slot->result.releaseLease = true;
    slot->result.leaseReleaseAuthoritative = arbiterReleaseSucceeded;
    if (!arbiterReleaseSucceeded)
    {
        slot->qualifyingReentrySignal = QuestRouteReentrySignal::None;
        AutoWowOracleRoute::LedgerUpdate const update = AutoWowOracleRoute::RecordBlocked(
            slot->ledger, AutoWowOracleRoute::MakeRouteLedgerKey(slot->result.routeKey), now,
            AutoWowOracleRoute::RouteFailure::LeaseReleaseFailed);
        if (update.accepted)
        {
            slot->ledger = update.ledger;
            slot->blockKey = update.ledgerKey;
            slot->nextRetryAt = update.nextRetryAt;
            slot->result.ledgerKey = update.ledgerKey;
            slot->result.nextRetryAt = update.nextRetryAt;
            slot->requiresWorldChange = update.requiresReentry;
        }
        slot->failure = AutoWowOracleRoute::RouteFailure::LeaseReleaseFailed;
        slot->blocked = true;
        slot->arrived = false;
        slot->result.failure = AutoWowOracleRoute::RouteFailure::LeaseReleaseFailed;
        slot->result.blocked = true;
        slot->result.arrived = false;
        slot->result.stage = AutoWowOracleRoute::RouteStage::Blocked;
    }
    if (slot->result.nextRetryAt == 0 && slot->blocked)
        slot->result.nextRetryAt = now;
    out = *slot;
    return true;
}

ValidationResult OracleQuestExecutor::Validate(ExecutorRequest const& request,
                                                QuestObjectiveObservation const& observation,
                                                bool allowNewerFrame) noexcept
{
    return ValidateRequestContext(request, observation, allowNewerFrame);
}

PreparedObjective OracleQuestExecutor::Prepare(ExecutorRequest const& request,
                                               QuestObjectiveObservation const& observation) noexcept
{
    ValidationResult const validation = Validate(request, observation, false);
    if (!validation.valid)
        return MakePreparationFailure(request, observation, validation);

    if (observation.objective.currentCount >= observation.objective.requiredCount)
    {
        ValidationResult const complete = Reject(ExecutorStatus::Blocked,
            QuestObjectiveReason::ObjectiveAlreadyComplete, ExecutorReason::OperationBlocked);
        return MakePreparationFailure(request, observation, complete);
    }

    PreparedObjective result;
    result.valid = true;
    result.accepted = true;
    result.status = ExecutorStatus::Accepted;
    result.reason = QuestObjectiveReason::None;
    result.sharedReason = ExecutorReason::None;
    result.request = request;
    result.objective = observation.objective;
    result.objectiveRuntime = MakeRuntimeLock(request, observation.objective);
    result.receipt = MakeReceipt(request, ExecutorStatus::Accepted, ExecutorReason::None,
                                 observation.world.ownership, observation.world.tick,
                                 request.proofBaseline);
    return result;
}

PreparedObjective OracleQuestExecutor::Prepare(IntentLease const& lease,
                                               QuestObjectiveObservation const& observation) noexcept
{
    AutoWowOracleExecutor::ExecutorResult const shared =
        PrepareRequest(lease, observation.world.ownership, observation.world.tick);
    if (!shared.accepted)
    {
        PreparedObjective result;
        result.request = shared.request;
        result.objective = observation.objective;
        result.receipt = shared.receipt;
        ValidationResult const validation = Reject(
            shared.status, MapSharedReason(shared.reason), shared.reason);
        CopyValidation(result, validation);
        if (shared.reason == ExecutorReason::LeaseMissing)
            result.reason = QuestObjectiveReason::MissingOwnership;
        return result;
    }

    return Prepare(shared.request, observation);
}

DispatchResult OracleQuestExecutor::Dispatch(PreparedObjective const& prepared,
                                              QuestObjectiveObservation const& before,
                                              NativeStepFunction nativeStep, void* context) noexcept
{
    if (!prepared.valid || !prepared.accepted || !prepared.request.valid ||
        !prepared.objectiveRuntime.locked || prepared.objectiveRuntime.maxSteps != 1 ||
        !prepared.objectiveRuntime.stopAfterStep)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::InvalidRequest,
                                   ExecutorReason::InvalidRequest, ExecutorStatus::Invalid);

    DispatchResult result;
    result.request = prepared.request;
    result.objectiveRuntime = prepared.objectiveRuntime;
    ValidationResult const beforeValidation = Validate(prepared.request, before, false);
    result.beforeValidated = beforeValidation.valid;
    if (!beforeValidation.valid)
    {
        CopyValidation(result, beforeValidation);
        return result;
    }
    if (before.objective.currentCount >= before.objective.requiredCount)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::ObjectiveAlreadyComplete,
                                   ExecutorReason::OperationBlocked, ExecutorStatus::Blocked);
    if (!nativeStep)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::NativeUnavailable,
                                   ExecutorReason::ExecutorFailed, ExecutorStatus::Failed);

    NativeObjectiveStepRequest const nativeRequest{
        prepared.request, before.objective, prepared.objectiveRuntime, 1, true};
    NativeStepObservation const native = nativeStep(context, nativeRequest);
    result.nativeCalled = true;
    result.stepsInvoked = native.stepsInvoked;

    // Revalidation is deliberately performed before interpreting the callback's transition flags.
    // A callback cannot turn a stale/preempted post-snapshot into a success by reporting a flag.
    ValidationResult const afterValidation = Validate(prepared.request, native.after, true);
    result.afterValidated = afterValidation.valid;
    if (!afterValidation.valid)
    {
        // A finisher/recovery phase after the callback is an automatic transition, not merely a
        // generic phase mismatch. Keep identity/lease mismatch reasons precise for diagnostics.
        if (native.after.objective.phase == ObjectiveStepPhase::Finisher ||
            native.after.objective.phase == ObjectiveStepPhase::Recovery ||
            native.after.objective.phase == ObjectiveStepPhase::StartQuest ||
            native.after.objective.phase == ObjectiveStepPhase::AcquireQuest ||
            native.after.objective.phase == ObjectiveStepPhase::AcceptQuest ||
            native.after.objective.phase == ObjectiveStepPhase::TurnInQuest)
            return MakeDispatchFailure(prepared, QuestObjectiveReason::AutomaticTransitionRejected,
                                       afterValidation.sharedReason);
        CopyValidation(result, afterValidation);
        return result;
    }
    if (!native.dispatchAccepted)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::NativeRejected,
                                   ExecutorReason::ExecutorFailed, ExecutorStatus::Failed);
    if (native.stepsInvoked != 1)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::StepBudgetExceeded,
                                   ExecutorReason::ExecutorFailed, ExecutorStatus::Failed);
    if (native.transitionedToAnotherObjective || native.enteredFinisher)
        return MakeDispatchFailure(prepared, QuestObjectiveReason::AutomaticTransitionRejected,
                                   ExecutorReason::ExecutorFailed);

    result.valid = true;
    result.accepted = true;
    result.status = native.after.objective.currentCount >= native.after.objective.requiredCount
        ? ExecutorStatus::Completed
        : ExecutorStatus::Progressing;
    result.reason = QuestObjectiveReason::None;
    result.sharedReason = ExecutorReason::None;
    result.receipt = MakeReceipt(prepared.request, result.status, ExecutorReason::None,
                                 native.after.world.ownership, native.after.world.tick,
                                 prepared.request.proofBaseline, 1);
    return result;
}

QuestRoutePolicyStep OracleQuestExecutor::StartQuestRoute(
    QuestRouteRequest const& request) noexcept
{
    QuestRoutePolicyStep result;
    if (!request.oracleManaged || !request.oracleTagged || !request.enabled)
        return result;

    result.applicable = true;
    result.route = AutoWowOracleRoute::StartRoute(
        request.intent, request.estimatedTravelSeconds, request.now, request.ledger,
        request.explicitReentrySignal, request.currentWorldChangeId);
    return result;
}

QuestRoutePolicyStep OracleQuestExecutor::AdvanceQuestRoute(
    bool oracleManaged, bool oracleTagged, bool enabled,
    AutoWowOracleRoute::RouteSession const& session,
    AutoWowOracleRoute::RouteObservation const& observation,
    AutoWowOracleRoute::Tick now) noexcept
{
    QuestRoutePolicyStep result;
    if (!oracleManaged || !oracleTagged || !enabled)
        return result;

    result.applicable = true;
    result.route = AutoWowOracleRoute::AdvanceRoute(session, observation, now);
    return result;
}

QuestRoutePolicyStep OracleQuestExecutor::ResetQuestRoute(
    bool oracleManaged, bool oracleTagged, bool enabled,
    AutoWowOracleRoute::RouteSession const& session,
    AutoWowOracleRoute::Tick now, bool explicitReentrySignal,
    AutoWowOracleRoute::WorldChangeId currentWorldChangeId) noexcept
{
    QuestRoutePolicyStep result;
    if (!oracleManaged || !oracleTagged || !enabled)
        return result;

    result.applicable = true;
    result.route = AutoWowOracleRoute::ResetAttempt(
        session, now, explicitReentrySignal, currentWorldChangeId);
    return result;
}

}  // namespace AutoWowOracleQuestExecutor
