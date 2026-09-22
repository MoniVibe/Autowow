/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "WorkerGatherAction.h"

#include "Config.h"
#include "GameObject.h"
#include "G3D/Vector3.h"
#include "GatheringSafetyPolicy.h"
#include "GatheringWorkerState.h"
#include "AutoWowOracleOwnershipGate.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "OracleGatherRuntimePolicy.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "TravelMgr.h"
#include "WorkerGatherWatchdogPolicy.h"

#include <algorithm>
#include <iterator>

namespace
{
AutoWowGather::LiquidContact ToPolicyContact(LiquidStatus status)
{
    switch (status)
    {
        case LIQUID_MAP_NO_WATER: return AutoWowGather::LiquidContact::NoWater;
        case LIQUID_MAP_ABOVE_WATER: return AutoWowGather::LiquidContact::AboveWater;
        case LIQUID_MAP_WATER_WALK: return AutoWowGather::LiquidContact::WaterWalk;
        case LIQUID_MAP_IN_WATER: return AutoWowGather::LiquidContact::InWater;
        case LIQUID_MAP_UNDER_WATER: return AutoWowGather::LiquidContact::UnderWater;
        default: return AutoWowGather::LiquidContact::Unknown;
    }
}

AutoWowGather::LiquidKind ToPolicyKind(LiquidData const& liquid, AutoWowGather::LiquidContact contact)
{
    if (contact == AutoWowGather::LiquidContact::NoWater ||
        contact == AutoWowGather::LiquidContact::AboveWater)
        return AutoWowGather::LiquidKind::None;

    if (liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME | MAP_LIQUID_TYPE_DARK_WATER))
        return AutoWowGather::LiquidKind::Hazardous;
    if (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN))
        return AutoWowGather::LiquidKind::Water;
    return AutoWowGather::LiquidKind::Unknown;
}

AutoWowGather::LiquidSafetyDecision CheckCandidateLiquidSafety(Player* bot,
                                                               AutoWowGather::Candidate const& candidate)
{
    Map* map = bot->GetMap();
    LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), candidate.x, candidate.y, candidate.z,
                                                 bot->GetCollisionHeight(), {});
    AutoWowGather::LiquidContact const contact = ToPolicyContact(liquid.Status);
    AutoWowGather::LiquidEndpointClass const endpoint =
        AutoWowGather::ClassifyLiquidEndpoint({contact, ToPolicyKind(liquid, contact)});

    AutoWowGather::SwimmingRouteProof route;
    if (endpoint == AutoWowGather::LiquidEndpointClass::DeepWater)
    {
        route.canSwim = bot->CanSwim();

        float const distance = bot->GetExactDist(candidate.x, candidate.y, candidate.z);
        bool const alreadyAtEndpoint =
            distance <= sPlayerbotAIConfig.lootDistance &&
            map->IsInWater(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                           bot->GetCollisionHeight());
        if (alreadyAtEndpoint)
        {
            route.pathCalculated = true;
            route.pathComplete = true;
            route.allPathPointsInWater = true;
        }
        else
        {
            PathGenerator path(bot);
            route.pathCalculated = path.CalculatePath(candidate.x, candidate.y, candidate.z);

            std::uint32_t const rejectedPathTypes = PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH |
                                                    PATHFIND_FARFROMPOLY;
            route.pathComplete = route.pathCalculated &&
                                 !(static_cast<std::uint32_t>(path.GetPathType()) & rejectedPathTypes);
            route.allPathPointsInWater = path.GetPath().size() >= 2 && path.IsWaterPath(path.GetPath());
        }
    }

    return AutoWowGather::EvaluateLiquidSafety(endpoint, route);
}

bool HasFixedTargetMiningTool(Player* bot)
{
    static std::uint32_t const tools[] = {
        756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893
    };
    return bot && std::any_of(std::begin(tools), std::end(tools),
                              [bot](std::uint32_t itemId) { return bot->HasItemCount(itemId, 1); });
}

