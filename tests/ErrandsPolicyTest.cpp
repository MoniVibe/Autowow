/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ErrandsPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowErrands;

Obs Healthy(std::uint32_t cls, std::uint32_t level)
{
    Obs o;
    o.cls = cls;
    o.level = level;
    o.have = {20, 20, 1000, 1000, 5};
    return o;
}

// ---- needs policy ---------------------------------------------------------------------------------
TEST(Errands, HealthyBotHasNoNeeds)
{
    Params p;
    Assessment const a = Assess(p, Healthy(kClassWarrior, 20));
    EXPECT_EQ(a.needs, 0U);
    EXPECT_EQ(a.urgent, 0U);
}

TEST(Errands, BagAndDurabilityThresholds)
{
    Params p;  // bags soft 70 / urgent 90, durability soft 50 / urgent 25
    Obs o = Healthy(kClassWarrior, 20);
    o.bagUsedPct = 69;
    o.durabilityPct = 50;
    EXPECT_EQ(Assess(p, o).needs, 0U);
    o.bagUsedPct = 70;
    o.durabilityPct = 49;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedBags | NeedRepair));
    EXPECT_EQ(a.urgent, 0U);
    o.bagUsedPct = 90;
    o.durabilityPct = 24;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedBags | NeedRepair));
}

TEST(Errands, ConsumableNeedsFollowClass)
{
    Params p;
    Obs o = Healthy(kClassWarrior, 20);
    o.have = {0, 0, 0, 0, 0};
    // Warrior: food only (no water, ammo, reagent).
    EXPECT_EQ(Assess(p, o).needs, std::uint32_t(NeedFood));
    // Priest: food + water.
    o.cls = kClassPriest;
    EXPECT_EQ(Assess(p, o).needs, std::uint32_t(NeedFood | NeedWater));
    // Mage conjures from level 6.
    o.cls = kClassMage;
    EXPECT_EQ(Assess(p, o).needs, 0U);
    o.level = 5;
    EXPECT_EQ(Assess(p, o).needs, std::uint32_t(NeedFood | NeedWater));
    // Shaman 30+: ankh.
    o.cls = kClassShaman;
    o.level = 30;
    EXPECT_EQ(Assess(p, o).needs, std::uint32_t(NeedFood | NeedWater | NeedReagent));
    o.level = 29;
    EXPECT_EQ(Assess(p, o).needs, std::uint32_t(NeedFood | NeedWater));
}

TEST(Errands, HunterWithoutAmmoIsUrgent)
{
    Params p;
    Obs o = Healthy(kClassHunter, 20);
    o.ammo = AmmoBullets;
    o.have[KindBullet] = 199;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedAmmo));
    EXPECT_EQ(a.urgent, 0U);
    o.have[KindBullet] = 0;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedAmmo));
    // Arrows in bags do not feed a gun.
    o.have[KindArrow] = 1000;
    EXPECT_EQ(Assess(p, o).urgent, std::uint32_t(NeedAmmo));
    // No ranged weapon: no ammo need.
    o.ammo = AmmoNone;
    EXPECT_EQ(Assess(p, o).needs, 0U);
}

TEST(Errands, UnaffordableRestockDropsConsumableNeeds)
{
    Params p;
    Obs o = Healthy(kClassHunter, 20);
    o.ammo = AmmoArrows;
    o.have = {0, 0, 0, 0, 0};
    o.restockAffordable = false;
    o.bagUsedPct = 75;
    Assessment const a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedBags));
    EXPECT_EQ(a.urgent, 0U);
}

TEST(Errands, TrainerHearthAndFlightPathNeeds)
{
    Params p;
    Obs o = Healthy(kClassWarrior, 20);
    o.classTrainDue = true;
    o.profTrainDue = true;
    o.hearthElsewhere = true;
    o.unknownFlightPath = true;
    EXPECT_EQ(Assess(p, o).needs,
              std::uint32_t(NeedClassTrain | NeedProfTrain | NeedHearth | NeedFlightPath));
    EXPECT_EQ(Assess(p, o).urgent, 0U);
}

TEST(Errands, ClassTrainDueEveryTwoLevelsWithBudget)
{
    EXPECT_EQ(ClassTrainBudgetCopper(20), 2000U);  // 20 silver
    EXPECT_FALSE(ClassTrainDue(1, 0, 1000000));    // nothing to learn at 1
    EXPECT_TRUE(ClassTrainDue(2, 0, 20));
    EXPECT_FALSE(ClassTrainDue(21, 20, 1000000));
    EXPECT_TRUE(ClassTrainDue(22, 20, 2420));
    EXPECT_FALSE(ClassTrainDue(22, 20, 2419));     // short of the budget
}

