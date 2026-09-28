/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "../src/Ai/Dungeon/Generic/DungeonNavigatorAmbientPolicy.h"
#include "../src/Ai/Dungeon/Generic/DungeonNavigatorConvoyPolicy.h"
#include "../src/Ai/Dungeon/Generic/DungeonNavigatorPacingPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
enum class FakeMapKind
{
    World,
    Dungeon,
    Raid
};

enum class FakeBotState
{
    NonCombat,
    Combat
};

struct FakeMap
{
    FakeMapKind kind = FakeMapKind::World;
    mutable unsigned dungeonChecks = 0;

    bool IsDungeon() const
    {
        ++dungeonChecks;
        return kind == FakeMapKind::Dungeon || kind == FakeMapKind::Raid;
    }
};

struct FakeBot
{
    bool inWorld = false;
    FakeMap const* map = nullptr;
    mutable unsigned mapChecks = 0;

    bool IsInWorld() const { return inWorld; }
    FakeMap const* GetMap() const
    {
        ++mapChecks;
        return map;
    }
};

struct FakeBotAI
{
    FakeBotState currentState = FakeBotState::NonCombat;
    bool navigatorInNonCombat = false;
    bool navigatorInCombat = false;
    unsigned strategyChecks = 0;
    std::string lastStrategy;
    FakeBotState lastState = FakeBotState::Combat;

    FakeBotState GetState() const { return currentState; }

    bool HasStrategy(std::string const& name, FakeBotState state)
    {
        ++strategyChecks;
        lastStrategy = name;
        lastState = state;
        if (name != "dungeon navigator")
            return false;
        return state == FakeBotState::NonCombat ? navigatorInNonCombat : navigatorInCombat;
    }
};

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string_view FunctionBody(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};
    std::size_t const finish = source.find(end, begin + start.size());
    return finish == std::string::npos ? std::string_view(source).substr(begin)
                                      : std::string_view(source).substr(begin, finish - begin);
}

std::size_t CountOccurrences(std::string_view source, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t position = source.find(needle); position != std::string_view::npos;
         position = source.find(needle, position + needle.size()))
    {
        ++count;
    }
    return count;
}

DungeonNavigatorConvoy::SharedRegroupBounds SharedRegroupBounds()
{
    return {8, 1.5f, 3.0f, 128.0f};
}

DungeonNavigatorConvoy::SharedCandidate MakeSharedCandidate(std::size_t routeIndex)
{
    DungeonNavigatorConvoy::SharedCandidate candidate;
    candidate.routeIndex = routeIndex;
    candidate.sameMap = true;
    candidate.ordinaryWalk = true;
    candidate.followerSafe = true;
    candidate.followerDestinationReached = true;
    candidate.followerPathLength = 24.0f;
    candidate.followerPhysicalProgress = 24.0f;
    candidate.leaderSafe = true;
    candidate.leaderDestinationReached = true;
    candidate.leaderPathLength = 36.0f;
    candidate.leaderPhysicalProgress = 36.0f;
    return candidate;
}
}

TEST(DungeonNavigatorAmbientPolicy, DungeonAndRaidUseTheSameIsDungeonGate)
{
    for (FakeMapKind const kind : {FakeMapKind::Dungeon, FakeMapKind::Raid})
    {
        FakeMap map{kind};
        FakeBot bot{true, &map};
        FakeBotAI botAI;
        botAI.navigatorInNonCombat = true;

        EXPECT_TRUE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
            &bot, &botAI, FakeBotState::NonCombat));
        EXPECT_EQ(map.dungeonChecks, 1u);
        EXPECT_EQ(botAI.strategyChecks, 1u);
        EXPECT_EQ(botAI.lastStrategy, "dungeon navigator");
        EXPECT_EQ(botAI.lastState, FakeBotState::NonCombat);
    }
}

TEST(DungeonNavigatorAmbientPolicy, WorldMapBehaviorIsPreserved)
{
    FakeMap map{FakeMapKind::World};
    FakeBot bot{true, &map};
    FakeBotAI botAI;
    botAI.navigatorInNonCombat = true;

    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        &bot, &botAI, FakeBotState::NonCombat));
    EXPECT_EQ(map.dungeonChecks, 1u);
    EXPECT_EQ(botAI.strategyChecks, 0u);
}

