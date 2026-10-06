/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GearUpgradePolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowGear;

// RAII toggle for the catch-up flag so a failing case cannot leak into the next.
struct CatchUpScope
{
    explicit CatchUpScope(bool on) { detail::gCatchUpEnabled = on; }
    ~CatchUpScope() { detail::gCatchUpEnabled = false; }
};

AhOffer O(std::uint32_t id, std::uint8_t slot, std::uint32_t gain, std::uint64_t price, bool twoHand = false)
{
    AhOffer o;
    o.id = id;
    o.item = id;
    o.slot = slot;
    o.gain = gain;
    o.price = price;
    o.twoHand = twoHand;
    return o;
}

bool HasSlot(std::vector<AhOffer> const& plan, std::uint8_t slot)
{
    for (AhOffer const& o : plan)
        if (o.slot == slot)
            return true;
    return false;
}

// The stock curve is untouched; catch-up follows the real green bands (== AutoWowNoWhite::GreenIlvl).
TEST(GearCatchUp, ExpectedIlvlCurves)
{
    EXPECT_EQ(ExpectedIlvl(70), 75u);
    EXPECT_EQ(ExpectedIlvl(75), 80u);
    EXPECT_EQ(ExpectedIlvlCatchUp(40), 45u);
    EXPECT_EQ(ExpectedIlvlCatchUp(57), 62u);
    EXPECT_EQ(ExpectedIlvlCatchUp(58), 62u);
    EXPECT_EQ(ExpectedIlvlCatchUp(70), 116u);
    EXPECT_EQ(ExpectedIlvlCatchUp(71), 130u);
    EXPECT_EQ(ExpectedIlvlCatchUp(80), 186u);
}

TEST(GearCatchUp, ExpectedIlvlEffIsGated)
{
    EXPECT_EQ(ExpectedIlvlEff(75), ExpectedIlvl(75));  // off: byte-identical to the stock curve
    CatchUpScope on(true);
    EXPECT_EQ(ExpectedIlvlEff(75), ExpectedIlvlCatchUp(75));
}

// A L75 bot averaging ilvl 70: adequate by the stock curve (80 * 75% = 60), far below by the real one (155 * 75%).
TEST(GearCatchUp, FarBelowFlipsForNorthrend)
{
    AhParams ap;  // ilvlPct 75
    EXPECT_FALSE(IlvlFarBelow(ap, 70, 75));
    CatchUpScope on(true);
    EXPECT_TRUE(IlvlFarBelow(ap, 70, 75));
}

// Off: the fixed order fills armor and the cap (3) runs out before the trinket slot. On: the empty trinket's
// full-ilvl gain ranks it ahead of marginal armor bumps, so it is reached.
TEST(GearCatchUp, AhPlanReachesEmptyAccessory)
{
    AhParams ap;  // maxBuys 3
    std::vector<AhOffer> const offers = {
        O(1, 4, 10, 100),    // chest +10
        O(2, 6, 10, 100),    // legs +10
        O(3, 0, 10, 100),    // head +10
        O(4, 12, 100, 100),  // empty trinket, full ilvl gain
    };
    std::vector<AhOffer> const off = AhPlan(ap, offers, 75, false, 1000000);
    EXPECT_EQ(off.size(), 3u);
    EXPECT_FALSE(HasSlot(off, 12));

    CatchUpScope on(true);
    std::vector<AhOffer> const plan = AhPlan(ap, offers, 75, false, 1000000);
    EXPECT_EQ(plan.size(), 3u);
    EXPECT_TRUE(HasSlot(plan, 12));
}

// The two-hander / off-hand rule still holds under catch-up: a 2H in the main hand drops the off-hand buy.
TEST(GearCatchUp, AhPlanKeepsTwoHandRule)
{
    CatchUpScope on(true);
    AhParams ap;
    std::vector<AhOffer> const offers = {
        O(1, kSlotMainHand, 10, 100, /*twoHand=*/true),
        O(2, kSlotOffHand, 10, 100),
    };
    std::vector<AhOffer> const plan = AhPlan(ap, offers, 75, false, 1000000);
    EXPECT_TRUE(HasSlot(plan, kSlotMainHand));
    EXPECT_FALSE(HasSlot(plan, kSlotOffHand));
}

// ---- AutoWow.Gear.EquipBagUpgrades ----------------------------------------------------------------------------------
namespace
{
EquipBagFacts Facts(std::uint32_t bagIlvl, std::uint32_t wornIlvl, EquipSlotKind kind, bool usable = true,
                    bool bestArmorType = true, bool stockUpgrade = false)
{
    EquipBagFacts f;
    f.bagIlvl = bagIlvl;
    f.wornIlvl = wornIlvl;
    f.kind = kind;
    f.usable = usable;
    f.bestArmorType = bestArmorType;
    f.stockUpgrade = stockUpgrade;
    return f;
}
}  // namespace

