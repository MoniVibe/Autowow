#include "../src/AutoWow/AutoWowOracleContract.h"

#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

namespace AutoWowOracle
{
struct OracleArbiterContractTestAccess
{
    template <typename Arbiter>
    static IntentLease InstallLease(Arbiter& arbiter, Decision const& decision)
    {
        // Test-only seeding for contract-mode reservation tests and negative renew tests. The
        // production Acquire() path remains the only runtime authority entry point.
        if (!decision.valid)
            return {};

        std::size_t const existing = arbiter.FindBot(decision.scope.botGuid);
        if (existing != arbiter.botLeases.size())
            return {};
        std::size_t const index = arbiter.FindFreeBot();
        if (index == arbiter.botLeases.size())
            return {};
        arbiter.botLeases[index] = {true, decision.scope.botGuid, {true, decision}};
        return arbiter.botLeases[index].lease;
    }

    template <typename Arbiter>
    static Receipt ReserveNode(Arbiter& arbiter, WorldReadFrame const& frame,
        IntentLease const& lease)
    {
        return arbiter.ReserveNodeForContractTest(frame, lease);
    }

    template <typename Arbiter>
    static Receipt ReleaseNode(Arbiter& arbiter, WorldReadFrame const& frame,
        IntentLease const& lease)
    {
        return arbiter.ReleaseNodeForContractTest(frame, lease);
    }

    template <typename Arbiter>
    static LeaseResult RenewContractLease(Arbiter& arbiter, WorldReadFrame const& frame,
        IntentLease const& lease)
    {
        return arbiter.RenewForContractTest(frame, lease);
    }
};
}

namespace
{
using namespace AutoWowOracle;

static_assert(std::is_same_v<decltype(Plan(std::declval<WorldReadFrame const&>())), PlanResult>);
static_assert(kMaxBotLeases > 0 && kMaxSquadOwners > 0 && kMaxItemReservations > 0);

WorldReadFrame MakeFrame(ScopeKind kind = ScopeKind::PersistentCampaign,
    SquadId squadId = 77, Guid botGuid = 42)
{
    WorldReadFrame frame;
    frame.version = 10;
    frame.tick = 100;
    frame.epoch = 3;
    frame.scope = {kind, squadId, botGuid};
    frame.bot = {botGuid, true, false, 1, 2};
    frame.evidence = {2, 3, 4, 5, 6, 0, 7, 1};
    frame.combat.decisionKey = 1000;
    frame.combat.self = {botGuid, 1000, false};
    return frame;
}

OperationCode OperationForTest(Domain domain, IntentCode intent)
{
    if (domain == Domain::Combat)
        return OperationCode::CombatAction;
    if (domain == Domain::Navigation)
        return intent == IntentCode::Disengage ? OperationCode::Disengage : OperationCode::Navigate;
    if (domain == Domain::Quest)
        return OperationCode::QuestObjective;
    if (domain == Domain::Gathering)
        return OperationCode::GatherSource;
    if (domain == Domain::CraftingEconomyItems)
        return OperationCode::Craft;
    if (domain == Domain::GroupRoster)
        return OperationCode::RosterAssignment;
    if (domain == Domain::Recovery)
        return OperationCode::Recover;
    if (domain == Domain::PvP)
        return OperationCode::PvPObjective;
    return OperationCode::Unknown;
}

OracleCandidate MakeCandidate(Domain domain, IntentCode intent, LeaseResource resource,
    std::string_view action, Guid targetGuid = 0, std::uint16_t priority = 100)
{
    OracleCandidate candidate;
    candidate.domain = domain;
    candidate.intent = intent;
    candidate.resource = resource;
    candidate.priority = priority;
    candidate.ttlTicks = 5;
    candidate.targetGuid = targetGuid;
    candidate.action = action;
    candidate.operation = OperationForTest(domain, intent);
    if (domain == Domain::Quest)
        candidate.quest = {targetGuid != 0, static_cast<QuestId>(targetGuid),
            QuestObjectiveFamily::NpcOrGameObject, 0, 1, 0};
    if (domain == Domain::PvP)
        candidate.objectiveId = targetGuid;
    return candidate;
}

// The production gate admits QuestObjective and the deliberate exact GatherSource tuple. Other
// operations remain blocked by the runtime authority.
OracleCandidate MakeVerifiedQuestCandidate(std::string_view action, Guid questId = 792,
    std::uint16_t priority = 100)
{
    OracleCandidate candidate = MakeCandidate(Domain::Quest, IntentCode::QuestObjective,
        LeaseResource::QuestGather,
        action, questId, priority);
    candidate.executorAvailable = true;
    return candidate;
}

Decision PlanOne(WorldReadFrame& frame, OracleCandidate const& candidate)
{
    EXPECT_TRUE(AddCandidate(frame, candidate));
    PlanResult const plan = Plan(frame);
    EXPECT_TRUE(plan.hasDecision);
    return plan.decision;
}

Decision MakeContractOnlyGatherDecision(WorldReadFrame& frame, OracleCandidate const& candidate)
{
    // Gather is intentionally blocked by the production Plan() gate until its native executor is
    // integrated. These tests exercise only arbiter reservation lifecycle with a typed decision.
    OracleCandidate contractCandidate = candidate;
    contractCandidate.executorAvailable = true;
    Decision const decision = MakeDecision(frame, contractCandidate);
    EXPECT_TRUE(decision.valid);
    return decision;
}
}

