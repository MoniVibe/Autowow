#include "OraclePvpCombatExecutor.h"

#include "gtest/gtest.h"

#include <type_traits>

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleExecutor;
using namespace AutoWowOraclePvpCombatExecutor;

constexpr Guid kBot = 42;
constexpr SquadId kSquad = 77;
constexpr Guid kTarget = 0xabc;
constexpr std::uint32_t kMap = 489;
constexpr std::uint32_t kInstance = 12;
constexpr std::uint32_t kActorFaction = 1;
constexpr std::uint32_t kTargetFaction = 2;
constexpr DecisionId kDecision = 101;
constexpr IntentId kIntent = 102;
constexpr Epoch kEpoch = 7;
constexpr FactVersion kFrame = 11;
constexpr FactVersion kTargetFacts = 21;
constexpr Tick kIssued = 100;
constexpr Tick kExpiry = 110;

HostilePlayerReference TargetReference()
{
    return {true, true, kTarget, kMap, kInstance, TeamRelation::Hostile,
            kActorFaction, kTargetFaction, kTargetFacts};
}

ActiveLeaseSnapshot Ownership(DecisionId decision = kDecision, IntentId intent = kIntent,
                              Epoch epoch = kEpoch, Tick expires = kExpiry)
{
    ActiveLeaseSnapshot proof;
    proof.active = true;
    proof.botGuid = kBot;
    proof.decisionId = decision;
    proof.intentId = intent;
    proof.epoch = epoch;
    proof.expiresTick = expires;
    proof.scope = {ScopeKind::LabFixture, kSquad, kBot};
    proof.operation = OperationCode::CombatAction;
    return proof;
}

IntentLease Lease()
{
    Decision decision;
    decision.valid = true;
    decision.decisionId = kDecision;
    decision.intentId = kIntent;
    decision.domain = Domain::Combat;
    decision.intent = IntentCode::CombatAction;
    decision.resource = LeaseResource::CombatPositioning;
    decision.scope = {ScopeKind::LabFixture, kSquad, kBot};
    decision.ttlTicks = 10;
    decision.issuedTick = kIssued;
    decision.expiresTick = kExpiry;
    decision.epoch = kEpoch;
    decision.targetGuid = kTarget;
    decision.actorGuid = kBot;
    decision.executorAvailable = true;
    decision.operation = OperationCode::CombatAction;
    decision.preconditions.frameVersion = kFrame;
    decision.preconditions.epoch = kEpoch;
    decision.preconditions.botGuid = kBot;
    decision.preconditions.squadId = kSquad;
    decision.preconditions.mapId = kMap;
    decision.preconditions.instanceId = kInstance;
    return {true, decision};
}

PvpCombatRequest Request()
{
    IntentLease const lease = Lease();
    PvpCombatRequest request;
    request.valid = true;
    request.requestId = 90001;
    request.operation = lease.decision.operation;
    request.domain = lease.decision.domain;
    request.intent = lease.decision.intent;
    request.resource = lease.decision.resource;
    request.scope = lease.decision.scope;
    request.decisionId = lease.decision.decisionId;
    request.intentId = lease.decision.intentId;
    request.actorGuid = lease.decision.actorGuid;
    request.executorAvailable = lease.decision.executorAvailable;
    request.frameVersion = lease.decision.preconditions.frameVersion;
    request.epoch = lease.decision.epoch;
    request.issuedTick = lease.decision.issuedTick;
    request.expiresTick = lease.decision.expiresTick;
    request.ttlTicks = lease.decision.ttlTicks;
    request.target = TargetReference();
    request.ownershipProof = Ownership();
    return request;
}

HostilePlayerState TargetState()
{
    HostilePlayerState state;
    state.isPlayer = true;
    state.targetGuid = kTarget;
    state.mapId = kMap;
    state.instanceId = kInstance;
    state.inWorld = true;
    state.alive = true;
    state.attackable = true;
    state.pvpPermitted = true;
    state.relation = TeamRelation::Hostile;
    state.actorFaction = kActorFaction;
    state.targetFaction = kTargetFaction;
    state.factVersion = kTargetFacts;
    return state;
}

