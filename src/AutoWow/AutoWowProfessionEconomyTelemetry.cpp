/*
 * Read-only profession, crafting, and bag-economy telemetry for AutoWow.
 */
#include "AutoWowProfessionEconomyTelemetry.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "Bag.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SharedDefines.h"

namespace AutoWowProfessionEconomyTelemetry
{
namespace
{
std::string JsonString(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f]
                        << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

std::size_t BoundedCount(std::size_t count, std::size_t capacity)
{
    return std::min(count, capacity);
}

std::string NormalizeProfessionName(std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char character : value)
    {
        if (std::isspace(character))
            continue;
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

bool IsKnownProfessionName(std::string const& normalized)
{
    for (ProfessionDefinition const& definition : kProfessionDefinitions)
        if (definition.name == normalized)
            return true;
    return false;
}

bool ObservedProfessionContains(
    std::array<ProfessionFact, kMaxProfessions> const& observedProfessions,
    std::size_t observedCount, std::string const& normalized)
{
    for (std::size_t index = 0; index < BoundedCount(observedCount, observedProfessions.size()); ++index)
        if (NormalizeProfessionName(observedProfessions[index].name) == normalized)
            return true;
    return false;
}

struct RecipeSkillChoice
{
    std::uint32_t skillId = 0;
    std::uint32_t requiredSkill = 0;
    bool learned = false;
};

RecipeSkillChoice ResolveRecipeSkill(Player const* bot, SpellInfo const* spellInfo)
{
    RecipeSkillChoice selected;
    if (!bot || !spellInfo)
        return selected;

    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
    for (SkillLineAbilityMap::const_iterator iterator = bounds.first; iterator != bounds.second;
         ++iterator)
    {
        SkillLineAbilityEntry const* ability = iterator->second;
        if (!ability || !IsCraftingSkill(ability->SkillLine))
            continue;

        bool const learned = bot->HasSkill(ability->SkillLine);
        bool const prefer = !selected.skillId ||
            (learned && !selected.learned) ||
            (learned == selected.learned && ability->SkillLine < selected.skillId);
        if (prefer)
        {
            selected.skillId = ability->SkillLine;
            selected.requiredSkill = ability->MinSkillLineRank;
            selected.learned = learned;
        }
    }
    return selected;
}

bool IsCreateEffect(std::uint32_t effect)
{
    return effect == SPELL_EFFECT_CREATE_ITEM || effect == SPELL_EFFECT_CREATE_ITEM_2 ||
        effect == SPELL_EFFECT_CREATE_RANDOM_ITEM;
}

void AddMaterial(MaterialFact const& material, std::map<std::uint32_t, std::uint64_t>& totals)
{
    if (!material.itemId || !material.count)
        return;
    totals[material.itemId] += material.count;
}

void AddBagMaterial(Item const* item, std::map<std::uint32_t, std::uint64_t>& totals)
{
    if (!item)
        return;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate ||
        (itemTemplate->Class != ITEM_CLASS_REAGENT && itemTemplate->Class != ITEM_CLASS_TRADE_GOODS))
        return;

    AddMaterial({item->GetEntry(), item->GetCount()}, totals);
}

void CollectBagMaterials(Player const* bot, Snapshot& snapshot)
{
    if (!bot)
        return;

    std::map<std::uint32_t, std::uint64_t> totals;
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddBagMaterial(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), totals);

    for (std::uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END;
         ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (std::uint32_t slot = 0; slot < bag->GetBagSize(); ++slot)
            AddBagMaterial(bag->GetItemByPos(static_cast<std::uint8_t>(slot)), totals);
    }

