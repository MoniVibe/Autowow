/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowQuestLedger.h"
#include "EngagementTracker.h"
#include "PackRisk.h"
#include "TacticalClassTables.h"
#include "TacticalPolicy.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <string>

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

TEST(TacticalPolicy, TacticIdsAreFamilySlotEncoded)
{
    // Wire-stable: id = 10 x family + slot; priest ids keep their T1 values.
    EXPECT_EQ(TacticOf(Family::Priest, kSlotSingle), TacticId::PriestWand);
    EXPECT_EQ(TacticOf(Family::Priest, kSlotEscape), TacticId::PriestEscape);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::WarlockSingle), 20u);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::MageMulti), 32u);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::RogueEscape), 54u);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::WarriorEmergency), 63u);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::ShamanSingle), 90u);
    EXPECT_EQ(static_cast<std::uint32_t>(TacticId::DeathKnightEscape), 104u);
    EXPECT_LT(static_cast<std::uint32_t>(TacticId::DeathKnightEscape), kMaxTacticId);
    EXPECT_EQ(FamilyOf(TacticId::HunterMulti), Family::Hunter);
    EXPECT_EQ(SlotOf(TacticId::PaladinEmergency), kSlotEmergency);
    // Severity by slot, same for every family.
    EXPECT_EQ(Severity(TacticId::PriestBurst), 1u);
    EXPECT_EQ(Severity(TacticId::DruidSingle), 1u);
    EXPECT_EQ(Severity(TacticId::DruidMulti), 2u);
    EXPECT_EQ(Severity(TacticId::WarriorEmergency), 3u);
    EXPECT_EQ(Severity(TacticId::MageEscape), 4u);
    EXPECT_EQ(Severity(TacticId::None), 0u);
    // Core class ids -> family; config class list.
    EXPECT_EQ(FamilyOfClass(1), Family::Warrior);
    EXPECT_EQ(FamilyOfClass(5), Family::Priest);
    EXPECT_EQ(FamilyOfClass(11), Family::Druid);
    EXPECT_EQ(FamilyOfClass(10), Family::None);
    EXPECT_EQ(ParseClassMask("priest"), 1u << 5);
    EXPECT_EQ(ParseClassMask("priest,warrior rogue"), (1u << 5) | (1u << 1) | (1u << 4));
    EXPECT_EQ(ParseClassMask("dk,bogus"), 1u << 6);
    EXPECT_EQ(ParseClassMask("all"), 0xBFEu);  // classes 1-9 and 11
    EXPECT_EQ(ParseClassMask(""), 0u);
}

