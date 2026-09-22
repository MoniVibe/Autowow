#include "../src/AutoWow/AutoWowProfessionEconomyTelemetry.h"

#include <string>

#include <gtest/gtest.h>

using namespace AutoWowProfessionEconomyTelemetry;

namespace
{
RecipeClassificationInput BaseRecipeInput()
{
    RecipeClassificationInput input;
    input.hasProfession = true;
    input.currentSkill = 150;
    input.requiredSkill = 125;
    input.outputItemId = 90001;
    input.outputItemValid = true;
    input.deterministicOutput = true;
    input.outputCapacity = true;
    input.reagentCount = 2;
    input.reagents[0] = {70001, 2, 10, 5};
    input.reagents[1] = {70002, 3, 7, 2};
    return input;
}
}

TEST(AutoWowProfessionEconomyTelemetry, ProfessionDefinitionsAreStableAndClassified)
{
    EXPECT_EQ(ProfessionName(171), "alchemy");
    EXPECT_EQ(ProfessionName(773), "inscription");
    EXPECT_TRUE(IsCraftingSkill(755));
    EXPECT_FALSE(IsCraftingSkill(186));
    EXPECT_FALSE(IsProfessionSkill(999999));
}

TEST(AutoWowProfessionEconomyTelemetry, ProfessionPlanRequiresObservedSkills)
{
    std::array<ProfessionFact, kMaxProfessions> observed{};
    observed[0] = {197, "tailoring", 25, 75, 25, 75};

    ProfessionPlanAssessment const missing =
        AssessProfessionPlan("Herbalism", "Tailoring", observed, 1);

    EXPECT_EQ(missing.status, ProfessionPlanStatus::MissingProfession);
    ASSERT_EQ(missing.desiredCount, 2U);
    EXPECT_FALSE(missing.observed[0]);
    EXPECT_TRUE(missing.observed[1]);
    ASSERT_EQ(missing.missingCount, 1U);
    EXPECT_EQ(missing.missing[0], "herbalism");
    EXPECT_EQ(ProfessionPlanStatusName(missing.status), "missing_profession");
}

TEST(AutoWowProfessionEconomyTelemetry, ProfessionPlanIsCaseInsensitiveAndRealizedOnlyByPresence)
{
    std::array<ProfessionFact, kMaxProfessions> observed{};
    observed[0] = {182, "Herbalism", 0, 0, 0, 0};
    observed[1] = {197, " tailoring ", 1, 75, 1, 75};

    ProfessionPlanAssessment const realized =
        AssessProfessionPlan("HERBALISM", "Tailoring", observed, 2);

    EXPECT_EQ(realized.status, ProfessionPlanStatus::Realized);
    EXPECT_EQ(realized.missingCount, 0U);
    EXPECT_TRUE(realized.observed[0]);
    EXPECT_TRUE(realized.observed[1]);
}

TEST(AutoWowProfessionEconomyTelemetry, ProfessionPlanRejectsMalformedDeclarations)
{
    std::array<ProfessionFact, kMaxProfessions> observed{};

    EXPECT_EQ(AssessProfessionPlan("Herbalism", "", observed, 0).status,
              ProfessionPlanStatus::Invalid);
    EXPECT_EQ(AssessProfessionPlan("Herbalism", "Herbalism", observed, 0).status,
              ProfessionPlanStatus::Invalid);
    EXPECT_EQ(AssessProfessionPlan("Unknown", "Tailoring", observed, 0).status,
              ProfessionPlanStatus::Invalid);
    EXPECT_EQ(AssessProfessionPlan("", "", observed, 0).status,
              ProfessionPlanStatus::Unspecified);
}

TEST(AutoWowProfessionEconomyTelemetry, ClassifiesLimitingReagentAndCraftCount)
{
    RecipeClassification const result = ClassifyRecipe(BaseRecipeInput());

    EXPECT_TRUE(result.immediatelyCraftable);
    EXPECT_EQ(result.reason, RecipeBlockReason::None);
    EXPECT_TRUE(result.reagentLimited);
    EXPECT_EQ(result.maxCraftsByReagents, 2U);
    EXPECT_EQ(result.limitingReagentItemId, 70002U);
    EXPECT_EQ(result.limitingReagentBagCount, 7U);
}

TEST(AutoWowProfessionEconomyTelemetry, MissingReagentBlocksImmediately)
{
    RecipeClassificationInput input = BaseRecipeInput();
    input.reagents[1].bagCount = 2;

    RecipeClassification const result = ClassifyRecipe(input);

    EXPECT_FALSE(result.immediatelyCraftable);
    EXPECT_EQ(result.reason, RecipeBlockReason::MissingReagent);
    EXPECT_EQ(result.maxCraftsByReagents, 0U);
    EXPECT_EQ(result.limitingReagentItemId, 70002U);
}