TEST(DungeonNavigatorAmbientPolicy, InstanceBotWithoutNonCombatNavigatorIsPreserved)
{
    FakeMap map{FakeMapKind::Dungeon};
    FakeBot bot{true, &map};
    FakeBotAI botAI;
    botAI.navigatorInCombat = true;

    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        &bot, &botAI, FakeBotState::NonCombat));
    EXPECT_EQ(botAI.strategyChecks, 1u);
    EXPECT_EQ(botAI.lastState, FakeBotState::NonCombat);
}

TEST(DungeonNavigatorAmbientPolicy, MissingBotMapOrAiFailsOpen)
{
    FakeMap map{FakeMapKind::Dungeon};
    FakeBot botOffMap{false, &map};
    FakeBot botWithoutMap{true, nullptr};
    FakeBot botWithMap{true, &map};
    FakeBotAI botAI;
    botAI.navigatorInNonCombat = true;

    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        &botOffMap, &botAI, FakeBotState::NonCombat));
    EXPECT_EQ(botOffMap.mapChecks, 0u);
    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        &botWithoutMap, &botAI, FakeBotState::NonCombat));
    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        &botWithMap, static_cast<FakeBotAI*>(nullptr), FakeBotState::NonCombat));
    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppress(
        static_cast<FakeBot const*>(nullptr), &botAI, FakeBotState::NonCombat));
    EXPECT_EQ(botAI.strategyChecks, 0u);
}

TEST(DungeonNavigatorAmbientPolicy, BotLedDungeonAndRaidConvoysSuppressOrdinaryFollow)
{
    for (FakeMapKind const kind : {FakeMapKind::Dungeon, FakeMapKind::Raid})
    {
        FakeMap map{kind};
        FakeBot bot{true, &map};
        FakeBotAI followerAI;
        FakeBotAI leaderAI;
        followerAI.currentState = FakeBotState::NonCombat;
        leaderAI.currentState = FakeBotState::NonCombat;
        followerAI.navigatorInNonCombat = true;
        leaderAI.navigatorInNonCombat = true;

        EXPECT_TRUE(DungeonNavigatorAmbientPolicy::ShouldSuppressOrdinaryFollow(
            &bot, &followerAI, &leaderAI, true, true,
            followerAI.GetState(), FakeBotState::NonCombat));
        EXPECT_EQ(followerAI.strategyChecks, 1u);
        EXPECT_EQ(leaderAI.strategyChecks, 1u);
    }
}

TEST(DungeonNavigatorAmbientPolicy, LeaderCombatRestoresOrdinaryFollow)
{
    FakeMap dungeon{FakeMapKind::Dungeon};
    FakeBot bot{true, &dungeon};
    FakeBotAI followerAI;
    FakeBotAI leaderAI;
    followerAI.currentState = FakeBotState::NonCombat;
    leaderAI.currentState = FakeBotState::Combat;
    followerAI.navigatorInNonCombat = true;
    leaderAI.navigatorInNonCombat = true;

    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppressOrdinaryFollow(
        &bot, &followerAI, &leaderAI, true, true,
        followerAI.GetState(), FakeBotState::NonCombat));
    EXPECT_EQ(followerAI.strategyChecks, 0u);
    EXPECT_EQ(leaderAI.strategyChecks, 0u);
}

TEST(DungeonNavigatorAmbientPolicy, FollowerCombatRestoresOrdinaryFollow)
{
    FakeMap dungeon{FakeMapKind::Dungeon};
    FakeBot bot{true, &dungeon};
    FakeBotAI followerAI;
    FakeBotAI leaderAI;
    followerAI.currentState = FakeBotState::Combat;
    leaderAI.currentState = FakeBotState::NonCombat;
    followerAI.navigatorInNonCombat = true;
    leaderAI.navigatorInNonCombat = true;

    EXPECT_FALSE(DungeonNavigatorAmbientPolicy::ShouldSuppressOrdinaryFollow(
        &bot, &followerAI, &leaderAI, true, true,
        followerAI.GetState(), FakeBotState::NonCombat));
    EXPECT_EQ(followerAI.strategyChecks, 0u);
    EXPECT_EQ(leaderAI.strategyChecks, 0u);
}

