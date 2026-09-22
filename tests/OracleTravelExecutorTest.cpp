#include "../src/AutoWow/OracleTravelExecutor.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
using namespace AutoWowOracleTravel;

WalkDomain Domain()
{
    return {1, 7, 9};
}

SafeEndpoint Endpoint(Position position, bool safe = true)
{
    return {Domain(), position, safe};
}

WalkWorldFacts Facts(Position position, FactVersion version = 100)
{
    WalkWorldFacts facts;
    facts.valid = true;
    facts.botGuid = 42;
    facts.domain = Domain();
    facts.factVersion = version;
    facts.position = position;
    facts.alive = true;
    return facts;
}

WalkReference Reference(PathProbeResult result = PathProbeResult::Complete,
                        Position endpointPosition = {10.0, 0.0, 0.0})
{
    WalkReference reference;
    reference.botGuid = 42;
    reference.domain = Domain();
    reference.factVersion = 100;
    reference.endpoint = Endpoint(endpointPosition);
    reference.arrivalRadius = 1.0;
    reference.maxSegmentLength = 5.0;
    reference.probeDigest = 0xBADC0FFEEULL;
    reference.probeResult = result;
    return reference;
}

PathProbe Probe(WalkReference const& reference,
                Position nextPoint = {3.0, 0.0, 0.0},
                double segmentLength = 3.0)
{
    PathProbe probe;
    probe.botGuid = reference.botGuid;
    probe.domain = reference.domain;
    probe.factVersion = reference.factVersion;
    probe.destination = reference.endpoint;
    probe.nextSafePoint = nextPoint;
    probe.nextPointSafe = true;
    probe.segmentLength = segmentLength;
    probe.digest = reference.probeDigest;
    probe.result = reference.probeResult;
    return probe;
}

WalkExecutionInput Input(PathProbeResult result = PathProbeResult::Complete,
                          Position endpointPosition = {10.0, 0.0, 0.0})
{
    WalkExecutionInput input;
    input.now = 10;
    input.request.botGuid = 42;
    input.request.decisionId = 77;
    input.request.reference = Reference(result, endpointPosition);
    input.expectedReference = input.request.reference;
    input.lease.ownershipVerified = true;
    input.lease.botGuid = 42;
    input.lease.decisionId = 77;
    input.lease.resource = LeaseResource::Transition;
    input.lease.acquiredAt = 9;
    input.lease.expiresAt = 12;
    input.before = Facts({0.0, 0.0, 0.0});
    input.preProbe = Probe(input.request.reference);
    return input;
}

NativeMoveResult AcceptedResult(NativeMoveCommand const& command,
                                FactVersion afterVersion = 101,
                                Position afterPosition = {3.0, 0.0, 0.0})
{
    NativeMoveResult result;
    result.accepted = true;
    result.ownershipStillVerified = true;
    result.nativeStepCount = 1;
    result.appliedPoint = command.nextSafePoint;
    result.appliedSegmentLength = command.plannedSegmentLength;
    result.after = Facts(afterPosition, afterVersion);
    return result;
}

}  // namespace

TEST(OracleTravelExecutor, RejectsRequestReferenceMismatchBeforeNativeCallback)
{
    WalkExecutionInput input = Input();
    input.request.reference.endpoint.position.x = 11.0;
    int callbackCount = 0;
    WalkExecutorState state;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const&) {
            ++callbackCount;
            return NativeMoveResult{};
        });

    EXPECT_EQ(receipt.status, WalkReceiptStatus::Rejected);
    EXPECT_EQ(receipt.reason, WalkFailureReason::RequestReferenceMismatch);
    EXPECT_EQ(callbackCount, 0);
}

TEST(OracleTravelExecutor, RejectsMissingLease)
{
    WalkExecutionInput input = Input();
    input.lease.ownershipVerified = false;
    int callbackCount = 0;
    WalkExecutorState state;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const&) {
            ++callbackCount;
            return NativeMoveResult{};
        });

    EXPECT_EQ(receipt.reason, WalkFailureReason::LeaseMissing);
    EXPECT_EQ(callbackCount, 0);
}

