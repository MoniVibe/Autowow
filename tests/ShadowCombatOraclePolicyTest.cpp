#include "../src/AutoWow/ShadowCombatOraclePolicy.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowShadowCombat;

void ExpectChoice(OracleChoice const& choice, Intent intent, Guid targetGuid, std::string_view actionName)
{
    ASSERT_TRUE(choice.hasChoice);
    EXPECT_EQ(choice.intent, intent);
    EXPECT_EQ(choice.targetGuid, targetGuid);
    EXPECT_EQ(choice.actionName, actionName);
}

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}

TEST(ShadowCombatOraclePolicyTest, GenericIntentNamesAreStable)
{
    EXPECT_EQ(IntentName(Intent::Survive), "survive");
    EXPECT_EQ(IntentName(Intent::AvoidHazard), "avoid_hazard");
    EXPECT_EQ(IntentName(Intent::Heal), "heal");
    EXPECT_EQ(IntentName(Intent::Interrupt), "interrupt");
    EXPECT_EQ(IntentName(Intent::ControlAdd), "control_add");
    EXPECT_EQ(IntentName(Intent::ProtectTank), "protect_tank");
    EXPECT_EQ(IntentName(Intent::Attack), "attack");
}

TEST(ShadowCombatOraclePolicyTest, ChoiceIsInvariantUnderEveryInputPermutation)
{
    DecisionFrame frame;
    frame.decisionKey = 55;
    frame.self = {1, 1000, false};
    frame.allies = {{30, 200, true, true}, {20, 700, false, false}};
    frame.enemies = {{300, 900, 100, true, true, true, true},
        {200, 0, 900, true, false, false, false}};
    frame.hazards = {{500, 900, true, true}, {400, 300, true, true}};
    frame.candidates = {{Intent::Attack, 200, "attack"}, {Intent::Interrupt, 300, "interrupt"},
        {Intent::Heal, 30, "critical_heal"}, {Intent::ProtectTank, 30, "guard"},
        {Intent::AvoidHazard, 400, "sidestep"}, {Intent::AvoidHazard, 500, "move_clear"}};

    OracleChoice const expected = ChooseOracle(frame);
    ExpectChoice(expected, Intent::AvoidHazard, 500, "move_clear");

    std::reverse(frame.allies.begin(), frame.allies.end());
    std::reverse(frame.enemies.begin(), frame.enemies.end());
    std::reverse(frame.hazards.begin(), frame.hazards.end());
    std::reverse(frame.candidates.begin(), frame.candidates.end());
    EXPECT_TRUE(SameChoice(expected, ChooseOracle(frame)));

    std::rotate(frame.candidates.begin(), frame.candidates.begin() + 2, frame.candidates.end());
    std::rotate(frame.enemies.begin(), frame.enemies.begin() + 1, frame.enemies.end());
    EXPECT_TRUE(SameChoice(expected, ChooseOracle(frame)));
}

TEST(ShadowCombatOraclePolicyTest, ActiveHazardOutranksOrdinaryOffense)
{
    DecisionFrame frame;
    frame.self = {1, 1000, false};
    frame.enemies = {{99, 0, 1000, true, false, false, false}};
    frame.hazards = {{77, 1, true, true}};
    frame.candidates = {
        {Intent::Attack, 99, "heavy_attack"}, {Intent::AvoidHazard, 77, "short_step"}};

    ExpectChoice(ChooseOracle(frame), Intent::AvoidHazard, 77, "short_step");
}

TEST(ShadowCombatOraclePolicyTest, CriticalSelfSurvivalIsHighestPriority)
{
    DecisionFrame frame;
    frame.self = {0xfffffffffffffff0ULL, 250, false};
    frame.enemies = {{9, 1000, 1000, true, true, true, true}};
    frame.hazards = {{8, 1000, true, true}};
    frame.candidates = {{Intent::Attack, 9, "attack"}, {Intent::Interrupt, 9, "interrupt"},
        {Intent::AvoidHazard, 8, "move"}, {Intent::Survive, frame.self.guid, "immunity"}};

    ExpectChoice(ChooseOracle(frame), Intent::Survive, frame.self.guid, "immunity");
}

