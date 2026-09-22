#include "AutoWowOracleReceiptStore.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace AutoWowOracleReceiptStore
{
namespace
{
std::atomic<std::uint64_t> sessionSerial{0};

std::uint64_t CurrentProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string MakeSessionId()
{
    std::uint64_t const timestamp = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uint64_t const serial = sessionSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    return "oracle-" + std::to_string(CurrentProcessId()) + "-" +
        std::to_string(timestamp) + "-" + std::to_string(serial);
}

std::string JsonString(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f]
                        << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

void AppendNullable(std::ostringstream& out, bool available, std::uint64_t value)
{
    if (available)
        out << value;
    else
        out << "null";
}

char const* QuestFamilyName(AutoWowOracle::QuestObjectiveFamily family)
{
    switch (family)
    {
        case AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject:
            return "npc_or_gameobject";
        case AutoWowOracle::QuestObjectiveFamily::Item:
            return "item";
        case AutoWowOracle::QuestObjectiveFamily::None:
            return "none";
    }
    return "unknown";
}

void AppendEvidence(std::ostringstream& out, AutoWowOracle::EvidenceCounters const& evidence)
{
    out << "{\"objective\":" << evidence.objective
        << ",\"progress\":" << evidence.progress
        << ",\"resource\":" << evidence.resource
        << ",\"combat\":" << evidence.combat
        << ",\"healing\":" << evidence.healing
        << ",\"deaths\":" << evidence.deaths
        << ",\"pvp\":" << evidence.pvp
        << ",\"failures\":" << evidence.failures << '}';
}

std::string UtcTimestamp()
{
    auto const now = std::chrono::system_clock::now();
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t const time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return out.str();
}

char const* DurabilityName(ReceiptRecord const& record, bool durable,
                           bool evidenceInconclusive)
{
    if (evidenceInconclusive || record.durabilityInconclusive)
        return "inconclusive";
    if (durable)
        return "durable";
    return record.queueAccepted ? "pending" : "not_configured";
}
}

bool ValidateConfig(Config const& config) noexcept
{
    return !config.enabled ||
        (!config.directory.empty() && config.rotationBytes != 0 &&
         config.retentionFiles > 0 && config.retentionFiles <= kMaxRetentionFiles);
}

ReceiptRecord CopyReceipt(AutoWowOracle::Receipt const& receipt,
                          std::uint64_t worldTick,
                          AutoWowOracle::Guid botGuid) noexcept
{
    ReceiptRecord record;
    record.worldTick = worldTick;
    record.botGuid = botGuid != 0 ? botGuid :
        receipt.scope.botGuid != 0 ? receipt.scope.botGuid : receipt.actorGuid;
    record.status = receipt.status;
    record.reason = receipt.reason;
    record.decisionId = receipt.decisionId;
    record.intentId = receipt.intentId;
    record.domain = receipt.domain;
    record.resource = receipt.resource;
    record.scope = receipt.scope;
    record.frameVersion = receipt.frameVersion;
    record.epoch = receipt.epoch;
    record.before = receipt.before;
    record.after = receipt.after;
    record.objectiveId = receipt.objectiveId;
    record.rallyPointId = receipt.rallyPointId;
    record.actorGuid = receipt.actorGuid;
    record.ownerGuid = receipt.ownerGuid;
    record.targetGuid = receipt.targetGuid;
    record.itemId = receipt.itemId;
    record.role = receipt.role;
    record.plannerMode = receipt.plannerMode;
    record.plannerReason = receipt.plannerReason;
    record.executorAvailable = receipt.executorAvailable;
    record.operation = receipt.operation;
    record.quest = receipt.quest;
    record.gather = receipt.gather;
    return record;
}

CursorError ValidateCursor(std::string_view requestedSession,
                           std::string_view currentSession,
                           std::uint64_t cursor,
                           std::uint64_t oldestSequence,
                           std::uint64_t latestSequence) noexcept
{
    if (!requestedSession.empty() && requestedSession != currentSession)
        return CursorError::StaleSession;
    if (cursor > latestSequence)
        return CursorError::CursorAhead;
    if (oldestSequence > 1 && cursor < oldestSequence - 1)
        return CursorError::StaleCursor;
    return CursorError::None;
}

std::string SerializeReceiptJson(ReceiptRecord const& record,
                                 std::string_view sessionId,
                                 bool durable,
                                 bool evidenceInconclusive,
                                 std::string_view timestampUtc)
{
    bool const hasQuest = record.quest.valid;
    bool const hasGather = record.gather.valid;
    bool const hasTarget = record.targetGuid != 0;
    bool const hasItem = record.itemId != 0;
    std::ostringstream out;
    out << "{\"schema\":1"
        << ",\"server_session_id\":" << JsonString(sessionId)
        << ",\"process_id\":";
    if (record.processId != 0)
        out << record.processId;
    else
        out << "null";
    out << ",\"sequence\":" << record.sequence
        << ",\"timestamp_utc\":";
    if (timestampUtc.empty())
        out << "null";
    else
        out << JsonString(timestampUtc);
    out << ",\"world_tick\":" << record.worldTick
        << ",\"bot_guid\":" << record.botGuid
        << ",\"controller_instance_id\":null"
        << ",\"job_id\":null"
        << ",\"lease_epoch\":";
    AppendNullable(out, record.epoch != 0, record.epoch);
    out << ",\"operation_id\":null"
        << ",\"decision_id\":" << record.decisionId
        << ",\"intent_id\":" << record.intentId
        << ",\"kind\":" << JsonString(AutoWowOracle::DomainName(record.domain))
        << ",\"event\":" << JsonString(AutoWowOracle::OperationCodeName(record.operation))
        << ",\"result\":" << JsonString(AutoWowOracle::ReceiptStatusName(record.status))
        << ",\"reason\":" << JsonString(AutoWowOracle::ReceiptReasonName(record.reason))
        << ",\"resource\":" << JsonString(AutoWowOracle::LeaseResourceName(record.resource))
        << ",\"subject\":{";
    out << "\"quest_id\":";
    AppendNullable(out, hasQuest, record.quest.questId);
    out << ",\"objective_index\":";
    AppendNullable(out, hasQuest, record.quest.objectiveSlot);
    out << ",\"objective_family\":";
    if (hasQuest)
        out << JsonString(QuestFamilyName(record.quest.objectiveFamily));
    else
        out << "null";
    out << ",\"target_identity\":";
    AppendNullable(out, hasTarget, record.targetGuid);
    out << ",\"item_id\":";
    AppendNullable(out, hasItem, record.itemId);
    out << ",\"source_spawn_id\":";
    AppendNullable(out, hasGather, record.gather.spawnId);
    out << ",\"source_entry\":";
    AppendNullable(out, hasGather, record.gather.entry);
    out << ",\"map_id\":";
    if (hasGather)
        out << record.gather.mapId;
    else
        out << "null";
    out << ",\"instance_id\":";
    AppendNullable(out, hasGather, record.gather.instanceId);
    out << "}"
        << ",\"route\":{\"segment\":null,\"path_status\":null"
        << ",\"distance_before\":null,\"distance_after\":null"
        << ",\"anchors_attempted\":null,\"target_loaded\":null"
        << ",\"teleport_used\":null}"
        << ",\"before\":";
    AppendEvidence(out, record.before);
    out << ",\"after\":";
    AppendEvidence(out, record.after);
    out << ",\"failure_code\":";
    if (record.reason == AutoWowOracle::ReceiptReason::None)
        out << "null";
    else
        out << JsonString(AutoWowOracle::ReceiptReasonName(record.reason));
    out << ",\"duration_us\":null"
        << ",\"next_retry_utc\":null"
        << ",\"build\":null"
        << ",\"frame_version\":" << record.frameVersion
        << ",\"epoch\":" << record.epoch
        << ",\"durability\":"
        << JsonString(DurabilityName(record, durable, evidenceInconclusive))
        << ",\"durability_inconclusive\":"
        << ((evidenceInconclusive || record.durabilityInconclusive) ? "true" : "false")
        << ",\"completion_claimable\":"
        << ((record.status == AutoWowOracle::ReceiptStatus::Completed && durable &&
             !evidenceInconclusive && !record.durabilityInconclusive) ? "true" : "false")
        << '}';
    return out.str();
}

Store::Store() : processId_(CurrentProcessId()), sessionId_(MakeSessionId())
{
}

Store::~Store()
{
    Stop();
}

void Store::Start(Config const& config)
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true))
        return;

    config_ = config;
    if (!ValidateConfig(config_))
    {
        config_ = {};
        config_.enabled = false;
        evidenceInconclusive_.store(true, std::memory_order_release);
        return;
    }

    stopRequested_.store(false, std::memory_order_release);
    if (config_.enabled)
        writer_ = std::thread(&Store::WriterLoop, this);
}