TEST(DungeonNavigatorAmbientPolicy, OrdinaryFollowIsPreservedOutsideExactNavigatorOwnership)
{
    FakeMap dungeon{FakeMapKind::Dungeon};
    FakeMap world{FakeMapKind::World};
    FakeBot dungeonBot{true, &dungeon};
    FakeBot worldBot{true, &world};
    FakeBotAI followerAI;
    FakeBotAI leaderAI;
    followerAI.navigatorInNonCombat = true;
    leaderAI.navigatorInNonCombat = true;

    auto suppressed = [&](FakeBot* bot, FakeBotState state, bool follower, bool sameContext,
                          FakeBotAI* leader)
    {
        return DungeonNavigatorAmbientPolicy::ShouldSuppressOrdinaryFollow(
            bot, &followerAI, leader, follower, sameContext,
            state, FakeBotState::NonCombat);
    };

    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::Combat, true, true, &leaderAI));
    EXPECT_FALSE(suppressed(&worldBot, FakeBotState::NonCombat, true, true, &leaderAI));
    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::NonCombat, false, true, &leaderAI));
    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::NonCombat, true, false, &leaderAI));
    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::NonCombat, true, true, nullptr));

    leaderAI.navigatorInNonCombat = false;
    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::NonCombat, true, true, &leaderAI));
    leaderAI.navigatorInNonCombat = true;
    followerAI.navigatorInNonCombat = false;
    EXPECT_FALSE(suppressed(&dungeonBot, FakeBotState::NonCombat, true, true, &leaderAI));
}

TEST(DungeonNavigatorPacingPolicy, DefaultProfileKeepsFiveSecondPacing)
{
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(5000u), 5000u);
}

TEST(DungeonNavigatorPacingPolicy, FastProfileUsesConfiguredMaximum)
{
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(1000u), 1000u);
}

TEST(DungeonNavigatorPacingPolicy, ZeroAndSmallValuesUseHundredMillisecondFloor)
{
    EXPECT_EQ(DungeonNavigatorPacing::SuccessfulMoveBackoffFloorMs, 100u);
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(0u), 100u);
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(1u), 100u);
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(99u), 100u);
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(100u), 100u);
}

TEST(DungeonNavigatorPacingPolicy, ValuesAboveCeilingRemainCapped)
{
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(5001u),
        DungeonNavigatorPacing::SuccessfulMoveBackoffCeilingMs);
    EXPECT_EQ(DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(10000u),
        DungeonNavigatorPacing::SuccessfulMoveBackoffCeilingMs);
}

TEST(DungeonNavigatorConvoyPolicy, SharedReachabilityRequiresBothOrdinaryWalks)
{
    auto const bounds = SharedRegroupBounds();
    auto candidate = MakeSharedCandidate(12);

    EXPECT_TRUE(DungeonNavigatorConvoy::IsSharedOrdinarilyReachable(candidate, bounds));

    candidate.sameMap = false;
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedOrdinarilyReachable(candidate, bounds));
    candidate.sameMap = true;
    candidate.followerSafe = false;
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedOrdinarilyReachable(candidate, bounds));
    candidate.followerSafe = true;
    candidate.leaderDestinationReached = false;
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedOrdinarilyReachable(candidate, bounds));
}

TEST(DungeonNavigatorConvoyPolicy, SharedSelectionChoosesFurthestSafeAnchor)
{
    std::vector<DungeonNavigatorConvoy::SharedCandidate> candidates = {
        MakeSharedCandidate(4), MakeSharedCandidate(11), MakeSharedCandidate(9),
        MakeSharedCandidate(15),
    };

    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 12, SharedRegroupBounds()), 1u);
}

