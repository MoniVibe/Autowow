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
#include "ErrandsPolicy.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SelfCraftPolicy.h"
#include "SupplyPolicy.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowSelfCraft
{
namespace
{
// Bot AI updates run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;

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

// A cohort adventurer: an independent AutoWoW bot, not Oracle-managed, not a supply rep / artisan.
bool Adventurer(PlayerbotAI* botAI, Player* bot)
{
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    return botAI && botAI->IsAutoWowIndependentParty() && !AutoWowOracleRuntime::IsManagedBot(guid) &&
           !(AutoWowSupply::Enabled() && AutoWowSupply::RoleOf(guid).role != AutoWowSupply::Role::None);
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

void Emit(Player* bot, Reason r, std::uint32_t line, std::uint32_t spell, std::uint32_t item, std::uint32_t count)
{
    std::uint32_t const skill = line && bot->HasSkill(line) ? bot->GetPureSkillValue(line) : 0;
    LOG_INFO("playerbots", "[SelfCraft] bot={} {} line={} skill={} spell={} item={} count={}", bot->GetName(),
             ReasonName(r), line, skill, spell, item, count);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSelfCraft(bot, ReasonName(r), LedgerFields(line, skill, spell, item, count));
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
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    BotState s = LoadState(guid);

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
    if (p.firstAid && now >= s.nextUseMs && bot->HasSkill(kSkillFirstAid) && !bot->IsMounted())
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
