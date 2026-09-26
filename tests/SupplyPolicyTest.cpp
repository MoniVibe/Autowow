/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SupplyPolicy.h"

#include <unordered_map>

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
    EXPECT_STREQ(ReasonName(Reason::List), "list");
    EXPECT_STREQ(ReasonName(Reason::Buy), "buy");
    EXPECT_STREQ(ReasonName(Reason::Sold), "sold");
    EXPECT_EQ(static_cast<int>(Reason::Sold), 12);
}

TEST(SupplyTiers, TableMatchesTheWorldDb)
{
    ASSERT_EQ(kTierCount, 3u);
    // Tier 0 is the V1 chain the flag-off path hardwires.
    EXPECT_EQ(kTiers[0].cloth, kLinen);
    EXPECT_EQ(kTiers[0].bagSpell, 3755u);
    EXPECT_EQ(kTiers[0].boltSpell, 2963u);
    EXPECT_EQ(kTiers[0].bag, 4238u);
    EXPECT_EQ(kTiers[0].thread, 2320u);
    EXPECT_EQ(kTiers[1].bag, 4240u);
    EXPECT_EQ(kTiers[1].bolt, 2997u);
    EXPECT_EQ(kTiers[2].bag, 4245u);
    EXPECT_EQ(kTiers[2].extra, 4234u);
    for (std::size_t i = 0; i < kTierCount; ++i)
    {
        EXPECT_EQ(kTiers[i].cloth, kTierCloth[i]);
        EXPECT_LE(kTiers[i].boltSkill, kTiers[i].bagSkill);
        if (i)
        {
            EXPECT_GT(kTiers[i].bagSlots, kTiers[i - 1].bagSlots);
            EXPECT_GT(kTiers[i].bagSkill, kTiers[i - 1].bagSkill);
        }
    }
}

TEST(SupplyTiers, ProductIsHighestKnownWantedTier)
{
    // Artisan knows linen + wool bags; members want both; the house can make both: wool.
    EXPECT_EQ(PickProduct({{true, 5, 2}, {true, 3, 1}, {false, 3, 0}}), 1);
    // Nothing wants the wool bag's 8 slots (every member wears 8+): no product even though linen is known
    // (a want for a lower tier always implies one for a higher tier: empty slots count for every tier).
    EXPECT_EQ(PickProduct({{true, 0, 2}, {true, 0, 1}, {false, 0, 0}}), kNoTier);
    // Wool known and wanted but no wool in the house: linen, which the house can make now.
    EXPECT_EQ(PickProduct({{true, 5, 2}, {true, 5, 0}, {false, 5, 0}}), 0);
    // Nothing craftable: the highest known wanted tier (its materials come from the market).
    EXPECT_EQ(PickProduct({{true, 5, 0}, {true, 5, 0}, {false, 5, 0}}), 1);
    // Below the linen bag recipe: nothing to order.
    EXPECT_EQ(PickProduct({{false, 5, 9}, {false, 5, 9}, {false, 5, 9}}), kNoTier);
}

TEST(SupplyTiers, SkillupIsCheapestNonGreyStockedRecipe)
{
    // skill 43: linen bolt (grey 50) and linen bag (grey 105) known; wool not learned yet.
    std::vector<SkillupOption> const opts = {
        {2963, 0, false, true, 50, true, 26},   // 2 linen * 13
        {3755, 0, true, true, 105, true, 138},  // 6 linen + 3 thread
        {2964, 1, false, false, 105, true, 99},
    };
    EXPECT_EQ(PickSkillup(43, opts, true), 0);
    // At 50 the linen bolt is grey: the linen bag levels.
    EXPECT_EQ(PickSkillup(50, opts, true), 1);
    // At 105 everything known is grey.
    EXPECT_EQ(PickSkillup(105, opts, true), -1);
    // No linen in the house: nothing stocked, but the market target ignores stock.
    std::vector<SkillupOption> empty = opts;
    for (SkillupOption& o : empty)
        o.stocked = false;
    EXPECT_EQ(PickSkillup(43, empty, true), -1);
    EXPECT_EQ(PickSkillup(43, empty, false), 0);
    // Equal cost: the lower spell id.
    EXPECT_EQ(PickSkillup(10, {{3000, 0, false, true, 50, true, 5}, {2000, 0, false, true, 50, true, 5}}, true), 1);
}

