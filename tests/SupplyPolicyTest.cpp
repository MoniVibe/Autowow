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

TEST(SupplyPolicy, OwnRowsCarryTheBotsHouseLine)
{
    // Soak S56: the Tinkers artisan (gear line eng only) logged its travel / junk rows as house=Weavers (bags).
    RoleInfo r;
    EXPECT_EQ(OwnLine(r), Line::Bags);  // no line at all
    r.gear = static_cast<std::uint8_t>(Line::Engineering);
    EXPECT_EQ(OwnLine(r), Line::Engineering);
    r.line = static_cast<std::uint8_t>(Line::Potions);
    EXPECT_EQ(OwnLine(r), Line::Potions);  // the catalog line before the gear line
    RoleInfo bags;
    bags.bagHouse = true;
    bags.gear = static_cast<std::uint8_t>(Line::ClothGear);
    EXPECT_EQ(OwnLine(bags), Line::Bags);  // the bag house keeps its bag label
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

TEST(SupplyArtisanUpkeep, ApprenticeGateAppliesToArtisansOnly)
{
    // soak-s45-full-r1: the level 1 / 2 Brewers artisans never reached Apprentice Alchemy (level 5).
    EXPECT_EQ(GatedRole(Role::Artisan, 1, 10, false), Role::None);
    EXPECT_EQ(GatedRole(Role::Artisan, 9, 10, false), Role::None);
    EXPECT_EQ(GatedRole(Role::Artisan, 10, 10, false), Role::Artisan);  // graduated
    EXPECT_EQ(GatedRole(Role::Artisan, 12, 10, true), Role::None);      // not pulled out of a dungeon run
    EXPECT_EQ(GatedRole(Role::Rep, 1, 10, false), Role::Rep);           // reps never gated
    EXPECT_EQ(GatedRole(Role::None, 1, 10, false), Role::None);
    EXPECT_EQ(GatedRole(Role::Artisan, 1, 0, false), Role::Artisan);    // 0 = off: the old behaviour
    EXPECT_EQ(GatedRole(Role::Artisan, 1, 0, true), Role::Artisan);
}

TEST(SupplyArtisanUpkeep, JunkClassification)
{
    EXPECT_EQ(ClassifyJunk({1, 0, false, false, false}), Junk::Destroy);  // e.g. Bristleback Quilboar Tusk 5085
    EXPECT_EQ(ClassifyJunk({1, 25, false, false, false}), Junk::Sell);
    EXPECT_EQ(ClassifyJunk({1, 0, true, false, false}), Junk::Keep);   // house material / product
    EXPECT_EQ(ClassifyJunk({1, 40, true, false, false}), Junk::Keep);  // e.g. Bolt of Linen Cloth
    EXPECT_EQ(ClassifyJunk({1, 0, false, true, false}), Junk::Keep);   // a quest in the log wants it
    EXPECT_EQ(ClassifyJunk({1, 0, false, false, true}), Junk::Keep);   // hearthstone / tool / container
    EXPECT_EQ(ClassifyJunk({1, 16, false, false, true}), Junk::Keep);  // Mining Pick (sellable, a tool)
}

TEST(SupplyArtisanUpkeep, PlanRoomSellsAllAndDestroysOnlyTheRest)
{
    std::vector<BagStack> const bags = {
        {9, 0, false, false, false},  // junk
        {5, 30, false, false, false},  // sellable
        {3, 0, false, false, false},  // junk
        {7, 40, true, false, false},  // house (bolts)
        {1, 0, false, false, false},  // junk
        {2, 0, false, true, false},   // quest in log
        {4, 0, false, false, false},  // junk
        {6, 0, false, false, true},   // hearthstone
    };
    // Full backpack, want 4: the sale frees one, three junk stacks go (ascending guid).
    RoomPlan const full = PlanRoom(bags, 0, 4);
    EXPECT_EQ(full.sell, (std::vector<std::uint32_t>{5}));
    EXPECT_EQ(full.destroy, (std::vector<std::uint32_t>{1, 3, 4}));
    // Two free: the sale covers one, one junk stack goes.
    RoomPlan const two = PlanRoom(bags, 2, 4);
    EXPECT_EQ(two.sell, (std::vector<std::uint32_t>{5}));
    EXPECT_EQ(two.destroy, (std::vector<std::uint32_t>{1}));
    // Enough room: nothing sold, nothing destroyed.
    RoomPlan const ok = PlanRoom(bags, 4, 4);
    EXPECT_TRUE(ok.sell.empty());
    EXPECT_TRUE(ok.destroy.empty());
    // Not enough junk: every junk stack goes, still short (the bag step's case).
    RoomPlan const lean = PlanRoom(bags, 0, 12);
    EXPECT_EQ(lean.destroy, (std::vector<std::uint32_t>{1, 3, 4, 9}));
}

TEST(SupplyArtisanUpkeep, SpaceTargetsAndBag)
{
    EXPECT_EQ(RoomTarget(4, false), 4u);
    EXPECT_EQ(RoomTarget(4, true), 4u);
    EXPECT_EQ(RoomTarget(0, false), 0u);  // off
    EXPECT_EQ(RoomTarget(0, true), 1u);   // a blocked craft still wants a slot
    EXPECT_TRUE(WantsBag(false, 2, 4));
    EXPECT_FALSE(WantsBag(true, 2, 4));   // a bag worn: never a second one here
    EXPECT_FALSE(WantsBag(false, 4, 4));  // the junk made enough room
}

TEST(SupplyArtisanUpkeep, LineItemsAreHouseItems)
{
    ProductLine const& potions = LineOf(Line::Potions);
    EXPECT_TRUE(LineItem(potions, 118));   // Minor Healing Potion (product)
    EXPECT_TRUE(LineItem(potions, 2447));  // Peacebloom (routed)
    EXPECT_TRUE(LineItem(potions, 3371));  // Empty Vial (vendor)
    EXPECT_TRUE(LineItem(potions, 929));   // Healing Potion
    EXPECT_FALSE(LineItem(potions, kLinen));
    EXPECT_FALSE(LineItem(potions, 5085));  // quest junk
    EXPECT_FALSE(LineItem(LineOf(Line::Bags), kLinen));  // bags: the bespoke tier chain (HouseMaterial), no table
}

TEST(SupplyArtisanUpkeep, WireAndDefaults)
{
    EXPECT_STREQ(ReasonName(Reason::Junk), "junk");
    EXPECT_EQ(static_cast<int>(Reason::Junk), 14);
    Params const p;
    EXPECT_EQ(p.artisanFreeSlots, 4u);
    EXPECT_EQ(p.artisanMinLevel, 10u);
    EXPECT_EQ(kStateVersion, 8u);
    EXPECT_EQ(kPouch, 4496u);
}

// soak-s47-full-r1: Player::HasQuestForItem(item, 0, true) said yes for every stack of the Horde tailor (an unused
// objective slot of any logged quest reads as met), so make-room kept its whole backpack and it stalled.
TEST(SupplyArtisanUpkeep, QuestItemsAreOnlyTheQuestsOwn)
{
    std::uint32_t const required[6] = {5481, 0, 0, 0, 0, 0};  // Satyr Horns, five unused slots
    std::uint32_t const drops[4] = {0, 0, 0, 0};
    EXPECT_TRUE(QuestWantsItem(5481, 0, required, drops));
    EXPECT_FALSE(QuestWantsItem(9779, 0, required, drops));  // Bandit Cloak: sellable junk, not the quest's
    EXPECT_FALSE(QuestWantsItem(0, 0, required, drops));     // no item never matches an unused slot
    EXPECT_TRUE(QuestWantsItem(16205, 16205, required, drops));  // the item the quest handed out
    std::uint32_t const drop[4] = {0, 5059, 0, 0};
    EXPECT_TRUE(QuestWantsItem(5059, 0, required, drop));  // a source drop
}

// soak-s47-full-r1: the Alliance tailor (skill 104) works toward Woolen Bags; its squad still farmed linen.
TEST(SupplyTiers, SquadDemandFollowsTheArtisansTier)
{
    // Goal = the highest known wanted bag tier, craftable or not.
    EXPECT_EQ(PickGoal({{true, 5, 2}, {true, 5, 0}, {false, 5, 0}}), 1);
    EXPECT_EQ(PickGoal({{true, 0, 2}, {true, 0, 0}, {false, 0, 0}}), kNoTier);
    // 104 on Woolen Bags, no skill-up stocked: wool first, linen done, silk out of reach.
    EXPECT_EQ(ClothDemandOf(0, 104, 1, kNoTier), ClothDemand::Done);
    EXPECT_EQ(ClothDemandOf(1, 104, 1, kNoTier), ClothDemand::First);
    EXPECT_EQ(ClothDemandOf(2, 104, 1, kNoTier), ClothDemand::Done);
    // 130 on Woolen Bags, silk reachable: silk stays normal (stock for the next tier).
    EXPECT_EQ(ClothDemandOf(2, 130, 1, kNoTier), ClothDemand::Normal);
    // Linen goal, wool bolts the skill-up: both first.
    EXPECT_EQ(ClothDemandOf(0, 78, 0, 1), ClothDemand::First);
    EXPECT_EQ(ClothDemandOf(1, 78, 0, 1), ClothDemand::First);
    // Nothing worked (no want, nothing stocked): every reachable tier normal, as before.
    EXPECT_EQ(ClothDemandOf(0, 21, kNoTier, kNoTier), ClothDemand::Normal);
    EXPECT_EQ(ClothDemandOf(1, 21, kNoTier, kNoTier), ClothDemand::Done);  // out of reach
    EXPECT_EQ(ClothDemandOf(0, 0, kNoTier, kNoTier), ClothDemand::Normal);  // artisan offline: linen, as before
}

// soak-s54-full-r1: the Weavers posted cloth_gear orders on wool / linen bolts; the bag line's tier gate starved them.
TEST(SupplyTiers, GearOrdersKeepTheirClothInDemand)
{
    RecipeTable const g{kTailorGear, static_cast<std::uint8_t>(std::size(kTailorGear))};
    std::uint8_t const gloves = TierOf(g, 4310), bracers = TierOf(g, 4308);  // 3 wool bolts; 3 linen bolts
    ASSERT_NE(gloves, kNoTier);
    ASSERT_NE(bracers, kNoTier);
    std::unordered_map<std::uint32_t, std::uint32_t> held{{2997, 1}, {kWool, 5}};  // 1 wool bolt, 5 wool
    auto have = [&](std::uint32_t item) { auto const it = held.find(item); return it == held.end() ? 0u : it->second; };
    // 2 gloves = 6 bolts = 18 wool, less 1 bolt (3) and 5 wool; 1 bracers = 3 bolts = 6 linen; no silk.
    std::array<std::uint32_t, kTierCount> const s = GearClothShort(g, {{gloves, 2, 0}, {bracers, 1, 0}}, have);
    EXPECT_EQ(s[0], 6u);
    EXPECT_EQ(s[1], 10u);
    EXPECT_EQ(s[2], 0u);
    EXPECT_EQ(GearClothShort(g, {}, have)[1], 0u);                      // no order: no gear demand
    EXPECT_EQ(GearClothShort(g, {{gloves, 1, 0}}, have)[1], 1u);        // 9 wool less 8 held
    // Bags done with wool (silk bag goal at 150): wool Done without the gear order, Normal with it.
    EXPECT_EQ(ClothDemandOf(1, 150, 2, kNoTier), ClothDemand::Done);
    EXPECT_EQ(ClothDemandOf(1, 150, 2, kNoTier, s[1]), ClothDemand::Normal);
    // A wool skill-up with linen bracers on order: linen stays in demand.
    EXPECT_EQ(ClothDemandOf(0, 112, kNoTier, 1), ClothDemand::Done);
    EXPECT_EQ(ClothDemandOf(0, 112, kNoTier, 1, s[0]), ClothDemand::Normal);
    // Out of reach stays Done (the bolt cannot be made).
    EXPECT_EQ(ClothDemandOf(2, 104, 1, kNoTier, 40), ClothDemand::Done);
}

// soak-s47-full-r1: the Alliance rep listed wool while its tailor was below 75.
TEST(SupplyMarket, NextTierIsAReserveNotSurplus)
{
    EXPECT_EQ(ReachTier(1), 0u);
    EXPECT_EQ(ReachTier(75), 1u);
    EXPECT_EQ(ReachTier(125), 2u);
    EXPECT_FALSE(Listable(1, 60));  // wool: the next tier
    EXPECT_TRUE(Listable(2, 60));   // silk: two above
    EXPECT_FALSE(Listable(2, 104));
    EXPECT_FALSE(Listable(0, 60));
}

// soak-s47-full-r1: the only wool on the Alliance house was the rep's own listings, which its buyouts skip.
TEST(SupplyMarket, OwnListingsOfAWantedItemComeBackFirst)
{
    std::vector<MarketWant> wants = {{kWool, 5, 33}, {kSilk, 10, 38}};
    std::vector<MarketListing> const cancels =
        PlanMarketCancels({{9, kWool, 2, 198}, {4, kWool, 4, 396}, {6, kLinen, 20, 400}, {7, kWool, 5, 495}}, wants);
    ASSERT_EQ(cancels.size(), 2u);
    EXPECT_EQ(cancels[0].id, 4u);  // ascending id; 4 + 2 = 6 >= 5: the last overshoots
    EXPECT_EQ(cancels[1].id, 7u);
    EXPECT_EQ(wants[0].units, 0u);  // covered: its buyouts plan nothing
    EXPECT_EQ(wants[1].units, 10u);
    EXPECT_TRUE(PlanMarketBuys({{11, kWool, 5, 100}}, wants, 400, 1000).empty());
    EXPECT_STREQ(ReasonName(Reason::Cancel), "cancel");
    EXPECT_EQ(static_cast<int>(Reason::Cancel), 15);
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

// ---- rep storage (AutoWow.Supply.RepStore; soak-s48-full-r1) ----

TEST(SupplyRepStore, DefaultsOn)
{
    Params const p;
    EXPECT_TRUE(p.repStore);
    EXPECT_EQ(p.repMailCap, 80u);
    EXPECT_EQ(p.repKeep, 60u);
}

TEST(SupplyRepStore, RepsRankBeforeEveryMember)
{
    Member rep{90, 1, 0, 0};
    rep.rep = true;
    Member full{95, 4, 0, 4};  // a rep with its want covered is still dropped
    full.rep = true;
    std::vector<Member> const ranked = RankNeeds({{10, 4, 0, 0}, rep, full, {5, 4, 0, 0}});
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].guid, 90u);
    EXPECT_EQ(ranked[1].guid, 5u);
    EXPECT_EQ(ranked[2].guid, 10u);
    // Its share of the delivery comes first.
    std::vector<Delivery> const d = PlanDeliveries(ranked, 2);
    ASSERT_EQ(d.size(), 2u);
    EXPECT_EQ(d[0].guid, 90u);
    EXPECT_EQ(d[0].bags, 1u);
}

