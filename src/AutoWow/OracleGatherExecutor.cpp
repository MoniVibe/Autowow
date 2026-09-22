/*
 * Pure fixed-target GatherSource executor.
 */
#include "OracleGatherExecutor.h"

namespace AutoWowOracleGatherExecutor
{
namespace
{
using AutoWowOracle::Decision;

ValidationResult Reject(ExecutorStatus status, FixedTargetReason reason,
                        ExecutorReason sharedReason) noexcept
{
    return {false, status, reason, sharedReason};
}

ValidationResult Accept() noexcept
{
    return {true, ExecutorStatus::Accepted, FixedTargetReason::None, ExecutorReason::None};
}

bool SameGatherReference(GatherSourceReference const& left,
                         GatherSourceReference const& right) noexcept
{
    return left.valid == right.valid && left.spawnId == right.spawnId && left.entry == right.entry &&
        left.mapId == right.mapId && left.materialItemId == right.materialItemId &&
        left.instanceId == right.instanceId && left.goal == right.goal;
}

bool EmptyQuestReference(AutoWowOracle::QuestReference const& quest) noexcept
{
    return !quest.valid && quest.questId == 0 &&
        quest.objectiveFamily == AutoWowOracle::QuestObjectiveFamily::None &&
        quest.objectiveSlot == 0 && quest.requiredEntry == 0 && quest.requiredItemId == 0;
}

bool ValidGatherRequestReference(ExecutorRequest const& request) noexcept
{
    return AutoWowOracle::ValidGatherSourceReference(request.gather) &&
        request.gather.materialItemId != 0 && request.targetGuid == request.gather.spawnId &&
        request.itemId == request.gather.materialItemId && request.objectiveId == 0 &&
        request.gatherGoal == request.gather.goal &&
        AutoWowOracle::ValidGatherGoal(request.gatherGoal) && EmptyQuestReference(request.quest);
}

bool SameCandidateFacts(CandidateFacts const& left, CandidateFacts const& right) noexcept
{
    return left.alive == right.alive && left.available == right.available &&
        left.reachable == right.reachable && left.profession == right.profession &&
        left.toolRequired == right.toolRequired && left.toolAvailable == right.toolAvailable &&
        left.sourceYieldsRequestedMaterial == right.sourceYieldsRequestedMaterial;
}

bool CandidateFactsSafe(CandidateFacts const& facts) noexcept
{
    return facts.alive && facts.available && facts.reachable &&
        facts.profession != AutoWowGather::Profession::None &&
        (!facts.toolRequired || facts.toolAvailable) && facts.sourceYieldsRequestedMaterial;
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
    return decision.valid && decision.decisionId == request.decisionId &&
        decision.intentId == request.intentId && decision.domain == request.domain &&
        decision.intent == request.intent && decision.resource == request.resource &&
        AutoWowOracle::SameScope(decision.scope, request.scope) &&
        decision.actorGuid == request.actorGuid && decision.targetGuid == request.targetGuid &&
        decision.itemId == request.itemId && decision.objectiveId == request.objectiveId &&
        decision.rallyPointId == request.rallyPointId && decision.operation == request.operation &&
        decision.executorAvailable == request.executorAvailable &&
        decision.epoch == request.epoch && decision.issuedTick == request.issuedTick &&
        decision.expiresTick == request.expiresTick && decision.ttlTicks == request.ttlTicks &&
        decision.preconditions.frameVersion == request.frameVersion &&
        decision.preconditions.epoch == request.epoch &&
        decision.preconditions.botGuid == request.actorGuid &&
        SameGatherReference(decision.gather, request.gather) &&
        request.gatherGoal == decision.gather.goal && EmptyQuestReference(decision.quest) &&
        decision.objectiveId == 0;
}

bool IsAcceptedNodeReservation(NodeReservationProof const& proof) noexcept
{
    return proof.arbiterReceipt.status == ReceiptStatus::Accepted &&
        (proof.arbiterReceipt.reason == ReceiptReason::LeaseAcquired ||
         proof.arbiterReceipt.reason == ReceiptReason::LeaseAlreadyOwned);
}

ValidationResult ValidateRequestShape(ExecutorRequest const& request,
                                      IntentLease const& lease) noexcept
{
    if (!request.valid || request.requestId == 0 || request.decisionId == 0 || request.intentId == 0 ||
        request.actorGuid == 0 || request.scope.botGuid == 0 ||
        request.actorGuid != request.scope.botGuid || request.ttlTicks == 0 ||
        request.expiresTick <= request.issuedTick)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::InvalidRequest,
                      ExecutorReason::InvalidRequest);

