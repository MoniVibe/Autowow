/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <filesystem>
#include <fstream>
#include <iterator>

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

std::string ReadModuleSource(std::filesystem::path const& relative)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relative,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

RelocationRetirementFacts EligibleRelocationRetirement()
{
    RelocationRetirementFacts facts;
    facts.tracked = true;
    facts.alive = true;
    facts.inWorld = true;
    facts.independent = true;
    facts.mapAvailable = true;
    facts.movementAllowed = true;
    return facts;
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

TEST(Errands, WalkAdmissionRetainsLongRouteAndTaxiFallback)
{
    Params p;
    LegInput in;
    in.walkYards = 3000;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Walk);  // a fresh long TravelMgr prefix authorizes the walk

    in.walkRouteAvailable = false;
    EXPECT_EQ(ChooseLeg(p, in), Leg::None);
    in.flight = true;
    in.fmYards = 50;
    in.flyYards = 2800;
    in.tailYards = 30;
    EXPECT_EQ(ChooseLeg(p, in), Leg::Flight);  // denied walk does not suppress the known taxi
}

TEST(Errands, DeniedCheapestTownFallsBackToReachableTown)
{
    std::vector<Candidate> const c = {{10, Leg::None, 1}, {20, Leg::Walk, 2000}};
    Candidate const* best = PickTown(c);
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->town, 20U);
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

TEST(Errands, SellTimeoutUsesDifferentVendorAndPreservesFutureStop)
{
    Town t;
    Npc first = MakeNpc(7, RoleRepair | RoleVendor, 20, kAlliance);
    Npc alternate = MakeNpc(9, RoleVendor, 30, kAlliance);
    t.npcs = {first, alternate};
    Plan plan;
    plan.count = kMaxStops;
    plan.stops[0] = Stop{first.spawn, first.entry, first.x, 0, 0,
                         OpSell | OpRepair | OpBuy | OpTool | OpGear | OpTrain, 1u << KindFood};
    for (std::uint8_t k = 1; k < plan.count; ++k)
        plan.stops[k] = Stop{std::uint32_t(20 + k), std::uint32_t(1020 + k), std::int32_t(40 + k), 0, 0,
                             k == 1 ? std::uint32_t(OpTrain) : std::uint32_t(OpBuy),
                             k == 1 ? 0u : std::uint32_t(1u << KindWater)};
    std::array<Stop, kMaxStops> const before = plan.stops;
    SellerRetry retry;
    Stop const* fallback = RetryTimedOutSeller(t, kAlliance, plan, 0, retry);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->spawn, alternate.spawn);
    EXPECT_EQ(fallback->ops, std::uint32_t(OpSell));
    EXPECT_EQ(fallback->buyKinds, 0U);
    EXPECT_EQ(plan.count, kMaxStops);
    for (std::uint8_t k = 1; k < plan.count; ++k)
    {
        EXPECT_EQ(plan.stops[k].spawn, before[k].spawn);
        EXPECT_EQ(plan.stops[k].entry, before[k].entry);
        EXPECT_EQ(plan.stops[k].ops, before[k].ops);
        EXPECT_EQ(plan.stops[k].buyKinds, before[k].buyKinds);
    }
}

TEST(Errands, SellFallbackExcludesWrongTeamNonVendorPlannedAndSameEntry)
{
    Town t;
    Npc failed = MakeNpc(7, RoleVendor, 10, kAlliance);
    Npc wrongTeam = MakeNpc(8, RoleVendor, 20, kHorde);
    Npc nonVendor = MakeNpc(9, RoleRepair, 30, kAlliance);
    Npc planned = MakeNpc(10, RoleVendor, 40, kAlliance);
    Npc clone = MakeNpc(11, RoleVendor, 50, kAlliance);
    clone.entry = failed.entry;
    t.npcs = {failed, wrongTeam, nonVendor, planned, clone};
    Plan plan;
    plan.count = 2;
    plan.stops[0] = Stop{failed.spawn, failed.entry, failed.x, 0, 0, OpSell, 0};
    plan.stops[1] = Stop{planned.spawn, planned.entry, planned.x, 0, 0, OpBuy, 1u << KindFood};
    SellerRetry retry;
    EXPECT_EQ(RetryTimedOutSeller(t, kAlliance, plan, 0, retry), nullptr);
    EXPECT_EQ(plan.stops[1].ops, std::uint32_t(OpBuy));
}

TEST(Errands, SellFallbackStopsAfterTwoAlternatives)
{
    Town t;
    Npc first = MakeNpc(7, RoleVendor, 10, kAlliance);
    Npc second = MakeNpc(8, RoleRepair | RoleVendor, 20, kAlliance);
    Npc third = MakeNpc(9, RoleVendor, 30, kAlliance);
    Npc fourth = MakeNpc(10, RoleVendor, 40, kAlliance);
    t.npcs = {fourth, third, second, first};
    Plan plan;
    plan.count = 1;
    plan.stops[0] = Stop{first.spawn, first.entry, first.x, 0, 0, OpSell, 0};
    SellerRetry retry;
    ASSERT_NE(RetryTimedOutSeller(t, kAlliance, plan, 0, retry), nullptr);
    EXPECT_EQ(plan.stops[0].spawn, second.spawn);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell | OpRepair));
    ASSERT_NE(RetryTimedOutSeller(t, kAlliance, plan, 0, retry), nullptr);
    EXPECT_EQ(plan.stops[0].spawn, third.spawn);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell));
    EXPECT_EQ(RetryTimedOutSeller(t, kAlliance, plan, 0, retry), nullptr);
    EXPECT_EQ(retry.count, 3U);
}

TEST(Errands, NonSellTimeoutHasNoFallback)
{
    Town t;
    t.npcs = {MakeNpc(8, RoleVendor, 20, kAlliance)};
    Plan plan;
    plan.count = 1;
    plan.stops[0] = Stop{7, 1007, 10, 0, 0, OpRepair, 0};
    SellerRetry retry;
    EXPECT_EQ(RetryTimedOutSeller(t, kAlliance, plan, 0, retry), nullptr);
    EXPECT_EQ(retry.count, 0U);
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
TEST(Errands, WalkBackoffMatchesTownZoneNeedAndExpires)
{
    WalkBackoffTable table{};
    RememberTownWalkFailure(table, 46341, 148, NeedProfTrain, 100, 1000);
    EXPECT_TRUE(TownWalkBackedOff(table, 46341, 148, NeedProfTrain, 999));
    EXPECT_FALSE(TownWalkBackedOff(table, 46341, 148, NeedBags, 999));
    EXPECT_FALSE(TownWalkBackedOff(table, 46341, 141, NeedProfTrain, 999));
    EXPECT_FALSE(TownWalkBackedOff(table, 46341, 148, NeedProfTrain, 1000));

    RememberTownWalkFailure(table, 46341, 148, NeedBags, 200, 1200);
    EXPECT_TRUE(TownWalkBackedOff(table, 46341, 148, NeedProfTrain | NeedBags, 1100));
    EXPECT_EQ(table[0].needs, std::uint32_t(NeedProfTrain | NeedBags));
    EXPECT_EQ(table[0].retryUntilMs, 1200U);

    RememberTownWalkFailure(table, 46341, 148, NeedFood, 1200, 2000);
    EXPECT_FALSE(TownWalkBackedOff(table, 46341, 148, NeedProfTrain, 1300));
    EXPECT_TRUE(TownWalkBackedOff(table, 46341, 148, NeedFood, 1300));
}

TEST(Errands, WalkBackoffReplacementIsFixedAndDeterministic)
{
    WalkBackoffTable table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        RememberTownWalkFailure(table, std::uint32_t(100 + i), 148, NeedProfTrain, 10,
                                1000 + i);
    RememberTownWalkFailure(table, 999, 148, NeedBags, 10, 2000);
    EXPECT_EQ(table[0].town, 999U);  // earliest expiry is replaced; array order breaks ties
    EXPECT_EQ(table.size(), kWalkBackoffs);
}

TEST(Errands, TownWalkGoalCleanupRequiresExactOwnedGoal)
{
    WalkGoalKey const town{1, 10127, 2224, 1328};
    WalkGoalKey const segmentEnd{1, 10020, 2190, 1300};
    WalkGoalKey const unrelated{1, 9000, 2000, 10};
    PreparedWalkSegment const prepared{
        true, town, segmentEnd, 1, 148, 77, PreparedWalkProofKind::TravelMgr};
    EXPECT_TRUE(OwnsTownWalkGoal(town, {true, town}, {}, {}, false, {}, false, 0, {}));
    EXPECT_TRUE(OwnsTownWalkGoal(town, {}, {true, town}, {}, false, {}, false, 0, {}));
    EXPECT_TRUE(OwnsTownWalkGoal(
        town, {true, town}, {true, town}, prepared, true, segmentEnd, true, 77, segmentEnd));
    EXPECT_TRUE(OwnsTownWalkGoal(town, {}, {}, prepared, true, segmentEnd, true, 77, segmentEnd));
    EXPECT_FALSE(OwnsTownWalkGoal(town, {true, unrelated}, {}, {}, false, {}, false, 0, {}));
    EXPECT_FALSE(OwnsTownWalkGoal(town, {}, {true, unrelated}, {}, false, {}, false, 0, {}));
    EXPECT_FALSE(OwnsTownWalkGoal(
        town, {true, town}, {true, unrelated}, prepared, true, segmentEnd, true, 77, segmentEnd));
    EXPECT_FALSE(OwnsTownWalkGoal(town, {}, {}, prepared, true, unrelated, true, 77, segmentEnd));
    EXPECT_FALSE(OwnsTownWalkGoal(town, {}, {}, prepared, true, segmentEnd, true, 78, segmentEnd));
    EXPECT_FALSE(OwnsTownWalkGoal(town, {}, {}, prepared, false, segmentEnd, false, 77, segmentEnd));

    // A matching old scheduler goal never grants ownership of a newer native spline.
    EXPECT_FALSE(OwnsTownWalkGoal(
        town, {}, {true, town}, {}, false, {}, true, 90, unrelated));
    PreparedWalkSegment stalePrepared = prepared;
    stalePrepared.splineId = 77;
    EXPECT_FALSE(OwnsTownWalkGoal(
        town, {}, {true, town}, stalePrepared, true, segmentEnd, true, 90, unrelated));
    // The same veto applies to unrelated active LastMovement metadata without a prepared record.
    EXPECT_FALSE(OwnsTownWalkGoal(
        town, {}, {true, town}, {}, true, unrelated, false, 0, {}));
    EXPECT_TRUE(OwnsTownWalkGoal(
        town, {}, {true, town}, {}, true, town, false, 0, {}));
}

TEST(Errands, FreshDirectAndTravelMgrProofsExecuteThroughPreparedSeam)
{
    WalkGoalKey const town{1, 10127, 2224, 1328};
    PreparedWalkFacts facts;
    facts.requestedGoal = town;
    facts.sourceZone = 148;
    facts.freshProof = PreparedWalkProofKind::Direct;
    EXPECT_EQ(DecidePreparedWalk({}, facts), PreparedWalkAction::ExecuteFresh);

    facts.freshProof = PreparedWalkProofKind::TravelMgr;
    EXPECT_EQ(DecidePreparedWalk({}, facts), PreparedWalkAction::ExecuteFresh);
    facts.freshProof = PreparedWalkProofKind::None;
    EXPECT_EQ(DecidePreparedWalk({}, facts), PreparedWalkAction::Deny);
}

TEST(Errands, StartWaitsForActivePredecessorButCanRetireFinishedMetadata)
{
    EXPECT_EQ(DecideErrandsStartHandoff(false, false),
              ErrandsStartHandoff::RetireFinishedPredecessor);
    EXPECT_EQ(DecideErrandsStartHandoff(true, false),
              ErrandsStartHandoff::WaitForActivePredecessor);
    EXPECT_EQ(DecideErrandsStartHandoff(false, true),
              ErrandsStartHandoff::WaitForActivePredecessor);
}

TEST(Errands, OwnedFinishedPredecessorRetiresOnlyAfterPhysicalMotionEnds)
{
    WalkGoalKey const flightMaster{530, -225, 1050, 54};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::TravelFlight;
    before.generation = 41;
    before.expectedGoal = flightMaster;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, 0, before.generation, flightMaster, before);

    ErrandsPredecessorFacts issued = before;
    issued.moveFar = {true, flightMaster};
    issued.intent = {true, flightMaster, 100, 110, 1, 0};
    issued.movement = {true, {530, -180, 1020, 52}, 111};
    ObserveErrandsPredecessorCall(predecessor, before, issued);
    ASSERT_TRUE(predecessor.moveFarOwned);
    ASSERT_TRUE(predecessor.intentOwned);
    ASSERT_TRUE(predecessor.movementOwned);

    issued.botMoving = true;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, issued),
              ErrandsPredecessorDecision::WaitForMotion);
    issued.botMoving = false;
    issued.nativeSplineActive = true;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, issued),
              ErrandsPredecessorDecision::WaitForMotion);
    issued.nativeSplineActive = false;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, issued),
              ErrandsPredecessorDecision::RetireOwned);
}

