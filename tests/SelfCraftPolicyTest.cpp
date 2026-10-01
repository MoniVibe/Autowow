/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfCraftPolicy.h"

#include "AutoWowQuestLedger.h"
#include "SupplyPolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowSelfCraft;

using BandageHave = std::array<Have, kBandages.size()>;
using FoodHave = std::array<Have, kFoods.size()>;

TEST(SelfCraftPolicy, TablesAscendAndStayCraftable)
{
    for (std::size_t i = 0; i < kBandages.size(); ++i)
    {
        EXPECT_TRUE(kBandages[i].spell && kBandages[i].item && kBandages[i].reagent && kBandages[i].count);
        EXPECT_LE(kBandages[i].useSkill, kBandages[i].minSkill);  // a crafter can use what it makes
        EXPECT_LT(kBandages[i].minSkill, kBandages[i].greyAt);
        if (i)
            EXPECT_GT(kBandages[i].minSkill, kBandages[i - 1].minSkill);
    }
    for (std::size_t i = 0; i < kFoods.size(); ++i)
    {
        EXPECT_TRUE(kFoods[i].spell && kFoods[i].item && kFoods[i].reagent && kFoods[i].count);
        EXPECT_LT(kFoods[i].minSkill, kFoods[i].greyAt);
        if (i)
            EXPECT_GE(kFoods[i].minSkill, kFoods[i - 1].minSkill);
    }
    EXPECT_EQ(kBandages[0].spell, 3275u);  // Linen Bandage
    EXPECT_EQ(kBandages[0].item, 1251u);
    EXPECT_EQ(kBandages[5].item, 6451u);   // Heavy Silk Bandage
}

TEST(SelfCraftPolicy, BandagePicksBestKnownWithCloth)
{
    BandageHave h{};
    h[0] = {true, 3};  // linen bandage, 3 linen
    h[1] = {true, 3};  // heavy linen, needs 2
    EXPECT_EQ(PickBandage(h, 45, 0, 10), 1);
    EXPECT_EQ(PickBandage(h, 39, 0, 10), 0);   // heavy linen below its trainer rank
    h[1].reagents = 1;
    EXPECT_EQ(PickBandage(h, 45, 0, 10), 0);   // one linen: only a linen bandage
    h[2] = {true, 0};                          // wool known, no wool
    EXPECT_EQ(PickBandage(h, 90, 0, 10), 0);
    h[2].reagents = 4;
    EXPECT_EQ(PickBandage(h, 90, 0, 10), 2);
}

TEST(SelfCraftPolicy, BandageStopsAtTarget)
{
    BandageHave h{};
    h[0] = {true, 20};
    EXPECT_EQ(PickBandage(h, 10, 9, 10), 0);
    EXPECT_EQ(PickBandage(h, 10, 10, 10), -1);
    EXPECT_EQ(PickBandage(h, 10, 0, 0), -1);
    BandageHave none{};
    EXPECT_EQ(PickBandage(none, 300, 0, 10), -1);
}

TEST(SelfCraftPolicy, ClothReserveFollowsTheBestKnownBandage)
{
    BandageHave h{};
    EXPECT_EQ(ReserveCloth(h, 1), 0u);        // no recipe: nothing held back
    h[0] = {true, 0};
    EXPECT_EQ(ReserveCloth(h, 1), 2589u);     // linen, even with none in the bags
    h[1] = {true, 0};
    h[2] = {true, 0};
    EXPECT_EQ(ReserveCloth(h, 85), 2592u);    // wool once Wool Bandage is known: linen goes on to routing
    EXPECT_EQ(ReserveCloth(h, 60), 2589u);    // wool recipe known below its rank: still linen

    // The reserve is whole stacks in ascending guid until BandageCloth units (AutoWowSupply::PickStacks).
    std::vector<AutoWowSupply::Stack> stacks = {{30, 20}, {10, 5}, {20, 20}};
    std::vector<std::uint32_t> const kept = AutoWowSupply::PickStacks(stacks, 20);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0], 10u);
    EXPECT_EQ(kept[1], 20u);
    EXPECT_TRUE(AutoWowSupply::PickStacks(stacks, 0).empty());
}