    // Classify a malformed exact gather tuple before comparing it with the lease decision. A
    // request whose goal/reference disagree is an invalid source reference, not an unrelated
    // lease/request mismatch.
    if (request.operation == OperationCode::GatherSource && !ValidGatherRequestReference(request))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::InvalidGatherSourceReference,
                      ExecutorReason::InvalidGatherSourceReference);

    if (!lease.valid || !SameRequestDecision(request, lease.decision))
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::InvalidRequest,
                      ExecutorReason::InvalidRequest);

    if (request.operation != OperationCode::GatherSource)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::WrongOperation,
                      ExecutorReason::OperationBlocked);
    if (request.domain != AutoWowOracle::Domain::Gathering)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::WrongDomain,
                      ExecutorReason::OperationBlocked);
    if (request.intent != AutoWowOracle::IntentCode::GatherSource)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::WrongIntent,
                      ExecutorReason::OperationBlocked);
    if (request.resource != AutoWowOracle::LeaseResource::QuestGather)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::WrongResource,
                      ExecutorReason::OperationBlocked);
    if (!request.executorAvailable)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::WrongOperation,
                      ExecutorReason::OperationBlocked);
    if (!ValidGatherRequestReference(request))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::InvalidGatherSourceReference,
                      ExecutorReason::InvalidGatherSourceReference);
    return Accept();
}

ValidationResult ValidateWorldAndOwnership(ExecutorRequest const& request,
                                           FixedTargetObservation const& observation,
                                           bool allowNewerFrame) noexcept
{
    FixedTargetWorldSnapshot const& world = observation.world;
    if (!world.onWorldThread)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::NotWorldThread,
                      ExecutorReason::InvalidRequest);
    if (world.tick >= request.expiresTick)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::LeaseExpired,
                      ExecutorReason::LeaseExpired);
    if (world.frameVersion < request.frameVersion ||
        (!allowNewerFrame && world.frameVersion != request.frameVersion))
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::StaleFrame,
                      ExecutorReason::StaleFrame);
    if (world.epoch != request.epoch)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::StaleEpoch,
                      ExecutorReason::StaleEpoch);
    if (world.botGuid != request.actorGuid || world.mapId != request.gather.mapId ||
        world.instanceId != request.gather.instanceId)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::WrongBot,
                      ExecutorReason::LeaseMismatch);
    if (!world.botAlive)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::CandidateFactsMismatch,
                      ExecutorReason::InvalidOperationPayload);

    if (!world.ownershipProof.active || !request.ownershipProof.active)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::MissingOwnershipProof,
                      ExecutorReason::MissingOwnershipProof);
    if (world.ownershipProof.botGuid != request.actorGuid ||
        !AutoWowOracle::SameScope(world.ownershipProof.scope, request.scope))
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    if (world.ownershipProof.decisionId != request.decisionId ||
        world.ownershipProof.intentId != request.intentId)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::LeasePreempted,
                      ExecutorReason::LeasePreempted);
    if (world.ownershipProof.epoch != request.epoch)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::StaleEpoch,
                      ExecutorReason::StaleEpoch);
    if (world.ownershipProof.expiresTick != request.expiresTick ||
        world.ownershipProof.operation != request.operation)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    if (!SameOwnership(request.ownershipProof, world.ownershipProof))
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::LeaseMismatch,
                      ExecutorReason::LeaseMismatch);
    return Accept();
}

