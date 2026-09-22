/*
 * Pure deterministic GATHER + CRAFT + ECONOMY policy for AutoWow Oracle.
 *
 * The planner consumes an immutable fact snapshot and returns declarative intents. It does not own
 * a Playerbot object, call a native action, read a clock, perform I/O, or mutate an item, gold, or
 * skill value. The executor is responsible for applying an intent and publishing the next snapshot.
 *
 * This header is intentionally independent of the native action classes. When the integrated
 * AutoWowOracleContract.h is available, the optional adapter at the bottom converts one planned
 * intent into the contract's OracleCandidate without changing the pure planning surface.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_GATHER_CRAFT_ECONOMY_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_GATHER_CRAFT_ECONOMY_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#if defined(__has_include)
#    if __has_include("AutoWowOracleContract.h")
#        include "AutoWowOracleContract.h"
#        define AUTOWOW_ORACLE_GCE_HAS_CONTRACT 1
#    endif
#endif

#ifndef AUTOWOW_ORACLE_GCE_HAS_CONTRACT
#    define AUTOWOW_ORACLE_GCE_HAS_CONTRACT 0
#endif

namespace AutoWowOracleGatherCraftEconomy
{
using Guid = std::uint64_t;
using NodeId = std::uint64_t;
using ItemId = std::uint32_t;
using RecipeId = std::uint32_t;
using Tick = std::uint64_t;

inline constexpr std::size_t kMaxSkills = 16;
inline constexpr std::size_t kMaxTools = 16;
inline constexpr std::size_t kMaxMaterials = 96;
inline constexpr std::size_t kMaxRecipes = 96;
inline constexpr std::size_t kMaxRecipeInputs = 8;
inline constexpr std::size_t kMaxGatherNodes = 96;
inline constexpr std::size_t kMaxGatherRoutes = 96;
inline constexpr std::size_t kMaxRouteWaypoints = 24;
inline constexpr std::size_t kMaxRequests = 16;
inline constexpr std::size_t kMaxGuildDemands = 32;
inline constexpr std::size_t kMaxReservations = 128;
inline constexpr std::size_t kMaxSourceNeeds = 96;
inline constexpr std::size_t kMaxCraftNodes = 96;
inline constexpr std::size_t kMaxPlanIntents = 128;
inline constexpr std::uint8_t kMaxRetryAttempts = 8;

inline constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();

enum class Profession : std::uint8_t
{
    None = 0,
    Alchemy,
    Blacksmithing,
    Cooking,
    Enchanting,
    Engineering,
    FirstAid,
    Herbalism,
    Inscription,
    Jewelcrafting,
    Leatherworking,
    Mining,
    Skinning,
    Tailoring,
    Fishing
};

inline constexpr std::string_view ProfessionName(Profession profession)
{
    switch (profession)
    {
        case Profession::Alchemy: return "alchemy";
        case Profession::Blacksmithing: return "blacksmithing";
        case Profession::Cooking: return "cooking";
        case Profession::Enchanting: return "enchanting";
        case Profession::Engineering: return "engineering";
        case Profession::FirstAid: return "first_aid";
        case Profession::Herbalism: return "herbalism";
        case Profession::Inscription: return "inscription";
        case Profession::Jewelcrafting: return "jewelcrafting";
        case Profession::Leatherworking: return "leatherworking";
        case Profession::Mining: return "mining";
        case Profession::Skinning: return "skinning";
        case Profession::Tailoring: return "tailoring";
        case Profession::Fishing: return "fishing";
        case Profession::None: return "none";
    }
    return "none";
}

enum class ReservationKind : std::uint8_t
{
    Node = 0,
    Item
};

enum class PlanStatus : std::uint8_t
{
    Planned = 0,
    Blocked,
    Invalid
};

enum class PlanBlockReason : std::uint8_t
{
    None = 0,
    InvalidFacts,
    NoDemand,
    BotUnavailable,
    InCombat,
    BudgetExhausted,
    ReservationCapacity,
    NodeReservationConflict,
    ItemReservationConflict,
    MissingMaterial,
    MissingProfession,
    MissingSkill,
    MissingTool,
    NoLegalMovementRoute,
    DependencyCycle,
    UnknownRecipe,
    GoldBudget,
    RetryLimit
};

inline constexpr std::string_view PlanBlockReasonName(PlanBlockReason reason)
{
    switch (reason)
    {
        case PlanBlockReason::None: return "none";
        case PlanBlockReason::InvalidFacts: return "invalid_facts";
        case PlanBlockReason::NoDemand: return "no_demand";
        case PlanBlockReason::BotUnavailable: return "bot_unavailable";
        case PlanBlockReason::InCombat: return "in_combat";
        case PlanBlockReason::BudgetExhausted: return "budget_exhausted";
        case PlanBlockReason::ReservationCapacity: return "reservation_capacity";
        case PlanBlockReason::NodeReservationConflict: return "node_reservation_conflict";
        case PlanBlockReason::ItemReservationConflict: return "item_reservation_conflict";
        case PlanBlockReason::MissingMaterial: return "missing_material";
        case PlanBlockReason::MissingProfession: return "missing_profession";
        case PlanBlockReason::MissingSkill: return "missing_skill";
        case PlanBlockReason::MissingTool: return "missing_tool";
        case PlanBlockReason::NoLegalMovementRoute: return "no_legal_movement_route";
        case PlanBlockReason::DependencyCycle: return "dependency_cycle";
        case PlanBlockReason::UnknownRecipe: return "unknown_recipe";
        case PlanBlockReason::GoldBudget: return "gold_budget";
        case PlanBlockReason::RetryLimit: return "retry_limit";
    }
    return "invalid_facts";
}

// These names match the native action constructors read by the policy owner. They are labels only;
// the planner never calls the actions.
enum class IntentKind : std::uint8_t
{
    GatherRoute = 0,
    GatherSource,
    Craft,
    BankWithdraw,
    BankDeposit,
    MailReceive,
    MailSend,
    VendorBuy,
    VendorSell,
    AuctionBuy,
    AuctionSell
};

inline constexpr bool IsGatherIntent(IntentKind kind)
{
    return kind == IntentKind::GatherRoute || kind == IntentKind::GatherSource;
}

inline constexpr bool IsCraftIntent(IntentKind kind)
{
    return kind == IntentKind::Craft;
}

inline constexpr bool IsEconomyIntent(IntentKind kind)
{
    return !IsGatherIntent(kind) && !IsCraftIntent(kind);
}

inline constexpr std::string_view NativeActionName(IntentKind kind)
{
    switch (kind)
    {
        case IntentKind::GatherRoute: return "move to travel target";
        case IntentKind::GatherSource: return "worker gather seek";
        case IntentKind::Craft: return "craft";
        case IntentKind::BankWithdraw:
        case IntentKind::BankDeposit: return "bank";
        case IntentKind::MailReceive: return "mail";
        case IntentKind::MailSend: return "sendmail";
        case IntentKind::VendorBuy: return "buy";
        case IntentKind::VendorSell: return "sell";
        case IntentKind::AuctionBuy:
        case IntentKind::AuctionSell: return "auction"; // Intent label; this module has no AuctionAction.
    }
    return "";
}

inline constexpr std::string_view IntentQualifier(IntentKind kind)
{
    switch (kind)
    {
        case IntentKind::GatherRoute: return "legal_route";
        case IntentKind::GatherSource: return "gather";
        case IntentKind::Craft: return "craft";
        case IntentKind::BankWithdraw: return "withdraw";
        case IntentKind::BankDeposit: return "deposit";
        case IntentKind::MailReceive: return "take";
        case IntentKind::MailSend: return "send";
        case IntentKind::VendorBuy: return "buy";
        case IntentKind::VendorSell: return "sell";
        case IntentKind::AuctionBuy: return "buy";
        case IntentKind::AuctionSell: return "sell";
    }
    return "";
}

struct ProfessionSkillFact
{
    Profession profession = Profession::None;
    std::uint32_t skill = 0;
    std::uint32_t maximumSkill = 0;
    bool learned = false;
};

struct ToolFact
{
    ItemId itemId = 0;
    Profession profession = Profession::None;
    std::uint32_t requiredSkill = 0;
    bool owned = false;
    bool equipped = false;
};

struct MaterialFact
{
    ItemId itemId = 0;
    std::uint32_t inventory = 0;
    std::uint32_t bank = 0;
    std::uint32_t mail = 0;
    std::uint32_t guildBank = 0;
    std::uint32_t reserved = 0;
    std::uint64_t vendorUnitPrice = 0;
    std::uint64_t auctionUnitPrice = 0;
    bool vendorAvailable = false;
    bool auctionAvailable = false;
    bool tradable = false;
};

struct RecipeInputFact
{
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
};

struct RecipeFact
{
    RecipeId recipeId = 0;
    ItemId outputItemId = 0;
    std::uint32_t outputQuantity = 0;
    Profession profession = Profession::None;
    std::uint32_t requiredSkill = 0;
    std::uint64_t craftFee = 0;
    std::array<RecipeInputFact, kMaxRecipeInputs> inputs{};
    std::size_t inputCount = 0;
    bool known = false;
};

struct RouteWaypointFact
{
    std::uint32_t mapId = 0;
    std::uint32_t waypointId = 0;
};

struct GatherNodeFact
{
    NodeId nodeId = 0;
    std::uint32_t entry = 0;
    std::uint32_t mapId = 0;
    ItemId materialItemId = 0;
    Profession profession = Profession::None;
    std::uint32_t requiredSkill = 0;
    ItemId requiredToolId = 0;
    std::uint16_t priority = 0;
    bool available = false;
    bool spawned = false;
};

// The route proof is supplied by the snapshot publisher after the normal movement/path checks.
// No route is accepted from coordinates alone: all four proof bits must be true.
struct LegalMovementRouteFact
{
    NodeId nodeId = 0;
    std::uint32_t mapId = 0;
    std::uint32_t routeCost = 0;
    std::array<RouteWaypointFact, kMaxRouteWaypoints> waypoints{};
    std::size_t waypointCount = 0;
    bool pathCalculated = false;
    bool pathComplete = false;
    bool usesOnlyLegalMovement = false;
    bool endpointsLegal = false;
};

inline constexpr bool HasLegalMovement(LegalMovementRouteFact const& route, std::uint32_t mapId)
{
    if (route.nodeId == 0 || route.mapId != mapId || route.waypointCount > route.waypoints.size() ||
        !route.pathCalculated || !route.pathComplete || !route.usesOnlyLegalMovement || !route.endpointsLegal)
        return false;
    for (std::size_t index = 0; index < route.waypointCount; ++index)
        if (!route.waypoints[index].waypointId || route.waypoints[index].mapId != route.mapId)
            return false;
    return true;
}

struct OutputRequestFact
{
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
    std::uint16_t priority = 0;
};

struct GuildDemandFact
{
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
    std::uint32_t fulfilled = 0;
    std::uint16_t priority = 0;
    Guid requesterGuid = 0;
    bool active = false;
};

struct EconomyAccessFacts
{
    bool bankAvailable = false;
    bool mailboxAvailable = false;
    bool guildBankAvailable = false;
    bool vendorAvailable = false;
    bool auctionAvailable = false;
    Guid bankGuid = 0;
    Guid mailboxGuid = 0;
    Guid vendorGuid = 0;
    Guid auctioneerGuid = 0;
};

struct PlannerBudget
{
    std::size_t maxIntents = kMaxPlanIntents;
    std::uint32_t maxGatherNodes = 8;
    std::uint32_t maxCrafts = 32;
    std::uint32_t maxEconomyActions = 32;
    std::uint64_t goldAvailable = 0;
    std::uint64_t goldReserve = 0;
    std::uint64_t maxSpend = 0;
    std::uint8_t maxAttempts = 3;
    Tick initialBackoffTicks = 1;
    Tick maxBackoffTicks = 64;
    std::uint32_t intentTtlTicks = 8;
};

struct NodeReservationFact
{
    bool active = false;
    NodeId nodeId = 0;
    Guid ownerGuid = 0;
    Tick expiresTick = 0;
    std::uint16_t priority = 0;
};

struct ItemReservationFact
{
    bool active = false;
    ItemId itemId = 0;
    Guid ownerGuid = 0;
    std::uint32_t quantity = 0;
    Tick expiresTick = 0;
    std::uint16_t priority = 0;
};

struct ReservationSnapshot
{
    std::array<NodeReservationFact, kMaxReservations> nodes{};
    std::size_t nodeCount = 0;
    std::array<ItemReservationFact, kMaxReservations> items{};
    std::size_t itemCount = 0;
};

struct ReservationClaim
{
    ReservationKind kind = ReservationKind::Node;
    NodeId nodeId = 0;
    ItemId itemId = 0;
    Guid ownerGuid = 0;
    std::uint32_t quantity = 0;
    Tick expiresTick = 0;
    std::uint16_t priority = 0;
};

struct AutoWowOracleGatherCraftEconomyInput
{
    Guid ownerGuid = 0;
    std::uint64_t factVersion = 0;
    std::uint64_t epoch = 0;
    Tick tick = 0;
    std::uint32_t mapId = 0;
    bool botAlive = true;
    bool inCombat = false;

    std::array<ProfessionSkillFact, kMaxSkills> skills{};
    std::size_t skillCount = 0;
    std::array<ToolFact, kMaxTools> tools{};
    std::size_t toolCount = 0;
    std::array<MaterialFact, kMaxMaterials> materials{};
    std::size_t materialCount = 0;
    std::array<RecipeFact, kMaxRecipes> recipes{};
    std::size_t recipeCount = 0;
    std::array<GatherNodeFact, kMaxGatherNodes> gatherNodes{};
    std::size_t gatherNodeCount = 0;
    std::array<LegalMovementRouteFact, kMaxGatherRoutes> gatherRoutes{};
    std::size_t gatherRouteCount = 0;
    std::array<OutputRequestFact, kMaxRequests> requests{};
    std::size_t requestCount = 0;
    std::array<GuildDemandFact, kMaxGuildDemands> guildDemands{};
    std::size_t guildDemandCount = 0;
    ReservationSnapshot reservations;
    EconomyAccessFacts access;
    PlannerBudget budget;
};

struct SourceNeed
{
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
    std::uint16_t priority = 0;
};

struct CraftDagNode
{
    ItemId outputItemId = 0;
    RecipeId recipeId = 0;
    std::uint32_t craftCount = 0;
    std::uint32_t outputQuantity = 0;
    std::uint64_t craftFee = 0;
    std::array<RecipeInputFact, kMaxRecipeInputs> inputs{};
    std::size_t inputCount = 0;
};

struct DependencyDag
{
    bool valid = false;
    PlanBlockReason failure = PlanBlockReason::None;
    std::array<CraftDagNode, kMaxCraftNodes> nodes{};
    std::size_t nodeCount = 0;
    std::array<std::size_t, kMaxCraftNodes> topologicalOrder{};
    std::size_t topologicalCount = 0;
    std::array<SourceNeed, kMaxSourceNeeds> sourceNeeds{};
    std::size_t sourceNeedCount = 0;
};

struct RetrySchedule
{
    bool allowed = false;
    std::uint8_t attempt = 0;
    std::uint8_t maxAttempts = 0;
    Tick backoffTicks = 0;
};

inline RetrySchedule RetryForAttempt(PlannerBudget const& budget, std::uint8_t attempt)
{
    RetrySchedule schedule;
    schedule.maxAttempts = budget.maxAttempts;
    if (budget.maxAttempts == 0 || budget.maxAttempts > kMaxRetryAttempts || attempt >= budget.maxAttempts)
        return schedule;

    Tick delay = budget.initialBackoffTicks;
    for (std::uint8_t index = 0; index < attempt; ++index)
    {
        if (delay > std::numeric_limits<Tick>::max() / 2)
        {
            delay = std::numeric_limits<Tick>::max();
            break;
        }
        delay *= 2;
    }
    if (delay > budget.maxBackoffTicks)
        delay = budget.maxBackoffTicks;

    schedule.allowed = true;
    schedule.attempt = attempt;
    schedule.backoffTicks = delay;
    return schedule;
}

enum class IntentDomain : std::uint8_t
{
    Navigation = 0,
    Gathering,
    CraftingEconomyItems
};

struct PlannedIntent
{
    IntentKind kind = IntentKind::GatherRoute;
    IntentDomain domain = IntentDomain::Gathering;
    Guid targetGuid = 0;
    NodeId nodeId = 0;
    ItemId itemId = 0;
    RecipeId recipeId = 0;
    std::uint32_t quantity = 0;
    std::uint64_t unitCost = 0;
    std::uint32_t nodeEntry = 0;
    std::uint32_t nodeMapId = 0;
    std::uint16_t priority = 0;
    std::uint32_t ttlTicks = 0;
    std::string_view action;
    std::string_view qualifier;
    RetrySchedule retry;
    LegalMovementRouteFact route;
    bool requiresBotAlive = true;
    bool requiresSameMap = true;
};

struct PlannerReceipt
{
    PlanStatus status = PlanStatus::Invalid;
    PlanBlockReason reason = PlanBlockReason::None;
    std::size_t intentCount = 0;
    std::size_t reservationCount = 0;
    std::uint64_t projectedSpend = 0;
};

struct AutoWowOracleGatherCraftEconomyPlan
{
    PlannerReceipt receipt;
    ItemId selectedItemId = 0;
    std::uint32_t selectedQuantity = 0;
    std::uint16_t selectedPriority = 0;
    DependencyDag dependencyDag;
    std::array<PlannedIntent, kMaxPlanIntents> intents{};
    std::size_t intentCount = 0;
    std::array<ReservationClaim, kMaxReservations> reservations{};
    std::size_t reservationCount = 0;
    std::uint32_t gatherNodesPlanned = 0;
    std::uint32_t craftsPlanned = 0;
    std::uint32_t economyActionsPlanned = 0;
    std::uint64_t projectedSpend = 0;
};

inline constexpr std::uint32_t SaturatingAdd32(std::uint32_t left, std::uint32_t right)
{
    constexpr std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max();
    return right > maximum - left ? maximum : left + right;
}

inline constexpr std::uint64_t SaturatingAdd64(std::uint64_t left, std::uint64_t right)
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

inline constexpr std::uint32_t SaturatingMultiply32(std::uint32_t left, std::uint32_t right)
{
    constexpr std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max();
    if (left == 0 || right == 0)
        return 0;
    return left > maximum / right ? maximum : left * right;
}

inline constexpr std::uint64_t SaturatingMultiply64(std::uint64_t left, std::uint64_t right)
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (left == 0 || right == 0)
        return 0;
    return left > maximum / right ? maximum : left * right;
}

inline constexpr Tick SaturatingAddTick(Tick left, Tick right)
{
    return right > std::numeric_limits<Tick>::max() - left ? std::numeric_limits<Tick>::max() : left + right;
}

inline constexpr std::uint32_t CeilDivide(std::uint32_t numerator, std::uint32_t denominator)
{
    return denominator == 0 ? 0 : numerator / denominator + (numerator % denominator == 0 ? 0 : 1);
}

inline std::size_t FindSkill(AutoWowOracleGatherCraftEconomyInput const& input, Profession profession)
{
    for (std::size_t index = 0; index < input.skillCount; ++index)
        if (input.skills[index].profession == profession)
            return index;
    return kInvalidIndex;
}

inline bool HasCraftingSkill(AutoWowOracleGatherCraftEconomyInput const& input,
    Profession profession, std::uint32_t requiredSkill)
{
    std::size_t const index = FindSkill(input, profession);
    return index != kInvalidIndex && input.skills[index].learned && input.skills[index].skill >= requiredSkill;
}

inline std::size_t FindTool(AutoWowOracleGatherCraftEconomyInput const& input, ItemId itemId)
{
    for (std::size_t index = 0; index < input.toolCount; ++index)
        if (input.tools[index].itemId == itemId)
            return index;
    return kInvalidIndex;
}

inline std::size_t FindMaterial(AutoWowOracleGatherCraftEconomyInput const& input, ItemId itemId)
{
    for (std::size_t index = 0; index < input.materialCount; ++index)
        if (input.materials[index].itemId == itemId)
            return index;
    return kInvalidIndex;
}

inline std::size_t FindRecipeNode(DependencyDag const& dag, ItemId itemId)
{
    for (std::size_t index = 0; index < dag.nodeCount; ++index)
        if (dag.nodes[index].outputItemId == itemId)
            return index;
    return kInvalidIndex;
}

inline std::size_t FindRoute(AutoWowOracleGatherCraftEconomyInput const& input, NodeId nodeId)
{
    for (std::size_t index = 0; index < input.gatherRouteCount; ++index)
        if (input.gatherRoutes[index].nodeId == nodeId)
            return index;
    return kInvalidIndex;
}

inline std::uint32_t AvailableInventory(AutoWowOracleGatherCraftEconomyInput const& input, ItemId itemId)
{
    std::size_t const index = FindMaterial(input, itemId);
    if (index == kInvalidIndex)
        return 0;
    MaterialFact const& material = input.materials[index];
    return material.reserved >= material.inventory ? 0 : material.inventory - material.reserved;
}

struct DemandSelection
{
    bool found = false;
    ItemId itemId = 0;
    std::uint32_t quantity = 0;
    std::uint16_t priority = 0;
    Guid requesterGuid = 0;
};

inline bool BetterDemand(OutputRequestFact const& candidate, OutputRequestFact const& current)
{
    if (candidate.priority != current.priority)
        return candidate.priority > current.priority;
    if (candidate.itemId != current.itemId)
        return candidate.itemId < current.itemId;
    return candidate.quantity > current.quantity;
}

inline bool BetterGuildDemand(GuildDemandFact const& candidate, GuildDemandFact const& current)
{
    if (candidate.priority != current.priority)
        return candidate.priority > current.priority;
    if (candidate.itemId != current.itemId)
        return candidate.itemId < current.itemId;
    if (candidate.requesterGuid != current.requesterGuid)
        return candidate.requesterGuid < current.requesterGuid;
    return candidate.quantity > current.quantity;
}

inline DemandSelection SelectDemand(AutoWowOracleGatherCraftEconomyInput const& input)
{
    DemandSelection selection;
    bool hasRequest = false;
    OutputRequestFact chosenRequest;
    for (std::size_t index = 0; index < input.requestCount; ++index)
    {
        OutputRequestFact const& request = input.requests[index];
        if (!request.itemId || !request.quantity)
            continue;
        if (!hasRequest || BetterDemand(request, chosenRequest))
        {
            chosenRequest = request;
            hasRequest = true;
        }
    }
    if (hasRequest)
    {
        selection.found = true;
        selection.itemId = chosenRequest.itemId;
        selection.quantity = chosenRequest.quantity;
        selection.priority = chosenRequest.priority;
        return selection;
    }

    bool hasGuildDemand = false;
    GuildDemandFact chosenDemand;
    for (std::size_t index = 0; index < input.guildDemandCount; ++index)
    {
        GuildDemandFact const& demand = input.guildDemands[index];
        if (!demand.active || !demand.itemId || demand.quantity <= demand.fulfilled)
            continue;
        if (!hasGuildDemand || BetterGuildDemand(demand, chosenDemand))
        {
            chosenDemand = demand;
            hasGuildDemand = true;
        }
    }
    if (hasGuildDemand)
    {
        selection.found = true;
        selection.itemId = chosenDemand.itemId;
        selection.quantity = chosenDemand.quantity - chosenDemand.fulfilled;
        selection.priority = chosenDemand.priority;
        selection.requesterGuid = chosenDemand.requesterGuid;
    }
    return selection;
}

inline bool ValidateFacts(AutoWowOracleGatherCraftEconomyInput const& input)
{
    if (!input.ownerGuid || input.skillCount > input.skills.size() || input.toolCount > input.tools.size() ||
        input.materialCount > input.materials.size() || input.recipeCount > input.recipes.size() ||
        input.gatherNodeCount > input.gatherNodes.size() || input.gatherRouteCount > input.gatherRoutes.size() ||
        input.requestCount > input.requests.size() || input.guildDemandCount > input.guildDemands.size() ||
        input.reservations.nodeCount > input.reservations.nodes.size() ||
        input.reservations.itemCount > input.reservations.items.size())
        return false;
    if (!input.budget.maxIntents || input.budget.maxIntents > kMaxPlanIntents ||
        !input.budget.intentTtlTicks || !input.budget.maxAttempts ||
        input.budget.maxAttempts > kMaxRetryAttempts || input.budget.maxBackoffTicks < input.budget.initialBackoffTicks)
        return false;
    if (input.budget.goldReserve > input.budget.goldAvailable)
        return false;
    for (std::size_t index = 0; index < input.skillCount; ++index)
    {
        if (input.skills[index].profession == Profession::None)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.skills[previous].profession == input.skills[index].profession)
                return false;
    }
    for (std::size_t index = 0; index < input.toolCount; ++index)
    {
        if (!input.tools[index].itemId || input.tools[index].profession == Profession::None)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.tools[previous].itemId == input.tools[index].itemId)
                return false;
    }
    for (std::size_t index = 0; index < input.recipeCount; ++index)
    {
        RecipeFact const& recipe = input.recipes[index];
        if (!recipe.recipeId || !recipe.outputItemId || !recipe.outputQuantity || recipe.inputCount > recipe.inputs.size())
            return false;
        if (recipe.known && recipe.profession == Profession::None)
            return false;
        for (std::size_t inputIndex = 0; inputIndex < recipe.inputCount; ++inputIndex)
            if (!recipe.inputs[inputIndex].itemId || !recipe.inputs[inputIndex].quantity)
                return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.recipes[previous].recipeId == recipe.recipeId)
                return false;
    }
    for (std::size_t index = 0; index < input.materialCount; ++index)
    {
        if (!input.materials[index].itemId)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.materials[previous].itemId == input.materials[index].itemId)
                return false;
    }
    for (std::size_t index = 0; index < input.gatherNodeCount; ++index)
    {
        GatherNodeFact const& node = input.gatherNodes[index];
        if (!node.nodeId || !node.materialItemId || node.profession == Profession::None)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.gatherNodes[previous].nodeId == node.nodeId)
                return false;
    }
    for (std::size_t index = 0; index < input.gatherRouteCount; ++index)
    {
        LegalMovementRouteFact const& route = input.gatherRoutes[index];
        if (!route.nodeId || route.waypointCount > route.waypoints.size())
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (input.gatherRoutes[previous].nodeId == route.nodeId)
                return false;
    }
    return true;
}

inline std::size_t FindCraftableRecipe(AutoWowOracleGatherCraftEconomyInput const& input,
    ItemId outputItemId, bool& knownRecipe)
{
    std::size_t selected = kInvalidIndex;
    knownRecipe = false;
    for (std::size_t index = 0; index < input.recipeCount; ++index)
    {
        RecipeFact const& recipe = input.recipes[index];
        if (!recipe.known || recipe.outputItemId != outputItemId)
            continue;
        knownRecipe = true;
        if (!HasCraftingSkill(input, recipe.profession, recipe.requiredSkill))
            continue;
        if (selected == kInvalidIndex || recipe.recipeId < input.recipes[selected].recipeId)
            selected = index;
    }
    return selected;
}

struct DagBuilder
{
    AutoWowOracleGatherCraftEconomyInput const& input;
    DependencyDag& dag;
    std::array<ItemId, kMaxCraftNodes> active{};
    std::size_t activeCount = 0;
    std::uint16_t priority = 0;

    bool AddSourceNeed(ItemId itemId, std::uint32_t quantity)
    {
        if (!itemId || !quantity)
            return true;
        for (std::size_t index = 0; index < dag.sourceNeedCount; ++index)
        {
            if (dag.sourceNeeds[index].itemId == itemId)
            {
                dag.sourceNeeds[index].quantity = SaturatingAdd32(dag.sourceNeeds[index].quantity, quantity);
                dag.sourceNeeds[index].priority = dag.sourceNeeds[index].priority > priority ?
                    dag.sourceNeeds[index].priority : priority;
                return dag.sourceNeeds[index].quantity != std::numeric_limits<std::uint32_t>::max();
            }
        }
        if (dag.sourceNeedCount >= dag.sourceNeeds.size())
            return false;
        dag.sourceNeeds[dag.sourceNeedCount++] = {itemId, quantity, priority};
        return true;
    }

    bool IsActive(ItemId itemId) const
    {
        for (std::size_t index = 0; index < activeCount; ++index)
            if (active[index] == itemId)
                return true;
        return false;
    }

    bool PushActive(ItemId itemId)
    {
        if (activeCount >= active.size())
            return false;
        active[activeCount++] = itemId;
        return true;
    }

    void PopActive()
    {
        if (activeCount)
            --activeCount;
    }

    bool BuildItem(ItemId itemId, std::uint32_t quantity)
    {
        if (!itemId || !quantity)
            return true;

        std::size_t const existingIndex = FindRecipeNode(dag, itemId);
        std::uint64_t covered = AvailableInventory(input, itemId);
        if (existingIndex != kInvalidIndex)
            covered = SaturatingAdd64(covered, SaturatingMultiply64(
                dag.nodes[existingIndex].craftCount, dag.nodes[existingIndex].outputQuantity));
        if (covered >= quantity)
            return true;

        std::uint64_t const deficit64 = static_cast<std::uint64_t>(quantity) - covered;
        if (deficit64 > std::numeric_limits<std::uint32_t>::max())
        {
            dag.failure = PlanBlockReason::BudgetExhausted;
            return false;
        }
        std::uint32_t const deficit = static_cast<std::uint32_t>(deficit64);
        if (IsActive(itemId))
        {
            dag.failure = PlanBlockReason::DependencyCycle;
            return false;
        }

        bool knownRecipe = false;
        std::size_t const recipeIndex = FindCraftableRecipe(input, itemId, knownRecipe);
        if (recipeIndex == kInvalidIndex)
        {
            if (!AddSourceNeed(itemId, deficit))
            {
                dag.failure = PlanBlockReason::BudgetExhausted;
                return false;
            }
            return true;
        }

        RecipeFact const& recipe = input.recipes[recipeIndex];
        std::uint32_t const additionalCrafts = CeilDivide(deficit, recipe.outputQuantity);
        if (!additionalCrafts)
            return true;
        if (!PushActive(itemId))
        {
            dag.failure = PlanBlockReason::DependencyCycle;
            return false;
        }

        bool childrenBuilt = true;
        for (std::size_t inputIndex = 0; inputIndex < recipe.inputCount; ++inputIndex)
        {
            RecipeInputFact const& ingredient = recipe.inputs[inputIndex];
            std::uint32_t const ingredientQuantity = SaturatingMultiply32(ingredient.quantity, additionalCrafts);
            if (!ingredientQuantity || !BuildItem(ingredient.itemId, ingredientQuantity))
            {
                childrenBuilt = false;
                break;
            }
        }
        PopActive();
        if (!childrenBuilt)
            return false;

        if (existingIndex == kInvalidIndex)
        {
            if (dag.nodeCount >= dag.nodes.size())
            {
                dag.failure = PlanBlockReason::BudgetExhausted;
                return false;
            }
            CraftDagNode node;
            node.outputItemId = recipe.outputItemId;
            node.recipeId = recipe.recipeId;
            node.craftCount = additionalCrafts;
            node.outputQuantity = recipe.outputQuantity;
            node.craftFee = recipe.craftFee;
            node.inputs = recipe.inputs;
            node.inputCount = recipe.inputCount;
            dag.nodes[dag.nodeCount++] = node;
        }
        else
        {
            std::uint32_t const newCount = SaturatingAdd32(
                dag.nodes[existingIndex].craftCount, additionalCrafts);
            if (newCount == std::numeric_limits<std::uint32_t>::max())
            {
                dag.failure = PlanBlockReason::BudgetExhausted;
                return false;
            }
            dag.nodes[existingIndex].craftCount = newCount;
        }
        return true;
    }
};

inline void SortSourceNeeds(DependencyDag& dag)
{
    for (std::size_t position = 0; position < dag.sourceNeedCount; ++position)
    {
        std::size_t best = position;
        for (std::size_t index = position + 1; index < dag.sourceNeedCount; ++index)
        {
            SourceNeed const& candidate = dag.sourceNeeds[index];
            SourceNeed const& current = dag.sourceNeeds[best];
            if (candidate.itemId < current.itemId ||
                (candidate.itemId == current.itemId && candidate.priority > current.priority))
                best = index;
        }
        if (best != position)
        {
            SourceNeed const saved = dag.sourceNeeds[position];
            dag.sourceNeeds[position] = dag.sourceNeeds[best];
            dag.sourceNeeds[best] = saved;
        }
    }
}

inline bool NodeDependsOn(CraftDagNode const& node, ItemId itemId)
{
    for (std::size_t index = 0; index < node.inputCount; ++index)
        if (node.inputs[index].itemId == itemId)
            return true;
    return false;
}

inline bool BuildTopologicalOrder(DependencyDag& dag)
{
    std::array<bool, kMaxCraftNodes> emitted{};
    for (std::size_t position = 0; position < dag.nodeCount; ++position)
    {
        std::size_t selected = kInvalidIndex;
        for (std::size_t index = 0; index < dag.nodeCount; ++index)
        {
            if (emitted[index])
                continue;
            CraftDagNode const& candidate = dag.nodes[index];
            bool ready = true;
            for (std::size_t inputIndex = 0; inputIndex < candidate.inputCount; ++inputIndex)
            {
                std::size_t const dependency = FindRecipeNode(dag, candidate.inputs[inputIndex].itemId);
                if (dependency != kInvalidIndex && !emitted[dependency])
                {
                    ready = false;
                    break;
                }
            }
            if (!ready)
                continue;
            if (selected == kInvalidIndex || candidate.outputItemId < dag.nodes[selected].outputItemId ||
                (candidate.outputItemId == dag.nodes[selected].outputItemId && candidate.recipeId < dag.nodes[selected].recipeId))
                selected = index;
        }
        if (selected == kInvalidIndex)
        {
            dag.failure = PlanBlockReason::DependencyCycle;
            return false;
        }
        emitted[selected] = true;
        dag.topologicalOrder[dag.topologicalCount++] = selected;
    }
    return true;
}

inline DependencyDag BuildDependencyDag(AutoWowOracleGatherCraftEconomyInput const& input,
    DemandSelection const& demand)
{
    DependencyDag dag;
    DagBuilder builder{input, dag, {}, 0, demand.priority};
    if (!builder.BuildItem(demand.itemId, demand.quantity))
        return dag;
    SortSourceNeeds(dag);
    if (!BuildTopologicalOrder(dag))
        return dag;
    dag.valid = true;
    return dag;
}

inline bool IsExpired(Tick now, Tick expiresTick)
{
    return now >= expiresTick;
}

inline bool IsNodeReservedForOther(ReservationSnapshot const& snapshot, NodeId nodeId, Guid ownerGuid, Tick now)
{
    for (std::size_t index = 0; index < snapshot.nodeCount; ++index)
    {
        NodeReservationFact const& reservation = snapshot.nodes[index];
        if (reservation.active && reservation.nodeId == nodeId && !IsExpired(now, reservation.expiresTick) &&
            reservation.ownerGuid != ownerGuid)
            return true;
    }
    return false;
}

inline bool IsItemReservedForOther(ReservationSnapshot const& snapshot, ItemId itemId, Guid ownerGuid, Tick now)
{
    for (std::size_t index = 0; index < snapshot.itemCount; ++index)
    {
        ItemReservationFact const& reservation = snapshot.items[index];
        if (reservation.active && reservation.itemId == itemId && !IsExpired(now, reservation.expiresTick) &&
            reservation.ownerGuid != ownerGuid)
            return true;
    }
    return false;
}

inline bool TryClaimNode(ReservationSnapshot& snapshot, NodeId nodeId, Guid ownerGuid,
    Tick now, Tick expiresTick, std::uint16_t priority)
{
    for (std::size_t index = 0; index < snapshot.nodeCount; ++index)
    {
        NodeReservationFact& reservation = snapshot.nodes[index];
        if (!reservation.active || IsExpired(now, reservation.expiresTick))
            continue;
        if (reservation.nodeId == nodeId)
            return reservation.ownerGuid == ownerGuid;
    }
    for (std::size_t index = 0; index < snapshot.nodeCount; ++index)
    {
        NodeReservationFact& reservation = snapshot.nodes[index];
        if (!reservation.active || IsExpired(now, reservation.expiresTick))
        {
            reservation = {true, nodeId, ownerGuid, expiresTick, priority};
            return true;
        }
    }
    if (snapshot.nodeCount >= snapshot.nodes.size())
        return false;
    snapshot.nodes[snapshot.nodeCount++] = {true, nodeId, ownerGuid, expiresTick, priority};
    return true;
}

inline bool TryClaimItem(ReservationSnapshot& snapshot, ItemId itemId, Guid ownerGuid,
    std::uint32_t quantity, Tick now, Tick expiresTick, std::uint16_t priority)
{
    for (std::size_t index = 0; index < snapshot.itemCount; ++index)
    {
        ItemReservationFact& reservation = snapshot.items[index];
        if (!reservation.active || IsExpired(now, reservation.expiresTick))
            continue;
        if (reservation.itemId == itemId)
        {
            if (reservation.ownerGuid != ownerGuid)
                return false;
            if (quantity > reservation.quantity)
                reservation.quantity = quantity;
            return true;
        }
    }
    for (std::size_t index = 0; index < snapshot.itemCount; ++index)
    {
        ItemReservationFact& reservation = snapshot.items[index];
        if (!reservation.active || IsExpired(now, reservation.expiresTick))
        {
            reservation = {true, itemId, ownerGuid, quantity, expiresTick, priority};
            return true;
        }
    }
    if (snapshot.itemCount >= snapshot.items.size())
        return false;
    snapshot.items[snapshot.itemCount++] = {true, itemId, ownerGuid, quantity, expiresTick, priority};
    return true;
}

inline bool AppendReservationClaim(AutoWowOracleGatherCraftEconomyPlan& plan,
    ReservationClaim const& claim)
{
    for (std::size_t index = 0; index < plan.reservationCount; ++index)
    {
        ReservationClaim& existing = plan.reservations[index];
        if (existing.kind == claim.kind && existing.nodeId == claim.nodeId && existing.itemId == claim.itemId &&
            existing.ownerGuid == claim.ownerGuid)
        {
            if (claim.quantity > existing.quantity)
                existing.quantity = claim.quantity;
            if (claim.expiresTick > existing.expiresTick)
                existing.expiresTick = claim.expiresTick;
            if (claim.priority > existing.priority)
                existing.priority = claim.priority;
            return true;
        }
    }
    if (plan.reservationCount >= plan.reservations.size())
        return false;
    plan.reservations[plan.reservationCount++] = claim;
    return true;
}

inline bool CanSpend(AutoWowOracleGatherCraftEconomyPlan const& plan,
    PlannerBudget const& budget, std::uint64_t spend)
{
    std::uint64_t const projected = SaturatingAdd64(plan.projectedSpend, spend);
    if (projected < plan.projectedSpend || projected > budget.maxSpend)
        return false;
    if (budget.goldAvailable < budget.goldReserve ||
        projected > budget.goldAvailable - budget.goldReserve)
        return false;
    return true;
}

inline IntentDomain IntentDomainFor(IntentKind kind)
{
    if (kind == IntentKind::GatherRoute)
        return IntentDomain::Navigation;
    if (kind == IntentKind::GatherSource)
        return IntentDomain::Gathering;
    return IntentDomain::CraftingEconomyItems;
}

inline bool AddIntent(AutoWowOracleGatherCraftEconomyInput const& input,
    AutoWowOracleGatherCraftEconomyPlan& plan, IntentKind kind, Guid targetGuid, NodeId nodeId,
    ItemId itemId, RecipeId recipeId, std::uint32_t quantity, std::uint16_t priority,
    std::uint64_t unitCost, PlanBlockReason& failure)
{
    if (plan.intentCount >= input.budget.maxIntents || plan.intentCount >= plan.intents.size())
    {
        failure = PlanBlockReason::BudgetExhausted;
        return false;
    }
    if (IsEconomyIntent(kind) && plan.economyActionsPlanned >= input.budget.maxEconomyActions)
    {
        failure = PlanBlockReason::BudgetExhausted;
        return false;
    }
    if (IsCraftIntent(kind) && SaturatingAdd32(plan.craftsPlanned, quantity) > input.budget.maxCrafts)
    {
        failure = PlanBlockReason::BudgetExhausted;
        return false;
    }
    RetrySchedule const retry = RetryForAttempt(input.budget, 0);
    if (!retry.allowed)
    {
        failure = PlanBlockReason::RetryLimit;
        return false;
    }

    PlannedIntent intent;
    intent.kind = kind;
    intent.domain = IntentDomainFor(kind);
    intent.targetGuid = targetGuid;
    intent.nodeId = nodeId;
    intent.itemId = itemId;
    intent.recipeId = recipeId;
    intent.quantity = quantity;
    intent.unitCost = unitCost;
    intent.priority = priority;
    intent.ttlTicks = input.budget.intentTtlTicks;
    intent.action = NativeActionName(kind);
    intent.qualifier = IntentQualifier(kind);
    intent.retry = retry;
    plan.intents[plan.intentCount++] = intent;
    if (IsGatherIntent(kind) && kind == IntentKind::GatherSource)
        ++plan.gatherNodesPlanned;
    if (IsCraftIntent(kind))
        plan.craftsPlanned = SaturatingAdd32(plan.craftsPlanned, quantity);
    if (IsEconomyIntent(kind))
        ++plan.economyActionsPlanned;
    return true;
}

struct GatherSearch
{
    bool found = false;
    std::size_t nodeIndex = kInvalidIndex;
    std::size_t routeIndex = kInvalidIndex;
    bool sawSkillFailure = false;
    bool sawToolFailure = false;
    bool sawRouteFailure = false;
    bool sawReservationConflict = false;
};

inline bool BetterGatherNode(GatherNodeFact const& candidate, LegalMovementRouteFact const& candidateRoute,
    GatherNodeFact const& current, LegalMovementRouteFact const& currentRoute)
{
    if (candidate.priority != current.priority)
        return candidate.priority > current.priority;
    if (candidateRoute.routeCost != currentRoute.routeCost)
        return candidateRoute.routeCost < currentRoute.routeCost;
    if (candidate.nodeId != current.nodeId)
        return candidate.nodeId < current.nodeId;
    return candidate.entry < current.entry;
}

inline GatherSearch FindBestGatherNode(AutoWowOracleGatherCraftEconomyInput const& input,
    ReservationSnapshot const& reservations, ItemId itemId, std::uint32_t mapId)
{
    GatherSearch search;
    for (std::size_t index = 0; index < input.gatherNodeCount; ++index)
    {
        GatherNodeFact const& node = input.gatherNodes[index];
        if (node.materialItemId != itemId || !node.available || !node.spawned || node.mapId != mapId)
            continue;

        std::size_t const skillIndex = FindSkill(input, node.profession);
        if (skillIndex == kInvalidIndex || !input.skills[skillIndex].learned ||
            input.skills[skillIndex].skill < node.requiredSkill)
        {
            search.sawSkillFailure = true;
            continue;
        }
        if (node.requiredToolId)
        {
            std::size_t const toolIndex = FindTool(input, node.requiredToolId);
            if (toolIndex == kInvalidIndex || !input.tools[toolIndex].owned || !input.tools[toolIndex].equipped ||
                input.tools[toolIndex].profession != node.profession ||
                input.tools[toolIndex].requiredSkill > input.skills[skillIndex].skill)
            {
                search.sawToolFailure = true;
                continue;
            }
        }

        std::size_t const routeIndex = FindRoute(input, node.nodeId);
        if (routeIndex == kInvalidIndex || !HasLegalMovement(input.gatherRoutes[routeIndex], mapId))
        {
            search.sawRouteFailure = true;
            continue;
        }
        if (IsNodeReservedForOther(reservations, node.nodeId, input.ownerGuid, input.tick))
        {
            search.sawReservationConflict = true;
            continue;
        }

        if (!search.found || BetterGatherNode(node, input.gatherRoutes[routeIndex],
            input.gatherNodes[search.nodeIndex], input.gatherRoutes[search.routeIndex]))
        {
            search.found = true;
            search.nodeIndex = index;
            search.routeIndex = routeIndex;
        }
    }
    return search;
}

inline bool PlanSourceNeed(AutoWowOracleGatherCraftEconomyInput const& input,
    ReservationSnapshot& reservations, AutoWowOracleGatherCraftEconomyPlan& plan, SourceNeed const& need)
{
    PlanBlockReason failure = PlanBlockReason::None;
    Tick const expiresTick = SaturatingAddTick(input.tick, input.budget.intentTtlTicks);
    if (IsItemReservedForOther(reservations, need.itemId, input.ownerGuid, input.tick) ||
        !TryClaimItem(reservations, need.itemId, input.ownerGuid, need.quantity, input.tick, expiresTick, need.priority) ||
        !AppendReservationClaim(plan, {ReservationKind::Item, 0, need.itemId, input.ownerGuid,
            need.quantity, expiresTick, need.priority}))
    {
        plan.receipt.reason = PlanBlockReason::ItemReservationConflict;
        return false;
    }

    std::uint32_t remaining = need.quantity;
    std::size_t const materialIndex = FindMaterial(input, need.itemId);
    MaterialFact const* material = materialIndex == kInvalidIndex ? nullptr : &input.materials[materialIndex];

    if (remaining && material && input.access.bankAvailable && material->bank)
    {
        std::uint32_t const quantity = remaining < material->bank ? remaining : material->bank;
        if (!AddIntent(input, plan, IntentKind::BankWithdraw, input.access.bankGuid, 0, need.itemId, 0,
            quantity, need.priority, 0, failure))
        {
            plan.receipt.reason = failure;
            return false;
        }
        remaining -= quantity;
    }
    if (remaining && material && input.access.mailboxAvailable && material->mail)
    {
        std::uint32_t const quantity = remaining < material->mail ? remaining : material->mail;
        if (!AddIntent(input, plan, IntentKind::MailReceive, input.access.mailboxGuid, 0, need.itemId, 0,
            quantity, need.priority, 0, failure))
        {
            plan.receipt.reason = failure;
            return false;
        }
        remaining -= quantity;
    }

    GatherSearch const gather = FindBestGatherNode(input, reservations, need.itemId, input.mapId);
    if (remaining && gather.found && plan.gatherNodesPlanned < input.budget.maxGatherNodes)
    {
        GatherNodeFact const& node = input.gatherNodes[gather.nodeIndex];
        if (!TryClaimNode(reservations, node.nodeId, input.ownerGuid, input.tick, expiresTick, need.priority) ||
            !AppendReservationClaim(plan, {ReservationKind::Node, node.nodeId, 0, input.ownerGuid,
                remaining, expiresTick, need.priority}))
        {
            plan.receipt.reason = PlanBlockReason::NodeReservationConflict;
            return false;
        }
        std::size_t const routeIntentIndex = plan.intentCount;
        if (!AddIntent(input, plan, IntentKind::GatherRoute, node.nodeId, node.nodeId, need.itemId, 0,
            0, need.priority, 0, failure) ||
            !AddIntent(input, plan, IntentKind::GatherSource, node.nodeId, node.nodeId, need.itemId, 0,
            remaining, need.priority, 0, failure))
        {
            plan.receipt.reason = failure;
            return false;
        }
        plan.intents[routeIntentIndex].route = input.gatherRoutes[gather.routeIndex];
        plan.intents[routeIntentIndex + 1].route = input.gatherRoutes[gather.routeIndex];
        plan.intents[routeIntentIndex].nodeEntry = node.entry;
        plan.intents[routeIntentIndex].nodeMapId = node.mapId;
        plan.intents[routeIntentIndex + 1].nodeEntry = node.entry;
        plan.intents[routeIntentIndex + 1].nodeMapId = node.mapId;
        remaining = 0;
    }

    bool goldLimited = false;
    if (remaining && material && input.access.vendorAvailable && material->vendorAvailable && material->vendorUnitPrice)
    {
        std::uint64_t const spend = SaturatingMultiply64(remaining, material->vendorUnitPrice);
        if (CanSpend(plan, input.budget, spend))
        {
            if (!AddIntent(input, plan, IntentKind::VendorBuy, input.access.vendorGuid, 0, need.itemId, 0,
                remaining, need.priority, material->vendorUnitPrice, failure))
            {
                plan.receipt.reason = failure;
                return false;
            }
            plan.projectedSpend = SaturatingAdd64(plan.projectedSpend, spend);
            remaining = 0;
        }
        else
            goldLimited = true;
    }
    if (remaining && material && input.access.auctionAvailable && material->auctionAvailable && material->auctionUnitPrice)
    {
        // The planner keeps the demand visible, but this module has no verified auction executor.
        // Do not project spend or treat this unsupported intent as completed; the shared adapter
        // marks it executor-missing and the shared contract returns ExecutorBlocked.
        if (AddIntent(input, plan, IntentKind::AuctionBuy, input.access.auctioneerGuid, 0, need.itemId, 0,
            remaining, need.priority, material->auctionUnitPrice, failure))
        {
            remaining = 0;
        }
        else
        {
            plan.receipt.reason = failure;
            return false;
        }
    }

    if (!remaining)
        return true;
    if (gather.sawReservationConflict)
        plan.receipt.reason = PlanBlockReason::NodeReservationConflict;
    else if (gather.sawToolFailure)
        plan.receipt.reason = PlanBlockReason::MissingTool;
    else if (gather.sawSkillFailure)
        plan.receipt.reason = PlanBlockReason::MissingSkill;
    else if (gather.sawRouteFailure)
        plan.receipt.reason = PlanBlockReason::NoLegalMovementRoute;
    else if (goldLimited)
        plan.receipt.reason = PlanBlockReason::GoldBudget;
    else
        plan.receipt.reason = PlanBlockReason::MissingMaterial;
    return false;
}

inline bool PlanCraftNodes(AutoWowOracleGatherCraftEconomyInput const& input,
    ReservationSnapshot& reservations, AutoWowOracleGatherCraftEconomyPlan& plan)
{
    for (std::size_t position = 0; position < plan.dependencyDag.topologicalCount; ++position)
    {
        CraftDagNode const& node = plan.dependencyDag.nodes[plan.dependencyDag.topologicalOrder[position]];
        if (SaturatingAdd32(plan.craftsPlanned, node.craftCount) > input.budget.maxCrafts)
        {
            plan.receipt.reason = PlanBlockReason::BudgetExhausted;
            return false;
        }
        Tick const expiresTick = SaturatingAddTick(input.tick, input.budget.intentTtlTicks);
        if (IsItemReservedForOther(reservations, node.outputItemId, input.ownerGuid, input.tick) ||
            !TryClaimItem(reservations, node.outputItemId, input.ownerGuid, node.craftCount,
                input.tick, expiresTick, plan.selectedPriority) ||
            !AppendReservationClaim(plan, {ReservationKind::Item, 0, node.outputItemId, input.ownerGuid,
                node.craftCount, expiresTick, plan.selectedPriority}))
        {
            plan.receipt.reason = PlanBlockReason::ItemReservationConflict;
            return false;
        }
        std::uint64_t const fee = SaturatingMultiply64(node.craftFee, node.craftCount);
        if (!CanSpend(plan, input.budget, fee))
        {
            plan.receipt.reason = PlanBlockReason::GoldBudget;
            return false;
        }
        PlanBlockReason failure = PlanBlockReason::None;
        if (!AddIntent(input, plan, IntentKind::Craft, 0, 0, node.outputItemId, node.recipeId,
            node.craftCount, plan.selectedPriority, node.craftFee, failure))
        {
            plan.receipt.reason = failure;
            return false;
        }
        plan.projectedSpend = SaturatingAdd64(plan.projectedSpend, fee);
    }
    return true;
}

inline bool TryAddOptionalIntent(AutoWowOracleGatherCraftEconomyInput const& input,
    AutoWowOracleGatherCraftEconomyPlan& plan, IntentKind kind, Guid targetGuid, ItemId itemId,
    std::uint32_t quantity, std::uint16_t priority)
{
    if (!quantity || plan.intentCount >= input.budget.maxIntents ||
        plan.economyActionsPlanned >= input.budget.maxEconomyActions)
        return false;
    PlanBlockReason ignored = PlanBlockReason::None;
    return AddIntent(input, plan, kind, targetGuid, 0, itemId, 0, quantity, priority, 0, ignored);
}

inline bool TryAddOptionalItemIntent(AutoWowOracleGatherCraftEconomyInput const& input,
    ReservationSnapshot& reservations, AutoWowOracleGatherCraftEconomyPlan& plan, IntentKind kind,
    Guid targetGuid, ItemId itemId, std::uint32_t quantity, std::uint16_t priority)
{
    Tick const expiresTick = SaturatingAddTick(input.tick, input.budget.intentTtlTicks);
    if (IsItemReservedForOther(reservations, itemId, input.ownerGuid, input.tick) ||
        !TryClaimItem(reservations, itemId, input.ownerGuid, quantity, input.tick, expiresTick, priority))
        return false;
    if (!TryAddOptionalIntent(input, plan, kind, targetGuid, itemId, quantity, priority))
        return false;
    return AppendReservationClaim(plan, {ReservationKind::Item, 0, itemId, input.ownerGuid,
        quantity, expiresTick, priority});
}

inline void PlanDisposition(AutoWowOracleGatherCraftEconomyInput const& input,
    ReservationSnapshot& reservations, AutoWowOracleGatherCraftEconomyPlan& plan,
    DemandSelection const& demand)
{
    // Mail delivery is demand-driven. It is emitted only from the observed inventory snapshot;
    // a later executor receipt must prove the item moved before another plan treats it as fulfilled.
    std::array<std::uint32_t, kMaxMaterials> mailed{};
    std::array<bool, kMaxGuildDemands> processedDemands{};
    for (std::size_t step = 0; step < input.guildDemandCount; ++step)
    {
        std::size_t selected = kInvalidIndex;
        for (std::size_t index = 0; index < input.guildDemandCount; ++index)
        {
            GuildDemandFact const& candidate = input.guildDemands[index];
            if (processedDemands[index] || !candidate.active || !candidate.itemId ||
                candidate.quantity <= candidate.fulfilled || !candidate.requesterGuid ||
                candidate.requesterGuid == input.ownerGuid || !input.access.mailboxAvailable)
                continue;
            if (selected == kInvalidIndex || BetterGuildDemand(candidate, input.guildDemands[selected]))
                selected = index;
        }
        if (selected == kInvalidIndex)
            break;
        processedDemands[selected] = true;
        GuildDemandFact const& guildDemand = input.guildDemands[selected];
        if (!guildDemand.active || !guildDemand.itemId || guildDemand.quantity <= guildDemand.fulfilled ||
            !guildDemand.requesterGuid || guildDemand.requesterGuid == input.ownerGuid || !input.access.mailboxAvailable)
            continue;
        std::size_t const materialIndex = FindMaterial(input, guildDemand.itemId);
        if (materialIndex == kInvalidIndex)
            continue;
        std::uint32_t const available = AvailableInventory(input, guildDemand.itemId);
        std::uint32_t const wanted = guildDemand.quantity - guildDemand.fulfilled;
        std::uint32_t const quantity = available < wanted ? available : wanted;
        if (quantity && TryAddOptionalItemIntent(input, reservations, plan, IntentKind::MailSend,
            guildDemand.requesterGuid, guildDemand.itemId, quantity, guildDemand.priority))
            mailed[materialIndex] = SaturatingAdd32(mailed[materialIndex], quantity);
    }

    // The disposal order is fixed: bank, vendor, auction. This prevents the fact array order from
    // changing the economic choice and keeps the policy conservative about market exposure.
    std::array<bool, kMaxMaterials> processedMaterials{};
    for (std::size_t step = 0; step < input.materialCount; ++step)
    {
        std::size_t index = kInvalidIndex;
        for (std::size_t candidateIndex = 0; candidateIndex < input.materialCount; ++candidateIndex)
        {
            if (processedMaterials[candidateIndex] || !input.materials[candidateIndex].itemId)
                continue;
            if (index == kInvalidIndex || input.materials[candidateIndex].itemId < input.materials[index].itemId)
                index = candidateIndex;
        }
        if (index == kInvalidIndex)
            break;
        processedMaterials[index] = true;
        MaterialFact const& material = input.materials[index];
        if (!material.itemId)
            continue;
        std::uint32_t available = AvailableInventory(input, material.itemId);
        available = available > mailed[index] ? available - mailed[index] : 0;
        std::uint32_t keep = material.itemId == demand.itemId ? demand.quantity : 0;
        if (available <= keep)
            continue;
        std::uint32_t const surplus = available - keep;
        if (input.access.bankAvailable)
        {
            TryAddOptionalItemIntent(input, reservations, plan, IntentKind::BankDeposit,
                input.access.bankGuid, material.itemId, surplus, demand.priority);
        }
        else if (input.access.vendorAvailable && material.vendorAvailable && material.vendorUnitPrice)
        {
            TryAddOptionalItemIntent(input, reservations, plan, IntentKind::VendorSell,
                input.access.vendorGuid, material.itemId, surplus, demand.priority);
        }
        else if (input.access.auctionAvailable && material.auctionAvailable && material.auctionUnitPrice && material.tradable)
        {
            TryAddOptionalItemIntent(input, reservations, plan, IntentKind::AuctionSell,
                input.access.auctioneerGuid, material.itemId, surplus, demand.priority);
        }
    }
}

inline void ClearPartialPlan(AutoWowOracleGatherCraftEconomyPlan& plan)
{
    plan.intentCount = 0;
    plan.reservationCount = 0;
    plan.gatherNodesPlanned = 0;
    plan.craftsPlanned = 0;
    plan.economyActionsPlanned = 0;
    plan.projectedSpend = 0;
}

inline AutoWowOracleGatherCraftEconomyPlan Plan(
    AutoWowOracleGatherCraftEconomyInput const& input)
{
    AutoWowOracleGatherCraftEconomyPlan plan;
    if (!ValidateFacts(input))
    {
        plan.receipt = {PlanStatus::Invalid, PlanBlockReason::InvalidFacts, 0, 0, 0};
        return plan;
    }
    if (!input.botAlive)
    {
        plan.receipt = {PlanStatus::Blocked, PlanBlockReason::BotUnavailable, 0, 0, 0};
        return plan;
    }
    if (input.inCombat)
    {
        plan.receipt = {PlanStatus::Blocked, PlanBlockReason::InCombat, 0, 0, 0};
        return plan;
    }

    DemandSelection const demand = SelectDemand(input);
    if (!demand.found)
    {
        plan.receipt = {PlanStatus::Blocked, PlanBlockReason::NoDemand, 0, 0, 0};
        return plan;
    }
    plan.selectedItemId = demand.itemId;
    plan.selectedQuantity = demand.quantity;
    plan.selectedPriority = demand.priority;
    plan.dependencyDag = BuildDependencyDag(input, demand);
    if (!plan.dependencyDag.valid)
    {
        plan.receipt = {PlanStatus::Blocked, plan.dependencyDag.failure, 0, 0, 0};
        return plan;
    }

    ReservationSnapshot workingReservations = input.reservations;
    Tick const expiresTick = SaturatingAddTick(input.tick, input.budget.intentTtlTicks);
    if (IsItemReservedForOther(workingReservations, demand.itemId, input.ownerGuid, input.tick) ||
        !TryClaimItem(workingReservations, demand.itemId, input.ownerGuid, demand.quantity,
            input.tick, expiresTick, demand.priority) ||
        !AppendReservationClaim(plan, {ReservationKind::Item, 0, demand.itemId, input.ownerGuid,
            demand.quantity, expiresTick, demand.priority}))
    {
        plan.receipt = {PlanStatus::Blocked, PlanBlockReason::ItemReservationConflict, 0, 0, 0};
        return plan;
    }

    for (std::size_t index = 0; index < plan.dependencyDag.sourceNeedCount; ++index)
    {
        if (!PlanSourceNeed(input, workingReservations, plan, plan.dependencyDag.sourceNeeds[index]))
        {
            PlanBlockReason const reason = plan.receipt.reason;
            ClearPartialPlan(plan);
            plan.receipt = {PlanStatus::Blocked, reason, 0, 0, 0};
            return plan;
        }
    }
    if (!PlanCraftNodes(input, workingReservations, plan))
    {
        PlanBlockReason const reason = plan.receipt.reason;
        ClearPartialPlan(plan);
        plan.receipt = {PlanStatus::Blocked, reason, 0, 0, 0};
        return plan;
    }

    PlanDisposition(input, workingReservations, plan, demand);
    plan.receipt = {PlanStatus::Planned, PlanBlockReason::None, plan.intentCount,
        plan.reservationCount, plan.projectedSpend};
    return plan;
}

#if AUTOWOW_ORACLE_GCE_HAS_CONTRACT
inline constexpr AutoWowOracle::OperationCode OperationFor(IntentKind kind)
{
    switch (kind)
    {
        case IntentKind::GatherRoute:
            return AutoWowOracle::OperationCode::GatherRoute;
        case IntentKind::GatherSource:
            return AutoWowOracle::OperationCode::GatherSource;
        case IntentKind::Craft:
            return AutoWowOracle::OperationCode::Craft;
        case IntentKind::BankWithdraw:
            return AutoWowOracle::OperationCode::BankWithdraw;
        case IntentKind::BankDeposit:
            return AutoWowOracle::OperationCode::BankDeposit;
        case IntentKind::MailReceive:
            return AutoWowOracle::OperationCode::MailReceive;
        case IntentKind::MailSend:
            return AutoWowOracle::OperationCode::MailSend;
        case IntentKind::VendorBuy:
            return AutoWowOracle::OperationCode::VendorBuy;
        case IntentKind::VendorSell:
            return AutoWowOracle::OperationCode::VendorSell;
        case IntentKind::AuctionBuy:
            return AutoWowOracle::OperationCode::AuctionBuy;
        case IntentKind::AuctionSell:
            return AutoWowOracle::OperationCode::AuctionSell;
    }
    return AutoWowOracle::OperationCode::Unknown;
}

inline AutoWowOracle::OracleCandidate ToOracleCandidate(PlannedIntent const& intent)
{
    AutoWowOracle::OracleCandidate candidate;
    candidate.domain = intent.domain == IntentDomain::Navigation ? AutoWowOracle::Domain::Navigation :
        intent.domain == IntentDomain::Gathering ? AutoWowOracle::Domain::Gathering :
        AutoWowOracle::Domain::CraftingEconomyItems;
    candidate.intent = intent.domain == IntentDomain::Navigation ? AutoWowOracle::IntentCode::Navigate :
        intent.domain == IntentDomain::Gathering ? AutoWowOracle::IntentCode::GatherSource :
        AutoWowOracle::IntentCode::CraftEconomyItems;
    candidate.operation = OperationFor(intent.kind);
    candidate.resource = intent.kind == IntentKind::GatherRoute ? AutoWowOracle::LeaseResource::Transition :
        AutoWowOracle::LeaseResource::QuestGather;
    candidate.classification = AutoWowOracle::IntentClassification::Persistent;
    candidate.priority = intent.priority;
    candidate.ttlTicks = intent.ttlTicks;
    candidate.targetGuid = intent.targetGuid;
    candidate.itemId = intent.itemId;
    candidate.action = intent.action;
    candidate.qualifier = intent.qualifier;
    candidate.available = true;
    // The policy still publishes deterministic plans for observability, but no gather, craft, or
    // economy executor is verified in this lane. Match-only gathering is enabled later by its
    // dedicated executor integration; until then every such candidate must remain blocked.
    candidate.executorAvailable = false;
    candidate.requiresBotAlive = intent.requiresBotAlive;
    candidate.requiresSameMap = intent.requiresSameMap;
    candidate.exclusiveSquadOwner = false;
    if (intent.kind == IntentKind::GatherRoute || intent.kind == IntentKind::GatherSource)
    {
        candidate.gather = {intent.nodeId != 0, intent.nodeId, intent.nodeEntry,
            intent.nodeMapId == 0 ? intent.route.mapId : intent.nodeMapId, intent.itemId, 0,
            intent.kind == IntentKind::GatherSource ? AutoWowOracle::GatherGoal::ObtainMaterial :
                                                     AutoWowOracle::GatherGoal::Unknown};
        candidate.route = {true, intent.route.nodeId, intent.route.mapId,
            intent.route.pathCalculated, intent.route.pathComplete,
            intent.route.usesOnlyLegalMovement, intent.route.endpointsLegal};
        if (!HasLegalMovement(intent.route, intent.route.mapId))
            candidate.executorAvailable = false;
        if (intent.kind == IntentKind::GatherSource && intent.nodeEntry == 0)
            candidate.executorAvailable = false;
    }
    if (candidate.operation == AutoWowOracle::OperationCode::Unknown)
        candidate.executorAvailable = false;
    return candidate;
}

inline bool TryToOracleCandidate(PlannedIntent const& intent,
    AutoWowOracle::OracleCandidate& candidate)
{
    candidate = ToOracleCandidate(intent);
    return candidate.available;
}
#endif
}

#endif
