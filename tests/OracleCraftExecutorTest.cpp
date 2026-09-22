#include "../src/AutoWow/OracleCraftExecutor.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <type_traits>

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowOracleCraftExecutor;
using AutoWowOracleExecutor::ExecutorStatus;

constexpr Guid kBot = 77;
constexpr SquadId kSquad = 8;
constexpr std::uint32_t kMap = 1;
constexpr std::uint32_t kInstance = 9;
constexpr std::uint32_t kRecipeSpell = 20001;
constexpr ItemId kOutput = 30001;
constexpr ItemId kReagentA = 40001;
constexpr ItemId kReagentB = 40002;
constexpr DecisionId kDecision = 1001;
constexpr IntentId kIntent = 2001;
constexpr Epoch kEpoch = 4;
constexpr FactVersion kFrame = 12;
constexpr Tick kIssued = 20;
constexpr Tick kExpiry = 40;

Scope TestScope()
{
    return {ScopeKind::LabFixture, kSquad, kBot};
}

ActiveLeaseSnapshot Ownership(DecisionId decisionId = kDecision, IntentId intentId = kIntent,
                              Epoch epoch = kEpoch, Tick expiresTick = kExpiry,
                              OperationCode operation = OperationCode::Craft)
{
    ActiveLeaseSnapshot proof;
    proof.active = true;
    proof.botGuid = kBot;
    proof.decisionId = decisionId;
    proof.intentId = intentId;
    proof.epoch = epoch;
    proof.expiresTick = expiresTick;
    proof.scope = TestScope();
    proof.operation = operation;
    return proof;
}

CraftReference Reference()
{
    CraftReference reference;
    reference.valid = true;
    reference.recipeSpellId = kRecipeSpell;
    reference.outputItemId = kOutput;
    reference.outputCount = 2;
    reference.reagents[0] = {kReagentA, 3};
    reference.reagents[1] = {kReagentB, 1};
    reference.reagentCount = 2;
    reference.profession = Profession::Alchemy;
    reference.requiredSkill = 125;
    reference.skillAtDecision = 150;
    reference.maximumSkillAtDecision = 225;
    reference.professionKnown = true;
    reference.knownRecipe = true;
    return reference;
}

EvidenceCounters Evidence()
{
    return {1, 2, 3, 4, 5, 0, 6, 0};
}

CraftRequest Request()
{
    CraftRequest request;
    request.request.valid = true;
    request.request.requestId = 70001;
    request.request.operation = OperationCode::Craft;
    request.request.domain = Domain::CraftingEconomyItems;
    request.request.intent = IntentCode::CraftEconomyItems;
    request.request.resource = LeaseResource::QuestGather;
    request.request.scope = TestScope();
    request.request.decisionId = kDecision;
    request.request.intentId = kIntent;
    request.request.actorGuid = kBot;
    request.request.itemId = kOutput;
    request.request.executorAvailable = true;
    request.request.epoch = kEpoch;
    request.request.frameVersion = kFrame;
    request.request.issuedTick = kIssued;
    request.request.expiresTick = kExpiry;
    request.request.ttlTicks = kExpiry - kIssued;
    request.request.proofBaseline = Evidence();
    request.request.ownershipProof = Ownership();
    request.craft = Reference();
    return request;
}

IntentLease Lease()
{
    Decision decision;
    decision.valid = true;
    decision.decisionId = kDecision;
    decision.intentId = kIntent;
    decision.domain = Domain::CraftingEconomyItems;
    decision.intent = IntentCode::CraftEconomyItems;
    decision.resource = LeaseResource::QuestGather;
    decision.scope = TestScope();
    decision.actorGuid = kBot;
    decision.itemId = kOutput;
    decision.executorAvailable = true;
    decision.operation = OperationCode::Craft;
    decision.epoch = kEpoch;
    decision.issuedTick = kIssued;
    decision.expiresTick = kExpiry;
    decision.ttlTicks = kExpiry - kIssued;
    decision.evidence = Evidence();
    decision.preconditions.frameVersion = kFrame;
    decision.preconditions.epoch = kEpoch;
    decision.preconditions.botGuid = kBot;
    decision.preconditions.squadId = kSquad;
    decision.preconditions.mapId = kMap;
    decision.preconditions.instanceId = kInstance;
    decision.preconditions.requireBotAlive = true;
    decision.preconditions.requireSameMap = true;
    return {true, decision};
}

