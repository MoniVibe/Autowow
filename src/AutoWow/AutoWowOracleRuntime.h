/*
 * World-thread-only runtime bridge for the bounded AutoWow Oracle quest executor.
 *
 * The runtime is deliberately default-off.  Its public policy helpers are allocation-free and
 * bounded so they can be tested without a worldserver or a live Playerbot.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_RUNTIME_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_RUNTIME_H

#include "AutoWowOracleContract.h"
#include "AutoWowOracleQuestSelectionPolicy.h"
#include "AutoWowOracleReceiptStore.h"
#include "OracleGatherExecutor.h"
#include "OracleQuestExecutor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// These quest-runtime types are defined by QuestObjectiveContext.h in the global namespace.
// Keep the heavy context header out of this public policy header: including it here introduces
// unrelated namespace collisions for consumers that only need the Oracle runtime contract.
struct QuestObjectiveSpec;
enum class QuestActionPhase : std::uint8_t;

namespace AutoWowOracleRuntime
{
using Guid = AutoWowOracle::Guid;
using Tick = AutoWowOracle::Tick;
using DecisionId = AutoWowOracle::DecisionId;
using IntentId = AutoWowOracle::IntentId;
using Receipt = AutoWowOracle::Receipt;
using IntentLease = AutoWowOracle::IntentLease;

inline constexpr std::size_t kMaxRuntimeBots = AutoWowOracle::kMaxBotLeases;
static_assert(kMaxRuntimeBots <= AutoWowOracle::kMaxBotLeases);
inline constexpr std::size_t kReceiptRingCapacity = 64;
inline constexpr std::uint32_t kMinCadenceMs = 100;
inline constexpr std::uint32_t kMaxCadenceMs = 5000;
inline constexpr std::uint32_t kMinLeaseTtlTicks = 1;
inline constexpr std::uint32_t kMaxLeaseTtlTicks = 64;

struct QuestBootstrapIdentity
{
    bool valid = false;
    bool bootstrap = false;
    Guid targetGuid = 0;
    std::uint32_t mapId = 0;
    std::uint32_t sourceEntry = 0;
    bool sourceIsGameObject = false;
};

// A resolver dispatch starts before the native quest action has a live target GUID. Use one exact,
// same-map database spawn as the temporary identity for that bounded handoff. The selection is
// independent of vector order, and the helper returns no identity when the spec has no usable
// same-map spawn. A pre-existing live target always wins and is never replaced by this bootstrap.
[[nodiscard]] QuestBootstrapIdentity ResolveQuestBootstrapIdentity(
    ::QuestObjectiveSpec const& spec, ::QuestActionPhase phase, std::uint32_t botMapId,
    Guid selectedTargetGuid) noexcept;

struct RuntimeConfig
{
    bool enabled = false;
    bool zoneTravelAssist = false;
    std::uint32_t cadenceMs = 1000;
    std::uint32_t maxBots = 1;
    std::uint32_t leaseTtlTicks = 3;
    // 0 = drive every managed bot on the cadence edge (legacy). N > 0 = round-robin slices of at
    // most N bots per world update; see PlanSliceStep.
    std::uint32_t sliceBots = 0;
    // 0 = legacy (a directive that yields no leasable candidate is kept forever). N > 0 = defer it
    // after N consecutive lease-less cadence passes; see ShouldDeferNoCandidateQuest.
    std::uint32_t deferNoCandidatePasses = 0;
    std::array<Guid, kMaxRuntimeBots> botGuids{};
    std::size_t botGuidCount = 0;
};

// The identity that is allowed to survive an arbiter renewal.  This is deliberately richer
// than Decision::targetGuid: the native quest state machine can change phase or replace a
// finisher while the same bot/quest remains selected.  Reusing the old decision for any such
// change would let a stale tagged event operate on a different world object.
struct IntentIdentity
{
    bool valid = false;
    bool finisherMode = false;
    bool finisherIsGameObject = false;
    std::uint32_t questId = 0;
    std::uint8_t phase = 0;
    std::uint8_t objectiveFamily = 0;
    std::uint8_t objectiveSlot = 0;
    std::uint32_t requiredEntry = 0;
    std::uint32_t requiredItemId = 0;
    Guid targetGuid = 0;
    std::int32_t finisherSignedEntry = 0;
    Guid finisherStableSpawnGuid = 0;
};

[[nodiscard]] inline constexpr bool SameIntentIdentity(IntentIdentity const& left,
                                                        IntentIdentity const& right) noexcept
{
    return left.valid && right.valid && left.finisherMode == right.finisherMode &&
           left.finisherIsGameObject == right.finisherIsGameObject && left.questId == right.questId &&
           left.phase == right.phase && left.objectiveFamily == right.objectiveFamily &&
           left.objectiveSlot == right.objectiveSlot && left.requiredEntry == right.requiredEntry &&
           left.requiredItemId == right.requiredItemId && left.targetGuid == right.targetGuid &&
           left.finisherSignedEntry == right.finisherSignedEntry &&
           left.finisherStableSpawnGuid == right.finisherStableSpawnGuid;
}

// Strict decimal allowlist parser. Whitespace and commas are separators; signs, hex notation,
// zero, empty comma fields, overflow, and distinct-capacity overflow are rejected. Duplicate
// entries are normalized away and the committed output is sorted ascending.
bool ParseGuidAllowlist(std::string_view text,
                        std::array<Guid, kMaxRuntimeBots>& out,
                        std::size_t& outCount) noexcept;

bool IsGuidAllowed(std::array<Guid, kMaxRuntimeBots> const& guids,
                   std::size_t count, Guid guid) noexcept;

// The runtime consumes at most one cadence interval per call.  A large diff causes one due tick,
// never a catch-up loop.
struct CadenceClock
{
    std::uint32_t elapsedMs = 0;
};

bool AdvanceCadence(CadenceClock& clock, std::uint32_t diffMs,
                    std::uint32_t cadenceMs) noexcept;

// Round-robin slice scheduler (AutoWow.OracleRuntime.SliceBots > 0). One pass covers the
// eligible bots of one cadence tick in ascending-guid order. Every world update advances the
// pass cursor by at most sliceBots. A cadence edge first drains the unfinished previous pass
// (still under the old tick), then starts a new pass under the new tick, so each bot runs
// exactly once per cadence tick: no starvation, no double run. Lease TTLs stay in cadence ticks.
struct SlicePass
{
    std::size_t cursor = 0;
    std::size_t count = 0;
};

struct SliceStep
{
    std::size_t drainBegin = 0; // [drainBegin, drainEnd) of the previous pass, old tick
    std::size_t drainEnd = 0;
    bool newTick = false;       // advance the cadence tick after the drain
    std::size_t begin = 0;      // [begin, end) of the current pass, current tick
    std::size_t end = 0;
};

[[nodiscard]] SliceStep PlanSliceStep(SlicePass& pass, bool cadenceEdge, std::size_t eligibleCount,
                                      std::uint32_t sliceBots) noexcept;

bool ValidateConfig(RuntimeConfig const& config) noexcept;

struct BotState
{
    bool occupied = false;
    Guid guid = 0;
    IntentLease lease;
    IntentIdentity leaseIdentity;
    Tick lastNoEligibleTelemetryTick = 0;
    Guid blockedQuestId = 0;
    Tick blockedQuestRetryTick = 0;
    std::uint8_t blockedQuestRetries = 0;
    struct ObjectiveProgress
    {
        bool known = false;
        std::uint32_t questId = 0;
        std::uint8_t family = 0;
        std::uint8_t slot = 0;
        std::uint32_t count = 0;
    } blockedObjective;
    NoCandidateStreak noCandidate;
};

[[nodiscard]] inline bool KeepBlockedQuestRetry(
    Guid blockedQuestId, Guid currentQuestId, bool questComplete,
    BotState::ObjectiveProgress const& blocked,
    BotState::ObjectiveProgress const& current) noexcept
{
    if (blockedQuestId == 0 || blockedQuestId != currentQuestId || questComplete)
        return false;
    if (blocked.known && current.known &&
        (blocked.questId != current.questId || blocked.family != current.family ||
         blocked.slot != current.slot || current.count > blocked.count))
        return false;
    return true;
}

// Persistent, fixed-capacity per-bot state.  No map or unbounded container is used here.
class BoundedBotState final
{
public:
    BotState* Find(Guid guid) noexcept;
    BotState const* Find(Guid guid) const noexcept;
    BotState* FindOrCreate(Guid guid) noexcept;
    bool Erase(Guid guid) noexcept;
    void Clear() noexcept;

    std::size_t size() const noexcept { return size_; }
    std::size_t activeLeaseCount() const noexcept;

private:
    std::array<BotState, kMaxRuntimeBots> slots_{};
    std::size_t size_ = 0;
};

// Fixed-capacity oldest-to-newest receipt history.
class ReceiptRing final
{
public:
    void Push(Receipt const& receipt) noexcept;
    Receipt const* At(std::size_t oldestIndex) const noexcept;
    void Clear() noexcept;

    std::size_t size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return kReceiptRingCapacity; }

private:
    std::array<Receipt, kReceiptRingCapacity> entries_{};
    std::size_t oldest_ = 0;
    std::size_t size_ = 0;
};

// World-thread-owned, value-only Oracle state published by the runtime. A false availability flag
// is intentional: the bridge must emit null for a strategic field that the runtime has not
// actually observed rather than reconstructing it from generic Playerbot activity.
struct OracleBotSnapshot
{
    bool occupied = false;
    bool managed = false;
    bool published = false;
    Guid guid = 0;
    Tick worldTick = 0;

    bool phaseAvailable = false;
    std::uint8_t phase = 0;
    bool failureAvailable = false;
    std::uint16_t failure = 0;
    bool questIdAvailable = false;
    std::uint32_t questId = 0;
    bool objectiveIndexAvailable = false;
    std::uint32_t objectiveIndex = 0;
    bool targetIdentityAvailable = false;
    Guid targetIdentity = 0;

    bool operationAvailable = false;
    AutoWowOracle::OperationCode operation = AutoWowOracle::OperationCode::Unknown;
    bool leaseAvailable = false;
    DecisionId decisionId = 0;
    IntentId intentId = 0;
    bool leaseEpochAvailable = false;
    AutoWowOracle::Epoch leaseEpoch = 0;
    bool leaseExpiresTickAvailable = false;
    Tick leaseExpiresTick = 0;

    std::uint64_t lastReceiptSequence = 0;
    std::uint64_t completionReceiptSequence = 0;
    bool completionDurabilityEligible = false;
    bool completionClaimable = false;
    bool evidenceInconclusive = false;
};

inline void ObserveCompletionReceipt(OracleBotSnapshot& snapshot,
                                     AutoWowOracle::ReceiptStatus status,
                                     std::uint64_t sequence,
                                     bool durabilityEligible) noexcept
{
    if (status != AutoWowOracle::ReceiptStatus::Completed ||
        sequence <= snapshot.completionReceiptSequence)
        return;

    snapshot.completionReceiptSequence = sequence;
    snapshot.completionDurabilityEligible = durabilityEligible;
    snapshot.completionClaimable = false;
}

[[nodiscard]] inline bool IsCompletionClaimable(OracleBotSnapshot const& snapshot,
                                                 std::uint64_t durableSequence,
                                                 bool evidenceInconclusive) noexcept
{
    return snapshot.completionReceiptSequence != 0 &&
        snapshot.completionDurabilityEligible &&
        durableSequence >= snapshot.completionReceiptSequence &&
        !evidenceInconclusive;
}

class Runtime final
{
public:
    static Runtime& instance();

    // Called from PlayerbotsWorldScript::OnUpdate, after the world-thread operation processor.
    void Update(std::uint32_t diffMs);

    bool IsEnabled() const noexcept { return config_.enabled; }
    RuntimeConfig const& Config() const noexcept { return config_; }
    std::size_t ActiveLeaseCount() const noexcept { return arbiter_.ActiveBotLeaseCount(); }
    std::size_t ReceiptCount() const noexcept { return receipts_.size(); }

    // The state lease is the fail-closed authority used by NewRpgDoQuestAction.  It remains true
    // even if the short-lived native gate expires between runtime cadences, so an untagged action
    // cannot run while the arbiter still owns the bot.  HasActiveLease additionally checks the
    // arbiter's logical expiry and exact decision id.
    bool RequiresTaggedDispatch(Guid botGuid) const noexcept;
    bool HasActiveLease(Guid botGuid, DecisionId decisionId = 0) const noexcept;
    bool IsManagedBot(Guid botGuid);
    DecisionId ActiveDecisionId(Guid botGuid) const noexcept;
    bool GetOracleSnapshot(Guid botGuid, OracleBotSnapshot& out);
    AutoWowOracleReceiptStore::StreamStatus GetOracleStreamStatus();
    AutoWowOracleReceiptStore::Store const& ReceiptStore() const noexcept { return receiptStore_; }

private:
    struct LiveQuestFrame;
    struct LiveGatherFrame;
    struct NativeDispatchContext;

    Runtime() = default;

    void LoadConfig();
    void ProcessConfiguredBot(Guid botGuid);
    void UpdateSliced(std::uint32_t diffMs);
    bool EnsureQuestDirective(Guid botGuid, BotState& state);
    bool BuildLiveQuestFrame(Guid botGuid, Tick tick, AutoWowOracle::FactVersion version,
                             AutoWowOracle::Epoch epoch,
                             AutoWowOracle::ActiveLeaseSnapshot const& ownership,
                             LiveQuestFrame& out) const;
    bool BuildLiveGatherFrame(Guid botGuid, Tick tick, AutoWowOracle::FactVersion version,
                              AutoWowOracle::Epoch epoch,
                              AutoWowOracle::ActiveLeaseSnapshot const& ownership,
                              LiveGatherFrame& out) const;
    void ProcessConfiguredGatherBot(Guid botGuid);
    void PublishQuestOracleSnapshot(Guid botGuid, LiveQuestFrame const& live) noexcept;
    void PublishGatherOracleSnapshot(Guid botGuid, LiveGatherFrame const& live) noexcept;
    void InvalidateOracleSnapshot(Guid botGuid) noexcept;
    OracleBotSnapshot* FindOracleSnapshot(Guid botGuid) noexcept;
    bool RefreshOracleOwnership(Guid botGuid, IntentLease const& lease) const noexcept;
    AutoWowOracle::WorldReadFrame MakeCleanupFrame(Guid botGuid,
                                                   IntentLease const& lease) const;
    void Record(Receipt const& receipt) noexcept;
    void ReleaseLease(BotState& state, AutoWowOracle::WorldReadFrame const& frame);
    Receipt MakeExecutorReceipt(AutoWowOracle::Decision const& decision,
                                AutoWowOracleQuestExecutor::DispatchResult const& result) const noexcept;
    Receipt MakeGatherExecutorReceipt(
        AutoWowOracle::Decision const& decision,
        AutoWowOracleGatherExecutor::DispatchResult const& result) const noexcept;

    static AutoWowOracleQuestExecutor::NativeStepObservation NativeDispatch(
        void* context, AutoWowOracleQuestExecutor::NativeObjectiveStepRequest const& request) noexcept;

    RuntimeConfig config_;
    bool configLoaded_ = false;
    bool warnedMissingGate_ = false;
    CadenceClock cadence_;
    SlicePass slicePass_;
    std::array<Guid, kMaxRuntimeBots> sliceGuids_{};
    Tick tick_ = 0;
    AutoWowOracle::FactVersion frameVersion_ = 0;
    AutoWowOracle::Epoch epoch_ = 1;

    AutoWowOracle::OracleArbiter<kMaxRuntimeBots> arbiter_;
    BoundedBotState state_;
    ReceiptRing receipts_;
    std::array<OracleBotSnapshot, kMaxRuntimeBots> oracleSnapshots_{};
    AutoWowOracleReceiptStore::Store receiptStore_;
};

void Update(std::uint32_t diffMs);

bool RequiresTaggedDispatch(Guid botGuid) noexcept;
bool HasActiveLease(Guid botGuid, DecisionId decisionId = 0) noexcept;
bool IsManagedBot(Guid botGuid);
// AutoWow.OracleRuntime.NoRandomTeleport (default 0, read once): true only for an Oracle-managed bot
// while the flag is on. With the flag off it never touches the runtime.
bool BlocksRandomTeleport(Guid botGuid);
DecisionId ActiveDecisionId(Guid botGuid) noexcept;
bool GetOracleSnapshot(Guid botGuid, OracleBotSnapshot& out);
AutoWowOracleReceiptStore::StreamStatus GetOracleStreamStatus();

}  // namespace AutoWowOracleRuntime

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_RUNTIME_H
