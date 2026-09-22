/*
 * Pure T2 route policy.
 *
 * This boundary contains only scalar values, fixed-size arrays, and deterministic transitions.
 * The caller supplies observations, safe anchors, and ground plans.  No world object or native
 * movement implementation is owned here.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_ROUTE_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_ROUTE_EXECUTOR_H

#include <array>
#include <cstdint>

namespace AutoWowOracleRoute
{
using ActorId = std::uint64_t;
using PurposeId = std::uint64_t;
using MapId = std::uint32_t;
using InstanceId = std::uint32_t;
using AreaId = std::uint32_t;
using EntryId = std::uint32_t;
using StableSpawnId = std::uint64_t;
using AnchorId = std::uint64_t;
using ControllerId = std::uint64_t;
using JobId = std::uint64_t;
using LeaseId = std::uint64_t;
using SegmentIndex = std::uint64_t;
using Tick = std::uint64_t;
using QuestId = std::uint32_t;
using ObjectiveFamilyId = std::uint8_t;
using ObjectiveSlotId = std::uint8_t;
using RouteCatalogVersion = std::uint32_t;
using WorldChangeId = std::uint64_t;

inline constexpr std::uint8_t kMaxSuppliedAnchors = 3;
inline constexpr std::uint8_t kMaxSessionLedgerEntries = 32;
inline constexpr double kProgressDistanceYards = 4.0;
inline constexpr Tick kRepathAfterSeconds = 15;
inline constexpr Tick kReresolveAfterSeconds = 30;
inline constexpr Tick kBlockAfterSeconds = 45;
inline constexpr double kHardDeadlineBufferSeconds = 30.0;
inline constexpr double kMaxEstimatedTravelSeconds = 86'400.0;
inline constexpr std::array<Tick, 3> kRetryDelaysSeconds{30, 120, 600};
inline constexpr RouteCatalogVersion kQuestRouteCatalogVersion = 2;
inline constexpr ObjectiveFamilyId kQuestFinisherObjectiveFamily = 0xFF;
inline constexpr ObjectiveSlotId kQuestFinisherObjectiveSlot = 0xFF;

enum class RoutePurpose : std::uint8_t
{
    Unknown = 0,
    QuestSource,
    QuestTarget,
    GatherSource,
    Travel,
    Recovery
};

enum class RouteTargetKind : std::uint8_t
{
    Unknown = 0,
    Creature,
    GameObject,
    AreaTrigger,
    QuestGiver,
    QuestFinisher,
    GatherSource
};

enum class RouteSegment : std::uint8_t
{
    GroundPath = 0,
    GridLoadApproach,
    AreaTrigger,
    Taxi,
    Transport,
    InteractableGate,
    MovingPlatform,
    InstanceAdmission
};

enum class RouteStage : std::uint8_t
{
    Descriptor = 0,
    SelectSafeAnchor,
    GroundPlan,
    Move,
    Reprobe,
    ExactLiveBind,
    Approach,
    Verify,
    Arrived,
    Blocked
};

enum class RouteCommandKind : std::uint8_t
{
    None = 0,
    SelectSafeAnchor,
    GroundPlan,
    Move,
    Reprobe,
    ExactLiveBind,
    Approach,
    Verify,
    Repath,
    Reresolve,
    Yield
};

enum class RouteFailure : std::uint8_t
{
    None = 0,
    InvalidIntent,
    InvalidEstimatedTravel,
    DeadlineOutOfBounds,
    InvalidObservation,
    UnsupportedTransition,
    RetryNotReady,
    ReentryRequired,
    SessionLedgerFull,
    DescriptorRejected,
    SafeAnchorRequired,
    UnsafeAnchor,
    AnchorIdentityMismatch,
    DuplicateAnchor,
    AnchorLimit,
    GroundPlanRequired,
    GroundPlanIdentityMismatch,
    InvalidGroundPlan,
    IdentityDrift,
    SegmentIndexRegressed,
    ReprobeRequired,
    TargetLoadStateRequired,
    TargetNotLoadedAfterGridProbe,
    ExactLiveBindRequired,
    ApproachRequired,
    VerificationFailed,
    DeadlineExceeded,
    Stalled,
    LeaseReleaseRequired,
    LeaseReleaseFailed,
    HandoffCapacityExceeded
};

enum class RouteFailureClass : std::uint8_t
{
    None = 0,
    Contract,
    Transition,
    Path,
    TargetAvailability,
    Identity,
    Interaction,
    Backoff
};

struct RouteCoordinate
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double o = 0.0;
};

// Exact route identity.  These fields are copied from the caller's descriptor; none are resolved
// or replaced by the policy.
struct RouteIntent
{
    ActorId actor = 0;
    PurposeId purpose = 0;
    MapId mapId = 0;
    InstanceId instanceId = 0;
    RouteTargetKind targetKind = RouteTargetKind::Unknown;
    EntryId entry = 0;
    StableSpawnId stableSpawn = 0;
    double dbX = 0.0;
    double dbY = 0.0;
    double dbZ = 0.0;
    double dbO = 0.0;
    double radius = 0.0;
    ControllerId controller = 0;
    JobId job = 0;
    LeaseId lease = 0;
    RouteSegment segment = RouteSegment::GroundPath;
    QuestId questId = 0;
    ObjectiveFamilyId objectiveFamily = 0;
    ObjectiveSlotId objectiveSlot = 0;
    RouteCatalogVersion routeCatalogVersion = 0;
};

// RouteKey is deliberately a complete copy of RouteIntent identity.  Equality is exact, including
// database coordinates and ownership fields.
struct RouteKey
{
    ActorId actor = 0;
    PurposeId purpose = 0;
    MapId mapId = 0;
    InstanceId instanceId = 0;
    RouteTargetKind targetKind = RouteTargetKind::Unknown;
    EntryId entry = 0;
    StableSpawnId stableSpawn = 0;
    double dbX = 0.0;
    double dbY = 0.0;
    double dbZ = 0.0;
    double dbO = 0.0;
    double radius = 0.0;
    ControllerId controller = 0;
    JobId job = 0;
    LeaseId lease = 0;
    RouteSegment segment = RouteSegment::GroundPath;
    QuestId questId = 0;
    ObjectiveFamilyId objectiveFamily = 0;
    ObjectiveSlotId objectiveSlot = 0;
    RouteCatalogVersion routeCatalogVersion = 0;
};

// Durable backoff identity.  Controller, job, and lease are intentionally excluded because they
// can change when the same actor reacquires ownership for the same target route.
struct RouteLedgerKey
{
    ActorId actor = 0;
    PurposeId purpose = 0;
    MapId mapId = 0;
    InstanceId instanceId = 0;
    RouteTargetKind targetKind = RouteTargetKind::Unknown;
    EntryId entry = 0;
    StableSpawnId stableSpawn = 0;
    double dbX = 0.0;
    double dbY = 0.0;
    double dbZ = 0.0;
    double dbO = 0.0;
    double radius = 0.0;
    RouteSegment segment = RouteSegment::GroundPath;
    QuestId questId = 0;
    ObjectiveFamilyId objectiveFamily = 0;
    ObjectiveSlotId objectiveSlot = 0;
    RouteCatalogVersion routeCatalogVersion = 0;
    RouteFailureClass failureClass = RouteFailureClass::None;
};

struct SafeAnchor
{
    AnchorId anchorId = 0;
    MapId mapId = 0;
    InstanceId instanceId = 0;
    AreaId areaId = 0;
    RouteCoordinate coordinate;
    bool callerSupplied = false;
    bool safe = false;
};

struct GroundPlan
{
    RouteKey routeKey;
    AnchorId anchorId = 0;
    RouteCoordinate nextPoint;
    double estimatedRemainingDistance = 0.0;
    bool callerSupplied = false;
    bool valid = false;
    bool nextPointSupplied = false;
};

struct RouteObservation
{
    bool descriptorConfirmed = false;
    bool safeAnchorSupplied = false;
    SafeAnchor safeAnchor;
    bool groundPlanSupplied = false;
    GroundPlan groundPlan;
    bool reprobeComplete = false;
    bool exactBindComplete = false;
    bool targetLoadKnown = false;
    bool targetLoaded = false;
    bool hasLiveIdentity = false;
    RouteKey liveIdentity;
    bool approachConfirmed = false;
    bool verificationPassed = false;
    bool withinRadius = false;

    bool hasGridState = false;
    bool gridLoaded = false;
    bool hasMapArea = false;
    MapId currentMapId = 0;
    AreaId currentAreaId = 0;
    bool hasRemainingDistance = false;
    double remainingDistance = 0.0;
    bool hasSegmentIndex = false;
    SegmentIndex segmentIndex = 0;

    // These are observed deltas, not mutation requests.
    bool objectiveDelta = false;
    bool questDelta = false;
    bool inventoryDelta = false;
    bool interactionDelta = false;

    // The anchor search is a deterministic snapshot of real path/grid APIs. When complete and no
    // distinct unattempted anchor remains after a grid probe, the policy can type the terminal as
    // TargetNotLoadedAfterGridProbe instead of waiting on a generic timer.
    bool anchorSearchComplete = false;
    std::uint8_t availableAnchorCount = 0;

    // Real world-change version (quest/objective/inventory/map/grid facts), sampled by the caller.
    // It is observed only after reprobe and is also retained with a blocked ledger entry.
    bool hasWorldChangeId = false;
    WorldChangeId worldChangeId = 0;
};

struct RouteCommand
{
    RouteCommandKind kind = RouteCommandKind::None;
    RouteKey routeKey;
    bool hasAnchor = false;
    SafeAnchor anchor;
    bool hasPlan = false;
    GroundPlan plan;
};

struct DistanceSummary
{
    bool available = false;
    double initial = 0.0;
    double remaining = 0.0;
    double reduced = 0.0;
};

struct SessionLedgerEntry
{
    bool occupied = false;
    RouteLedgerKey ledgerKey;
    std::uint8_t blockCount = 0;
    Tick nextRetryAt = 0;
    bool requiresReentry = false;
    WorldChangeId blockedWorldChangeId = 0;
};

struct SessionLedger
{
    std::array<SessionLedgerEntry, kMaxSessionLedgerEntries> entries{};
    std::uint8_t entryCount = 0;
};

struct LedgerUpdate
{
    SessionLedger ledger;
    bool accepted = false;
    bool requiresReentry = false;
    Tick nextRetryAt = 0;
    RouteLedgerKey ledgerKey;
};

struct RouteState
{
    RouteKey routeKey;
    RouteLedgerKey ledgerKey;
    RouteStage stage = RouteStage::Descriptor;
    RouteFailure failure = RouteFailure::None;
    Tick startedAt = 0;
    Tick lastProgressAt = 0;
    Tick hardDeadlineAt = 0;
    double estimatedTravelSeconds = 0.0;
    double initialRemainingDistance = 0.0;
    double lastRemainingDistance = 0.0;
    bool hasDistance = false;
    bool hasLastGridState = false;
    bool lastGridLoaded = false;
    bool hasLastMapArea = false;
    MapId lastMapId = 0;
    AreaId lastAreaId = 0;
    bool hasSegmentIndex = false;
    SegmentIndex lastSegmentIndex = 0;
    std::uint32_t progressEvents = 0;
    std::uint8_t anchorAttemptCount = 0;
    std::array<AnchorId, kMaxSuppliedAnchors> anchorAttempts{};
    SafeAnchor selectedAnchor;
    GroundPlan groundPlan;
    bool repathUsed = false;
    bool reresolveUsed = false;
    bool exactLiveBound = false;
    bool releaseLease = false;
    bool leaseReleaseAuthoritative = false;
    bool hasWorldChangeId = false;
    WorldChangeId lastWorldChangeId = 0;
};

struct RouteSession
{
    RouteState state;
    SessionLedger ledger;
};

struct RouteResult
{
    RouteKey routeKey;
    RouteLedgerKey ledgerKey;
    RouteStage stage = RouteStage::Descriptor;
    RouteFailure failure = RouteFailure::None;
    RouteCommand command;
    std::uint8_t anchorAttempts = 0;
    std::array<AnchorId, kMaxSuppliedAnchors> attemptedAnchors{};
    DistanceSummary distance;
    bool segmentIndexAvailable = false;
    SegmentIndex segmentIndex = 0;
    Tick nextRetryAt = 0;
    bool releaseLease = false;
    bool leaseReleaseAuthoritative = false;
    bool arrived = false;
    bool blocked = false;
    bool progressObserved = false;
};

struct RouteStep
{
    RouteSession session;
    RouteResult result;
};

[[nodiscard]] RouteKey MakeRouteKey(RouteIntent const& intent) noexcept;
[[nodiscard]] RouteLedgerKey MakeRouteLedgerKey(RouteKey const& routeKey) noexcept;
[[nodiscard]] RouteLedgerKey MakeRouteLedgerKey(RouteKey const& routeKey,
                                                RouteFailure failure) noexcept;
[[nodiscard]] RouteFailureClass ClassifyRouteFailure(RouteFailure failure) noexcept;
[[nodiscard]] bool operator==(RouteKey const& left, RouteKey const& right) noexcept;
[[nodiscard]] bool operator!=(RouteKey const& left, RouteKey const& right) noexcept;
[[nodiscard]] bool operator==(RouteLedgerKey const& left,
                              RouteLedgerKey const& right) noexcept;
[[nodiscard]] bool operator!=(RouteLedgerKey const& left,
                              RouteLedgerKey const& right) noexcept;
[[nodiscard]] bool IsSupportedSegment(RouteSegment segment) noexcept;
[[nodiscard]] RouteFailure ValidateRouteIntent(RouteIntent const& intent) noexcept;
[[nodiscard]] double HardDeadlineSeconds(double estimatedTravelSeconds) noexcept;
[[nodiscard]] bool DeadlineWithinBounds(Tick startedAt,
                                        double estimatedTravelSeconds) noexcept;
[[nodiscard]] Tick HardDeadlineAt(Tick startedAt, double estimatedTravelSeconds) noexcept;

[[nodiscard]] LedgerUpdate RecordBlocked(SessionLedger const& ledger,
                                         RouteLedgerKey const& ledgerKey, Tick now,
                                         RouteFailure failure = RouteFailure::Stalled,
                                         WorldChangeId blockedWorldChangeId = 0) noexcept;
[[nodiscard]] bool RetryAllowed(SessionLedger const& ledger,
                                RouteLedgerKey const& ledgerKey,
                                Tick now, bool explicitReentrySignal,
                                WorldChangeId currentWorldChangeId = 0) noexcept;

[[nodiscard]] RouteStep StartRoute(RouteIntent const& intent, double estimatedTravelSeconds,
                                   Tick now, SessionLedger const& ledger,
                                   bool explicitReentrySignal = false,
                                   WorldChangeId currentWorldChangeId = 0) noexcept;
[[nodiscard]] RouteStep AdvanceRoute(RouteSession const& session,
                                     RouteObservation const& observation, Tick now) noexcept;
[[nodiscard]] RouteStep ResetAttempt(RouteSession const& session, Tick now,
                                     bool explicitReentrySignal = false,
                                     WorldChangeId currentWorldChangeId = 0) noexcept;

}  // namespace AutoWowOracleRoute

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_ROUTE_EXECUTOR_H