TEST(DungeonNavigatorConvoyPolicy, SharedSelectionHonorsCandidateAndPathBounds)
{
    auto bounds = SharedRegroupBounds();
    bounds.maximumCandidates = 2;
    std::vector<DungeonNavigatorConvoy::SharedCandidate> candidates = {
        MakeSharedCandidate(7), MakeSharedCandidate(6), MakeSharedCandidate(12),
    };
    candidates[0].ordinaryWalk = false;
    candidates[1].leaderPathLength = 129.0f;

    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 19, bounds), DungeonNavigatorConvoy::NoSelection);

    bounds.maximumCandidates = 3;
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 19, bounds), 2u);

    bounds.maximumPathLength = 32.0f;
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 19, bounds), DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, SharedSelectionFailsClosedWithoutCandidate)
{
    auto unsafe = MakeSharedCandidate(12);
    unsafe.followerSafe = false;
    auto crossMap = MakeSharedCandidate(11);
    crossMap.sameMap = false;
    auto incomplete = MakeSharedCandidate(10);
    incomplete.followerDestinationReached = false;
    auto special = MakeSharedCandidate(9);
    special.ordinaryWalk = false;

    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        {unsafe, crossMap, incomplete, special}, 20, 18, SharedRegroupBounds()),
        DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, SharedSelectionIsRouteMonotonic)
{
    std::vector<DungeonNavigatorConvoy::SharedCandidate> candidates = {
        MakeSharedCandidate(17), MakeSharedCandidate(18), MakeSharedCandidate(19),
        MakeSharedCandidate(20),
    };
    auto const bounds = SharedRegroupBounds();

    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 18, bounds), 1u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 18, bounds, 18), 1u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
        candidates, 20, 18, bounds, 19), DungeonNavigatorConvoy::NoSelection);
}

namespace
{
DungeonNavigatorConvoy::Candidate MakeCandidate(std::size_t routeIndex, bool reached,
                                                float distance)
{
    return {routeIndex, true, reached, reached, distance, distance};
}
}

// S52 RFK/BFD/Gnomeregan: slot 9 is unreachable, the follower stands on 8. V1 is not settled
// (slot 9 is 6 yd away) and SelectTarget rejects 8 (under the 1.5 yd minimum move), so it walks
// to 7, then back to 8, forever while the leader waits.
TEST(DungeonNavigatorConvoyPolicy, UnreachableSlotBouncesInV1AndSettlesInV2)
{
    std::vector<DungeonNavigatorConvoy::Candidate> const candidates = {
        MakeCandidate(7, true, 5.0f), MakeCandidate(8, true, 0.8f),
        MakeCandidate(9, false, 6.0f),
    };

    EXPECT_FALSE(DungeonNavigatorConvoy::IsSettledAtAssignedPoint(true, 6.0f, 3.0f));
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(candidates, 10, 1, 1.5f, 45.0f), 0u);

    EXPECT_EQ(DungeonNavigatorConvoy::SettledRouteIndex(candidates, 9, 3.0f, 45.0f), 8u);
    // Standing on the point settles even when the tiny probe itself is rejected.
    auto standing = candidates;
    standing[1].safe = false;
    EXPECT_EQ(DungeonNavigatorConvoy::SettledRouteIndex(standing, 9, 3.0f, 45.0f), 8u);
}

TEST(DungeonNavigatorConvoyPolicy, V2SettleOnlyWhenBestReachablePointIsUnderFoot)
{
    // The slot itself is reachable: move to it, do not settle short.
    std::vector<DungeonNavigatorConvoy::Candidate> reachableSlot = {
        MakeCandidate(8, true, 0.8f), MakeCandidate(9, true, 6.0f),
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SettledRouteIndex(reachableSlot, 9, 3.0f, 45.0f),
        DungeonNavigatorConvoy::NoSelection);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(reachableSlot, 10, 1, 1.5f, 45.0f), 1u);

    // Best reachable point is 8 but the follower stands on 6: walk to 8.
    std::vector<DungeonNavigatorConvoy::Candidate> behind = {
        MakeCandidate(6, true, 0.5f), MakeCandidate(8, true, 9.0f),
        MakeCandidate(9, false, 14.0f),
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SettledRouteIndex(behind, 9, 3.0f, 45.0f),
        DungeonNavigatorConvoy::NoSelection);

    // A point beyond the slot never counts.
    EXPECT_EQ(DungeonNavigatorConvoy::SettledRouteIndex(reachableSlot, 7, 3.0f, 45.0f),
        DungeonNavigatorConvoy::NoSelection);
}