TEST(SupplyRepStore, WearsTheBiggestBagsFirst)
{
    // Woolen 8, linen 6, silk pack 10: the two biggest into two empty slots; ties the lower guid.
    EXPECT_EQ(BagsToWear({{7, 6}, {3, 8}, {9, 10}, {2, 8}}, 2), (std::vector<std::uint32_t>{9, 2}));
    EXPECT_EQ(BagsToWear({{7, 6}}, 4), (std::vector<std::uint32_t>{7}));
    EXPECT_TRUE(BagsToWear({{7, 6}}, 0).empty());
    EXPECT_TRUE(BagsToWear({{7, 0}}, 4).empty());
}

TEST(SupplyRepStore, DonorsStopBelowTheCoreMailCap)
{
    EXPECT_TRUE(MailRoom(79, 80));
    EXPECT_FALSE(MailRoom(80, 80));
    EXPECT_FALSE(MailRoom(120, 80));  // soak-s48-full-r1: 120 mails at the Weavers rep
}

TEST(SupplyRepStore, StashIsDueOverThreeQuartersFull)
{
    EXPECT_FALSE(StashDue(4, 16));  // 12 of 16 used: exactly 75%
    EXPECT_TRUE(StashDue(3, 16));
    EXPECT_FALSE(StashDue(12, 16));  // soak-s48-full-r1: the rep used 4 of 16
    EXPECT_TRUE(StashDue(0, 16));
    EXPECT_FALSE(StashDue(0, 0));
}