GameObject* FindExactLiveGatherSource(Player* bot, AutoWowGather::Candidate const& candidate,
                                      std::uint32_t instanceId)
{
    if (!bot || !bot->GetMap() || bot->GetMapId() != candidate.mapId ||
        (instanceId != 0 && bot->GetInstanceId() != instanceId))
        return nullptr;

    // This is the same exact spawn-id lookup used by ordinary WorkerGatherAction::Execute. It is
    // a visibility load for the published source coordinates, never a selector or movement call.
    bot->GetMap()->LoadGrid(candidate.x, candidate.y);
    auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(candidate.spawnId);
    for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
    {
        GameObject* go = iterator->second;
        if (!go || go->GetEntry() != candidate.entry || go->GetSpawnId() != candidate.spawnId ||
            go->GetMapId() != candidate.mapId || (instanceId != 0 && go->GetInstanceId() != instanceId))
            continue;
        return go;
    }
    return nullptr;
}

bool IsReadyExactGatherSource(GameObject* go)
{
    return go && go->IsInWorld() && go->isSpawned() && go->GetGoState() == GO_STATE_READY &&
        !go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND | GO_FLAG_NOT_SELECTABLE);
}

AutoWowOracleGatherExecutor::FixedTargetCandidate MakeFixedTargetCandidate(
    Player* bot, AutoWowGather::Candidate const& candidate,
    AutoWowOracle::GatherSourceReference const& reference, GameObject* source)
{
    AutoWowOracleGatherExecutor::FixedTargetCandidate exact;
    exact.reference = reference;
    exact.facts.alive = bot && bot->IsAlive();
    exact.facts.available = IsReadyExactGatherSource(source);
    exact.facts.reachable = bot && bot->GetMapId() == candidate.mapId &&
        (reference.instanceId == 0 || bot->GetInstanceId() == reference.instanceId);
    exact.facts.profession = candidate.profession;
    exact.facts.toolRequired = candidate.profession == AutoWowGather::Profession::Mining;
    exact.facts.toolAvailable = !exact.facts.toolRequired || HasFixedTargetMiningTool(bot);
    exact.facts.sourceYieldsRequestedMaterial =
        AutoWowGather::SourceYieldsMaterial(source, reference.materialItemId);
    return exact;
}

std::optional<AutoWowOracleGatherExecutor::FixedTargetCandidate> ResolveFixedTargetCandidate(
    Player* bot, PlayerbotAI* botAI, AutoWowOracle::GatherSourceReference const& reference)
{
    if (!bot || !botAI || !AutoWowOracle::ValidGatherSourceReference(reference) ||
        reference.materialItemId == 0)
        return std::nullopt;

    std::optional<AutoWowGather::Candidate> const candidate =
        AutoWowGather::ResolveExactCandidate(reference);
    if (!candidate || candidate->mapId != bot->GetMapId() ||
        bot->GetInstanceId() != reference.instanceId)
        return std::nullopt;

    GameObject* const source = FindExactLiveGatherSource(bot, *candidate, reference.instanceId);
    if (!IsReadyExactGatherSource(source))
        return std::nullopt;

    return MakeFixedTargetCandidate(bot, *candidate, reference, source);
}

void PreserveFixedTargetObservation(
    AutoWowOracleGatherExecutor::NativeStepObservation& result,
    WorkerGatherAction::FixedTargetNativeContext const& context,
    AutoWowOracleGatherExecutor::NativeStepRequest const& request)
{
    result.after = context.before;
    // These are caller-owned proofs. Copy the exact objects from the request back into the after
    // observation so the native adapter cannot accidentally replace them with local guesses.
    result.after.nodeReservation = request.nodeReservation;
    result.after.world.ownershipProof = request.request.ownershipProof;
    result.after.candidate = request.candidate;
}
}

