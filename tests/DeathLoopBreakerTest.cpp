/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DeathLoopBreaker.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowDeathLoop;

DeathSample At(std::uint64_t ms, std::int32_t x, std::int32_t y, std::uint32_t map = 0)
{
    return DeathSample{ms, map, x, y};
}

// Bot 112 (soak-s5-cohort-r2): L18 scout dying every 15-150 s at (-7988,-2371) in Burning Steppes.
TEST(DeathLoopBreaker, SameSpotDeathsClusterWithinWindowAndRadius)
{
    BotState s;
    DeathSample c;
    EXPECT_EQ(RecordDeath(s, At(1000, -7988, -2371), 1800000, 60, c), 1U);
    EXPECT_EQ(RecordDeath(s, At(60000, -7990, -2380), 1800000, 60, c), 2U);
    EXPECT_EQ(RecordDeath(s, At(90000, -7960, -2350), 1800000, 60, c), 3U);
    // Integer centroid, truncating division: (-7988-7990-7960)/3, (-2371-2380-2350)/3.
    EXPECT_EQ(c.x, -7979);
    EXPECT_EQ(c.y, -2367);
    // Far away (other side of the zone) and another map do not join the cluster.
    EXPECT_EQ(RecordDeath(s, At(100000, -7700, -2371), 1800000, 60, c), 1U);
    EXPECT_EQ(RecordDeath(s, At(110000, -7988, -2371, 1), 1800000, 60, c), 1U);
    EXPECT_EQ(s.deathCount, 5U);
}

TEST(DeathLoopBreaker, DeathsOutsideTheWindowOrFromTheFutureAreForgotten)
{
    BotState s;
    DeathSample c;
    RecordDeath(s, At(1000, 0, 0), 10000, 60, c);
    RecordDeath(s, At(5000, 0, 0), 10000, 60, c);
    EXPECT_EQ(RecordDeath(s, At(12000, 0, 0), 10000, 60, c), 2U);  // 1000 is 11 s old
    EXPECT_EQ(s.deathCount, 2U);
    // A clock step back drops samples newer than the new death.
    EXPECT_EQ(RecordDeath(s, At(4000, 0, 0), 10000, 60, c), 1U);
    EXPECT_EQ(s.deathCount, 1U);
}

TEST(DeathLoopBreaker, RingIsBoundedAndDropsTheOldest)
{
    BotState s;
    DeathSample c;
    for (std::uint64_t k = 0; k < kMaxDeaths + 3; ++k)
        RecordDeath(s, At(1000 + k, static_cast<std::int32_t>(k * 1000), 0), 1800000, 60, c);
    EXPECT_EQ(s.deathCount, kMaxDeaths);
    EXPECT_EQ(s.deaths[0].ms, 1003U);
    EXPECT_EQ(s.deaths[kMaxDeaths - 1].ms, 1000U + kMaxDeaths + 2);
}

TEST(DeathLoopBreaker, RadiusBoundaryIsInclusive)
{
    EXPECT_TRUE(Within(0, 0, 36, 48, 60));    // exactly 60
    EXPECT_FALSE(Within(0, 0, 36, 49, 60));
    EXPECT_TRUE(Within(-2147483000, 0, -2147483000, 0, 0));  // no overflow at the int32 edge
}

