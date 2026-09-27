/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonProbePolicy.h"

#include "AutoWowQuestLedger.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowDungeonProbe;

TEST(DungeonProbePolicyTest, ParsesPartiesAndRefusesMalformedOrOverlapping)
{
    std::vector<PartyDef> const parties = ParseParties(
        "ally:105,101,102,103,104; horde : 201,202 ;bad name:1;six:1,2,3,4,5,6;zero:0,7;dup:8,8;"
        "ally:9;steal:101,300;nocolon;empty:");
    ASSERT_EQ(parties.size(), 2u);
    EXPECT_EQ(parties[0].name, "ally");
    EXPECT_EQ(parties[0].guids, (std::vector<std::uint32_t>{101, 102, 103, 104, 105}));
    EXPECT_EQ(parties[1].name, "horde");
    EXPECT_EQ(parties[1].guids, (std::vector<std::uint32_t>{201, 202}));
    EXPECT_TRUE(ParseParties("").empty());
}

TEST(DungeonProbePolicyTest, QueueKeepsOrderAndFirstOccurrence)
{
    EXPECT_EQ(ParseQueue("36, 43,389,x,0,43,47"), (std::vector<std::uint32_t>{36, 43, 389, 47}));
    EXPECT_TRUE(ParseQueue("").empty());
}

TEST(DungeonProbePolicyTest, LevelBandIsRecommendedMaxPlusOverCapped)
{
    std::vector<LevelOverride> const overrides = ParseLevels("36:20,574:72,bad,43:0,36:21");
    ASSERT_EQ(overrides.size(), 2u);
    EXPECT_EQ(BandLevel(389, 2, {}, 80), 20u);         // Ragefire 18 + 2
    EXPECT_EQ(BandLevel(36, 2, {}, 80), 28u);          // Deadmines 26 + 2
    EXPECT_EQ(BandLevel(36, 2, overrides, 80), 23u);   // override (last one wins) 21 + 2
    EXPECT_EQ(BandLevel(574, 2, overrides, 80), 74u);  // extension: Utgarde Keep
    EXPECT_EQ(BandLevel(329, 2, {}, 60), 60u);         // capped at max player level
    EXPECT_EQ(BandLevel(9999, 2, {}, 80), 0u);         // unknown map
    for (std::uint32_t map : ParseQueue("36,43,389,47,48,90,129,70,109,209,230,329,349,429"))
        EXPECT_GT(BandLevel(map, 2, {}, 80), 0u) << map;  // every default dungeon has a band
}

TEST(DungeonProbePolicyTest, FixedSpecsAndQualityLadder)
{
    EXPECT_EQ(SpecFor(1), 2);   // warrior prot pve
    EXPECT_EQ(SpecFor(5), 1);   // priest holy pve
    EXPECT_EQ(SpecFor(8), 2);   // mage frost pve
    EXPECT_EQ(SpecFor(4), 1);   // rogue combat pve
    EXPECT_EQ(SpecFor(3), 0);   // hunter bm pve
    EXPECT_EQ(SpecFor(11), -1);  // others: stored spec
    EXPECT_EQ(QualityLadder(4), (std::vector<std::uint32_t>{4, 3, 2}));
    EXPECT_EQ(QualityLadder(9), (std::vector<std::uint32_t>{5, 4, 3, 2}));
    EXPECT_EQ(QualityLadder(2), (std::vector<std::uint32_t>{2}));
    EXPECT_TRUE(QualityLadder(1).empty());
}

TEST(DungeonProbePolicyTest, StuckTrackerFiresOnceperWindowWithoutProgress)
{
    StuckTracker t;
    EXPECT_FALSE(NoteProgress(t, 0, 0, 0, false, 1000, 10, 300000));  // arms
    EXPECT_FALSE(NoteProgress(t, 5, 5, 0, false, 200000, 10, 300000));  // 7 yd: no progress, not yet stuck
    EXPECT_TRUE(NoteProgress(t, 5, 5, 0, false, 301000, 10, 300000));   // stuck, re-anchored in time
    EXPECT_FALSE(NoteProgress(t, 5, 5, 0, false, 400000, 10, 300000));
    EXPECT_TRUE(NoteProgress(t, 5, 5, 0, false, 601000, 10, 300000));
    EXPECT_FALSE(NoteProgress(t, 5, 5, 0, true, 950000, 10, 300000));   // combat / boss = progress
    EXPECT_FALSE(NoteProgress(t, 5, 5, 11, false, 1300000, 10, 300000));  // moved 11 yd (vertical counts)
    EXPECT_EQ(t.sinceMs, 1300000u);
}

