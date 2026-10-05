/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_GEAR_UPGRADE_POLICY_H
#define AUTOWOW_GEAR_UPGRADE_POLICY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// AutoWow.Gear.Upgrades (default 0; the vendor part needs AutoWow.Errands.Enable). soak-s21-full-r1
// (LOWMOB_DEATHS_S21 cause 1): avg equipped ilvl 5.5 at L9-24, rogues at L20 on 1-2 DPS starter daggers,
// 8.2 of 19 slots worn. The stock equip pipeline works (22 loot/quest equips logged in the run); the supply
// is missing: 41 of 46 rewarded quests carry no item and none a weapon choice. With the flag on:
//   - an errand town run also shops at the town's weapon/armor vendors (real gold): the best affordable
//     class-appropriate main hand (and off hand for dual wielders) whose DPS is >= UpgradePct of the
//     equipped one, then cheap armor for EMPTY slots (each piece <= ArmorSpendPct of what is left), always
//     keeping the class-trainer budget plus a food reserve; the stock equip action puts them on.
//   - gear is a soft errand need once per level with MinBudget gold in hand, urgent when the main hand's
//     DPS is below FarBelowPct of the level expectation.
//   - a quest choice reward with no equip upgrade among the choices takes the one that sells for most
//     (gold for the vendor weapon) instead of index 0.
// Each purchase logs "[Gear]"; a run that bought gear sets the errand `done` bit 128 (DoneGeared).
//
// Value-only, integer copper / milli-DPS, no floats in decisions, no RNG; ties break on lower price, then
// lower item id, then lower npc spawn.
namespace AutoWowGear
{
inline constexpr std::uint8_t kPolicyVersion = 1;

// AutoWow.Gear.CatchUp (default 0): forward flag so the auction catch-up helpers below (ExpectedIlvlEff, AhPlan) can
// read it. Set by AutoWowErrands::LoadConfig; the rationale sits with ExpectedIlvlCatchUp and AhPlan below.
namespace detail
{
inline bool gCatchUpEnabled = false;
}  // namespace detail
inline bool CatchUpEnabled() { return detail::gCatchUpEnabled; }

struct Params
{
    std::uint32_t upgradePct = 150;    // AutoWow.Gear.UpgradePct: a vendor weapon must reach this % of the current DPS
    std::uint32_t farBelowPct = 50;    // AutoWow.Gear.FarBelowPct: main hand below this % of expected = urgent run
    std::uint32_t armorSpendPct = 25;  // AutoWow.Gear.ArmorSpendPct: one armor piece <= this % of what is left
    std::uint32_t maxArmorBuys = 4;    // AutoWow.Gear.MaxArmorBuys: armor pieces per run
};

inline constexpr std::uint8_t kSlotMainHand = 15;  // EQUIPMENT_SLOT_MAINHAND
inline constexpr std::uint8_t kSlotOffHand = 16;   // EQUIPMENT_SLOT_OFFHAND

// Weapon DPS x1000 from integer damage (the adapter truncates the template floats) and delay ms.
[[nodiscard]] inline std::uint32_t DpsMilli(std::uint32_t dmgMin, std::uint32_t dmgMax, std::uint32_t delayMs)
{
    return delayMs ? static_cast<std::uint32_t>((std::uint64_t(dmgMin) + dmgMax) * 500000 / delayMs) : 0;
}

// Main-hand DPS a level should carry. World DB 2026-09-24, vendor weapons by RequiredLevel (avg / max DPS):
// L10 8.2 / 10.1, L20 14.1 / 17.6, L30 21 / 27, L40 32.7 / 34.7; 0.7 x level sits on that curve.
[[nodiscard]] inline std::uint32_t ExpectedDpsMilli(std::uint32_t level) { return level * 700; }

[[nodiscard]] inline bool FarBelow(Params const& p, std::uint32_t curMilli, std::uint32_t level)
{
    return std::uint64_t(curMilli) * 100 < std::uint64_t(ExpectedDpsMilli(level)) * p.farBelowPct;
}

// A candidate weapon is worth buying: >= UpgradePct of the equipped DPS (any DPS beats an empty hand).
[[nodiscard]] inline bool Beats(Params const& p, std::uint32_t candMilli, std::uint32_t curMilli)
{
    return candMilli > curMilli && std::uint64_t(candMilli) * 100 >= std::uint64_t(curMilli) * p.upgradePct;
}

// Gold kept back from gear: one class-trainer rank (ErrandsPolicy ClassTrainBudgetCopper, level^2 x 5)
// plus a food reserve of 1 silver per level.
[[nodiscard]] inline std::uint64_t ReserveCopper(std::uint32_t level)
{
    return std::uint64_t(level) * level * 5 + std::uint64_t(level) * 100;
}

// Gold that makes a gear trip worth it (vendor weapons, world DB: L10 20-44s, L20 58s-1g01s).
// ponytail: level^2 x 5 on top of the reserve (x 20 gated 48/50 cohort bots out in soak-s26-full-r1, mean
// purse 18.8s at L18); the vendor list at the town still bounds what is bought.
[[nodiscard]] inline std::uint64_t MinBudgetCopper(std::uint32_t level) { return std::uint64_t(level) * level * 5; }

[[nodiscard]] inline std::uint64_t Spendable(std::uint64_t money, std::uint32_t level)
{
    std::uint64_t const keep = ReserveCopper(level);
    return money > keep ? money - keep : 0;
}

// Soft need once per level with the budget in hand; urgent when the main hand is far below expectation.
struct Due
{
    bool soft = false;
    bool urgent = false;
};

[[nodiscard]] inline Due GearDue(Params const& p, std::uint32_t level, std::uint32_t lastGearLevel,
                                 std::uint64_t money, std::uint32_t mainHandMilli)
{
    Due d;
    d.soft = level > lastGearLevel && Spendable(money, level) >= MinBudgetCopper(level);
    d.urgent = d.soft && FarBelow(p, mainHandMilli, level);
    return d;
}

// One vendor offer the bot may use in `slot` (EQUIPMENT_SLOT_*): the adapter admits only items the stock
// "item upgrade" value rates an equip for this bot (class / spec weights, proficiency, level).
struct Offer
{
    std::uint32_t item = 0;
    std::uint32_t npc = 0;       // vendor spawn guid
    std::uint64_t price = 0;     // BuyPrice copper (before the reputation discount: an upper bound)
    std::uint32_t dpsMilli = 0;  // weapons
    std::uint32_t armor = 0;     // armor pieces
    std::uint8_t slot = 0;
    bool weapon = false;
    bool twoHand = false;  // a two-hander in the main hand leaves no off hand to shop for
};

inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Lower (price, item, npc) wins a tie on the primary key.
[[nodiscard]] inline bool Cheaper(Offer const& a, Offer const& b)
{
    if (a.price != b.price)
        return a.price < b.price;
    if (a.item != b.item)
        return a.item < b.item;
    return a.npc < b.npc;
}

// Highest-DPS affordable weapon for `slot` that beats curMilli.
[[nodiscard]] inline std::size_t PickWeapon(Params const& p, std::vector<Offer> const& offers, std::uint8_t slot,
                                            std::uint32_t curMilli, std::uint64_t budget)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        Offer const& o = offers[i];
        if (!o.weapon || o.slot != slot || o.price > budget || !Beats(p, o.dpsMilli, curMilli))
            continue;
        if (best == kNone || o.dpsMilli > offers[best].dpsMilli ||
            (o.dpsMilli == offers[best].dpsMilli && Cheaper(o, offers[best])))
            best = i;
    }
    return best;
}