TEST(OracleTravelExecutor, RejectsWrongLeaseResourceAndExpiredLease)
{
    WalkExecutorState state;
    WalkExecutionInput wrongResource = Input();
    wrongResource.lease.resource = LeaseResource::None;
    WalkReceipt wrongResourceReceipt = ExecuteWalkStep(wrongResource, state, {});
    EXPECT_EQ(wrongResourceReceipt.reason, WalkFailureReason::LeaseResourceMismatch);

    WalkExecutionInput expired = Input();
    expired.lease.expiresAt = expired.now;
    WalkReceipt expiredReceipt = ExecuteWalkStep(expired, state, {});
    EXPECT_EQ(expiredReceipt.reason, WalkFailureReason::LeaseExpired);

    WalkExecutionInput wrongOwner = Input();
    wrongOwner.lease.botGuid = 43;
    WalkReceipt wrongOwnerReceipt = ExecuteWalkStep(wrongOwner, state, {});
    EXPECT_EQ(wrongOwnerReceipt.reason, WalkFailureReason::LeaseOwnerMismatch);

    WalkExecutionInput wrongDecision = Input();
    wrongDecision.lease.decisionId = 78;
    WalkReceipt wrongDecisionReceipt = ExecuteWalkStep(wrongDecision, state, {});
    EXPECT_EQ(wrongDecisionReceipt.reason, WalkFailureReason::LeaseDecisionMismatch);
}

TEST(OracleTravelExecutor, RejectsStaleFactsAndProbeDigest)
{
    WalkExecutorState state;
    WalkExecutionInput staleFacts = Input();
    staleFacts.before.factVersion = 99;
    WalkReceipt staleFactsReceipt = ExecuteWalkStep(staleFacts, state, {});
    EXPECT_EQ(staleFactsReceipt.reason, WalkFailureReason::StaleFacts);

    WalkExecutionInput staleDigest = Input();
    staleDigest.preProbe.digest++;
    WalkReceipt staleDigestReceipt = ExecuteWalkStep(staleDigest, state, {});
    EXPECT_EQ(staleDigestReceipt.reason, WalkFailureReason::ProbeDigestMismatch);

    WalkExecutionInput wrongProbeResult = Input();
    wrongProbeResult.preProbe.result = PathProbeResult::Partial;
    WalkReceipt wrongProbeResultReceipt = ExecuteWalkStep(wrongProbeResult, state, {});
    EXPECT_EQ(wrongProbeResultReceipt.reason, WalkFailureReason::ProbeResultMismatch);
}

TEST(OracleTravelExecutor, RejectsCrossMapAndUnsafeEndpoint)
{
    WalkExecutorState state;
    WalkExecutionInput crossMap = Input();
    crossMap.request.reference.endpoint.domain.mapId = 2;
    crossMap.expectedReference = crossMap.request.reference;
    WalkReceipt crossMapReceipt = ExecuteWalkStep(crossMap, state, {});
    EXPECT_EQ(crossMapReceipt.reason, WalkFailureReason::CrossMap);

    WalkExecutionInput wrongPhase = Input();
    wrongPhase.request.reference.endpoint.domain.phase = 10;
    wrongPhase.expectedReference = wrongPhase.request.reference;
    wrongPhase.preProbe.destination = wrongPhase.request.reference.endpoint;
    WalkReceipt wrongPhaseReceipt = ExecuteWalkStep(wrongPhase, state, {});
    EXPECT_EQ(wrongPhaseReceipt.reason, WalkFailureReason::DomainMismatch);

    WalkExecutionInput unsafe = Input();
    unsafe.request.reference.endpoint.safe = false;
    unsafe.expectedReference = unsafe.request.reference;
    unsafe.preProbe.destination = unsafe.request.reference.endpoint;
    WalkReceipt unsafeReceipt = ExecuteWalkStep(unsafe, state, {});
    EXPECT_EQ(unsafeReceipt.reason, WalkFailureReason::UnsafeEndpoint);
}

