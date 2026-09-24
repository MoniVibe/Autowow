/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "../src/Ai/Base/Actions/CorpseRouteRetryPolicy.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
using namespace CorpseRouteRetryPolicy;

RouteEndpoint LocalOutlandGrave()
{
    return {94, 530, -803.0f, 2703.0f, 106.8f};
}

std::string ReadModuleSource(std::string const& relative)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relative);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string FunctionBody(std::string const& source, std::string_view signature, std::string_view nextSignature)
{
    std::size_t const begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    std::size_t const end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}
}

TEST(CorpseRouteRetryPolicy, PikliLikeLargeVerticalSeparationPinsOneLocalGraveForDeath)
{
    float constexpr corpseZ = -0.476f;
    float constexpr ghostZ = 106.8f;
    ASSERT_GT(std::fabs(ghostZ - corpseZ), 107.0f);

    State state;
    DeathIdentity const death{9001, 123456};
    RouteEndpoint const local = LocalOutlandGrave();
    ASSERT_TRUE(state.BeginDeath(death, local));

    RouteEndpoint const driftingStartingZone{4, 0, -8949.0f, -132.0f, 83.5f};
    EXPECT_FALSE(state.BeginDeath(death, driftingStartingZone));
    EXPECT_EQ(state.Endpoint().graveId, local.graveId);
    EXPECT_EQ(state.Endpoint().mapId, 530u);
    EXPECT_FLOAT_EQ(state.Endpoint().z, ghostZ);
}

TEST(CorpseRouteRetryPolicy, StartingZoneFallbackOnAnotherMapIsNotLocal)
{
    RouteEndpoint const local = LocalOutlandGrave();
    RouteEndpoint const startingZone{4, 0, -8949.0f, -132.0f, 83.5f};

    EXPECT_TRUE(local.IsLocalTo(530));
    EXPECT_FALSE(startingZone.IsLocalTo(530));
}

TEST(CorpseRouteRetryPolicy, DuplicateInProgressAndWaitingMovementDoNotConsumeRetries)
{
    State state;
    state.BeginDeath({9002, 123457}, LocalOutlandGrave());

    EXPECT_EQ(ClassifyPreflight(true, true, false, true), MovementObservation::Duplicate);
    EXPECT_EQ(ClassifyPreflight(true, false, true, true), MovementObservation::Waiting);
    EXPECT_EQ(ClassifyPreflight(true, false, false, true), MovementObservation::InProgress);
    EXPECT_EQ(ClassifyPreflight(false, false, false, false), MovementObservation::Waiting);

    state.Observe(MovementObservation::Duplicate);
    state.Observe(MovementObservation::InProgress);
    state.Observe(MovementObservation::Waiting);
    EXPECT_EQ(state.RouteAttempts(), 0u);
    EXPECT_EQ(state.RouteFailures(), 0u);
    EXPECT_FALSE(state.IsBlocked());
}

TEST(CorpseRouteRetryPolicy, GenuineRouteFailuresExhaustAtTheExactBound)
{
    State state;
    state.BeginDeath({9003, 123458}, LocalOutlandGrave());

    for (std::uint32_t failure = 1; failure < MaxRouteFailures; ++failure)
    {
        Transition const transition = state.Observe(MovementObservation::RouteFailure);
        EXPECT_FALSE(transition.blocked);
        EXPECT_FALSE(transition.emitTerminalReceipt);
        EXPECT_EQ(state.RouteAttempts(), failure);
        EXPECT_EQ(state.RouteFailures(), failure);
    }

    Transition const terminal = state.Observe(MovementObservation::RouteFailure);
    EXPECT_TRUE(terminal.blocked);
    EXPECT_TRUE(terminal.emitTerminalReceipt);
    EXPECT_EQ(state.RouteAttempts(), MaxRouteFailures);
    EXPECT_EQ(state.RouteFailures(), MaxRouteFailures);
    EXPECT_EQ(RetryDelayMs(MaxRouteFailures + 100), MaxRouteFailures * WaitingRetryDelayMs);
}

TEST(CorpseRouteRetryPolicy, AcceptedRouteThatStopsShortConsumesOneAttemptAndOneFailure)
{
    State state;
    state.BeginDeath({9006, 123461}, LocalOutlandGrave());

    state.Observe(MovementObservation::RouteStarted);
    EXPECT_EQ(state.RouteAttempts(), 1u);
    EXPECT_EQ(state.RouteFailures(), 0u);
    ASSERT_TRUE(state.IsRouteInProgress());

    state.Observe(MovementObservation::RouteFailure);
    EXPECT_EQ(state.RouteAttempts(), 1u);
    EXPECT_EQ(state.RouteFailures(), 1u);
    EXPECT_FALSE(state.IsRouteInProgress());
}

