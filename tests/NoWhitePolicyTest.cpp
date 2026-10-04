/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NoWhitePolicy.h"
#include "WeaponOrderPolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowNoWhite;

Worn W(std::uint8_t quality, std::uint32_t ilvl, bool weapon = true) { return Worn{true, weapon, quality, ilvl}; }

TEST(NoWhitePolicy, GreenCurveBands)
{
    EXPECT_EQ(GreenIlvl(1), 6u);
    EXPECT_EQ(GreenIlvl(40), 45u);
    EXPECT_EQ(GreenIlvl(57), 62u);
    EXPECT_EQ(GreenIlvl(58), 62u);
    EXPECT_EQ(GreenIlvl(59), 66u);   // 62 + 4.5 truncated
    EXPECT_EQ(GreenIlvl(66), 98u);
    EXPECT_EQ(GreenIlvl(67), 102u);
    EXPECT_EQ(GreenIlvl(70), 116u);
    EXPECT_EQ(GreenIlvl(71), 130u);
    EXPECT_EQ(GreenIlvl(80), 186u);  // 130 + 56.7 truncated
    Params p;
    p.floorPct = 85;
    EXPECT_EQ(FloorIlvl(p, 67), 86u);
}

TEST(NoWhitePolicy, FloorIsQualityOrCurve)
{
    Params const p;
    EXPECT_TRUE(BelowFloor(p, Worn{}, 20));              // empty
    EXPECT_TRUE(BelowFloor(p, W(1, 60), 20));            // white, however high
    EXPECT_TRUE(BelowFloor(p, W(0, 60), 20));            // grey
    EXPECT_TRUE(BelowFloor(p, W(2, 24), 20));            // green under L+5
    EXPECT_FALSE(BelowFloor(p, W(2, 25), 20));           // green on the curve
    EXPECT_FALSE(BelowFloor(p, W(3, 25), 20));           // blue
    EXPECT_TRUE(BelowFloor(p, W(2, 47), 71));            // Thalindor: L71 on ilvl 47
}

TEST(NoWhitePolicy, FloorSlotsPerRole)
{
    Params const p;
    Worn const good = W(2, 30), white = W(1, 30);
    EXPECT_EQ(FloorSlots(p, 20, good, good, good, false, false), 0u);
    EXPECT_EQ(FloorSlots(p, 20, white, good, good, false, false), 1u << 15);
    // the off hand counts only for a dual wielder holding a weapon there (not a warrior's shield / empty hand)
    EXPECT_EQ(FloorSlots(p, 20, good, white, good, false, true), 1u << 16);
    EXPECT_EQ(FloorSlots(p, 20, good, W(1, 30, false), good, false, true), 0u);
    EXPECT_EQ(FloorSlots(p, 20, good, Worn{}, good, false, true), 0u);
    EXPECT_EQ(FloorSlots(p, 20, good, white, good, false, false), 0u);
    // the ranged slot counts for hunters only
    EXPECT_EQ(FloorSlots(p, 20, good, good, white, true, false), 1u << 17);
    EXPECT_EQ(FloorSlots(p, 20, good, good, white, false, false), 0u);
}

TEST(NoWhitePolicy, FitsWeaponSlots)
{
    EXPECT_TRUE(Fits(kInvWeapon, kSlotMainHand, false, false));
    EXPECT_TRUE(Fits(kInvMainHand, kSlotMainHand, false, false));
    EXPECT_FALSE(Fits(kInv2H, kSlotMainHand, true, false));  // a two-hander with the off hand occupied
    EXPECT_TRUE(Fits(kInv2H, kSlotMainHand, true, true));
    EXPECT_FALSE(Fits(kInvOffHand, kSlotOffHand, false, true));  // not a dual wielder
    EXPECT_TRUE(Fits(kInvOffHand, kSlotOffHand, true, true));
    EXPECT_FALSE(Fits(kInvMainHand, kSlotOffHand, true, true));
    EXPECT_TRUE(Fits(kInvRangedRight, kSlotRanged, false, true));
    EXPECT_FALSE(Fits(kInvRanged, kSlotMainHand, false, true));
}