TEST(Errands, ProfessionRankDueAtCapAndLevel)
{
    EXPECT_TRUE(ProfessionRankDue(50, 75, 10));    // apprentice -> journeyman
    EXPECT_FALSE(ProfessionRankDue(49, 75, 10));
    EXPECT_FALSE(ProfessionRankDue(75, 75, 9));
    EXPECT_TRUE(ProfessionRankDue(200, 225, 35));  // expert -> artisan
    EXPECT_FALSE(ProfessionRankDue(200, 225, 34));
    EXPECT_TRUE(ProfessionRankDue(350, 375, 65));  // master -> grand master
    EXPECT_FALSE(ProfessionRankDue(450, 450, 80)); // capped
}

// ---- trigger decision ------------------------------------------------------------------------------
TEST(Errands, RunOnUrgentOrTwoSoftNeeds)
{
    EXPECT_FALSE(ShouldRun(0, 0));
    EXPECT_FALSE(ShouldRun(NeedFood, 0));
    EXPECT_TRUE(ShouldRun(NeedFood | NeedHearth, 0));
    EXPECT_TRUE(ShouldRun(NeedRepair, NeedRepair));
    EXPECT_TRUE(ShouldRun(NeedBags | NeedFood | NeedClassTrain, 0));
}

TEST(Errands, TownServesOnlyWhatItCan)
{
    TownFacts f;
    f.sells = 1u << KindFood;
    std::uint32_t const needs = NeedFood | NeedWater | NeedHearth | NeedFlightPath;
    EXPECT_EQ(needs & Serves(f), std::uint32_t(NeedFood));  // other zone, no water vendor
    EXPECT_FALSE(ShouldRun(needs & Serves(f), 0));
    f.inBotZone = true;
    f.unknownFlightMaster = true;
    EXPECT_EQ(needs & Serves(f), std::uint32_t(NeedFood | NeedHearth | NeedFlightPath));
    EXPECT_EQ(Serves(TownFacts{}) & (NeedBags | NeedRepair), std::uint32_t(NeedBags | NeedRepair));
}

// ---- town choice -------------------------------------------------------------------------------------
TEST(Errands, LegChoice)
{
    Params p;  // hearth > 800 yd, flight >= 600 yd, walk <= 4000 yd
    LegInput in;
    in.walkYards = 300;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Walk);
    in.hearthHere = in.hearthReady = true;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Walk);  // near: walk even when bound here
    in.walkYards = 900;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Hearth);
    in.hearthReady = false;  // on cooldown
    EXPECT_EQ(ChooseLeg(p, in), Leg::Walk);
    in.flight = true;
    in.fmYards = 50;
    in.flyYards = 900;
    in.tailYards = 30;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Flight);  // 128 s walk vs 11 s + 30 s + 10 s
    in.walkYards = 2000;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Flight);
    in.fmYards = 1900;  // the flight master is as far as the town
    EXPECT_EQ(ChooseLeg(p, in), Leg::Walk);
    in.flight = false;
    in.walkYards = 4001;
    EXPECT_EQ(ChooseLeg(p, in), Leg::None);
    in.sameMap = false;
    in.hearthHere = in.hearthReady = true;
    EXPECT_EQ(ChooseLeg(p, in), Leg::None);  // never hearth off the bot's continent
}

TEST(Errands, LegCosts)
{
    Params p;
    LegInput in;
    in.walkYards = 700;
    in.fmYards = 70;
    in.flyYards = 300;
    in.tailYards = 0;
    EXPECT_EQ(LegCostMs(p, Leg::Walk, in), 100000U);
    EXPECT_EQ(LegCostMs(p, Leg::Flight, in), 10000U + 10000U + 10000U);
    EXPECT_EQ(LegCostMs(p, Leg::Hearth, in), 15000U);
    EXPECT_EQ(LegCostMs(p, Leg::None, in), UINT32_MAX);
}

TEST(Errands, PickTownCheapestThenLowerId)
{
    std::vector<Candidate> c = {{30, Leg::Walk, 50000}, {20, Leg::None, 1}, {40, Leg::Flight, 40000},
                                {10, Leg::Walk, 40000}};
    Candidate const* best = PickTown(c);
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->town, 10U);  // 40 s tie: lower id; the unreachable 1 ms candidate is ignored
    EXPECT_EQ(PickTown({{5, Leg::None, 0}}), nullptr);
}

