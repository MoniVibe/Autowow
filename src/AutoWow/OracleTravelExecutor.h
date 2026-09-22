/*
 * Pure, fail-closed Oracle same-map walk executor.
 *
 * This adapter deliberately owns no Player, map, pathfinder, or database object.  A caller must
 * supply a complete, deterministic path-probe result and a native movement callback.  The
 * callback is granted a budget of one movement step; it is not allowed to select a replacement
 * target or transition.  This keeps this slice independent from the central Oracle contract and
 * safe to test without a running world.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_TRAVEL_EXECUTOR_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_TRAVEL_EXECUTOR_H

#include <cstdint>
#include <functional>
#include <string_view>

namespace AutoWowOracleTravel
{
using Guid = std::uint64_t;
using MapId = std::uint32_t;
using InstanceId = std::uint32_t;
using PhaseId = std::uint64_t;
using FactVersion = std::uint64_t;
using PathDigest = std::uint64_t;
using DecisionId = std::uint64_t;
using Tick = std::uint64_t;

inline constexpr double kMaximumCoordinateMagnitude = 1'000'000.0;
inline constexpr double kMaximumArrivalRadius = 25.0;
inline constexpr double kMaximumSegmentLength = 100.0;
inline constexpr double kProgressEpsilon = 1e-6;
inline constexpr std::uint32_t kDefaultNoProgressLimit = 3;

struct Position
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct WalkDomain
{
    MapId mapId = 0;
    InstanceId instanceId = 0;
    PhaseId phase = 0;
};

struct SafeEndpoint
{
    WalkDomain domain;
    Position position;
    bool safe = false;
};

enum class PathProbeResult : std::uint8_t
{
    NoPath,
    Partial,
    Complete
};

enum class LeaseResource : std::uint8_t
{
    None,
    Transition
};

struct WalkReference
{
    Guid botGuid = 0;
    WalkDomain domain;
    FactVersion factVersion = 0;
    SafeEndpoint endpoint;
    double arrivalRadius = 0.0;
    double maxSegmentLength = 0.0;
    PathDigest probeDigest = 0;
    PathProbeResult probeResult = PathProbeResult::NoPath;
};

struct WalkRequest
{
    Guid botGuid = 0;
    DecisionId decisionId = 0;
    WalkReference reference;
};

struct LeaseProof
{
    bool ownershipVerified = false;
    Guid botGuid = 0;
    DecisionId decisionId = 0;
    LeaseResource resource = LeaseResource::None;
    Tick acquiredAt = 0;
    Tick expiresAt = 0;
};

struct WalkWorldFacts
{
    bool valid = false;
    Guid botGuid = 0;
    WalkDomain domain;
    FactVersion factVersion = 0;
    Position position;
    bool alive = false;
    bool inCombat = false;
    bool onTransport = false;
};

struct PathProbe
{
    Guid botGuid = 0;
    WalkDomain domain;
    FactVersion factVersion = 0;
    SafeEndpoint destination;
    Position nextSafePoint;
    bool nextPointSafe = false;
    double segmentLength = 0.0;
    PathDigest digest = 0;
    PathProbeResult result = PathProbeResult::NoPath;
};

struct NativeMoveCommand
{
    Guid botGuid = 0;
    DecisionId decisionId = 0;
    WalkDomain domain;
    SafeEndpoint destination;
    Position nextSafePoint;
    double plannedSegmentLength = 0.0;
    double maxSegmentLength = 0.0;
    PathDigest probeDigest = 0;
    std::uint32_t nativeStepBudget = 1;
};

struct NativeMoveResult
{
    bool accepted = false;
    bool ownershipStillVerified = false;
    bool substituted = false;
    bool overBudget = false;
    std::uint32_t nativeStepCount = 0;
    Position appliedPoint;
    double appliedSegmentLength = 0.0;
    WalkWorldFacts after;
};

struct WalkExecutionInput
{
    Tick now = 0;
    WalkRequest request;
    WalkReference expectedReference;
    LeaseProof lease;
    WalkWorldFacts before;
    PathProbe preProbe;
};

struct WalkExecutorState
{
    std::uint32_t noProgressCount = 0;
    std::uint32_t noProgressLimit = kDefaultNoProgressLimit;
    bool stuck = false;
};

enum class WalkReceiptStatus : std::uint8_t
{
    Rejected,
    Progressed,
    Arrived,
    NoProgress,
    Stuck
};

enum class WalkFailureReason : std::uint8_t
{
    None,
    AlreadyStuck,
    InvalidRequest,
    RequestReferenceMismatch,
    LeaseMissing,
    LeaseExpired,
    LeaseResourceMismatch,
    LeaseOwnerMismatch,
    LeaseDecisionMismatch,
    StaleFacts,
    BotMismatch,
    DomainMismatch,
    CrossMap,
    UnsafeEndpoint,
    InvalidBounds,
    ProbeNoPath,
    ProbeReferenceMismatch,
    ProbeDigestMismatch,
    ProbeFactsStale,
    ProbeResultMismatch,
    NativeRejected,
    NativeOwnershipLost,
    NativeOverBudget,
    NativeSubstitution,
    PostFactsStale,
    PostStateInvalid,
    PostDomainMismatch,
    PostCombatOrTransport,
    NoProgress,
    NoProgressLimit
};

struct WalkReceipt
{
    WalkReceiptStatus status = WalkReceiptStatus::Rejected;
    WalkFailureReason reason = WalkFailureReason::None;
    Guid botGuid = 0;
    DecisionId decisionId = 0;
    PathProbeResult probeResult = PathProbeResult::NoPath;
    PathDigest probeDigest = 0;
    FactVersion beforeFactVersion = 0;
    FactVersion afterFactVersion = 0;
    std::uint32_t nativeCallbackCount = 0;
    std::uint32_t noProgressCount = 0;
    bool partialPath = false;
    bool arrived = false;
    double beforeDistance = 0.0;
    double afterDistance = 0.0;
};

using NativeMoveCallback = std::function<NativeMoveResult(NativeMoveCommand const&)>;

[[nodiscard]] bool operator==(Position const& left, Position const& right);
[[nodiscard]] bool operator==(WalkDomain const& left, WalkDomain const& right);
[[nodiscard]] bool operator==(SafeEndpoint const& left, SafeEndpoint const& right);
[[nodiscard]] bool operator==(WalkReference const& left, WalkReference const& right);

[[nodiscard]] std::string_view WalkFailureReasonName(WalkFailureReason reason);
[[nodiscard]] std::string_view WalkReceiptStatusName(WalkReceiptStatus status);
[[nodiscard]] std::string_view PathProbeResultName(PathProbeResult result);

[[nodiscard]] WalkReceipt ExecuteWalkStep(WalkExecutionInput const& input,
                                           WalkExecutorState& state,
                                           NativeMoveCallback const& nativeMove);
}  // namespace AutoWowOracleTravel

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_TRAVEL_EXECUTOR_H