TEST(SupplyRepStore, StashKeepsRepKeepLoosePerItem)
{
    // Wool 4 x 20 (keep 60: one stack goes), silk 20 + 20 (40 < keep + 20: none), linen 5 x 20 (two go).
    std::vector<StashStack> const loose = {{1, kWool, 20}, {2, kWool, 20}, {3, kWool, 20}, {4, kWool, 20},
                                           {5, kSilk, 20}, {6, kSilk, 20}, {11, kLinen, 20}, {12, kLinen, 20},
                                           {13, kLinen, 20}, {14, kLinen, 20}, {15, kLinen, 20}};
    EXPECT_EQ(PlanStash(loose, 60, 28), (std::vector<std::uint32_t>{1, 11, 12}));
    EXPECT_EQ(PlanStash(loose, 60, 2), (std::vector<std::uint32_t>{1, 11}));  // bank room
    EXPECT_TRUE(PlanStash(loose, 60, 0).empty());
    EXPECT_EQ(PlanStash(loose, 0, 28).size(), loose.size());
}

TEST(SupplyRepStore, RefillUnderRepKeepWithoutRefillingPastThreeQuarters)
{
    std::vector<StashStack> const loose = {{1, kWool, 20}, {2, kLinen, 20}, {3, kLinen, 20}, {4, kLinen, 20}};
    std::vector<StashStack> const banked = {{30, kWool, 20}, {31, kWool, 20}, {32, kWool, 20}, {40, kLinen, 20},
                                            {50, kSilk, 5}};
    // Wool 20 -> 40 -> 60 (two stacks), linen at keep, silk 0 -> 5 (the last may stay short: nothing more banked).
    EXPECT_EQ(PlanUnstash(banked, loose, 60, 10, 16), (std::vector<std::uint32_t>{30, 31, 50}));
    // 6 free of 16: one stack leaves 5 free (11 used, under 75%); a second would leave 4 (exactly 75%, allowed);
    // a third 3 (over): stop.
    EXPECT_EQ(PlanUnstash(banked, loose, 60, 6, 16), (std::vector<std::uint32_t>{30, 31}));
    EXPECT_EQ(PlanUnstash(banked, loose, 60, 5, 16), (std::vector<std::uint32_t>{30}));
    EXPECT_TRUE(PlanUnstash(banked, loose, 60, 4, 16).empty());
    EXPECT_TRUE(PlanUnstash(banked, loose, 60, 0, 16).empty());
    // A withdrawal never makes a deposit due (no stash / refill loop).
    EXPECT_FALSE(StashDue(4, 16));
}

// ---- throughput (lane U) ----

TEST(SupplyThroughput, ArtisanTakesDirectOnlyAtHomeWithRoom)
{
    EXPECT_TRUE(ArtisanTakes(true, true, 5, 4, true));
    EXPECT_FALSE(ArtisanTakes(false, true, 5, 4, true));  // apprentice / offline
    EXPECT_FALSE(ArtisanTakes(true, false, 5, 4, true));  // away from home
    EXPECT_FALSE(ArtisanTakes(true, true, 4, 4, true));   // at its make-room target: full
    EXPECT_FALSE(ArtisanTakes(true, true, 9, 4, false));  // mailbox at RepMailCap
    Params const p;
    EXPECT_FALSE(p.directRoutes);
    EXPECT_FALSE(p.mailPickup);
    EXPECT_FALSE(p.mailOrders);
    EXPECT_EQ(p.mailPickupYards, 40u);
}

TEST(SupplyThroughput, OrdersPriceAtBuyMaxPctWithinTheBank)
{
    // wool (sell 33) 30 units, linen (sell 13) 50 units; BuyMaxPct 400: linen 52/unit, wool 132/unit.
    std::vector<MailOrder> o = PlanOrders(7, {{kWool, 30, 33}, {kLinen, 50, 13}}, 400, 100000);
    ASSERT_EQ(o.size(), 2u);
    EXPECT_EQ(o[0].item, kLinen);  // ascending item
    EXPECT_EQ(o[0].units, 50u);
    EXPECT_EQ(o[0].unitPrice, 52u);
    EXPECT_EQ(o[0].rep, 7u);
    EXPECT_EQ(o[1].unitPrice, 132u);
    // Bank 2000: linen takes 38 units (1976), wool none left.
    o = PlanOrders(7, {{kWool, 30, 33}, {kLinen, 50, 13}}, 400, 2000);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].units, 38u);
    EXPECT_TRUE(PlanOrders(7, {{kLinen, 10, 0}}, 400, 100000).empty());  // no vendor value
    EXPECT_TRUE(PlanOrders(7, {{kLinen, 0, 13}}, 400, 100000).empty());
}

