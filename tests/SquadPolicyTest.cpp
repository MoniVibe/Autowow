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

TEST(Squad, LegacyProvenanceExpansionIsAtomicAndBounded)
{
    std::vector<std::uint32_t> out = {99};
    EXPECT_FALSE(ExpandGuidSpansBounded({GuidSpan{1, 11}}, kMaxMembers, out));
    EXPECT_EQ(out, (std::vector<std::uint32_t>{99}));
    EXPECT_FALSE(ExpandGuidSpansBounded({GuidSpan{1, 6}, GuidSpan{20, 24}}, kMaxMembers, out));
    EXPECT_EQ(out, (std::vector<std::uint32_t>{99}));
    EXPECT_FALSE(ExpandGuidSpansBounded({GuidSpan{4, 3}}, kMaxMembers, out));
    EXPECT_EQ(out, (std::vector<std::uint32_t>{99}));
    EXPECT_TRUE(ExpandGuidSpansBounded({GuidSpan{5, 7}, GuidSpan{1, 2}}, kMaxMembers, out));
    EXPECT_EQ(out, (std::vector<std::uint32_t>{1, 2, 5, 6, 7}));
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

TEST(Squad, RoughStoneDemandUsesMiningNodeContract)
{
    ASSERT_GT(kMaterialCount, 0u);
    EXPECT_EQ(kMaterials[kMaterialCount - 1].item, 2835u);  // append-only: preserve existing material tie order
    EXPECT_EQ(kMaterials[kMaterialCount - 1].kind, Kind::Ore);

    std::vector<Want> const ranked = RankDemand({{2835, 6}, {2835, 10}, {2592, 5, true}}, 5);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].item, 2592u);  // current-tier cloth priority remains unchanged
    EXPECT_TRUE(ranked[0].first);
    EXPECT_EQ(ranked[1].item, 2835u);
    EXPECT_EQ(ranked[1].count, 10u);  // duplicate needs keep the larger count
    EXPECT_EQ(ranked[1].kind, Kind::Ore);
    EXPECT_TRUE(RankDemand({{2835, 4}}, 5).empty());

    std::vector<Spawn> const copperNodes = {Src(1, 1731, 0, 0, 1, 1)};
    auto lock = [](std::uint32_t entry) { return entry == 1731 ? 65u : ~0u; };
    Skills skills;  // runtime publishes Mining here only when a squad member also owns an admitted pick
    EXPECT_TRUE(Workable(copperNodes, Kind::Ore, skills, lock).empty());
    skills.mining = 64;
    EXPECT_TRUE(Workable(copperNodes, Kind::Ore, skills, lock).empty());
    skills.mining = 65;
    ASSERT_EQ(Workable(copperNodes, Kind::Ore, skills, lock).size(), 1u);
    EXPECT_EQ(Workable(copperNodes, Kind::Ore, skills, lock).front().entry, 1731u);
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

TEST(Squad, MaterialCrewLayoutIsAtomicExclusiveAndDeterministic)
{
    std::array<std::vector<std::uint32_t>, kCrewCount> configured;
    configured[CrewIndex(0, Kind::Ore)] = {30, 10, 20};
    configured[CrewIndex(0, Kind::Herb)] = {50, 40};
    configured[CrewIndex(1, Kind::Ore)] = {80, 60, 70};
    configured[CrewIndex(1, Kind::Herb)] = {100, 90};
    CrewLayout const layout = BuildCrewLayout(configured);
    ASSERT_TRUE(layout.Valid());
    EXPECT_EQ(layout.crews[CrewIndex(0, Kind::Ore)], (std::vector<std::uint32_t>{10, 20, 30}));
    EXPECT_EQ(layout.crews[CrewIndex(1, Kind::Leather)], (std::vector<std::uint32_t>{}));
    EXPECT_EQ(layout.teams[0], (std::vector<std::uint32_t>{10, 20, 30, 40, 50}));
    EXPECT_EQ(layout.teams[1], (std::vector<std::uint32_t>{60, 70, 80, 90, 100}));
    EXPECT_EQ(CrewTeam(CrewIndex(1, Kind::Herb)), 1u);
    EXPECT_EQ(CrewKind(CrewIndex(1, Kind::Herb)), Kind::Herb);

    configured[CrewIndex(1, Kind::Cloth)] = {10};  // cross-crew duplicate: reject everything
    CrewLayout const duplicate = BuildCrewLayout(configured);
    EXPECT_EQ(duplicate.error, CrewLayoutError::DuplicateGuid);
    for (auto const& crew : duplicate.crews)
        EXPECT_TRUE(crew.empty());
    for (auto const& team : duplicate.teams)
        EXPECT_TRUE(team.empty());

    configured = {};
    configured[CrewIndex(0, Kind::Ore)] = {10, 10};  // within-crew duplicate is equally invalid
    EXPECT_EQ(BuildCrewLayout(configured).error, CrewLayoutError::DuplicateGuid);
}

