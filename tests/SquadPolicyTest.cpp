/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SquadPolicy.h"

#include "AutoWowQuestLedger.h"
#include "PartyPolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowSquad;

Spawn Src(std::uint32_t id, std::uint32_t entry, std::int32_t x, std::int32_t y, std::uint8_t lo, std::uint8_t hi,
          std::uint8_t teams = AutoWowContracts::kAlliance | AutoWowContracts::kHorde)
{
    Spawn s;
    s.spawnId = id;
    s.entry = entry;
    s.x = x;
    s.y = y;
    s.minLevel = lo;
    s.maxLevel = hi;
    s.teams = teams;
    return s;
}

void Pack(std::vector<Spawn>& out, std::uint32_t firstId, std::uint32_t entry, std::int32_t x, std::int32_t y,
          std::uint32_t n, std::uint8_t lo, std::uint8_t hi)
{
    for (std::uint32_t k = 0; k < n; ++k)
        out.push_back(Src(firstId + k, entry, x + std::int32_t(k) * 5, y, lo, hi));
}

TeamState Running(Kind kind = Kind::Cloth)
{
    TeamState s;
    Want const w{2589, 40, kind};
    Cluster c;
    c.center = 7;
    c.x = 1000;
    c.y = 1000;
    c.entries = {111, 222};
    Issue(s, w, c, 0, 50, 1, 10, 1000);
    return s;
}

TEST(Squad, DemandRanksLargestFirstTiesByTierAndDropsSmallOrUnknown)
{
    std::vector<AutoWowSupply::MaterialNeed> const needs = {
        {2592, 40}, {2589, 40}, {2770, 90}, {99999, 500}, {765, 3}, {2447, 0}, {2450, 60}, {2450, 80}};
    std::vector<Want> const r = RankDemand(needs, 5);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].item, 2770u);  // ore 90
    EXPECT_EQ(r[0].kind, Kind::Ore);
    EXPECT_EQ(r[1].item, 2450u);  // listed twice: the larger count
    EXPECT_EQ(r[1].count, 80u);
    EXPECT_EQ(r[2].item, 2589u);  // 40 each: linen before wool (table order)
    EXPECT_EQ(r[3].item, 2592u);
    EXPECT_TRUE(Demanded(r, 2592));
    EXPECT_FALSE(Demanded(r, 765));  // below MinDemand
    EXPECT_TRUE(RankDemand({}, 5).empty());
    // The bag artisan's current tier (first) outranks bigger needs (soak-s47-full-r1: wool for a Woolen Bag tailor).
    std::vector<Want> const f = RankDemand({{2770, 90}, {2592, 40, true}, {2592, 60}}, 5);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0].item, 2592u);
    EXPECT_EQ(f[0].count, 60u);
    EXPECT_TRUE(f[0].first);
    EXPECT_EQ(f[1].item, 2770u);
}

TEST(Squad, SkinningRequirementAndWorkability)
{
    EXPECT_EQ(SkinReq(5), 0u);
    EXPECT_EQ(SkinReq(15), 50u);
    EXPECT_EQ(SkinReq(25), 125u);
    Skills s;
    EXPECT_TRUE(CanWork(Kind::Cloth, s, 0));
    EXPECT_FALSE(CanWork(Kind::Herb, s, 0));      // nobody has herbalism
    EXPECT_FALSE(CanWork(Kind::Leather, s, 0));   // nobody skins (or no knife)
    s.herbalism = 70;
    s.mining = 60;
    s.skinning = 50;
    EXPECT_TRUE(CanWork(Kind::Herb, s, 70));      // briarthorn lock 70
    EXPECT_FALSE(CanWork(Kind::Ore, s, 65));      // tin vein 65 > mining 60
    EXPECT_TRUE(CanWork(Kind::Leather, s, SkinReq(15)));
    EXPECT_FALSE(CanWork(Kind::Leather, s, SkinReq(16)));
    {
        Skills any = s;  // AutoWow.Gather.AnySkill: a learned skill works any source
        any.anySkill = true;
        EXPECT_TRUE(CanWork(Kind::Ore, any, 65));
        EXPECT_TRUE(CanWork(Kind::Leather, any, SkinReq(60)));
        any.herbalism = 0;
        EXPECT_FALSE(CanWork(Kind::Herb, any, 1));  // still needs the skill
    }

    std::vector<Spawn> beasts = {Src(1, 10, 0, 0, 14, 15), Src(2, 11, 0, 0, 16, 17)};
    auto noNode = [](std::uint32_t) { return ~0u; };
    std::vector<Spawn> const w = Workable(beasts, Kind::Leather, s, noNode);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].spawnId, 1u);
    std::vector<Spawn> nodes = {Src(3, 1731, 0, 0, 1, 1), Src(4, 1732, 0, 0, 1, 1)};
    std::vector<Spawn> const ore =
        Workable(nodes, Kind::Ore, s, [](std::uint32_t e) { return e == 1731 ? 1u : 65u; });
    ASSERT_EQ(ore.size(), 1u);
    EXPECT_EQ(ore[0].entry, 1731u);
}

