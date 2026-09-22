#include "../src/AutoWow/AutoWowOraclePvpOpenWorldAdapter.h"

#include <cstdint>
#include <limits>
#include <type_traits>

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowOraclePvpOpenWorld;

SquadMemberFacts MakeMember(Guid guid)
{
    SquadMemberFacts member;
    member.guid = guid;
    member.alive = true;
    member.connected = true;
    member.healthPermille = kPermilleMax;
    member.distanceToObjectivePermille = kPermilleMax;
    return member;
}

SquadFacts MakeSquad()
{
    SquadFacts squad;
    squad.squadId = 77;
    squad.memberCount = 4;
    squad.members[0] = MakeMember(10);
    squad.members[1] = MakeMember(20);
    squad.members[1].canHeal = true;
    squad.members[2] = MakeMember(30);
    squad.members[2].canTank = true;
    squad.members[3] = MakeMember(40);
    squad.members[3].canCarryFlag = true;
    return squad;
}

ObjectiveFacts MakeCaptureObjective(StableId objectiveId)
{
    ObjectiveFacts objective;
    objective.objectiveId = objectiveId;
    objective.kind = ObjectiveKind::Capture;
    objective.friendlyFlagAtBase = true;
    objective.enemyFlagAtBase = true;
    return objective;
}

PlannerFrame MakePlannerFrame(PlannerMode mode, Guid selfGuid = 10)
{
    PlannerFrame frame;
    frame.selfGuid = selfGuid;
    frame.decisionKey = 100;
    frame.mode = mode;
    frame.squad = MakeSquad();
    frame.recovery.selfGuid = selfGuid;
    frame.recovery.selfAlive = true;
    if (mode == PlannerMode::OpenWorld)
    {
        frame.openWorld.inOpenWorld = true;
        frame.openWorld.selfGuid = selfGuid;
        frame.openWorld.selfAlive = true;
        frame.openWorld.selfDefenseEnabled = true;
    }
    return frame;
}

AutoWowOracle::WorldReadFrame MakeOracleFrame(Guid botGuid = 10, bool alive = true)
{
    AutoWowOracle::WorldReadFrame frame;
    frame.version = 10;
    frame.tick = 100;
    frame.epoch = 3;
    frame.scope = {AutoWowOracle::ScopeKind::PersistentCampaign, 77, botGuid};
    frame.bot = {botGuid, alive, false, 1, 2};
    frame.evidence = {2, 3, 4, 5, 6, 0, 7, 1};
    return frame;
}
}

static_assert(std::is_same_v<decltype(AutoWowOracle::PriorityFromScore(std::uint32_t{})),
    std::uint16_t>);
static_assert(std::is_trivially_copyable_v<AutoWowOracle::OracleCandidate>);

