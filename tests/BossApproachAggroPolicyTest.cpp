/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#include "../src/AutoWow/BossApproachAggroPolicy.h"

#include <limits>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowBossApproachAggroPolicy;

HazardFacts KnownHazard()
{
    HazardFacts hazard;
    hazard.guid = 101;
    hazard.knownHostile = true;
    hazard.canStartAttack = true;
    hazard.socialAssistPossible = true;
    hazard.tankProbe = true;
    hazard.petFootprint = true;
    hazard.distanceToCorridor = 4.0f;
    hazard.aggroRadius = 8.0f;
    hazard.combatReach = 2.0f;
    hazard.footprintBuffer = 1.0f;
    return hazard;
}
}

TEST(BossApproachAggroPolicy, FormationCorridorHazardBlocksAndReportsPullRisk)
{
    RiskResult const result = Evaluate({KnownHazard()}, false);

    EXPECT_FALSE(result.safe);
    EXPECT_EQ(result.hazardsInCorridor, 1u);
    EXPECT_EQ(result.predictedHazards, 1u);
    EXPECT_EQ(result.accidentalPullCount, 1u);
    EXPECT_EQ(result.socialAssistCandidates, 1u);
    EXPECT_EQ(result.tankProbeHazards, 1u);
    EXPECT_EQ(result.petHazards, 1u);
    EXPECT_EQ(result.reason, "predicted_aggro_or_assist");
    EXPECT_FLOAT_EQ(result.closestMargin, -7.0f);
}

TEST(BossApproachAggroPolicy, IntentionalTargetIsExemptOnlyAfterPullAuthorization)
{
    HazardFacts target = KnownHazard();
    target.intentionalTarget = true;

    RiskResult const beforeAuthorization = Evaluate({target}, false);
    EXPECT_FALSE(beforeAuthorization.safe);
    EXPECT_EQ(beforeAuthorization.predictedHazards, 1u);
    EXPECT_EQ(beforeAuthorization.intentionalExemptions, 0u);

    RiskResult const afterAuthorization = Evaluate({target}, true);
    EXPECT_TRUE(afterAuthorization.safe);
    EXPECT_EQ(afterAuthorization.hazardsInCorridor, 0u);
    EXPECT_EQ(afterAuthorization.predictedHazards, 0u);
    EXPECT_EQ(afterAuthorization.intentionalExemptions, 1u);
    EXPECT_EQ(afterAuthorization.closestMargin, std::numeric_limits<float>::infinity());
}

TEST(BossApproachAggroPolicy, ScriptedOrDetectionUnknownNearCorridorHoldsConservatively)
{
    HazardFacts unknown;
    unknown.guid = 202;
    unknown.scriptedUnknown = true;
    unknown.distanceToCorridor = 5.0f;
    unknown.combatReach = 1.0f;
    unknown.footprintBuffer = 1.0f;

    RiskResult const result = Evaluate({unknown}, false);

    EXPECT_FALSE(result.safe);
    EXPECT_TRUE(result.unknownBlocks);
    EXPECT_EQ(result.unknownHazards, 1u);
    EXPECT_EQ(result.predictedHazards, 0u);
    EXPECT_EQ(result.reason, "hazard_visibility_or_script_unknown");
}

TEST(BossApproachAggroPolicy, PositiveMarginOutsideHoldBandIsNotClaimedAsCorridorHazard)
{
    HazardFacts distant = KnownHazard();
    distant.distanceToCorridor = 30.0f;

    RiskResult const result = Evaluate({distant}, false);

    EXPECT_TRUE(result.safe);
    EXPECT_EQ(result.hazardsInCorridor, 0u);
    EXPECT_EQ(result.predictedHazards, 0u);
    EXPECT_EQ(result.accidentalPullCount, 0u);
    // Margin is distance-to-corridor minus the hazard's aggro, reach, and footprint:
    // 30 - (8 + 2 + 1) = 19. It remains outside the 8-unit unknown hold band.
    EXPECT_FLOAT_EQ(result.closestMargin, 19.0f);
}