TEST(ShadowCombatOraclePolicyTest, CriticalHealingOutranksInterruptAndAttack)
{
    DecisionFrame frame;
    frame.self = {1, 1000, false};
    frame.allies = {{10, 200, true, true}};
    frame.enemies = {{20, 1000, 1000, true, true, false, false}};
    frame.candidates = {{Intent::Attack, 20, "attack"}, {Intent::Interrupt, 20, "interrupt"},
        {Intent::Heal, 10, "life_saving_heal"}};

    ExpectChoice(ChooseOracle(frame), Intent::Heal, 10, "life_saving_heal");
}

TEST(ShadowCombatOraclePolicyTest, InterruptOutranksNoncriticalHealingAndAttack)
{
    DecisionFrame frame;
    frame.self = {1, 1000, false};
    frame.allies = {{10, 500, false, false}};
    frame.enemies = {{20, 1, 1000, true, true, false, false}};
    frame.candidates = {{Intent::Attack, 20, "attack"}, {Intent::Heal, 10, "heal"},
        {Intent::Interrupt, 20, "interrupt"}};

    ExpectChoice(ChooseOracle(frame), Intent::Interrupt, 20, "interrupt");
}

TEST(ShadowCombatOraclePolicyTest, AddControlAndTankProtectionRemainGenericFactDrivenIntents)
{
    DecisionFrame frame;
    frame.self = {1, 1000, false};
    frame.allies = {{10, 800, true, true}};
    frame.enemies = {{20, 0, 900, true, false, true, true}};
    frame.candidates = {{Intent::Attack, 20, "attack"}, {Intent::ControlAdd, 20, "control"},
        {Intent::ProtectTank, 10, "protect"}};

    ExpectChoice(ChooseOracle(frame), Intent::ProtectTank, 10, "protect");
    frame.allies[0].underAttack = false;
    ExpectChoice(ChooseOracle(frame), Intent::ControlAdd, 20, "control");
}

TEST(ShadowCombatOraclePolicyTest, UsesFullUint64GuidThenActionNameForStableTies)
{
    Guid constexpr lowerFullGuid = 0x0000000100000002ULL;
    Guid constexpr higherFullGuidWithLowerLowWord = 0x0000000200000001ULL;

    DecisionFrame frame;
    frame.enemies = {{higherFullGuidWithLowerLowWord, 0, 500, true, false, false, false},
        {lowerFullGuid, 0, 500, true, false, false, false}};
    frame.candidates = {{Intent::Attack, higherFullGuidWithLowerLowWord, "attack"},
        {Intent::Attack, lowerFullGuid, "zeta"}, {Intent::Attack, lowerFullGuid, "alpha"}};

    ExpectChoice(ChooseOracle(frame), Intent::Attack, lowerFullGuid, "alpha");
    std::reverse(frame.enemies.begin(), frame.enemies.end());
    std::reverse(frame.candidates.begin(), frame.candidates.end());
    ExpectChoice(ChooseOracle(frame), Intent::Attack, lowerFullGuid, "alpha");
}

TEST(ShadowCombatOraclePolicyTest, ComparisonGradesCoverAllContractOutcomes)
{
    OracleChoice const oracle = {true, Intent::Heal, 10, "fast_heal", 600000};

    EXPECT_EQ(Compare(oracle, {true, Intent::Heal, 10, "fast_heal"}), ComparisonGrade::Exact);
    EXPECT_EQ(Compare(oracle, {true, Intent::Attack, 10, "fast_heal"}), ComparisonGrade::ActionOnly);
    EXPECT_EQ(Compare(oracle, {true, Intent::Heal, 10, "slow_heal"}), ComparisonGrade::IntentOnly);
    EXPECT_EQ(Compare(oracle, {true, Intent::Heal, 11, "fast_heal"}), ComparisonGrade::TargetMismatch);
    EXPECT_EQ(Compare(oracle, {}), ComparisonGrade::NoActual);

    EXPECT_EQ(ComparisonGradeName(ComparisonGrade::Exact), "exact");
    EXPECT_EQ(ComparisonGradeName(ComparisonGrade::ActionOnly), "action_only");
    EXPECT_EQ(ComparisonGradeName(ComparisonGrade::IntentOnly), "intent_only");
    EXPECT_EQ(ComparisonGradeName(ComparisonGrade::TargetMismatch), "target_mismatch");
    EXPECT_EQ(ComparisonGradeName(ComparisonGrade::NoActual), "no_actual");
}