// Most armor within budget for an empty `slot`.
[[nodiscard]] inline std::size_t PickArmor(std::vector<Offer> const& offers, std::uint8_t slot, std::uint64_t budget)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        Offer const& o = offers[i];
        if (o.weapon || o.slot != slot || o.price > budget)
            continue;
        if (best == kNone || o.armor > offers[best].armor || (o.armor == offers[best].armor && Cheaper(o, offers[best])))
            best = i;
    }
    return best;
}

inline constexpr std::size_t kMaxPicks = 6;  // main hand, off hand, up to four armor pieces

// The run's shopping list, in buy order: main hand (whole spendable), off hand when dual wielding (what is
// left), then armor for the empty slots in ascending slot order, each <= ArmorSpendPct of what is left.
// emptySlots: bit per EQUIPMENT_SLOT_* that is empty and may take vendor armor.
[[nodiscard]] inline std::vector<Offer> ShoppingList(Params const& p, std::vector<Offer> const& offers,
                                                     std::uint32_t mainHandMilli, std::uint32_t offHandMilli,
                                                     bool dualWield, std::uint32_t emptySlots, std::uint64_t spendable)
{
    std::vector<Offer> out;
    std::uint64_t left = spendable;
    auto take = [&](std::size_t i)
    {
        if (i == kNone)
            return;
        out.push_back(offers[i]);
        left -= offers[i].price;
    };
    take(PickWeapon(p, offers, kSlotMainHand, mainHandMilli, left));
    if (dualWield && (out.empty() || !out.back().twoHand))
        take(PickWeapon(p, offers, kSlotOffHand, offHandMilli, left));
    std::uint32_t armorBuys = 0;
    for (std::uint8_t slot = 0; slot < 32 && armorBuys < p.maxArmorBuys && out.size() < kMaxPicks; ++slot)
    {
        if (!(emptySlots & (1u << slot)))
            continue;
        std::size_t const a = PickArmor(offers, slot, left * p.armorSpendPct / 100);
        if (a != kNone)
        {
            take(a);
            ++armorBuys;
        }
    }
    return out;
}

