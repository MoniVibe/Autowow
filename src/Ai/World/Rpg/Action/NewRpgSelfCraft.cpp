/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.SelfCraft runtime (policy: AutoWow/SelfCraftPolicy.h): cohort adventurers bandage themselves out of
// combat and craft bandages / cook food from their own loot. One NewRpg status-update step (map thread).

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "DBCStores.h"
#include "ErrandsPolicy.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SelfCraftPolicy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SupplyPolicy.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowSelfCraft
{
namespace
{
// Bot AI updates run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;
std::vector<SmeltRecipe> gSmelts;
bool gSmeltsLoaded = false;

BotState LoadState(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    return it == gStates.end() ? BotState{} : it->second;
}

void StoreState(std::uint32_t guid, BotState const& s)
{
    std::lock_guard<std::mutex> guard(gLock);
    gStates[guid] = s;
}

std::uint64_t NowMs()
{
    return static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
}

// A cohort adventurer: an independent AutoWoW bot, not Oracle-managed, not a supply rep / artisan.
bool Adventurer(PlayerbotAI* botAI, Player* bot)
{
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    return botAI && botAI->IsAutoWowIndependentParty() && !AutoWowOracleRuntime::IsManagedBot(guid) &&
           !(AutoWowSupply::Enabled() && AutoWowSupply::ActiveRoleOf(bot).role != AutoWowSupply::Role::None);
}

bool SmeltAdmitted(PlayerbotAI* botAI, Player* bot, SmeltOwner owner)
{
    if (!SmeltingOn() || !botAI || !bot || owner == SmeltOwner::None || !bot->IsAlive() || bot->IsInCombat() ||
        bot->IsInFlight() || bot->IsBeingTeleported() || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster() ||
        botAI->IsAutoWowPaused() || bot->GetGroup() || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        bot->GetTransport() || bot->GetVehicle() || bot->GetVehicleBase() || bot->IsMounted() ||
        !botAI->CanMove() || bot->IsRooted() || bot->HasStunAura() ||
        AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter()) || !bot->HasSkill(kSkillMining))
        return false;
    if (owner == SmeltOwner::SelfCraft)
        return Adventurer(botAI, bot) && botAI->rpgInfo.GetStatus() == RPG_IDLE &&
               !(AutoWowErrands::Enabled() && AutoWowErrands::Active(bot->GetGUID().GetCounter())) &&
               !(AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(bot->GetGUID().GetCounter()));
    return AutoWowSupply::Enabled() &&
           AutoWowSupply::ActiveRoleOf(bot).role != AutoWowSupply::Role::None;
}

bool HardProtected(ItemUsage usage)
{
    return usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_QUEST ||
           usage == ITEM_USAGE_USE || usage == ITEM_USAGE_GUILD_TASK || usage == ITEM_USAGE_AMMO;
}

bool HeldDonation(Player* bot, std::uint32_t entry)
{
    auto held = [&](Item* item)
    {
        return item && item->GetEntry() == entry &&
               AutoWowSupply::HeldForDonation(static_cast<std::uint32_t>(item->GetGUID().GetCounter()));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (held(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            return true;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* inventory = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < inventory->GetBagSize(); ++slot)
                if (held(inventory->GetItemByPos(slot)))
                    return true;
    return false;
}

void LoadSmeltCatalog()
{
    if (gSmeltsLoaded)
        return;
    gSmeltsLoaded = true;
    for (SkillLineAbilityEntry const* ability : sSkillLineAbilityStore)
    {
        if (!ability || ability->SkillLine != kSkillMining)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!spell || spell->RequiresSpellFocus != kForgeFocus)
            continue;
        std::size_t outputIndex = MAX_SPELL_EFFECTS;
        std::size_t outputEffects = 0;
        bool unsupported = false;
        for (std::size_t i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
            {
                outputIndex = i;
                ++outputEffects;
            }
            else if (spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM_2 ||
                     spell->Effects[i].Effect == SPELL_EFFECT_CREATE_RANDOM_ITEM)
                unsupported = true;
        }
        if (unsupported || outputEffects != 1)
            continue;
        SpellEffectInfo const& effect = spell->Effects[outputIndex];
        ItemTemplate const* output = effect.ItemType ? sObjectMgr->GetItemTemplate(effect.ItemType) : nullptr;
        if (!output || effect.DieSides > 1 || effect.RealPointsPerLevel != 0.0f || effect.PointsPerComboPoint != 0.0f)
            continue;
        int32 const calculated = effect.CalcValue();
        std::uint32_t quantity = calculated > 0 ? static_cast<std::uint32_t>(calculated) : 1;
        quantity = std::min(quantity, std::max<std::uint32_t>(1, output->GetMaxStackSize()));
        SmeltRecipe recipe;
        recipe.spell = spell->Id;
        recipe.output = output->ItemId;
        recipe.outputCount = quantity;
        recipe.greyAt = ability->TrivialSkillLineRankHigh;
        bool valid = true;
        for (std::size_t i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 const reagentId = spell->Reagent[i];
            std::uint32_t const required = spell->ReagentCount[i];
            if (reagentId <= 0 && !required)
                continue;
            if (reagentId <= 0 || !required || recipe.reagentCount >= kMaxSmeltReagents)
            {
                valid = false;
                break;
            }
            std::uint32_t const item = static_cast<std::uint32_t>(reagentId);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto || item == recipe.output)
            {
                valid = false;
                break;
            }
            for (std::size_t r = 0; r < recipe.reagentCount; ++r)
                if (recipe.reagents[r].item == item)
                    valid = false;
            if (!valid)
                break;
            recipe.reagents[recipe.reagentCount++] = {item, required};
            recipe.reagentValue += static_cast<std::uint64_t>(proto->SellPrice) * required;
        }
        if (valid && recipe.reagentCount)
            gSmelts.push_back(recipe);
    }
    std::sort(gSmelts.begin(), gSmelts.end(), [](SmeltRecipe const& a, SmeltRecipe const& b)
              { return a.spell < b.spell; });
    gSmelts.erase(std::unique(gSmelts.begin(), gSmelts.end(), [](SmeltRecipe const& a, SmeltRecipe const& b)
                              { return a.spell == b.spell; }),
                  gSmelts.end());
    LOG_INFO("playerbots", "[SelfCraft] native smelting catalog recipes={}", gSmelts.size());
}

std::vector<SmeltOption> SmeltOptions(PlayerbotAI* botAI, Player* bot, bool focusReady)
{
    std::vector<SmeltOption> options;
    options.reserve(gSmelts.size());
    auto* context = botAI->GetAiObjectContext();
    for (SmeltRecipe const& recipe : gSmelts)
    {
        ItemUsage const outputUsage = context->GetValue<ItemUsage>("item usage", recipe.output)->Get();
        bool const material = outputUsage == ITEM_USAGE_SKILL || outputUsage == ITEM_USAGE_KEEP;
        SmeltOption option;
        option.spell = recipe.spell;
        option.reagentValue = recipe.reagentValue;
        option.known = bot->HasSpell(recipe.spell);
        option.reagentsReady = true;
        option.materialWanted = material;
        option.skillUp = ItemUsageValue::SpellGivesSkillUp(recipe.spell, bot);
        option.focusReady = focusReady;
        ItemPosCountVec destinations;
        option.outputRoom =
            bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, recipe.output, recipe.outputCount) == EQUIP_ERR_OK;
        for (std::size_t i = 0; i < recipe.reagentCount; ++i)
        {
            SmeltReagent const& reagent = recipe.reagents[i];
            option.reagentsReady = option.reagentsReady &&
                bot->GetItemCount(reagent.item, false) >= reagent.count;
            ItemUsage const usage = context->GetValue<ItemUsage>("item usage", reagent.item)->Get();
            option.protectedReagent = option.protectedReagent || HardProtected(usage) || HeldDonation(bot, reagent.item) ||
                (!material && (usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_KEEP));
        }
        options.push_back(option);
    }
    return options;
}

