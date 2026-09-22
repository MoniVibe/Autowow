/*
 * Bounded exact-hostile-player combat executor.
 *
 * This slice consumes the existing AutoWow lease/decision types but owns no authority. It does
 * not widen AutoWowOracleContract, choose a target, queue a battleground, teleport, mutate PvP
 * rewards, or invoke an ordinary target selector. The caller supplies a fresh world observation
 * and a native one-step callback; this adapter only validates the exact target before and after
 * that callback.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_COMBAT_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_COMBAT_EXECUTOR_H

#include "OracleExecutorTypes.h"

#include <cstdint>
#include <string_view>

namespace AutoWowOraclePvpCombatExecutor
{
using AutoWowOracle::ActiveLeaseSnapshot;
using AutoWowOracle::DecisionId;
using AutoWowOracle::Domain;
using AutoWowOracle::Epoch;
using AutoWowOracle::FactVersion;
using AutoWowOracle::Guid;
using AutoWowOracle::IntentCode;
using AutoWowOracle::IntentLease;
using AutoWowOracle::LeaseResource;
using AutoWowOracle::OperationCode;
using AutoWowOracle::Scope;
using AutoWowOracle::Tick;
using AutoWowOracleExecutor::ExecutorReason;
using AutoWowOracleExecutor::ExecutorStatus;

// This is deliberately a small typed relation, not a caller-provided "hostile" string. The
// native resolver must prove the relationship from the actor and target faction/team facts.
enum class TeamRelation : std::uint8_t
{
    Unknown = 0,
    Friendly,
    Hostile
};

[[nodiscard]] constexpr std::string_view TeamRelationName(TeamRelation relation) noexcept
{
    switch (relation)
    {
        case TeamRelation::Unknown: return "unknown";
        case TeamRelation::Friendly: return "friendly";
        case TeamRelation::Hostile: return "hostile";
    }
    return "unknown";
}

// Full identity and faction facts selected by an upstream planner/resolver. The fact version is
// part of the reference so a stale hostile-player observation cannot be reused for an attack.
struct HostilePlayerReference
{
    bool valid = false;
    bool isPlayer = false;
    Guid targetGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    TeamRelation relation = TeamRelation::Unknown;
    std::uint32_t actorFaction = 0;
    std::uint32_t targetFaction = 0;
    FactVersion factVersion = 0;
};

// Fresh target facts returned by the world-thread resolver. Identity/faction fields must remain
// exact; alive/attackable/PvP-permitted are preconditions for the native step. A post-step target
// may be dead or no longer attackable when the exact attack killed it, but it may not be replaced.
struct HostilePlayerState
{
    bool isPlayer = false;
    Guid targetGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    bool inWorld = false;
    bool alive = false;
    bool attackable = false;
    bool pvpPermitted = false;
    TeamRelation relation = TeamRelation::Unknown;
    std::uint32_t actorFaction = 0;
    std::uint32_t targetFaction = 0;
    FactVersion factVersion = 0;
};

struct CombatWorldSnapshot
{
    bool onWorldThread = false;
    bool actorAlive = false;
    Guid actorGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    Tick tick = 0;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    ActiveLeaseSnapshot ownershipProof;
};

struct HostilePlayerObservation
{
    CombatWorldSnapshot world;
    HostilePlayerState target;
};

// This is an adapter request, not a second lease authority. The copied ownershipProof is checked
// against the fresh proof in every observation and against the authoritative IntentLease.
struct PvpCombatRequest
{
    bool valid = false;
    std::uint64_t requestId = 0;
    OperationCode operation = OperationCode::Unknown;
    Domain domain = Domain::Combat;
    IntentCode intent = IntentCode::CombatAction;
    LeaseResource resource = LeaseResource::Idle;
    Scope scope;
    DecisionId decisionId = 0;
    AutoWowOracle::IntentId intentId = 0;
    Guid actorGuid = 0;
    bool executorAvailable = false;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    Tick issuedTick = 0;
    Tick expiresTick = 0;
    std::uint32_t ttlTicks = 0;
    HostilePlayerReference target;
    ActiveLeaseSnapshot ownershipProof;
};

enum class PvpCombatReason : std::uint8_t
{
    None = 0,
    InvalidRequest,
    WrongOperation,
    WrongDomain,
    WrongIntent,
    WrongResource,
    ExecutorUnavailable,
    InvalidTargetReference,
    TargetReferenceMismatch,
    NotWorldThread,
    WrongBot,
    ActorDead,
    StaleFrame,
    StaleEpoch,
    FactVersionStale,
    LeaseExpired,
    MissingOwnershipProof,
    LeaseMismatch,
    LeasePreempted,
    TargetGuidMismatch,
    TargetMapMismatch,
    TargetInstanceMismatch,
    TargetNotPlayer,
    TargetNotInWorld,
    TargetDead,
    TargetNotAttackable,
    PvpProhibited,
    TeamRelationMismatch,
    FactionMismatch,
    NativeUnavailable,
    NativeRejected,
    StepBudgetExceeded,
    TargetSubstitution,
    PostconditionMismatch
};

[[nodiscard]] constexpr std::string_view PvpCombatReasonName(PvpCombatReason reason) noexcept
{
    switch (reason)
    {
        case PvpCombatReason::None: return "none";
        case PvpCombatReason::InvalidRequest: return "invalid_request";
        case PvpCombatReason::WrongOperation: return "wrong_operation";
        case PvpCombatReason::WrongDomain: return "wrong_domain";
        case PvpCombatReason::WrongIntent: return "wrong_intent";
        case PvpCombatReason::WrongResource: return "wrong_resource";
        case PvpCombatReason::ExecutorUnavailable: return "executor_unavailable";
        case PvpCombatReason::InvalidTargetReference: return "invalid_target_reference";
        case PvpCombatReason::TargetReferenceMismatch: return "target_reference_mismatch";
        case PvpCombatReason::NotWorldThread: return "not_world_thread";
        case PvpCombatReason::WrongBot: return "wrong_bot";
        case PvpCombatReason::ActorDead: return "actor_dead";
        case PvpCombatReason::StaleFrame: return "stale_frame";
        case PvpCombatReason::StaleEpoch: return "stale_epoch";
        case PvpCombatReason::FactVersionStale: return "fact_version_stale";
        case PvpCombatReason::LeaseExpired: return "lease_expired";
        case PvpCombatReason::MissingOwnershipProof: return "missing_ownership_proof";
        case PvpCombatReason::LeaseMismatch: return "lease_mismatch";
        case PvpCombatReason::LeasePreempted: return "lease_preempted";
        case PvpCombatReason::TargetGuidMismatch: return "target_guid_mismatch";
        case PvpCombatReason::TargetMapMismatch: return "target_map_mismatch";
        case PvpCombatReason::TargetInstanceMismatch: return "target_instance_mismatch";
        case PvpCombatReason::TargetNotPlayer: return "target_not_player";
        case PvpCombatReason::TargetNotInWorld: return "target_not_in_world";
        case PvpCombatReason::TargetDead: return "target_dead";
        case PvpCombatReason::TargetNotAttackable: return "target_not_attackable";
        case PvpCombatReason::PvpProhibited: return "pvp_prohibited";
        case PvpCombatReason::TeamRelationMismatch: return "team_relation_mismatch";
        case PvpCombatReason::FactionMismatch: return "faction_mismatch";
        case PvpCombatReason::NativeUnavailable: return "native_unavailable";
        case PvpCombatReason::NativeRejected: return "native_rejected";
        case PvpCombatReason::StepBudgetExceeded: return "step_budget_exceeded";
        case PvpCombatReason::TargetSubstitution: return "target_substitution";
        case PvpCombatReason::PostconditionMismatch: return "postcondition_mismatch";
    }
    return "invalid_request";
}

struct ValidationResult
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    PvpCombatReason reason = PvpCombatReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
};

struct NativeStepRequest
{
    PvpCombatRequest request;
    IntentLease lease;
    HostilePlayerReference exactTarget;
    std::uint8_t maxSteps = 1;
    bool stopAfterStep = true;
};

struct NativeStepObservation
{
    bool dispatchAccepted = false;
    std::uint8_t stepsInvoked = 0;
    Guid selectedTargetGuid = 0;
    HostilePlayerObservation after;
};

using NativeStepFunction = NativeStepObservation (*)(void* context,
                                                      NativeStepRequest const& request);

struct DispatchResult
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    PvpCombatReason reason = PvpCombatReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
    std::uint8_t stepsInvoked = 0;
    bool nativeCalled = false;
    bool beforeValidated = false;
    bool afterValidated = false;
    Guid selectedTargetGuid = 0;
    PvpCombatRequest request;
    HostilePlayerObservation before;
    HostilePlayerObservation after;
};

// Exact identity comparison intentionally excludes transient alive/attackable state. Those facts
// are validated separately, allowing a successful attack to observe the same target dead after
// the native step without permitting a target swap.
[[nodiscard]] bool ExactTargetMatches(HostilePlayerReference const& expected,
                                       HostilePlayerReference const& observed) noexcept;
[[nodiscard]] bool ExactTargetMatches(HostilePlayerReference const& expected,
                                       HostilePlayerState const& observed,
                                       bool allowNewerFactVersion = false) noexcept;
[[nodiscard]] bool TargetFactsSafe(HostilePlayerState const& state) noexcept;

class OraclePvpCombatExecutor final
{
public:
    // Pure validation. The observation's ownership proof is the current authority; the copied
    // request proof is checked only for consistency. allowNewerFrame is true only for an
    // immediately returned post-step observation.
    [[nodiscard]] static ValidationResult Validate(PvpCombatRequest const& request,
                                                    IntentLease const& lease,
                                                    HostilePlayerReference const& exactTarget,
                                                    HostilePlayerObservation const& observation,
                                                    bool allowNewerFrame = false) noexcept;

    // The only native boundary. It invokes the callback at most once, with one exact target and a
    // one-step budget, then revalidates ownership, frame, target identity, faction facts, and the
    // target's fresh fact version before reporting progress.
    [[nodiscard]] static DispatchResult Dispatch(PvpCombatRequest const& request,
                                                  IntentLease const& lease,
                                                  HostilePlayerReference const& exactTarget,
                                                  HostilePlayerObservation const& before,
                                                  NativeStepFunction nativeStep,
                                                  void* context = nullptr) noexcept;
};

} // namespace AutoWowOraclePvpCombatExecutor

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_COMBAT_EXECUTOR_H