ValidationResult ValidateReservation(ExecutorRequest const& request,
                                     FixedTargetObservation const& observation) noexcept
{
    NodeReservationProof const& proof = observation.nodeReservation;
    if (!IsAcceptedNodeReservation(proof))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::ReservationMissing,
                      ExecutorReason::OperationBlocked);
    if (!proof.expiresTick || observation.world.tick >= proof.expiresTick)
        return Reject(ExecutorStatus::Rejected, FixedTargetReason::ReservationExpired,
                      ExecutorReason::LeaseExpired);
    if (proof.expiresTick != request.expiresTick)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::ReservationMismatch,
                      ExecutorReason::ProofMismatch);

    AutoWowOracle::Receipt const& receipt = proof.arbiterReceipt;
    if (receipt.operation != request.operation || receipt.decisionId != request.decisionId ||
        receipt.intentId != request.intentId || receipt.actorGuid != request.actorGuid ||
        receipt.epoch != request.epoch ||
        receipt.gather.materialItemId != request.itemId ||
        !AutoWowOracle::SameScope(receipt.scope, request.scope) ||
        !SameGatherReference(receipt.gather, request.gather))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::ReservationMismatch,
                      ExecutorReason::ProofMismatch);
    return Accept();
}

ValidationResult ValidateCandidate(ExecutorRequest const& request,
                                   FixedTargetCandidate const& candidate) noexcept
{
    if (!candidate.reference.valid || !request.gather.valid ||
        !SameGatherReference(request.gather, candidate.reference) ||
        request.targetGuid != request.gather.spawnId ||
        request.itemId != request.gather.materialItemId)
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::CandidateMismatch,
                      ExecutorReason::ProofMismatch);
    if (!CandidateFactsSafe(candidate.facts))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::CandidateFactsMismatch,
                      ExecutorReason::InvalidOperationPayload);
    return Accept();
}

void SetValidation(DispatchResult& result, ValidationResult const& validation) noexcept
{
    result.status = validation.status;
    result.reason = validation.reason;
    result.sharedReason = validation.sharedReason;
}

bool GatherRouteSourceMatchesDescriptor(GatherRouteIntent const& intent) noexcept
{
    GatherSourceReference const& source = intent.source;
    AutoWowOracleRoute::RouteIntent const& route = intent.route;
    return AutoWowOracle::ValidGatherSourceReference(source) &&
        route.mapId == source.mapId && route.instanceId == source.instanceId &&
        route.entry == source.entry && route.stableSpawn == source.spawnId &&
        (route.targetKind == AutoWowOracleRoute::RouteTargetKind::GameObject ||
         route.targetKind == AutoWowOracleRoute::RouteTargetKind::GatherSource);
}

bool SessionMatchesGatherRoute(GatherRouteIntent const& intent,
                               RouteSession const& session) noexcept
{
    RouteKey const routeKey = AutoWowOracleRoute::MakeRouteKey(intent.route);
    RouteLedgerKey const ledgerKey = AutoWowOracleRoute::MakeRouteLedgerKey(routeKey);
    return session.state.routeKey == routeKey && session.state.ledgerKey == ledgerKey;
}

RouteObservation NormalizeRouteObservation(GatherRouteObservation const& observation) noexcept
{
    RouteObservation normalized = observation.route;
    normalized.objectiveDelta = observation.objectiveDeltaAvailable && observation.objectiveDelta;
    normalized.questDelta = observation.questDeltaAvailable && observation.questDelta;
    normalized.inventoryDelta = observation.inventoryDeltaAvailable && observation.inventoryDelta;
    normalized.interactionDelta =
        observation.interactionDeltaAvailable && observation.interactionDelta;
    return normalized;
}

GatherRouteEvidence MakeGatherRouteEvidence(
    RouteResult const& result, GatherRouteObservation const* observation) noexcept
{
    GatherRouteEvidence evidence;
    evidence.distanceAvailable = result.distance.available;
    evidence.initialRemainingDistance = result.distance.initial;
    evidence.remainingDistance = result.distance.remaining;
    evidence.reducedDistance = result.distance.reduced;
    evidence.segmentIndexAvailable = result.segmentIndexAvailable;
    evidence.segmentIndex = result.segmentIndex;

    if (!observation)
        return evidence;

    evidence.objectiveDeltaAvailable = observation->objectiveDeltaAvailable;
    evidence.objectiveDelta =
        observation->objectiveDeltaAvailable && observation->objectiveDelta;
    evidence.questDeltaAvailable = observation->questDeltaAvailable;
    evidence.questDelta = observation->questDeltaAvailable && observation->questDelta;
    evidence.inventoryDeltaAvailable = observation->inventoryDeltaAvailable;
    evidence.inventoryDelta =
        observation->inventoryDeltaAvailable && observation->inventoryDelta;
    evidence.interactionDeltaAvailable = observation->interactionDeltaAvailable;
    evidence.interactionDelta =
        observation->interactionDeltaAvailable && observation->interactionDelta;
    return evidence;
}