TEST(Errands, ZoneTooHighSkipsDangerousTowns)
{
    Params p;  // level + 3
    EXPECT_FALSE(ZoneTooHigh(p, 20, 0));   // unbracketed (capital)
    EXPECT_FALSE(ZoneTooHigh(p, 20, 23));
    EXPECT_TRUE(ZoneTooHigh(p, 20, 24));
}

Npc MakeNpc(std::uint32_t spawn, std::uint32_t roles, std::int32_t x, std::uint8_t teams = kAlliance | kHorde)
{
    Npc n;
    n.spawn = spawn;
    n.entry = spawn + 1000;
    n.x = x;
    n.roles = roles;
    n.teams = teams;
    return n;
}

TEST(Errands, BuildTownsNeedsInnRepairAndFood)
{
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0, kAlliance);
    inn.sells = 1u << KindFood;
    inn.items = {4540};
    Npc smith = MakeNpc(7, RoleRepair | RoleVendor, 60, kAlliance);
    Npc far = MakeNpc(9, RoleRepair, 500, kAlliance);
    Npc fm = MakeNpc(3, RoleFlight, -80, kAlliance);
    Npc lonelyInn = MakeNpc(11, RoleInn, 5000, kAlliance);   // no repair nearby
    Npc hordeInn = MakeNpc(13, RoleInn, 40, kHorde);         // horde has no repair / food here
    std::vector<Town> towns = BuildTowns({smith, far, inn, lonelyInn, fm, hordeInn}, 120);
    ASSERT_EQ(towns.size(), 1U);
    EXPECT_EQ(towns[0].id, 5U);
    EXPECT_EQ(towns[0].teams, kAlliance);
    EXPECT_EQ(towns[0].sells, 1u << KindFood);
    ASSERT_EQ(towns[0].npcs.size(), 3U);  // fm, inn, smith (spawn order); the far smith and horde inn excluded
    EXPECT_EQ(towns[0].npcs[0].spawn, 3U);
    EXPECT_EQ(towns[0].npcs[2].spawn, 7U);
}

// ---- restock ------------------------------------------------------------------------------------------
TEST(Errands, BestTierByLevelAndStock)
{
    EXPECT_EQ(BestTier(KindFood, 1, nullptr), 4540U);
    EXPECT_EQ(BestTier(KindFood, 24, nullptr), 4542U);
    EXPECT_EQ(BestTier(KindFood, 25, nullptr), 4544U);
    EXPECT_EQ(BestTier(KindWater, 69, nullptr), 28399U);  // Pungent Seal Whey needs 70
    EXPECT_EQ(BestTier(KindWater, 70, nullptr), 33444U);
    EXPECT_EQ(BestTier(KindArrow, 80, nullptr), 41586U);
    EXPECT_EQ(BestTier(KindReagent, 29, nullptr), 0U);
    std::vector<std::uint32_t> const sold = {159, 4540, 4541};  // a starter-town vendor
    EXPECT_EQ(BestTier(KindFood, 40, &sold), 4541U);  // best it has, not the best there is
    EXPECT_EQ(BestTier(KindWater, 40, &sold), 159U);
    EXPECT_EQ(BestTier(KindBullet, 40, &sold), 0U);
}

TEST(Errands, RestockListByClassAndLevel)
{
    Params p;
    std::vector<std::uint32_t> const sold = {159, 1179, 1205, 1708, 2512, 2515, 2516, 2519, 3030, 3033,
                                             4540, 4541, 4542, 4544, 17030};
    std::array<std::uint32_t, kKinds> have = {3, 12, 0, 150, 0};
    // Level 26 hunter with a gun: food, water, bullets topped up to target; arrows not bought.
    std::vector<RestockLine> lines = RestockList(p, kClassHunter, 26, AmmoBullets, have, sold);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[0].kind, KindFood);
    EXPECT_EQ(lines[0].item, 4544U);
    EXPECT_EQ(lines[0].target - lines[0].have, 17U);
    EXPECT_EQ(lines[1].kind, KindWater);
    EXPECT_EQ(lines[1].item, 1708U);
    EXPECT_EQ(lines[2].kind, KindBullet);
    EXPECT_EQ(lines[2].item, 3033U);
    EXPECT_EQ(lines[2].target - lines[2].have, 850U);
    // Level 30 shaman: food, water, ankh.
    lines = RestockList(p, kClassShaman, 30, AmmoNone, have, sold);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[2].kind, KindReagent);
    EXPECT_EQ(lines[2].item, 17030U);
    // Level 12 rogue: food only.
    lines = RestockList(p, kClassRogue, 12, AmmoNone, have, sold);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines[0].item, 4541U);
    // Level 40 mage: conjures, buys nothing. Full stocks: nothing.
    EXPECT_TRUE(RestockList(p, kClassMage, 40, AmmoNone, have, sold).empty());
    std::array<std::uint32_t, kKinds> const full = {20, 20, 1000, 1000, 5};
    EXPECT_TRUE(RestockList(p, kClassHunter, 26, AmmoBullets, full, sold).empty());
}