TEST(OracleTravelExecutor, RejectsNoPathAndExcessiveSegment)
{
    WalkExecutorState state;
    WalkExecutionInput noPath = Input(PathProbeResult::NoPath);
    WalkReceipt noPathReceipt = ExecuteWalkStep(noPath, state, {});
    EXPECT_EQ(noPathReceipt.reason, WalkFailureReason::ProbeNoPath);

    WalkExecutionInput excessive = Input();
    excessive.preProbe.segmentLength = excessive.request.reference.maxSegmentLength + 1.0;
    WalkReceipt excessiveReceipt = ExecuteWalkStep(excessive, state, {});
    EXPECT_EQ(excessiveReceipt.reason, WalkFailureReason::InvalidBounds);
}

TEST(OracleTravelExecutor, RejectsCombatAndTransportStates)
{
    WalkExecutorState state;
    WalkExecutionInput combat = Input();
    combat.before.inCombat = true;
    WalkReceipt combatReceipt = ExecuteWalkStep(combat, state, {});
    EXPECT_EQ(combatReceipt.reason, WalkFailureReason::PostCombatOrTransport);

    WalkExecutionInput transport = Input();
    transport.before.onTransport = true;
    WalkReceipt transportReceipt = ExecuteWalkStep(transport, state, {});
    EXPECT_EQ(transportReceipt.reason, WalkFailureReason::PostCombatOrTransport);
}

TEST(OracleTravelExecutor, RejectsNativeOverBudget)
{
    WalkExecutionInput input = Input();
    int callbackCount = 0;
    WalkExecutorState state;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const& command) {
            ++callbackCount;
            NativeMoveResult result = AcceptedResult(command);
            result.nativeStepCount = 2;
            return result;
        });

    EXPECT_EQ(receipt.status, WalkReceiptStatus::Rejected);
    EXPECT_EQ(receipt.reason, WalkFailureReason::NativeOverBudget);
    EXPECT_EQ(receipt.nativeCallbackCount, 1u);
    EXPECT_EQ(callbackCount, 1);
}

TEST(OracleTravelExecutor, RejectsNativeSubstitution)
{
    WalkExecutionInput input = Input();
    WalkExecutorState state;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const& command) {
            NativeMoveResult result = AcceptedResult(command);
            result.substituted = true;
            return result;
        });

    EXPECT_EQ(receipt.status, WalkReceiptStatus::Rejected);
    EXPECT_EQ(receipt.reason, WalkFailureReason::NativeSubstitution);

    WalkExecutionInput ownershipLost = Input();
    WalkReceipt ownershipLostReceipt = ExecuteWalkStep(
        ownershipLost, state, [&](NativeMoveCommand const& command) {
            NativeMoveResult result = AcceptedResult(command);
            result.ownershipStillVerified = false;
            return result;
        });
    EXPECT_EQ(ownershipLostReceipt.reason, WalkFailureReason::NativeOwnershipLost);
}

TEST(OracleTravelExecutor, RejectsStaleOrUnsafePostState)
{
    WalkExecutorState state;
    WalkExecutionInput stale = Input();
    WalkReceipt staleReceipt = ExecuteWalkStep(
        stale, state, [&](NativeMoveCommand const& command) {
            return AcceptedResult(command, stale.before.factVersion - 1);
        });
    EXPECT_EQ(staleReceipt.reason, WalkFailureReason::PostFactsStale);

    WalkExecutionInput postCombat = Input();
    WalkReceipt postCombatReceipt = ExecuteWalkStep(
        postCombat, state, [&](NativeMoveCommand const& command) {
            NativeMoveResult result = AcceptedResult(command);
            result.after.inCombat = true;
            return result;
        });
    EXPECT_EQ(postCombatReceipt.reason, WalkFailureReason::PostCombatOrTransport);
}