SmeltRecipe const* PickRuntimeSmelt(PlayerbotAI* botAI, Player* bot, bool requireFocus)
{
    std::vector<SmeltOption> const options = SmeltOptions(botAI, bot, requireFocus);
    int const pick = PickSmelt(options, requireFocus);
    return pick < 0 ? nullptr : &gSmelts[static_cast<std::size_t>(pick)];
}

void ClearPendingSmelt(BotState& state)
{
    state.smeltSpell = 0;
    state.smeltOutput = 0;
    state.smeltOutputCount = 0;
    state.smeltOutputBefore = 0;
    state.smeltSkillBefore = 0;
    state.smeltIssuedMs = 0;
    state.smeltReagentCount = 0;
    state.smeltReagents = {};
    state.smeltReagentBefore = {};
}

void EndSmeltJob(BotState& state, std::uint64_t now)
{
    state.nextSmeltMs = now + detail::gParams.smeltingCheckIntervalMs;
    state.smeltSinceMs = 0;
    state.smeltMoveSinceMs = 0;
    state.smeltMove = {};
    state.smeltCasts = 0;
    state.smeltActive = false;
    state.smeltOwner = SmeltOwner::None;
}

std::string SmeltFields(BotState const& state, Player* bot, SmeltReceipt receipt)
{
    std::string fields = ",\"result\":\"" + std::string(SmeltReceiptName(receipt)) + "\"" +
        ",\"skill_before\":" + std::to_string(state.smeltSkillBefore) + ",\"skill_after\":" +
        std::to_string(bot->GetPureSkillValue(kSkillMining)) + ",\"output_before\":" +
        std::to_string(state.smeltOutputBefore) + ",\"output_after\":" +
        std::to_string(bot->GetItemCount(state.smeltOutput, false));
    for (std::size_t i = 0; i < state.smeltReagentCount; ++i)
    {
        std::string const n = std::to_string(i);
        fields += ",\"reagent_" + n + "\":" + std::to_string(state.smeltReagents[i].item) +
            ",\"reagent_" + n + "_before\":" + std::to_string(state.smeltReagentBefore[i]) +
            ",\"reagent_" + n + "_after\":" +
            std::to_string(bot->GetItemCount(state.smeltReagents[i].item, false)) +
            ",\"reagent_" + n + "_required\":" + std::to_string(state.smeltReagents[i].count);
    }
    return fields;
}

