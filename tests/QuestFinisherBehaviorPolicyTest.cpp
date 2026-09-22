#include "../src/Ai/World/Rpg/StrictFinisherMovementPolicy.h"
#include "../src/Ai/World/Rpg/Action/NewRpgAction.h"
#include "../src/AutoWow/AutoWowOracleRuntime.h"
#include "../src/AutoWow/OracleQuestDispatchPolicy.h"

#include "gtest/gtest.h"

#include <array>

namespace
{
namespace Dispatch = AutoWowOracleQuestDispatchPolicy;
    namespace StrictMovement = StrictFinisherMovementPolicy;
}

TEST(QuestFinisherBehaviorPolicyTest, InitiallyUnleasedCompletedQuestRejectsUntaggedBridgeEvent)
{
    Dispatch::OwnershipState const initiallyUnleased{true, false, false, false, 0};
    Dispatch::EventState const untaggedCompletedQuest{
        false, false, 0, Dispatch::WorkKind::Finisher, 1978};

    EXPECT_EQ(Dispatch::Evaluate(initiallyUnleased, untaggedCompletedQuest), Dispatch::Result::Reject);
    EXPECT_FALSE(Dispatch::AllowsLegacyQuestMaintenance(
        Dispatch::Evaluate(initiallyUnleased, untaggedCompletedQuest)));
}

TEST(QuestFinisherBehaviorPolicyTest, UnmanagedDirectiveRetainsLegacyQuestMaintenance)
{
    Dispatch::OwnershipState const unmanaged{};
    Dispatch::EventState const ordinary{};
    Dispatch::Result const result = Dispatch::Evaluate(unmanaged, ordinary);

    EXPECT_EQ(result, Dispatch::Result::Ordinary);
    EXPECT_TRUE(Dispatch::AllowsLegacyQuestMaintenance(result));
    EXPECT_FALSE(Dispatch::AllowsLegacyQuestMaintenance(Dispatch::Result::Reject));
    EXPECT_FALSE(Dispatch::AllowsLegacyQuestMaintenance(Dispatch::Result::OracleTagged));
}

TEST(QuestFinisherBehaviorPolicyTest, ActiveLeaseRequiresExactTaggedDecision)
{
    Dispatch::OwnershipState const owned = Dispatch::Acquired(true, 42);
    EXPECT_EQ(Dispatch::Evaluate(owned, {false, false, 0, Dispatch::WorkKind::Objective, 0}),
              Dispatch::Result::Reject);
    EXPECT_EQ(Dispatch::Evaluate(owned, {true, false, 0, Dispatch::WorkKind::Objective, 0}),
              Dispatch::Result::Reject);
    EXPECT_EQ(Dispatch::Evaluate(owned, {true, true, 41, Dispatch::WorkKind::Objective, 0}),
              Dispatch::Result::Reject);
    EXPECT_EQ(Dispatch::Evaluate(owned, {true, true, 42, Dispatch::WorkKind::Objective, 0}),
              Dispatch::Result::OracleTagged);
}

TEST(QuestFinisherBehaviorPolicyTest, StalePhaseTargetOrFinisherIdentityRequiresReplan)
{
    using AutoWowOracleRuntime::IntentIdentity;
    using AutoWowOracleRuntime::SameIntentIdentity;

    IntentIdentity const current{true, true, false, 2520, 12, 0, 0, 1978, 0, 111, 1978, 987};
    EXPECT_TRUE(SameIntentIdentity(current, current));

    IntentIdentity stale = current;
    stale.phase++;
    EXPECT_FALSE(SameIntentIdentity(current, stale));
    stale = current;
    stale.targetGuid++;
    EXPECT_FALSE(SameIntentIdentity(current, stale));
    stale = current;
    stale.finisherIsGameObject = true;
    EXPECT_FALSE(SameIntentIdentity(current, stale));
    stale = current;
    stale.finisherSignedEntry = -1978;
    EXPECT_FALSE(SameIntentIdentity(current, stale));
    stale = current;
    stale.finisherStableSpawnGuid++;
    EXPECT_FALSE(SameIntentIdentity(current, stale));
}