TEST(Errands, NewerSameGoalIntentAndMovementAreNeverRelabelled)
{
    WalkGoalKey const stopGoal{530, -713, 2613, 94};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 2;
    before.generation = 77;
    before.expectedGoal = stopGoal;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, stopGoal, before);

    ErrandsPredecessorFacts owned = before;
    owned.moveFar = {true, stopGoal};
    owned.intent = {true, stopGoal, 100, 110, 1, 0};
    owned.movement = {true, stopGoal, 111};
    ObserveErrandsPredecessorCall(predecessor, before, owned);

    ErrandsPredecessorFacts foreign = owned;
    foreign.intent.createdMs = 200;
    foreign.intent.lastObservedMs = 210;
    foreign.movement.msTime = 211;
    ErrandsPredecessorFacts afterForeignCall = foreign;
    afterForeignCall.intent.lastObservedMs = 220;
    afterForeignCall.movement.msTime = 221;
    ObserveErrandsPredecessorCall(predecessor, foreign, afterForeignCall);

    EXPECT_EQ(predecessor.ownedIntent.createdMs, 100U);
    EXPECT_EQ(predecessor.ownedMovement.msTime, 111U);
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, afterForeignCall),
              ErrandsPredecessorDecision::PreserveTravelIntent);
    afterForeignCall.intent = {};
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, afterForeignCall),
              ErrandsPredecessorDecision::PreserveLastMovement);
}

TEST(Errands, OwnedCallMayAdvanceOnlyItsUnchangedPriorIdentity)
{
    WalkGoalKey const goal{530, -713, 2613, 94};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 1;
    before.generation = 3;
    before.expectedGoal = goal;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, goal, before);
    ErrandsPredecessorFacts first = before;
    first.moveFar = {true, goal, 1, 2, 3};
    first.intent = {true, goal, 10, 20, 1, 0};
    first.movement = {true, goal, 30};
    ObserveErrandsPredecessorCall(predecessor, before, first);

    ErrandsPredecessorFacts next = first;
    next.intent.lastObservedMs = 40;
    next.intent.segmentsCommitted = 2;
    next.movement.msTime = 50;
    ObserveErrandsPredecessorCall(predecessor, first, next);
    EXPECT_EQ(predecessor.ownedIntent.lastObservedMs, 40U);
    EXPECT_EQ(predecessor.ownedMovement.msTime, 50U);
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, next),
              ErrandsPredecessorDecision::RetireOwned);
}

TEST(Errands, NewerSubYardMoveFarIsPreservedEvenWhenIntegerGoalMatches)
{
    WalkGoalKey const goal{530, -713, 2613, 94};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 1;
    before.generation = 4;
    before.expectedGoal = goal;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, goal, before);
    ErrandsPredecessorFacts owned = before;
    owned.moveFar = {true, goal, 10, 20, 30};
    owned.intent = {true, goal, 100, 110, 1, 0};
    owned.movement = {true, goal, 120};
    ObserveErrandsPredecessorCall(predecessor, before, owned);

    ErrandsPredecessorFacts foreign = owned;
    foreign.moveFar.xBits = 11;  // same integer yard, different native float position
    ObserveErrandsPredecessorCall(predecessor, foreign, foreign);
    EXPECT_EQ(predecessor.ownedMoveFar.xBits, 10U);
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, foreign),
              ErrandsPredecessorDecision::PreserveMoveFar);
}

TEST(Errands, PredecessorSourceGenerationAndGoalMismatchFailClosed)
{
    WalkGoalKey const expected{1, 100, 200, 30};
    ErrandsPredecessorFacts facts;
    facts.source = ErrandsPredecessorSource::ReturnFlight;
    facts.generation = 9;
    facts.expectedGoal = expected;
    ErrandsPredecessor const predecessor = BeginErrandsPredecessor(
        facts.source, 0, facts.generation, expected, facts);

    facts.source = ErrandsPredecessorSource::TravelFlight;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, facts),
              ErrandsPredecessorDecision::PreserveSourceMismatch);
    facts.source = ErrandsPredecessorSource::ReturnFlight;
    ++facts.generation;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, facts),
              ErrandsPredecessorDecision::PreserveSourceMismatch);
    --facts.generation;
    ++facts.expectedGoal.x;
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, facts),
              ErrandsPredecessorDecision::PreserveSourceMismatch);
}

TEST(Errands, BareSameGoalMoveFarIsAmbiguousAndPreserved)
{
    WalkGoalKey const expected{1, 100, 200, 30};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 1;
    before.generation = 2;
    before.expectedGoal = expected;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, expected, before);
    ErrandsPredecessorFacts after = before;
    after.moveFar = {true, expected};
    ObserveErrandsPredecessorCall(predecessor, before, after);
    ASSERT_TRUE(predecessor.moveFarOwned);
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, after),
              ErrandsPredecessorDecision::PreserveMoveFar);
}

TEST(Errands, FlightCaptureGenerationAndReturnStatusAreExact)
{
    WalkGoalKey const payload{530, -225, 1050, 54};
    ErrandsPredecessorFacts baseline;
    baseline.source = ErrandsPredecessorSource::ReturnFlight;
    baseline.generation = 8;
    baseline.expectedGoal = payload;
    ErrandsPredecessor const predecessor = BeginErrandsPredecessor(
        baseline.source, 0, baseline.generation, payload, baseline);
    ErrandsFlightCaptureToken token{true, baseline.source, baseline.generation, payload};
    EXPECT_TRUE(MatchesFlightCapture(predecessor, token));
    EXPECT_TRUE(FlightStatusOwnsPredecessor(predecessor, true));
    EXPECT_FALSE(FlightStatusOwnsPredecessor(predecessor, false));
    ++token.generation;
    EXPECT_FALSE(MatchesFlightCapture(predecessor, token));
}

TEST(Errands, ReturnFlightCrossZoneLandingRetiresWithoutZoneContinuity)
{
    WalkGoalKey const flightMaster{530, -225, 1050, 54};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ReturnFlight;
    before.generation = 9;
    before.expectedGoal = flightMaster;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, 0, before.generation, flightMaster, before);
    ErrandsPredecessorFacts issued = before;
    issued.moveFar = {true, flightMaster, 1, 2, 3};
    issued.intent = {true, flightMaster, 10, 20, 1, 0};
    issued.movement = {true, flightMaster, 30};
    ObserveErrandsPredecessorCall(predecessor, before, issued);

    // Zone is deliberately absent from predecessor identity: a normal taxi may land in another
    // zone on the same allowed map. Only source, generation, payload and exact generic owners matter.
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, issued),
              ErrandsPredecessorDecision::RetireOwned);
}

TEST(Errands, ServiceCompletionAndTimeoutRequireOldOwnerBeforeNextBoundary)
{
    WalkGoalKey const firstGoal{530, -713, 2613, 94};
    WalkGoalKey const secondGoal{530, -680, 2640, 95};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 1;
    before.generation = 12;
    before.expectedGoal = firstGoal;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, firstGoal, before);
    EXPECT_FALSE(ServiceBoundaryNeedsHandoff(predecessor, false, 1, firstGoal));
    EXPECT_TRUE(ServiceBoundaryNeedsHandoff(predecessor, false, 2, secondGoal));
    EXPECT_TRUE(ServiceBoundaryNeedsHandoff(predecessor, true, 1, firstGoal));

    ErrandsPredecessorFacts owned = before;
    owned.moveFar = {true, firstGoal, 1, 2, 3};
    owned.intent = {true, firstGoal, 10, 20, 1, 0};
    owned.movement = {true, firstGoal, 30};
    ObserveErrandsPredecessorCall(predecessor, before, owned);
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, owned),
              ErrandsPredecessorDecision::RetireOwned);

    predecessor = {};
    EXPECT_FALSE(ServiceBoundaryNeedsHandoff(predecessor, true, 2, secondGoal));
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, {}),
              ErrandsPredecessorDecision::CompleteAbsent);
}