TEST(TacticalPolicy, ClassLadderMirrorsPriestLadder)
{
    ClassParams const p;
    Family const f = Family::Warrior;
    EngagementSnapshot s = Single(100, 0);
    EXPECT_EQ(DesiredClass(f, s, p, TacticId::None, false), TacticId::WarriorSingle);
    s.attackers = 2;
    s.load = 200;
    EXPECT_EQ(DesiredClass(f, s, p, TacticId::None, false), TacticId::WarriorMulti);
    s.hpPct = 29;
    EXPECT_EQ(DesiredClass(f, s, p, TacticId::WarriorMulti, false), TacticId::WarriorEmergency);
    s.hpPct = 40;  // hysteresis band: stays in emergency, does not enter it
    EXPECT_EQ(DesiredClass(f, s, p, TacticId::WarriorEmergency, false), TacticId::WarriorEmergency);
    EXPECT_EQ(DesiredClass(f, s, p, TacticId::WarriorMulti, false), TacticId::WarriorMulti);

    // Escape: overwhelmed, losing, known control/defensive tools spent, an escape tool ready; never without
    // known tools, never without a ready escape tool (S20: run-away without one died 14 of 15 times).
    EngagementSnapshot bad = s;
    bad.attackers = 3;
    bad.load = 400;
    bad.cds = kCdControlKnown | kCdDefensiveKnown;
    EXPECT_EQ(DesiredClass(f, bad, p, TacticId::WarriorMulti, false), TacticId::WarriorMulti);
    bad.cds |= kCdEscapeKnown;  // known but on cooldown
    EXPECT_EQ(DesiredClass(f, bad, p, TacticId::WarriorMulti, false), TacticId::WarriorMulti);
    bad.cds |= kCdEscape;
    EXPECT_EQ(DesiredClass(f, bad, p, TacticId::WarriorMulti, false), TacticId::WarriorEscape);
    EXPECT_NE(DesiredClass(f, bad, p, TacticId::WarriorMulti, true), TacticId::WarriorEscape);
    bad.cds = 0;
    EXPECT_EQ(DesiredClass(f, bad, p, TacticId::WarriorMulti, false), TacticId::WarriorMulti);
    bad.cds = kCdControlKnown | kCdControl;  // shout ready: not spent
    EXPECT_EQ(DesiredClass(f, bad, p, TacticId::WarriorMulti, false), TacticId::WarriorMulti);

    // Hysteresis: immediate escalation, dwell on downgrade, escape hold then no re-entry.
    EngagementSnapshot single = Single(100, 0);
    EngagementSnapshot multi = single;
    multi.attackers = 2;
    multi.load = 200;
    EXPECT_EQ(ChooseClass(f, multi, p, TacticId::WarriorSingle, 10, false), TacticId::WarriorMulti);
    EXPECT_EQ(ChooseClass(f, single, p, TacticId::WarriorMulti, 2999, false), TacticId::WarriorMulti);
    EXPECT_EQ(ChooseClass(f, single, p, TacticId::WarriorMulti, 3000, false), TacticId::WarriorSingle);
    bad.cds = kCdControlKnown;
    EXPECT_EQ(ChooseClass(f, single, p, TacticId::WarriorEscape, 7999, true), TacticId::WarriorEscape);
    EXPECT_EQ(ChooseClass(f, bad, p, TacticId::WarriorEscape, 8000, true), TacticId::WarriorMulti);
}

TEST(TacticalPolicy, ClassCapacityCountsPetAndTools)
{
    ClassParams const p;
    EngagementSnapshot s;
    s.hpPct = 100;
    s.manaPct = 100;
    EXPECT_EQ(Capacity(s, p), 100u);
    s.cds = kCdPet;
    EXPECT_EQ(Capacity(s, p), 200u);
    s.cds = kCdPet | kCdControl | kCdDefensive;
    EXPECT_EQ(Capacity(s, p), 350u);
    s.hpPct = 50;
    s.manaPct = 0;
    EXPECT_EQ(Capacity(s, p), 87u);  // 350 x 0.5 x 0.5, truncated
    // A hunter with its pet alive handles a pair it would otherwise flee from.
    EngagementSnapshot pair = Single(100, kCdControlKnown | kCdDefensiveKnown | kCdEscapeKnown | kCdEscape);
    pair.attackers = 2;
    pair.load = 130;  // 13000 vs capacity 44 x 160 = 7040 (no pet) / 88 x 160 = 14080 (pet)
    pair.hpPct = 44;
    EXPECT_EQ(DesiredClass(Family::Hunter, pair, p, TacticId::HunterMulti, false), TacticId::HunterEscape);
    pair.cds |= kCdPet;
    EXPECT_EQ(DesiredClass(Family::Hunter, pair, p, TacticId::HunterMulti, false), TacticId::HunterMulti);
}

