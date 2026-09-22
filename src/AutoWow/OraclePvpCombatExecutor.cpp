/*
 * Pure fixed-target hostile-player combat executor.
 */
#include "OraclePvpCombatExecutor.h"

namespace AutoWowOraclePvpCombatExecutor
{
namespace
{
using AutoWowOracle::Decision;

ValidationResult Reject(ExecutorStatus status, PvpCombatReason reason,
                        ExecutorReason sharedReason) noexcept
{
    return {false, status, reason, sharedReason};
}

ValidationResult Accept() noexcept
{
    return {true, ExecutorStatus::Accepted, PvpCombatReason::None, ExecutorReason::None};
}

bool SameOwnership(ActiveLeaseSnapshot const& left,
                   ActiveLeaseSnapshot const& right) noexcept
{
    return left.active == right.active && left.botGuid == right.botGuid &&
        left.decisionId == right.decisionId && left.intentId == right.intentId &&
        left.epoch == right.epoch && left.expiresTick == right.expiresTick &&
        AutoWowOracle::SameScope(left.scope, right.scope) && left.operation == right.operation;
}

bool SameReference(HostilePlayerReference const& left,
                   HostilePlayerReference const& right) noexcept
{
    return left.valid == right.valid && left.isPlayer == right.isPlayer &&
        left.targetGuid == right.targetGuid && left.mapId == right.mapId &&
        left.instanceId == right.instanceId && left.relation == right.relation &&
        left.actorFaction == right.actorFaction && left.targetFaction == right.targetFaction &&
        left.factVersion == right.factVersion;
}

bool ValidReference(HostilePlayerReference const& reference) noexcept
{
    return reference.valid && reference.isPlayer && reference.targetGuid != 0 &&
        reference.relation == TeamRelation::Hostile && reference.actorFaction != 0 &&
        reference.targetFaction != 0 && reference.actorFaction != reference.targetFaction &&
        reference.factVersion != 0;
}

bool SameRequestDecision(PvpCombatRequest const& request, Decision const& decision) noexcept
{
    return decision.valid && decision.decisionId == request.decisionId &&
        decision.intentId == request.intentId && decision.domain == request.domain &&
        decision.intent == request.intent && decision.resource == request.resource &&
        AutoWowOracle::SameScope(decision.scope, request.scope) &&
        decision.actorGuid == request.actorGuid && decision.targetGuid == request.target.targetGuid &&
        decision.executorAvailable == request.executorAvailable &&
        decision.epoch == request.epoch && decision.issuedTick == request.issuedTick &&
        decision.expiresTick == request.expiresTick && decision.ttlTicks == request.ttlTicks &&
        decision.operation == request.operation &&
        decision.preconditions.frameVersion == request.frameVersion &&
        decision.preconditions.epoch == request.epoch &&
        decision.preconditions.botGuid == request.actorGuid &&
        decision.preconditions.mapId == request.target.mapId &&
        decision.preconditions.instanceId == request.target.instanceId;
}

ValidationResult ValidateRequestShape(PvpCombatRequest const& request,
                                      IntentLease const& lease,
                                      HostilePlayerReference const& exactTarget) noexcept
{
    if (!request.valid || request.requestId == 0 || request.decisionId == 0 ||
        request.intentId == 0 || request.actorGuid == 0 || request.scope.botGuid == 0 ||
        request.actorGuid != request.scope.botGuid || request.ttlTicks == 0 ||
        request.expiresTick <= request.issuedTick || !lease.valid ||
        !SameRequestDecision(request, lease.decision))
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::InvalidRequest,
                      ExecutorReason::InvalidRequest);

    if (request.operation != OperationCode::CombatAction)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::WrongOperation,
                      ExecutorReason::OperationBlocked);
    if (request.domain != Domain::Combat)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::WrongDomain,
                      ExecutorReason::OperationBlocked);
    if (request.intent != IntentCode::CombatAction)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::WrongIntent,
                      ExecutorReason::OperationBlocked);
    if (request.resource != LeaseResource::CombatPositioning)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::WrongResource,
                      ExecutorReason::OperationBlocked);
    if (!request.executorAvailable)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::ExecutorUnavailable,
                      ExecutorReason::OperationBlocked);
    if (!ValidReference(request.target) || !ValidReference(exactTarget) ||
        request.target.targetGuid == request.actorGuid)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::InvalidTargetReference,
                      ExecutorReason::InvalidOperationPayload);
    if (!SameReference(request.target, exactTarget))
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetReferenceMismatch,
                      ExecutorReason::ProofMismatch);
    return Accept();
}