TEST(AutoWowOracleContractTest, DomainNamesAndStableIdsAreDeterministic)
{
    EXPECT_EQ(DomainName(Domain::Combat), "combat");
    EXPECT_EQ(DomainName(Domain::Navigation), "navigation");
    EXPECT_EQ(DomainName(Domain::Quest), "quest");
    EXPECT_EQ(DomainName(Domain::Gathering), "gathering");
    EXPECT_EQ(DomainName(Domain::CraftingEconomyItems), "crafting_economy_items");
    EXPECT_EQ(DomainName(Domain::GroupRoster), "group_roster");
    EXPECT_EQ(DomainName(Domain::Recovery), "recovery");
    EXPECT_EQ(DomainName(Domain::PvP), "pvp");
    EXPECT_GT(ResourcePrecedence(LeaseResource::Recovery),
        ResourcePrecedence(LeaseResource::CombatPositioning));
    EXPECT_GT(ResourcePrecedence(LeaseResource::CombatPositioning),
        ResourcePrecedence(LeaseResource::Transition));
    EXPECT_GT(ResourcePrecedence(LeaseResource::Transition),
        ResourcePrecedence(LeaseResource::QuestGather));
    EXPECT_GT(ResourcePrecedence(LeaseResource::QuestGather),
        ResourcePrecedence(LeaseResource::Idle));
    EXPECT_EQ(LeaseResourceName(LeaseResource::Recovery), "recovery");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Rejected), "rejected");
    EXPECT_EQ(ReceiptReasonName(ReceiptReason::StaleFrame), "stale_frame");
    EXPECT_EQ(ReceiptReasonName(ReceiptReason::ExecutorFailed), "executor_failed");
    EXPECT_EQ(OperationCodeName(OperationCode::QuestObjective), "quest_objective");
    EXPECT_EQ(OperationCodeName(OperationCode::GatherSource), "gather_source");

    WorldReadFrame first = MakeFrame();
    WorldReadFrame second = first;
    OracleCandidate candidate = MakeCandidate(Domain::Quest, IntentCode::QuestObjective,
        LeaseResource::QuestGather, "quest:792", 792);
    Decision const left = MakeDecision(first, candidate);
    Decision const right = MakeDecision(second, candidate);

    EXPECT_TRUE(left.valid);
    EXPECT_EQ(left.decisionId, right.decisionId);
    EXPECT_EQ(left.intentId, right.intentId);
    EXPECT_NE(left.decisionId, 0u);
    EXPECT_NE(left.intentId, 0u);

    second.tick++;
    EXPECT_NE(left.decisionId, MakeDecision(second, candidate).decisionId);
}