void Store::Stop()
{
    if (!started_.load(std::memory_order_acquire))
        return;

    stopRequested_.store(true, std::memory_order_release);
    waitCondition_.notify_one();
    if (writer_.joinable())
        writer_.join();
    started_.store(false, std::memory_order_release);
}

bool Store::TryPush(ReceiptRecord const& record) noexcept
{
    std::uint64_t const write = writeIndex_.load(std::memory_order_relaxed);
    std::uint64_t const read = readIndex_.load(std::memory_order_acquire);
    if (write - read >= kWriterQueueCapacity)
        return false;

    writerQueue_[write % kWriterQueueCapacity] = record;
    writeIndex_.store(write + 1, std::memory_order_release);
    return true;
}

bool Store::TryPop(ReceiptRecord& record) noexcept
{
    std::uint64_t const read = readIndex_.load(std::memory_order_relaxed);
    std::uint64_t const write = writeIndex_.load(std::memory_order_acquire);
    if (read == write)
        return false;

    record = writerQueue_[read % kWriterQueueCapacity];
    readIndex_.store(read + 1, std::memory_order_release);
    return true;
}

void Store::PushQuery(ReceiptRecord const& record) noexcept
{
    std::size_t const slot = (queryOldest_ + querySize_) % queryRing_.size();
    queryRing_[slot] = record;
    if (querySize_ < queryRing_.size())
    {
        ++querySize_;
        return;
    }
    queryOldest_ = (queryOldest_ + 1) % queryRing_.size();
}