TEST(CorpseRouteRetryPolicy, TerminalReceiptIsExactlyOnceAndStateStaysFailClosed)
{
    State state;
    state.BeginDeath({9004, 123459}, LocalOutlandGrave());

    std::uint32_t receiptCount = 0;
    for (std::uint32_t i = 0; i < MaxRouteFailures + 5; ++i)
    {
        Transition const transition = state.Observe(MovementObservation::RouteFailure);
        receiptCount += transition.emitTerminalReceipt ? 1u : 0u;
    }

    EXPECT_EQ(receiptCount, 1u);
    EXPECT_TRUE(state.IsBlocked());
    EXPECT_TRUE(state.HasEmittedTerminalReceipt());
    EXPECT_EQ(state.RouteAttempts(), MaxRouteFailures);
    EXPECT_EQ(state.RouteFailures(), MaxRouteFailures);
}

TEST(CorpseRouteRetryPolicy, NormalRouteAndSpiritHealerSuccessRemainAvailable)
{
    State state;
    state.BeginDeath({9005, 123460}, LocalOutlandGrave());

    state.Observe(MovementObservation::RouteStarted);
    EXPECT_EQ(state.RouteAttempts(), 1u);
    EXPECT_EQ(state.RouteFailures(), 0u);
    EXPECT_TRUE(state.IsRouteInProgress());

    Transition const success = state.Observe(MovementObservation::RecoverySucceeded);
    EXPECT_TRUE(success.recoverySucceeded);
    EXPECT_TRUE(state.IsComplete());
    EXPECT_FALSE(state.IsBlocked());
    EXPECT_EQ(state.RouteFailures(), 0u);
}

TEST(CorpseRouteRetrySourceContract, ProtectedPathBansTeleportRandomRelogDatabaseAndFabricatedRevive)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/ReviveFromCorpseAction.cpp");
    ASSERT_FALSE(source.empty());

    std::string const protectedPath = FunctionBody(source,
        "bool SpiritHealerAction::ExecuteNoTeleportCorpseRecovery",
        "bool SpiritHealerAction::TrySpiritHealerInteraction");
    ASSERT_FALSE(protectedPath.empty());

    // Owner ruling (2026-09-24): a logged portal fallback is acceptable. The one flag-gated block
    // (AutoWow.Survival.CorpsePortal, default off) is the only place the banned calls may appear.
    std::string scanned = protectedPath;
    std::size_t const portalBegin = scanned.find("// corpse-portal-begin");
    std::size_t const portalEnd = scanned.find("// corpse-portal-end");
    if (portalBegin != std::string::npos && portalEnd != std::string::npos && portalBegin < portalEnd)
    {
        EXPECT_NE(scanned.find("AutoWow.Survival.CorpsePortal", portalBegin), std::string::npos);
        scanned.erase(portalBegin, portalEnd - portalBegin);
    }

    for (std::string_view forbidden : {"TeleportTo(", "NearTeleportTo(", "RandomTeleport", "MoveRandom",
             "urand(", "frand(", "rand_norm", "Relog", "CharacterDatabase", "WorldDatabase", "SaveToDB(",
             "ResurrectPlayer(", "SpawnCorpseBones(", "death count"})
        EXPECT_EQ(scanned.find(forbidden), std::string::npos) << forbidden;

    EXPECT_NE(protectedPath.find("GetClosestGraveyard(bot, bot->GetTeamId())"), std::string::npos);
    EXPECT_NE(protectedPath.find("corpse_recovery_blocked_no_path"), std::string::npos);
    EXPECT_EQ(source.find("corpse_recovery_blocked_no_path"), source.rfind("corpse_recovery_blocked_no_path"));
    EXPECT_EQ(std::count(source.begin(), source.end(), '\0'), 0);
    std::size_t const blocked = protectedPath.find("corpseRouteState_.IsBlocked()");
    std::size_t const interaction = protectedPath.find("TrySpiritHealerInteraction");
    ASSERT_NE(blocked, std::string::npos);
    ASSERT_NE(interaction, std::string::npos);
    EXPECT_LT(blocked, interaction);
}