TEST(SelfCraftPolicy, UsePicksBestUsableBandage)
{
    std::array<std::uint32_t, kBandages.size()> held{};
    EXPECT_EQ(PickUse(held, 300), -1);
    held[0] = 2;
    held[3] = 1;  // heavy wool needs First Aid 75
    EXPECT_EQ(PickUse(held, 74), 0);
    EXPECT_EQ(PickUse(held, 75), 3);
    EXPECT_EQ(PickUse(held, 0), -1);
}

TEST(SelfCraftPolicy, ShouldBandageOnlyHurtAndSafe)
{
    EXPECT_TRUE(ShouldBandage(40, 60, false, 0, false, true));
    EXPECT_FALSE(ShouldBandage(60, 60, false, 0, false, true));  // at the threshold: no
    EXPECT_FALSE(ShouldBandage(40, 60, true, 0, false, true));
    EXPECT_FALSE(ShouldBandage(40, 60, false, 1, false, true));
    EXPECT_FALSE(ShouldBandage(40, 60, false, 0, true, true));   // Recently Bandaged
    EXPECT_FALSE(ShouldBandage(40, 60, false, 0, false, false));
}

TEST(SelfCraftPolicy, MayCraftWhenFreeAndRested)
{
    Params p;
    EXPECT_TRUE(MayCraft(p, false, false, 70, 0, false));
    EXPECT_FALSE(MayCraft(p, false, false, 69, 0, false));
    EXPECT_FALSE(MayCraft(p, true, false, 100, 100, true));
    EXPECT_FALSE(MayCraft(p, false, true, 100, 100, true));
    EXPECT_TRUE(MayCraft(p, false, false, 100, 50, true));
    EXPECT_FALSE(MayCraft(p, false, false, 100, 49, true));
}

TEST(SelfCraftPolicy, CookedFoodCountsNearTheVendorTier)
{
    EXPECT_EQ(VendorFoodLevel(1), 1u);
    EXPECT_EQ(VendorFoodLevel(14), 5u);
    EXPECT_EQ(VendorFoodLevel(27), 25u);
    EXPECT_EQ(VendorFoodLevel(38), 35u);
    EXPECT_TRUE(CountsAsFood(1, 10));    // L10 buys the L5 tier: L1 food is within 10 levels
    EXPECT_TRUE(CountsAsFood(5, 20));
    EXPECT_FALSE(CountsAsFood(1, 20));   // L20 buys L15 food
    EXPECT_TRUE(CountsAsFood(15, 27));
    EXPECT_FALSE(CountsAsFood(5, 27));
    EXPECT_FALSE(CountsAsFood(35, 34));  // not usable yet
}

TEST(SelfCraftPolicy, CookFoodFirstThenSkillUps)
{
    FoodHave h{};
    h[0] = {true, 4};   // Charred Wolf Meat (L1 food, grey at 85)
    h[5] = {true, 4};   // Coyote Steak (L5 food, trainer 50, grey at 130)
    // L20 under the food target: Coyote Steak counts as food there.
    EXPECT_EQ(PickFood(h, 60, 20, 0, 20), 5);
    // L30: neither counts as food; the skill-up is the highest non-grey recipe.
    EXPECT_EQ(PickFood(h, 60, 30, 0, 20), 5);
    EXPECT_EQ(PickFood(h, 130, 30, 0, 20), -1);  // Coyote grey at 130, Charred at 85
    EXPECT_EQ(PickFood(h, 100, 30, 0, 20), 5);
    // Food stock full: skill-ups only.
    EXPECT_EQ(PickFood(h, 90, 8, 20, 20), 5);
    EXPECT_EQ(PickFood(h, 40, 8, 20, 20), 0);    // Coyote Steak above the skill
    // Spider Sausage takes two White Spider Meat.
    FoodHave s{};
    s[10] = {true, 1};
    EXPECT_EQ(PickFood(s, 210, 38, 0, 20), -1);
    s[10].reagents = 2;
    EXPECT_EQ(PickFood(s, 210, 38, 0, 20), 10);
    FoodHave none{};
    EXPECT_EQ(PickFood(none, 225, 30, 0, 20), -1);
}

