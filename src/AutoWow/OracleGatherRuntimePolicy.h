/*
 * Pure runtime/worker policy for an externally published exact gathering source.
 *
 * This header owns no world objects and performs no movement. Production code and focused tests
 * use the same decisions for publication gating, exact-motion provenance, and renewed-lease
 * cleanup.
 */
#ifndef MOD_PLAYERBOTS_ORACLE_GATHER_RUNTIME_POLICY_H
#define MOD_PLAYERBOTS_ORACLE_GATHER_RUNTIME_POLICY_H

#include "AutoWowOracleContract.h"

namespace AutoWowGatherRuntimePolicy
{
struct ExactSourcePublicationObservation
{
    bool ordinaryMovementActive = false;
    bool lastMovementValid = false;
    bool exactMotionProvenanceValid = false;
};

struct ExactSourcePublicationDecision
{
    bool cancelActiveMovement = false;
    bool invalidateLastMovement = false;
    bool invalidateExactMotionProvenance = false;
};

inline constexpr ExactSourcePublicationDecision EvaluateExactSourcePublication(
    ExactSourcePublicationObservation const& observation) noexcept
{
    // Publication is a hard ownership boundary. Clear every movement surface even when one
    // appears idle so stale state cannot resume while the runtime is disabled or not due.
    (void)observation;
    return {true, true, true};
}

struct WorkerControlObservation
{
    bool exactSourcePublished = false;
    bool canonicalReceiptPending = false;
    bool runtimeEnabled = false;
    bool runtimeDue = false;
    bool ownershipActive = false;
    bool ownershipMatchesExactSource = false;
};

struct WorkerControlDecision
{
    bool allowReadOnlyReceipt = false;
    bool suppressOrdinaryMovement = false;
    bool suppressWatchdogMutation = false;
    bool allowExactNativeExecutor = false;
};

inline constexpr WorkerControlDecision EvaluateWorkerControl(
    WorkerControlObservation const& observation) noexcept
{
    WorkerControlDecision decision;
    decision.allowReadOnlyReceipt = observation.exactSourcePublished &&
        observation.canonicalReceiptPending;

    // Publication itself is the fail-closed boundary. Runtime disabled/not-due cannot hand the
    // published candidate back to ordinary movement or watchdog expiry.
    decision.suppressOrdinaryMovement = observation.exactSourcePublished || observation.ownershipActive;
    decision.suppressWatchdogMutation = decision.suppressOrdinaryMovement;

    // Only the fixed-target native path may move, and only under ownership bound to this source.
    decision.allowExactNativeExecutor = observation.exactSourcePublished && observation.ownershipActive &&
        observation.ownershipMatchesExactSource;
    return decision;
}

struct ExactMotionProvenance
{
    bool valid = false;
    AutoWowOracle::DecisionId decisionId = 0;
    AutoWowOracle::GatherSourceReference source;
};

inline constexpr bool MatchesExactMotionProvenance(
    ExactMotionProvenance const& provenance, AutoWowOracle::DecisionId decisionId,
    AutoWowOracle::GatherSourceReference const& source) noexcept
{
    return provenance.valid && decisionId != 0 && provenance.decisionId == decisionId &&
        AutoWowOracle::SameGatherSourceReference(provenance.source, source);
}

struct ExactMotionObservation
{
    bool activeMatchingOwnership = false;
    bool motionInProgress = false;
    bool currentMotionHasExactSourceProvenance = false;
};

struct ExactMotionDecision
{
    bool clearInheritedMotion = false;
    bool waitForCurrentExactMotion = false;
    bool issueExactMotion = false;
};

inline constexpr ExactMotionDecision EvaluateExactMotion(
    ExactMotionObservation const& observation) noexcept
{
    if (!observation.activeMatchingOwnership)
        return {};

    if (observation.motionInProgress && observation.currentMotionHasExactSourceProvenance)
        return {false, true, false};

    // The first matching ownership for this decision/source must evict any inherited movement.
    // The caller may then issue the already-probed exact route in the same bounded native step.
    if (observation.motionInProgress)
        return {true, false, true};

    return {false, false, true};
}

using GateRefreshFunction = bool (*)(void*, AutoWowOracle::IntentLease const&) noexcept;
using RenewedLeaseReleaseFunction = void (*)(void*, AutoWowOracle::IntentLease const&) noexcept;

struct RenewalCommitResult
{
    bool keepRenewedLease = false;
    bool releasedRenewedLease = false;
};

inline RenewalCommitResult CommitRenewedGatherLease(
    AutoWowOracle::LeaseResult const& renewed, void* context,
    GateRefreshFunction refreshGate, RenewedLeaseReleaseFunction releaseRenewedLease) noexcept
{
    if (!renewed.hasLease || !renewed.lease.valid)
        return {};

    if (refreshGate && refreshGate(context, renewed.lease))
        return {true, false};

    // A successful arbiter renewal cannot be orphaned when the native gate refresh fails.
    if (releaseRenewedLease)
    {
        releaseRenewedLease(context, renewed.lease);
        return {false, true};
    }
    return {};
}
}

#endif  // MOD_PLAYERBOTS_ORACLE_GATHER_RUNTIME_POLICY_H
