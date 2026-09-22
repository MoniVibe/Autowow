#include "OracleTravelExecutor.h"

#include <cmath>

namespace AutoWowOracleTravel
{
namespace
{
bool FiniteCoordinate(double value)
{
    return std::isfinite(value) && std::abs(value) <= kMaximumCoordinateMagnitude;
}

bool ValidPosition(Position const& position)
{
    return FiniteCoordinate(position.x) && FiniteCoordinate(position.y) &&
        FiniteCoordinate(position.z);
}

bool ValidDomain(WalkDomain const& domain)
{
    // Map 0 and phase 0 are valid AzerothCore world identifiers; only the composite equality is
    // authoritative here.  This function exists to keep validation explicit if the domain grows.
    (void)domain;
    return true;
}

bool SameMapAndInstance(WalkDomain const& left, WalkDomain const& right)
{
    return left.mapId == right.mapId && left.instanceId == right.instanceId;
}

double Distance(Position const& left, Position const& right)
{
    double const dx = left.x - right.x;
    double const dy = left.y - right.y;
    double const dz = left.z - right.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

WalkReceipt Rejected(WalkExecutionInput const& input, WalkFailureReason reason)
{
    WalkReceipt receipt;
    receipt.status = WalkReceiptStatus::Rejected;
    receipt.reason = reason;
    receipt.botGuid = input.request.botGuid;
    receipt.decisionId = input.request.decisionId;
    receipt.probeResult = input.preProbe.result;
    receipt.probeDigest = input.preProbe.digest;
    receipt.beforeFactVersion = input.before.factVersion;
    return receipt;
}

WalkFailureReason ValidateReference(WalkReference const& reference)
{
    if (reference.botGuid == 0 || reference.factVersion == 0 || reference.probeDigest == 0 ||
        !ValidDomain(reference.domain))
    {
        return WalkFailureReason::InvalidRequest;
    }

    if (!reference.endpoint.safe || !ValidDomain(reference.endpoint.domain) ||
        !ValidPosition(reference.endpoint.position))
    {
        return WalkFailureReason::UnsafeEndpoint;
    }

    if (!SameMapAndInstance(reference.endpoint.domain, reference.domain))
        return WalkFailureReason::CrossMap;
    if (reference.endpoint.domain.phase != reference.domain.phase)
        return WalkFailureReason::DomainMismatch;

    if (!std::isfinite(reference.arrivalRadius) || reference.arrivalRadius <= 0.0 ||
        reference.arrivalRadius > kMaximumArrivalRadius ||
        !std::isfinite(reference.maxSegmentLength) || reference.maxSegmentLength <= 0.0 ||
        reference.maxSegmentLength > kMaximumSegmentLength ||
        reference.arrivalRadius > reference.maxSegmentLength)
    {
        return WalkFailureReason::InvalidBounds;
    }

    return WalkFailureReason::None;
}

WalkFailureReason ValidateBeforeFacts(WalkExecutionInput const& input)
{
    WalkReference const& reference = input.request.reference;
    WalkWorldFacts const& before = input.before;

    if (!before.valid || before.factVersion != reference.factVersion)
        return WalkFailureReason::StaleFacts;
    if (before.botGuid != reference.botGuid || input.request.botGuid != reference.botGuid)
        return WalkFailureReason::BotMismatch;
    if (before.domain != reference.domain)
    {
        if (!SameMapAndInstance(before.domain, reference.domain))
            return WalkFailureReason::CrossMap;
        return WalkFailureReason::DomainMismatch;
    }
    if (!ValidPosition(before.position) || !before.alive)
        return WalkFailureReason::StaleFacts;
    if (before.inCombat || before.onTransport)
        return WalkFailureReason::PostCombatOrTransport;
    return WalkFailureReason::None;
}

WalkFailureReason ValidateProbe(WalkExecutionInput const& input)
{
    WalkReference const& reference = input.request.reference;
    PathProbe const& probe = input.preProbe;

    if (probe.result == PathProbeResult::NoPath)
        return WalkFailureReason::ProbeNoPath;
    if (probe.botGuid != reference.botGuid || probe.domain != reference.domain ||
        probe.destination != reference.endpoint)
    {
        if (!SameMapAndInstance(probe.domain, reference.domain))
            return WalkFailureReason::CrossMap;
        return WalkFailureReason::ProbeReferenceMismatch;
    }
    if (probe.factVersion != reference.factVersion)
        return WalkFailureReason::ProbeFactsStale;
    if (probe.digest != reference.probeDigest)
        return WalkFailureReason::ProbeDigestMismatch;
    if (probe.result != reference.probeResult)
        return WalkFailureReason::ProbeResultMismatch;
    if (!probe.nextPointSafe || !ValidPosition(probe.nextSafePoint) ||
        !std::isfinite(probe.segmentLength) || probe.segmentLength <= 0.0 ||
        probe.segmentLength > reference.maxSegmentLength ||
        probe.segmentLength > kMaximumSegmentLength ||
        Distance(probe.nextSafePoint, input.before.position) > reference.maxSegmentLength +
            kProgressEpsilon)
    {
        return WalkFailureReason::InvalidBounds;
    }
    return WalkFailureReason::None;
}

WalkFailureReason ValidateLease(WalkExecutionInput const& input)
{
    LeaseProof const& lease = input.lease;
    if (!lease.ownershipVerified)
        return WalkFailureReason::LeaseMissing;
    if (lease.resource != LeaseResource::Transition)
        return WalkFailureReason::LeaseResourceMismatch;
    if (lease.botGuid != input.request.botGuid)
        return WalkFailureReason::LeaseOwnerMismatch;
    if (lease.decisionId != input.request.decisionId)
        return WalkFailureReason::LeaseDecisionMismatch;
    if (lease.acquiredAt > input.now || lease.expiresAt <= input.now)
        return WalkFailureReason::LeaseExpired;
    return WalkFailureReason::None;
}

bool NativeResultWithinBudget(NativeMoveResult const& result,
                              NativeMoveCommand const& command,
                              WalkWorldFacts const& before)
{
    if (!result.accepted || result.nativeStepCount != command.nativeStepBudget || result.overBudget)
        return false;
    if (!std::isfinite(result.appliedSegmentLength) || result.appliedSegmentLength <= 0.0 ||
        result.appliedSegmentLength > command.maxSegmentLength)
    {
        return false;
    }
    return Distance(before.position, result.after.position) <=
        command.maxSegmentLength + kProgressEpsilon;
}

}  // namespace

bool operator==(Position const& left, Position const& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool operator==(WalkDomain const& left, WalkDomain const& right)
{
    return left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.phase == right.phase;
}

bool operator==(SafeEndpoint const& left, SafeEndpoint const& right)
{
    return left.domain == right.domain && left.position == right.position && left.safe == right.safe;
}

bool operator==(WalkReference const& left, WalkReference const& right)
{
    return left.botGuid == right.botGuid && left.domain == right.domain &&
        left.factVersion == right.factVersion && left.endpoint == right.endpoint &&
        left.arrivalRadius == right.arrivalRadius &&
        left.maxSegmentLength == right.maxSegmentLength &&
        left.probeDigest == right.probeDigest && left.probeResult == right.probeResult;
}

std::string_view WalkFailureReasonName(WalkFailureReason reason)
{
    switch (reason)
    {
        case WalkFailureReason::None: return "none";
        case WalkFailureReason::AlreadyStuck: return "already_stuck";
        case WalkFailureReason::InvalidRequest: return "invalid_request";
        case WalkFailureReason::RequestReferenceMismatch: return "request_reference_mismatch";
        case WalkFailureReason::LeaseMissing: return "lease_missing";
        case WalkFailureReason::LeaseExpired: return "lease_expired";
        case WalkFailureReason::LeaseResourceMismatch: return "lease_resource_mismatch";
        case WalkFailureReason::LeaseOwnerMismatch: return "lease_owner_mismatch";
        case WalkFailureReason::LeaseDecisionMismatch: return "lease_decision_mismatch";
        case WalkFailureReason::StaleFacts: return "stale_facts";
        case WalkFailureReason::BotMismatch: return "bot_mismatch";
        case WalkFailureReason::DomainMismatch: return "domain_mismatch";
        case WalkFailureReason::CrossMap: return "cross_map";
        case WalkFailureReason::UnsafeEndpoint: return "unsafe_endpoint";
        case WalkFailureReason::InvalidBounds: return "invalid_bounds";
        case WalkFailureReason::ProbeNoPath: return "probe_no_path";
        case WalkFailureReason::ProbeReferenceMismatch: return "probe_reference_mismatch";
        case WalkFailureReason::ProbeDigestMismatch: return "probe_digest_mismatch";
        case WalkFailureReason::ProbeFactsStale: return "probe_facts_stale";
        case WalkFailureReason::ProbeResultMismatch: return "probe_result_mismatch";
        case WalkFailureReason::NativeRejected: return "native_rejected";
        case WalkFailureReason::NativeOwnershipLost: return "native_ownership_lost";
        case WalkFailureReason::NativeOverBudget: return "native_over_budget";
        case WalkFailureReason::NativeSubstitution: return "native_substitution";
        case WalkFailureReason::PostFactsStale: return "post_facts_stale";
        case WalkFailureReason::PostStateInvalid: return "post_state_invalid";
        case WalkFailureReason::PostDomainMismatch: return "post_domain_mismatch";
        case WalkFailureReason::PostCombatOrTransport: return "post_combat_or_transport";
        case WalkFailureReason::NoProgress: return "no_progress";
        case WalkFailureReason::NoProgressLimit: return "no_progress_limit";
    }
    return "invalid";
}

std::string_view WalkReceiptStatusName(WalkReceiptStatus status)
{
    switch (status)
    {
        case WalkReceiptStatus::Rejected: return "rejected";
        case WalkReceiptStatus::Progressed: return "progressed";
        case WalkReceiptStatus::Arrived: return "arrived";
        case WalkReceiptStatus::NoProgress: return "no_progress";
        case WalkReceiptStatus::Stuck: return "stuck";
    }
    return "invalid";
}

std::string_view PathProbeResultName(PathProbeResult result)
{
    switch (result)
    {
        case PathProbeResult::NoPath: return "no_path";
        case PathProbeResult::Partial: return "partial";
        case PathProbeResult::Complete: return "complete";
    }
    return "invalid";
}

WalkReceipt ExecuteWalkStep(WalkExecutionInput const& input,
                            WalkExecutorState& state,
                            NativeMoveCallback const& nativeMove)
{
    if (state.stuck)
    {
        WalkReceipt receipt = Rejected(input, WalkFailureReason::AlreadyStuck);
        receipt.status = WalkReceiptStatus::Stuck;
        receipt.noProgressCount = state.noProgressCount;
        return receipt;
    }

    if (input.request.reference != input.expectedReference)
        return Rejected(input, WalkFailureReason::RequestReferenceMismatch);

    WalkFailureReason reason = ValidateReference(input.request.reference);
    if (reason != WalkFailureReason::None)
        return Rejected(input, reason);

    if (input.request.botGuid != input.request.reference.botGuid)
        return Rejected(input, WalkFailureReason::BotMismatch);
    if (input.request.decisionId == 0)
        return Rejected(input, WalkFailureReason::InvalidRequest);
    if (state.noProgressLimit == 0)
        return Rejected(input, WalkFailureReason::InvalidBounds);

    reason = ValidateLease(input);
    if (reason != WalkFailureReason::None)
        return Rejected(input, reason);

    reason = ValidateBeforeFacts(input);
    if (reason != WalkFailureReason::None)
        return Rejected(input, reason);

    reason = ValidateProbe(input);
    if (reason != WalkFailureReason::None)
        return Rejected(input, reason);

    if (!nativeMove)
        return Rejected(input, WalkFailureReason::NativeRejected);

    NativeMoveCommand command;
    command.botGuid = input.request.botGuid;
    command.decisionId = input.request.decisionId;
    command.domain = input.request.reference.domain;
    command.destination = input.request.reference.endpoint;
    command.nextSafePoint = input.preProbe.nextSafePoint;
    command.plannedSegmentLength = input.preProbe.segmentLength;
    command.maxSegmentLength = input.request.reference.maxSegmentLength;
    command.probeDigest = input.request.reference.probeDigest;

    NativeMoveResult const native = nativeMove(command);
    WalkReceipt receipt;
    receipt.botGuid = input.request.botGuid;
    receipt.decisionId = input.request.decisionId;
    receipt.probeResult = input.preProbe.result;
    receipt.probeDigest = input.preProbe.digest;
    receipt.beforeFactVersion = input.before.factVersion;
    receipt.nativeCallbackCount = 1;
    receipt.partialPath = input.preProbe.result == PathProbeResult::Partial;

    if (!native.accepted)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::NativeRejected;
        return receipt;
    }
    if (!native.ownershipStillVerified)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::NativeOwnershipLost;
        return receipt;
    }
    if (!NativeResultWithinBudget(native, command, input.before))
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::NativeOverBudget;
        return receipt;
    }
    if (native.substituted || native.appliedPoint != command.nextSafePoint ||
        native.after.position != native.appliedPoint)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::NativeSubstitution;
        return receipt;
    }

    receipt.afterFactVersion = native.after.factVersion;
    if (!native.after.valid || !ValidPosition(native.after.position) || !native.after.alive)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::PostStateInvalid;
        return receipt;
    }
    if (native.after.botGuid != input.request.reference.botGuid)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::BotMismatch;
        return receipt;
    }
    if (native.after.domain != input.request.reference.domain)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::PostDomainMismatch;
        return receipt;
    }
    if (native.after.factVersion < input.before.factVersion)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::PostFactsStale;
        return receipt;
    }
    if (native.after.inCombat || native.after.onTransport)
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::PostCombatOrTransport;
        return receipt;
    }

    receipt.beforeDistance = Distance(input.before.position, input.request.reference.endpoint.position);
    receipt.afterDistance = Distance(native.after.position, input.request.reference.endpoint.position);
    if (!std::isfinite(receipt.beforeDistance) || !std::isfinite(receipt.afterDistance))
    {
        receipt.status = WalkReceiptStatus::Rejected;
        receipt.reason = WalkFailureReason::PostStateInvalid;
        return receipt;
    }

    bool const progressed = receipt.afterDistance + kProgressEpsilon < receipt.beforeDistance;
    if (!progressed)
    {
        if (state.noProgressCount < state.noProgressLimit)
            ++state.noProgressCount;
        receipt.noProgressCount = state.noProgressCount;
        if (state.noProgressCount >= state.noProgressLimit)
        {
            state.stuck = true;
            receipt.status = WalkReceiptStatus::Stuck;
            receipt.reason = WalkFailureReason::NoProgressLimit;
        }
        else
        {
            receipt.status = WalkReceiptStatus::NoProgress;
            receipt.reason = WalkFailureReason::NoProgress;
        }
        return receipt;
    }

    state.noProgressCount = 0;
    receipt.noProgressCount = 0;
    if (input.preProbe.result == PathProbeResult::Complete &&
        receipt.afterDistance <= input.request.reference.arrivalRadius)
    {
        receipt.status = WalkReceiptStatus::Arrived;
        receipt.reason = WalkFailureReason::None;
        receipt.arrived = true;
        return receipt;
    }

    receipt.status = WalkReceiptStatus::Progressed;
    receipt.reason = WalkFailureReason::None;
    return receipt;
}
}  // namespace AutoWowOracleTravel