PublishResult Store::Publish(AutoWowOracle::Receipt const& receipt,
                             std::uint64_t worldTick,
                             AutoWowOracle::Guid botGuid) noexcept
{
    ReceiptRecord record = CopyReceipt(receipt, worldTick, botGuid);
    record.sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    record.processId = processId_;

    bool const exportReady = started_.load(std::memory_order_acquire) && config_.enabled &&
        !writerFailed_.load(std::memory_order_acquire);
    if (exportReady && TryPush(record))
    {
        record.queueAccepted = true;
        waitCondition_.notify_one();
    }
    else
    {
        if (config_.enabled && started_.load(std::memory_order_acquire))
            dropCount_.fetch_add(1, std::memory_order_relaxed);
        record.durabilityInconclusive = true;
        evidenceInconclusive_.store(true, std::memory_order_release);
    }

    PushQuery(record);
    return {record.sequence, record.queueAccepted};
}

StreamStatus Store::Status() const
{
    StreamStatus status;
    status.sessionId = sessionId_;
    status.processId = processId_;
    status.sequence = nextSequence_.load(std::memory_order_acquire);
    status.durableSequence = durableSequence_.load(std::memory_order_acquire);
    status.dropCount = dropCount_.load(std::memory_order_acquire);
    std::uint64_t const write = writeIndex_.load(std::memory_order_acquire);
    std::uint64_t const read = readIndex_.load(std::memory_order_acquire);
    status.queueDepth = static_cast<std::size_t>(write >= read ? write - read : 0);
    if (status.queueDepth > status.queueCapacity)
        status.queueDepth = status.queueCapacity;
    status.queryDepth = querySize_;
    status.exportEnabled = started_.load(std::memory_order_acquire) && config_.enabled;
    status.writerFailed = writerFailed_.load(std::memory_order_acquire);
    status.evidenceInconclusive = evidenceInconclusive_.load(std::memory_order_acquire) ||
        status.dropCount != 0 || status.writerFailed;
    return status;
}

std::uint64_t Store::QueryOldestSequence() const noexcept
{
    if (querySize_ == 0)
        return 0;
    return queryRing_[queryOldest_].sequence;
}

ReceiptRecord const* Store::QueryAt(std::size_t oldestIndex) const noexcept
{
    if (oldestIndex >= querySize_)
        return nullptr;
    return &queryRing_[(queryOldest_ + oldestIndex) % queryRing_.size()];
}

bool Store::IsSequenceDurable(ReceiptRecord const& record) const noexcept
{
    return record.queueAccepted &&
        durableSequence_.load(std::memory_order_acquire) >= record.sequence;
}

void Store::WriterLoop()
{
    std::filesystem::path const directory(config_.directory);
    std::ofstream file;
    std::size_t fileBytes = 0;
    std::size_t fileIndex = 0;

    auto openFile = [&]() -> bool
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return false;
        std::filesystem::path const path = directory /
            ("oracle-receipts-" + sessionId_ + "-" + std::to_string(fileIndex) + ".jsonl");
        file.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
        fileBytes = 0;
        return file.is_open() && file.good();
    };

    for (;;)
    {
        ReceiptRecord record;
        if (!TryPop(record))
        {
            std::uint64_t const read = readIndex_.load(std::memory_order_acquire);
            std::uint64_t const write = writeIndex_.load(std::memory_order_acquire);
            if (stopRequested_.load(std::memory_order_acquire) && read == write)
                break;

            std::unique_lock<std::mutex> lock(waitMutex_);
            waitCondition_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        if (writerFailed_.load(std::memory_order_acquire))
        {
            dropCount_.fetch_add(1, std::memory_order_relaxed);
            evidenceInconclusive_.store(true, std::memory_order_release);
            continue;
        }

        if (!file.is_open() && !openFile())
        {
            writerFailed_.store(true, std::memory_order_release);
            evidenceInconclusive_.store(true, std::memory_order_release);
            dropCount_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::string const line = SerializeReceiptJson(
            record, sessionId_, true, evidenceInconclusive_.load(std::memory_order_acquire),
            UtcTimestamp());
        std::size_t const bytes = line.size() + 1;
        if (fileBytes != 0 && fileBytes + bytes > config_.rotationBytes)
        {
            file.close();
            fileIndex = (fileIndex + 1) % config_.retentionFiles;
            if (!openFile())
            {
                writerFailed_.store(true, std::memory_order_release);
                evidenceInconclusive_.store(true, std::memory_order_release);
                dropCount_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }

        file << line << '\n';
        file.flush();
        if (!file.good())
        {
            writerFailed_.store(true, std::memory_order_release);
            evidenceInconclusive_.store(true, std::memory_order_release);
            dropCount_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        fileBytes += bytes;
        durableSequence_.store(record.sequence, std::memory_order_release);
    }

    if (file.is_open())
    {
        file.flush();
        file.close();
    }
}

}  // namespace AutoWowOracleReceiptStore
