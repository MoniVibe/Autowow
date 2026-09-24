/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_GEAR_UPGRADE_POLICY_H
#define AUTOWOW_GEAR_UPGRADE_POLICY_H

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
// ponytail: level^2 x 20 on top of the reserve; a real ask comes from the vendor list at the town.
[[nodiscard]] inline std::uint64_t MinBudgetCopper(std::uint32_t level) { return std::uint64_t(level) * level * 20; }

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

// ---- runtime (flag + params; read by AutoWowErrands::LoadConfig, used by NewRpgErrands.cpp and
// NewRpgBaseAction::BestRewardIndex) ------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
inline Params const& Get() { return detail::gParams; }
}  // namespace AutoWowGear

#endif  // AUTOWOW_GEAR_UPGRADE_POLICY_H