// ---- AutoWow.Supply.OutfitGear weapon floor (default 0; needs Gear.Upgrades + Supply.Outfit) ---------------
// S62-S65 (1190 cohort deaths, 3.55 / bot-h): rogues 9.5 / bot-h, 4 of 7 on starter weapons (0.9-2.1 DPS) at
// L27-43 with 0.4 g; 48 errands had gear due, 2 bought (the reserve left no budget). A main hand (a hunter's
// ranged too) under FarBelowPct of ExpectedDpsMilli (the world DB vendor-weapon curve) is an urgent gear need once
// per level whatever the purse; its buy skips the trainer / food reserve, and the house treasury grants the part
// the bot's own gold cannot pay (up to the grant room). Offers are the stock "item upgrade" scorer's (GearOffers).
inline constexpr std::uint8_t kSlotRanged = 17;  // EQUIPMENT_SLOT_RANGED

[[nodiscard]] inline bool FloorDue(Params const& p, std::uint32_t level, std::uint32_t lastGearLevel,
                                   std::uint32_t mainHandMilli, bool hunter, std::uint32_t rangedMilli)
{
    return level > lastGearLevel &&
           (FarBelow(p, mainHandMilli, level) || (hunter && FarBelow(p, rangedMilli, level)));
}

// Floor weapon for `slot`: the best upgrade the bot's own gold buys when it clears the floor; else the cheapest
// upgrade clearing it within money + grantRoom (the treasury pays the rest); else the best upgrade within
// money + grantRoom (all below the floor: still the most DPS for the gold).
[[nodiscard]] inline std::size_t PickFloorWeapon(Params const& p, std::vector<Offer> const& offers, std::uint8_t slot,
                                                 std::uint32_t curMilli, std::uint32_t level, std::uint64_t money,
                                                 std::uint64_t grantRoom)
{
    std::size_t const own = PickWeapon(p, offers, slot, curMilli, money);
    if (own != kNone && !FarBelow(p, offers[own].dpsMilli, level))
        return own;
    std::size_t best = kNone;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        Offer const& o = offers[i];
        if (!o.weapon || o.slot != slot || o.price > money + grantRoom || !Beats(p, o.dpsMilli, curMilli) ||
            FarBelow(p, o.dpsMilli, level))
            continue;
        if (best == kNone || Cheaper(o, offers[best]))
            best = i;
    }
    return best != kNone ? best : PickWeapon(p, offers, slot, curMilli, money + grantRoom);
}