bool LedgerRequiresReentry(RouteSession const& session,
                           RouteLedgerKey const& ledgerKey) noexcept
{
    for (std::uint8_t index = 0;
         index < session.ledger.entryCount &&
         index < AutoWowOracleRoute::kMaxSessionLedgerEntries; ++index)
    {
        AutoWowOracleRoute::SessionLedgerEntry const& entry = session.ledger.entries[index];
        if (entry.occupied && entry.ledgerKey == ledgerKey)
            return entry.requiresReentry;
    }
    return false;
}

GatherRouteStep MapGatherRouteStep(GatherRouteIntent const& intent,
                                   AutoWowOracleRoute::RouteStep const& routeStep,
                                   GatherRouteObservation const* observation,
                                   bool valid = true) noexcept
{
    GatherRouteStep mapped;
    mapped.valid = valid;
    mapped.intent = intent;
    mapped.session = routeStep.session;
    mapped.result = routeStep.result;
    mapped.evidence = MakeGatherRouteEvidence(routeStep.result, observation);

    if (observation)
    {
        mapped.targetLoadKnown = observation->route.targetLoadKnown;
        mapped.targetLoaded = observation->route.targetLoadKnown && observation->route.targetLoaded;
        mapped.liveIdentityKnown = observation->route.hasLiveIdentity;
        if (mapped.liveIdentityKnown)
            mapped.liveIdentity = observation->route.liveIdentity;
    }

    bool const sessionMatches = SessionMatchesGatherRoute(intent, routeStep.session);
    mapped.exactCurrentIdentity = mapped.targetLoadKnown && mapped.targetLoaded &&
        mapped.liveIdentityKnown && mapped.liveIdentity == routeStep.result.routeKey;
    mapped.exactLiveBound = sessionMatches && routeStep.session.state.exactLiveBound;
    mapped.interactionReady = valid && sessionMatches && mapped.exactLiveBound &&
        mapped.exactCurrentIdentity && routeStep.result.arrived && !routeStep.result.blocked &&
        routeStep.result.stage == AutoWowOracleRoute::RouteStage::Arrived &&
        routeStep.result.failure == RouteFailure::None;

    mapped.releaseLease = routeStep.result.releaseLease;
    mapped.nextRetryAt = routeStep.result.nextRetryAt;
    mapped.backoffAvailable = mapped.nextRetryAt != 0;
    mapped.requiresReentry =
        LedgerRequiresReentry(routeStep.session, routeStep.result.ledgerKey);
    return mapped;
}

GatherRouteStep RejectGatherRoute(GatherRouteIntent const& intent,
                                  SessionLedger const& ledger,
                                  RouteFailure failure) noexcept
{
    AutoWowOracleRoute::RouteStep rejected;
    rejected.session.ledger = ledger;
    rejected.session.state.routeKey = AutoWowOracleRoute::MakeRouteKey(intent.route);
    rejected.session.state.ledgerKey =
        AutoWowOracleRoute::MakeRouteLedgerKey(rejected.session.state.routeKey);
    rejected.session.state.stage = AutoWowOracleRoute::RouteStage::Blocked;
    rejected.session.state.failure = failure;
    rejected.session.state.releaseLease = true;

    rejected.result.routeKey = rejected.session.state.routeKey;
    rejected.result.ledgerKey = rejected.session.state.ledgerKey;
    rejected.result.stage = AutoWowOracleRoute::RouteStage::Blocked;
    rejected.result.failure = failure;
    rejected.result.command.kind = AutoWowOracleRoute::RouteCommandKind::Yield;
    rejected.result.command.routeKey = rejected.result.routeKey;
    rejected.result.releaseLease = true;
    rejected.result.blocked = true;
    return MapGatherRouteStep(intent, rejected, nullptr, false);
}

void CopyRouteResult(DispatchResult& result, GatherRouteStep const& routeStep) noexcept
{
    result.routeExactLiveBound = routeStep.exactLiveBound;
    result.routeInteractionReady = routeStep.interactionReady;
    result.routeFailure = routeStep.result.failure;
    result.routeResult = routeStep.result;
    result.routeEvidence = routeStep.evidence;
}
}