TEST(SelfCraftPolicy, FoodPrefersTheHighestProductLevel)
{
    FoodHave h{};
    h[8] = {true, 1};   // Goblin Deviled Clams, L15 food
    h[9] = {true, 1};   // Barbecued Buzzard Wing, L25 food
    EXPECT_EQ(PickFood(h, 180, 30, 0, 20), 9);
    EXPECT_EQ(PickFood(h, 180, 24, 0, 20), 8);  // the wing is not usable at L24
}

TEST(SelfCraftPolicy, SmeltingPrefersMaterialDemandThenCheapSkillUp)
{
    SmeltOption material;
    material.spell = 3307;
    material.reagentValue = 100;
    material.known = material.reagentsReady = material.outputRoom = material.materialWanted = true;
    SmeltOption skill = material;
    skill.spell = 2657;
    skill.reagentValue = 1;
    skill.materialWanted = false;
    skill.skillUp = true;
    std::vector<SmeltOption> options = {skill, material};
    EXPECT_EQ(PickSmelt(options, false), 1);  // useful grey material outranks a cheaper non-grey skill-up

    material.materialWanted = false;
    options = {skill, material};
    EXPECT_EQ(PickSmelt(options, false), 0);
    SmeltOption tied = skill;
    tied.spell = 3304;
    options.push_back(tied);
    EXPECT_EQ(PickSmelt(options, false), 0);  // equal value: lower spell id
}

TEST(SelfCraftPolicy, SmeltingRejectsUnknownMissingFullAndProtected)
{
    SmeltOption ready;
    ready.spell = 2657;
    ready.known = ready.reagentsReady = ready.outputRoom = ready.materialWanted = true;
    std::vector<SmeltOption> options = {ready};
    EXPECT_EQ(PickSmelt(options, false), 0);
    options[0].known = false;
    EXPECT_EQ(PickSmelt(options, false), -1);
    options[0] = ready;
    options[0].reagentsReady = false;
    EXPECT_EQ(PickSmelt(options, false), -1);
    options[0] = ready;
    options[0].outputRoom = false;
    EXPECT_EQ(PickSmelt(options, false), -1);
    options[0] = ready;
    options[0].protectedReagent = true;
    EXPECT_EQ(PickSmelt(options, false), -1);
}

TEST(SelfCraftPolicy, SmeltingReceiptRequiresOwnerAndExactNativeDeltas)
{
    SmeltReceiptFacts facts;
    facts.expectedOwner = facts.actualOwner = SmeltOwner::SelfCraft;
    facts.outputBefore = 3;
    facts.outputAfter = 5;
    facts.outputExpected = 2;
    facts.reagentCount = 2;
    facts.reagents[0] = {8, 7, 1};
    facts.reagents[1] = {4, 3, 1};
    EXPECT_EQ(EvaluateSmeltReceipt(facts), SmeltReceipt::Success);  // skill may stay grey; inventory proves the cast
    facts.stillCasting = true;
    EXPECT_EQ(EvaluateSmeltReceipt(facts), SmeltReceipt::Pending);
    facts.stillCasting = false;
    facts.actualOwner = SmeltOwner::Supply;
    EXPECT_EQ(EvaluateSmeltReceipt(facts), SmeltReceipt::WrongOwner);
    facts.actualOwner = SmeltOwner::SelfCraft;
    facts.outputAfter = facts.outputBefore;
    facts.reagents[0].after = facts.reagents[0].before;
    facts.reagents[1].after = facts.reagents[1].before;
    EXPECT_EQ(EvaluateSmeltReceipt(facts), SmeltReceipt::Interrupted);
    facts.outputAfter = 4;
    EXPECT_EQ(EvaluateSmeltReceipt(facts), SmeltReceipt::Ambiguous);
}

