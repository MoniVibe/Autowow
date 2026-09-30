/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SUPPLYPOLICY_H
#define _PLAYERBOT_SUPPLYPOLICY_H

// AutoWow.Supply (docs/SUPPLY_CHAIN_PLAN.md lane B): the bag production chain. Adventurers route cloth to the
// bag house's rep, the rep feeds the house artisan (a tailor who stays home and crafts), the overlord (no body,
// one per team) sizes bag orders from the cohort's empty bag slots, the treasury pays the artisan per bag and
// the rep mails bags to the members with the most empty slots. Pure rules here (unit-tested); runtime in
// AutoWowSupply.cpp (world thread) and NewRpgSupply.cpp (the role bots' map-thread step). No free gold or
// items: every item and copper moves by mail, vendor, trainer, auction house or the guild bank.
// Product catalog (lane D, AutoWow.Supply.Products): kCatalog lists the lines; bags keep the bespoke chain
// above, a single-step line (potions) runs on the generic LineTick over its tier table.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "AutoWowGuildsPolicy.h"

class Player;

namespace AutoWowSupply
{
inline constexpr std::uint32_t kStateVersion = 10;  // RoleState / TeamState layout; bump on change (2: tiers, market;
                                                    // 3: catalog LineView / RoleInfo.line; 4: RoleState apprentice /
                                                    // craftBlocked; 5: TeamState goal; 6: DirectRoutes targets;
                                                    // 7: gear lines: RoleInfo.gear, RoleState castLine, per-tier wants;
                                                    // 8: TeamState gearCloth; 9: TeamState extraRooms; 10: kMaxLineTiers
                                                    // 8 (LineView surplus, LineState wants), LineTier family)

// Cloth routed to the bag house (item entries): linen, wool, silk. Only linen feeds the V1 recipe chain;
// wool and silk are stored for the next bags.
inline constexpr std::uint32_t kLinen = 2589, kWool = 2592, kSilk = 4306;
// ponytail: linen only while the Linen Bag is the one product; wool and silk (101 of 141 donated units in
// soak-s41-full-r1) filled ClothCap without feeding a recipe. AutoWow.Supply.Tiers routes kTierCloth instead,
// each cloth under its own ClothCap.
inline constexpr std::uint32_t kCloth[] = {kLinen};
inline constexpr std::uint32_t kTierCloth[] = {kLinen, kWool, kSilk};  // == kTiers[i].cloth
inline constexpr std::uint32_t kMaxMailStacks = 12;  // core MAX_MAIL_ITEMS

struct Params
{
    std::uint32_t tickMs = 30000;       // AutoWow.Supply.TickMs: role step and world ops stride
    std::uint32_t overlordMs = 300000;  // AutoWow.Supply.OverlordMs: need scan / order stride
    bool routeCloth = false;            // AutoWow.Supply.RouteCloth
    std::uint32_t clothCap = 200;       // AutoWow.Supply.ClothCap: house cloth stock above which routing stops
    std::uint32_t maxOrder = 6;         // AutoWow.Supply.MaxOrder: bags per order
    std::uint32_t bagPayPct = 200;      // AutoWow.Supply.BagPayPct: pay per bag = vendor sell value * this / 100
    std::uint32_t workXpPerItem = 0;    // AutoWow.Supply.WorkXpPerItem: 0 = XpForLevel(level) / kXpDivisor
    std::uint32_t repXpPerDeal = 0;     // AutoWow.Supply.RepXpPerDeal: 0 = the same rule
    std::uint32_t surplusKeep = 4;      // AutoWow.Supply.SurplusKeep: bags the rep keeps with no open need
    std::uint32_t homeYards = 25;       // AutoWow.Supply.HomeYards: "at home" radius
    std::uint32_t skillupCloth = 40;    // AutoWow.Supply.SkillupCloth: linen kept at an artisan still below
                                        // the bag recipe (bolts level the skill and are the bag's reagent)
    bool tiers = false;                 // AutoWow.Supply.Tiers: linen / wool / silk tiers (kTiers)
    bool market = false;                // AutoWow.Supply.Market: the bag-house rep trades on the faction AH
    std::uint32_t buyMaxPct = 400;      // AutoWow.Supply.BuyMaxPct: AH unit price <= vendor sell value * this
    std::uint32_t buyBudget = 500;      // AutoWow.Supply.BuyBudget: copper the rep may spend per market visit
    std::uint32_t sellKeep = 60;        // AutoWow.Supply.SellKeep: units of an out-of-reach cloth kept, not listed
    std::uint32_t listFloat = 1000;     // AutoWow.Supply.ListFloat: copper the bank keeps on the rep for deposits
    // Product catalog (AutoWow.Supply.Products; bit i = kCatalog[i]; default bags only):
    std::uint8_t lines = 1;             // AutoWow.Supply.Products
    bool routeHerbs = false;            // AutoWow.Supply.RouteHerbs
    std::uint32_t herbCap = 100;        // AutoWow.Supply.HerbCap: per routed herb, rep stock above which routing stops
    std::uint32_t potionTarget = 5;     // AutoWow.Supply.PotionTarget: potions of its best tier a member should hold
    std::uint32_t potionPayPct = 200;   // AutoWow.Supply.PotionPayPct: pay per potion = vendor sell value * this / 100
    std::uint32_t potionMaxOrder = 20;  // AutoWow.Supply.PotionMaxOrder: potions per order
    std::uint32_t potionKeep = 20;      // AutoWow.Supply.PotionSurplusKeep: potions per tier the rep keeps, no need
    std::uint32_t skillupCasts = 10;    // AutoWow.Supply.SkillupCasts: casts of reagents kept at a leveling artisan
    // Raw materials (lane G): ore to the Smiths rep, leather to the Tanners rep, no line consumes them yet.
    bool routeRaw = false;              // AutoWow.Supply.RouteRaw
    std::uint32_t rawCap = 100;         // AutoWow.Supply.RawCap: per raw item, rep stock above which routing stops
    // Outfitting (AutoWow.Supply.Outfit): the treasury funds members' gathering tools and planned trainer ranks.
    bool outfit = false;                      // AutoWow.Supply.Outfit
    std::uint32_t outfitCheckMs = 600000;     // AutoWow.Supply.OutfitCheckMs: a missing tool starts a run at most
                                              // this often on its own
    std::uint32_t outfitMaxCopper = 500;      // AutoWow.Supply.OutfitMaxCopper: grants per bot per level
    std::uint32_t outfitBudgetPerHour = 5000;  // AutoWow.Supply.OutfitBudgetPerHour: grants per team per game hour
    // Weapon / food floors (lane AN; off by default; GearUpgradePolicy.h FloorWeapons, ErrandsPolicy.h FloorKinds):
    bool outfitGear = false;                        // AutoWow.Supply.OutfitGear
    std::uint32_t outfitGearCopper = 25;            // AutoWow.Supply.OutfitGearCopper: + this x level^2 per bot per level
    std::uint32_t outfitGearBudgetPerHour = 50000;  // AutoWow.Supply.OutfitGearBudgetPerHour: + this per team per hour
    // Artisan upkeep (lane F):
    std::uint32_t artisanFreeSlots = 4;  // AutoWow.Supply.ArtisanFreeSlots: the artisan makes room below this many
                                         // free bag slots (0 = off; a blocked craft still makes one)
    std::uint32_t artisanMinLevel = 10;  // AutoWow.Supply.ArtisanMinLevel: below it a configured artisan is an
                                         // apprentice, an ordinary cohort adventurer (0 = off)
    // Rep storage (lane T, soak-s48-full-r1; a bug fix, on by default):
    bool repStore = true;           // AutoWow.Supply.RepStore: rep bags, bank stash, mail cap, batched mails
    std::uint32_t repMailCap = 80;  // AutoWow.Supply.RepMailCap: donors skip a rep holding this many mails
    std::uint32_t repKeep = 60;     // AutoWow.Supply.RepKeep: units per material the rep keeps in its bags
    // Throughput (lane U; all off by default):
    bool directRoutes = false;             // AutoWow.Supply.DirectRoutes: donor -> artisan, artisan -> member mails
    bool mailPickup = false;               // AutoWow.Supply.MailPickup: members take mail at mailboxes they pass
    std::uint32_t mailPickupYards = 40;    // AutoWow.Supply.MailPickupYards
    std::uint32_t mailRunMs = 1800000;     // AutoWow.Supply.MailRunMs: supply mail alone starts a town run this often
    bool mailOrders = false;               // AutoWow.Market.MailOrders: reps' buy orders filled by random bots' COD mail
    // Need-driven production (lane V; off by default):
    bool demandOnly = false;               // AutoWow.Supply.DemandOnly: consumer-first skill-ups, `waste` sales, consumer
                                           // field on order / deliver rows
    std::uint32_t repStockPerItem = 2;     // AutoWow.Supply.RepStockPerItem: gear lines, units per wanted recipe the rep
                                           // keeps beyond the open demand
    std::uint32_t gearMaxOrder = 4;        // AutoWow.Supply.GearMaxOrder: gear lines, top ranked needs one order serves
    std::uint32_t gearPayPct = 200;        // AutoWow.Supply.GearPayPct: pay per gear piece = vendor sell value * this / 100
    // Tinkers (lane AA; Products eng, off by default):
    std::uint32_t ammoTarget = 1000;       // AutoWow.Supply.AmmoTarget: bullets a gun hunter should hold (bags + mailbox)
    // Gear line bootstrap (lane bootstrap; off by default):
    bool gearBootstrap = false;            // AutoWow.Supply.GearBootstrap: skill-up orders for needs only skill blocks,
                                           // the starter recipes (gearStarters), the wider station radius
    // Tinkers guns (lane tinkers2; off by default):
    bool engGuns = false;                  // AutoWow.Supply.EngGuns: the eng table's gearGuns rows (anvil parts, Rough
                                           // Boomstick, the Bronze Tube skill bridge), the anvil station, tool purchases
    // Craft flow (lane craftflow, soak S75; all off by default):
    bool routeBagExtra = false;            // AutoWow.Supply.RouteBagExtra: donors route a bag tier's extra reagent (Heavy
                                           // Leather) to the bag house (ExtraRoom)
    bool gearStockSell = false;            // AutoWow.Supply.GearStockSell: a gear-line rep vendors pieces beyond
                                           // RepStockPerItem per recipe (PlanGearStockSale)
    bool gearSkillupRestock = false;       // AutoWow.Supply.GearSkillupRestock: GearBootstrap skill-up past the stock gate
                                           // when every option is stocked (PlanGearSkillupRestock)
    // Potion tiers (lane brewtiers; off by default):
    bool potionTiers = false;              // AutoWow.Supply.PotionTiers: the potions line's tierExtra rows (mana potions,
                                           // Greater Healing, the Elixir of Wisdom skill bridge; ActiveLine), a need per
                                           // family (RankStockTiers), heal products first (PickLineProduct)
};

// Raw materials routed with AutoWow.Supply.RouteRaw (3.3.5 item ids): each to its kind's house rep
// (AutoWow.Supply.House.Ore, default Smiths; AutoWow.Supply.House.Leather, default Tanners; AutoWow.Supply.House.Stone,
// default Tinkers: a kind whose house is not an AutoWow.Guilds.Houses house is off), each under RawCap.
inline constexpr std::uint32_t kOre[] = {2770, 2771, 2772};      // copper, tin, iron ore
inline constexpr std::uint32_t kLeather[] = {2934, 2318, 2319};  // ruined leather scraps, light, medium leather
inline constexpr std::uint32_t kStone[] = {2835, 2836, 2838};    // rough, coarse, heavy stone (blasting powder)

// A material a house wants routed now (AutoWow.Squad reads it): item and units of room (stock target minus the
// rep's stock, minus donations already queued).
struct MaterialNeed
{
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    bool first = false;  // the bag artisan's current tier (ClothDemand First): ranked before every other need
};

// ---- bag need (overlord) ----

// One online cohort member of the team. empty = equipped bag slots 19-22 without a bag; smaller = equipped
// general bags with fewer slots than the product bag; incoming = unequipped general bags in its inventory
// or its mailbox (already delivered, not yet worn).
struct Member
{
    std::uint32_t guid = 0;
    std::uint32_t empty = 0;
    std::uint32_t smaller = 0;
    std::uint32_t incoming = 0;
    bool rep = false;  // a house rep (RepStore): the hubs' storage ranks before every member
};

[[nodiscard]] inline std::uint32_t Wants(Member const& m)
{
    std::uint32_t const want = m.empty + m.smaller;
    return want > m.incoming ? want - m.incoming : 0;
}

// Members that want a bag, reps first, then most empty slots, then most smaller bags, ties the lower guid.
[[nodiscard]] inline std::vector<Member> RankNeeds(std::vector<Member> members)
{
    members.erase(std::remove_if(members.begin(), members.end(), [](Member const& m) { return !Wants(m); }),
                  members.end());
    std::sort(members.begin(), members.end(), [](Member const& a, Member const& b)
              {
                  if (a.rep != b.rep)
                      return a.rep;
                  if (a.empty != b.empty)
                      return a.empty > b.empty;
                  if (a.smaller != b.smaller)
                      return a.smaller > b.smaller;
                  return a.guid < b.guid;
              });
    return members;
}

[[nodiscard]] inline std::uint32_t TotalWant(std::vector<Member> const& ranked)
{
    std::uint32_t n = 0;
    for (Member const& m : ranked)
        n += Wants(m);
    return n;
}

// The two-step chain: bolt = clothPerBolt linen; bag = boltsPerBag bolts + threadPerBag thread (read from the
// recipe spells at load; these are the 3.3.5 values).
struct Recipe
{
    std::uint32_t clothPerBolt = 2;
    std::uint32_t boltsPerBag = 3;
    std::uint32_t threadPerBag = 3;
};

// Bags the linen and bolts can make.
[[nodiscard]] inline std::uint32_t BagsFrom(Recipe const& r, std::uint32_t linen, std::uint32_t bolts)
{
    if (!r.clothPerBolt || !r.boltsPerBag)
        return 0;
    return (linen / r.clothPerBolt + bolts) / r.boltsPerBag;
}

// Order size: the want not already covered by finished bags (rep + artisan), within what the house's
// materials can make and MaxOrder.
[[nodiscard]] inline std::uint32_t OrderSize(std::uint32_t want, std::uint32_t finished, std::uint32_t craftable,
                                             std::uint32_t maxOrder)
{
    std::uint32_t const open = want > finished ? want - finished : 0;
    return std::min({open, craftable, maxOrder});
}

// ---- rep -> artisan feed ----

// Linen the artisan still needs for `remaining` bags beyond what it holds (linen, bolts, finished bags).
// Below the bag recipe (canBag false) it is the skill-up stock instead: SkillupCloth minus its linen.
[[nodiscard]] inline std::uint32_t LinenShort(Recipe const& r, std::uint32_t remaining, bool canBag,
                                              std::uint32_t linen, std::uint32_t bolts, std::uint32_t bags,
                                              std::uint32_t skillupCloth)
{
    if (!canBag)
        return skillupCloth > linen ? skillupCloth - linen : 0;
    std::uint32_t const toMake = remaining > bags ? remaining - bags : 0;
    std::uint64_t const need = std::uint64_t(toMake) * r.boltsPerBag * r.clothPerBolt;
    std::uint64_t const have = std::uint64_t(bolts) * r.clothPerBolt + linen;
    return need > have ? static_cast<std::uint32_t>(need - have) : 0;
}

struct Stack
{
    std::uint32_t guid = 0;  // item guid low
    std::uint32_t count = 0;
};

// Whole stacks (ascending guid) until `units` are covered, at most kMaxMailStacks. Empty when units is 0.
[[nodiscard]] inline std::vector<std::uint32_t> PickStacks(std::vector<Stack> stacks, std::uint32_t units)
{
    std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    std::uint64_t got = 0;
    for (Stack const& s : stacks)
    {
        if (got >= units || out.size() >= kMaxMailStacks)
            break;
        out.push_back(s.guid);
        got += s.count;
    }
    return out;
}

// Copper the artisan lacks for the thread of `remaining` bags (at the vendor price) plus `extra` (a due
// trainer rank or recipe), beyond its purse.
[[nodiscard]] inline std::uint64_t CopperShort(Recipe const& r, std::uint32_t remaining, std::uint32_t thread,
                                               std::uint32_t threadPrice, std::uint64_t extra, std::uint64_t purse)
{
    std::uint64_t const needThread = std::uint64_t(remaining) * r.threadPerBag;
    std::uint64_t const want = (needThread > thread ? needThread - thread : 0) * threadPrice + extra;
    return want > purse ? want - purse : 0;
}

// ---- material tiers (AutoWow.Supply.Tiers) ----

// One tailoring tier: its cloth -> bolt -> bag chain. Entries checked against the 3.3.5 world DB (item_template,
// trainer_spell of trainer 74 = Stormwind 1346 / Orgrimmar 3363) and Spell.dbc / SkillLineAbility.dbc; the
// runtime re-checks outputs and reagent counts against the loaded spells and disables Tiers on a mismatch.
struct Tier
{
    std::uint32_t cloth = 0, boltSpell = 0, bolt = 0, bagSpell = 0, bag = 0, thread = 0;
    Recipe recipe;
    std::uint32_t extra = 0, extraPerBag = 0;  // a non-cloth reagent per bag (0 = none)
    std::uint32_t boltSkill = 0, bagSkill = 0;  // tailoring needed to learn the recipe (trainer ReqSkillRank)
    std::uint32_t boltGrey = 0, bagGrey = 0;    // no skill-up at or above (TrivialSkillLineRankHigh)
    std::uint32_t bagSlots = 0;
};

// Linen: Bolt of Linen Cloth 2963 (learned with the skill) -> Linen Bag 3755 (45, coarse thread).
// Wool: Bolt of Woolen Cloth 2964 (75) -> Woolen Bag 3757 (80, fine thread).
// Silk: Bolt of Silk Cloth 3839 (125) -> Small Silk Pack 3813 (150, fine thread + 2 Heavy Leather 4234).
inline constexpr Tier kTiers[] = {
    {kLinen, 2963, 2996, 3755, 4238, 2320, {2, 3, 3}, 0, 0, 1, 45, 50, 105, 6},
    {kWool, 2964, 2997, 3757, 4240, 2321, {3, 3, 1}, 0, 0, 75, 80, 105, 140, 8},
    {kSilk, 3839, 4305, 3813, 4245, 2321, {4, 3, 3}, 4234, 2, 125, 150, 145, 200, 10},
};
inline constexpr std::size_t kTierCount = std::size(kTiers);
inline constexpr std::uint8_t kNoTier = 0xFF;

// Per-tier product option (ascending tier): known = the artisan knows the bag recipe; want = the members' bag
// want for that tier's slot count; craftable = bags the house's cloth / bolts / extras make now.
struct ProductOption
{
    bool known = false;
    std::uint32_t want = 0;
    std::uint32_t craftable = 0;
};

// The product: the highest tier the artisan can make that members want and the house can make now; else the
// highest known wanted tier (its materials are bought on the market); kNoTier = nothing to make.
[[nodiscard]] inline std::uint8_t PickProduct(std::vector<ProductOption> const& tiers)
{
    for (std::size_t i = tiers.size(); i-- > 0;)
        if (tiers[i].known && tiers[i].want && tiers[i].craftable)
            return static_cast<std::uint8_t>(i);
    for (std::size_t i = tiers.size(); i-- > 0;)
        if (tiers[i].known && tiers[i].want)
            return static_cast<std::uint8_t>(i);
    return kNoTier;
}

// The tier the artisan works toward: the highest known tier members want, craftable now or not (PickProduct's
// fallback). Its cloth is what the house should gather, even while spare lower cloth makes a lower product craftable.
[[nodiscard]] inline std::uint8_t PickGoal(std::vector<ProductOption> const& tiers)
{
    for (std::size_t i = tiers.size(); i-- > 0;)
        if (tiers[i].known && tiers[i].want)
            return static_cast<std::uint8_t>(i);
    return kNoTier;
}

// A skill-up candidate: one recipe (a tier's bolt or bag). cost = vendor value of one cast's materials (copper).
struct SkillupOption
{
    std::uint32_t spell = 0;
    std::uint8_t tier = 0;
    bool bag = false;
    bool known = false;
    std::uint32_t grey = 0;
    bool stocked = false;  // the house (rep + artisan) holds one cast's cloth (and extras)
    std::uint64_t cost = 0;
    bool consumer = false;  // DemandOnly: its output has a consumer (a member wants it, or a wanted product uses it)
};

// The skill-up recipe: known, still below grey at `skill`, (needStock) stocked; cheapest, ties the lower spell.
// Returns the index into `options`, or -1.
[[nodiscard]] inline int PickSkillup(std::uint32_t skill, std::vector<SkillupOption> const& options, bool needStock)
{
    int best = -1;
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        SkillupOption const& o = options[i];
        if (!o.known || skill >= o.grey || (needStock && !o.stocked))
            continue;
        SkillupOption const* b = best < 0 ? nullptr : &options[static_cast<std::size_t>(best)];
        if (!b || o.cost < b->cost || (o.cost == b->cost && o.spell < b->spell))
            best = static_cast<int>(i);
    }
    return best;
}