TEST(ShadowCombatOraclePolicyTest, ExperienceTierValuesAreExactData)
{
    ExperienceProfile const novice = ProfileFor(ExperienceTier::Novice);
    EXPECT_EQ(novice.minLatencyMs, 900u);
    EXPECT_EQ(novice.maxLatencyMs, 1600u);
    EXPECT_EQ(novice.errorPermille, 200u);
    EXPECT_EQ(novice.knowledgePerTenThousand, 6000u);

    ExperienceProfile const experienced = ProfileFor(ExperienceTier::Experienced);
    EXPECT_EQ(experienced.minLatencyMs, 350u);
    EXPECT_EQ(experienced.maxLatencyMs, 800u);
    EXPECT_EQ(experienced.errorPermille, 80u);
    EXPECT_EQ(experienced.knowledgePerTenThousand, 8000u);

    ExperienceProfile const veteran = ProfileFor(ExperienceTier::Veteran);
    EXPECT_EQ(veteran.minLatencyMs, 100u);
    EXPECT_EQ(veteran.maxLatencyMs, 300u);
    EXPECT_EQ(veteran.errorPermille, 20u);
    EXPECT_EQ(veteran.knowledgePerTenThousand, 9500u);

    ExperienceProfile const oracle = ProfileFor(ExperienceTier::Oracle);
    EXPECT_EQ(oracle.minLatencyMs, 0u);
    EXPECT_EQ(oracle.maxLatencyMs, 0u);
    EXPECT_EQ(oracle.errorPermille, 0u);
    EXPECT_EQ(oracle.knowledgePerTenThousand, 10000u);
}

TEST(ShadowCombatOraclePolicyTest, KnowledgeMaskIsSeededCanonicalAndPermutationStable)
{
    DecisionFrame frame;
    frame.decisionKey = 1234;
    frame.self = {1, 900, false};
    frame.allies = {{20, 400, false, false}, {10, 300, true, true}};
    frame.enemies = {{20, 0, 200, true, false, false, false},
        {10, 800, 100, true, true, true, true}};
    frame.hazards = {{30, 900, true, true}, {20, 100, true, true}};
    frame.candidates = {{Intent::Attack, 20, "zeta"}, {Intent::Attack, 10, "alpha"}};

    DecisionFrame const masked = ApplyKnowledgeMask(frame, 6000, 77);
    ASSERT_EQ(masked.allies.size(), 1u);
    EXPECT_EQ(masked.allies[0].guid, 10u);
    ASSERT_EQ(masked.enemies.size(), 2u);
    EXPECT_EQ(masked.enemies[0].guid, 10u);
    EXPECT_EQ(masked.enemies[1].guid, 20u);
    ASSERT_EQ(masked.hazards.size(), 1u);
    EXPECT_EQ(masked.hazards[0].stableId, 20u);
    ASSERT_EQ(masked.candidates.size(), 2u);
    EXPECT_EQ(masked.candidates[0].targetGuid, 10u);
    EXPECT_EQ(masked.candidates[1].targetGuid, 20u);

    std::reverse(frame.allies.begin(), frame.allies.end());
    std::reverse(frame.enemies.begin(), frame.enemies.end());
    std::reverse(frame.hazards.begin(), frame.hazards.end());
    std::reverse(frame.candidates.begin(), frame.candidates.end());
    DecisionFrame const permuted = ApplyKnowledgeMask(frame, 6000, 77);
    ASSERT_EQ(permuted.allies.size(), masked.allies.size());
    ASSERT_EQ(permuted.enemies.size(), masked.enemies.size());
    ASSERT_EQ(permuted.hazards.size(), masked.hazards.size());
    ASSERT_EQ(permuted.candidates.size(), masked.candidates.size());
    EXPECT_EQ(permuted.allies[0].guid, masked.allies[0].guid);
    EXPECT_EQ(permuted.enemies[0].guid, masked.enemies[0].guid);
    EXPECT_EQ(permuted.enemies[1].guid, masked.enemies[1].guid);
    EXPECT_EQ(permuted.hazards[0].stableId, masked.hazards[0].stableId);
    EXPECT_EQ(permuted.candidates[0].targetGuid, masked.candidates[0].targetGuid);
    EXPECT_EQ(permuted.candidates[1].targetGuid, masked.candidates[1].targetGuid);

    EXPECT_TRUE(ApplyKnowledgeMask(frame, 0, 77).allies.empty());
    EXPECT_TRUE(ApplyKnowledgeMask(frame, 0, 77).enemies.empty());
    EXPECT_TRUE(ApplyKnowledgeMask(frame, 0, 77).hazards.empty());
    EXPECT_EQ(ApplyKnowledgeMask(frame, 10000, 77).allies.size(), frame.allies.size());
}

