/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TradePolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowTrade;

Holding Green(std::uint32_t entry, std::uint32_t guid, std::uint32_t sellPrice)
{
    Holding h;
    h.entry = entry;
    h.guid = guid;
    h.count = 1;
    h.quality = kQualityUncommon;
    h.itemClass = 4;  // armor
    h.sellPrice = sellPrice;
    h.usageAh = true;
    h.deposit = 100;
    return h;
}

// ---- posting ----------------------------------------------------------------------------------------
TEST(Trade, PostableNeedsAhUsageAndGreenOrMats)
{
    Holding h = Green(100, 1, 50);
    EXPECT_TRUE(Postable(h));
    h.usageAh = false;  // needed (equip / quest / skill) or soulbound: never listed
    EXPECT_FALSE(Postable(h));
    h.usageAh = true;
    h.quality = 1;  // white armor: vendor it
    EXPECT_FALSE(Postable(h));
    h.itemClass = kClassTradeGoods;  // white cloth / ore / herbs: listed
    EXPECT_TRUE(Postable(h));
    h.itemClass = kClassReagent;
    EXPECT_TRUE(Postable(h));
    h.sellPrice = 0;  // no vendor value: no price anchor
    EXPECT_FALSE(Postable(h));
}

TEST(Trade, UnitPriceVendorMultipleUndercutAndFloor)
{
    Params p;  // mult 300, floor 150, undercut 1
    EXPECT_EQ(UnitPrice(p, 100, 0), 300U);    // no competition: vendor * 3
    EXPECT_EQ(UnitPrice(p, 100, 400), 399U);  // undercut the lowest competing buyout
    EXPECT_EQ(UnitPrice(p, 100, 151), 150U);  // at the floor
    EXPECT_EQ(UnitPrice(p, 100, 150), 0U);    // under the floor: do not list
    EXPECT_EQ(UnitPrice(p, 3, 0), 9U);        // floor rounds up (4.5 -> 5 <= 9)
    EXPECT_EQ(UnitPrice(p, 3, 5), 0U);        // 4 < 5
}

TEST(Trade, PlanPostsOrdersByEntryGuidAndRespectsDepositBudgetAndCap)
{
    Params p;
    p.maxPosts = 2;
    std::vector<Holding> hs = {Green(300, 9, 50), Green(200, 7, 50), Green(200, 3, 50)};
    std::vector<Post> posts = PlanPosts(p, hs, 1000);  // deposit budget 500
    ASSERT_EQ(posts.size(), 2U);
    EXPECT_EQ(posts[0].entry, 200U);
    EXPECT_EQ(posts[0].guid, 3U);
    EXPECT_EQ(posts[1].guid, 7U);
    EXPECT_EQ(posts[0].buyout, 150U);
    EXPECT_EQ(posts[0].bid, 135U);  // 90 %
    EXPECT_EQ(posts[0].deposit, 100U);
    // Budget 150 (money 300 * 50 %): one deposit of 100 fits, the second does not.
    p.maxPosts = 6;
    posts = PlanPosts(p, hs, 300);
    ASSERT_EQ(posts.size(), 1U);
    EXPECT_EQ(posts[0].guid, 3U);
    // Broke bot: nothing.
    EXPECT_TRUE(PlanPosts(p, hs, 0).empty());
}

TEST(Trade, PlanPostsStackPriceAndSkipsUnderFloor)
{
    Params p;
    Holding cloth = Green(2589, 5, 13);  // linen cloth, white trade good, stack of 20
    cloth.quality = 1;
    cloth.itemClass = kClassTradeGoods;
    cloth.count = 20;
    std::vector<Post> posts = PlanPosts(p, {cloth}, 10000);
    ASSERT_EQ(posts.size(), 1U);
    EXPECT_EQ(posts[0].buyout, 39U * 20);
    cloth.lowestOther = 15;  // someone lists at 15/unit: 14 < floor 20 -> vendor instead
    EXPECT_TRUE(PlanPosts(p, {cloth}, 10000).empty());
}

// ---- buying -----------------------------------------------------------------------------------------
TEST(Trade, BuyBudgetKeepsReserve)
{
    Params p;  // 50 %
    EXPECT_EQ(BuyBudget(p, 1000, 200), 400U);
    EXPECT_EQ(BuyBudget(p, 200, 200), 0U);
    EXPECT_EQ(BuyBudget(p, 100, 200), 0U);
}