TEST(QuestFinisherBehaviorPolicyTest, PreExistingRandomMovementIsRejectedUntilCurrentStrictRouteIssuesIt)
{
    StrictMovement::RouteIdentity const current{42, 2520, 987};
    StrictMovement::LastMovementFacts const randomMovement{true, 100, 1, 10.0f, 20.0f, 30.0f};
    StrictMovement::Provenance none;
    EXPECT_EQ(StrictMovement::Evaluate(current, randomMovement, none),
              StrictMovement::Decision::RejectAndClearInherited);

    StrictMovement::Provenance stale{
        true, {41, 2520, 987}, {true, 100, 1, 10.0f, 20.0f, 30.0f}};
    EXPECT_EQ(StrictMovement::Evaluate(current, randomMovement, stale),
              StrictMovement::Decision::RejectAndClearInherited);

    StrictMovement::Provenance exact{
        true, current, {true, 100, 1, 10.0f, 20.0f, 30.0f}};
    EXPECT_EQ(StrictMovement::Evaluate(current, randomMovement, exact),
              StrictMovement::Decision::ContinueCurrentStrictRoute);
}

TEST(QuestFinisherBehaviorPolicyTest, PreparedQuestWalkHandoffSurvivesTheNextStrictTick)
{
    StrictMovement::RouteIdentity const route{77, 8336, 17379391218647095054ULL};
    StrictMovement::LastMovementFacts const preparedSegment{
        true, 123456, 530, 10429.1f, -6366.14f, 38.1402f};
    StrictMovement::Provenance const prepared{true, route, preparedSegment};

    // WalkPrepared executes an intermediate, freshly-probed segment while the strict route
    // remains authoritative. The next MoveFarTo tick must inherit that segment instead of
    // clearing it as unrelated movement.
    EXPECT_EQ(StrictMovement::Evaluate(route, preparedSegment, prepared),
              StrictMovement::Decision::ContinueCurrentStrictRoute);
}

TEST(QuestFinisherBehaviorPolicyTest, LoadedStableFinisherTargetRequiresEveryIdentityGate)
{
    using AutoWowQuestFinisher::IsExactLoadedStableTarget;
    using AutoWowQuestFinisher::LoadedStableTargetFacts;

    LoadedStableTargetFacts valid{
        true, true, true, true, true, true, true, true, true};
    EXPECT_TRUE(IsExactLoadedStableTarget(valid));

    struct Gate
    {
        char const* name;
        void (*disable)(LoadedStableTargetFacts&);
    };
    std::array<Gate, 9> const gates{
        Gate{"present", [](LoadedStableTargetFacts& facts) { facts.present = false; }},
        Gate{"in_world", [](LoadedStableTargetFacts& facts) { facts.inWorld = false; }},
        Gate{"usable", [](LoadedStableTargetFacts& facts) { facts.usable = false; }},
        Gate{"map", [](LoadedStableTargetFacts& facts) { facts.sameMap = false; }},
        Gate{"instance", [](LoadedStableTargetFacts& facts) { facts.sameInstance = false; }},
        Gate{"entry", [](LoadedStableTargetFacts& facts) { facts.exactEntry = false; }},
        Gate{"spawn", [](LoadedStableTargetFacts& facts) { facts.exactSpawn = false; }},
        Gate{"runtime_guid_present", [](LoadedStableTargetFacts& facts) { facts.runtimeGuidPresent = false; }},
        Gate{"family", [](LoadedStableTargetFacts& facts) { facts.exactFamily = false; }},
    };

    for (Gate const& gate : gates)
    {
        LoadedStableTargetFacts rejected = valid;
        gate.disable(rejected);
        EXPECT_FALSE(IsExactLoadedStableTarget(rejected)) << gate.name;
    }
}

TEST(QuestFinisherBehaviorPolicyTest, StableFinisherTargetDoesNotAdmitACloserSameEntrySubstitute)
{
    using AutoWowQuestFinisher::IsExactLoadedStableTarget;
    using AutoWowQuestFinisher::LoadedStableTargetFacts;

    LoadedStableTargetFacts sameEntryWrongSpawn{
        true, true, true, true, true, true, false, true, true};
    EXPECT_FALSE(IsExactLoadedStableTarget(sameEntryWrongSpawn));

    LoadedStableTargetFacts sameSpawnWrongFamily{
        true, true, true, true, true, true, true, true, false};
    EXPECT_FALSE(IsExactLoadedStableTarget(sameSpawnWrongFamily));
}