GatherRouteIntent MakeGatherRouteIntent(AutoWowOracleRoute::RouteIntent const& route,
                                        GatherSourceReference const& source) noexcept
{
    return {route, source};
}

GatherRouteIntent MakeGatherRouteIntent(ExecutorRequest const& request,
                                        AutoWowOracleRoute::RouteIntent const& route) noexcept
{
    return MakeGatherRouteIntent(route, request.gather);
}

RouteFailure ValidateGatherRouteIntent(GatherRouteIntent const& intent) noexcept
{
    if (!GatherRouteSourceMatchesDescriptor(intent))
        return RouteFailure::InvalidIntent;

    // Area-trigger and world-transition segments are not exact gathering approaches. Preserve the
    // route policy's typed outcome so the caller can release/back off without substituting a node.
    if (intent.route.segment != AutoWowOracleRoute::RouteSegment::GroundPath &&
        intent.route.segment != AutoWowOracleRoute::RouteSegment::GridLoadApproach)
    {
        return RouteFailure::UnsupportedTransition;
    }
    return AutoWowOracleRoute::ValidateRouteIntent(intent.route);
}

bool ExactGatherRouteMatches(ExecutorRequest const& request,
                             GatherRouteIntent const& intent) noexcept
{
    return request.operation == OperationCode::GatherSource &&
        ValidGatherRequestReference(request) &&
        ValidateGatherRouteIntent(intent) == RouteFailure::None &&
        request.actorGuid == intent.route.actor &&
        SameGatherReference(request.gather, intent.source) &&
        request.gatherGoal == intent.source.goal && request.targetGuid == intent.route.stableSpawn &&
        request.itemId == intent.source.materialItemId;
}

GatherRouteStep StartGatherRoute(GatherRouteIntent const& intent,
                                 double estimatedTravelSeconds,
                                 Tick now,
                                 SessionLedger const& ledger,
                                 bool explicitReentrySignal) noexcept
{
    RouteFailure const failure = ValidateGatherRouteIntent(intent);
    if (failure != RouteFailure::None)
        return RejectGatherRoute(intent, ledger, failure);

    return MapGatherRouteStep(intent,
        AutoWowOracleRoute::StartRoute(
            intent.route, estimatedTravelSeconds, now, ledger, explicitReentrySignal),
        nullptr);
}

GatherRouteStep AdvanceGatherRoute(GatherRouteIntent const& intent,
                                   RouteSession const& session,
                                   GatherRouteObservation const& observation,
                                   Tick now) noexcept
{
    RouteFailure const failure = ValidateGatherRouteIntent(intent);
    if (failure != RouteFailure::None)
        return RejectGatherRoute(intent, session.ledger, failure);
    if (!SessionMatchesGatherRoute(intent, session))
        return RejectGatherRoute(intent, session.ledger, RouteFailure::IdentityDrift);

    RouteObservation const normalized = NormalizeRouteObservation(observation);
    return MapGatherRouteStep(
        intent, AutoWowOracleRoute::AdvanceRoute(session, normalized, now), &observation);
}

GatherRouteStep ResetGatherRoute(GatherRouteIntent const& intent,
                                 RouteSession const& session,
                                 Tick now,
                                 bool explicitReentrySignal) noexcept
{
    RouteFailure const failure = ValidateGatherRouteIntent(intent);
    if (failure != RouteFailure::None)
        return RejectGatherRoute(intent, session.ledger, failure);
    if (!SessionMatchesGatherRoute(intent, session))
        return RejectGatherRoute(intent, session.ledger, RouteFailure::IdentityDrift);

    return MapGatherRouteStep(intent,
        AutoWowOracleRoute::ResetAttempt(session, now, explicitReentrySignal), nullptr);
}

bool IsExactLiveBound(GatherRouteIntent const& intent,
                      RouteSession const& session) noexcept
{
    return ValidateGatherRouteIntent(intent) == RouteFailure::None &&
        SessionMatchesGatherRoute(intent, session) && session.state.exactLiveBound;
}

bool ExactCandidateMatches(FixedTargetCandidate const& expected,
                           FixedTargetCandidate const& observed) noexcept
{
    return expected.reference.valid && observed.reference.valid &&
        SameGatherReference(expected.reference, observed.reference) &&
        SameCandidateFacts(expected.facts, observed.facts) && CandidateFactsSafe(expected.facts) &&
        CandidateFactsSafe(observed.facts);
}

