/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadModuleSource(std::string const& relative)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relative);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST(PersistentRosterRecoveryContractTest, ExactLeagueRosterCannotAutonomouslyLeave)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/LeaveGroupAction.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const guard = source.find("AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter())");
    ASSERT_NE(guard, std::string::npos);
    EXPECT_NE(source.find("return false;", guard), std::string::npos);
    EXPECT_LT(guard, source.find("uint32 dCount", guard));
}

TEST(PersistentRosterRecoveryContractTest, RepeatedLeagueDeathsEscalateToWalkingSpiritHealer)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/ReviveFromCorpseAction.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const findCorpse = source.find("bool FindCorpseAction::Execute");
    std::size_t const policySnapshot = source.find(
        "persistentNoTeleport = AutoWowPolicy::IsNoTeleport", findCorpse);
    std::size_t const repeatedDeath = source.find("if (dCount >= 5)", policySnapshot);
    std::size_t const leagueGuard = source.find("if (persistentNoTeleport)", repeatedDeath);
    std::size_t const spiritHealer = source.find("persistent corpse recovery", leagueGuard);
    std::size_t const legacyRevive = source.find("sRandomPlayerbotMgr.Revive(bot)", repeatedDeath);
    ASSERT_NE(findCorpse, std::string::npos);
    ASSERT_NE(policySnapshot, std::string::npos);
    ASSERT_NE(repeatedDeath, std::string::npos);
    ASSERT_NE(leagueGuard, std::string::npos);
    ASSERT_NE(spiritHealer, std::string::npos);
    ASSERT_NE(legacyRevive, std::string::npos);
    EXPECT_LT(policySnapshot, leagueGuard);
    EXPECT_LT(leagueGuard, legacyRevive);
    EXPECT_LT(spiritHealer, legacyRevive);
}