bool WorkerGatherAction::MoveDeterministicToExact(
    WorldPosition const& destination, AutoWowOracle::DecisionId decisionId,
    AutoWowOracle::GatherSourceReference const& source, bool* outStuck)
{
    if (outStuck)
        *outStuck = false;

    if (!bot || !botAI || destination == WorldPosition() || destination.GetMapId() != bot->GetMapId())
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    bool const motionInProgress = IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL) ||
        bot->isMoving();
    AutoWowGatherRuntimePolicy::ExactMotionProvenance const provenance =
        AutoWowGather::GetOracleMotionProvenance(botGuid);
    bool const matchingProvenance = AutoWowGatherRuntimePolicy::MatchesExactMotionProvenance(
        provenance, decisionId, source);
    AutoWowGatherRuntimePolicy::ExactMotionDecision const motion =
        AutoWowGatherRuntimePolicy::EvaluateExactMotion(
            {true, motionInProgress, matchingProvenance});

    if (motion.clearInheritedMotion)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        AutoWowGather::ClearOracleMotionProvenance(botGuid);
    }

    if (motion.waitForCurrentExactMotion)
    {
        // Only motion issued by this decision for this full source identity may renew by waiting.
        return true;
    }

    if (!motion.issueExactMotion)
        return false;

    // Oracle movement is pre-probed and deterministic. A failed mmap probe is surfaced to the
    // caller; it never falls through to an ordinary randomized route or a teleport.
    PathGenerator path(bot);
    if (!path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ()))
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    // A published Oracle route is a complete probe, not permission to walk a best-effort partial
    // path. Any shortcut, incomplete/far-from-poly endpoint, no-path, or direct/no-path fallback
    // is rejected before MoveTo; the ordinary random/direct fallback branch is never used.
    std::uint32_t const disallowedPathTypes = PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE |
                                               PATHFIND_NOPATH | PATHFIND_FARFROMPOLY |
                                               PATHFIND_NOT_USING_PATH;
    if (static_cast<std::uint32_t>(path.GetPathType()) & disallowedPathTypes)
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    G3D::Vector3 const& endpoint = path.GetActualEndPosition();
    float const currentDistance = bot->GetExactDist(destination);
    if (destination.GetExactDist(endpoint.x, endpoint.y, endpoint.z) + 1.0f >= currentDistance)
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    bool const accepted = MoveTo(bot->GetMapId(), endpoint.x, endpoint.y, endpoint.z,
                                 false, false, false, true);
    if (!accepted)
        return false;

    if (!AutoWowGather::SetOracleMotionProvenance(botGuid, decisionId, source))
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        return false;
    }
    return true;
}

bool WorkerGatherAction::ObserveFixedTargetReceipt(
    void* rawContext, AutoWowGather::GatherCreditObservation* observation) noexcept
{
    FixedTargetNativeContext* context = static_cast<FixedTargetNativeContext*>(rawContext);
    if (!context || !observation)
        return false;

    if (context->controlledReceiptSnapshot)
    {
        AutoWowGather::GatherReceiptSnapshot snapshot;
        void* const snapshotContext = context->controlledReceiptSnapshotContext
            ? context->controlledReceiptSnapshotContext : context;
        if (!context->controlledReceiptSnapshot(snapshotContext, &snapshot))
            return false;

        *observation = AutoWowGather::MakeGatherCreditObservation(
            snapshot, context->before.candidate.reference.goal ==
                          AutoWowOracle::GatherGoal::ObtainMaterial);
        return true;
    }

    if (!context->bot || !context->botAI)
        return false;

    return AutoWowGather::ObserveCandidateCredit(context->bot, context->botAI, observation).has_value();
}

bool WorkerGatherAction::DispatchFixedTargetNativeStep(
    void* rawContext, AutoWowOracleGatherExecutor::NativeStepRequest const& request) noexcept
{
    FixedTargetNativeContext* context = static_cast<FixedTargetNativeContext*>(rawContext);
    if (!context || !context->bot || !context->botAI || !context->source || !context->candidateValid)
        return false;

    WorkerGatherAction action(context->botAI);
    bool accepted = false;
    switch (request.phase)
    {
        case AutoWowOracleGatherExecutor::NativeStepPhase::Seek:
        {
            bool stuck = false;
            accepted = action.MoveDeterministicToExact(
                WorldPosition(context->candidate.mapId, context->candidate.x,
                              context->candidate.y, context->candidate.z),
                request.request.decisionId, request.request.gather, &stuck);
            if (stuck)
                AutoWowGather::SetRouteStatus(context->bot->GetGUID().GetCounter(), "blocked",
                                              "oracle_exact_path_unavailable", "preprobe_failed");
            break;
        }
        case AutoWowOracleGatherExecutor::NativeStepPhase::Interact:
            AutoWowGather::SetCandidateRuntimeIdentity(context->bot->GetGUID().GetCounter(),
                                                       request.request.gather,
                                                       context->source->GetGUID());
            accepted = context->botAI->DoSpecificAction(
                "add gathering loot", Event("worker gather", context->source->GetGUID()), true);
            break;
        case AutoWowOracleGatherExecutor::NativeStepPhase::Loot:
        {
            LootObject exactLoot(context->bot, context->source->GetGUID());
            if (exactLoot.IsEmpty())
                return false;
            action.context->GetValue<LootObject>("loot target")->Set(exactLoot);
            AutoWowGather::SetCandidateRuntimeIdentity(context->bot->GetGUID().GetCounter(),
                                                       request.request.gather,
                                                       context->source->GetGUID());
            accepted = context->botAI->DoSpecificAction(
                "open loot", Event("worker gather", context->source->GetGUID()), true);
            break;
        }
        case AutoWowOracleGatherExecutor::NativeStepPhase::Unknown:
            return false;
    }
    return accepted;
}