    snapshot.bagMaterialTypeCount = totals.size();
    snapshot.materialsTruncated = totals.size() > snapshot.materials.size();
    for (auto const& [itemId, count] : totals)
    {
        if (snapshot.materialCount >= snapshot.materials.size())
            break;
        snapshot.materials[snapshot.materialCount++] = {itemId, count};
    }
}

void CollectProfessions(Player const* bot, Snapshot& snapshot)
{
    if (!bot)
        return;

    for (ProfessionDefinition const& definition : kProfessionDefinitions)
    {
        std::uint16_t const current = bot->GetSkillValue(definition.skillId);
        std::uint16_t const maximum = bot->GetMaxSkillValue(definition.skillId);
        if (!bot->HasSkill(definition.skillId) && !current && !maximum)
            continue;

        if (snapshot.professionCount >= snapshot.professions.size())
            break;

        snapshot.professions[snapshot.professionCount++] = {
            definition.skillId,
            std::string(definition.name),
            current,
            maximum,
            bot->GetBaseSkillValue(definition.skillId),
            bot->GetPureMaxSkillValue(definition.skillId)};
    }
}

std::uint32_t OutputQuantity(SpellEffectInfo const& effect, ItemTemplate const* itemTemplate)
{
    int32 const calculated = effect.CalcValue();
    std::uint32_t quantity = calculated > 0 ? static_cast<std::uint32_t>(calculated) : 1;
    std::uint32_t const maximum = itemTemplate ? itemTemplate->GetMaxStackSize() : 1;
    return std::clamp(quantity, 1u, std::max(maximum, 1u));
}

RecipeFact MakeRecipe(Player const* bot, SpellInfo const* spellInfo, RecipeSkillChoice const& skill,
                      std::size_t effectIndex, bool usable)
{
    RecipeFact recipe;
    recipe.spellId = spellInfo ? spellInfo->Id : 0;
    recipe.name = spellInfo && spellInfo->SpellName[0] ? spellInfo->SpellName[0] : "";
    recipe.skillId = skill.skillId;
    recipe.profession = std::string(ProfessionName(skill.skillId));
    recipe.requiredSkill = skill.requiredSkill;
    recipe.skillCurrent = bot ? bot->GetSkillValue(skill.skillId) : 0;
    recipe.skillMaximum = bot ? bot->GetMaxSkillValue(skill.skillId) : 0;
    recipe.known = true;
    recipe.usable = usable;

    SpellEffectInfo const& effect = spellInfo->Effects[effectIndex];
    recipe.deterministicOutput = effect.Effect == SPELL_EFFECT_CREATE_ITEM;
    recipe.outputItemId = effect.ItemType;
    ItemTemplate const* itemTemplate = recipe.outputItemId
        ? sObjectMgr->GetItemTemplate(recipe.outputItemId)
        : nullptr;
    recipe.outputQuantity = OutputQuantity(effect, itemTemplate);
    recipe.outputCapacity = recipe.deterministicOutput && itemTemplate;

    if (recipe.outputCapacity && bot)
    {
        ItemPosCountVec destinations;
        recipe.outputCapacity = bot->CanStoreNewItem(
            NULL_BAG, NULL_SLOT, destinations, recipe.outputItemId, recipe.outputQuantity) == EQUIP_ERR_OK;
    }

    for (std::size_t index = 0; index < MAX_SPELL_REAGENTS && recipe.reagentCount < kMaxReagents;
         ++index)
    {
        int32 const reagentId = spellInfo->Reagent[index];
        std::uint32_t const required = spellInfo->ReagentCount[index];
        if (reagentId <= 0 && required == 0)
            continue;

        ReagentFact& reagent = recipe.reagents[recipe.reagentCount++];
        reagent.itemId = reagentId > 0 ? static_cast<std::uint32_t>(reagentId) : 0;
        reagent.requiredPerCraft = required;
        reagent.bagCount = bot && reagent.itemId ? bot->GetItemCount(reagent.itemId, false) : 0;
        reagent.craftsAvailable = reagent.requiredPerCraft
            ? reagent.bagCount / reagent.requiredPerCraft
            : 0;
    }

    RecipeClassificationInput input;
    input.hasProfession = bot && skill.learned;
    input.usable = recipe.usable;
    input.currentSkill = recipe.skillCurrent;
    input.requiredSkill = recipe.requiredSkill;
    input.outputItemId = recipe.outputItemId;
    input.outputItemValid = itemTemplate != nullptr;
    input.deterministicOutput = recipe.deterministicOutput;
    input.outputCapacity = recipe.outputCapacity;
    input.reagentCount = recipe.reagentCount;
    input.reagents = recipe.reagents;

    RecipeClassification const classification = ClassifyRecipe(input);
    recipe.immediatelyCraftable = classification.immediatelyCraftable;
    recipe.maxCraftsByReagents = classification.maxCraftsByReagents;
    recipe.reagentLimited = classification.reagentLimited;
    recipe.limitingReagentItemId = classification.limitingReagentItemId;
    recipe.limitingReagentBagCount = classification.limitingReagentBagCount;
    recipe.blockReason = classification.reason;
    return recipe;
}

void CollectRecipes(Player const* bot, Snapshot& snapshot)
{
    if (!bot)
        return;

    std::vector<RecipeFact> recipes;
    recipes.reserve(32);
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        RecipeSkillChoice const skill = ResolveRecipeSkill(bot, spellInfo);
        if (!skill.skillId)
            continue;

        std::size_t outputEffect = MAX_SPELL_EFFECTS;
        for (std::size_t index = 0; index < MAX_SPELL_EFFECTS; ++index)
        {
            if (IsCreateEffect(spellInfo->Effects[index].Effect))
            {
                outputEffect = index;
                break;
            }
        }
        if (outputEffect == MAX_SPELL_EFFECTS)
            continue;

        recipes.push_back(MakeRecipe(bot, spellInfo, skill, outputEffect, playerSpell->Active));
    }