TEST(AutoWowOracleContractTest, MetadataAndDisengageRemainBackwardCompatible)
{
    EXPECT_EQ(IntentCodeName(IntentCode::Disengage), "disengage");
    EXPECT_EQ(RoleCodeName(RoleCode::FlagCarrier), "flag_carrier");

    WorldReadFrame frame = MakeFrame();
    OracleCandidate candidate = MakeCandidate(Domain::Navigation, IntentCode::Disengage,
        LeaseResource::CombatPositioning, "disengage", 901);
    candidate.objectiveId = 7001;
    candidate.rallyPointId = 8001;
    candidate.actorGuid = frame.bot.guid;
    candidate.ownerGuid = frame.scope.squadId;
    candidate.role = RoleCode::Damage;
    candidate.plannerMode = 2;
    candidate.plannerReason = 3;

    Decision const decision = MakeDecision(frame, candidate);
    ASSERT_TRUE(decision.valid);
    EXPECT_EQ(decision.objectiveId, 7001U);
    EXPECT_EQ(decision.rallyPointId, 8001U);
    EXPECT_EQ(decision.actorGuid, frame.bot.guid);
    EXPECT_EQ(decision.ownerGuid, frame.scope.squadId);
    EXPECT_EQ(decision.role, RoleCode::Damage);
    EXPECT_EQ(decision.plannerMode, 2U);
    EXPECT_EQ(decision.plannerReason, 3U);

    ASSERT_TRUE(AddCandidate(frame, candidate));
    PlanResult const result = Plan(frame);
    EXPECT_FALSE(result.hasDecision);
    EXPECT_EQ(result.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(result.receipt.reason, ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(result.receipt.objectiveId, 7001U);
    EXPECT_EQ(result.receipt.rallyPointId, 8001U);
    EXPECT_EQ(result.receipt.actorGuid, frame.bot.guid);
    EXPECT_EQ(result.receipt.plannerMode, 2U);
}

TEST(AutoWowOracleContractTest, UnverifiedCombatIsBlockedBeforeExecutorDispatch)
{
    WorldReadFrame frame = MakeFrame();
    frame.combat.allies = {{900, 200, true, true}};
    frame.combat.enemies = {{901, 1000, 500, true, true, false, false}};
    frame.combat.candidates = {{AutoWowShadowCombat::Intent::Attack, 901, "attack"},
        {AutoWowShadowCombat::Intent::Heal, 900, "critical_heal"}};
    OracleCandidate combat = MakeCandidate(Domain::Combat, IntentCode::CombatAction,
        LeaseResource::CombatPositioning, "placeholder", 901);
    ASSERT_TRUE(AddCandidate(frame, combat));

    PlanResult const plan = Plan(frame);

    EXPECT_FALSE(plan.hasDecision);
    EXPECT_EQ(plan.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(plan.receipt.reason, ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(plan.receipt.domain, Domain::Combat);
    EXPECT_EQ(plan.receipt.operation, OperationCode::CombatAction);
}

TEST(AutoWowOracleContractTest, ResourcePrecedenceAndSingleBotLeaseAreDeterministic)
{
    WorldReadFrame frame = MakeFrame();
    ASSERT_TRUE(AddCandidate(frame, MakeVerifiedQuestCandidate("quest", 792, 100)));
    OracleCandidate combat = MakeVerifiedQuestCandidate("combat-slot", 901, 100);
    combat.resource = LeaseResource::CombatPositioning;
    ASSERT_TRUE(AddCandidate(frame, combat));
    OracleCandidate recovery = MakeVerifiedQuestCandidate("recovery-slot", 42, 1);
    recovery.resource = LeaseResource::Recovery;
    ASSERT_TRUE(AddCandidate(frame, recovery));

    PlanResult const recoveryPlan = Plan(frame);
    EXPECT_FALSE(recoveryPlan.hasDecision);
    EXPECT_EQ(recoveryPlan.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(recoveryPlan.receipt.reason, ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(recoveryPlan.receipt.resource, LeaseResource::Recovery);

    frame.candidateCount = 2;
    PlanResult const combatPlan = Plan(frame);
    EXPECT_FALSE(combatPlan.hasDecision);
    EXPECT_EQ(combatPlan.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(combatPlan.receipt.reason, ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(combatPlan.receipt.resource, LeaseResource::CombatPositioning);

    OracleArbiter<> arbiter;
    WorldReadFrame highFrame = MakeFrame();
    Decision const highDecision = PlanOne(highFrame,
        MakeVerifiedQuestCandidate("high", 901, 200));
    LeaseResult const high = arbiter.Acquire(highFrame, highDecision);
    ASSERT_TRUE(high.hasLease);

    WorldReadFrame lowerFrame = MakeFrame();
    Decision const lower = PlanOne(lowerFrame, MakeVerifiedQuestCandidate("quest", 792, 100));
    LeaseResult const rejected = arbiter.Acquire(lowerFrame, lower);
    EXPECT_FALSE(rejected.hasLease);
    EXPECT_EQ(rejected.receipt.status, ReceiptStatus::Rejected);
    EXPECT_EQ(rejected.receipt.reason, ReceiptReason::BotLeaseConflict);
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 1u);
}

TEST(AutoWowOracleContractTest, SquadOwnerIsExclusiveAndCanBeReleased)
{
    WorldReadFrame firstFrame = MakeFrame(ScopeKind::PersistentCampaign, 88, 42);
    OracleCandidate ownerCandidate = MakeVerifiedQuestCandidate("own_squad", 88, 100);
    ownerCandidate.exclusiveSquadOwner = true;
    Decision const firstDecision = PlanOne(firstFrame, ownerCandidate);

    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 88, 43);
    Decision const secondDecision = PlanOne(secondFrame, ownerCandidate);
    OracleArbiter<> arbiter;
    LeaseResult const first = arbiter.Acquire(firstFrame, firstDecision);
    ASSERT_TRUE(first.hasLease);
    LeaseResult const second = arbiter.Acquire(secondFrame, secondDecision);
    EXPECT_FALSE(second.hasLease);
    EXPECT_EQ(second.receipt.reason, ReceiptReason::SquadOwnerConflict);

    EXPECT_EQ(arbiter.Release(firstFrame, first.lease).status, ReceiptStatus::Completed);
    LeaseResult const afterRelease = arbiter.Acquire(secondFrame, secondDecision);
    EXPECT_TRUE(afterRelease.hasLease);
    EXPECT_EQ(arbiter.ActiveSquadOwnerCount(), 1u);
}

TEST(AutoWowOracleContractTest, ItemReservationsArbitrateAcrossBotsAndAreIdempotent)
{
    WorldReadFrame firstFrame = MakeFrame(ScopeKind::PersistentCampaign, 90, 42);
    OracleCandidate itemCandidate = MakeVerifiedQuestCandidate("reserve:900");
    itemCandidate.itemId = 900;
    itemCandidate.quest = {true, 792, QuestObjectiveFamily::Item, 1, 0, 900};
    Decision const firstDecision = PlanOne(firstFrame, itemCandidate);

    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 90, 43);
    Decision const secondDecision = PlanOne(secondFrame, itemCandidate);
    OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.ReserveItem(firstFrame, firstDecision).reason, ReceiptReason::LeaseAcquired);
    EXPECT_EQ(arbiter.ReserveItem(firstFrame, firstDecision).reason, ReceiptReason::LeaseAlreadyOwned);
    EXPECT_EQ(arbiter.ReserveItem(secondFrame, secondDecision).reason,
        ReceiptReason::ItemReservationConflict);
    EXPECT_EQ(arbiter.ReleaseItem(firstFrame, firstDecision).status, ReceiptStatus::Completed);
    EXPECT_EQ(arbiter.ReserveItem(secondFrame, secondDecision).status, ReceiptStatus::Accepted);
}

TEST(AutoWowOracleContractTest, RenewReleaseAndExpiryProduceTypedReceipts)
{
    WorldReadFrame frame = MakeFrame();
    Decision const decision = PlanOne(frame, MakeVerifiedQuestCandidate("walk", 123, 100));
    OracleArbiter<> arbiter;
    LeaseResult const acquired = arbiter.Acquire(frame, decision);
    ASSERT_TRUE(acquired.hasLease);

    WorldReadFrame progressed = frame;
    progressed.version = 11;
    progressed.tick = 101;
    progressed.evidence.progress = 4;
    LeaseResult const renewed = arbiter.Renew(progressed, acquired.lease);
    ASSERT_TRUE(renewed.hasLease);
    EXPECT_EQ(renewed.receipt.status, ReceiptStatus::Progressing);
    EXPECT_EQ(renewed.receipt.before.progress, 3u);
    EXPECT_EQ(renewed.receipt.after.progress, 4u);
    EXPECT_FALSE(arbiter.OwnsActiveLease(acquired.lease, progressed.tick));
    EXPECT_TRUE(arbiter.OwnsActiveLease(renewed.lease, progressed.tick));
    EXPECT_EQ(arbiter.Renew(progressed, acquired.lease).receipt.reason, ReceiptReason::NotOwner);
    EXPECT_EQ(arbiter.Release(progressed, acquired.lease).reason, ReceiptReason::NotOwner);
    EXPECT_EQ(arbiter.Release(progressed, renewed.lease).status, ReceiptStatus::Completed);

    WorldReadFrame expiringFrame = MakeFrame();
    OracleCandidate shortCandidate = MakeVerifiedQuestCandidate("short", 42, 100);
    shortCandidate.ttlTicks = 1;
    Decision const shortDecision = PlanOne(expiringFrame, shortCandidate);
    LeaseResult const shortLease = arbiter.Acquire(expiringFrame, shortDecision);
    ASSERT_TRUE(shortLease.hasLease);
    expiringFrame.version = 11;
    expiringFrame.tick = 101;
    LeaseResult const expired = arbiter.Renew(expiringFrame, shortLease.lease);
    EXPECT_FALSE(expired.hasLease);
    EXPECT_EQ(expired.receipt.status, ReceiptStatus::Rejected);
    EXPECT_EQ(expired.receipt.reason, ReceiptReason::Expired);
}

TEST(AutoWowOracleContractTest, StaleFrameEpochAndPreconditionChangesAreRejected)
{
    WorldReadFrame sourceFrame = MakeFrame();
    Decision const decision = PlanOne(sourceFrame, MakeVerifiedQuestCandidate("quest", 792));
    OracleArbiter<> arbiter;

    WorldReadFrame staleFrame = sourceFrame;
    staleFrame.version = 11;
    EXPECT_EQ(arbiter.Acquire(staleFrame, decision).receipt.reason, ReceiptReason::StaleFrame);

    WorldReadFrame staleEpoch = sourceFrame;
    staleEpoch.epoch = 4;
    EXPECT_EQ(arbiter.Acquire(staleEpoch, decision).receipt.reason, ReceiptReason::StaleEpoch);

    WorldReadFrame changedFacts = sourceFrame;
    changedFacts.bot.mapId = 2;
    EXPECT_EQ(arbiter.Acquire(changedFacts, decision).receipt.reason,
        ReceiptReason::PreconditionFailed);
}

TEST(AutoWowOracleContractTest, LabOnlyIntentCannotEnterPersistentCampaign)
{
    WorldReadFrame persistent = MakeFrame(ScopeKind::PersistentCampaign);
    OracleCandidate labCandidate = MakeVerifiedQuestCandidate("lab-pull", 901, 100);
    labCandidate.classification = IntentClassification::LabOnly;
    Decision const persistentDecision = PlanOne(persistent, labCandidate);
    OracleArbiter<> arbiter;
    EXPECT_EQ(arbiter.Acquire(persistent, persistentDecision).receipt.reason,
        ReceiptReason::LabOnlyPersistentScope);

    WorldReadFrame lab = MakeFrame(ScopeKind::LabFixture);
    Decision const labDecision = PlanOne(lab, labCandidate);
    EXPECT_EQ(arbiter.Acquire(lab, labDecision).receipt.status, ReceiptStatus::Accepted);
}

TEST(AutoWowOracleContractTest, ReceiptVocabularyCarriesTypedEvidence)
{
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Accepted), "accepted");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Progressing), "progressing");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Completed), "completed");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Blocked), "blocked");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Failed), "failed");
    EXPECT_EQ(ReceiptStatusName(ReceiptStatus::Rejected), "rejected");

    WorldReadFrame frame = MakeFrame();
    frame.candidateCount = 0;
    PlanResult const blocked = Plan(frame);
    EXPECT_EQ(blocked.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(blocked.receipt.reason, ReceiptReason::NoEligibleIntent);
    EXPECT_EQ(blocked.receipt.before.objective, 2u);
    EXPECT_EQ(blocked.receipt.after.resource, 4u);

    OracleCandidate candidate = MakeVerifiedQuestCandidate("capture", 123);
    Decision const decision = PlanOne(frame, candidate);
    EXPECT_EQ(decision.evidence.pvp, 7u);
}

