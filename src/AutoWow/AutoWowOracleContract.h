/*
 * Pure AutoWow Oracle contract.
 *
 * This file is deliberately a policy/data boundary. A caller publishes one immutable
 * WorldReadFrame, asks Plan() for a decision, acquires an IntentLease, and hands the lease to
 * an existing Playerbot executor. The contract does not move players, cast spells, touch a
 * database, read a clock, perform I/O, or own a live Playerbot object.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_CONTRACT_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_CONTRACT_H

#include "ShadowCombatOraclePolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <tuple>

namespace AutoWowOracle
{
using Guid = std::uint64_t;
using SquadId = std::uint64_t;
using QuestId = std::uint32_t;
using NodeId = std::uint64_t;
using ItemId = std::uint32_t;
using Tick = std::uint64_t;
using FactVersion = std::uint64_t;
using Epoch = std::uint64_t;
using DecisionId = std::uint64_t;
using IntentId = std::uint64_t;
using StableId = std::uint64_t;
using PlannerModeId = std::uint8_t;
using PlannerReasonId = std::uint8_t;

inline constexpr PlannerModeId kPlannerModeUnknown = 0;
inline constexpr PlannerReasonId kPlannerReasonNone = 0;

// Domain policies may keep their richer role enum locally. This stable, domain-neutral
// vocabulary carries actor/owner metadata without coupling the contract to one policy.
enum class RoleCode : std::uint8_t
{
    Unknown = 0,
    Captain,
    Healer,
    Tank,
    FlagCarrier,
    Escort,
    Interceptor,
    Defender,
    Recovery,
    Damage
};

inline constexpr std::string_view RoleCodeName(RoleCode role)
{
    switch (role)
    {
        case RoleCode::Unknown:
            return "unknown";
        case RoleCode::Captain:
            return "captain";
        case RoleCode::Healer:
            return "healer";
        case RoleCode::Tank:
            return "tank";
        case RoleCode::FlagCarrier:
            return "flag_carrier";
        case RoleCode::Escort:
            return "escort";
        case RoleCode::Interceptor:
            return "interceptor";
        case RoleCode::Defender:
            return "defender";
        case RoleCode::Recovery:
            return "recovery";
        case RoleCode::Damage:
            return "damage";
    }
    return "unknown";
}

inline constexpr bool RoleCodeWithinBounds(RoleCode role)
{
    return static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(RoleCode::Damage);
}

inline constexpr std::size_t kMaxWorldCandidates = 32;
inline constexpr std::size_t kMaxCombatAllies = 256;
inline constexpr std::size_t kMaxCombatEnemies = 512;
inline constexpr std::size_t kMaxCombatHazards = 256;
inline constexpr std::size_t kMaxCombatCandidates = 64;
inline constexpr std::size_t kMaxBotLeases = 256;
inline constexpr std::size_t kMaxSquadOwners = 64;
inline constexpr std::size_t kMaxItemReservations = 512;
inline constexpr std::size_t kMaxNodeReservations = 512;

enum class Domain : std::uint8_t
{
    Combat,
    Navigation,
    Quest,
    Gathering,
    CraftingEconomyItems,
    GroupRoster,
    Recovery,
    PvP
};

inline constexpr std::string_view DomainName(Domain domain)
{
    switch (domain)
    {
        case Domain::Combat:
            return "combat";
        case Domain::Navigation:
            return "navigation";
        case Domain::Quest:
            return "quest";
        case Domain::Gathering:
            return "gathering";
        case Domain::CraftingEconomyItems:
            return "crafting_economy_items";
        case Domain::GroupRoster:
            return "group_roster";
        case Domain::Recovery:
            return "recovery";
        case Domain::PvP:
            return "pvp";
    }
    return "combat";
}

enum class IntentCode : std::uint8_t
{
    CombatAction,
    Navigate,
    QuestObjective,
    GatherSource,
    CraftEconomyItems,
    RosterAssignment,
    Recover,
    PvPObjective,
    Disengage
};

// OperationCode is the execution authority. IntentCode and the diagnostic text fields below are
// retained as policy metadata for existing producers and telemetry, but neither may authorize an
// executor on its own.
enum class OperationCode : std::uint8_t
{
    Unknown = 0,
    CombatAction,
    Navigate,
    QuestAcquire,
    QuestAccept,
    QuestObjective,
    QuestTurnIn,
    GatherRoute,
    GatherSource,
    Craft,
    BankWithdraw,
    BankDeposit,
    MailReceive,
    MailSend,
    VendorBuy,
    VendorSell,
    AuctionBuy,
    AuctionSell,
    RosterAssignment,
    Recover,
    PvPObjective,
    PvPChallenge,
    Disengage
};

inline constexpr std::string_view OperationCodeName(OperationCode operation)
{
    switch (operation)
    {
        case OperationCode::Unknown:
            return "unknown";
        case OperationCode::CombatAction:
            return "combat_action";
        case OperationCode::Navigate:
            return "navigate";
        case OperationCode::QuestAcquire:
            return "quest_acquire";
        case OperationCode::QuestAccept:
            return "quest_accept";
        case OperationCode::QuestObjective:
            return "quest_objective";
        case OperationCode::QuestTurnIn:
            return "quest_turn_in";
        case OperationCode::GatherRoute:
            return "gather_route";
        case OperationCode::GatherSource:
            return "gather_source";
        case OperationCode::Craft:
            return "craft";
        case OperationCode::BankWithdraw:
            return "bank_withdraw";
        case OperationCode::BankDeposit:
            return "bank_deposit";
        case OperationCode::MailReceive:
            return "mail_receive";
        case OperationCode::MailSend:
            return "mail_send";
        case OperationCode::VendorBuy:
            return "vendor_buy";
        case OperationCode::VendorSell:
            return "vendor_sell";
        case OperationCode::AuctionBuy:
            return "auction_buy";
        case OperationCode::AuctionSell:
            return "auction_sell";
        case OperationCode::RosterAssignment:
            return "roster_assignment";
        case OperationCode::Recover:
            return "recover";
        case OperationCode::PvPObjective:
            return "pvp_objective";
        case OperationCode::PvPChallenge:
            return "pvp_challenge";
        case OperationCode::Disengage:
            return "disengage";
    }
    return "unknown";
}

inline constexpr std::string_view IntentCodeName(IntentCode intent)
{
    switch (intent)
    {
        case IntentCode::CombatAction:
            return "combat_action";
        case IntentCode::Navigate:
            return "navigate";
        case IntentCode::QuestObjective:
            return "quest_objective";
        case IntentCode::GatherSource:
            return "gather_source";
        case IntentCode::CraftEconomyItems:
            return "craft_economy_items";
        case IntentCode::RosterAssignment:
            return "roster_assignment";
        case IntentCode::Recover:
            return "recover";
        case IntentCode::PvPObjective:
            return "pvp_objective";
        case IntentCode::Disengage:
            return "disengage";
    }
    return "combat_action";
}

enum class ScopeKind : std::uint8_t
{
    PersistentCampaign,
    LabFixture
};

inline constexpr std::string_view ScopeKindName(ScopeKind kind)
{
    switch (kind)
    {
        case ScopeKind::PersistentCampaign:
            return "persistent_campaign";
        case ScopeKind::LabFixture:
            return "lab_fixture";
    }
    return "persistent_campaign";
}

struct Scope
{
    ScopeKind kind = ScopeKind::PersistentCampaign;
    SquadId squadId = 0;
    Guid botGuid = 0;
};

inline constexpr bool SameScope(Scope const& left, Scope const& right)
{
    return left.kind == right.kind && left.squadId == right.squadId && left.botGuid == right.botGuid;
}

enum class IntentClassification : std::uint8_t
{
    Persistent,
    LabOnly
};

inline constexpr std::string_view IntentClassificationName(IntentClassification classification)
{
    switch (classification)
    {
        case IntentClassification::Persistent:
            return "persistent";
        case IntentClassification::LabOnly:
            return "lab_only";
    }
    return "persistent";
}

// The ordering is part of the contract. Do not replace this with caller-provided priority.
enum class LeaseResource : std::uint8_t
{
    Idle = 0,
    QuestGather = 1,
    Transition = 2,
    CombatPositioning = 3,
    Recovery = 4
};

inline constexpr std::uint8_t ResourcePrecedence(LeaseResource resource)
{
    return static_cast<std::uint8_t>(resource);
}

// Planner scores are wider than the contract priority. A fixed quantum preserves ordering over
// the bounded policy score range while the final clamp makes overflow deterministic.
inline constexpr std::uint32_t kPriorityScoreQuantum = 32;

inline constexpr std::uint16_t PriorityFromScore(std::uint32_t score)
{
    std::uint32_t const quantized = score / kPriorityScoreQuantum;
    std::uint32_t const maximum = std::numeric_limits<std::uint16_t>::max();
    return static_cast<std::uint16_t>(quantized > maximum ? maximum : quantized);
}

inline constexpr std::uint16_t PriorityFromScore(std::int32_t score)
{
    return score <= 0 ? 0 : PriorityFromScore(static_cast<std::uint32_t>(score));
}

inline constexpr std::string_view LeaseResourceName(LeaseResource resource)
{
    switch (resource)
    {
        case LeaseResource::Idle:
            return "idle";
        case LeaseResource::QuestGather:
            return "quest_gather";
        case LeaseResource::Transition:
            return "transition";
        case LeaseResource::CombatPositioning:
            return "combat_positioning";
        case LeaseResource::Recovery:
            return "recovery";
    }
    return "idle";
}

enum class ReceiptStatus : std::uint8_t
{
    Accepted,
    Progressing,
    Completed,
    Blocked,
    Failed,
    Rejected
};

inline constexpr std::string_view ReceiptStatusName(ReceiptStatus status)
{
    switch (status)
    {
        case ReceiptStatus::Accepted:
            return "accepted";
        case ReceiptStatus::Progressing:
            return "progressing";
        case ReceiptStatus::Completed:
            return "completed";
        case ReceiptStatus::Blocked:
            return "blocked";
        case ReceiptStatus::Failed:
            return "failed";
        case ReceiptStatus::Rejected:
            return "rejected";
    }
    return "rejected";
}

enum class ReceiptReason : std::uint8_t
{
    None,
    Planned,
    LeaseAcquired,
    LeaseAlreadyOwned,
    LeaseRenewed,
    LeaseReleased,
    NoEligibleIntent,
    InvalidDecision,
    InvalidScope,
    InvalidFrame,
    StaleFrame,
    StaleEpoch,
    Expired,
    PreconditionFailed,
    LabOnlyPersistentScope,
    BotLeaseConflict,
    SquadOwnerConflict,
    ItemReservationConflict,
    CapacityExceeded,
    NotOwner,
    ExecutorBlocked,
    ExecutorFailed,
    NodeReservationConflict
};

inline constexpr std::string_view ReceiptReasonName(ReceiptReason reason)
{
    switch (reason)
    {
        case ReceiptReason::None:
            return "none";
        case ReceiptReason::Planned:
            return "planned";
        case ReceiptReason::LeaseAcquired:
            return "lease_acquired";
        case ReceiptReason::LeaseAlreadyOwned:
            return "lease_already_owned";
        case ReceiptReason::LeaseRenewed:
            return "lease_renewed";
        case ReceiptReason::LeaseReleased:
            return "lease_released";
        case ReceiptReason::NoEligibleIntent:
            return "no_eligible_intent";
        case ReceiptReason::InvalidDecision:
            return "invalid_decision";
        case ReceiptReason::InvalidScope:
            return "invalid_scope";
        case ReceiptReason::InvalidFrame:
            return "invalid_frame";
        case ReceiptReason::StaleFrame:
            return "stale_frame";
        case ReceiptReason::StaleEpoch:
            return "stale_epoch";
        case ReceiptReason::Expired:
            return "expired";
        case ReceiptReason::PreconditionFailed:
            return "precondition_failed";
        case ReceiptReason::LabOnlyPersistentScope:
            return "lab_only_persistent_scope";
        case ReceiptReason::BotLeaseConflict:
            return "bot_lease_conflict";
        case ReceiptReason::SquadOwnerConflict:
            return "squad_owner_conflict";
        case ReceiptReason::ItemReservationConflict:
            return "item_reservation_conflict";
        case ReceiptReason::CapacityExceeded:
            return "capacity_exceeded";
        case ReceiptReason::NotOwner:
            return "not_owner";
        case ReceiptReason::ExecutorBlocked:
            return "executor_blocked";
        case ReceiptReason::ExecutorFailed:
            return "executor_failed";
        case ReceiptReason::NodeReservationConflict:
            return "node_reservation_conflict";
    }
    return "none";
}

// Domain-neutral counters are copied into every receipt. Executors can interpret the counters
// according to their domain without making the policy depend on a database or a live object.
struct EvidenceCounters
{
    std::uint64_t objective = 0;
    std::uint64_t progress = 0;
    std::uint64_t resource = 0;
    std::uint64_t combat = 0;
    std::uint64_t healing = 0;
    std::uint64_t deaths = 0;
    std::uint64_t pvp = 0;
    std::uint64_t failures = 0;
};

struct BotFacts
{
    Guid guid = 0;
    bool alive = true;
    bool inCombat = false;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
};

// A directed transition proof is optional for backward compatibility. When required is false,
// the candidate is an ordinary same-map action and the remaining fields are ignored. A required
// proof is deliberately typed here rather than encoded in action text so every arbiter path can
// validate the same source/destination identity and safety flags.
struct TransitionPrecondition
{
    bool required = false;
    std::uint64_t stableId = 0;
    std::uint32_t sourceMapId = 0;
    std::uint32_t sourceInstanceId = 0;
    std::uint32_t destinationMapId = 0;
    std::uint32_t destinationInstanceId = 0;
    bool validated = false;
    bool directedProof = false;
    bool cheatBacked = false;
    bool grantBacked = false;
};

// A same-map movement proof is distinct from a cross-map/instance transition. It is populated by
// a policy adapter from a bounded, already-validated route snapshot and is never inferred from
// action text or coordinates.
struct RouteProofPrecondition
{
    bool required = false;
    StableId stableId = 0;
    std::uint32_t mapId = 0;
    bool pathCalculated = false;
    bool pathComplete = false;
    bool usesOnlyLegalMovement = false;
    bool endpointsLegal = false;
};

enum class QuestObjectiveFamily : std::uint8_t
{
    None = 0,
    NpcOrGameObject,
    Item
};

// This reference is deliberately independent of the quest planner's richer ObjectiveKey type.
// The operation carries the phase; the reference carries only the verified quest/objective
// identity needed by an executor.
struct QuestReference
{
    bool valid = false;
    QuestId questId = 0;
    QuestObjectiveFamily objectiveFamily = QuestObjectiveFamily::None;
    std::uint8_t objectiveSlot = 0;
    std::uint32_t requiredEntry = 0;
    ItemId requiredItemId = 0;
};

enum class GatherGoal : std::uint8_t
{
    Unknown = 0,
    HarvestNode,
    ObtainMaterial
};

inline constexpr bool ValidGatherGoal(GatherGoal goal)
{
    return goal == GatherGoal::HarvestNode || goal == GatherGoal::ObtainMaterial;
}

inline constexpr std::string_view GatherGoalName(GatherGoal goal)
{
    switch (goal)
    {
        case GatherGoal::HarvestNode: return "harvest_node";
        case GatherGoal::ObtainMaterial: return "obtain_material";
        case GatherGoal::Unknown: return "unknown";
    }
    return "unknown";
}

struct GatherSourceReference
{
    bool valid = false;
    NodeId spawnId = 0;
    std::uint32_t entry = 0;
    std::uint32_t mapId = 0;
    ItemId materialItemId = 0;
    // Instance identity is part of a live source reservation. Exterior-world sources use zero;
    // dungeon/raid reservations must publish the actual instance.
    std::uint32_t instanceId = 0;
    // Completion semantics are explicit and part of the reserved source identity. HarvestNode
    // accepts canonical node/skill credit; ObtainMaterial requires the requested item delta.
    GatherGoal goal = GatherGoal::Unknown;
};

inline constexpr bool SameGatherSourceReference(GatherSourceReference const& left,
                                                 GatherSourceReference const& right)
{
    return left.valid == right.valid && left.spawnId == right.spawnId && left.entry == right.entry &&
        left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.materialItemId == right.materialItemId && left.goal == right.goal;
}

inline constexpr bool ValidQuestReference(QuestReference const& reference)
{
    return reference.valid && reference.questId != 0;
}

inline constexpr bool ValidGatherRouteReference(GatherSourceReference const& reference)
{
    // Map 0 is a valid Azeroth map; validity is carried explicitly by the publisher.
    return reference.valid && reference.spawnId != 0;
}

inline constexpr bool ValidGatherSourceReference(GatherSourceReference const& reference)
{
    return ValidGatherRouteReference(reference) && reference.entry != 0 &&
        reference.materialItemId != 0 && ValidGatherGoal(reference.goal);
}

inline constexpr bool OperationPayloadValid(OperationCode operation, QuestReference const& quest,
    GatherSourceReference const& gather, ItemId itemId, StableId objectiveId)
{
    switch (operation)
    {
        case OperationCode::Unknown:
            return false;
        case OperationCode::QuestAcquire:
        case OperationCode::QuestAccept:
        case OperationCode::QuestTurnIn:
            return ValidQuestReference(quest);
        case OperationCode::QuestObjective:
            if (!ValidQuestReference(quest) || quest.objectiveFamily == QuestObjectiveFamily::None)
                return false;
            return quest.objectiveFamily == QuestObjectiveFamily::Item ?
                quest.requiredItemId != 0 : quest.requiredEntry != 0;
        case OperationCode::GatherRoute:
            return ValidGatherRouteReference(gather);
        case OperationCode::GatherSource:
            return ValidGatherSourceReference(gather);
        case OperationCode::Craft:
        case OperationCode::BankWithdraw:
        case OperationCode::BankDeposit:
        case OperationCode::MailReceive:
        case OperationCode::MailSend:
        case OperationCode::VendorBuy:
        case OperationCode::VendorSell:
        case OperationCode::AuctionBuy:
        case OperationCode::AuctionSell:
            return itemId != 0;
        case OperationCode::PvPObjective:
        case OperationCode::PvPChallenge:
            return objectiveId != 0;
        case OperationCode::CombatAction:
        case OperationCode::Navigate:
        case OperationCode::RosterAssignment:
        case OperationCode::Recover:
        case OperationCode::Disengage:
            return true;
    }
    return false;
}

inline constexpr bool IsEmptyGatherReference(GatherSourceReference const& gather)
{
    return !gather.valid && gather.spawnId == 0 && gather.entry == 0 && gather.mapId == 0 &&
        gather.materialItemId == 0 && gather.instanceId == 0 && gather.goal == GatherGoal::Unknown;
}

inline constexpr bool IsEmptyQuestReference(QuestReference const& quest)
{
    return !quest.valid && quest.questId == 0 &&
        quest.objectiveFamily == QuestObjectiveFamily::None && quest.objectiveSlot == 0 &&
        quest.requiredEntry == 0 && quest.requiredItemId == 0;
}

// The active executor is deliberately narrower than OperationPayloadValid(). The latter keeps
// every planned operation representable for receipts and contract-only reservation tests. The
// runtime allowlist below describes the one operation whose native executor is currently verified.
inline constexpr bool CurrentQuestObjectivePayloadConsistent(Guid targetGuid, ItemId itemId,
    StableId objectiveId, QuestReference const& quest, GatherSourceReference const& gather)
{
    if (targetGuid == 0 || objectiveId != 0 || !IsEmptyGatherReference(gather) ||
        !ValidQuestReference(quest) || quest.objectiveFamily == QuestObjectiveFamily::None)
        return false;

    switch (quest.objectiveFamily)
    {
        case QuestObjectiveFamily::NpcOrGameObject:
            return quest.requiredEntry != 0 && quest.requiredItemId == 0 && itemId == 0;
        case QuestObjectiveFamily::Item:
            return quest.requiredEntry == 0 && quest.requiredItemId != 0 &&
                itemId == quest.requiredItemId;
        case QuestObjectiveFamily::None:
            return false;
    }
    return false;
}

inline constexpr bool CurrentGatherSourcePayloadConsistent(Guid targetGuid, ItemId itemId,
    StableId objectiveId, QuestReference const& quest, GatherSourceReference const& gather)
{
    // GatherSource is deliberately a separate verified tuple. The target is the published
    // database spawn identity, while the requested material is part of that identity; a runtime
    // GUID, coordinate, or ordinary selector result cannot satisfy this contract.
    return targetGuid != 0 && targetGuid == gather.spawnId && objectiveId == 0 &&
        itemId != 0 && itemId == gather.materialItemId && ValidGatherSourceReference(gather) &&
        IsEmptyQuestReference(quest);
}

struct Preconditions
{
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    Guid botGuid = 0;
    SquadId squadId = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    bool requireBotAlive = true;
    bool requireSameMap = true;
    TransitionPrecondition transition;
    RouteProofPrecondition route;
};

struct OracleCandidate
{
    Domain domain = Domain::Combat;
    IntentCode intent = IntentCode::CombatAction;
    LeaseResource resource = LeaseResource::Idle;
    IntentClassification classification = IntentClassification::Persistent;
    std::uint16_t priority = 0;
    std::uint32_t ttlTicks = 1;
    Guid targetGuid = 0;
    ItemId itemId = 0;
    // The referenced storage must outlive every Decision/IntentLease that carries this action.
    std::string_view action;
    std::string_view qualifier;
    bool available = true;
    // An explicit unsupported executor remains visible to Plan() and produces ExecutorBlocked;
    // it is never silently converted into a successful executable candidate.
    bool executorAvailable = false;
    bool requiresBotAlive = true;
    bool requiresSameMap = true;
    bool exclusiveSquadOwner = false;
    TransitionPrecondition transition;
    RouteProofPrecondition route;
    StableId objectiveId = 0;
    StableId rallyPointId = 0;
    Guid actorGuid = 0;
    Guid ownerGuid = 0;
    RoleCode role = RoleCode::Unknown;
    PlannerModeId plannerMode = kPlannerModeUnknown;
    PlannerReasonId plannerReason = kPlannerReasonNone;
    OperationCode operation = OperationCode::Unknown;
    QuestReference quest;
    GatherSourceReference gather;
};

// Construct this once on the world thread and publish it read-only to Plan() and the arbiter.
// The policy never mutates a frame; candidateCount must be <= kMaxWorldCandidates.
struct WorldReadFrame
{
    FactVersion version = 0;
    Tick tick = 0;
    Epoch epoch = 0;
    Scope scope;
    BotFacts bot;
    EvidenceCounters evidence;
    std::array<OracleCandidate, kMaxWorldCandidates> candidates{};
    std::size_t candidateCount = 0;
    AutoWowShadowCombat::DecisionFrame combat;
    PlannerModeId plannerMode = kPlannerModeUnknown;
    PlannerReasonId plannerReason = kPlannerReasonNone;
};

inline constexpr bool IsVerifiedOperationTuple(bool executorAvailable, Domain domain,
    IntentCode intent, LeaseResource resource, OperationCode operation,
    QuestReference const& quest, GatherSourceReference const& gather, ItemId itemId,
    StableId objectiveId)
{
    // This is the current runtime allowlist. Other operations remain planning-visible, but
    // neither a direct MakeDecision() caller nor an arbiter may turn them into authority. Quest
    // objective and exact GatherSource are the only two deliberately verified operations.
    bool const questObjective = executorAvailable && domain == Domain::Quest &&
        intent == IntentCode::QuestObjective && resource == LeaseResource::QuestGather &&
        operation == OperationCode::QuestObjective &&
        OperationPayloadValid(operation, quest, gather, itemId, objectiveId);
    bool const gatherSource = executorAvailable && domain == Domain::Gathering &&
        intent == IntentCode::GatherSource && resource == LeaseResource::QuestGather &&
        operation == OperationCode::GatherSource &&
        OperationPayloadValid(operation, quest, gather, itemId, objectiveId);
    return questObjective || gatherSource;
}

inline constexpr bool IsCurrentVerifiedOperation(OracleCandidate const& candidate)
{
    return candidate.available && candidate.ttlTicks != 0 &&
        IsVerifiedOperationTuple(candidate.executorAvailable, candidate.domain, candidate.intent,
            candidate.resource, candidate.operation, candidate.quest, candidate.gather,
            candidate.itemId, candidate.objectiveId) &&
        (CurrentQuestObjectivePayloadConsistent(candidate.targetGuid, candidate.itemId,
             candidate.objectiveId, candidate.quest, candidate.gather) ||
         CurrentGatherSourcePayloadConsistent(candidate.targetGuid, candidate.itemId,
             candidate.objectiveId, candidate.quest, candidate.gather));
}

inline bool FrameWithinBounds(WorldReadFrame const& frame)
{
    if (frame.candidateCount > frame.candidates.size() ||
        frame.combat.allies.size() > kMaxCombatAllies ||
        frame.combat.enemies.size() > kMaxCombatEnemies ||
        frame.combat.hazards.size() > kMaxCombatHazards ||
        frame.combat.candidates.size() > kMaxCombatCandidates)
        return false;

    for (std::size_t index = 0; index < frame.candidateCount; ++index)
        if (!RoleCodeWithinBounds(frame.candidates[index].role))
            return false;
    return true;
}

inline bool AddCandidate(WorldReadFrame& frame, OracleCandidate const& candidate)
{
    if (frame.candidateCount >= frame.candidates.size())
        return false;
    frame.candidates[frame.candidateCount++] = candidate;
    return true;
}

struct Decision
{
    bool valid = false;
    DecisionId decisionId = 0;
    IntentId intentId = 0;
    Domain domain = Domain::Combat;
    IntentCode intent = IntentCode::CombatAction;
    LeaseResource resource = LeaseResource::Idle;
    IntentClassification classification = IntentClassification::Persistent;
    Scope scope;
    std::uint16_t priority = 0;
    std::uint32_t ttlTicks = 0;
    Tick issuedTick = 0;
    Tick expiresTick = 0;
    Epoch epoch = 0;
    Guid targetGuid = 0;
    ItemId itemId = 0;
    std::string_view action;
    std::string_view qualifier;
    bool exclusiveSquadOwner = false;
    Preconditions preconditions;
    EvidenceCounters evidence;
    StableId objectiveId = 0;
    StableId rallyPointId = 0;
    Guid actorGuid = 0;
    Guid ownerGuid = 0;
    RoleCode role = RoleCode::Unknown;
    PlannerModeId plannerMode = kPlannerModeUnknown;
    PlannerReasonId plannerReason = kPlannerReasonNone;
    bool executorAvailable = true;
    OperationCode operation = OperationCode::Unknown;
    QuestReference quest;
    GatherSourceReference gather;
};

inline constexpr bool IsCurrentVerifiedOperation(Decision const& decision)
{
    return decision.valid && decision.ttlTicks != 0 &&
        IsVerifiedOperationTuple(decision.executorAvailable, decision.domain, decision.intent,
            decision.resource, decision.operation, decision.quest, decision.gather,
            decision.itemId, decision.objectiveId) &&
        (CurrentQuestObjectivePayloadConsistent(decision.targetGuid, decision.itemId,
             decision.objectiveId, decision.quest, decision.gather) ||
         CurrentGatherSourcePayloadConsistent(decision.targetGuid, decision.itemId,
             decision.objectiveId, decision.quest, decision.gather));
}

struct IntentLease
{
    bool valid = false;
    Decision decision;
};

struct Receipt
{
    ReceiptStatus status = ReceiptStatus::Rejected;
    ReceiptReason reason = ReceiptReason::None;
    DecisionId decisionId = 0;
    IntentId intentId = 0;
    Domain domain = Domain::Combat;
    LeaseResource resource = LeaseResource::Idle;
    Scope scope;
    FactVersion frameVersion = 0;
    Epoch epoch = 0;
    EvidenceCounters before;
    EvidenceCounters after;
    StableId objectiveId = 0;
    StableId rallyPointId = 0;
    Guid actorGuid = 0;
    Guid ownerGuid = 0;
    // Receipt identity is copied from the selected decision. Keeping the exact target and item
    // on the receipt avoids forcing telemetry consumers to infer them from action text or from a
    // later live-world read.
    Guid targetGuid = 0;
    ItemId itemId = 0;
    RoleCode role = RoleCode::Unknown;
    PlannerModeId plannerMode = kPlannerModeUnknown;
    PlannerReasonId plannerReason = kPlannerReasonNone;
    bool executorAvailable = true;
    OperationCode operation = OperationCode::Unknown;
    QuestReference quest;
    GatherSourceReference gather;
};

struct PlanResult
{
    bool hasDecision = false;
    Decision decision;
    Receipt receipt;
};

struct LeaseResult
{
    bool hasLease = false;
    IntentLease lease;
    Receipt receipt;
};

// Read-only ownership evidence exposed to adapters. A snapshot is intentionally bounded and
// contains enough identity to reject a preempted but not-yet-expired lease.
struct ActiveLeaseSnapshot
{
    bool active = false;
    Guid botGuid = 0;
    DecisionId decisionId = 0;
    IntentId intentId = 0;
    Epoch epoch = 0;
    Tick expiresTick = 0;
    Scope scope;
    OperationCode operation = OperationCode::Unknown;
};

namespace Detail
{
enum class ValidationMode : std::uint8_t
{
    RuntimeAuthority,
    ContractReservationTest
};

inline constexpr bool IsContractReservationOperation(Decision const& decision)
{
    if (!decision.executorAvailable || !OperationPayloadValid(decision.operation, decision.quest,
            decision.gather, decision.itemId, decision.objectiveId))
        return false;

    switch (decision.operation)
    {
        case OperationCode::GatherSource:
            return decision.domain == Domain::Gathering &&
                decision.intent == IntentCode::GatherSource &&
                decision.resource == LeaseResource::QuestGather;
        case OperationCode::GatherRoute:
            return decision.domain == Domain::Navigation &&
                decision.intent == IntentCode::Navigate &&
                decision.resource == LeaseResource::Transition;
        default:
            return false;
    }
}

inline constexpr bool IsOperationAllowed(Decision const& decision, ValidationMode mode)
{
    return mode == ValidationMode::RuntimeAuthority ? IsCurrentVerifiedOperation(decision) :
                                                       IsContractReservationOperation(decision);
}

inline constexpr std::uint64_t kDecisionIdDomain = 0x41574f4445434953ULL;
inline constexpr std::uint64_t kIntentIdDomain = 0x41574f494e54454eULL;

inline constexpr std::uint64_t HashText(std::string_view text)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (char value : text)
    {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

inline constexpr std::uint64_t ScopeKey(Scope const& scope)
{
    std::uint64_t key = static_cast<std::uint64_t>(scope.kind);
    key = AutoWowShadowCombat::DeterministicHash(key, scope.squadId, scope.botGuid);
    return key;
}

inline constexpr std::uint64_t TransitionKey(TransitionPrecondition const& transition)
{
    std::uint64_t key = static_cast<std::uint64_t>(transition.required);
    key = AutoWowShadowCombat::DeterministicHash(key, transition.stableId,
        transition.sourceMapId);
    key = AutoWowShadowCombat::DeterministicHash(key, transition.sourceInstanceId,
        transition.destinationMapId);
    key = AutoWowShadowCombat::DeterministicHash(key, transition.destinationInstanceId,
        static_cast<std::uint64_t>(transition.validated));
    key = AutoWowShadowCombat::DeterministicHash(key, static_cast<std::uint64_t>(transition.directedProof),
        static_cast<std::uint64_t>(transition.cheatBacked));
    return AutoWowShadowCombat::DeterministicHash(key,
        static_cast<std::uint64_t>(transition.grantBacked), 0);
}

inline constexpr std::uint64_t RouteProofKey(RouteProofPrecondition const& route)
{
    std::uint64_t key = static_cast<std::uint64_t>(route.required);
    key = AutoWowShadowCombat::DeterministicHash(key, route.stableId, route.mapId);
    key = AutoWowShadowCombat::DeterministicHash(key,
        static_cast<std::uint64_t>(route.pathCalculated),
        static_cast<std::uint64_t>(route.pathComplete));
    return AutoWowShadowCombat::DeterministicHash(key,
        static_cast<std::uint64_t>(route.usesOnlyLegalMovement),
        static_cast<std::uint64_t>(route.endpointsLegal));
}

inline constexpr std::uint64_t QuestReferenceKey(QuestReference const& quest)
{
    std::uint64_t key = static_cast<std::uint64_t>(quest.valid);
    key = AutoWowShadowCombat::DeterministicHash(key, quest.questId,
        static_cast<std::uint64_t>(quest.objectiveFamily));
    key = AutoWowShadowCombat::DeterministicHash(key, quest.objectiveSlot, quest.requiredEntry);
    return AutoWowShadowCombat::DeterministicHash(key, quest.requiredItemId, 0);
}

inline constexpr std::uint64_t GatherSourceReferenceKey(GatherSourceReference const& gather)
{
    std::uint64_t key = static_cast<std::uint64_t>(gather.valid);
    key = AutoWowShadowCombat::DeterministicHash(key, gather.spawnId, gather.entry);
    key = AutoWowShadowCombat::DeterministicHash(key, gather.mapId, gather.instanceId);
    return AutoWowShadowCombat::DeterministicHash(key, gather.materialItemId,
        static_cast<std::uint64_t>(gather.goal));
}

inline constexpr std::uint64_t OperationKey(OperationCode operation,
    QuestReference const& quest, GatherSourceReference const& gather)
{
    std::uint64_t key = static_cast<std::uint64_t>(operation);
    key = AutoWowShadowCombat::DeterministicHash(key, QuestReferenceKey(quest),
        GatherSourceReferenceKey(gather));
    return key;
}

inline constexpr std::uint64_t CandidateKey(OracleCandidate const& candidate)
{
    std::uint64_t key = static_cast<std::uint64_t>(candidate.domain);
    key = AutoWowShadowCombat::DeterministicHash(key,
        static_cast<std::uint64_t>(candidate.intent), candidate.targetGuid);
    key = AutoWowShadowCombat::DeterministicHash(key, candidate.itemId,
        OperationKey(candidate.operation, candidate.quest, candidate.gather));
    key = AutoWowShadowCombat::DeterministicHash(key,
        static_cast<std::uint64_t>(candidate.executorAvailable), candidate.objectiveId);
    if (candidate.transition.required)
        key = AutoWowShadowCombat::DeterministicHash(key, TransitionKey(candidate.transition), 0);
    if (candidate.route.required)
        key = AutoWowShadowCombat::DeterministicHash(key, RouteProofKey(candidate.route), 0);
    return key;
}

inline constexpr std::uint64_t NonZero(std::uint64_t value)
{
    return value == 0 ? 1 : value;
}

inline constexpr Tick SaturatingAdd(Tick left, std::uint64_t right)
{
    constexpr Tick maximum = std::numeric_limits<Tick>::max();
    return right > maximum - left ? maximum : left + right;
}

inline constexpr bool CandidateLess(OracleCandidate const& left, OracleCandidate const& right)
{
    if (ResourcePrecedence(left.resource) != ResourcePrecedence(right.resource))
        return ResourcePrecedence(left.resource) > ResourcePrecedence(right.resource);
    if (left.priority != right.priority)
        return left.priority > right.priority;
    if (left.domain != right.domain)
        return static_cast<std::uint8_t>(left.domain) < static_cast<std::uint8_t>(right.domain);
    if (left.intent != right.intent)
        return static_cast<std::uint8_t>(left.intent) < static_cast<std::uint8_t>(right.intent);
    if (left.targetGuid != right.targetGuid)
        return left.targetGuid < right.targetGuid;
    if (left.itemId != right.itemId)
        return left.itemId < right.itemId;
    if (left.operation != right.operation)
        return static_cast<std::uint8_t>(left.operation) <
            static_cast<std::uint8_t>(right.operation);
    if (left.executorAvailable != right.executorAvailable)
        return left.executorAvailable < right.executorAvailable;
    if (QuestReferenceKey(left.quest) != QuestReferenceKey(right.quest))
        return QuestReferenceKey(left.quest) < QuestReferenceKey(right.quest);
    if (GatherSourceReferenceKey(left.gather) != GatherSourceReferenceKey(right.gather))
        return GatherSourceReferenceKey(left.gather) < GatherSourceReferenceKey(right.gather);
    if (left.objectiveId != right.objectiveId)
        return left.objectiveId < right.objectiveId;
    if (left.rallyPointId != right.rallyPointId)
        return left.rallyPointId < right.rallyPointId;
    if (left.actorGuid != right.actorGuid)
        return left.actorGuid < right.actorGuid;
    if (left.ownerGuid != right.ownerGuid)
        return left.ownerGuid < right.ownerGuid;
    if (left.role != right.role)
        return static_cast<std::uint8_t>(left.role) < static_cast<std::uint8_t>(right.role);
    if (left.plannerMode != right.plannerMode)
        return left.plannerMode < right.plannerMode;
    if (left.plannerReason != right.plannerReason)
        return left.plannerReason < right.plannerReason;
    if (left.classification != right.classification)
        return static_cast<std::uint8_t>(left.classification) <
            static_cast<std::uint8_t>(right.classification);
    if (left.ttlTicks != right.ttlTicks)
        return left.ttlTicks < right.ttlTicks;
    if (left.requiresBotAlive != right.requiresBotAlive)
        return left.requiresBotAlive < right.requiresBotAlive;
    if (left.requiresSameMap != right.requiresSameMap)
        return left.requiresSameMap < right.requiresSameMap;
    if (left.exclusiveSquadOwner != right.exclusiveSquadOwner)
        return left.exclusiveSquadOwner < right.exclusiveSquadOwner;
    auto const leftTransition = std::tuple(left.transition.required, left.transition.stableId,
        left.transition.sourceMapId, left.transition.sourceInstanceId,
        left.transition.destinationMapId, left.transition.destinationInstanceId,
        left.transition.validated, left.transition.directedProof, left.transition.cheatBacked,
        left.transition.grantBacked);
    auto const rightTransition = std::tuple(right.transition.required, right.transition.stableId,
        right.transition.sourceMapId, right.transition.sourceInstanceId,
        right.transition.destinationMapId, right.transition.destinationInstanceId,
        right.transition.validated, right.transition.directedProof, right.transition.cheatBacked,
        right.transition.grantBacked);
    if (leftTransition != rightTransition)
        return leftTransition < rightTransition;
    auto const leftRoute = std::tuple(left.route.required, left.route.stableId, left.route.mapId,
        left.route.pathCalculated, left.route.pathComplete, left.route.usesOnlyLegalMovement,
        left.route.endpointsLegal);
    auto const rightRoute = std::tuple(right.route.required, right.route.stableId, right.route.mapId,
        right.route.pathCalculated, right.route.pathComplete, right.route.usesOnlyLegalMovement,
        right.route.endpointsLegal);
    return leftRoute < rightRoute;
}

inline constexpr Receipt MakeReceipt(
    Decision const& decision, ReceiptStatus status, ReceiptReason reason, FactVersion frameVersion,
    EvidenceCounters const& before, EvidenceCounters const& after)
{
    Receipt receipt;
    receipt.status = status;
    receipt.reason = reason;
    receipt.decisionId = decision.decisionId;
    receipt.intentId = decision.intentId;
    receipt.domain = decision.domain;
    receipt.resource = decision.resource;
    receipt.scope = decision.scope;
    receipt.frameVersion = frameVersion;
    receipt.epoch = decision.epoch;
    receipt.before = before;
    receipt.after = after;
    receipt.objectiveId = decision.objectiveId;
    receipt.rallyPointId = decision.rallyPointId;
    receipt.actorGuid = decision.actorGuid;
    receipt.ownerGuid = decision.ownerGuid;
    receipt.targetGuid = decision.targetGuid;
    receipt.itemId = decision.itemId;
    receipt.role = decision.role;
    receipt.plannerMode = decision.plannerMode;
    receipt.plannerReason = decision.plannerReason;
    receipt.executorAvailable = decision.executorAvailable;
    receipt.operation = decision.operation;
    receipt.quest = decision.quest;
    receipt.gather = decision.gather;
    return receipt;
}

inline constexpr Receipt MakeFrameReceipt(
    WorldReadFrame const& frame, ReceiptStatus status, ReceiptReason reason)
{
    Receipt receipt;
    receipt.status = status;
    receipt.reason = reason;
    receipt.scope = frame.scope;
    receipt.frameVersion = frame.version;
    receipt.epoch = frame.epoch;
    receipt.before = frame.evidence;
    receipt.after = frame.evidence;
    receipt.plannerMode = frame.plannerMode;
    receipt.plannerReason = frame.plannerReason;
    return receipt;
}

inline constexpr bool ValidTransitionPrecondition(
    WorldReadFrame const& frame, Preconditions const& preconditions)
{
    TransitionPrecondition const& transition = preconditions.transition;
    if (!preconditions.requireSameMap && !transition.required)
        return false;
    if (!transition.required)
        return true;
    if (preconditions.requireSameMap || transition.stableId == 0 || !transition.validated ||
        !transition.directedProof || transition.cheatBacked || transition.grantBacked)
        return false;
    if (transition.sourceMapId != frame.bot.mapId ||
        transition.sourceInstanceId != frame.bot.instanceId)
        return false;
    if (transition.sourceMapId == transition.destinationMapId &&
        transition.sourceInstanceId == transition.destinationInstanceId)
        return false;
    return true;
}

inline constexpr bool ValidRouteProofPrecondition(
    WorldReadFrame const& frame, Preconditions const& preconditions)
{
    RouteProofPrecondition const& route = preconditions.route;
    if (!route.required)
        return true;
    if (!preconditions.requireSameMap || route.stableId == 0 ||
        route.mapId != frame.bot.mapId || route.mapId != preconditions.mapId ||
        !route.pathCalculated || !route.pathComplete || !route.usesOnlyLegalMovement ||
        !route.endpointsLegal)
        return false;
    return true;
}

inline constexpr bool ValidTypedOperationPrecondition(WorldReadFrame const& frame,
    Decision const& decision)
{
    if (!decision.preconditions.requireSameMap)
        return true;
    switch (decision.operation)
    {
        case OperationCode::GatherRoute:
        case OperationCode::GatherSource:
            // A source reference is not a movable hint: its map is part of the
            // executor identity and must agree with the current frame.
            return decision.gather.mapId == frame.bot.mapId;
        default:
            return true;
    }
}

inline constexpr ReceiptReason ValidateDecision(WorldReadFrame const& frame, Decision const& decision,
    ValidationMode mode = ValidationMode::RuntimeAuthority)
{
    if (!decision.valid || decision.decisionId == 0 || decision.intentId == 0)
        return ReceiptReason::InvalidDecision;
    if (!IsOperationAllowed(decision, mode))
        return ReceiptReason::ExecutorBlocked;
    if (!SameScope(frame.scope, decision.scope) || frame.bot.guid != decision.scope.botGuid)
        return ReceiptReason::InvalidScope;
    if (decision.actorGuid != 0 && decision.actorGuid != frame.bot.guid)
        return ReceiptReason::PreconditionFailed;
    if (decision.classification == IntentClassification::LabOnly &&
        decision.scope.kind == ScopeKind::PersistentCampaign)
        return ReceiptReason::LabOnlyPersistentScope;
    if (decision.preconditions.frameVersion != frame.version)
        return ReceiptReason::StaleFrame;
    if (decision.preconditions.epoch != frame.epoch || decision.epoch != frame.epoch)
        return ReceiptReason::StaleEpoch;
    if (frame.tick >= decision.expiresTick)
        return ReceiptReason::Expired;
    if (decision.preconditions.botGuid != frame.bot.guid ||
        decision.preconditions.squadId != frame.scope.squadId)
        return ReceiptReason::PreconditionFailed;
    if (decision.preconditions.requireBotAlive && !frame.bot.alive)
        return ReceiptReason::PreconditionFailed;
    if (decision.preconditions.requireSameMap &&
        (decision.preconditions.mapId != frame.bot.mapId ||
            decision.preconditions.instanceId != frame.bot.instanceId))
        return ReceiptReason::PreconditionFailed;
    if (!ValidTypedOperationPrecondition(frame, decision))
        return ReceiptReason::PreconditionFailed;
    if (!ValidTransitionPrecondition(frame, decision.preconditions))
        return ReceiptReason::PreconditionFailed;
    if (!ValidRouteProofPrecondition(frame, decision.preconditions))
        return ReceiptReason::PreconditionFailed;
    return ReceiptReason::None;
}

inline constexpr ReceiptReason ValidateRenewFrame(WorldReadFrame const& frame,
    Decision const& decision, ValidationMode mode = ValidationMode::RuntimeAuthority)
{
    if (!decision.valid || decision.decisionId == 0 || decision.intentId == 0)
        return ReceiptReason::InvalidDecision;
    if (!IsOperationAllowed(decision, mode))
        return ReceiptReason::ExecutorBlocked;
    if (!SameScope(frame.scope, decision.scope) || frame.bot.guid != decision.scope.botGuid)
        return ReceiptReason::InvalidScope;
    if (decision.actorGuid != 0 && decision.actorGuid != frame.bot.guid)
        return ReceiptReason::PreconditionFailed;
    if (decision.classification == IntentClassification::LabOnly &&
        decision.scope.kind == ScopeKind::PersistentCampaign)
        return ReceiptReason::LabOnlyPersistentScope;
    if (frame.epoch != decision.epoch)
        return ReceiptReason::StaleEpoch;
    if (frame.version < decision.preconditions.frameVersion)
        return ReceiptReason::StaleFrame;
    if (decision.preconditions.botGuid != frame.bot.guid ||
        decision.preconditions.squadId != frame.scope.squadId)
        return ReceiptReason::PreconditionFailed;
    if (decision.preconditions.requireBotAlive && !frame.bot.alive)
        return ReceiptReason::PreconditionFailed;
    if (decision.preconditions.requireSameMap &&
        (decision.preconditions.mapId != frame.bot.mapId ||
            decision.preconditions.instanceId != frame.bot.instanceId))
        return ReceiptReason::PreconditionFailed;
    if (!ValidTypedOperationPrecondition(frame, decision))
        return ReceiptReason::PreconditionFailed;
    if (!ValidTransitionPrecondition(frame, decision.preconditions))
        return ReceiptReason::PreconditionFailed;
    if (!ValidRouteProofPrecondition(frame, decision.preconditions))
        return ReceiptReason::PreconditionFailed;
    return ReceiptReason::None;
}

inline constexpr Decision MakeDecision(
    WorldReadFrame const& frame, OracleCandidate const& candidate)
{
    Decision decision;
    if (!candidate.available || candidate.ttlTicks == 0)
        return decision;

    std::uint64_t candidateKey = CandidateKey(candidate);
    if (candidate.objectiveId != 0 || candidate.rallyPointId != 0 || candidate.actorGuid != 0 ||
        candidate.ownerGuid != 0 || candidate.role != RoleCode::Unknown ||
        candidate.plannerMode != kPlannerModeUnknown || candidate.plannerReason != kPlannerReasonNone)
    {
        std::uint64_t metadataKey = AutoWowShadowCombat::DeterministicHash(
            candidate.objectiveId, candidate.rallyPointId, candidate.actorGuid);
        metadataKey = AutoWowShadowCombat::DeterministicHash(
            metadataKey, candidate.ownerGuid, static_cast<std::uint64_t>(candidate.role));
        metadataKey = AutoWowShadowCombat::DeterministicHash(
            metadataKey, candidate.plannerMode, candidate.plannerReason);
        candidateKey = AutoWowShadowCombat::DeterministicHash(candidateKey, metadataKey,
            frame.version);
    }
    std::uint64_t const scopeKey = ScopeKey(frame.scope);
    decision.valid = true;
    decision.domain = candidate.domain;
    decision.intent = candidate.intent;
    decision.resource = candidate.resource;
    decision.classification = candidate.classification;
    decision.scope = frame.scope;
    decision.priority = candidate.priority;
    decision.ttlTicks = candidate.ttlTicks;
    decision.issuedTick = frame.tick;
    decision.expiresTick = SaturatingAdd(frame.tick, candidate.ttlTicks);
    decision.epoch = frame.epoch;
    decision.targetGuid = candidate.targetGuid;
    decision.itemId = candidate.itemId;
    decision.action = candidate.action;
    decision.qualifier = candidate.qualifier;
    decision.exclusiveSquadOwner = candidate.exclusiveSquadOwner;
    decision.objectiveId = candidate.objectiveId;
    decision.rallyPointId = candidate.rallyPointId;
    decision.actorGuid = candidate.actorGuid == 0 ? frame.bot.guid : candidate.actorGuid;
    decision.ownerGuid = candidate.ownerGuid;
    decision.role = candidate.role;
    decision.plannerMode = candidate.plannerMode == kPlannerModeUnknown ? frame.plannerMode :
        candidate.plannerMode;
    decision.plannerReason = candidate.plannerReason == kPlannerReasonNone ? frame.plannerReason :
        candidate.plannerReason;
    decision.executorAvailable = candidate.executorAvailable;
    decision.operation = candidate.operation;
    decision.quest = candidate.quest;
    decision.gather = candidate.gather;
    decision.preconditions.frameVersion = frame.version;
    decision.preconditions.epoch = frame.epoch;
    decision.preconditions.botGuid = frame.bot.guid;
    decision.preconditions.squadId = frame.scope.squadId;
    decision.preconditions.mapId = frame.bot.mapId;
    decision.preconditions.instanceId = frame.bot.instanceId;
    decision.preconditions.requireBotAlive = candidate.requiresBotAlive;
    decision.preconditions.requireSameMap = candidate.requiresSameMap;
    decision.preconditions.transition = candidate.transition;
    decision.preconditions.route = candidate.route;
    decision.evidence = frame.evidence;
    decision.decisionId = NonZero(AutoWowShadowCombat::DeterministicHash(
        scopeKey ^ frame.tick, kDecisionIdDomain ^ frame.epoch, candidateKey));
    decision.intentId = NonZero(AutoWowShadowCombat::DeterministicHash(
        decision.decisionId, kIntentIdDomain,
        OperationKey(decision.operation, decision.quest, decision.gather)));
    return decision;
}
}

inline Decision MakeDecision(WorldReadFrame const& frame, OracleCandidate const& candidate)
{
    return Detail::MakeDecision(frame, candidate);
}

inline PlanResult Plan(WorldReadFrame const& frame)
{
    PlanResult result;
    if (!FrameWithinBounds(frame))
    {
        result.receipt = Detail::MakeFrameReceipt(frame, ReceiptStatus::Rejected,
            ReceiptReason::InvalidFrame);
        return result;
    }

    OracleCandidate selected;
    bool hasSelected = false;

    // A recovery candidate is considered before combat, and the remaining candidates are ordered
    // by the hard resource precedence. This is the shared policy boundary for every domain.
    for (std::size_t index = 0; index < frame.candidateCount; ++index)
    {
        OracleCandidate const& candidate = frame.candidates[index];
        if (!candidate.available || candidate.ttlTicks == 0)
            continue;
        if (!hasSelected || Detail::CandidateLess(candidate, selected))
        {
            selected = candidate;
            hasSelected = true;
        }
    }

    if (!hasSelected)
    {
        result.receipt = Detail::MakeFrameReceipt(frame, ReceiptStatus::Blocked,
            ReceiptReason::NoEligibleIntent);
        return result;
    }

    if (selected.resource == LeaseResource::CombatPositioning && selected.domain == Domain::Combat)
    {
        // Combat scoring and experience projection remain exclusively in ShadowCombatOraclePolicy.
        AutoWowShadowCombat::OracleChoice const choice = AutoWowShadowCombat::ChooseOracle(frame.combat);
        if (choice.hasChoice)
        {
            selected.intent = IntentCode::CombatAction;
            selected.targetGuid = choice.targetGuid;
            selected.action = choice.actionName;
            selected.ttlTicks = selected.ttlTicks == 0 ? 1 : selected.ttlTicks;
        }
    }

    if (!IsCurrentVerifiedOperation(selected))
    {
        Decision const blockedDecision = Detail::MakeDecision(frame, selected);
        if (blockedDecision.valid)
            result.receipt = Detail::MakeReceipt(blockedDecision, ReceiptStatus::Blocked,
                ReceiptReason::ExecutorBlocked, frame.version, frame.evidence, frame.evidence);
        else
        {
            result.receipt = Detail::MakeFrameReceipt(frame, ReceiptStatus::Blocked,
                ReceiptReason::ExecutorBlocked);
            result.receipt.domain = selected.domain;
            result.receipt.resource = selected.resource;
            result.receipt.operation = selected.operation;
            result.receipt.quest = selected.quest;
            result.receipt.gather = selected.gather;
        }
        return result;
    }

    result.decision = Detail::MakeDecision(frame, selected);
    if (!result.decision.valid)
    {
        result.receipt = Detail::MakeFrameReceipt(frame, ReceiptStatus::Blocked,
            ReceiptReason::InvalidDecision);
        return result;
    }
    result.hasDecision = true;
    result.receipt = Detail::MakeReceipt(result.decision, ReceiptStatus::Accepted,
        ReceiptReason::Planned, frame.version, frame.evidence, frame.evidence);
    return result;
}

// Defined only by the contract unit test. The production arbiter has no public path for the
// contract-only reservation mode; this friend is deliberately a test-only access seam.
struct OracleArbiterContractTestAccess;

template <std::size_t MaxBots = kMaxBotLeases,
    std::size_t MaxSquads = kMaxSquadOwners,
    std::size_t MaxItems = kMaxItemReservations,
    std::size_t MaxNodes = kMaxNodeReservations>
class OracleArbiter
{
    friend struct OracleArbiterContractTestAccess;

    static_assert(MaxBots <= kMaxBotLeases);
    static_assert(MaxSquads <= kMaxSquadOwners);
    static_assert(MaxItems <= kMaxItemReservations);
    static_assert(MaxNodes <= kMaxNodeReservations);

    struct BotLeaseSlot
    {
        bool occupied = false;
        Guid botGuid = 0;
        IntentLease lease;
    };

    struct SquadOwnerSlot
    {
        bool occupied = false;
        SquadId squadId = 0;
        Guid ownerBotGuid = 0;
        DecisionId decisionId = 0;
        Epoch epoch = 0;
        Tick expiresTick = 0;
        ScopeKind scopeKind = ScopeKind::PersistentCampaign;
    };

    struct ItemReservationSlot
    {
        bool occupied = false;
        ItemId itemId = 0;
        Scope scope;
        DecisionId decisionId = 0;
        IntentId intentId = 0;
        Tick expiresTick = 0;
        Epoch epoch = 0;
    };

    struct NodeReservationSlot
    {
        bool occupied = false;
        // A spawn counter is only unique inside one map/instance and does not identify the
        // requested output. Retain the complete published source identity for arbitration.
        GatherSourceReference gather;
        Scope scope;
        DecisionId decisionId = 0;
        IntentId intentId = 0;
        Tick expiresTick = 0;
        Epoch epoch = 0;
    };

    std::array<BotLeaseSlot, MaxBots> botLeases{};
    std::array<SquadOwnerSlot, MaxSquads> squadOwners{};
    std::array<ItemReservationSlot, MaxItems> itemReservations{};
    std::array<NodeReservationSlot, MaxNodes> nodeReservations{};

    static bool Expired(Tick now, Tick expiresTick)
    {
        return now >= expiresTick;
    }

    void ReapExpired(Tick now)
    {
        for (std::size_t index = 0; index < botLeases.size(); ++index)
        {
            if (botLeases[index].occupied &&
                Expired(now, botLeases[index].lease.decision.expiresTick))
                ClearBotLease(index);
        }
        for (SquadOwnerSlot& slot : squadOwners)
            if (slot.occupied && Expired(now, slot.expiresTick))
                slot = {};
        for (ItemReservationSlot& slot : itemReservations)
            if (slot.occupied && Expired(now, slot.expiresTick))
                slot = {};
        for (NodeReservationSlot& slot : nodeReservations)
            if (slot.occupied && Expired(now, slot.expiresTick))
                slot = {};
    }

    void ClearSquadOwner(SquadId squadId, DecisionId decisionId)
    {
        for (SquadOwnerSlot& slot : squadOwners)
        {
            if (slot.occupied && slot.squadId == squadId && slot.decisionId == decisionId)
            {
                slot = {};
                return;
            }
        }
    }

    void ClearNodeReservations(Decision const& decision)
    {
        for (NodeReservationSlot& slot : nodeReservations)
        {
            if (slot.occupied && SameScope(slot.scope, decision.scope) &&
                slot.decisionId == decision.decisionId && slot.intentId == decision.intentId &&
                slot.epoch == decision.epoch)
                slot = {};
        }
    }

    void RenewNodeReservations(Decision const& previous, Decision const& renewed)
    {
        for (NodeReservationSlot& slot : nodeReservations)
        {
            if (!slot.occupied || !SameScope(slot.scope, previous.scope) ||
                slot.decisionId != previous.decisionId || slot.intentId != previous.intentId ||
                slot.epoch != previous.epoch)
                continue;
            slot.scope = renewed.scope;
            slot.expiresTick = renewed.expiresTick;
            slot.epoch = renewed.epoch;
        }
    }

    void ClearBotLease(std::size_t index)
    {
        BotLeaseSlot& slot = botLeases[index];
        if (slot.occupied && slot.lease.valid)
        {
            if (slot.lease.decision.exclusiveSquadOwner)
                ClearSquadOwner(slot.lease.decision.scope.squadId,
                    slot.lease.decision.decisionId);
            ClearNodeReservations(slot.lease.decision);
        }
        slot = {};
    }

    std::size_t FindBot(Guid botGuid) const
    {
        for (std::size_t index = 0; index < botLeases.size(); ++index)
            if (botLeases[index].occupied && botLeases[index].botGuid == botGuid)
                return index;
        return botLeases.size();
    }

    std::size_t FindFreeBot() const
    {
        for (std::size_t index = 0; index < botLeases.size(); ++index)
            if (!botLeases[index].occupied)
                return index;
        return botLeases.size();
    }

    std::size_t FindSquad(SquadId squadId) const
    {
        for (std::size_t index = 0; index < squadOwners.size(); ++index)
            if (squadOwners[index].occupied && squadOwners[index].squadId == squadId)
                return index;
        return squadOwners.size();
    }

    std::size_t FindFreeSquad() const
    {
        for (std::size_t index = 0; index < squadOwners.size(); ++index)
            if (!squadOwners[index].occupied)
                return index;
        return squadOwners.size();
    }

    std::size_t FindItem(ItemId itemId) const
    {
        for (std::size_t index = 0; index < itemReservations.size(); ++index)
            if (itemReservations[index].occupied && itemReservations[index].itemId == itemId)
                return index;
        return itemReservations.size();
    }

    std::size_t FindFreeItem() const
    {
        for (std::size_t index = 0; index < itemReservations.size(); ++index)
            if (!itemReservations[index].occupied)
                return index;
        return itemReservations.size();
    }

    std::size_t FindNode(GatherSourceReference const& gather) const
    {
        for (std::size_t index = 0; index < nodeReservations.size(); ++index)
            if (nodeReservations[index].occupied &&
                SameGatherSourceReference(nodeReservations[index].gather, gather))
                return index;
        return nodeReservations.size();
    }

    std::size_t FindFreeNode() const
    {
        for (std::size_t index = 0; index < nodeReservations.size(); ++index)
            if (!nodeReservations[index].occupied)
                return index;
        return nodeReservations.size();
    }

    static bool CanPreempt(Decision const& incoming, Decision const& current)
    {
        if (incoming.epoch < current.epoch)
            return false;
        if (ResourcePrecedence(incoming.resource) != ResourcePrecedence(current.resource))
            return ResourcePrecedence(incoming.resource) > ResourcePrecedence(current.resource);
        if (incoming.priority != current.priority)
            return incoming.priority > current.priority;
        return incoming.decisionId < current.decisionId;
    }

    Receipt ClaimSquad(WorldReadFrame const& frame, Decision const& decision)
    {
        if (!decision.exclusiveSquadOwner)
            return {};
        if (decision.scope.squadId == 0 || decision.scope.botGuid == 0)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected, ReceiptReason::InvalidScope,
                frame.version, frame.evidence, frame.evidence);

        std::size_t index = FindSquad(decision.scope.squadId);
        if (index == squadOwners.size())
        {
            index = FindFreeSquad();
            if (index == squadOwners.size())
                return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                    ReceiptReason::CapacityExceeded, frame.version, frame.evidence, frame.evidence);
            squadOwners[index] = {true, decision.scope.squadId, decision.scope.botGuid,
                decision.decisionId, decision.epoch, decision.expiresTick, decision.scope.kind};
            return {};
        }

        SquadOwnerSlot& owner = squadOwners[index];
        if (Expired(frame.tick, owner.expiresTick))
        {
            owner = {true, decision.scope.squadId, decision.scope.botGuid,
                decision.decisionId, decision.epoch, decision.expiresTick, decision.scope.kind};
            return {};
        }
        if (owner.ownerBotGuid == decision.scope.botGuid && owner.decisionId == decision.decisionId)
            return {};
        if (owner.ownerBotGuid == decision.scope.botGuid && decision.epoch > owner.epoch)
        {
            owner = {true, decision.scope.squadId, decision.scope.botGuid,
                decision.decisionId, decision.epoch, decision.expiresTick, decision.scope.kind};
            return {};
        }
        return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
            ReceiptReason::SquadOwnerConflict, frame.version, frame.evidence, frame.evidence);
    }

    void RollbackSquadClaim(Decision const& decision)
    {
        if (decision.exclusiveSquadOwner)
            ClearSquadOwner(decision.scope.squadId, decision.decisionId);
    }

    static Receipt WithFrameEvidence(Receipt receipt, WorldReadFrame const& frame)
    {
        receipt.frameVersion = frame.version;
        receipt.epoch = frame.epoch;
        receipt.after = frame.evidence;
        return receipt;
    }

    Receipt ReserveNodeImpl(WorldReadFrame const& frame, IntentLease const& lease,
        Detail::ValidationMode mode)
    {
        if (!lease.valid)
            return Detail::MakeReceipt(lease.decision, ReceiptStatus::Rejected,
                ReceiptReason::InvalidDecision, frame.version, frame.evidence, frame.evidence);

        Decision const& decision = lease.decision;
        bool const expired = Expired(frame.tick, decision.expiresTick);
        ReapExpired(frame.tick);
        if (expired || !OwnsActiveLease(lease, frame.tick))
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                expired ? ReceiptReason::Expired : ReceiptReason::NotOwner,
                frame.version, frame.evidence, frame.evidence);

        ReceiptReason const validation = Detail::ValidateDecision(frame, decision, mode);
        if (validation != ReceiptReason::None)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected, validation,
                frame.version, frame.evidence, frame.evidence);
        bool const validNodeReference = decision.operation == OperationCode::GatherSource ?
            ValidGatherSourceReference(decision.gather) :
            decision.operation == OperationCode::GatherRoute &&
                ValidGatherRouteReference(decision.gather);
        if (!validNodeReference)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::ExecutorBlocked, frame.version, frame.evidence, frame.evidence);

        std::size_t index = FindNode(decision.gather);
        if (index != nodeReservations.size())
        {
            NodeReservationSlot const& current = nodeReservations[index];
            if (SameScope(current.scope, decision.scope) &&
                current.decisionId == decision.decisionId && current.intentId == decision.intentId &&
                current.epoch == decision.epoch && current.expiresTick == decision.expiresTick)
                return Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
                    ReceiptReason::LeaseAlreadyOwned, frame.version, frame.evidence, frame.evidence);
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::NodeReservationConflict, frame.version, frame.evidence, frame.evidence);
        }

        index = FindFreeNode();
        if (index == nodeReservations.size())
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::CapacityExceeded, frame.version, frame.evidence, frame.evidence);
        nodeReservations[index] = {true, decision.gather, decision.scope,
            decision.decisionId, decision.intentId, decision.expiresTick, decision.epoch};
        return Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
            ReceiptReason::LeaseAcquired, frame.version, frame.evidence, frame.evidence);
    }

    Receipt ReleaseNodeImpl(WorldReadFrame const& frame, IntentLease const& lease,
        Detail::ValidationMode mode)
    {
        if (!lease.valid)
            return Detail::MakeReceipt(lease.decision, ReceiptStatus::Rejected,
                ReceiptReason::InvalidDecision, frame.version, frame.evidence, frame.evidence);

        Decision const& decision = lease.decision;
        bool const expired = Expired(frame.tick, decision.expiresTick);
        ReapExpired(frame.tick);
        if (expired || !OwnsActiveLease(lease, frame.tick))
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                expired ? ReceiptReason::Expired : ReceiptReason::NotOwner,
                frame.version, frame.evidence, frame.evidence);

        ReceiptReason const validation = Detail::ValidateDecision(frame, decision, mode);
        if (validation != ReceiptReason::None)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected, validation,
                frame.version, frame.evidence, frame.evidence);
        bool const validNodeReference = decision.operation == OperationCode::GatherSource ?
            ValidGatherSourceReference(decision.gather) :
            decision.operation == OperationCode::GatherRoute &&
                ValidGatherRouteReference(decision.gather);
        if (!validNodeReference)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::ExecutorBlocked, frame.version, frame.evidence, frame.evidence);
        std::size_t const index = FindNode(decision.gather);
        if (index == nodeReservations.size())
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
        NodeReservationSlot const& current = nodeReservations[index];
        if (!SameScope(current.scope, decision.scope) ||
            current.decisionId != decision.decisionId || current.intentId != decision.intentId ||
            current.epoch != decision.epoch || current.expiresTick != decision.expiresTick)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
        nodeReservations[index] = {};
        return Detail::MakeReceipt(decision, ReceiptStatus::Completed,
            ReceiptReason::LeaseReleased, frame.version, frame.evidence, frame.evidence);
    }

    LeaseResult RenewImpl(WorldReadFrame const& frame, IntentLease const& lease,
        Detail::ValidationMode mode)
    {
        LeaseResult result;
        if (!lease.valid)
        {
            result.receipt = Detail::MakeFrameReceipt(frame, ReceiptStatus::Rejected,
                ReceiptReason::InvalidDecision);
            return result;
        }

        std::size_t const index = FindBot(lease.decision.scope.botGuid);
        if (index == botLeases.size() || !botLeases[index].occupied ||
            botLeases[index].lease.decision.decisionId != lease.decision.decisionId ||
            botLeases[index].lease.decision.intentId != lease.decision.intentId ||
            botLeases[index].lease.decision.epoch != lease.decision.epoch ||
            botLeases[index].lease.decision.expiresTick != lease.decision.expiresTick ||
            !SameScope(botLeases[index].lease.decision.scope, lease.decision.scope))
        {
            result.receipt = Detail::MakeReceipt(lease.decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
            return result;
        }
        if (Expired(frame.tick, botLeases[index].lease.decision.expiresTick))
        {
            Decision const expired = botLeases[index].lease.decision;
            ClearBotLease(index);
            result.receipt = Detail::MakeReceipt(expired, ReceiptStatus::Rejected,
                ReceiptReason::Expired, frame.version, expired.evidence, frame.evidence);
            return result;
        }

        ReceiptReason const validation = Detail::ValidateRenewFrame(
            frame, botLeases[index].lease.decision, mode);
        if (validation != ReceiptReason::None)
        {
            ClearBotLease(index);
            result.receipt = Detail::MakeReceipt(lease.decision, ReceiptStatus::Rejected,
                validation, frame.version, frame.evidence, frame.evidence);
            return result;
        }

        Decision const previous = botLeases[index].lease.decision;
        Decision renewed = previous;
        renewed.preconditions.frameVersion = frame.version;
        renewed.preconditions.epoch = frame.epoch;
        renewed.preconditions.mapId = frame.bot.mapId;
        renewed.preconditions.instanceId = frame.bot.instanceId;
        renewed.evidence = frame.evidence;
        renewed.issuedTick = frame.tick;
        renewed.expiresTick = Detail::SaturatingAdd(frame.tick, renewed.ttlTicks);
        botLeases[index].lease.decision = renewed;
        RenewNodeReservations(previous, renewed);
        result.hasLease = true;
        result.lease = botLeases[index].lease;
        result.receipt = Detail::MakeReceipt(renewed, ReceiptStatus::Progressing,
            ReceiptReason::LeaseRenewed, frame.version, lease.decision.evidence, frame.evidence);
        return result;
    }

    // These wrappers are private and reachable only through the test access friend above. They
    // exercise reservation bookkeeping for a typed gather decision without granting that decision
    // runtime lease authority.
    Receipt ReserveNodeForContractTest(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return ReserveNodeImpl(frame, lease, Detail::ValidationMode::ContractReservationTest);
    }

    Receipt ReleaseNodeForContractTest(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return ReleaseNodeImpl(frame, lease, Detail::ValidationMode::ContractReservationTest);
    }

    LeaseResult RenewForContractTest(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return RenewImpl(frame, lease, Detail::ValidationMode::ContractReservationTest);
    }

public:
    LeaseResult Acquire(WorldReadFrame const& frame, Decision const& decision)
    {
        LeaseResult result;
        ReceiptReason const validation = Detail::ValidateDecision(
            frame, decision, Detail::ValidationMode::RuntimeAuthority);
        if (validation != ReceiptReason::None)
        {
            result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Rejected, validation,
                frame.version, frame.evidence, frame.evidence);
            return result;
        }

        ReapExpired(frame.tick);

        std::size_t index = FindBot(decision.scope.botGuid);
        if (index == botLeases.size())
        {
            index = FindFreeBot();
            if (index == botLeases.size())
            {
                result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                    ReceiptReason::CapacityExceeded, frame.version, frame.evidence, frame.evidence);
                return result;
            }
        }

        if (botLeases[index].occupied && Expired(frame.tick, botLeases[index].lease.decision.expiresTick))
            ClearBotLease(index);

        if (botLeases[index].occupied)
        {
            Decision const& current = botLeases[index].lease.decision;
            if (current.decisionId == decision.decisionId && current.intentId == decision.intentId)
            {
                result.hasLease = true;
                result.lease = botLeases[index].lease;
                result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
                    ReceiptReason::LeaseAlreadyOwned, frame.version, current.evidence, frame.evidence);
                return result;
            }
            if (decision.epoch < current.epoch)
            {
                result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                    ReceiptReason::StaleEpoch, frame.version, frame.evidence, frame.evidence);
                return result;
            }
            if (!CanPreempt(decision, current))
            {
                result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                    ReceiptReason::BotLeaseConflict, frame.version, frame.evidence, frame.evidence);
                return result;
            }
        }

        Receipt const squadError = ClaimSquad(frame, decision);
        if (squadError.reason != ReceiptReason::None)
        {
            result.receipt = squadError;
            return result;
        }

        if (botLeases[index].occupied)
            ClearBotLease(index);
        botLeases[index] = {true, decision.scope.botGuid, {true, decision}};
        result.hasLease = true;
        result.lease = botLeases[index].lease;
        result.receipt = Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
            ReceiptReason::LeaseAcquired, frame.version, frame.evidence, frame.evidence);
        return result;
    }

    ActiveLeaseSnapshot QueryActiveLease(Guid botGuid, Tick now) const
    {
        ActiveLeaseSnapshot snapshot;
        std::size_t const index = FindBot(botGuid);
        if (index == botLeases.size() || !botLeases[index].occupied)
            return snapshot;

        Decision const& decision = botLeases[index].lease.decision;
        if (Expired(now, decision.expiresTick))
            return snapshot;

        snapshot.active = true;
        snapshot.botGuid = botGuid;
        snapshot.decisionId = decision.decisionId;
        snapshot.intentId = decision.intentId;
        snapshot.epoch = decision.epoch;
        snapshot.expiresTick = decision.expiresTick;
        snapshot.scope = decision.scope;
        snapshot.operation = decision.operation;
        return snapshot;
    }

    bool OwnsActiveLease(IntentLease const& lease, Tick now) const
    {
        if (!lease.valid)
            return false;
        ActiveLeaseSnapshot const active = QueryActiveLease(lease.decision.scope.botGuid, now);
        return active.active && active.decisionId == lease.decision.decisionId &&
            active.intentId == lease.decision.intentId && active.epoch == lease.decision.epoch &&
            active.expiresTick == lease.decision.expiresTick &&
            SameScope(active.scope, lease.decision.scope);
    }

    LeaseResult Renew(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return RenewImpl(frame, lease, Detail::ValidationMode::RuntimeAuthority);
    }

    Receipt Release(WorldReadFrame const& frame, IntentLease const& lease)
    {
        if (!lease.valid)
            return Detail::MakeFrameReceipt(frame, ReceiptStatus::Rejected,
                ReceiptReason::InvalidDecision);
        std::size_t const index = FindBot(lease.decision.scope.botGuid);
        if (index == botLeases.size() || !botLeases[index].occupied ||
            botLeases[index].lease.decision.decisionId != lease.decision.decisionId ||
            botLeases[index].lease.decision.intentId != lease.decision.intentId ||
            botLeases[index].lease.decision.epoch != lease.decision.epoch ||
            botLeases[index].lease.decision.expiresTick != lease.decision.expiresTick ||
            !SameScope(botLeases[index].lease.decision.scope, lease.decision.scope))
            return Detail::MakeReceipt(lease.decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
        if (Expired(frame.tick, botLeases[index].lease.decision.expiresTick))
        {
            Decision const expired = botLeases[index].lease.decision;
            ClearBotLease(index);
            return Detail::MakeReceipt(expired, ReceiptStatus::Rejected,
                ReceiptReason::Expired, frame.version, expired.evidence, frame.evidence);
        }
        Decision const released = botLeases[index].lease.decision;
        ClearBotLease(index);
        return Detail::MakeReceipt(released, ReceiptStatus::Completed,
            ReceiptReason::LeaseReleased, frame.version, released.evidence, frame.evidence);
    }

    Receipt ReserveItem(WorldReadFrame const& frame, Decision const& decision)
    {
        ReceiptReason const validation = Detail::ValidateDecision(frame, decision);
        if (validation != ReceiptReason::None)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected, validation,
                frame.version, frame.evidence, frame.evidence);
        if (decision.itemId == 0)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::PreconditionFailed, frame.version, frame.evidence, frame.evidence);

        ReapExpired(frame.tick);

        std::size_t index = FindItem(decision.itemId);
        if (index != itemReservations.size() && Expired(frame.tick, itemReservations[index].expiresTick))
        {
            itemReservations[index] = {};
            index = itemReservations.size();
        }
        if (index != itemReservations.size())
        {
            ItemReservationSlot const& current = itemReservations[index];
            if (SameScope(current.scope, decision.scope) &&
                current.decisionId == decision.decisionId && current.intentId == decision.intentId)
                return Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
                    ReceiptReason::LeaseAlreadyOwned, frame.version, frame.evidence, frame.evidence);
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::ItemReservationConflict, frame.version, frame.evidence, frame.evidence);
        }

        index = FindFreeItem();
        if (index == itemReservations.size())
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::CapacityExceeded, frame.version, frame.evidence, frame.evidence);
        itemReservations[index] = {true, decision.itemId, decision.scope, decision.decisionId,
            decision.intentId, decision.expiresTick, decision.epoch};
        return Detail::MakeReceipt(decision, ReceiptStatus::Accepted,
            ReceiptReason::LeaseAcquired, frame.version, frame.evidence, frame.evidence);
    }

    Receipt ReleaseItem(WorldReadFrame const& frame, Decision const& decision)
    {
        std::size_t const index = FindItem(decision.itemId);
        if (index == itemReservations.size())
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
        ItemReservationSlot const& current = itemReservations[index];
        if (Expired(frame.tick, current.expiresTick))
        {
            itemReservations[index] = {};
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::Expired, frame.version, frame.evidence, frame.evidence);
        }
        if (!SameScope(current.scope, decision.scope) ||
            current.decisionId != decision.decisionId || current.intentId != decision.intentId)
            return Detail::MakeReceipt(decision, ReceiptStatus::Rejected,
                ReceiptReason::NotOwner, frame.version, frame.evidence, frame.evidence);
        itemReservations[index] = {};
        return Detail::MakeReceipt(decision, ReceiptStatus::Completed,
            ReceiptReason::LeaseReleased, frame.version, frame.evidence, frame.evidence);
    }

    Receipt ReserveNode(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return ReserveNodeImpl(frame, lease, Detail::ValidationMode::RuntimeAuthority);
    }

    Receipt ReleaseNode(WorldReadFrame const& frame, IntentLease const& lease)
    {
        return ReleaseNodeImpl(frame, lease, Detail::ValidationMode::RuntimeAuthority);
    }

    std::size_t ActiveBotLeaseCount() const
    {
        std::size_t count = 0;
        for (BotLeaseSlot const& slot : botLeases)
            if (slot.occupied)
                ++count;
        return count;
    }

    std::size_t ActiveSquadOwnerCount() const
    {
        std::size_t count = 0;
        for (SquadOwnerSlot const& slot : squadOwners)
            if (slot.occupied)
                ++count;
        return count;
    }

    std::size_t ActiveItemReservationCount() const
    {
        std::size_t count = 0;
        for (ItemReservationSlot const& slot : itemReservations)
            if (slot.occupied)
                ++count;
        return count;
    }

    std::size_t ActiveNodeReservationCount() const
    {
        std::size_t count = 0;
        for (NodeReservationSlot const& slot : nodeReservations)
            if (slot.occupied)
                ++count;
        return count;
    }
};
}

#endif