TEST(Trade, PlanBuysCheapestUpgradeOnlyWithinBudget)
{
    Params p;
    std::vector<Listing> ls = {{11, 500, 1, 900, 100, Want::Upgrade},
                               {12, 501, 1, 700, 100, Want::Upgrade},
                               {10, 502, 1, 700, 100, Want::Upgrade},
                               {13, 503, 1, 50, 10, Want::None}};
    std::vector<Buy> b = PlanBuys(p, ls, 800);
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].id, 10U);  // 700 tie: lower auction id
    EXPECT_EQ(b[0].want, Want::Upgrade);
    EXPECT_TRUE(PlanBuys(p, ls, 699).empty());
}

TEST(Trade, PlanBuysMatsCheapestPerUnitCappedByCountPriceAndBudget)
{
    Params p;  // unit <= vendor * 4, <= 20 units per entry
    p.matCountMax = 10;
    std::vector<Listing> ls = {{1, 2770, 5, 100, 5, Want::Mat},   // 20/unit: at cap
                               {2, 2770, 5, 50, 5, Want::Mat},    // 10/unit: cheapest
                               {3, 2770, 5, 60, 5, Want::Mat},    // 12/unit
                               {4, 2770, 1, 25, 5, Want::Mat},    // 25/unit: above cap
                               {5, 2771, 2, 30, 5, Want::Mat}};   // other entry
    std::vector<Buy> b = PlanBuys(p, ls, 1000);
    ASSERT_EQ(b.size(), 3U);
    EXPECT_EQ(b[0].id, 2U);
    EXPECT_EQ(b[1].id, 3U);  // 10 units reached: listing 1 skipped
    EXPECT_EQ(b[2].id, 5U);
    // Budget 70: 50 fits, 60 does not, 30 no longer fits (20 left).
    b = PlanBuys(p, ls, 70);
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].id, 2U);
}

TEST(Trade, PlanBuysUpgradeFirstThenMatsFromTheRest)
{
    Params p;
    std::vector<Listing> ls = {{7, 2770, 5, 50, 5, Want::Mat}, {8, 600, 1, 400, 50, Want::Upgrade}};
    std::vector<Buy> b = PlanBuys(p, ls, 420);
    ASSERT_EQ(b.size(), 1U);  // upgrade takes 400; 20 left < 50
    EXPECT_EQ(b[0].id, 8U);
}

// ---- mail -------------------------------------------------------------------------------------------
TEST(Trade, ParseAuctionSubjectAndSaleBid)
{
    AuctionMail const m = ParseAuctionSubject("2589:0:2:1234:20");
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.entry, 2589U);
    EXPECT_EQ(m.response, kAuctionSuccessful);
    EXPECT_EQ(m.auctionId, 1234U);
    EXPECT_EQ(m.count, 20U);
    EXPECT_EQ(MailAction(m), Action::Sold);
    EXPECT_EQ(MailAction(ParseAuctionSubject("2589:0:3:1:20")), Action::Expired);
    EXPECT_EQ(MailAction(ParseAuctionSubject("2589:0:5:1:20")), Action::Expired);
    EXPECT_EQ(MailAction(ParseAuctionSubject("2589:0:1:1:20")), Action::Mail);  // won
    EXPECT_FALSE(ParseAuctionSubject("Hello").ok);
    EXPECT_FALSE(ParseAuctionSubject("1:1:2:3:4").ok);
    EXPECT_FALSE(ParseAuctionSubject("1:0:2:3").ok);
    EXPECT_EQ(MailAction(ParseAuctionSubject("")), Action::Mail);
    EXPECT_EQ(ParseSaleBid("     1000000f5ab:780:780:117:39:3600:0"), 780U);
    EXPECT_EQ(ParseSaleBid("garbage"), 0U);
    EXPECT_EQ(ParseSaleDeposit("     1000000f5ab:780:780:117:39:3600:0"), 117U);
    EXPECT_EQ(ParseSaleDeposit("garbage"), 0U);
}