TEST(DungeonProbePolicyTest, InsideDecisionOrder)
{
    InsideParams const p;  // maxWipes 3, stuckLimit 2, 1 h, revive grace 30 s
    InsideFacts f;
    f.size = f.online = f.alive = 5;
    f.allMask = 0b111;
    f.nowMs = 10000;
    f.enteredMs = 0;
    End end = End::None;
    EXPECT_EQ(DecideInside(f, p, end), Verdict::Continue);

    InsideFacts offline = f;
    offline.online = 4;
    offline.mask = 0b111;
    EXPECT_EQ(DecideInside(offline, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::Abandoned);  // offline before completed

    InsideFacts left = f;
    left.leaderOnDungeonMap = false;
    EXPECT_EQ(DecideInside(left, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::Abandoned);

    InsideFacts done = f;
    done.mask = 0b111;
    done.alive = 0;
    EXPECT_EQ(DecideInside(done, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::Completed);  // the last boss can take the party with it

    InsideFacts wipe = f;
    wipe.alive = 0;
    wipe.wipes = 2;
    EXPECT_EQ(DecideInside(wipe, p, end), Verdict::Wiped);
    wipe.wipes = 3;
    EXPECT_EQ(DecideInside(wipe, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::WipedOut);

    InsideFacts late = f;
    late.nowMs = 3600000;
    late.stucks = 5;
    EXPECT_EQ(DecideInside(late, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::Timeout);

    InsideFacts stuck = f;
    stuck.stucks = 1;
    EXPECT_EQ(DecideInside(stuck, p, end), Verdict::Continue);
    stuck.stucks = 2;
    EXPECT_EQ(DecideInside(stuck, p, end), Verdict::Finish);
    EXPECT_EQ(end, End::Stuck);

    InsideFacts dead = f;
    dead.alive = 3;
    dead.quietDeadSinceMs = 1;
    dead.nowMs = 30000;
    EXPECT_EQ(DecideInside(dead, p, end), Verdict::Continue);
    dead.nowMs = 30001;
    EXPECT_EQ(DecideInside(dead, p, end), Verdict::Revive);

    InsideFacts noData = f;  // no encounter data: never "completed", ends by timeout / stuck
    noData.allMask = 0;
    EXPECT_EQ(DecideInside(noData, p, end), Verdict::Continue);
}

TEST(DungeonProbePolicyTest, EncounterHelpers)
{
    EXPECT_EQ(NextEncounter(0b0101, 0b1111), 1);
    EXPECT_EQ(NextEncounter(0b1111, 0b1111), -1);
    EXPECT_EQ(NextEncounter(0, 0), -1);
    EXPECT_EQ(Bits(0b1011), 3u);
    EXPECT_STREQ(EndName(End::WipedOut), "wiped_out");
    EXPECT_STREQ(EndName(End::None), "");
    EXPECT_EQ(static_cast<int>(End::PrepareFailed), 7);
}

TEST(DungeonProbePolicyTest, LedgerFieldsAreFixedOrder)
{
    RunRecord r;
    r.party = "ally";
    r.rid = 3;
    r.map = 36;
    r.level = 28;
    r.quality = 4;
    r.instance = 12;
    r.mask = 0b011;
    r.allMask = 0b111;
    r.wipes = 1;
    r.revives = 2;
    r.stucks = 0;
    r.members = {101, 102};
    r.deaths = {1, 0};
    r.startMs = 1000;
    r.end = End::Completed;
    EXPECT_EQ(Fields(r, 61000),
              ",\"party\":\"ally\",\"rid\":3,\"dmap\":36,\"plvl\":28,\"q\":4,\"inst\":12,\"enc\":-1,\"mask\":3,"
              "\"all\":7,\"bosses\":2,\"total\":3,\"wipes\":1,\"revives\":2,\"stucks\":0,\"deaths\":[1,0],"
              "\"dur_ms\":60000,\"end\":\"completed\",\"members\":[101,102]");
    StuckPoint const s{36, -5, 10, 3, 2, 639};
    EXPECT_EQ(Fields(r, 500, 1, &s).substr(Fields(r, 500, 1).size()),
              ",\"smap\":36,\"sx\":-5,\"sy\":10,\"sz\":3,\"next\":2,\"boss\":639");
    EXPECT_NE(Fields(r, 500, 1).find(",\"enc\":1,"), std::string::npos);
    EXPECT_NE(Fields(r, 500).find(",\"dur_ms\":0,"), std::string::npos);  // clock before start clamps
}

TEST(DungeonProbePolicyTest, LedgerEventIsAppendOnly)
{
    using AutoWowQuestLedger::Event;
    EXPECT_EQ(static_cast<int>(Event::ClassQuest), 23);
    EXPECT_EQ(static_cast<int>(Event::DungeonProbe), 24);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::DungeonProbe), "dprobe");
}

TEST(DungeonProbePolicyTest, ProbeRegistryIsEmptyByDefault)
{
    detail::gProbeGuids.clear();
    EXPECT_FALSE(IsProbeBot(101));
    detail::gProbeGuids = {101, 205};
    EXPECT_TRUE(IsProbeBot(101));
    EXPECT_FALSE(IsProbeBot(102));
    detail::gProbeGuids.clear();
}
}  // namespace
