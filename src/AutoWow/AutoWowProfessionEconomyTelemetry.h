/*
 * Read-only profession, crafting, and bag-economy telemetry for AutoWow.
 *
 * The pure data model and classifier in this file are deliberately independent of Player, the
 * bridge socket, SQL, and native actions. The runtime adapter in the .cpp file only observes an
 * already-enrolled, already-online bot on the world thread; it never learns skills, casts spells,
 * stores items, changes money, or writes a database row.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_PROFESSION_ECONOMY_TELEMETRY_H
#define MOD_PLAYERBOTS_AUTOWOW_PROFESSION_ECONOMY_TELEMETRY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

class Player;

namespace AutoWowProfessionEconomyTelemetry
{
inline constexpr std::string_view kSchema = "autowow.profession.economy.v1";
inline constexpr std::size_t kMaxProfessions = 16;
inline constexpr std::size_t kMaxRecipes = 128;
inline constexpr std::size_t kMaxReagents = 8;
inline constexpr std::size_t kMaxMaterials = 128;

// WotLK SkillLine.dbc ids. Keeping this table local makes the pure contract usable by tests that
// do not link the game core; the runtime adapter uses the same ids through this table.
struct ProfessionDefinition
{
    std::uint32_t skillId = 0;
    std::string_view name;
    bool crafting = false;
};

inline constexpr std::array<ProfessionDefinition, 14> kProfessionDefinitions = {{
    {171, "alchemy", true},
    {164, "blacksmithing", true},
    {185, "cooking", true},
    {333, "enchanting", true},
    {202, "engineering", true},
    {129, "first_aid", true},
    {182, "herbalism", false},
    {773, "inscription", true},
    {755, "jewelcrafting", true},
    {165, "leatherworking", true},
    {186, "mining", false},
    {393, "skinning", false},
    {197, "tailoring", true},
    {356, "fishing", false},
}};

inline constexpr ProfessionDefinition const* FindProfessionDefinition(std::uint32_t skillId)
{
    for (ProfessionDefinition const& definition : kProfessionDefinitions)
        if (definition.skillId == skillId)
            return &definition;
    return nullptr;
}

inline constexpr bool IsProfessionSkill(std::uint32_t skillId)
{
    return FindProfessionDefinition(skillId) != nullptr;
}

inline constexpr bool IsCraftingSkill(std::uint32_t skillId)
{
    ProfessionDefinition const* definition = FindProfessionDefinition(skillId);
    return definition && definition->crafting;
}

inline constexpr std::string_view ProfessionName(std::uint32_t skillId)
{
    ProfessionDefinition const* definition = FindProfessionDefinition(skillId);
    return definition ? definition->name : "unknown";
}

enum class RecipeBlockReason : std::uint8_t
{
    None = 0,
    MissingOutput,
    RandomOutputUnsupported,
    InactiveRecipeRank,
    MissingProfession,
    SkillInsufficient,
    OutputInventoryFull,
    MissingReagent,
    MalformedReagent,
};

inline constexpr std::string_view RecipeBlockReasonName(RecipeBlockReason reason)
{
    switch (reason)
    {
        case RecipeBlockReason::None: return "none";
        case RecipeBlockReason::MissingOutput: return "missing_output";
        case RecipeBlockReason::RandomOutputUnsupported: return "random_output_unsupported";
        case RecipeBlockReason::InactiveRecipeRank: return "inactive_recipe_rank";
        case RecipeBlockReason::MissingProfession: return "missing_profession";
        case RecipeBlockReason::SkillInsufficient: return "skill_insufficient";
        case RecipeBlockReason::OutputInventoryFull: return "output_inventory_full";
        case RecipeBlockReason::MissingReagent: return "missing_reagent";
        case RecipeBlockReason::MalformedReagent: return "malformed_reagent";
    }
    return "malformed_reagent";
}

struct ProfessionFact
{
    std::uint32_t skillId = 0;
    std::string name;
    std::uint16_t current = 0;
    std::uint16_t maximum = 0;
    std::uint16_t baseCurrent = 0;
    std::uint16_t baseMaximum = 0;
};

enum class ProfessionPlanStatus : std::uint8_t
{
    Unspecified = 0,
    Invalid,
    MissingProfession,
    Realized,
};

inline constexpr std::string_view ProfessionPlanStatusName(ProfessionPlanStatus status)
{
    switch (status)
    {
        case ProfessionPlanStatus::Unspecified: return "unspecified";
        case ProfessionPlanStatus::Invalid: return "invalid";
        case ProfessionPlanStatus::MissingProfession: return "missing_profession";
        case ProfessionPlanStatus::Realized: return "realized";
    }
    return "invalid";
}

// Read-only comparison between the controller's declared primary-profession pair and the
// professions actually observed on the character. Presence in the telemetry snapshot is the
// only evidence accepted here; this helper never infers a skill from a campaign plan and never
// learns, grants, or repairs a profession.
struct ProfessionPlanAssessment
{
    ProfessionPlanStatus status = ProfessionPlanStatus::Unspecified;
    std::array<std::string, 2> desired{};
    std::array<bool, 2> observed{};
    std::array<std::string, 2> missing{};
    std::size_t desiredCount = 0;
    std::size_t missingCount = 0;
};

[[nodiscard]] ProfessionPlanAssessment AssessProfessionPlan(
    std::string_view firstProfession, std::string_view secondProfession,
    std::array<ProfessionFact, kMaxProfessions> const& observedProfessions,
    std::size_t observedCount);

struct ReagentFact
{
    std::uint32_t itemId = 0;
    std::uint32_t requiredPerCraft = 0;
    std::uint32_t bagCount = 0;
    std::uint32_t craftsAvailable = 0;
};

struct RecipeFact
{
    std::uint32_t spellId = 0;
    std::string name;
    std::uint32_t skillId = 0;
    std::string profession;
    std::uint32_t requiredSkill = 0;
    std::uint32_t skillCurrent = 0;
    std::uint32_t skillMaximum = 0;
    std::uint32_t outputItemId = 0;
    std::uint32_t outputQuantity = 0;
    bool known = false;
    bool usable = false;
    bool deterministicOutput = true;
    bool outputCapacity = false;
    bool immediatelyCraftable = false;
    std::uint32_t maxCraftsByReagents = 0;
    bool reagentLimited = false;
    std::uint32_t limitingReagentItemId = 0;
    std::uint32_t limitingReagentBagCount = 0;
    RecipeBlockReason blockReason = RecipeBlockReason::MissingOutput;
    std::array<ReagentFact, kMaxReagents> reagents{};
    std::size_t reagentCount = 0;
};

struct MaterialFact
{
    std::uint32_t itemId = 0;
    std::uint64_t count = 0;
};

struct CraftEvidenceFact
{
    // There is currently no existing per-bot craft receipt/counter surface in Playerbots. Keep
    // this explicit so consumers cannot mistake absent evidence for zero successful crafts.
    bool available = false;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
    std::uint64_t failures = 0;
    std::string source = "none";
};

struct Snapshot
{
    bool ok = true;
    std::string error;
    std::uint32_t botGuid = 0;
    bool enrolled = false;
    bool online = false;
    bool playerbot = false;
    bool readOnly = true;
    std::uint32_t moneyCopper = 0;

    std::array<ProfessionFact, kMaxProfessions> professions{};
    std::size_t professionCount = 0;

    std::array<RecipeFact, kMaxRecipes> recipes{};
    std::size_t recipeCount = 0;
    std::size_t knownRecipeCount = 0;
    bool recipesTruncated = false;

    std::array<MaterialFact, kMaxMaterials> materials{};
    std::size_t materialCount = 0;
    std::size_t bagMaterialTypeCount = 0;
    bool materialsTruncated = false;

    CraftEvidenceFact craftEvidence;
};

struct RecipeClassificationInput
{
    bool hasProfession = false;
    bool usable = true;
    std::uint32_t currentSkill = 0;
    std::uint32_t requiredSkill = 0;
    std::uint32_t outputItemId = 0;
    bool outputItemValid = false;
    bool deterministicOutput = true;
    bool outputCapacity = false;
    std::array<ReagentFact, kMaxReagents> reagents{};
    std::size_t reagentCount = 0;
};

struct RecipeClassification
{
    bool immediatelyCraftable = false;
    std::uint32_t maxCraftsByReagents = 0;
    bool reagentLimited = false;
    std::uint32_t limitingReagentItemId = 0;
    std::uint32_t limitingReagentBagCount = 0;
    RecipeBlockReason reason = RecipeBlockReason::MissingOutput;
};

// Pure, deterministic recipe classifier shared by the runtime adapter and unit tests.
[[nodiscard]] RecipeClassification ClassifyRecipe(RecipeClassificationInput const& input) noexcept;

// Pure stable JSON serialization. The result is a telemetry snapshot only; it contains no
// command field and no mutation capability.
[[nodiscard]] std::string BuildJson(Snapshot const& snapshot);

// World-thread adapter. `enrolled` is supplied by the bridge's existing read-only league
// membership gate; this helper intentionally does not query SQL or own enrollment policy.
// Calling it for a non-enrolled or offline bot returns an error JSON object and no game state.
[[nodiscard]] std::string Build(Player* bot, bool enrolled);
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_PROFESSION_ECONOMY_TELEMETRY_H