TEST(AutoWowOraclePvpOpenWorldAdapterTest, PriorityQuantizationIsDeterministicAndSaturating)
{
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(0U), 0U);
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(31U), 0U);
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(32U), 1U);
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(32U * 65535U), 65535U);
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(std::numeric_limits<std::uint32_t>::max()),
        65535U);
    EXPECT_EQ(AutoWowOracle::PriorityFromScore(-1), 0U);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, ObjectiveMapsIntoSharedPlanAndArbiter)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::Battleground, 40);
    planner.objectiveCount = 1;
    planner.objectives[0] = MakeCaptureObjective(100);
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.intent, IntentKind::CaptureObjective);

    AutoWowOracle::WorldReadFrame frame = MakeOracleFrame(40);
    ASSERT_TRUE(ContractAdapter::AppendCandidate(planner, planned, frame));
    AutoWowOracle::PlanResult const oraclePlan = AutoWowOracle::Plan(frame);
    EXPECT_EQ(oraclePlan.receipt.plannerMode,
        static_cast<AutoWowOracle::PlannerModeId>(PlannerMode::Battleground));
    EXPECT_EQ(oraclePlan.receipt.plannerReason,
        static_cast<AutoWowOracle::PlannerReasonId>(planned.intent.reason));
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);

    AutoWowOracle::OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, OwnerMismatchCannotEmitAnotherBotsObjective)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::Battleground, 10);
    planner.objectiveCount = 1;
    planner.objectives[0] = MakeCaptureObjective(101);
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.ownerGuid, 40U);

    AutoWowOracle::OracleCandidate candidate;
    EXPECT_FALSE(ContractAdapter::TryBuildCandidate(planner, planned, candidate));
    EXPECT_FALSE(candidate.available);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, RecoveryCarriesDeadSelfPrecondition)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::Battleground);
    planner.recovery.selfAlive = false;
    planner.recovery.corpseKnown = true;
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.intent, IntentKind::RecoverDeath);

    AutoWowOracle::WorldReadFrame frame = MakeOracleFrame(10, false);
    ASSERT_TRUE(ContractAdapter::AppendCandidate(planner, planned, frame));
    AutoWowOracle::PlanResult const oraclePlan = AutoWowOracle::Plan(frame);
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(oraclePlan.receipt.operation, AutoWowOracle::OperationCode::Recover);

    AutoWowOracle::OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, DisengageRemainsExplicitNavigationIntent)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::OpenWorld);
    planner.openWorld.hostileActionObserved = true;
    planner.openWorld.selfUnderAttack = true;
    planner.openWorld.aggressorGuid = 800;
    planner.openWorld.aggressorHostile = true;
    planner.openWorld.criticalHealth = true;
    planner.openWorld.outnumbered = true;
    planner.openWorld.disengageAvailable = true;
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.intent, IntentKind::Disengage);

    AutoWowOracle::WorldReadFrame frame = MakeOracleFrame();
    frame.combat.candidates = {{AutoWowShadowCombat::Intent::Attack, 900, "shadow_attack"}};
    ASSERT_TRUE(ContractAdapter::AppendCandidate(planner, planned, frame));
    AutoWowOracle::PlanResult const oraclePlan = AutoWowOracle::Plan(frame);
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(oraclePlan.receipt.operation, AutoWowOracle::OperationCode::Disengage);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, AttackHostileIsExecutorBlockedAndGetsNoLease)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::OpenWorld);
    planner.openWorld.hostileActionObserved = true;
    planner.openWorld.selfUnderAttack = true;
    planner.openWorld.aggressorGuid = 801;
    planner.openWorld.aggressorHostile = true;
    planner.openWorld.criticalHealth = false;
    planner.openWorld.outnumbered = false;
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.intent, IntentKind::AttackHostile);

    AutoWowOracle::WorldReadFrame frame = MakeOracleFrame();
    AutoWowOracle::OracleCandidate candidate;
    ASSERT_TRUE(ContractAdapter::TryBuildCandidate(planner, planned, candidate));
    EXPECT_EQ(candidate.operation, AutoWowOracle::OperationCode::CombatAction);
    EXPECT_FALSE(candidate.executorAvailable);
    ASSERT_TRUE(ContractAdapter::AppendCandidate(planner, planned, frame));
    AutoWowOracle::PlanResult const oraclePlan = AutoWowOracle::Plan(frame);
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(oraclePlan.receipt.operation, AutoWowOracle::OperationCode::CombatAction);

    AutoWowOracle::OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, RegroupCarriesRallyPointAndPlannerMetadata)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::Campaign);
    planner.recovery.regroupNeeded = true;
    planner.recovery.rallyPointId = 9001;
    PlanResult const planned = Plan(planner);
    ASSERT_EQ(planned.status, PlanStatus::Planned);
    ASSERT_EQ(planned.intent.intent, IntentKind::Regroup);

    AutoWowOracle::OracleCandidate candidate;
    ASSERT_TRUE(ContractAdapter::TryBuildCandidate(planner, planned, candidate));
    EXPECT_EQ(candidate.domain, AutoWowOracle::Domain::Navigation);
    EXPECT_EQ(candidate.intent, AutoWowOracle::IntentCode::Navigate);
    EXPECT_EQ(candidate.operation, AutoWowOracle::OperationCode::Navigate);
    EXPECT_EQ(candidate.resource, AutoWowOracle::LeaseResource::Transition);
    EXPECT_FALSE(candidate.executorAvailable);
    EXPECT_EQ(candidate.objectiveId, 0U);
    EXPECT_EQ(candidate.rallyPointId, 9001U);
    EXPECT_EQ(candidate.plannerMode,
        static_cast<AutoWowOracle::PlannerModeId>(PlannerMode::Campaign));
    EXPECT_EQ(candidate.classification, AutoWowOracle::IntentClassification::Persistent);
}

TEST(AutoWowOraclePvpOpenWorldAdapterTest, PlanIntoOracleUsesOneSharedArbiterPath)
{
    PlannerFrame planner = MakePlannerFrame(PlannerMode::Campaign);
    planner.campaign.challengeObserved = true;
    planner.campaign.isCampaignChallenge = true;
    planner.campaign.challengerGuid = 700;
    planner.campaign.acceptAllowed = true;
    planner.campaign.safeToAccept = true;

    AutoWowOracle::WorldReadFrame frame = MakeOracleFrame();
    AutoWowOracle::PlanResult const oraclePlan = ContractAdapter::PlanIntoOracle(planner, frame);
    EXPECT_FALSE(oraclePlan.hasDecision);
    EXPECT_EQ(oraclePlan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(oraclePlan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(oraclePlan.receipt.operation, AutoWowOracle::OperationCode::PvPChallenge);
    EXPECT_EQ(oraclePlan.receipt.plannerMode,
        static_cast<AutoWowOracle::PlannerModeId>(PlannerMode::Campaign));

    AutoWowOracle::OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
}