bool ExactCandidateMatches(ExecutorRequest const& request,
                           FixedTargetCandidate const& candidate) noexcept
{
    return request.operation == OperationCode::GatherSource && request.gather.valid &&
        request.gather.materialItemId != 0 && request.targetGuid == request.gather.spawnId &&
        request.itemId == request.gather.materialItemId &&
        SameGatherReference(request.gather, candidate.reference) && CandidateFactsSafe(candidate.facts);
}

ValidationResult OracleGatherExecutor::Validate(ExecutorRequest const& request,
                                                IntentLease const& lease,
                                                FixedTargetCandidate const& exactTarget,
                                                FixedTargetObservation const& observation,
                                                bool allowNewerFrame) noexcept
{
    ValidationResult result = ValidateRequestShape(request, lease);
    if (!result.valid)
        return result;

    result = ValidateWorldAndOwnership(request, observation, allowNewerFrame);
    if (!result.valid)
        return result;
    result = ValidateReservation(request, observation);
    if (!result.valid)
        return result;
    result = ValidateCandidate(request, observation.candidate);
    if (!result.valid)
        return result;
    if (!ExactCandidateMatches(exactTarget, observation.candidate))
        return Reject(ExecutorStatus::Blocked, FixedTargetReason::CandidateMismatch,
                      ExecutorReason::ProofMismatch);
    return Accept();
}

DispatchResult OracleGatherExecutor::Dispatch(ExecutorRequest const& request,
                                              IntentLease const& lease,
                                              FixedTargetCandidate const& exactTarget,
                                              FixedTargetObservation const& before,
                                              NativeStepFunction nativeStep,
                                              void* context) noexcept
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
    result.phase = before.world.lootReady ? NativeStepPhase::Loot :
        before.world.withinInteractRange ? NativeStepPhase::Interact : NativeStepPhase::Seek;

    if (!nativeStep)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, FixedTargetReason::NativeUnavailable,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    NativeStepRequest const nativeRequest{request, lease, before.nodeReservation,
        exactTarget, result.phase, 1, true};
    NativeStepObservation const native = nativeStep(context, nativeRequest);
    result.nativeCalled = true;
    result.stepsInvoked = native.stepsInvoked;
    result.after = native.after;

    ValidationResult const afterValidation =
        Validate(request, lease, exactTarget, native.after, true);
    if (!afterValidation.valid)
    {
        SetValidation(result, afterValidation);
        return result;
    }
    result.afterValidated = true;

    if (native.selectedAnotherCandidate)
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked, FixedTargetReason::NoFallback,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (!ExactCandidateMatches(exactTarget, native.after.candidate))
    {
        SetValidation(result, Reject(ExecutorStatus::Blocked, FixedTargetReason::PostconditionMismatch,
                                     ExecutorReason::ProofMismatch));
        return result;
    }
    if (!native.dispatchAccepted)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, FixedTargetReason::NativeRejected,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }
    if (native.stepsInvoked != 1)
    {
        SetValidation(result, Reject(ExecutorStatus::Failed, FixedTargetReason::StepBudgetExceeded,
                                     ExecutorReason::ExecutorFailed));
        return result;
    }

    result.accepted = true;
    result.credit = native.credit;
    // The native seam's acceptance is the interaction attempt. It is not completion: the receipt
    // policy still requires exact-source, canonical loot/skill/material evidence below.
    result.credit.interactionAttempted = true;
    result.credit.materialFulfillmentRequired =
        request.gatherGoal == AutoWowOracle::GatherGoal::ObtainMaterial;
    result.creditDecision = AutoWowGather::EvaluateGatherCredit(result.credit);
    // Do not infer completion from dispatchAccepted or from a loot/open-queue boolean. ObtainMaterial
    // requires the requested-material inventory delta; consumed-node/template evidence is only
    // sufficient for the weaker HarvestNode receipt.
    bool const confirmed = request.gatherGoal == AutoWowOracle::GatherGoal::ObtainMaterial
        ? AutoWowGather::IsConfirmedMaterialCredit(result.creditDecision)
        : AutoWowGather::IsConfirmedHarvestNodeCredit(result.creditDecision);
    if (confirmed)
    {
        result.status = ExecutorStatus::Completed;
        result.reason = FixedTargetReason::None;
        result.sharedReason = ExecutorReason::None;
    }
    else if (result.creditDecision == AutoWowGather::GatherCreditDecision::AwaitingInteraction ||
             result.creditDecision == AutoWowGather::GatherCreditDecision::AwaitingCredit)
    {
        result.status = ExecutorStatus::Progressing;
        result.reason = FixedTargetReason::None;
        result.sharedReason = ExecutorReason::None;
    }
    else
    {
        // A bounded receipt timeout/no-credit result is a failed attempt, never a progressing
        // lease. The caller may release/replan, but no fallback source is selected here.
        result.status = ExecutorStatus::Failed;
        result.reason = FixedTargetReason::NativeRejected;
        result.sharedReason = ExecutorReason::ExecutorFailed;
    }
    return result;
}