    std::sort(recipes.begin(), recipes.end(), [](RecipeFact const& left, RecipeFact const& right)
    {
        if (left.skillId != right.skillId)
            return left.skillId < right.skillId;
        return left.spellId < right.spellId;
    });

    snapshot.knownRecipeCount = recipes.size();
    snapshot.recipesTruncated = recipes.size() > snapshot.recipes.size();
    snapshot.recipeCount = BoundedCount(recipes.size(), snapshot.recipes.size());
    for (std::size_t index = 0; index < snapshot.recipeCount; ++index)
        snapshot.recipes[index] = std::move(recipes[index]);
}
}

RecipeClassification ClassifyRecipe(RecipeClassificationInput const& input) noexcept
{
    RecipeClassification result;
    result.reagentLimited = input.reagentCount != 0;

    if (!input.deterministicOutput)
    {
        result.reason = RecipeBlockReason::RandomOutputUnsupported;
        return result;
    }
    if (!input.outputItemId || !input.outputItemValid)
    {
        result.reason = RecipeBlockReason::MissingOutput;
        return result;
    }
    if (!input.hasProfession)
    {
        result.reason = RecipeBlockReason::MissingProfession;
        return result;
    }
    if (input.currentSkill < input.requiredSkill)
    {
        result.reason = RecipeBlockReason::SkillInsufficient;
        return result;
    }
    if (!input.outputCapacity)
    {
        result.reason = RecipeBlockReason::OutputInventoryFull;
        return result;
    }
    if (!input.usable)
    {
        result.reason = RecipeBlockReason::InactiveRecipeRank;
        return result;
    }
    if (input.reagentCount > kMaxReagents)
    {
        result.reason = RecipeBlockReason::MalformedReagent;
        return result;
    }

    result.reason = RecipeBlockReason::None;
    std::uint32_t minimumCrafts = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0; index < input.reagentCount; ++index)
    {
        ReagentFact const& reagent = input.reagents[index];
        if (!reagent.itemId || !reagent.requiredPerCraft)
        {
            result.reason = RecipeBlockReason::MalformedReagent;
            return result;
        }

        std::uint32_t const available = reagent.bagCount / reagent.requiredPerCraft;
        if (available < minimumCrafts ||
            (available == minimumCrafts && reagent.itemId < result.limitingReagentItemId))
        {
            minimumCrafts = available;
            result.limitingReagentItemId = reagent.itemId;
            result.limitingReagentBagCount = reagent.bagCount;
        }
        if (reagent.bagCount < reagent.requiredPerCraft)
            result.reason = RecipeBlockReason::MissingReagent;
    }

    if (input.reagentCount)
    {
        result.maxCraftsByReagents = minimumCrafts;
        if (result.maxCraftsByReagents == 0)
        {
            result.reason = RecipeBlockReason::MissingReagent;
            return result;
        }
    }

    if (result.reason == RecipeBlockReason::MissingReagent)
        return result;

    result.immediatelyCraftable = true;
    result.reason = RecipeBlockReason::None;
    return result;
}