TEST(OracleTravelExecutor, ExactCompleteProbeDispatchesOneNativeStepAndArrives)
{
    WalkExecutionInput input = Input(PathProbeResult::Complete, {3.0, 0.0, 0.0});
    input.preProbe = Probe(input.request.reference, {3.0, 0.0, 0.0}, 3.0);
    WalkExecutorState state;
    int callbackCount = 0;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const& command) {
            ++callbackCount;
            EXPECT_EQ(command.botGuid, input.request.botGuid);
            EXPECT_EQ(command.decisionId, input.request.decisionId);
            EXPECT_EQ(command.domain, input.request.reference.domain);
            EXPECT_EQ(command.destination, input.request.reference.endpoint);
            EXPECT_EQ(command.nextSafePoint, input.preProbe.nextSafePoint);
            EXPECT_EQ(command.probeDigest, input.request.reference.probeDigest);
            EXPECT_EQ(command.nativeStepBudget, 1u);
            return AcceptedResult(command, 101, {3.0, 0.0, 0.0});
        });

    EXPECT_EQ(receipt.status, WalkReceiptStatus::Arrived);
    EXPECT_EQ(receipt.reason, WalkFailureReason::None);
    EXPECT_TRUE(receipt.arrived);
    EXPECT_FALSE(receipt.partialPath);
    EXPECT_EQ(receipt.nativeCallbackCount, 1u);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(state.noProgressCount, 0u);
}

TEST(OracleTravelExecutor, PartialPathIsProgressButNeverArrival)
{
    WalkExecutionInput input = Input(PathProbeResult::Partial, {3.0, 0.0, 0.0});
    input.preProbe = Probe(input.request.reference, {3.0, 0.0, 0.0}, 3.0);
    WalkExecutorState state;

    WalkReceipt receipt = ExecuteWalkStep(
        input, state, [&](NativeMoveCommand const& command) {
            return AcceptedResult(command, 101, {3.0, 0.0, 0.0});
        });

    EXPECT_EQ(receipt.status, WalkReceiptStatus::Progressed);
    EXPECT_EQ(receipt.reason, WalkFailureReason::None);
    EXPECT_TRUE(receipt.partialPath);
    EXPECT_FALSE(receipt.arrived);
}

TEST(OracleTravelExecutor, BoundedNoProgressEmitsStuckThenStopsDispatching)
{
    WalkExecutionInput input = Input();
    input.before.position = {3.0, 0.0, 0.0};
    input.preProbe.nextSafePoint = input.before.position;
    input.preProbe.segmentLength = 0.5;
    WalkExecutorState state;
    int callbackCount = 0;

    auto noProgress = [&](NativeMoveCommand const& command) {
        ++callbackCount;
        NativeMoveResult result = AcceptedResult(command, 101, input.before.position);
        result.appliedSegmentLength = 0.5;
        return result;
    };

    WalkReceipt first = ExecuteWalkStep(input, state, noProgress);
    WalkReceipt second = ExecuteWalkStep(input, state, noProgress);
    WalkReceipt third = ExecuteWalkStep(input, state, noProgress);
    WalkReceipt fourth = ExecuteWalkStep(input, state, noProgress);

    EXPECT_EQ(first.status, WalkReceiptStatus::NoProgress);
    EXPECT_EQ(first.reason, WalkFailureReason::NoProgress);
    EXPECT_EQ(first.noProgressCount, 1u);
    EXPECT_EQ(second.status, WalkReceiptStatus::NoProgress);
    EXPECT_EQ(second.noProgressCount, 2u);
    EXPECT_EQ(third.status, WalkReceiptStatus::Stuck);
    EXPECT_EQ(third.reason, WalkFailureReason::NoProgressLimit);
    EXPECT_EQ(third.noProgressCount, 3u);
    EXPECT_EQ(fourth.status, WalkReceiptStatus::Stuck);
    EXPECT_EQ(fourth.reason, WalkFailureReason::AlreadyStuck);
    EXPECT_EQ(fourth.nativeCallbackCount, 0u);
    EXPECT_EQ(callbackCount, 3);
}
