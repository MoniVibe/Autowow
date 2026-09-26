/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_SELFCRAFT_POLICY_H
#define AUTOWOW_SELFCRAFT_POLICY_H

#include <array>
#include <cstdint>
#include <string>

#include "ErrandsPolicy.h"

class Item;
class Player;
class PlayerbotAI;

// Self-sufficiency secondary professions for cohort adventurers (AutoWow.SelfCraft.Enable, default 0;
// sub-flags AutoWow.SelfCraft.FirstAid / .Cooking). Food and water were the largest money sink and deaths /
// rest the largest XP drags; a player answers both with First Aid and Cooking from its own loot.
//
// Learning is the stock path: an errand town stop at a trade trainer runs the `trainer` action, whose learn
// filter admits the AutoWow.Professions.Secondaries lines (default 129,185,356) and every rank / recipe of a
// line the bot has, paid in real gold. The errand stop only adds the `selfcraft learn` ledger rows.
// With the flag on, out of combat (NewRpgSelfCraft.cpp, one step of the NewRpg status update):
//   use    - HP under UseHpPct, no attacker, no Recently Bandaged: the best bandage the skill can use (the
//            stock "try emergency" bandage branch is wired to no strategy, so nothing else ever bandages).
//   craft  - bandages up to BandageTarget from the bot's own cloth; food from its own meat at a cooking fire
//            (Basic Campfire 818: learned with Cooking, no reagent or tool in 3.3.5) while its food stock is
//            under FoodTarget, else skill-ups on non-grey recipes. At most BatchMax casts per CheckIntervalMs.
//   stock  - cooked food at most kFoodSlackLevels below the bot's vendor food tier counts toward the errand
//            food stock (errands buy less); the cloth of the bandage the bot works toward, BandageCloth units
//            in whole stacks, is not sold (SellAction).
// Ledger `selfcraft` (event 21): learn | craft | use | skip. No free items or gold: reagents are loot,
// trainer ranks are paid, the campfire is a spell.
//
// Value-only below the runtime section: integer skill / level / percent / game-time ms, no floats, no RNG,
// fixed table order (ascending recipe rank) with ties to the higher table row.
namespace AutoWowSelfCraft
{
inline constexpr std::uint8_t kStateVersion = 1;
inline constexpr std::uint32_t kSkillFirstAid = 129;  // SKILL_FIRST_AID
inline constexpr std::uint32_t kSkillCooking = 185;   // SKILL_COOKING
inline constexpr std::uint32_t kCampfireSpell = 818;  // Basic Campfire (summons a spell-focus 4 cooking fire)
inline constexpr std::uint32_t kRecentlyBandaged = 11196;
inline constexpr std::uint32_t kUseRetryMs = 10000;  // a refused bandage (no aura) is not retried sooner

struct Recipe
{
    std::uint32_t spell = 0;     // craft spell (Spell.dbc SPELL_EFFECT_CREATE_ITEM)
    std::uint32_t item = 0;      // product
    std::uint32_t reagent = 0;   // its only reagent
    std::uint32_t count = 0;     // reagent units per cast
    std::uint32_t minSkill = 0;  // trainer_spell.ReqSkillRank (1 = learned with the skill line)
    std::uint32_t greyAt = 0;    // SkillLineAbility TrivialSkillLineRankHigh: no skill-up at or above
    std::uint32_t useLevel = 0;  // product item_template.RequiredLevel
    std::uint32_t useSkill = 0;  // product item_template.RequiredSkillRank (bandages: First Aid)
};

// First Aid bandages, ascending (Spell.dbc, SkillLineAbility.dbc, item_template, trainer_spell; world DB and
// 3.3.5 DBCs checked 2026-09-26). All trainer-taught but Linen Bandage (learned with First Aid).
inline constexpr std::array<Recipe, 6> kBandages = {{
    {3275, 1251, 2589, 1, 1, 60, 0, 1},       // Linen Bandage       <- Linen Cloth
    {3276, 2581, 2589, 2, 40, 100, 0, 20},    // Heavy Linen Bandage <- 2 Linen Cloth
    {3277, 3530, 2592, 1, 80, 150, 0, 50},    // Wool Bandage        <- Wool Cloth
    {3278, 3531, 2592, 2, 115, 185, 0, 75},   // Heavy Wool Bandage  <- 2 Wool Cloth
    {7928, 6450, 4306, 1, 150, 210, 0, 100},  // Silk Bandage        <- Silk Cloth
    {7929, 6451, 4306, 2, 180, 240, 0, 125},  // Heavy Silk Bandage  <- 2 Silk Cloth
}};

// Cooking to skill 225: every recipe learned with Cooking or taught by the cooking trainers whose one reagent
// is a beast drop (vendor-bought reagents and recipe books left out). Every one needs a cooking fire (spell
// focus 4). Ascending by minSkill. Same sources as kBandages.
inline constexpr std::array<Recipe, 11> kFoods = {{
    {2538, 2679, 2672, 1, 1, 85, 1, 0},          // Charred Wolf Meat      <- Stringy Wolf Meat
    {2540, 2681, 769, 1, 1, 85, 1, 0},           // Roasted Boar Meat      <- Chunk of Boar Meat
    {8604, 6888, 6889, 1, 1, 85, 1, 0},          // Herb Baked Egg         <- Small Egg
    {2539, 2680, 2672, 1, 10, 90, 1, 0},         // Spiced Wolf Meat       <- Stringy Wolf Meat
    {2795, 2888, 2886, 1, 25, 100, 1, 0},        // Beer Basted Boar Ribs  <- Crag Boar Rib
    {2541, 2684, 2673, 1, 50, 130, 5, 0},        // Coyote Steak           <- Coyote Meat
    {2544, 2683, 2674, 1, 75, 155, 5, 0},        // Crab Cake              <- Crawler Meat
    {2546, 2687, 2677, 1, 80, 160, 5, 0},        // Dry Pork Ribs          <- Boar Ribs
    {6500, 5527, 5504, 1, 125, 205, 15, 0},      // Goblin Deviled Clams   <- Tangy Clam Meat
    {4094, 4457, 3404, 1, 175, 255, 25, 0},      // Barbecued Buzzard Wing <- Buzzard Wing
    {21175, 17222, 12205, 2, 200, 250, 35, 0},   // Spider Sausage         <- 2 White Spider Meat
}};

struct Params
{
    bool firstAid = true;                  // AutoWow.SelfCraft.FirstAid
    bool cooking = true;                   // AutoWow.SelfCraft.Cooking
    std::uint32_t bandageTarget = 10;      // AutoWow.SelfCraft.BandageTarget: bandages kept
    std::uint32_t bandageCloth = 20;       // AutoWow.SelfCraft.BandageCloth: cloth units kept from sale
    std::uint32_t useHpPct = 60;           // AutoWow.SelfCraft.UseHpPct: bandage below this health
    std::uint32_t foodTarget = 20;         // AutoWow.SelfCraft.FoodTarget: cook for food under this stock
    std::uint32_t checkIntervalMs = 20000; // AutoWow.SelfCraft.CheckIntervalMs
    std::uint32_t batchMax = 5;            // AutoWow.SelfCraft.BatchMax: casts per check
    std::uint32_t craftMinHpPct = 70;      // AutoWow.SelfCraft.CraftMinHpPct: below, rest comes first
    std::uint32_t craftMinManaPct = 50;    // AutoWow.SelfCraft.CraftMinManaPct (mana users)
};

// What the bot has of one table row: the recipe known, reagent units in its bags.
struct Have
{
    bool known = false;
    std::uint32_t reagents = 0;
};

// Highest recipe the bot knows at its skill (reagents ignored), -1 = none.
template <std::size_t N>
[[nodiscard]] inline int BestKnown(std::array<Recipe, N> const& table, std::array<Have, N> const& have,
                                   std::uint32_t skill)
{
    for (std::size_t i = N; i-- > 0;)
        if (have[i].known && skill >= table[i].minSkill)
            return static_cast<int>(i);
    return -1;
}

// Bandage to craft now: the highest known recipe at the skill with its reagents in hand, while the bandage
// stock is under target. -1 = none.
[[nodiscard]] inline int PickBandage(std::array<Have, kBandages.size()> const& have, std::uint32_t skill,
                                     std::uint32_t stock, std::uint32_t target)
{
    if (stock >= target)
        return -1;
    for (std::size_t i = kBandages.size(); i-- > 0;)
        if (have[i].known && skill >= kBandages[i].minSkill && have[i].reagents >= kBandages[i].count)
            return static_cast<int>(i);
    return -1;
}

// Cloth kept for bandages: the reagent of the bandage the bot works toward (BestKnown), 0 = none. The runtime
// keeps BandageCloth units of it in whole stacks, ascending item guid (AutoWowSupply::PickStacks order).
[[nodiscard]] inline std::uint32_t ReserveCloth(std::array<Have, kBandages.size()> const& have, std::uint32_t skill)
{
    int const i = BestKnown(kBandages, have, skill);
    return i < 0 ? 0 : kBandages[i].reagent;
}

// Best bandage the bot may use now: the highest table row it holds with its First Aid at the product's
// required rank. -1 = none. `held[i]` = units of kBandages[i].item in its bags.
[[nodiscard]] inline int PickUse(std::array<std::uint32_t, kBandages.size()> const& held, std::uint32_t skill)
{
    for (std::size_t i = kBandages.size(); i-- > 0;)
        if (held[i] && skill >= kBandages[i].useSkill)
            return static_cast<int>(i);
    return -1;
}

// Bandage now: hurt under the threshold, out of combat, nobody attacking, no Recently Bandaged, one in hand.
[[nodiscard]] inline bool ShouldBandage(std::uint32_t hpPct, std::uint32_t useHpPct, bool inCombat,
                                        std::uint32_t attackers, bool recentlyBandaged, bool haveBandage)
{
    return hpPct < useHpPct && !inCombat && attackers == 0 && !recentlyBandaged && haveBandage;
}

// A craft cast may start: free (out of combat, not on a travel leg / rest / group run) and not in need of
// rest (the food and drink triggers run below the NewRpg step, a cast would stand the bot up).
[[nodiscard]] inline bool MayCraft(Params const& p, bool inCombat, bool busy, std::uint32_t hpPct,
                                   std::uint32_t manaPct, bool usesMana)
{
    return !inCombat && !busy && hpPct >= p.craftMinHpPct && (!usesMana || manaPct >= p.craftMinManaPct);
}

// ---- food ------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kFoodSlackLevels = 10;

// Required level of the vendor food tier errands buy at a level (AutoWowErrands::kFood).
[[nodiscard]] inline std::uint32_t VendorFoodLevel(std::uint32_t level)
{
    std::uint32_t best = 0;
    for (AutoWowErrands::Tier const& t : AutoWowErrands::kFood)
        if (t.minLevel <= level)
            best = t.minLevel;
    return best;
}

// Cooked food worth its bag slot at a level: usable, and at most kFoodSlackLevels under the vendor tier (the
// stock eat action takes the highest item level first, so lower food only fills gaps).
[[nodiscard]] inline bool CountsAsFood(std::uint32_t useLevel, std::uint32_t level)
{
    return useLevel <= level && useLevel + kFoodSlackLevels >= VendorFoodLevel(level);
}

// Recipe to cook now, -1 = none. Food first: while the food stock is under target, the known recipe with its
// reagents in hand whose product counts as food (highest product level, then the higher row). Otherwise a
// skill-up: the highest known recipe with reagents still below grey.
[[nodiscard]] inline int PickFood(std::array<Have, kFoods.size()> const& have, std::uint32_t skill,
                                  std::uint32_t level, std::uint32_t foodStock, std::uint32_t target)
{
    auto ready = [&](std::size_t i)
    { return have[i].known && skill >= kFoods[i].minSkill && have[i].reagents >= kFoods[i].count; };
    int pick = -1;
    if (foodStock < target)
        for (std::size_t i = 0; i < kFoods.size(); ++i)
            if (ready(i) && CountsAsFood(kFoods[i].useLevel, level) &&
                (pick < 0 || kFoods[i].useLevel >= kFoods[pick].useLevel))
                pick = static_cast<int>(i);
    if (pick >= 0)
        return pick;
    for (std::size_t i = kFoods.size(); i-- > 0;)
        if (ready(i) && skill < kFoods[i].greyAt)
            return static_cast<int>(i);
    return -1;
}

// ---- ledger ----------------------------------------------------------------------------------------------
// Wire-stable reason names of `selfcraft` rows.
enum class Reason : std::uint8_t
{
    Learn = 0,  // a secondary line, rank or recipe learned at a trainer
    Craft = 1,  // a craft cast finished (count = products made)
    Use = 2,    // a bandage applied
    Skip = 3    // a wanted craft could not start (no fire / cast refused)
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Learn: return "learn";
        case Reason::Craft: return "craft";
        case Reason::Use: return "use";
        case Reason::Skip: return "skip";
    }
    return "unknown";
}

