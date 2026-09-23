/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatPerformanceTelemetry.h"
#include "DotLifetimeGate.h"

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

namespace
{
using AutoWowCombatPerformanceTelemetry::LifetimeCounters;
using AutoWowCombatPerformanceTelemetry::LifetimeTotals;
}

// C2: lifetime totals do not retire with the 30 s idle window.
TEST(CombatLifetime, TotalsSurviveWindowIdleAndSplitWallTime)
{
    LifetimeCounters c;
    c.RecordFight();
    c.Update(1000, true, false, false);
    c.RecordDamageDone(1000, 300, 0);
    c.RecordDamageTaken(40);
    c.RecordHealing(25);
    c.Update(60000, false, false, false);   // long idle: well past kIdleTimeoutMs
    c.Update(5000, false, true, false);     // dead / ghost
    c.Update(2000, true, false, true);      // starved in combat
    c.RecordDeath();
    c.RecordCast(true);
    c.RecordCast(false);

    LifetimeTotals const& t = c.Totals();
    EXPECT_EQ(t.wallMs, 68000U);
    EXPECT_EQ(t.combatMs, 3000U);
    EXPECT_EQ(t.deadMs, 5000U);
    EXPECT_EQ(t.starvedMs, 2000U);
    EXPECT_EQ(t.damageDone, 300U);
    EXPECT_EQ(t.damageTaken, 40U);
    EXPECT_EQ(t.healing, 25U);
    EXPECT_EQ(t.fights, 1U);
    EXPECT_EQ(t.deaths, 1U);
    EXPECT_EQ(t.casts, 2U);
    EXPECT_EQ(t.gcdCasts, 1U);
}

// C4: TTK runs from the bot's first damage on the creature to its killing blow; repeat hits do not restart it.
TEST(CombatLifetime, TtkFromFirstDamageToKill)
{
    LifetimeCounters c;
    c.RecordDamageDone(1000, 10, 77);
    c.RecordDamageDone(4000, 10, 77);
    c.RecordKill(9000, 77, 2);
    c.RecordKill(9500, 88, 0);  // never engaged: a kill, no TTK sample

    EXPECT_EQ(c.Totals().kills, 2U);
    EXPECT_EQ(c.Totals().ttkCount, 1U);
    EXPECT_EQ(c.PendingTtk(), 1U);

    // The engagement closed on kill: a re-used key starts fresh.
    c.RecordDamageDone(20000, 10, 77);
    c.RecordKill(21000, 77, 0);
    EXPECT_EQ(c.Totals().ttkCount, 2U);
}

TEST(CombatLifetime, TtkSlotsEvictOldestDeterministically)
{
    LifetimeCounters c;
    for (std::uint64_t k = 1; k <= AutoWowCombatPerformanceTelemetry::kTtkTracked; ++k)
        c.RecordDamageDone(1000 + k, 1, k);
    c.RecordDamageDone(5000, 1, 999);  // table full: evicts key 1 (oldest)
    c.RecordKill(6000, 1, 0);
    EXPECT_EQ(c.Totals().ttkCount, 0U);
    c.RecordKill(6000, 2, 0);
    c.RecordKill(6000, 999, 0);
    EXPECT_EQ(c.Totals().ttkCount, 2U);

    // An engagement older than kTtkEngageExpiryMs (evade/leash, respawn with the same guid) yields no TTK.
    LifetimeCounters d;
    d.RecordDamageDone(1000, 1, 5);
    d.RecordKill(1000 + AutoWowCombatPerformanceTelemetry::kTtkEngageExpiryMs, 5, 0);
    EXPECT_EQ(d.Totals().kills, 1U);
    EXPECT_EQ(d.Totals().ttkCount, 0U);
}

TEST(CombatLifetime, RecentDpsNeedsCombatAndDecays)
{
    LifetimeCounters c;
    c.RecordDamageDone(0, 1000, 0);
    c.Update(4000, true, false, false);
    EXPECT_EQ(c.RecentDps(), 0U);  // < kRecentDpsMinMs of combat
    c.Update(6000, true, false, false);
    EXPECT_EQ(c.RecentDps(), 100U);  // 1000 dmg / 10 s
    c.Update(51000, true, false, false);  // 61 s > window: halves damage and time
    EXPECT_EQ(c.RecentDps(), 500U * 1000U / 30500U);
}