TEST(Errands, PacksLimitedByMoney)
{
    PackBuy b = PacksToBuy(17, 5, 1000, 100000);
    EXPECT_EQ(b.packs, 4U);  // 17 items in packs of 5
    EXPECT_FALSE(b.shortOfMoney);
    b = PacksToBuy(17, 5, 1000, 2500);
    EXPECT_EQ(b.packs, 2U);
    EXPECT_TRUE(b.shortOfMoney);
    b = PacksToBuy(850, 200, 300, 0);
    EXPECT_EQ(b.packs, 0U);
    EXPECT_TRUE(b.shortOfMoney);
    EXPECT_EQ(PacksToBuy(0, 5, 1000, 100000).packs, 0U);
}

// ---- errand batch ---------------------------------------------------------------------------------------
TEST(Errands, PlanStopsBatchesInOrderAndMergesNpcs)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0);
    inn.items = {159, 4540};
    Npc smith = MakeNpc(7, RoleRepair | RoleVendor, 20);
    Npc trainer = MakeNpc(8, RoleClassTrainer, 30);
    Npc bowyer = MakeNpc(9, RoleVendor, 40);
    bowyer.items = {2512};
    Npc fm = MakeNpc(12, RoleFlight, 50);
    t.npcs = {inn, smith, trainer, bowyer, fm};
    PlanInput in;
    in.team = kAlliance;
    in.buyItems[KindFood] = 4540;
    in.buyItems[KindWater] = 159;
    in.buyItems[KindArrow] = 2512;
    in.trainers = {8};
    in.bind = true;
    in.learnFp = true;
    Plan const plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 5U);
    // smith: sell + repair; inn: food + water, later bind (merged); bowyer: arrows; trainer; fm.
    EXPECT_EQ(plan.stops[0].spawn, 7U);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell | OpRepair));
    EXPECT_EQ(plan.stops[1].spawn, 5U);
    EXPECT_EQ(plan.stops[1].ops, std::uint32_t(OpBuy | OpBind));
    EXPECT_EQ(plan.stops[1].buyKinds, std::uint32_t((1u << KindFood) | (1u << KindWater)));
    EXPECT_EQ(plan.stops[2].spawn, 9U);
    EXPECT_EQ(plan.stops[2].buyKinds, std::uint32_t(1u << KindArrow));
    EXPECT_EQ(plan.stops[3].spawn, 8U);
    EXPECT_EQ(plan.stops[3].ops, std::uint32_t(OpTrain));
    EXPECT_EQ(plan.stops[4].spawn, 12U);
    EXPECT_EQ(plan.stops[4].ops, std::uint32_t(OpLearnFp));
}

TEST(Errands, PlanStopsMinimalRunAndTeamFilter)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn, 0);
    Npc hordeSmith = MakeNpc(6, RoleRepair | RoleVendor, 10, kHorde);
    Npc smith = MakeNpc(7, RoleRepair, 20);           // repairs, sells nothing
    Npc grocer = MakeNpc(8, RoleVendor, 30);
    t.npcs = {inn, hordeSmith, smith, grocer};
    PlanInput in;
    in.team = kAlliance;
    Plan const plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 2U);
    EXPECT_EQ(plan.stops[0].spawn, 8U);  // junk sold at a vendor
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell));
    EXPECT_EQ(plan.stops[1].spawn, 7U);
    EXPECT_EQ(plan.stops[1].ops, std::uint32_t(OpRepair));
}

