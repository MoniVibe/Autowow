/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EncounterRoleTriggerPolicy.h"
#include "CombatMovementPolicy.h"
#include "OnyBreathSignal.h"
#include "RaidFearResponsePolicy.h"

#include "gtest/gtest.h"

TEST(EncounterRoleTriggerPolicy, OnyxiaPhaseTwoSplitsGroundAddsFromAirPressure)
{
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(false, true, false, true));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(false, false, false, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(false, false, true, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(true, false, true, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(false, true, false, false));

    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldPressureFlyingOnyxia(false, true, true, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPressureFlyingOnyxia(false, true, false, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPressureFlyingOnyxia(true, false, true, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPressureFlyingOnyxia(false, true, true, false));
}

TEST(EncounterRoleTriggerPolicy, NotBehindIngvarUsesNegativeBehindPolarity)
{
    EXPECT_TRUE(EncounterRoleTriggerPolicy::IsNotBehindIngvar(false));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::IsNotBehindIngvar(true));
}

TEST(EncounterRoleTriggerPolicy, OnyxiaDeepBreathSpellCatalogIsExact)
{
    for (uint32 spellId : {17086u, 18351u, 18576u, 18609u, 18564u, 18584u, 18596u, 18617u})
        EXPECT_TRUE(OnyxiaBreathSignal::IsDeepBreathSpell(spellId));

    EXPECT_FALSE(OnyxiaBreathSignal::IsDeepBreathSpell(0));
    EXPECT_FALSE(OnyxiaBreathSignal::IsDeepBreathSpell(18392));
    EXPECT_FALSE(OnyxiaBreathSignal::IsDeepBreathSpell(18435));
}

TEST(EncounterRoleTriggerPolicy, OnyxiaDeepBreathSignalSurvivesTheCastObject)
{
    ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(4242);
    OnyxiaBreathSignal::Clear(botGuid);
    EXPECT_EQ(OnyxiaBreathSignal::GetActive(botGuid), 0u);

    OnyxiaBreathSignal::Record(botGuid, 18596);
    EXPECT_EQ(OnyxiaBreathSignal::GetActive(botGuid), 18596u);

    OnyxiaBreathSignal::Clear(botGuid);
    EXPECT_EQ(OnyxiaBreathSignal::GetActive(botGuid), 0u);
}

TEST(EncounterRoleTriggerPolicy, PriorityRaidAddsPreserveCoreRoles)
{
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(true, false, false, false, true, true, true, true));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, true, true, false, false, true, true, true));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, true, false, false, false, false, false));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, false, true, false, false, false, false));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, false, true, true, false, false, false));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, false, true, true, true, false, false));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, false, true, true, false, true, false));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldHandlePriorityRaidAdd(false, false, false, true, true, false, false, true));
}

TEST(EncounterRoleTriggerPolicy, PriorityRaidAddScoringIsDeterministic)
{
    EXPECT_GT(EncounterRoleTriggerPolicy::PriorityRaidAddScore(true, false, false, false),
        EncounterRoleTriggerPolicy::PriorityRaidAddScore(false, false, false, true));
    EXPECT_GT(EncounterRoleTriggerPolicy::PriorityRaidAddScore(false, true, true, false),
        EncounterRoleTriggerPolicy::PriorityRaidAddScore(false, false, false, true));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferPriorityRaidAdd(false, 1000, 20.0f, 20, false, 900, 1.0f, 1));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferPriorityRaidAdd(false, 1000, 10.0f, 20, false, 1000, 20.0f, 1));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferPriorityRaidAdd(false, 1000, 10.0f, 10, false, 1000, 10.0f, 20));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::PreferPriorityRaidAdd(false, 900, 1.0f, 1, false, 1000, 20.0f, 20));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferPriorityRaidAdd(true, 1, 50.0f, 99, false, 5000, 1.0f, 1));
}

