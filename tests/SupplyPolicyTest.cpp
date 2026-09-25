/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SupplyPolicy.h"

#include "AutoWowQuestLedger.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowSupply;

TEST(SupplyPolicy, RanksMostEmptyFirstTiesLowerGuid)
{
    std::vector<Member> const ranked = RankNeeds({
        {30, 2, 0, 0},
        {10, 4, 0, 0},
        {20, 2, 0, 0},
        {40, 0, 0, 0},  // no want: dropped
        {50, 4, 0, 4},  // four bags incoming already: dropped
        {60, 2, 1, 0},  // same empty as 20/30, one smaller bag: ahead of them
    });
    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0].guid, 10u);
    EXPECT_EQ(ranked[1].guid, 60u);
    EXPECT_EQ(ranked[2].guid, 20u);
    EXPECT_EQ(ranked[3].guid, 30u);
    EXPECT_EQ(TotalWant(ranked), 4u + 3u + 2u + 2u);
    EXPECT_EQ(Wants({1, 3, 0, 1}), 2u);
}

TEST(SupplyPolicy, OrderSizeIsWantWithinMaterialsAndCap)
{
    Recipe const r;  // 2 linen / bolt, 3 bolts + 3 thread / bag
    EXPECT_EQ(BagsFrom(r, 12, 0), 2u);
    EXPECT_EQ(BagsFrom(r, 5, 1), 1u);  // 2 bolts from linen + 1 held
    EXPECT_EQ(BagsFrom(Recipe{0, 3, 3}, 100, 0), 0u);
    EXPECT_EQ(OrderSize(10, 0, 100, 6), 6u);  // MaxOrder
    EXPECT_EQ(OrderSize(10, 7, 100, 6), 3u);  // finished bags cover part of the want
    EXPECT_EQ(OrderSize(10, 0, 2, 6), 2u);    // materials
    EXPECT_EQ(OrderSize(3, 5, 100, 6), 0u);
}

TEST(SupplyPolicy, FeedCoversTheOrderOrTheSkillupStock)
{
    Recipe const r;
    // 2 bags to make = 12 linen; holds 1 bolt (2) + 3 linen.
    EXPECT_EQ(LinenShort(r, 2, true, 3, 1, 0, 40), 7u);
    EXPECT_EQ(LinenShort(r, 2, true, 3, 1, 2, 40), 0u);   // both bags already made
    EXPECT_EQ(LinenShort(r, 0, false, 15, 0, 0, 40), 25u);  // below the recipe: skill-up stock
    EXPECT_EQ(LinenShort(r, 0, false, 50, 0, 0, 40), 0u);

    std::vector<std::uint32_t> const pick = PickStacks({{9, 20}, {3, 5}, {7, 5}}, 8);
    ASSERT_EQ(pick.size(), 2u);  // guid order: 3 (5), 7 (5) covers 8
    EXPECT_EQ(pick[0], 3u);
    EXPECT_EQ(pick[1], 7u);
    EXPECT_TRUE(PickStacks({{1, 5}}, 0).empty());
    std::vector<Stack> many;
    for (std::uint32_t i = 1; i <= 20; ++i)
        many.push_back({i, 1});
    EXPECT_EQ(PickStacks(many, 100).size(), kMaxMailStacks);

    // 2 bags: 6 thread, holds 2 -> 4 * 10c + 100c trainer = 140, purse 90 -> 50 short.
    EXPECT_EQ(CopperShort(r, 2, 2, 10, 100, 90), 50u);
    EXPECT_EQ(CopperShort(r, 2, 6, 10, 0, 0), 0u);
}

TEST(SupplyPolicy, CraftsBagsWhenPossibleElseBolts)
{
    Recipe const r;
    EXPECT_EQ(NextCraft(r, 2, true, 0, 3, 3, 0), Craft::Bag);
    EXPECT_EQ(NextCraft(r, 2, true, 4, 3, 2, 0), Craft::Bolt);   // thread short, bolts still lacking (3 < 6)
    EXPECT_EQ(NextCraft(r, 2, true, 4, 6, 0, 0), Craft::None);   // bolts enough, waiting for thread
    EXPECT_EQ(NextCraft(r, 2, true, 4, 3, 3, 2), Craft::None);   // order already made
    EXPECT_EQ(NextCraft(r, 0, false, 2, 30, 0, 0), Craft::Bolt); // below the recipe: every bolt is a skill-up
    EXPECT_EQ(NextCraft(r, 0, false, 1, 0, 0, 0), Craft::None);
}