TEST(SupplyTiers, PerClothCapsDoNotStarveEachOther)
{
    // A full out-of-reach wool stock leaves linen and silk room.
    std::array<std::uint32_t, 3> const rooms = ClothRooms<3>({10, 200, 0}, 200, true);
    EXPECT_EQ(rooms[0], 190u);
    EXPECT_EQ(rooms[1], 0u);
    EXPECT_EQ(rooms[2], 200u);
    std::array<std::uint32_t, 3> const off = ClothRooms<3>({0, 0, 0}, 200, false);
    EXPECT_EQ(off[0] + off[1] + off[2], 0u);
}

TEST(SupplyTiers, CraftsProductThenSkillup)
{
    Tier const& linen = kTiers[0];
    Tier const& silk = kTiers[2];
    // Order open, reagents in hand: the bag.
    EXPECT_EQ(NextTierCraft(linen, 1, true, {0, 3, 3, 0, 0}, nullptr, false, {}), Craft::Bag);
    // Silk pack: thread and bolts but no Heavy Leather: bolts only when short, else wait.
    EXPECT_EQ(NextTierCraft(silk, 1, true, {8, 3, 3, 1, 0}, nullptr, false, {}), Craft::None);
    EXPECT_EQ(NextTierCraft(silk, 1, true, {8, 3, 3, 2, 0}, nullptr, false, {}), Craft::Bag);
    // No order: the skill-up bolt from its cloth.
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, &linen, false, {2, 0, 0, 0, 0}), Craft::Bolt);
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, &linen, false, {1, 0, 0, 0, 0}), Craft::None);
    // Bag skill-up: bolts first, then the bag once thread is in hand.
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, &linen, true, {6, 1, 0, 0, 0}), Craft::Bolt);
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, &linen, true, {0, 3, 0, 0, 0}), Craft::None);
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, &linen, true, {0, 3, 3, 0, 0}), Craft::Bag);
    EXPECT_EQ(NextTierCraft(linen, 0, true, {}, nullptr, false, {}), Craft::None);
}

TEST(SupplyMarket, BuysUnderThePriceCeilingWithinBudget)
{
    // Linen sells to a vendor for 13: ceiling 400% = 52 per unit. Want 25 linen, budget 1000.
    std::vector<MarketListing> const listings = {
        {1, kLinen, 20, 1100},  // 55 / unit: over the ceiling
        {2, kLinen, 10, 400},   // 40 / unit
        {3, kLinen, 5, 150},    // 30 / unit: cheapest first
        {4, kWool, 20, 100},    // not wanted
        {5, kLinen, 20, 800},   // 40 / unit, id after 2
        {6, kLinen, 20, 0},     // bid only
    };
    // Cheapest per unit first: 3 (150), 2 (400); 5 (800) no longer fits the 450 left.
    std::vector<MarketListing> buys = PlanMarketBuys(listings, {{kLinen, 25, 13}}, 400, 1000);
    ASSERT_EQ(buys.size(), 2u);
    EXPECT_EQ(buys[0].id, 3u);
    EXPECT_EQ(buys[1].id, 2u);
    // A bigger budget: 5 too (15 of 25 held, the last stack may overshoot the want).
    buys = PlanMarketBuys(listings, {{kLinen, 25, 13}}, 400, 2000);
    ASSERT_EQ(buys.size(), 3u);
    EXPECT_EQ(buys[2].id, 5u);
    // Budget 500: 3 (150), then 2 (400) does not fit the 350 left.
    buys = PlanMarketBuys(listings, {{kLinen, 25, 13}}, 400, 500);
    ASSERT_EQ(buys.size(), 1u);
    EXPECT_EQ(buys[0].id, 3u);
    // Want already covered: nothing.
    EXPECT_TRUE(PlanMarketBuys(listings, {{kLinen, 0, 13}}, 400, 1000).empty());
    // Ceiling 100%: nothing at or under 13 a unit.
    EXPECT_TRUE(PlanMarketBuys(listings, {{kLinen, 25, 13}}, 100, 100000).empty());
}

TEST(SupplyMarket, SellsWholeStacksAboveTheKeep)
{
    // 20 + 20 + 15 wool, keep 20: list the stacks while 20 stay (ascending guid).
    std::vector<std::uint32_t> const sell = SellStacks({{30, 15}, {10, 20}, {20, 20}}, 20);
    ASSERT_EQ(sell.size(), 2u);
    EXPECT_EQ(sell[0], 10u);  // 55 -> 35 left; guid 20 would leave 15 < 20: kept
    EXPECT_EQ(sell[1], 30u);  // 35 -> 20 left
    EXPECT_TRUE(SellStacks({{1, 20}}, 20).empty());
    EXPECT_EQ(SellStacks({{1, 20}, {2, 20}}, 0).size(), 2u);
    EXPECT_TRUE(OutOfReach(kTiers[1], 43));
    EXPECT_FALSE(OutOfReach(kTiers[0], 43));
    EXPECT_FALSE(OutOfReach(kTiers[1], 75));
    EXPECT_EQ(Short(10, 4), 6u);
    EXPECT_EQ(Short(4, 10), 0u);
}

