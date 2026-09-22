/*
 * Bounded exact one-craft AutoWow executor.
 *
 * This lane consumes a typed request and a fresh lease supplied by an upstream arbiter. It does
 * not widen AutoWowOracle's runtime allowlist, select a recipe, train a profession, buy or move
 * materials, invoke a loop, or mutate a Playerbot itself. The only effect boundary is the native
 * one-step callback supplied by the caller.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_CRAFT_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_CRAFT_EXECUTOR_H

#include "AutoWowOracleGatherCraftEconomyPolicy.h"
#include "OracleExecutorTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AutoWowOracleCraftExecutor
{
using AutoWowOracle::ActiveLeaseSnapshot;
using AutoWowOracle::Decision;
using AutoWowOracle::Epoch;
using AutoWowOracle::FactVersion;
using AutoWowOracle::Guid;
using AutoWowOracle::IntentLease;
using AutoWowOracle::ItemId;
using AutoWowOracle::OperationCode;
using AutoWowOracle::Scope;
using AutoWowOracle::Tick;
using AutoWowOracleExecutor::ExecutorReason;
using AutoWowOracleExecutor::ExecutorRequest;
using AutoWowOracleExecutor::ExecutorStatus;
using Profession = AutoWowOracleGatherCraftEconomy::Profession;

inline constexpr std::size_t kMaxCraftReagents =
    AutoWowOracleGatherCraftEconomy::kMaxRecipeInputs;

// A reagent slot is positional. The executor deliberately does not normalize or substitute
// reagent IDs: the reference emitted by the planner and the native observation must agree slot by
// slot.
struct CraftReagent
{
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
};

// The complete recipe identity and the skill evidence used to authorize one cast. recipeSpellId
// is the learned create-item spell, not an output item or a planner label.
struct CraftReference
{
    bool valid = false;
    std::uint32_t recipeSpellId = 0;
    ItemId outputItemId = 0;
    std::uint32_t outputCount = 0;
    std::array<CraftReagent, kMaxCraftReagents> reagents{};
    std::size_t reagentCount = 0;
    Profession profession = Profession::None;
    std::uint32_t requiredSkill = 0;
    std::uint32_t skillAtDecision = 0;
    std::uint32_t maximumSkillAtDecision = 0;
    bool professionKnown = false;
    bool knownRecipe = false;
};

// ExecutorRequest cannot be widened in this lane. The craft reference therefore travels beside
// the normalized request while the normalized request still carries the typed Craft operation and
// output item identity.
struct CraftRequest
{
    ExecutorRequest request;
    CraftReference craft;
};

struct CraftCandidate
{
    CraftReference reference;
};

// Counts are indexed by CraftReference::reagents[0..reagentCount). No other inventory item is a
// valid input to this operation.
struct CraftInventorySnapshot
{
    std::array<std::uint32_t, kMaxCraftReagents> reagentCounts{};
    std::uint32_t outputCount = 0;
    // Number of output units that can be accepted by the inventory (existing stack room plus
    // empty slots, as published by the native snapshotter).
    std::uint32_t freeOutputCapacity = 0;
};

struct CraftWorldSnapshot
{
    bool onWorldThread = false;
    bool botAlive = false;
    bool inCombat = false;
    bool cheatPathActive = true;
    bool knownRecipe = false;
    bool professionKnown = false;
    Guid botGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t currentSkill = 0;
    std::uint32_t maximumSkill = 0;
    Tick tick = 0;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    ActiveLeaseSnapshot ownershipProof;
};

struct CraftObservation
{
    CraftWorldSnapshot world;
    CraftCandidate candidate;
    CraftInventorySnapshot inventory;
};

enum class CraftStepPhase : std::uint8_t
{
    Unknown = 0,
    Cast
};

inline constexpr std::string_view CraftStepPhaseName(CraftStepPhase phase) noexcept
{
    switch (phase)
    {
        case CraftStepPhase::Unknown: return "unknown";
        case CraftStepPhase::Cast: return "cast";
    }
    return "unknown";
}

enum class CraftReason : std::uint8_t
{
    None = 0,
    InvalidRequest,
    InvalidCraftReference,
    WrongOperation,
    WrongDomain,
    WrongIntent,
    WrongResource,
    MissingLease,
    MissingOwnershipProof,
    LeaseExpired,
    LeaseMismatch,
    LeasePreempted,
    NotWorldThread,
    WrongBot,
    StaleFrame,
    StaleEpoch,
    Dead,
    InCombat,
    CheatPath,
    KnownRecipeMissing,
    ProfessionMissing,
    SkillInsufficient,
    InventoryCapacity,
    ReagentShortage,
    CandidateMismatch,
    NativeUnavailable,
    NativeRejected,
    StepBudgetExceeded,
    SelectedAnotherRecipe,
    OutputDeltaMismatch,
    ReagentDeltaMismatch,
    PostconditionMismatch
};

inline constexpr std::string_view CraftReasonName(CraftReason reason) noexcept
{
    switch (reason)
    {
        case CraftReason::None: return "none";
        case CraftReason::InvalidRequest: return "invalid_request";
        case CraftReason::InvalidCraftReference: return "invalid_craft_reference";
        case CraftReason::WrongOperation: return "wrong_operation";
        case CraftReason::WrongDomain: return "wrong_domain";
        case CraftReason::WrongIntent: return "wrong_intent";
        case CraftReason::WrongResource: return "wrong_resource";
        case CraftReason::MissingLease: return "missing_lease";
        case CraftReason::MissingOwnershipProof: return "missing_ownership_proof";
        case CraftReason::LeaseExpired: return "lease_expired";
        case CraftReason::LeaseMismatch: return "lease_mismatch";
        case CraftReason::LeasePreempted: return "lease_preempted";
        case CraftReason::NotWorldThread: return "not_world_thread";
        case CraftReason::WrongBot: return "wrong_bot";
        case CraftReason::StaleFrame: return "stale_frame";
        case CraftReason::StaleEpoch: return "stale_epoch";
        case CraftReason::Dead: return "dead";
        case CraftReason::InCombat: return "in_combat";
        case CraftReason::CheatPath: return "cheat_path";
        case CraftReason::KnownRecipeMissing: return "known_recipe_missing";
        case CraftReason::ProfessionMissing: return "profession_missing";
        case CraftReason::SkillInsufficient: return "skill_insufficient";
        case CraftReason::InventoryCapacity: return "inventory_capacity";
        case CraftReason::ReagentShortage: return "reagent_shortage";
        case CraftReason::CandidateMismatch: return "candidate_mismatch";
        case CraftReason::NativeUnavailable: return "native_unavailable";
        case CraftReason::NativeRejected: return "native_rejected";
        case CraftReason::StepBudgetExceeded: return "step_budget_exceeded";
        case CraftReason::SelectedAnotherRecipe: return "selected_another_recipe";
        case CraftReason::OutputDeltaMismatch: return "output_delta_mismatch";
        case CraftReason::ReagentDeltaMismatch: return "reagent_delta_mismatch";
        case CraftReason::PostconditionMismatch: return "postcondition_mismatch";
    }
    return "invalid_request";
}

struct ValidationResult
{
    bool valid = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    CraftReason reason = CraftReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
};

struct NativeStepRequest
{
    CraftRequest request;
    IntentLease lease;
    CraftCandidate candidate;
    CraftStepPhase phase = CraftStepPhase::Unknown;
    std::uint8_t maxSteps = 1;
    bool stopAfterStep = true;
};

struct NativeStepObservation
{
    bool dispatchAccepted = false;
    std::uint8_t stepsInvoked = 0;
    bool selectedAnotherRecipe = false;
    CraftObservation after;
};

using NativeStepFunction = NativeStepObservation (*)(void* context,
                                                      NativeStepRequest const& request);

struct DispatchResult
{
    bool valid = false;
    bool accepted = false;
    ExecutorStatus status = ExecutorStatus::Invalid;
    CraftReason reason = CraftReason::InvalidRequest;
    ExecutorReason sharedReason = ExecutorReason::InvalidRequest;
    CraftStepPhase phase = CraftStepPhase::Unknown;
    std::uint8_t stepsInvoked = 0;
    bool nativeCalled = false;
    bool beforeValidated = false;
    bool afterValidated = false;
    CraftRequest request;
    CraftCandidate candidate;
    CraftObservation before;
    CraftObservation after;
};

[[nodiscard]] bool ValidCraftReference(CraftReference const& reference) noexcept;
[[nodiscard]] bool ExactReferenceMatches(CraftReference const& expected,
                                          CraftReference const& observed) noexcept;
[[nodiscard]] bool ExactCandidateMatches(CraftCandidate const& expected,
                                          CraftCandidate const& observed) noexcept;
[[nodiscard]] bool ExactCandidateMatches(CraftRequest const& request,
                                          CraftCandidate const& candidate) noexcept;

class OracleCraftExecutor final
{
public:
    // The current shared contract intentionally rejects Craft in IsVerifiedOperationTuple(). This
    // pure adapter validates the future Craft payload without calling PrepareRequest() or widening
    // that authority gate.
    [[nodiscard]] static ValidationResult Validate(CraftRequest const& request,
                                                    IntentLease const& lease,
                                                    CraftCandidate const& exactCandidate,
                                                    CraftObservation const& observation) noexcept;

    // Exactly one native cast callback is permitted. The callback must return the same recipe
    // candidate and exact reagent/output deltas; no retry, selector, fallback, or loop exists.
    [[nodiscard]] static DispatchResult Dispatch(CraftRequest const& request,
                                                  IntentLease const& lease,
                                                  CraftCandidate const& exactCandidate,
                                                  CraftObservation const& before,
                                                  NativeStepFunction nativeStep,
                                                  void* context = nullptr) noexcept;
};

} // namespace AutoWowOracleCraftExecutor

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_CRAFT_EXECUTOR_H