// AutoWow.Trade: auctioneer then mailbox after every other stop; a mailbox spawn sharing a creature's
// number (kGoSpawnBit) never merges into that creature's stop.
TEST(Errands, PlanStopsTradeStopsLastAndMailboxIdSpaceDisjoint)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn, 0);
    Npc smith = MakeNpc(7, RoleRepair | RoleVendor, 20);
    Npc hordeAuctioneer = MakeNpc(8, RoleAuction, 25, kHorde);
    Npc auctioneer = MakeNpc(9, RoleAuction, 30);
    Npc box = MakeNpc(7 | kGoSpawnBit, RoleMailbox, 40);
    t.npcs = {inn, smith, hordeAuctioneer, auctioneer, box};
    PlanInput in;
    in.team = kAlliance;
    in.bind = true;
    // Flag off (defaults): the plan is the pre-trade plan.
    Plan plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 2U);
    in.auction = true;
    in.mail = true;
    plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 4U);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell | OpRepair));
    EXPECT_EQ(plan.stops[1].spawn, 5U);
    EXPECT_EQ(plan.stops[1].ops, std::uint32_t(OpBind));
    EXPECT_EQ(plan.stops[2].spawn, 9U);
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpAuction));
    EXPECT_EQ(plan.stops[3].spawn, 7U | kGoSpawnBit);
    EXPECT_EQ(plan.stops[3].ops, std::uint32_t(OpMail));
    // No auctioneer in town: mail only.
    t.npcs = {inn, smith, box};
    plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpMail));
}

// ---- state + ledger ----------------------------------------------------------------------------------------
TEST(Errands, AfterRunKeepsTrainLevelAndCoolsDown)
{
    Params p;
    BotState s;
    s.phase = Phase::Return;
    s.town = 99;
    s.lastClassTrainLevel = 24;
    s.spent = 500;
    BotState const n = AfterRun(p, s, 1000);
    EXPECT_EQ(n.phase, Phase::None);
    EXPECT_EQ(n.town, 0U);
    EXPECT_EQ(n.spent, 0U);
    EXPECT_EQ(n.lastClassTrainLevel, 24U);
    EXPECT_EQ(n.cooldownUntilMs, 1000U + p.cooldownMs);
    EXPECT_EQ(n.version, kStateVersion);
}

TEST(Errands, LegExhaustedByTimeoutOrReissues)
{
    Params p;
    BotState s;
    s.phaseMs = 1000;
    EXPECT_FALSE(LegExhausted(p, s, 1000 + p.travelTimeoutMs, p.travelTimeoutMs));
    EXPECT_TRUE(LegExhausted(p, s, 1001 + p.travelTimeoutMs, p.travelTimeoutMs));
    s.reissues = p.maxReissues + 1;
    EXPECT_TRUE(LegExhausted(p, s, 1000, p.travelTimeoutMs));
}

TEST(Errands, DurabilityAndBagPercent)
{
    EXPECT_EQ(DurabilityPct(0, 0), 100U);
    EXPECT_EQ(DurabilityPct(30, 120), 25U);
    EXPECT_EQ(BagUsedPct(12, 16), 75U);
    EXPECT_EQ(Yards(0, 0, 300, 400), 500U);
    EXPECT_EQ(ISqrt(99), 9U);
}

TEST(Errands, LedgerFieldsAreStable)
{
    BotState s;
    s.town = 3002;
    s.needs = NeedBags | NeedFood;
    s.done = DoneSold | DoneRepaired | DoneRestocked;
    s.spent = 1234;
    s.sold = 560;
    s.durBefore = 40;
    s.durAfter = 100;
    s.bagFreeBefore = 2;
    s.bagFreeAfter = 11;
    s.travelMs = 90000;
    s.travelLeg = Leg::Hearth;
    s.hearthUsed = true;
    EXPECT_EQ(LedgerFields(s, 12, 45000),
              ",\"town\":3002,\"town_zone\":12,\"needs\":5,\"done\":7,\"spent\":1234,\"sold\":560,\"dur0\":40,"
              "\"dur1\":100,\"bag0\":2,\"bag1\":11,\"travel_ms\":90000,\"return_ms\":45000,\"leg\":\"hearth\","
              "\"hearth\":true");
    EXPECT_STREQ(OutcomeName(Outcome::ReturnGaveUp), "return_gave_up");
    EXPECT_STREQ(LegName(Leg::Flight), "flight");
}