TEST(EngagementTracker, ClassFamilyTickRecordsClassTactics)
{
    EngagementTracker t;
    ClassParams const p;
    EngageRecord rec;
    EngagementSnapshot single = Single(100, 0);
    EngagementSnapshot multi = single;
    multi.attackers = 2;
    multi.load = 200;
    EXPECT_FALSE(t.Tick(Tick(0, true, single), Family::Rogue, p, rec));
    EXPECT_EQ(t.Current(), TacticId::RogueSingle);
    EXPECT_FALSE(t.Tick(Tick(500, true, multi), Family::Rogue, p, rec));
    EXPECT_EQ(t.Current(), TacticId::RogueMulti);
    EXPECT_EQ(t.CreditId(), TacticId::RogueSingle);  // the 500 ms just spent
    EXPECT_FALSE(t.Tick(Tick(1000, false, multi), Family::Rogue, p, rec));
    EXPECT_TRUE(t.Tick(Tick(2000, false, multi), Family::Rogue, p, rec));
    EXPECT_EQ(rec.tac0, TacticId::RogueSingle);
    ASSERT_EQ(rec.segN, 2u);
    EXPECT_EQ(rec.segs[1].id, TacticId::RogueMulti);
    EXPECT_EQ(rec.outcome, Outcome::NoKill);
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

// ---- per-class tactic selection, thresholds and escape conditions (non-priest families) ------------
namespace
{
constexpr Family kClassFamilies[] = {Family::Warlock, Family::Mage,    Family::Hunter, Family::Rogue,      Family::Warrior,
                                     Family::Paladin, Family::Druid, Family::Shaman, Family::DeathKnight};

ClassParams ParamsOf(Family f)
{
    ClassParams p;
    p.healHpPct = kClassTables[static_cast<std::uint32_t>(f)].healHpPct;
    return p;
}

TriggerFacts Facts(std::uint32_t hp, std::uint32_t mana = 100, bool usesMana = true)
{
    TriggerFacts f;
    f.hpPct = hp;
    f.manaPct = mana;
    f.usesMana = usesMana;
    return f;
}
}  // namespace

TEST(TacticalClassPolicy, EveryFamilySelectsItsOwnLadder)
{
    for (Family const f : kClassFamilies)
    {
        SCOPED_TRACE(static_cast<int>(f));
        ClassParams const p = ParamsOf(f);
        EngagementSnapshot s = Single(100, 0);
        EXPECT_EQ(ChooseClass(f, s, p, TacticId::None, 0, false), TacticOf(f, kSlotSingle));
        s.attackers = 2;
        s.load = 200;
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotSingle), 100, false), TacticOf(f, kSlotMulti));
        s.hpPct = 25;
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotMulti), 100, false), TacticOf(f, kSlotEmergency));
        s.attackers = 3;
        s.load = 400;
        s.hpPct = 25;
        s.cds = kCdControlKnown | kCdDefensiveKnown | kCdEscapeKnown | kCdEscape;  // tools spent, escape ready
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotEmergency), 100, false), TacticOf(f, kSlotEscape));
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotEscape), 8000, true), TacticOf(f, kSlotEmergency));
        EXPECT_EQ(FamilyOf(TacticOf(f, kSlotEscape)), f);
        EXPECT_LT(FactorIndexOfSlot(SlotOf(TacticOf(f, kSlotEscape))), 4u);
    }
}

TEST(TacticalClassPolicy, HealThresholdPerClass)
{
    EngagementSnapshot const s = Single(100, 0);
    for (Family const f : kClassFamilies)
    {
        SCOPED_TRACE(static_cast<int>(f));
        ClassParams const p = ParamsOf(f);
        std::uint32_t const h = p.healHpPct;
        EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Heal, TacticOf(f, kSlotSingle), s, p, Facts(h - 1)));
        EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Heal, TacticOf(f, kSlotSingle), s, p, Facts(h)));
        EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Heal, TacticOf(f, kSlotMulti), s, p, Facts(h + 9)));  // +10
        EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Heal, TacticOf(f, kSlotEmergency), s, p, Facts(1)));
    }
    // Drain-tank: a warlock drains at 60 % where a warrior does not heal yet.
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Heal, TacticId::WarlockSingle, s, ParamsOf(Family::Warlock), Facts(60)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Heal, TacticId::WarriorSingle, s, ParamsOf(Family::Warrior), Facts(60)));
    // Priest ids never fire the class triggers (priests run their own strategy).
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Heal, TacticId::PriestWand, s, ClassParams{}, Facts(1)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Single, TacticId::None, s, ClassParams{}, Facts(100)));
}