TEST(Trade, LedgerFieldsWireFormat)
{
    EXPECT_EQ(LedgerFields(Action::Post, 2589, 20, 780, -117, 0),
              ",\"action\":\"post\",\"item\":2589,\"count\":20,\"price\":780,\"gold\":-117,\"ah\":0");
    EXPECT_EQ(LedgerFields(Action::Fee, 0, 0, 55, -55, 0, FeeKindName(FeeKind::Flight)),
              ",\"action\":\"fee\",\"item\":0,\"count\":0,\"price\":55,\"gold\":-55,\"ah\":0,\"kind\":\"flight\"");
    // AutoWow.Gear.AuctionUpgrades: an ah_gear buy appends its ilvl gain; 0 appends nothing.
    EXPECT_EQ(LedgerFields(Action::Buy, 9811, 1, 20259, -20259, 1899, nullptr, 12),
              ",\"action\":\"buy\",\"item\":9811,\"count\":1,\"price\":20259,\"gold\":-20259,\"ah\":1899,\"gain\":12");
    EXPECT_EQ(LedgerFields(Action::Buy, 9811, 1, 400, -400, 7, nullptr, 0),
              LedgerFields(Action::Buy, 9811, 1, 400, -400, 7));
    // Wire-stable names.
    EXPECT_STREQ(ActionName(Action::Sold), "sold");
    EXPECT_STREQ(ActionName(Action::Expired), "expired");
    EXPECT_STREQ(ActionName(Action::Buy), "buy");
    EXPECT_STREQ(ActionName(Action::Mail), "mail");
    EXPECT_STREQ(FeeKindName(FeeKind::Repair), "repair");
    EXPECT_STREQ(FeeKindName(FeeKind::Train), "train");
}

// Lane U: random sellers' listings + COD mails per game hour; the window resets on a new hour.
TEST(TradeMarket, SellerCapPerHour)
{
    SellerWindow w;
    EXPECT_EQ(SellerRoom(w, 5, 6), 6u);
    NoteSeller(w, 5, 4);
    EXPECT_EQ(SellerRoom(w, 5, 6), 2u);
    NoteSeller(w, 5, 3);
    EXPECT_EQ(SellerRoom(w, 5, 6), 0u);  // over the cap: none left, never negative
    EXPECT_EQ(SellerRoom(w, 6, 6), 6u);  // the next hour
    NoteSeller(w, 6, 1);
    EXPECT_EQ(w.hour, 6u);
    EXPECT_EQ(w.used, 1u);
    EXPECT_EQ(SellerRoom(w, 6, 0), 0u);
    Params const p;
    EXPECT_FALSE(p.randomSellers);
    EXPECT_EQ(p.sellerCapPerHour, 6u);
    EXPECT_STREQ(ActionName(Action::CodSell), "cod_sell");
    EXPECT_STREQ(ActionName(Action::CodBuy), "cod_buy");
    EXPECT_STREQ(ActionName(Action::CodReturn), "cod_return");
    EXPECT_EQ(static_cast<int>(Action::CodSell), 7);
}

// soak-s48-full-r1: 120 emptied mails held the Weavers rep's box at the core cap.
TEST(TradeMail, EmptiedDeliveredMailIsDeleted)
{
    EXPECT_TRUE(EmptyMail(true, false, 0, false));
    EXPECT_FALSE(EmptyMail(false, false, 0, false));  // in transit
    EXPECT_FALSE(EmptyMail(true, true, 0, false));    // COD
    EXPECT_FALSE(EmptyMail(true, false, 5, false));   // money left
    EXPECT_FALSE(EmptyMail(true, false, 0, true));    // items left (bags full)
    EXPECT_TRUE(Params{}.deleteEmptyMail);
}

// ---- AutoWow.Auction.ListLoot -----------------------------------------------------------------------
TEST(AuctionListLoot, ListsUnwantedBoeGreenBlueEquip)
{
    // INVTYPE_CHEST = 5, bonding NO_BIND (0): a green chest the bot does not want is listed.
    EXPECT_TRUE(ListLootEquip(kQualityUncommon, 5, 0, false, false));
    EXPECT_TRUE(ListLootEquip(kQualityRare, 5, 2 /*BoE*/, false, false));  // blue BoE
    EXPECT_FALSE(ListLootEquip(kQualityUncommon, 5, 0, false, true));      // an upgrade it wants: kept to wear
    EXPECT_FALSE(ListLootEquip(1, 5, 0, false, false));                    // white: vendored, not listed here
    EXPECT_FALSE(ListLootEquip(4 /*epic*/, 5, 0, false, false));           // only green / blue
    EXPECT_FALSE(ListLootEquip(kQualityUncommon, 0 /*non-equip*/, 0, false, false));
    EXPECT_FALSE(ListLootEquip(kQualityUncommon, 5, kBindOnPickup, false, false));
    EXPECT_FALSE(ListLootEquip(kQualityUncommon, 5, kBindQuestItem, false, false));
    EXPECT_FALSE(ListLootEquip(kQualityUncommon, 5, 0, true /*soulbound*/, false));
}

