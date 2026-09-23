/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatPerformanceTelemetry.h"

#include "gtest/gtest.h"

namespace
{
using AutoWowCombatPerformanceTelemetry::CounterSnapshot;
using AutoWowCombatPerformanceTelemetry::RollingCounters;
}

TEST(CombatPerformanceTelemetry, AccumulatesContributionAndAggroMetrics)
{
    RollingCounters counters;
    counters.RecordCombatEntry(1000);
    counters.RecordDamageDone(1100, 1200);
    counters.RecordEffectiveHealing(1200, 800);
    counters.RecordDamageTaken(1300, 500);
    counters.RecordDeath(1400);
    counters.RecordThreatSample(1500, 3, 1, 900.0f);
    counters.RecordThreatSample(2500, 2, 2, 1200.0f);
    counters.RecordCombatExit(2600);

    CounterSnapshot const snapshot = counters.Snapshot(3000);
    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.windowStartUnixMs, 1000U);
    EXPECT_EQ(snapshot.windowDurationMs, 2000U);
    EXPECT_EQ(snapshot.damageDone, 1200U);
    EXPECT_EQ(snapshot.effectiveHealing, 800U);
    EXPECT_EQ(snapshot.damageTaken, 500U);
    EXPECT_EQ(snapshot.deaths, 1U);
    EXPECT_EQ(snapshot.combatEntries, 1U);
    EXPECT_EQ(snapshot.combatExits, 1U);
    EXPECT_EQ(snapshot.threatSamples, 2U);
    EXPECT_EQ(snapshot.maxThreatenedByMe, 3U);
    EXPECT_EQ(snapshot.maxOwnersTargetingBot, 2U);
    EXPECT_FLOAT_EQ(snapshot.maxThreat, 1200.0f);
    EXPECT_FALSE(snapshot.overhealingAvailable);
}

TEST(CombatPerformanceTelemetry, ThreatSamplingIsRateLimited)
{
    RollingCounters counters;
    counters.RecordCombatEntry(1000);
    counters.RecordThreatSample(1000, 1, 0, 100.0f);
    counters.RecordThreatSample(1500, 4, 4, 400.0f);

    CounterSnapshot const beforeInterval = counters.Snapshot(1600);
    EXPECT_EQ(beforeInterval.threatSamples, 1U);
    EXPECT_EQ(beforeInterval.maxThreatenedByMe, 1U);

    counters.RecordThreatSample(2000, 4, 4, 400.0f);
    CounterSnapshot const afterInterval = counters.Snapshot(2100);
    EXPECT_EQ(afterInterval.threatSamples, 2U);
    EXPECT_EQ(afterInterval.maxThreatenedByMe, 4U);
    EXPECT_EQ(afterInterval.maxOwnersTargetingBot, 4U);
    EXPECT_FLOAT_EQ(afterInterval.maxThreat, 400.0f);
}

TEST(CombatPerformanceTelemetry, IdleTimeoutRetiresWindowWithoutSnapshotMutation)
{
    RollingCounters counters;
    counters.RecordDamageDone(1000, 100);

    CounterSnapshot const expired = counters.Snapshot(
        1000 + AutoWowCombatPerformanceTelemetry::kIdleTimeoutMs);
    EXPECT_FALSE(expired.active);
    EXPECT_EQ(expired.damageDone, 0U);

    // The read above is const-only. The next event starts a clean window and cannot inherit the
    // retired contribution.
    counters.RecordDamageDone(32001, 7);
    CounterSnapshot const restarted = counters.Snapshot(32001);
    EXPECT_TRUE(restarted.active);
    EXPECT_EQ(restarted.windowStartUnixMs, 32001U);
    EXPECT_EQ(restarted.damageDone, 7U);
}

TEST(CombatPerformanceTelemetry, MaximumWindowRetiresEvenWhenEventsContinue)
{
    RollingCounters counters;
    counters.RecordDamageDone(1000, 10);
    counters.RecordDamageDone(1000 + AutoWowCombatPerformanceTelemetry::kMaxWindowMs, 20);

    CounterSnapshot const snapshot = counters.Snapshot(
        1000 + AutoWowCombatPerformanceTelemetry::kMaxWindowMs);
    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.windowStartUnixMs,
              1000U + AutoWowCombatPerformanceTelemetry::kMaxWindowMs);
    EXPECT_EQ(snapshot.damageDone, 20U);
}

// C1: the player-side entry hook feeds RecordCombatEntry once per false->true transition; exits come
// from the core OnUnitExitCombat. Alternating transitions within one window count both sides.
TEST(CombatPerformanceTelemetry, PlayerSideEntriesAndExitsPairUp)
{
    RollingCounters counters;
    counters.RecordCombatEntry(1000);
    counters.RecordCombatExit(2000);
    counters.RecordCombatEntry(3000);
    counters.RecordCombatExit(4000);

    CounterSnapshot const snapshot = counters.Snapshot(4500);
    EXPECT_EQ(snapshot.combatEntries, 2U);
    EXPECT_EQ(snapshot.combatExits, 2U);
}

TEST(CombatPerformanceTelemetry, TelemetryFlagDefaultsOff)
{
    EXPECT_FALSE(AutoWowCombatPerformanceTelemetry::TelemetryEnabled());
}