TEST(NoWhitePolicy, PickCandidatePrefersFloorThenIlvlThenStableKeys)
{
    Params const p;
    Worn const worn = W(1, 20);  // white at L30 (floor 35)
    std::vector<Candidate> c = {
        {7, 70, 1000, 2, 33},  // green under the floor
        {5, 50, 1001, 2, 36},  // clears
        {3, 40, 1002, 1, 99},  // white never
        {4, 41, 1003, 2, 36},  // clears, same ilvl, lower holder -> wins
    };
    EXPECT_EQ(PickCandidate(p, 30, worn, c), 3u);
    c.pop_back();
    EXPECT_EQ(PickCandidate(p, 30, worn, c), 1u);
    c.erase(c.begin() + 1);
    EXPECT_EQ(PickCandidate(p, 30, worn, c), 0u);  // below the floor still improves on a white
    // a green worn: only higher ilvl improves
    EXPECT_EQ(PickCandidate(p, 30, W(2, 34), {{1, 1, 1, 2, 34}}), kNone);
    EXPECT_EQ(PickCandidate(p, 30, W(2, 34), {{1, 1, 1, 2, 35}}), 0u);
    EXPECT_EQ(PickCandidate(p, 30, worn, {}), kNone);
}

TEST(NoWhitePolicy, SourceOrderBagAuctionHandDownSmith)
{
    Params p;
    p.auctionWaitPasses = 3;
    Track t;
    Advance(p, t, 40);
    EXPECT_EQ(NextStep(p, t, true), Step::Bag);
    t.tried |= SrcBag;
    EXPECT_EQ(NextStep(p, t, true), Step::WaitAuction);
    EXPECT_EQ(NextStep(p, t, false), Step::HandDown);  // roles skip the auction
    t.tried |= SrcAuction;
    EXPECT_EQ(NextStep(p, t, true), Step::HandDown);
    t.tried |= SrcHandDown;
    EXPECT_EQ(NextStep(p, t, true), Step::SmithOrder);
    t.tried |= SrcSmith;
    EXPECT_EQ(NextStep(p, t, true), Step::Idle);

    // the auction wait is bounded
    Track w;
    Advance(p, w, 40);
    w.tried = SrcBag;
    Advance(p, w, 40);
    Advance(p, w, 40);
    EXPECT_EQ(w.passes, 3u);
    EXPECT_EQ(NextStep(p, w, true), Step::HandDown);

    // a level-up or RetryPasses start over
    Advance(p, w, 41);
    EXPECT_EQ(w.tried, 0);
    EXPECT_EQ(w.passes, 1u);
    p.retryPasses = 2;
    w.tried = SrcBag | SrcHandDown;
    Advance(p, w, 41);
    EXPECT_EQ(w.tried, SrcBag | SrcHandDown);
    Advance(p, w, 41);
    EXPECT_EQ(w.tried, 0);
}

TEST(NoWhitePolicy, VendorWhiteOnlyAsLastResort)
{
    Params const p;
    Track t;
    t.tried = SrcBag | SrcHandDown;
    EXPECT_TRUE(VendorLastResort(p, t, false, Worn{}));
    EXPECT_TRUE(VendorLastResort(p, t, false, W(0, 15)));  // grey
    EXPECT_TRUE(VendorLastResort(p, t, false, W(1, 2)));   // starter white
    EXPECT_FALSE(VendorLastResort(p, t, false, W(1, 20)));  // a real white: no other white
    EXPECT_FALSE(VendorLastResort(p, t, true, Worn{}));     // errand bot: the auction comes first
    t.tried |= SrcAuction;
    EXPECT_TRUE(VendorLastResort(p, t, true, Worn{}));
    t.tried = SrcBag;
    EXPECT_FALSE(VendorLastResort(p, t, false, Worn{}));  // hand-me-downs not tried yet
}