// ---- AutoWow.Auction.SeedThinSlots ------------------------------------------------------------------
TEST(AuctionSeed, SlotCategoryOfInventoryType)
{
    EXPECT_EQ(SeedSlotOf(12), SeedSlot::Trinket);
    EXPECT_EQ(SeedSlotOf(11), SeedSlot::Ring);
    EXPECT_EQ(SeedSlotOf(2), SeedSlot::Neck);
    EXPECT_EQ(SeedSlotOf(16), SeedSlot::Back);
    EXPECT_EQ(SeedSlotOf(14), SeedSlot::OffHand);  // shield
    EXPECT_EQ(SeedSlotOf(23), SeedSlot::OffHand);  // held in off hand
    EXPECT_EQ(SeedSlotOf(15), SeedSlot::Ranged);
    EXPECT_EQ(SeedSlotOf(25), SeedSlot::Ranged);   // thrown
    EXPECT_EQ(SeedSlotOf(28), SeedSlot::Ranged);   // relic
    EXPECT_EQ(SeedSlotOf(5), SeedSlot::None);      // chest: not a seeded slot
    EXPECT_EQ(SeedSlotOf(1), SeedSlot::None);      // head
}

TEST(AuctionSeed, LevelBands)
{
    EXPECT_EQ(SeedBandOf(0), 0);
    EXPECT_EQ(SeedBandOf(1), 0);
    EXPECT_EQ(SeedBandOf(10), 0);
    EXPECT_EQ(SeedBandOf(11), 1);
    EXPECT_EQ(SeedBandOf(71), 7);
    EXPECT_EQ(SeedBandOf(80), 7);
    EXPECT_EQ(SeedBandOf(200), kSeedBands - 1);  // clamp
}

TEST(AuctionSeed, SeedCountTopsUpToThreshold)
{
    EXPECT_EQ(SeedCount(0, 3, 10), 3u);  // empty slot: fill to the threshold
    EXPECT_EQ(SeedCount(2, 3, 10), 1u);  // one short
    EXPECT_EQ(SeedCount(3, 3, 10), 0u);  // not thin
    EXPECT_EQ(SeedCount(5, 3, 10), 0u);
    EXPECT_EQ(SeedCount(0, 3, 2), 2u);   // capped by room
    EXPECT_EQ(SeedCount(0, 3, 0), 0u);
}

TEST(AuctionSeed, BandOrderByDemandThenHigherBand)
{
    std::array<std::uint32_t, kSeedBands> demand{};
    EXPECT_TRUE(SeedBandOrder(demand).empty());  // no demand: seed nothing

    demand = {};
    demand[6] = 9;
    demand[7] = 12;  // L71-80 the neediest (the soak gap)
    EXPECT_EQ(SeedBandOrder(demand), (std::vector<std::uint8_t>{7, 6}));

    demand = {};
    demand[2] = 5;
    demand[5] = 5;  // tie -> higher band first
    EXPECT_EQ(SeedBandOrder(demand), (std::vector<std::uint8_t>{5, 2}));

    demand = {};
    demand[0] = 3;  // only band 0 has demand; zero bands dropped
    EXPECT_EQ(SeedBandOrder(demand), (std::vector<std::uint8_t>{0}));

    demand = {};  // soak S120: low bands crowded, high bands the real gap -> high bands still ordered by count
    demand[1] = 6;
    demand[3] = 3;
    demand[6] = 1;
    demand[7] = 9;
    EXPECT_EQ(SeedBandOrder(demand), (std::vector<std::uint8_t>{7, 1, 3, 6}));
}

TEST(AuctionSeed, DailyCapWindow)
{
    SeedWindow w;
    EXPECT_EQ(SeedRoom(w, 100, 20), 20u);
    NoteSeed(w, 100, 15);
    EXPECT_EQ(SeedRoom(w, 100, 20), 5u);
    NoteSeed(w, 100, 5);
    EXPECT_EQ(SeedRoom(w, 100, 20), 0u);
    EXPECT_EQ(SeedRoom(w, 101, 20), 20u);  // a new day resets
    NoteSeed(w, 101, 1);
    EXPECT_EQ(SeedRoom(w, 101, 20), 19u);
}
}  // namespace
