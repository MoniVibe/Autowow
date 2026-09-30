/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ERRANDS_POLICY_H
#define AUTOWOW_ERRANDS_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "GearUpgradePolicy.h"

// Town runs for independent AutoWoW bots (AutoWow.Errands.Enable, default 0). Random bots are kept
// alive by cheats (free repair on revive, re-rolled gear, level teleports); a persistent bot must
// maintain itself like a player: when its bags fill, its gear wears out, its food/water/ammo/reagents
// run low, or a trainer is due, it travels to a friendly town (innkeeper + repair NPC + food vendor,
// flight master when present), runs its errands with real gold, and walks/flies back.
//
// Order at the town (one batch): sell junk (stock `sell`), repair all (stock `repair`), restock
// consumables (core vendor purchase), train (stock `trainer` + profession learn filter), bind the
// hearthstone at the innkeeper, learn the flight path. Travel: the hearthstone when it is bound to the
// town, off cooldown and the town is far; else a known flight path; else a walk (no teleport).
// Each finished run emits ledger `errand` (event 14).
//
// Value-only below the runtime section: integer yards / copper / game-time ms, no floats in decisions,
// no RNG, stable orders (spawn guid ascending; ties by lower id).
namespace AutoWowErrands
{
inline constexpr std::uint8_t kStateVersion = 11;  // 2: sellUntilMs / sellRetryMs (KeepConsumables); 3: rescued;
                                                  // 4: lastGearLevel / gearItems / gearNpcs (Gear.Upgrades);
                                                  // 5: nextOutfitMs (Supply.Outfit); 6: nextMailMs (Supply.MailPickup);
                                                  // 7: nextTrainRunMs (Professions.TrainRuns);
                                                  // 8: floorGear (Supply.OutfitGear);
                                                  // 9: lastAhGearLevel / ahGearItems (Gear.AuctionUpgrades);
                                                  // 10: nextMountMs / mountGrantMs / rideTier / rideLearn /
                                                  //     mountItem (Errands.Mounts)
                                                  // 11: walk admission / bounded town walk backoff

// ---- needs ---------------------------------------------------------------------------------------
// Wire-stable bits (ledger `needs`); append only.
enum Need : std::uint32_t
{
    NeedBags = 1u << 0,        // bags >= BagSoftPct used
    NeedRepair = 1u << 1,      // average equipped durability < DurabilitySoftPct
    NeedFood = 1u << 2,
    NeedWater = 1u << 3,
    NeedAmmo = 1u << 4,
    NeedReagent = 1u << 5,
    NeedClassTrain = 1u << 6,  // two levels since the last class-trainer visit and the budget in hand
    NeedProfTrain = 1u << 7,   // a profession rank is learnable (skill near cap, level reached)
    NeedHearth = 1u << 8,      // hearthstone bound outside the current zone
    NeedFlightPath = 1u << 9,  // current zone's flight master node unknown
    NeedGear = 1u << 10,       // AutoWow.Gear.Upgrades: vendor weapon / armor shopping due (AutoWowGear::GearDue)
    NeedTool = 1u << 11,       // AutoWow.Supply.Outfit: a known Mining / Skinning without its gathering tool
    NeedMail = 1u << 12,       // AutoWow.Supply.MailPickup: a supply mail (bag / potions from its house) waits
    NeedAhGear = 1u << 13,     // AutoWow.Gear.AuctionUpgrades: gear far under the ilvl curve, gold for the AH
    NeedRiding = 1u << 14      // AutoWow.Errands.Mounts: a riding rank / mount run (its own site, not a town)
};
inline constexpr std::uint32_t kConsumableNeeds = NeedFood | NeedWater | NeedAmmo | NeedReagent;

// Consumable kinds; bit (1 << kind) is a vendor's `sells` mask.
enum Kind : std::uint8_t
{
    KindFood = 0,
    KindWater = 1,
    KindArrow = 2,
    KindBullet = 3,
    KindReagent = 4
};
inline constexpr std::size_t kKinds = 5;

struct Tier
{
    std::uint32_t item = 0;
    std::uint32_t minLevel = 0;  // item_template.RequiredLevel (world DB, checked 2026-09-24)
};

// Standard vendor stock, lowest tier first. Food/water packs of 5; ammo packs of 200.
inline constexpr std::array<Tier, 9> kFood = {{{4540, 1}, {4541, 5}, {4542, 15}, {4544, 25}, {4601, 35},
                                               {8950, 45}, {27855, 55}, {33449, 65}, {35950, 75}}};
inline constexpr std::array<Tier, 9> kWater = {{{159, 1}, {1179, 5}, {1205, 15}, {1708, 25}, {1645, 35},
                                                {8766, 45}, {28399, 60}, {33444, 70}, {33445, 75}}};
inline constexpr std::array<Tier, 6> kArrows = {{{2512, 1}, {2515, 10}, {3030, 25}, {11285, 40}, {28053, 55},
                                                 {41586, 75}}};
inline constexpr std::array<Tier, 6> kBullets = {{{2516, 1}, {2519, 10}, {3033, 25}, {11284, 40}, {28060, 55},
                                                  {41584, 75}}};
// Shaman Reincarnation (learned at 30) consumes an Ankh. Rogue Flash Powder (Vanish) has no vendor in
// this world DB, so no rogue reagent line.
inline constexpr std::array<Tier, 1> kShamanReagent = {{{17030, 30}}};

inline constexpr std::uint32_t kClassWarrior = 1, kClassPaladin = 2, kClassHunter = 3, kClassRogue = 4,
                               kClassPriest = 5, kClassDeathKnight = 6, kClassShaman = 7, kClassMage = 8,
                               kClassWarlock = 9, kClassDruid = 11;

// Ranged weapon ammo: 0 none, 1 arrows (bow/crossbow), 2 bullets (gun).
enum RangedAmmo : std::uint8_t
{
    AmmoNone = 0,
    AmmoArrows = 1,
    AmmoBullets = 2
};

// Kinds a class buys at a level (bit per Kind). Mages conjure food and water from level 6.
[[nodiscard]] inline std::uint32_t KindsFor(std::uint32_t cls, std::uint32_t level, RangedAmmo ammo)
{
    std::uint32_t kinds = 0;
    bool const mageConjures = cls == kClassMage && level >= 6;
    if (!mageConjures)
        kinds |= 1u << KindFood;
    bool const mana = cls == kClassPaladin || cls == kClassHunter || cls == kClassPriest || cls == kClassShaman ||
                      cls == kClassMage || cls == kClassWarlock || cls == kClassDruid;
    if (mana && !mageConjures)
        kinds |= 1u << KindWater;
    if (cls == kClassHunter && ammo == AmmoArrows)
        kinds |= 1u << KindArrow;
    if (cls == kClassHunter && ammo == AmmoBullets)
        kinds |= 1u << KindBullet;
    if (cls == kClassShaman && level >= kShamanReagent[0].minLevel)
        kinds |= 1u << KindReagent;
    return kinds;
}

// AutoWow.Supply.OutfitGear food floor: every errand tops food (and drink for mana users) up to Target, a mage too
// (S62-S65: mages out of mana 23% of combat; squad mage 70581 held no drink).
[[nodiscard]] inline std::uint32_t FloorKinds(std::uint32_t cls, std::uint32_t level, RangedAmmo ammo)
{
    return KindsFor(cls, level, ammo) | (cls == kClassMage ? (1u << KindFood) | (1u << KindWater) : 0u);
}

// Tier table of a kind (the reagent table is the shaman's; KindsFor gates the class).
struct TierSpan
{
    Tier const* data = nullptr;
    std::size_t size = 0;
};

[[nodiscard]] inline TierSpan TiersOf(Kind kind)
{
    switch (kind)
    {
        case KindFood: return {kFood.data(), kFood.size()};
        case KindWater: return {kWater.data(), kWater.size()};
        case KindArrow: return {kArrows.data(), kArrows.size()};
        case KindBullet: return {kBullets.data(), kBullets.size()};
        case KindReagent: return {kShamanReagent.data(), kShamanReagent.size()};
    }
    return {};
}

// Best tier the bot may use: highest minLevel <= level, restricted to `available` (sorted item ids the
// town sells) when given. 0 = none.
[[nodiscard]] inline std::uint32_t BestTier(Kind kind, std::uint32_t level, std::vector<std::uint32_t> const* available)
{
    TierSpan const t = TiersOf(kind);
    for (std::size_t k = t.size; k-- > 0;)
    {
        if (t.data[k].minLevel > level)
            continue;
        if (available && !std::binary_search(available->begin(), available->end(), t.data[k].item))
            continue;
        return t.data[k].item;
    }
    return 0;
}

// True when `item` belongs to a tier table of `kind`.
[[nodiscard]] inline bool IsTierItem(Kind kind, std::uint32_t item)
{
    TierSpan const t = TiersOf(kind);
    for (std::size_t k = 0; k < t.size; ++k)
        if (t.data[k].item == item)
            return true;
    return false;
}

// AutoWow.Supply.Outfit gathering tools: bit per tool (Npc::tools, Obs::missingTools, PlanInput::tools) and the
// item bought for it. Mining Pick 2901 (BuyPrice 81) / Skinning Knife 7005 (BuyPrice 82): RequiredLevel 1, plain
// gold on 202 / 103 npc_vendor spawns (trade / mining / leatherworking supplies; world DB checked 2026-09-26).
// Any pick / knife of AutoWowSquad::kMiningPicks / kSkinningKnives counts as held (a pick equipped too).
enum Tool : std::uint8_t
{
    ToolPick = 1u << 0,
    ToolKnife = 1u << 1
};
inline constexpr std::size_t kTools = 2;
inline constexpr std::uint32_t kToolItems[kTools] = {2901, 7005};
inline constexpr std::uint32_t kToolSkills[kTools] = {186, 393};  // Mining, Skinning

// Tool bits missing: a tool whose skill the bot wants (`wants` bit i = kToolSkills[i]) and holds none of.
[[nodiscard]] inline std::uint8_t MissingTools(std::uint8_t wants, std::uint8_t holds)
{
    return static_cast<std::uint8_t>(wants & ~holds & (ToolPick | ToolKnife));
}

// A missing tool alone starts a run once per AutoWow.Supply.OutfitCheckMs (nextOutfitMs = run start + it).
[[nodiscard]] inline bool OutfitRunDue(std::uint8_t missingTools, std::uint64_t nowMs, std::uint64_t nextOutfitMs)
{
    return missingTools && nowMs >= nextOutfitMs;
}

// A waiting supply mail alone starts a run once per AutoWow.Supply.MailRunMs (nextMailMs = run check + it); else it
// rides along as a soft need (low priority: the bot mostly takes it at a mailbox it passes, MailPickup).
[[nodiscard]] inline bool MailRunDue(bool supplyMail, std::uint64_t nowMs, std::uint64_t nextMailMs)
{
    return supplyMail && nowMs >= nextMailMs;
}

struct Params
{
    std::uint32_t checkIntervalMs = 60000;     // AutoWow.Errands.CheckIntervalMs
    std::uint32_t cooldownMs = 900000;         // AutoWow.Errands.CooldownMs (after any finished run)
    std::uint32_t bagSoftPct = 70;             // AutoWow.Errands.BagSoftPct
    std::uint32_t bagUrgentPct = 90;           // AutoWow.Errands.BagUrgentPct
    std::uint32_t durabilitySoftPct = 50;      // AutoWow.Errands.DurabilitySoftPct
    std::uint32_t durabilityUrgentPct = 25;    // AutoWow.Errands.DurabilityUrgentPct
    std::uint32_t foodLow = 5;                 // AutoWow.Errands.FoodLow / FoodTarget
    std::uint32_t foodTarget = 20;
    std::uint32_t waterLow = 5;                // AutoWow.Errands.WaterLow / WaterTarget
    std::uint32_t waterTarget = 20;
    std::uint32_t ammoLow = 200;               // AutoWow.Errands.AmmoLow / AmmoTarget (0 stock = urgent)
    std::uint32_t ammoTarget = 1000;
    std::uint32_t reagentLow = 1;              // AutoWow.Errands.ReagentLow / ReagentTarget
    std::uint32_t reagentTarget = 5;
    std::uint32_t townRadius = 180;            // AutoWow.Errands.TownRadius: npc cluster around an innkeeper
    std::uint32_t arriveYards = 25;            // at the innkeeper = arrived
    std::uint32_t hearthMinYards = 800;        // AutoWow.Errands.HearthMinYards
    std::uint32_t flightMinYards = 600;        // shorter trips always walk
    std::uint32_t maxWalkYards = 4000;         // AutoWow.Errands.MaxWalkYards
    std::uint32_t levelOver = 3;               // skip towns in zones whose bracket starts above level + this
    std::uint32_t candidateTowns = 8;          // nearest same-map towns costed per decision
    bool sellTradeGoods = false;               // AutoWow.Errands.SellTradeGoods: vendor unneeded trade goods
    std::uint32_t auctionDetourMs = 0;         // AutoWow.Errands.AuctionDetourMs: with AutoWow.Trade on, a
                                               // town with an auctioneer costs this much less
    std::uint32_t travelTimeoutMs = 1200000;   // AutoWow.Errands.TravelTimeoutMs
    std::uint32_t errandsTimeoutMs = 600000;
    std::uint32_t stopTimeoutMs = 90000;       // per npc stop
    std::uint32_t returnMinYards = 150;        // closer than this: no return leg
    std::uint32_t returnTimeoutMs = 1200000;
    std::uint32_t maxReissues = 6;             // stuck walks / failed flights / hearth casts per leg
    std::uint32_t hearthCostMs = 15000;        // cast + load, for town choice
    std::uint32_t flightOverheadMs = 10000;    // taxi activation + landing
    std::uint32_t walkYardsPerSec = 7;
    std::uint32_t flyYardsPerSec = 30;
    // AutoWow.Survival.KeepConsumables (default 0; needs AutoWow.Errands.Enable): out of food (drink for
    // mana users) is urgent, a restock is affordable when money + grey loot value covers one pack of each
    // needed kind, the town buys to Low first (food before water) then to Target, greys are sold at every
    // vendor stop and at a vendor the bot passes (within SellDetourYards).
    bool keepConsumables = false;
    std::uint32_t sellDetourYards = 30;        // AutoWow.Survival.KeepConsumables.SellDetourYards
    std::uint32_t sellDetourMs = 30000;        // walk-to-vendor budget per detour
    std::uint32_t sellRetryMs = 300000;        // after a detour (sold or given up)
    // AutoWow.Errands.Mounts (default 0; cohort bots; grants need AutoWow.Supply.Outfit).
    bool mounts = false;
    std::uint32_t mountsCheckMs = 600000;          // AutoWow.Errands.MountsCheckMs: one mount decision per window
    std::uint32_t mountsMaxTier = 2;               // AutoWow.Errands.MountsMaxTier: 1 apprentice, 2 journeyman,
                                                   // 3 expert (flying, Outland)
    std::uint32_t mountsGrantBudgetPerHour = 100000;  // AutoWow.Errands.MountsGrantBudgetPerHour: mount grants per
                                                      // team per game hour (copper)
};

// Integer yards / percentages ------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t Dist2(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
{
    std::int64_t const dx = std::int64_t(ax) - bx;
    std::int64_t const dy = std::int64_t(ay) - by;
    return dx * dx + dy * dy;
}

[[nodiscard]] inline std::uint32_t ISqrt(std::int64_t v)
{
    if (v <= 0)
        return 0;
    std::uint64_t r = static_cast<std::uint64_t>(v);
    std::uint64_t x = r;
    std::uint64_t y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + r / x) / 2;
    }
    return static_cast<std::uint32_t>(x);
}

