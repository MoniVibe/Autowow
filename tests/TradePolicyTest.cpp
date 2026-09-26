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
    // Wire-stable names.
    EXPECT_STREQ(ActionName(Action::Sold), "sold");
    EXPECT_STREQ(ActionName(Action::Expired), "expired");
    EXPECT_STREQ(ActionName(Action::Buy), "buy");
    EXPECT_STREQ(ActionName(Action::Mail), "mail");
    EXPECT_STREQ(FeeKindName(FeeKind::Repair), "repair");
    EXPECT_STREQ(FeeKindName(FeeKind::Train), "train");
}
}  // namespace
