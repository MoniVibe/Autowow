#include "../src/AutoWow/OracleGatherRuntimePolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowOracle;
using namespace AutoWowGatherRuntimePolicy;

GatherSourceReference Source(GatherGoal goal = GatherGoal::HarvestNode,
                             NodeId spawnId = 9001)
{
    return {true, spawnId, 7001, 1, 500, 9, goal};
}

TEST(OracleGatherRuntimePolicyTest, PublishedSourceBlocksOrdinaryWorkerWhenRuntimeDisabledOrNotDue)
{
    WorkerControlObservation observation;
    observation.exactSourcePublished = true;
    observation.canonicalReceiptPending = true;
    observation.runtimeEnabled = false;
    observation.runtimeDue = false;

    WorkerControlDecision decision = EvaluateWorkerControl(observation);
    EXPECT_TRUE(decision.allowReadOnlyReceipt);
    EXPECT_TRUE(decision.suppressOrdinaryMovement);
    EXPECT_TRUE(decision.suppressWatchdogMutation);
    EXPECT_FALSE(decision.allowExactNativeExecutor);

    observation.runtimeEnabled = true;
    observation.runtimeDue = false;
    decision = EvaluateWorkerControl(observation);
    EXPECT_TRUE(decision.suppressOrdinaryMovement);
    EXPECT_FALSE(decision.allowExactNativeExecutor);

    observation.runtimeDue = true;
    observation.ownershipActive = true;
    observation.ownershipMatchesExactSource = true;
    decision = EvaluateWorkerControl(observation);
    EXPECT_TRUE(decision.suppressOrdinaryMovement);
    EXPECT_TRUE(decision.suppressWatchdogMutation);
    EXPECT_TRUE(decision.allowExactNativeExecutor);
}

TEST(OracleGatherRuntimePolicyTest, PublicationCancelsOrdinaryMovementBeforeOwnershipExists)
{
    ExactSourcePublicationDecision const publication = EvaluateExactSourcePublication(
        {true, true, true});
    EXPECT_TRUE(publication.cancelActiveMovement);
    EXPECT_TRUE(publication.invalidateLastMovement);
    EXPECT_TRUE(publication.invalidateExactMotionProvenance);

    bool const splineActive = !publication.cancelActiveMovement;
    bool const lastMovementValid = !publication.invalidateLastMovement;
    bool const provenanceValid = !publication.invalidateExactMotionProvenance;
    WorkerControlDecision const control = EvaluateWorkerControl(
        {true, false, false, false, false, false});
    EXPECT_FALSE(splineActive);
    EXPECT_FALSE(lastMovementValid);
    EXPECT_FALSE(provenanceValid);
    EXPECT_TRUE(control.suppressOrdinaryMovement);
    EXPECT_TRUE(control.suppressWatchdogMutation);
    EXPECT_FALSE(control.allowExactNativeExecutor);
}

TEST(OracleGatherRuntimePolicyTest, FirstOwnershipClearsInheritedMotionAndWaitsOnlyForExactProvenance)
{
    ExactMotionDecision inherited = EvaluateExactMotion({true, true, false});
    EXPECT_TRUE(inherited.clearInheritedMotion);
    EXPECT_FALSE(inherited.waitForCurrentExactMotion);
    EXPECT_TRUE(inherited.issueExactMotion);

    ExactMotionDecision exact = EvaluateExactMotion({true, true, true});
    EXPECT_FALSE(exact.clearInheritedMotion);
    EXPECT_TRUE(exact.waitForCurrentExactMotion);
    EXPECT_FALSE(exact.issueExactMotion);

    ExactMotionDecision unowned = EvaluateExactMotion({false, true, true});
    EXPECT_FALSE(unowned.clearInheritedMotion);
    EXPECT_FALSE(unowned.waitForCurrentExactMotion);
    EXPECT_FALSE(unowned.issueExactMotion);

    ExactMotionProvenance provenance{true, 101, Source()};
    EXPECT_TRUE(MatchesExactMotionProvenance(provenance, 101, Source()));
    EXPECT_FALSE(MatchesExactMotionProvenance(provenance, 102, Source()));
    EXPECT_FALSE(MatchesExactMotionProvenance(provenance, 101, Source(GatherGoal::HarvestNode, 9002)));
    EXPECT_FALSE(MatchesExactMotionProvenance(provenance, 101, Source(GatherGoal::ObtainMaterial)));
}

struct RenewalProbe
{
    bool refreshResult = false;
    std::uint32_t refreshCalls = 0;
    std::uint32_t releaseCalls = 0;
    DecisionId releasedDecisionId = 0;
};

bool RefreshGate(void* raw, IntentLease const&) noexcept
{
    RenewalProbe* probe = static_cast<RenewalProbe*>(raw);
    ++probe->refreshCalls;
    return probe->refreshResult;
}

void ReleaseRenewed(void* raw, IntentLease const& lease) noexcept
{
    RenewalProbe* probe = static_cast<RenewalProbe*>(raw);
    ++probe->releaseCalls;
    probe->releasedDecisionId = lease.decision.decisionId;
}

LeaseResult RenewedLease()
{
    LeaseResult renewed;
    renewed.hasLease = true;
    renewed.lease.valid = true;
    renewed.lease.decision.valid = true;
    renewed.lease.decision.decisionId = 404;
    renewed.lease.decision.operation = OperationCode::GatherSource;
    renewed.lease.decision.gather = Source(GatherGoal::ObtainMaterial);
    return renewed;
}

TEST(OracleGatherRuntimePolicyTest, GateRefreshFailureReleasesTheRenewedArbiterLease)
{
    RenewalProbe probe;
    RenewalCommitResult const result = CommitRenewedGatherLease(
        RenewedLease(), &probe, &RefreshGate, &ReleaseRenewed);

    EXPECT_FALSE(result.keepRenewedLease);
    EXPECT_TRUE(result.releasedRenewedLease);
    EXPECT_EQ(probe.refreshCalls, 1U);
    EXPECT_EQ(probe.releaseCalls, 1U);
    EXPECT_EQ(probe.releasedDecisionId, 404U);
}

TEST(OracleGatherRuntimePolicyTest, SuccessfulGateRefreshKeepsLeaseWithoutRelease)
{
    RenewalProbe probe;
    probe.refreshResult = true;
    RenewalCommitResult const result = CommitRenewedGatherLease(
        RenewedLease(), &probe, &RefreshGate, &ReleaseRenewed);

    EXPECT_TRUE(result.keepRenewedLease);
    EXPECT_FALSE(result.releasedRenewedLease);
    EXPECT_EQ(probe.refreshCalls, 1U);
    EXPECT_EQ(probe.releaseCalls, 0U);
}

TEST(OracleGatherRuntimePolicyTest, GatherGoalIsExplicitAndPartOfSourceIdentity)
{
    GatherSourceReference unknown = Source();
    unknown.goal = GatherGoal::Unknown;
    EXPECT_FALSE(ValidGatherSourceReference(unknown));
    EXPECT_TRUE(ValidGatherSourceReference(Source(GatherGoal::HarvestNode)));
    EXPECT_TRUE(ValidGatherSourceReference(Source(GatherGoal::ObtainMaterial)));
    EXPECT_FALSE(SameGatherSourceReference(
        Source(GatherGoal::HarvestNode), Source(GatherGoal::ObtainMaterial)));
}
}