TEST(PersistentRosterRecoveryContractTest, LeagueSpiritHealerHasNoTeleportFallback)
{
    std::string const source = ReadModuleSource("src/Ai/Base/Actions/ReviveFromCorpseAction.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const execute = source.find("bool SpiritHealerAction::Execute");
    std::size_t const noTeleport = source.find("AutoWowPolicy::IsNoTeleport", execute);
    std::size_t const explicitFalse = source.find("teleport_fallback=false", noTeleport);
    std::size_t const legacyTeleport = source.find("bot->TeleportTo(ClosestGrave->Map", execute);
    ASSERT_NE(execute, std::string::npos);
    ASSERT_NE(noTeleport, std::string::npos);
    ASSERT_NE(explicitFalse, std::string::npos);
    ASSERT_NE(legacyTeleport, std::string::npos);
    EXPECT_LT(noTeleport, legacyTeleport);
    EXPECT_LT(explicitFalse, legacyTeleport);
}

TEST(PersistentRosterRecoveryContractTest, NoTeleportGuardsBothRandomTeleportOverloadsBeforeEffects)
{
    std::string const source = ReadModuleSource("src/Bot/RandomPlayerbotMgr.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const vectorOverload = source.find(
        "void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth)");
    std::size_t const vectorGuard = source.find("AutoWowPolicy::IsNoTeleport", vectorOverload);
    std::size_t const oracleGuard = source.find("AutoWowOracleRuntime::BlocksRandomTeleport", vectorOverload);
    std::size_t const vectorTeleport = source.find("bot->TeleportTo", vectorOverload);
    ASSERT_NE(vectorOverload, std::string::npos);
    ASSERT_NE(vectorGuard, std::string::npos);
    ASSERT_NE(oracleGuard, std::string::npos);
    ASSERT_NE(vectorTeleport, std::string::npos);
    EXPECT_LT(vectorGuard, oracleGuard);
    EXPECT_LT(vectorGuard, vectorTeleport);

    std::size_t const plainOverload =
        source.find("void RandomPlayerbotMgr::RandomTeleport(Player* bot)", vectorOverload);
    std::size_t const plainGuard = source.find("AutoWowPolicy::IsNoTeleport", plainOverload);
    std::size_t const perf = source.find("sPerfMonitor.start", plainOverload);
    std::size_t const update = source.find("bot->UpdatePosition", plainOverload);
    std::size_t const refresh = source.find("Refresh(bot)", plainOverload);
    ASSERT_NE(plainOverload, std::string::npos);
    ASSERT_NE(plainGuard, std::string::npos);
    EXPECT_LT(plainGuard, perf);
    EXPECT_LT(plainGuard, update);
    EXPECT_LT(plainGuard, refresh);
}

TEST(PersistentRosterRecoveryContractTest, NorthrendProductionAdapterIsExactAndFailsClosed)
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const loaded = source.find("static AutoWowTransports::LoadedTransportFacts LoadedFacts");
    std::size_t const goInfo = source.find("transport->GetGOInfo()", loaded);
    std::size_t const pathNodes = source.find("sTaxiPathNodesByPath", loaded);
    std::size_t const current = source.find("static AutoWowTransports::CurrentTransportFacts", loaded);
    std::size_t const progress = source.find("GetPathProgress()", current);
    std::size_t const frames = source.find("GetKeyFrames()", current);
    std::size_t const chain = source.find("static bool ChainStep", current);
    std::size_t const exactMatch =
        source.find("LoadedTransportMatches(*native, LoadedFacts(*native, candidate))", chain);
    std::size_t const nativeStop = source.find("AtNativeStop(*native", chain);
    std::size_t const instanceGuid = source.find("GetGUID().GetRawValue()", nativeStop);
    std::size_t const effective = source.find("EffectiveMode(northrendOwned", chain);
    std::size_t const dockTeleport = source.find("bot->TeleportTo(x.exitMap", chain);
    ASSERT_NE(loaded, std::string::npos);
    ASSERT_NE(goInfo, std::string::npos);
    ASSERT_NE(pathNodes, std::string::npos);
    ASSERT_NE(current, std::string::npos);
    ASSERT_NE(progress, std::string::npos);
    ASSERT_NE(frames, std::string::npos);
    ASSERT_NE(exactMatch, std::string::npos);
    ASSERT_NE(nativeStop, std::string::npos);
    ASSERT_NE(instanceGuid, std::string::npos);
    ASSERT_NE(effective, std::string::npos);
    ASSERT_NE(dockTeleport, std::string::npos);
    EXPECT_LT(goInfo, current);
    EXPECT_LT(pathNodes, current);
    EXPECT_LT(progress, chain);
    EXPECT_LT(frames, chain);
    EXPECT_LT(effective, dockTeleport);
    EXPECT_EQ(source.find("AddPassenger", chain), std::string::npos);
}

TEST(PersistentRosterRecoveryContractTest, NorthrendBlocksHubPortalAndSkipsTaxiTeaching)
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const travel = source.find("if (s.phase == Phase::Travel)");
    std::size_t const portalModeBlock = source.find("northrend_failed reason=portal_mode_blocked", travel);
    std::size_t const fallbackGate = source.find("if (transports && !arrivalRequired && !northrendEntry)", travel);
    std::size_t const hubTeleport = source.find("bot->TeleportTo(target.GetMapId()", fallbackGate);
    std::size_t const receiptGate =
        source.find("(!northrendEntry || AutoWowTransports::PassageComplete(chain))", travel);
    std::size_t const finish = source.find("finish(true);", travel);
    std::size_t const northrendFinish = source.find("if (northrendEntry)", finish);
    std::size_t const northrendReturn = source.find("return true;", northrendFinish);
    std::size_t const learnPhase = source.find("s.phase = Phase::LearnFp", finish);
    std::size_t const teach = source.find("SendLearnNewTaxiNode", learnPhase);
    ASSERT_NE(portalModeBlock, std::string::npos);
    ASSERT_NE(fallbackGate, std::string::npos);
    ASSERT_NE(hubTeleport, std::string::npos);
    ASSERT_NE(receiptGate, std::string::npos);
    ASSERT_NE(northrendFinish, std::string::npos);
    ASSERT_NE(northrendReturn, std::string::npos);
    ASSERT_NE(learnPhase, std::string::npos);
    ASSERT_NE(teach, std::string::npos);
    EXPECT_LT(portalModeBlock, fallbackGate);
    EXPECT_LT(fallbackGate, hubTeleport);
    EXPECT_LT(receiptGate, finish);
    EXPECT_LT(northrendFinish, learnPhase);
    EXPECT_LT(northrendReturn, learnPhase);
    EXPECT_LT(learnPhase, teach);
}