TEST(SupplyThroughput, FillsNeverOverfillAnOrder)
{
    EXPECT_EQ(PickWithin({{9, 20}, {3, 5}, {4, 20}}, 30), (std::vector<std::uint32_t>{3, 4}));  // 25 of 30
    EXPECT_TRUE(PickWithin({{1, 20}}, 19).empty());
    EXPECT_EQ(PickWithin({{1, 20}}, 20), (std::vector<std::uint32_t>{1}));
    std::vector<Stack> many;
    for (std::uint32_t g = 1; g <= 20; ++g)
        many.push_back({g, 1});
    EXPECT_EQ(PickWithin(many, 100).size(), kMaxMailStacks);
}

TEST(SupplyThroughput, CodIsAcceptedOnlyAsThePendingFill)
{
    CodPending const pending{20, 1040};
    EXPECT_EQ(DecideCod(pending, 20, 1040, 5000, true), CodVerdict::Accept);
    EXPECT_EQ(DecideCod(pending, 10, 520, 5000, true), CodVerdict::Accept);   // part of it
    EXPECT_EQ(DecideCod(pending, 20, 1040, 5000, false), CodVerdict::Wait);   // no bag room
    EXPECT_EQ(DecideCod(pending, 21, 1040, 5000, true), CodVerdict::Return);  // more than mailed
    EXPECT_EQ(DecideCod(pending, 20, 1041, 5000, true), CodVerdict::Return);  // over the price
    EXPECT_EQ(DecideCod(pending, 20, 1040, 1039, true), CodVerdict::Return);  // over the budget
    EXPECT_EQ(DecideCod(CodPending{}, 1, 1, 5000, true), CodVerdict::Return);  // unordered
    EXPECT_EQ(DecideCod(pending, 0, 0, 5000, true), CodVerdict::Return);
}

// soak-s48-full-r1: the rep -> artisan feed and the artisan -> rep delivery carry every item in one mail (the
// runtime packs its picks in item order the way PlanRoute does with unlimited rooms), at most MAX_MAIL_ITEMS stacks.
TEST(SupplyRepStore, OneMailCarriesEveryItemUpTo12Stacks)
{
    std::uint32_t const all = 0xFFFFFFFFu;
    RoutePlan const two = PlanRoute({{{1, 20}, {2, 20}}, {{7, 2}}}, {all, all});
    EXPECT_EQ(two.picks, (std::vector<std::uint32_t>{1, 2, 7}));
    EXPECT_EQ(two.units, (std::vector<std::uint32_t>{40, 2}));
    std::vector<Stack> ten, five;
    for (std::uint32_t g = 1; g <= 10; ++g)
        ten.push_back({g, 1});
    for (std::uint32_t g = 21; g <= 25; ++g)
        five.push_back({g, 1});
    RoutePlan const capped = PlanRoute({ten, five}, {all, all});
    EXPECT_EQ(capped.picks.size(), kMaxMailStacks);
    EXPECT_EQ(capped.units, (std::vector<std::uint32_t>{10, 2}));
}

// ---- lane V: need-driven production (DemandOnly) and gear lines ----

TEST(SupplyDemand, SkillupPrefersARecipeWithAConsumer)
{
    // skill 60: linen bolt grey (50); linen bag (no member wants one) and wool bolt known; the bag is cheaper.
    std::vector<SkillupOption> opts = {
        {2963, 0, false, true, 50, true, 26, true},
        {3755, 0, true, true, 105, true, 90, false},   // nobody wants a Linen Bag: its output would be sold
        {2964, 1, false, true, 105, true, 99, true},   // a wanted Woolen Bag / wool gear piece uses the bolt
    };
    EXPECT_EQ(PickSkillup(60, opts, true), 1);                // flag off: the cheapest (the useless bag)
    EXPECT_EQ(PickSkillupFor(60, opts, true, false), 1);      // off == PickSkillup
    EXPECT_EQ(PickSkillupFor(60, opts, true, true), 2);       // on: the recipe with a consumer
    // A member wants the Linen Bag now: the bag has a consumer and is cheapest.
    opts[1].consumer = true;
    EXPECT_EQ(PickSkillupFor(60, opts, true, true), 1);
    // No output has a consumer: the last resort is the cheapest (sold later as `waste`).
    for (SkillupOption& o : opts)
        o.consumer = false;
    EXPECT_EQ(PickSkillupFor(60, opts, true, true), 1);
    // A consumer recipe the house holds no cloth for loses to a stocked consumer-less one (needStock).
    opts[2].consumer = true;
    opts[2].stocked = false;
    EXPECT_EQ(PickSkillupFor(60, opts, true, true), 1);
    EXPECT_EQ(PickSkillupFor(60, opts, false, true), 2);  // the market target ignores stock
}

TEST(SupplyDemand, WasteAndConsumerWire)
{
    EXPECT_STREQ(ReasonName(Reason::Waste), "waste");
    EXPECT_EQ(static_cast<int>(Reason::Waste), 16);
    EXPECT_EQ(static_cast<int>(Reason::Cancel), 15);
    EXPECT_EQ(SaleReason(false), Reason::Surplus);
    EXPECT_EQ(SaleReason(true), Reason::Waste);
    EXPECT_EQ(ConsumerField(70001), ",\"consumer\":70001");
    EXPECT_EQ(ConsumerField(0), ",\"consumer\":0");
    Params const p;
    EXPECT_FALSE(p.demandOnly);
    EXPECT_EQ(p.repStockPerItem, 2u);
    EXPECT_EQ(p.gearMaxOrder, 4u);
    EXPECT_EQ(p.gearPayPct, 200u);
    EXPECT_EQ(p.lines, 1u);  // gear lines are off unless Products names them
}

TEST(SupplyGear, CatalogLinesAndProducts)
{
    EXPECT_EQ(static_cast<int>(Line::ClothGear), 2);
    EXPECT_EQ(static_cast<int>(Line::MailGear), 3);
    EXPECT_EQ(static_cast<int>(Line::LeatherGear), 4);
    ASSERT_EQ(kLineCount, 6u);
    std::uint8_t mask = 0;
    EXPECT_TRUE(ParseProducts("bags,cloth_gear", mask));
    EXPECT_EQ(mask, 5u);
    EXPECT_TRUE(ParseProducts("leather_gear", mask));
    EXPECT_EQ(mask, 16u);
    EXPECT_EQ(LineField(Line::ClothGear), ",\"line\":\"cloth_gear\"");
    ProductLine const& cloth = LineOf(Line::ClothGear);
    EXPECT_STREQ(cloth.house, "Weavers");
    EXPECT_EQ(cloth.skillLine, 197u);
    EXPECT_EQ(cloth.need, NeedRule::GearSlots);
    EXPECT_EQ(cloth.consumer, Consumer::EquipGear);
    EXPECT_EQ(cloth.tierCount, 0u);  // bespoke runtime: never a LineTick line
    EXPECT_STREQ(LineOf(Line::LeatherGear).house, "Tanners");
    EXPECT_EQ(LineOf(Line::LeatherGear).skillLine, 165u);
    EXPECT_EQ(LineOf(Line::MailGear).gearCount, 0u);  // data hook: turns itself off at load
    EXPECT_STREQ(LineOf(Line::MailGear).house, "Smiths");
}

