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