[[nodiscard]] inline std::uint32_t Yards(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
{
    return ISqrt(Dist2(ax, ay, bx, by));
}

// Sum of current / max durability over equipped items that have durability. No such item = 100.
[[nodiscard]] inline std::uint32_t DurabilityPct(std::uint64_t cur, std::uint64_t max)
{
    return max ? static_cast<std::uint32_t>(std::min<std::uint64_t>(cur, max) * 100 / max) : 100;
}

[[nodiscard]] inline std::uint32_t BagUsedPct(std::uint32_t used, std::uint32_t total)
{
    return total ? std::min<std::uint32_t>(used, total) * 100 / total : 100;
}

// Class-trainer trip budget (copper): a trip is worth it only with roughly one rank's gold in hand.
// ponytail: level^2 * 5 approximates 3.3.5 rank costs (lvl 20 ~ 20s, lvl 60 ~ 1g80s); a real ask comes
// from the trainer at the town (the stock trainer action skips what the bot cannot afford).
[[nodiscard]] inline std::uint64_t ClassTrainBudgetCopper(std::uint32_t level)
{
    return std::uint64_t(level) * level * 5;
}

// Two levels since the last class-trainer visit (0 = never this session) and the budget in hand.
[[nodiscard]] inline bool ClassTrainDue(std::uint32_t level, std::uint32_t lastTrainLevel, std::uint64_t money)
{
    return level >= 2 && level >= lastTrainLevel + 2 && money >= ClassTrainBudgetCopper(level);
}

// A profession's next rank (journeyman .. grand master) is learnable: skill within 25 of the cap and
// the rank's level reached. Artisan -> master etc. per 3.3.5 (cap 75/150/225/300/375 -> lvl 10/20/35/50/65).
[[nodiscard]] inline bool ProfessionRankDue(std::uint32_t value, std::uint32_t max, std::uint32_t level)
{
    static constexpr std::uint32_t kCap[] = {75, 150, 225, 300, 375};
    static constexpr std::uint32_t kLevel[] = {10, 20, 35, 50, 65};
    for (std::size_t k = 0; k < 5; ++k)
        if (max == kCap[k])
            return value + 25 >= max && level >= kLevel[k];
    return false;
}

// AutoWow.Professions.TrainRuns: a planned profession needs a trainer when the bot lacks it (from level 5,
// the apprentice level) or sits at its rank max with the next rank's level reached (ProfessionRankDue).
[[nodiscard]] inline bool PlannedTrainNeeded(bool known, std::uint32_t value, std::uint32_t max, std::uint32_t level)
{
    return known ? max && value >= max && ProfessionRankDue(value, max, level) : level >= 5;
}

// ... and alone starts a run once per TrainRunCooldownMs (nextTrainRunMs = check + it).
[[nodiscard]] inline bool TrainRunDue(bool needed, std::uint64_t nowMs, std::uint64_t nextTrainRunMs)
{
    return needed && nowMs >= nextTrainRunMs;
}

struct Obs
{
    std::uint32_t bagUsedPct = 0;
    std::uint32_t durabilityPct = 100;
    std::uint32_t cls = 0;
    std::uint32_t level = 1;
    RangedAmmo ammo = AmmoNone;
    std::array<std::uint32_t, kKinds> have{};  // stock per kind (tier items in bags)
    bool restockAffordable = true;             // money covers a quarter of the deficit at vendor price
    bool classTrainDue = false;
    bool profTrainDue = false;
    bool hearthElsewhere = false;
    bool unknownFlightPath = false;
    AutoWowGear::Due gear;                     // AutoWow.Gear.Upgrades only (all false with the flag off)
    std::uint8_t missingTools = 0;             // AutoWow.Supply.Outfit only: Tool bits (MissingTools)
    bool toolRunDue = false;                   // AutoWow.Supply.Outfit only: OutfitRunDue
    bool supplyMail = false;                   // AutoWow.Supply.MailPickup only: HasSupplyMail
    bool mailRunDue = false;                   // AutoWow.Supply.MailPickup only: MailRunDue
    bool trainRunDue = false;                  // AutoWow.Professions.TrainRuns only: TrainRunDue
    bool ahGearDue = false;                    // AutoWow.Gear.AuctionUpgrades only: AutoWowGear::AhRunDue
};

struct Assessment
{
    std::uint32_t needs = 0;   // every need, urgent ones included
    std::uint32_t urgent = 0;  // subset that alone justifies a run
};

// Low/target thresholds of a kind.
[[nodiscard]] inline std::uint32_t LowOf(Params const& p, Kind kind)
{
    switch (kind)
    {
        case KindFood: return p.foodLow;
        case KindWater: return p.waterLow;
        case KindArrow:
        case KindBullet: return p.ammoLow;
        case KindReagent: return p.reagentLow;
    }
    return 0;
}

[[nodiscard]] inline std::uint32_t TargetOf(Params const& p, Kind kind)
{
    switch (kind)
    {
        case KindFood: return p.foodTarget;
        case KindWater: return p.waterTarget;
        case KindArrow:
        case KindBullet: return p.ammoTarget;
        case KindReagent: return p.reagentTarget;
    }
    return 0;
}

[[nodiscard]] inline std::uint32_t NeedOf(Kind kind)
{
    switch (kind)
    {
        case KindFood: return NeedFood;
        case KindWater: return NeedWater;
        case KindArrow:
        case KindBullet: return NeedAmmo;
        case KindReagent: return NeedReagent;
    }
    return 0;
}

// Urgent: bags >= BagUrgentPct, durability < DurabilityUrgentPct, a hunter with no ammo. The rest soft.
// Consumable needs drop when the bot cannot afford a useful restock.
[[nodiscard]] inline Assessment Assess(Params const& p, Obs const& o)
{
    Assessment a;
    if (o.bagUsedPct >= p.bagSoftPct)
        a.needs |= NeedBags;
    if (o.bagUsedPct >= p.bagUrgentPct)
        a.urgent |= NeedBags;
    if (o.durabilityPct < p.durabilitySoftPct)
        a.needs |= NeedRepair;
    if (o.durabilityPct < p.durabilityUrgentPct)
        a.urgent |= NeedRepair;
    std::uint32_t const kinds = KindsFor(o.cls, o.level, o.ammo);
    for (std::size_t k = 0; k < kKinds; ++k)
    {
        Kind const kind = static_cast<Kind>(k);
        if (!(kinds & (1u << k)) || o.have[k] >= LowOf(p, kind))
            continue;
        a.needs |= NeedOf(kind);
        if ((kind == KindArrow || kind == KindBullet) && o.have[k] == 0)
            a.urgent |= NeedAmmo;
        // AutoWow.Survival.KeepConsumables: nothing to eat / drink left is urgent (a rest without it
        // takes the full regen time; soak-s14: 33 of 50 bots carried none).
        if (p.keepConsumables && (kind == KindFood || kind == KindWater) && o.have[k] == 0)
            a.urgent |= NeedOf(kind);
    }
    if (!o.restockAffordable)
    {
        a.needs &= ~kConsumableNeeds;
        a.urgent &= ~kConsumableNeeds;
    }
    if (o.classTrainDue)
        a.needs |= NeedClassTrain;
    if (o.profTrainDue)
        a.needs |= NeedProfTrain;
    if (o.hearthElsewhere)
        a.needs |= NeedHearth;
    if (o.unknownFlightPath)
        a.needs |= NeedFlightPath;
    if (o.gear.soft)
        a.needs |= NeedGear;
    if (o.gear.urgent)
        a.urgent |= NeedGear;
    // AutoWow.Supply.Outfit: a missing tool is soft, and alone starts a run once per OutfitCheckMs.
    if (o.missingTools)
        a.needs |= NeedTool;
    if (o.missingTools && o.toolRunDue)
        a.urgent |= NeedTool;
    // AutoWow.Supply.MailPickup: a waiting supply mail is soft, and alone starts a run once per MailRunMs.
    if (o.supplyMail)
        a.needs |= NeedMail;
    if (o.supplyMail && o.mailRunDue)
        a.urgent |= NeedMail;
    // AutoWow.Professions.TrainRuns: a missing / capped planned profession alone starts a run.
    if (o.trainRunDue)
        a.urgent |= NeedProfTrain;
    // AutoWow.Gear.AuctionUpgrades: alone starts a run (to an auction town) once per level.
    if (o.ahGearDue)
        a.urgent |= NeedAhGear;
    a.needs |= a.urgent;
    return a;
}

// A run: any urgent need, or two soft needs.
[[nodiscard]] inline bool ShouldRun(std::uint32_t needs, std::uint32_t urgent)
{
    std::uint32_t n = 0;
    for (std::uint32_t v = needs; v; v &= v - 1)
        ++n;
    return urgent != 0 || n >= 2;
}

// AutoWow.Survival.KeepConsumables affordability: money plus the vendor value of the grey loot the town
// visit sells first covers one pack of every needed kind (the plain rule wants a quarter of the deficit).
[[nodiscard]] inline bool KeepAffordable(std::uint64_t money, std::uint64_t greyCopper, std::uint64_t onePackCopper)
{
    return money + greyCopper >= onePackCopper;
}

// ---- towns ---------------------------------------------------------------------------------------
enum Role : std::uint32_t
{
    RoleInn = 1u << 0,
    RoleRepair = 1u << 1,
    RoleVendor = 1u << 2,
    RoleFlight = 1u << 3,
    RoleClassTrainer = 1u << 4,
    RoleTradeTrainer = 1u << 5,
    RoleAuction = 1u << 6,  // AutoWow.Trade: auctioneer (catalogued only with the flag on)
    RoleMailbox = 1u << 7   // AutoWow.Trade: mailbox gameobject (spawn = GO guid | kGoSpawnBit)
};

// Mailbox spawns are gameobject DB guids with this bit set: a disjoint id space from creature spawns
// (creature guids stay below 2^31), so spawn stays a unique, never-reused stop key.
inline constexpr std::uint32_t kGoSpawnBit = 0x80000000u;

// Team bits: 1 alliance, 2 horde (core TeamId + 1 as a bit).
inline constexpr std::uint8_t kAlliance = 1, kHorde = 2;

struct Npc
{
    std::uint32_t spawn = 0;  // creature DB guid: stable, never reused
    std::uint32_t entry = 0;
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t roles = 0;
    std::uint8_t teams = 0;               // teams it does not attack (faction hostile mask)
    std::uint32_t sells = 0;              // (1 << Kind) of tier items on its vendor list
    std::vector<std::uint32_t> items;     // tier items it sells, ascending
    std::vector<std::uint32_t> gear;      // AutoWow.Gear.Upgrades: weapons / armor it sells, ascending
    std::uint8_t tools = 0;               // Tool bits of the kToolItems it sells for plain gold (read by Outfit only)
    std::uint32_t nodeAlliance = 0;       // flight master: nearest taxi node per team
    std::uint32_t nodeHorde = 0;
};

struct Town
{
    std::uint32_t id = 0;  // innkeeper spawn guid
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t teams = 0;         // teams that find inn + repair + food/water vendor here
    std::uint32_t sells = 0;        // union of its vendors
    std::vector<Npc> npcs;          // spawn ascending, the innkeeper included
};

// One town per innkeeper with, inside `radius` yards on its map, a repair NPC and a vendor of food or
// water usable by the same team. Every role NPC in the radius joins it. Towns ascend by innkeeper spawn.
[[nodiscard]] inline std::vector<Town> BuildTowns(std::vector<Npc> npcs, std::uint32_t radius)
{
    std::sort(npcs.begin(), npcs.end(), [](Npc const& a, Npc const& b) { return a.spawn < b.spawn; });
    std::int64_t const r2 = std::int64_t(radius) * radius;
    std::uint32_t const foodOrWater = (1u << KindFood) | (1u << KindWater);
    std::vector<Town> towns;
    for (Npc const& inn : npcs)
    {
        if (!(inn.roles & RoleInn) || !inn.teams)
            continue;
        Town t;
        t.id = inn.spawn;
        t.map = inn.map;
        t.zone = inn.zone;
        t.x = inn.x;
        t.y = inn.y;
        t.z = inn.z;
        std::uint8_t repair = 0, food = 0;
        for (Npc const& n : npcs)
        {
            if (n.map != inn.map || !(n.teams & inn.teams) || Dist2(inn.x, inn.y, n.x, n.y) > r2)
                continue;
            t.npcs.push_back(n);
            if (n.roles & RoleRepair)
                repair |= n.teams;
            if ((n.roles & RoleVendor) && (n.sells & foodOrWater))
                food |= n.teams;
        }
        t.teams = inn.teams & repair & food;
        if (!t.teams)
            continue;
        for (Npc const& n : t.npcs)
            if (n.teams & t.teams)
                t.sells |= n.sells;
        towns.push_back(std::move(t));
    }
    return towns;
}

// ---- travel --------------------------------------------------------------------------------------
// Wire-stable names (ledger `leg`); append only.
enum class Leg : std::uint8_t
{
    None = 0,
    Walk = 1,
    Flight = 2,
    Hearth = 3
};

inline constexpr char const* LegName(Leg leg)
{
    switch (leg)
    {
        case Leg::None: return "none";
        case Leg::Walk: return "walk";
        case Leg::Flight: return "flight";
        case Leg::Hearth: return "hearth";
    }
    return "none";
}

struct LegInput
{
    bool sameMap = true;
    std::uint32_t walkYards = 0;      // straight line bot -> destination
    bool walkRouteAvailable = true;   // exact complete path or fresh reachable TravelMgr prefix
    bool hearthHere = false;       // hearthstone bound within TownRadius of the destination (same map)
    bool hearthReady = false;      // hearthstone in bags, spell off cooldown
    bool flight = false;           // taxi path from the nearest flight master to a known node there
    std::uint32_t fmYards = 0;     // bot -> nearest flight master
    std::uint32_t flyYards = 0;    // flight master -> destination node (straight line)
    std::uint32_t tailYards = 0;   // destination node -> destination
};

[[nodiscard]] inline std::uint32_t LegCostMs(Params const& p, Leg leg, LegInput const& in)
{
    std::uint64_t const walk = std::max<std::uint32_t>(1, p.walkYardsPerSec);
    std::uint64_t const fly = std::max<std::uint32_t>(1, p.flyYardsPerSec);
    std::uint64_t ms = 0;
    switch (leg)
    {
        case Leg::None: return UINT32_MAX;
        case Leg::Walk: ms = std::uint64_t(in.walkYards) * 1000 / walk; break;
        case Leg::Flight:
            ms = (std::uint64_t(in.fmYards) + in.tailYards) * 1000 / walk + std::uint64_t(in.flyYards) * 1000 / fly +
                 p.flightOverheadMs;
            break;
        case Leg::Hearth: ms = p.hearthCostMs; break;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, UINT32_MAX - 1));
}

