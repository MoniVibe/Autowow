/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowQuestLedger.h"
#include "EngagementTracker.h"
#include "PackRisk.h"
#include "TacticalPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowTactics;

EngagementSnapshot Single(std::uint32_t manaPct, std::uint32_t cds = kCdWand | kCdShieldKnown | kCdShield)
{
    EngagementSnapshot s;
    s.attackers = 1;
    s.melee = 1;
    s.load = 100;
    s.hpPct = 100;
    s.manaPct = manaPct;
    s.cds = cds;
    return s;
}

TickInput Tick(std::uint64_t ms, bool combat, EngagementSnapshot const& snap = Single(90), std::uint64_t hp = 1000)
{
    TickInput in;
    in.nowMs = ms;
    in.inCombat = combat;
    in.hp = hp;
    in.hpPct = snap.hpPct;
    in.mana = snap.manaPct * 10;
    in.manaPct = snap.manaPct;
    in.snap = snap;
    return in;
}
}  // namespace

TEST(TacticalPolicy, MobWeightIsIntegerAndClamped)
{
    LoadParams const p;
    EXPECT_EQ(MobWeight(kRankNormal, 0, false, p), 100u);
    EXPECT_EQ(MobWeight(kRankNormal, 2, false, p), 130u);
    EXPECT_EQ(MobWeight(kRankNormal, -10, false, p), 40u);   // floor 0.4
    EXPECT_EQ(MobWeight(kRankNormal, 10, false, p), 200u);   // cap 2.0
    EXPECT_EQ(MobWeight(kRankElite, 0, false, p), 300u);
    EXPECT_EQ(MobWeight(kRankRare, 0, false, p), 150u);
    EXPECT_EQ(MobWeight(kRankRareElite, 0, false, p), 450u);
    EXPECT_EQ(MobWeight(kRankNormal, 0, true, p), 120u);
    EXPECT_EQ(MobWeight(kRankElite, 1, true, p), 414u);      // 300 x 1.15 x 1.2
}

TEST(TacticalPolicy, CapacityScalesWithToolsHpAndMana)
{
    PriestParams const p;
    EngagementSnapshot s = Single(100, kCdScream | kCdShield);
    EXPECT_EQ(Capacity(s, p), 250u);
    s.hpPct = 50;
    s.manaPct = 0;
    EXPECT_EQ(Capacity(s, p), 62u);  // 250 x 0.5 x 0.5
    s.cds = 0;
    s.hpPct = 100;
    s.manaPct = 100;
    EXPECT_EQ(Capacity(s, p), 100u);
}

TEST(TacticalPolicy, CheapFightWandsFirstEvenAtFullMana)
{
    PriestParams const p;
    EXPECT_EQ(DesiredPriest(Single(100), p, TacticId::None, false), TacticId::PriestWand);
    // Not cheap (caster target / +2 level / rare): burst while mana >= WandManaPct, wand below it.
    EngagementSnapshot caster = Single(100);
    caster.targetCaster = true;
    EXPECT_EQ(DesiredPriest(caster, p, TacticId::None, false), TacticId::PriestBurst);
    caster.manaPct = 59;
    EXPECT_EQ(DesiredPriest(caster, p, TacticId::None, false), TacticId::PriestWand);
    EngagementSnapshot high = Single(80);
    high.lvlDmax = 2;
    EXPECT_EQ(DesiredPriest(high, p, TacticId::None, false), TacticId::PriestBurst);
    // Burst keeps going down to BurstExitManaPct (hysteresis band 50-60).
    high.manaPct = 55;
    EXPECT_EQ(DesiredPriest(high, p, TacticId::PriestBurst, false), TacticId::PriestBurst);
    EXPECT_EQ(DesiredPriest(high, p, TacticId::PriestWand, false), TacticId::PriestWand);
    high.manaPct = 49;
    EXPECT_EQ(DesiredPriest(high, p, TacticId::PriestBurst, false), TacticId::PriestWand);
    // No wand equipped: never the wand-cycle.
    EXPECT_EQ(DesiredPriest(Single(10, 0), p, TacticId::None, false), TacticId::PriestBurst);
}