TEST(DeathLoopBreaker, EscalationChoice)
{
    Params p;  // deaths 3, gap 10, margin 5
    // Bot 7: L13 in the Barrens (10-25) dying to a L13-14 mob: only the 3rd same-spot death escalates.
    EXPECT_FALSE(Evaluate(p, 2, 13, 14, 10).Escalate());
    Decision d = Evaluate(p, 3, 13, 14, 10);
    EXPECT_EQ(d.trigger, Trigger::Cluster);
    EXPECT_FALSE(d.relocate);
    // Bot 112: L18 killed by a L54 in Burning Steppes (51-60): the first death escalates and relocates.
    d = Evaluate(p, 1, 18, 54, 51);
    EXPECT_EQ(d.trigger, Trigger::LevelGap);
    EXPECT_TRUE(d.relocate);
    EXPECT_EQ(Evaluate(p, 3, 18, 54, 51).trigger, Trigger::ClusterAndLevelGap);
    // Bot 307: L55 in Burning Steppes, killer +2/+3: cluster only, no relocation (in band).
    d = Evaluate(p, 3, 55, 58, 51);
    EXPECT_EQ(d.trigger, Trigger::Cluster);
    EXPECT_FALSE(d.relocate);
    // Gap boundary (+10 inclusive), unknown killer level never triggers the gap rule.
    EXPECT_EQ(Evaluate(p, 1, 20, 30, 0).trigger, Trigger::LevelGap);
    EXPECT_FALSE(Evaluate(p, 1, 20, 29, 0).Escalate());
    EXPECT_FALSE(Evaluate(p, 1, 20, 0, 0).Escalate());
    // Relocation needs a known bracket strictly more than margin above the bot.
    EXPECT_FALSE(Evaluate(p, 3, 18, 0, 0).relocate);
    EXPECT_FALSE(Evaluate(p, 3, 18, 0, 23).relocate);
    EXPECT_TRUE(Evaluate(p, 3, 18, 0, 24).relocate);
    // Rules can be switched off.
    Params off = p;
    off.deaths = 0;
    off.killerLevelGap = 0;
    EXPECT_FALSE(Evaluate(off, 50, 1, 80, 70).Escalate());
    EXPECT_FALSE(Evaluate(off, 50, 1, 80, 70).relocate);
}

TEST(DeathLoopBreaker, DangerAreaCooldownAndSlots)
{
    BotState s;
    MarkDanger(s, At(0, -7980, -1330), 60, 1000, 1000 + 3600000);
    EXPECT_TRUE(IsDangerous(s, 0, -7980, -1330, 2000));
    EXPECT_TRUE(IsDangerous(s, 0, -7980 + 60, -1330, 2000));
    EXPECT_FALSE(IsDangerous(s, 0, -7980 + 61, -1330, 2000));
    EXPECT_FALSE(IsDangerous(s, 1, -7980, -1330, 2000));        // other map
    EXPECT_FALSE(IsDangerous(s, 0, -7980, -1330, 1000 + 3600000));  // cooldown over

    // A re-escalation at the same spot refreshes the same slot instead of taking a new one.
    MarkDanger(s, At(0, -7970, -1320), 60, 5000, 5000 + 3600000);
    EXPECT_EQ(s.danger[0].untilMs, 5000U + 3600000);
    EXPECT_EQ(s.danger[1].untilMs, 0U);

    // Distinct spots fill free slots, then evict the one expiring first (lowest index on ties).
    MarkDanger(s, At(0, 0, 0), 60, 6000, 6000 + 100);
    MarkDanger(s, At(0, 1000, 0), 60, 6000, 6000 + 200);
    MarkDanger(s, At(0, 2000, 0), 60, 6000, 6000 + 300);
    MarkDanger(s, At(0, 3000, 0), 60, 6000, 6000 + 400);
    EXPECT_EQ(s.danger[1].x, 3000);  // evicted the +100 slot
    EXPECT_TRUE(IsDangerous(s, 0, -7970, -1320, 6050));
    // Expired slots are reused first.
    MarkDanger(s, At(0, 4000, 0), 60, 6250, 6250 + 1000);
    EXPECT_EQ(s.danger[2].x, 4000);  // the +200 slot expired at 6200
}