// The run's floor weapons in buy order (main hand, then a hunter's ranged), each only for a slot under the floor;
// money then grantRoom are drawn down in turn.
[[nodiscard]] inline std::vector<Offer> FloorWeapons(Params const& p, std::vector<Offer> const& offers,
                                                     std::uint32_t level, std::uint32_t mainHandMilli, bool hunter,
                                                     std::uint32_t rangedMilli, std::uint64_t money,
                                                     std::uint64_t grantRoom)
{
    std::vector<Offer> out;
    auto pick = [&](std::uint8_t slot, std::uint32_t cur)
    {
        if (!FarBelow(p, cur, level))
            return;
        std::size_t const i = PickFloorWeapon(p, offers, slot, cur, level, money, grantRoom);
        if (i == kNone)
            return;
        out.push_back(offers[i]);
        std::uint64_t const own = std::min(money, offers[i].price);
        money -= own;
        grantRoom -= offers[i].price - own;
    };
    pick(kSlotMainHand, mainHandMilli);
    if (hunter)
        pick(kSlotRanged, rangedMilli);
    return out;
}

// Quest choice reward when none is an equip upgrade: the one with the highest vendor price (ties: lower
// index). sellPrices[k] = SellPrice of choice k.
[[nodiscard]] inline std::size_t MostValuableChoice(std::vector<std::uint32_t> const& sellPrices)
{
    std::size_t best = 0;
    for (std::size_t k = 1; k < sellPrices.size(); ++k)
        if (sellPrices[k] > sellPrices[best])
            best = k;
    return best;
}

// ---- AutoWow.Gear.AuctionUpgrades (default 0; needs AutoWow.Trade.Enable + AutoWow.Errands.Enable) ----------
// Owner 2026-09-28 "they are dying because they are low geared": S65 cohort (avg L40) wore avg ilvl ~29 at L42,
// 22-55% of pieces grey / white, 3.5 deaths / bot-h; gear came from loot / quest equips and 6 vendor errands (white
// items only); 50 errand auctioneer visits bought 3 pieces (TradePolicy PlanBuys: the one cheapest upgrade). Purses
// are skewed (avg 12.7 g, max 173 g). With the flag on the auctioneer stop instead buys up to MaxBuys buyout
// listings the stock "item upgrade" scorer rates an equip (EQUIP / REPLACE) that raise the item level of the slot
// (FindEquipSlot), slot by slot in AhSlotOrder (main hand, a hunter's ranged, off hand, chest, legs, ...), per slot
// the most ilvl gain per copper, each <= level^2 x PriceMult copper, all within Spendable (trainer rank + food) less
// the repair bill; the mail stop brings them and the errand's end runs the stock equip action. A bot whose average
// equipped ilvl sits under IlvlPct of ExpectedIlvl with MinSpendPct of one item cap to spend makes an auction-town
// run once per level (ErrandsPolicy NeedAhGear). Logs "[AhGear]"; trade ledger reason ah_gear (+ gain).
struct AhParams
{
    std::uint32_t maxBuys = 3;       // AutoWow.Gear.AuctionMaxBuys (<= kAhMaxBuys): purchases per auctioneer visit
    std::uint32_t priceMult = 20;    // AutoWow.Gear.AuctionPriceMult: one item <= level^2 x this copper
    std::uint32_t ilvlPct = 75;      // AutoWow.Gear.AuctionIlvlPct: avg ilvl under this % of ExpectedIlvl = run due
    std::uint32_t minSpendPct = 50;  // AutoWow.Gear.AuctionMinSpendPct: ... with AhBudget >= this % of AhItemCap
};

inline constexpr std::size_t kAhMaxBuys = 4;