TEST(EncounterRoleTriggerPolicy, PriorityRaidAllocationCapsAndRotatesHelpers)
{
    EXPECT_EQ(EncounterRoleTriggerPolicy::kMaxRespondersPerPriorityRaidAdd, 2u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::kMinimumBossPressureDps, 1u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlots(false), 2u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlots(true), 1u);

    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlot(5, 4, 0), 1u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlot(5, 4, 1), 2u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlot(6, 4, 0), 2u);
    EXPECT_EQ(EncounterRoleTriggerPolicy::PriorityRaidHelperSlot(6, 4, 1), 3u);
}

TEST(EncounterRoleTriggerPolicy, PriorityRaidAllocationPrefersRangedPressureReserveThenGuid)
{
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferBossPressureReserve(true, 50, false, 1));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::PreferBossPressureReserve(false, 1, true, 50));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::PreferBossPressureReserve(true, 10, true, 20));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::PreferBossPressureReserve(true, 20, true, 10));
}

TEST(EncounterRoleTriggerPolicy, PriorityRaidAddStickinessOnlyYieldsToHigherUrgencyOrSelfDefense)
{
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPreemptPriorityRaidAdd(false, 1000, false, 1000));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPreemptPriorityRaidAdd(false, 900, false, 1000));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldPreemptPriorityRaidAdd(false, 1100, false, 1000));
    EXPECT_TRUE(EncounterRoleTriggerPolicy::ShouldPreemptPriorityRaidAdd(true, 1, false, 5000));
    EXPECT_FALSE(EncounterRoleTriggerPolicy::ShouldPreemptPriorityRaidAdd(false, 5000, true, 1));
}

TEST(CombatMovementPolicy, GroundBotsApproachTerrainBelowFlyingTargets)
{
    EXPECT_TRUE(CombatMovementPolicy::ShouldProjectTargetToGround(true, false));
    EXPECT_FALSE(CombatMovementPolicy::ShouldProjectTargetToGround(false, false));
    EXPECT_FALSE(CombatMovementPolicy::ShouldProjectTargetToGround(true, true));

    EXPECT_FLOAT_EQ(CombatMovementPolicy::ResolveApproachZ(true, true, 42.0f, 7.0f, 3.0f), 7.0f);
    EXPECT_FLOAT_EQ(CombatMovementPolicy::ResolveApproachZ(true, false, 42.0f, -100000.0f, 3.0f), 3.0f);
    EXPECT_FLOAT_EQ(CombatMovementPolicy::ResolveApproachZ(false, true, 42.0f, 7.0f, 3.0f), 42.0f);
}

TEST(RaidFearResponsePolicy, DetectsMechanicAndAuraBasedFear)
{
    EXPECT_TRUE(RaidFearResponsePolicy::SpellUsesFear(1ULL << MECHANIC_FEAR, false));
    EXPECT_TRUE(RaidFearResponsePolicy::SpellUsesFear(0, true));
    EXPECT_FALSE(RaidFearResponsePolicy::SpellUsesFear(0, false));
}

TEST(RaidFearResponsePolicy, OnlyPriestsAndShamansPrepareDuringARealRaidWarning)
{
    EXPECT_TRUE(RaidFearResponsePolicy::ShouldPrepare(true, true, true, true, false));
    EXPECT_TRUE(RaidFearResponsePolicy::ShouldPrepare(true, true, true, false, true));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldPrepare(false, true, true, true, false));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldPrepare(true, false, true, true, false));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldPrepare(true, true, false, true, false));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldPrepare(true, true, true, false, false));
}

TEST(RaidFearResponsePolicy, OnyxiaAdapterWarnsOnlyDuringLowHealthFlight)
{
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(true, true, 45.1f));
    EXPECT_TRUE(RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(true, true, 45.0f));
    EXPECT_TRUE(RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(true, true, 39.9f));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(true, false, 39.9f));
    EXPECT_FALSE(RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(false, true, 39.9f));
}
