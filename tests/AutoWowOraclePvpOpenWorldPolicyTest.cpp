#include "../src/AutoWow/AutoWowOraclePvpOpenWorldPolicy.h"

#include <algorithm>
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
    squad.memberCount = 9;

    squad.members[0] = MakeMember(10);
    squad.members[1] = MakeMember(20);
    squad.members[1].canHeal = true;
    squad.members[2] = MakeMember(30);
    squad.members[2].canTank = true;
    squad.members[3] = MakeMember(40);
    squad.members[3].canCarryFlag = true;
    squad.members[4] = MakeMember(50);
    squad.members[4].canEscort = true;
    squad.members[5] = MakeMember(60);
    squad.members[5].canIntercept = true;
    squad.members[6] = MakeMember(70);
    squad.members[6].canDefend = true;
    squad.members[7] = MakeMember(80);
    squad.members[7].canRecover = true;
    squad.members[8] = MakeMember(90);
    return squad;
}

ObjectiveFacts MakeObjective(ObjectiveKind kind, StableId objectiveId)
{
    ObjectiveFacts objective;
    objective.kind = kind;
    objective.objectiveId = objectiveId;
    objective.available = true;
    objective.friendlyFlagAtBase = true;
    objective.enemyFlagAtBase = true;
    return objective;
}

PlannerFrame MakeFrame(PlannerMode mode)
{
    PlannerFrame frame;
    frame.selfGuid = 10;
    frame.decisionKey = 100;
    frame.mode = mode;
    frame.squad = MakeSquad();
    frame.recovery.selfGuid = frame.selfGuid;
    frame.recovery.selfAlive = true;
    if (mode == PlannerMode::OpenWorld)
    {
        frame.openWorld.inOpenWorld = true;
        frame.openWorld.selfGuid = frame.selfGuid;
        frame.openWorld.selfAlive = true;
        frame.openWorld.selfDefenseEnabled = true;
    }
    return frame;
}

SquadRole RoleForGuid(SquadRolePlan const& plan, Guid guid)
{
    return RoleFor(plan, guid);
}
}

static_assert(std::is_trivially_copyable_v<PlannerFrame>);
static_assert(std::is_trivially_copyable_v<PlanIntent>);