TEST(AutoWowOracleContractTest, FixedCapacityAndTieOrderingAreDeterministic)
{
    WorldReadFrame frame = MakeFrame();
    ASSERT_TRUE(AddCandidate(frame, MakeVerifiedQuestCandidate("zeta", 20)));
    ASSERT_TRUE(AddCandidate(frame, MakeVerifiedQuestCandidate("alpha", 10)));
    PlanResult const plan = Plan(frame);
    ASSERT_TRUE(plan.hasDecision);
    EXPECT_EQ(plan.decision.targetGuid, 10u);
    EXPECT_EQ(plan.decision.action, "alpha");

    OracleArbiter<1, 1, 1> bounded;
    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 78, 43);
    Decision const firstDecision = plan.decision;
    Decision const secondDecision = PlanOne(secondFrame, MakeVerifiedQuestCandidate("second", 30));
    ASSERT_TRUE(bounded.Acquire(frame, firstDecision).hasLease);
    LeaseResult const capacity = bounded.Acquire(secondFrame, secondDecision);
    EXPECT_FALSE(capacity.hasLease);
    EXPECT_EQ(capacity.receipt.reason, ReceiptReason::CapacityExceeded);
}

TEST(AutoWowOracleContractTest, RejectsUnboundedFramesBeforePolicyTraversal)
{
    WorldReadFrame frame = MakeFrame();
    frame.candidateCount = frame.candidates.size() + 1;
    PlanResult invalidCandidates = Plan(frame);
    EXPECT_FALSE(invalidCandidates.hasDecision);
    EXPECT_EQ(invalidCandidates.receipt.status, ReceiptStatus::Rejected);
    EXPECT_EQ(invalidCandidates.receipt.reason, ReceiptReason::InvalidFrame);

    frame = MakeFrame();
    frame.combat.enemies.resize(kMaxCombatEnemies + 1);
    PlanResult invalidCombatFacts = Plan(frame);
    EXPECT_FALSE(invalidCombatFacts.hasDecision);
    EXPECT_EQ(invalidCombatFacts.receipt.reason, ReceiptReason::InvalidFrame);

    frame = MakeFrame();
    frame.candidateCount = 1;
    frame.candidates[0].role = static_cast<RoleCode>(255);
    PlanResult invalidRole = Plan(frame);
    EXPECT_FALSE(invalidRole.hasDecision);
    EXPECT_EQ(invalidRole.receipt.reason, ReceiptReason::InvalidFrame);
}

TEST(AutoWowOracleContractTest, ExpiredCapacityIsReclaimedDeterministically)
{
    OracleArbiter<1, 1, 1> arbiter;
    WorldReadFrame firstFrame = MakeFrame(ScopeKind::PersistentCampaign, 91, 42);
    OracleCandidate shortCandidate = MakeVerifiedQuestCandidate("short", 123, 100);
    shortCandidate.ttlTicks = 1;
    Decision const firstDecision = PlanOne(firstFrame, shortCandidate);
    ASSERT_TRUE(arbiter.Acquire(firstFrame, firstDecision).hasLease);

    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 92, 43);
    secondFrame.tick = 101;
    secondFrame.version = 11;
    Decision const secondDecision = PlanOne(secondFrame,
        MakeVerifiedQuestCandidate("after_expiry", 792));
    EXPECT_TRUE(arbiter.Acquire(secondFrame, secondDecision).hasLease);
}