TEST(TacticalClassPolicy, ControlConditions)
{
    ClassParams const p;
    EngagementSnapshot s = Single(100, kCdControlKnown | kCdControl);
    s.attackers = 2;
    s.melee = 2;
    s.fearableMelee = 2;
    TacticId const multi = TacticId::WarriorMulti;
    // Area control: >= 2 controllable melee and (hp < 60 or >= 3 attackers), no idle adds near.
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Control, multi, s, p, Facts(59)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Control, multi, s, p, Facts(60)));
    s.attackers = 3;
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Control, multi, s, p, Facts(90)));
    s.addsNear = 1;  // a fear would run mobs into the next pack
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Control, multi, s, p, Facts(50)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::ControlAdd, multi, s, p, Facts(50)));
    s.addsNear = 0;
    s.fearableMelee = 1;  // one immune / out of range
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Control, multi, s, p, Facts(50)));
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::ControlAdd, multi, s, p, Facts(90)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::ControlAdd, TacticId::WarriorSingle, s, p, Facts(90)));
    s.cds = kCdControlKnown;  // on cooldown
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::ControlAdd, multi, s, p, Facts(90)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::EmergencyControl, TacticId::WarriorEmergency, s, p, Facts(20)));
    s.cds = kCdControlKnown | kCdControl;
    // Emergency control: any controllable melee, emergency or escape only.
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::EmergencyControl, TacticId::MageEmergency, s, p, Facts(20)));
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::EmergencyControl, TacticId::MageEscape, s, p, Facts(20)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::EmergencyControl, TacticId::MageMulti, s, p, Facts(20)));
    s.fearableMelee = 0;
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::EmergencyControl, TacticId::MageEmergency, s, p, Facts(20)));
}

TEST(TacticalClassPolicy, ResourcePetKiteAndRunnerConditions)
{
    ClassParams const p;  // LowManaPct 25, EmergencyExitHpPct 45
    EngagementSnapshot s = Single(100, 0);
    // Life Tap discipline: mana < 25 and hp >= 60, while fighting only.
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::LifeTap, TacticId::WarlockSingle, s, p, Facts(60, 24)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::LifeTap, TacticId::WarlockSingle, s, p, Facts(59, 24)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::LifeTap, TacticId::WarlockSingle, s, p, Facts(90, 25)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::LifeTap, TacticId::WarlockEmergency, s, p, Facts(90, 5)));
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::LowMana, TacticId::MageSingle, s, p, Facts(90, 24)));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::LowMana, TacticId::WarriorSingle, s, p, Facts(90, 0, false)));

    TriggerFacts f = Facts(90);
    f.petLow = true;
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::PetLow, TacticId::HunterSingle, s, p, f));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::PetLow, TacticId::HunterEscape, s, p, f));
    f = Facts(90);
    f.targetRootedInMelee = true;
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Kite, TacticId::MageSingle, s, p, f));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Kite, TacticId::MageEmergency, s, p, f));
    // Step-out only from a lone target: never in multi, never with a second attacker or idle adds near.
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Kite, TacticId::MageMulti, s, p, f));
    EngagementSnapshot two = s;
    two.attackers = 2;
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Kite, TacticId::MageSingle, two, p, f));
    EngagementSnapshot adds = s;
    adds.addsNear = 1;
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Kite, TacticId::MageSingle, adds, p, f));
    f = Facts(90);
    f.targetFleeing = true;
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::Runner, TacticId::WarriorSingle, s, p, f));
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::Runner, TacticId::WarriorEscape, s, p, f));
    EXPECT_TRUE(ConditionHolds(ClassTacticCondition::MeleeOnMe, TacticId::HunterSingle, s, p, Facts(90)));
    s.melee = 0;
    EXPECT_FALSE(ConditionHolds(ClassTacticCondition::MeleeOnMe, TacticId::HunterSingle, s, p, Facts(90)));
}