void WorkerGatherAction::CaptureFixedTargetEvidence(
    void* rawContext, std::uint32_t materialItemId,
    AutoWowOracle::EvidenceCounters& evidence) noexcept
{
    FixedTargetNativeContext* context = static_cast<FixedTargetNativeContext*>(rawContext);
    if (!context || !context->bot)
        return;

    if (context->before.candidate.reference.goal == AutoWowOracle::GatherGoal::ObtainMaterial)
    {
        if (materialItemId == 0)
            return;
        evidence.resource = context->bot->GetItemCount(materialItemId, false);
        evidence.progress = evidence.resource;
        return;
    }

    SkillType skill = SKILL_NONE;
    switch (context->candidate.profession)
    {
        case AutoWowGather::Profession::Herbalism: skill = SKILL_HERBALISM; break;
        case AutoWowGather::Profession::Mining: skill = SKILL_MINING; break;
        case AutoWowGather::Profession::Skinning: skill = SKILL_SKINNING; break;
        case AutoWowGather::Profession::None: break;
    }
    evidence.resource = 0;
    evidence.progress = skill == SKILL_NONE ? 0 : context->bot->GetSkillValue(skill);
}

AutoWowOracleGatherExecutor::NativeStepObservation WorkerGatherAction::ExecuteFixedTargetNativeStep(
    void* rawContext, AutoWowOracleGatherExecutor::NativeStepRequest const& request) noexcept
{
    AutoWowOracleGatherExecutor::NativeStepObservation result;
    FixedTargetNativeContext* context = static_cast<FixedTargetNativeContext*>(rawContext);
    if (context)
        PreserveFixedTargetObservation(result, *context, request);

    if (!context || !context->botAI || request.maxSteps != 1 || !request.stopAfterStep)
        return result;

    PlayerbotAI* const botAI = context->botAI;
    Player* const bot = botAI->GetBot();
    if (!bot || botAI->IsAutoWowPaused())
        return result;

    context->bot = bot;
    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    AutoWowGather::RouteSnapshot const route = AutoWowGather::GetRouteSnapshot(botGuid);
    bool const ownershipActive = AutoWowOracleRuntime::IsOwned(
        botGuid, AutoWowOracleRuntime::CurrentTick());
    bool const matchingOwnership = AutoWowOracleRuntime::OwnsGatherSource(
        botGuid, request.request.decisionId, request.request.gather,
        AutoWowOracleRuntime::CurrentTick());
    AutoWowGatherRuntimePolicy::WorkerControlDecision const control =
        AutoWowGatherRuntimePolicy::EvaluateWorkerControl(
            {route.oracleExactSourceOnly, route.awaitingCredit, true, true,
             ownershipActive, matchingOwnership});
    if (!control.allowExactNativeExecutor)
        return result;

    std::optional<AutoWowGather::Candidate> const candidate =
        bot ? AutoWowGather::ResolveExactCandidate(request.request.gather) : std::nullopt;
    if (!bot || !candidate)
        return result;

    context->candidate = *candidate;
    context->candidateValid = true;

    GameObject* const source = FindExactLiveGatherSource(bot, *candidate, request.request.gather.instanceId);
    LootObject pendingLoot(bot, source ? source->GetGUID() : ObjectGuid::Empty);
    bool const pendingLootPhase = request.phase == AutoWowOracleGatherExecutor::NativeStepPhase::Loot &&
                                  !pendingLoot.IsEmpty();
    if (!IsReadyExactGatherSource(source) && !pendingLootPhase)
    {
        // Preserve the canonical receipt path after the source has been consumed and removed from
        // the live grid. A missing receipt remains a hard failure; no source or ordinary fallback
        // is substituted.
        return ExecuteCanonicalReceiptLoop(rawContext, request, context->before,
                                           &WorkerGatherAction::ObserveFixedTargetReceipt, nullptr,
                                           &WorkerGatherAction::CaptureFixedTargetEvidence);
    }

    AutoWowOracleGatherExecutor::FixedTargetCandidate liveTarget =
        MakeFixedTargetCandidate(bot, *candidate, request.request.gather, source);
    bool const latchedSourceProof = route.oracleExactSourceOnly && route.sourceMatched &&
        route.sourceGuid != 0 && route.sourceEntry == request.request.gather.entry &&
        route.requestedMaterialItemId == request.request.gather.materialItemId &&
        AutoWowOracle::SameGatherSourceReference(route.oracleSource, request.request.gather) &&
        route.sourceYieldsRequestedMaterial;
    if (!liveTarget.facts.sourceYieldsRequestedMaterial && latchedSourceProof)
        liveTarget.facts.sourceYieldsRequestedMaterial = true;
    if (pendingLootPhase)
        liveTarget.facts.available = true;
    if (!AutoWowOracleGatherExecutor::ExactCandidateMatches(request.candidate, liveTarget))
    {
        // Preserve the exact reference while reporting changed native facts. Never substitute a
        // different cache candidate to make the callback pass.
        result.after.candidate = liveTarget;
        return result;
    }

    if (!AutoWowGather::AdoptExactCandidate(static_cast<std::uint32_t>(request.request.actorGuid), *candidate,
                                            request.request.gather))
        return result;

    context->source = source;
    return ExecuteCanonicalReceiptLoop(rawContext, request, context->before,
                                      &WorkerGatherAction::ObserveFixedTargetReceipt,
                                      &WorkerGatherAction::DispatchFixedTargetNativeStep,
                                      &WorkerGatherAction::CaptureFixedTargetEvidence);
}

