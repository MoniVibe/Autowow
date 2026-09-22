#include "OracleRouteExecutor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AutoWowOracleRoute
{
namespace
{
inline constexpr std::uint8_t kInvalidLedgerIndex = kMaxSessionLedgerEntries;
inline constexpr std::uint8_t kReentryBlockCount = 3;

bool Finite(double value) noexcept
{
    return std::isfinite(value);
}

bool FiniteCoordinate(RouteCoordinate const& coordinate) noexcept
{
    return Finite(coordinate.x) && Finite(coordinate.y) && Finite(coordinate.z) &&
        Finite(coordinate.o);
}

Tick SaturatingAdd(Tick left, Tick right) noexcept
{
    Tick const maximum = std::numeric_limits<Tick>::max();
    if (right > maximum - left)
        return maximum;
    return left + right;
}

std::uint8_t FindLedgerIndex(SessionLedger const& ledger,
                             RouteLedgerKey const& ledgerKey) noexcept
{
    for (std::uint8_t index = 0;
         index < ledger.entryCount && index < kMaxSessionLedgerEntries; ++index)
    {
        if (ledger.entries[index].occupied && ledger.entries[index].ledgerKey == ledgerKey)
            return index;
    }
    return kInvalidLedgerIndex;
}

bool SameRouteIdentity(RouteLedgerKey const& left, RouteLedgerKey const& right) noexcept
{
    RouteLedgerKey leftBase = left;
    RouteLedgerKey rightBase = right;
    leftBase.failureClass = RouteFailureClass::None;
    rightBase.failureClass = RouteFailureClass::None;
    return leftBase == rightBase;
}

RouteResult MakeResult(RouteSession const& session, RouteCommandKind commandKind,
                       RouteFailure failure, bool progressObserved = false) noexcept
{
    RouteState const& state = session.state;
    RouteResult result;
    result.routeKey = state.routeKey;
    result.ledgerKey = state.ledgerKey;
    result.stage = state.stage;
    result.failure = failure;
    result.command.kind = commandKind;
    result.command.routeKey = state.routeKey;
    result.anchorAttempts = state.anchorAttemptCount;
    result.attemptedAnchors = state.anchorAttempts;
    result.distance.available = state.hasDistance;
    result.distance.initial = state.initialRemainingDistance;
    result.distance.remaining = state.lastRemainingDistance;
    result.distance.reduced = state.hasDistance &&
            state.initialRemainingDistance > state.lastRemainingDistance
        ? state.initialRemainingDistance - state.lastRemainingDistance
        : 0.0;
    result.segmentIndexAvailable = state.hasSegmentIndex;
    result.segmentIndex = state.lastSegmentIndex;
    result.releaseLease = state.releaseLease;
    result.leaseReleaseAuthoritative = state.leaseReleaseAuthoritative;
    result.progressObserved = progressObserved;
    return result;
}

RouteCommand MakeCommand(RouteState const& state, RouteCommandKind kind) noexcept
{
    RouteCommand command;
    command.kind = kind;
    command.routeKey = state.routeKey;

    if (kind == RouteCommandKind::GroundPlan || kind == RouteCommandKind::Move ||
        kind == RouteCommandKind::Approach || kind == RouteCommandKind::Repath)
    {
        if (state.selectedAnchor.callerSupplied && state.selectedAnchor.safe)
        {
            command.hasAnchor = true;
            command.anchor = state.selectedAnchor;
        }
    }

    if (kind == RouteCommandKind::Move || kind == RouteCommandKind::Approach)
    {
        if (state.groundPlan.callerSupplied && state.groundPlan.valid &&
            state.groundPlan.nextPointSupplied)
        {
            command.hasPlan = true;
            command.plan = state.groundPlan;
        }
    }

    return command;
}

RouteResult MakeCommandResult(RouteSession const& session, RouteCommandKind commandKind,
                              RouteFailure failure, bool progressObserved = false) noexcept
{
    RouteResult result = MakeResult(session, commandKind, failure, progressObserved);
    result.command = MakeCommand(session.state, commandKind);
    return result;
}

RouteStep YieldWithoutLedgerWrite(RouteSession session, RouteFailure failure,
                                  Tick nextRetryAt = 0) noexcept
{
    session.state.stage = RouteStage::Blocked;
    session.state.failure = failure;
    session.state.releaseLease = true;
    RouteResult result = MakeCommandResult(session, RouteCommandKind::Yield, failure);
    result.blocked = true;
    result.releaseLease = true;
    result.nextRetryAt = nextRetryAt;
    return RouteStep{session, result};
}

RouteStep Block(RouteSession session, RouteFailure failure, Tick now) noexcept
{
    session.state.stage = RouteStage::Blocked;
    session.state.failure = failure;
    session.state.releaseLease = true;

    LedgerUpdate const update = RecordBlocked(
        session.ledger, session.state.ledgerKey, now, failure,
        session.state.hasWorldChangeId ? session.state.lastWorldChangeId : 0);
    session.ledger = update.ledger;

    if (!update.accepted)
        session.state.failure = RouteFailure::SessionLedgerFull;

    RouteResult result = MakeCommandResult(session, RouteCommandKind::Yield,
                                           session.state.failure);
    result.blocked = true;
    result.releaseLease = true;
    result.leaseReleaseAuthoritative = false;
    result.nextRetryAt = update.nextRetryAt;
    result.ledgerKey = update.ledgerKey;
    return RouteStep{session, result};
}

RouteStep Pending(RouteSession session, RouteCommandKind commandKind,
                  RouteFailure failure, bool progressObserved = false) noexcept
{
    RouteResult result = MakeCommandResult(session, commandKind, failure, progressObserved);
    result.nextRetryAt = 0;
    return RouteStep{session, result};
}

bool HasIdentityDrift(RouteState const& state, RouteObservation const& observation) noexcept
{
    return observation.hasLiveIdentity && observation.liveIdentity != state.routeKey;
}

bool ObserveProgress(RouteState& state, RouteObservation const& observation, Tick now) noexcept
{
    bool progressed = false;

    if (observation.hasSegmentIndex)
    {
        if (state.hasSegmentIndex && observation.segmentIndex > state.lastSegmentIndex)
            progressed = true;
        state.hasSegmentIndex = true;
        state.lastSegmentIndex = observation.segmentIndex;
    }

    if (observation.hasRemainingDistance && Finite(observation.remainingDistance))
    {
        if (state.hasDistance &&
            state.lastRemainingDistance - observation.remainingDistance >=
                kProgressDistanceYards)
        {
            progressed = true;
        }
        if (!state.hasDistance)
        {
            state.initialRemainingDistance = observation.remainingDistance;
            state.hasDistance = true;
        }
        state.lastRemainingDistance = observation.remainingDistance;
    }

    if (observation.hasGridState)
    {
        if (state.hasLastGridState && !state.lastGridLoaded && observation.gridLoaded)
            progressed = true;
        state.hasLastGridState = true;
        state.lastGridLoaded = observation.gridLoaded;
    }

    if (observation.hasMapArea)
    {
        if (state.hasLastMapArea &&
            (state.lastMapId != observation.currentMapId ||
             state.lastAreaId != observation.currentAreaId))
        {
            progressed = true;
        }
        state.hasLastMapArea = true;
        state.lastMapId = observation.currentMapId;
        state.lastAreaId = observation.currentAreaId;
    }

    if (observation.objectiveDelta || observation.questDelta || observation.inventoryDelta ||
        observation.interactionDelta)
    {
        progressed = true;
    }

    if (observation.hasWorldChangeId && observation.worldChangeId != 0)
    {
        state.hasWorldChangeId = true;
        state.lastWorldChangeId = observation.worldChangeId;
    }

    if (progressed)
    {
        state.lastProgressAt = now;
        ++state.progressEvents;
    }
    return progressed;
}

struct RecoveryDecision
{
    bool block = false;
    bool commandReady = false;
    RouteCommandKind command = RouteCommandKind::None;
    RouteFailure failure = RouteFailure::None;
};

RecoveryDecision Recover(RouteState& state, Tick now) noexcept
{
    if (now >= state.hardDeadlineAt)
        return RecoveryDecision{true, false, RouteCommandKind::Yield,
                                RouteFailure::DeadlineExceeded};

    Tick const stalledFor = now >= state.lastProgressAt ? now - state.lastProgressAt : 0;
    if (stalledFor >= kBlockAfterSeconds)
        return RecoveryDecision{true, false, RouteCommandKind::Yield, RouteFailure::Stalled};

    if (stalledFor >= kReresolveAfterSeconds && !state.reresolveUsed)
    {
        state.reresolveUsed = true;
        state.stage = RouteStage::ExactLiveBind;
        return RecoveryDecision{false, true, RouteCommandKind::Reresolve,
                                RouteFailure::None};
    }

    if (stalledFor >= kRepathAfterSeconds && !state.repathUsed)
    {
        state.repathUsed = true;
        state.stage = RouteStage::GroundPlan;
        return RecoveryDecision{false, true, RouteCommandKind::Repath,
                                RouteFailure::None};
    }

    return RecoveryDecision{};
}

RouteFailure ValidateAnchor(RouteKey const& routeKey, SafeAnchor const& anchor) noexcept
{
    if (!anchor.callerSupplied || !anchor.safe || anchor.anchorId == 0 ||
        anchor.mapId != routeKey.mapId || anchor.instanceId != routeKey.instanceId ||
        !FiniteCoordinate(anchor.coordinate))
    {
        return RouteFailure::UnsafeAnchor;
    }
    return RouteFailure::None;
}

RouteFailure ValidatePlan(RouteState const& state, GroundPlan const& plan) noexcept
{
    if (!plan.callerSupplied || !plan.valid || !plan.nextPointSupplied ||
        plan.routeKey != state.routeKey)
    {
        return plan.routeKey == state.routeKey ? RouteFailure::InvalidGroundPlan
                                               : RouteFailure::GroundPlanIdentityMismatch;
    }
    if (plan.anchorId == 0 || plan.anchorId != state.selectedAnchor.anchorId ||
        !FiniteCoordinate(plan.nextPoint) || !Finite(plan.estimatedRemainingDistance) ||
        plan.estimatedRemainingDistance < 0.0)
    {
        return RouteFailure::InvalidGroundPlan;
    }
    return RouteFailure::None;
}

bool AnchorWasAttempted(RouteState const& state, AnchorId anchorId) noexcept
{
    for (std::uint8_t index = 0;
         index < state.anchorAttemptCount && index < kMaxSuppliedAnchors; ++index)
    {
        if (state.anchorAttempts[index] == anchorId)
            return true;
    }
    return false;
}

RouteStep SelectNextAnchorAfterGridProbe(RouteSession session, Tick now,
                                         bool progressObserved) noexcept
{
    if (session.state.anchorAttemptCount >= kMaxSuppliedAnchors)
        return Block(session, RouteFailure::TargetNotLoadedAfterGridProbe, now);

    // Keep anchorAttempts and anchorAttemptCount as the exact history.  Clear only the transient
    // selection and plan so the next command cannot reuse coordinates from the prior attempt.
    session.state.selectedAnchor = SafeAnchor{};
    session.state.groundPlan = GroundPlan{};
    session.state.exactLiveBound = false;
    session.state.stage = RouteStage::SelectSafeAnchor;
    return Pending(session, RouteCommandKind::SelectSafeAnchor, RouteFailure::None,
                   progressObserved);
}

RouteStep FinishArrived(RouteSession session) noexcept
{
    session.state.stage = RouteStage::Arrived;
    session.state.failure = RouteFailure::None;
    // Arrival hands the exact identity to native interaction. Strategic/tactical ownership is
    // still required until objective credit/reward or a terminal block is observed by Runtime.
    session.state.releaseLease = false;
    session.state.leaseReleaseAuthoritative = false;
    RouteResult result = MakeCommandResult(session, RouteCommandKind::None, RouteFailure::None);
    result.arrived = true;
    result.releaseLease = false;
    result.leaseReleaseAuthoritative = false;
    return RouteStep{session, result};
}

}  // namespace

RouteKey MakeRouteKey(RouteIntent const& intent) noexcept
{
    RouteKey key;
    key.actor = intent.actor;
    key.purpose = intent.purpose;
    key.mapId = intent.mapId;
    key.instanceId = intent.instanceId;
    key.targetKind = intent.targetKind;
    key.entry = intent.entry;
    key.stableSpawn = intent.stableSpawn;
    key.dbX = intent.dbX;
    key.dbY = intent.dbY;
    key.dbZ = intent.dbZ;
    key.dbO = intent.dbO;
    key.radius = intent.radius;
    key.controller = intent.controller;
    key.job = intent.job;
    key.lease = intent.lease;
    key.segment = intent.segment;
    key.questId = intent.questId;
    key.objectiveFamily = intent.objectiveFamily;
    key.objectiveSlot = intent.objectiveSlot;
    key.routeCatalogVersion = intent.routeCatalogVersion;
    return key;
}

RouteLedgerKey MakeRouteLedgerKey(RouteKey const& routeKey) noexcept
{
    RouteLedgerKey key;
    key.actor = routeKey.actor;
    key.purpose = routeKey.purpose;
    key.mapId = routeKey.mapId;
    key.instanceId = routeKey.instanceId;
    key.targetKind = routeKey.targetKind;
    key.entry = routeKey.entry;
    key.stableSpawn = routeKey.stableSpawn;
    key.dbX = routeKey.dbX;
    key.dbY = routeKey.dbY;
    key.dbZ = routeKey.dbZ;
    key.dbO = routeKey.dbO;
    key.radius = routeKey.radius;
    key.segment = routeKey.segment;
    key.questId = routeKey.questId;
    key.objectiveFamily = routeKey.objectiveFamily;
    key.objectiveSlot = routeKey.objectiveSlot;
    key.routeCatalogVersion = routeKey.routeCatalogVersion;
    return key;
}

RouteFailureClass ClassifyRouteFailure(RouteFailure failure) noexcept
{
    switch (failure)
    {
        case RouteFailure::None: return RouteFailureClass::None;
        case RouteFailure::UnsupportedTransition: return RouteFailureClass::Transition;
        case RouteFailure::TargetLoadStateRequired:
        case RouteFailure::TargetNotLoadedAfterGridProbe:
        case RouteFailure::ExactLiveBindRequired: return RouteFailureClass::TargetAvailability;
        case RouteFailure::IdentityDrift:
        case RouteFailure::AnchorIdentityMismatch:
        case RouteFailure::GroundPlanIdentityMismatch: return RouteFailureClass::Identity;
        case RouteFailure::ApproachRequired:
        case RouteFailure::VerificationFailed: return RouteFailureClass::Interaction;
        case RouteFailure::RetryNotReady:
        case RouteFailure::ReentryRequired: return RouteFailureClass::Backoff;
        case RouteFailure::SafeAnchorRequired:
        case RouteFailure::UnsafeAnchor:
        case RouteFailure::DuplicateAnchor:
        case RouteFailure::AnchorLimit:
        case RouteFailure::GroundPlanRequired:
        case RouteFailure::InvalidGroundPlan:
        case RouteFailure::SegmentIndexRegressed:
        case RouteFailure::ReprobeRequired:
        case RouteFailure::DeadlineExceeded:
        case RouteFailure::Stalled: return RouteFailureClass::Path;
        case RouteFailure::InvalidIntent:
        case RouteFailure::InvalidEstimatedTravel:
        case RouteFailure::DeadlineOutOfBounds:
        case RouteFailure::InvalidObservation:
        case RouteFailure::SessionLedgerFull:
        case RouteFailure::DescriptorRejected:
        case RouteFailure::LeaseReleaseRequired:
        case RouteFailure::HandoffCapacityExceeded: return RouteFailureClass::Contract;
        case RouteFailure::LeaseReleaseFailed: return RouteFailureClass::Interaction;
    }
    return RouteFailureClass::Contract;
}

RouteLedgerKey MakeRouteLedgerKey(RouteKey const& routeKey, RouteFailure failure) noexcept
{
    RouteLedgerKey key = MakeRouteLedgerKey(routeKey);
    key.failureClass = ClassifyRouteFailure(failure);
    return key;
}

bool operator==(RouteKey const& left, RouteKey const& right) noexcept
{
    return left.actor == right.actor && left.purpose == right.purpose &&
        left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.targetKind == right.targetKind && left.entry == right.entry &&
        left.stableSpawn == right.stableSpawn && left.dbX == right.dbX &&
        left.dbY == right.dbY && left.dbZ == right.dbZ && left.dbO == right.dbO &&
        left.radius == right.radius && left.controller == right.controller &&
        left.job == right.job && left.lease == right.lease && left.segment == right.segment &&
        left.questId == right.questId && left.objectiveFamily == right.objectiveFamily &&
        left.objectiveSlot == right.objectiveSlot &&
        left.routeCatalogVersion == right.routeCatalogVersion;
}

bool operator!=(RouteKey const& left, RouteKey const& right) noexcept
{
    return !(left == right);
}

bool operator==(RouteLedgerKey const& left, RouteLedgerKey const& right) noexcept
{
    return left.actor == right.actor && left.purpose == right.purpose &&
        left.mapId == right.mapId && left.instanceId == right.instanceId &&
        left.targetKind == right.targetKind && left.entry == right.entry &&
        left.stableSpawn == right.stableSpawn && left.dbX == right.dbX &&
        left.dbY == right.dbY && left.dbZ == right.dbZ && left.dbO == right.dbO &&
        left.radius == right.radius && left.segment == right.segment &&
        left.questId == right.questId && left.objectiveFamily == right.objectiveFamily &&
        left.objectiveSlot == right.objectiveSlot &&
        left.routeCatalogVersion == right.routeCatalogVersion &&
        left.failureClass == right.failureClass;
}

bool operator!=(RouteLedgerKey const& left, RouteLedgerKey const& right) noexcept
{
    return !(left == right);
}

bool IsSupportedSegment(RouteSegment segment) noexcept
{
    switch (segment)
    {
        case RouteSegment::GroundPath:
        case RouteSegment::GridLoadApproach:
        case RouteSegment::AreaTrigger:
            return true;
        case RouteSegment::Taxi:
        case RouteSegment::Transport:
        case RouteSegment::InteractableGate:
        case RouteSegment::MovingPlatform:
        case RouteSegment::InstanceAdmission:
            return false;
    }
    return false;
}

RouteFailure ValidateRouteIntent(RouteIntent const& intent) noexcept
{
    if (intent.actor == 0 || intent.purpose == 0 || intent.targetKind == RouteTargetKind::Unknown ||
        intent.entry == 0 || intent.stableSpawn == 0 || intent.controller == 0 || intent.job == 0 ||
        intent.lease == 0 || !Finite(intent.dbX) || !Finite(intent.dbY) || !Finite(intent.dbZ) ||
        !Finite(intent.dbO) || !Finite(intent.radius) || intent.radius <= 0.0)
    {
        return RouteFailure::InvalidIntent;
    }
    RoutePurpose const purpose = static_cast<RoutePurpose>(intent.purpose);
    if ((purpose == RoutePurpose::QuestSource || purpose == RoutePurpose::QuestTarget) &&
        (intent.questId == 0 || intent.routeCatalogVersion == 0))
        return RouteFailure::InvalidIntent;
    if (!IsSupportedSegment(intent.segment))
        return RouteFailure::UnsupportedTransition;
    return RouteFailure::None;
}

double HardDeadlineSeconds(double estimatedTravelSeconds) noexcept
{
    return 3.0 * estimatedTravelSeconds + kHardDeadlineBufferSeconds;
}

bool DeadlineWithinBounds(Tick startedAt, double estimatedTravelSeconds) noexcept
{
    if (!Finite(estimatedTravelSeconds) || estimatedTravelSeconds < 0.0 ||
        estimatedTravelSeconds > kMaxEstimatedTravelSeconds)
    {
        return false;
    }

    double const seconds = HardDeadlineSeconds(estimatedTravelSeconds);
    if (!Finite(seconds) || seconds <= 0.0)
        return false;

    double const rounded = std::ceil(seconds);
    if (rounded > static_cast<double>(std::numeric_limits<Tick>::max()))
        return false;
    Tick const duration = static_cast<Tick>(rounded);
    return duration <= std::numeric_limits<Tick>::max() - startedAt;
}

Tick HardDeadlineAt(Tick startedAt, double estimatedTravelSeconds) noexcept
{
    if (!DeadlineWithinBounds(startedAt, estimatedTravelSeconds))
        return startedAt;
    double const rounded = std::ceil(HardDeadlineSeconds(estimatedTravelSeconds));
    return startedAt + static_cast<Tick>(rounded);
}

LedgerUpdate RecordBlocked(SessionLedger const& ledger, RouteLedgerKey const& ledgerKey,
                           Tick now, RouteFailure failure,
                           WorldChangeId blockedWorldChangeId) noexcept
{
    LedgerUpdate update;
    update.ledger = ledger;
    RouteLedgerKey failureKey = ledgerKey;
    failureKey.failureClass = ClassifyRouteFailure(failure);
    update.ledgerKey = failureKey;

    std::uint8_t index = FindLedgerIndex(update.ledger, failureKey);
    if (index == kInvalidLedgerIndex)
    {
        if (update.ledger.entryCount >= kMaxSessionLedgerEntries)
            return update;
        index = update.ledger.entryCount;
        ++update.ledger.entryCount;
        update.ledger.entries[index] = SessionLedgerEntry{};
        update.ledger.entries[index].occupied = true;
        update.ledger.entries[index].ledgerKey = failureKey;
    }

    SessionLedgerEntry& entry = update.ledger.entries[index];
    if (entry.blockCount < kReentryBlockCount)
        ++entry.blockCount;

    std::size_t delayIndex = entry.blockCount == 0 ? 0 : entry.blockCount - 1;
    if (delayIndex >= kRetryDelaysSeconds.size())
        delayIndex = kRetryDelaysSeconds.size() - 1;
    // A loaded grid with no exact live quest target cannot be fixed by immediately
    // repeating the same route. Leave a useful grind window before trying again.
    Tick const retryDelay = failure == RouteFailure::ExactLiveBindRequired
        ? std::max<Tick>(kRetryDelaysSeconds[delayIndex], 300)
        : kRetryDelaysSeconds[delayIndex];
    entry.nextRetryAt = SaturatingAdd(now, retryDelay);
    entry.requiresReentry = entry.blockCount >= kReentryBlockCount;
    if (blockedWorldChangeId != 0)
        entry.blockedWorldChangeId = blockedWorldChangeId;

    update.accepted = true;
    update.requiresReentry = entry.requiresReentry;
    update.nextRetryAt = entry.nextRetryAt;
    return update;
}

bool RetryAllowed(SessionLedger const& ledger, RouteLedgerKey const& ledgerKey, Tick now,
                  bool explicitReentrySignal, WorldChangeId currentWorldChangeId) noexcept
{
    for (std::uint8_t index = 0;
         index < ledger.entryCount && index < kMaxSessionLedgerEntries; ++index)
    {
        SessionLedgerEntry const& entry = ledger.entries[index];
        if (!entry.occupied || !SameRouteIdentity(entry.ledgerKey, ledgerKey))
            continue;
        if (now < entry.nextRetryAt)
            return false;
        if (entry.requiresReentry &&
            (!explicitReentrySignal || currentWorldChangeId == 0))
            return false;
    }
    return true;
}

RouteStep StartRoute(RouteIntent const& intent, double estimatedTravelSeconds, Tick now,
                     SessionLedger const& ledger, bool explicitReentrySignal,
                     WorldChangeId currentWorldChangeId) noexcept
{
    RouteSession session;
    session.ledger = ledger;
    session.state.routeKey = MakeRouteKey(intent);
    session.state.ledgerKey = MakeRouteLedgerKey(session.state.routeKey);
    session.state.startedAt = now;
    session.state.lastProgressAt = now;
    session.state.estimatedTravelSeconds = estimatedTravelSeconds;
    session.state.hasWorldChangeId = currentWorldChangeId != 0;
    session.state.lastWorldChangeId = currentWorldChangeId;

    RouteFailure const intentFailure = ValidateRouteIntent(intent);
    if (intentFailure != RouteFailure::None)
        return YieldWithoutLedgerWrite(session, intentFailure);

    if (!Finite(estimatedTravelSeconds) || estimatedTravelSeconds < 0.0 ||
        estimatedTravelSeconds > kMaxEstimatedTravelSeconds)
    {
        return YieldWithoutLedgerWrite(session, RouteFailure::InvalidEstimatedTravel);
    }

    if (!DeadlineWithinBounds(now, estimatedTravelSeconds))
        return YieldWithoutLedgerWrite(session, RouteFailure::DeadlineOutOfBounds);
    session.state.hardDeadlineAt = HardDeadlineAt(now, estimatedTravelSeconds);

    bool hasRouteEntry = false;
    Tick nextRetryAt = 0;
    bool requiresReentry = false;
    for (std::uint8_t index = 0;
         index < ledger.entryCount && index < kMaxSessionLedgerEntries; ++index)
    {
        SessionLedgerEntry const& entry = ledger.entries[index];
        if (!entry.occupied || !SameRouteIdentity(entry.ledgerKey, session.state.ledgerKey))
            continue;
        hasRouteEntry = true;
        if (entry.nextRetryAt > nextRetryAt)
            nextRetryAt = entry.nextRetryAt;
        requiresReentry = requiresReentry || entry.requiresReentry;
    }
    if (!hasRouteEntry && ledger.entryCount >= kMaxSessionLedgerEntries)
        return YieldWithoutLedgerWrite(session, RouteFailure::SessionLedgerFull);

    if (!RetryAllowed(ledger, session.state.ledgerKey, now, explicitReentrySignal,
                      currentWorldChangeId))
    {
        RouteFailure const failure = requiresReentry
            ? RouteFailure::ReentryRequired : RouteFailure::RetryNotReady;
        return YieldWithoutLedgerWrite(session, failure, nextRetryAt);
    }

    RouteResult result = MakeResult(session, RouteCommandKind::None, RouteFailure::None);
    return RouteStep{session, result};
}

RouteStep AdvanceRoute(RouteSession const& inputSession, RouteObservation const& observation,
                       Tick now) noexcept
{
    RouteSession session = inputSession;
    if (session.state.stage == RouteStage::Arrived)
        return FinishArrived(session);
    if (session.state.stage == RouteStage::Blocked)
    {
        RouteResult result = MakeCommandResult(session, RouteCommandKind::Yield,
                                               session.state.failure);
        result.blocked = true;
        result.releaseLease = true;
        return RouteStep{session, result};
    }

    // Move is command issuance only.  Its accompanying observation is deliberately ignored; the
    // caller must return a completed reprobe before any evidence can update progress state.
    if (now >= session.state.hardDeadlineAt)
        return Block(session, RouteFailure::DeadlineExceeded, now);
    if (session.state.stage == RouteStage::Move)
    {
        session.state.stage = RouteStage::Reprobe;
        return Pending(session, RouteCommandKind::Reprobe, RouteFailure::None, false);
    }

    if (observation.hasRemainingDistance &&
        (!Finite(observation.remainingDistance) || observation.remainingDistance < 0.0))
    {
        return Block(session, RouteFailure::InvalidObservation, now);
    }

    if (HasIdentityDrift(session.state, observation))
        return Block(session, RouteFailure::IdentityDrift, now);

    if (observation.hasSegmentIndex && session.state.hasSegmentIndex &&
        observation.segmentIndex < session.state.lastSegmentIndex)
    {
        return Block(session, RouteFailure::SegmentIndexRegressed, now);
    }

    switch (session.state.stage)
    {
        case RouteStage::Descriptor:
            if (!observation.descriptorConfirmed)
                return Pending(session, RouteCommandKind::None,
                               RouteFailure::DescriptorRejected, false);
            session.state.stage = RouteStage::SelectSafeAnchor;
            return Pending(session, RouteCommandKind::SelectSafeAnchor, RouteFailure::None,
                           false);

        case RouteStage::SelectSafeAnchor:
        {
            if (observation.availableAnchorCount > kMaxSuppliedAnchors)
                return Block(session, RouteFailure::InvalidObservation, now);
            if (!observation.safeAnchorSupplied)
            {
                if (observation.anchorSearchComplete && observation.targetLoadKnown &&
                    !observation.targetLoaded && session.state.anchorAttemptCount > 0)
                    return Block(session, RouteFailure::TargetNotLoadedAfterGridProbe, now);
                return Pending(session, RouteCommandKind::SelectSafeAnchor,
                               RouteFailure::SafeAnchorRequired, false);
            }

            RouteFailure const anchorFailure =
                ValidateAnchor(session.state.routeKey, observation.safeAnchor);
            if (anchorFailure != RouteFailure::None)
                return Block(session, anchorFailure, now);
            if (AnchorWasAttempted(session.state, observation.safeAnchor.anchorId))
            {
                if (observation.anchorSearchComplete && observation.targetLoadKnown &&
                    !observation.targetLoaded && observation.availableAnchorCount <=
                        session.state.anchorAttemptCount)
                    return Block(session, RouteFailure::TargetNotLoadedAfterGridProbe, now);
                return Pending(session, RouteCommandKind::SelectSafeAnchor,
                               RouteFailure::DuplicateAnchor, false);
            }
            if (session.state.anchorAttemptCount >= kMaxSuppliedAnchors)
                return Block(session, RouteFailure::AnchorLimit, now);

            session.state.anchorAttempts[session.state.anchorAttemptCount] =
                observation.safeAnchor.anchorId;
            ++session.state.anchorAttemptCount;
            session.state.selectedAnchor = observation.safeAnchor;
            session.state.stage = RouteStage::GroundPlan;
            return Pending(session, RouteCommandKind::GroundPlan, RouteFailure::None,
                           false);
        }

        case RouteStage::GroundPlan:
        {
            if (!observation.groundPlanSupplied)
                return Pending(session, RouteCommandKind::GroundPlan,
                               RouteFailure::GroundPlanRequired, false);

            RouteFailure const planFailure = ValidatePlan(session.state, observation.groundPlan);
            if (planFailure != RouteFailure::None)
                return Block(session, planFailure, now);

            session.state.groundPlan = observation.groundPlan;
            if (!session.state.hasDistance)
                session.state.initialRemainingDistance =
                    observation.groundPlan.estimatedRemainingDistance;
            session.state.lastRemainingDistance =
                observation.groundPlan.estimatedRemainingDistance;
            session.state.hasDistance = true;
            session.state.stage = RouteStage::Move;
            return Pending(session, RouteCommandKind::Move, RouteFailure::None,
                           false);
        }

        case RouteStage::Move:
            return Pending(session, RouteCommandKind::Reprobe, RouteFailure::None, false);

        case RouteStage::Reprobe:
        {
            if (!observation.reprobeComplete)
            {
                RecoveryDecision const recovery = Recover(session.state, now);
                if (recovery.block)
                    return Block(session, recovery.failure, now);
                if (recovery.commandReady)
                    return Pending(session, recovery.command, recovery.failure, false);
                return Pending(session, RouteCommandKind::Reprobe,
                               RouteFailure::ReprobeRequired, false);
            }

            bool const progressObserved = ObserveProgress(session.state, observation, now);
            if (observation.targetLoadKnown && !observation.targetLoaded)
                return SelectNextAnchorAfterGridProbe(session, now, progressObserved);

            RecoveryDecision const recovery = Recover(session.state, now);
            if (recovery.block)
                return Block(session, recovery.failure, now);
            if (recovery.commandReady)
                return Pending(session, recovery.command, recovery.failure, progressObserved);
            session.state.stage = RouteStage::ExactLiveBind;
            return Pending(session, RouteCommandKind::ExactLiveBind, RouteFailure::None,
                           progressObserved);
        }

        case RouteStage::ExactLiveBind:
        {
            if (!observation.exactBindComplete)
            {
                RecoveryDecision const recovery = Recover(session.state, now);
                if (recovery.block)
                    return Block(session, recovery.failure, now);
                if (recovery.commandReady)
                    return Pending(session, recovery.command, recovery.failure, false);
                return Pending(session, RouteCommandKind::ExactLiveBind,
                               RouteFailure::ExactLiveBindRequired, false);
            }
            if (!observation.targetLoadKnown)
                return Block(session, RouteFailure::TargetLoadStateRequired, now);
            if (!observation.targetLoaded)
            {
                bool const progressObserved = ObserveProgress(session.state, observation, now);
                return SelectNextAnchorAfterGridProbe(session, now, progressObserved);
            }
            if (!observation.hasLiveIdentity)
                return Block(session, RouteFailure::ExactLiveBindRequired, now);
            if (observation.liveIdentity != session.state.routeKey)
                return Block(session, RouteFailure::IdentityDrift, now);

            bool const progressObserved = ObserveProgress(session.state, observation, now);
            session.state.exactLiveBound = true;
            session.state.stage = RouteStage::Approach;
            return Pending(session, RouteCommandKind::Approach, RouteFailure::None,
                           progressObserved);
        }

        case RouteStage::Approach:
        {
            if (!observation.approachConfirmed)
            {
                RecoveryDecision const recovery = Recover(session.state, now);
                if (recovery.block)
                    return Block(session, recovery.failure, now);
                if (recovery.commandReady)
                    return Pending(session, recovery.command, recovery.failure, false);
                return Pending(session, RouteCommandKind::Approach,
                               RouteFailure::ApproachRequired, false);
            }
            if (!session.state.exactLiveBound)
                return Block(session, RouteFailure::ExactLiveBindRequired, now);

            bool const progressObserved = ObserveProgress(session.state, observation, now);
            RecoveryDecision const recovery = Recover(session.state, now);
            if (recovery.block)
                return Block(session, recovery.failure, now);
            if (recovery.commandReady)
                return Pending(session, recovery.command, recovery.failure, progressObserved);
            session.state.stage = RouteStage::Verify;
            return Pending(session, RouteCommandKind::Verify, RouteFailure::None,
                           progressObserved);
        }

        case RouteStage::Verify:
        {
            if (!observation.targetLoadKnown || !observation.targetLoaded ||
                !observation.hasLiveIdentity)
            {
                return Block(session, RouteFailure::VerificationFailed, now);
            }
            if (observation.liveIdentity != session.state.routeKey)
                return Block(session, RouteFailure::IdentityDrift, now);
            if (!observation.verificationPassed || !observation.withinRadius)
                return Block(session, RouteFailure::VerificationFailed, now);
            (void)ObserveProgress(session.state, observation, now);
            return FinishArrived(session);
        }

        case RouteStage::Arrived:
            return FinishArrived(session);
        case RouteStage::Blocked:
            return Block(session, session.state.failure, now);
    }

    return Block(session, RouteFailure::InvalidIntent, now);
}

RouteStep ResetAttempt(RouteSession const& inputSession, Tick now,
                       bool explicitReentrySignal,
                       WorldChangeId currentWorldChangeId) noexcept
{
    RouteSession session = inputSession;
    RouteKey const key = inputSession.state.routeKey;
    RouteLedgerKey const ledgerKey = MakeRouteLedgerKey(key);
    double const estimate = inputSession.state.estimatedTravelSeconds;
    session.state = RouteState{};
    session.state.routeKey = key;
    session.state.ledgerKey = ledgerKey;
    session.state.startedAt = now;
    session.state.lastProgressAt = now;
    session.state.estimatedTravelSeconds = estimate;
    session.state.hasWorldChangeId = currentWorldChangeId != 0;
    session.state.lastWorldChangeId = currentWorldChangeId;

    if (!Finite(estimate) || estimate < 0.0 || estimate > kMaxEstimatedTravelSeconds)
        return YieldWithoutLedgerWrite(session, RouteFailure::InvalidEstimatedTravel);
    if (!DeadlineWithinBounds(now, estimate))
        return YieldWithoutLedgerWrite(session, RouteFailure::DeadlineOutOfBounds);
    session.state.hardDeadlineAt = HardDeadlineAt(now, estimate);

    bool hasRouteEntry = false;
    Tick nextRetryAt = 0;
    bool requiresReentry = false;
    for (std::uint8_t index = 0;
         index < session.ledger.entryCount && index < kMaxSessionLedgerEntries; ++index)
    {
        SessionLedgerEntry const& entry = session.ledger.entries[index];
        if (!entry.occupied || !SameRouteIdentity(entry.ledgerKey, ledgerKey))
            continue;
        hasRouteEntry = true;
        if (entry.nextRetryAt > nextRetryAt)
            nextRetryAt = entry.nextRetryAt;
        requiresReentry = requiresReentry || entry.requiresReentry;
    }
    if (!hasRouteEntry && session.ledger.entryCount >= kMaxSessionLedgerEntries)
    {
        return YieldWithoutLedgerWrite(session, RouteFailure::SessionLedgerFull);
    }

    if (!RetryAllowed(session.ledger, ledgerKey, now, explicitReentrySignal,
                      currentWorldChangeId))
    {
        RouteFailure const failure = requiresReentry
            ? RouteFailure::ReentryRequired : RouteFailure::RetryNotReady;
        return YieldWithoutLedgerWrite(session, failure, nextRetryAt);
    }

    RouteResult result = MakeResult(session, RouteCommandKind::None, RouteFailure::None);
    return RouteStep{session, result};
}

}  // namespace AutoWowOracleRoute