TEST(TacticalClassTables, RowsAreWellFormed)
{
    for (Family const f : kClassFamilies)
    {
        ClassTable const& t = kClassTables[static_cast<std::uint32_t>(f)];
        SCOPED_TRACE(std::string(t.key));
        ASSERT_FALSE(t.key.empty());
        EXPECT_FALSE(t.control[0].empty());
        EXPECT_GE(t.healHpPct, 30u);
        EXPECT_LE(t.healHpPct, 80u);
        for (std::string_view const table : t.factors)
        {
            std::size_t const entries = table.empty() ? 0 : std::size_t(std::count(table.begin(), table.end(), ',')) + 1;
            FactorTable const parsed = ParseFactors(table);
            EXPECT_EQ(parsed.size(), entries) << table;  // no malformed entry silently dropped
            for (auto const& [name, permille] : parsed)
                EXPECT_LE(permille, 2000u) << name;
        }
        // Escape factors switch damage off: every escape table has at least one x0 entry.
        bool zero = false;
        for (auto const& [name, permille] : ParseFactors(t.factors[3]))
            zero = zero || permille == 0;
        EXPECT_TRUE(zero);
    }
}

// S20 A/B (soak-s20-tactics-r1) tuning: the classes whose run-away escapes died (mage 10/11, paladin 4/4) or that
// have no escape tool (warrior) never escape; the fighting tactics never switch a class's core damage off, so a
// bot in emergency still kills its mob (S20 paladin emergency had offence x0: 0 kills in 180 s fights).
TEST(TacticalClassTables, S20NoRunAwayAndCoreDamageStaysOn)
{
    for (Family const f : {Family::Warrior, Family::Mage, Family::Paladin})
        for (std::string_view const tool : kClassTables[static_cast<std::uint32_t>(f)].escape)
            EXPECT_TRUE(tool.empty()) << kClassTables[static_cast<std::uint32_t>(f)].key << " escape tool " << tool;
    // Warrior readiness: no Shield Wall (needs a shield; read "ready" on two-handed warriors).
    for (std::string_view const tool : kClassTables[static_cast<std::uint32_t>(Family::Warrior)].defensive)
        EXPECT_NE(tool, "shield wall");

    struct Core
    {
        Family family;
        std::initializer_list<char const*> actions;
    };
    Core const cores[] = {
        {Family::Warrior, {"mortal strike", "execute", "overpower", "bloodthirst", "slam", "whirlwind", "melee"}},
        {Family::Rogue, {"sinister strike", "eviscerate", "backstab", "mutilate", "melee"}},
        {Family::Mage, {"frostbolt", "fireball", "fire blast", "arcane blast", "frostfire bolt", "shoot"}},
        {Family::Paladin, {"crusader strike", "judgement", "hammer of wrath", "divine storm", "melee"}},
    };
    for (Core const& c : cores)
        for (std::uint32_t i = 0; i < 3; ++i)  // single, multi, emergency (escape switches damage off)
        {
            FactorTable const table = ParseFactors(kClassTables[static_cast<std::uint32_t>(c.family)].factors[i]);
            for (char const* action : c.actions)
                EXPECT_GT(FactorOf(table, action), 0u)
                    << kClassTables[static_cast<std::uint32_t>(c.family)].key << " " << kClassSlotKeys[i] << " " << action;
        }
}

// With no escape tool in its table the ladder tops out at emergency: a warrior / mage / paladin snapshot that
// used to pick escape (overwhelmed, losing, tools spent) now stays in emergency and keeps fighting.
TEST(TacticalClassPolicy, S20OverwhelmedWithoutEscapeToolStaysInEmergency)
{
    ClassParams const p;
    EngagementSnapshot s = Single(100, kCdControlKnown | kCdDefensiveKnown);  // no kCdEscape*: table has none
    s.attackers = 3;
    s.melee = 3;
    s.load = 400;
    s.hpPct = 25;
    for (Family const f : {Family::Warrior, Family::Mage, Family::Paladin})
    {
        SCOPED_TRACE(static_cast<int>(f));
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotMulti), 100, false), TacticOf(f, kSlotEmergency));
        EXPECT_EQ(ChooseClass(f, s, p, TacticOf(f, kSlotEmergency), 60000, false), TacticOf(f, kSlotEmergency));
    }
}