AutoWowOracleGatherExecutor::DispatchResult WorkerGatherAction::ExecuteFixedTargetGather(
    PlayerbotAI* botAI,
    AutoWowOracleExecutor::ExecutorRequest const& request,
    AutoWowOracle::IntentLease const& lease,
    AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot const& world,
    AutoWowOracleGatherExecutor::NodeReservationProof const& nodeReservation)
{
    AutoWowOracleGatherExecutor::FixedTargetCandidate exact;
    std::optional<AutoWowOracleGatherExecutor::FixedTargetCandidate> resolved;
    if (botAI)
        resolved = ResolveFixedTargetCandidate(botAI->GetBot(), botAI, request.gather);

    if (resolved)
        exact = *resolved;
    else
        exact.reference = request.gather;

    AutoWowOracleGatherExecutor::FixedTargetObservation before;
    before.world = world;
    before.nodeReservation = nodeReservation;
    before.candidate = exact;

    FixedTargetNativeContext context;
    context.botAI = botAI;
    context.before = before;

    // Missing/invalid exact targets are rejected before the native boundary. There is no callback
    // invocation that could select a replacement candidate.
    return AutoWowOracleGatherExecutor::OracleGatherExecutor::Dispatch(
        request, lease, exact, before,
        resolved ? FixedTargetNativeCallback() : nullptr,
        resolved ? static_cast<void*>(&context) : nullptr);
}

AutoWowOracleGatherExecutor::DispatchResult WorkerGatherAction::ExecuteFixedTargetGather(
    PlayerbotAI* botAI,
    AutoWowOracleExecutor::ExecutorRequest const& request,
    AutoWowOracle::IntentLease const& lease,
    AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot const& world,
    AutoWowOracleGatherExecutor::NodeReservationProof const& nodeReservation,
    AutoWowOracleGatherExecutor::FixedTargetCandidate const& exactCandidate)
{
    AutoWowOracleGatherExecutor::FixedTargetObservation before;
    before.world = world;
    before.nodeReservation = nodeReservation;
    before.candidate = exactCandidate;

    FixedTargetNativeContext context;
    context.botAI = botAI;
    context.before = before;
    return AutoWowOracleGatherExecutor::OracleGatherExecutor::Dispatch(
        request, lease, exactCandidate, before, FixedTargetNativeCallback(), &context);
}