TEST(Errands, ForeignOwnerIsPreservedAndEnclosingPhaseRemainsFinite)
{
    WalkGoalKey const expected{1, 100, 200, 30};
    WalkGoalKey const foreignGoal{1, 400, 500, 60};
    ErrandsPredecessorFacts before;
    before.source = ErrandsPredecessorSource::ServiceStop;
    before.stop = 1;
    before.generation = 14;
    before.expectedGoal = expected;
    ErrandsPredecessor predecessor = BeginErrandsPredecessor(
        before.source, before.stop, before.generation, expected, before);
    ErrandsPredecessorFacts foreign = before;
    foreign.moveFar = {true, foreignGoal, 4, 5, 6};
    EXPECT_EQ(DecideErrandsPredecessorHandoff(predecessor, foreign),
              ErrandsPredecessorDecision::PreserveMoveFar);

    Params p;
    BotState s;
    s.phaseMs = 100;
    EXPECT_TRUE(LegExhausted(p, s, 101 + p.returnTimeoutMs, p.returnTimeoutMs));
}

TEST(Errands, PredecessorGenerationNeverWrapsOrReuses)
{
    std::uint64_t generation = 0;
    EXPECT_EQ(NextErrandsPredecessorGeneration(generation), 1U);
    generation = UINT64_MAX - 1;
    EXPECT_EQ(NextErrandsPredecessorGeneration(generation), UINT64_MAX);
    EXPECT_EQ(NextErrandsPredecessorGeneration(generation), 0U);
    EXPECT_EQ(generation, UINT64_MAX);
}

TEST(Errands, OnlyExactLivePreparedSegmentBypassesFreshProbe)
{
    WalkGoalKey const town{1, 10127, 2224, 1328};
    WalkGoalKey const endpoint{1, 10020, 2190, 1300};
    WalkGoalKey const unrelated{1, 9000, 2000, 10};
    PreparedWalkSegment const segment{
        true, town, endpoint, 1, 148, 77, PreparedWalkProofKind::TravelMgr};
    PreparedWalkFacts facts;
    facts.requestedGoal = town;
    facts.sourceMap = 1;
    facts.sourceZone = 148;
    facts.movementActive = true;
    facts.movementEndpoint = endpoint;
    facts.nativeSplineActive = true;
    facts.nativeSplineId = 77;
    facts.nativeSplineEndpoint = endpoint;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ContinueExact);

    facts.movementActive = false;  // death / stopped motion invalidates the record
    facts.freshProof = PreparedWalkProofKind::Direct;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.movementActive = true;
    facts.sourceZone = 141;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.sourceZone = 148;
    facts.requestedGoal = unrelated;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.requestedGoal = town;
    facts.movementEndpoint = unrelated;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.movementEndpoint = endpoint;
    facts.nativeSplineId = 78;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.nativeSplineId = 77;
    facts.nativeSplineActive = false;  // stale LastMovement metadata with the same endpoint is insufficient
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.nativeSplineActive = true;
    facts.sourceMap = 0;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::ExecuteFresh);
    facts.sourceMap = 1;

    facts.ownershipConflict = true;
    EXPECT_EQ(DecidePreparedWalk(segment, facts), PreparedWalkAction::WaitForConflict);
}

TEST(Errands, OnlyAttemptedTravelWalkFailureIsRemembered)
{
    EXPECT_TRUE(ShouldRememberTownWalkFailure(Outcome::TravelGaveUp, true));
    EXPECT_FALSE(ShouldRememberTownWalkFailure(Outcome::TravelGaveUp, false));
    EXPECT_FALSE(ShouldRememberTownWalkFailure(Outcome::Done, true));
    EXPECT_FALSE(ShouldRememberTownWalkFailure(Outcome::ReturnGaveUp, true));
}

TEST(Errands, AfterRunKeepsTrainLevelAndCoolsDown)
{
    Params p;
    BotState s;
    s.phase = Phase::Return;
    s.town = 99;
    s.lastClassTrainLevel = 24;
    s.spent = 500;
    s.predecessorGeneration = 44;
    s.predecessor.active = true;
    s.sellerRetry.count = 1;
    s.sellerRetry.spawns[0] = 77;
    RememberTownWalkFailure(s.walkBackoffs, 46341, 148, NeedProfTrain, 100, 5000);
    BotState const n = AfterRun(p, s, 1000);
    EXPECT_EQ(n.phase, Phase::None);
    EXPECT_EQ(n.town, 0U);
    EXPECT_EQ(n.spent, 0U);
    EXPECT_EQ(n.predecessorGeneration, 44U);
    EXPECT_FALSE(n.predecessor.active);
    EXPECT_EQ(n.lastClassTrainLevel, 24U);
    EXPECT_EQ(n.sellerRetry.count, 0U);
    EXPECT_TRUE(TownWalkBackedOff(n.walkBackoffs, 46341, 148, NeedProfTrain, 1000));
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

TEST(Errands, RelocationRetiresOnlyExpiredTravelOrReturn)
{
    Params p;
    BotState s;
    RelocationRetirementFacts facts = EligibleRelocationRetirement();
    s.phase = Phase::Travel;
    s.phaseMs = 1000;

    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 1000 + p.travelTimeoutMs), RelocationRetirement::Wait);
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 1001 + p.travelTimeoutMs), RelocationRetirement::TravelGaveUp);

    s.phase = Phase::Return;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 1001 + p.returnTimeoutMs), RelocationRetirement::ReturnGaveUp);

    s.phase = Phase::Errands;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 1001 + p.returnTimeoutMs), RelocationRetirement::Wait);
    s.phase = Phase::None;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 1001 + p.returnTimeoutMs), RelocationRetirement::Wait);
}

TEST(Errands, RelocationRetirementFailsClosedOnInvalidStateOrClock)
{
    Params p;
    BotState s;
    RelocationRetirementFacts facts = EligibleRelocationRetirement();
    s.phase = Phase::Travel;
    s.phaseMs = 5000;
    s.reissues = p.maxReissues + 1;

    facts.tracked = false;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 5001 + p.travelTimeoutMs), RelocationRetirement::Wait);
    facts.tracked = true;
    s.version = kStateVersion - 1;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 5001 + p.travelTimeoutMs), RelocationRetirement::Wait);
    s.version = kStateVersion;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 4999), RelocationRetirement::Wait);
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 5000), RelocationRetirement::Wait);
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, 5001 + p.travelTimeoutMs), RelocationRetirement::TravelGaveUp);
}

TEST(Errands, RelocationRetirementRequiresStrictRuntimeAdmission)
{
    RelocationRetirementFacts facts = EligibleRelocationRetirement();
    EXPECT_TRUE(RelocationRetirementAdmitted(facts));
    facts.alive = false;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.alive = true;
    facts.inWorld = false;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.inWorld = true;
    facts.duringRemove = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.duringRemove = false;
    facts.inFlight = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.inFlight = false;
    facts.inCombat = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.inCombat = false;
    facts.aiCombat = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.aiCombat = false;
    facts.independent = false;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.independent = true;
    facts.hasMaster = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.hasMaster = false;
    facts.selfControlled = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.selfControlled = false;
    facts.realPlayerMaster = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.realPlayerMaster = false;
    facts.paused = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.paused = false;
    facts.grouped = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.grouped = false;
    facts.oracleManaged = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.oracleManaged = false;
    facts.mapAvailable = false;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.mapAvailable = true;
    facts.instance = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.instance = false;
    facts.transport = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.transport = false;
    facts.vehicle = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.vehicle = false;
    facts.teleporting = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.teleporting = false;
    facts.casting = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.casting = false;
    facts.rooted = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.rooted = false;
    facts.stunned = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.stunned = false;
    facts.movementAllowed = false;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.movementAllowed = true;
    facts.botMoving = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.botMoving = false;
    facts.movementPending = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.movementPending = false;
    facts.nativeSplineActive = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.nativeSplineActive = false;
    facts.moveFarActive = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.moveFarActive = false;
    facts.travelIntentActive = true;
    EXPECT_FALSE(RelocationRetirementAdmitted(facts));
    facts.travelIntentActive = false;
    EXPECT_TRUE(RelocationRetirementAdmitted(facts));
}

TEST(Errands, RelocationRetirementRequiresNoTypedPredecessor)
{
    Params p;
    BotState s;
    s.phase = Phase::Travel;
    s.phaseMs = 1000;
    std::uint64_t const now = 1001 + p.travelTimeoutMs;
    RelocationRetirementFacts facts = EligibleRelocationRetirement();

    facts.predecessorActive = true;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, now), RelocationRetirement::Wait);
    facts.predecessorActive = false;
    EXPECT_EQ(DecideRelocationRetirement(p, s, facts, now), RelocationRetirement::TravelGaveUp);
}

TEST(Errands, RelocationRetirementResetsRunBeforeNextEscapeTick)
{
    Params p;
    BotState s;
    s.phase = Phase::Travel;
    s.phaseMs = 1000;
    s.town = 10076;
    s.predecessorGeneration = 44;
    RelocationRetirementFacts facts = EligibleRelocationRetirement();
    std::uint64_t const now = 1001 + p.travelTimeoutMs;

    ASSERT_EQ(DecideRelocationRetirement(p, s, facts, now), RelocationRetirement::TravelGaveUp);
    s.outcome = Outcome::TravelGaveUp;
    BotState const next = AfterRun(p, s, now);
    EXPECT_FALSE(IsRunActive(next));
    EXPECT_EQ(next.town, 0U);
    EXPECT_EQ(next.predecessorGeneration, 44U);
    EXPECT_EQ(next.cooldownUntilMs, now + p.cooldownMs);
    EXPECT_EQ(DecideRelocationRetirement(p, next, facts, now), RelocationRetirement::Wait);
}