TEST(SupplyGear, TablesMatchTheWorldDb)
{
    std::vector<std::uint32_t> const vendor = {2320, 2321, 4291, 2604, 2605, 6260, 4340};
    for (Line const l : {Line::ClothGear, Line::LeatherGear})
    {
        RecipeTable const g = GearTable(LineOf(l));
        ASSERT_GT(g.tierCount, 2u);
        for (std::size_t i = 0; i < g.tierCount; ++i)
            for (Reagent const& r : g.tiers[i].reagents)
            {
                if (!r.item)
                    continue;
                EXPECT_GT(r.count, 0u);
                if (r.source == Source::Craft)  // every intermediate is a table recipe (a bolt, Medium / Heavy Leather)
                {
                    std::uint8_t const sub = TierOf(g, r.item);
                    ASSERT_NE(sub, kNoTier);
                    EXPECT_EQ(g.tiers[sub].reqLevel, 0u);
                }
                if (r.source == Source::Vendor)  // the trade-supplies vendors near home sell all of them
                {
                    EXPECT_NE(std::find(vendor.begin(), vendor.end(), r.item), vendor.end());
                }
                EXPECT_NE(r.source, Source::Market);
            }
    }
    RecipeTable const cloth = GearTable(LineOf(Line::ClothGear));
    ASSERT_EQ(cloth.tierCount, 24u);
    EXPECT_EQ(cloth.tiers[0].spell, 2963u);  // Bolt of Linen Cloth, learned with the skill
    EXPECT_EQ(cloth.tiers[0].skill, 1u);
    EXPECT_EQ(cloth.tiers[1].product, 2997u);  // Bolt of Woolen Cloth
    EXPECT_EQ(cloth.tiers[2].product, 4305u);  // Bolt of Silk Cloth
    std::uint8_t const pants = TierOf(cloth, 4343);  // Brown Linen Pants: tailoring 30, RequiredLevel 5
    ASSERT_NE(pants, kNoTier);
    EXPECT_EQ(cloth.tiers[pants].spell, 3914u);
    EXPECT_EQ(cloth.tiers[pants].skill, 30u);
    EXPECT_EQ(cloth.tiers[pants].grey, 90u);
    EXPECT_EQ(cloth.tiers[pants].reqLevel, 5u);
    EXPECT_EQ(cloth.tiers[pants].reagents[0].item, 2996u);
    EXPECT_EQ(cloth.tiers[pants].reagents[0].count, 2u);
    std::uint8_t const top = TierOf(cloth, 7062);  // Crimson Silk Pantaloons: tailoring 195, RequiredLevel 34
    ASSERT_NE(top, kNoTier);
    EXPECT_EQ(cloth.tiers[top].skill, 195u);
    EXPECT_EQ(cloth.tiers[top].reqLevel, 34u);
    RecipeTable const leather = GearTable(LineOf(Line::LeatherGear));
    ASSERT_EQ(leather.tierCount, 16u);
    EXPECT_EQ(leather.tiers[TierOf(leather, 3719)].reagents[0].item, 4234u);  // Hillman's Cloak <- Heavy Leather
    EXPECT_TRUE(LineItem(cloth, 2605));    // Green Dye (vendor): kept by the artisan's make-room
    EXPECT_TRUE(LineItem(cloth, 7046));    // Azure Silk Pants (product)
    EXPECT_FALSE(LineItem(cloth, 2318));   // Light Leather is the Tanners'
    EXPECT_TRUE(LineItem(leather, 2318));
}

TEST(SupplyGear, CastsAndLacksWalkTheIntermediates)
{
    RecipeTable const g = GearTable(LineOf(Line::ClothGear));
    std::uint8_t const pants = TierOf(g, 4343);  // 2 linen bolts + coarse thread
    std::unordered_map<std::uint32_t, std::uint32_t> held{{kLinen, 7}, {2996, 1}};
    auto have = [&](std::uint32_t item)
    {
        auto const it = held.find(item);
        return it == held.end() ? 0u : it->second;
    };
    EXPECT_EQ(Casts(g, pants, have), 2u);  // 1 bolt + 3 more from 7 linen = 4 bolts; thread is bought
    // Three pants: 6 bolts, 1 held, 5 to make = 10 linen, 3 short; 3 thread from the vendor.
    std::vector<Lack> const lack = Lacks(g, pants, 3, have);
    ASSERT_EQ(lack.size(), 2u);
    EXPECT_EQ(lack[0].item, kLinen);
    EXPECT_EQ(lack[0].units, 3u);
    EXPECT_EQ(lack[0].source, Source::Route);
    EXPECT_EQ(lack[1].item, 2320u);
    EXPECT_EQ(lack[1].units, 3u);
    EXPECT_EQ(lack[1].source, Source::Vendor);
    // Next cast toward the pants: a bolt (one held, two needed); with two bolts and thread, the pants.
    EXPECT_EQ(NextCast(g, pants, have), 0u);
    held[2996] = 2;
    EXPECT_EQ(NextCast(g, pants, have), kNoTier);  // no thread yet
    held[2320] = 1;
    EXPECT_EQ(NextCast(g, pants, have), pants);
    // Leather: Hillman's Cloak <- Heavy Leather <- Medium Leather <- Light Leather (three levels).
    RecipeTable const l = GearTable(LineOf(Line::LeatherGear));
    std::unordered_map<std::uint32_t, std::uint32_t> skins{{2318, 100}};
    auto hides = [&](std::uint32_t item)
    {
        auto const it = skins.find(item);
        return it == skins.end() ? 0u : it->second;
    };
    EXPECT_EQ(Casts(l, TierOf(l, 3719), hides), 1u);  // 100 light = 25 medium = 5 heavy = 1 cloak
}

TEST(SupplyGear, NeedsRankPriorityThenWorstGeared)
{
    std::vector<GearNeed> const ranked = RankGearNeeds({
        {30, 3, 7, kNoPriority, 120},
        {10, 4, 5, kNoPriority, 300},
        {20, 3, 7, kNoPriority, 120},  // same gear as 30: the lower guid first
        {90, 5, 9, 1, 900},            // priority list, second place
        {80, 6, 4, 0, 950},            // priority list, first place: before everyone however well geared
        {20, 8, 1, kNoPriority, 120},  // 20's head before its legs (slot order)
    });
    ASSERT_EQ(ranked.size(), 6u);
    EXPECT_EQ(ranked[0].guid, 80u);
    EXPECT_EQ(ranked[1].guid, 90u);
    EXPECT_EQ(ranked[2].guid, 20u);
    EXPECT_EQ(ranked[2].slot, 1u);
    EXPECT_EQ(ranked[3].guid, 20u);
    EXPECT_EQ(ranked[3].slot, 7u);
    EXPECT_EQ(ranked[4].guid, 30u);
    EXPECT_EQ(ranked[5].guid, 10u);
    std::vector<std::uint32_t> list;
    EXPECT_TRUE(ParseGuids(" 80, 90 ", list));
    EXPECT_EQ(list, (std::vector<std::uint32_t>{80, 90}));
    EXPECT_EQ(PriorityOf(list, 90), 1u);
    EXPECT_EQ(PriorityOf(list, 20), kNoPriority);
    EXPECT_FALSE(ParseGuids("80,x", list));
    EXPECT_EQ(list.size(), 2u);  // untouched
    EXPECT_TRUE(ParseGuids("", list));
    EXPECT_TRUE(list.empty());
}