TEST(Squad, MaterialCrewLayoutRejectsMalformedAndOverCap)
{
    std::array<std::vector<std::uint32_t>, kCrewCount> configured;
    configured[CrewIndex(0, Kind::Ore)] = {0};
    EXPECT_EQ(BuildCrewLayout(configured).error, CrewLayoutError::ZeroGuid);

    configured = {};
    for (std::uint32_t guid = 1; guid <= kMaxCrewMembers + 1; ++guid)
        configured[CrewIndex(0, Kind::Ore)].push_back(guid);
    EXPECT_EQ(BuildCrewLayout(configured).error, CrewLayoutError::CrewOverCap);

    configured = {};
    for (std::uint32_t guid = 1; guid <= 5; ++guid)
        configured[CrewIndex(0, Kind::Ore)].push_back(guid);
    for (std::uint32_t guid = 6; guid <= 10; ++guid)
        configured[CrewIndex(0, Kind::Herb)].push_back(guid);
    configured[CrewIndex(0, Kind::Cloth)] = {11};
    EXPECT_EQ(BuildCrewLayout(configured).error, CrewLayoutError::TeamOverCap);
}

TEST(Squad, MaterialCrewDemandIsKindScopedWhileLegacyDemandIsUnchanged)
{
    std::vector<AutoWowSupply::MaterialNeed> const needs = {{2770, 30}, {2835, 20}, {2447, 40}, {2589, 50}};
    std::vector<Want> const legacy = RankDemand(needs, 5);
    ASSERT_EQ(legacy.size(), 4u);
    EXPECT_EQ(legacy.front().item, 2589u);
    std::vector<Want> const ore = RankDemandForKind(needs, 5, Kind::Ore);
    ASSERT_EQ(ore.size(), 2u);
    EXPECT_EQ(ore[0].item, 2770u);
    EXPECT_EQ(ore[1].item, 2835u);
    std::vector<Want> const herb = RankDemandForKind(needs, 5, Kind::Herb);
    ASSERT_EQ(herb.size(), 1u);
    EXPECT_EQ(herb[0].item, 2447u);
    EXPECT_TRUE(RankDemandForKind(needs, 5, Kind::Leather).empty());
}

TEST(Squad, MaterialCrewStatesAndCooldownsAreIndependent)
{
    std::array<TeamState, kCrewCount> states;
    std::size_t const ore = CrewIndex(0, Kind::Ore);
    std::size_t const herb = CrewIndex(0, Kind::Herb);
    Cluster cluster;
    cluster.center = 7;
    cluster.x = 1000;
    cluster.y = 1000;
    Issue(states[ore], Want{2770, 20, Kind::Ore}, cluster, 0, 10, 1, 0, 1000);
    Issue(states[herb], Want{2447, 10, Kind::Herb}, cluster, 0, 40, 2, 0, 1000);
    TeamState const herbBefore = states[herb];
    NoteHeld(states[ore], 5);
    Finish(states[ore], Params{}, 5000);
    EXPECT_EQ(states[ore].phase, Phase::None);
    EXPECT_TRUE(AnchorCooling(Params{}, states[ore], 0, 1000, 1000, 5001));
    EXPECT_EQ(states[herb].phase, herbBefore.phase);
    EXPECT_EQ(states[herb].id, herbBefore.id);
    EXPECT_EQ(states[herb].item, herbBefore.item);
    EXPECT_EQ(states[herb].gathered, herbBefore.gathered);
    EXPECT_EQ(states[herb].cooldownUntilMs, herbBefore.cooldownUntilMs);
    EXPECT_TRUE(Holds(states[herb], false));
}

TEST(Squad, StintIdsNeverReuseOrWrap)
{
    std::uint32_t last = 0, first = 0, second = 0;
    EXPECT_TRUE(NextStintId(last, first));
    EXPECT_TRUE(NextStintId(last, second));
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    EXPECT_NE(first, second);
    last = std::numeric_limits<std::uint32_t>::max() - 1;
    EXPECT_TRUE(NextStintId(last, first));
    EXPECT_EQ(first, std::numeric_limits<std::uint32_t>::max());
    EXPECT_FALSE(NextStintId(last, second));
    EXPECT_EQ(second, 0u);
    EXPECT_EQ(last, std::numeric_limits<std::uint32_t>::max());
}