TEST(DeathLoopBreaker, LedgerFieldsAreStable)
{
    Params p;
    Decision d{Trigger::LevelGap, true};
    EXPECT_EQ(LedgerFields(1, 54, 51, d, At(0, -7988, -2371), p),
              ",\"deaths\":1,\"klvl\":54,\"zlow\":51,\"relocate\":1,\"dx\":-7988,\"dy\":-2371,\"dr\":60,"
              "\"cool_ms\":3600000");
    EXPECT_STREQ(TriggerName(Trigger::ClusterAndLevelGap), "cluster_level_gap");
}

// soak-s13-full-r1 bot 62957: L13 in Duskwood (bracket low 19), margin 5.
TEST(DeathLoopBreaker, OvershootIsBracketLowAboveLevelPlusMargin)
{
    EXPECT_TRUE(Overshoot(19, 13, 5));
    EXPECT_FALSE(Overshoot(18, 13, 5));  // strictly above
    EXPECT_FALSE(Overshoot(0, 13, 5));   // unknown bracket: never
    EXPECT_TRUE(Overshoot(15, 13, 0));
    // Evaluate's relocation keeps the same rule.
    Params p;
    EXPECT_TRUE(Evaluate(p, 1, 13, 24, 19).relocate);
    EXPECT_FALSE(Evaluate(p, 1, 13, 24, 18).relocate);
}

TEST(DeathLoopBreaker, RecentDeathsCountsTheWindowOnly)
{
    BotState s;
    DeathSample c;
    RecordDeath(s, At(1000, 0, 0), 1800000, 60, c);
    RecordDeath(s, At(500000, 900, 900), 1800000, 60, c);  // another spot still counts
    RecordDeath(s, At(900000, 0, 0), 1800000, 60, c);
    EXPECT_EQ(RecentDeaths(s, 900000, 1800000), 3U);
    EXPECT_EQ(RecentDeaths(s, 1801000, 1800000), 3U);  // the first is exactly WindowMs old
    EXPECT_EQ(RecentDeaths(s, 1801001, 1800000), 2U);
    EXPECT_EQ(RecentDeaths(s, 999, 1800000), 0U);      // clock stepped back: all in the future
}

TEST(DeathLoopBreaker, EscapePortalAfterPortalDeaths)
{
    EXPECT_FALSE(EscapePortalNow(2, 3));
    EXPECT_TRUE(EscapePortalNow(3, 3));
    EXPECT_TRUE(EscapePortalNow(8, 3));   // 61 deaths in soak-s13: portal at once
    EXPECT_FALSE(EscapePortalNow(8, 0));  // 0 = never at once
    EXPECT_EQ(Params{}.escapePortalDeaths, 3U);
}

// AutoWow.DeathLoop.V2 (b), soak-s14-full-r1: 20 escalations, 1 relocation - the zone bracket fit, so the
// bot rezzed at the same graveyard (62964: DeathLoop count 5, 6, 7 in one 60 yd circle, relocate=false).
TEST(DeathLoopBreaker, MarkDangerReportsARepeatInTheSameLiveArea)
{
    BotState s;
    EXPECT_FALSE(MarkDanger(s, At(0, -5347, -2850), 60, 1000, 1000 + 3600000));  // first escalation
    EXPECT_TRUE(MarkDanger(s, At(0, -5330, -2840), 60, 2000, 2000 + 3600000));   // same circle
    EXPECT_FALSE(MarkDanger(s, At(0, -5000, -2850), 60, 3000, 3000 + 3600000));  // elsewhere
    EXPECT_FALSE(MarkDanger(s, At(0, -5347, -2850, 1), 60, 3000, 3000 + 3600000));  // other map
    // Once the first area expired, dying there again is a first escalation again.
    EXPECT_FALSE(MarkDanger(s, At(0, -5347, -2850), 60, 2000 + 3600000, 2000 + 7200000));
}

TEST(DeathLoopBreaker, V2DefaultsOff)
{
    BotState const s;
    EXPECT_FALSE(s.restPending);
    EXPECT_FALSE(V2Enabled());
    EXPECT_FALSE(RestPending(62964));
}