// ---- AutoWow.Survival.KeepConsumables ------------------------------------------------------------
// soak-s14-full-r1: 33 of 50 bots carried no food/drink; a lone "food" soft need never started a run.
TEST(Errands, KeepConsumablesMakesEmptyFoodAndDrinkUrgent)
{
    Params p;
    Obs o = Healthy(kClassRogue, 11);
    o.have = {0, 0, 0, 0, 0};
    EXPECT_EQ(Assess(p, o).urgent, 0U);  // flag off: soft only
    EXPECT_FALSE(ShouldRun(Assess(p, o).needs, Assess(p, o).urgent));
    p.keepConsumables = true;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedFood));  // rogue: no water
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    o.cls = kClassPriest;
    EXPECT_EQ(Assess(p, o).urgent, std::uint32_t(NeedFood | NeedWater));
    o.have = {3, 0, 0, 0, 0};  // some food left: soft
    EXPECT_EQ(Assess(p, o).urgent, std::uint32_t(NeedWater));
    o.restockAffordable = false;  // still dropped when nothing can be bought
    EXPECT_EQ(Assess(p, o).urgent, 0U);
}

TEST(Errands, KeepAffordableCountsGreyLootAgainstOnePack)
{
    // Median cohort purse 228 c; Freshly Baked Bread 4541 is 125 c per 5-pack.
    EXPECT_TRUE(KeepAffordable(228, 0, 125));
    EXPECT_FALSE(KeepAffordable(39, 0, 125));
    EXPECT_TRUE(KeepAffordable(39, 100, 125));  // selling the greys first pays for it
    EXPECT_TRUE(KeepAffordable(0, 0, 0));
}

TEST(Errands, KeepBuysToLowThenTarget)
{
    Params p;  // food/water low 5 target 20, ammo 200/1000, reagent 1/5
    EXPECT_EQ(PassTarget(p, KindFood, 0), 5U);
    EXPECT_EQ(PassTarget(p, KindFood, 1), 20U);
    EXPECT_EQ(PassTarget(p, KindWater, 0), 5U);
    EXPECT_EQ(PassTarget(p, KindArrow, 0), 200U);
    EXPECT_EQ(PassTarget(p, KindReagent, 1), 5U);
    p.foodLow = 30;  // a low above the target never overbuys
    EXPECT_EQ(PassTarget(p, KindFood, 0), 20U);
}

TEST(Errands, KeepConsumablesDefaults)
{
    Params const p;
    EXPECT_FALSE(p.keepConsumables);
    EXPECT_EQ(p.sellDetourYards, 30U);
    EXPECT_EQ(p.sellDetourMs, 30000U);
    EXPECT_EQ(p.sellRetryMs, 300000U);
    BotState const s;
    EXPECT_EQ(s.version, 4U);
    EXPECT_FALSE(s.rescued);
    EXPECT_EQ(s.sellUntilMs, 0U);
    EXPECT_EQ(s.sellRetryMs, 0U);
}
// AutoWow.Travel.Safe: one rescue leg per run; hearth to a serving bound town first, else a known flight;
// the exhausted leg is never its own rescue.
TEST(Errands, RescueLegOncePerRun)
{
    EXPECT_EQ(RescueLeg(false, Leg::Walk, true, true), Leg::Hearth);
    EXPECT_EQ(RescueLeg(false, Leg::Walk, false, true), Leg::Flight);
    EXPECT_EQ(RescueLeg(false, Leg::Walk, false, false), Leg::None);
    EXPECT_EQ(RescueLeg(true, Leg::Walk, true, true), Leg::None);
    EXPECT_EQ(RescueLeg(false, Leg::Hearth, true, true), Leg::Flight);
    EXPECT_EQ(RescueLeg(false, Leg::Flight, false, true), Leg::None);
    EXPECT_EQ(RescueLeg(false, Leg::None, true, false), Leg::Hearth);
}

// ---- AutoWow.Gear.Upgrades (GearUpgradePolicy.h) -------------------------------------------------------
// soak-s21-full-r1: Worn Dirk 1-2 dmg / 1.6 s at L20; a L20 vendor sword 14 DPS.
TEST(Gear, DpsMilliAndLevelExpectation)
{
    EXPECT_EQ(AutoWowGear::DpsMilli(1, 2, 1600), 937U);    // Worn Dirk ~0.94
    EXPECT_EQ(AutoWowGear::DpsMilli(2, 5, 1900), 1842U);
    EXPECT_EQ(AutoWowGear::DpsMilli(10, 20, 0), 0U);        // no delay: not a weapon
    EXPECT_EQ(AutoWowGear::ExpectedDpsMilli(20), 14000U);
    AutoWowGear::Params const p;
    EXPECT_TRUE(AutoWowGear::FarBelow(p, 937, 20));
    EXPECT_TRUE(AutoWowGear::FarBelow(p, 6999, 20));
    EXPECT_FALSE(AutoWowGear::FarBelow(p, 7000, 20));   // exactly half of 14.0
    EXPECT_TRUE(AutoWowGear::Beats(p, 1, 0));           // an empty hand takes anything
    EXPECT_TRUE(AutoWowGear::Beats(p, 1500, 1000));     // 1.5x
    EXPECT_FALSE(AutoWowGear::Beats(p, 1499, 1000));
    EXPECT_FALSE(AutoWowGear::Beats(p, 0, 0));
}