TEST(ErrandsRelocationSourceContract, TerminalReceiptPrecedesEscapeAndUnblocksTheFollowingTick)
{
    std::string const action = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::string const errands = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgErrands.cpp");
    std::string const zone = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    ASSERT_FALSE(action.empty());
    ASSERT_FALSE(errands.empty());
    ASSERT_FALSE(zone.empty());

    std::size_t const relocationGate =
        action.find("if (status != RPG_TRAVEL_FLIGHT && bot->IsAlive() && relocationOwners.Any())");
    std::size_t const terminal = action.find("ErrandsStep(true)", relocationGate);
    std::size_t const escape = action.find("TryDeathLoopEscape()", terminal);
    ASSERT_NE(relocationGate, std::string::npos);
    ASSERT_NE(terminal, std::string::npos);
    ASSERT_NE(escape, std::string::npos);
    EXPECT_LT(relocationGate, terminal);
    EXPECT_LT(terminal, escape);
    EXPECT_NE(action.substr(terminal, escape - terminal).find("return true;"), std::string::npos);
    EXPECT_NE(action.find("AcceptRelocationOwners(", escape), std::string::npos);

    std::size_t const step = errands.find("bool NewRpgBaseAction::ErrandsStep(bool relocationRetirementOnly)");
    std::size_t const admission = errands.find("if (!RelocationRetirementAdmitted(retirementFacts))", step);
    std::size_t const stateRead = errands.find("tracked = ReadStateForDiagnostics(guid, s);", admission);
    ASSERT_NE(step, std::string::npos);
    ASSERT_NE(admission, std::string::npos);
    ASSERT_NE(stateRead, std::string::npos);
    EXPECT_LT(admission, stateRead);

    std::size_t const finish = errands.find("auto finish = [&]()");
    std::size_t const retirement = errands.find("if (relocationRetirementOnly)", finish);
    ASSERT_NE(finish, std::string::npos);
    ASSERT_NE(retirement, std::string::npos);
    std::size_t const rescue = errands.find("RescueLeg(", retirement);
    ASSERT_NE(rescue, std::string::npos);
    EXPECT_LT(retirement, rescue);
    std::string const terminalReceipt = errands.substr(finish, retirement - finish);
    EXPECT_NE(terminalReceipt.find("s = AfterRun(p, s, now);"), std::string::npos);
    EXPECT_NE(terminalReceipt.find("StoreState(guid, s);"), std::string::npos);
    EXPECT_NE(terminalReceipt.find("info.ChangeToIdle();"), std::string::npos);

    EXPECT_NE(errands.find("bool Active(std::uint32_t guid) { return IsRunActive(LoadState(guid)); }"),
              std::string::npos);
    EXPECT_NE(zone.find("AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)"), std::string::npos);
    std::size_t const zoneStep = zone.find("bool NewRpgBaseAction::ZoneProgressionStep()");
    ASSERT_NE(zoneStep, std::string::npos);
    EXPECT_NE(zone.find("MovementBlock(bot, botAI)", zoneStep), std::string::npos);
    EXPECT_NE(zone.find("if (!Movable(bot, botAI))", zoneStep), std::string::npos);
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
    EXPECT_EQ(s.version, kStateVersion);
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
    std::uint64_t const budget = AutoWowGear::MinBudgetCopper(20);  // 2000
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

// ---- AutoWow.Gear.Flow (GearUpgradePolicy.h Flows / PickTaker) ----
TEST(GearFlow, OnlyTradeableGearNoUpgradeForTheHolderFlows)
{
    AutoWowGear::FlowParams fp;
    EXPECT_TRUE(AutoWowGear::Flows(fp, 2, true, true, false));   // green BoE the looter will not wear
    EXPECT_TRUE(AutoWowGear::Flows(fp, 1, true, true, false));   // white: default FlowMinQuality 1
    EXPECT_FALSE(AutoWowGear::Flows(fp, 0, true, true, false));  // grey
    EXPECT_FALSE(AutoWowGear::Flows(fp, 2, true, true, true));   // the holder's own upgrade stays
    EXPECT_FALSE(AutoWowGear::Flows(fp, 2, true, false, false)); // bound (worn once): the mail would refuse it
    EXPECT_FALSE(AutoWowGear::Flows(fp, 3, false, true, false)); // not a weapon / armor piece
    fp.minQuality = 2;
    EXPECT_FALSE(AutoWowGear::Flows(fp, 1, true, true, false));
}

TEST(GearFlow, TakerIsTheBiggestGainThenLowerGuid)
{
    using T = AutoWowGear::FlowTaker;
    EXPECT_EQ(AutoWowGear::PickTaker({}), AutoWowGear::kNone);
    EXPECT_EQ(AutoWowGear::PickTaker({T{62970, 0}}), AutoWowGear::kNone);  // no gain: nobody
    EXPECT_EQ(AutoWowGear::PickTaker({T{62970, 5}, T{62960, 12}, T{62980, 3}}), 1U);
    EXPECT_EQ(AutoWowGear::PickTaker({T{62990, 12}, T{62960, 12}, T{62955, 4}}), 1U);  // tie: lower guid
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

TEST(Outfit, MissingToolIsSoftAndAloneStartsARunOncePerWindow)
{
    EXPECT_EQ(MissingTools(ToolPick | ToolKnife, ToolKnife), std::uint8_t(ToolPick));
    EXPECT_EQ(MissingTools(0, 0), 0u);
    EXPECT_EQ(MissingTools(ToolPick, ToolPick | ToolKnife), 0u);
    EXPECT_TRUE(OutfitRunDue(ToolPick, 1000, 1000));
    EXPECT_FALSE(OutfitRunDue(ToolPick, 999, 1000));
    EXPECT_FALSE(OutfitRunDue(0, 5000, 0));

    Params const p;
    Obs o = Healthy(kClassWarrior, 12);
    o.missingTools = ToolPick;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedTool));
    EXPECT_EQ(a.urgent, 0U);
    EXPECT_FALSE(ShouldRun(a.needs, a.urgent));
    o.toolRunDue = true;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedTool));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    o.missingTools = 0;  // flag off: all zero
    EXPECT_EQ(Assess(p, o).needs, 0U);

    TownFacts f;
    EXPECT_EQ(Serves(f) & NeedTool, 0U);
    f.tools = ToolKnife;
    EXPECT_EQ(Serves(f) & NeedTool, std::uint32_t(NeedTool));

    BotState s;
    s.nextOutfitMs = 700000;
    EXPECT_EQ(AfterRun(p, s, 1000).nextOutfitMs, 700000U);
    EXPECT_EQ(kStateVersion, 14u);
    EXPECT_EQ(kToolItems[0], 2901u);
    EXPECT_EQ(kToolItems[1], 7005u);
}

// Lane U (AutoWow.Supply.MailPickup): a bag / potions mail from the house is a soft need, alone a run once per
// MailRunMs, served by any town with a mailbox; the window survives runs.
TEST(MailPickup, SupplyMailIsSoftAndAloneStartsARunOncePerWindow)
{
    EXPECT_TRUE(MailRunDue(true, 1000, 1000));
    EXPECT_FALSE(MailRunDue(true, 999, 1000));
    EXPECT_FALSE(MailRunDue(false, 5000, 0));

    Params const p;
    Obs o = Healthy(kClassWarrior, 12);
    o.supplyMail = true;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs, std::uint32_t(NeedMail));
    EXPECT_EQ(a.urgent, 0U);
    EXPECT_FALSE(ShouldRun(a.needs, a.urgent));
    o.mailRunDue = true;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedMail));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    o.supplyMail = false;  // flag off: all zero
    EXPECT_EQ(Assess(p, o).needs, 0U);

    TownFacts f;
    EXPECT_EQ(Serves(f) & NeedMail, 0U);
    f.mailbox = true;
    EXPECT_EQ(Serves(f) & NeedMail, std::uint32_t(NeedMail));
    EXPECT_EQ(std::uint32_t(NeedMail), 1u << 12);  // wire-stable ledger bit

    BotState s;
    s.nextMailMs = 900000;
    EXPECT_EQ(AfterRun(p, s, 1000).nextMailMs, 900000U);
}

TEST(Outfit, ToolStopsFollowTrainingAtTheFirstVendorSellingEach)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0);
    Npc trainer = MakeNpc(8, RoleTradeTrainer, 30);
    Npc supplies = MakeNpc(9, RoleVendor, 40);
    supplies.tools = ToolPick | ToolKnife;
    Npc leather = MakeNpc(7, RoleVendor, 50);
    leather.tools = ToolKnife;  // lower spawn: first for the knife
    Npc horde = MakeNpc(6, RoleVendor, 60, kHorde);
    horde.tools = ToolPick;  // unusable for the alliance bot
    t.npcs = {inn, horde, leather, trainer, supplies};
    PlanInput in;
    in.team = kAlliance;
    in.trainers = {8};
    in.tools = ToolPick | ToolKnife;
    Plan const plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 4U);
    EXPECT_EQ(plan.stops[1].spawn, 8U);
    EXPECT_EQ(plan.stops[2].spawn, 9U);  // pick
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpTool));
    EXPECT_EQ(plan.stops[3].spawn, 7U);  // knife
    EXPECT_EQ(plan.stops[3].ops, std::uint32_t(OpTool));
    in.tools = 0;  // flag off: no tool stop
    EXPECT_EQ(PlanStops(t, in).count, 2U);
}

TEST(ErrandsPolicy, AuctionDetourDiscountsOnlyAuctionTowns)
{
    EXPECT_EQ(DetourCostMs(300000, true, 180000), 120000u);
    EXPECT_EQ(DetourCostMs(100000, true, 180000), 0u);
    EXPECT_EQ(DetourCostMs(300000, false, 180000), 300000u);
    EXPECT_EQ(DetourCostMs(300000, true, 0), 300000u);
}