DispatchResult OracleGatherExecutor::DispatchAfterRoute(
    ExecutorRequest const& request,
    IntentLease const& lease,
    FixedTargetCandidate const& exactTarget,
    FixedTargetObservation const& before,
    GatherRouteIntent const& routeIntent,
    GatherRouteStep const& routeStep,
    NativeStepFunction nativeStep,
    void* context) noexcept
{
    DispatchResult gate;
    gate.valid = true;
    gate.request = request;
    gate.before = before;
    CopyRouteResult(gate, routeStep);

    RouteKey const expectedRouteKey = AutoWowOracleRoute::MakeRouteKey(routeIntent.route);
    RouteLedgerKey const expectedLedgerKey =
        AutoWowOracleRoute::MakeRouteLedgerKey(expectedRouteKey);
    bool const routeIdentityMatches = routeStep.valid &&
        ExactGatherRouteMatches(request, routeIntent) &&
        ValidateGatherRouteIntent(routeStep.intent) == RouteFailure::None &&
        SameGatherReference(routeStep.intent.source, routeIntent.source) &&
        AutoWowOracleRoute::MakeRouteKey(routeStep.intent.route) == expectedRouteKey &&
        SessionMatchesGatherRoute(routeIntent, routeStep.session) &&
        routeStep.result.routeKey == expectedRouteKey &&
        routeStep.result.ledgerKey == expectedLedgerKey &&
        routeStep.result.command.routeKey == expectedRouteKey &&
        routeStep.result.stage == routeStep.session.state.stage;
    if (!routeIdentityMatches)
    {
        gate.status = ExecutorStatus::Blocked;
        gate.reason = FixedTargetReason::RouteIntentMismatch;
        gate.sharedReason = ExecutorReason::ProofMismatch;
        gate.routeExactLiveBound = false;
        gate.routeInteractionReady = false;
        gate.routeFailure = RouteFailure::IdentityDrift;
        return gate;
    }
    gate.routeValidated = true;

    if (routeStep.result.blocked ||
        routeStep.result.stage == AutoWowOracleRoute::RouteStage::Blocked)
    {
        gate.status = ExecutorStatus::Blocked;
        gate.reason = FixedTargetReason::RouteBlocked;
        gate.sharedReason = ExecutorReason::OperationBlocked;
        gate.routeInteractionReady = false;
        return gate;
    }

    bool const exactCurrentIdentity = routeStep.targetLoadKnown && routeStep.targetLoaded &&
        routeStep.liveIdentityKnown && routeStep.liveIdentity == expectedRouteKey;
    bool const interactionReady = routeStep.interactionReady && exactCurrentIdentity &&
        routeStep.exactLiveBound && routeStep.session.state.exactLiveBound &&
        routeStep.result.arrived &&
        routeStep.result.stage == AutoWowOracleRoute::RouteStage::Arrived &&
        routeStep.result.failure == RouteFailure::None;
    if (!interactionReady)
    {
        gate.status = ExecutorStatus::Progressing;
        gate.reason = FixedTargetReason::RouteNotReady;
        gate.sharedReason = ExecutorReason::None;
        gate.routeInteractionReady = false;
        return gate;
    }

    DispatchResult result = Dispatch(
        request, lease, exactTarget, before, nativeStep, context);
    CopyRouteResult(result, routeStep);
    result.routeValidated = true;
    return result;
}

} // namespace AutoWowOracleGatherExecutor