// A green of RequiredLevel L carries ilvl ~L+5 (world DB AH census 2026-09-28: req 35 -> 40, 45 -> 50, 49 -> 54).
[[nodiscard]] inline std::uint32_t ExpectedIlvl(std::uint32_t level) { return level + 5; }

// AutoWow.Gear.CatchUp: the real per-band GREEN item-level curve (mirrors AutoWowNoWhite::GreenIlvl; kept local to
// avoid a header cycle, NoWhitePolicy.h already includes this file). level + 5 only holds to ~L55; above it the
// stock ExpectedIlvl undershoots real content ~2x (live world DB 2026-10-05: green ilvl 133 at L70, 162 at L75,
// 174 at L78), so the catch-up run never fired for the Outland / Northrend band (see the AuctionUpgrades notes).
[[nodiscard]] inline std::uint32_t ExpectedIlvlCatchUp(std::uint32_t level)
{
    if (level <= 57)
        return level + 5;
    if (level <= 70)
        return 62 + 9 * (level - 58) / 2;  // Outland greens: 62 + 4.5 x (L - 58), truncated
    return 130 + 63 * (level - 71) / 10;   // Northrend greens: 130 + 6.3 x (L - 71), truncated
}

// The ilvl expectation the catch-up run-due test uses: the real green curve with AutoWow.Gear.CatchUp, else the
// stock level + 5 (OFF: byte-identical).
[[nodiscard]] inline std::uint32_t ExpectedIlvlEff(std::uint32_t level)
{
    return CatchUpEnabled() ? ExpectedIlvlCatchUp(level) : ExpectedIlvl(level);
}

// Average equipped ilvl (integer; empty slots count 0) under IlvlPct of the curve.
[[nodiscard]] inline bool IlvlFarBelow(AhParams const& ap, std::uint32_t avgIlvl, std::uint32_t level)
{
    return std::uint64_t(avgIlvl) * 100 < std::uint64_t(ExpectedIlvlEff(level)) * ap.ilvlPct;
}

[[nodiscard]] inline std::uint64_t AhItemCap(AhParams const& ap, std::uint32_t level)
{
    return std::uint64_t(level) * level * ap.priceMult;
}

// Copper the auction gear may take: Spendable (trainer rank + food kept) of the money less the repair bill.
[[nodiscard]] inline std::uint64_t AhBudget(std::uint64_t money, std::uint32_t level, std::uint64_t repairCopper)
{
    return Spendable(money > repairCopper ? money - repairCopper : 0, level);
}

// An auction-town run: once per level (lastAhLevel = level of the last due check), gear far under the curve and
// MinSpendPct of one item cap to spend.
[[nodiscard]] inline bool AhRunDue(AhParams const& ap, std::uint32_t level, std::uint32_t lastAhLevel,
                                   std::uint64_t money, std::uint64_t repairCopper, std::uint32_t avgIlvl)
{
    return level > lastAhLevel && IlvlFarBelow(ap, avgIlvl, level) &&
           AhBudget(money, level, repairCopper) * 100 >= AhItemCap(ap, level) * ap.minSpendPct;
}

// One buyout listing the bot may equip, with its ilvl gain over the piece worn in `slot` (EQUIPMENT_SLOT_*).
struct AhOffer
{
    std::uint32_t id = 0;     // auction id: stable, never reused
    std::uint32_t item = 0;
    std::uint64_t price = 0;  // buyout copper
    std::uint32_t gain = 0;   // ItemLevel - worn ItemLevel (empty slot: ItemLevel)
    std::uint8_t slot = 0;
    bool twoHand = false;     // a two-hander bought for the main hand leaves the off hand out
    std::uint8_t quality = 0; // ItemTemplate Quality (AutoWow.Gear.NoWhite drops grey / white weapons)
};