TEST(SelfCraftPolicy, SmeltingBatchMovementAndFocusAreBounded)
{
    EXPECT_TRUE(SmeltJobOpen(true, 4, 5, 100, 180099));
    EXPECT_FALSE(SmeltJobOpen(true, 5, 5, 100, 101));
    EXPECT_FALSE(SmeltJobOpen(true, 4, 5, 100, 180100));
    EXPECT_FALSE(SmeltJobOpen(false, 0, 5, 0, 0));
    EXPECT_FALSE(SmeltMoveTimedOut(true, 100, 30099));
    EXPECT_TRUE(SmeltMoveTimedOut(true, 100, 30100));

    SmeltOption option;
    option.spell = 2657;
    option.known = option.reagentsReady = option.outputRoom = option.materialWanted = true;
    std::vector<SmeltOption> options = {option};
    EXPECT_EQ(PickSmelt(options, false), 0);  // enough to plan a forge visit
    EXPECT_EQ(PickSmelt(options, true), -1);  // final cast still requires native focus
    options[0].focusReady = true;
    EXPECT_EQ(PickSmelt(options, true), 0);
}

TEST(SelfCraftPolicy, SmeltingForgeLegRequiresExactMovementOwnership)
{
    SmeltMoveFacts facts;
    facts.forgeGuid = 0x1234;
    facts.lastMovementPresent = true;
    facts.movementIssuedAtMs = 77;
    facts.movementEndpoint = {1, 10.0f, 20.0f, 30.0f};
    facts.nativeSplineActive = true;
    facts.nativeSplineId = 9;
    facts.nativeSplineEndpoint = {1, 11.0f, 21.0f, 31.0f};
    facts.botMoving = true;
    SmeltMoveProof const proof = CaptureSmeltMoveProof(facts);
    ASSERT_EQ(proof.version, kSmeltMoveProofVersion);
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::OwnedLive);

    facts.nativeSplineActive = false;
    facts.botMoving = false;
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::OwnedArrived);

    // A newer LastMovement remains a replacement after that foreign move has itself finished. The smelter must
    // release instead of treating the idle bot as an arrived forge leg and casting over the newer goal.
    facts.movementIssuedAtMs = 78;
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::Replaced);

    facts.movementIssuedAtMs = 77;
    facts.nativeSplineActive = true;
    facts.nativeSplineId = 10;
    facts.botMoving = true;
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::Replaced);

    facts.nativeSplineId = 9;
    facts.moveFarActive = true;
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::Replaced);

    facts.moveFarActive = false;
    facts.travelIntent.createdMs = 90;  // a newer intent remains foreign even after it has become inactive
    EXPECT_FALSE(facts.travelIntent.active);
    EXPECT_EQ(EvaluateSmeltMove(proof, facts), SmeltMoveDecision::Replaced);
}

TEST(SelfCraftPolicy, SupplySmeltingNeverPreemptsOwnedCraftWork)
{
    EXPECT_TRUE(SupplySmeltMayRun(false, false, false));
    EXPECT_FALSE(SupplySmeltMayRun(true, false, false));
    EXPECT_FALSE(SupplySmeltMayRun(false, true, false));
    EXPECT_FALSE(SupplySmeltMayRun(false, false, true));
}

TEST(SelfCraftPolicy, LedgerEventAndFields)
{
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::SelfCraft), 21);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(AutoWowQuestLedger::Event::SelfCraft), "selfcraft");
    EXPECT_STREQ(ReasonName(Reason::Learn), "learn");
    EXPECT_STREQ(ReasonName(Reason::Craft), "craft");
    EXPECT_STREQ(ReasonName(Reason::Use), "use");
    EXPECT_STREQ(ReasonName(Reason::Skip), "skip");
    EXPECT_EQ(LedgerFields(129, 45, 3276, 2581, 1), ",\"line\":129,\"skill\":45,\"spell\":3276,\"item\":2581,\"count\":1");
}

TEST(SelfCraftPolicy, DefaultsAreOffAndSmall)
{
    Params p;
    EXPECT_FALSE(Enabled());
    EXPECT_FALSE(p.smelting);
    EXPECT_EQ(p.smeltingCheckIntervalMs, 60000u);
    EXPECT_EQ(p.smeltingBatchMax, 5u);
    EXPECT_EQ(p.bandageTarget, 10u);
    EXPECT_EQ(p.bandageCloth, 20u);
    BotState s;
    EXPECT_EQ(s.version, kStateVersion);
}
}  // namespace
