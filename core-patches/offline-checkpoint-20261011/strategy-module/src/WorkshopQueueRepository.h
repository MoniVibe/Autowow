/* AutoWoW original durable draft workshop queue. GPL v2 or later. */
#ifndef AUTOWOW_WORKSHOP_QUEUE_REPOSITORY_H
#define AUTOWOW_WORKSHOP_QUEUE_REPOSITORY_H

#include "WorkshopPolicy.h"
#include <functional>
#include <memory>
#include <string_view>

namespace AutoWowStrategy
{
// Immutable draft only: no funding, resource reservation, elapsed work, cast or output authorization.
// The trusted future demand adapter supplies durable unique order identity and validated native profile terms.
struct WorkshopDraft
{
    WorkshopOrder order;
    WorkshopProfile profile;
    std::uint64_t recipientGuid = 0;
    std::uint32_t outputItemEntry = 0;
};

enum class WorkshopDraftResult : std::uint8_t { Refused, Failed, Recorded, Conflict };
struct WorkshopDraftView
{
    bool querySucceeded = false;
    bool present = false;
    WorkshopDraft draft{};
};

// Same owner-thread/infrastructure lifecycle contract as KnowledgeRepository. No active script instantiates it.
// Failed does not prove absence. Explicit retries must preserve the entire immutable draft, not issue a new ID.
// Recorded includes exact existing drafts; it is never authority to reserve/charge/craft/reward again.
class WorkshopQueueRepository
{
public:
    using DraftHandler = std::function<void(WorkshopDraftResult)>;
    using LookupHandler = std::function<void(WorkshopDraftView)>;
    WorkshopQueueRepository();
    ~WorkshopQueueRepository();
    WorkshopQueueRepository(WorkshopQueueRepository const&) = delete;
    WorkshopQueueRepository& operator=(WorkshopQueueRepository const&) = delete;
    bool Initialize(std::string_view connectionInfo, std::uint8_t asyncThreads, std::uint8_t syncThreads,
        bool schemaAdmitted, bool transactionApiAdmitted = false);
    void SubmitDraftAsync(WorkshopDraft draft, DraftHandler callback);
    void ReadDraftAsync(std::uint64_t orderId, LookupHandler callback);
    void Poll();
    void Close();
    std::uint64_t HandlerFailureCount() const;
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
} // namespace AutoWowStrategy
#endif