TEST(DungeonNavigatorConvoyPolicy, V2RouteFloorBlocksStepBelowSettledPoint)
{
    std::vector<DungeonNavigatorConvoy::Candidate> const displaced = {
        MakeCandidate(7, true, 5.0f), MakeCandidate(8, true, 4.0f),
        MakeCandidate(9, false, 8.0f),
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(displaced, 10, 1, 1.5f, 45.0f), 1u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(displaced, 10, 1, 1.5f, 45.0f, 8), 1u);

    std::vector<DungeonNavigatorConvoy::Candidate> const onSettled = {
        MakeCandidate(7, true, 5.0f), MakeCandidate(8, true, 0.8f),
    };
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(onSettled, 10, 1, 1.5f, 45.0f), 0u);
    EXPECT_EQ(DungeonNavigatorConvoy::SelectTarget(onSettled, 10, 1, 1.5f, 45.0f, 8),
        DungeonNavigatorConvoy::NoSelection);
}

// S52 WC Alliance / DM Horde: slot 0 gives a one-point window, and V1 treats it as exhausted on
// the first attempt, latching a permanent terminal. V2 spends the real attempt budget.
TEST(DungeonNavigatorConvoyPolicy, V2SharedRegroupSpendsRealAttemptBudget)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::IsSharedRegroupExhausted(false, 1, 3, 1, 1));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedRegroupExhausted(true, 1, 3, 1, 1));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedRegroupExhausted(true, 2, 3, 1, 1));
    EXPECT_TRUE(DungeonNavigatorConvoy::IsSharedRegroupExhausted(true, 3, 3, 1, 1));
    EXPECT_FALSE(DungeonNavigatorConvoy::IsSharedRegroupExhausted(false, 1, 3, 5, 128));
}

TEST(DungeonNavigatorConvoyPolicy, V2SharedRegroupTerminalReleasesOnGeometryChange)
{
    EXPECT_FALSE(DungeonNavigatorConvoy::ShouldReleaseSharedRegroupTerminal(4, 4, 60.0f, 45.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::ShouldReleaseSharedRegroupTerminal(4, 5, 60.0f, 45.0f));
    EXPECT_TRUE(DungeonNavigatorConvoy::ShouldReleaseSharedRegroupTerminal(4, 4, 45.0f, 45.0f));
    EXPECT_FALSE(DungeonNavigatorConvoy::ShouldReleaseSharedRegroupTerminal(
        4, 4, std::numeric_limits<float>::infinity(), 45.0f));
}

// S53 WC 143 / RFK enc 4 index 0: the arrival scan advanced the leader past the point underfoot, the
// backward re-anchor rewound onto it, its zero-length probe was rejected and the leader reported
// no_reachable_waypoint forever. V2 keeps the cursor; an earlier anchor (drift back) still re-anchors.
TEST(DungeonNavigatorConvoyPolicy, V2LeaderKeepsArrivedPointInsteadOfReanchoringOntoIt)
{
    EXPECT_TRUE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(false, 143, 144));
    EXPECT_FALSE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(true, 143, 144));
    EXPECT_FALSE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(true, 0, 1));
    EXPECT_TRUE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(true, 140, 144));
    EXPECT_FALSE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(true, 144, 144));
    EXPECT_FALSE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(false, 144, 144));
    EXPECT_FALSE(DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(
        true, DungeonNavigatorConvoy::NoSelection, 144));
}

// S53 RFK index 59: 13.7 yd hop, 4.2 yd drop, path_type SHORTCUT|NOPATH.
TEST(DungeonNavigatorConvoyPolicy, V2DirectHopIsBoundedToShortNoPathHopsWithSafeReverse)
{
    auto hop = [](bool v2, bool noPath, float h, float v, bool reverse, unsigned attempts)
    { return DungeonNavigatorConvoy::CanDirectHop(v2, noPath, h, v, reverse, 15.0f, 5.5f, attempts, 2); };
    EXPECT_TRUE(hop(true, true, 12.9f, -4.2f, true, 0));
    EXPECT_TRUE(hop(true, true, 12.9f, 4.2f, true, 1));
    EXPECT_FALSE(hop(false, true, 12.9f, -4.2f, true, 0));
    EXPECT_FALSE(hop(true, false, 12.9f, -4.2f, true, 0));
    EXPECT_FALSE(hop(true, true, 12.9f, -4.2f, false, 0));
    EXPECT_FALSE(hop(true, true, 15.5f, -4.2f, true, 0));
    EXPECT_FALSE(hop(true, true, 12.9f, -5.6f, true, 0));
    EXPECT_FALSE(hop(true, true, 12.9f, -4.2f, true, 2));
    EXPECT_FALSE(hop(true, true, std::numeric_limits<float>::infinity(), 0.0f, true, 0));
}