TEST(Gear, DueOncePerLevelWithBudgetUrgentWhenFarBelow)
{
    AutoWowGear::Params const p;
    std::uint64_t const reserve = AutoWowGear::ReserveCopper(20);  // 2000 + 2000
    EXPECT_EQ(reserve, 4000U);
    std::uint64_t const budget = AutoWowGear::MinBudgetCopper(20);  // 8000
    AutoWowGear::Due d = AutoWowGear::GearDue(p, 20, 19, reserve + budget, 937);
    EXPECT_TRUE(d.soft);
    EXPECT_TRUE(d.urgent);
    d = AutoWowGear::GearDue(p, 20, 19, reserve + budget, 14000);  // on the curve: soft only
    EXPECT_TRUE(d.soft);
    EXPECT_FALSE(d.urgent);
    d = AutoWowGear::GearDue(p, 20, 20, reserve + budget, 937);  // already shopped this level
    EXPECT_FALSE(d.soft);
    EXPECT_FALSE(d.urgent);
    d = AutoWowGear::GearDue(p, 20, 19, reserve + budget - 1, 937);  // the reserve is not spendable
    EXPECT_FALSE(d.soft);
    EXPECT_EQ(AutoWowGear::Spendable(3000, 20), 0U);
}

AutoWowGear::Offer Weapon(std::uint32_t item, std::uint32_t npc, std::uint64_t price, std::uint32_t dps,
                          std::uint8_t slot, bool twoHand = false)
{
    AutoWowGear::Offer o;
    o.item = item;
    o.npc = npc;
    o.price = price;
    o.dpsMilli = dps;
    o.slot = slot;
    o.weapon = true;
    o.twoHand = twoHand;
    return o;
}

AutoWowGear::Offer Armor(std::uint32_t item, std::uint32_t npc, std::uint64_t price, std::uint32_t armor,
                         std::uint8_t slot)
{
    AutoWowGear::Offer o;
    o.item = item;
    o.npc = npc;
    o.price = price;
    o.armor = armor;
    o.slot = slot;
    return o;
}

TEST(Gear, PickWeaponBestAffordableUpgradeDeterministic)
{
    AutoWowGear::Params const p;
    using AutoWowGear::kSlotMainHand;
    std::vector<AutoWowGear::Offer> const offers = {
        Weapon(10, 3, 5000, 12000, kSlotMainHand), Weapon(11, 2, 9000, 16000, kSlotMainHand),
        Weapon(12, 1, 4000, 12000, kSlotMainHand), Weapon(13, 1, 4000, 12000, AutoWowGear::kSlotOffHand),
        Weapon(14, 1, 3000, 2000, kSlotMainHand)};
    EXPECT_EQ(AutoWowGear::PickWeapon(p, offers, kSlotMainHand, 1000, 10000), 1U);   // most DPS in budget
    EXPECT_EQ(AutoWowGear::PickWeapon(p, offers, kSlotMainHand, 1000, 8999), 2U);    // tie 12.0: cheaper wins
    EXPECT_EQ(AutoWowGear::PickWeapon(p, offers, kSlotMainHand, 10000, 10000), 1U);  // 16 >= 1.5 x 10
    EXPECT_EQ(AutoWowGear::PickWeapon(p, offers, kSlotMainHand, 11000, 10000), AutoWowGear::kNone);  // 16 < 16.5
    EXPECT_EQ(AutoWowGear::PickWeapon(p, offers, kSlotMainHand, 1000, 2999), AutoWowGear::kNone);
    std::vector<AutoWowGear::Offer> const tie = {Weapon(20, 9, 4000, 12000, kSlotMainHand),
                                                 Weapon(20, 4, 4000, 12000, kSlotMainHand)};
    EXPECT_EQ(AutoWowGear::PickWeapon(p, tie, kSlotMainHand, 0, 5000), 1U);  // same item: lower npc spawn
}

