#include "../src/AutoWow/AutoWowOracleOwnershipGate.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <limits>
#include <string>

namespace
{
using AutoWowOracle::DecisionId;
using AutoWowOracle::Guid;
using AutoWowOracle::Tick;
using namespace AutoWowOracleRuntime;

constexpr Guid kBotGuid = 0xA000000000000001ULL;

Tick TestBaseTick()
{
    return CurrentTick();
}

AutoWowOracle::GatherSourceReference GatherSource(
    AutoWowOracle::NodeId spawnId = 9001,
    AutoWowOracle::GatherGoal goal = AutoWowOracle::GatherGoal::HarvestNode)
{
    return {true, spawnId, 7001, 1, 500, 9, goal};
}
}

TEST(AutoWowOracleOwnershipGateTest, ClaimOwnsAndPreemptsByBot)
{
    Release(kBotGuid);
    Tick const base = TestBaseTick();

    ASSERT_TRUE(Claim(kBotGuid, 101, base + 100));
    EXPECT_TRUE(IsOwned(kBotGuid, base + 1));
    EXPECT_TRUE(Owns(kBotGuid, 101, base + 1));

    ASSERT_TRUE(Claim(kBotGuid, 202, base + 200));
    EXPECT_FALSE(Owns(kBotGuid, 101, base + 1));
    EXPECT_TRUE(Owns(kBotGuid, 202, base + 1));

    EXPECT_TRUE(Release(kBotGuid));
    EXPECT_FALSE(IsOwned(kBotGuid, base + 1));
}

TEST(AutoWowOracleOwnershipGateTest, ExpiryIsExclusiveAndReapsTheSlot)
{
    Release(kBotGuid);
    Tick const base = TestBaseTick();
    Tick const expires = base + 2;

    ASSERT_TRUE(Claim(kBotGuid, 303, expires));
    EXPECT_TRUE(Owns(kBotGuid, 303, expires - 1));
    EXPECT_FALSE(Owns(kBotGuid, 303, expires));
    EXPECT_FALSE(IsOwned(kBotGuid, expires));
    EXPECT_FALSE(Release(kBotGuid));
}

TEST(AutoWowOracleOwnershipGateTest, ReleaseClearsOwnership)
{
    Release(kBotGuid);
    Tick const base = TestBaseTick();

    ASSERT_TRUE(Claim(kBotGuid, 404, base + 100));
    ASSERT_TRUE(Release(kBotGuid));
    EXPECT_FALSE(Owns(kBotGuid, 404, base + 1));
    EXPECT_FALSE(Release(kBotGuid));
}

TEST(AutoWowOracleOwnershipGateTest, TaggedDecisionParsingIsWholeAndDecimal)
{
    DecisionId parsed = 0;
    EXPECT_TRUE(ParseDecisionId("42", parsed));
    EXPECT_EQ(parsed, 42U);
    EXPECT_TRUE(ParseDecisionId("00042", parsed));
    EXPECT_EQ(parsed, 42U);

    EXPECT_FALSE(ParseDecisionId("", parsed));
    EXPECT_FALSE(ParseDecisionId("0", parsed));
    EXPECT_FALSE(ParseDecisionId("42x", parsed));
    EXPECT_FALSE(ParseDecisionId(" 42", parsed));
    EXPECT_FALSE(ParseDecisionId("42 ", parsed));
    EXPECT_FALSE(ParseDecisionId("+42", parsed));
    EXPECT_FALSE(ParseDecisionId(std::to_string(std::numeric_limits<DecisionId>::max()) + "0", parsed));
}

TEST(AutoWowOracleOwnershipGateTest, UnclaimedBotsRemainOrdinaryByDefault)
{
    Release(kBotGuid);
    EXPECT_FALSE(IsOwned(kBotGuid, CurrentTick()));
    EXPECT_FALSE(Owns(kBotGuid, 505, CurrentTick()));
    EXPECT_FALSE(TaggedEventSource().empty());
    EXPECT_EQ(TaggedEventSource(), "autowow.oracle");
    static_assert(kMaxOracleOwnedBots <= AutoWowOracle::kMaxBotLeases);
}

TEST(AutoWowOracleOwnershipGateTest, GatherOwnershipRequiresMatchingDecisionAndFullSource)
{
    Release(kBotGuid);
    Tick const base = TestBaseTick();
    ASSERT_TRUE(ClaimGather(kBotGuid, 606, GatherSource(), base + 100));

    EXPECT_TRUE(OwnsGatherSource(kBotGuid, 606, GatherSource(), base + 1));
    EXPECT_TRUE(OwnsGatherSource(kBotGuid, GatherSource(), base + 1));
    EXPECT_FALSE(OwnsGatherSource(kBotGuid, 607, GatherSource(), base + 1));
    EXPECT_FALSE(OwnsGatherSource(kBotGuid, 606, GatherSource(9002), base + 1));
    EXPECT_FALSE(OwnsGatherSource(
        kBotGuid, 606, GatherSource(9001, AutoWowOracle::GatherGoal::ObtainMaterial), base + 1));
    EXPECT_TRUE(Release(kBotGuid));
}