// DemandOnly (lane V): a skill-up recipe whose output has a consumer first; a consumer-less one (its output is sold:
// `waste`) only as the last resort. Off: PickSkillup.
[[nodiscard]] inline int PickSkillupFor(std::uint32_t skill, std::vector<SkillupOption> const& options, bool needStock,
                                        bool demandOnly)
{
    if (demandOnly)
    {
        std::vector<SkillupOption> used = options;
        for (SkillupOption& o : used)
            o.known = o.known && o.consumer;
        if (int const i = PickSkillup(skill, used, needStock); i >= 0)
            return i;
    }
    return PickSkillup(skill, options, needStock);
}

// Donor room per cloth: each cloth under its own cap, so a full out-of-reach tier cannot starve a usable one.
template <std::size_t N>
[[nodiscard]] std::array<std::uint32_t, N> ClothRooms(std::array<std::uint32_t, N> const& stock, std::uint32_t cap,
                                                      bool repReady)
{
    std::array<std::uint32_t, N> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = repReady && cap > stock[i] ? cap - stock[i] : 0;
    return out;
}

// ---- faction AH market (AutoWow.Supply.Market) ----

// Units a house is short of: need minus holdings.
[[nodiscard]] inline std::uint32_t Short(std::uint64_t need, std::uint64_t have)
{
    return need > have ? static_cast<std::uint32_t>(std::min<std::uint64_t>(need - have, 0xFFFFFFFFu)) : 0;
}

// A cloth tier is out of the artisan's reach below its bolt recipe's skill.
[[nodiscard]] inline bool OutOfReach(Tier const& t, std::uint32_t skill) { return skill < t.boltSkill; }

// The highest cloth tier within the artisan's reach (linen always).
[[nodiscard]] inline std::size_t ReachTier(std::uint32_t skill)
{
    std::size_t out = 0;
    for (std::size_t i = 1; i < kTierCount; ++i)
        if (!OutOfReach(kTiers[i], skill))
            out = i;
    return out;
}

// Market: a cloth tier the rep may list (above SellKeep). The next tier above the artisan's reach is a reserve,
// not surplus (soak-s47-full-r1: the Alliance rep listed its wool while its tailor was below 75; at 104 on Woolen
// Bags the house had none).
[[nodiscard]] inline bool Listable(std::size_t tier, std::uint32_t skill) { return tier > ReachTier(skill) + 1; }

// Squad demand for cloth tier `i`: the tiers the artisan works now (goal = PickGoal, skill-up; kNoTier = none) come
// First; a reachable tier above them stays Normal; a tier below every worked one is Done (its use ended:
// soak-s47-full-r1, a 104-skill tailor on Woolen Bags had its squad farming linen). Nothing worked: every reachable
// tier Normal. A reachable tier the house's cloth_gear orders still lack (gearShort, GearClothShort) is never Done
// (soak-s54-full-r1: the Weavers posted gear orders on bolts the bag line had no use for).
enum class ClothDemand : std::uint8_t
{
    Done = 0,
    Normal = 1,
    First = 2
};

[[nodiscard]] inline ClothDemand ClothDemandOf(std::size_t i, std::uint32_t skill, std::uint8_t goal,
                                               std::uint8_t skillup, std::uint32_t gearShort = 0)
{
    if (i && OutOfReach(kTiers[i], skill))
        return ClothDemand::Done;
    if (i == goal || i == skillup)
        return ClothDemand::First;
    std::uint8_t const floor = std::min(goal, skillup);  // kNoTier = 0xFF: none
    return floor == kNoTier || i > floor || gearShort ? ClothDemand::Normal : ClothDemand::Done;
}

// Whole stacks (ascending guid) to list while at least `keep` units stay; at most kMaxMailStacks.
[[nodiscard]] inline std::vector<std::uint32_t> SellStacks(std::vector<Stack> stacks, std::uint32_t keep)
{
    std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::uint64_t left = 0;
    for (Stack const& s : stacks)
        left += s.count;
    std::vector<std::uint32_t> out;
    for (Stack const& s : stacks)
    {
        if (out.size() >= kMaxMailStacks || left < std::uint64_t(keep) + s.count)
            continue;
        out.push_back(s.guid);
        left -= s.count;
    }
    return out;
}

// What the rep wants from the AH: item, units short, vendor sell value per unit.
struct MarketWant
{
    std::uint32_t item = 0;
    std::uint32_t units = 0;
    std::uint32_t sellPrice = 0;
};

// An AH listing (the rep's own excluded by the caller).
struct MarketListing
{
    std::uint32_t id = 0;  // auction id: stable, never reused
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    std::uint32_t buyout = 0;  // 0 = bid only (never bought)
};

// The rep's own listings to take back for one visit (its buyouts skip them): per want (in order), its listings of
// that item by auction id while the want is open (the last may overshoot); each want shrinks by what they cover
// (soak-s47-full-r1: the only wool on the Alliance house was the rep's own, so its wool want never bought).
[[nodiscard]] inline std::vector<MarketListing> PlanMarketCancels(std::vector<MarketListing> own,
                                                                  std::vector<MarketWant>& wants)
{
    std::sort(own.begin(), own.end(), [](MarketListing const& a, MarketListing const& b) { return a.id < b.id; });
    std::vector<MarketListing> out;
    for (MarketWant& w : wants)
        for (MarketListing const& l : own)
        {
            if (!w.units)
                break;
            if (l.item != w.item || !l.count)
                continue;
            w.units -= std::min(w.units, l.count);
            out.push_back(l);
        }
    return out;
}

// Buyouts for one visit: per want (in order), the cheapest listings per unit (then auction id) while the want
// is open (the last one may overshoot it), each unit at most sellPrice * maxPct / 100, all within `budget`.
[[nodiscard]] inline std::vector<MarketListing> PlanMarketBuys(std::vector<MarketListing> listings,
                                                               std::vector<MarketWant> const& wants,
                                                               std::uint32_t maxPct, std::uint64_t budget)
{
    // Per-unit order without floats: a/ac < b/bc  <=>  a*bc < b*ac.
    std::sort(listings.begin(), listings.end(), [](MarketListing const& a, MarketListing const& b)
              {
                  std::uint64_t const l = std::uint64_t(a.buyout) * std::max<std::uint32_t>(1, b.count);
                  std::uint64_t const r = std::uint64_t(b.buyout) * std::max<std::uint32_t>(1, a.count);
                  return l != r ? l < r : a.id < b.id;
              });
    std::vector<MarketListing> out;
    for (MarketWant const& w : wants)
    {
        std::uint64_t got = 0;
        for (MarketListing const& l : listings)
        {
            if (got >= w.units)
                break;
            if (l.item != w.item || !l.buyout || !l.count ||
                std::uint64_t(l.buyout) * 100 > std::uint64_t(w.sellPrice) * maxPct * l.count || l.buyout > budget)
                continue;
            budget -= l.buyout;
            got += l.count;
            out.push_back(l);
        }
    }
    return out;
}

// ---- artisan craft ----

enum class Craft : std::uint8_t
{
    None = 0,
    Bolt = 1,
    Bag = 2
};

// Next cast: a bag while the order wants more than the finished ones and the reagents are in hand; else a
// bolt from spare linen (below the bag recipe every bolt is a skill-up and a future reagent; with the recipe
// only the bolts the order still lacks).
[[nodiscard]] inline Craft NextCraft(Recipe const& r, std::uint32_t remaining, bool canBag, std::uint32_t linen,
                                     std::uint32_t bolts, std::uint32_t thread, std::uint32_t bags)
{
    std::uint32_t const toMake = remaining > bags ? remaining - bags : 0;
    if (canBag && toMake && bolts >= r.boltsPerBag && thread >= r.threadPerBag)
        return Craft::Bag;
    if (linen < r.clothPerBolt)
        return Craft::None;
    if (!canBag)
        return Craft::Bolt;
    return std::uint64_t(bolts) < std::uint64_t(toMake) * r.boltsPerBag ? Craft::Bolt : Craft::None;
}

// Tiers: NextCraft for the product tier (its extra reagent gates the bag like thread does); with no order left,
// the skill-up recipe (skillup = kTiers index, kNoTier = none): its bag when the reagents are in hand, else a
// bolt from its cloth (for a bag skill-up the bolts are its reagent).
struct Hand
{
    std::uint32_t cloth = 0, bolts = 0, thread = 0, extra = 0, bags = 0;
};

[[nodiscard]] inline Craft NextTierCraft(Tier const& product, std::uint32_t remaining, bool canBag, Hand const& p,
                                         Tier const* skillup, bool skillupBag, Hand const& s)
{
    if (remaining)
        return NextCraft(product.recipe, remaining, canBag, p.cloth, p.bolts,
                         p.extra >= product.extraPerBag ? p.thread : 0, p.bags);
    if (!skillup)
        return Craft::None;
    Recipe const& r = skillup->recipe;
    if (skillupBag && s.bolts >= r.boltsPerBag && s.thread >= r.threadPerBag && s.extra >= skillup->extraPerBag)
        return Craft::Bag;
    if (skillupBag && s.bolts >= r.boltsPerBag)
        return Craft::None;  // bolts ready, thread / extra still to come
    return s.cloth >= r.clothPerBolt ? Craft::Bolt : Craft::None;
}