// ---- AutoWow.Supply.OutfitGear floors (lane AN) ---------------------------------------------------------------
TEST(OutfitGear, FloorDueOncePerLevelWhateverThePurse)
{
    AutoWowGear::Params const p;  // floor at L36: 50 % of 25.2 DPS = 12.6
    EXPECT_TRUE(AutoWowGear::FloorDue(p, 36, 35, 937, false, 0));     // starter dagger at L36
    EXPECT_FALSE(AutoWowGear::FloorDue(p, 36, 36, 937, false, 0));    // shopped this level
    EXPECT_FALSE(AutoWowGear::FloorDue(p, 36, 35, 12600, false, 0));  // on the floor; no ranged for a rogue
    EXPECT_TRUE(AutoWowGear::FloorDue(p, 36, 35, 12600, true, 0));    // a hunter without a bow
    EXPECT_FALSE(AutoWowGear::FloorDue(p, 36, 35, 12600, true, 12600));
    Obs o;  // the assessment turns it urgent: a run on its own
    o.gear.soft = o.gear.urgent = true;
    Assessment const a = Assess(Params{}, o);
    EXPECT_TRUE(a.urgent & NeedGear);
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
}

TEST(OutfitGear, PickFloorWeaponOwnGoldThenCheapestGrantThenBest)
{
    AutoWowGear::Params const p;
    using AutoWowGear::kSlotMainHand;
    std::vector<AutoWowGear::Offer> const offers = {
        Weapon(101, 1, 5800, 14300, kSlotMainHand),   // clears 12.6
        Weapon(102, 1, 27000, 21000, kSlotMainHand),  // clears
        Weapon(103, 1, 1300, 7300, kSlotMainHand)};   // an upgrade, under the floor
    // Own gold buys the best: it clears the floor.
    EXPECT_EQ(AutoWowGear::PickFloorWeapon(p, offers, kSlotMainHand, 937, 36, 30000, 0), 1U);
    // Own gold only reaches 103 (under the floor): the cheapest clearing one within gold + room.
    EXPECT_EQ(AutoWowGear::PickFloorWeapon(p, offers, kSlotMainHand, 937, 36, 2000, 32400), 0U);
    // Nothing clears within reach: the best upgrade there is.
    EXPECT_EQ(AutoWowGear::PickFloorWeapon(p, offers, kSlotMainHand, 937, 36, 2000, 0), 2U);
    EXPECT_EQ(AutoWowGear::PickFloorWeapon(p, offers, kSlotMainHand, 937, 36, 0, 1000), AutoWowGear::kNone);
    // Ties on price: lower item, then lower npc.
    std::vector<AutoWowGear::Offer> const tie = {Weapon(105, 9, 5800, 14300, kSlotMainHand),
                                                 Weapon(104, 9, 5800, 13000, kSlotMainHand)};
    EXPECT_EQ(AutoWowGear::PickFloorWeapon(p, tie, kSlotMainHand, 937, 36, 0, 6000), 1U);
}

TEST(OutfitGear, FloorWeaponsMainHandThenHunterRangedDrawDownGoldThenRoom)
{
    AutoWowGear::Params const p;
    using AutoWowGear::kSlotMainHand;
    using AutoWowGear::kSlotRanged;
    std::vector<AutoWowGear::Offer> const offers = {Weapon(101, 1, 5800, 14300, kSlotMainHand),
                                                    Weapon(201, 2, 6300, 12900, kSlotRanged)};
    // 2000 own + 3800 room for the sword; 6300 of the 28600 room left for the bow.
    std::vector<AutoWowGear::Offer> list = AutoWowGear::FloorWeapons(p, offers, 36, 937, true, 0, 2000, 32400);
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[0].item, 101U);
    EXPECT_EQ(list[1].item, 201U);
    EXPECT_EQ(list[1].slot, kSlotRanged);
    // Room for the sword only.
    list = AutoWowGear::FloorWeapons(p, offers, 36, 937, true, 0, 2000, 3800);
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list[0].item, 101U);
    EXPECT_TRUE(AutoWowGear::FloorWeapons(p, offers, 36, 937, false, 0, 0, 3000).empty());     // out of reach
    EXPECT_TRUE(AutoWowGear::FloorWeapons(p, offers, 36, 12600, false, 0, 99999, 0).empty());  // on the floor
    list = AutoWowGear::FloorWeapons(p, offers, 36, 12600, true, 0, 99999, 0);  // hunter: the bow only
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list[0].item, 201U);
}

TEST(OutfitGear, FoodFloorKindsAndCopper)
{
    EXPECT_EQ(KindsFor(kClassMage, 30, AmmoNone), 0U);  // conjures: the plain restock buys nothing
    EXPECT_EQ(FloorKinds(kClassMage, 30, AmmoNone), std::uint32_t((1u << KindFood) | (1u << KindWater)));
    EXPECT_EQ(FloorKinds(kClassRogue, 30, AmmoNone), std::uint32_t(1u << KindFood));
    EXPECT_EQ(FloorKinds(kClassPriest, 30, AmmoNone), std::uint32_t((1u << KindFood) | (1u << KindWater)));
    // Soft Banana Bread 4601: 2000 copper per pack of 5.
    EXPECT_EQ(FloorCopper(20, 5, 2000), 8000U);
    EXPECT_EQ(FloorCopper(16, 5, 2000), 8000U);  // whole packs
    EXPECT_EQ(FloorCopper(15, 5, 2000), 6000U);
    EXPECT_EQ(FloorCopper(0, 5, 2000), 0U);
    EXPECT_EQ(FloorCopper(3, 0, 25), 75U);  // a BuyCount of 0 sells one
}

TEST(OutfitGear, PlanStopsFloorWeaponBeforeTrainer)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0);
    Npc smith = MakeNpc(7, RoleRepair | RoleVendor, 20);
    Npc trainer = MakeNpc(8, RoleClassTrainer, 30);
    Npc weapons = MakeNpc(9, RoleVendor, 40);
    t.npcs = {inn, smith, trainer, weapons};
    PlanInput in;
    in.team = kAlliance;
    in.trainers = {8};
    in.gearNpcs = {9};
    Plan plan = PlanStops(t, in);  // flag off: gear after training
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.stops[1].spawn, 8U);
    EXPECT_EQ(plan.stops[2].spawn, 9U);
    in.gearFirst = true;
    plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.stops[1].spawn, 9U);
    EXPECT_EQ(plan.stops[1].ops, std::uint32_t(OpGear));
    EXPECT_EQ(plan.stops[2].spawn, 8U);
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpTrain));
    EXPECT_EQ(std::uint32_t(DoneWeaponFloor), 512U);
    EXPECT_EQ(std::uint32_t(DoneFoodFloor), 1024U);
}

// ---- AutoWow.Gear.AuctionUpgrades (GearUpgradePolicy.h AhPlan / AhRunDue; ErrandsPolicy NeedAhGear) -----------
namespace
{
AutoWowGear::AhOffer Ah(std::uint32_t id, std::uint8_t slot, std::uint64_t price, std::uint32_t gain, bool twoHand = false)
{
    AutoWowGear::AhOffer o;
    o.id = id;
    o.item = 1000 + id;
    o.slot = slot;
    o.price = price;
    o.gain = gain;
    o.twoHand = twoHand;
    return o;
}
}  // namespace

TEST(AhGear, IlvlCurveBudgetAndRunDue)
{
    AutoWowGear::AhParams const ap;
    EXPECT_EQ(AutoWowGear::ExpectedIlvl(42), 47U);
    EXPECT_TRUE(AutoWowGear::IlvlFarBelow(ap, 29, 42));    // S65 cohort: 29 at L42 (< 35.25)
    EXPECT_TRUE(AutoWowGear::IlvlFarBelow(ap, 35, 42));
    EXPECT_FALSE(AutoWowGear::IlvlFarBelow(ap, 36, 42));
    EXPECT_EQ(AutoWowGear::AhItemCap(ap, 40), 32000U);     // 3.2 g per item at L40
    // Spendable keeps level^2 x 5 + level x 100 (L40: 8000 + 4000); the repair bill comes off first.
    EXPECT_EQ(AutoWowGear::AhBudget(50000, 40, 0), 38000U);
    EXPECT_EQ(AutoWowGear::AhBudget(50000, 40, 8000), 30000U);
    EXPECT_EQ(AutoWowGear::AhBudget(10000, 40, 0), 0U);
    EXPECT_EQ(AutoWowGear::AhBudget(5000, 40, 9000), 0U);
    // Due: new level, far below, budget >= 50% of the item cap (16000 at L40 -> money 28000 with no repair).
    EXPECT_TRUE(AutoWowGear::AhRunDue(ap, 40, 39, 28000, 0, 29));
    EXPECT_FALSE(AutoWowGear::AhRunDue(ap, 40, 39, 27999, 0, 29));   // one copper short
    EXPECT_FALSE(AutoWowGear::AhRunDue(ap, 40, 39, 28000, 1, 29));   // the repair bill tips it
    EXPECT_FALSE(AutoWowGear::AhRunDue(ap, 40, 40, 900000, 0, 29));  // level already spent
    EXPECT_FALSE(AutoWowGear::AhRunDue(ap, 40, 39, 900000, 0, 40));  // gear on the curve
}

TEST(AhGear, PlanWeaponsFirstThenArmorBestGainPerCopper)
{
    AutoWowGear::AhParams const ap;  // 3 buys, cap level^2 x 20
    std::vector<AutoWowGear::AhOffer> const offers = {
        Ah(1, 6, 9000, 10),    // legs: 10 / 9000
        Ah(2, 4, 3000, 6),     // chest: 6 / 3000 (better ratio than 3)
        Ah(3, 4, 12000, 15),   // chest: 15 / 12000
        Ah(4, 15, 20000, 8),   // main hand
        Ah(5, 0, 1000, 5),     // head: 4th in line, over the per-visit cap
    };
    std::vector<AutoWowGear::AhOffer> const plan = AutoWowGear::AhPlan(ap, offers, 40, false, 100000);
    ASSERT_EQ(plan.size(), 3U);
    EXPECT_EQ(plan[0].id, 4U);  // main hand first
    EXPECT_EQ(plan[1].id, 2U);  // chest: most gain per copper
    EXPECT_EQ(plan[2].id, 1U);  // legs
}