TEST(Squad, LevelSafeNearestSource)
{
    Params p;  // above 1, below 255, cluster 120, min 5
    AutoWowContracts::Params const cp = ClusterParams(p);
    std::vector<Spawn> spawns;
    Pack(spawns, 100, 500, 400, 0, 6, 8, 10);    // near, L8-10
    Pack(spawns, 200, 501, 1200, 0, 12, 3, 6);   // farther, bigger, grey-ish L3-6
    Pack(spawns, 300, 502, 100, 0, 8, 11, 13);   // nearest but L13 > avg 10 + 1
    std::uint32_t const avg = AvgLevel({9, 10, 11});
    ASSERT_EQ(avg, 10u);
    std::vector<Cluster> const ranked = RankNearest(AutoWowContracts::RankClusters(
        cp, AutoWowContracts::Candidates(cp, spawns, AutoWowContracts::kAlliance, avg, 0, 0), 0, 0));
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked.front().entries.front(), 500u);  // the level-unsafe pack never shows; nearest beats bigger
    for (Cluster const& c : ranked)
        EXPECT_NE(c.entries.front(), 502u);
    // Nodes carry level 1..1: any squad level may work them.
    std::vector<Spawn> nodes;
    Pack(nodes, 400, 1731, 50, 0, 5, 1, 1);
    EXPECT_EQ(AutoWowContracts::Candidates(cp, nodes, AutoWowContracts::kHorde, 40, 0, 0).size(), 5u);
}

TEST(Squad, ProgressCountsOnlyRisesAndEndCauseOrder)
{
    Params p;
    TeamState s = Running();
    EXPECT_EQ(s.phase, Phase::Stint);
    EXPECT_EQ(s.target, 40u);
    NoteHeld(s, 25);  // +15
    NoteHeld(s, 5);   // a delivery: no progress lost
    NoteHeld(s, 20);  // +15
    EXPECT_EQ(s.gathered, 30u);
    EXPECT_STREQ(EndCause(p, s, 2000, true, true, false, false), "");
    EXPECT_STREQ(EndCause(p, s, 2000, false, true, false, false), "demand_met");
    EXPECT_STREQ(EndCause(p, s, 2000, true, false, true, false), "empty");
    EXPECT_STREQ(EndCause(p, s, 2000, true, true, true, false), "danger");
    EXPECT_STREQ(EndCause(p, s, 2000, true, true, false, true), "stuck");
    EXPECT_STREQ(EndCause(p, s, 1000 + p.stintMs, true, true, false, false), "timeout");
    NoteHeld(s, 30);
    EXPECT_STREQ(EndCause(p, s, 1000 + p.stintMs, false, false, true, true), "done");  // done beats everything
    TeamState none;
    EXPECT_STREQ(EndCause(p, none, 0, false, false, true, true), "");
}

TEST(Squad, FinishCoolsTheAnchorAndKeepsIdAndHold)
{
    Params p;
    TeamState s = Running();
    Finish(s, p, 5000);
    EXPECT_EQ(s.phase, Phase::None);
    EXPECT_EQ(s.id, 1u);
    EXPECT_TRUE(s.holding);
    EXPECT_EQ(s.entryCount, 0u);
    EXPECT_EQ(s.nextSearchMs, 5000u);
    EXPECT_TRUE(AnchorCooling(p, s, 0, 1050, 1000, 6000));
    EXPECT_FALSE(AnchorCooling(p, s, 0, 1500, 1000, 6000));          // another pack
    EXPECT_FALSE(AnchorCooling(p, s, 1, 1050, 1000, 6000));          // another map
    EXPECT_FALSE(AnchorCooling(p, s, 0, 1050, 1000, 5000 + p.cooldownMs));
}

TEST(Squad, HoldAndRelease)
{
    TeamState s;
    EXPECT_FALSE(Holds(s, false));  // no workable demand: the members quest and graduate
    s = Running();
    EXPECT_TRUE(Holds(s, false));
    EXPECT_FALSE(Holds(s, true));   // too dangerous here: graduation / escape wins
}