TEST(CorpseRouteRetrySourceContract, LegacyNoTeleportGuardsAndNormalInteractionRemainIntact)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/ReviveFromCorpseAction.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const findCorpse = source.find("bool FindCorpseAction::Execute");
    std::size_t const nextAction = source.find("bool FindCorpseAction::isUseful", findCorpse);
    std::string const findCorpseBody = source.substr(findCorpse, nextAction - findCorpse);
    EXPECT_NE(findCorpseBody.find("corpse->GetMapId() != bot->GetMapId()"), std::string::npos);
    EXPECT_NE(findCorpseBody.find("persistent cross-map corpse recovery"), std::string::npos);
    EXPECT_NE(findCorpseBody.find("persistent corpse route timeout"), std::string::npos);
    std::size_t const persistentGuard = findCorpseBody.find("if (persistentNoTeleport)");
    std::size_t const legacyCorpseTeleport = findCorpseBody.find("bot->TeleportTo(moveToPos.GetMapId()");
    ASSERT_NE(persistentGuard, std::string::npos);
    ASSERT_NE(legacyCorpseTeleport, std::string::npos);
    EXPECT_LT(persistentGuard, legacyCorpseTeleport);

    std::size_t const repeatedDeath = source.find("if (dCount >= 5)", findCorpse);
    std::size_t const guard = source.find("if (persistentNoTeleport)", repeatedDeath);
    std::size_t const legacyRandomRevive = source.find("sRandomPlayerbotMgr.Revive(bot)", repeatedDeath);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(legacyRandomRevive, std::string::npos);
    EXPECT_LT(guard, legacyRandomRevive);

    std::size_t const execute = source.find("bool SpiritHealerAction::Execute");
    std::size_t const executeGuard = source.find("AutoWowPolicy::IsNoTeleport", execute);
    std::size_t const protectedCall = source.find("ExecuteNoTeleportCorpseRecovery(corpse)", executeGuard);
    std::size_t const legacyTeleport = source.find("bot->TeleportTo(ClosestGrave->Map", executeGuard);
    ASSERT_NE(executeGuard, std::string::npos);
    ASSERT_NE(protectedCall, std::string::npos);
    ASSERT_NE(legacyTeleport, std::string::npos);
    EXPECT_LT(protectedCall, legacyTeleport);

    std::string const interaction = FunctionBody(source,
        "bool SpiritHealerAction::TrySpiritHealerInteraction",
        "bool SpiritHealerAction::isUseful");
    EXPECT_NE(interaction.find("UNIT_NPC_FLAG_SPIRITHEALER"), std::string::npos);
    EXPECT_NE(interaction.find("bot->ResurrectPlayer(0.5f)"), std::string::npos);
    EXPECT_NE(interaction.find("bot->SpawnCorpseBones()"), std::string::npos);
}

TEST(CorpseRouteRetrySourceContract, PersistedRemoteCorpseLocationUsesOrdinarySpiritHealerRoute)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/ReviveFromCorpseAction.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const findCorpse = source.find("bool FindCorpseAction::Execute");
    std::size_t const remoteLocation = source.find(
        "!corpse && persistentNoTeleport && bot->HasCorpse()", findCorpse);
    std::size_t const corpseLocation = source.find("bot->GetCorpseLocation().GetMapId()", remoteLocation);
    std::size_t const normalRecovery = source.find(
        "persistent cross-map corpse-location recovery", corpseLocation);
    std::size_t const nullReturn = source.find("if (!corpse)\n        return false;", normalRecovery);
    ASSERT_NE(findCorpse, std::string::npos);
    ASSERT_NE(remoteLocation, std::string::npos);
    ASSERT_NE(corpseLocation, std::string::npos);
    ASSERT_NE(normalRecovery, std::string::npos);
    ASSERT_NE(nullReturn, std::string::npos);
    EXPECT_LT(normalRecovery, nullReturn);

    std::size_t const spiritHealer = source.find("bool SpiritHealerAction::Execute");
    std::size_t const persistedFallback = source.find(
        "ExecuteNoTeleportCorpseRecovery(nullptr)", spiritHealer);
    std::size_t const protectedBody = source.find(
        "bool SpiritHealerAction::ExecuteNoTeleportCorpseRecovery", persistedFallback);
    ASSERT_NE(persistedFallback, std::string::npos);
    ASSERT_NE(protectedBody, std::string::npos);
    EXPECT_LT(persistedFallback, protectedBody);
}
