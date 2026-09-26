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
#include <vector>

#include "AutoWowGuildsPolicy.h"

class Player;

namespace AutoWowSupply
{
inline constexpr std::uint32_t kStateVersion = 3;  // RoleState / TeamState layout; bump on change (2: tiers, market;
                                                   // 3: catalog LineView / RoleInfo.line)

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
};

[[nodiscard]] inline std::uint32_t Wants(Member const& m)
{
    std::uint32_t const want = m.empty + m.smaller;
    return want > m.incoming ? want - m.incoming : 0;
}

// Members that want a bag, most empty slots first, then most smaller bags, ties the lower guid.
[[nodiscard]] inline std::vector<Member> RankNeeds(std::vector<Member> members)
{
    members.erase(std::remove_if(members.begin(), members.end(), [](Member const& m) { return !Wants(m); }),
                  members.end());
    std::sort(members.begin(), members.end(), [](Member const& a, Member const& b)
              {
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

// ---- product catalog (AutoWow.Supply.Products) ----

// A product line: one house's profession making one family of goods for the members. Wire-stable ids (the
// ledger `line` field and the Products bit), append only.
enum class Line : std::uint8_t
{
    Bags = 0,
    Potions = 1
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
    PotionStock = 1  // member stock of its best usable tier below PotionTarget (RankStock)
};

enum class Consumer : std::uint8_t
{
    EquipBag = 0,     // the stock equip action wears a delivered bag
    DrinkAtLowHp = 1  // the stock combat "potions" strategy: critical health -> healthstone -> healing potion
};

struct Reagent
{
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    Source source = Source::Route;
};

inline constexpr std::size_t kMaxReagents = 3;
inline constexpr std::size_t kMaxLineTiers = 3;

// One recipe of a single-step line: spell -> one product per cast.
struct LineTier
{
    std::uint32_t spell = 0, product = 0;
    std::uint32_t skill = 0;     // profession skill to learn it (trainer ReqSkillRank; 1 = learned with the skill)
    std::uint32_t grey = 0;      // no skill-up at or above (SkillLineAbility TrivialSkillLineRankHigh)
    std::uint32_t reqLevel = 0;  // product item RequiredLevel (the need rule's "usable at their level")
    std::array<Reagent, kMaxReagents> reagents{};
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
};

// Bags: Weavers / tailoring 197, the lane B/C runtime (TeamTick / TierTick over kTiers), unchanged.
// Potions (Brewers / alchemy 171), checked against the 3.3.5 world DB (item_template, trainer_spell of trainer
// 67 = Stormwind 5499 / Orgrimmar 3347) and Spell.dbc / SkillLineAbility.dbc; the runtime re-checks outputs and
// reagent counts against the loaded spells and disables the line on a mismatch:
//   Minor Healing Potion 118 (req 1): spell 2330, learned with the skill, grey 95; Peacebloom 2447 + Silverleaf
//     765 + Empty Vial 3371 (vendor).
//   Lesser Healing Potion 858 (req 3): spell 2337, alchemy 55, grey 125; Minor Healing Potion + Briarthorn 2450.
//   Healing Potion 929 (req 12): spell 3447, alchemy 110, grey 175; Bruiseweed 2453 + Briarthorn + Leaded Vial
//     3372 (vendor).
inline constexpr ProductLine kCatalog[] = {
    {Line::Bags, "bags", "Bags", "Weavers", "", 197, NeedRule::BagSlots, Consumer::EquipBag, {}, 0},
    // Alchemy ranks (trainer 67): Apprentice 2275 (level 5), Journeyman 2280 (skill 50, level 10), Expert 3465
    // (skill 125, level 20).
    {Line::Potions, "potions", "Potions", "Brewers", "2275,2280,3465", 171, NeedRule::PotionStock,
     Consumer::DrinkAtLowHp,
     {{{2330, 118, 1, 95, 1, {{{2447, 1, Source::Route}, {765, 1, Source::Route}, {3371, 1, Source::Vendor}}}},
       {2337, 858, 55, 125, 3, {{{118, 1, Source::Craft}, {2450, 1, Source::Route}, {}}}},
       {3447, 929, 110, 175, 12, {{{2453, 1, Source::Route}, {2450, 1, Source::Route}, {3372, 1, Source::Vendor}}}}}},
     3},
};
inline constexpr std::size_t kLineCount = std::size(kCatalog);

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
[[nodiscard]] inline std::uint8_t TierOf(ProductLine const& l, std::uint32_t item)
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
// have(item) -> units. Holdings shared by two levels are counted at both (not so in kCatalog).
template <typename Have>
[[nodiscard]] std::uint32_t Casts(ProductLine const& l, std::size_t i, Have&& have, std::size_t depth = kMaxLineTiers)
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

template <typename Have>
void AddLacks(ProductLine const& l, std::size_t i, std::uint64_t n, Have&& have, std::vector<Lack>& out,
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

template <typename Have>
[[nodiscard]] std::vector<Lack> Lacks(ProductLine const& l, std::size_t i, std::uint64_t n, Have&& have)
{
    std::vector<Lack> out;
    AddLacks(l, i, n, have, out);
    return out;
}

// The next cast toward tier `i`: `i` when every reagent is in hand, else the tier of a short Craft reagent whose
// own next cast is possible, else kNoTier. have(item) -> units in the artisan's bags.
template <typename Have>
[[nodiscard]] std::uint8_t NextCast(ProductLine const& l, std::size_t i, Have&& have, std::size_t depth = kMaxLineTiers)
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
};

// The member's tier: the highest the house can make (known) usable at its level, else the highest usable at
// its level (nothing known yet); kNoTier = none usable.
[[nodiscard]] inline std::uint8_t BestTier(ProductLine const& l, std::uint32_t level,
                                           std::array<bool, kMaxLineTiers> const& known)
{
    std::uint8_t best = kNoTier, bestKnown = kNoTier;
    for (std::uint8_t i = 0; i < l.tierCount; ++i)
        if (l.tiers[i].reqLevel <= level)
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

// The want of the ranked members on tier `tier`.
[[nodiscard]] inline std::uint32_t TierWant(std::vector<StockNeed> const& ranked, std::uint8_t tier)
{
    std::uint32_t n = 0;
    for (StockNeed const& s : ranked)
        if (s.tier == tier)
            n += s.want;
    return n;
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
    Sold = 12     // Market: a rep's auction sold; its proceeds went to the house bank (copper = proceeds)
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
};

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
Stations const& StationsOf(bool alliance);
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
// Emit a `supply` row (any thread; no-op unless the player is a recorded bot) and a [Supply] log line.
void Emit(Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count, std::uint64_t copper,
          std::uint32_t from, std::uint32_t to, char const* op = nullptr);
// Errand sell stop (map thread), before any sell: a cohort non-tailor mails its cloth to the bag house rep of
// its team (queued to the world thread) and the stacks are held back from this stop's sales.
void RouteCloth(Player* bot);
// SellAction: true = the item (guid-low) is queued for a cloth donation (not sold).
bool HeldForDonation(std::uint32_t itemGuid);
inline bool Tiers() { return detail::gEnabled && detail::gParams.tiers; }
inline bool Market() { return Tiers() && detail::gParams.market; }
std::uint32_t PriceOf(std::uint32_t item);      // vendor buy price per unit (thread), 0 = unknown
std::uint32_t SellPriceOf(std::uint32_t item);  // vendor sell value per unit, 0 = unknown
// Market, map thread (the rep at its faction auctioneer): queue these buyouts; the world thread pays the rep
// their total from the house bank, bids, and deposits whatever a rejected bid did not spend back.
void QueueMarketBuys(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> buys);
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
              std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op = nullptr);
}  // namespace AutoWowSupply

#endif