CraftCandidate Candidate()
{
    CraftCandidate candidate;
    candidate.reference = Reference();
    return candidate;
}

CraftObservation Before()
{
    CraftObservation observation;
    observation.world.onWorldThread = true;
    observation.world.botAlive = true;
    observation.world.inCombat = false;
    observation.world.cheatPathActive = false;
    observation.world.knownRecipe = true;
    observation.world.professionKnown = true;
    observation.world.botGuid = kBot;
    observation.world.mapId = kMap;
    observation.world.instanceId = kInstance;
    observation.world.currentSkill = 150;
    observation.world.maximumSkill = 225;
    observation.world.tick = kIssued + 1;
    observation.world.frameVersion = kFrame;
    observation.world.epoch = kEpoch;
    observation.world.ownershipProof = Ownership();
    observation.candidate = Candidate();
    observation.inventory.reagentCounts[0] = 10;
    observation.inventory.reagentCounts[1] = 4;
    observation.inventory.outputCount = 5;
    observation.inventory.freeOutputCapacity = 2;
    return observation;
}

CraftObservation After()
{
    CraftObservation observation = Before();
    observation.world.tick = kIssued + 2;
    observation.world.frameVersion = kFrame + 1;
    observation.inventory.reagentCounts[0] = 7;
    observation.inventory.reagentCounts[1] = 3;
    observation.inventory.outputCount = 7;
    observation.inventory.freeOutputCapacity = 0;
    return observation;
}

struct NativeProbe
{
    int calls = 0;
    CraftStepPhase phase = CraftStepPhase::Unknown;
    std::uint8_t maxSteps = 0;
    bool stopAfterStep = false;
    CraftCandidate candidate;
    CraftObservation after = After();
    bool accept = true;
    std::uint8_t steps = 1;
    bool selectedAnotherRecipe = false;
};

NativeStepObservation NativeStep(void* context, NativeStepRequest const& request)
{
    NativeProbe* probe = static_cast<NativeProbe*>(context);
    ++probe->calls;
    probe->phase = request.phase;
    probe->maxSteps = request.maxSteps;
    probe->stopAfterStep = request.stopAfterStep;
    probe->candidate = request.candidate;

    NativeStepObservation observation;
    observation.dispatchAccepted = probe->accept;
    observation.stepsInvoked = probe->steps;
    observation.selectedAnotherRecipe = probe->selectedAnotherRecipe;
    observation.after = probe->after;
    return observation;
}

ValidationResult Validate(CraftRequest const& request = Request(),
                          IntentLease const& lease = Lease(),
                          CraftCandidate const& candidate = Candidate(),
                          CraftObservation const& observation = Before())
{
    return OracleCraftExecutor::Validate(request, lease, candidate, observation);
}

TEST(OracleCraftExecutorTest, UsesTypedRecipeIdentityAndLeavesRuntimeAllowlistClosed)
{
    static_assert(std::is_same_v<decltype(CraftReference{}.recipeSpellId), std::uint32_t>);
    static_assert(std::is_same_v<decltype(CraftReference{}.outputItemId), ItemId>);
    static_assert(std::is_same_v<decltype(CraftReference{}.reagents[0].quantity),
                                 std::uint32_t>);

    CraftRequest const request = Request();
    EXPECT_TRUE(ValidCraftReference(request.craft));
    EXPECT_EQ(request.craft.recipeSpellId, kRecipeSpell);
    EXPECT_EQ(request.craft.outputItemId, kOutput);
    EXPECT_EQ(request.craft.outputCount, 2U);
    EXPECT_EQ(request.craft.reagents[0].itemId, kReagentA);
    EXPECT_EQ(request.craft.reagents[1].quantity, 1U);

    // Craft remains planning-visible but is not current runtime authority. This executor must not
    // turn this false into true by calling PrepareRequest or changing the shared contract.
    EXPECT_FALSE(IsVerifiedOperationTuple(request.request.executorAvailable,
                                           request.request.domain, request.request.intent,
                                           request.request.resource, request.request.operation,
                                           request.request.quest, request.request.gather,
                                           request.request.itemId, request.request.objectiveId));
}