TEST(AhGear, PlanCapsPriceBudgetAndTies)
{
    AutoWowGear::AhParams const ap;
    // L20: item cap 8000. Main hand 8001 is over the cap; budget 5000 leaves the 6000 chest out.
    std::vector<AutoWowGear::AhOffer> offers = {Ah(1, 15, 8001, 20), Ah(2, 4, 6000, 10), Ah(3, 6, 4000, 4),
                                                Ah(4, 9, 1000, 0)};  // gain 0: never
    std::vector<AutoWowGear::AhOffer> plan = AutoWowGear::AhPlan(ap, offers, 20, false, 5000);
    ASSERT_EQ(plan.size(), 1U);
    EXPECT_EQ(plan[0].id, 3U);
    // The budget is drawn down in slot order: main hand 4000 leaves 1000, the 2000 chest no longer fits.
    offers = {Ah(1, 15, 4000, 5), Ah(2, 4, 2000, 5), Ah(3, 6, 1000, 1)};
    plan = AutoWowGear::AhPlan(ap, offers, 20, false, 5000);
    ASSERT_EQ(plan.size(), 2U);
    EXPECT_EQ(plan[0].id, 1U);
    EXPECT_EQ(plan[1].id, 3U);
    // Equal gain per copper: more gain wins; equal everything: lower auction id.
    offers = {Ah(7, 4, 2000, 4), Ah(6, 4, 1000, 2)};
    EXPECT_EQ(AutoWowGear::AhPlan(ap, offers, 20, false, 5000)[0].id, 7U);
    offers = {Ah(9, 4, 1000, 2), Ah(8, 4, 1000, 2)};
    EXPECT_EQ(AutoWowGear::AhPlan(ap, offers, 20, false, 5000)[0].id, 8U);
    // Input order does not matter.
    std::vector<AutoWowGear::AhOffer> rev(offers.rbegin(), offers.rend());
    EXPECT_EQ(AutoWowGear::AhPlan(ap, rev, 20, false, 5000)[0].id, 8U);
}

TEST(AhGear, PlanHunterRangedSecondTwoHanderSkipsOffHand)
{
    AutoWowGear::AhParams ap;
    ap.maxBuys = 9;  // clamped to kAhMaxBuys (4)
    std::vector<AutoWowGear::AhOffer> const offers = {Ah(1, 16, 500, 3), Ah(2, 17, 500, 3), Ah(3, 15, 500, 3, true),
                                                      Ah(4, 4, 500, 3), Ah(5, 6, 500, 3), Ah(6, 0, 500, 3)};
    std::vector<AutoWowGear::AhOffer> plan = AutoWowGear::AhPlan(ap, offers, 30, true, 100000);
    ASSERT_EQ(plan.size(), AutoWowGear::kAhMaxBuys);
    EXPECT_EQ(plan[0].id, 3U);  // two-hander main hand
    EXPECT_EQ(plan[1].id, 2U);  // hunter ranged
    EXPECT_EQ(plan[2].id, 4U);  // off hand skipped: chest
    EXPECT_EQ(plan[3].id, 5U);  // legs
    // A non-hunter: off hand (no two-hander bought) before armor, ranged after back.
    std::vector<AutoWowGear::AhOffer> const caster = {Ah(1, 16, 500, 3), Ah(2, 17, 500, 3), Ah(4, 4, 500, 3)};
    plan = AutoWowGear::AhPlan(ap, caster, 30, false, 100000);
    ASSERT_EQ(plan.size(), 3U);
    EXPECT_EQ(plan[0].id, 1U);
    EXPECT_EQ(plan[1].id, 4U);
    EXPECT_EQ(plan[2].id, 2U);
    ap.maxBuys = 0;  // 0 buys = nothing
    EXPECT_TRUE(AutoWowGear::AhPlan(ap, caster, 30, false, 100000).empty());
}

TEST(AhGear, AuctionTownPreferenceFallsBackOnlyForIndependentWork)
{
    EXPECT_FALSE(AuctionTownRequired(0));
    EXPECT_FALSE(AuctionTownRequired(NeedFood | NeedClassTrain));
    EXPECT_TRUE(AuctionTownRequired(NeedAhGear));
    EXPECT_TRUE(AuctionTownRequired(NeedAhGear | NeedFood));

    std::uint32_t const mixedNeeds = NeedAhGear | NeedFood;
    std::uint32_t const mixedUrgent = NeedAhGear | NeedFood;
    // A found preferred town is used; fallback is never considered.
    EXPECT_FALSE(ShouldTryNonAuctionTown(true, mixedNeeds, mixedUrgent));
    TownFacts withAuction;
    withAuction.sells = 1u << KindFood;
    withAuction.auction = true;
    std::uint32_t const preferredNeeds = mixedNeeds & Serves(withAuction);
    EXPECT_EQ(preferredNeeds, mixedNeeds);
    EXPECT_TRUE(ShouldRun(preferredNeeds, mixedUrgent & Serves(withAuction)));
    EXPECT_EQ(AhGearLevelAfterScan(39, 40, preferredNeeds, true), 40U);

    // With no safe auction town, independently urgent food can still proceed at a food town.
    EXPECT_TRUE(ShouldTryNonAuctionTown(false, mixedNeeds, mixedUrgent));
    TownFacts foodTown;
    foodTown.sells = 1u << KindFood;
    std::uint32_t const fallbackNeeds = mixedNeeds & Serves(foodTown);
    EXPECT_EQ(fallbackNeeds, std::uint32_t(NeedFood));
    EXPECT_TRUE(ShouldRun(fallbackNeeds, mixedUrgent & Serves(foodTown)));
    EXPECT_EQ(AhGearLevelAfterScan(39, 40, fallbackNeeds, false), 39U);

    // A sole auction need, or one accompanied by only one soft need, cannot bypass the auction requirement.
    EXPECT_FALSE(ShouldTryNonAuctionTown(false, NeedAhGear, NeedAhGear));
    EXPECT_FALSE(ShouldTryNonAuctionTown(false, NeedAhGear | NeedFood, NeedAhGear));
    EXPECT_EQ(AhGearLevelAfterScan(39, 40, NeedAhGear, false), 39U);
    // Two non-auction soft needs retain the existing ShouldRun admission rule.
    EXPECT_TRUE(ShouldTryNonAuctionTown(false, NeedAhGear | NeedFood | NeedRepair, NeedAhGear));
}

TEST(AhGear, LevelIsConsumedOnlyByCompletedDueScan)
{
    constexpr std::uint32_t lastLevel = 39;
    constexpr std::uint32_t level = 40;

    // No town, service rejection, travel failure, stop timeout, and an invalid house all mean no scan.
    EXPECT_EQ(AhGearLevelAfterScan(lastLevel, level, NeedAhGear, false), lastLevel);
    // A valid empty/over-cap scan consumes the assessment even when it queues no item.
    EXPECT_EQ(AhGearLevelAfterScan(lastLevel, level, NeedAhGear, true), level);
    // A valid scan that queues a buy consumes at the same boundary; purchase/equip is not the release condition.
    BotState queued;
    queued.ahGearItems[0] = 9811;
    queued.lastAhGearLevel = AhGearLevelAfterScan(lastLevel, level, NeedAhGear, true);
    EXPECT_EQ(queued.lastAhGearLevel, level);
    EXPECT_EQ(queued.ahGearItems[0], 9811U);
    // An ordinary auction visit without a due auction-gear need never consumes the level.
    EXPECT_EQ(AhGearLevelAfterScan(lastLevel, level, NeedFood, true), lastLevel);

    AutoWowGear::AhParams const ap;
    EXPECT_FALSE(AutoWowGear::AhRunDue(ap, level, queued.lastAhGearLevel, 900000, 0, 29));
    EXPECT_TRUE(AutoWowGear::AhRunDue(ap, level + 1, queued.lastAhGearLevel, 900000, 0, 29));
}

TEST(AhGear, NeedIsUrgentServedByAuctionTownAndLevelSurvivesRuns)
{
    Params p;
    Obs o = Healthy(kClassWarrior, 40);
    o.ahGearDue = true;
    Assessment const a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedAhGear));
    EXPECT_EQ(a.needs, std::uint32_t(NeedAhGear));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    EXPECT_EQ(Assess(p, Healthy(kClassWarrior, 40)).needs, 0U);  // flag off: never set
    TownFacts f;
    EXPECT_EQ(Serves(f) & NeedAhGear, 0U);
    f.auction = true;
    EXPECT_EQ(Serves(f) & NeedAhGear, std::uint32_t(NeedAhGear));
    BotState s;
    s.lastAhGearLevel = 40;
    s.ahGearItems[0] = 9811;
    BotState const next = AfterRun(p, s, 1000);
    EXPECT_EQ(next.lastAhGearLevel, 40U);
    EXPECT_EQ(next.ahGearItems[0], 0U);  // run-scoped
    // Wire-stable bits.
    EXPECT_EQ(std::uint32_t(NeedAhGear), 8192U);
    EXPECT_EQ(std::uint32_t(DoneAhGear), 2048U);
}

// ---- AutoWow.Errands.Mounts ---------------------------------------------------------------------------
TEST(Mounts, NextRideFollowsTierLevelsAndMaxTier)
{
    EXPECT_EQ(NextRide(19, 0, 0, 2).tier, 0U);  // below Apprentice
    Ride const a = NextRide(20, 0, 0, 2);
    EXPECT_EQ(a.tier, 1U);
    EXPECT_TRUE(a.learn);
    EXPECT_TRUE(a.buy);
    // A paladin / warlock class mount (60%) covers the tier-1 mount: learn only.
    Ride const cls = NextRide(20, 0, 1, 2);
    EXPECT_EQ(cls.tier, 1U);
    EXPECT_TRUE(cls.learn);
    EXPECT_FALSE(cls.buy);
    // Apprentice known, no mount: the mount first (even at L45, before Journeyman).
    Ride const buyOnly = NextRide(45, 1, 0, 2);
    EXPECT_EQ(buyOnly.tier, 1U);
    EXPECT_FALSE(buyOnly.learn);
    EXPECT_TRUE(buyOnly.buy);
    EXPECT_EQ(NextRide(39, 1, 1, 2).tier, 0U);  // Journeyman waits for 40
    Ride const j = NextRide(40, 1, 1, 2);
    EXPECT_EQ(j.tier, 2U);
    EXPECT_TRUE(j.learn);
    EXPECT_TRUE(j.buy);
    EXPECT_EQ(NextRide(60, 2, 2, 2).tier, 0U);  // Expert past MaxTier 2
    EXPECT_EQ(NextRide(60, 2, 2, 3).tier, 3U);
    EXPECT_EQ(NextRide(80, 3, 3, 3).tier, 0U);  // nothing past Expert
    EXPECT_EQ(NextRide(20, 0, 0, 0).tier, 0U);  // MaxTier 0: off
}