TEST(QuestFinisherBehaviorPolicyTest, StableDbSpawnBindsDifferentRuntimeGuid)
{
    using AutoWowQuestFinisher::MatchesStableSpawn;
    using AutoWowQuestFinisher::StableSpawnIdentity;
    StableSpawnIdentity const expected{530, 0, 1234, 98765, false};
    StableSpawnIdentity const live{530, 0, 1234, 98765, false};
    // Runtime GUID 0xF130...0042 has a distinct generated low counter from DB spawn 98765.
    EXPECT_TRUE(MatchesStableSpawn(expected, live, 0xF13004D200000042ULL));
    EXPECT_FALSE(MatchesStableSpawn(expected, live, 0));
    StableSpawnIdentity wrongSpawn = live;
    wrongSpawn.spawnId = 98766;
    EXPECT_FALSE(MatchesStableSpawn(expected, wrongSpawn, 0xF13004D200000042ULL));
    StableSpawnIdentity wrongInstance = live;
    wrongInstance.instanceId = 1;
    EXPECT_FALSE(MatchesStableSpawn(expected, wrongInstance, 0xF13004D200000042ULL));
    StableSpawnIdentity wrongFamily = live;
    wrongFamily.gameObject = true;
    EXPECT_FALSE(MatchesStableSpawn(expected, wrongFamily, 0xF13004D200000042ULL));
}

TEST(QuestFinisherBehaviorPolicyTest, StrictFinisherMovementIsExplicitlyOracleOptIn)
{
    EXPECT_FALSE(StrictMovement::IsEnabled(false, 0));
    EXPECT_FALSE(StrictMovement::IsEnabled(false, 42));
    EXPECT_FALSE(StrictMovement::IsEnabled(true, 0));
    EXPECT_TRUE(StrictMovement::IsEnabled(true, 42));
    EXPECT_FALSE(StrictMovement::UseQuestNoTeleport(false, 0));
    EXPECT_FALSE(StrictMovement::UseQuestNoTeleport(false, 42));
    EXPECT_FALSE(StrictMovement::UseQuestNoTeleport(true, 0));
    EXPECT_TRUE(StrictMovement::UseQuestNoTeleport(true, 42));
    EXPECT_TRUE(StrictMovement::AllowLegacyTeleportRecovery(false, 0));
    EXPECT_TRUE(StrictMovement::AllowLegacyTeleportRecovery(false, 42));
    EXPECT_TRUE(StrictMovement::AllowLegacyTeleportRecovery(true, 0));
    EXPECT_FALSE(StrictMovement::AllowLegacyTeleportRecovery(true, 42));
}

TEST(QuestFinisherBehaviorPolicyTest, TerminalReleaseClearsAuthorityAndRemainsFailClosed)
{
    Dispatch::OwnershipState const terminal = Dispatch::TerminalRelease(Dispatch::Acquired(true, 42));
    EXPECT_TRUE(terminal.managed);
    EXPECT_FALSE(terminal.leaseRequired);
    EXPECT_FALSE(terminal.activeLease);
    EXPECT_FALSE(terminal.gateOwned);
    EXPECT_EQ(terminal.activeDecisionId, 0U);
    EXPECT_EQ(Dispatch::Evaluate(terminal, {true, true, 42, Dispatch::WorkKind::Finisher, 1978}),
              Dispatch::Result::Reject);
    EXPECT_EQ(Dispatch::Evaluate(terminal, {false, false, 0, Dispatch::WorkKind::Finisher, 1978}),
              Dispatch::Result::Reject);
}

TEST(QuestFinisherBehaviorPolicyTest, TwoMembersDispatchCreatureAndNegativeEntryGameObjectFinishers)
{
    Dispatch::OwnershipState const creatureMember = Dispatch::Acquired(true, 101);
    Dispatch::OwnershipState const gameObjectMember = Dispatch::Acquired(true, 202);
    Dispatch::EventState const creatureFinisher{
        true, true, 101, Dispatch::WorkKind::Finisher, 1978};
    Dispatch::EventState const gameObjectFinisher{
        true, true, 202, Dispatch::WorkKind::Finisher, -1978};

    EXPECT_EQ(Dispatch::Evaluate(creatureMember, creatureFinisher), Dispatch::Result::OracleTagged);
    EXPECT_EQ(Dispatch::Evaluate(gameObjectMember, gameObjectFinisher), Dispatch::Result::OracleTagged);
}
