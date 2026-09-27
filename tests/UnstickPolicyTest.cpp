/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyPolicy.h"
#include "UnstickPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowUnstickV2;
using AutoWowZoneProgression::Route;

TEST(UnstickV2, GrindBandsWidenFromTheBaseReachNearestFirst)
{
    auto const bands = GrindBands(1000);
    ASSERT_EQ(bands.size(), 3u);
    EXPECT_EQ(bands[0], std::make_pair(250u, 500u));
    EXPECT_EQ(bands[1], std::make_pair(500u, 750u));
    EXPECT_EQ(bands[2], std::make_pair(750u, 1000u));
    EXPECT_EQ(GrindBands(600).back(), std::make_pair(500u, 600u));  // last band clipped
    EXPECT_TRUE(GrindBands(250).empty());                            // no widening
}

// soak-s51: a L30 in Desolace (405). No route leaves 405 for L30; Dustwallow (min 35) must not be picked.
TEST(UnstickV2, UnstickRoutePrefersFittingBandThenSameMapNearest)
{
    std::vector<Route> const routes = {
        {2, 405, 15, 35, 45, 1, -3000, -2500, 30, 1, false},   // Dustwallow: starts 5 above -> never
        {2, 400, 405, 28, 36, 1, -1600, 3100, 90, 2, false},   // into Desolace itself -> never
        {1, 17, 406, 20, 30, 1, 900, 900, 10, 3, false},       // alliance only
        {2, 17, 400, 25, 35, 1, -5400, -2400, 90, 4, false},   // Thousand Needles holds 30, far
        {2, 406, 17, 25, 35, 1, -400, 2600, 90, 5, false},     // holds 30, nearer on map 1
        {2, 38, 44, 25, 35, 0, 0, 0, 0, 6, false},             // holds 30, other map
        {2, 215, 17, 10, 25, 1, -1500, 2800, 90, 7, false},    // band below: gap 5
    };
    Route const* r = PickUnstickRoute(routes, 2, 30, 2, 405, 1, -1600, 3000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->inn, 5u);
    // Only a lower band fits: it is taken (gap 5) rather than nothing.
    std::vector<Route> const low = {routes[0], routes[6]};
    r = PickUnstickRoute(low, 2, 30, 2, 405, 1, -1600, 3000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->inn, 7u);
    // Within the margin a band starting just above the level counts (gap 2 beats gap 5).
    std::vector<Route> const near = {routes[6], {2, 405, 15, 32, 45, 1, -3000, -2500, 30, 8, false}};
    EXPECT_EQ(PickUnstickRoute(near, 2, 30, 2, 405, 1, -1600, 3000)->inn, 8u);
    EXPECT_EQ(PickUnstickRoute({routes[0], routes[1]}, 2, 30, 2, 405, 1, 0, 0), nullptr);
}

TEST(UnstickV2, XpWatchNeedsAFullWindowWithoutXp)
{
    XpWatch w;
    EXPECT_FALSE(NoteXp(w, 30, 100, 1000, 60000));   // first sample
    EXPECT_FALSE(NoteXp(w, 30, 100, 60999, 60000));
    EXPECT_TRUE(NoteXp(w, 30, 100, 61000, 60000));
    EXPECT_FALSE(NoteXp(w, 30, 150, 62000, 60000));  // XP moved: window restarts
    EXPECT_FALSE(NoteXp(w, 30, 150, 100000, 60000));
    EXPECT_FALSE(NoteXp(w, 31, 0, 200000, 60000));   // level up restarts too
    EXPECT_FALSE(NoteXp(w, 31, 0, 999999, 0));       // 0 = off
}

QuestSchedulerPolicy::Observation Obs(std::uint32_t quest, std::uint16_t kills, bool complete = false)
{
    QuestSchedulerPolicy::Observation o;
    o.counters.quest = quest;
    o.counters.c[0] = kills;
    o.complete = complete;
    return o;
}

TEST(UnstickV2, AgeBookStampsFirstSightAndProgressOnly)
{
    AgeBook book;
    book.Observe({Obs(1, 0), Obs(2, 0)}, 1000);
    book.Observe({Obs(1, 0), Obs(2, 3), Obs(3, 0)}, 5000);
    ASSERT_EQ(book.entries.size(), 3u);
    EXPECT_EQ(book.entries[0].sinceMs, 1000u);  // unchanged
    EXPECT_EQ(book.entries[1].sinceMs, 5000u);  // counter moved
    EXPECT_EQ(book.entries[2].sinceMs, 5000u);  // new
    book.Observe({Obs(2, 3, true)}, 9000);       // 1 and 3 left the log; 2 completed
    ASSERT_EQ(book.entries.size(), 1u);
    EXPECT_EQ(book.entries[0].sinceMs, 9000u);
}