TEST(AutoWowProfessionEconomyTelemetry, SkillAndOutputGatesAreExplicit)
{
    RecipeClassificationInput input = BaseRecipeInput();
    input.currentSkill = 100;
    EXPECT_EQ(ClassifyRecipe(input).reason, RecipeBlockReason::SkillInsufficient);

    input.currentSkill = 150;
    input.hasProfession = false;
    EXPECT_EQ(ClassifyRecipe(input).reason, RecipeBlockReason::MissingProfession);

    input.hasProfession = true;
    input.outputCapacity = false;
    EXPECT_EQ(ClassifyRecipe(input).reason, RecipeBlockReason::OutputInventoryFull);

    input.outputCapacity = true;
    input.deterministicOutput = false;
    EXPECT_EQ(ClassifyRecipe(input).reason, RecipeBlockReason::RandomOutputUnsupported);
}

TEST(AutoWowProfessionEconomyTelemetry, NoReagentRecipeCanBeImmediatelyCraftable)
{
    RecipeClassificationInput input = BaseRecipeInput();
    input.reagentCount = 0;

    RecipeClassification const result = ClassifyRecipe(input);

    EXPECT_TRUE(result.immediatelyCraftable);
    EXPECT_FALSE(result.reagentLimited);
    EXPECT_EQ(result.maxCraftsByReagents, 0U);
    EXPECT_EQ(result.reason, RecipeBlockReason::None);
}

TEST(AutoWowProfessionEconomyTelemetry, KeepsKnownButInactiveRanksNonCraftable)
{
    RecipeClassificationInput input = BaseRecipeInput();
    input.usable = false;

    RecipeClassification const result = ClassifyRecipe(input);

    EXPECT_FALSE(result.immediatelyCraftable);
    EXPECT_EQ(result.reason, RecipeBlockReason::InactiveRecipeRank);
}

TEST(AutoWowProfessionEconomyTelemetry, RejectsUnboundedReagentShape)
{
    RecipeClassificationInput input = BaseRecipeInput();
    input.reagentCount = kMaxReagents + 1;

    RecipeClassification const result = ClassifyRecipe(input);

    EXPECT_FALSE(result.immediatelyCraftable);
    EXPECT_EQ(result.reason, RecipeBlockReason::MalformedReagent);
}

TEST(AutoWowProfessionEconomyTelemetry, JsonSchemaIsStableReadOnlyAndEscaped)
{
    Snapshot snapshot;
    snapshot.botGuid = 42;
    snapshot.enrolled = true;
    snapshot.online = true;
    snapshot.playerbot = true;
    snapshot.moneyCopper = 12345;
    snapshot.professions[0] = {171, "alchemy", 150, 225, 150, 225};
    snapshot.professionCount = 1;
    snapshot.recipes[0].spellId = 20001;
    snapshot.recipes[0].name = "A \"test\" recipe";
    snapshot.recipes[0].skillId = 171;
    snapshot.recipes[0].profession = "alchemy";
    snapshot.recipes[0].requiredSkill = 125;
    snapshot.recipes[0].skillCurrent = 150;
    snapshot.recipes[0].skillMaximum = 225;
    snapshot.recipes[0].outputItemId = 90001;
    snapshot.recipes[0].outputQuantity = 2;
    snapshot.recipes[0].known = true;
    snapshot.recipes[0].outputCapacity = true;
    snapshot.recipes[0].immediatelyCraftable = true;
    snapshot.recipes[0].blockReason = RecipeBlockReason::None;
    snapshot.recipes[0].reagents[0] = {70001, 2, 10, 5};
    snapshot.recipes[0].reagentCount = 1;
    snapshot.recipes[0].maxCraftsByReagents = 5;
    snapshot.recipes[0].reagentLimited = true;
    snapshot.recipes[0].limitingReagentItemId = 70001;
    snapshot.recipes[0].limitingReagentBagCount = 10;
    snapshot.recipeCount = 1;
    snapshot.knownRecipeCount = 1;
    snapshot.materials[0] = {70001, 10};
    snapshot.materialCount = 1;
    snapshot.bagMaterialTypeCount = 1;

    std::string const json = BuildJson(snapshot);

    EXPECT_NE(json.find("\"schema\":\"autowow.profession.economy.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"read_only\":true"), std::string::npos);
    EXPECT_NE(json.find("\"money_copper\":12345"), std::string::npos);
    EXPECT_NE(json.find("\"known_count\":1"), std::string::npos);
    EXPECT_NE(json.find("\"immediately_craftable\":true"), std::string::npos);
    EXPECT_NE(json.find("\"max_crafts_by_reagents\":5"), std::string::npos);
    EXPECT_NE(json.find("A \\\"test\\\" recipe"), std::string::npos);
    EXPECT_NE(json.find("\"craft_evidence\":{\"available\":false"), std::string::npos);
    EXPECT_EQ(json.find("\"command\""), std::string::npos);
}

TEST(AutoWowProfessionEconomyTelemetry, AbsentCraftEvidenceIsNotReportedAsZeroSuccess)
{
    Snapshot snapshot;
    snapshot.craftEvidence.available = false;
    snapshot.craftEvidence.source = "none";

    std::string const json = BuildJson(snapshot);

    EXPECT_NE(json.find("\"available\":false"), std::string::npos);
    EXPECT_NE(json.find("\"source\":\"none\""), std::string::npos);
}