TEST(GearEquipBag, MeetsIlvlMargin)
{
    EXPECT_TRUE(MeetsIlvlMargin(146, 112, 115));   // +30% clears the 115% armor margin
    EXPECT_FALSE(MeetsIlvlMargin(120, 110, 115));  // only +9%: below the off-type margin
    EXPECT_FALSE(MeetsIlvlMargin(100, 100, 100));  // equal ilvl is never an upgrade
    EXPECT_TRUE(MeetsIlvlMargin(101, 100, 100));   // best-type: any strictly-higher ilvl
    EXPECT_TRUE(MeetsIlvlMargin(142, 0, 115));     // empty slot handled by the caller, but margin holds vs 0
}

// The S121 cases: usable, same-slot, far-higher-ilvl bag pieces the stock scorer left in the bags.
TEST(GearEquipBag, SoakCasesForceEquip)
{
    EquipBagParams p;
    // Faelthas (druid) cloak ilvl142 over a worn ilvl59 cloak.
    EXPECT_EQ(DecideEquipBag(p, Facts(142, 59, EquipSlotKind::Cloak)), EquipWhy::Ilvl);
    // Riplash Wristguards (leather) 138 over Elunarian Cuffs 58.
    EXPECT_EQ(DecideEquipBag(p, Facts(138, 58, EquipSlotKind::Armor)), EquipWhy::Ilvl);
    // Caribou Vest (leather, druid best type) 146 over a cloth Robe of the Crimson Order 112.
    EXPECT_EQ(DecideEquipBag(p, Facts(146, 112, EquipSlotKind::Armor)), EquipWhy::Ilvl);
    // Durnstan (hunter) Orca Helmet (mail) 146 over Helm of Lupine Grace 96.
    EXPECT_EQ(DecideEquipBag(p, Facts(146, 96, EquipSlotKind::Armor)), EquipWhy::Ilvl);
    // Delphyne (mage) Bloodspore Sandals (cloth) 134 over Audi's Embroidered Boots 111.
    EXPECT_EQ(DecideEquipBag(p, Facts(134, 111, EquipSlotKind::Armor)), EquipWhy::Ilvl);
}

TEST(GearEquipBag, EmptySlotWearsAnyUsable)
{
    EquipBagParams p;
    EXPECT_EQ(DecideEquipBag(p, Facts(130, 0, EquipSlotKind::Armor)), EquipWhy::Empty);
    EXPECT_EQ(DecideEquipBag(p, Facts(5, 0, EquipSlotKind::Ranged)), EquipWhy::Empty);
    // but only when usable.
    EXPECT_EQ(DecideEquipBag(p, Facts(130, 0, EquipSlotKind::Armor, /*usable=*/false)), EquipWhy::None);
}

TEST(GearEquipBag, DefersToStockWhenItAlreadyEquips)
{
    EquipBagParams p;
    EXPECT_EQ(DecideEquipBag(p, Facts(146, 112, EquipSlotKind::Armor, true, true, /*stockUpgrade=*/true)),
              EquipWhy::Score);
}

TEST(GearEquipBag, PrefersBestArmorTypeAtMargin)
{
    EquipBagParams p;
    // Off-type body armor needs the full margin; a +9% off-type piece is left in the bag.
    EXPECT_EQ(DecideEquipBag(p, Facts(120, 110, EquipSlotKind::Armor, true, /*bestArmorType=*/false)), EquipWhy::None);
    // The same ilvls in the class's best type force on (any strictly-higher ilvl).
    EXPECT_EQ(DecideEquipBag(p, Facts(120, 110, EquipSlotKind::Armor, true, /*bestArmorType=*/true)), EquipWhy::Ilvl);
    // A sidegrade (equal ilvl) never fires.
    EXPECT_EQ(DecideEquipBag(p, Facts(112, 112, EquipSlotKind::Armor, true, true)), EquipWhy::None);
}

TEST(GearEquipBag, WeaponsNeedABiggerJump)
{
    EquipBagParams p;  // weaponMarginPct = 130
    EXPECT_EQ(DecideEquipBag(p, Facts(120, 100, EquipSlotKind::Weapon)), EquipWhy::None);  // +20%: not enough
    EXPECT_EQ(DecideEquipBag(p, Facts(140, 100, EquipSlotKind::Weapon)), EquipWhy::Ilvl);  // +40%: force
    EXPECT_EQ(DecideEquipBag(p, Facts(135, 100, EquipSlotKind::Ranged)), EquipWhy::Ilvl);  // +35%: force
    EXPECT_EQ(DecideEquipBag(p, Facts(146, 0, EquipSlotKind::Weapon)), EquipWhy::Empty);   // empty hand
}
}  // namespace
