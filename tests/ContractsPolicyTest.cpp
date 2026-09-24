/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ContractsPolicy.h"

#include "AutoWowQuestLedger.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowContracts;

Spawn Mob(std::uint32_t id, std::uint32_t entry, std::int32_t x, std::int32_t y, std::uint8_t lo = 20,
          std::uint8_t hi = 20, std::uint8_t teams = kAlliance | kHorde)
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

// n spawns of `entry` around (x, y), ids from `firstId`.
void Pack(std::vector<Spawn>& out, std::uint32_t firstId, std::uint32_t entry, std::int32_t x, std::int32_t y,
          std::uint32_t n)
{
    for (std::uint32_t k = 0; k < n; ++k)
        out.push_back(Mob(firstId + k, entry, x + std::int32_t(k) * 5, y));
}

TEST(Contracts, LevelWindow)
{
    Params p;  // below 3, above 1
    EXPECT_TRUE(InLevelWindow(p, 20, Mob(1, 1, 0, 0, 17, 21)));
    EXPECT_FALSE(InLevelWindow(p, 20, Mob(1, 1, 0, 0, 16, 18)));  // min below the window
    EXPECT_FALSE(InLevelWindow(p, 20, Mob(1, 1, 0, 0, 20, 22)));  // max above the window
    EXPECT_TRUE(InLevelWindow(p, 2, Mob(1, 1, 0, 0, 1, 3)));      // low clamps at 1
}

TEST(Contracts, CandidatesFilterTeamLevelAndSearchRadius)
{
    Params p;
    std::vector<Spawn> map = {Mob(1, 10, 100, 0), Mob(2, 10, 800, 0), Mob(3, 10, 50, 0, 20, 20, kHorde),
                              Mob(4, 10, 60, 0, 25, 25), Mob(5, 10, 10, 0)};
    std::vector<Spawn> const c = Candidates(p, map, kAlliance, 20, 0, 0);
    ASSERT_EQ(c.size(), 2u);  // 800 yd out of search, horde-only, over level
    EXPECT_EQ(c[0].spawnId, 5u);  // nearest first
    EXPECT_EQ(c[1].spawnId, 1u);
}

TEST(Contracts, ClusterScoringPrefersDensityThenDistanceThenSpawnId)
{
    Params p;  // cluster 120 yd, min 6 spawns
    std::vector<Spawn> map;
    Pack(map, 100, 7, 400, 0, 8);   // dense far pack
    Pack(map, 200, 8, 100, 0, 6);   // smaller near pack
    Pack(map, 300, 9, -600, 0, 5);  // below MinClusterSpawns
    std::vector<Cluster> const ranked = RankClusters(p, Candidates(p, map, kAlliance, 20, 0, 0), 0, 0);
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked[0].spawns, 8u);
    EXPECT_EQ(ranked[0].entries, std::vector<std::uint32_t>{7});
    for (Cluster const& c : ranked)
        EXPECT_GE(c.spawns, p.minClusterSpawns);  // the 5-pack never scores

    // Equal density: the nearer center wins; equal distance too: the lower spawn id.
    std::vector<Spawn> tie = {Mob(9, 1, 10, 0), Mob(8, 1, -10, 0)};
    Params one = p;
    one.minClusterSpawns = 1;
    one.clusterYards = 5;
    std::vector<Cluster> const t = RankClusters(one, Candidates(one, tie, kAlliance, 20, 0, 0), 0, 0);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].center, 8u);
}

TEST(Contracts, EntriesMostSpawnsFirstCapped)
{
    Params p;
    p.minClusterSpawns = 1;
    std::vector<Spawn> map;
    for (std::uint32_t e = 0; e < kMaxEntries + 2; ++e)
        map.push_back(Mob(100 + e, 50 + e, 0, 0));
    map.push_back(Mob(300, 59, 1, 0));  // entry 59 has two spawns
    std::vector<Cluster> const ranked = RankClusters(p, Candidates(p, map, kAlliance, 20, 0, 0), 0, 0);
    ASSERT_FALSE(ranked.empty());
    ASSERT_EQ(ranked[0].entries.size(), kMaxEntries);
    EXPECT_EQ(ranked[0].entries[0], 59u);
    EXPECT_EQ(ranked[0].entries[1], 50u);
}

TEST(Contracts, PickSkipsRejectedAnchors)
{
    Params p;
    std::vector<Spawn> map;
    Pack(map, 100, 7, 400, 0, 8);   // best: danger area
    Pack(map, 200, 8, -400, 0, 7);  // next: over-level zone
    Pack(map, 300, 9, 0, 400, 6);
    std::vector<Cluster> const ranked = RankClusters(p, Candidates(p, map, kAlliance, 20, 0, 0), 0, 0);
    auto danger = [](Cluster const& c) { return c.x >= 300; };
    auto zoneTooHigh = [](Cluster const& c) { return c.x <= -300; };
    Cluster const* pick = PickCluster(ranked, [&](Cluster const& c) { return danger(c) || zoneTooHigh(c); });
    ASSERT_NE(pick, nullptr);
    EXPECT_EQ(pick->entries, std::vector<std::uint32_t>{9});
    EXPECT_EQ(PickCluster(ranked, [](Cluster const&) { return true; }), nullptr);
}

