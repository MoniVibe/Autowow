/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatPerformanceTelemetry.h"
#include "CombatReactivity.h"
#include "DotLifetimeGate.h"

#include <thread>
#include <vector>

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

TEST(CombatLifetime, TacticsSwitchLineToCv2AndKeepCv1Fields)
{
    LifetimeCounters c;
    c.Update(2000, true, false, false);
    std::string const cv1 = c.DrainEmitFields(5);
    EXPECT_EQ(cv1.rfind(",\"cv\":1,\"cls\":5,", 0), 0u);
    EXPECT_EQ(cv1.find("tac_ms"), std::string::npos);  // untouched by the tactical layer: byte-identical cv=1

    c.RecordTactic(12, 500, 1);
    c.RecordTactic(10, 1500, 1);
    c.RecordTactic(10, 500, 1);
    c.RecordTactic(200, 700, 1);  // beyond kTacticSlots: ignored
    std::string const cv2 = c.DrainEmitFields(5);
    std::string expectedCv1Body = cv1.substr(std::string(",\"cv\":1").size());
    EXPECT_EQ(cv2, ",\"cv\":2" + expectedCv1Body + ",\"tac_ms\":[[10,2000],[12,500]],\"arm\":1");
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

// The always-on v1 combat window store is written from map threads (MapUpdate.Threads = 4). Hammer it
// from four threads, including create/evict (more bots than kMaxTrackedBots) and Forget, which rehash
// the map; without the store lock this corrupts the table (crash / hang).
TEST(CombatPerformanceTelemetryStore, ConcurrentWritersFromMapThreads)
{
    namespace T = AutoWowCombatPerformanceTelemetry;
    std::vector<std::thread> threads;
    for (std::uint32_t t = 0; t < 4; ++t)
    {
        threads.emplace_back([t]()
        {
            for (std::uint32_t k = 0; k < 20000; ++k)
            {
                std::uint32_t const guid = 900000 + (k * 7 + t) % (T::kMaxTrackedBots + 64);
                std::uint64_t const now = 1000 + k;
                T::RecordDamageDone(guid, now, 10);
                T::RecordDamageTaken(guid, now, 5);
                T::RecordThreatSample(guid, now, 1, 1, 1.0f);
                T::RecordCombatExit(guid, now);
                if (k % 97 == 0)
                    T::Forget(guid);
                if (k % 13 == 0)
                    (void)T::SnapshotFor(guid);
            }
        });
    }
    for (std::thread& th : threads)
        th.join();
    for (std::uint32_t g = 900000; g < 900000 + T::kMaxTrackedBots + 64; ++g)
        T::Forget(g);
    EXPECT_FALSE(T::SnapshotFor(900000).tracked);
}

// ---- lane combatp0: GCD wake scheduler, cv=3/4 reactivity appendix ----
TEST(CombatReactivity, WakeDelayOnlyEverShortens)
{
    namespace R = AutoWowCombatReactivity;
    EXPECT_EQ(R::WakeDelay(500, 0), 500U);      // not blocked: react delay
    EXPECT_EQ(R::WakeDelay(500, 100), 150U);    // GCD ends in 100 ms: wake 50 ms after
    EXPECT_EQ(R::WakeDelay(500, 449), 499U);
    EXPECT_EQ(R::WakeDelay(500, 450), 500U);    // block + slack reaches the react tick
    EXPECT_EQ(R::WakeDelay(500, 1400), 500U);   // long block: never later than the react tick
    EXPECT_EQ(R::WakeDelay(500, 0xFFFFFFFFu), 500U);
    EXPECT_FALSE(R::GcdWakeEnabled());
    EXPECT_EQ(R::ReactMultiplier(), 5U);        // default = upstream literal
}

TEST(CombatReactivity, EffectiveGcdHasteAndClamp)
{
    namespace T = AutoWowCombatPerformanceTelemetry;
    EXPECT_EQ(T::EffectiveGcdMs(0, true, 800), 0U);         // off-GCD
    EXPECT_EQ(T::EffectiveGcdMs(1500, false, 800), 1500U);  // melee/ability: no haste
    EXPECT_EQ(T::EffectiveGcdMs(1500, true, 800), 1200U);   // cast speed 0.8
    EXPECT_EQ(T::EffectiveGcdMs(1500, true, 500), 1000U);   // floor 1 s
    EXPECT_EQ(T::EffectiveGcdMs(1000, false, 1000), 1000U); // energy GCD
    EXPECT_EQ(T::EffectiveGcdMs(2000, true, 500), 2000U);   // outside 1-1.5 s: untouched
}

TEST(CombatReactivity, PercentileNearestRank)
{
    namespace T = AutoWowCombatPerformanceTelemetry;
    EXPECT_EQ(T::PercentileNearestRank(nullptr, 0, 50), 0U);
    std::uint32_t one[] = {70};
    EXPECT_EQ(T::PercentileNearestRank(one, 1, 90), 70U);
    std::uint32_t v[] = {900, 100, 500, 300, 700, 200, 800, 400, 1000, 600};
    EXPECT_EQ(T::PercentileNearestRank(v, 10, 50), 500U);
    EXPECT_EQ(T::PercentileNearestRank(v, 10, 90), 900U);
    EXPECT_EQ(T::PercentileNearestRank(v, 10, 100), 1000U);
    std::uint32_t w[] = {30, 10, 20};
    EXPECT_EQ(T::PercentileNearestRank(w, 3, 50), 20U);
    EXPECT_EQ(T::PercentileNearestRank(w, 3, 90), 30U);
}

TEST(CombatReactivity, SpellHistogramTopEightDeterministic)
{
    AutoWowCombatPerformanceTelemetry::SpellHistogram h;
    EXPECT_EQ(h.Drain(), "[]");
    // ids 1..10 cast id times each, id 50 three times: the tie at 3 goes to the lower id.
    for (std::uint32_t id = 1; id <= 10; ++id)
        for (std::uint32_t k = 0; k < id; ++k)
            h.Record(id);
    for (int k = 0; k < 3; ++k)
        h.Record(50);
    EXPECT_EQ(h.Drain(), "[[10,10],[9,9],[8,8],[7,7],[6,6],[5,5],[4,4],[3,3]]");
    EXPECT_EQ(h.Distinct(), 0U);  // window cleared
    for (std::uint32_t id = 1; id <= AutoWowCombatPerformanceTelemetry::kSpellSlots + 5; ++id)
        h.Record(id);
    EXPECT_EQ(h.Distinct(), AutoWowCombatPerformanceTelemetry::kSpellSlots);  // bounded
}

TEST(CombatReactivity, ReactionLatencyEventToNextCastStart)
{
    AutoWowCombatPerformanceTelemetry::ReactionLatency r;
    r.Cast(500);         // nothing pending
    r.Event(1000);
    r.Event(1100);       // still pending: first event wins
    r.Cast(900);         // cast started before the event: not a reaction
    r.Cast(1350);
    r.Event(5000);
    r.Cast(5800);
    r.Event(9000);
    r.Expire(9000 + AutoWowCombatPerformanceTelemetry::kReactionExpiryMs);  // never answered: miss
    EXPECT_EQ(r.Drain(), "[2,350,800,1]");
    EXPECT_EQ(r.Drain(), "[0,0,0,0]");
}

TEST(CombatReactivity, ObservedEventsAndBusyUnion)
{
    LifetimeCounters c;
    c.Update(100, true, false, false);
    c.ObserveReactivity(1000, 0, 100);   // baseline: no event
    c.ObserveReactivity(1100, 1, 100);   // attacker added
    c.ObserveReactivity(1200, 1, 34);    // HP crossed below 35%
    c.ObserveReactivity(1300, 1, 20);    // still low: no new event
    // Instant with 1.5 s GCD at 1500 answers both: 400 ms and 300 ms.
    c.RecordCastTiming(1500, 133, 0, 0, 1500, true);
    // Off-GCD instant inside the running GCD adds no busy time.
    c.RecordCastTiming(2000, 7, 0, 0, 0, true);
    // 2.5 s cast ending at 5500 (started 3000, after the GCD ended): busy 3000..5500.
    c.RecordCastTiming(5500, 116, 2500, 0, 1500, true);
    // 5 s channel at 6000 with a 1.5 s GCD: busy 6000..11000.
    c.RecordCastTiming(6000, 5143, 0, 5000, 1500, true);
    // Out of combat (buff): counted in the histogram, not in busy.
    c.RecordCastTiming(20000, 1459, 0, 0, 1500, false);
    EXPECT_EQ(c.BusyMs(), 1500U + 2500U + 5000U);

    std::string const line = c.DrainEmitFields(8, true, true);
    EXPECT_EQ(line.rfind(",\"cv\":3,\"cls\":8,", 0), 0u);
    std::string const tail = ",\"busy_ms\":9000,\"spells\":[[7,1],[116,1],[133,1],[1459,1],[5143,1]],"
                             "\"rl_att\":[1,400,400,0],\"rl_hp\":[1,300,300,0],\"human\":1";
    ASSERT_GE(line.size(), tail.size());
    EXPECT_EQ(line.substr(line.size() - tail.size()), tail);
}

TEST(CombatReactivity, AppendixOffKeepsCv1Cv2ByteIdenticalAndTacticsGoCv4)
{
    LifetimeCounters plain;
    LifetimeCounters fed;
    plain.Update(2000, true, false, false);
    fed.Update(2000, true, false, false);
    fed.ObserveReactivity(100, 2, 100);
    fed.RecordCastTiming(1500, 133, 0, 0, 1500, true);
    fed.RecordCast(true);
    plain.RecordCast(true);
    EXPECT_EQ(fed.DrainEmitFields(5), plain.DrainEmitFields(5));  // appendix off: identical cv=1

    LifetimeCounters t;
    t.RecordTactic(10, 500, 1);
    std::string const cv2 = t.DrainEmitFields(5);
    std::string const cv4 = t.DrainEmitFields(5, true);
    EXPECT_EQ(cv2.rfind(",\"cv\":2,", 0), 0u);
    EXPECT_EQ(cv4.rfind(",\"cv\":4,", 0), 0u);
    std::string const cv2Body = cv2.substr(std::string(",\"cv\":2").size());
    EXPECT_EQ(cv4, ",\"cv\":4" + cv2Body + ",\"busy_ms\":0,\"spells\":[],\"rl_att\":[0,0,0,0],\"rl_hp\":[0,0,0,0]");
}

// A training-dummy fight: in combat, damage and casts, no kill and no death. Combat end is reported once.
TEST(CombatReactivity, DummyFightEndsOnceWithoutKillOrDeath)
{
    LifetimeCounters c;
    EXPECT_FALSE(c.TakeCombatEnded());
    c.Update(100, true, false, false);
    c.RecordDamageDone(100, 250, 0x1234);
    c.RecordCastTiming(200, 133, 0, 0, 1500, true);
    c.Update(5000, true, false, false);
    EXPECT_FALSE(c.TakeCombatEnded());
    c.Update(100, false, false, false);
    EXPECT_TRUE(c.TakeCombatEnded());
    EXPECT_FALSE(c.TakeCombatEnded());
    std::string const line = c.DrainEmitFields(1, true, true);
    EXPECT_NE(line.find("\"combat_ms\":5100,"), std::string::npos);
    EXPECT_NE(line.find("\"dmg\":250,"), std::string::npos);
    EXPECT_NE(line.find("\"kills\":0,"), std::string::npos);
    EXPECT_NE(line.find("\"busy_ms\":1500,"), std::string::npos);
    EXPECT_NE(line.find("\"spells\":[[133,1]]"), std::string::npos);
}

TEST(CombatReactivity, NewTelemetryFlagsDefaultOff)
{
    EXPECT_FALSE(AutoWowCombatPerformanceTelemetry::ReactivityEnabled());
    EXPECT_FALSE(AutoWowCombatPerformanceTelemetry::PlayersEnabled());
}