GameObject* NearbyForge(PlayerbotAI* botAI, Player* bot)
{
    GameObject* nearest = nullptr;
    float distance = 0.0f;
    GuidVector const objects =
        botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects")->Get();
    for (ObjectGuid const& guid : objects)
    {
        GameObject* forge = botAI->GetGameObject(guid);
        GameObjectTemplate const* info = forge ? forge->GetGOInfo() : nullptr;
        if (!forge || !forge->isSpawned() || !info || info->type != GAMEOBJECT_TYPE_SPELL_FOCUS ||
            info->spellFocus.focusId != kForgeFocus)
            continue;
        float const d = bot->GetExactDist(forge);
        if (!nearest || d < distance)
        {
            nearest = forge;
            distance = d;
        }
    }
    return nearest;
}

SmeltMoveFacts CurrentSmeltMoveFacts(PlayerbotAI* botAI, Player* bot, GameObject* forge)
{
    SmeltMoveFacts facts;
    facts.forgeGuid = forge ? forge->GetGUID().GetRawValue() : 0;
    auto* context = botAI->GetAiObjectContext();
    LastMovement const& movement = AI_VALUE(LastMovement&, "last movement");
    facts.lastMovementPresent = movement.msTime != 0;
    facts.movementIssuedAtMs = movement.msTime;
    facts.movementEndpoint = {
        movement.lastMoveToMapId, movement.lastMoveToX, movement.lastMoveToY, movement.lastMoveToZ};
    facts.nativeSplineActive =
        bot->movespline && bot->movespline->Initialized() && !bot->movespline->Finalized();
    if (facts.nativeSplineActive)
    {
        facts.nativeSplineId = bot->movespline->GetId();
        G3D::Vector3 const endpoint = bot->movespline->FinalDestination();
        facts.nativeSplineEndpoint = {bot->GetMapId(), endpoint.x, endpoint.y, endpoint.z};
    }
    NewRpgInfo const& info = botAI->rpgInfo;
    facts.moveFarActive = info.moveFarPos != WorldPosition();
    facts.moveFarEndpoint = {info.moveFarPos.GetMapId(), info.moveFarPos.GetPositionX(),
                             info.moveFarPos.GetPositionY(), info.moveFarPos.GetPositionZ()};
    TravelIntentPolicy::Intent const& intent = info.travelIntent;
    facts.travelIntent = {intent.version, intent.active, intent.goal.mapId, intent.goal.x, intent.goal.y,
                          intent.goal.z, intent.createdMs};
    facts.botMoving = bot->isMoving();
    return facts;
}