TEST(DeathLoopBreaker, RuntimeQueriesAreInertWhenDisabled)
{
    ASSERT_FALSE(Enabled());
    EXPECT_FALSE(EscapeEnabled());
    EXPECT_FALSE(WantsSpiritHealer(112));
    EXPECT_FALSE(IsDangerous(112, 0, -7988.0f, -2371.0f));
    EXPECT_FALSE(TakeRelocation(112));
    EXPECT_FALSE(TakeQuestDeferral(112, 4183));
    EXPECT_EQ(RecentDeaths(112U), 0U);
}

// ---- AutoWow.Survival.HardEscape (3) --------------------------------------------------------------------
TEST(DeathLoopBreaker, HardEscapeDefaultsOff)
{
    EXPECT_FALSE(HardEscapeEnabled());
    HardParams const h;
    EXPECT_EQ(h.walkZoneMargin, 5U);
    EXPECT_EQ(h.zoneGap, 10U);
    EXPECT_EQ(h.deaths, 4U);
    EXPECT_EQ(h.killerGap, 10U);
    EXPECT_EQ(h.stuckMs, 120000U);
    EXPECT_FALSE(BotState{}.hard.active);
}

TEST(DeathLoopBreaker, HardConditionIsAFarOverZoneOrADeathLoopFarAbove)
{
    HardParams const h;
    // soak-s22-full-r1: Taelorin L18 in Burning Steppes (low 51 > 28), killer 57.
    EXPECT_TRUE(HardCondition(h, 18, 51, 0, 0));
    EXPECT_FALSE(HardCondition(h, 18, 28, 0, 0));      // strictly above level + ZoneGap
    EXPECT_FALSE(HardCondition(h, 18, 0, 0, 0));       // unknown zone
    EXPECT_TRUE(HardCondition(h, 18, 10, 4, 28));      // 4 deaths, killer 10 above, in a fitting zone
    EXPECT_FALSE(HardCondition(h, 18, 10, 3, 57));     // 3 deaths
    EXPECT_FALSE(HardCondition(h, 18, 10, 8, 27));     // killer only 9 above (Westfall loops: klvl 16-17)
    HardParams off = h;
    off.deaths = 0;
    EXPECT_FALSE(HardCondition(off, 18, 10, 8, 57));
}

// soak-s22-full-r1 replay: Taelorin rezzes at (-7924,-1354) and dies again within seconds, in combat on every
// alive sample (no hearth), for 55 minutes. Here the portal fires once 120 s pass without movement.
TEST(DeathLoopBreaker, HardStepReplaysTaelorin)
{
    HardParams const h;
    HardState s;
    std::uint64_t t = 1000000;
    EXPECT_EQ(HardStep(h, s, t, true, true, 0, -7924, -1354, false), HardAction::None);  // episode starts
    EXPECT_TRUE(s.active);
    for (std::uint64_t dt = 5000; dt < 120000; dt += 5000)
    {
        bool const alive = (dt / 5000) % 3 == 0;  // dead two samples in three; ghost walks do not reset
        std::int32_t const x = alive ? -7924 + std::int32_t(dt % 40) : -7600;
        EXPECT_EQ(HardStep(h, s, t + dt, true, alive, 0, x, -1354, false), HardAction::None) << dt;
    }
    EXPECT_EQ(HardStep(h, s, t + 120000, true, false, 0, -7924, -1354, false), HardAction::None);  // dead: waits
    EXPECT_EQ(HardStep(h, s, t + 125000, true, true, 0, -7910, -1350, false), HardAction::Portal);
    EXPECT_FALSE(s.active);
    // Cooldown: nothing until it runs out, then a fresh episode.
    EXPECT_EQ(HardStep(h, s, t + 125000 + 600000, true, true, 0, -7910, -1350, false), HardAction::None);
    EXPECT_FALSE(s.active);
    EXPECT_EQ(HardStep(h, s, t + 125000 + h.cooldownMs, true, true, 0, -7910, -1350, false), HardAction::None);
    EXPECT_TRUE(s.active);
}