TEST(SupplyPolicy, RoutingDeliveryPayXpSurplus)
{
    EXPECT_TRUE(RoutesCloth(true, true, false, 10));
    EXPECT_FALSE(RoutesCloth(false, true, false, 10));
    EXPECT_FALSE(RoutesCloth(true, false, false, 10));
    EXPECT_FALSE(RoutesCloth(true, true, true, 10));  // tailors keep their cloth
    EXPECT_FALSE(RoutesCloth(true, true, false, 0));
    EXPECT_EQ(ClothRoom(150, 200, true), 50u);
    EXPECT_EQ(ClothRoom(250, 200, true), 0u);
    EXPECT_EQ(ClothRoom(0, 200, false), 0u);

    std::vector<Delivery> const d = PlanDeliveries(RankNeeds({{7, 4, 0, 0}, {3, 1, 0, 0}, {5, 4, 0, 0}}), 6);
    ASSERT_EQ(d.size(), 2u);
    EXPECT_EQ(d[0].guid, 5u);
    EXPECT_EQ(d[0].bags, 4u);
    EXPECT_EQ(d[1].guid, 7u);
    EXPECT_EQ(d[1].bags, 2u);
    EXPECT_TRUE(PlanDeliveries(RankNeeds({{1, 4, 0, 0}}), 0).empty());

    EXPECT_EQ(BagPay(200, 200, 3), 1200u);
    EXPECT_EQ(BagPay(200, 150, 1), 300u);
    EXPECT_EQ(WorkXp(0, 400), 20u);
    EXPECT_EQ(WorkXp(0, 10), 1u);
    EXPECT_EQ(WorkXp(55, 400), 55u);
    EXPECT_EQ(Surplus(0, 7, 4), 3u);
    EXPECT_EQ(Surplus(1, 7, 4), 0u);
    EXPECT_EQ(Surplus(0, 4, 4), 0u);
}

TEST(SupplyPolicy, ParsesHome)
{
    Home h;
    ASSERT_TRUE(ParseHome("0,-8813,650,95", h));
    EXPECT_TRUE(h.set);
    EXPECT_EQ(h.map, 0u);
    EXPECT_EQ(h.x, -8813);
    EXPECT_EQ(h.y, 650);
    EXPECT_EQ(h.z, 95);
    ASSERT_TRUE(ParseHome(" 1 , 1660 , -4436 , 18 ", h));
    EXPECT_EQ(h.map, 1u);
    EXPECT_EQ(h.y, -4436);
    Home bad;
    EXPECT_FALSE(ParseHome("0,1,2", bad));
    EXPECT_FALSE(ParseHome("0,1,2,3,4", bad));
    EXPECT_FALSE(ParseHome("-1,1,2,3", bad));
    EXPECT_FALSE(ParseHome("0,x,2,3", bad));
    EXPECT_FALSE(bad.set);
}

TEST(SupplyPolicy, LedgerWireIsStable)
{
    EXPECT_STREQ(AutoWowQuestLedger::EventName(AutoWowQuestLedger::Event::Supply), "supply");
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::Supply), 20);
    EXPECT_STREQ(ReasonName(Reason::Order), "order");
    EXPECT_STREQ(ReasonName(Reason::Refused), "refused");
    EXPECT_STREQ(ReasonName(Reason::Travel), "travel");
    EXPECT_EQ(static_cast<int>(Reason::Travel), 9);
    EXPECT_EQ(LedgerFields("Weavers", 3, 4238, 2, 800, 62964, 70001),
              ",\"house\":\"Weavers\",\"oid\":3,\"item\":4238,\"count\":2,\"copper\":800,\"from\":62964,\"to\":70001");
    EXPECT_EQ(LedgerFields("Weavers", 0, 0, 0, 0, 1, 1, "stuck"),
              ",\"house\":\"Weavers\",\"oid\":0,\"item\":0,\"count\":0,\"copper\":0,\"from\":1,\"to\":1,\"op\":\"stuck\"");
}
}  // namespace