template <std::size_t N>
std::array<Have, N> HavesOf(Player* bot, std::array<Recipe, N> const& table)
{
    std::array<Have, N> have{};
    for (std::size_t i = 0; i < N; ++i)
        have[i] = Have{bot->HasSpell(table[i].spell), bot->GetItemCount(table[i].reagent, false)};
    return have;
}

std::uint32_t BandageStock(Player* bot)
{
    std::uint32_t n = 0;
    for (Recipe const& r : kBandages)
        n += bot->GetItemCount(r.item, false);
    return n;
}

// Errand food stock: vendor tier food (AutoWowErrands Stock) plus cooked food that counts.
std::uint32_t FoodStock(Player* bot)
{
    std::uint32_t n = CookedFoodStock(bot);
    for (AutoWowErrands::Tier const& t : AutoWowErrands::kFood)
        n += bot->GetItemCount(t.item, false);
    return n;
}

void Emit(Player* bot, Reason r, std::uint32_t line, std::uint32_t spell, std::uint32_t item, std::uint32_t count,
          std::string const& details = {})
{
    std::uint32_t const skill = line && bot->HasSkill(line) ? bot->GetPureSkillValue(line) : 0;
    LOG_INFO("playerbots", "[SelfCraft] bot={} {} line={} skill={} spell={} item={} count={}", bot->GetName(),
             ReasonName(r), line, skill, spell, item, count);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSelfCraft(bot, ReasonName(r), LedgerFields(line, skill, spell, item, count) + details);
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.SelfCraft.Enable", false);
    Params& p = detail::gParams;
    p.firstAid = sConfigMgr->GetOption<bool>("AutoWow.SelfCraft.FirstAid", true);
    p.cooking = sConfigMgr->GetOption<bool>("AutoWow.SelfCraft.Cooking", true);
    p.bandageTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.BandageTarget", 10);
    p.bandageCloth = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.BandageCloth", 20);
    p.useHpPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.UseHpPct", 60);
    p.foodTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.FoodTarget", 20);
    p.checkIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.CheckIntervalMs", 20000);
    p.batchMax = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.BatchMax", 5);
    p.craftMinHpPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.CraftMinHpPct", 70);
    p.craftMinManaPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.CraftMinManaPct", 50);
    bool const smelting = sConfigMgr->GetOption<bool>("AutoWow.SelfCraft.Smelting", false);
    p.smeltingCheckIntervalMs =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.SmeltingCheckIntervalMs", 60000);
    p.smeltingBatchMax = sConfigMgr->GetOption<std::uint32_t>("AutoWow.SelfCraft.SmeltingBatchMax", 5);
    if (smelting)
        LoadSmeltCatalog();
    p.smelting = smelting;  // publish the gate only after the immutable catalog is complete
}

std::uint32_t CookedFoodStock(Player* bot)
{
    std::uint32_t n = 0;
    for (Recipe const& r : kFoods)
        if (CountsAsFood(r.useLevel, bot->GetLevel()))
            n += bot->GetItemCount(r.item, false);
    return n;
}