TEST(TacticalPolicy, MultiEmergencyAndEscapeLadder)
{
    PriestParams const p;
    EngagementSnapshot s = Single(80);
    s.attackers = 2;
    s.load = 200;
    EXPECT_EQ(DesiredPriest(s, p, TacticId::None, false), TacticId::PriestMulti);

    // Emergency enters below 30 and leaves only above 45.
    s.hpPct = 29;
    EXPECT_EQ(DesiredPriest(s, p, TacticId::PriestMulti, false), TacticId::PriestEmergency);
    s.hpPct = 40;
    EXPECT_EQ(DesiredPriest(s, p, TacticId::PriestMulti, false), TacticId::PriestMulti);
    EXPECT_EQ(DesiredPriest(s, p, TacticId::PriestEmergency, false), TacticId::PriestEmergency);
    s.hpPct = 46;
    EXPECT_EQ(DesiredPriest(s, p, TacticId::PriestEmergency, false), TacticId::PriestMulti);
    // A fast hp slope is an emergency at any hp.
    s.hpPct = 90;
    s.deathEtaMs = 5000;
    EXPECT_EQ(DesiredPriest(s, p, TacticId::PriestMulti, false), TacticId::PriestEmergency);

    // Escape: overwhelmed + losing + known tools on cooldown.
    EngagementSnapshot bad = s;
    bad.deathEtaMs = 0;
    bad.attackers = 3;
    bad.load = 300;
    bad.hpPct = 40;
    bad.manaPct = 20;
    bad.cds = kCdShieldKnown | kCdWand;
    EXPECT_EQ(DesiredPriest(bad, p, TacticId::PriestMulti, false), TacticId::PriestEscape);
    EXPECT_NE(DesiredPriest(bad, p, TacticId::PriestMulti, true), TacticId::PriestEscape);  // one attempt
    bad.cds = kCdWand;  // knows no control tool yet (levels 1-5): fight, never flee
    EXPECT_EQ(DesiredPriest(bad, p, TacticId::PriestMulti, false), TacticId::PriestMulti);
    bad.cds = kCdShieldKnown | kCdShield;  // shield still ready: not spent
    EXPECT_EQ(DesiredPriest(bad, p, TacticId::PriestMulti, false), TacticId::PriestMulti);
}

TEST(TacticalPolicy, HysteresisDwellOnDowngradeImmediateEscalation)
{
    PriestParams const p;
    EngagementSnapshot multi = Single(80);
    multi.attackers = 2;
    multi.load = 200;
    EngagementSnapshot single = Single(80);

    EXPECT_EQ(ChoosePriest(multi, p, TacticId::PriestWand, 100, false), TacticId::PriestMulti);   // escalate now
    EXPECT_EQ(ChoosePriest(single, p, TacticId::PriestMulti, 2999, false), TacticId::PriestMulti);  // dwell
    EXPECT_EQ(ChoosePriest(single, p, TacticId::PriestMulti, 3000, false), TacticId::PriestWand);
    EXPECT_EQ(ChoosePriest(single, p, TacticId::None, 0, false), TacticId::PriestWand);             // first pick

    EngagementSnapshot caster = Single(100);
    caster.targetCaster = true;  // wants burst: lateral switch waits too
    EXPECT_EQ(ChoosePriest(caster, p, TacticId::PriestWand, 1000, false), TacticId::PriestWand);
    EXPECT_EQ(ChoosePriest(caster, p, TacticId::PriestWand, 3000, false), TacticId::PriestBurst);

    // Escape holds EscapeMaxMs, then re-evaluates and never re-enters.
    EngagementSnapshot bad = multi;
    bad.attackers = 3;
    bad.load = 300;
    bad.hpPct = 40;
    bad.manaPct = 20;
    bad.cds = kCdShieldKnown | kCdWand;
    EXPECT_EQ(ChoosePriest(single, p, TacticId::PriestEscape, 1000, false), TacticId::PriestEscape);
    EXPECT_EQ(ChoosePriest(bad, p, TacticId::PriestEscape, 8000, true), TacticId::PriestMulti);
}