TEST(AutoWowOraclePvpOpenWorldPolicyTest, SquadRolesAreStableAndBounded)
{
    SquadFacts squad = MakeSquad();
    SquadRolePlan const first = BuildSquadRolePlan(squad);

    ASSERT_TRUE(first.valid);
    ASSERT_EQ(first.assignmentCount, squad.memberCount);
    EXPECT_EQ(RoleForGuid(first, 10), SquadRole::Captain);
    EXPECT_EQ(RoleForGuid(first, 20), SquadRole::Healer);
    EXPECT_EQ(RoleForGuid(first, 30), SquadRole::Tank);
    EXPECT_EQ(RoleForGuid(first, 40), SquadRole::FlagCarrier);
    EXPECT_EQ(RoleForGuid(first, 50), SquadRole::Escort);
    EXPECT_EQ(RoleForGuid(first, 60), SquadRole::Interceptor);
    EXPECT_EQ(RoleForGuid(first, 70), SquadRole::Defender);
    EXPECT_EQ(RoleForGuid(first, 80), SquadRole::Recovery);
    EXPECT_EQ(RoleForGuid(first, 90), SquadRole::Damage);

    std::reverse(squad.members.begin(), squad.members.begin() + squad.memberCount);
    SquadRolePlan const permuted = BuildSquadRolePlan(squad);
    ASSERT_EQ(permuted.assignmentCount, first.assignmentCount);
    for (Guid guid : {10ULL, 20ULL, 30ULL, 40ULL, 50ULL, 60ULL, 70ULL, 80ULL, 90ULL})
        EXPECT_EQ(RoleForGuid(permuted, guid), RoleForGuid(first, guid));

    SquadFacts oversized = MakeSquad();
    oversized.memberCount = kMaxSquadMembers + 1;
    EXPECT_FALSE(SquadFrameWithinBounds(oversized));
    EXPECT_EQ(BuildSquadRolePlan(oversized).reason, Reason::InvalidFrame);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, WsgObjectiveOwnershipCoversAllFiveRoles)
{
    SquadFacts const squad = MakeSquad();
    SquadRolePlan const roles = BuildSquadRolePlan(squad);

    ObjectiveFacts capture = MakeObjective(ObjectiveKind::Capture, 101);
    ObjectiveOwnership const captureOwner = OwnObjective(capture, squad, roles);
    ASSERT_TRUE(captureOwner.claimed);
    EXPECT_EQ(captureOwner.ownerGuid, 40ULL);

    ObjectiveFacts defend = MakeObjective(ObjectiveKind::Defend, 102);
    defend.threatPermille = 800;
    ObjectiveOwnership const defendOwner = OwnObjective(defend, squad, roles);
    ASSERT_TRUE(defendOwner.claimed);
    EXPECT_EQ(defendOwner.ownerGuid, 70ULL);

    ObjectiveFacts escort = MakeObjective(ObjectiveKind::Escort, 103);
    escort.enemyFlagCarrierGuid = 40;
    ObjectiveOwnership const escortOwner = OwnObjective(escort, squad, roles);
    ASSERT_TRUE(escortOwner.claimed);
    EXPECT_EQ(escortOwner.ownerGuid, 50ULL);

    ObjectiveFacts intercept = MakeObjective(ObjectiveKind::Intercept, 104);
    intercept.friendlyFlagCarrierGuid = 900;
    ObjectiveOwnership const interceptOwner = OwnObjective(intercept, squad, roles);
    ASSERT_TRUE(interceptOwner.claimed);
    EXPECT_EQ(interceptOwner.ownerGuid, 60ULL);

    ObjectiveFacts recover = MakeObjective(ObjectiveKind::Recover, 105);
    recover.friendlyFlagDropped = true;
    ObjectiveOwnership const recoverOwner = OwnObjective(recover, squad, roles);
    ASSERT_TRUE(recoverOwner.claimed);
    EXPECT_EQ(recoverOwner.ownerGuid, 80ULL);

    capture.previousOwnerGuid = 40;
    capture.previousOwnerLeaseActive = true;
    ObjectiveOwnership const retained = OwnObjective(capture, squad, roles);
    EXPECT_TRUE(retained.retained);
    EXPECT_EQ(retained.reason, Reason::RetainedOwner);
    EXPECT_EQ(retained.ownerGuid, 40ULL);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, ObjectiveSelectionIsStableAcrossInputOrder)
{
    SquadFacts const squad = MakeSquad();
    SquadRolePlan const roles = BuildSquadRolePlan(squad);
    std::array<ObjectiveFacts, kMaxObjectives> objectives{};
    objectives[0] = MakeObjective(ObjectiveKind::Capture, 1);
    objectives[1] = MakeObjective(ObjectiveKind::Defend, 2);
    objectives[2] = MakeObjective(ObjectiveKind::Escort, 3);
    objectives[2].enemyFlagCarrierGuid = 40;
    objectives[3] = MakeObjective(ObjectiveKind::Intercept, 4);
    objectives[3].friendlyFlagCarrierGuid = 900;
    objectives[4] = MakeObjective(ObjectiveKind::Recover, 5);
    objectives[4].friendlyFlagDropped = true;

    ObjectiveSelection const first = SelectObjective(squad, roles, objectives, 5);
    ASSERT_TRUE(first.hasSelection);
    EXPECT_EQ(first.ownership.kind, ObjectiveKind::Intercept);
    EXPECT_EQ(first.ownership.ownerGuid, 60ULL);

    std::reverse(objectives.begin(), objectives.begin() + 5);
    ObjectiveSelection const permuted = SelectObjective(squad, roles, objectives, 5);
    ASSERT_TRUE(permuted.hasSelection);
    EXPECT_EQ(permuted.ownership.kind, first.ownership.kind);
    EXPECT_EQ(permuted.ownership.objectiveId, first.ownership.objectiveId);
    EXPECT_EQ(permuted.ownership.ownerGuid, first.ownership.ownerGuid);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, TargetScoringPrioritizesFlagCarriersAndProtectiveThreats)
{
    std::array<TargetFacts, kMaxTargets> targets{};
    targets[0] = {3, true, true, false, false, false, false, false, false, false, false, 200, 900, 100};
    targets[1] = {2, true, true, false, false, false, true, false, true, false, true, 300, 800, 200};
    targets[2] = {1, true, true, true, false, true, false, false, false, false, false, 100, 900, 300};

    TargetContext const battleground = {PlannerMode::Battleground, false};
    TargetChoice const first = ChooseTarget(targets, 3, battleground);
    ASSERT_TRUE(first.hasTarget);
    EXPECT_EQ(first.targetGuid, 1ULL);
    EXPECT_EQ(first.intent, IntentKind::InterceptCarrier);

    std::reverse(targets.begin(), targets.begin() + 3);
    TargetChoice const permuted = ChooseTarget(targets, 3, battleground);
    EXPECT_EQ(permuted.targetGuid, first.targetGuid);
    EXPECT_EQ(permuted.score, first.score);

    TargetFacts deadCarrier = targets[0];
    deadCarrier.alive = false;
    EXPECT_FALSE(ScoreTarget(deadCarrier, battleground).eligible);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, OpenWorldTargetScoringRequiresObservedHostility)
{
    TargetContext const openWorld = {PlannerMode::OpenWorld, true};
    TargetFacts hostile;
    hostile.guid = 22;
    hostile.hostile = true;

    TargetScore const idle = ScoreTarget(hostile, openWorld);
    EXPECT_FALSE(idle.eligible);
    EXPECT_EQ(idle.reason, Reason::SelfDefenseNotTriggered);

    hostile.attackingSelf = true;
    TargetScore const observed = ScoreTarget(hostile, openWorld);
    EXPECT_TRUE(observed.eligible);
    EXPECT_GT(observed.score, 0);

    TargetContext const disabled = {PlannerMode::OpenWorld, false};
    EXPECT_EQ(ScoreTarget(hostile, disabled).reason, Reason::SelfDefenseNotEnabled);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, HealerAndTankProtectionAreTypedIntents)
{
    SquadFacts squad = MakeSquad();
    squad.members[1].healthPermille = 200;
    squad.members[1].underAttack = true;
    squad.members[2].healthPermille = 800;
    squad.members[2].underAttack = true;
    SquadRolePlan const roles = BuildSquadRolePlan(squad);

    ProtectionChoice const healerPriority = ChooseProtection(squad, roles);
    ASSERT_TRUE(healerPriority.hasChoice);
    EXPECT_EQ(healerPriority.intent, IntentKind::ProtectHealer);
    EXPECT_EQ(healerPriority.targetGuid, 20ULL);

    squad.members[1].healthPermille = kPermilleMax;
    squad.members[1].underAttack = false;
    ProtectionChoice const tankPriority = ChooseProtection(squad, roles);
    ASSERT_TRUE(tankPriority.hasChoice);
    EXPECT_EQ(tankPriority.intent, IntentKind::ProtectTank);
    EXPECT_EQ(tankPriority.targetGuid, 30ULL);

    squad.members[2].underAttack = false;
    squad.members[2].healthPermille = kPermilleMax;
    EXPECT_FALSE(ChooseProtection(squad, roles).hasChoice);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, RecoveryAndRegroupUseFiniteBudgets)
{
    RecoveryFacts dead;
    dead.selfGuid = 10;
    dead.selfAlive = false;
    dead.corpseKnown = true;
    RecoveryDecision const death = EvaluateRecovery(dead);
    EXPECT_EQ(death.intent, IntentKind::RecoverDeath);
    EXPECT_EQ(death.targetGuid, 10ULL);

    dead.recoveryAttempts = kMaxRecoveryAttempts;
    EXPECT_EQ(EvaluateRecovery(dead).reason, Reason::RecoveryBudgetExhausted);

    RecoveryFacts regroup;
    regroup.selfGuid = 10;
    regroup.regroupNeeded = true;
    regroup.rallyPointId = 9001;
    RecoveryDecision const regroupDecision = EvaluateRecovery(regroup);
    EXPECT_EQ(regroupDecision.intent, IntentKind::Regroup);
    EXPECT_EQ(regroupDecision.rallyPointId, 9001ULL);

    regroup.regroupNoProgress = kMaxRegroupNoProgress;
    EXPECT_EQ(EvaluateRecovery(regroup).reason, Reason::RegroupBudgetExhausted);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, CampaignChallengeHooksOnlyEmitFactsBasedIntents)
{
    CampaignChallengeFacts facts;
    facts.challengeObserved = true;
    facts.isCampaignChallenge = true;
    facts.challengerGuid = 500;
    facts.acceptAllowed = true;
    facts.safeToAccept = true;

    CampaignDecision const accept = EvaluateCampaignChallenge(facts);
    EXPECT_EQ(accept.intent, IntentKind::AcceptCampaignChallenge);
    EXPECT_EQ(accept.challengerGuid, 500ULL);

    facts.safeToAccept = false;
    CampaignDecision const decline = EvaluateCampaignChallenge(facts);
    EXPECT_EQ(decline.intent, IntentKind::DeclineCampaignChallenge);
    EXPECT_EQ(decline.reason, Reason::ChallengeDeclined);

    facts.challengeObserved = false;
    EXPECT_EQ(EvaluateCampaignChallenge(facts).intent, IntentKind::None);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, OpenWorldSelfDefenseDoesNotRetaliateWithoutFacts)
{
    OpenWorldFacts facts;
    facts.inOpenWorld = true;
    facts.selfDefenseEnabled = true;
    facts.selfGuid = 10;
    facts.aggressorGuid = 20;
    facts.aggressorHostile = true;

    EXPECT_EQ(EvaluateOpenWorldSelfDefense(facts).reason, Reason::SelfDefenseNotTriggered);

    facts.hostileActionObserved = true;
    facts.selfUnderAttack = true;
    OpenWorldDecision const defend = EvaluateOpenWorldSelfDefense(facts);
    EXPECT_EQ(defend.intent, IntentKind::AttackHostile);
    EXPECT_EQ(defend.targetGuid, 20ULL);

    facts.criticalHealth = true;
    facts.outnumbered = true;
    facts.disengageAvailable = true;
    EXPECT_EQ(EvaluateOpenWorldSelfDefense(facts).intent, IntentKind::Disengage);

    facts.selfUnderAttack = false;
    facts.criticalHealth = false;
    facts.outnumbered = false;
    facts.disengageAvailable = false;
    facts.allyUnderAttack = true;
    EXPECT_EQ(EvaluateOpenWorldSelfDefense(facts).intent, IntentKind::DefendAlly);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, PlannerPrioritizesRecoveryAndEmitsObjectiveOwnership)
{
    PlannerFrame recovering = MakeFrame(PlannerMode::Battleground);
    recovering.recovery.selfAlive = false;
    recovering.recovery.corpseKnown = true;
    recovering.objectiveCount = 1;
    recovering.objectives[0] = MakeObjective(ObjectiveKind::Capture, 100);
    recovering.targetCount = 1;
    recovering.targets[0].guid = 999;
    recovering.targets[0].alive = true;
    recovering.targets[0].hostile = true;

    PlanResult const recoveryPlan = Plan(recovering);
    EXPECT_EQ(recoveryPlan.status, PlanStatus::Planned);
    EXPECT_EQ(recoveryPlan.intent.intent, IntentKind::RecoverDeath);
    EXPECT_EQ(recoveryPlan.intent.ttlTicks, kIntentTtlTicks);

    PlannerFrame objectiveFrame = MakeFrame(PlannerMode::Battleground);
    objectiveFrame.objectiveCount = 1;
    objectiveFrame.objectives[0] = MakeObjective(ObjectiveKind::Capture, 100);
    PlanResult const objectivePlan = Plan(objectiveFrame);
    ASSERT_EQ(objectivePlan.status, PlanStatus::Planned);
    EXPECT_EQ(objectivePlan.intent.intent, IntentKind::CaptureObjective);
    EXPECT_EQ(objectivePlan.intent.objectiveId, 100ULL);
    EXPECT_EQ(objectivePlan.intent.ownerGuid, 40ULL);
    EXPECT_EQ(objectivePlan.intent.targetGuid, 40ULL);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, PlannerUsesCampaignAndOpenWorldFactsOnly)
{
    PlannerFrame campaign = MakeFrame(PlannerMode::Campaign);
    campaign.campaign.challengeObserved = true;
    campaign.campaign.isCampaignChallenge = true;
    campaign.campaign.challengerGuid = 700;
    campaign.campaign.acceptAllowed = true;
    campaign.campaign.safeToAccept = true;
    PlanResult const campaignPlan = Plan(campaign);
    EXPECT_EQ(campaignPlan.status, PlanStatus::Planned);
    EXPECT_EQ(campaignPlan.intent.intent, IntentKind::AcceptCampaignChallenge);
    EXPECT_EQ(campaignPlan.intent.targetGuid, 700ULL);

    PlannerFrame openWorld = MakeFrame(PlannerMode::OpenWorld);
    openWorld.openWorld.hostileActionObserved = true;
    openWorld.openWorld.selfUnderAttack = true;
    openWorld.openWorld.aggressorGuid = 800;
    openWorld.openWorld.aggressorHostile = true;
    PlanResult const selfDefense = Plan(openWorld);
    EXPECT_EQ(selfDefense.status, PlanStatus::Planned);
    EXPECT_EQ(selfDefense.intent.intent, IntentKind::AttackHostile);
    EXPECT_EQ(selfDefense.intent.targetGuid, 800ULL);

    openWorld.openWorld.hostileActionObserved = false;
    openWorld.openWorld.selfUnderAttack = false;
    PlanResult const noRetaliation = Plan(openWorld);
    EXPECT_EQ(noRetaliation.status, PlanStatus::Blocked);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, PlannerRejectsUnboundedFramesBeforeTraversal)
{
    PlannerFrame frame = MakeFrame(PlannerMode::Battleground);
    frame.targetCount = kMaxTargets + 1;
    EXPECT_EQ(Plan(frame).status, PlanStatus::Invalid);

    frame = MakeFrame(PlannerMode::Battleground);
    frame.objectiveCount = kMaxObjectives + 1;
    EXPECT_EQ(Plan(frame).status, PlanStatus::Invalid);
}