// Hearthstone when bound here, ready and the trip is long (same map only: a hearth onto another
// continent would strand the bot away from its quests). Else the cheaper of flight (known path, trip >=
// FlightMinYards) and walk (<= MaxWalkYards). None = unreachable.
[[nodiscard]] inline Leg ChooseLeg(Params const& p, LegInput const& in)
{
    if (!in.sameMap)
        return Leg::None;
    if (in.hearthHere && in.hearthReady && in.walkYards > p.hearthMinYards)
        return Leg::Hearth;
    bool const canFly = in.flight && in.walkYards >= p.flightMinYards;
    bool const canWalk = in.walkRouteAvailable && in.walkYards <= p.maxWalkYards;
    if (canFly && (!canWalk || LegCostMs(p, Leg::Flight, in) < LegCostMs(p, Leg::Walk, in)))
        return Leg::Flight;
    return canWalk ? Leg::Walk : Leg::None;
}

struct Candidate
{
    std::uint32_t town = 0;
    Leg leg = Leg::None;
    std::uint32_t costMs = 0;
};

// Travel cost less the auction detour allowance (floor 0).
[[nodiscard]] inline std::uint32_t DetourCostMs(std::uint32_t costMs, bool auction, std::uint32_t detourMs)
{
    return auction ? (costMs > detourMs ? costMs - detourMs : 0) : costMs;
}