TEST(PersistentRosterRecoveryContractTest, NorthrendPhysicalArrivalAdaptersFeedAllLiveCustodyAndPositionFacts)
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    ASSERT_FALSE(source.empty());
    std::size_t const disembark = source.find("DisembarkFacts const disembark");
    std::size_t const anyTransport = source.find("bot->GetTransport() != nullptr", disembark);
    std::size_t const flight = source.find("bot->IsInFlight()", disembark);
    std::size_t const teleporting = source.find("bot->IsBeingTeleported()", disembark);
    std::size_t const liveZ = source.find("bot->GetPositionZ()", disembark);
    std::size_t const disembarkGate = source.find("PhysicalDisembark(*native, disembark", disembark);
    std::size_t const hubFacts = source.find("PhysicalHubFacts const hubFacts", disembarkGate);
    std::size_t const hubTransport = source.find("bot->GetTransport() != nullptr", hubFacts);
    std::size_t const hubFlight = source.find("bot->IsInFlight()", hubFacts);
    std::size_t const hubTeleporting = source.find("bot->IsBeingTeleported()", hubFacts);
    std::size_t const hubZ = source.find("bot->GetPositionZ()", hubFacts);
    std::size_t const hubGate = source.find("AtPhysicalNorthrendHub(s.route, hubFacts)", hubFacts);
    ASSERT_NE(disembark, std::string::npos);
    ASSERT_NE(anyTransport, std::string::npos);
    ASSERT_NE(flight, std::string::npos);
    ASSERT_NE(teleporting, std::string::npos);
    ASSERT_NE(liveZ, std::string::npos);
    ASSERT_NE(disembarkGate, std::string::npos);
    ASSERT_NE(hubFacts, std::string::npos);
    ASSERT_NE(hubTransport, std::string::npos);
    ASSERT_NE(hubFlight, std::string::npos);
    ASSERT_NE(hubTeleporting, std::string::npos);
    ASSERT_NE(hubZ, std::string::npos);
    ASSERT_NE(hubGate, std::string::npos);
    EXPECT_LT(anyTransport, disembarkGate);
    EXPECT_LT(flight, disembarkGate);
    EXPECT_LT(teleporting, disembarkGate);
    EXPECT_LT(liveZ, disembarkGate);
    EXPECT_LT(disembarkGate, hubFacts);
    EXPECT_LT(hubTransport, hubGate);
    EXPECT_LT(hubFlight, hubGate);
    EXPECT_LT(hubTeleporting, hubGate);
    EXPECT_LT(hubZ, hubGate);
}

TEST(PersistentRosterRecoveryContractTest, NorthrendOwnershipGuardDoesNotChangeSharedEscapeClassifier)
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    std::size_t const movement = source.find("static AutoWowDeathLoop::RelocationBlock MovementBlock");
    std::size_t const movable = source.find("static bool Movable", movement);
    std::size_t const north = source.find("static bool NorthrendControlBlocked", movable);
    std::size_t const activeUse = source.find("IsNorthrendEntry(s.route) && NorthrendControlBlocked", north);
    std::size_t const startUse = source.find("IsNorthrendEntry(*route) && NorthrendControlBlocked", activeUse);
    ASSERT_NE(movement, std::string::npos);
    ASSERT_NE(movable, std::string::npos);
    ASSERT_NE(north, std::string::npos);
    ASSERT_NE(activeUse, std::string::npos);
    ASSERT_NE(startUse, std::string::npos);
    EXPECT_EQ(source.substr(movement, movable - movement).find("HasRealPlayerMaster"), std::string::npos);
    EXPECT_LT(north, activeUse);
    EXPECT_LT(activeUse, startUse);
}