ProfessionPlanAssessment AssessProfessionPlan(
    std::string_view firstProfession, std::string_view secondProfession,
    std::array<ProfessionFact, kMaxProfessions> const& observedProfessions,
    std::size_t observedCount)
{
    ProfessionPlanAssessment assessment;
    std::array<std::string_view, 2> const requested = {firstProfession, secondProfession};

    for (std::string_view profession : requested)
    {
        std::string const normalized = NormalizeProfessionName(profession);
        if (normalized.empty())
            continue;

        if (assessment.desiredCount >= assessment.desired.size())
        {
            assessment.status = ProfessionPlanStatus::Invalid;
            return assessment;
        }

        assessment.desired[assessment.desiredCount++] = normalized;
    }

    if (assessment.desiredCount == 0)
        return assessment;

    if (assessment.desiredCount != requested.size() ||
        !IsKnownProfessionName(assessment.desired[0]) ||
        !IsKnownProfessionName(assessment.desired[assessment.desiredCount - 1]) ||
        (assessment.desiredCount == 2 && assessment.desired[0] == assessment.desired[1]))
    {
        assessment.status = ProfessionPlanStatus::Invalid;
        return assessment;
    }

    for (std::size_t index = 0; index < assessment.desiredCount; ++index)
    {
        assessment.observed[index] = ObservedProfessionContains(
            observedProfessions, observedCount, assessment.desired[index]);
        if (!assessment.observed[index])
            assessment.missing[assessment.missingCount++] = assessment.desired[index];
    }

    assessment.status = assessment.missingCount == 0
        ? ProfessionPlanStatus::Realized
        : ProfessionPlanStatus::MissingProfession;
    return assessment;
}