TEST(AutoWowOraclePvpOpenWorldPolicyTest, NamesAndIntentBoundsAreStable)
{
    EXPECT_EQ(PlannerModeName(PlannerMode::OpenWorld), "open_world");
    EXPECT_EQ(SquadRoleName(SquadRole::FlagCarrier), "flag_carrier");
    EXPECT_EQ(ObjectiveKindName(ObjectiveKind::Intercept), "intercept");
    EXPECT_EQ(IntentKindName(IntentKind::RecoverDeath), "recover_death");
    EXPECT_EQ(ReasonName(Reason::ChallengeDeclined), "challenge_declined");
    EXPECT_EQ(kIntentTtlTicks, 3U);
}

#include "AutoWowRandomBotPolicy.h"

TEST(AutoWowRandomBotPolicyTest, PvpFlagForcedExceptOnPvpRealmWithZoneRules)
{
    using AutoWowRandomBotPolicy::ShouldForceRandomBotPvpFlag;
    EXPECT_TRUE(ShouldForceRandomBotPvpFlag(false, false));  // PvE, legacy
    EXPECT_TRUE(ShouldForceRandomBotPvpFlag(false, true));   // PvE unchanged by the flag
    EXPECT_TRUE(ShouldForceRandomBotPvpFlag(true, false));   // PvP, legacy forced flag
    EXPECT_FALSE(ShouldForceRandomBotPvpFlag(true, true));   // PvP + zone rules: no forced write
}