TEST(OracleCraftExecutorTest, RejectsMalformedOrInadequatelyProvenReferences)
{
    CraftReference malformed = Reference();
    malformed.recipeSpellId = 0;
    EXPECT_FALSE(ValidCraftReference(malformed));

    malformed = Reference();
    malformed.reagents[1].itemId = kReagentA;
    EXPECT_FALSE(ValidCraftReference(malformed));

    malformed = Reference();
    malformed.knownRecipe = false;
    EXPECT_FALSE(ValidCraftReference(malformed));

    malformed = Reference();
    malformed.skillAtDecision = malformed.requiredSkill - 1;
    EXPECT_FALSE(ValidCraftReference(malformed));

    malformed = Reference();
    malformed.maximumSkillAtDecision = malformed.requiredSkill - 1;
    EXPECT_FALSE(ValidCraftReference(malformed));
}

TEST(OracleCraftExecutorTest, ExactCandidateMatcherRejectsEveryRecipeIdentityMismatch)
{
    CraftCandidate const expected = Candidate();
    CraftCandidate observed = expected;
    ASSERT_TRUE(ExactCandidateMatches(expected, observed));

    observed.reference.recipeSpellId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.outputItemId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.outputCount++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.reagents[0].itemId++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.reagents[1].quantity++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.profession = Profession::Engineering;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
    observed = expected;
    observed.reference.skillAtDecision++;
    EXPECT_FALSE(ExactCandidateMatches(expected, observed));
}

TEST(OracleCraftExecutorTest, RejectsWrongOperationDomainIntentAndResource)
{
    CraftRequest request = Request();
    request.request.operation = OperationCode::GatherSource;
    EXPECT_EQ(Validate(request).reason, CraftReason::WrongOperation);

    request = Request();
    request.request.domain = Domain::Gathering;
    EXPECT_EQ(Validate(request).reason, CraftReason::WrongDomain);

    request = Request();
    request.request.intent = IntentCode::GatherSource;
    EXPECT_EQ(Validate(request).reason, CraftReason::WrongIntent);

    request = Request();
    request.request.resource = LeaseResource::Transition;
    EXPECT_EQ(Validate(request).reason, CraftReason::WrongResource);
}

TEST(OracleCraftExecutorTest, RejectsMissingExpiredPreemptedAndMismatchedLeases)
{
    EXPECT_EQ(Validate(Request(), IntentLease{}).reason, CraftReason::MissingLease);

    CraftObservation expired = Before();
    expired.world.tick = kExpiry;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), expired).reason,
              CraftReason::LeaseExpired);

    CraftObservation preempted = Before();
    preempted.world.ownershipProof.decisionId++;
    EXPECT_EQ(Validate().reason, CraftReason::None);
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), preempted).reason,
              CraftReason::LeasePreempted);

    CraftRequest request = Request();
    request.request.ownershipProof.expiresTick++;
    EXPECT_EQ(Validate(request).reason, CraftReason::LeaseMismatch);
}

TEST(OracleCraftExecutorTest, RejectsStaleOrUnsafeWorldFacts)
{
    CraftObservation observation = Before();
    observation.world.onWorldThread = false;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::NotWorldThread);

    observation = Before();
    observation.world.frameVersion = kFrame - 1;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::StaleFrame);

    observation = Before();
    observation.world.epoch = kEpoch + 1;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::StaleEpoch);

    observation = Before();
    observation.world.botAlive = false;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason, CraftReason::Dead);

    observation = Before();
    observation.world.inCombat = true;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::InCombat);

    observation = Before();
    observation.world.cheatPathActive = true;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::CheatPath);

    observation = Before();
    observation.world.knownRecipe = false;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::KnownRecipeMissing);

    observation = Before();
    observation.world.professionKnown = false;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::ProfessionMissing);

    observation = Before();
    observation.world.currentSkill = 124;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::SkillInsufficient);
}