TEST(SupplyGear, OrdersServeTheTopNeedsPlusStockNeverMore)
{
    std::vector<GearNeed> const ranked = {
        {80, 5, 7, 0, 10}, {81, 5, 7, 1, 10}, {82, 9, 5, kNoPriority, 20}, {83, 5, 7, kNoPriority, 30},
        {84, 11, 1, kNoPriority, 40},  // beyond the top 4: not ordered this round
    };
    std::vector<std::uint32_t> held(24, 0);
    held[9] = 1;  // the rep holds one finished recipe-9 piece
    std::vector<GearOrder> const o = PlanGearOrders(ranked, held, 4, 2);
    ASSERT_EQ(o.size(), 2u);
    EXPECT_EQ(o[0].recipe, 5u);
    EXPECT_EQ(o[0].units, 3u + 2u);  // three needs + RepStockPerItem
    EXPECT_EQ(o[0].consumer, 80u);   // its first ranked need
    EXPECT_EQ(o[1].recipe, 9u);
    EXPECT_EQ(o[1].units, 1u + 2u - 1u);
    EXPECT_EQ(o[1].consumer, 82u);
    // The house already holds demand + stock: nothing to make.
    held[5] = 5;
    EXPECT_EQ(PlanGearOrders(ranked, held, 4, 2).size(), 1u);
    // No need: no order, whatever the stock target (never stock for a recipe nobody wants).
    EXPECT_TRUE(PlanGearOrders({}, held, 4, 2).empty());
    // Stock 0: exactly the demand.
    EXPECT_EQ(PlanGearOrders(ranked, std::vector<std::uint32_t>(24, 0), 4, 0)[0].units, 3u);
}

TEST(SupplyGear, TargetIsTheFirstShortEntryTheHouseCanCast)
{
    EXPECT_EQ(PickGearTarget({2, 3, 1}, {0, 4, 1}), 1);  // entry 0 short but no materials: 1 can be cast now
    EXPECT_EQ(PickGearTarget({2, 3}, {0, 0}), 0);        // nothing castable: the first short one
    EXPECT_EQ(PickGearTarget({0, 3}, {5, 0}), 1);        // entry 0 done
    EXPECT_EQ(PickGearTarget({0, 0}, {5, 5}), -1);
    EXPECT_EQ(PickGearTarget({}, {}), -1);
}

TEST(SupplyGear, DeliveriesGiveOnePiecePerNeedInRankOrder)
{
    std::vector<GearNeed> const ranked = {{80, 5, 7}, {81, 9, 5}, {82, 5, 7}, {83, 5, 7}};
    std::vector<GearDelivery> const d = PlanGearDeliveries(ranked, {{5, 301}, {9, 300}, {5, 299}, {12, 298}});
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0].need, 0u);
    EXPECT_EQ(d[0].item, 299u);  // lowest guid of its recipe
    EXPECT_EQ(d[1].need, 1u);
    EXPECT_EQ(d[1].item, 300u);
    EXPECT_EQ(d[2].need, 2u);
    EXPECT_EQ(d[2].item, 301u);  // need 3 (83) waits: no piece left; recipe 12 has no need (stays with the sender)
    EXPECT_TRUE(PlanGearDeliveries({}, {{5, 1}}).empty());
}

TEST(SupplyGear, RecipesRankByItemLevelLeavingIntermediatesOut)
{
    RecipeTable const g = GearTable(LineOf(Line::ClothGear));
    std::vector<std::uint32_t> ilvl(g.tierCount, 0);
    ilvl[3] = 8;   // Brown Linen Vest 2385
    ilvl[4] = 9;   // Linen Belt 8776
    ilvl[5] = 10;  // Brown Linen Pants 3914
    ilvl[6] = 10;  // Brown Linen Robe 7623
    EXPECT_EQ(RankGearRecipes(g, ilvl), (std::vector<std::uint8_t>{5, 6, 4, 3}));  // 10 (spell 3914 < 7623), 9, 8
}

// ---- lane AA Tinkers (Products eng) ----

TEST(SupplyEng, CatalogLineAndStone)
{
    EXPECT_EQ(static_cast<int>(Line::Engineering), 5);
    std::uint8_t mask = 0;
    EXPECT_TRUE(ParseProducts("eng", mask));
    EXPECT_EQ(mask, 32u);
    EXPECT_TRUE(ParseProducts("bags,leather_gear,eng", mask));
    EXPECT_EQ(mask, 1u + 16u + 32u);
    EXPECT_EQ(LineField(Line::Engineering), ",\"line\":\"eng\"");
    ProductLine const& e = LineOf(Line::Engineering);
    EXPECT_STREQ(e.house, "Tinkers");
    EXPECT_STREQ(e.key, "Engineering");
    EXPECT_EQ(e.skillLine, 202u);
    EXPECT_EQ(e.need, NeedRule::AmmoStock);
    EXPECT_EQ(e.consumer, Consumer::LoadAmmo);
    EXPECT_EQ(e.tierCount, 0u);  // gear runtime: never a LineTick line
    EXPECT_EQ(e.gearCount, 9u);
    EXPECT_STREQ(e.learn, "4039,4040,4041,12657,2581,2582,3568,10249");  // engineering + mining ranks
    EXPECT_EQ(kStone[0], 2835u);
    EXPECT_EQ(kStone[1], 2836u);
    EXPECT_EQ(kStone[2], 2838u);
    EXPECT_EQ(Params{}.ammoTarget, 1000u);
    EXPECT_EQ(kAmmoSlot, 19u);  // past every equipment slot (EQUIPMENT_SLOT_END)
}

