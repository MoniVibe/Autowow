/*
 * Guarded, exact one-craft runtime adapter for AutoWow.
 */
#include "AutoWowCraftControl.h"

#include "AutoWowProfessionEconomyTelemetry.h"
#include "DBCStores.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>

namespace AutoWowCraftControl
{
namespace
{
using AutoWowProfessionEconomyTelemetry::IsCraftingSkill;
using AutoWowProfessionEconomyTelemetry::kMaxReagents;
using AutoWowProfessionEconomyTelemetry::ProfessionName;

inline constexpr std::string_view kSchema = "autowow.craft.control.v1";
inline constexpr std::uint32_t kPostconditionTimeoutMs = 15000;
inline constexpr std::size_t kPendingSlots = 256;

struct ReagentSnapshot
{
    std::uint32_t itemId = 0;
    std::uint32_t required = 0;
    std::uint32_t before = 0;
};

struct PendingCraft
{
    bool active = false;
    std::uint32_t botGuid = 0;
    std::uint32_t recipeSpellId = 0;
    std::uint32_t outputItemId = 0;
    std::uint32_t outputQuantity = 0;
    std::uint32_t outputBefore = 0;
    std::uint32_t issuedAtMs = 0;
    std::array<ReagentSnapshot, kMaxReagents> reagents{};
    std::size_t reagentCount = 0;
};

std::array<PendingCraft, kPendingSlots> pendingCrafts{};

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

std::string Error(std::uint32_t guid, std::uint32_t recipeSpellId, std::string_view code)
{
    std::ostringstream out;
    out << "{\"ok\":false,\"schema\":" << JsonString(kSchema)
        << ",\"order\":\"craft\",\"guid\":" << guid
        << ",\"recipe_spell_id\":" << recipeSpellId
        << ",\"error\":" << JsonString(code) << '}';
    return out.str();
}

PendingCraft* FindPending(std::uint32_t guid)
{
    for (PendingCraft& pending : pendingCrafts)
        if (pending.active && pending.botGuid == guid)
            return &pending;
    return nullptr;
}

PendingCraft* FindFreePending()
{
    for (PendingCraft& pending : pendingCrafts)
        if (!pending.active)
            return &pending;
    return nullptr;
}

void Clear(PendingCraft& pending)
{
    pending = {};
}

struct RecipeSkill
{
    std::uint32_t skillId = 0;
    std::uint32_t requiredSkill = 0;
    bool learned = false;
};

RecipeSkill ResolveRecipeSkill(Player const* bot, SpellInfo const* spellInfo)
{
    RecipeSkill selected;
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

std::uint32_t OutputQuantity(SpellEffectInfo const& effect, ItemTemplate const* itemTemplate)
{
    int32 const calculated = effect.CalcValue();
    std::uint32_t quantity = calculated > 0 ? static_cast<std::uint32_t>(calculated) : 1;
    std::uint32_t const maximum = itemTemplate ? itemTemplate->GetMaxStackSize() : 1;
    if (quantity < 1)
        quantity = 1;
    if (quantity > maximum && maximum > 0)
        quantity = maximum;
    return quantity;
}

bool HasDuplicateReagent(PendingCraft const& pending, std::uint32_t itemId)
{
    for (std::size_t index = 0; index < pending.reagentCount; ++index)
        if (pending.reagents[index].itemId == itemId)
            return true;
    return false;
}

bool ExactOutput(PendingCraft const& pending, std::uint32_t current)
{
    return static_cast<std::uint64_t>(current) ==
        static_cast<std::uint64_t>(pending.outputBefore) + pending.outputQuantity;
}

bool ExactReagents(PendingCraft const& pending, Player const* bot)
{
    if (!bot)
        return false;

    for (std::size_t index = 0; index < pending.reagentCount; ++index)
    {
        ReagentSnapshot const& reagent = pending.reagents[index];
        std::uint32_t const current = bot->GetItemCount(reagent.itemId, false);
        if (static_cast<std::uint64_t>(current) + reagent.required != reagent.before)
            return false;
    }
    return true;
}

bool InventoryDeltaImpossible(PendingCraft const& pending, Player const* bot)
{
    if (!bot)
        return true;

    std::uint32_t const output = bot->GetItemCount(pending.outputItemId, false);
    if (output < pending.outputBefore ||
        static_cast<std::uint64_t>(output) >
            static_cast<std::uint64_t>(pending.outputBefore) + pending.outputQuantity)
        return true;

    for (std::size_t index = 0; index < pending.reagentCount; ++index)
    {
        ReagentSnapshot const& reagent = pending.reagents[index];
        std::uint32_t const current = bot->GetItemCount(reagent.itemId, false);
        if (current > reagent.before ||
            static_cast<std::uint64_t>(current) + reagent.required < reagent.before)
            return true;
    }
    return false;
}

std::string PendingResponse(PendingCraft const& pending, Player const* bot,
                            std::uint32_t elapsedMs)
{
    std::uint32_t const output = bot ? bot->GetItemCount(pending.outputItemId, false) : 0;
    std::ostringstream out;
    out << "{\"ok\":true,\"schema\":" << JsonString(kSchema)
        << ",\"order\":\"craft\",\"guid\":" << pending.botGuid
        << ",\"recipe_spell_id\":" << pending.recipeSpellId
        << ",\"state\":\"pending\",\"pending\":true,\"completed\":false"
        << ",\"cast_accepted\":true,\"proof\":\"inventory_delta_pending\""
        << ",\"output_item_id\":" << pending.outputItemId
        << ",\"output_before\":" << pending.outputBefore
        << ",\"output_current\":" << output
        << ",\"output_expected_delta\":" << pending.outputQuantity
        << ",\"elapsed_ms\":" << elapsedMs << '}';
    return out.str();
}

std::string StatusResponse(PendingCraft& pending, Player const* bot)
{
    std::uint32_t const elapsedMs = getMSTimeDiff(pending.issuedAtMs, getMSTime());
    std::uint32_t const output = bot ? bot->GetItemCount(pending.outputItemId, false) : 0;
    bool const outputExact = ExactOutput(pending, output);
    bool const reagentsExact = ExactReagents(pending, bot);

    if (outputExact && reagentsExact)
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"schema\":" << JsonString(kSchema)
            << ",\"order\":\"craft-status\",\"guid\":" << pending.botGuid
            << ",\"recipe_spell_id\":" << pending.recipeSpellId
            << ",\"state\":\"completed\",\"pending\":false,\"completed\":true"
            << ",\"success\":true,\"proof\":\"exact_inventory_delta\""
            << ",\"output_item_id\":" << pending.outputItemId
            << ",\"output_before\":" << pending.outputBefore
            << ",\"output_current\":" << output
            << ",\"output_delta\":" << pending.outputQuantity
            << ",\"elapsed_ms\":" << elapsedMs << '}';
        Clear(pending);
        return out.str();
    }

    if (InventoryDeltaImpossible(pending, bot) || elapsedMs >= kPostconditionTimeoutMs)
    {
        std::ostringstream out;
        out << "{\"ok\":false,\"schema\":" << JsonString(kSchema)
            << ",\"order\":\"craft-status\",\"guid\":" << pending.botGuid
            << ",\"recipe_spell_id\":" << pending.recipeSpellId
            << ",\"state\":\"failed\",\"pending\":false,\"completed\":false"
            << ",\"error\":\"craft:postcondition_not_observed\""
            << ",\"output_item_id\":" << pending.outputItemId
            << ",\"output_before\":" << pending.outputBefore
            << ",\"output_current\":" << output
            << ",\"elapsed_ms\":" << elapsedMs << '}';
        Clear(pending);
        return out.str();
    }

    return PendingResponse(pending, bot, elapsedMs);
}
}

std::string Execute(Player* bot, PlayerbotAI* botAI, std::uint32_t recipeSpellId)
{
    std::uint32_t const guid = bot ? bot->GetGUID().GetCounter() : 0;
    if (!bot || !botAI || !bot->IsInWorld())
        return Error(guid, recipeSpellId, "craft:bot_not_online");
    if (!recipeSpellId)
        return Error(guid, recipeSpellId, "craft:recipe_spell_required");
    if (FindPending(guid))
        return Error(guid, recipeSpellId, "craft:pending");
    if (botAI->IsAutoWowPaused())
        return Error(guid, recipeSpellId, "craft:bot_paused");
    if (!bot->IsAlive())
        return Error(guid, recipeSpellId, "craft:bot_dead");
    if (bot->IsInCombat())
        return Error(guid, recipeSpellId, "craft:in_combat");
    if (!bot->HasSpell(recipeSpellId))
        return Error(guid, recipeSpellId, "craft:known_recipe_missing");

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(recipeSpellId);
    if (!spellInfo)
        return Error(guid, recipeSpellId, "craft:spell_missing");

    RecipeSkill const skill = ResolveRecipeSkill(bot, spellInfo);
    if (!skill.skillId || !skill.learned)
        return Error(guid, recipeSpellId, "craft:profession_missing");
    if (bot->GetSkillValue(skill.skillId) < skill.requiredSkill)
        return Error(guid, recipeSpellId, "craft:skill_insufficient");

    std::size_t createEffectCount = 0;
    std::size_t createEffectIndex = MAX_SPELL_EFFECTS;
    for (std::size_t index = 0; index < MAX_SPELL_EFFECTS; ++index)
    {
        std::uint32_t const effect = spellInfo->Effects[index].Effect;
        if (effect == SPELL_EFFECT_CREATE_RANDOM_ITEM)
            return Error(guid, recipeSpellId, "craft:random_output_unsupported");
        if (effect == SPELL_EFFECT_CREATE_ITEM_2)
            return Error(guid, recipeSpellId, "craft:secondary_output_unsupported");
        if (effect == SPELL_EFFECT_CREATE_ITEM)
        {
            ++createEffectCount;
            createEffectIndex = index;
        }
    }
    if (createEffectCount != 1)
        return Error(guid, recipeSpellId, "craft:deterministic_output_required");

    SpellEffectInfo const& outputEffect = spellInfo->Effects[createEffectIndex];
    std::uint32_t const outputItemId = outputEffect.ItemType;
    ItemTemplate const* outputTemplate = outputItemId ? sObjectMgr->GetItemTemplate(outputItemId) : nullptr;
    if (!outputItemId || !outputTemplate)
        return Error(guid, recipeSpellId, "craft:output_missing");

    std::uint32_t const outputQuantity = OutputQuantity(outputEffect, outputTemplate);
    ItemPosCountVec destinations;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, outputItemId, outputQuantity) != EQUIP_ERR_OK)
        return Error(guid, recipeSpellId, "craft:output_inventory_full");

    PendingCraft staged;
    staged.botGuid = guid;
    staged.recipeSpellId = recipeSpellId;
    staged.outputItemId = outputItemId;
    staged.outputQuantity = outputQuantity;
    staged.outputBefore = bot->GetItemCount(outputItemId, false);

    for (std::size_t index = 0; index < MAX_SPELL_REAGENTS; ++index)
    {
        int32 const reagentId = spellInfo->Reagent[index];
        std::uint32_t const required = spellInfo->ReagentCount[index];
        if (reagentId <= 0 && required == 0)
            continue;
        if (reagentId <= 0 || !required || staged.reagentCount >= kMaxReagents)
            return Error(guid, recipeSpellId, "craft:malformed_reagent");

        std::uint32_t const itemId = static_cast<std::uint32_t>(reagentId);
        if (itemId == outputItemId || HasDuplicateReagent(staged, itemId))
            return Error(guid, recipeSpellId, "craft:ambiguous_inventory_delta");

        ReagentSnapshot& reagent = staged.reagents[staged.reagentCount++];
        reagent.itemId = itemId;
        reagent.required = required;
        reagent.before = bot->GetItemCount(itemId, false);
        if (reagent.before < required)
            return Error(guid, recipeSpellId, "craft:missing_reagent");
    }

    if (!botAI->CanCastSpell(recipeSpellId, bot, true))
        return Error(guid, recipeSpellId, "craft:cast_rejected");

    PendingCraft* slot = FindFreePending();
    if (!slot)
        return Error(guid, recipeSpellId, "craft:pending_capacity");

    if (!botAI->CastSpell(recipeSpellId, bot))
        return Error(guid, recipeSpellId, "craft:cast_rejected");

    staged.active = true;
    staged.issuedAtMs = getMSTime();
    *slot = staged;
    return PendingResponse(*slot, bot, 0);
}

std::string Status(Player* bot)
{
    std::uint32_t const guid = bot ? bot->GetGUID().GetCounter() : 0;
    PendingCraft* pending = FindPending(guid);
    if (!pending)
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"schema\":" << JsonString(kSchema)
            << ",\"order\":\"craft-status\",\"guid\":" << guid
            << ",\"state\":\"idle\",\"pending\":false,\"completed\":false";
        if (!bot)
            out << ",\"error\":" << JsonString("craft:bot_not_online");
        out << '}';
        return out.str();
    }

    return StatusResponse(*pending, bot);
}
}