// ---- product catalog (AutoWow.Supply.Products) ----

TEST(SupplyCatalog, ProductsSelectLines)
{
    std::uint8_t mask = 0xAA;
    EXPECT_TRUE(ParseProducts("bags", mask));
    EXPECT_EQ(mask, 1u);
    EXPECT_TRUE(ParseProducts("bags,potions", mask));
    EXPECT_EQ(mask, 3u);
    EXPECT_TRUE(ParseProducts(" potions ", mask));
    EXPECT_EQ(mask, 2u);
    EXPECT_FALSE(ParseProducts("bags,elixirs", mask));  // unknown name: untouched
    EXPECT_FALSE(ParseProducts("", mask));
    EXPECT_EQ(mask, 2u);
    // Wire-stable ids and ledger names; the default is bags only (the lane B/C chain, unchanged).
    EXPECT_EQ(static_cast<int>(Line::Bags), 0);
    EXPECT_EQ(static_cast<int>(Line::Potions), 1);
    EXPECT_EQ(Params{}.lines, 1u);
    EXPECT_EQ(LineField(Line::Bags), ",\"line\":\"bags\"");
    EXPECT_EQ(LineField(Line::Potions), ",\"line\":\"potions\"");
    EXPECT_EQ(LineOf(Line::Bags).tierCount, 0u);  // bespoke runtime (kTiers)
    EXPECT_STREQ(LineOf(Line::Bags).house, "Weavers");
}

TEST(SupplyCatalog, PotionTableMatchesTheWorldDb)
{
    ProductLine const& l = LineOf(Line::Potions);
    EXPECT_STREQ(l.house, "Brewers");
    EXPECT_EQ(l.skillLine, 171u);
    EXPECT_EQ(l.need, NeedRule::PotionStock);
    EXPECT_EQ(l.consumer, Consumer::DrinkAtLowHp);
    ASSERT_EQ(l.tierCount, 3u);
    EXPECT_EQ(l.tiers[0].spell, 2330u);
    EXPECT_EQ(l.tiers[0].product, 118u);
    EXPECT_EQ(l.tiers[1].spell, 2337u);
    EXPECT_EQ(l.tiers[1].product, 858u);
    EXPECT_EQ(l.tiers[1].skill, 55u);
    EXPECT_EQ(l.tiers[2].spell, 3447u);
    EXPECT_EQ(l.tiers[2].product, 929u);
    EXPECT_EQ(l.tiers[2].skill, 110u);
    EXPECT_EQ(l.tiers[2].reqLevel, 12u);
    EXPECT_EQ(TierOf(l, 858), 1u);
    EXPECT_EQ(TierOf(l, 2447), kNoTier);
    // Routed herbs: Peacebloom, Silverleaf, Briarthorn, Bruiseweed (vials are bought, minors are crafted).
    EXPECT_EQ(RouteItems(l), (std::vector<std::uint32_t>{2447, 765, 2450, 2453}));
}