// ---- routing (adventurer sell stop) ----

// Cloth a donor mails: a cohort non-tailor, routing on, and house room left (ClothCap minus the house stock).
[[nodiscard]] inline bool RoutesCloth(bool routeCloth, bool inCohort, bool tailor, std::uint32_t room)
{
    return routeCloth && inCohort && !tailor && room > 0;
}

[[nodiscard]] inline std::uint32_t ClothRoom(std::uint32_t stock, std::uint32_t cap, bool repReady)
{
    return repReady && cap > stock ? cap - stock : 0;
}

// RouteBagExtra (lane craftflow; soak S75: members wanted 92 / 86 Small Silk Packs while the Weavers held 214 / 209 silk
// and 1 / 0 Heavy Leather, the pack's extra reagent: no route carried it, cohort skinners vendored theirs and the rep's
// buy orders went unfilled). Donor room for a tier's extra reagent: what the bags members want that the house cloth
// makes still lack (held = house stock, mail included); 0 without an extra, a known recipe or a ready rep.
[[nodiscard]] inline std::uint32_t ExtraRoom(Tier const& t, bool known, std::uint32_t want, std::uint32_t clothBags,
                                             std::uint32_t held, bool repReady)
{
    if (!t.extra || !known || !repReady)
        return 0;
    return Short(std::uint64_t(std::min(want, clothBags)) * t.extraPerBag, held);
}

// ---- delivery, pay, XP ----

struct Delivery
{
    std::uint32_t guid = 0;
    std::uint32_t bags = 0;
};

// The rep's bags to the ranked members in order, each up to its want.
[[nodiscard]] inline std::vector<Delivery> PlanDeliveries(std::vector<Member> const& ranked, std::uint32_t bags)
{
    std::vector<Delivery> out;
    for (Member const& m : ranked)
    {
        if (!bags)
            break;
        std::uint32_t const n = std::min(std::min(Wants(m), bags), kMaxMailStacks);
        if (!n)
            continue;
        out.push_back({m.guid, n});
        bags -= n;
    }
    return out;
}

// Treasury pay for delivered bags: vendor sell value * pct / 100 each.
[[nodiscard]] inline std::uint64_t BagPay(std::uint32_t sellPrice, std::uint32_t pct, std::uint32_t count)
{
    return std::uint64_t(sellPrice) * pct / 100 * count;
}

inline constexpr std::uint32_t kXpDivisor = 20;  // auto work XP: about one level per 20 items at that level

// XP per item (artisan) or per deal (rep): the configured value, else XpForLevel(level) / kXpDivisor (min 1).
[[nodiscard]] inline std::uint32_t WorkXp(std::uint32_t configured, std::uint32_t xpForLevel)
{
    if (configured)
        return configured;
    return std::max<std::uint32_t>(1, xpForLevel / kXpDivisor);
}

// Bags the rep sells with no open need, keeping SurplusKeep.
[[nodiscard]] inline std::uint32_t Surplus(std::uint32_t openWant, std::uint32_t repBags, std::uint32_t keep)
{
    return !openWant && repBags > keep ? repBags - keep : 0;
}

// ---- rep storage (AutoWow.Supply.RepStore) ----
// soak-s48-full-r1: the Alliance Weavers rep (level 3, 16 backpack slots, no bag) is the house hub; with Tiers its
// ClothCap alone is 600 units (30 stacks). It wears bags, stashes materials in its character bank and donors stop
// at RepMailCap mails (the core refuses a mail at 100).

// A loose general bag the rep holds.
struct LooseBag
{
    std::uint32_t guid = 0;  // item guid low
    std::uint32_t slots = 0;
};