TEST(AutoWowOracleContractTest, TransitionProofIsOptionalButDirectedAndIdentityBound)
{
    WorldReadFrame frame = MakeFrame();
    OracleCandidate ordinary = MakeVerifiedQuestCandidate("ordinary", 700);
    Decision const ordinaryDecision = MakeDecision(frame, ordinary);
    EXPECT_EQ(Detail::ValidateDecision(frame, ordinaryDecision), ReceiptReason::None);

    OracleCandidate transition = MakeVerifiedQuestCandidate("travel", 701, 100);
    transition.requiresSameMap = false;
    transition.transition = {true, 77, 1, 2, 3, 4, true, true, false, false};
    Decision const decision = MakeDecision(frame, transition);
    ASSERT_TRUE(decision.valid);
    EXPECT_EQ(decision.preconditions.transition.stableId, 77U);
    EXPECT_EQ(decision.preconditions.transition.destinationMapId, 3U);
    EXPECT_EQ(Detail::ValidateDecision(frame, decision), ReceiptReason::None);
    EXPECT_EQ(Detail::ValidateRenewFrame(frame, decision), ReceiptReason::None);

    OracleCandidate otherTransition = transition;
    otherTransition.transition.stableId = 78;
    EXPECT_NE(decision.decisionId, MakeDecision(frame, otherTransition).decisionId);
    EXPECT_TRUE(Detail::CandidateLess(transition, otherTransition) ||
        Detail::CandidateLess(otherTransition, transition));

    auto rejected = [&](auto mutate) {
        Decision invalid = decision;
        mutate(invalid.preconditions.transition);
        EXPECT_EQ(Detail::ValidateDecision(frame, invalid), ReceiptReason::PreconditionFailed);
        EXPECT_EQ(Detail::ValidateRenewFrame(frame, invalid), ReceiptReason::PreconditionFailed);
    };
    rejected([](TransitionPrecondition& value) { value.stableId = 0; });
    rejected([](TransitionPrecondition& value) { value.sourceMapId = 9; });
    rejected([](TransitionPrecondition& value) { value.validated = false; });
    rejected([](TransitionPrecondition& value) { value.directedProof = false; });
    rejected([](TransitionPrecondition& value) { value.cheatBacked = true; });
    rejected([](TransitionPrecondition& value) { value.grantBacked = true; });

    Decision mismatched = decision;
    mismatched.preconditions.requireSameMap = true;
    EXPECT_EQ(Detail::ValidateDecision(frame, mismatched), ReceiptReason::PreconditionFailed);

    OracleCandidate missing = transition;
    missing.transition = {true};
    Decision missingDecision = MakeDecision(frame, missing);
    ASSERT_TRUE(missingDecision.valid);
    EXPECT_EQ(Detail::ValidateDecision(frame, missingDecision), ReceiptReason::PreconditionFailed);
}

TEST(AutoWowOracleContractTest, UnsupportedExecutorIsBlockedAndSameMapRouteProofIsTyped)
{
    WorldReadFrame auctionFrame = MakeFrame();
    OracleCandidate auction = MakeCandidate(Domain::CraftingEconomyItems,
        IntentCode::CraftEconomyItems, LeaseResource::QuestGather, "auction", 0, 20);
    auction.qualifier = "buy";
    auction.itemId = 303;
    auction.operation = OperationCode::AuctionBuy;
    auction.executorAvailable = false;
    ASSERT_TRUE(AddCandidate(auctionFrame, auction));
    PlanResult const blocked = Plan(auctionFrame);
    EXPECT_FALSE(blocked.hasDecision);
    EXPECT_EQ(blocked.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(blocked.receipt.reason, ReceiptReason::ExecutorBlocked);
    Decision const missingExecutor = MakeDecision(auctionFrame, auction);
    ASSERT_TRUE(missingExecutor.valid);
    EXPECT_EQ(Detail::ValidateDecision(auctionFrame, missingExecutor), ReceiptReason::ExecutorBlocked);
    OracleArbiter<> auctionArbiter;
    EXPECT_FALSE(auctionArbiter.Acquire(auctionFrame, missingExecutor).hasLease);
    EXPECT_EQ(auctionArbiter.ActiveBotLeaseCount(), 0u);

    WorldReadFrame routeFrame = MakeFrame();
    OracleCandidate route = MakeVerifiedQuestCandidate("move to travel target", 80, 50);
    route.qualifier = "legal_route";
    route.route = {true, 80, routeFrame.bot.mapId, true, true, true, true};
    Decision const routeDecision = MakeDecision(routeFrame, route);
    ASSERT_TRUE(routeDecision.valid);
    EXPECT_EQ(routeDecision.qualifier, "legal_route");
    EXPECT_EQ(routeDecision.preconditions.frameVersion, routeFrame.version);
    EXPECT_EQ(routeDecision.preconditions.epoch, routeFrame.epoch);
    EXPECT_EQ(routeDecision.preconditions.botGuid, routeFrame.bot.guid);
    EXPECT_EQ(routeDecision.preconditions.squadId, routeFrame.scope.squadId);
    EXPECT_EQ(routeDecision.preconditions.mapId, routeFrame.bot.mapId);
    EXPECT_EQ(routeDecision.preconditions.instanceId, routeFrame.bot.instanceId);
    EXPECT_TRUE(routeDecision.preconditions.requireBotAlive);
    EXPECT_TRUE(routeDecision.preconditions.requireSameMap);
    EXPECT_TRUE(routeDecision.preconditions.route.required);
    EXPECT_EQ(Detail::ValidateDecision(routeFrame, routeDecision), ReceiptReason::None);

    Decision invalidRoute = routeDecision;
    invalidRoute.preconditions.route.pathComplete = false;
    EXPECT_EQ(Detail::ValidateDecision(routeFrame, invalidRoute), ReceiptReason::PreconditionFailed);
    EXPECT_EQ(Detail::ValidateRenewFrame(routeFrame, invalidRoute), ReceiptReason::PreconditionFailed);
}

TEST(AutoWowOracleContractTest, DiagnosticLabelsDoNotChangeTypedIdentity)
{
    WorldReadFrame frame = MakeFrame();
    OracleCandidate first = MakeVerifiedQuestCandidate("quest.objective", 792);
    OracleCandidate second = first;
    second.action = "a completely different diagnostic label";
    second.qualifier = "another diagnostic label";

    EXPECT_EQ(Detail::CandidateKey(first), Detail::CandidateKey(second));
    EXPECT_FALSE(Detail::CandidateLess(first, second));
    EXPECT_FALSE(Detail::CandidateLess(second, first));
    Decision const left = MakeDecision(frame, first);
    Decision const right = MakeDecision(frame, second);
    EXPECT_EQ(left.decisionId, right.decisionId);
    EXPECT_EQ(left.intentId, right.intentId);
    EXPECT_EQ(left.operation, OperationCode::QuestObjective);
}

TEST(AutoWowOracleContractTest, TypedPayloadsRoundTripIntoDecisionAndReceipt)
{
    WorldReadFrame questFrame = MakeFrame();
    OracleCandidate quest = MakeVerifiedQuestCandidate("quest.objective", 792);
    quest.itemId = 1234;
    quest.quest = {true, 792, QuestObjectiveFamily::Item, 2, 0, 1234};
    ASSERT_TRUE(AddCandidate(questFrame, quest));
    PlanResult const questPlan = Plan(questFrame);
    ASSERT_TRUE(questPlan.hasDecision);
    EXPECT_EQ(questPlan.decision.operation, OperationCode::QuestObjective);
    EXPECT_EQ(questPlan.decision.quest.questId, 792U);
    EXPECT_EQ(questPlan.decision.quest.objectiveSlot, 2U);
    EXPECT_EQ(questPlan.receipt.quest.requiredItemId, 1234U);

    WorldReadFrame gatherFrame = MakeFrame();
    OracleCandidate gather = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "worker gather seek", 0);
    gather.targetGuid = 9001;
    gather.itemId = 600;
    gather.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};
    gather.executorAvailable = true;
    ASSERT_TRUE(AddCandidate(gatherFrame, gather));
    PlanResult const gatherPlan = Plan(gatherFrame);
    ASSERT_TRUE(gatherPlan.hasDecision);
    EXPECT_EQ(gatherPlan.decision.operation, OperationCode::GatherSource);
    EXPECT_EQ(gatherPlan.decision.gather.spawnId, 9001U);
    EXPECT_EQ(gatherPlan.decision.gather.entry, 12345U);
}

