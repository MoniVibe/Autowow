/*
 * Pure contracts for exact quest finisher wire identity and lease identity.
 *
 * Production behavior is covered by QuestFinisherBehaviorPolicyTest. This file deliberately avoids
 * source-text assertions, which become obsolete when equivalent production policy is refactored.
 */

#include "AutoWowOracleFinisherIntent.h"
#include "AutoWowOracleRuntime.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gtest/gtest.h"

namespace
{
std::string ReadModuleSource(std::string const& relativePath)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST(QuestFinisherIntentContractTest, RoundTripsExactTaggedIdentity)
{
    using namespace AutoWowOracleFinisher;
    Intent const expected{42, 2520, -1978, 987654321};
    Intent actual;
    ASSERT_TRUE(Parse(Encode(expected), actual));
    EXPECT_EQ(actual.decisionId, expected.decisionId);
    EXPECT_EQ(actual.questId, expected.questId);
    EXPECT_EQ(actual.signedEntry, expected.signedEntry);
    EXPECT_EQ(actual.stableSpawnGuid, expected.stableSpawnGuid);
}

TEST(QuestFinisherIntentContractTest, RejectsMalformedOrAmbiguousIdentity)
{
    using namespace AutoWowOracleFinisher;
    Intent parsed;
    EXPECT_FALSE(Parse("finisher:42:2520:-1978:987:extra", parsed));
    EXPECT_FALSE(Parse("finisher:0:2520:-1978:987", parsed));
    EXPECT_FALSE(Parse("finisher:42:0:-1978:987", parsed));
    EXPECT_FALSE(Parse("finisher:42:2520:0:987", parsed));
    EXPECT_FALSE(Parse("finisher:42:2520:-1978:0", parsed));
    EXPECT_FALSE(Parse("finisher:42:2520:-1978:-1", parsed));
    EXPECT_FALSE(Parse("oracle:42:2520:-1978:987", parsed));
}

TEST(QuestFinisherIntentContractTest, AnyExactIdentityDriftRequiresReplan)
{
    using namespace AutoWowOracleRuntime;
    IntentIdentity const base{true, true, false, 2520, 14, 0, 0, 1978, 0, 111, 1978, 987654321};
    EXPECT_TRUE(SameIntentIdentity(base, base));

    IntentIdentity changed = base;
    changed.phase++;
    EXPECT_FALSE(SameIntentIdentity(base, changed));
    changed = base;
    changed.finisherIsGameObject = true;
    EXPECT_FALSE(SameIntentIdentity(base, changed));
    changed = base;
    changed.finisherSignedEntry = -1978;
    EXPECT_FALSE(SameIntentIdentity(base, changed));
    changed = base;
    changed.finisherStableSpawnGuid++;
    EXPECT_FALSE(SameIntentIdentity(base, changed));
    changed = base;
    changed.targetGuid++;
    EXPECT_FALSE(SameIntentIdentity(base, changed));
}

TEST(QuestFinisherBranchWiringContractTest, LegacyMaintenanceOccursOnlyAfterAuthorityDecision)
{
    std::string const action = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::size_t const authority = action.find("AutoWowOracleQuestDispatchPolicy::Evaluate(ownership, dispatchEvent)");
    std::size_t const legacyGate = action.find("AllowsLegacyQuestMaintenance(authority)", authority);
    std::size_t const legacyCall = action.find("SearchQuestGiverAndAcceptOrReward()", legacyGate);

    ASSERT_NE(authority, std::string::npos);
    ASSERT_NE(legacyGate, std::string::npos);
    ASSERT_NE(legacyCall, std::string::npos);
    EXPECT_LT(authority, legacyGate);
    EXPECT_LT(legacyGate, legacyCall);
}

TEST(QuestFinisherBranchWiringContractTest, OracleOptInControlsStrictAndNoTeleportArguments)
{
    std::string const action = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    EXPECT_NE(action.find("IsEnabled(\n                rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId)"),
              std::string::npos);
    EXPECT_NE(action.find("UseQuestNoTeleport(\n                rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId)"),
              std::string::npos);
    EXPECT_NE(action.find("AllowLegacyTeleportRecovery(\n                    rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId)"),
              std::string::npos);
    EXPECT_NE(action.find("/*questNoTeleport*/ oracleQuestNoTeleport"), std::string::npos);
    EXPECT_NE(action.find("/*deterministicPath*/ strictOracleRoute"), std::string::npos);
    EXPECT_NE(action.find("strictRoute,\n                                         allowLegacyTeleportRecovery"),
              std::string::npos);
}