// The rep's own bags to wear in its `empty` bag slots: the biggest first, ties the lower guid.
[[nodiscard]] inline std::vector<std::uint32_t> BagsToWear(std::vector<LooseBag> bags, std::uint32_t empty)
{
    std::sort(bags.begin(), bags.end(), [](LooseBag const& a, LooseBag const& b)
              { return a.slots != b.slots ? a.slots > b.slots : a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    for (LooseBag const& b : bags)
        if (out.size() < empty && b.slots)
            out.push_back(b.guid);
    return out;
}

// Donors route to a rep only while its mailbox holds fewer than `cap` mails.
[[nodiscard]] inline bool MailRoom(std::uint32_t mails, std::uint32_t cap) { return mails < cap; }

// The rep's bags (backpack + worn bags: `total` slots, `free` empty) are more than 75% full.
[[nodiscard]] inline bool StashDue(std::uint32_t free, std::uint32_t total)
{
    std::uint64_t const used = total > free ? total - free : 0;
    return used * 4 > std::uint64_t(total) * 3;
}

struct StashStack
{
    std::uint32_t guid = 0;  // item guid low
    std::uint32_t item = 0;
    std::uint32_t count = 0;
};

[[nodiscard]] inline std::uint64_t UnitsOf(std::vector<StashStack> const& stacks, std::uint32_t item)
{
    std::uint64_t n = 0;
    for (StashStack const& s : stacks)
        if (s.item == item)
            n += s.count;
    return n;
}

// Bank deposit (bags over 75% full): whole loose material stacks, ascending guid, while that item's loose units left
// stay at least `keep`; at most `bankFree` stacks.
[[nodiscard]] inline std::vector<std::uint32_t> PlanStash(std::vector<StashStack> loose, std::uint32_t keep,
                                                          std::uint32_t bankFree)
{
    std::sort(loose.begin(), loose.end(), [](StashStack const& a, StashStack const& b) { return a.guid < b.guid; });
    std::vector<std::pair<std::uint32_t, std::uint64_t>> left;  // item -> loose units left (bounded by the bags)
    std::vector<std::uint32_t> out;
    for (StashStack const& s : loose)
    {
        if (out.size() >= bankFree)
            break;
        auto it = std::find_if(left.begin(), left.end(), [&](auto const& l) { return l.first == s.item; });
        if (it == left.end())
            it = left.insert(left.end(), {s.item, UnitsOf(loose, s.item)});
        if (it->second < std::uint64_t(keep) + s.count)
            continue;
        out.push_back(s.guid);
        it->second -= s.count;
    }
    return out;
}

// Bank withdrawal: banked stacks (ascending guid) of an item whose loose units are under `keep` (the last may
// overshoot), one free slot each, never leaving the bags over 75% full (no stash / refill loop).
[[nodiscard]] inline std::vector<std::uint32_t> PlanUnstash(std::vector<StashStack> banked,
                                                            std::vector<StashStack> const& loose, std::uint32_t keep,
                                                            std::uint32_t free, std::uint32_t total)
{
    std::sort(banked.begin(), banked.end(), [](StashStack const& a, StashStack const& b) { return a.guid < b.guid; });
    std::vector<std::pair<std::uint32_t, std::uint64_t>> have;
    std::vector<std::uint32_t> out;
    for (StashStack const& s : banked)
    {
        if (!free || StashDue(free - 1, total))
            break;
        auto it = std::find_if(have.begin(), have.end(), [&](auto const& h) { return h.first == s.item; });
        if (it == have.end())
            it = have.insert(have.end(), {s.item, UnitsOf(loose, s.item)});
        if (it->second >= keep)
            continue;
        out.push_back(s.guid);
        it->second += s.count;
        --free;
    }
    return out;
}

// ---- product catalog (AutoWow.Supply.Products) ----

// A product line: one house's profession making one family of goods for the members. Wire-stable ids (the
// ledger `line` field and the Products bit), append only.
enum class Line : std::uint8_t
{
    Bags = 0,
    Potions = 1,
    ClothGear = 2,   // lane V gear lines (bespoke runtime GearTick over ProductLine::gear)
    MailGear = 3,    // data hook: no recipe table yet (a line with none turns itself off at load)
    LeatherGear = 4,
    Engineering = 5  // lane AA Tinkers: gun hunters' ammo on the gear runtime (GearTick over ProductLine::gear)
};
inline constexpr std::uint8_t kNoLine = 0xFF;

// Where a reagent comes from: Route = adventurers mail it to the house rep (and, with Market, the rep buys what
// is short on the faction AH); Vendor = the artisan buys it at the vendor near home (treasury-funded copper);
// Market = the rep buys it on the faction AH only; Craft = the artisan makes it (the product of a lower tier).
enum class Source : std::uint8_t
{
    Route = 0,
    Vendor = 1,
    Market = 2,
    Craft = 3
};

enum class NeedRule : std::uint8_t
{
    BagSlots = 0,    // empty / smaller equipped bag slots (RankNeeds)
    PotionStock = 1, // member stock of its best usable tier below PotionTarget (RankStock)
    GearSlots = 2,   // equipment slots a known recipe's product upgrades (stock "item upgrade"; RankGearNeeds)
    AmmoStock = 3    // a gun hunter under AmmoTarget bullets, the shot not weaker than its loaded ammo (WantsAmmo)
};

enum class Consumer : std::uint8_t
{
    EquipBag = 0,     // the stock equip action wears a delivered bag
    DrinkAtLowHp = 1, // the stock combat "potions" strategy: critical health -> healthstone -> healing potion
    EquipGear = 2,    // the stock "equip upgrades packet action" (non-combat "random" trigger) wears a delivered piece
    LoadAmmo = 3      // the gear tick loads a gun hunter's house shot (Player::SetAmmo, LoadsAmmo): the stock ammo
                      // equip never fires (ItemUsageValue::QueryItemUsageForAmmo's class check is always true)
};

struct Reagent
{
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    Source source = Source::Route;
};

inline constexpr std::size_t kMaxReagents = 3;
inline constexpr std::size_t kMaxLineTiers = 8;  // potions: 3 rows + 5 PotionTiers rows (tierExtra)

// A line tier's family (PotionStock need, PotionTiers): a member wants its best tier of each family it uses; within a
// family the table order is ascending (a later row is a better tier). Bridge = a skill-up recipe no member wants.
inline constexpr std::uint8_t kFamilyHeal = 0, kFamilyMana = 1, kFamilyBridge = 2;

// One recipe of a single-step line: spell -> one product per cast.
struct LineTier
{
    std::uint32_t spell = 0, product = 0;
    std::uint32_t skill = 0;     // profession skill to learn it (trainer ReqSkillRank; 1 = learned with the skill)
    std::uint32_t grey = 0;      // no skill-up at or above (SkillLineAbility TrivialSkillLineRankHigh)
    std::uint32_t reqLevel = 0;  // product item RequiredLevel (the need rule's "usable at their level")
    std::array<Reagent, kMaxReagents> reagents{};
    std::uint8_t family = kFamilyHeal;  // potions (BestTier); gear rows leave it
};

struct ProductLine
{
    Line id = Line::Bags;
    char const* name = "";   // AutoWow.Supply.Products token and ledger `line`
    char const* key = "";    // config suffix: AutoWow.Supply.House.<key>, AutoWow.Supply.Artisan.Learn.<key>
    char const* house = "";  // default house (bags: AutoWow.Supply.House)
    char const* learn = "";  // default trainer spells the artisan learns (profession ranks), plus tier recipes
    std::uint32_t skillLine = 0;
    NeedRule need = NeedRule::BagSlots;
    Consumer consumer = Consumer::EquipBag;
    std::array<LineTier, kMaxLineTiers> tiers{};
    std::uint8_t tierCount = 0;  // 0 = a bespoke runtime (bags: the bolt -> bag chain over kTiers)
    LineTier const* gear = nullptr;  // gear lines (NeedRule::GearSlots): recipe table, intermediates (reqLevel 0) first
    std::uint8_t gearCount = 0;
    std::uint8_t gearStarters = 0;  // the last rows of `gear`: in the table only with GearBootstrap (GearTable)
    std::uint8_t gearGuns = 0;      // the last rows of `gear`: in the table only with EngGuns (GearTable)
    std::uint8_t tierExtra = 0;     // tiers[tierCount .. tierCount + tierExtra): in the table only with PotionTiers
                                    // (ActiveLine; off: tierCount rows, the lane D table as was)
};

// A recipe table the catalog helpers below walk (ProductLine tiers, or a gear line's GearTable).
struct RecipeTable
{
    LineTier const* tiers = nullptr;
    std::uint8_t tierCount = 0;
};

// Gear line recipe tables (lane V), checked against the 3.3.5 world DB (trainer_spell ReqSkillRank, item_template
// RequiredLevel) and Spell.dbc / SkillLineAbility.dbc (outputs, reagent counts, TrivialSkillLineRankHigh = grey); the
// runtime re-checks outputs, reagent counts and RequiredLevel against the loaded spells / items and turns a line off on
// a mismatch. Only trainer-taught recipes (no BoP / world-drop patterns) whose reagents the house gets: routed cloth /
// leather, the house's own intermediates (Craft) and vendor thread / dye (Vendor: the trade-supplies vendor near home).
// Tailoring (trainer 74 = Stormwind 1346 / Orgrimmar 3363; vendor Stormwind 1347 / Orgrimmar 5817-3364): cloth armor
// usable at L3-34. Skipped: boots (Light Leather is routed to the Tanners), leather / pearl / spider silk / elemental /
// buckle / potion reagents, shirts and dresses (no stats), mageweave (not routed).
inline constexpr LineTier kTailorGear[] = {
    // intermediates (RequiredLevel 0, not equipment): the bolts
    {2963, 2996, 1, 50, 0, {{{kLinen, 2, Source::Route}, {}, {}}}},
    {2964, 2997, 75, 105, 0, {{{kWool, 3, Source::Route}, {}, {}}}},
    {3839, 4305, 125, 145, 0, {{{kSilk, 4, Source::Route}, {}, {}}}},
    // cloth armor: spell, product, skill, grey, RequiredLevel
    {2385, 2568, 10, 70, 3, {{{2996, 1, Source::Craft}, {2320, 1, Source::Vendor}, {}}}},     // Brown Linen Vest
    {8776, 7026, 15, 85, 4, {{{2996, 1, Source::Craft}, {2320, 1, Source::Vendor}, {}}}},     // Linen Belt
    {3914, 4343, 30, 90, 5, {{{2996, 2, Source::Craft}, {2320, 1, Source::Vendor}, {}}}},     // Brown Linen Pants
    {7623, 6238, 30, 90, 5, {{{2996, 3, Source::Craft}, {2320, 1, Source::Vendor}, {}}}},     // Brown Linen Robe
    {3840, 4307, 35, 95, 5, {{{2996, 2, Source::Craft}, {2320, 1, Source::Vendor}, {}}}},     // Heavy Linen Gloves
    {2397, 2580, 60, 120, 7, {{{2996, 2, Source::Craft}, {2320, 3, Source::Vendor}, {}}}},    // Reinforced Linen Cape
    {3841, 4308, 60, 120, 7,
     {{{2996, 3, Source::Craft}, {2320, 2, Source::Vendor}, {2605, 1, Source::Vendor}}}},     // Green Linen Bracers
    {3842, 4309, 70, 130, 9, {{{2996, 4, Source::Craft}, {2321, 2, Source::Vendor}, {}}}},    // Handstitched Linen Britches
    {12046, 10047, 75, 135, 10, {{{2996, 4, Source::Craft}, {2321, 1, Source::Vendor}, {}}}}, // Simple Kilt
    {2402, 2584, 75, 135, 11, {{{2997, 1, Source::Craft}, {2321, 1, Source::Vendor}, {}}}},   // Woolen Cape
    {2399, 2582, 85, 145, 12,
     {{{2997, 2, Source::Craft}, {2321, 2, Source::Vendor}, {2605, 1, Source::Vendor}}}},     // Green Woolen Vest
    {3843, 4310, 85, 145, 12, {{{2997, 3, Source::Craft}, {2321, 1, Source::Vendor}, {}}}},   // Heavy Woolen Gloves
    {3848, 4314, 110, 170, 17, {{{2997, 3, Source::Craft}, {2321, 2, Source::Vendor}, {}}}},  // Double-stitched Woolen Shoulders
    {3850, 4316, 110, 170, 17, {{{2997, 5, Source::Craft}, {2321, 4, Source::Vendor}, {}}}},  // Heavy Woolen Pants
    {8758, 7046, 140, 190, 23,
     {{{4305, 4, Source::Craft}, {6260, 2, Source::Vendor}, {2321, 3, Source::Vendor}}}},     // Azure Silk Pants
    {8760, 7048, 145, 165, 24,
     {{{4305, 2, Source::Craft}, {6260, 2, Source::Vendor}, {2321, 1, Source::Vendor}}}},     // Azure Silk Hood
    {3859, 4324, 150, 200, 25, {{{4305, 5, Source::Craft}, {6260, 4, Source::Vendor}, {}}}},  // Azure Silk Vest
    {8762, 7050, 160, 180, 27, {{{4305, 3, Source::Craft}, {2321, 2, Source::Vendor}, {}}}},  // Silk Headband
    {8791, 7058, 185, 225, 30,
     {{{4305, 4, Source::Craft}, {2604, 2, Source::Vendor}, {2321, 2, Source::Vendor}}}},     // Crimson Silk Vest
    {8774, 7057, 180, 230, 31, {{{4305, 5, Source::Craft}, {4291, 2, Source::Vendor}, {}}}},  // Green Silken Shoulders
    {8799, 7062, 195, 235, 34,
     {{{4305, 4, Source::Craft}, {2604, 2, Source::Vendor}, {4291, 2, Source::Vendor}}}},     // Crimson Silk Pantaloons
};
// Leatherworking (trainer 61 = Stormwind 5564 / Orgrimmar 3365; same trade-supplies vendors): leather armor usable at
// L5-25 from Light Leather (routed to the Tanners by RouteRaw) and its Medium / Heavy Leather. Skipped: cured hides (no
// hide routing), elixir / spider silk / buckle / oil / elemental reagents, kits, quivers, ammo pouches.
inline constexpr LineTier kLeatherGear[] = {
    {20648, 2319, 100, 110, 0, {{{2318, 4, Source::Route}, {}, {}}}},  // Medium Leather
    {20649, 4234, 150, 160, 0, {{{2319, 5, Source::Craft}, {}, {}}}},  // Heavy Leather
    {2153, 2303, 15, 75, 5, {{{2318, 4, Source::Route}, {2320, 1, Source::Vendor}, {}}}},    // Handstitched Leather Pants
    {3753, 4237, 25, 85, 5, {{{2318, 6, Source::Route}, {2320, 1, Source::Vendor}, {}}}},    // Handstitched Leather Belt
    {2160, 2300, 40, 100, 7, {{{2318, 8, Source::Route}, {2320, 4, Source::Vendor}, {}}}},   // Embossed Leather Vest
    {3756, 4239, 55, 115, 8, {{{2318, 3, Source::Route}, {2320, 2, Source::Vendor}, {}}}},   // Embossed Leather Gloves
    {2162, 2310, 60, 120, 8, {{{2318, 5, Source::Route}, {2320, 2, Source::Vendor}, {}}}},   // Embossed Leather Cloak
    {2161, 2309, 55, 115, 10, {{{2318, 8, Source::Route}, {2320, 5, Source::Vendor}, {}}}},  // Embossed Leather Boots
    {9065, 7281, 70, 130, 9, {{{2318, 6, Source::Route}, {2320, 4, Source::Vendor}, {}}}},   // Light Leather Bracers
    {3763, 4246, 80, 140, 11, {{{2318, 6, Source::Route}, {2320, 2, Source::Vendor}, {}}}},  // Fine Leather Belt
    {2159, 2308, 85, 135, 10, {{{2318, 10, Source::Route}, {2321, 2, Source::Vendor}, {}}}}, // Fine Leather Cloak
    {2167, 2315, 100, 150, 15,
     {{{2319, 4, Source::Craft}, {2321, 2, Source::Vendor}, {4340, 1, Source::Vendor}}}},    // Dark Leather Boots
    {2168, 2316, 110, 160, 17,
     {{{2319, 8, Source::Craft}, {2321, 1, Source::Vendor}, {4340, 1, Source::Vendor}}}},    // Dark Leather Cloak
    {7135, 5961, 115, 165, 18,
     {{{2319, 12, Source::Craft}, {4340, 1, Source::Vendor}, {2321, 1, Source::Vendor}}}},   // Dark Leather Pants
    {3764, 4247, 145, 195, 24, {{{2319, 14, Source::Craft}, {2321, 4, Source::Vendor}, {}}}}, // Hillman's Leather Gloves
    {3760, 3719, 150, 190, 25, {{{4234, 5, Source::Craft}, {2321, 2, Source::Vendor}, {}}}},  // Hillman's Cloak
    // Starters (GearBootstrap only, kLeatherStarters): learned with the skill (SkillLineAbility AcquireMethod 1, no
    // trainer_spell row), the only casts below Handstitched Leather Pants (15): an artisan at skill 1 levels on them.
    {2881, 2318, 1, 40, 0, {{{2934, 3, Source::Route}, {}, {}}}},                          // Light Leather <- scraps
    {2149, 2302, 1, 70, 3, {{{2318, 2, Source::Route}, {2320, 1, Source::Vendor}, {}}}},   // Handstitched Leather Boots
    {9058, 7276, 1, 70, 4, {{{2318, 2, Source::Route}, {2320, 1, Source::Vendor}, {}}}},   // Handstitched Leather Cloak
};
inline constexpr std::uint8_t kLeatherStarters = 3;
// Engineering (lane AA, Tinkers; trainer 92 = Stormwind 5518 / Orgrimmar 11017, 466 / 491 yards from the homes) and its
// smelting (Mining 186, trainer 80 = Stormwind 5513 / Orgrimmar 3357; spell focus 3: a forge near home): gun hunters'
// shot (200 per cast) from routed stone (House.Stone) and ore (House.Ore). Only reagents the house gets and a consumer
// the stock AI has. Dropped: goggles (RequiredSkill Engineering: no adventurer can wear them), scopes (no stock path
// applies one to a weapon; tubes need an anvil + Blacksmith Hammer), bombs / dynamite (no stock AI throws them), arrows
// (engineering makes none). Engineering 60 -> 75 (Heavy Shot) has no anvil-free recipe: the Heavy / Solid rows wait for
// the EngGuns rows (the anvil bridge).
inline constexpr LineTier kEngGear[] = {
    // intermediates (RequiredLevel 0): blasting powder, then the bars (Mining; skill = mining)
    {3918, 4357, 1, 40, 0, {{{2835, 1, Source::Route}, {}, {}}}},     // Rough Blasting Powder (learned with the skill)
    {3929, 4364, 75, 95, 0, {{{2836, 1, Source::Route}, {}, {}}}},    // Coarse Blasting Powder
    {3945, 4377, 125, 145, 0, {{{2838, 1, Source::Route}, {}, {}}}},  // Heavy Blasting Powder
    {2657, 2840, 1, 70, 0, {{{2770, 1, Source::Route}, {}, {}}}},     // Smelt Copper (learned with Mining)
    {3304, 3576, 65, 75, 0, {{{2771, 1, Source::Route}, {}, {}}}},    // Smelt Tin
    {2659, 2841, 65, 115, 0, {{{2840, 1, Source::Craft}, {3576, 1, Source::Craft}, {}}}},  // Smelt Bronze (2 bars)
    // shot (bullets): spell, product, skill, grey, RequiredLevel
    {3920, 8067, 1, 60, 5, {{{4357, 1, Source::Craft}, {2840, 1, Source::Craft}, {}}}},     // Crafted Light Shot
    {3930, 8068, 75, 95, 15, {{{4364, 1, Source::Craft}, {2840, 1, Source::Craft}, {}}}},   // Crafted Heavy Shot
    {3947, 8069, 125, 145, 30, {{{4377, 1, Source::Craft}, {2841, 1, Source::Craft}, {}}}}, // Crafted Solid Shot
    // Guns (lane tinkers2, EngGuns only, kEngGuns): an anvil (spell focus 1) and a Blacksmith Hammer (TotemCategory 162,
    // kTools) for every row; Weak Flux / Wooden Stock from the trade-supplies vendor near home (Stormwind 1286, Orgrimmar
    // 5817 sell both and the hammer). Rough Boomstick is a ranged upgrade for a gun user (hunter, warrior, rogue with the
    // Guns skill) whose ranged slot is worse; worn, a hunter's bullet need follows. Copper Tube / Rough Boomstick (grey
    // 110) bridge engineering 60 -> 75 (Heavy Shot), Bronze Tube (grey 155) 110 -> 125 (Solid Shot): skill-up casts only
    // toward a blocked need (GearBootstrap). Dropped: Deadly Blunderbuss (Medium Leather is routed to the Tanners; the
    // Arclight Spanner no vendor sells), Silver-plated Shotgun (Silver Bar, a wool-cloth gizmo, 4 reagents), Lovingly
    // Crafted Boomstick / Moonsight Rifle (schematic items, not trainer-taught).
    {3922, 4359, 30, 60, 0, {{{2840, 1, Source::Craft}, {}, {}}}},                               // Handful of Copper Bolts
    {3924, 4361, 50, 110, 0, {{{2840, 2, Source::Craft}, {2880, 1, Source::Vendor}, {}}}},        // Copper Tube
    {3925, 4362, 50, 110, 5,
     {{{4361, 1, Source::Craft}, {4359, 1, Source::Craft}, {4399, 1, Source::Vendor}}}},          // Rough Boomstick
    {3938, 4371, 105, 155, 0, {{{2841, 2, Source::Craft}, {2880, 1, Source::Vendor}, {}}}},       // Bronze Tube
};
inline constexpr std::uint8_t kEngGuns = 4;
// Tools a gear line recipe needs (Spell TotemCategory) and the vendor item that is one (lane tinkers2: the artisan buys it
// at the line vendor, once). A table recipe needing a category not listed turns the line off at load (EngGuns).
struct Tool
{
    std::uint32_t category = 0, item = 0;
};
inline constexpr Tool kTools[] = {{162, 5956}};  // Blacksmith Hammer
[[nodiscard]] inline constexpr std::uint32_t ToolFor(std::uint32_t category)
{
    for (Tool const& t : kTools)
        if (t.category == category)
            return t.item;
    return 0;
}
inline constexpr std::uint32_t kForgeFocus = 3;  // SpellFocusObject.dbc: Forge (smelting)
inline constexpr std::uint32_t kAnvilFocus = 1;  // SpellFocusObject.dbc: Anvil (engineering parts, lane tinkers2)
// Blacksmithing (MailGear, Smiths): no table yet. Its bars come from smelting (a Mining spell, not blacksmithing): a
// Smiths line needs a smelting intermediate the artisan can cast (it must also be a miner) or bars routed / bought, and
// the blacksmith trainers sit 390-565 yards from the homes (kStationYards 400). Add the rows here and its artisan config.

// Bags: Weavers / tailoring 197, the lane B/C runtime (TeamTick / TierTick over kTiers), unchanged.
// Potions (Brewers / alchemy 171), checked against the 3.3.5 world DB (item_template, trainer_spell of trainer
// 67 = Stormwind 5499 / Orgrimmar 3347) and Spell.dbc / SkillLineAbility.dbc; the runtime re-checks outputs and
// reagent counts against the loaded spells and disables the line on a mismatch:
//   Minor Healing Potion 118 (req 1): spell 2330, learned with the skill, grey 95; Peacebloom 2447 + Silverleaf
//     765 + Empty Vial 3371 (vendor).
//   Lesser Healing Potion 858 (req 3): spell 2337, alchemy 55, grey 125; Minor Healing Potion + Briarthorn 2450.
//   Healing Potion 929 (req 12): spell 3447, alchemy 110, grey 175; Bruiseweed 2453 + Briarthorn + Leaded Vial
//     3372 (vendor).
// PotionTiers rows (lane brewtiers, tierExtra; same checks; both vials sold by the line vendors Stormwind 1286 /
// Orgrimmar 5817; Mageroyal 785, Stranglekelp 3820, Kingsblood 3356, Liferoot 3357 are routed):
//   Minor Mana Potion 2455 (req 5): spell 2331, alchemy 25, grey 105; Mageroyal + Silverleaf + Empty Vial.
//   Lesser Mana Potion 3385 (req 14): spell 3173, alchemy 120, grey 185; Mageroyal + Stranglekelp + Empty Vial.
//   Greater Healing Potion 1710 (req 21): spell 7181, alchemy 155 (Expert: artisan level 20), grey 215; Liferoot +
//     Kingsblood + Leaded Vial.
//   Mana Potion 3827 (req 22): spell 3452, alchemy 160 (Expert), grey 220; Stranglekelp + Kingsblood + Leaded Vial.
//   Elixir of Wisdom 3383 (bridge): spell 3171, alchemy 90, grey 160 (orange to 120); Mageroyal + 2 Briarthorn +
//     Empty Vial. The only trainer recipe from 105 (Minor Mana grey) to Healing Potion's 110 without Peacebloom (soak
//     S75: 1 Peacebloom donated; Lesser Healing needs a Minor Healing Potion); no member wants it (DemandOnly: a
//     consumer-less last-resort skill-up, its output sold as `waste`).
//   Dropped: Superior Healing 3928 (alchemy 215) and Greater Mana 6149 (205) need Artisan alchemy (character level 35,
//     not taught by trainer 67; the artisans are level 12-13); Swiftness Potion (recipe item, not trainer-taught).
inline constexpr ProductLine kCatalog[] = {
    {Line::Bags, "bags", "Bags", "Weavers", "", 197, NeedRule::BagSlots, Consumer::EquipBag, {}, 0},
    // Alchemy ranks (trainer 67): Apprentice 2275 (level 5), Journeyman 2280 (skill 50, level 10), Expert 3465
    // (skill 125, level 20).
    {Line::Potions, "potions", "Potions", "Brewers", "2275,2280,3465", 171, NeedRule::PotionStock,
     Consumer::DrinkAtLowHp,
     {{{2330, 118, 1, 95, 1, {{{2447, 1, Source::Route}, {765, 1, Source::Route}, {3371, 1, Source::Vendor}}}},
       {2337, 858, 55, 125, 3, {{{118, 1, Source::Craft}, {2450, 1, Source::Route}, {}}}},
       {3447, 929, 110, 175, 12, {{{2453, 1, Source::Route}, {2450, 1, Source::Route}, {3372, 1, Source::Vendor}}}},
       // PotionTiers only (tierExtra):
       {2331, 2455, 25, 105, 5, {{{785, 1, Source::Route}, {765, 1, Source::Route}, {3371, 1, Source::Vendor}}},
        kFamilyMana},
       {3173, 3385, 120, 185, 14, {{{785, 1, Source::Route}, {3820, 1, Source::Route}, {3371, 1, Source::Vendor}}},
        kFamilyMana},
       {7181, 1710, 155, 215, 21, {{{3357, 1, Source::Route}, {3356, 1, Source::Route}, {3372, 1, Source::Vendor}}}},
       {3452, 3827, 160, 220, 22, {{{3820, 1, Source::Route}, {3356, 1, Source::Route}, {3372, 1, Source::Vendor}}},
        kFamilyMana},
       {3171, 3383, 90, 160, 10, {{{785, 1, Source::Route}, {2450, 2, Source::Route}, {3371, 1, Source::Vendor}}},
        kFamilyBridge}}},
     3, nullptr, 0, 0, 0, 5},
    // Gear lines (lane V): learn = the profession ranks (Apprentice .. Artisan; for the bag house the bag chain's own
    // Artisan.Learn already teaches them) plus every trainer-taught table recipe.
    {Line::ClothGear, "cloth_gear", "ClothGear", "Weavers", "3911,3912,3913,12181", 197, NeedRule::GearSlots,
     Consumer::EquipGear, {}, 0, kTailorGear, static_cast<std::uint8_t>(std::size(kTailorGear))},
    {Line::MailGear, "mail_gear", "MailGear", "Smiths", "", 164, NeedRule::GearSlots, Consumer::EquipGear, {}, 0,
     nullptr, 0},
    {Line::LeatherGear, "leather_gear", "LeatherGear", "Tanners", "2155,2154,3812,10663", 165, NeedRule::GearSlots,
     Consumer::EquipGear, {}, 0, kLeatherGear, static_cast<std::uint8_t>(std::size(kLeatherGear)), kLeatherStarters},
    // Engineering ranks (trainer 92): Apprentice 4039 (level 5), Journeyman 4040 (50, level 10), Expert 4041 (125, level
    // 20), Artisan 12657 (200, level 35); Mining ranks (trainer 80): 2581, 2582, 3568, 10249 (same gates).
    {Line::Engineering, "eng", "Engineering", "Tinkers", "4039,4040,4041,12657,2581,2582,3568,10249", 202,
     NeedRule::AmmoStock, Consumer::LoadAmmo, {}, 0, kEngGear, static_cast<std::uint8_t>(std::size(kEngGear)), 0,
     kEngGuns},
};
inline constexpr std::size_t kLineCount = std::size(kCatalog);
// PotionTiers rows fit the tier array.
static_assert([] {
    for (ProductLine const& l : kCatalog)
        if (l.tierCount + l.tierExtra > kMaxLineTiers)
            return false;
    return true;
}());
// GearTable drops either tail by its own flag: a line has starter rows or gun rows, never both.
static_assert([] {
    for (ProductLine const& l : kCatalog)
        if (l.gearStarters && l.gearGuns)
            return false;
    return true;
}());

[[nodiscard]] inline constexpr ProductLine const& LineOf(Line l) { return kCatalog[static_cast<std::size_t>(l)]; }

// ",\"line\":\"bags\"": appended to every `supply` row (append-only schema).
inline std::string LineField(Line l)
{
    std::string out = ",\"line\":\"";
    out += LineOf(l).name;
    out += "\"";
    return out;
}

// "bags,potions" -> bit i per kCatalog[i]. False (out untouched) on an unknown or empty name.
inline bool ParseProducts(std::string_view text, std::uint8_t& out)
{
    std::uint8_t mask = 0;
    bool ok = true;
    AutoWowGuilds::detail::Split(text, ',', [&](std::string_view token) {
        token = AutoWowGuilds::detail::Trim(token);
        std::size_t i = 0;
        while (i < kLineCount && token != kCatalog[i].name)
            ++i;
        if (i == kLineCount)
            ok = false;
        else
            mask |= static_cast<std::uint8_t>(1u << i);
    });
    if (!ok || !mask)
        return false;
    out = mask;
    return true;
}

// The tier whose product is `item`, else kNoTier.
template <typename L>
[[nodiscard]] std::uint8_t TierOf(L const& l, std::uint32_t item)
{
    for (std::uint8_t i = 0; i < l.tierCount; ++i)
        if (l.tiers[i].product == item)
            return i;
    return kNoTier;
}

// The distinct Route reagents of the line (routed by adventurers), in table order.
[[nodiscard]] inline std::vector<std::uint32_t> RouteItems(ProductLine const& l)
{
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < l.tierCount; ++i)
        for (Reagent const& r : l.tiers[i].reagents)
            if (r.item && r.source == Source::Route && std::find(out.begin(), out.end(), r.item) == out.end())
                out.push_back(r.item);
    return out;
}

// Casts of tier `i` the holdings make: every Route / Market / Craft reagent in hand (a Craft reagent also
// counts the casts its own tier makes from the holdings); Vendor reagents are bought, never a limit.
// have(item) -> units. Holdings shared by two levels are counted at both (EngGuns: Rough Boomstick's Copper Tube and
// Handful of Copper Bolts both count the Copper Bars: an upper bound; Lacks is exact).
template <typename L, typename Have>
[[nodiscard]] std::uint32_t Casts(L const& l, std::size_t i, Have&& have, std::size_t depth = kMaxLineTiers)
{
    std::uint64_t best = 0xFFFFFFFFu;
    if (i >= l.tierCount || !depth)
        return 0;
    for (Reagent const& r : l.tiers[i].reagents)
    {
        if (!r.item || !r.count || r.source == Source::Vendor)
            continue;
        std::uint64_t units = have(r.item);
        if (r.source == Source::Craft)
            if (std::uint8_t const sub = TierOf(l, r.item); sub != kNoTier && sub != i)
                units += Casts(l, sub, have, depth - 1);
        best = std::min<std::uint64_t>(best, units / r.count);
    }
    return static_cast<std::uint32_t>(best);
}

// What `n` casts of tier `i` still lack beyond the holdings, per non-Craft reagent (item, units, source); a
// short Craft reagent adds its own tier's reagents for the missing casts. Merged per item, table order.
struct Lack
{
    std::uint32_t item = 0;
    std::uint32_t units = 0;
    Source source = Source::Route;
};

template <typename L, typename Have>
void AddLacks(L const& l, std::size_t i, std::uint64_t n, Have&& have, std::vector<Lack>& out,
              std::size_t depth = kMaxLineTiers)
{
    if (i >= l.tierCount || !depth || !n)
        return;
    for (Reagent const& r : l.tiers[i].reagents)
    {
        if (!r.item || !r.count)
            continue;
        std::uint32_t const miss = Short(n * r.count, have(r.item));
        if (!miss)
            continue;
        if (r.source == Source::Craft)
        {
            if (std::uint8_t const sub = TierOf(l, r.item); sub != kNoTier && sub != i)
                AddLacks(l, sub, miss, have, out, depth - 1);
            continue;
        }
        auto const it = std::find_if(out.begin(), out.end(), [&](Lack const& x) { return x.item == r.item; });
        if (it == out.end())
            out.push_back({r.item, miss, r.source});
        else
            it->units = static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFu, std::uint64_t(it->units) + miss));
    }
}