inline std::string LedgerFields(std::uint32_t skillLine, std::uint32_t skill, std::uint32_t spell,
                                std::uint32_t item, std::uint32_t count)
{
    return ",\"line\":" + std::to_string(skillLine) + ",\"skill\":" + std::to_string(skill) +
           ",\"spell\":" + std::to_string(spell) + ",\"item\":" + std::to_string(item) +
           ",\"count\":" + std::to_string(count);
}

// ---- per-bot state (runtime; map threads, under the runtime lock) ----------------------------------------
struct BotState
{
    std::uint8_t version = kStateVersion;
    std::uint64_t nextCheckMs = 0;  // game-time ms of the next craft check
    std::uint64_t nextUseMs = 0;    // game-time ms before which no bandage is tried again
    std::uint32_t castSpell = 0;    // our craft / campfire cast in flight (0 = none)
    std::uint32_t castItem = 0;     // its product (0 = campfire)
    std::uint32_t castLine = 0;     // its skill line
    std::uint32_t castBefore = 0;   // product units before the cast
    std::uint32_t crafted = 0;      // casts in this batch
    std::uint32_t skipSpell = 0;    // last `skip` row's spell: one row until a cast starts
    bool bandaging = false;         // our bandage channel in flight
};

// What the bot knows of the two lines (trainer-stop learn rows).
struct Known
{
    std::uint32_t firstAidMax = 0;  // pure max skill (0 = not learned)
    std::uint32_t cookingMax = 0;
    std::uint32_t bandages = 0;     // bit per kBandages row
    std::uint32_t foods = 0;        // bit per kFoods row
};

// ---- runtime (NewRpgSelfCraft.cpp) ----------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }
inline bool CookingOn() { return detail::gEnabled && detail::gParams.cooking; }

void LoadConfig();
// Cooked food of the bot that CountsAsFood at its level (errand food stock).
std::uint32_t CookedFoodStock(Player* bot);
// SellAction: the item is part of the bandage cloth reserve of a cohort adventurer.
bool ReservedCloth(PlayerbotAI* botAI, Player* bot, Item* item);
// Errand trainer stop: snapshot before the `trainer` action, then one `learn` row per new line / rank / recipe.
Known KnownOf(Player* bot);
void NoteLearned(Player* bot, Known const& before);
}  // namespace AutoWowSelfCraft

#endif  // AUTOWOW_SELFCRAFT_POLICY_H
