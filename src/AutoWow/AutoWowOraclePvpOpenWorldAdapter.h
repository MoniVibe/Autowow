/*
 * Contract adapter for the pure PvP/open-world planner.
 *
 * The planner remains responsible for facts and deterministic choice. This adapter only
 * translates one planned intent into the shared AutoWowOracle candidate model; the shared Plan()
 * and OracleArbiter are the only lease/selection path.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_ADAPTER_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_ADAPTER_H

#include "AutoWowOracleContract.h"
#include "AutoWowOraclePvpOpenWorldPolicy.h"

namespace AutoWowOraclePvpOpenWorld
{
namespace ContractAdapter
{
struct IntentMapping
{
    bool supported = false;
    AutoWowOracle::Domain domain = AutoWowOracle::Domain::Combat;
    AutoWowOracle::IntentCode intent = AutoWowOracle::IntentCode::CombatAction;
    AutoWowOracle::OperationCode operation = AutoWowOracle::OperationCode::Unknown;
    AutoWowOracle::LeaseResource resource = AutoWowOracle::LeaseResource::Idle;
    bool requiresBotAlive = true;
    bool requiresSameMap = true;
};

inline constexpr AutoWowOracle::RoleCode ToOracleRole(SquadRole role)
{
    switch (role)
    {
        case SquadRole::Captain:
            return AutoWowOracle::RoleCode::Captain;
        case SquadRole::Healer:
            return AutoWowOracle::RoleCode::Healer;
        case SquadRole::Tank:
            return AutoWowOracle::RoleCode::Tank;
        case SquadRole::FlagCarrier:
            return AutoWowOracle::RoleCode::FlagCarrier;
        case SquadRole::Escort:
            return AutoWowOracle::RoleCode::Escort;
        case SquadRole::Interceptor:
            return AutoWowOracle::RoleCode::Interceptor;
        case SquadRole::Defender:
            return AutoWowOracle::RoleCode::Defender;
        case SquadRole::Recovery:
            return AutoWowOracle::RoleCode::Recovery;
        case SquadRole::Damage:
            return AutoWowOracle::RoleCode::Damage;
        case SquadRole::Unassigned:
            return AutoWowOracle::RoleCode::Unknown;
    }
    return AutoWowOracle::RoleCode::Unknown;
}

inline constexpr AutoWowOracle::PlannerModeId ToOraclePlannerMode(PlannerMode mode)
{
    return static_cast<AutoWowOracle::PlannerModeId>(mode);
}

inline constexpr AutoWowOracle::PlannerReasonId ToOraclePlannerReason(Reason reason)
{
    return static_cast<AutoWowOracle::PlannerReasonId>(reason);
}

inline constexpr IntentMapping MapIntent(IntentKind intent, bool selfAlive)
{
    switch (intent)
    {
        case IntentKind::CaptureObjective:
        case IntentKind::DefendObjective:
        case IntentKind::EscortCarrier:
        case IntentKind::InterceptCarrier:
        case IntentKind::RecoverFlag:
            return {true, AutoWowOracle::Domain::PvP, AutoWowOracle::IntentCode::PvPObjective,
                AutoWowOracle::OperationCode::PvPObjective,
                AutoWowOracle::LeaseResource::CombatPositioning, true, true};
        case IntentKind::AttackHostile:
        case IntentKind::DefendAlly:
        case IntentKind::ProtectHealer:
        case IntentKind::ProtectTank:
            return {true, AutoWowOracle::Domain::Combat, AutoWowOracle::IntentCode::CombatAction,
                AutoWowOracle::OperationCode::CombatAction,
                AutoWowOracle::LeaseResource::CombatPositioning, true, true};
        case IntentKind::Disengage:
            // Keep this as a navigation intent so shared Plan() does not replace it with a
            // ShadowCombat attack choice. CombatPositioning still gives escape precedence over
            // ordinary quest/gather work.
            return {true, AutoWowOracle::Domain::Navigation, AutoWowOracle::IntentCode::Disengage,
                AutoWowOracle::OperationCode::Disengage,
                AutoWowOracle::LeaseResource::CombatPositioning, true, true};
        case IntentKind::Regroup:
            return {true, AutoWowOracle::Domain::Navigation, AutoWowOracle::IntentCode::Navigate,
                AutoWowOracle::OperationCode::Navigate,
                AutoWowOracle::LeaseResource::Transition, true, true};
        case IntentKind::RecoverDeath:
            return {true, AutoWowOracle::Domain::Recovery, AutoWowOracle::IntentCode::Recover,
                AutoWowOracle::OperationCode::Recover,
                AutoWowOracle::LeaseResource::Recovery, selfAlive, true};
        case IntentKind::AcceptCampaignChallenge:
        case IntentKind::DeclineCampaignChallenge:
            return {true, AutoWowOracle::Domain::PvP, AutoWowOracle::IntentCode::PvPObjective,
                AutoWowOracle::OperationCode::PvPChallenge,
                AutoWowOracle::LeaseResource::Transition, true, true};
        case IntentKind::None:
            return {};
    }
    return {};
}

inline bool TryBuildCandidate(PlannerFrame const& frame, PlanResult const& plan,
    AutoWowOracle::OracleCandidate& candidate)
{
    candidate = {};
    candidate.available = false;
    if (plan.status != PlanStatus::Planned || !plan.intent.valid || frame.selfGuid == 0 ||
        !PlannerFrameWithinBounds(frame) || plan.intent.ttlTicks == 0)
        return false;

    // An objective owner is the only bot allowed to emit that objective's candidate. Intents with
    // ownerGuid == 0 are deliberately squad-visible and remain arbitrated by the shared contract.
    if (plan.intent.ownerGuid != 0 && plan.intent.ownerGuid != frame.selfGuid)
        return false;

    IntentMapping const mapping = MapIntent(plan.intent.intent, frame.recovery.selfAlive);
    if (!mapping.supported)
        return false;

    candidate.domain = mapping.domain;
    candidate.intent = mapping.intent;
    candidate.operation = mapping.operation;
    candidate.resource = mapping.resource;
    candidate.classification = AutoWowOracle::IntentClassification::Persistent;
    candidate.priority = AutoWowOracle::PriorityFromScore(plan.intent.priority);
    candidate.ttlTicks = plan.intent.ttlTicks;
    candidate.targetGuid = plan.intent.targetGuid;
    candidate.action = IntentKindName(plan.intent.intent);
    candidate.available = true;
    // Every PvP/open-world operation remains planning-visible but blocked until it receives an
    // explicit executor and postcondition contract. Semantic combat labels are not executors.
    candidate.executorAvailable = false;
    candidate.requiresBotAlive = mapping.requiresBotAlive;
    candidate.requiresSameMap = mapping.requiresSameMap;
    candidate.objectiveId = plan.intent.intent == IntentKind::Regroup ? 0 : plan.intent.objectiveId;
    candidate.rallyPointId = plan.intent.intent == IntentKind::Regroup ? plan.intent.objectiveId : 0;
    candidate.actorGuid = frame.selfGuid;
    candidate.ownerGuid = plan.intent.ownerGuid;
    candidate.role = ToOracleRole(plan.intent.role);
    candidate.plannerMode = ToOraclePlannerMode(plan.intent.mode);
    candidate.plannerReason = ToOraclePlannerReason(plan.intent.reason);
    return true;
}

inline bool AppendCandidate(PlannerFrame const& frame, PlanResult const& plan,
    AutoWowOracle::WorldReadFrame& oracleFrame)
{
    AutoWowOracle::OracleCandidate candidate;
    if (!TryBuildCandidate(frame, plan, candidate))
        return false;
    return AutoWowOracle::AddCandidate(oracleFrame, candidate);
}

// This is the sole planner-to-contract entry point. It emits no operation and owns no lease;
// shared AutoWowOracle::Plan() and OracleArbiter<> remain the only selection and arbitration path.
inline AutoWowOracle::PlanResult PlanIntoOracle(PlannerFrame const& frame,
    AutoWowOracle::WorldReadFrame& oracleFrame)
{
    PlanResult const plannerResult = Plan(frame);
    oracleFrame.plannerMode = ToOraclePlannerMode(plannerResult.intent.mode);
    oracleFrame.plannerReason = ToOraclePlannerReason(plannerResult.intent.reason);
    AppendCandidate(frame, plannerResult, oracleFrame);
    return AutoWowOracle::Plan(oracleFrame);
}
} // namespace ContractAdapter
} // namespace AutoWowOraclePvpOpenWorld

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_ADAPTER_H