TEST(DeathLoopBreaker, HardStepHearthsFirstThenPortals)
{
    HardParams const h;
    HardState s;
    EXPECT_EQ(HardStep(h, s, 0, true, true, 0, 0, 0, true), HardAction::None);
    EXPECT_EQ(HardStep(h, s, 119999, true, true, 0, 0, 0, true), HardAction::None);
    EXPECT_EQ(HardStep(h, s, 120000, true, true, 0, 50, 0, true), HardAction::Hearth);
    // The hearth did not move it (interrupted): the portal after kHardHearthRetryMs, never a second hearth.
    EXPECT_EQ(HardStep(h, s, 120000 + kHardHearthRetryMs - 1, true, true, 0, 0, 0, true), HardAction::None);
    EXPECT_EQ(HardStep(h, s, 120000 + kHardHearthRetryMs, true, true, 0, 0, 0, true), HardAction::Portal);
}

TEST(DeathLoopBreaker, HardStepMovingOrSafeEndsTheEpisode)
{
    HardParams const h;
    HardState s;
    EXPECT_EQ(HardStep(h, s, 0, true, true, 0, 0, 0, false), HardAction::None);
    // Walking out: every sample 101 yd on restarts the clock.
    for (std::uint64_t t = 60000; t <= 600000; t += 60000)
        EXPECT_EQ(HardStep(h, s, t, true, true, 0, std::int32_t(t / 60000) * 101, 0, false), HardAction::None);
    // Another map is movement too.
    EXPECT_EQ(HardStep(h, s, 700000, true, true, 1, 1010, 0, false), HardAction::None);
    EXPECT_EQ(s.sinceMs, 700000U);
    // Out of the hard condition: the episode ends; back in it: a new one.
    EXPECT_EQ(HardStep(h, s, 800000, false, true, 1, 1010, 0, false), HardAction::None);
    EXPECT_FALSE(s.active);
    EXPECT_EQ(HardStep(h, s, 900000, true, true, 1, 1010, 0, false), HardAction::None);
    EXPECT_EQ(HardStep(h, s, 1019999, true, true, 1, 1010, 0, false), HardAction::None);
    EXPECT_EQ(HardStep(h, s, 1020000, true, true, 1, 1010, 0, false), HardAction::Portal);
}

TEST(DeathLoopBreaker, HardPortalTargetIsAFittingBindElseTheCapital)
{
    Place const westfall{0, -10653, 1166, 34, 40};  // Sentinel Hill inn, Westfall low 10
    Place const steppes{0, -8365, -2737, 186, 46};  // low 51
    // Taelorin bound in Westfall: home. Bound in the Steppes (or nowhere): Stormwind.
    Place p = HardPortalTarget(1, 0, westfall, 10, 18, 5);
    EXPECT_EQ(p.zone, 40U);
    EXPECT_EQ(p.x, -10653);
    p = HardPortalTarget(1, 0, steppes, 51, 18, 5);
    EXPECT_EQ(p.zone, 1519U);
    EXPECT_EQ(p.x, -8833);
    EXPECT_EQ(HardPortalTarget(1, 0, Place{}, 0, 18, 5).zone, 1519U);
    EXPECT_EQ(HardPortalTarget(1, 1, steppes, 51, 18, 5).zone, 1657U);  // Darnassus
    EXPECT_EQ(HardPortalTarget(2, 0, steppes, 51, 18, 5).zone, 1497U);  // Undercity
    EXPECT_EQ(HardPortalTarget(2, 1, steppes, 51, 18, 5).zone, 1637U);  // Orgrimmar
    EXPECT_EQ(HardPortalTarget(2, 530, steppes, 51, 18, 5).map, 1U);   // elsewhere: Orgrimmar
}
}  // namespace