TEST(ShadowCombatOraclePolicyTest, HashAndLatencySelectionHaveGoldenDeterministicValues)
{
    EXPECT_EQ(DeterministicHash(1, 2, 3), 0xbd64a5d9adefe000ULL);
    EXPECT_EQ(SelectLatencyMs(ProfileFor(ExperienceTier::Novice), 77, 1234), 1399u);
    EXPECT_EQ(SelectLatencyMs(ProfileFor(ExperienceTier::Experienced), 77, 1234), 661u);
    EXPECT_EQ(SelectLatencyMs(ProfileFor(ExperienceTier::Veteran), 77, 1234), 282u);
    EXPECT_EQ(SelectLatencyMs(ProfileFor(ExperienceTier::Oracle), 77, 1234), 0u);
}

TEST(ShadowCombatOraclePolicyTest, ErrorInjectionIsReproducibleAndCandidateOrderIndependent)
{
    DecisionFrame frame;
    frame.decisionKey = 1234;
    frame.self = {1, 100, false};
    frame.enemies = {{10, 0, 0, true, false, false, false},
        {20, 0, 0, true, false, true, true}};
    frame.candidates = {{Intent::ControlAdd, 20, "control"}, {Intent::Attack, 10, "attack"},
        {Intent::Survive, 1, "survive"}};

    EXPECT_TRUE(ShouldInjectError(ProfileFor(ExperienceTier::Novice), 6, frame.decisionKey));
    EXPECT_FALSE(ShouldInjectError(ProfileFor(ExperienceTier::Oracle), 6, frame.decisionKey));

    ExperienceProjection const first = ProjectExperience(frame, ExperienceTier::Novice, 6);
    ASSERT_TRUE(first.errorInjected);
    EXPECT_FALSE(first.choice.actionName == "survive");
    EXPECT_EQ(first.latencyMs, SelectLatencyMs(ProfileFor(ExperienceTier::Novice), 6, frame.decisionKey));

    std::reverse(frame.enemies.begin(), frame.enemies.end());
    std::reverse(frame.candidates.begin(), frame.candidates.end());
    ExperienceProjection const second = ProjectExperience(frame, ExperienceTier::Novice, 6);
    EXPECT_TRUE(second.errorInjected);
    EXPECT_TRUE(SameChoice(first.choice, second.choice));
    EXPECT_EQ(first.latencyMs, second.latencyMs);

    ExperienceProjection const perfect = ProjectExperience(frame, ExperienceTier::Oracle, 6);
    EXPECT_FALSE(perfect.errorInjected);
    EXPECT_EQ(perfect.latencyMs, 0u);
    ExpectChoice(perfect.choice, Intent::Survive, 1, "survive");
}

TEST(ShadowCombatOraclePolicyTest, HeaderHasNoLiveBehaviorOrNondeterministicDependencies)
{
    std::string const source = ReadSource(ModuleRoot() / "src/AutoWow/ShadowCombatOraclePolicy.h");
    for (std::string_view forbidden : {"Engine.h", "Engine.cpp", "PlayerbotAI", "Strategy.h",
             "AutoWowBridge", "WorldSession", "ConfigMgr", "Database", "urand", "rand(",
             "<chrono>", "<random>", "<unordered_", "<iostream>", "<fstream>", "std::cout",
             "printf(", "Onyxia", "bossId", "bossEntry"})
        EXPECT_EQ(source.find(forbidden), std::string::npos) << forbidden;
}