template <typename L, typename Have>
[[nodiscard]] std::vector<Lack> Lacks(L const& l, std::size_t i, std::uint64_t n, Have&& have)
{
    std::vector<Lack> out;
    AddLacks(l, i, n, have, out);
    return out;
}

// The next cast toward tier `i`: `i` when every reagent is in hand, else the tier of a short Craft reagent whose
// own next cast is possible, else kNoTier. have(item) -> units in the artisan's bags.
template <typename L, typename Have>
[[nodiscard]] std::uint8_t NextCast(L const& l, std::size_t i, Have&& have, std::size_t depth = kMaxLineTiers)
{
    if (i >= l.tierCount || !depth)
        return kNoTier;
    bool ready = true;
    for (Reagent const& r : l.tiers[i].reagents)
        if (r.item && r.count && have(r.item) < r.count)
        {
            ready = false;
            if (r.source == Source::Craft)
                if (std::uint8_t const sub = TierOf(l, r.item); sub != kNoTier && sub != i)
                    if (std::uint8_t const c = NextCast(l, sub, have, depth - 1); c != kNoTier)
                        return c;
        }
    return ready ? static_cast<std::uint8_t>(i) : kNoTier;
}

// A routed reagent the artisan can use now: some tier needing it is learned with the profession (skill 1) or
// within the artisan's skill (0 = offline / unknown: skill-1 tiers only).
[[nodiscard]] inline bool UsableNow(ProductLine const& l, std::uint32_t item, std::uint32_t skill)
{
    for (std::size_t i = 0; i < l.tierCount; ++i)
        if (l.tiers[i].skill <= std::max<std::uint32_t>(1, skill))
            for (Reagent const& r : l.tiers[i].reagents)
                if (r.item == item)
                    return true;
    return false;
}

// Units of `item` the artisan keeps (not shipped to the rep) as a Craft reagent of `casts` casts of tier `i`.
[[nodiscard]] inline std::uint32_t CraftReserve(ProductLine const& l, std::uint8_t i, std::uint32_t casts,
                                                std::uint32_t item)
{
    if (i >= l.tierCount)
        return 0;
    for (Reagent const& r : l.tiers[i].reagents)
        if (r.item == item && r.source == Source::Craft)
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFu, std::uint64_t(casts) * r.count));
    return 0;
}

// ---- stock need (NeedRule::PotionStock) ----

// One online cohort member of the team: level and held units (bags + mailbox) of each tier's product.
struct StockMember
{
    std::uint32_t guid = 0;
    std::uint32_t level = 0;
    std::array<std::uint32_t, kMaxLineTiers> held{};
    bool mana = false;  // a mana user (max mana > 0): PotionTiers ranks it on the mana family too (RankStockTiers)
};

// The member's tier of `family`: the highest the house can make (known) usable at its level, else the highest usable
// at its level (nothing known yet); kNoTier = none usable.
[[nodiscard]] inline std::uint8_t BestTier(ProductLine const& l, std::uint32_t level,
                                           std::array<bool, kMaxLineTiers> const& known,
                                           std::uint8_t family = kFamilyHeal)
{
    std::uint8_t best = kNoTier, bestKnown = kNoTier;
    for (std::uint8_t i = 0; i < l.tierCount; ++i)
        if (l.tiers[i].reqLevel <= level && l.tiers[i].family == family)
        {
            best = i;
            if (known[i])
                bestKnown = i;
        }
    return bestKnown != kNoTier ? bestKnown : best;
}

struct StockNeed
{
    std::uint32_t guid = 0;
    std::uint8_t tier = kNoTier;
    std::uint32_t stock = 0;
    std::uint32_t want = 0;  // target - stock
};

// Members below `target` of their tier's product, lowest stock first, ties the lower guid.
[[nodiscard]] inline std::vector<StockNeed> RankStock(ProductLine const& l, std::vector<StockMember> const& members,
                                                      std::array<bool, kMaxLineTiers> const& known,
                                                      std::uint32_t target)
{
    std::vector<StockNeed> out;
    for (StockMember const& m : members)
    {
        std::uint8_t const tier = BestTier(l, m.level, known);
        if (tier == kNoTier || m.held[tier] >= target)
            continue;
        out.push_back({m.guid, tier, m.held[tier], target - m.held[tier]});
    }
    std::sort(out.begin(), out.end(), [](StockNeed const& a, StockNeed const& b)
              { return a.stock != b.stock ? a.stock < b.stock : a.guid < b.guid; });
    return out;
}

// PotionTiers: RankStock per family: every member on the heal family, a mana user also on the mana family (bridge
// rows: never). The stock of its tier counts the family's later (better) rows usable at its level too: a Greater
// Healing Potion in the bags covers a Healing Potion want. Lowest stock first, ties the lower guid, then the lower tier.
[[nodiscard]] inline std::vector<StockNeed> RankStockTiers(ProductLine const& l, std::vector<StockMember> const& members,
                                                           std::array<bool, kMaxLineTiers> const& known,
                                                           std::uint32_t target)
{
    std::vector<StockNeed> out;
    for (StockMember const& m : members)
        for (std::uint8_t const family : {kFamilyHeal, kFamilyMana})
        {
            if (family == kFamilyMana && !m.mana)
                continue;
            std::uint8_t const tier = BestTier(l, m.level, known, family);
            if (tier == kNoTier)
                continue;
            std::uint64_t stock = 0;
            for (std::size_t j = tier; j < l.tierCount; ++j)
                if (l.tiers[j].family == family && l.tiers[j].reqLevel <= m.level)
                    stock += m.held[j];
            if (stock < target)
                out.push_back({m.guid, tier, static_cast<std::uint32_t>(stock),
                               target - static_cast<std::uint32_t>(stock)});
        }
    std::sort(out.begin(), out.end(), [](StockNeed const& a, StockNeed const& b)
              { return std::tie(a.stock, a.guid, a.tier) < std::tie(b.stock, b.guid, b.tier); });
    return out;
}

// The want of the ranked members on tier `tier`.
[[nodiscard]] inline std::uint32_t TierWant(std::vector<StockNeed> const& ranked, std::uint8_t tier)
{
    std::uint32_t n = 0;
    for (StockNeed const& s : ranked)
        if (s.tier == tier)
            n += s.want;
    return n;
}

// PotionTiers: PickProduct over a line with families: a craftable tier before a merely known one (as PickProduct), and
// at each step the heal family first (the critical-health consumer), then any family; the later row within a step.
[[nodiscard]] inline std::uint8_t PickLineProduct(ProductLine const& l, std::vector<ProductOption> const& tiers)
{
    for (bool const craftable : {true, false})
        for (bool const heal : {true, false})
            for (std::size_t i = std::min<std::size_t>(tiers.size(), l.tierCount); i-- > 0;)
                if (tiers[i].known && tiers[i].want && (!craftable || tiers[i].craftable) &&
                    (!heal || l.tiers[i].family == kFamilyHeal))
                    return static_cast<std::uint8_t>(i);
    return kNoTier;
}

struct StackDelivery
{
    std::uint32_t guid = 0;
    std::vector<std::uint32_t> stacks;  // item guid lows
    std::uint32_t units = 0;
};