TEST(SupplyEng, TableIntegrity)
{
    RecipeTable const g = GearTable(LineOf(Line::Engineering));
    std::vector<std::uint32_t> const shots = {8067, 8068, 8069};  // Crafted Light / Heavy / Solid Shot
    std::vector<std::uint32_t> spells;
    for (std::size_t i = 0; i < g.tierCount; ++i)
    {
        LineTier const& t = g.tiers[i];
        EXPECT_EQ(std::find(spells.begin(), spells.end(), t.spell), spells.end());
        spells.push_back(t.spell);
        EXPECT_GE(t.skill, 1u);
        EXPECT_LT(t.skill, t.grey);  // a skill-up at the learning skill
        bool const shot = std::find(shots.begin(), shots.end(), t.product) != shots.end();
        EXPECT_EQ(shot, t.reqLevel > 0);  // intermediates (RequiredLevel 0) and shot only: no unconsumed product
        for (Reagent const& r : t.reagents)
        {
            if (!r.item)
                continue;
            EXPECT_EQ(r.count, 1u);
            if (r.source == Source::Craft)  // every intermediate is a table recipe, listed before its user
            {
                std::uint8_t const sub = TierOf(g, r.item);
                ASSERT_NE(sub, kNoTier);
                EXPECT_EQ(g.tiers[sub].reqLevel, 0u);
                EXPECT_LT(sub, i);
            }
            else  // routed raw: ore (House.Ore) or stone (House.Stone); nothing bought
            {
                EXPECT_EQ(r.source, Source::Route);
                bool const raw = std::find(std::begin(kOre), std::end(kOre), r.item) != std::end(kOre) ||
                                 std::find(std::begin(kStone), std::end(kStone), r.item) != std::end(kStone);
                EXPECT_TRUE(raw) << r.item;
            }
        }
    }
    // Checked against the 3.3.5 world DB / Spell.dbc: Crafted Light Shot 3920 = Rough Blasting Powder + Copper Bar,
    // learned with the skill, grey 60, RequiredLevel 5.
    std::uint8_t const light = TierOf(g, 8067);
    ASSERT_NE(light, kNoTier);
    EXPECT_EQ(g.tiers[light].spell, 3920u);
    EXPECT_EQ(g.tiers[light].skill, 1u);
    EXPECT_EQ(g.tiers[light].grey, 60u);
    EXPECT_EQ(g.tiers[light].reqLevel, 5u);
    EXPECT_EQ(g.tiers[light].reagents[0].item, 4357u);
    EXPECT_EQ(g.tiers[light].reagents[1].item, 2840u);
    std::uint8_t const solid = TierOf(g, 8069);  // Crafted Solid Shot 3947: Heavy Blasting Powder + Bronze Bar
    ASSERT_NE(solid, kNoTier);
    EXPECT_EQ(g.tiers[solid].skill, 125u);
    EXPECT_EQ(g.tiers[solid].reqLevel, 30u);
    EXPECT_EQ(g.tiers[solid].reagents[1].item, 2841u);
    EXPECT_EQ(g.tiers[TierOf(g, 2840)].spell, 2657u);  // Smelt Copper, learned with Mining
    EXPECT_TRUE(LineItem(g, 2770));   // Copper Ore: kept by the artisan's make-room
    EXPECT_TRUE(LineItem(g, 2835));   // Rough Stone
    EXPECT_FALSE(LineItem(g, 2318));  // no leather: goggles dropped
}

TEST(SupplyEng, RecipeSelectionAndCasts)
{
    RecipeTable const g = GearTable(LineOf(Line::Engineering));
    std::vector<std::uint32_t> ilvl(g.tierCount, 0);  // item levels as the load reads them: shot 10 / 20 / 35
    ilvl[TierOf(g, 8067)] = 10;
    ilvl[TierOf(g, 8068)] = 20;
    ilvl[TierOf(g, 8069)] = 35;
    std::vector<std::uint8_t> const rank = RankGearRecipes(g, ilvl);
    ASSERT_EQ(rank.size(), 3u);  // intermediates left out
    EXPECT_EQ(g.tiers[rank[0]].product, 8069u);
    EXPECT_EQ(g.tiers[rank[2]].product, 8067u);
    std::unordered_map<std::uint32_t, std::uint32_t> held{{2770, 3}, {2835, 5}};
    auto have = [&](std::uint32_t item)
    {
        auto const it = held.find(item);
        return it == held.end() ? 0u : it->second;
    };
    std::uint8_t const light = TierOf(g, 8067);
    EXPECT_EQ(Casts(g, light, have), 3u);  // 5 powder from stone, 3 bars from ore
    // Four casts lack one ore; powder and bars are made, nothing bought.
    std::vector<Lack> const lack = Lacks(g, light, 4, have);
    ASSERT_EQ(lack.size(), 1u);
    EXPECT_EQ(lack[0].item, 2770u);
    EXPECT_EQ(lack[0].units, 1u);
    EXPECT_EQ(lack[0].source, Source::Route);
    // Next cast: the powder (first reagent), then the smelt (the forge trip), then the shot.
    EXPECT_EQ(NextCast(g, light, have), TierOf(g, 4357));
    held[4357] = 1;
    EXPECT_EQ(NextCast(g, light, have), TierOf(g, 2840));
    held[2840] = 1;
    EXPECT_EQ(NextCast(g, light, have), light);
    // Solid Shot walks three levels: ore -> copper / tin bar -> bronze bar.
    std::unordered_map<std::uint32_t, std::uint32_t> ores{{2770, 4}, {2771, 4}, {2838, 3}};
    auto rock = [&](std::uint32_t item)
    {
        auto const it = ores.find(item);
        return it == ores.end() ? 0u : it->second;
    };
    EXPECT_EQ(Casts(g, TierOf(g, 8069), rock), 3u);  // heavy stone limits (bronze counted one per cast: conservative)
    EXPECT_EQ(NextCast(g, TierOf(g, 8069), rock), TierOf(g, 4377));
}

TEST(SupplyEng, AmmoNeedAndLoad)
{
    // hunter, gun, bullets, level, reqLevel, damage, loaded damage, held, target
    EXPECT_TRUE(WantsAmmo(true, true, true, 10, 5, 4, 3, 400, 1000));    // Crafted Light Shot over vendor Light Shot
    EXPECT_TRUE(WantsAmmo(true, true, true, 10, 5, 4, 0, 0, 1000));      // nothing loaded / left
    EXPECT_TRUE(WantsAmmo(true, true, true, 10, 5, 4, 4, 999, 1000));    // as strong: still wanted when short
    EXPECT_FALSE(WantsAmmo(true, true, true, 10, 5, 4, 3, 1000, 1000));  // stocked
    EXPECT_FALSE(WantsAmmo(true, true, true, 16, 5, 4, 7, 0, 1000));     // Heavy Shot loaded: a weaker shot never
    EXPECT_FALSE(WantsAmmo(true, true, true, 4, 5, 4, 0, 0, 1000));      // below RequiredLevel
    EXPECT_FALSE(WantsAmmo(true, false, true, 10, 5, 4, 0, 0, 1000));    // a bow: arrows, engineering makes none
    EXPECT_FALSE(WantsAmmo(false, true, true, 10, 5, 4, 0, 0, 1000));    // not a hunter
    EXPECT_FALSE(WantsAmmo(true, true, false, 10, 5, 4, 0, 0, 1000));    // not bullets
    EXPECT_TRUE(LoadsAmmo(4, 3, 200));   // stronger than the loaded ammo
    EXPECT_FALSE(LoadsAmmo(4, 4, 200));  // as strong, loaded ammo left: keep it
    EXPECT_TRUE(LoadsAmmo(4, 7, 0));     // the loaded ammo is gone: anything beats nothing
    EXPECT_FALSE(LoadsAmmo(4, 7, 1));
    EXPECT_EQ(CastUnits(600, 200), 3u);  // shot counts in casts
    EXPECT_EQ(CastUnits(199, 200), 0u);
    EXPECT_EQ(CastUnits(2, 1), 2u);      // a piece is a unit
    EXPECT_EQ(CastUnits(5, 0), 5u);
    // An ammo need is an ordinary gear need on kAmmoSlot: the order counts casts, the delivery one stack per need.
    std::vector<GearNeed> const ranked =
        RankGearNeeds({{71, 6, kAmmoSlot, kNoPriority, 90}, {70, 6, kAmmoSlot, kNoPriority, 40}});
    EXPECT_EQ(ranked[0].guid, 70u);  // the worse geared hunter first
    std::vector<std::uint32_t> held(9, 0);
    held[6] = CastUnits(250, 200);  // one cast's worth held
    std::vector<GearOrder> const o = PlanGearOrders(ranked, held, 4, 2);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].units, 2u + 2u - 1u);
    EXPECT_EQ(o[0].consumer, 70u);
}