TEST(NoWhitePolicy, QuestRewardWeaponClearingFloorWins)
{
    Params const p;
    std::array<Worn, 3> const worn = {W(1, 20), Worn{}, Worn{}};
    std::vector<RewardChoice> choices = {
        {0xFF, true, 3, 40},  // armor: never by this rule
        {15, true, 2, 36},    // clears L30 floor 35
        {15, true, 2, 38},    // higher -> wins
        {15, false, 3, 50},   // unusable
        {15, true, 1, 60},    // white
    };
    EXPECT_EQ(PickFloorReward(p, 30, choices, worn), 2u);
    choices[2].ilvl = 36;
    EXPECT_EQ(PickFloorReward(p, 30, choices, worn), 1u);  // tie: lower index
    std::vector<RewardChoice> under = {{15, true, 2, 30}};
    EXPECT_EQ(PickFloorReward(p, 30, under, worn), kNone);  // under the floor: the stock choice stands
}

TEST(NoWhitePolicy, AuctionDropsWhiteWeaponsOnly)
{
    std::vector<AutoWowGear::AhOffer> offers = {
        {1, 100, 50, 5, 15, false, 1},  // white main hand: dropped
        {2, 101, 50, 5, 15, false, 2},  // green main hand
        {3, 102, 50, 5, 4, false, 1},   // white chest: kept (armor is not this rule)
        {4, 103, 50, 5, 17, false, 0},  // grey ranged: dropped
        {5, 104, 50, 5, 16, false, 3},  // blue off hand
    };
    DropWhiteWeapons(offers);
    ASSERT_EQ(offers.size(), 3u);
    EXPECT_EQ(offers[0].id, 2u);
    EXPECT_EQ(offers[1].id, 3u);
    EXPECT_EQ(offers[2].id, 5u);
    AutoWowGear::AhParams const ah;
    EXPECT_EQ(AhAllowanceCopper(ah, 30), AutoWowGear::ReserveCopper(30) + 30u * 30u * ah.priceMult);
}

TEST(NoWhitePolicy, LedgerFields)
{
    EXPECT_EQ(LedgerFields(15, 2000, 36, 2, 35, 62960, 0),
              ",\"slot\":15,\"item\":2000,\"ilvl\":36,\"quality\":2,\"floor\":35,\"from\":62960,\"oid\":0");
}

TEST(WeaponOrderPolicy, QueueIdsNeverReusedAndDeduped)
{
    AutoWowWeaponOrder::Queue q;
    AutoWowWeaponOrder::Order o;
    o.guid = 63000;
    o.slot = 15;
    o.alliance = true;
    std::uint32_t const a = q.Post(o);
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(q.Post(o), a);  // same (guid, slot): the open order
    o.slot = 17;
    std::uint32_t const b = q.Post(o);
    EXPECT_EQ(b, 2u);
    o.alliance = false;
    o.guid = 70000;
    EXPECT_EQ(q.Post(o), 3u);
    ASSERT_EQ(q.Pending(true).size(), 2u);
    EXPECT_EQ(q.Pending(true)[0].id, 1u);  // FIFO by id
    EXPECT_EQ(q.Pending(false).size(), 1u);
    EXPECT_TRUE(q.Remove(a));
    EXPECT_FALSE(q.Remove(a));
    EXPECT_FALSE(q.Has(63000, 15));
    o.guid = 63000;
    o.slot = 15;
    o.alliance = true;
    EXPECT_EQ(q.Post(o), 4u);  // a re-post gets a fresh id
    for (std::uint32_t g = 0; q.Size() < AutoWowWeaponOrder::kMaxOrders; ++g)
    {
        o.guid = 100000 + g;
        q.Post(o);
    }
    o.guid = 1;
    EXPECT_EQ(q.Post(o), 0u);  // full
}
}  // namespace