TEST(SupplyCatalog, CastsAndLacksFollowCraftReagents)
{
    ProductLine const& l = LineOf(Line::Potions);
    std::unordered_map<std::uint32_t, std::uint32_t> held{{2447, 5}, {765, 3}, {2450, 4}, {118, 1}};
    auto have = [&](std::uint32_t item)
    {
        auto const it = held.find(item);
        return it == held.end() ? 0u : it->second;
    };
    EXPECT_EQ(Casts(l, 0, have), 3u);  // silverleaf-bound; vials never limit
    EXPECT_EQ(Casts(l, 1, have), 4u);  // briarthorn 4, minors 1 held + 3 craftable
    EXPECT_EQ(Casts(l, 2, have), 0u);  // no bruiseweed
    // 6 lessers: briarthorn 2 short; minors 5 short -> 5 minor casts: peacebloom 0, silverleaf 2, vials 5.
    std::vector<Lack> const lack = Lacks(l, 1, 6, have);
    ASSERT_EQ(lack.size(), 3u);
    EXPECT_EQ(lack[0].item, 765u);
    EXPECT_EQ(lack[0].units, 2u);
    EXPECT_EQ(lack[1].item, 3371u);
    EXPECT_EQ(lack[1].units, 5u);
    EXPECT_EQ(lack[1].source, Source::Vendor);
    EXPECT_EQ(lack[2].item, 2450u);
    EXPECT_EQ(lack[2].units, 2u);
    EXPECT_TRUE(Lacks(l, 0, 0, have).empty());
    // Next cast toward a lesser: it (a minor and a briarthorn in hand); with no minor, a minor (vial in hand).
    EXPECT_EQ(NextCast(l, 1, have), 1u);
    held[118] = 0;
    EXPECT_EQ(NextCast(l, 1, have), kNoTier);  // no vial for the minor
    held[3371] = 1;
    EXPECT_EQ(NextCast(l, 1, have), 0u);
    EXPECT_EQ(CraftReserve(l, 1, 6, 118), 6u);  // an order of 6 lessers keeps 6 minors at the artisan
    EXPECT_EQ(CraftReserve(l, 0, 6, 118), 0u);
    EXPECT_EQ(CraftReserve(l, kNoTier, 6, 118), 0u);
}

TEST(SupplyCatalog, PotionNeedRanksLowestStockOfTheBestUsableTier)
{
    ProductLine const& l = LineOf(Line::Potions);
    std::array<bool, kMaxLineTiers> const minorOnly{true, false, false}, all{true, true, true}, none{};
    // Best tier: the highest known usable at the level, else the highest usable at the level.
    EXPECT_EQ(BestTier(l, 30, all), 2u);
    EXPECT_EQ(BestTier(l, 11, all), 1u);   // Healing Potion needs 12
    EXPECT_EQ(BestTier(l, 30, minorOnly), 0u);
    EXPECT_EQ(BestTier(l, 30, none), 2u);
    EXPECT_EQ(BestTier(l, 0, all), kNoTier);
    std::vector<StockMember> const members = {
        {40, 30, {0, 0, 2}},   // healing 2 -> want 3
        {10, 30, {9, 9, 5}},   // at target: dropped
        {30, 5, {0, 0, 0}},    // lesser 0 -> want 5
        {20, 20, {0, 0, 2}},   // healing 2, lower guid than 40
        {50, 2, {1, 0, 0}},    // minor 1 -> want 4
    };
    std::vector<StockNeed> const ranked = RankStock(l, members, all, 5);
    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0].guid, 30u);
    EXPECT_EQ(ranked[0].tier, 1u);
    EXPECT_EQ(ranked[1].guid, 50u);
    EXPECT_EQ(ranked[2].guid, 20u);
    EXPECT_EQ(ranked[3].guid, 40u);
    EXPECT_EQ(ranked[3].want, 3u);
    EXPECT_EQ(TierWant(ranked, 2), 6u);
    EXPECT_EQ(TierWant(ranked, 1), 5u);
    EXPECT_EQ(TierWant(ranked, 0), 4u);
    // Only minors known: every member wants minors (its best known usable tier).
    EXPECT_EQ(TierWant(RankStock(l, members, minorOnly, 5), 0), 5u + 5u + 5u + 4u);
    // Deliveries: whole stacks, ascending guid, lowest stock first; the last stack may overshoot.
    std::vector<StackDelivery> const d = PlanStackDeliveries(ranked, 2, {{7, 5}, {3, 2}, {5, 1}});
    ASSERT_EQ(d.size(), 2u);
    EXPECT_EQ(d[0].guid, 20u);
    EXPECT_EQ(d[0].stacks, (std::vector<std::uint32_t>{3, 5}));
    EXPECT_EQ(d[0].units, 3u);
    EXPECT_EQ(d[1].guid, 40u);
    EXPECT_EQ(d[1].units, 5u);
    EXPECT_TRUE(PlanStackDeliveries(ranked, 2, {}).empty());
}

