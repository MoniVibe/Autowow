/*
 * Bounded, value-only receipt transport for the AutoWow Oracle runtime.
 *
 * The world thread calls Publish() with an already materialized Oracle receipt. Publish() only
 * copies scalar/value evidence into fixed-capacity storage and signals the writer. The writer owns
 * JSON serialization and all filesystem access; no world object or string_view crosses that
 * boundary.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_RECEIPT_STORE_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_RECEIPT_STORE_H

#include "AutoWowOracleContract.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace AutoWowOracleReceiptStore
{
inline constexpr std::size_t kWriterQueueCapacity = 16'384;
inline constexpr std::size_t kQueryRingCapacity = 8'192;
inline constexpr std::size_t kMaxRetentionFiles = 8;
inline constexpr std::size_t kDefaultRotationBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxOracleLogBatch = 256;

struct Config
{
    bool enabled = true;
    std::string directory = "logs/autowow-oracle";
    std::size_t rotationBytes = kDefaultRotationBytes;
    std::size_t retentionFiles = kMaxRetentionFiles;
};

[[nodiscard]] bool ValidateConfig(Config const& config) noexcept;

// This is deliberately a value-only projection of AutoWowOracle::Receipt. In particular it has
// no std::string_view, Player, Map, WorldObject, or other world-owned pointer.
struct ReceiptRecord
{
    std::uint64_t sequence = 0;
    std::uint64_t worldTick = 0;
    std::uint64_t processId = 0;
    AutoWowOracle::Guid botGuid = 0;

    AutoWowOracle::ReceiptStatus status = AutoWowOracle::ReceiptStatus::Rejected;
    AutoWowOracle::ReceiptReason reason = AutoWowOracle::ReceiptReason::None;
    AutoWowOracle::DecisionId decisionId = 0;
    AutoWowOracle::IntentId intentId = 0;
    AutoWowOracle::Domain domain = AutoWowOracle::Domain::Combat;
    AutoWowOracle::LeaseResource resource = AutoWowOracle::LeaseResource::Idle;
    AutoWowOracle::Scope scope;
    AutoWowOracle::FactVersion frameVersion = 0;
    AutoWowOracle::Epoch epoch = 0;
    AutoWowOracle::EvidenceCounters before;
    AutoWowOracle::EvidenceCounters after;
    AutoWowOracle::StableId objectiveId = 0;
    AutoWowOracle::StableId rallyPointId = 0;
    AutoWowOracle::Guid actorGuid = 0;
    AutoWowOracle::Guid ownerGuid = 0;
    AutoWowOracle::Guid targetGuid = 0;
    AutoWowOracle::ItemId itemId = 0;
    AutoWowOracle::RoleCode role = AutoWowOracle::RoleCode::Unknown;
    AutoWowOracle::PlannerModeId plannerMode = AutoWowOracle::kPlannerModeUnknown;
    AutoWowOracle::PlannerReasonId plannerReason = AutoWowOracle::kPlannerReasonNone;
    bool executorAvailable = true;
    AutoWowOracle::OperationCode operation = AutoWowOracle::OperationCode::Unknown;
    AutoWowOracle::QuestReference quest;
    AutoWowOracle::GatherSourceReference gather;

    // queueAccepted is a world-thread fact. The writer never changes the record in the query
    // ring; durable_sequence is used with this flag to report durable/pending/inconclusive state.
    bool queueAccepted = false;
    bool durabilityInconclusive = false;
};

[[nodiscard]] ReceiptRecord CopyReceipt(AutoWowOracle::Receipt const& receipt,
                                        std::uint64_t worldTick,
                                        AutoWowOracle::Guid botGuid = 0) noexcept;

enum class CursorError : std::uint8_t
{
    None,
    StaleSession,
    StaleCursor,
    CursorAhead
};

[[nodiscard]] CursorError ValidateCursor(std::string_view requestedSession,
                                         std::string_view currentSession,
                                         std::uint64_t cursor,
                                         std::uint64_t oldestSequence,
                                         std::uint64_t latestSequence) noexcept;

[[nodiscard]] std::string SerializeReceiptJson(ReceiptRecord const& record,
                                                std::string_view sessionId,
                                                bool durable,
                                                bool evidenceInconclusive,
                                                std::string_view timestampUtc = {});

struct PublishResult
{
    std::uint64_t sequence = 0;
    bool queueAccepted = false;
};

struct StreamStatus
{
    std::string sessionId;
    std::uint64_t processId = 0;
    std::uint64_t sequence = 0;
    std::uint64_t durableSequence = 0;
    std::uint64_t dropCount = 0;
    std::size_t queueDepth = 0;
    std::size_t queueCapacity = kWriterQueueCapacity;
    std::size_t queryDepth = 0;
    std::size_t queryCapacity = kQueryRingCapacity;
    bool exportEnabled = false;
    bool writerFailed = false;
    bool evidenceInconclusive = false;
};

class Store final
{
public:
    Store();
    ~Store();

    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;

    // Start does not open files. It only copies bounded configuration and starts the writer; all
    // directory creation, file opening, serialization, rotation, and flushing happen there.
    void Start(Config const& config);
    void Stop();

    // World-thread producer API. It is non-blocking and performs no filesystem I/O.
    [[nodiscard]] PublishResult Publish(AutoWowOracle::Receipt const& receipt,
                                        std::uint64_t worldTick,
                                        AutoWowOracle::Guid botGuid = 0) noexcept;

    [[nodiscard]] StreamStatus Status() const;
    [[nodiscard]] std::string const& SessionId() const noexcept { return sessionId_; }
    [[nodiscard]] std::size_t QueryDepth() const noexcept { return querySize_; }
    [[nodiscard]] std::uint64_t QueryOldestSequence() const noexcept;
    [[nodiscard]] ReceiptRecord const* QueryAt(std::size_t oldestIndex) const noexcept;
    [[nodiscard]] bool IsSequenceDurable(ReceiptRecord const& record) const noexcept;

private:
    [[nodiscard]] bool TryPush(ReceiptRecord const& record) noexcept;
    [[nodiscard]] bool TryPop(ReceiptRecord& record) noexcept;
    void PushQuery(ReceiptRecord const& record) noexcept;
    void WriterLoop();

    Config config_;
    std::uint64_t processId_ = 0;
    std::string sessionId_;
    std::atomic_bool started_{false};
    std::atomic_bool stopRequested_{false};
    std::atomic<std::uint64_t> nextSequence_{0};
    std::atomic<std::uint64_t> durableSequence_{0};
    std::atomic<std::uint64_t> dropCount_{0};
    std::atomic_bool writerFailed_{false};
    std::atomic_bool evidenceInconclusive_{false};

    // Single producer (world thread) / single consumer (writer thread) bounded queue.
    std::array<ReceiptRecord, kWriterQueueCapacity> writerQueue_{};
    std::atomic<std::uint64_t> writeIndex_{0};
    std::atomic<std::uint64_t> readIndex_{0};
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
    std::thread writer_;

    // Read-only bridge queries are world-thread operations, so this ring is intentionally not
    // shared with the writer. It retains the latest bounded value-only records for oraclelog.
    std::array<ReceiptRecord, kQueryRingCapacity> queryRing_{};
    std::size_t queryOldest_ = 0;
    std::size_t querySize_ = 0;
};

}  // namespace AutoWowOracleReceiptStore

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_RECEIPT_STORE_H