HostilePlayerObservation Observation()
{
    HostilePlayerObservation observation;
    observation.world.onWorldThread = true;
    observation.world.actorAlive = true;
    observation.world.actorGuid = kBot;
    observation.world.mapId = kMap;
    observation.world.instanceId = kInstance;
    observation.world.tick = kIssued + 1;
    observation.world.frameVersion = kFrame;
    observation.world.epoch = kEpoch;
    observation.world.ownershipProof = Ownership();
    observation.target = TargetState();
    return observation;
}

HostilePlayerObservation PostObservation()
{
    HostilePlayerObservation observation = Observation();
    observation.world.tick = kIssued + 2;
    observation.world.frameVersion = kFrame + 1;
    observation.target.factVersion = kTargetFacts + 1;
    return observation;
}

struct NativeProbe
{
    int calls = 0;
    std::uint8_t maxSteps = 0;
    bool stopAfterStep = false;
    Guid exactTargetGuid = 0;
    bool accept = true;
    std::uint8_t stepsInvoked = 1;
    Guid selectedTargetGuid = 0;
    HostilePlayerObservation after = PostObservation();
};

NativeStepObservation ProbeStep(void* context, NativeStepRequest const& request)
{
    NativeProbe* probe = static_cast<NativeProbe*>(context);
    ++probe->calls;
    probe->maxSteps = request.maxSteps;
    probe->stopAfterStep = request.stopAfterStep;
    probe->exactTargetGuid = request.exactTarget.targetGuid;
    NativeStepObservation result;
    result.dispatchAccepted = probe->accept;
    result.stepsInvoked = probe->stepsInvoked;
    result.selectedTargetGuid = probe->selectedTargetGuid == 0
        ? request.exactTarget.targetGuid
        : probe->selectedTargetGuid;
    result.after = probe->after;
    return result;
}

DispatchResult Dispatch(NativeProbe& probe,
                        HostilePlayerObservation const& before = Observation(),
                        HostilePlayerReference const& exactTarget = TargetReference())
{
    return OraclePvpCombatExecutor::Dispatch(Request(), Lease(), exactTarget, before,
                                             &ProbeStep, &probe);
}

TEST(OraclePvpCombatExecutorTest, UsesTypedExactHostilePlayerReferenceAndLease)
{
    static_assert(std::is_same_v<decltype(PvpCombatRequest{}.target.targetGuid), Guid>);
    static_assert(std::is_same_v<decltype(IntentLease{}.decision), Decision>);

    PvpCombatRequest const request = Request();
    HostilePlayerReference const target = TargetReference();
    EXPECT_TRUE(request.valid);
    EXPECT_EQ(request.operation, OperationCode::CombatAction);
    EXPECT_EQ(request.target.targetGuid, kTarget);
    EXPECT_EQ(request.target.mapId, kMap);
    EXPECT_EQ(request.target.instanceId, kInstance);
    EXPECT_EQ(request.target.relation, TeamRelation::Hostile);
    EXPECT_EQ(request.target.actorFaction, kActorFaction);
    EXPECT_EQ(request.target.targetFaction, kTargetFaction);
    EXPECT_TRUE(ExactTargetMatches(target, Observation().target));
    EXPECT_TRUE(TargetFactsSafe(Observation().target));
}

TEST(OraclePvpCombatExecutorTest, ExactTargetMatcherRejectsIdentityAndFactionChanges)
{
    HostilePlayerReference const expected = TargetReference();
    HostilePlayerReference observed = expected;
    ASSERT_TRUE(ExactTargetMatches(expected, observed));

    observed.targetGuid++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.mapId++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.instanceId++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.relation = TeamRelation::Friendly;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.actorFaction++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.targetFaction++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));
    observed = expected;
    observed.factVersion++;
    EXPECT_FALSE(ExactTargetMatches(expected, observed));

    HostilePlayerState state = TargetState();
    state.targetGuid++;
    EXPECT_FALSE(ExactTargetMatches(expected, state));
    state = TargetState();
    state.factVersion++;
    EXPECT_FALSE(ExactTargetMatches(expected, state));
    EXPECT_TRUE(ExactTargetMatches(expected, state, true));
}