// S56 offline Detour replay: slope-checked route legs NOPATH (BFD, Gnomeregan) or slope-truncated
// NORMAL|INCOMPLETE (RFK, WC, Deadmines past the Iron Clad Door); the same legs slope-free are complete.
TEST(DungeonNavigatorConvoyPolicy, V2RouteLegFallsBackToSlopeFreeProbe)
{
    using DungeonNavigatorConvoy::UseSlopeFreeLeg;
    EXPECT_FALSE(UseSlopeFreeLeg(true, true, true, true, true));     // checked reached: kept
    EXPECT_TRUE(UseSlopeFreeLeg(true, false, false, true, true));    // checked NOPATH, slope-free reaches
    EXPECT_TRUE(UseSlopeFreeLeg(true, true, false, true, true));     // checked truncated, slope-free reaches
    EXPECT_FALSE(UseSlopeFreeLeg(true, true, false, true, false));   // both partial: checked kept
    EXPECT_TRUE(UseSlopeFreeLeg(true, false, false, true, false));   // slope-free the only safe progress
    EXPECT_FALSE(UseSlopeFreeLeg(true, false, false, false, false)); // slope-free unsafe
    EXPECT_FALSE(UseSlopeFreeLeg(false, false, false, true, true));  // ConvoyV2 off
    EXPECT_FALSE(UseSlopeFreeLeg(false, true, false, true, true));
}

// S56 RFK stored link Ramtusk->Willix: 39.6 yd drop over 7.1 yd at its first segment.
TEST(DungeonNavigatorConvoyPolicy, V2StoredWalkCliffIsAVerticalStepBeyondItsRun)
{
    using DungeonNavigatorConvoy::IsStoredWalkCliff;
    EXPECT_TRUE(IsStoredWalkCliff(7.1f, -39.6f, 5.5f));
    EXPECT_TRUE(IsStoredWalkCliff(0.9f, 12.2f, 5.5f));
    EXPECT_FALSE(IsStoredWalkCliff(7.4f, 1.2f, 5.5f));
    EXPECT_FALSE(IsStoredWalkCliff(6.3f, -10.6f, 5.5f));
    EXPECT_FALSE(IsStoredWalkCliff(0.0f, 5.5f, 5.5f));
    EXPECT_TRUE(IsStoredWalkCliff(0.0f, 5.6f, 5.5f));
    EXPECT_TRUE(IsStoredWalkCliff(std::numeric_limits<float>::infinity(), 0.0f, 5.5f));
    EXPECT_TRUE(IsStoredWalkCliff(1.0f, std::numeric_limits<float>::quiet_NaN(), 5.5f));

    // RFK: Ramtusk->Willix (331 yd, cliff) loses to Ramtusk->Kraul->Charlga (141 + 510 yd).
    using DungeonNavigatorConvoy::StoredWalkLinkCost;
    EXPECT_GT(StoredWalkLinkCost(true, 331.0f), StoredWalkLinkCost(false, 141.0f) +
        StoredWalkLinkCost(false, 510.0f));
    EXPECT_EQ(StoredWalkLinkCost(false, 331.0f), 331.0f);
}