ValidationResult ValidateWorldAndOwnership(PvpCombatRequest const& request,
                                           HostilePlayerObservation const& observation,
                                           bool allowNewerFrame) noexcept
{
    CombatWorldSnapshot const& world = observation.world;
    if (!world.onWorldThread)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::NotWorldThread,
                      ExecutorReason::InvalidRequest);
    if (world.tick >= request.expiresTick)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::LeaseExpired,
                      ExecutorReason::LeaseExpired);
    if (world.frameVersion < request.frameVersion ||
        (!allowNewerFrame && world.frameVersion != request.frameVersion))
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::StaleFrame,
                      ExecutorReason::StaleFrame);
    if (world.epoch != request.epoch)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::StaleEpoch,
                      ExecutorReason::StaleEpoch);
    if (world.actorGuid != request.actorGuid || world.mapId != request.target.mapId ||
        world.instanceId != request.target.instanceId)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::WrongBot,
                      ExecutorReason::LeaseMismatch);
    if (!world.actorAlive)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::ActorDead,
                      ExecutorReason::InvalidOperationPayload);

    if (!world.ownershipProof.active || !request.ownershipProof.active)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::MissingOwnershipProof,
                      ExecutorReason::MissingOwnershipProof);
    if (world.ownershipProof.botGuid != request.actorGuid ||
        !AutoWowOracle::SameScope(world.ownershipProof.scope, request.scope))
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    if (world.ownershipProof.decisionId != request.decisionId ||
        world.ownershipProof.intentId != request.intentId)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::LeasePreempted,
                      ExecutorReason::LeasePreempted);
    if (world.ownershipProof.epoch != request.epoch)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::StaleEpoch,
                      ExecutorReason::StaleEpoch);
    if (world.ownershipProof.expiresTick != request.expiresTick ||
        world.ownershipProof.operation != request.operation)
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    if (!SameOwnership(request.ownershipProof, world.ownershipProof))
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    return Accept();
}

ValidationResult ValidateTarget(HostilePlayerReference const& exactTarget,
                                HostilePlayerState const& target,
                                bool allowNewerFactVersion,
                                bool allowTerminalTarget) noexcept
{
    if (target.targetGuid != exactTarget.targetGuid)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetGuidMismatch,
                      ExecutorReason::ProofMismatch);
    if (target.mapId != exactTarget.mapId)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetMapMismatch,
                      ExecutorReason::ProofMismatch);
    if (target.instanceId != exactTarget.instanceId)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetInstanceMismatch,
                      ExecutorReason::ProofMismatch);
    if (!target.isPlayer)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetNotPlayer,
                      ExecutorReason::InvalidOperationPayload);
    if (target.relation != exactTarget.relation || target.relation != TeamRelation::Hostile)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TeamRelationMismatch,
                      ExecutorReason::InvalidOperationPayload);
    if (target.actorFaction != exactTarget.actorFaction ||
        target.targetFaction != exactTarget.targetFaction ||
        target.actorFaction == target.targetFaction)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::FactionMismatch,
                      ExecutorReason::InvalidOperationPayload);
    if (target.factVersion < exactTarget.factVersion ||
        (!allowNewerFactVersion && target.factVersion != exactTarget.factVersion))
        return Reject(ExecutorStatus::Rejected, PvpCombatReason::FactVersionStale,
                      ExecutorReason::StaleFrame);
    if (!target.inWorld)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetNotInWorld,
                      ExecutorReason::InvalidOperationPayload);
    if (!target.pvpPermitted)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::PvpProhibited,
                      ExecutorReason::InvalidOperationPayload);
    if (!allowTerminalTarget && !target.alive)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetDead,
                      ExecutorReason::InvalidOperationPayload);
    if (!allowTerminalTarget && !target.attackable)
        return Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetNotAttackable,
                      ExecutorReason::InvalidOperationPayload);
    return Accept();
}

void SetValidation(DispatchResult& result, ValidationResult const& validation) noexcept
{
    result.status = validation.status;
    result.reason = validation.reason;
    result.sharedReason = validation.sharedReason;
}
}

