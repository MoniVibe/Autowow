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
}  // namespace