// Slot buy order: weapons (main hand, a hunter's ranged, off hand), chest, legs, head, shoulders, hands, feet,
// waist, wrists, back, a non-hunter's ranged (wand / thrown), neck, rings, trinkets. Shirt / tabard never.
inline constexpr std::array<std::uint8_t, 17> kAhOrderHunter = {15, 17, 16, 4, 6, 0, 2, 9, 7, 5, 8, 14, 1, 10, 11, 12, 13};
inline constexpr std::array<std::uint8_t, 17> kAhOrderOther = {15, 16, 4, 6, 0, 2, 9, 7, 5, 8, 14, 17, 1, 10, 11, 12, 13};

// Same slot: more gain per copper (cross-multiplied, no floats), then more gain, then cheaper, then lower id.
[[nodiscard]] inline bool AhBetter(AhOffer const& a, AhOffer const& b)
{
    std::uint64_t const l = std::uint64_t(a.gain) * b.price, r = std::uint64_t(b.gain) * a.price;
    if (l != r)
        return l > r;
    if (a.gain != b.gain)
        return a.gain > b.gain;
    if (a.price != b.price)
        return a.price < b.price;
    return a.id < b.id;
}

// The best offer a slot has within `cap` (AhBetter), or kNone. AutoWow.Gear.CatchUp uses it to rank slots.
[[nodiscard]] inline std::size_t BestOfferForSlot(std::vector<AhOffer> const& offers, std::uint8_t slot,
                                                  std::uint64_t cap)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        AhOffer const& o = offers[i];
        if (o.slot != slot || !o.gain || !o.price || o.price > cap)
            continue;
        if (best == kNone || AhBetter(o, offers[best]))
            best = i;
    }
    return best;
}

// The visit's purchases in buy order: per slot of the order the AhBetter-best offer with a gain, <= AhItemCap and
// <= what is left of `budget`; at most MaxBuys (<= kAhMaxBuys). AutoWow.Gear.CatchUp keeps the weapon slots first
// (the two-hander / off-hand rule depends on main hand before off hand) but reorders the armor + accessory tail by
// each slot's best offer, so an empty trinket / ring / neck / back (its full-ilvl gain ranks high) is reached before
// the cap runs out instead of being walked last. OFF: the fixed order, byte-identical.
[[nodiscard]] inline std::vector<AhOffer> AhPlan(AhParams const& ap, std::vector<AhOffer> const& offers,
                                                 std::uint32_t level, bool hunter, std::uint64_t budget)
{
    std::uint64_t const cap = AhItemCap(ap, level);
    std::size_t const maxBuys = std::min<std::size_t>(ap.maxBuys, kAhMaxBuys);
    std::array<std::uint8_t, 17> const& order = hunter ? kAhOrderHunter : kAhOrderOther;
    std::vector<std::uint8_t> seq(order.begin(), order.end());
    if (CatchUpEnabled())
    {
        std::size_t const weapons = hunter ? 3 : 2;  // {15,17,16} / {15,16}: keep ahead of the sorted tail
        std::stable_sort(seq.begin() + weapons, seq.end(),
                         [&](std::uint8_t a, std::uint8_t b)
                         {
                             std::size_t const ia = BestOfferForSlot(offers, a, cap);
                             std::size_t const ib = BestOfferForSlot(offers, b, cap);
                             if (ia == kNone || ib == kNone)
                                 return ia != kNone && ib == kNone;  // slots with an offer first
                             return AhBetter(offers[ia], offers[ib]);
                         });
    }
    std::vector<AhOffer> out;
    bool twoHander = false;
    for (std::uint8_t const slot : seq)
    {
        if (out.size() >= maxBuys)
            break;
        if (slot == kSlotOffHand && twoHander)
            continue;
        std::size_t best = kNone;
        for (std::size_t i = 0; i < offers.size(); ++i)
        {
            AhOffer const& o = offers[i];
            if (o.slot != slot || !o.gain || !o.price || o.price > cap || o.price > budget)
                continue;
            if (best == kNone || AhBetter(o, offers[best]))
                best = i;
        }
        if (best == kNone)
            continue;
        out.push_back(offers[best]);
        budget -= offers[best].price;
        twoHander = twoHander || (slot == kSlotMainHand && offers[best].twoHand);
    }
    return out;
}