// Cheapest reachable candidate; ties go to the lower town id. nullptr = none.
[[nodiscard]] inline Candidate const* PickTown(std::vector<Candidate> const& cands)
{
    Candidate const* best = nullptr;
    for (Candidate const& c : cands)
        if (c.leg != Leg::None &&
            (!best || c.costMs < best->costMs || (c.costMs == best->costMs && c.town < best->town)))
            best = &c;
    return best;
}

// Town zone too dangerous to reach on foot or by flight: its bracket starts above level + LevelOver.
[[nodiscard]] inline bool ZoneTooHigh(Params const& p, std::uint32_t level, std::uint32_t bracketLow)
{
    return bracketLow && bracketLow > level + p.levelOver;
}

// Needs a town can serve: bags/repair always; a consumable when a vendor there sells its tiers;
// trainers when present (class: valid for the bot); hearth/flight path only in the bot's zone.
struct TownFacts
{
    std::uint32_t sells = 0;
    bool classTrainer = false;
    bool tradeTrainer = false;
    bool inBotZone = false;
    bool unknownFlightMaster = false;  // town has a flight master whose node the bot lacks
    bool gearVendor = false;           // AutoWow.Gear.Upgrades: a usable vendor sells weapons / armor
    std::uint8_t tools = 0;            // AutoWow.Supply.Outfit: missing Tool bits a usable vendor sells
    bool mailbox = false;              // AutoWow.Supply.MailPickup: the town has a mailbox (catalogued with Trade on)
    bool auction = false;              // AutoWow.Gear.AuctionUpgrades: a usable auctioneer (catalogued with Trade on)
};

[[nodiscard]] inline std::uint32_t Serves(TownFacts const& f)
{
    std::uint32_t m = NeedBags | NeedRepair;
    if (f.sells & (1u << KindFood))
        m |= NeedFood;
    if (f.sells & (1u << KindWater))
        m |= NeedWater;
    if (f.sells & ((1u << KindArrow) | (1u << KindBullet)))
        m |= NeedAmmo;
    if (f.sells & (1u << KindReagent))
        m |= NeedReagent;
    if (f.classTrainer)
        m |= NeedClassTrain;
    if (f.tradeTrainer)
        m |= NeedProfTrain;
    if (f.inBotZone)
        m |= NeedHearth;
    if (f.inBotZone && f.unknownFlightMaster)
        m |= NeedFlightPath;
    if (f.gearVendor)
        m |= NeedGear;
    if (f.tools)
        m |= NeedTool;
    if (f.mailbox)
        m |= NeedMail;
    if (f.auction)
        m |= NeedAhGear;
    return m;
}