bool WorkerGatherAction::isUseful()
{
    return bot && AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter());
}

bool WorkerGatherAction::FinishLeaseObservation(AutoWowGather::Candidate const& candidate,
                                                AutoWowGather::CandidateLeaseEvent leaseEvent)
{
    AutoWowGather::WorkerGatherWatchdogObservation observation;
    observation.explicitWorker = true;
    observation.hasCandidate = true;
    observation.candidateId = candidate.spawnId;
    observation.leaseCandidateId = candidate.spawnId;
    observation.leaseEvent = leaseEvent;

    AutoWowGather::WorkerGatherWatchdogDecision decision =
        AutoWowGather::EvaluateWorkerGatherWatchdog(observation);
    if (!decision.actionSucceeded)
        return false;

    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    if (!lootTarget.IsEmpty())
    {
        if (GameObject* lootGameObject = botAI->GetGameObject(lootTarget.guid);
            lootGameObject && lootGameObject->GetEntry() == candidate.entry &&
            lootGameObject->GetSpawnId() == candidate.spawnId)
            observation.lootTargetCandidateId = candidate.spawnId;
    }

    decision = AutoWowGather::EvaluateWorkerGatherWatchdog(observation);

    // ObserveCandidateProgress has already cooled and cleared this exact candidate, recorded the
    // typed candidate_lease_expired receipt, and advanced its event sequence exactly once.
    if (decision.clearMatchingLootTarget)
    {
        AI_VALUE(LootObjectStack*, "available loot")->Remove(lootTarget.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
    }
    return true;
}

bool WorkerGatherAction::ExecuteWatchdog()
{
    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    if (!AutoWowGather::IsExplicitWorker(botGuid) ||
        !sConfigMgr->GetOption<bool>("AutoWow.GatherSeek.Enabled", true) || botAI->IsAutoWowPaused() ||
        !bot->IsAlive() || bot->IsInCombat())
        return false;

    // Exact publication suppresses watchdog mutation even before the runtime is due or enabled.
    // A matching native claim activates only the fixed-target executor, never this watchdog.
    AutoWowGather::RouteSnapshot const route = AutoWowGather::GetRouteSnapshot(botGuid);
    bool const oracleOwned = AutoWowOracleRuntime::IsOwned(
        botGuid, AutoWowOracleRuntime::CurrentTick());
    bool const matchingOwnership = route.oracleExactSourceOnly &&
        AutoWowOracleRuntime::OwnsGatherSource(
            botGuid, route.oracleSource, AutoWowOracleRuntime::CurrentTick());
    AutoWowGatherRuntimePolicy::WorkerControlDecision const control =
        AutoWowGatherRuntimePolicy::EvaluateWorkerControl(
            {route.oracleExactSourceOnly, route.awaitingCredit, true, true,
             oracleOwned, matchingOwnership});
    if (control.suppressWatchdogMutation)
        return false;

    std::optional<AutoWowGather::Candidate> const candidate = AutoWowGather::CurrentCandidate(botGuid);
    if (!candidate || candidate->mapId != bot->GetMapId())
        return false;

    // A cast accepted by the normal loot action is still awaiting canonical credit. Do not let
    // the watchdog expire or re-route that candidate while its loot/skill receipt is pending.
    if (AutoWowGather::HasPendingCandidateCredit(botGuid))
        return false;

    std::uint32_t const missCooldown =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.GatherSeek.MissCooldownSeconds", 300);
    float const distanceToCandidate = bot->GetExactDist(candidate->x, candidate->y, candidate->z);
    AutoWowGather::CandidateLeaseEvent const leaseEvent = AutoWowGather::ObserveCandidateProgress(
        botGuid, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), distanceToCandidate, missCooldown);
    return FinishLeaseObservation(*candidate, leaseEvent);
}