// The rep's stacks of tier `tier` (ascending guid) to the ranked members of that tier in order: whole stacks
// until each want is covered (the last stack may overshoot it), at most kMaxMailStacks per mail.
// ponytail: whole stacks, no split (a split is a second item + mail); overshoot <= one stack, the next want
// scan sees it as stock.
[[nodiscard]] inline std::vector<StackDelivery> PlanStackDeliveries(std::vector<StockNeed> const& ranked,
                                                                    std::uint8_t tier, std::vector<Stack> stacks)
{
    std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<StackDelivery> out;
    std::size_t next = 0;
    for (StockNeed const& s : ranked)
    {
        if (s.tier != tier)
            continue;
        if (next >= stacks.size())
            break;
        StackDelivery d{s.guid, {}, 0};
        for (; next < stacks.size() && d.units < s.want && d.stacks.size() < kMaxMailStacks; ++next)
        {
            d.stacks.push_back(stacks[next].guid);
            d.units += stacks[next].count;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// ---- routing of a line's reagents (adventurer sell stop) ----

// Per routed item (stacks[i] of items[i]): whole stacks within its own room, at most kMaxMailStacks in all.
struct RoutePlan
{
    std::vector<std::uint32_t> picks;  // item guid lows
    std::vector<std::uint32_t> units;  // per item
};

[[nodiscard]] inline RoutePlan PlanRoute(std::vector<std::vector<Stack>> const& stacks,
                                         std::vector<std::uint32_t> const& rooms)
{
    RoutePlan out;
    out.units.assign(stacks.size(), 0);
    for (std::size_t i = 0; i < stacks.size() && i < rooms.size(); ++i)
        for (std::uint32_t const g : PickStacks(stacks[i], rooms[i]))
        {
            if (out.picks.size() >= kMaxMailStacks)
                break;
            out.picks.push_back(g);
            for (Stack const& s : stacks[i])
                if (s.guid == g)
                    out.units[i] += s.count;
        }
    return out;
}

// ---- outfitting grants (AutoWow.Supply.Outfit) ----

// A member's pending grant request: it wants `need` copper in hand for its gathering tools / planned trainer
// ranks (the errand arrival sums them). The overlord pays need - money at the next tick.
struct GrantRequest
{
    std::uint32_t guid = 0;
    std::uint32_t level = 0;
    std::uint64_t need = 0;
};

// Lowest level first, ties the lower guid.
[[nodiscard]] inline std::vector<GrantRequest> RankGrants(std::vector<GrantRequest> reqs)
{
    std::sort(reqs.begin(), reqs.end(), [](GrantRequest const& a, GrantRequest const& b)
              { return a.level != b.level ? a.level < b.level : a.guid < b.guid; });
    return reqs;
}

// Copper granted to a bot at `level` (a level-up opens a fresh window).
struct GrantWindow
{
    std::uint32_t level = 0;
    std::uint64_t granted = 0;
};

// Copper granted to a team in game hour `hour` (game time ms / kGrantHourMs).
struct GrantBudget
{
    std::uint64_t hour = 0;
    std::uint64_t spent = 0;
};

inline constexpr std::uint64_t kGrantHourMs = 3600000;
inline constexpr std::uint64_t kGrantBufferCopper = 50;  // on top of a trainer rank's cost (reputation rounding)

// Wire-stable (the refused row's op); append only.
enum class GrantVerdict : std::uint8_t
{
    Pay = 0,
    Covered = 1,     // the bot's own money covers the need now: nothing paid
    BotCap = 2,      // OutfitMaxCopper for this level would be exceeded
    TeamBudget = 3   // OutfitBudgetPerHour of the team would be exceeded
};

inline constexpr char const* GrantVerdictName(GrantVerdict v)
{
    switch (v)
    {
        case GrantVerdict::Pay: return "grant";
        case GrantVerdict::Covered: return "grant_covered";
        case GrantVerdict::BotCap: return "grant_bot_cap";
        case GrantVerdict::TeamBudget: return "grant_team_budget";
    }
    return "grant";
}

struct GrantDecision
{
    GrantVerdict verdict = GrantVerdict::Covered;
    std::uint64_t copper = 0;  // the shortfall (paid on Pay, refused otherwise)
};

// The shortfall (need - money) is granted whole or not at all (a part would not buy the tool or rank).
[[nodiscard]] inline GrantDecision DecideGrant(GrantRequest const& r, std::uint64_t money, GrantWindow const& w,
                                               GrantBudget const& b, std::uint64_t hour, std::uint64_t maxCopper,
                                               std::uint64_t budgetPerHour)
{
    GrantDecision d;
    d.copper = r.need > money ? r.need - money : 0;
    if (!d.copper)
        return d;
    std::uint64_t const granted = w.level == r.level ? w.granted : 0;
    std::uint64_t const spent = b.hour == hour ? b.spent : 0;
    if (granted + d.copper > maxCopper)
        d.verdict = GrantVerdict::BotCap;
    else if (spent + d.copper > budgetPerHour)
        d.verdict = GrantVerdict::TeamBudget;
    else
        d.verdict = GrantVerdict::Pay;
    return d;
}

// AutoWow.Supply.OutfitGear: a bot's grant cap per level grows by gearCopper x level^2 (vendor weapons, world DB
// 2026-09-28, cheapest by RequiredLevel: L20 58s, L30 1g57s, L40 3g57s; 25 x level^2 = L20 1g, L30 2g25s, L40 4g)
// so a floor weapon plus a stack of food and drink fits one level's grants. Off: maxCopper.
[[nodiscard]] inline std::uint64_t GrantCapCopper(std::uint64_t maxCopper, bool gear, std::uint32_t gearCopper,
                                                  std::uint32_t level)
{
    return maxCopper + (gear ? std::uint64_t(gearCopper) * level * level : 0);
}

// Books a paid grant into the bot's level window and the team's hour.
inline void NoteGrant(GrantWindow& w, GrantBudget& b, std::uint32_t level, std::uint64_t hour, std::uint64_t copper)
{
    if (w.level != level)
        w = {level, 0};
    if (b.hour != hour)
        b = {hour, 0};
    w.granted += copper;
    b.spent += copper;
}

// ---- config parsing ----

// "map,x,y,z" (integers, yards). False (out untouched) on anything else.
struct Home
{
    std::uint32_t map = 0;
    std::int32_t x = 0, y = 0, z = 0;
    bool set = false;
};

inline bool ParseInt(std::string_view s, std::int32_t& out)
{
    bool const neg = !s.empty() && s.front() == '-';
    if (neg)
        s.remove_prefix(1);
    std::uint32_t v = 0;
    if (!AutoWowGuilds::detail::ParseU32(s, v) || v > 0x7FFFFFFFu)
        return false;
    out = neg ? -static_cast<std::int32_t>(v) : static_cast<std::int32_t>(v);
    return true;
}

inline bool ParseHome(std::string_view text, Home& out)
{
    std::vector<std::int32_t> v;
    bool ok = true;
    AutoWowGuilds::detail::Split(text, ',', [&](std::string_view token) {
        std::int32_t x = 0;
        ok = ok && ParseInt(token, x);
        v.push_back(x);
    });
    if (!ok || v.size() != 4 || v[0] < 0)
        return false;
    out = {static_cast<std::uint32_t>(v[0]), v[1], v[2], v[3], true};
    return true;
}

// ---- ledger `supply` (event 20) ----

// Wire-stable; append only.
enum class Reason : std::uint8_t
{
    Order = 0,    // overlord posted a craft order (count = bags)
    Donate = 1,   // adventurer -> rep cloth mail
    Feed = 2,     // rep -> artisan cloth mail, or treasury -> artisan copper (thread / trainer)
    Craft = 3,    // artisan finished a cast (item = product, count = units made)
    Deliver = 4,  // artisan -> rep or rep -> member bag mail
    Pay = 5,      // treasury -> artisan per delivered bag
    Surplus = 6,  // rep listed / sold spare bags
    Xp = 7,       // work XP (artisan per item, rep per deal); copper = XP amount
    Refused = 8,  // a movement not made; op names it
    Travel = 9,   // role bot relocated home by portal fallback (op = other_map | stuck)
    List = 10,    // Market: rep listed a house surplus stack on the faction AH (copper = buyout)
    Buy = 11,     // Market: rep bought out a listing with treasury copper (copper = price)
    Sold = 12,    // Market: a rep's auction sold; its proceeds went to the house bank (copper = proceeds)
    Outfit = 13,  // Outfit: a member bought a gathering tool at a vendor with its own gold (copper = price)
    Junk = 14,    // artisan make-room: op vendor (count = stacks sold, copper = proceeds) | destroy (item, count)
    Cancel = 15,  // Market: rep took back its own listing of a wanted item (copper = its buyout; item by mail)
    Waste = 16,   // DemandOnly: a surplus sale (house output nobody wanted), in place of `surplus`
    WeaponFloor = 17,  // OutfitGear: a member bought its floor weapon at a vendor (copper = price; line outfit)
    FoodFloor = 18,    // OutfitGear: a member bought food / drink at an errand vendor (copper = price; line outfit)
    Grant = 19         // OutfitGear: the treasury paid a member's outfit grant (item 0, from 0, copper; line outfit)
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Order: return "order";
        case Reason::Donate: return "donate";
        case Reason::Feed: return "feed";
        case Reason::Craft: return "craft";
        case Reason::Deliver: return "deliver";
        case Reason::Pay: return "pay";
        case Reason::Surplus: return "surplus";
        case Reason::Xp: return "xp";
        case Reason::Refused: return "refused";
        case Reason::Travel: return "travel";
        case Reason::List: return "list";
        case Reason::Buy: return "buy";
        case Reason::Sold: return "sold";
        case Reason::Outfit: return "outfit";
        case Reason::Junk: return "junk";
        case Reason::Cancel: return "cancel";
        case Reason::Waste: return "waste";
        case Reason::WeaponFloor: return "weapon_floor";
        case Reason::FoodFloor: return "food_floor";
        case Reason::Grant: return "grant";
    }
    return "refused";
}

// Trailing fields: house, oid (team order id, run-scoped, never reused; 0 = none), item, count, copper, from,
// to (guid-lows; 0 = the treasury / no player), and op when given. team is the row's own field.
inline std::string LedgerFields(std::string_view house, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
                                std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op = nullptr)
{
    std::string out = ",\"house\":\"";
    out += house;
    out += "\",\"oid\":" + std::to_string(oid) + ",\"item\":" + std::to_string(item) +
           ",\"count\":" + std::to_string(count) + ",\"copper\":" + std::to_string(copper) +
           ",\"from\":" + std::to_string(from) + ",\"to\":" + std::to_string(to);
    if (op)
    {
        out += ",\"op\":\"";
        out += op;
        out += "\"";
    }
    return out;
}

// ---- throughput (lane U, docs/SUPPLY_CHAIN_PLAN.md; soak-s49-full-r1: 187 crafts in 2 h once fed, a handful of bags
// delivered) ----

// DirectRoutes: donors mail a house material straight to the artisan (one hop instead of donor -> rep -> artisan)
// while it works at home with more free bag slots than its make-room target and mailbox room; else the rep.
[[nodiscard]] inline bool ArtisanTakes(bool working, bool atHome, std::uint32_t freeSlots, std::uint32_t roomTarget,
                                       bool mailRoom)
{
    return working && atHome && freeSlots > roomTarget && mailRoom;
}

// MailOrders: a rep's standing buy order, filled by random bots' cash-on-delivery mail (the item at the order price).
struct MailOrder
{
    std::uint32_t rep = 0;        // guid-low of the house rep the goods go to
    std::uint32_t item = 0;
    std::uint32_t units = 0;      // still open (sellers reserve units as they mail)
    std::uint32_t unitPrice = 0;  // copper per unit = vendor sell value * BuyMaxPct / 100
};

// A house's orders from its market wants (ascending item): each priced at BuyMaxPct of the vendor value, and only as
// many units as the house bank's `budget` covers (earlier items first). Items with no vendor value never order.
[[nodiscard]] inline std::vector<MailOrder> PlanOrders(std::uint32_t rep, std::vector<MarketWant> wants,
                                                       std::uint32_t buyMaxPct, std::uint64_t budget)
{
    std::sort(wants.begin(), wants.end(), [](MarketWant const& a, MarketWant const& b) { return a.item < b.item; });
    std::vector<MailOrder> out;
    for (MarketWant const& w : wants)
    {
        std::uint64_t const unit = std::uint64_t(w.sellPrice) * buyMaxPct / 100;
        if (!unit || !w.units || unit > 0xFFFFFFFFull)
            continue;
        std::uint32_t const units = static_cast<std::uint32_t>(std::min<std::uint64_t>(w.units, budget / unit));
        if (!units)
            continue;
        budget -= units * unit;
        out.push_back({rep, w.item, units, static_cast<std::uint32_t>(unit)});
    }
    return out;
}

// Whole stacks (ascending guid) whose total stays within `units` (never over: an order is not overfilled), at most
// kMaxMailStacks. ponytail: no split, a stack larger than the rest of the order stays with the seller.
[[nodiscard]] inline std::vector<std::uint32_t> PickWithin(std::vector<Stack> stacks, std::uint32_t units)
{
    std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    for (Stack const& s : stacks)
    {
        if (out.size() >= kMaxMailStacks)
            break;
        if (!s.count || s.count > units)
            continue;
        out.push_back(s.guid);
        units -= s.count;
    }
    return out;
}

// The COD a rep expects from one seller for one item (units mailed, copper asked): recorded when the seller's mail
// goes, taken when the rep accepts it.
struct CodPending
{
    std::uint32_t units = 0;
    std::uint64_t cod = 0;
};

enum class CodVerdict : std::uint8_t
{
    Accept = 0,
    Return = 1,  // not an order fill of this seller / over its price / over the budget: back to the seller
    Wait = 2     // no bag room: the mail stays for the next visit
};

// A COD mail at the rep: it must match what the seller mailed for the order (units and COD within the pending
// fill), the house must afford it (bank + the rep's purse) and the rep must have room.
[[nodiscard]] inline CodVerdict DecideCod(CodPending const& pending, std::uint32_t units, std::uint64_t cod,
                                          std::uint64_t funds, bool room)
{
    if (!units || units > pending.units || cod > pending.cod || cod > funds)
        return CodVerdict::Return;
    return room ? CodVerdict::Accept : CodVerdict::Wait;
}

// ---- need-driven production (lane V, docs/SUPPLY_CHAIN_PLAN.md; owner 2026-09-27: artisans make only useful things,
// houses supply gear) ----

// DemandOnly: a sale of house output nobody wanted is `waste`, else `surplus`.
[[nodiscard]] inline Reason SaleReason(bool demandOnly) { return demandOnly ? Reason::Waste : Reason::Surplus; }

// ",\"consumer\":guid" (DemandOnly order / deliver rows, every gear line row; 0 = none named yet: rep stock).
inline std::string ConsumerField(std::uint32_t guid) { return ",\"consumer\":" + std::to_string(guid); }

// "1,2, 3" -> guids in config order (AutoWow.Supply.PriorityGuids). Empty text = empty list. False (out untouched) on a
// bad token.
inline bool ParseGuids(std::string_view text, std::vector<std::uint32_t>& out)
{
    std::vector<std::uint32_t> v;
    bool ok = true;
    if (!AutoWowGuilds::detail::Trim(text).empty())
        AutoWowGuilds::detail::Split(text, ',', [&](std::string_view token) {
            std::uint32_t g = 0;
            ok = ok && AutoWowGuilds::detail::ParseU32(AutoWowGuilds::detail::Trim(token), g) && g;
            v.push_back(g);
        });
    if (!ok)
        return false;
    out = std::move(v);
    return true;
}

inline constexpr std::uint32_t kNoPriority = 0xFFFFFFFFu;

// The recipient's place in AutoWow.Supply.PriorityGuids (the overlord / guild masters' list), else kNoPriority.
[[nodiscard]] inline std::uint32_t PriorityOf(std::vector<std::uint32_t> const& list, std::uint32_t guid)
{
    auto const it = std::find(list.begin(), list.end(), guid);
    return it == list.end() ? kNoPriority : static_cast<std::uint32_t>(it - list.begin());
}

// A gear line's equipment recipes, best first: highest product item level, ties the lower spell. ilvl[i] = 0 = not
// equipment (an intermediate): left out.
[[nodiscard]] inline std::vector<std::uint8_t> RankGearRecipes(RecipeTable const& g, std::vector<std::uint32_t> const& ilvl)
{
    std::vector<std::uint8_t> out;
    for (std::uint8_t i = 0; i < g.tierCount && i < ilvl.size(); ++i)
        if (ilvl[i])
            out.push_back(i);
    std::sort(out.begin(), out.end(), [&](std::uint8_t a, std::uint8_t b)
              { return ilvl[a] != ilvl[b] ? ilvl[a] > ilvl[b] : g.tiers[a].spell < g.tiers[b].spell; });
    return out;
}

// One equipment slot of a recipient a known recipe's product upgrades (the stock "item upgrade" value: EQUIP into an
// empty slot or REPLACE the worn piece, class / spec weights, armor type, level). ilvl = the recipient's summed
// equipped item level (the worst geared rank first).
struct GearNeed
{
    std::uint32_t guid = 0;
    std::uint8_t recipe = kNoTier;  // gear table index
    std::uint8_t slot = 0;          // EQUIPMENT_SLOT_*
    std::uint32_t priority = kNoPriority;
    std::uint32_t ilvl = 0;
};

// Priority list first (its order), then the worst geared (lowest summed item level), ties the lower guid, then slot.
[[nodiscard]] inline std::vector<GearNeed> RankGearNeeds(std::vector<GearNeed> needs)
{
    std::sort(needs.begin(), needs.end(), [](GearNeed const& a, GearNeed const& b)
              { return std::tie(a.priority, a.ilvl, a.guid, a.slot, a.recipe) <
                       std::tie(b.priority, b.ilvl, b.guid, b.slot, b.recipe); });
    return needs;
}

// A gear order entry: units of one recipe still to make and ship; consumer = its first ranked need.
struct GearOrder
{
    std::uint8_t recipe = kNoTier;
    std::uint32_t units = 0;
    std::uint32_t consumer = 0;
};

// The order for the top `maxNeeds` ranked needs: per recipe (first-ranked order) its need count plus `stock` for the
// rep (RepStockPerItem), minus the finished units the house holds (held[recipe]). Never more than demand + stock.
[[nodiscard]] inline std::vector<GearOrder> PlanGearOrders(std::vector<GearNeed> const& ranked,
                                                           std::vector<std::uint32_t> const& held,
                                                           std::uint32_t maxNeeds, std::uint32_t stock)
{
    std::vector<GearOrder> out;
    for (std::size_t k = 0; k < ranked.size() && k < maxNeeds; ++k)
    {
        auto const it = std::find_if(out.begin(), out.end(), [&](GearOrder const& o) { return o.recipe == ranked[k].recipe; });
        if (it == out.end())
            out.push_back({ranked[k].recipe, 1, ranked[k].guid});
        else
            ++it->units;
    }
    for (GearOrder& o : out)
    {
        std::uint64_t const target = std::uint64_t(o.units) + stock;
        std::uint32_t const have = o.recipe < held.size() ? held[o.recipe] : 0;
        o.units = target > have ? static_cast<std::uint32_t>(std::min<std::uint64_t>(target - have, 0xFFFFFFFFu)) : 0;
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](GearOrder const& o) { return !o.units; }), out.end());
    return out;
}