TEST(OraclePvpCombatExecutorTest, RejectsInvalidOperationAndTargetBeforeNative)
{
    auto expectRejected = [](PvpCombatRequest request, PvpCombatReason reason)
    {
        NativeProbe probe;
        DispatchResult const result = OraclePvpCombatExecutor::Dispatch(
            request, Lease(), TargetReference(), Observation(), &ProbeStep, &probe);
        EXPECT_FALSE(result.accepted);
        EXPECT_EQ(result.reason, reason);
        EXPECT_EQ(probe.calls, 0);
    };

    PvpCombatRequest request = Request();
    request.valid = false;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.operation = OperationCode::PvPObjective;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.domain = Domain::PvP;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.intent = IntentCode::PvPObjective;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.resource = LeaseResource::QuestGather;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.executorAvailable = false;
    expectRejected(request, PvpCombatReason::InvalidRequest);
    request = Request();
    request.target.targetGuid = 0;
    expectRejected(request, PvpCombatReason::InvalidRequest);

    NativeProbe probe;
    HostilePlayerReference wrongTarget = TargetReference();
    wrongTarget.targetGuid++;
    DispatchResult const mismatch = OraclePvpCombatExecutor::Dispatch(
        Request(), Lease(), wrongTarget, Observation(), &ProbeStep, &probe);
    EXPECT_FALSE(mismatch.accepted);
    EXPECT_EQ(mismatch.reason, PvpCombatReason::TargetReferenceMismatch);
    EXPECT_EQ(probe.calls, 0);
}

TEST(OraclePvpCombatExecutorTest, RequiresFreshWorldAndOwnershipProof)
{
    auto expectRejected = [](HostilePlayerObservation observation, PvpCombatReason reason)
    {
        NativeProbe probe;
        DispatchResult const result = Dispatch(probe, observation);
        EXPECT_FALSE(result.accepted);
        EXPECT_EQ(result.reason, reason);
        EXPECT_EQ(probe.calls, 0);
    };

    HostilePlayerObservation observation = Observation();
    observation.world.onWorldThread = false;
    expectRejected(observation, PvpCombatReason::NotWorldThread);
    observation = Observation();
    observation.world.ownershipProof.active = false;
    expectRejected(observation, PvpCombatReason::MissingOwnershipProof);
    observation = Observation();
    observation.world.tick = kExpiry;
    expectRejected(observation, PvpCombatReason::LeaseExpired);
    observation = Observation();
    observation.world.frameVersion++;
    expectRejected(observation, PvpCombatReason::StaleFrame);
    observation = Observation();
    observation.world.epoch++;
    expectRejected(observation, PvpCombatReason::StaleEpoch);
    observation = Observation();
    observation.world.ownershipProof.decisionId++;
    expectRejected(observation, PvpCombatReason::LeasePreempted);
    observation = Observation();
    observation.world.ownershipProof.expiresTick++;
    expectRejected(observation, PvpCombatReason::LeaseMismatch);
    observation = Observation();
    observation.world.actorGuid++;
    expectRejected(observation, PvpCombatReason::WrongBot);
    observation = Observation();
    observation.world.actorAlive = false;
    expectRejected(observation, PvpCombatReason::ActorDead);
}