bool WorkerGatherAction::Execute(Event /*event*/)
{
    if (getQualifier() == "watchdog")
        return ExecuteWatchdog();

    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    if (!AutoWowGather::IsExplicitWorker(botGuid))
        return false;

    // PlayerbotAI only returns to the non-combat engine after observing an alive bot. This is the
    // handoff point that clears the dead worker's stale node/loot receipt and permits a fresh
    // gather selection; no gather movement is resumed from the dead engine.
    if (bot->IsAlive())
        AutoWowGather::ObserveWorkerAlive(botGuid);

    if (!sConfigMgr->GetOption<bool>("AutoWow.GatherSeek.Enabled", true))
    {
        AutoWowGather::SetRouteStatus(botGuid, "suppressed", "gather_seek_disabled", "config_disabled");
        return false;
    }

    if (botAI->IsAutoWowPaused())
    {
        AutoWowGather::SetRouteStatus(botGuid, "suppressed", "worker_paused", "paused");
        return false;
    }

    // Exact publication is fail-closed even if the default-off runtime is disabled or this update
    // is not due. Only the read-only receipt observation below remains active in the worker.
    AutoWowGather::RouteSnapshot const route = AutoWowGather::GetRouteSnapshot(botGuid);
    bool const oracleOwned = AutoWowOracleRuntime::IsOwned(
        botGuid, AutoWowOracleRuntime::CurrentTick());
    bool const matchingOwnership = route.oracleExactSourceOnly &&
        AutoWowOracleRuntime::OwnsGatherSource(
            botGuid, route.oracleSource, AutoWowOracleRuntime::CurrentTick());
    AutoWowGatherRuntimePolicy::WorkerControlDecision const control =
        AutoWowGatherRuntimePolicy::EvaluateWorkerControl(
            {route.oracleExactSourceOnly, route.awaitingCredit, true, true,
             oracleOwned, matchingOwnership});

    // This is the only ordinary-worker work permitted after exact publication. It observes the
    // already-attempted source/inventory/skill receipt and cannot select or move a candidate.
    if (control.allowReadOnlyReceipt)
        AutoWowGather::ObserveCandidateCredit(bot, botAI);

    if (control.suppressOrdinaryMovement)
    {
        AutoWowGather::SetRouteStatus(
            botGuid, "suppressed",
            matchingOwnership ? "oracle_exact_executor_owned" : "oracle_exact_waiting_ownership",
            route.oracleExactSourceOnly ? "external_exact_source_only" : "oracle_owned");
        return false;
    }

    if (!bot->IsAlive())
    {
        AutoWowGather::SetRouteStatus(botGuid, "suppressed", "worker_dead", "dead");
        return false;
    }

    if (bot->IsInCombat())
    {
        AutoWowGather::SetRouteStatus(botGuid, "suppressed", "worker_in_combat", "combat");
        return false;
    }

    // The legacy OpenLootAction call marks only an interaction attempt. Hold the candidate until
    // the normal loot pipeline or core gathering-skill update gives durable evidence, including
    // explicit no-credit outcomes after the bounded confirmation window.
    if (AutoWowGather::ObserveCandidateCredit(bot, botAI))
        return true;

    std::optional<AutoWowGather::Candidate> candidate = AutoWowGather::CurrentCandidate(botGuid);
    if (candidate && candidate->mapId != bot->GetMapId())
    {
        AutoWowGather::MarkCandidateMiss(
            botGuid, sConfigMgr->GetOption<std::uint32_t>("AutoWow.GatherSeek.MissCooldownSeconds", 300),
            "map_changed");
        candidate.reset();
    }

    if (!candidate && AutoWowGather::IsOracleExactSourceOnly(botGuid))
    {
        AutoWowGather::SetRouteStatus(botGuid, "blocked", "exact_source_missing",
                                      "external_exact_source_required");
        return false;
    }

    if (!candidate)
        candidate = AutoWowGather::SelectCandidateFor(bot, botAI);
    if (!candidate)
        return false;

    WorldPosition destination(candidate->mapId, candidate->x, candidate->y, candidate->z);
    float const sightDistance = sConfigMgr->GetOption<float>("AutoWow.GatherSeek.SightDistance", 55.0f);
    std::uint32_t const missCooldown =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.GatherSeek.MissCooldownSeconds", 300);

    if (AutoWowGather::CandidateSafetyCheckRequired(botGuid))
    {
        AutoWowGather::LiquidSafetyDecision const liquidSafety = CheckCandidateLiquidSafety(bot, *candidate);
        if (!AutoWowGather::IsLiquidSafetyAllowed(liquidSafety))
        {
            AutoWowGather::MarkCandidateMiss(botGuid, missCooldown, AutoWowGather::LiquidSafetyReason(liquidSafety),
                                             AutoWowGather::CandidateFailureKind::UnsafeLiquid);
            return true;
        }
        AutoWowGather::MarkCandidateSafetyAccepted(botGuid);
    }

    float const distanceToCandidate = bot->GetExactDist(candidate->x, candidate->y, candidate->z);
    AutoWowGather::CandidateLeaseEvent const leaseEvent = AutoWowGather::ObserveCandidateProgress(
        botGuid, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), distanceToCandidate, missCooldown);
    if (FinishLeaseObservation(*candidate, leaseEvent))
        return true;

    if (distanceToCandidate <= sightDistance)
    {
        // This is the first spawned-state check. It is deliberately local: distant candidates are
        // remembered static spawns only, never queried through the pool manager as a spawned-state oracle.
        // A gameobject's DB spawn id is not its runtime ObjectGuid low value. Resolve the loaded
        // object through Map's spawn-id index; constructing a runtime GUID from spawnId makes every
        // pooled herb/ore node look absent even when that exact DB spawn is active in this grid.
        // The spawn-id store contains live objects only, so load this already-nearby grid before
        // consulting it. This is server-side visibility loading, not movement or a pool oracle.
        bot->GetMap()->LoadGrid(candidate->x, candidate->y);
        GameObject* go = nullptr;
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(candidate->spawnId);
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
        {
            GameObject* loaded = iterator->second;
            if (loaded && loaded->GetEntry() == candidate->entry)
            {
                go = loaded;
                break;
            }
        }
        if (!go || !go->IsInWorld() || !go->isSpawned() || go->GetGoState() != GO_STATE_READY ||
            go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND | GO_FLAG_NOT_SELECTABLE))
        {
            AutoWowGather::MarkCandidateMiss(botGuid, missCooldown, "not_spawned_or_ready_in_sight");
            return true;
        }

        // Sight distance is intentionally wider than ordinary loot distance so the worker can
        // validate a live node before committing to it. Do not hand off until the bot is inside
        // the canonical loot radius, though: the loot strategy applies that same radius and would
        // otherwise retain an unreachable queued GUID while this worker stopped walking.
        if (bot->GetDistance(go) <= sPlayerbotAIConfig.lootDistance)
        {
            // Hand off to the canonical gathering/loot path. AddGatheringLootAction re-checks the
            // live lock, profession rank, required tool and lootability; OpenLootAction casts the
            // normal gathering spell. Register the exact runtime source first so the receipt
            // captures skill, carried material, source loot, and packet baselines before that
            // canonical call can synchronously update them. This action never writes inventory or
            // profession skill state.
            AutoWowGather::SetCandidateRuntimeGuid(botGuid, candidate->spawnId, go->GetGUID());
            bool const added =
                botAI->DoSpecificAction("add gathering loot", Event("worker gather", go->GetGUID()), true);
            AutoWowGather::SetRouteStatus(botGuid, "in_sight",
                                          added ? "gather_loot_enqueued" : "gather_scan_waiting");
            return true;
        }
    }

    bool stuck = false;
    // questNoTeleport=true selects the strict no-teleport MoveFarTo branch even if bridge lifecycle
    // state is accidentally lost. Deploy also sets the league no-teleport policy as defense in depth.
    bool const walking = MoveFarTo(destination, /*questNoTeleport*/ true, &stuck);
    if (stuck)
    {
        AutoWowGather::MarkCandidateMiss(botGuid, missCooldown, "mmap_stuck_no_teleport");
        return true;
    }

    char const* movementEvent = walking ? "mmap_walk_started" : "mmap_walk_pending";
    if (leaseEvent == AutoWowGather::CandidateLeaseEvent::Progress)
        movementEvent = "candidate_progress";
    AutoWowGather::SetRouteStatus(botGuid, walking ? "walking" : "movement_wait", movementEvent);
    return walking;
}