bool ExactTargetMatches(HostilePlayerReference const& expected,
                        HostilePlayerReference const& observed) noexcept
{
    return ValidReference(expected) && ValidReference(observed) && SameReference(expected, observed);
}

bool ExactTargetMatches(HostilePlayerReference const& expected,
                        HostilePlayerState const& observed,
                        bool allowNewerFactVersion) noexcept
{
    return ValidReference(expected) && observed.isPlayer &&
        observed.targetGuid == expected.targetGuid && observed.mapId == expected.mapId &&
        observed.instanceId == expected.instanceId && observed.relation == expected.relation &&
        observed.actorFaction == expected.actorFaction &&
        observed.targetFaction == expected.targetFaction &&
        observed.factVersion >= expected.factVersion &&
        (allowNewerFactVersion || observed.factVersion == expected.factVersion);
}

bool TargetFactsSafe(HostilePlayerState const& state) noexcept
{
    return state.isPlayer && state.inWorld && state.alive && state.attackable &&
        state.pvpPermitted && state.relation == TeamRelation::Hostile &&
        state.actorFaction != 0 && state.targetFaction != 0 &&
        state.actorFaction != state.targetFaction && state.factVersion != 0;
}

ValidationResult OraclePvpCombatExecutor::Validate(
    PvpCombatRequest const& request, IntentLease const& lease,
    HostilePlayerReference const& exactTarget, HostilePlayerObservation const& observation,
    bool allowNewerFrame) noexcept
{
    ValidationResult result = ValidateRequestShape(request, lease, exactTarget);
    if (!result.valid)
        return result;

    result = ValidateWorldAndOwnership(request, observation, allowNewerFrame);
    if (!result.valid)
        return result;

    return ValidateTarget(exactTarget, observation.target, allowNewerFrame, false);
}

DispatchResult OraclePvpCombatExecutor::Dispatch(
    PvpCombatRequest const& request, IntentLease const& lease,
    HostilePlayerReference const& exactTarget, HostilePlayerObservation const& before,
    NativeStepFunction nativeStep, void* context) noexcept
{
    DispatchResult result;
    result.valid = true;
    result.request = request;
    result.before = before;

    ValidationResult const beforeValidation = Validate(request, lease, exactTarget, before, false);
    if (!beforeValidation.valid)
    {
        SetValidation(result, beforeValidation);
        return result;
    }
    result.beforeValidated = true;

    if (!nativeStep)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, PvpCombatReason::NativeUnavailable,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    NativeStepRequest const nativeRequest{request, lease, exactTarget, 1, true};
    NativeStepObservation const native = nativeStep(context, nativeRequest);
    result.nativeCalled = true;
    result.stepsInvoked = native.stepsInvoked;
    result.selectedTargetGuid = native.selectedTargetGuid;
    result.after = native.after;

    // A post-step snapshot is allowed to advance, and a dead target is a valid terminal result,
    // but all identity/faction/PvP facts and fresh ownership must still be revalidated.
    ValidationResult const afterValidation = ValidateTarget(exactTarget, native.after.target,
                                                            true, true);
    if (!afterValidation.valid)
    {
        SetValidation(result, afterValidation);
        return result;
    }
    ValidationResult const afterWorld = ValidateWorldAndOwnership(request, native.after, true);
    if (!afterWorld.valid)
    {
        SetValidation(result, afterWorld);
        return result;
    }
    result.afterValidated = true;

    if (native.selectedTargetGuid != exactTarget.targetGuid)
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked, PvpCombatReason::TargetSubstitution,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (native.after.world.frameVersion <= before.world.frameVersion ||
        native.after.target.factVersion <= before.target.factVersion)
    {
        SetValidation(result, Reject(ExecutorStatus::Rejected,
                                     PvpCombatReason::PostconditionMismatch,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (native.stepsInvoked != 1)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed,
                                     PvpCombatReason::StepBudgetExceeded,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }
    if (!native.dispatchAccepted)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, PvpCombatReason::NativeRejected,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    result.accepted = true;
    result.status = native.after.target.alive ? ExecutorStatus::Progressing :
                                                  ExecutorStatus::Completed;
    result.reason = PvpCombatReason::None;
    result.sharedReason = ExecutorReason::None;
    return result;
}

} // namespace AutoWowOraclePvpCombatExecutor