TEST(DungeonNavigatorConvoySourceContract, RouteLegsFallBackSlopeFreeAndSkipCliffsOnlyUnderV2)
{
    std::string const source = ReadSource(
        ModuleRoot() / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const helper = source.find("AutoWowDungeonPath::ProbeResult ProbeLeg(");
    ASSERT_NE(helper, std::string::npos);
    std::size_t const offGate = source.find("if (!ConvoyV2Enabled())\n        return checked;", helper);
    std::size_t const slopeFree = source.find("AutoWowDungeonPath::Probe(player, x, y, z, false)", helper);
    ASSERT_NE(offGate, std::string::npos);
    ASSERT_NE(slopeFree, std::string::npos);
    EXPECT_LT(offGate, slopeFree);
    EXPECT_NE(source.find("ProbeLeg(bot, point.x, point.y, point.z);"), std::string::npos);
    EXPECT_EQ(source.find("AutoWowDungeonPath::Probe(bot, point.x, point.y, point.z);"), std::string::npos);
    EXPECT_NE(source.find("(ConvoyV2Enabled() && StoredWalkPathHasCliff(path))"), std::string::npos);
    EXPECT_NE(source.find("ConvoyV2Enabled() && StoredWalkPathHasCliff(path), path->getDistance()"),
        std::string::npos);
}

TEST(DungeonNavigatorConvoySourceContract, SharedRegroupIsBoundedAndOrdinaryWalkOnly)
{
    std::string const source = ReadSource(
        ModuleRoot() / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const start = source.find(
        "A follower can be stranded on a collision layer");
    std::size_t const end = source.find(
        "A vertical split can leave the follower", start);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    std::string_view const shared(source.data() + start, end - start);

    EXPECT_NE(shared.find("SelectSharedBackwardReanchor"), std::string_view::npos);
    EXPECT_NE(shared.find("ConvoyBackwardReanchorPointLimit"), std::string_view::npos);
    EXPECT_NE(shared.find("ConvoySharedRegroupAttemptLimit"), std::string_view::npos);
    EXPECT_EQ(shared.find("NearTeleportTo"), std::string_view::npos);
    EXPECT_EQ(shared.find("TravelNodeMap"), std::string_view::npos);
    EXPECT_EQ(shared.find("getFullPath"), std::string_view::npos);
}

TEST(DungeonNavigatorPacingPolicy, NavigatorRoutesEverySuccessfulRescanThroughPolicy)
{
    std::string const source = ReadSource(
        ModuleRoot() / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    ASSERT_FALSE(source.empty());

    // The count includes the helper definition plus the twelve successful-rescan call sites
    // (three are the AutoWow.DungeonNav.Gates step move/attack/key-holder walk).
    EXPECT_EQ(CountOccurrences(source, "SuccessfulMoveRescanDelayMs()"), 13u);
    EXPECT_EQ(source.find("constexpr uint32 SuccessfulMoveBackoffMs"), std::string::npos);
    EXPECT_NE(source.find("DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs"),
        std::string::npos);
}

TEST(DungeonNavigatorAmbientSourceContract, FollowGuardPrecedesFormationAndDoesNotMutateStrategies)
{
    std::filesystem::path const root = ModuleRoot();
    std::string const follow = ReadSource(root / "src/Ai/Base/Actions/FollowActions.cpp");
    std::string const policy = ReadSource(
        root / "src/Ai/Dungeon/Generic/DungeonNavigatorAmbientPolicy.h");
    std::string const walk = ReadSource(root / "src/AutoWow/DungeonPathWalkAction.h");
    ASSERT_FALSE(follow.empty());
    ASSERT_FALSE(policy.empty());
    ASSERT_FALSE(walk.empty());

    std::string_view const useful =
        FunctionBody(follow, "bool FollowAction::isUseful", "bool FleeToGroupLeaderAction::Execute");
    ASSERT_FALSE(useful.empty());
    std::size_t const gate = useful.find("ShouldSuppressOrdinaryFollow");
    std::size_t const formation = useful.find("AI_VALUE(Formation*, \"formation\")");
    ASSERT_NE(gate, std::string_view::npos);
    ASSERT_NE(formation, std::string_view::npos);
    EXPECT_LT(gate, formation);
    EXPECT_NE(useful.find("botAI->GetState(), BOT_STATE_NON_COMBAT"),
        std::string_view::npos);

    std::size_t const leaderCombatGate =
        policy.find("leaderAI->GetState() != nonCombatState");
    std::size_t const navigatorOwnership =
        policy.find("botAI->HasStrategy(\"dungeon navigator\", nonCombatState)", leaderCombatGate);
    ASSERT_NE(leaderCombatGate, std::string::npos);
    ASSERT_NE(navigatorOwnership, std::string::npos);
    EXPECT_LT(leaderCombatGate, navigatorOwnership);

    for (std::string_view forbidden : {"ChangeStrategy(", "addStrategy(", "removeStrategy(",
                                       "TeleportTo(", "NearTeleportTo("})
    {
        EXPECT_EQ(useful.find(forbidden), std::string_view::npos) << forbidden;
        EXPECT_EQ(policy.find(forbidden), std::string::npos) << forbidden;
    }

    EXPECT_NE(walk.find("MovementPriority::MOVEMENT_NORMAL"), std::string::npos);
    EXPECT_NE(walk.find("GetValue<LastMovement&>(\"last movement\")"), std::string::npos);
    EXPECT_NE(walk.find("reservationMs"), std::string::npos);
    EXPECT_LT(walk.find("GetValue<LastMovement&>(\"last movement\")"),
        walk.find("WaitForReach(probe.pathLength)"));
}

TEST(DungeonNavigatorAmbientSourceContract, GatesOnlyAmbientMovementAndLegacyGrindInRequiredOrder)
{
    std::filesystem::path const root = ModuleRoot();
    std::string const movement = ReadSource(root / "src/Ai/Base/Actions/MovementActions.cpp");
    std::string const grind = ReadSource(root / "src/Ai/Base/Value/GrindTargetValue.cpp");
    ASSERT_FALSE(movement.empty());
    ASSERT_FALSE(grind.empty());

    std::string_view const execute =
        FunctionBody(movement, "bool MoveRandomAction::Execute", "bool MoveRandomAction::isUseful");
    std::string_view const useful =
        FunctionBody(movement, "bool MoveRandomAction::isUseful", "bool MoveInsideAction::Execute");
    std::string_view const findTarget = FunctionBody(grind,
        "Unit* GrindTargetValue::FindTargetForGrinding", "bool GrindTargetValue::needForQuest");
    ASSERT_FALSE(execute.empty());
    ASSERT_FALSE(useful.empty());
    ASSERT_FALSE(findTarget.empty());

    constexpr std::string_view gate =
        "DungeonNavigatorAmbientPolicy::ShouldSuppress(bot, botAI, BOT_STATE_NON_COMBAT)";
    EXPECT_NE(execute.find(gate), std::string_view::npos);
    EXPECT_LT(execute.find(gate), execute.find("float distance ="));
    EXPECT_NE(useful.find(gate), std::string_view::npos);
    EXPECT_LT(useful.find(gate), useful.find("AI_VALUE(GuidPosition, \"rpg target\")"));
    EXPECT_EQ(CountOccurrences(movement, gate), 2u);
    EXPECT_EQ(CountOccurrences(grind, gate), 1u);

    std::size_t const selfDefense = findTarget.find("GuidVector attackers");
    std::size_t const selfDefenseReturn = findTarget.find("return unit;", selfDefense);
    std::size_t const objectiveSelection =
        findTarget.find("if (objectiveSpec.hasLock())", selfDefenseReturn);
    std::size_t const objectiveReturn = findTarget.find("return bestTarget;", objectiveSelection);
    std::size_t const ambientGate = findTarget.find(gate, objectiveReturn);
    std::size_t const legacySelection = findTarget.find("float distance = 0;", ambientGate);
    ASSERT_NE(selfDefense, std::string_view::npos);
    ASSERT_NE(selfDefenseReturn, std::string_view::npos);
    ASSERT_NE(objectiveSelection, std::string_view::npos);
    ASSERT_NE(objectiveReturn, std::string_view::npos);
    ASSERT_NE(ambientGate, std::string_view::npos);
    ASSERT_NE(legacySelection, std::string_view::npos);
    EXPECT_LT(selfDefenseReturn, objectiveSelection);
    EXPECT_LT(objectiveReturn, ambientGate);
    EXPECT_LT(ambientGate, legacySelection);
    EXPECT_NE(findTarget.substr(0, ambientGate).find("QuestActionPhase::LootSource"),
        std::string_view::npos);

    std::string const policy =
        ReadSource(root / "src/Ai/Dungeon/Generic/DungeonNavigatorAmbientPolicy.h");
    ASSERT_FALSE(policy.empty());
    EXPECT_NE(policy.find("bot->IsInWorld()"), std::string::npos);
    EXPECT_NE(policy.find("map->IsDungeon()"), std::string::npos);
    EXPECT_NE(policy.find("HasStrategy(\"dungeon navigator\", nonCombatState)"),
        std::string::npos);

    std::string const guardedScope =
        policy + std::string(execute) + std::string(useful) + std::string(findTarget);
    for (std::string_view forbidden : {"TeleportTo(", "NearTeleportTo(", "CharacterDatabase",
                                       "WorldDatabase", "LoginDatabase", "addStrategy(",
                                       "removeStrategy(", "ChangeStrategy("})
    {
        EXPECT_EQ(guardedScope.find(forbidden), std::string::npos) << forbidden;
    }
}