// ---- restock ---------------------------------------------------------------------------------------
struct RestockLine
{
    Kind kind = KindFood;
    std::uint32_t item = 0;   // best tier the town sells (0 = none: nothing to buy)
    std::uint32_t have = 0;
    std::uint32_t target = 0;
};

// What to buy at a town: every kind the class uses whose stock is below target (topped up to target,
// not only when under Low: the bot is at the vendor anyway), best tier sold there.
[[nodiscard]] inline std::vector<RestockLine> RestockList(Params const& p, std::uint32_t cls, std::uint32_t level,
                                                          RangedAmmo ammo,
                                                          std::array<std::uint32_t, kKinds> const& have,
                                                          std::vector<std::uint32_t> const& available)
{
    std::vector<RestockLine> out;
    std::uint32_t const kinds = KindsFor(cls, level, ammo);
    for (std::size_t k = 0; k < kKinds; ++k)
    {
        Kind const kind = static_cast<Kind>(k);
        if (!(kinds & (1u << k)) || have[k] >= TargetOf(p, kind))
            continue;
        std::uint32_t const item = BestTier(kind, level, &available);
        if (item)
            out.push_back({kind, item, have[k], TargetOf(p, kind)});
    }
    return out;
}

// AutoWow.Survival.KeepConsumables buys in two passes over the kinds (food first): pass 0 up to Low,
// pass 1 up to Target, so scarce gold buys some food and some drink before topping either up.
[[nodiscard]] inline std::uint32_t PassTarget(Params const& p, Kind kind, std::uint32_t pass)
{
    return pass == 0 ? std::min(LowOf(p, kind), TargetOf(p, kind)) : TargetOf(p, kind);
}

// Packs to buy for `deficit` items sold `perPack` at a time, `packPrice` copper each, `spendable`
// copper in hand. Short = money limited the purchase.
struct PackBuy
{
    std::uint32_t packs = 0;
    bool shortOfMoney = false;
};

[[nodiscard]] inline PackBuy PacksToBuy(std::uint32_t deficit, std::uint32_t perPack, std::uint64_t packPrice,
                                        std::uint64_t spendable)
{
    PackBuy b;
    if (!deficit)
        return b;
    std::uint32_t const per = std::max<std::uint32_t>(1, perPack);
    std::uint32_t const want = (deficit + per - 1) / per;
    std::uint64_t const afford = packPrice ? spendable / packPrice : want;
    b.packs = static_cast<std::uint32_t>(std::min<std::uint64_t>(want, afford));
    b.shortOfMoney = b.packs < want;
    return b;
}

// AutoWow.Supply.OutfitGear: copper of the whole packs topping `deficit` up (the food floor's grant share).
[[nodiscard]] inline std::uint64_t FloorCopper(std::uint32_t deficit, std::uint32_t perPack, std::uint64_t packPrice)
{
    std::uint32_t const per = std::max<std::uint32_t>(1, perPack);
    return std::uint64_t((deficit + per - 1) / per) * packPrice;
}

// ---- errands at the town --------------------------------------------------------------------------
// Wire-stable bits (ledger `done`); append only.
enum Done : std::uint32_t
{
    DoneSold = 1u << 0,
    DoneRepaired = 1u << 1,
    DoneRestocked = 1u << 2,
    DoneTrained = 1u << 3,
    DoneBound = 1u << 4,
    DoneLearnedFp = 1u << 5,
    DoneSkipped = 1u << 6,  // an item or trainer rank was skipped as unaffordable (logged)
    DoneGeared = 1u << 7,   // AutoWow.Gear.Upgrades: a vendor weapon / armor piece was bought
    DoneTooled = 1u << 8,   // AutoWow.Supply.Outfit: a gathering tool was bought
    DoneWeaponFloor = 1u << 9,  // AutoWow.Supply.OutfitGear: a floor weapon was bought
    DoneFoodFloor = 1u << 10,   // AutoWow.Supply.OutfitGear: food / drink was bought
    DoneAhGear = 1u << 11,      // AutoWow.Gear.AuctionUpgrades: an auction-bought piece was equipped
    DoneRiding = 1u << 12,      // AutoWow.Errands.Mounts: the riding rank was learned
    DoneMount = 1u << 13        // AutoWow.Errands.Mounts: a mount was learned
};

// Operations at one npc, run in bit order.
enum Op : std::uint32_t
{
    OpSell = 1u << 0,
    OpRepair = 1u << 1,
    OpBuy = 1u << 2,
    OpTrain = 1u << 3,
    OpBind = 1u << 4,
    OpLearnFp = 1u << 5,
    OpAuction = 1u << 6,  // AutoWow.Trade (TradePolicy.h): post / buy at the auctioneer
    OpMail = 1u << 7,     // AutoWow.Trade: collect mail at a mailbox (entry = gameobject entry)
    OpGear = 1u << 8,     // AutoWow.Gear.Upgrades: buy the planned weapon / armor this vendor sells
    OpTool = 1u << 9,     // AutoWow.Supply.Outfit: buy the missing gathering tools this vendor sells
    OpRide = 1u << 10,    // AutoWow.Errands.Mounts: learn the run's riding rank at the riding trainer
    OpMount = 1u << 11    // AutoWow.Errands.Mounts: buy (unless held) and learn the run's mount
};

struct Stop
{
    std::uint32_t spawn = 0;
    std::uint32_t entry = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t ops = 0;
    std::uint32_t buyKinds = 0;  // (1 << Kind) bought here
};

inline constexpr std::size_t kMaxStops = 8;

struct Plan
{
    std::array<Stop, kMaxStops> stops{};
    std::uint8_t count = 0;
};

inline constexpr std::size_t kMaxSellerFailures = 3;
struct SellerRetry
{
    std::array<std::uint32_t, kMaxSellerFailures> spawns{};
    std::array<std::uint32_t, kMaxSellerFailures> entries{};
    std::uint8_t count = 0;
};

struct PlanInput
{
    std::uint8_t team = 0;                             // kAlliance | kHorde
    std::array<std::uint32_t, kKinds> buyItems{};      // item per kind to buy (0 = none)
    std::vector<std::uint32_t> trainers;               // trainer spawns with work for the bot, ascending
    bool bind = false;                                 // hearthstone not bound here
    bool learnFp = false;                              // town flight master node unknown
    bool auction = false;                              // AutoWow.Trade: visit the town's auctioneer
    bool mail = false;                                 // AutoWow.Trade: visit the town's mailbox
    std::vector<std::uint32_t> gearNpcs;               // AutoWow.Gear.Upgrades: vendors with a planned buy
    std::uint8_t tools = 0;                            // AutoWow.Supply.Outfit: Tool bits to buy
    bool gearFirst = false;                            // AutoWow.Supply.OutfitGear: a floor weapon is planned
};

// Errand batch in order: sell junk, repair, restock, train, bind, flight path. Operations on the same
// npc merge into its first stop, so stops order by their first operation; npcs ascend by spawn.
[[nodiscard]] inline Plan PlanStops(Town const& town, PlanInput const& in)
{
    Plan plan;
    auto add = [&](Npc const& n, std::uint32_t op, std::uint32_t buyKinds)
    {
        for (std::uint8_t k = 0; k < plan.count; ++k)
            if (plan.stops[k].spawn == n.spawn)
            {
                plan.stops[k].ops |= op;
                plan.stops[k].buyKinds |= buyKinds;
                return;
            }
        if (plan.count == kMaxStops)
            return;
        plan.stops[plan.count++] = Stop{n.spawn, n.entry, n.x, n.y, n.z, op, buyKinds};
    };
    auto usable = [&](Npc const& n) { return (n.teams & in.team) != 0; };
    // Repairer: the first usable one, a vendor-repairer preferred (junk is sold there too).
    Npc const* repairer = nullptr;
    for (Npc const& n : town.npcs)
    {
        if (!usable(n) || !(n.roles & RoleRepair))
            continue;
        if (!repairer || ((n.roles & RoleVendor) && !(repairer->roles & RoleVendor)))
            repairer = &n;
    }
    Npc const* seller = repairer && (repairer->roles & RoleVendor) ? repairer : nullptr;
    for (Npc const& n : town.npcs)
        if (!seller && usable(n) && (n.roles & RoleVendor))
            seller = &n;
    if (seller)
        add(*seller, OpSell, 0);
    if (repairer)
        add(*repairer, OpRepair, 0);
    for (std::size_t k = 0; k < kKinds; ++k)
    {
        std::uint32_t const item = in.buyItems[k];
        if (!item)
            continue;
        for (Npc const& n : town.npcs)
            if (usable(n) && (n.roles & RoleVendor) && std::binary_search(n.items.begin(), n.items.end(), item))
            {
                add(n, OpBuy, 1u << k);
                break;
            }
    }
    // AutoWow.Supply.OutfitGear: a floor weapon's gold (a grant, maybe) is spent before any trainer takes it.
    if (in.gearFirst)
        for (std::uint32_t spawn : in.gearNpcs)
            for (Npc const& n : town.npcs)
                if (n.spawn == spawn && usable(n))
                    add(n, OpGear, 0);
    for (std::uint32_t spawn : in.trainers)
        for (Npc const& n : town.npcs)
            if (n.spawn == spawn && usable(n))
                add(n, OpTrain, 0);
    // AutoWow.Supply.Outfit: after training (a tool of a profession learned there is bought too), per missing
    // tool the first usable vendor selling it.
    for (std::size_t i = 0; i < kTools; ++i)
        if (in.tools & (1u << i))
            for (Npc const& n : town.npcs)
                if (usable(n) && (n.roles & RoleVendor) && (n.tools & (1u << i)))
                {
                    add(n, OpTool, 0);
                    break;
                }
    // AutoWow.Gear.Upgrades: after training (its gold comes first), before bind / flight path.
    for (std::uint32_t spawn : in.gearNpcs)
        for (Npc const& n : town.npcs)
            if (n.spawn == spawn && usable(n))
                add(n, OpGear, 0);
    if (in.bind)
        for (Npc const& n : town.npcs)
            if (n.spawn == town.id)
                add(n, OpBind, 0);
    if (in.learnFp)
        for (Npc const& n : town.npcs)
            if (usable(n) && (n.roles & RoleFlight))
            {
                add(n, OpLearnFp, 0);
                break;
            }
    // AutoWow.Trade: after every other stop, the first usable auctioneer, then the first mailbox.
    if (in.auction)
        for (Npc const& n : town.npcs)
            if (usable(n) && (n.roles & RoleAuction))
            {
                add(n, OpAuction, 0);
                break;
            }
    if (in.mail)
        for (Npc const& n : town.npcs)
            if (usable(n) && (n.roles & RoleMailbox))
            {
                add(n, OpMail, 0);
                break;
            }
    return plan;
}

