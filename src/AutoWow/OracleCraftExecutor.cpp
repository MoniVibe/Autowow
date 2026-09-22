/*
 * Pure exact one-craft executor.
 */
#include "OracleCraftExecutor.h"

#include <limits>

namespace AutoWowOracleCraftExecutor
{
namespace
{
ValidationResult Reject(ExecutorStatus status, CraftReason reason,
                        ExecutorReason sharedReason) noexcept
{
    return {false, status, reason, sharedReason};
}

ValidationResult Accept() noexcept
{
    return {true, ExecutorStatus::Accepted, CraftReason::None, ExecutorReason::None};
}

bool SameEvidence(AutoWowOracle::EvidenceCounters const& left,
                  AutoWowOracle::EvidenceCounters const& right) noexcept
{
    return left.objective == right.objective && left.progress == right.progress &&
        left.resource == right.resource && left.combat == right.combat &&
        left.healing == right.healing && left.deaths == right.deaths &&
        left.pvp == right.pvp && left.failures == right.failures;
}

bool SameQuest(AutoWowOracle::QuestReference const& left,
               AutoWowOracle::QuestReference const& right) noexcept
{
    return left.valid == right.valid && left.questId == right.questId &&
        left.objectiveFamily == right.objectiveFamily && left.objectiveSlot == right.objectiveSlot &&
        left.requiredEntry == right.requiredEntry && left.requiredItemId == right.requiredItemId;
}

bool SameGather(AutoWowOracle::GatherSourceReference const& left,
                AutoWowOracle::GatherSourceReference const& right) noexcept
{
    return left.valid == right.valid && left.spawnId == right.spawnId && left.entry == right.entry &&
        left.mapId == right.mapId && left.materialItemId == right.materialItemId &&
        left.instanceId == right.instanceId && left.goal == right.goal;
}

bool EmptyQuest(AutoWowOracle::QuestReference const& reference) noexcept
{
    return !reference.valid && reference.questId == 0 &&
        reference.objectiveFamily == AutoWowOracle::QuestObjectiveFamily::None &&
        reference.objectiveSlot == 0 && reference.requiredEntry == 0 &&
        reference.requiredItemId == 0;
}

bool SameTransition(AutoWowOracle::TransitionPrecondition const& left,
                    AutoWowOracle::TransitionPrecondition const& right) noexcept
{
    return left.required == right.required && left.stableId == right.stableId &&
        left.sourceMapId == right.sourceMapId && left.sourceInstanceId == right.sourceInstanceId &&
        left.destinationMapId == right.destinationMapId &&
        left.destinationInstanceId == right.destinationInstanceId && left.validated == right.validated &&
        left.directedProof == right.directedProof && left.cheatBacked == right.cheatBacked &&
        left.grantBacked == right.grantBacked;
}

bool SameRoute(AutoWowOracle::RouteProofPrecondition const& left,
               AutoWowOracle::RouteProofPrecondition const& right) noexcept
{
    return left.required == right.required && left.stableId == right.stableId &&
        left.mapId == right.mapId && left.pathCalculated == right.pathCalculated &&
        left.pathComplete == right.pathComplete &&
        left.usesOnlyLegalMovement == right.usesOnlyLegalMovement &&
        left.endpointsLegal == right.endpointsLegal;
}

bool SamePreconditions(AutoWowOracle::Preconditions const& left,
                       AutoWowOracle::Preconditions const& right) noexcept
{
    return left.frameVersion == right.frameVersion && left.epoch == right.epoch &&
        left.botGuid == right.botGuid && left.squadId == right.squadId &&
        left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.requireBotAlive == right.requireBotAlive && left.requireSameMap == right.requireSameMap &&
        SameTransition(left.transition, right.transition) && SameRoute(left.route, right.route);
}

bool SameOwnership(ActiveLeaseSnapshot const& left,
                   ActiveLeaseSnapshot const& right) noexcept
{
    return left.active == right.active && left.botGuid == right.botGuid &&
        left.decisionId == right.decisionId && left.intentId == right.intentId &&
        left.epoch == right.epoch && left.expiresTick == right.expiresTick &&
        AutoWowOracle::SameScope(left.scope, right.scope) && left.operation == right.operation;
}

bool SameRequestDecision(ExecutorRequest const& request, Decision const& decision) noexcept
{
    return decision.valid && request.valid && decision.decisionId == request.decisionId &&
        decision.intentId == request.intentId && decision.domain == request.domain &&
        decision.intent == request.intent && decision.resource == request.resource &&
        AutoWowOracle::SameScope(decision.scope, request.scope) &&
        decision.actorGuid == request.actorGuid && decision.targetGuid == request.targetGuid &&
        decision.itemId == request.itemId && decision.objectiveId == request.objectiveId &&
        decision.rallyPointId == request.rallyPointId && decision.role == request.role &&
        decision.executorAvailable == request.executorAvailable &&
        decision.epoch == request.epoch && decision.issuedTick == request.issuedTick &&
        decision.expiresTick == request.expiresTick && decision.ttlTicks == request.ttlTicks &&
        SamePreconditions(decision.preconditions,
                          AutoWowOracle::Preconditions{request.frameVersion, request.epoch,
                              request.scope.botGuid, request.scope.squadId,
                              decision.preconditions.mapId, decision.preconditions.instanceId,
                              decision.preconditions.requireBotAlive,
                              decision.preconditions.requireSameMap,
                              decision.preconditions.transition, decision.preconditions.route}) &&
        SameEvidence(decision.evidence, request.proofBaseline) &&
        decision.operation == request.operation && SameQuest(decision.quest, request.quest) &&
        SameGather(decision.gather, request.gather);
}

ValidationResult ValidateRequestShape(CraftRequest const& request) noexcept
{
    ExecutorRequest const& base = request.request;
    if (!base.valid || base.requestId == 0 || base.decisionId == 0 || base.intentId == 0 ||
        base.actorGuid == 0 || base.scope.botGuid == 0 || base.actorGuid != base.scope.botGuid ||
        base.ttlTicks == 0 || base.expiresTick <= base.issuedTick)
        return Reject(ExecutorStatus::Invalid, CraftReason::InvalidRequest,
                      ExecutorReason::InvalidRequest);

    if (!ValidCraftReference(request.craft))
        return Reject(ExecutorStatus::Blocked, CraftReason::InvalidCraftReference,
                      ExecutorReason::InvalidOperationPayload);
    if (base.operation != OperationCode::Craft)
        return Reject(ExecutorStatus::Blocked, CraftReason::WrongOperation,
                      ExecutorReason::OperationBlocked);
    if (base.domain != AutoWowOracle::Domain::CraftingEconomyItems)
        return Reject(ExecutorStatus::Blocked, CraftReason::WrongDomain,
                      ExecutorReason::OperationBlocked);
    if (base.intent != AutoWowOracle::IntentCode::CraftEconomyItems)
        return Reject(ExecutorStatus::Blocked, CraftReason::WrongIntent,
                      ExecutorReason::OperationBlocked);
    // The existing planner maps craft work to QuestGather until the contract grows a dedicated
    // crafting resource. Keeping this exact tuple prevents a text label from authorizing craft.
    if (base.resource != AutoWowOracle::LeaseResource::QuestGather)
        return Reject(ExecutorStatus::Blocked, CraftReason::WrongResource,
                      ExecutorReason::OperationBlocked);
    if (!base.executorAvailable)
        return Reject(ExecutorStatus::Blocked, CraftReason::WrongOperation,
                      ExecutorReason::OperationBlocked);
    if (base.targetGuid != 0 || base.objectiveId != 0 || base.rallyPointId != 0 ||
        base.itemId != request.craft.outputItemId || !EmptyQuest(base.quest) ||
        !AutoWowOracle::IsEmptyGatherReference(base.gather))
        return Reject(ExecutorStatus::Blocked, CraftReason::InvalidCraftReference,
                      ExecutorReason::InvalidOperationPayload);
    return Accept();
}

ValidationResult ValidateLease(CraftRequest const& request, IntentLease const& lease,
                               CraftWorldSnapshot const& world) noexcept
{
    if (!lease.valid)
        return Reject(ExecutorStatus::Rejected, CraftReason::MissingLease,
                      ExecutorReason::LeaseMissing);

    if (!request.request.ownershipProof.active || !world.ownershipProof.active)
        return Reject(ExecutorStatus::Rejected, CraftReason::MissingOwnershipProof,
                      ExecutorReason::MissingOwnershipProof);

    if (!SameRequestDecision(request.request, lease.decision))
        return Reject(ExecutorStatus::Rejected, CraftReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);

    if (world.ownershipProof.decisionId != request.request.decisionId ||
        world.ownershipProof.intentId != request.request.intentId)
        return Reject(ExecutorStatus::Rejected, CraftReason::LeasePreempted,
                      ExecutorReason::LeasePreempted);
    if (world.ownershipProof.botGuid != request.request.actorGuid ||
        !AutoWowOracle::SameScope(world.ownershipProof.scope, request.request.scope))
        return Reject(ExecutorStatus::Rejected, CraftReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    if (world.ownershipProof.epoch != request.request.epoch ||
        world.ownershipProof.expiresTick != request.request.expiresTick ||
        world.ownershipProof.operation != request.request.operation ||
        !SameOwnership(request.request.ownershipProof, world.ownershipProof))
        return Reject(ExecutorStatus::Rejected, CraftReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    return Accept();
}

ValidationResult ValidateWorld(CraftRequest const& request, IntentLease const& lease,
                               CraftObservation const& observation,
                               bool allowNewerFrame) noexcept
{
    CraftWorldSnapshot const& world = observation.world;
    CraftReference const& reference = request.craft;

    if (!world.onWorldThread)
        return Reject(ExecutorStatus::Rejected, CraftReason::NotWorldThread,
                      ExecutorReason::InvalidRequest);
    if (world.tick >= request.request.expiresTick)
        return Reject(ExecutorStatus::Rejected, CraftReason::LeaseExpired,
                      ExecutorReason::LeaseExpired);
    if (world.frameVersion < request.request.frameVersion ||
        (!allowNewerFrame && world.frameVersion != request.request.frameVersion))
        return Reject(ExecutorStatus::Rejected, CraftReason::StaleFrame,
                      ExecutorReason::StaleFrame);
    if (world.epoch != request.request.epoch)
        return Reject(ExecutorStatus::Rejected, CraftReason::StaleEpoch,
                      ExecutorReason::StaleEpoch);
    if (world.botGuid != request.request.actorGuid)
        return Reject(ExecutorStatus::Rejected, CraftReason::WrongBot,
                      ExecutorReason::LeaseMismatch);
    if (lease.decision.preconditions.requireSameMap &&
        (world.mapId != lease.decision.preconditions.mapId ||
         world.instanceId != lease.decision.preconditions.instanceId))
        return Reject(ExecutorStatus::Rejected, CraftReason::WrongBot,
                      ExecutorReason::InvalidOperationPayload);
    if (!world.botAlive)
        return Reject(ExecutorStatus::Blocked, CraftReason::Dead,
                      ExecutorReason::InvalidOperationPayload);
    if (world.inCombat)
        return Reject(ExecutorStatus::Blocked, CraftReason::InCombat,
                      ExecutorReason::InvalidOperationPayload);
    if (world.cheatPathActive)
        return Reject(ExecutorStatus::Blocked, CraftReason::CheatPath,
                      ExecutorReason::InvalidOperationPayload);
    if (!world.knownRecipe || !reference.knownRecipe)
        return Reject(ExecutorStatus::Blocked, CraftReason::KnownRecipeMissing,
                      ExecutorReason::InvalidOperationPayload);
    if (!world.professionKnown || !reference.professionKnown)
        return Reject(ExecutorStatus::Blocked, CraftReason::ProfessionMissing,
                      ExecutorReason::InvalidOperationPayload);
    if (world.currentSkill < reference.requiredSkill ||
        world.currentSkill < reference.skillAtDecision ||
        world.maximumSkill < world.currentSkill ||
        world.maximumSkill < reference.requiredSkill)
        return Reject(ExecutorStatus::Blocked, CraftReason::SkillInsufficient,
                      ExecutorReason::InvalidOperationPayload);

    return Accept();
}

ValidationResult ValidateCommon(CraftRequest const& request, IntentLease const& lease,
                                CraftCandidate const& exactCandidate,
                                CraftObservation const& observation,
                                bool allowNewerFrame) noexcept
{
    ValidationResult result = ValidateRequestShape(request);
    if (!result.valid)
        return result;
    // Validate lease identity before consulting lease-owned map preconditions. An absent lease must
    // be reported as absent, never converted into a misleading location mismatch from its zeroed
    // decision.
    result = ValidateLease(request, lease, observation.world);
    if (!result.valid)
        return result;
    if (!ExactCandidateMatches(request, exactCandidate) ||
        !ExactCandidateMatches(exactCandidate, observation.candidate))
        return Reject(ExecutorStatus::Blocked, CraftReason::CandidateMismatch,
                      ExecutorReason::ProofMismatch);
    result = ValidateWorld(request, lease, observation, allowNewerFrame);
    if (!result.valid)
        return result;
    return Accept();
}

ValidationResult ValidateInputs(CraftRequest const& request,
                                CraftObservation const& observation) noexcept
{
    CraftReference const& reference = request.craft;
    if (observation.inventory.freeOutputCapacity < reference.outputCount)
        return Reject(ExecutorStatus::Blocked, CraftReason::InventoryCapacity,
                      ExecutorReason::InvalidOperationPayload);
    for (std::size_t index = 0; index < reference.reagentCount; ++index)
        if (observation.inventory.reagentCounts[index] < reference.reagents[index].quantity)
            return Reject(ExecutorStatus::Blocked, CraftReason::ReagentShortage,
                          ExecutorReason::InvalidOperationPayload);
    return Accept();
}

bool ExactOutputDelta(CraftReference const& reference,
                      CraftInventorySnapshot const& before,
                      CraftInventorySnapshot const& after) noexcept
{
    std::uint64_t const expected = static_cast<std::uint64_t>(before.outputCount) +
        reference.outputCount;
    return expected <= std::numeric_limits<std::uint32_t>::max() &&
        after.outputCount == static_cast<std::uint32_t>(expected);
}

bool ExactReagentDelta(CraftReference const& reference,
                       CraftInventorySnapshot const& before,
                       CraftInventorySnapshot const& after) noexcept
{
    for (std::size_t index = 0; index < reference.reagentCount; ++index)
    {
        std::uint32_t const required = reference.reagents[index].quantity;
        if (before.reagentCounts[index] < required ||
            after.reagentCounts[index] != before.reagentCounts[index] - required)
            return false;
    }
    return true;
}

void SetValidation(DispatchResult& result, ValidationResult const& validation) noexcept
{
    result.status = validation.status;
    result.reason = validation.reason;
    result.sharedReason = validation.sharedReason;
}
}

bool ValidCraftReference(CraftReference const& reference) noexcept
{
    if (!reference.valid || reference.recipeSpellId == 0 || reference.outputItemId == 0 ||
        reference.outputCount == 0 || reference.reagentCount > kMaxCraftReagents ||
        reference.profession == Profession::None || !reference.professionKnown ||
        !reference.knownRecipe || reference.skillAtDecision < reference.requiredSkill ||
        reference.maximumSkillAtDecision < reference.skillAtDecision ||
        reference.maximumSkillAtDecision < reference.requiredSkill)
        return false;

    for (std::size_t index = 0; index < reference.reagentCount; ++index)
    {
        CraftReagent const& reagent = reference.reagents[index];
        if (reagent.itemId == 0 || reagent.quantity == 0)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (reference.reagents[previous].itemId == reagent.itemId)
                return false;
    }
    for (std::size_t index = reference.reagentCount; index < kMaxCraftReagents; ++index)
        if (reference.reagents[index].itemId != 0 || reference.reagents[index].quantity != 0)
            return false;
    return true;
}

bool ExactReferenceMatches(CraftReference const& expected,
                           CraftReference const& observed) noexcept
{
    if (!ValidCraftReference(expected) || !ValidCraftReference(observed))
        return false;
    if (expected.valid != observed.valid || expected.recipeSpellId != observed.recipeSpellId ||
        expected.outputItemId != observed.outputItemId ||
        expected.outputCount != observed.outputCount ||
        expected.reagentCount != observed.reagentCount || expected.profession != observed.profession ||
        expected.requiredSkill != observed.requiredSkill ||
        expected.skillAtDecision != observed.skillAtDecision ||
        expected.maximumSkillAtDecision != observed.maximumSkillAtDecision ||
        expected.professionKnown != observed.professionKnown ||
        expected.knownRecipe != observed.knownRecipe)
        return false;
    for (std::size_t index = 0; index < expected.reagentCount; ++index)
        if (expected.reagents[index].itemId != observed.reagents[index].itemId ||
            expected.reagents[index].quantity != observed.reagents[index].quantity)
            return false;
    return true;
}

bool ExactCandidateMatches(CraftCandidate const& expected,
                           CraftCandidate const& observed) noexcept
{
    return ExactReferenceMatches(expected.reference, observed.reference);
}

bool ExactCandidateMatches(CraftRequest const& request,
                           CraftCandidate const& candidate) noexcept
{
    ExecutorRequest const& base = request.request;
    return base.valid && base.operation == OperationCode::Craft && base.targetGuid == 0 &&
        base.objectiveId == 0 && base.rallyPointId == 0 &&
        base.itemId == request.craft.outputItemId && EmptyQuest(base.quest) &&
        AutoWowOracle::IsEmptyGatherReference(base.gather) &&
        ExactReferenceMatches(request.craft, candidate.reference);
}

ValidationResult OracleCraftExecutor::Validate(CraftRequest const& request,
                                                IntentLease const& lease,
                                                CraftCandidate const& exactCandidate,
                                                CraftObservation const& observation) noexcept
{
    ValidationResult result = ValidateCommon(request, lease, exactCandidate, observation, false);
    if (!result.valid)
        return result;
    return ValidateInputs(request, observation);
}

DispatchResult OracleCraftExecutor::Dispatch(CraftRequest const& request,
                                              IntentLease const& lease,
                                              CraftCandidate const& exactCandidate,
                                              CraftObservation const& before,
                                              NativeStepFunction nativeStep,
                                              void* context) noexcept
{
    DispatchResult result;
    result.valid = true;
    result.request = request;
    result.candidate = exactCandidate;
    result.before = before;

    ValidationResult const beforeValidation = Validate(request, lease, exactCandidate, before);
    if (!beforeValidation.valid)
    {
        SetValidation(result, beforeValidation);
        return result;
    }
    result.beforeValidated = true;
    result.phase = CraftStepPhase::Cast;

    if (!nativeStep)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, CraftReason::NativeUnavailable,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    NativeStepRequest const nativeRequest{request, lease, exactCandidate, CraftStepPhase::Cast, 1,
                                          true};
    NativeStepObservation const native = nativeStep(context, nativeRequest);
    result.nativeCalled = true;
    result.stepsInvoked = native.stepsInvoked;
    result.after = native.after;

    if (native.stepsInvoked != 1)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, CraftReason::StepBudgetExceeded,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }
    if (native.selectedAnotherRecipe)
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked,
                                     CraftReason::SelectedAnotherRecipe,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (!native.dispatchAccepted)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, CraftReason::NativeRejected,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    ValidationResult const afterValidation =
        ValidateCommon(request, lease, exactCandidate, native.after, true);
    if (!afterValidation.valid)
    {
        SetValidation(result, afterValidation);
        return result;
    }
    result.afterValidated = true;

    if (!ExactOutputDelta(request.craft, before.inventory, native.after.inventory))
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked, CraftReason::OutputDeltaMismatch,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (!ExactReagentDelta(request.craft, before.inventory, native.after.inventory))
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked, CraftReason::ReagentDeltaMismatch,
                                     ExecutorReason::ProofMismatch));
        return result;
    }

    result.accepted = true;
    result.status = ExecutorStatus::Completed;
    result.reason = CraftReason::None;
    result.sharedReason = ExecutorReason::None;
    return result;
}

} // namespace AutoWowOracleCraftExecutor