TEST(UnstickV2, TrimOnlyStaleIncompleteOtherZoneQuestsDownToTheThreshold)
{
    std::uint64_t const now = 10000000;
    std::uint64_t const stale = 3600000;
    std::vector<TrimFact> log;
    for (std::uint32_t q = 1; q <= 16; ++q)
        log.push_back({q, 405, false, 0});                  // in zone: never
    log.push_back({100, 17, false, 5000});                  // stale, other zone
    log.push_back({101, 17, false, 1000});                  // stale, older
    log.push_back({102, 17, true, 0});                      // complete: never
    log.push_back({103, -141, false, 0});                   // class / profession sort: never
    log.push_back({104, 17, false, now - stale + 1});       // not stale yet
    // 21 entries, threshold 18 -> at most 3, oldest first.
    EXPECT_EQ(PickTrims(log, 405, now, 18, stale), (std::vector<std::uint32_t>{101, 100}));
    // Threshold 20 -> room for one: the oldest.
    EXPECT_EQ(PickTrims(log, 405, now, 20, stale), (std::vector<std::uint32_t>{101}));
    EXPECT_TRUE(PickTrims(log, 405, now, 21, stale).empty());  // not clogged
    EXPECT_TRUE(PickTrims(log, 405, now, 0, stale).empty());   // off
    // In zone 17 those are the bot's own quests.
    EXPECT_TRUE(PickTrims(log, 17, now, 1, stale).size() == 16u);
}

// soak-s51: Brisenne in combat 3059 s, 0 damage taken, 0 kills.
TEST(UnstickV2, CombatStallCountsFromTheLaterOfCombatStartAndLastActivity)
{
    EXPECT_FALSE(CombatStalled(1000, 0, 180999, 180000));
    EXPECT_TRUE(CombatStalled(1000, 0, 181000, 180000));
    EXPECT_FALSE(CombatStalled(1000, 100000, 200000, 180000));  // activity inside the combat
    EXPECT_TRUE(CombatStalled(1000, 100000, 280000, 180000));
    EXPECT_FALSE(CombatStalled(50000, 900, 200000, 180000));    // activity before this combat does not reset it...
    EXPECT_TRUE(CombatStalled(50000, 900, 230000, 180000));     // ...nor extend it
    EXPECT_FALSE(CombatStalled(0, 0, 999999, 180000));          // not in combat
    EXPECT_FALSE(CombatStalled(1000, 0, 999999, 0));            // off
}

TEST(UnstickV2, PartyStallRestartsOnAnyQuestLogChange)
{
    std::uint64_t sig = 0;
    std::uint64_t since = 0;
    std::uint64_t const a = Fold(Fold(kFoldSeed, 1234), 3);
    std::uint64_t const b = Fold(Fold(kFoldSeed, 1234), 4);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, Fold(Fold(kFoldSeed, 1234), 3));  // deterministic
    EXPECT_FALSE(PartyStalled(sig, since, a, 1000, 60000));  // first look
    EXPECT_FALSE(PartyStalled(sig, since, a, 60999, 60000));
    EXPECT_TRUE(PartyStalled(sig, since, a, 61000, 60000));
    EXPECT_FALSE(PartyStalled(sig, since, b, 62000, 60000));  // progress
    EXPECT_FALSE(PartyStalled(sig, since, b, 999999, 0));     // off
}

TEST(UnstickV2, WireNamesAppendOnly)
{
    EXPECT_STREQ(AutoWowZoneProgression::TriggerName(AutoWowZoneProgression::Trigger::Stuck), "unstick");
    EXPECT_EQ(static_cast<int>(AutoWowZoneProgression::Trigger::Stuck), 4);
    EXPECT_STREQ(AutoWowParty::DisbandName(AutoWowParty::Disband::Stalled), "stalled");
    EXPECT_EQ(static_cast<int>(AutoWowParty::Disband::Stalled), 9);
    EXPECT_FALSE(AutoWowZoneProgression::BotState{}.noFlight);
}
}  // namespace