TEST(Squad, MaterialStintDeferralOnlySchedulesTheExistingRetry)
{
    Params p;
    TeamState state;
    state.id = 19;
    state.item = 2835;
    state.kind = Kind::Ore;
    state.target = 40;
    state.gathered = 21;
    state.lastHeld = 31;
    state.leader = 924056;
    state.map = 1;
    state.x = -500;
    state.y = 700;
    state.z = 90;
    state.anchorSpawn = 1234;
    state.entries[0] = 1731;
    state.entryCount = 1;
    state.startMs = 3000;
    state.nextSearchMs = 4000;
    state.together = false;
    state.holding = true;
    state.cooldownMap = 1;
    state.cooldownX = -550;
    state.cooldownY = 750;
    state.cooldownUntilMs = 9000;
    state.anchorZone = 11;
    state.cooldownZone = 12;
    state.zoneCooldownUntilMs = 10000;
    TeamState const before = state;

    ScheduleMaterialStintSearchRetry(state, p, 5000);

    EXPECT_EQ(state.nextSearchMs, 5000u + p.searchRetryMs);
    EXPECT_EQ(state.version, before.version);
    EXPECT_EQ(state.phase, before.phase);
    EXPECT_EQ(state.id, before.id);
    EXPECT_EQ(state.item, before.item);
    EXPECT_EQ(state.kind, before.kind);
    EXPECT_EQ(state.target, before.target);
    EXPECT_EQ(state.gathered, before.gathered);
    EXPECT_EQ(state.lastHeld, before.lastHeld);
    EXPECT_EQ(state.leader, before.leader);
    EXPECT_EQ(state.map, before.map);
    EXPECT_EQ(state.x, before.x);
    EXPECT_EQ(state.y, before.y);
    EXPECT_EQ(state.z, before.z);
    EXPECT_EQ(state.anchorSpawn, before.anchorSpawn);
    EXPECT_EQ(state.entries, before.entries);
    EXPECT_EQ(state.entryCount, before.entryCount);
    EXPECT_EQ(state.startMs, before.startMs);
    EXPECT_EQ(state.together, before.together);
    EXPECT_EQ(state.holding, before.holding);
    EXPECT_EQ(state.cooldownMap, before.cooldownMap);
    EXPECT_EQ(state.cooldownX, before.cooldownX);
    EXPECT_EQ(state.cooldownY, before.cooldownY);
    EXPECT_EQ(state.cooldownUntilMs, before.cooldownUntilMs);
    EXPECT_EQ(state.anchorZone, before.anchorZone);
    EXPECT_EQ(state.cooldownZone, before.cooldownZone);
    EXPECT_EQ(state.zoneCooldownUntilMs, before.zoneCooldownUntilMs);
}

TEST(SquadPolicy, PoolWeightCountsLiveNodes)
{
    // S107b anchors: Durotar Skull Rock 3 of 12, Bramblescar 3 of 10, Mulgore Bottom 8 of 25.
    EXPECT_EQ(PoolLivePermille(3, 12), 250u);
    EXPECT_EQ(PoolLivePermille(3, 10), 300u);
    EXPECT_EQ(PoolLivePermille(8, 25), 320u);
    EXPECT_EQ(PoolLivePermille(0, 25), 1000u);  // max_limit 0: every member spawns
    EXPECT_EQ(PoolLivePermille(5, 0), 1000u);
    EXPECT_EQ(PoolLivePermille(30, 25), 1000u);
    // A 9-spawn-point cluster of a 3 / 12 pool is ~2 live veins: not worth a stint at MinClusterSpawns 5.
    EXPECT_FALSE(EnoughLiveNodes(9 * 250, 5));
    EXPECT_TRUE(EnoughLiveNodes(5 * 1000, 5));
    EXPECT_TRUE(EnoughLiveNodes(20 * 250, 5));
    EXPECT_TRUE(EnoughLiveNodes(1000, 0));  // MinClusterSpawns is at least 1
    EXPECT_FALSE(EnoughLiveNodes(999, 0));
    EXPECT_FALSE(Params{}.poolWeight);  // opt-in
}
}  // namespace