TEST(CombatLifetime, ResourceStarvedThresholds)
{
    using AutoWowCombatPerformanceTelemetry::IsResourceStarved;
    EXPECT_TRUE(IsResourceStarved(0, 149, 1000));
    EXPECT_FALSE(IsResourceStarved(0, 150, 1000));
    EXPECT_FALSE(IsResourceStarved(0, 0, 0));
    EXPECT_TRUE(IsResourceStarved(1, 99, 1000));
    EXPECT_FALSE(IsResourceStarved(1, 100, 1000));
    EXPECT_TRUE(IsResourceStarved(3, 19, 100));
    EXPECT_FALSE(IsResourceStarved(6, 0, 1000));  // runic power: not measured
}

TEST(CombatLifetime, DotSkipCountsConsecutiveKeyOnce)
{
    LifetimeCounters c;
    c.RecordDotSkip(5);
    c.RecordDotSkip(5);
    c.RecordDotSkip(6);
    c.RecordDotSkip(5);
    EXPECT_EQ(c.Totals().dotSkips, 3U);
}

// C6: the periodic emit arms on first call, then fires once per interval; fields are cumulative and the
// TTK sample list drains per line.
TEST(CombatLifetime, EmitIntervalAndFieldFormat)
{
    LifetimeCounters c;
    EXPECT_FALSE(c.EmitDue(1000, 60000));
    EXPECT_FALSE(c.EmitDue(60999, 60000));
    EXPECT_TRUE(c.EmitDue(61000, 60000));
    EXPECT_FALSE(c.EmitDue(61001, 60000));
    EXPECT_FALSE(c.EmitDue(200000, 0));

    c.RecordFight();
    c.Update(2000, true, false, false);
    c.RecordDamageDone(1000, 120, 9);
    c.RecordKill(4000, 9, -2);
    EXPECT_EQ(c.DrainEmitFields(8),
              ",\"cv\":1,\"cls\":8,\"wall_ms\":2000,\"combat_ms\":2000,\"dead_ms\":0,\"starved_ms\":0,"
              "\"fights\":1,\"kills\":1,\"deaths\":0,\"dmg\":120,\"taken\":0,\"heal\":0,\"casts\":0,"
              "\"gcd_casts\":0,\"dot_skips\":0,\"ttk_n\":1,\"ttk_drop\":0,\"ttk\":[[3000,-2]]");
    std::string const second = c.DrainEmitFields(8);
    EXPECT_NE(second.find("\"ttk_n\":1,"), std::string::npos);
    EXPECT_NE(second.find("\"ttk\":[]"), std::string::npos);
}

TEST(CombatLifetime, TtkOverflowIsCountedNotLost)
{
    LifetimeCounters c;
    for (std::uint64_t k = 1; k <= AutoWowCombatPerformanceTelemetry::kTtkPendingMax + 3; ++k)
    {
        c.RecordDamageDone(k * 10, 1, k);
        c.RecordKill(k * 10 + 5, k, 0);
    }
    EXPECT_EQ(c.Totals().ttkCount, AutoWowCombatPerformanceTelemetry::kTtkPendingMax + 3);
    EXPECT_EQ(c.Totals().ttkDropped, 3U);
    EXPECT_EQ(c.PendingTtk(), AutoWowCombatPerformanceTelemetry::kTtkPendingMax);
}

// ---- C5 DoT/debuff lifetime gate (pure predicate) ----
namespace
{
namespace G = AutoWowDotLifetimeGate;

G::Input Trash(std::uint32_t hpPct, std::uint64_t hp)
{
    G::Input in;
    in.kind = G::Kind::Dot;
    in.hpPct = hpPct;
    in.hp = hp;
    in.botLevel = 20;          // high-HP floor = 20 x 150 = 3000
    in.durationMs = 18000;
    return in;
}
}

