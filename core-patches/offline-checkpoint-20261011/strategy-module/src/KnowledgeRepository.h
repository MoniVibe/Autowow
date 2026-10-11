/* AutoWoW original asynchronous knowledge repository. GPL v2 or later. */
#ifndef AUTOWOW_KNOWLEDGE_REPOSITORY_H
#define AUTOWOW_KNOWLEDGE_REPOSITORY_H

#include "KnowledgeSettlementPolicy.h"

#include <functional>
#include <memory>
#include <string_view>

namespace AutoWowStrategy
{
// Uninstantiated by StrategyScripts. All methods and callbacks belong to one owner thread.
// Initialize/Close are blocking infrastructure lifecycle operations, never gameplay/map callbacks.
// Future owners must drain Poll before shutdown; Failed can mean an unacknowledged commit, so retry the same terms.
class KnowledgeRepository
{
public:
    using SettlementHandler = std::function<void(KnowledgeSettlement)>;
    using LookupHandler = std::function<void(bool present, bool querySucceeded)>;

    KnowledgeRepository();
    ~KnowledgeRepository();
    KnowledgeRepository(KnowledgeRepository const&) = delete;
    KnowledgeRepository& operator=(KnowledgeRepository const&) = delete;

    // No implicit migration. Both isolated schema and checked BEGIN/COMMIT semantics need independent admission.
    // The private checked adapter needs isolated native fault/lifecycle qualification; keep admission false.
    bool Initialize(std::string_view charactersConnectionInfo, std::uint8_t asyncThreads,
        std::uint8_t syncThreads, bool schemaAdmitted, bool transactionApiAdmitted = false);
    void SettleKnowledgeAsync(KnowledgeIntent intent, SettlementHandler callback);
    void HasKnowledgeAsync(KnowledgeScope scope, std::uint64_t scopeId, std::uint32_t knowledgeId,
        LookupHandler callback);
    void Poll();
    void Close();

    // External handlers are attempted once; exceptions are contained, never replayed or reclassified.
    // Owner-thread diagnostic is monotonic across Close/Initialize and saturates instead of wrapping.
    std::uint64_t HandlerFailureCount() const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
} // namespace AutoWowStrategy

#endif
