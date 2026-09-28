#include "../src/AutoWow/GatherScalePolicy.h"
#include "../src/AutoWow/ErrandsPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowGatherScale;

TEST(GatherScalePolicy, CanAttemptKeepsStockRuleWithFlagOff)
{
    EXPECT_TRUE(CanAttempt(false, kSkillMining, 0, 0));
    EXPECT_TRUE(CanAttempt(false, kSkillMining, 125, 125));
    EXPECT_FALSE(CanAttempt(false, kSkillMining, 124, 125));
    EXPECT_FALSE(CanAttempt(false, kSkillHerbalism, 8, 150));
}

TEST(GatherScalePolicy, CanAttemptAnySkillNeedsAKnownGatheringProfession)
{
    EXPECT_TRUE(CanAttempt(true, kSkillHerbalism, 8, 150));
    EXPECT_TRUE(CanAttempt(true, kSkillMining, 1, 300));
    EXPECT_TRUE(CanAttempt(true, kSkillSkinning, 3, 200));
    EXPECT_FALSE(CanAttempt(true, kSkillSkinning, 0, 200));  // not known
    EXPECT_FALSE(CanAttempt(true, 633, 50, 100));            // lockpicking keeps the stock rule
    EXPECT_FALSE(CanAttempt(true, 202, 50, 100));            // engineering corpses too
}

TEST(GatherScalePolicy, DeficitAndSkinningRequired)
{
    EXPECT_EQ(Deficit(125, 8), 117);
    EXPECT_EQ(Deficit(50, 75), -25);
    EXPECT_EQ(SkinningRequired(5), 0u);
    EXPECT_EQ(SkinningRequired(15), 50u);
    EXPECT_EQ(SkinningRequired(38), 190u);
}

TEST(GatherScalePolicy, YieldFallsWithDeficitNeverBelowOneUnit)
{
    EXPECT_EQ(YieldPct(-10), 100u);
    EXPECT_EQ(YieldPct(0), 100u);
    EXPECT_EQ(YieldPct(10), 80u);
    EXPECT_EQ(YieldPct(25), 50u);
    EXPECT_EQ(YieldPct(50), 0u);
    EXPECT_EQ(YieldPct(200), 0u);

    EXPECT_EQ(ScaledCount(4, 0), 4u);
    EXPECT_EQ(ScaledCount(4, -5), 4u);
    EXPECT_EQ(ScaledCount(4, 25), 2u);
    EXPECT_EQ(ScaledCount(3, 25), 1u);   // floor(1.5)
    EXPECT_EQ(ScaledCount(2, 10), 1u);   // floor(1.6)
    EXPECT_EQ(ScaledCount(1, 40), 1u);   // min one unit
    EXPECT_EQ(ScaledCount(5, 120), 1u);
    EXPECT_EQ(ScaledCount(0, 30), 0u);   // nothing stays nothing
}

TEST(GatherScalePolicy, ScrapsAboveFiftyToPreviousTier)
{
    EXPECT_FALSE(Scraps(50));
    EXPECT_TRUE(Scraps(51));
    EXPECT_EQ(ScrapItem(2772), 2771u);   // Iron Ore -> Tin Ore
    EXPECT_EQ(ScrapItem(2771), 2770u);   // Tin Ore -> Copper Ore
    EXPECT_EQ(ScrapItem(3858), 2772u);   // Mithril Ore -> Iron Ore
    EXPECT_EQ(ScrapItem(3356), 2450u);   // Kingsblood -> Briarthorn
    EXPECT_EQ(ScrapItem(785), 765u);     // Mageroyal -> Silverleaf
    EXPECT_EQ(ScrapItem(4234), 2319u);   // Heavy Leather -> Medium Leather
    EXPECT_EQ(ScrapItem(2318), 2934u);   // Light Leather -> Ruined Leather Scraps
    EXPECT_EQ(ScrapItem(2770), 0u);      // tier one keeps its item
    EXPECT_EQ(ScrapItem(765), 0u);
    EXPECT_EQ(ScrapItem(1210), 0u);      // a gem keeps its item
}

TEST(GatherScalePolicy, ScrapChainsEndAtATierOneMaterial)
{
    for (std::uint32_t item : {36910u, 13468u, 38425u, 12365u})
    {
        int steps = 0;
        while (std::uint32_t const next = ScrapItem(item))
        {
            item = next;
            ASSERT_LT(++steps, 10) << item;
        }
    }
}

TEST(GatherScalePolicy, GatherGainGrowsWithDeficitCappedAtFive)
{
    EXPECT_EQ(GatherGain(1, -30), 1u);
    EXPECT_EQ(GatherGain(1, 0), 1u);
    EXPECT_EQ(GatherGain(1, 1), 1u);
    EXPECT_EQ(GatherGain(1, 24), 1u);
    EXPECT_EQ(GatherGain(1, 25), 2u);
    EXPECT_EQ(GatherGain(1, 74), 3u);
    EXPECT_EQ(GatherGain(1, 100), 5u);
    EXPECT_EQ(GatherGain(1, 290), 5u);
    EXPECT_EQ(GatherGain(3, 10), 3u);  // never below the configured base
}

TEST(GatherScalePolicy, CraftGainOneMoreForOrange)
{
    EXPECT_EQ(CraftGain(1, 40, 45), 2u);  // orange
    EXPECT_EQ(CraftGain(1, 45, 45), 1u);  // yellow
    EXPECT_EQ(CraftGain(1, 70, 45), 1u);
}

TEST(GatherScalePolicy, CapGainNeverPassesTheRankMax)
{
    EXPECT_EQ(CapGain(5, 70, 75), 5u);
    EXPECT_EQ(CapGain(5, 72, 75), 3u);
    EXPECT_EQ(CapGain(5, 74, 75), 1u);
    EXPECT_EQ(CapGain(2, 75, 75), 2u);  // the core's own no-op at the cap
    EXPECT_EQ(CapGain(1, 10, 150), 1u);
}

TEST(GatherScalePolicy, TrainRunsNeedMissingOrCappedPlannedProfession)
{
    using namespace AutoWowErrands;
    EXPECT_TRUE(PlannedTrainNeeded(false, 0, 0, 38));   // planned, never learned
    EXPECT_FALSE(PlannedTrainNeeded(false, 0, 0, 4));   // below the apprentice level
    EXPECT_TRUE(PlannedTrainNeeded(true, 75, 75, 38));  // capped, journeyman level reached
    EXPECT_FALSE(PlannedTrainNeeded(true, 60, 75, 38)); // not capped: the soft rank rule covers it
    EXPECT_FALSE(PlannedTrainNeeded(true, 75, 75, 9));  // capped, journeyman needs level 10
    EXPECT_FALSE(PlannedTrainNeeded(true, 225, 225, 34));
    EXPECT_TRUE(PlannedTrainNeeded(true, 225, 225, 35));

    EXPECT_TRUE(TrainRunDue(true, 1000, 1000));
    EXPECT_FALSE(TrainRunDue(true, 999, 1000));
    EXPECT_FALSE(TrainRunDue(false, 5000, 0));

    Params p;
    Obs o;
    o.level = 38;
    EXPECT_EQ(Assess(p, o).urgent & NeedProfTrain, 0u);
    o.trainRunDue = true;
    Assessment const a = Assess(p, o);
    EXPECT_EQ(a.urgent, static_cast<std::uint32_t>(NeedProfTrain));
    EXPECT_TRUE(ShouldRun(a.needs, a.urgent));
}
}  // namespace