// Lane bootstrap (soak S53-S59: Tanners artisans stuck at leatherworking 1, no need, no order, no skill-up).
TEST(SupplyGearBootstrap, ReachIsTheRankCapOrTheNextRankTheLevelTrains)
{
    EXPECT_EQ(ReachSkill(0, 4), 0u);      // untrained, below Apprentice's level 5
    EXPECT_EQ(ReachSkill(0, 10), 75u);    // untrained: Apprentice
    EXPECT_EQ(ReachSkill(75, 10), 150u);  // Apprentice at level 10: Journeyman (skill 50 comes from skill-ups)
    EXPECT_EQ(ReachSkill(75, 9), 75u);
    EXPECT_EQ(ReachSkill(150, 10), 150u);  // Expert needs level 20
    EXPECT_EQ(ReachSkill(150, 20), 225u);
    EXPECT_EQ(ReachSkill(450, 80), 450u);  // Grand Master: nothing above
}

TEST(SupplyGearBootstrap, StarterRowsOnlyWithTheFlag)
{
    ProductLine const& L = LineOf(Line::LeatherGear);
    EXPECT_EQ(GearTable(L).tierCount, 16u);  // off: the lane V table unchanged
    detail::gEnabled = true;
    detail::gParams.gearBootstrap = true;
    RecipeTable const g = GearTable(L);
    detail::gParams.gearBootstrap = false;
    detail::gEnabled = false;
    ASSERT_EQ(g.tierCount, 19u);
    EXPECT_EQ(GearTable(LineOf(Line::Engineering)).tierCount, 9u);  // no starters
    // Starters: learned with the skill (skill 1), Light Leather from routed scraps (kLeather), then boots / cloak.
    for (std::size_t i = 16; i < 19; ++i)
        EXPECT_EQ(g.tiers[i].skill, 1u);
    std::uint8_t const scraps = TierOf(g, 2318);
    ASSERT_EQ(scraps, 16u);
    EXPECT_EQ(g.tiers[scraps].reagents[0].item, 2934u);
    EXPECT_EQ(g.tiers[scraps].reagents[0].count, 3u);
    EXPECT_EQ(g.tiers[scraps].reqLevel, 0u);  // intermediate: never ranked, never a need
    std::uint8_t const boots = TierOf(g, 2302);
    ASSERT_NE(boots, kNoTier);
    EXPECT_EQ(g.tiers[boots].grey, 70u);
    EXPECT_EQ(g.tiers[boots].reqLevel, 3u);
    // Medium Leather's Light Leather stays a Route reagent: Casts / Lacks never recurse into the scraps row.
    std::unordered_map<std::uint32_t, std::uint32_t> held{{2934, 30}};
    auto have = [&](std::uint32_t item)
    {
        auto const it = held.find(item);
        return it == held.end() ? 0u : it->second;
    };
    EXPECT_EQ(Casts(g, TierOf(g, 2319), have), 0u);
    EXPECT_EQ(Casts(g, scraps, have), 10u);
}

TEST(SupplyGearBootstrap, SkillBlockedNeedGetsTheCheapestSkillupForItsRecipient)
{
    detail::gEnabled = true;
    detail::gParams.gearBootstrap = true;
    RecipeTable const g = GearTable(LineOf(Line::LeatherGear));
    detail::gParams.gearBootstrap = false;
    detail::gEnabled = false;
    std::uint8_t const pants = TierOf(g, 2303), vest = TierOf(g, 2300), boots = TierOf(g, 2302),
                       cloak = TierOf(g, 7276), scraps = TierOf(g, 2318);
    // Tanwyll, leatherworking 1: knows the starters only.
    std::vector<SkillupOption> const opts = {
        {2881, scraps, false, true, 40, true, 3, true},
        {2149, boots, false, true, 70, true, 30, true},
        {9058, cloak, false, true, 70, true, 30, true},
        {2153, pants, false, false, 75, true, 20, true},  // not known yet
    };
    std::uint8_t const belt = TierOf(g, 4246);  // Fine Leather Belt, skill 80
    std::vector<GearNeed> const blocked = RankGearNeeds(
        {{72002, belt, 5, kNoPriority, 200}, {72001, vest, 4, kNoPriority, 90}, {70573, pants, 6, 0, 300}});
    std::vector<GearOrder> o = PlanGearSkillup(g, blocked, 1, opts, 10);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].recipe, scraps);     // cheapest non-grey known recipe
    EXPECT_EQ(o[0].units, 10u);         // SkillupCasts
    EXPECT_EQ(o[0].consumer, 70573u);   // the top ranked blocked need (priority list)
    o = PlanGearSkillup(g, blocked, 45, opts, 10);  // pants / vest learnable now: the belt; scraps grey at 40
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].recipe, boots);  // boots and cloak cost the same: the lower spell
    EXPECT_EQ(o[0].consumer, 72002u);
    EXPECT_TRUE(PlanGearSkillup(g, blocked, 70, opts, 10).empty());         // everything known is grey
    EXPECT_TRUE(PlanGearSkillup(g, {}, 1, opts, 10).empty());               // nothing blocked
    EXPECT_TRUE(PlanGearSkillup(g, blocked, 1, opts, 0).empty());           // SkillupCasts 0
    // Pants learnable now (skill 20 >= 15): the trainer serves it, the vest (skill 40) still gets the skill-up.
    o = PlanGearSkillup(g, blocked, 20, opts, 10);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].consumer, 72001u);
    EXPECT_TRUE(PlanGearSkillup(g, {{70573, pants, 6, 0, 300}}, 20, opts, 10).empty());
    // Untrained (Tinkers at engineering 0): no known recipe, no skill-up (the trainer trip teaches Apprentice).
    std::vector<SkillupOption> none = opts;
    for (SkillupOption& x : none)
        x.known = false;
    EXPECT_TRUE(PlanGearSkillup(g, blocked, 0, none, 10).empty());
}
}  // namespace