// Replace a timed-out sell stop in place, retaining later planned services and excluding failed/template clones.
[[nodiscard]] inline Stop const* RetryTimedOutSeller(Town const& town, std::uint8_t team, Plan& plan,
                                                     std::uint8_t stop, SellerRetry& retry)
{
    if (stop >= plan.count || !(plan.stops[stop].ops & OpSell))
        return nullptr;
    Stop const failed = plan.stops[stop];
    bool known = false;
    for (std::uint8_t k = 0; k < retry.count; ++k)
        if (retry.spawns[k] == failed.spawn || retry.entries[k] == failed.entry)
            known = true;
    if (!known && retry.count < kMaxSellerFailures)
    {
        retry.spawns[retry.count] = failed.spawn;
        retry.entries[retry.count] = failed.entry;
        ++retry.count;
    }
    if (retry.count >= kMaxSellerFailures)
        return nullptr;

    Npc const* best = nullptr;
    for (Npc const& n : town.npcs)
    {
        if (!n.spawn || !n.entry || !(n.roles & RoleVendor) || !(n.teams & team))
            continue;
        bool excluded = false;
        for (std::uint8_t k = 0; k < retry.count; ++k)
            if (retry.spawns[k] == n.spawn || retry.entries[k] == n.entry)
                excluded = true;
        for (std::uint8_t k = 0; !excluded && k < plan.count; ++k)
            if (plan.stops[k].spawn == n.spawn || plan.stops[k].entry == n.entry)
                excluded = true;
        if (!excluded && (!best || n.spawn < best->spawn || (n.spawn == best->spawn && n.entry < best->entry)))
            best = &n;
    }
    if (!best)
        return nullptr;
    plan.stops[stop] = Stop{best->spawn, best->entry, best->x, best->y, best->z,
                            OpSell | ((best->roles & RoleRepair) ? OpRepair : 0u), 0};
    return &plan.stops[stop];
}

// ---- AutoWow.Errands.Mounts --------------------------------------------------------------------------
// Cohort bots walked everywhere (S76: 10 of 48 cohort bots at L20+ knew Riding). With the flag a cohort bot that
// reaches a riding tier's level runs to its own race's riding trainer + mount vendor (another race's mounts need
// exalted), learns the rank with its own gold (a house-treasury grant tops a shortfall up, at most the tier's cost),
// then buys and learns the cheapest mount of the rank; the stock `mount` strategy (CheckMountStateAction) rides it.
// World DB checked 2026-09-30: riding trainers teach every race of their faction (trainer.Requirement 0); prices are
// read from the trainer / vendor at run time (Apprentice 4g, Journeyman 50g, Expert 250g; mounts 1g / 10g / 50g).
// Cold Weather Flying (54197: L77, 1000g, Dalaran) is not planned.
struct RidingTier
{
    std::uint32_t spell = 0;
    std::uint32_t level = 0;
    std::uint32_t mountRank = 0;  // item_template.RequiredSkillRank of the tier's mounts
};
inline constexpr std::size_t kRidingTiers = 3;
inline constexpr RidingTier kRiding[kRidingTiers] = {{33388, 20, 75}, {33391, 40, 150}, {34090, 60, 225}};
inline constexpr std::uint32_t kRidingSkill = 762;  // SKILL_RIDING

// A riding trainer + mount vendor pair. races: bit (1 << (race - 1)); tiers: bit (1 << (tier - 1)).
struct MountSite
{
    std::uint32_t races = 0;
    std::uint8_t tiers = 0;
    std::uint32_t trainer = 0;  // creature entry
    std::uint32_t vendor = 0;   // creature entry
};
inline constexpr std::size_t kMountSites = 12;
inline constexpr MountSite kSites[kMountSites] = {
    {1u << 0, 3, 4732, 384},      // Human: Randal Hunter / Katie Hunter, Eastvale Logging Camp (map 0)
    {1u << 1, 3, 4752, 3362},     // Orc: Kildar / Ogunaro Wolfrunner, Orgrimmar (1)
    {1u << 2, 3, 4772, 1261},     // Dwarf: Ultham Ironhorn / Veron Amberstill, Amberstill Ranch (0)
    {1u << 3, 3, 4753, 4730},     // Night Elf: Jartsam / Lelanai, Darnassus (1)
    {1u << 4, 3, 4773, 4731},     // Undead: Velma Warnam / Zachariah Post, Brill (0)
    {1u << 5, 3, 3690, 3685},     // Tauren: Kar Stormsinger / Harb Clawhoof, Bloodhoof Village (1)
    {1u << 6, 3, 7954, 7955},     // Gnome: Binjy / Milli Featherwhistle, Steelgrill's Depot (0)
    {1u << 7, 3, 7953, 7952},     // Troll: Xar'Ti / Zjolnir, Sen'jin Village (1)
    {1u << 9, 3, 16280, 16264},   // Blood Elf: Perascamin / Winaestra, Eversong Woods (530)
    {1u << 10, 3, 20914, 17584},  // Draenei: Aalun / Torallius the Pack Handler, Azuremyst Isle (530)
    {1101, 4, 35100, 35101},      // Alliance expert: Hargen / Grunda Bronzewing, Honor Hold (530)
    {690, 4, 35093, 35099},       // Horde expert: Wind Rider Jahubo / Bana Wildmane, Thrallmar (530)
};

// Site row of a race and tier; kMountSites = none.
[[nodiscard]] inline std::size_t SiteFor(std::uint32_t race, std::uint8_t tier)
{
    if (!race || race > 32 || !tier || tier > kRidingTiers)
        return kMountSites;
    for (std::size_t k = 0; k < kMountSites; ++k)
        if ((kSites[k].races & (1u << (race - 1))) && (kSites[k].tiers & (1u << (tier - 1))))
            return k;
    return kMountSites;
}

// Mount tier of the bot's best known mount: a flying mount 3, a 100% ground mount (speed 99) 2, a 60% one (59) 1.
[[nodiscard]] inline std::uint8_t MountTierOf(std::int32_t groundSpeed, bool flying)
{
    return flying ? 3 : groundSpeed >= 99 ? 2 : groundSpeed >= 59 ? 1 : 0;
}

struct Ride
{
    std::uint8_t tier = 0;  // 0 = nothing due
    bool learn = false;     // learn kRiding[tier - 1].spell
    bool buy = false;       // buy a mount of kRiding[tier - 1].mountRank
};

// The next ride errand: a known rank without its mount buys it; else the next rank once its level is reached (up to
// maxTier), with its mount unless one of that tier is known (a paladin / warlock class mount covers tier 1).
[[nodiscard]] inline Ride NextRide(std::uint32_t level, std::uint8_t knownTier, std::uint8_t mountTier, std::uint32_t maxTier)
{
    if (mountTier < knownTier && knownTier <= maxTier)
        return {knownTier, false, true};
    std::uint32_t const next = knownTier + 1u;
    if (next > maxTier || next > kRidingTiers || level < kRiding[next - 1].level)
        return {};
    return {static_cast<std::uint8_t>(next), true, mountTier < next};
}