TEST(OracleCraftExecutorTest, RejectsCapacityAndReagentShortage)
{
    CraftObservation observation = Before();
    observation.inventory.freeOutputCapacity = 1;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::InventoryCapacity);

    observation = Before();
    observation.inventory.reagentCounts[0] = 2;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observation).reason,
              CraftReason::ReagentShortage);
}

TEST(OracleCraftExecutorTest, RejectsRequestAndObservedCandidateSubstitution)
{
    CraftCandidate substituted = Candidate();
    substituted.reference.recipeSpellId++;
    EXPECT_EQ(Validate(Request(), Lease(), substituted, Before()).reason,
              CraftReason::CandidateMismatch);

    CraftObservation observed = Before();
    observed.candidate.reference.outputItemId++;
    EXPECT_EQ(Validate(Request(), Lease(), Candidate(), observed).reason,
              CraftReason::CandidateMismatch);
}

TEST(OracleCraftExecutorTest, DispatchRejectsMissingNativeBoundary)
{
    DispatchResult const result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), nullptr);
    ASSERT_TRUE(result.valid);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Failed);
    EXPECT_EQ(result.reason, CraftReason::NativeUnavailable);
    EXPECT_FALSE(result.nativeCalled);
}

TEST(OracleCraftExecutorTest, RejectsNativeCastFailureWithoutAcceptingProgress)
{
    NativeProbe probe;
    probe.accept = false;
    DispatchResult const result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    ASSERT_TRUE(result.valid);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Failed);
    EXPECT_EQ(result.reason, CraftReason::NativeRejected);
    EXPECT_EQ(probe.calls, 1);
}

TEST(OracleCraftExecutorTest, DispatchesExactlyOneCastAndProvesExactDeltas)
{
    NativeProbe probe;
    DispatchResult const result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.status, ExecutorStatus::Completed);
    EXPECT_EQ(result.reason, CraftReason::None);
    EXPECT_EQ(result.phase, CraftStepPhase::Cast);
    EXPECT_TRUE(result.beforeValidated);
    EXPECT_TRUE(result.afterValidated);
    EXPECT_TRUE(result.nativeCalled);
    EXPECT_EQ(result.stepsInvoked, 1U);
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.phase, CraftStepPhase::Cast);
    EXPECT_EQ(probe.maxSteps, 1U);
    EXPECT_TRUE(probe.stopAfterStep);
    EXPECT_TRUE(ExactCandidateMatches(Candidate(), probe.candidate));
    EXPECT_EQ(result.before.inventory.reagentCounts[0] - result.after.inventory.reagentCounts[0],
              3U);
    EXPECT_EQ(result.before.inventory.reagentCounts[1] - result.after.inventory.reagentCounts[1],
              1U);
    EXPECT_EQ(result.after.inventory.outputCount - result.before.inventory.outputCount, 2U);
}

TEST(OracleCraftExecutorTest, RejectsNativeLoopsAndRecipeSubstitution)
{
    NativeProbe probe;
    probe.steps = 2;
    DispatchResult result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::StepBudgetExceeded);
    EXPECT_EQ(probe.calls, 1);

    probe = NativeProbe();
    probe.selectedAnotherRecipe = true;
    result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::SelectedAnotherRecipe);
    EXPECT_EQ(probe.calls, 1);
}

TEST(OracleCraftExecutorTest, RejectsOutputAndReagentDeltaForgery)
{
    NativeProbe probe;
    probe.after.inventory.outputCount++;
    DispatchResult result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::OutputDeltaMismatch);

    probe = NativeProbe();
    probe.after.inventory.reagentCounts[0]--;
    result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::ReagentDeltaMismatch);
}

TEST(OracleCraftExecutorTest, RevalidatesOwnershipAndSafetyAfterTheNativeStep)
{
    NativeProbe probe;
    probe.after.world.ownershipProof.decisionId++;
    DispatchResult result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::LeasePreempted);
    EXPECT_FALSE(result.afterValidated);

    probe = NativeProbe();
    probe.after.world.inCombat = true;
    result = OracleCraftExecutor::Dispatch(
        Request(), Lease(), Candidate(), Before(), &NativeStep, &probe);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, CraftReason::InCombat);
    EXPECT_FALSE(result.afterValidated);
}

} // namespace