TEST(Squad, MemberStepTravelsTogetherAndLeashes)
{
    Params p;  // leash 150, together 60
    TeamState s = Running();
    s.together = false;
    EXPECT_EQ(MemberStep(p, s, true, false, 0, 0, 0), Step::Wait);     // the leader waits for stragglers
    EXPECT_EQ(MemberStep(p, s, false, false, 0, 0, 0), Step::Travel);  // members walk on
    s.together = true;
    EXPECT_EQ(MemberStep(p, s, true, false, 0, 0, 0), Step::Travel);
    EXPECT_EQ(MemberStep(p, s, false, false, 0, 1020, 1000), Step::Hunt);   // arrived (30 yd)
    EXPECT_EQ(MemberStep(p, s, false, false, 0, 1100, 1000), Step::Travel); // not yet arrived
    EXPECT_EQ(MemberStep(p, s, false, true, 0, 1100, 1000), Step::Hunt);    // hunting: leash 150
    EXPECT_EQ(MemberStep(p, s, false, true, 0, 1200, 1000), Step::Travel);  // drifted past the leash
    EXPECT_EQ(MemberStep(p, s, false, true, 1, 1000, 1000), Step::Idle);    // another map
    EXPECT_EQ(MemberStep(p, TeamState{}, true, false, 0, 0, 0), Step::Idle);
    EXPECT_TRUE(Together(p, 0, 0, 1000, 0, {{{10, 10}}, {{-50, 20}}}));
    EXPECT_FALSE(Together(p, 0, 0, 1000, 0, {{{10, 10}}, {{-61, 0}}}));  // a straggler behind
    EXPECT_TRUE(Together(p, 0, 0, 1000, 0, {{{10, 10}}, {{400, 0}}}));   // ahead toward the anchor: not waited for
}

TEST(Squad, HuntTargetFiltersToSourceWithinLeash)
{
    Params p;
    TeamState s = Running(Kind::Cloth);
    EXPECT_TRUE(HuntTarget(p, s, 111, 0, 1100, 1000));
    EXPECT_FALSE(HuntTarget(p, s, 333, 0, 1100, 1000));   // not a source entry
    EXPECT_FALSE(HuntTarget(p, s, 111, 0, 1200, 1000));   // outside the leash
    EXPECT_FALSE(HuntTarget(p, s, 111, 1, 1100, 1000));   // other map
    TeamState n = Running(Kind::Herb);
    EXPECT_TRUE(HuntTarget(p, n, 333, 0, 1100, 1000));    // node stint: anything within the leash
    EXPECT_FALSE(HuntTarget(p, TeamState{}, 111, 0, 1000, 1000));
}

TEST(Squad, LedgerSchema)
{
    TeamState s = Running();
    std::string const f = LedgerFields(s, Kind::Cloth, 2589, 12, 4000, "done");
    EXPECT_EQ(f, ",\"sid\":1,\"kind\":\"cloth\",\"item\":2589,\"count\":12,\"target\":40,\"gathered\":0,\"amap\":0,"
                 "\"ax\":1000,\"ay\":1000,\"anchor\":7,\"dur_ms\":3000,\"cause\":\"done\"");
    EXPECT_STREQ(ReasonName(Reason::Stint), "stint");
    EXPECT_STREQ(ReasonName(Reason::Gather), "gather");
    EXPECT_STREQ(ReasonName(Reason::Deliver), "deliver");
    EXPECT_STREQ(ReasonName(Reason::Hold), "hold");
    EXPECT_STREQ(ReasonName(Reason::Release), "release");
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::Squad), 22);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(AutoWowQuestLedger::Event::Squad), "squad");
    EXPECT_STREQ(AutoWowParty::ReasonName(AutoWowParty::Reason::Squad), "squad");
    EXPECT_EQ(static_cast<int>(AutoWowParty::Reason::Squad), 3);
}

TEST(Squad, LevelWindowOffKeepsZoneLevelMargin)
{
    Params p;
    EXPECT_FALSE(LevelWindowOn(p));
    EXPECT_EQ(ZoneMargin(p), p.zoneLevelMargin);
    EXPECT_STREQ(BenchCause(p, true, 99), "");  // off: nobody benched
    TeamState s = Running();
    EXPECT_FALSE(AnchorDeathCluster(p, s, 0, 1000, 1000, 99));
    TeamState const v = FilterView(p, s, 1, true);  // off: other map / benched still filtered
    EXPECT_EQ(v.phase, Phase::Stint);
}

TEST(Squad, LevelWindowZoneBracket)
{
    Params p;
    p.levelWindow = 2;
    EXPECT_EQ(ZoneMargin(p), 2u);
    // soak-s62: squad average 23, Alterac Mountains (bracket 30) out; Hillsbrad (20) and a 25 zone in; unknown in.
    EXPECT_TRUE(ZoneTooHigh(30, 23, ZoneMargin(p)));
    EXPECT_FALSE(ZoneTooHigh(25, 23, ZoneMargin(p)));
    EXPECT_TRUE(ZoneTooHigh(26, 23, ZoneMargin(p)));
    EXPECT_FALSE(ZoneTooHigh(20, 23, ZoneMargin(p)));
    EXPECT_FALSE(ZoneTooHigh(0, 23, ZoneMargin(p)));
}