struct Afford
{
    bool go = false;
    std::uint64_t grantNeed = 0;  // > 0: ask the treasury (AutoWowSupply::RequestGrant need = money + shortfall)
};

// Own gold after a reserve pays: go. Else, once per window (asked), a grant of the shortfall (reserve included,
// at most the tier's cost); after the ask the bot goes as soon as its money covers the cost itself.
[[nodiscard]] inline Afford MountAfford(std::uint64_t money, std::uint64_t cost, std::uint64_t reserve, bool grantable,
                                        bool asked)
{
    if (money >= cost + reserve || (asked && money >= cost))
        return {true, 0};
    if (!grantable || asked)
        return {};
    return {false, money + std::min(cost, cost + reserve - money)};
}

struct MountOffer
{
    std::uint32_t item = 0;
    std::uint64_t price = 0;  // BuyPrice (before the reputation discount)
    std::uint32_t rank = 0;   // RequiredSkillRank
    std::uint32_t races = 0;  // AllowableRace (-1 = every race)
};

// Cheapest mount of `rank` the race may buy; ties the lower item. nullptr = none.
[[nodiscard]] inline MountOffer const* CheapestMount(std::vector<MountOffer> const& offers, std::uint32_t rank,
                                                     std::uint32_t raceBit)
{
    MountOffer const* best = nullptr;
    for (MountOffer const& o : offers)
        if (o.rank == rank && (o.races & raceBit) &&
            (!best || o.price < best->price || (o.price == best->price && o.item < best->item)))
            best = &o;
    return best;
}

// One site per kSites row (same index): id = the trainer's lowest spawn guid (0 = not spawned), the vendor spawn
// nearest the trainer on its map and the nearest same-team flight master on that map (for the flight leg; the walk
// from it is the tail). Teams: those both npcs serve. Npcs ascend by spawn.
[[nodiscard]] inline std::vector<Town> BuildMountSites(std::vector<Npc> const& npcs)
{
    std::vector<Town> sites(kMountSites);
    for (std::size_t k = 0; k < kMountSites; ++k)
    {
        Npc const* trainer = nullptr;
        for (Npc const& n : npcs)
            if (n.entry == kSites[k].trainer && (!trainer || n.spawn < trainer->spawn))
                trainer = &n;
        if (!trainer)
            continue;
        auto nearest = [&](auto const& match)
        {
            Npc const* best = nullptr;
            std::int64_t bestD2 = 0;
            for (Npc const& n : npcs)
            {
                if (n.map != trainer->map || !match(n))
                    continue;
                std::int64_t const d2 = Dist2(trainer->x, trainer->y, n.x, n.y);
                if (!best || d2 < bestD2 || (d2 == bestD2 && n.spawn < best->spawn))
                {
                    best = &n;
                    bestD2 = d2;
                }
            }
            return best;
        };
        Npc const* vendor = nearest([&](Npc const& n) { return n.entry == kSites[k].vendor; });
        std::uint8_t const teams = vendor ? static_cast<std::uint8_t>(trainer->teams & vendor->teams) : 0;
        if (!teams)
            continue;
        Town& t = sites[k];
        t.id = trainer->spawn;
        t.map = trainer->map;
        t.x = trainer->x;
        t.y = trainer->y;
        t.z = trainer->z;
        t.teams = teams;
        t.npcs = {*trainer, *vendor};
        if (Npc const* fm = nearest([&](Npc const& n) { return (n.roles & RoleFlight) && (n.teams & teams); }))
            t.npcs.push_back(*fm);
        std::sort(t.npcs.begin(), t.npcs.end(), [](Npc const& a, Npc const& b) { return a.spawn < b.spawn; });
    }
    return sites;
}

// The ride stops: the rank at the trainer (the site's own spawn) first, as a mount needs its skill, then the vendor.
[[nodiscard]] inline Plan PlanRide(Town const& site, bool learn, bool buy, std::uint32_t vendorEntry)
{
    Plan plan;
    for (Npc const& n : site.npcs)
        if (learn && n.spawn == site.id)
            plan.stops[plan.count++] = Stop{n.spawn, n.entry, n.x, n.y, n.z, OpRide, 0};
    for (Npc const& n : site.npcs)
        if (buy && n.entry == vendorEntry)
        {
            plan.stops[plan.count++] = Stop{n.spawn, n.entry, n.x, n.y, n.z, OpMount, 0};
            break;
        }
    return plan;
}

// Trailing fields of the ledger `errand` rows riding / mount (AutoWowQuestLedger.h documents them).
inline std::string MountLedgerFields(std::uint32_t site, std::uint8_t tier, char const* key, std::uint32_t id,
                                     std::uint64_t copper, bool learned)
{
    return ",\"site\":" + std::to_string(site) + ",\"tier\":" + std::to_string(tier) + ",\"" + key +
           "\":" + std::to_string(id) + ",\"copper\":" + std::to_string(copper) +
           ",\"learned\":" + (learned ? "true" : "false");
}

// ---- per-bot state -----------------------------------------------------------------------------------
inline constexpr std::size_t kWalkBackoffs = 8;

struct WalkBackoff
{
    std::uint32_t town = 0;          // stable Town::id (innkeeper spawn guid)
    std::uint32_t sourceZone = 0;    // zone where this town walk was admitted / denied
    std::uint32_t needs = 0;         // only overlapping needs are held
    std::uint64_t retryUntilMs = 0;
};

using WalkBackoffTable = std::array<WalkBackoff, kWalkBackoffs>;

[[nodiscard]] inline bool WalkBackoffMatches(WalkBackoff const& b, std::uint32_t town,
                                             std::uint32_t sourceZone, std::uint32_t needs,
                                             std::uint64_t nowMs)
{
    return b.town == town && b.sourceZone == sourceZone && nowMs < b.retryUntilMs && (b.needs & needs) != 0;
}

[[nodiscard]] inline bool TownWalkBackedOff(WalkBackoffTable const& table, std::uint32_t town,
                                            std::uint32_t sourceZone, std::uint32_t needs,
                                            std::uint64_t nowMs)
{
    return std::any_of(table.begin(), table.end(), [&](WalkBackoff const& b)
                       { return WalkBackoffMatches(b, town, sourceZone, needs, nowMs); });
}

// One entry per town/source-zone pair. Repeated failures combine need bits and extend the hold.
// New pairs use the first empty/expired slot, otherwise the entry expiring first (array order breaks ties).
inline void RememberTownWalkFailure(WalkBackoffTable& table, std::uint32_t town,
                                    std::uint32_t sourceZone, std::uint32_t needs,
                                    std::uint64_t nowMs, std::uint64_t retryUntilMs)
{
    if (!town || !needs || retryUntilMs <= nowMs)
        return;

    for (WalkBackoff& b : table)
        if (b.town == town && b.sourceZone == sourceZone)
        {
            if (nowMs < b.retryUntilMs)
            {
                b.needs |= needs;
                b.retryUntilMs = std::max(b.retryUntilMs, retryUntilMs);
            }
            else
                b = {town, sourceZone, needs, retryUntilMs};
            return;
        }

    std::size_t replace = table.size();
    for (std::size_t i = 0; i < table.size(); ++i)
        if (!table[i].town || nowMs >= table[i].retryUntilMs)
        {
            replace = i;
            break;
        }
    if (replace == table.size())
        for (std::size_t i = 0; i < table.size(); ++i)
            if (replace == table.size() || table[i].retryUntilMs < table[replace].retryUntilMs)
                replace = i;

    table[replace] = {town, sourceZone, needs, retryUntilMs};
}