TEST(Mounts, MountTierFromKnownSpeeds)
{
    EXPECT_EQ(MountTierOf(0, false), 0U);
    EXPECT_EQ(MountTierOf(59, false), 1U);
    EXPECT_EQ(MountTierOf(99, false), 2U);
    EXPECT_EQ(MountTierOf(59, true), 3U);
}

TEST(Mounts, AffordOwnGoldThenOneGrantUpToCost)
{
    std::uint64_t const cost = 50000;    // Apprentice 4g + a 1g mount
    std::uint64_t const reserve = 2000;  // L20 class-trainer reserve
    Afford const own = MountAfford(52000, cost, reserve, true, false);
    EXPECT_TRUE(own.go);
    EXPECT_EQ(own.grantNeed, 0U);
    // Short with a treasury: ask for the shortfall incl. the reserve (need = money + shortfall).
    Afford const ask = MountAfford(30000, cost, reserve, true, false);
    EXPECT_FALSE(ask.go);
    EXPECT_EQ(ask.grantNeed, 30000U + 22000U);
    // Broke: the grant is capped at the cost (the reserve stays unfunded).
    Afford const broke = MountAfford(0, cost, reserve, true, false);
    EXPECT_EQ(broke.grantNeed, cost);
    // No treasury: wait.
    Afford const none = MountAfford(30000, cost, reserve, false, false);
    EXPECT_FALSE(none.go);
    EXPECT_EQ(none.grantNeed, 0U);
    // Asked this window: go once the money covers the cost itself, else wait (no second ask).
    EXPECT_TRUE(MountAfford(50000, cost, reserve, true, true).go);
    Afford const pending = MountAfford(30000, cost, reserve, true, true);
    EXPECT_FALSE(pending.go);
    EXPECT_EQ(pending.grantNeed, 0U);
}

TEST(Mounts, CheapestMountOfRankForRace)
{
    std::uint32_t const human = 1u << 0, orc = 1u << 1;
    std::vector<MountOffer> const offers = {{5656, 10000, 75, 1101},   {2414, 10000, 75, 1101},
                                            {18776, 100000, 150, 1101}, {1132, 10000, 75, 690},
                                            {33976, 100000, 75, 0xFFFFFFFFu}};
    MountOffer const* a = CheapestMount(offers, 75, human);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->item, 2414U);  // price tie: lower item
    EXPECT_EQ(CheapestMount(offers, 150, human)->item, 18776U);
    EXPECT_EQ(CheapestMount(offers, 75, orc)->item, 1132U);
    EXPECT_EQ(CheapestMount(offers, 150, orc), nullptr);
    EXPECT_EQ(CheapestMount(offers, 225, human), nullptr);
}

TEST(Mounts, SiteForOwnRaceAndExpertPerTeam)
{
    EXPECT_EQ(kSites[SiteFor(1, 1)].trainer, 4732U);   // Human -> Randal Hunter
    EXPECT_EQ(kSites[SiteFor(1, 2)].vendor, 384U);     // Katie Hunter
    EXPECT_EQ(kSites[SiteFor(2, 1)].trainer, 4752U);   // Orc -> Kildar
    EXPECT_EQ(kSites[SiteFor(10, 2)].vendor, 16264U);  // Blood Elf -> Winaestra
    EXPECT_EQ(kSites[SiteFor(11, 1)].trainer, 20914U);  // Draenei -> Aalun
    EXPECT_EQ(kSites[SiteFor(4, 3)].trainer, 35100U);  // Night Elf expert: Honor Hold
    EXPECT_EQ(kSites[SiteFor(8, 3)].trainer, 35093U);  // Troll expert: Thrallmar
    EXPECT_EQ(SiteFor(6, 4), kMountSites);             // no tier 4
    EXPECT_EQ(SiteFor(0, 1), kMountSites);
    // Every playable race has a tier-1 site.
    for (std::uint32_t race : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u})
        EXPECT_LT(SiteFor(race, 1), kMountSites) << race;
}

TEST(Mounts, BuildMountSitesPairsTrainerVendorAndNearestFlightMaster)
{
    std::vector<Npc> npcs;
    auto npc = [&](std::uint32_t spawn, std::uint32_t entry, std::uint32_t map, std::int32_t x, std::uint32_t roles,
                   std::uint8_t teams)
    {
        Npc n;
        n.spawn = spawn;
        n.entry = entry;
        n.map = map;
        n.x = x;
        n.roles = roles;
        n.teams = teams;
        npcs.push_back(n);
    };
    npc(900, 4732, 0, 0, RoleTradeTrainer, kAlliance);     // Randal Hunter
    npc(800, 4732, 0, 5000, RoleTradeTrainer, kAlliance);  // a second spawn: the lower guid wins
    npc(901, 384, 0, 10, RoleVendor, kAlliance);           // Katie Hunter near 900
    npc(902, 384, 0, 5010, RoleVendor, kAlliance);         // ... and near 800
    npc(50, 1, 0, 5600, RoleFlight, kAlliance);            // nearest alliance flight master to 800
    npc(40, 2, 0, 5100, RoleFlight, kHorde);               // nearer, but horde
    npc(30, 3, 1, 5000, RoleFlight, kAlliance);            // other map
    std::vector<Town> const sites = BuildMountSites(npcs);
    ASSERT_EQ(sites.size(), kMountSites);
    Town const& h = sites[SiteFor(1, 1)];
    EXPECT_EQ(h.id, 800U);
    EXPECT_EQ(h.x, 5000);
    EXPECT_EQ(h.teams, kAlliance);
    ASSERT_EQ(h.npcs.size(), 3U);
    EXPECT_EQ(h.npcs[0].spawn, 50U);  // spawn ascending
    EXPECT_EQ(h.npcs[1].spawn, 800U);
    EXPECT_EQ(h.npcs[2].spawn, 902U);
    EXPECT_EQ(sites[SiteFor(2, 1)].id, 0U);  // Orc site not spawned
    // The stops: rank first, then the mount.
    Plan const both = PlanRide(h, true, true, 384);
    ASSERT_EQ(both.count, 2U);
    EXPECT_EQ(both.stops[0].spawn, 800U);
    EXPECT_EQ(both.stops[0].ops, std::uint32_t(OpRide));
    EXPECT_EQ(both.stops[1].spawn, 902U);
    EXPECT_EQ(both.stops[1].ops, std::uint32_t(OpMount));
    Plan const buy = PlanRide(h, false, true, 384);
    ASSERT_EQ(buy.count, 1U);
    EXPECT_EQ(buy.stops[0].ops, std::uint32_t(OpMount));
}

TEST(Mounts, StateAndWireBits)
{
    Params p;
    EXPECT_FALSE(p.mounts);  // default off
    BotState s;
    s.nextMountMs = 7000;
    s.mountGrantMs = 5000;
    s.rideTier = 2;
    s.rideLearn = true;
    s.mountItem = 18776;
    BotState const next = AfterRun(p, s, 1000);
    EXPECT_EQ(next.nextMountMs, 7000U);  // survives runs
    EXPECT_EQ(next.mountGrantMs, 0U);    // run-scoped
    EXPECT_EQ(next.rideTier, 0U);
    EXPECT_EQ(next.mountItem, 0U);
    EXPECT_EQ(kStateVersion, 14U);
    EXPECT_EQ(std::uint32_t(NeedRiding), 16384U);
    EXPECT_EQ(std::uint32_t(DoneRiding), 4096U);
    EXPECT_EQ(std::uint32_t(DoneMount), 8192U);
    EXPECT_EQ(MountLedgerFields(81389, 1, "spell", 33388, 40000, true),
              ",\"site\":81389,\"tier\":1,\"spell\":33388,\"copper\":40000,\"learned\":true");
}

TEST(ErrandsMaterialStintAdmission, AllowsOnlyAbsentOrCurrentIdleUnownedState)
{
    BotState state;
    state.version = kStateVersion - 1;
    state.phase = Phase::Return;
    state.predecessor.active = true;
    EXPECT_FALSE(ShouldDeferMaterialStintAdmission(false, state));

    state = {};
    EXPECT_FALSE(ShouldDeferMaterialStintAdmission(true, state));

    state.predecessor.source = ErrandsPredecessorSource::ServiceStop;
    EXPECT_FALSE(ShouldDeferMaterialStintAdmission(true, state));
}

TEST(ErrandsMaterialStintAdmission, DefersEveryActivePhaseAndInvalidState)
{
    BotState state;
    for (Phase const phase : {Phase::Travel, Phase::Errands, Phase::Return})
    {
        state.phase = phase;
        EXPECT_TRUE(ShouldDeferMaterialStintAdmission(true, state));
    }

    state.phase = Phase::None;
    state.version = kStateVersion - 1;
    EXPECT_TRUE(ShouldDeferMaterialStintAdmission(true, state));
}

TEST(ErrandsMaterialStintAdmission, DefersAnyActivePredecessorSource)
{
    BotState state;
    state.predecessor.active = true;
    for (ErrandsPredecessorSource const source :
         {ErrandsPredecessorSource::TravelFlight, ErrandsPredecessorSource::ReturnFlight,
          ErrandsPredecessorSource::ServiceStop, ErrandsPredecessorSource::None,
          static_cast<ErrandsPredecessorSource>(0xff)})
    {
        state.predecessor.source = source;
        EXPECT_TRUE(ShouldDeferMaterialStintAdmission(true, state));
    }
}

TEST(ErrandsDiagnostics, ClassifiesAbsentCurrentAndInvalidStoredState)
{
    EXPECT_EQ(ClassifyDiagnosticState(false, 0, kStateVersion), DiagnosticState::Absent);
    EXPECT_EQ(ClassifyDiagnosticState(true, kStateVersion, kStateVersion), DiagnosticState::Current);
    EXPECT_EQ(ClassifyDiagnosticState(true, kStateVersion - 1, kStateVersion), DiagnosticState::InvalidVersion);
    EXPECT_STREQ(DiagnosticStateName(DiagnosticState::Absent), "absent");
    EXPECT_STREQ(DiagnosticStateName(DiagnosticState::Current), "current");
    EXPECT_STREQ(DiagnosticStateName(DiagnosticState::InvalidVersion), "invalid_version");
}