TEST(SupplyCatalog, HerbRoutingKeepsEachHerbUnderItsCap)
{
    // Rooms per herb from the rep's stock under HerbCap 100; none while the rep is not ready.
    std::array<std::uint32_t, 4> const rooms = ClothRooms<4>({100, 40, 0, 130}, 100, true);
    EXPECT_EQ(rooms[0], 0u);
    EXPECT_EQ(rooms[1], 60u);
    EXPECT_EQ(rooms[2], 100u);
    EXPECT_EQ(rooms[3], 0u);
    EXPECT_EQ(ClothRooms<4>({0, 0, 0, 0}, 100, false)[2], 0u);
    // Peacebloom full (room 0): kept for the vendor; silverleaf within 60 units; briarthorn all.
    RoutePlan const plan = PlanRoute({{{1, 20}}, {{12, 20}, {11, 20}, {13, 20}, {14, 20}}, {{21, 7}}, {}},
                                     {rooms[0], rooms[1], rooms[2], rooms[3]});
    EXPECT_EQ(plan.picks, (std::vector<std::uint32_t>{11, 12, 13, 21}));
    EXPECT_EQ(plan.units, (std::vector<std::uint32_t>{0, 60, 7, 0}));
    // At most one mail of stacks.
    std::vector<Stack> many;
    for (std::uint32_t g = 1; g <= 20; ++g)
        many.push_back({g, 1});
    EXPECT_EQ(PlanRoute({many, many}, {100, 100}).picks.size(), kMaxMailStacks);
    EXPECT_TRUE(PlanRoute({many}, {0}).picks.empty());
}

// ---- outfitting grants (AutoWow.Supply.Outfit) ----

TEST(SupplyOutfit, GrantsRankLowestLevelFirstTiesLowerGuid)
{
    std::vector<GrantRequest> const ranked = RankGrants({{30, 12, 100}, {10, 20, 100}, {20, 12, 50}, {5, 30, 1}});
    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0].guid, 20u);
    EXPECT_EQ(ranked[1].guid, 30u);
    EXPECT_EQ(ranked[2].guid, 10u);
    EXPECT_EQ(ranked[3].guid, 5u);
}

TEST(SupplyOutfit, GrantPaysTheWholeShortfallWithinBothCaps)
{
    GrantRequest const r{7, 10, 581};  // Mining Pick 81 + Journeyman 450 + buffer 50
    GrantWindow w;
    GrantBudget b;
    GrantDecision d = DecideGrant(r, 100, w, b, 3, 500, 5000);
    EXPECT_EQ(d.verdict, GrantVerdict::Pay);
    EXPECT_EQ(d.copper, 481u);
    d = DecideGrant(r, 600, w, b, 3, 500, 5000);  // the bot's own money covers it
    EXPECT_EQ(d.verdict, GrantVerdict::Covered);
    EXPECT_EQ(d.copper, 0u);
    EXPECT_EQ(DecideGrant(r, 0, w, b, 3, 500, 5000).verdict, GrantVerdict::BotCap);  // 581 > 500: never a part
    NoteGrant(w, b, 10, 3, 481);
    EXPECT_EQ(w.granted, 481u);
    EXPECT_EQ(b.spent, 481u);
    // Same level: 19 left in the window (need - money = the shortfall).
    EXPECT_EQ(DecideGrant({7, 10, 119}, 100, w, b, 3, 500, 5000).verdict, GrantVerdict::Pay);
    EXPECT_EQ(DecideGrant({7, 10, 120}, 100, w, b, 3, 500, 5000).verdict, GrantVerdict::BotCap);
    // A level-up opens a fresh window.
    EXPECT_EQ(DecideGrant({7, 11, 400}, 0, w, b, 3, 500, 5000).verdict, GrantVerdict::Pay);
    // Team budget: another bot, same hour; the next hour starts over.
    EXPECT_EQ(DecideGrant({8, 5, 20}, 0, {}, b, 3, 500, 500).verdict, GrantVerdict::TeamBudget);
    EXPECT_EQ(DecideGrant({8, 5, 20}, 0, {}, b, 4, 500, 500).verdict, GrantVerdict::Pay);
    NoteGrant(w, b, 11, 4, 100);
    EXPECT_EQ(w.level, 11u);
    EXPECT_EQ(w.granted, 100u);
    EXPECT_EQ(b.hour, 4u);
    EXPECT_EQ(b.spent, 100u);
    EXPECT_STREQ(GrantVerdictName(GrantVerdict::BotCap), "grant_bot_cap");
    EXPECT_STREQ(GrantVerdictName(GrantVerdict::TeamBudget), "grant_team_budget");
}

TEST(SupplyOutfit, LedgerWireIsStable)
{
    EXPECT_STREQ(ReasonName(Reason::Outfit), "outfit");
    EXPECT_EQ(static_cast<int>(Reason::Outfit), 13);
    Params const p;
    EXPECT_FALSE(p.outfit);
    EXPECT_EQ(p.outfitCheckMs, 600000u);
    EXPECT_EQ(p.outfitMaxCopper, 500u);
    EXPECT_EQ(p.outfitBudgetPerHour, 5000u);
}
}  // namespace