std::string BuildJson(Snapshot const& snapshot)
{
    std::ostringstream out;
    out << "{\"ok\":" << (snapshot.ok ? "true" : "false")
        << ",\"schema\":" << JsonString(kSchema)
        << ",\"read_only\":" << (snapshot.readOnly ? "true" : "false")
        << ",\"guid\":" << snapshot.botGuid
        << ",\"enrolled\":" << (snapshot.enrolled ? "true" : "false")
        << ",\"online\":" << (snapshot.online ? "true" : "false")
        << ",\"playerbot\":" << (snapshot.playerbot ? "true" : "false");
    if (!snapshot.error.empty())
        out << ",\"error\":" << JsonString(snapshot.error);

    out << ",\"money_copper\":" << snapshot.moneyCopper << ",\"professions\":[";
    bool first = true;
    for (std::size_t index = 0; index < BoundedCount(snapshot.professionCount, snapshot.professions.size());
         ++index)
    {
        ProfessionFact const& profession = snapshot.professions[index];
        if (!first)
            out << ',';
        first = false;
        out << "{\"skill_id\":" << profession.skillId
            << ",\"name\":" << JsonString(profession.name)
            << ",\"current\":" << profession.current
            << ",\"maximum\":" << profession.maximum
            << ",\"base_current\":" << profession.baseCurrent
            << ",\"base_maximum\":" << profession.baseMaximum << '}';
    }
    out << "]";

    out << ",\"recipes\":{\"known_count\":" << snapshot.knownRecipeCount
        << ",\"returned_count\":" << snapshot.recipeCount
        << ",\"truncated\":" << (snapshot.recipesTruncated ? "true" : "false")
        << ",\"items\":[";
    first = true;
    for (std::size_t index = 0; index < BoundedCount(snapshot.recipeCount, snapshot.recipes.size());
         ++index)
    {
        RecipeFact const& recipe = snapshot.recipes[index];
        if (!first)
            out << ',';
        first = false;
        out << "{\"spell_id\":" << recipe.spellId
            << ",\"name\":" << JsonString(recipe.name)
            << ",\"skill_id\":" << recipe.skillId
            << ",\"profession\":" << JsonString(recipe.profession)
            << ",\"required_skill\":" << recipe.requiredSkill
            << ",\"skill_current\":" << recipe.skillCurrent
            << ",\"skill_maximum\":" << recipe.skillMaximum
            << ",\"output_item_id\":" << recipe.outputItemId
            << ",\"output_quantity\":" << recipe.outputQuantity
            << ",\"known\":" << (recipe.known ? "true" : "false")
            << ",\"usable\":" << (recipe.usable ? "true" : "false")
            << ",\"deterministic_output\":" << (recipe.deterministicOutput ? "true" : "false")
            << ",\"output_capacity\":" << (recipe.outputCapacity ? "true" : "false")
            << ",\"immediately_craftable\":"
            << (recipe.immediatelyCraftable ? "true" : "false")
            << ",\"max_crafts_by_reagents\":" << recipe.maxCraftsByReagents
            << ",\"reagent_limited\":" << (recipe.reagentLimited ? "true" : "false")
            << ",\"limiting_reagent_item_id\":" << recipe.limitingReagentItemId
            << ",\"limiting_reagent_bag_count\":" << recipe.limitingReagentBagCount
            << ",\"block_reason\":" << JsonString(RecipeBlockReasonName(recipe.blockReason))
            << ",\"reagents\":[";

        bool firstReagent = true;
        for (std::size_t reagentIndex = 0;
             reagentIndex < BoundedCount(recipe.reagentCount, recipe.reagents.size()); ++reagentIndex)
        {
            ReagentFact const& reagent = recipe.reagents[reagentIndex];
            if (!firstReagent)
                out << ',';
            firstReagent = false;
            out << "{\"item_id\":" << reagent.itemId
                << ",\"required_per_craft\":" << reagent.requiredPerCraft
                << ",\"bag_count\":" << reagent.bagCount
                << ",\"crafts_available\":" << reagent.craftsAvailable << '}';
        }
        out << "]}";
    }
    out << "]}";

    out << ",\"bag_materials\":{\"type_count\":" << snapshot.bagMaterialTypeCount
        << ",\"returned_count\":" << snapshot.materialCount
        << ",\"truncated\":" << (snapshot.materialsTruncated ? "true" : "false")
        << ",\"items\":[";
    first = true;
    for (std::size_t index = 0; index < BoundedCount(snapshot.materialCount, snapshot.materials.size());
         ++index)
    {
        MaterialFact const& material = snapshot.materials[index];
        if (!first)
            out << ',';
        first = false;
        out << "{\"item_id\":" << material.itemId << ",\"count\":" << material.count << '}';
    }
    out << "]}";

    out << ",\"craft_evidence\":{\"available\":"
        << (snapshot.craftEvidence.available ? "true" : "false")
        << ",\"attempts\":" << snapshot.craftEvidence.attempts
        << ",\"successes\":" << snapshot.craftEvidence.successes
        << ",\"failures\":" << snapshot.craftEvidence.failures
        << ",\"source\":" << JsonString(snapshot.craftEvidence.source) << "}}";
    return out.str();
}

std::string Build(Player* bot, bool enrolled)
{
    Snapshot snapshot;
    snapshot.enrolled = enrolled;
    snapshot.botGuid = bot ? bot->GetGUID().GetCounter() : 0;

    if (!enrolled)
    {
        snapshot.ok = false;
        snapshot.error = "bot_not_enrolled_in_league";
        return BuildJson(snapshot);
    }
    if (!bot)
    {
        snapshot.ok = false;
        snapshot.error = "bot_not_online";
        return BuildJson(snapshot);
    }

    snapshot.online = bot->IsInWorld();
    snapshot.playerbot = PlayerbotsMgr::instance().GetPlayerbotAI(bot) != nullptr;
    if (!snapshot.playerbot)
    {
        snapshot.ok = false;
        snapshot.error = "not_a_playerbot";
        return BuildJson(snapshot);
    }

    snapshot.moneyCopper = bot->GetMoney();
    CollectProfessions(bot, snapshot);
    CollectRecipes(bot, snapshot);
    CollectBagMaterials(bot, snapshot);
    return BuildJson(snapshot);
}
}