TEST(Gear, ShoppingListWeaponsFirstThenCheapArmorForEmptySlots)
{
    AutoWowGear::Params const p;  // armor piece <= 25 % of what is left, 4 pieces
    using AutoWowGear::kSlotMainHand;
    using AutoWowGear::kSlotOffHand;
    std::vector<AutoWowGear::Offer> const offers = {
        Weapon(1, 7, 6000, 12000, kSlotMainHand), Weapon(1, 7, 6000, 12000, kSlotOffHand),
        Weapon(2, 7, 1500, 5000, kSlotOffHand),   Armor(30, 8, 900, 40, 4 /* chest */),
        Armor(31, 8, 500, 30, 4),                 Armor(32, 8, 300, 10, 9 /* hands */),
        Armor(33, 8, 200, 12, 7 /* feet: worn */)};
    std::uint32_t const empty = (1u << 4) | (1u << 9);
    // Rogue: 10000 spendable, dual wield. Main hand 6000, off hand 1500 (the 6000 copy no longer fits),
    // 2500 left: chest <= 625 -> item 31; 2000 left: hands <= 500 -> item 32.
    std::vector<AutoWowGear::Offer> list = AutoWowGear::ShoppingList(p, offers, 937, 937, true, empty, 10000);
    ASSERT_EQ(list.size(), 4U);
    EXPECT_EQ(list[0].item, 1U);
    EXPECT_EQ(list[0].slot, kSlotMainHand);
    EXPECT_EQ(list[1].item, 2U);
    EXPECT_EQ(list[2].item, 31U);
    EXPECT_EQ(list[3].item, 32U);
    // No dual wield: no off hand; 4000 left: chest <= 1000 -> item 30, 3100 left: hands 300 <= 775.
    list = AutoWowGear::ShoppingList(p, offers, 937, 0, false, empty, 10000);
    ASSERT_EQ(list.size(), 3U);
    EXPECT_EQ(list[1].item, 30U);
    EXPECT_EQ(list[2].item, 32U);
    // A two-hander main hand leaves no off hand to buy.
    std::vector<AutoWowGear::Offer> const big = {Weapon(3, 7, 2000, 20000, kSlotMainHand, true),
                                                 Weapon(2, 7, 1500, 5000, kSlotOffHand)};
    list = AutoWowGear::ShoppingList(p, big, 937, 0, true, 0, 10000);
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list[0].item, 3U);
    // Nothing affordable: nothing bought.
    EXPECT_TRUE(AutoWowGear::ShoppingList(p, offers, 937, 937, true, empty, 100).empty());
}

TEST(Gear, QuestChoiceWithoutUpgradeTakesMostValuable)
{
    EXPECT_EQ(AutoWowGear::MostValuableChoice({120, 450, 450, 30}), 1U);
    EXPECT_EQ(AutoWowGear::MostValuableChoice({0, 0}), 0U);
    EXPECT_EQ(AutoWowGear::MostValuableChoice({}), 0U);
}

TEST(Gear, ErrandNeedServedOnlyByAGearTownAndPlannedAfterTraining)
{
    Obs o = Healthy(kClassRogue, 20);
    o.gear.soft = true;
    Params const p;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedGear));
    EXPECT_EQ(a.urgent, 0U);
    o.gear.urgent = true;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedGear));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    TownFacts f;
    EXPECT_EQ(Serves(f) & NeedGear, 0U);  // flag off / no gear vendor: never served
    f.gearVendor = true;
    EXPECT_EQ(Serves(f) & NeedGear, std::uint32_t(NeedGear));

    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0);
    Npc trainer = MakeNpc(8, RoleClassTrainer, 30);
    Npc weaponsmith = MakeNpc(9, RoleVendor, 40);
    weaponsmith.gear = {2488};
    t.npcs = {inn, trainer, weaponsmith};
    PlanInput in;
    in.team = kAlliance;
    in.trainers = {8};
    in.gearNpcs = {9};
    in.bind = true;
    Plan const plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell | OpBind));  // the inn sells junk, binds last (merged)
    EXPECT_EQ(plan.stops[1].spawn, 8U);
    EXPECT_EQ(plan.stops[2].spawn, 9U);
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpGear));
    BotState s;
    s.lastGearLevel = 17;
    EXPECT_EQ(AfterRun(p, s, 1000).lastGearLevel, 17U);
}
}  // namespace