TEST(AutoWowOracleContractTest, MissingTypedPayloadIsExecutorBlocked)
{
    WorldReadFrame frame = MakeFrame();
    OracleCandidate missing = MakeCandidate(Domain::Quest, IntentCode::QuestObjective,
        LeaseResource::QuestGather, "quest.objective", 792);
    missing.quest = {};
    ASSERT_TRUE(AddCandidate(frame, missing));
    PlanResult const blocked = Plan(frame);
    EXPECT_FALSE(blocked.hasDecision);
    EXPECT_EQ(blocked.receipt.status, ReceiptStatus::Blocked);
    EXPECT_EQ(blocked.receipt.reason, ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(blocked.receipt.operation, OperationCode::QuestObjective);
}

TEST(AutoWowOracleContractTest, CurrentVerifiedOperationGateBlocksUnverifiedDomains)
{
    OracleCandidate navigation = MakeCandidate(Domain::Navigation, IntentCode::Navigate,
        LeaseResource::Transition, "navigate", 1);
    OracleCandidate recovery = MakeCandidate(Domain::Recovery, IntentCode::Recover,
        LeaseResource::Recovery, "recover", 1);
    OracleCandidate roster = MakeCandidate(Domain::GroupRoster, IntentCode::RosterAssignment,
        LeaseResource::Transition, "roster", 1);
    OracleCandidate craft = MakeCandidate(Domain::CraftingEconomyItems,
        IntentCode::CraftEconomyItems, LeaseResource::QuestGather, "craft", 1);
    craft.itemId = 1;
    OracleCandidate combat = MakeCandidate(Domain::Combat, IntentCode::CombatAction,
        LeaseResource::CombatPositioning, "combat", 1);

    auto expectBlocked = [](OracleCandidate const& candidate) {
        EXPECT_FALSE(candidate.executorAvailable);
        WorldReadFrame frame = MakeFrame();
        ASSERT_TRUE(AddCandidate(frame, candidate));
        PlanResult const result = Plan(frame);
        EXPECT_FALSE(result.hasDecision);
        EXPECT_EQ(result.receipt.status, ReceiptStatus::Blocked);
        EXPECT_EQ(result.receipt.reason, ReceiptReason::ExecutorBlocked);
    };
    expectBlocked(navigation);
    expectBlocked(recovery);
    expectBlocked(roster);
    expectBlocked(craft);
    expectBlocked(combat);

    WorldReadFrame verifiedFrame = MakeFrame();
    ASSERT_TRUE(AddCandidate(verifiedFrame, MakeVerifiedQuestCandidate("quest.objective")));
    PlanResult const verified = Plan(verifiedFrame);
    ASSERT_TRUE(verified.hasDecision);
    EXPECT_EQ(verified.decision.operation, OperationCode::QuestObjective);
}

TEST(AutoWowOracleContractTest, DirectAcquireAndRenewEnforceCurrentVerifiedAllowlist)
{
    OracleCandidate gather = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "gather", 9001);
    gather.itemId = 600;
    gather.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};
    gather.executorAvailable = true;

    OracleCandidate pvp = MakeCandidate(Domain::PvP, IntentCode::PvPObjective,
        LeaseResource::CombatPositioning, "pvp", 7001);
    pvp.executorAvailable = true;

    OracleCandidate navigation = MakeCandidate(Domain::Navigation, IntentCode::Navigate,
        LeaseResource::Transition, "travel", 7002);
    navigation.executorAvailable = true;

    OracleCandidate recovery = MakeCandidate(Domain::Recovery, IntentCode::Recover,
        LeaseResource::Recovery, "recover", 7003);
    recovery.executorAvailable = true;

    OracleCandidate craft = MakeCandidate(Domain::CraftingEconomyItems,
        IntentCode::CraftEconomyItems, LeaseResource::QuestGather, "craft", 0);
    craft.itemId = 601;
    craft.operation = OperationCode::Craft;
    craft.executorAvailable = true;

    OracleCandidate roster = MakeCandidate(Domain::GroupRoster, IntentCode::RosterAssignment,
        LeaseResource::Transition, "roster", 7004);
    roster.operation = OperationCode::RosterAssignment;
    roster.executorAvailable = true;

    OracleCandidate questAcquire = MakeCandidate(Domain::Quest, IntentCode::QuestObjective,
        LeaseResource::QuestGather, "quest.acquire", 7005);
    questAcquire.operation = OperationCode::QuestAcquire;
    questAcquire.executorAvailable = true;

    OracleCandidate inconsistentQuest = MakeVerifiedQuestCandidate("quest.objective", 7006);
    inconsistentQuest.itemId = 999;

    auto expectBlocked = [](OracleCandidate const& candidate) {
        WorldReadFrame frame = MakeFrame();
        Decision const decision = MakeDecision(frame, candidate);
        ASSERT_TRUE(decision.valid);

        OracleArbiter<> acquireArbiter;
        LeaseResult const acquired = acquireArbiter.Acquire(frame, decision);
        EXPECT_FALSE(acquired.hasLease);
        EXPECT_EQ(acquired.receipt.reason, ReceiptReason::ExecutorBlocked);
        EXPECT_EQ(acquireArbiter.ActiveBotLeaseCount(), 0U);

        OracleArbiter<> renewArbiter;
        IntentLease const seeded = OracleArbiterContractTestAccess::InstallLease(
            renewArbiter, decision);
        ASSERT_TRUE(seeded.valid);
        WorldReadFrame renewFrame = frame;
        renewFrame.version = frame.version + 1;
        renewFrame.tick = frame.tick + 1;
        LeaseResult const renewed = renewArbiter.Renew(renewFrame, seeded);
        EXPECT_FALSE(renewed.hasLease);
        EXPECT_EQ(renewed.receipt.reason, ReceiptReason::ExecutorBlocked);
        EXPECT_EQ(renewArbiter.ActiveBotLeaseCount(), 0U);
    };

    WorldReadFrame gatherFrame = MakeFrame();
    ASSERT_TRUE(AddCandidate(gatherFrame, gather));
    Decision const gatherDecision = MakeDecision(gatherFrame, gather);
    ASSERT_TRUE(gatherDecision.valid);
    OracleArbiter<> gatherArbiter;
    EXPECT_TRUE(gatherArbiter.Acquire(gatherFrame, gatherDecision).hasLease);

    expectBlocked(pvp);
    expectBlocked(navigation);
    expectBlocked(recovery);
    expectBlocked(craft);
    expectBlocked(roster);
    expectBlocked(questAcquire);
    expectBlocked(inconsistentQuest);
}