TEST(Squad, LevelWindowBench)
{
    Params p;
    p.levelWindow = 2;
    EXPECT_STREQ(BenchCause(p, false, 0), "");
    EXPECT_STREQ(BenchCause(p, false, 2), "");
    EXPECT_STREQ(BenchCause(p, false, 3), "deaths");
    EXPECT_STREQ(BenchCause(p, true, 0), "route");
    EXPECT_STREQ(BenchCause(p, true, 5), "route");  // route beats deaths
    p.deathCluster = 0;                              // death rule off
    EXPECT_STREQ(BenchCause(p, false, 50), "");
    EXPECT_STREQ(ReasonName(Reason::Bench), "bench");
    EXPECT_EQ(static_cast<int>(Reason::Bench), 5);
}

TEST(Squad, LevelWindowFilterView)
{
    Params p;
    p.levelWindow = 2;
    TeamState s = Running();  // anchor map 0
    EXPECT_EQ(FilterView(p, s, 0, false).phase, Phase::Stint);  // taking part: filtered
    EXPECT_EQ(FilterView(p, s, 1, false).phase, Phase::None);   // another continent: its own targets
    EXPECT_EQ(FilterView(p, s, 0, true).phase, Phase::None);    // benched
    EXPECT_EQ(FilterView(p, s, 1, false).id, s.id);             // the rest of the state stays
}

TEST(Squad, LevelWindowAnchorDeathClusterCoolsTheZone)
{
    Params p;
    p.levelWindow = 2;
    TeamState s = Running();  // anchor (1000, 1000) map 0
    EXPECT_TRUE(AnchorDeathCluster(p, s, 0, 1100, 1000, 3));
    EXPECT_FALSE(AnchorDeathCluster(p, s, 0, 1100, 1000, 2));  // fewer than DeathCluster
    EXPECT_FALSE(AnchorDeathCluster(p, s, 0, 1200, 1000, 9));  // outside the leash: a walk death, not the anchor
    EXPECT_FALSE(AnchorDeathCluster(p, s, 1, 1100, 1000, 9));  // other map
    EXPECT_FALSE(AnchorDeathCluster(p, TeamState{}, 0, 1000, 1000, 9));
    s.anchorZone = 11;
    CoolZone(s, p, s.anchorZone, 5000);
    Finish(s, p, 5000);  // the zone cooldown outlives the stint
    EXPECT_TRUE(ZoneCooling(s, 11, 5000));
    EXPECT_TRUE(ZoneCooling(s, 11, 5000 + p.dangerZoneMs - 1));
    EXPECT_FALSE(ZoneCooling(s, 11, 5000 + p.dangerZoneMs));
    EXPECT_FALSE(ZoneCooling(s, 12, 5000));
    EXPECT_FALSE(ZoneCooling(s, 0, 5000));
    EXPECT_FALSE(ZoneCooling(TeamState{}, 0, 0));
    EXPECT_EQ(TeamState{}.version, 2);
}

TEST(Squad, LevelWindowBenchHoldsAndMajorityStops)
{
    Params p;
    EXPECT_FALSE(TooManyBenched(p, 5, 5));  // window off: never
    p.levelWindow = 2;
    std::uint64_t const until = BenchUntil(p, 1000);
    EXPECT_EQ(until, 1000u + p.benchMs);
    EXPECT_TRUE(BenchActive(until, 1000));
    EXPECT_TRUE(BenchActive(until, until - 1));  // held: no re-evaluation until it lapses
    EXPECT_FALSE(BenchActive(until, until));
    EXPECT_FALSE(BenchActive(0, 0));            // never benched
    EXPECT_FALSE(TooManyBenched(p, 2, 5));
    EXPECT_TRUE(TooManyBenched(p, 3, 5));       // soak-s64: more than half out -> no stint
    EXPECT_FALSE(TooManyBenched(p, 2, 4));      // exactly half: the stint goes on
    EXPECT_TRUE(TooManyBenched(p, 1, 1));
    EXPECT_FALSE(TooManyBenched(p, 0, 0));
}

TEST(Squad, SupplyUsableNowFollowsArtisanSkill)
{
    AutoWowSupply::ProductLine const& potions = AutoWowSupply::LineOf(AutoWowSupply::Line::Potions);
    EXPECT_TRUE(AutoWowSupply::UsableNow(potions, 2447, 0));    // peacebloom: learned with the skill
    EXPECT_FALSE(AutoWowSupply::UsableNow(potions, 2450, 54));  // briarthorn: alchemy 55
    EXPECT_TRUE(AutoWowSupply::UsableNow(potions, 2450, 55));
    EXPECT_FALSE(AutoWowSupply::UsableNow(potions, 2453, 109)); // bruiseweed: alchemy 110
}
}  // namespace