TEST(Contracts, KillCountDoneAndTimeout)
{
    Params p;
    p.kills = 3;
    Cluster c;
    c.center = 42;
    c.x = 100;
    c.entries = {7, 8};
    BotState s;
    Issue(s, p, c, 0, 1, 1000);
    EXPECT_EQ(s.phase, Phase::Travel);
    EXPECT_FALSE(NoteKill(s, 9));  // not a contract entry
    EXPECT_TRUE(NoteKill(s, 7));
    EXPECT_TRUE(NoteKill(s, 8));
    EXPECT_EQ(Judge(p, s, 2000, false), Reason::Issued);
    EXPECT_EQ(Judge(p, s, 1000 + p.timeoutMs, false), Reason::Expired);
    EXPECT_EQ(Judge(p, s, 2000, true), Reason::Expired);  // anchor became a danger area
    EXPECT_TRUE(NoteKill(s, 7));
    EXPECT_FALSE(NoteKill(s, 7));  // capped at the target
    EXPECT_EQ(Judge(p, s, 1000 + p.timeoutMs, true), Reason::Done);  // done beats expired
}

TEST(Contracts, FinishCoolsTheAnchor)
{
    Params p;
    Cluster c;
    c.center = 42;
    c.x = 100;
    c.y = 100;
    c.entries = {7};
    BotState s;
    Issue(s, p, c, 1, 5, 0);
    Finish(s, p, 10000);
    EXPECT_EQ(s.phase, Phase::None);
    EXPECT_EQ(s.version, kStateVersion);
    EXPECT_FALSE(Accepts(s, 7));
    EXPECT_TRUE(AnchorCooling(p, s, 1, 150, 100, 10000));          // same pack
    EXPECT_FALSE(AnchorCooling(p, s, 1, 400, 100, 10000));         // another pack
    EXPECT_FALSE(AnchorCooling(p, s, 0, 100, 100, 10000));         // another map
    EXPECT_FALSE(AnchorCooling(p, s, 1, 100, 100, 10000 + p.cooldownMs));
    EXPECT_EQ(s.nextSearchMs, 10000u);
}

TEST(Contracts, TravelHuntLeashAndDisplacement)
{
    Params p;  // leash 150
    Cluster c;
    c.x = 1000;
    c.entries = {7};
    BotState s;
    Issue(s, p, c, 0, 1, 0);
    EXPECT_EQ(NextPhase(p, s, 0, 900, 0), Phase::Travel);
    EXPECT_EQ(NextPhase(p, s, 0, 1000 - std::int32_t(kArriveYards), 0), Phase::Hunt);
    s.phase = Phase::Hunt;
    EXPECT_EQ(NextPhase(p, s, 0, 900, 0), Phase::Hunt);   // inside the leash
    EXPECT_EQ(NextPhase(p, s, 0, 849, 0), Phase::Travel); // drifted past it: walk back
    EXPECT_EQ(NextPhase(p, s, 1, 1000, 0), Phase::Travel);
    EXPECT_FALSE(Displaced(p, s, 1000 - 850, 0));
    EXPECT_TRUE(Displaced(p, s, 1000 - 851, 0));

    EXPECT_TRUE(HuntTarget(p, s, 7, 0, 1100, 0));
    EXPECT_FALSE(HuntTarget(p, s, 7, 0, 1151, 0));  // beyond the leash
    EXPECT_FALSE(HuntTarget(p, s, 8, 0, 1000, 0));  // not a contract entry
    EXPECT_FALSE(HuntTarget(p, s, 7, 1, 1000, 0));  // another map
}

TEST(Contracts, LedgerLine)
{
    Params p;
    p.kills = 15;
    Cluster c;
    c.center = 42;
    c.x = -9000;
    c.y = 300;
    c.entries = {7, 8};
    BotState s;
    Issue(s, p, c, 0, 3, 1000);
    s.kills = 4;
    EXPECT_EQ(LedgerFields(s, 61000),
              ",\"issuer\":\"faction_board\",\"cid\":3,\"amap\":0,\"ax\":-9000,\"ay\":300,\"anchor\":42,"
              "\"entries\":2,\"kills\":4,\"target\":15,\"dur_ms\":60000");
    EXPECT_STREQ(ReasonName(Reason::Abandoned), "abandoned");

    using AutoWowQuestLedger::Event;
    EXPECT_EQ(static_cast<int>(Event::Contract), 18);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Contract), "contract");
    AutoWowQuestLedger::Row row;
    row.ev = Event::Contract;
    row.reason = ReasonName(Reason::Done);
    std::string const fields = LedgerFields(s, 61000);
    row.extra = fields;
    std::string const line = AutoWowQuestLedger::FormatLine("r", row);
    EXPECT_NE(line.find("\"ev\":\"contract\""), std::string::npos);
    EXPECT_NE(line.find("\"reason\":\"done\",\"phase\":\"\",\"issuer\":\"faction_board\""), std::string::npos);
}
}  // namespace