bool ReservedCloth(PlayerbotAI* botAI, Player* bot, Item* item)
{
    Params const& p = detail::gParams;
    if (!detail::gEnabled || !p.firstAid || !p.bandageCloth || !item || !bot->HasSkill(kSkillFirstAid) ||
        !Adventurer(botAI, bot))
        return false;
    std::uint32_t const cloth = ReserveCloth(HavesOf(bot, kBandages), bot->GetPureSkillValue(kSkillFirstAid));
    if (!cloth || item->GetEntry() != cloth)
        return false;
    std::vector<AutoWowSupply::Stack> stacks;
    auto add = [&](Item const* it)
    {
        if (it && it->GetEntry() == cloth)
            stacks.push_back({static_cast<std::uint32_t>(it->GetGUID().GetCounter()), it->GetCount()});
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag const* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                add(b->GetItemByPos(slot));
    std::vector<std::uint32_t> const kept = AutoWowSupply::PickStacks(stacks, p.bandageCloth);
    return std::find(kept.begin(), kept.end(), static_cast<std::uint32_t>(item->GetGUID().GetCounter())) !=
           kept.end();
}

Known KnownOf(Player* bot)
{
    Known k;
    if (!detail::gEnabled)
        return k;
    if (bot->HasSkill(kSkillFirstAid))
        k.firstAidMax = bot->GetPureMaxSkillValue(kSkillFirstAid);
    if (bot->HasSkill(kSkillCooking))
        k.cookingMax = bot->GetPureMaxSkillValue(kSkillCooking);
    for (std::size_t i = 0; i < kBandages.size(); ++i)
        if (bot->HasSpell(kBandages[i].spell))
            k.bandages |= 1u << i;
    for (std::size_t i = 0; i < kFoods.size(); ++i)
        if (bot->HasSpell(kFoods[i].spell))
            k.foods |= 1u << i;
    return k;
}

void NoteLearned(Player* bot, Known const& before)
{
    if (!detail::gEnabled)
        return;
    Known const now = KnownOf(bot);
    // A new line or rank: count = its max skill (the errand row carries the trainer cost).
    if (now.firstAidMax != before.firstAidMax)
        Emit(bot, Reason::Learn, kSkillFirstAid, 0, 0, now.firstAidMax);
    if (now.cookingMax != before.cookingMax)
        Emit(bot, Reason::Learn, kSkillCooking, 0, 0, now.cookingMax);
    for (std::size_t i = 0; i < kBandages.size(); ++i)
        if ((now.bandages & ~before.bandages) & (1u << i))
            Emit(bot, Reason::Learn, kSkillFirstAid, kBandages[i].spell, kBandages[i].item, 0);
    for (std::size_t i = 0; i < kFoods.size(); ++i)
        if ((now.foods & ~before.foods) & (1u << i))
            Emit(bot, Reason::Learn, kSkillCooking, kFoods[i].spell, kFoods[i].item, 0);
}

void ReconcileSmelting(Player* bot, SmeltOwner owner)
{
    if (!bot || owner == SmeltOwner::None)
        return;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    BotState state = LoadState(guid);
    if (!state.smeltSpell || state.smeltOwner != owner)
        return;
    SmeltReceiptFacts facts;
    facts.expectedOwner = owner;
    facts.actualOwner = state.smeltOwner;
    facts.stillCasting = bot->IsNonMeleeSpellCast(false);
    facts.outputBefore = state.smeltOutputBefore;
    facts.outputAfter = bot->GetItemCount(state.smeltOutput, false);
    facts.outputExpected = state.smeltOutputCount;
    facts.reagentCount = state.smeltReagentCount;
    for (std::size_t i = 0; i < state.smeltReagentCount; ++i)
        facts.reagents[i] = {state.smeltReagentBefore[i],
                             bot->GetItemCount(state.smeltReagents[i].item, false), state.smeltReagents[i].count};
    SmeltReceipt const receipt = EvaluateSmeltReceipt(facts);
    if (receipt == SmeltReceipt::Pending || receipt == SmeltReceipt::WrongOwner)
        return;
    std::string const details = SmeltFields(state, bot, receipt);
    LOG_INFO("playerbots",
             "[SelfCraft] smelt bot={} result={} spell={} output={} before={} after={} skill={}->{} casts={}",
             bot->GetName(), SmeltReceiptName(receipt), state.smeltSpell, state.smeltOutput,
             state.smeltOutputBefore, facts.outputAfter, state.smeltSkillBefore,
             bot->GetPureSkillValue(kSkillMining), state.smeltCasts);
    Emit(bot, receipt == SmeltReceipt::Success ? Reason::Craft : Reason::Skip, kSkillMining, state.smeltSpell,
         state.smeltOutput, receipt == SmeltReceipt::Success ? state.smeltOutputCount : 0, details);
    ClearPendingSmelt(state);
    std::uint64_t const now = NowMs();
    if (receipt != SmeltReceipt::Success ||
        !SmeltJobOpen(state.smeltActive, state.smeltCasts, detail::gParams.smeltingBatchMax,
                      state.smeltSinceMs, now))
        EndSmeltJob(state, now);
    StoreState(guid, state);
}

bool SmeltingWanted(PlayerbotAI* botAI, Player* bot, SmeltOwner owner)
{
    if (!SmeltAdmitted(botAI, bot, owner) || bot->IsNonMeleeSpellCast(false))
        return false;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    std::uint64_t const now = NowMs();
    BotState state = LoadState(guid);
    bool const foreignGenericGoal = owner == SmeltOwner::SelfCraft &&
        (botAI->rpgInfo.moveFarPos != WorldPosition() || botAI->rpgInfo.travelIntent.active);
    if (state.smeltSpell || now < state.nextSmeltMs ||
        (state.smeltActive && state.smeltOwner != owner) ||
        (!state.smeltActive && owner == SmeltOwner::SelfCraft && (bot->isMoving() || foreignGenericGoal)))
        return false;
    if (state.smeltActive &&
        !SmeltJobOpen(true, state.smeltCasts, detail::gParams.smeltingBatchMax, state.smeltSinceMs, now))
    {
        EndSmeltJob(state, now);
        StoreState(guid, state);
        return false;
    }
    if (!PickRuntimeSmelt(botAI, bot, false))
    {
        EndSmeltJob(state, now);
        StoreState(guid, state);
        return false;
    }
    if (!state.smeltActive)
    {
        state.smeltActive = true;
        state.smeltOwner = owner;
        state.smeltSinceMs = now;
        state.smeltCasts = 0;
        StoreState(guid, state);
    }
    return true;
}

bool StartSmelting(PlayerbotAI* botAI, Player* bot, SmeltOwner owner)
{
    if (!SmeltingWanted(botAI, bot, owner))
        return false;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    std::uint64_t const now = NowMs();
    BotState state = LoadState(guid);
    SmeltRecipe const* recipe = PickRuntimeSmelt(botAI, bot, true);
    if (!recipe || !botAI->CanCastSpell(recipe->spell, bot, true))
    {
        EndSmeltJob(state, now);
        StoreState(guid, state);
        return false;
    }
    state.smeltSpell = recipe->spell;
    state.smeltOutput = recipe->output;
    state.smeltOutputCount = recipe->outputCount;
    state.smeltOutputBefore = bot->GetItemCount(recipe->output, false);
    state.smeltSkillBefore = bot->GetPureSkillValue(kSkillMining);
    state.smeltIssuedMs = now;
    state.smeltReagentCount = recipe->reagentCount;
    for (std::size_t i = 0; i < recipe->reagentCount; ++i)
    {
        state.smeltReagents[i] = recipe->reagents[i];
        state.smeltReagentBefore[i] = bot->GetItemCount(recipe->reagents[i].item, false);
    }
    if (!botAI->CastSpell(recipe->spell, bot))
    {
        std::string const details = SmeltFields(state, bot, SmeltReceipt::Interrupted);
        Emit(bot, Reason::Skip, kSkillMining, recipe->spell, recipe->output, 0, details);
        ClearPendingSmelt(state);
        EndSmeltJob(state, now);
        StoreState(guid, state);
        return false;
    }
    ++state.smeltCasts;
    state.smeltMoveSinceMs = 0;
    state.smeltMove = {};
    StoreState(guid, state);
    LOG_INFO("playerbots", "[SelfCraft] smelt bot={} issued spell={} output={} count={} skill={} cast={}/{}",
             bot->GetName(), recipe->spell, recipe->output, recipe->outputCount, state.smeltSkillBefore,
             state.smeltCasts, detail::gParams.smeltingBatchMax);
    return true;
}
}  // namespace AutoWowSelfCraft