struct WalkGoalKey
{
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

[[nodiscard]] inline bool SameWalkGoal(WalkGoalKey const& a, WalkGoalKey const& b)
{
    return a.map == b.map && a.x == b.x && a.y == b.y && a.z == b.z;
}

// Motion can be retired only when the current MoveFarTo or committed intent still owns this exact town goal.
[[nodiscard]] inline bool OwnsTownWalkGoal(WalkGoalKey const& expected, bool moveFarActive,
                                           WalkGoalKey const& moveFar, bool intentActive,
                                           WalkGoalKey const& intent)
{
    return (moveFarActive || intentActive) &&
           (!moveFarActive || SameWalkGoal(expected, moveFar)) &&
           (!intentActive || SameWalkGoal(expected, intent));
}

enum class Phase : std::uint8_t
{
    None = 0,     // assessing needs
    Travel = 1,   // heading to the town
    Errands = 2,  // walking the stop list
    Return = 3    // heading back to the pre-run position
};

// Wire-stable ledger reasons; append only.
enum class Outcome : std::uint8_t
{
    Done = 0,
    TravelGaveUp = 1,
    ErrandsTimeout = 2,
    ReturnGaveUp = 3
};

inline constexpr char const* OutcomeName(Outcome o)
{
    switch (o)
    {
        case Outcome::Done: return "done";
        case Outcome::TravelGaveUp: return "travel_gave_up";
        case Outcome::ErrandsTimeout: return "errands_timeout";
        case Outcome::ReturnGaveUp: return "return_gave_up";
    }
    return "done";
}

[[nodiscard]] inline bool ShouldRememberTownWalkFailure(Outcome outcome, bool townWalkAttempted)
{
    return outcome == Outcome::TravelGaveUp && townWalkAttempted;
}

struct BotState
{
    std::uint8_t version = kStateVersion;
    Phase phase = Phase::None;
    std::uint64_t nextCheckMs = 0;
    std::uint64_t cooldownUntilMs = 0;
    std::uint32_t lastClassTrainLevel = 0;  // survives runs (not restarts)
    WalkBackoffTable walkBackoffs{};        // survives runs, process-memory only
    // run
    std::uint32_t town = 0;                 // Town::id
    std::uint32_t needs = 0;
    std::uint32_t done = 0;
    Leg leg = Leg::None;                    // current leg (travel or return)
    Leg travelLeg = Leg::None;              // last leg that moved the bot toward the town
    bool legIssued = false;                 // flight handed to the flight status / hearth cast requested
    bool hearthUsed = false;
    bool walkAdmitted = false;              // route handoff checked for the current town-bound walk leg
    bool townWalkAttempted = false;          // a town-bound WalkLeg was called in this run
    std::uint32_t walkSourceZone = 0;        // captured before that walk moves across a zone boundary
    std::uint32_t reissues = 0;
    std::uint64_t startMs = 0;
    std::uint64_t phaseMs = 0;              // current phase start
    std::uint64_t legMs = 0;                // current leg / stop start
    std::uint64_t travelMs = 0;
    std::uint32_t backMap = 0;              // pre-run position
    std::int32_t backX = 0;
    std::int32_t backY = 0;
    std::int32_t backZ = 0;
    std::uint32_t durBefore = 0;
    std::uint32_t durAfter = 0;
    std::uint32_t bagFreeBefore = 0;
    std::uint32_t bagFreeAfter = 0;
    std::uint64_t spent = 0;                // copper debited by repair / purchases / training
    std::uint64_t sold = 0;                 // copper credited by selling
    Plan plan;
    std::uint8_t stop = 0;
    SellerRetry sellerRetry;
    std::array<std::uint32_t, kKinds> buyItems{};
    Outcome outcome = Outcome::Done;
    // AutoWow.Survival.KeepConsumables: passing-vendor grey sale (Phase::None only).
    std::uint64_t sellUntilMs = 0;          // current detour deadline (0 = none)
    std::uint64_t sellRetryMs = 0;          // no detour before this
    // AutoWow.Travel.Safe: the run's one rescue leg is spent.
    bool rescued = false;
    // AutoWow.Gear.Upgrades: level of the last gear shopping (survives runs, not restarts) and this run's
    // planned buys (item / vendor spawn pairs, 0 = none).
    std::uint32_t lastGearLevel = 0;
    std::array<std::uint32_t, AutoWowGear::kMaxPicks> gearItems{};
    std::array<std::uint32_t, AutoWowGear::kMaxPicks> gearNpcs{};
    std::uint8_t floorGear = 0;  // AutoWow.Supply.OutfitGear: bit k = gearItems[k] is a floor weapon (no reserve)
    // AutoWow.Supply.Outfit: no tool-triggered run before this (survives runs, not restarts).
    std::uint64_t nextOutfitMs = 0;
    // AutoWow.Supply.MailPickup: no mail-triggered run before this (survives runs, not restarts).
    std::uint64_t nextMailMs = 0;
    // AutoWow.Professions.TrainRuns: no train-triggered run before this (survives runs, not restarts).
    std::uint64_t nextTrainRunMs = 0;
    // AutoWow.Gear.AuctionUpgrades: level of the last auction-run due check (survives runs, not restarts) and this
    // run's auction gear purchases (item entries queued at the auctioneer, 0 = none), equipped at the errands' end.
    std::uint32_t lastAhGearLevel = 0;
    std::array<std::uint32_t, AutoWowGear::kAhMaxBuys> ahGearItems{};
    // AutoWow.Errands.Mounts: no mount decision before this (survives runs, not restarts), the last grant request
    // (0 = none; cleared by a run) and this run's plan (rideTier 0 = a town run).
    std::uint64_t nextMountMs = 0;
    std::uint64_t mountGrantMs = 0;
    std::uint8_t rideTier = 0;
    bool rideLearn = false;
    std::uint32_t mountItem = 0;  // 0 = no mount to buy
};

// Travel / return leg exhausted: past its timeout or out of reissues.
[[nodiscard]] inline bool LegExhausted(Params const& p, BotState const& s, std::uint64_t nowMs, std::uint32_t timeoutMs)
{
    return (nowMs >= s.phaseMs && nowMs - s.phaseMs > timeoutMs) || s.reissues > p.maxReissues;
}

// AutoWow.Travel.Safe: the rescue leg of an exhausted travel phase, once per run. soak-s16-full-r1: all 15
// travel_gave_up runs were walks that never got going (7 intent_replan_exhausted give-ups from a spot the
// navmesh routes nothing out of: 11 frozen in place, 4 circling a local minimum); the same bots blocked
// every quest walk too. A hearthstone bound at a town serving the run leaves that spot legitimately;
// else a known flight. `failed` = the leg that was exhausted (never retried as its own rescue).
[[nodiscard]] inline Leg RescueLeg(bool rescued, Leg failed, bool hearthToServingTown, bool flight)
{
    if (rescued)
        return Leg::None;
    if (hearthToServingTown && failed != Leg::Hearth)
        return Leg::Hearth;
    if (flight && failed != Leg::Flight)
        return Leg::Flight;
    return Leg::None;
}

// State after a finished run: cooldown, keep the class-trainer level.
[[nodiscard]] inline BotState AfterRun(Params const& p, BotState const& s, std::uint64_t nowMs)
{
    BotState next;
    next.lastClassTrainLevel = s.lastClassTrainLevel;
    next.walkBackoffs = s.walkBackoffs;
    next.lastGearLevel = s.lastGearLevel;
    next.nextOutfitMs = s.nextOutfitMs;
    next.nextMailMs = s.nextMailMs;
    next.nextTrainRunMs = s.nextTrainRunMs;
    next.lastAhGearLevel = s.lastAhGearLevel;
    next.nextMountMs = s.nextMountMs;
    next.cooldownUntilMs = nowMs + p.cooldownMs;
    next.nextCheckMs = nowMs + p.checkIntervalMs;
    return next;
}

// Trailing fields of the ledger `errand` line (AutoWowQuestLedger.h documents them).
inline std::string LedgerFields(BotState const& s, std::uint32_t townZone, std::uint64_t returnMs)
{
    return ",\"town\":" + std::to_string(s.town) + ",\"town_zone\":" + std::to_string(townZone) +
           ",\"needs\":" + std::to_string(s.needs) + ",\"done\":" + std::to_string(s.done) +
           ",\"spent\":" + std::to_string(s.spent) + ",\"sold\":" + std::to_string(s.sold) +
           ",\"dur0\":" + std::to_string(s.durBefore) + ",\"dur1\":" + std::to_string(s.durAfter) +
           ",\"bag0\":" + std::to_string(s.bagFreeBefore) + ",\"bag1\":" + std::to_string(s.bagFreeAfter) +
           ",\"travel_ms\":" + std::to_string(s.travelMs) + ",\"return_ms\":" + std::to_string(returnMs) +
           ",\"leg\":\"" + LegName(s.travelLeg) + "\",\"hearth\":" + (s.hearthUsed ? "true" : "false");
}

// ---- runtime (NewRpgErrands.cpp) -------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
inline std::vector<Town> gTowns;  // built once at world init with the flag on; read-only afterwards
inline std::vector<Town> gMountSites;  // AutoWow.Errands.Mounts: kSites order (BuildMountSites), same lifetime
}
inline bool Enabled() { return detail::gEnabled; }

void LoadConfig();
// A run (travel, errands or return) is under way for this bot. Zone progression waits for it.
bool Active(std::uint32_t guid);
}  // namespace AutoWowErrands

#endif  // AUTOWOW_ERRANDS_POLICY_H