// cloth_gear on the bag house: raw cloth per kTiers tier the open orders' bolts still lack beyond the house holdings
// (have(item) -> units; a held bolt counts as its cloth). Feeds the squad demand (ClothDemandOf) and the rep's market
// wants when the bag line has no use for that tier.
template <typename Have>
[[nodiscard]] std::array<std::uint32_t, kTierCount> GearClothShort(RecipeTable const& g,
                                                                   std::vector<GearOrder> const& orders, Have&& have)
{
    std::array<std::uint64_t, kTierCount> bolts{};
    for (GearOrder const& o : orders)
        if (o.recipe < g.tierCount)
            for (Reagent const& r : g.tiers[o.recipe].reagents)
                for (std::size_t i = 0; i < kTierCount; ++i)
                    if (r.item && r.item == kTiers[i].bolt)
                        bolts[i] += std::uint64_t(o.units) * r.count;
    std::array<std::uint32_t, kTierCount> out{};
    for (std::size_t i = 0; i < kTierCount; ++i)
    {
        std::uint64_t const per = kTiers[i].recipe.clothPerBolt;
        out[i] = Short(bolts[i] * per, std::uint64_t(have(kTiers[i].bolt)) * per + have(kTiers[i].cloth));
    }
    return out;
}

// The order entry the artisan works now: the first still short (toMake > 0) the house can cast now (casts > 0), else
// the first short one (its materials are on the way). -1 = none.
[[nodiscard]] inline int PickGearTarget(std::vector<std::uint32_t> const& toMake, std::vector<std::uint32_t> const& casts)
{
    int first = -1;
    for (std::size_t i = 0; i < toMake.size(); ++i)
    {
        if (!toMake[i])
            continue;
        if (i < casts.size() && casts[i])
            return static_cast<int>(i);
        if (first < 0)
            first = static_cast<int>(i);
    }
    return first;
}

// GearBootstrap: the highest profession skill a gear artisan can reach before its next trainer rank is out of reach:
// its rank's cap (maxSkill), or the next rank's when its level trains it (3.3.5 trainer_spell: Apprentice 75 at level
// 5, Journeyman 150 at 10, Expert 225 at 20, Artisan 300 at 35, Master 375 at 50, Grand Master 450 at 65).
[[nodiscard]] inline std::uint32_t ReachSkill(std::uint32_t maxSkill, std::uint32_t level)
{
    constexpr std::uint32_t kRankCap[] = {75, 150, 225, 300, 375, 450}, kRankLevel[] = {5, 10, 20, 35, 50, 65};
    for (std::size_t i = 0; i < std::size(kRankCap); ++i)
        if (kRankCap[i] > maxSkill)
            return level >= kRankLevel[i] ? kRankCap[i] : maxSkill;
    return maxSkill;
}

// GearBootstrap: `blocked` = the ranked needs of recipes the artisan can reach but does not know (no known recipe
// serves any need). The first one whose recipe is above `skill` (the others it learns at the trainer now) gets a
// skill-up order: `casts` of the skill-up recipe (PickSkillup over `options`, no stock needed: the order's reagents are
// fed), its consumer that need's recipient (DemandOnly). Empty when nothing is skill-blocked or no recipe levels it.
[[nodiscard]] inline std::vector<GearOrder> PlanGearSkillup(RecipeTable const& g, std::vector<GearNeed> const& blocked,
                                                            std::uint32_t skill,
                                                            std::vector<SkillupOption> const& options,
                                                            std::uint32_t casts)
{
    auto const top = std::find_if(blocked.begin(), blocked.end(), [&](GearNeed const& n)
                                  { return n.recipe < g.tierCount && g.tiers[n.recipe].skill > skill; });
    int const pick = top == blocked.end() || !casts ? -1 : PickSkillup(skill, options, false);
    if (pick < 0)
        return {};
    return {{options[static_cast<std::size_t>(pick)].tier, casts, top->guid}};
}

// GearSkillupRestock (lane craftflow; soak S75: both Tinkers artisans sat at engineering 46 / 49 for 2.3 h, below
// Rough Boomstick's 50, `skillup=0` in every bootstrap scan: each known recipe below grey was stocked, 10 Handful of
// Copper Bolts at each artisan and 20 casts of Crafted Light Shot at each rep, and nothing consumes skill-up stock).
// PlanGearSkillup over `options` built without the stock gate; the order is the artisan's finished units of that recipe
// (`mine`, which GearTick counts against the order) + `casts`, so it still casts `casts` more. Grey ends it.
[[nodiscard]] inline std::vector<GearOrder> PlanGearSkillupRestock(RecipeTable const& g,
                                                                   std::vector<GearNeed> const& blocked,
                                                                   std::uint32_t skill,
                                                                   std::vector<SkillupOption> const& options,
                                                                   std::vector<std::uint32_t> const& mine,
                                                                   std::uint32_t casts)
{
    std::vector<GearOrder> out = PlanGearSkillup(g, blocked, skill, options, casts);
    for (GearOrder& o : out)
        o.units += o.recipe < mine.size() ? mine[o.recipe] : 0;
    return out;
}

// GearStockSell (lane craftflow; soak S75: the Horde Tanners rep's 40 bag slots held 28 leather pieces nobody wanted,
// GearBootstrap skill-up output and RepStockPerItem stock, so it could not take 37 mails holding 95 Light Leather and
// its artisan, 25 Fine Leather Belt orders open, was never fed). A gear-line rep's finished pieces (rows with a
// RequiredLevel) beyond `keep` per recipe go to the vendor: the lowest guids stay, the rest in ascending guid, sellable
// ones only (shot sells for 0: never planned, no trip loop).
struct GearPiece
{
    std::uint8_t recipe = kNoTier;
    std::uint32_t guid = 0;       // item guid-low
    std::uint32_t sellPrice = 0;  // vendor sell value per unit
};

[[nodiscard]] inline std::vector<std::uint32_t> PlanGearStockSale(std::vector<GearPiece> pieces, std::uint32_t keep)
{
    std::sort(pieces.begin(), pieces.end(), [](GearPiece const& a, GearPiece const& b) { return a.guid < b.guid; });
    std::vector<std::pair<std::uint8_t, std::uint32_t>> kept;  // recipe -> pieces kept (bounded by the table)
    std::vector<std::uint32_t> out;
    for (GearPiece const& p : pieces)
    {
        auto it = std::find_if(kept.begin(), kept.end(), [&](auto const& k) { return k.first == p.recipe; });
        if (it == kept.end())
            it = kept.insert(kept.end(), {p.recipe, 0});
        if (it->second < keep)
            ++it->second;
        else if (p.sellPrice)
            out.push_back(p.guid);
    }
    return out;
}

// A finished piece a sender holds: its recipe and item guid-low.
struct GearItem
{
    std::uint8_t recipe = kNoTier;
    std::uint32_t guid = 0;
};

struct GearDelivery
{
    std::size_t need = 0;   // index into the ranked needs
    std::uint32_t item = 0; // item guid-low
};

// The sender's pieces (ascending guid) to the ranked needs in order: one piece per need, of its recipe.
[[nodiscard]] inline std::vector<GearDelivery> PlanGearDeliveries(std::vector<GearNeed> const& ranked,
                                                                  std::vector<GearItem> items)
{
    std::sort(items.begin(), items.end(), [](GearItem const& a, GearItem const& b) { return a.guid < b.guid; });
    std::vector<bool> used(items.size(), false);
    std::vector<GearDelivery> out;
    for (std::size_t k = 0; k < ranked.size(); ++k)
        for (std::size_t j = 0; j < items.size(); ++j)
            if (!used[j] && items[j].recipe == ranked[k].recipe)
            {
                used[j] = true;
                out.push_back({k, items[j].guid});
                break;
            }
    return out;
}

// ---- ammo (lane AA Tinkers, NeedRule::AmmoStock; a gear line whose products are shot) ----

inline constexpr std::uint8_t kAmmoSlot = 19;  // GearNeed.slot of an ammo need (EQUIPMENT_SLOT_END: no equipment slot)

// A recipient wants this shot: a hunter with a gun, shot (bullets) usable at its level, not weaker than the ammo it has
// loaded (damage = DamageMin + DamageMax; 0 = none loaded) and fewer than `target` bullets held (bags + mailbox).
[[nodiscard]] inline bool WantsAmmo(bool hunter, bool gun, bool bullets, std::uint32_t level, std::uint32_t reqLevel,
                                    std::uint32_t damage, std::uint32_t loadedDamage, std::uint32_t held,
                                    std::uint32_t target)
{
    return hunter && gun && bullets && reqLevel <= level && damage >= loadedDamage && held < target;
}

// The house shot a gun hunter loads (Player::SetAmmo): stronger than its loaded ammo, or that ammo is gone.
[[nodiscard]] inline bool LoadsAmmo(std::uint32_t damage, std::uint32_t loadedDamage, std::uint32_t loadedHeld)
{
    return !loadedHeld || damage > loadedDamage;
}

// Finished units of a recipe in order units (casts): a shot cast makes `yield` (200), a gear piece 1.
[[nodiscard]] inline std::uint32_t CastUnits(std::uint32_t items, std::uint32_t yield)
{
    return yield ? items / yield : items;
}

// ---- runtime (AutoWowSupply.cpp). ----
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }

enum class Role : std::uint8_t
{
    None = 0,
    Rep = 1,
    Artisan = 2
};

// A role bot's static facts (read-only after LoadConfig).
struct RoleInfo
{
    Role role = Role::None;
    bool alliance = false;
    bool bagHouse = false;  // its house makes the V1 product (only that artisan crafts)
    Home home;
    std::uint8_t line = kNoLine;  // the enabled catalog line (tierCount > 0) its house makes, else kNoLine
    std::uint8_t gear = kNoLine;  // the enabled gear line (Products cloth_gear / leather_gear / eng) its house makes
};

// The line (hence the house label) of a role bot's own rows (travel, junk): the bag line for the bag house, else
// its catalog line, else its gear line, else bags. Soak S56: the Tinkers artisan (gear line eng only) logged
// house=Weavers, the bag house, while in the Tinkers guild.
[[nodiscard]] inline Line OwnLine(RoleInfo const& r)
{
    if (r.bagHouse)
        return Line::Bags;
    if (r.line != kNoLine)
        return static_cast<Line>(r.line);
    return r.gear != kNoLine ? static_cast<Line>(r.gear) : Line::Bags;
}

// ---- artisan upkeep (lane F) ----

// Apprentice phase (ArtisanMinLevel, 0 = off): a configured artisan below the level is no role at all (it quests,
// runs errands, joins parties like any cohort adventurer; soak-s45-full-r1: the level 1-2 Brewers artisans could
// never learn Apprentice Alchemy, which needs level 5); at the level it goes home, but never out of a dungeon run.
// Reps are never gated.
[[nodiscard]] inline Role GatedRole(Role r, std::uint32_t level, std::uint32_t minLevel, bool inInstance)
{
    return r == Role::Artisan && minLevel && (level < minLevel || inInstance) ? Role::None : r;
}

// A material or product of a catalog line (any tier's product or reagent).
template <typename L>
[[nodiscard]] bool LineItem(L const& l, std::uint32_t item)
{
    for (std::size_t i = 0; i < l.tierCount; ++i)
    {
        if (l.tiers[i].product == item)
            return true;
        for (Reagent const& r : l.tiers[i].reagents)
            if (r.item == item)
                return true;
    }
    return false;
}

// One loose stack in an artisan's bags, as the make-room step sees it.
struct BagStack
{
    std::uint32_t guid = 0;       // item guid low
    std::uint32_t sellPrice = 0;  // vendor sell value per unit
    bool house = false;           // a material / product of its house's line
    bool quest = false;           // needed by a quest in its log
    bool keep = false;            // hearthstone, profession tool, container, not user-destroyable
};