TEST(TacticalPolicy, ArmHashIsPinnedAndProportional)
{
    // Pinned: murmur3 fmix32(guid ^ salt x golden) % 100 < ArmPct. Changing the hash reshuffles arms.
    std::uint8_t const expected[10] = {0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    for (std::uint32_t g = 1; g <= 10; ++g)
        EXPECT_EQ(ArmOf(g, 1, 50), expected[g - 1]) << g;
    EXPECT_EQ(Hash32(1), 1364076727u);
    std::uint32_t n50 = 0, n30 = 0;
    for (std::uint32_t g = 1; g <= 10000; ++g)
    {
        n50 += ArmOf(g, 1, 50);
        n30 += ArmOf(g, 7, 30);
    }
    EXPECT_EQ(n50, 4948u);
    EXPECT_EQ(n30, 3053u);
    EXPECT_EQ(ArmOf(5, 1, 0), 0);
    EXPECT_EQ(ArmOf(5, 1, 100), 1);
}

TEST(TacticalPolicy, FactorTableParsing)
{
    FactorTable const t = ParseFactors("smite:0, mind blast:0.5,shoot:3,bad,:2,x:1.2345,y:z");
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(FactorOf(t, "smite"), 0u);
    EXPECT_EQ(FactorOf(t, "mind blast"), 500u);
    EXPECT_EQ(FactorOf(t, "shoot"), 3000u);
    EXPECT_EQ(FactorOf(t, "x"), 1234u);
    EXPECT_EQ(FactorOf(t, "renew"), 1000u);
    EXPECT_TRUE(ParseFactors("").empty());
}

TEST(PackRisk, ScoreBandAndReject)
{
    namespace R = AutoWowPackRisk;
    R::Verdict v = R::Score(100, 0, 250, 160);
    EXPECT_EQ(v.risk, 100u);
    EXPECT_EQ(v.band, 1u);
    EXPECT_FALSE(v.reject);
    EXPECT_FALSE(R::Score(100, 300, 250, 160).reject);  // 400 == 250 x 1.6: allowed
    v = R::Score(100, 301, 250, 160);
    EXPECT_TRUE(v.reject);
    EXPECT_EQ(v.band, 4u);
    EXPECT_FALSE(R::Score(500, 0, 10, 160).reject);     // a lone mob is never a pack

    EXPECT_TRUE(R::HoldPull(100, 40, true, 60, 50));
    EXPECT_FALSE(R::HoldPull(100, 40, false, 60, 50));  // rage/energy user: mana gate does not apply
    EXPECT_TRUE(R::HoldPull(55, 100, true, 60, 50));
    EXPECT_FALSE(R::HoldPull(10, 10, true, 0, 0));      // 0 disables
}

TEST(EngagementTracker, WinWithDebouncedEndAndTacticPath)
{
    PriestParams const p;
    EngagementTracker t;
    EngageRecord r;
    EXPECT_FALSE(t.Tick(Tick(1000, false), p, r));
    EXPECT_FALSE(t.Active());
    EXPECT_FALSE(t.Tick(Tick(2000, true), p, r));
    EXPECT_EQ(t.Current(), TacticId::PriestWand);
    EXPECT_EQ(t.CreditMs(), 0u);  // the start tick credits nothing
    t.NoteCast(CastKind::Shield);
    EXPECT_FALSE(t.Tick(Tick(2500, true), p, r));
    EXPECT_EQ(t.CreditId(), TacticId::PriestWand);
    EXPECT_EQ(t.CreditMs(), 500u);
    t.NoteKill();
    EXPECT_FALSE(t.Tick(Tick(3000, false), p, r));  // combat off: debounce starts
    EXPECT_EQ(t.Current(), TacticId::None);
    EXPECT_FALSE(t.Tick(Tick(3500, false), p, r));
    ASSERT_TRUE(t.Tick(Tick(4000, false), p, r));
    EXPECT_EQ(r.engId, 1u);
    EXPECT_EQ(r.durMs, 1000u);
    EXPECT_EQ(r.outcome, Outcome::Win);
    EXPECT_EQ(r.kills, 1u);
    EXPECT_EQ(r.shieldN, 1u);
    EXPECT_EQ(r.casts, 1u);
    EXPECT_EQ(r.tac0, TacticId::PriestWand);
    ASSERT_EQ(r.segN, 1u);
    EXPECT_EQ(r.segs[0].ms, 1000u);  // 2000 -> 3000: the combat-off tick credits its interval too
    EXPECT_EQ(r.gapMs, -1);
    EXPECT_EQ(r.mobsMax, 1u);
}

TEST(EngagementTracker, ReentryInsideDebounceContinuesAndDeathEnds)
{
    PriestParams const p;
    EngagementTracker t;
    EngageRecord r;
    t.Tick(Tick(2000, true), p, r);
    t.Tick(Tick(3000, false), p, r);
    EXPECT_FALSE(t.Tick(Tick(3500, true), p, r));  // back in combat within 1 s: same engagement
    EXPECT_TRUE(t.Active());
    TickInput dead = Tick(5000, false);
    dead.dead = true;
    ASSERT_TRUE(t.Tick(dead, p, r));
    EXPECT_EQ(r.engId, 1u);
    EXPECT_EQ(r.durMs, 3000u);
    EXPECT_EQ(r.outcome, Outcome::Died);
    EXPECT_EQ(r.hp1, 0u);
}

TEST(EngagementTracker, HpSlopeDeathEtaForcesEmergency)
{
    PriestParams const p;
    EngagementTracker t;
    EngageRecord r;
    EngagementSnapshot s = Single(90);
    s.hpPct = 70;  // above every hp threshold: only the slope can trigger
    t.Tick(Tick(2000, true, s, 1000), p, r);
    t.Tick(Tick(2500, true, s, 900), p, r);
    t.Tick(Tick(3000, true, s, 800), p, r);
    EXPECT_EQ(t.Current(), TacticId::PriestWand);
    t.Tick(Tick(3500, true, s, 700), p, r);  // 300 hp in 1.5 s -> 700 hp left = 3.5 s < 6 s
    EXPECT_EQ(t.Current(), TacticId::PriestEmergency);
    EXPECT_EQ(t.Last().deathEtaMs, 3500u);
    t.Tick(Tick(4000, false, s, 700), p, r);
    ASSERT_TRUE(t.Tick(Tick(5000, false, s, 700), p, r));
    ASSERT_EQ(r.segN, 2u);
    EXPECT_EQ(r.segs[0].id, TacticId::PriestWand);
    EXPECT_EQ(r.segs[0].ms, 1500u);
    EXPECT_EQ(r.segs[1].id, TacticId::PriestEmergency);
    EXPECT_EQ(r.segs[1].ms, 500u);
    EXPECT_EQ(r.hpLost, 300u);
    EXPECT_EQ(r.outcome, Outcome::NoKill);
}

TEST(EngagementTracker, GapAndRestBetweenEngagements)
{
    PriestParams const p;
    EngagementTracker t;
    EngageRecord r;
    t.Tick(Tick(2000, true), p, r);
    t.Tick(Tick(3000, false), p, r);
    ASSERT_TRUE(t.Tick(Tick(4000, false), p, r));
    TickInput rest = Tick(5000, false);
    rest.resting = true;
    t.Tick(rest, p, r);   // sample-and-hold: 4000 -> 5000 counts as resting
    rest.nowMs = 6000;
    t.Tick(rest, p, r);   // 5000 -> 6000
    t.Tick(Tick(6500, false), p, r);
    TickInput pull = Tick(7000, true);
    pull.pullRisk = 2;
    t.Tick(pull, p, r);
    t.Tick(Tick(8000, false), p, r);
    ASSERT_TRUE(t.Tick(Tick(9000, false), p, r));
    EXPECT_EQ(r.engId, 2u);
    EXPECT_EQ(r.gapMs, 4000);      // 3000 (previous end) -> 7000
    EXPECT_EQ(r.gapRestMs, 2000);
    EXPECT_EQ(r.pullRisk, 2);
}

TEST(EngagementTracker, EngageFieldFormatIsFixed)
{
    EngageRecord r;
    r.engId = 7;
    r.tac0 = TacticId::PriestWand;
    r.segs[0] = TacticSeg{TacticId::PriestWand, 1500};
    r.segs[1] = TacticSeg{TacticId::PriestMulti, 3000};
    r.segN = 2;
    r.mobsMax = 2;
    r.addsMax = 1;
    r.lvlDmax = -1;
    r.loadMax = 230;
    r.durMs = 4500;
    r.kills = 2;
    r.outcome = Outcome::Win;
    r.hp0 = 100;
    r.hp1 = 62;
    r.mp0 = 80;
    r.mp1 = 41;
    r.manaSpent = 350;
    r.hpLost = 410;
    r.casts = 6;
    r.wandMs = 2500;
    r.ccN = 1;
    r.shieldN = 1;
    r.gapMs = 12000;
    r.gapRestMs = 9000;
    EXPECT_EQ(FormatEngageFields(r, 5, 2, 1),
              ",\"ecv\":1,\"cls\":5,\"tab\":2,\"arm\":1,\"eng\":7,\"tac0\":10,\"tacs\":[[10,1500],[12,3000]],"
              "\"tacs_drop\":0,\"mobs_max\":2,\"adds\":1,\"elite_n\":0,\"lvl_dmax\":-1,\"load_max\":230,\"pvp\":0,"
              "\"dur_ms\":4500,\"kills\":2,\"outcome\":0,\"hp0\":100,\"hp1\":62,\"mp0\":80,\"mp1\":41,"
              "\"mana_spent\":350,\"hp_lost\":410,\"casts\":6,\"wand_ms\":2500,\"cc_n\":1,\"shield_n\":1,"
              "\"gap_ms\":12000,\"gap_rest_ms\":9000,\"pull_risk\":-1");
}

TEST(EngagementTracker, LedgerEngageEventIsAppendOnly)
{
    using AutoWowQuestLedger::Event;
    EXPECT_EQ(static_cast<int>(Event::Engage), 12);
    EXPECT_EQ(static_cast<int>(Event::SkillUp), 11);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Engage), "engage");
    AutoWowQuestLedger::Row row;
    row.ev = Event::Engage;
    row.ms = 5;
    row.bot = 42;
    row.level = 12;
    row.extra = ",\"ecv\":1";
    std::string const line = AutoWowQuestLedger::FormatLine("r", row);
    EXPECT_NE(line.find("\"ev\":\"engage\""), std::string::npos);
    EXPECT_EQ(line.substr(line.size() - 9), ",\"ecv\":1}");
}