// ---- AutoWow.Gear.Flow (default 0) --------------------------------------------------------------------------------
// Lane AQ gearflow audit (soak-s65/s66-full-r1, cohort 62955-63004 avg L42): worn avg ilvl 30.2, 11.2 of 17 slots
// filled, 38% of worn pieces grey / white. The own-upgrade pipeline holds (stock periodic bag scan: 13 of 84 evaluated
// pieces never worn, mostly bags; 7 of 7 quest gear rewards worn; 0 cohort AH posts / house donations of gear). Leaks:
//   1. crafter jam: a Weavers artisan (a cohort bot) wears its own products; a worn-then-replaced piece is soulbound,
//      yet the gear line keeps planning it (lowest guid first): 1508 / 1299 refused gear-line rows (624 / 458
//      `bad_item` mails) per soak; the artisan -> rep mail (all pieces in one) and the consumer the bound piece was
//      planned for starve for good.
//   2. hand-me-downs: 28 of the 40 green+ weapon / armor pieces in cohort bags at the end of s66 are BoE upgrades
//      (ilvl, armor type, proficiency) for a same-faction cohort member; the looter's errand vendors them (usage AH).
// With the flag on: gear lines deliver tradeable pieces only; a world-thread pass every FlowTickMs mails each loose,
// tradeable weapon / armor piece of FlowMinQuality+ that is no upgrade for its holder (stock "item upgrade" scorer)
// to the same-faction gear recipient it upgrades most (ilvl gain; the scorer confirms), at most FlowMaxMails per pass,
// one per recipient; the recipient's mail run takes it (HasSupplyMail) and the stock bag scan wears it. Logs
// "[GearFlow]".
struct FlowParams
{
    std::uint32_t tickMs = 60000;   // AutoWow.Gear.FlowTickMs
    std::uint32_t maxMails = 10;    // AutoWow.Gear.FlowMaxMails: hand-me-down mails per pass (both factions)
    std::uint32_t minQuality = 1;   // AutoWow.Gear.FlowMinQuality: ITEM_QUALITY_NORMAL (white) and up
};

inline constexpr char kFlowSubject[] = "AutoWoW gear flow";

// A loose piece that may leave its holder: gear of minQuality+, tradeable, no upgrade for the holder.
[[nodiscard]] inline bool Flows(FlowParams const& fp, std::uint32_t quality, bool gear, bool tradeable,
                                bool holderUpgrade)
{
    return gear && tradeable && !holderUpgrade && quality >= fp.minQuality;
}

// One member the piece upgrades, by item level over what it wears in that slot (empty = 0).
struct FlowTaker
{
    std::uint32_t guid = 0;
    std::uint32_t gain = 0;
};

// Most gain, then lower guid; kNone when no taker gains.
[[nodiscard]] inline std::size_t PickTaker(std::vector<FlowTaker> const& takers)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < takers.size(); ++i)
        if (takers[i].gain && (best == kNone || takers[i].gain > takers[best].gain ||
                               (takers[i].gain == takers[best].gain && takers[i].guid < takers[best].guid)))
            best = i;
    return best;
}

// ---- runtime (flag + params; read by AutoWowErrands::LoadConfig, used by NewRpgErrands.cpp and
// NewRpgBaseAction::BestRewardIndex; the auction part by AutoWowTrade.cpp; the flow by AutoWowSupply.cpp) -----------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
inline bool gAuctionEnabled = false;
inline AhParams gAhParams;
inline bool gFlowEnabled = false;
inline FlowParams gFlowParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
inline Params const& Get() { return detail::gParams; }
inline bool AuctionEnabled() { return detail::gAuctionEnabled; }
inline AhParams const& GetAh() { return detail::gAhParams; }
inline bool FlowEnabled() { return detail::gFlowEnabled; }
inline FlowParams const& GetFlow() { return detail::gFlowParams; }
}  // namespace AutoWowGear

#endif  // AUTOWOW_GEAR_UPGRADE_POLICY_H