// BagStack.quest for one quest in the log (incomplete or complete, not yet rewarded): `item` is its source item, an
// item objective (collected ones too: they are turned in) or a source drop. Unused (0) slots never match. Not
// Player::HasQuestForItem(item, 0, true): its turn-in branch answers true for ANY item once some objective slot of a
// logged quest is met, an unused one (0 >= 0) included (soak-s47-full-r1: every stack of the Horde tailor read as
// quest, make-room found nothing to free and it never crafted again).
template <std::size_t R, std::size_t D>
[[nodiscard]] bool QuestWantsItem(std::uint32_t item, std::uint32_t src, std::uint32_t const (&required)[R],
                                  std::uint32_t const (&drops)[D])
{
    if (!item)
        return false;
    return item == src || std::find(std::begin(required), std::end(required), item) != std::end(required) ||
           std::find(std::begin(drops), std::end(drops), item) != std::end(drops);
}

enum class Junk : std::uint8_t
{
    Keep = 0,
    Sell = 1,
    Destroy = 2  // unsellable (SellPrice 0)
};

[[nodiscard]] inline Junk ClassifyJunk(BagStack const& s)
{
    if (s.house || s.quest || s.keep)
        return Junk::Keep;
    return s.sellPrice ? Junk::Sell : Junk::Destroy;
}

// Free slots the make-room step works toward: ArtisanFreeSlots, at least one while a craft has no room.
[[nodiscard]] inline std::uint32_t RoomTarget(std::uint32_t freeSlots, bool craftBlocked)
{
    return std::max<std::uint32_t>(freeSlots, craftBlocked ? 1 : 0);
}

struct RoomPlan
{
    std::vector<std::uint32_t> sell;     // every sellable junk stack (the next vendor trip)
    std::vector<std::uint32_t> destroy;  // unsellable junk, only for the slots the sales cannot free
};

// Nothing while free >= want. Ascending guid.
[[nodiscard]] inline RoomPlan PlanRoom(std::vector<BagStack> stacks, std::uint32_t free, std::uint32_t want)
{
    RoomPlan out;
    if (free >= want)
        return out;
    std::sort(stacks.begin(), stacks.end(), [](BagStack const& a, BagStack const& b) { return a.guid < b.guid; });
    for (BagStack const& s : stacks)
        if (ClassifyJunk(s) == Junk::Sell)
            out.sell.push_back(s.guid);
    std::uint64_t room = std::uint64_t(free) + out.sell.size();
    for (BagStack const& s : stacks)
        if (room < want && ClassifyJunk(s) == Junk::Destroy)
        {
            out.destroy.push_back(s.guid);
            ++room;
        }
    return out;
}

// A bag to buy: none worn and still short of `want` free slots once the junk went (no sale left to make).
[[nodiscard]] inline bool WantsBag(bool bagWorn, std::uint32_t free, std::uint32_t want)
{
    return !bagWorn && free < want;
}

// Station near a team's home (read-only after LoadConfig): creature entry (or mailbox go entry) + position.
struct Station
{
    std::uint32_t entry = 0;  // 0 = none found
    std::uint32_t map = 0;
    std::int32_t x = 0, y = 0, z = 0;
};

struct Stations
{
    Station mailbox, trainer, threadVendor, auctioneer;
    Station banker;  // RepStore: the rep's bank stash (Stormwind 2455 Olivia Burnside, Orgrimmar 3318 Koma)
    Station forge;     // a forge (spell focus 3) near home: gear lines' smelting (lane AA)
    Station anvil;     // an anvil (spell focus 1) near home: EngGuns parts (lane tinkers2; none when off)
    Station trainer2;  // gear lines: the trainer of the learn spells `trainer` does not teach (Engineering: mining)
};

// Shared per-team state (world thread writes, role bots' map threads read), copied out under the lock.
struct TeamView
{
    std::uint32_t version = kStateVersion;
    std::uint32_t orderId = 0;    // current order (0 = none yet)
    std::uint32_t remaining = 0;  // bags the order still wants delivered to the rep
    std::uint32_t surplus = 0;    // bags the rep should list / sell now (Tiers: the sum of surplusBags)
    // Tiers (all kNoTier / 0 when off):
    std::uint8_t product = kNoTier;  // kTiers index of the ordered bag
    std::uint8_t skillup = kNoTier;  // kTiers index of the skill-up recipe
    bool skillupBag = false;         // the skill-up recipe is that tier's bag (else its bolt)
    std::uint32_t artisanSkill = 0;  // the artisan's tailoring (0 = offline / unknown)
    std::array<std::uint32_t, kTierCount> surplusBags{};
    std::vector<MarketWant> buy;     // Market: what the rep buys on the faction AH (at most kTierCount + 1)
};

// A catalog line's per-team view (lines with tierCount > 0; world thread writes, its role bots read).
struct LineView
{
    std::uint32_t version = kStateVersion;
    std::uint32_t orderId = 0;
    std::uint32_t remaining = 0;     // product units the order still wants delivered to the rep
    std::uint8_t product = kNoTier;  // tier index of the ordered product
    std::uint32_t productWant = 0;
    std::uint8_t skillup = kNoTier;  // tier index of the skill-up recipe
    std::uint32_t artisanSkill = 0;  // the artisan's profession skill (0 = offline / unknown)
    std::vector<std::uint32_t> rooms;                    // per RouteItems(line): donor room (units)
    std::array<std::uint32_t, kMaxLineTiers> surplus{};  // units per tier the rep lists / sells now
    std::vector<MarketWant> buy;                         // Market: what the rep buys on the faction AH
    std::vector<MarketWant> vendor;                      // what the artisan holds at least of each Vendor reagent
};

// Reads AutoWow.Supply.* (after AutoWowGuilds::LoadConfig; needs AutoWow.Guilds.Enable) and finds the stations.
void LoadConfig();
// World thread (PlayerbotsWorldScript::OnUpdate): overlord, feed, delivery, pay, XP, routing room.
void WorldUpdate(std::uint32_t diff);
RoleInfo RoleOf(std::uint32_t guid);
// RoleOf with the apprentice gate (GatedRole) applied: what every gate uses (supply step, party exclusion, self
// craft, the chain's artisan and members). Any thread for the bot itself; the world thread for anyone.
RoleInfo ActiveRoleOf(Player* p);
Stations const& StationsOf(bool alliance);
// The general-goods vendor near home selling kPouch (entry 0 = none), for an artisan with no bag worn.
Station const& BagVendorOf(bool alliance);
inline constexpr std::uint32_t kPouch = 4496;  // Small Brown Pouch, 6 slots, 500 copper (checked in the world DB:
                                               // Thurman Mullby 1285 in Stormwind, Gotri 3369 in Orgrimmar)
TeamView ViewOf(bool alliance);
Recipe const& BagRecipe();
std::uint32_t BagItem();
std::uint32_t BagSpell();
std::uint32_t BoltItem();
std::uint32_t BoltSpell();
std::uint32_t ThreadItem();
std::uint32_t ThreadPrice();  // copper per unit at the vendor (before reputation discount)
std::string const& BagHouseName();
std::vector<std::uint32_t> const& LearnSpells();  // trainer spell ids the artisan learns
// Map thread (artisan step): copper it lacks for due thread / training; the world tick tops it up.
void SetArtisanWant(bool alliance, std::uint64_t copper, bool canBag);
// Map thread: the rep's surplus listing / sale happened (the world tick recomputes it).
void ClearSurplus(bool alliance);
// Emit a `supply` row (any thread; no-op unless the player is a recorded bot) and a [Supply] log line. consumer >= 0
// appends ConsumerField; DemandOnly names a deliver row's consumer itself (the receiver unless a role bot, else 0).
void Emit(Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count, std::uint64_t copper,
          std::uint32_t from, std::uint32_t to, char const* op = nullptr, std::int64_t consumer = -1);
// Errand sell stop (map thread), before any sell: a cohort non-tailor mails its cloth to the bag house rep of
// its team (queued to the world thread) and the stacks are held back from this stop's sales.
void RouteCloth(Player* bot);
// SellAction: true = the item (guid-low) is queued for a cloth donation (not sold).
bool HeldForDonation(std::uint32_t itemGuid);
inline bool Tiers() { return detail::gEnabled && detail::gParams.tiers; }
inline bool Market() { return Tiers() && detail::gParams.market; }
inline bool RepStore() { return detail::gEnabled && detail::gParams.repStore; }
std::uint32_t PriceOf(std::uint32_t item);      // vendor buy price per unit (thread), 0 = unknown
std::uint32_t SellPriceOf(std::uint32_t item);  // vendor sell value per unit, 0 = unknown
// Market, map thread (the rep at its faction auctioneer): queue these buyouts; the world thread pays the rep
// their total from the house bank, bids, and deposits whatever a rejected bid did not spend back.
void QueueMarketBuys(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> buys);
// Market, map thread: cancel these own listings (PlanMarketCancels) on the world thread; the items come back by mail
// (the deposit stays spent), a `cancel` row each.
void QueueMarketCancels(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> cancels);
// World thread (AutoWowTrade mail collection): a rep took an auction-sale mail; with Market on its proceeds
// (`gold` copper) go to its house bank and a `sold` row is written. No-op for anyone else.
void OnAuctionSold(Player* bot, std::uint32_t item, std::uint32_t count, std::int64_t gold);

// ---- catalog lines (AutoWow.Supply.Products) ----
inline bool LineOn(Line l) { return detail::gEnabled && (detail::gParams.lines >> static_cast<unsigned>(l) & 1u); }
LineView LineViewOf(Line l, bool alliance);
Stations const& LineStationsOf(Line l, bool alliance);  // mailbox / auctioneer as the team's; trainer and
                                                        // threadVendor = the line's trainer and reagent vendor
std::vector<std::uint32_t> const& LineLearnSpells(Line l);
// Map thread (line artisan step): copper it lacks for vendor reagents / training / postage.
void SetLineArtisanWant(Line l, bool alliance, std::uint64_t copper);
void ClearLineSurplus(Line l, bool alliance);
// Emit a `supply` row of a line (Emit = the bags line).
void EmitLine(Line l, Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
              std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op = nullptr,
              std::int64_t consumer = -1);

// ---- need-driven production (lane V) ----
inline bool DemandOnly() { return detail::gEnabled && detail::gParams.demandOnly; }
inline bool GearBootstrap() { return detail::gEnabled && detail::gParams.gearBootstrap; }
inline bool EngGuns() { return detail::gEnabled && detail::gParams.engGuns; }
inline bool GearStockSell() { return detail::gEnabled && detail::gParams.gearStockSell; }
inline bool PotionTiers() { return detail::gEnabled && detail::gParams.potionTiers; }
// A catalog line as the runtime walks it: its tierExtra rows join only with PotionTiers (off: LineOf, the lane D table
// as was). A copy: callers keep it for the scope that reads its tiers.
[[nodiscard]] inline ProductLine ActiveLine(Line l)
{
    ProductLine out = LineOf(l);
    if (PotionTiers())
        out.tierCount = static_cast<std::uint8_t>(out.tierCount + out.tierExtra);
    return out;
}
// A gear line's recipe table: its gearStarters last rows only with GearBootstrap, its gearGuns last rows only with
// EngGuns (off: the lane V / AA table as was).
[[nodiscard]] inline RecipeTable GearTable(ProductLine const& l)
{
    return {l.gear, static_cast<std::uint8_t>(l.gearCount - (GearBootstrap() ? 0 : l.gearStarters) -
                                              (EngGuns() ? 0 : l.gearGuns))};
}
// Gear line recipe `recipe`: items one cast makes (the spell's create-item count; shot 200, a piece 1). Read-only.
std::uint32_t GearYield(Line l, std::uint8_t recipe);
inline Reason SurplusReason() { return SaleReason(DemandOnly()); }

// ---- material demand (lane G, read by AutoWow.Squad) ----
// The team's routing rooms of materials the houses can use now (any thread): cloth tiers within the bag artisan's
// reach (RouteCloth), the enabled lines' Route reagents UsableNow (RouteHerbs), raw ore / leather / stone (RouteRaw).
// Table order; zero rooms included. Flag off: empty.
std::vector<MaterialNeed> MaterialDemand(bool alliance);

// ---- outfitting (AutoWow.Supply.Outfit) ----
inline bool Outfit() { return detail::gEnabled && detail::gParams.outfit; }
// AutoWow.Supply.OutfitGear (needs Outfit; the weapon floor also AutoWow.Gear.Upgrades for the vendor catalog).
inline bool OutfitGear() { return Outfit() && detail::gParams.outfitGear; }
// Map thread (errand arrival): the bot wants `need` copper in hand for its tools / planned trainer ranks. Every
// TickMs the world thread ranks the pending requests (RankGrants) and pays each shortfall from the bot's house
// bank (AutoWowGuilds::Pay, reason grant; a short bank levies or refuses) within OutfitMaxCopper per bot per level
// and OutfitBudgetPerHour per team. One pending request per bot (a newer one replaces it).
// AutoWow.Errands.Mounts: room > 0 widens that request's per-bot cap by room and books it against its own team
// budget of roomBudgetPerHour (so a mount grant neither needs nor starves the outfit budget); `supply` grant rows
// carry op "mount".
void RequestGrant(Player* bot, std::uint64_t need, std::uint64_t room = 0, std::uint64_t roomBudgetPerHour = 0);
bool GrantPending(std::uint32_t guid);
// Any thread: a `supply` row of the bot's house with line "outfit": a tool bought (item, from = the bot, to 0 =
// the vendor) or a refusal (op; a grant refusal has item 0, from 0 = the treasury, to = the bot).
void EmitOutfit(Player* bot, Reason r, std::uint32_t item, std::uint64_t copper, char const* op = nullptr);

// ---- throughput (lane U) ----
inline bool DirectRoutes() { return detail::gEnabled && detail::gParams.directRoutes; }
inline bool MailPickup() { return detail::gEnabled && detail::gParams.mailPickup; }
inline bool MailOrders() { return detail::gEnabled && detail::gParams.mailOrders; }
// MailPickup (any thread for the bot itself): it holds a delivered, non-COD mail with items from a house rep or artisan
// (the only mail role bots send a member: its bags / potions; decided on the sender's role, not the subject).
bool HasSupplyMail(Player* bot);
// AutoWow.Gear.Flow (world thread, maps idle; GearUpgradePolicy.h): hand-me-down gear mails among the gear recipients.
// Runs without AutoWow.Supply.Enable (a flow mail also counts as a supply mail for MailPickup).
void GearFlowUpdate(std::uint32_t diff);
// MailOrders (map thread, a random seller): it holds a loose stack that fits an open order of its team.
bool HoldsOrderedItem(Player* bot);
// MailOrders (map thread, a random seller at a mailbox): fill its team's open orders from its own loose stacks, at
// most `maxMails` COD mails (one per order; units reserved at once), sent on the world thread. Returns the mails.
std::uint32_t FillOrders(Player* bot, std::uint32_t maxMails);
// MailOrders (world thread, the rep taking its mail): the verdict on a delivered COD mail of one item from `seller`
// (DecideCod against the pending fill); Accept has topped the rep's purse up to the COD from its house bank. `why`
// names a Return (unordered | budget). The pending fill is settled on Accept and Return.
CodVerdict CodAtRep(Player* rep, std::uint32_t seller, std::uint32_t item, std::uint32_t units, std::uint64_t cod,
                    bool room, char const** why);
}  // namespace AutoWowSupply

#endif