TEST(AutoWowOracleContractTest, PreemptedUnexpiredLeaseIsRejectedByOwnershipQuery)
{
    OracleArbiter<> arbiter;
    WorldReadFrame lowFrame = MakeFrame();
    Decision const lowDecision = PlanOne(lowFrame, MakeVerifiedQuestCandidate("walk", 10, 10));
    LeaseResult const low = arbiter.Acquire(lowFrame, lowDecision);
    ASSERT_TRUE(low.hasLease);

    WorldReadFrame highFrame = MakeFrame();
    Decision const highDecision = PlanOne(highFrame, MakeVerifiedQuestCandidate("recover", 11,
        500));
    LeaseResult const high = arbiter.Acquire(highFrame, highDecision);
    ASSERT_TRUE(high.hasLease);

    ActiveLeaseSnapshot const snapshot = arbiter.QueryActiveLease(highFrame.bot.guid,
        highFrame.tick);
    ASSERT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.decisionId, highDecision.decisionId);
    EXPECT_EQ(snapshot.intentId, highDecision.intentId);
    EXPECT_EQ(snapshot.operation, OperationCode::QuestObjective);
    EXPECT_FALSE(arbiter.OwnsActiveLease(low.lease, highFrame.tick));
    EXPECT_TRUE(arbiter.OwnsActiveLease(high.lease, highFrame.tick));
    EXPECT_EQ(arbiter.Renew(highFrame, low.lease).receipt.reason, ReceiptReason::NotOwner);
}

TEST(AutoWowOracleContractTest, NodeReservationsAreBoundedAndOwnedByTypedSource)
{
    OracleArbiter<2, 2, 2, 1> arbiter;
    WorldReadFrame firstFrame = MakeFrame(ScopeKind::PersistentCampaign, 77, 42);
    OracleCandidate source = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "worker gather seek", 0, 100);
    source.itemId = 600;
    source.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};
    Decision const firstDecision = MakeContractOnlyGatherDecision(firstFrame, source);
    IntentLease const unacquired{true, firstDecision};
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, firstFrame, unacquired).reason,
        ReceiptReason::NotOwner);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReleaseNode(arbiter, firstFrame, unacquired).reason,
        ReceiptReason::NotOwner);

    IntentLease const firstLease = OracleArbiterContractTestAccess::InstallLease(
        arbiter, firstDecision);
    ASSERT_TRUE(firstLease.valid);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, firstFrame, firstLease).reason,
        ReceiptReason::LeaseAcquired);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, firstFrame, firstLease).reason,
        ReceiptReason::LeaseAlreadyOwned);

    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 77, 43);
    Decision const secondDecision = MakeContractOnlyGatherDecision(secondFrame, source);
    IntentLease const secondLease = OracleArbiterContractTestAccess::InstallLease(
        arbiter, secondDecision);
    ASSERT_TRUE(secondLease.valid);
    Receipt const conflict = OracleArbiterContractTestAccess::ReserveNode(
        arbiter, secondFrame, secondLease);
    EXPECT_EQ(conflict.reason, ReceiptReason::NodeReservationConflict);
    EXPECT_EQ(arbiter.ActiveNodeReservationCount(), 1U);
    EXPECT_EQ(arbiter.Release(firstFrame, firstLease).status, ReceiptStatus::Completed);
    EXPECT_EQ(arbiter.ActiveNodeReservationCount(), 0U);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, secondFrame, secondLease).status,
        ReceiptStatus::Accepted);
}

TEST(AutoWowOracleContractTest, NodeReservationsUseFullMapInstanceSpawnEntryMaterialGoalIdentity)
{
    OracleArbiter<4, 4, 4, 3> arbiter;
    OracleCandidate source = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "worker gather seek", 0, 100);
    source.itemId = 600;
    source.gather = {true, 9001, 12345, 1, 600, 2, GatherGoal::ObtainMaterial};

    WorldReadFrame firstFrame = MakeFrame(ScopeKind::PersistentCampaign, 77, 42);
    firstFrame.bot.instanceId = 2;
    Decision const firstDecision = MakeContractOnlyGatherDecision(firstFrame, source);
    IntentLease const firstLease = OracleArbiterContractTestAccess::InstallLease(arbiter, firstDecision);
    ASSERT_TRUE(firstLease.valid);
    ASSERT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, firstFrame, firstLease).status,
              ReceiptStatus::Accepted);

    OracleCandidate differentInstance = source;
    differentInstance.gather.instanceId = 3;
    WorldReadFrame secondFrame = MakeFrame(ScopeKind::PersistentCampaign, 77, 43);
    secondFrame.bot.instanceId = 3;
    Decision const secondDecision = MakeContractOnlyGatherDecision(secondFrame, differentInstance);
    IntentLease const secondLease = OracleArbiterContractTestAccess::InstallLease(arbiter, secondDecision);
    ASSERT_TRUE(secondLease.valid);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, secondFrame, secondLease).status,
              ReceiptStatus::Accepted);

    OracleCandidate differentMaterial = source;
    differentMaterial.itemId = 601;
    differentMaterial.gather.materialItemId = 601;
    WorldReadFrame thirdFrame = MakeFrame(ScopeKind::PersistentCampaign, 77, 44);
    thirdFrame.bot.instanceId = 2;
    Decision const thirdDecision = MakeContractOnlyGatherDecision(thirdFrame, differentMaterial);
    IntentLease const thirdLease = OracleArbiterContractTestAccess::InstallLease(arbiter, thirdDecision);
    ASSERT_TRUE(thirdLease.valid);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, thirdFrame, thirdLease).status,
              ReceiptStatus::Accepted);

    EXPECT_EQ(arbiter.ActiveNodeReservationCount(), 3U);
    EXPECT_FALSE(SameGatherSourceReference(source.gather, differentInstance.gather));
    EXPECT_FALSE(SameGatherSourceReference(source.gather, differentMaterial.gather));
    OracleCandidate differentGoal = source;
    differentGoal.gather.goal = GatherGoal::HarvestNode;
    EXPECT_FALSE(SameGatherSourceReference(source.gather, differentGoal.gather));
}