bool NewRpgBaseAction::SelfCraftStep()
{
    using namespace AutoWowSelfCraft;
    using AutoWowSelfCraft::BotState;  // other AutoWow policies also name these
    using AutoWowSelfCraft::Params;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive() || bot->IsInFlight() || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        bot->GetTransport() || !Adventurer(botAI, bot))
        return false;

    Params const& p = detail::gParams;
    std::uint64_t const now = NowMs();
    ReconcileSmelting(bot, SmeltOwner::SelfCraft);
    BotState s = LoadState(guid);

    // A native smelt we issued owns the tick until its exact output/reagent receipt can be observed.
    if (s.smeltSpell)
        return true;

    // Our cast or bandage channel in flight: hold the RPG in place until it ends (combat breaks it).
    if (s.castSpell || s.bandaging)
    {
        if (bot->IsNonMeleeSpellCast(false) && !bot->IsInCombat())
            return true;
        if (s.castSpell && s.castItem)
        {
            std::uint32_t const have = bot->GetItemCount(s.castItem, false);
            std::uint32_t const made = have > s.castBefore ? have - s.castBefore : 0;
            Emit(bot, made ? Reason::Craft : Reason::Skip, s.castLine, s.castSpell, s.castItem, made);
        }
        s.castSpell = s.castItem = s.castLine = s.castBefore = 0;
        s.bandaging = false;
    }
    bool const inCombat = bot->IsInCombat();

    // Use: a bandage before resting (the rest-gate food triggers run below this step).
    if (detail::gEnabled && p.firstAid && now >= s.nextUseMs && bot->HasSkill(kSkillFirstAid) && !bot->IsMounted())
    {
        std::array<std::uint32_t, kBandages.size()> held{};
        for (std::size_t i = 0; i < kBandages.size(); ++i)
            held[i] = bot->GetItemCount(kBandages[i].item, false);
        int const use = PickUse(held, bot->GetPureSkillValue(kSkillFirstAid));
        Item* bandage = use >= 0 ? bot->GetItemByEntry(kBandages[use].item) : nullptr;
        if (ShouldBandage(static_cast<std::uint32_t>(bot->GetHealthPct()), p.useHpPct, inCombat,
                          AI_VALUE(uint8, "my attacker count"), bot->HasAura(kRecentlyBandaged), bandage))
        {
            bot->StopMoving();
            botAI->ImbueItem(bandage, bot);
            s.bandaging = true;
            s.nextUseMs = now + kUseRetryMs;
            StoreState(guid, s);
            Emit(bot, Reason::Use, kSkillFirstAid, 0, kBandages[use].item, 1);
            return true;
        }
    }

    // Mining: an independently gated, finite local detour to a naturally nearby normal forge. The exact forge,
    // LastMovement write and native spline are retained before this path may stop or cast over that movement.
    if (SmeltingWanted(botAI, bot, SmeltOwner::SelfCraft))
    {
        GameObject* forge = NearbyForge(botAI, bot);
        s = LoadState(guid);
        if (!forge)
        {
            EndSmeltJob(s, now);
            StoreState(guid, s);
        }
        else
        {
            SmeltMoveFacts facts = CurrentSmeltMoveFacts(botAI, bot, forge);
            SmeltMoveDecision move = EvaluateSmeltMove(s.smeltMove, facts);
            bool const hasMoveProof = s.smeltMove.version != 0;
            GameObjectTemplate const* info = forge->GetGOInfo();
            float const focusYards = info ? static_cast<float>(info->spellFocus.dist) : 0.0f;
            bool const atForge = focusYards > 0.0f && bot->IsWithinDistInMap(forge, focusYards);

            if (hasMoveProof && (move == SmeltMoveDecision::NoLeg || move == SmeltMoveDecision::Replaced))
            {
                // Another idle mover owns the current or most recent goal. Release without stop or cast.
                EndSmeltJob(s, now);
                StoreState(guid, s);
            }
            else if (!hasMoveProof &&
                     (facts.botMoving || facts.nativeSplineActive || facts.moveFarActive ||
                      facts.travelIntent.active))
            {
                // Starting in forge range does not authorize cancelling or casting over foreign motion.
                EndSmeltJob(s, now);
                StoreState(guid, s);
            }
            else if (atForge)
            {
                // Stop only the still-live native spline proven to be the leg we issued. A completed owned leg and a
                // stationary bot that began in focus need no stop before the cast.
                if (move == SmeltMoveDecision::OwnedLive)
                    bot->StopMoving();
                if (StartSmelting(botAI, bot, SmeltOwner::SelfCraft))
                    return true;
            }
            else if (hasMoveProof)
            {
                if (move != SmeltMoveDecision::OwnedLive)
                {
                    EndSmeltJob(s, now);
                    StoreState(guid, s);
                }
                else if (SmeltMoveTimedOut(true, s.smeltMoveSinceMs, now))
                {
                    bot->StopMoving();  // exact owned live spline, established above
                    EndSmeltJob(s, now);
                    StoreState(guid, s);
                }
                else
                {
                    StoreState(guid, s);
                    return true;
                }
            }
            else
            {
                s.smeltMoveSinceMs = now;
                bool const issued = MoveTo(forge->GetMapId(), forge->GetPositionX(), forge->GetPositionY(),
                                           forge->GetPositionZ(), false, false, false, true);
                facts = CurrentSmeltMoveFacts(botAI, bot, forge);
                s.smeltMove = issued ? CaptureSmeltMoveProof(facts) : SmeltMoveProof{};
                if (!s.smeltMove.version)
                {
                    // MoveTo can decline for an inherited waiting goal. It remains untouched and wins this tick.
                    EndSmeltJob(s, now);
                    StoreState(guid, s);
                }
                else
                {
                    StoreState(guid, s);
                    return true;
                }
            }
        }
    }

    // Smelting may activate this hook on its own; all pre-existing First Aid/Cooking behavior still requires the
    // parent SelfCraft switch.
    if (!detail::gEnabled)
        return false;

    // Craft: one cast per tick, at most BatchMax per check.
    if (now < s.nextCheckMs)
    {
        StoreState(guid, s);
        return false;
    }
    auto rest = [&]()
    {
        s.crafted = 0;
        s.nextCheckMs = now + p.checkIntervalMs;
        StoreState(guid, s);
        return false;
    };
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    bool const busy = bot->IsMounted() || bot->GetGroup() || status == RPG_REST || status == RPG_TRAVEL_FLIGHT ||
                      (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)) ||
                      (AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid));
    bool const usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    if (s.crafted >= p.batchMax ||
        !MayCraft(p, inCombat, busy, static_cast<std::uint32_t>(bot->GetHealthPct()),
                  usesMana ? static_cast<std::uint32_t>(bot->GetPowerPct(POWER_MANA)) : 0, usesMana))
        return rest();

    auto start = [&](std::uint32_t line, std::uint32_t spell, std::uint32_t item)
    {
        bot->StopMoving();
        if (!botAI->CanCastSpell(spell, bot, true))
            return false;
        std::uint32_t const before = item ? bot->GetItemCount(item, false) : 0;
        if (!botAI->CastSpell(spell, bot))
            return false;
        s.castSpell = spell;
        s.castItem = item;
        s.castLine = line;
        s.castBefore = before;
        s.skipSpell = 0;
        ++s.crafted;
        StoreState(guid, s);
        return true;
    };
    // One `skip` row per refused recipe until a cast starts again (the check repeats every interval).
    auto skip = [&](std::uint32_t line, Recipe const& r)
    {
        if (s.skipSpell != r.spell)
            Emit(bot, Reason::Skip, line, r.spell, r.item, 0);
        s.skipSpell = r.spell;
    };

    if (p.firstAid && bot->HasSkill(kSkillFirstAid))
    {
        int const i = PickBandage(HavesOf(bot, kBandages), bot->GetPureSkillValue(kSkillFirstAid),
                                  BandageStock(bot), p.bandageTarget);
        if (i >= 0)
        {
            if (start(kSkillFirstAid, kBandages[i].spell, kBandages[i].item))
                return true;
            skip(kSkillFirstAid, kBandages[i]);
        }
    }
    if (p.cooking && bot->HasSkill(kSkillCooking))
    {
        int const i = PickFood(HavesOf(bot, kFoods), bot->GetPureSkillValue(kSkillCooking), bot->GetLevel(),
                               FoodStock(bot), p.foodTarget);
        if (i >= 0)
        {
            if (start(kSkillCooking, kFoods[i].spell, kFoods[i].item))
                return true;
            // No cooking fire in reach: light one (the next tick cooks at it).
            if (bot->HasSpell(kCampfireSpell) && start(kSkillCooking, kCampfireSpell, 0))
                return true;
            skip(kSkillCooking, kFoods[i]);
        }
    }
    return rest();
}