TEST(ErrandsDiagnostics, MissingGetterDoesNotInsertOrNormalizeState)
{
    constexpr std::uint32_t guid = 0xFFFFFFFEu;
    BotState first;
    BotState second;
    EXPECT_FALSE(ReadStateForDiagnostics(guid, first));
    EXPECT_FALSE(ReadStateForDiagnostics(guid, second));
    EXPECT_EQ(first.version, kStateVersion);
    EXPECT_EQ(second.version, kStateVersion);
}

// ---- AutoWow.Survival.PotionFloor ------------------------------------------------------------------

Params PotionsOn()
{
    Params p;
    p.potionFloor = true;
    return p;
}

TEST(PotionFloor, DefaultsOffAndWireBits)
{
    Params const p;
    EXPECT_FALSE(p.potionFloor);
    EXPECT_EQ(p.potionMinLevel, 60U);
    EXPECT_EQ(p.potionLow, 2U);
    EXPECT_EQ(p.potionTarget, 5U);
    EXPECT_EQ(p.potionRunMs, 1800000U);
    EXPECT_EQ(p.potionAhMaxUnitCopper, 40000U);
    EXPECT_EQ(std::uint32_t(NeedPotion), 32768U);
    EXPECT_EQ(std::uint32_t(DonePotions), 16384U);
    EXPECT_EQ(std::uint32_t(OpPotion), 4096U);
    // flag off: no family is ever short, whatever the stock
    EXPECT_EQ(PotionShort(p, kClassPriest, 75, {0, 0}, 5), 0U);
}

TEST(PotionFloor, TableAscendsPerFamilyAndMatchesTheWorldDb)
{
    for (std::size_t k = 1; k < kPotions.size(); ++k)
    {
        if (kPotions[k].family == kPotions[k - 1].family)
        {
            EXPECT_LT(kPotions[k - 1].minLevel, kPotions[k].minLevel);
        }
    }
    EXPECT_TRUE(IsPotionItem(22829));   // Super Healing Potion, RequiredLevel 55
    EXPECT_TRUE(IsPotionItem(33448));   // Runic Mana Potion, 70
    EXPECT_FALSE(IsPotionItem(1710));   // Greater Healing Potion (21): too weak for the floor
    EXPECT_FALSE(IsPotionItem(4540));
    EXPECT_EQ(PotionFamiliesFor(kClassWarrior), 1u << kPotionHeal);
    EXPECT_EQ(PotionFamiliesFor(kClassRogue), 1u << kPotionHeal);
    EXPECT_EQ(PotionFamiliesFor(kClassDeathKnight), 1u << kPotionHeal);
    EXPECT_EQ(PotionFamiliesFor(kClassPriest), (1u << kPotionHeal) | (1u << kPotionMana));
    EXPECT_EQ(PotionFamiliesFor(kClassDruid), (1u << kPotionHeal) | (1u << kPotionMana));
}

TEST(PotionFloor, PotionsForIsBestTierFirstWithinLevelAndStock)
{
    EXPECT_EQ(PotionsFor(kPotionHeal, 72, nullptr), (std::vector<std::uint32_t>{33447, 39671, 22829, 13446}));
    EXPECT_EQ(PotionsFor(kPotionHeal, 65, nullptr), (std::vector<std::uint32_t>{39671, 22829, 13446}));
    EXPECT_EQ(PotionsFor(kPotionMana, 48, nullptr), (std::vector<std::uint32_t>{}));
    EXPECT_EQ(PotionsFor(kPotionMana, 60, nullptr), (std::vector<std::uint32_t>{22832, 13444}));
    // a Dalaran healer stock (sorted): Major / Super / Runic Healing
    std::vector<std::uint32_t> const dalaran = {13446, 22829, 33447};
    EXPECT_EQ(PotionsFor(kPotionHeal, 68, &dalaran), (std::vector<std::uint32_t>{22829, 13446}));
    EXPECT_EQ(PotionsFor(kPotionMana, 80, &dalaran), (std::vector<std::uint32_t>{}));
}

TEST(PotionFloor, ShortNeedsLevelFamilyAndStock)
{
    Params const p = PotionsOn();
    EXPECT_EQ(PotionShort(p, kClassWarrior, 59, {0, 0}, 2), 0U);  // under MinLevel
    EXPECT_EQ(PotionShort(p, kClassWarrior, 60, {0, 0}, 2), 1u << kPotionHeal);  // a warrior drinks no mana
    EXPECT_EQ(PotionShort(p, kClassWarrior, 60, {2, 0}, 2), 0U);
    EXPECT_EQ(PotionShort(p, kClassMage, 70, {1, 3}, 2), 1u << kPotionHeal);
    EXPECT_EQ(PotionShort(p, kClassMage, 70, {1, 1}, 2), (1u << kPotionHeal) | (1u << kPotionMana));
    Params low = p;
    low.potionMinLevel = 40;
    EXPECT_EQ(PotionShort(low, kClassMage, 46, {0, 0}, 2), 1u << kPotionHeal);  // no mana tier usable at 46
    EXPECT_FALSE(PotionRunDue(0, 10, 0));
    EXPECT_TRUE(PotionRunDue(1, 10, 10));
    EXPECT_FALSE(PotionRunDue(1, 9, 10));
}

TEST(PotionFloor, AssessSoftThenUrgentOncePerWindow)
{
    Params const p = PotionsOn();
    Obs o = Healthy(kClassWarrior, 70);
    EXPECT_EQ(Assess(p, o).needs & NeedPotion, 0U);
    o.potionShort = 1u << kPotionHeal;
    Assessment a = Assess(p, o);
    EXPECT_EQ(a.needs & NeedPotion, std::uint32_t(NeedPotion));
    EXPECT_EQ(a.urgent & NeedPotion, 0U);
    EXPECT_FALSE(ShouldRun(a.needs, a.urgent));  // one soft need alone waits
    o.potionRunDue = true;
    a = Assess(p, o);
    EXPECT_EQ(a.urgent, std::uint32_t(NeedPotion));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
    TownFacts f;
    EXPECT_EQ(Serves(f) & NeedPotion, 0U);
    f.potions = true;
    EXPECT_EQ(Serves(f) & NeedPotion, std::uint32_t(NeedPotion));
    BotState s;
    s.nextPotionMs = 900000;
    EXPECT_EQ(AfterRun(p, s, 1000).nextPotionMs, 900000U);  // survives runs
}

TEST(PotionFloor, PlanStopsAddsEveryPotionVendorLastWithoutDisplacing)
{
    Town t;
    t.id = 5;
    Npc inn = MakeNpc(5, RoleInn | RoleVendor, 0);
    inn.items = {33444};
    Npc smith = MakeNpc(7, RoleRepair | RoleVendor, 20);
    Npc healer = MakeNpc(8, RoleVendor, 30);
    healer.potions = {13446, 22829, 33447};
    Npc magic = MakeNpc(9, RoleVendor, 40);
    magic.potions = {22832, 33448};
    Npc hordeAlch = MakeNpc(10, RoleVendor, 50, kHorde);
    hordeAlch.potions = {22829};
    t.npcs = {inn, smith, healer, magic, hordeAlch};
    PlanInput in;
    in.team = kAlliance;
    in.buyItems[KindWater] = 33444;
    Plan plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 2U);  // no potions planned: unchanged
    in.potions = {33447, 22829};
    plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 3U);
    EXPECT_EQ(plan.stops[2].spawn, 8U);
    EXPECT_EQ(plan.stops[2].ops, std::uint32_t(OpPotion));
    in.potions = {22829, 33448};
    plan = PlanStops(t, in);
    ASSERT_EQ(plan.count, 4U);
    EXPECT_EQ(plan.stops[2].spawn, 8U);
    EXPECT_EQ(plan.stops[3].spawn, 9U);  // the horde vendor never joins
    // the smith selling potions too: merged into its first stop
    t.npcs[1].potions = {22829};
    in.potions = {22829};
    plan = PlanStops(t, in);
    EXPECT_EQ(plan.stops[0].spawn, 7U);
    EXPECT_EQ(plan.stops[0].ops, std::uint32_t(OpSell | OpRepair | OpPotion));
}

TEST(PotionFloor, AuctionLotsBestTierCheapestWithinCapAndBudget)
{
    Params const p = PotionsOn();  // target 5, unit cap 40000
    std::vector<PotionLot> const lots = {
        {40, 22829, 5, 75000},    // Super Healing 15000 / unit
        {41, 33447, 2, 100000},   // Runic Healing 50000 / unit: over the cap
        {42, 39671, 3, 90000},    // Resurgent Healing 30000 / unit (req 65)
        {43, 39671, 3, 60000},    // Resurgent Healing 20000 / unit
        {44, 22832, 5, 50000},    // Super Mana: a warrior wants none
        {45, 1710, 5, 500},       // not a floor potion
        {46, 13446, 0, 500},      // empty lot
    };
    std::vector<PotionLot> got = PlanPotionLots(p, kClassWarrior, 72, {0, 0}, lots, 1000000);
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].id, 43U);  // best tier, cheaper unit
    EXPECT_EQ(got[1].id, 42U);  // still short (3 < 5): next resurgent lot
    // the budget bounds it: only the cheaper resurgent lot fits
    got = PlanPotionLots(p, kClassWarrior, 72, {0, 0}, lots, 70000);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].id, 43U);
    // at level 60 resurgent is out of reach: Super Healing
    got = PlanPotionLots(p, kClassWarrior, 60, {0, 0}, lots, 1000000);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].id, 40U);
    // stocked already: nothing; a priest short of mana only buys mana
    EXPECT_TRUE(PlanPotionLots(p, kClassWarrior, 72, {5, 0}, lots, 1000000).empty());
    got = PlanPotionLots(p, kClassPriest, 72, {5, 0}, lots, 1000000);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].id, 44U);
    // a lot that would carry the family past twice Target is skipped
    std::vector<PotionLot> const big = {{50, 22829, 20, 100000}};
    EXPECT_TRUE(PlanPotionLots(p, kClassWarrior, 72, {0, 0}, big, 1000000).empty());
    // flag off: nothing
    EXPECT_TRUE(PlanPotionLots(Params{}, kClassWarrior, 72, {0, 0}, lots, 1000000).empty());
}

}  // namespace