TEST(AutoWowOracleContractTest, NodeReservationsRenewWithLeaseAndClearLifecycleEvents)
{
    OracleCandidate source = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "worker gather seek", 0, 100);
    source.itemId = 600;
    source.ttlTicks = 5;
    source.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};

    OracleArbiter<2, 2, 2, 1> renewedArbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = MakeContractOnlyGatherDecision(frame, source);
    IntentLease const acquired = OracleArbiterContractTestAccess::InstallLease(
        renewedArbiter, decision);
    ASSERT_TRUE(acquired.valid);
    ASSERT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        renewedArbiter, frame, acquired).reason,
        ReceiptReason::LeaseAcquired);

    WorldReadFrame progressed = frame;
    progressed.version = 11;
    progressed.tick = 101;
    progressed.evidence.progress = 4;
    LeaseResult const renewed = OracleArbiterContractTestAccess::RenewContractLease(
        renewedArbiter, progressed, acquired);
    ASSERT_TRUE(renewed.hasLease);
    EXPECT_EQ(renewedArbiter.ActiveNodeReservationCount(), 1U);

    // The original lease would expire at tick 105. A renewed reservation must still exist
    // there because the owning lease now expires at tick 106.
    WorldReadFrame nearOriginalExpiry = progressed;
    nearOriginalExpiry.tick = decision.expiresTick;
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        renewedArbiter, nearOriginalExpiry, renewed.lease).reason,
        ReceiptReason::LeaseAlreadyOwned);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReleaseNode(
        renewedArbiter, nearOriginalExpiry, renewed.lease).status,
        ReceiptStatus::Completed);
    EXPECT_EQ(renewedArbiter.ActiveNodeReservationCount(), 0U);

    OracleArbiter<2, 2, 2, 1> preemptedArbiter;
    IntentLease const first = OracleArbiterContractTestAccess::InstallLease(
        preemptedArbiter, decision);
    ASSERT_TRUE(first.valid);
    ASSERT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        preemptedArbiter, frame, first).status, ReceiptStatus::Accepted);

    WorldReadFrame highFrame = frame;
    Decision const highDecision = PlanOne(highFrame, MakeVerifiedQuestCandidate("recover", 99,
        500));
    ASSERT_TRUE(preemptedArbiter.Acquire(highFrame, highDecision).hasLease);
    EXPECT_EQ(preemptedArbiter.ActiveNodeReservationCount(), 0U);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        preemptedArbiter, frame, first).reason, ReceiptReason::NotOwner);
    EXPECT_EQ(OracleArbiterContractTestAccess::ReleaseNode(
        preemptedArbiter, frame, first).reason, ReceiptReason::NotOwner);

    OracleArbiter<2, 2, 2, 1> expiredArbiter;
    IntentLease const shortLease = OracleArbiterContractTestAccess::InstallLease(
        expiredArbiter, decision);
    ASSERT_TRUE(shortLease.valid);
    ASSERT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        expiredArbiter, frame, shortLease).status,
        ReceiptStatus::Accepted);

    WorldReadFrame expiredFrame = frame;
    expiredFrame.tick = decision.expiresTick;
    EXPECT_EQ(OracleArbiterContractTestAccess::ReserveNode(
        expiredArbiter, expiredFrame, shortLease).reason,
        ReceiptReason::Expired);
    EXPECT_EQ(expiredArbiter.ActiveNodeReservationCount(), 0U);
    EXPECT_EQ(expiredArbiter.ActiveBotLeaseCount(), 0U);
}

TEST(AutoWowOracleContractTest, FailedRenewalClearsBotLeaseAndNodeReservation)
{
    OracleCandidate source = MakeCandidate(Domain::Gathering, IntentCode::GatherSource,
        LeaseResource::QuestGather, "worker gather seek", 0, 100);
    source.itemId = 600;
    source.ttlTicks = 20;
    source.gather = {true, 9001, 12345, 1, 600, 0, GatherGoal::ObtainMaterial};

    OracleArbiter<2, 2, 2, 1> arbiter;
    WorldReadFrame frame = MakeFrame();
    Decision const decision = MakeContractOnlyGatherDecision(frame, source);
    IntentLease const acquired = OracleArbiterContractTestAccess::InstallLease(arbiter, decision);
    ASSERT_TRUE(acquired.valid);
    ASSERT_EQ(OracleArbiterContractTestAccess::ReserveNode(arbiter, frame, acquired).status,
        ReceiptStatus::Accepted);
    ASSERT_EQ(arbiter.ActiveBotLeaseCount(), 1U);
    ASSERT_EQ(arbiter.ActiveNodeReservationCount(), 1U);

    WorldReadFrame invalidFrame = frame;
    invalidFrame.version = 11;
    invalidFrame.tick = 101;
    invalidFrame.bot.mapId = frame.bot.mapId + 1;
    LeaseResult const failed = OracleArbiterContractTestAccess::RenewContractLease(
        arbiter, invalidFrame, acquired);
    EXPECT_FALSE(failed.hasLease);
    EXPECT_EQ(failed.receipt.status, ReceiptStatus::Rejected);
    EXPECT_EQ(failed.receipt.reason, ReceiptReason::PreconditionFailed);
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);
    EXPECT_EQ(arbiter.ActiveNodeReservationCount(), 0U);
}