TEST(DotLifetimeGate, TrashUnderHpFloorIsSkipped)
{
    G::Params const p;
    EXPECT_EQ(G::Evaluate(Trash(9, 90), p), G::Verdict::SkipLowHp);
    EXPECT_EQ(G::Evaluate(Trash(10, 100), p), G::Verdict::Allow);
    G::Input mark = Trash(5, 50);
    mark.kind = G::Kind::Debuff;
    EXPECT_EQ(G::Evaluate(mark, p), G::Verdict::SkipLowHp);
}

TEST(DotLifetimeGate, BossAndEliteAreNeverGated)
{
    G::Params const p;
    G::Input boss = Trash(3, 30);
    boss.bossOrElite = true;
    boss.estTtkMs = 500;
    EXPECT_EQ(G::Evaluate(boss, p), G::Verdict::Allow);

    G::Input elite = Trash(8, 400000);
    elite.bossOrElite = true;
    EXPECT_EQ(G::Evaluate(elite, p), G::Verdict::Allow);
}

TEST(DotLifetimeGate, HighAbsoluteHpOverridesHpFloorOnly)
{
    G::Params const p;
    G::Input big = Trash(5, 3000);   // 5% but 3000 HP >= 20 x 150
    EXPECT_EQ(G::Evaluate(big, p), G::Verdict::Allow);
    big.estTtkMs = 2000;             // ... still dies in 2 s: TTK rule applies
    EXPECT_EQ(G::Evaluate(big, p), G::Verdict::SkipShortTtk);
    EXPECT_EQ(G::Evaluate(Trash(5, 2999), p), G::Verdict::SkipLowHp);
}

TEST(DotLifetimeGate, ShortTtkSkipsFirstApplication)
{
    G::Params const p;
    G::Input in = Trash(60, 600);
    in.estTtkMs = 8999;              // < 50% of 18 s
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::SkipShortTtk);
    in.estTtkMs = 9000;
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::Allow);
    in.estTtkMs = 0;                 // unknown TTK: HP floor only
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::Allow);
    in.estTtkMs = 1000;
    in.kind = G::Kind::Debuff;       // debuffs ignore the TTK rules
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::Allow);
}

TEST(DotLifetimeGate, RefreshSkippedWhenRunningAuraOutlivesTarget)
{
    G::Params const p;
    G::Input in = Trash(60, 600);
    in.estTtkMs = 10000;             // first application would be allowed (>= 9 s)
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::Allow);
    in.remainingMs = 12000;          // refresh: the running DoT outlasts the target
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::SkipRefreshOutlives);
    in.remainingMs = 3000;           // refresh near expiry on a target that lives on
    EXPECT_EQ(G::Evaluate(in, p), G::Verdict::Allow);
}

TEST(DotLifetimeGate, ClassifyAndTtkEstimate)
{
    EXPECT_EQ(G::Classify("shadow word: pain"), G::Kind::Dot);
    EXPECT_EQ(G::Classify("blood plague"), G::Kind::Dot);
    EXPECT_EQ(G::Classify("hunter's mark"), G::Kind::Debuff);
    EXPECT_EQ(G::Classify("arcane missiles"), G::Kind::None);
    EXPECT_EQ(G::Classify("mangle (cat)"), G::Kind::None);

    EXPECT_EQ(G::EstimateTtkMs(500, 1000, 0, 0), 0U);            // nothing known
    EXPECT_EQ(G::EstimateTtkMs(500, 1000, 0, 50), 10000U);       // bot recent DPS only
    EXPECT_EQ(G::EstimateTtkMs(500, 1000, 5000, 50), 5000U);     // observed 100/s beats 50/s
    EXPECT_EQ(G::EstimateTtkMs(500, 1000, 1999, 50), 10000U);    // < 2 s observed: ignored
    EXPECT_EQ(G::EstimateTtkMs(1000, 1000, 9000, 0), 0U);        // no HP loss observed yet
}

TEST(DotLifetimeGate, FlagDefaultsOff)
{
    EXPECT_FALSE(G::Enabled());
}