TEST(OraclePvpCombatExecutorTest, RejectsEveryUnsafeHostilePlayerFactBeforeNative)
{
    auto expectRejected = [](HostilePlayerObservation observation, PvpCombatReason reason)
    {
        NativeProbe probe;
        DispatchResult const result = Dispatch(probe, observation);
        EXPECT_FALSE(result.accepted);
        EXPECT_EQ(result.reason, reason);
        EXPECT_EQ(probe.calls, 0);
    };

    HostilePlayerObservation observation = Observation();
    observation.target.targetGuid++;
    expectRejected(observation, PvpCombatReason::TargetGuidMismatch);
    observation = Observation();
    observation.target.mapId++;
    expectRejected(observation, PvpCombatReason::TargetMapMismatch);
    observation = Observation();
    observation.target.instanceId++;
    expectRejected(observation, PvpCombatReason::TargetInstanceMismatch);
    observation = Observation();
    observation.target.isPlayer = false;
    expectRejected(observation, PvpCombatReason::TargetNotPlayer);
    observation = Observation();
    observation.target.inWorld = false;
    expectRejected(observation, PvpCombatReason::TargetNotInWorld);
    observation = Observation();
    observation.target.alive = false;
    expectRejected(observation, PvpCombatReason::TargetDead);
    observation = Observation();
    observation.target.attackable = false;
    expectRejected(observation, PvpCombatReason::TargetNotAttackable);
    observation = Observation();
    observation.target.pvpPermitted = false;
    expectRejected(observation, PvpCombatReason::PvpProhibited);
    observation = Observation();
    observation.target.relation = TeamRelation::Friendly;
    expectRejected(observation, PvpCombatReason::TeamRelationMismatch);
    observation = Observation();
    observation.target.targetFaction = kActorFaction;
    expectRejected(observation, PvpCombatReason::FactionMismatch);
    observation = Observation();
    observation.target.factVersion--;
    expectRejected(observation, PvpCombatReason::FactVersionStale);
}

TEST(OraclePvpCombatExecutorTest, DispatchesExactlyOneNativeAttackAgainstTheNamedPlayer)
{
    NativeProbe probe;
    DispatchResult const result = Dispatch(probe);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Progressing);
    EXPECT_TRUE(result.beforeValidated);
    EXPECT_TRUE(result.afterValidated);
    EXPECT_TRUE(result.nativeCalled);
    EXPECT_EQ(result.stepsInvoked, 1);
    EXPECT_EQ(result.selectedTargetGuid, kTarget);
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.maxSteps, 1);
    EXPECT_TRUE(probe.stopAfterStep);
    EXPECT_EQ(probe.exactTargetGuid, kTarget);
}

TEST(OraclePvpCombatExecutorTest, NativeFailureBudgetAndTargetSubstitutionNeverBecomeSuccess)
{
    NativeProbe rejected;
    rejected.accept = false;
    DispatchResult result = Dispatch(rejected);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::NativeRejected);

    NativeProbe overBudget;
    overBudget.stepsInvoked = 2;
    result = Dispatch(overBudget);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::StepBudgetExceeded);

    NativeProbe substituted;
    substituted.selectedTargetGuid = kTarget + 1;
    result = Dispatch(substituted);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::TargetSubstitution);

    NativeProbe noNative;
    result = OraclePvpCombatExecutor::Dispatch(Request(), Lease(), TargetReference(),
                                                Observation(), nullptr, &noNative);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::NativeUnavailable);
    EXPECT_EQ(noNative.calls, 0);
}

TEST(OraclePvpCombatExecutorTest, RevalidatesPostStepTargetOwnershipAndFactVersions)
{
    NativeProbe changedTarget;
    changedTarget.after.target.targetGuid++;
    DispatchResult result = Dispatch(changedTarget);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::TargetGuidMismatch);

    NativeProbe preempted;
    preempted.after.world.ownershipProof.decisionId++;
    result = Dispatch(preempted);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::LeasePreempted);

    NativeProbe staleFrame;
    staleFrame.after.world.frameVersion = kFrame;
    result = Dispatch(staleFrame);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::PostconditionMismatch);

    NativeProbe staleFacts;
    staleFacts.after.target.factVersion = kTargetFacts;
    result = Dispatch(staleFacts);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, PvpCombatReason::PostconditionMismatch);
}

TEST(OraclePvpCombatExecutorTest, ExactAttackMayFinishTheNamedTargetWithoutChangingIdentity)
{
    NativeProbe probe;
    probe.after.target.alive = false;
    probe.after.target.attackable = false;
    DispatchResult const result = Dispatch(probe);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.after.target.targetGuid, kTarget);
    EXPECT_EQ(result.selectedTargetGuid, kTarget);
}

} // namespace
