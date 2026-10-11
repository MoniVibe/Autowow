/* AutoWoW original asynchronous knowledge repository. GPL v2 or later. */
#include "KnowledgeRepository.h"
#include "CheckedTransactionPolicy.h"
#include "CheckedAsyncOperation.h"
#include "OwnedConnectionInfo.h"

#include "Field.h"
#include "ModuleDatabasePool.h"
#include "MySQLPreparedStatement.h"
#include "MySQLHacks.h"
#include "PCQueue.h"
#include "SQLOperation.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "Transaction.h"

#include <array>
#include <map>
#include <utility>
#include <vector>

namespace AutoWowStrategy
{
namespace
{
enum KnowledgeStatement : std::uint32_t
{
    ClaimReceipt,
    InsertKnowledge,
    SettleReceipt,
    ReadReceipt,
    ReadKnowledge,
    StatementCount
};

class KnowledgeConnection;

class KnowledgeTransaction final : public Transaction<KnowledgeConnection>
{
public:
    std::vector<SQLElementData> const& Queries() const { return m_queries; }
};

class KnowledgeConnection final : private OwnedConnectionInfo, public MySQLConnection
{
public:
    explicit KnowledgeConnection(MySQLConnectionInfo& info)
        : OwnedConnectionInfo(info), MySQLConnection(OwnedInfo()) { }
    KnowledgeConnection(ProducerConsumerQueue<SQLOperation*>* queue, MySQLConnectionInfo& info)
        : OwnedConnectionInfo(info), MySQLConnection(queue, OwnedInfo()) { }

    uint32 Open() override
    {
        uint32 const error = MySQLConnection::Open();
        if (error)
            return error;
        MySQLBool reconnect = false;
        if (mysql_options(m_Mysql, MYSQL_OPT_RECONNECT, &reconnect))
        {
            QuarantineSession();
            return 1;
        }
        return 0;
    }

    unsigned long SessionId() const { return m_Mysql ? mysql_thread_id(m_Mysql) : 0; }

    bool CommitChecked(KnowledgeTransaction const& transaction)
    {
        // This repository constructs exactly three prepared statements before handing off ownership.
        if (transaction.GetSize() != 3 || !m_Mysql)
            return false;

        struct Backend
        {
            KnowledgeConnection& connection;
            KnowledgeTransaction const& transaction;
            unsigned long session = 0;
            bool SameSession() const { return session && session == connection.SessionId(); }
            bool Begin()
            {
                session = connection.SessionId();
                return SameSession() && connection.Execute("START TRANSACTION") && SameSession();
            }
            bool Step(std::size_t index)
            {
                auto const& data = transaction.Queries()[index];
                auto statement = std::get_if<PreparedStatementBase*>(&data.element);
                return SameSession() && data.type == SQL_ELEMENT_PREPARED && statement && *statement &&
                    connection.Execute(*statement) && SameSession();
            }
            bool Commit() { return SameSession() && connection.Execute("COMMIT") && SameSession(); }
            bool Rollback() { return SameSession() && connection.Execute("ROLLBACK") && SameSession(); }
            void Quarantine() { connection.QuarantineSession(); }
        } backend{*this, transaction};
        return ExecuteCheckedTransaction(transaction.GetSize(), backend);
    }

    void QuarantineSession()
    {
        // Execute has returned before this call: no prepared-statement stack pointer is invalidated in use.
        // Never call core Close from its own worker: that would try to join the worker thread itself.
        m_stmts.clear();
        if (m_Mysql)
        {
            mysql_close(m_Mysql);
            m_Mysql = nullptr;
        }
        // Core queries now return null. Re-admission requires an owner-thread Close/Initialize, never replay.
    }

private:
    bool _HandleMySQLErrno(uint32, char const*, uint8) override
    {
        // This private pool must not reconnect and replay one statement across a transaction/session boundary.
        // Read failures likewise return unknown; the core's reconnect/abort policy remains unchanged elsewhere.
        return false;
    }

    void DoPrepareStatements() override
    {
        if (!m_reconnecting)
            m_stmts.resize(StatementCount);

        PrepareStatement(ClaimReceipt,
            "INSERT INTO `autowow_knowledge_receipts` "
            "(`source_kind`,`source_family_id`,`event_id`,`contributor_guid`,`knowledge_id`,"
            "`scope_kind`,`scope_id`,`outcome`) "
            "VALUES (?,?,?,?,?,?,?,0) "
            "ON DUPLICATE KEY UPDATE `event_id`=`autowow_knowledge_receipts`.`event_id`", CONNECTION_ASYNC);
        PrepareStatement(InsertKnowledge,
            "INSERT INTO `autowow_knowledge` "
            "(`scope_kind`,`scope_id`,`knowledge_id`,`source_kind`,`source_family_id`,"
            "`original_event_id`,`contributor_guid`) "
            "SELECT r.`scope_kind`,r.`scope_id`,r.`knowledge_id`,r.`source_kind`,r.`source_family_id`,r.`event_id`,"
            "r.`contributor_guid` FROM `autowow_knowledge_receipts` r "
            "WHERE r.`source_kind`=? AND r.`source_family_id`=? AND r.`event_id`=? AND r.`contributor_guid`=? "
            "AND r.`knowledge_id`=? AND r.`scope_kind`=? AND r.`scope_id`=? "
            "ON DUPLICATE KEY UPDATE `original_event_id`=`autowow_knowledge`.`original_event_id`", CONNECTION_ASYNC);
        PrepareStatement(SettleReceipt,
            "UPDATE `autowow_knowledge_receipts` r JOIN `autowow_knowledge` k "
            "ON k.`scope_kind`=r.`scope_kind` AND k.`scope_id`=r.`scope_id` AND k.`knowledge_id`=r.`knowledge_id` "
            "SET r.`outcome`=IF(k.`source_kind`=r.`source_kind` AND k.`source_family_id`=r.`source_family_id` "
            "AND k.`original_event_id`=r.`event_id` AND k.`contributor_guid`=r.`contributor_guid`,1,2) "
            "WHERE r.`source_kind`=? AND r.`source_family_id`=? AND r.`event_id`=? AND r.`contributor_guid`=? "
            "AND r.`knowledge_id`=? AND r.`scope_kind`=? AND r.`scope_id`=?", CONNECTION_ASYNC);
        // Aggregate reads distinguish a successful miss from the native null-on-query-failure result.
        PrepareStatement(ReadReceipt,
            "SELECT COUNT(*),MAX(r.`scope_kind`),MAX(r.`scope_id`),"
            "MAX(r.`outcome`),MAX(k.`original_event_id`),"
            "MAX(k.`contributor_guid`),MAX(k.`source_kind`),"
            "MAX(k.`source_family_id`) FROM `autowow_knowledge_receipts` r "
            "LEFT JOIN `autowow_knowledge` k ON k.`scope_kind`=r.`scope_kind` AND k.`scope_id`=r.`scope_id` "
            "AND k.`knowledge_id`=r.`knowledge_id` WHERE r.`source_kind`=? AND r.`source_family_id`=? "
            "AND r.`event_id`=? AND r.`contributor_guid`=? AND r.`knowledge_id`=?", CONNECTION_ASYNC);
        PrepareStatement(ReadKnowledge,
            "SELECT COUNT(*) FROM `autowow_knowledge` WHERE `scope_kind`=? AND `scope_id`=? AND `knowledge_id`=?",
            CONNECTION_ASYNC);
    }
};

class KnowledgePool final : public ModuleDatabasePool
{
public:
    // Quarantined sessions may have no MYSQL handle; this pool never queues native Ping operations.
    void KeepAlive() = delete;

    TransactionCallback AsyncCommitChecked(std::shared_ptr<KnowledgeTransaction> transaction)
    {
        auto task = std::make_unique<CheckedAsyncOperation>(
            [transaction = std::move(transaction)](MySQLConnection* native)
            {
                auto connection = dynamic_cast<KnowledgeConnection*>(native);
                return connection && connection->CommitChecked(*transaction);
            },
            [](MySQLConnection* native)
            {
                if (auto connection = dynamic_cast<KnowledgeConnection*>(native))
                    connection->QuarantineSession();
            });
        auto future = task->GetFuture();
        if (_operations)
        {
            // Borrow only the core-owned queue supplied to this pool's own asynchronous factory.
            _operations->Push(task.get());
            task.release();
        }
        return TransactionCallback(std::move(future));
    }

    void Close()
    {
        _operations = nullptr;
        ModuleDatabasePool::Close();
    }

private:
    ProducerConsumerQueue<SQLOperation*>* _operations = nullptr;
    MySQLConnection* CreateConnection(MySQLConnectionInfo& info) override
    {
        return new KnowledgeConnection(info);
    }

    MySQLConnection* CreateConnection(ProducerConsumerQueue<SQLOperation*>* queue,
        MySQLConnectionInfo& info) override
    {
        _operations = queue;
        return new KnowledgeConnection(queue, info);
    }
};

using KnowledgePreparedStatement = PreparedStatement<KnowledgeConnection>;

std::unique_ptr<KnowledgePreparedStatement> MakeStatement(KnowledgePool const& pool, KnowledgeStatement index)
{
    return std::make_unique<KnowledgePreparedStatement>(index, pool.GetPreparedStatementParamCount(index));
}

void BindReceiptKey(KnowledgePreparedStatement& statement, KnowledgeIntent const& intent)
{
    statement.SetArguments(intent.source, intent.sourceFamilyId, intent.eventId, intent.characterGuid,
        intent.knowledgeId);
}

void BindReceiptTerms(KnowledgePreparedStatement& statement, KnowledgeIntent const& intent)
{
    statement.SetArguments(intent.source, intent.sourceFamilyId, intent.eventId, intent.characterGuid,
        intent.knowledgeId, intent.scope, intent.scopeId);
}
} // namespace

struct KnowledgeRepository::Impl : std::enable_shared_from_this<KnowledgeRepository::Impl>
{
    // Bounded callbacks retain immutable scalars only. No gameplay pointers or speculative knowledge cache.
    static constexpr std::size_t MaximumPending = 128;
    std::unique_ptr<KnowledgePool> pool;
    bool ready = false;
    bool polling = false;
    std::uint64_t nextRequest = 0;
    std::uint64_t handlerFailures = 0;
    std::map<std::uint64_t, std::function<void()>> pendingFailures;
    std::vector<TransactionCallback> transactions;
    std::vector<QueryCallback> queries;
    std::vector<std::function<void()>> notifications;

    bool AdmitsRequest() const
    {
        return ready && pendingFailures.size() < MaximumPending && nextRequest != UINT64_MAX;
    }

    template<class Handler, class... Args>
    void InvokeHandler(Handler const& handler, Args&&... args) noexcept
    {
        try
        {
            handler(std::forward<Args>(args)...);
        }
        catch (...)
        {
            // One attempt only. A handler failure cannot change a durable settlement or cancel later handlers.
            if (handlerFailures != UINT64_MAX)
                ++handlerFailures;
        }
    }

    void Complete(std::uint64_t request, std::function<void()> notification)
    {
        if (pendingFailures.erase(request))
            notifications.push_back(std::move(notification));
    }

    void ReadSettlement(std::uint64_t request, KnowledgeIntent intent, SettlementHandler callback)
    {
        if (!ready || pendingFailures.find(request) == pendingFailures.end())
            return;

        auto statement = MakeStatement(*pool, ReadReceipt);
        BindReceiptKey(*statement, intent);
        auto query = pool->AsyncQuery(statement.release());
        std::weak_ptr<Impl> const weak = shared_from_this();
        query.WithPreparedCallback([weak, request, intent, callback](PreparedQueryResult result)
        {
            auto self = weak.lock();
            if (!self || !self->ready)
                return;

            KnowledgeReceiptView row;
            if (result && result->GetRowCount() == 1 && result->GetFieldCount() == 8)
            {
                Field const* fields = result->Fetch();
                row = {true, fields[0].Get<std::uint64_t>(),
                    static_cast<KnowledgeScope>(fields[1].Get<std::uint8_t>()), fields[2].Get<std::uint64_t>(),
                    fields[3].Get<std::uint8_t>(), fields[4].Get<std::uint64_t>(), fields[5].Get<std::uint64_t>(),
                    static_cast<DiscoverySource>(fields[6].Get<std::uint8_t>()), fields[7].Get<std::uint32_t>()};
            }
            auto settlement = ClassifyKnowledgeSettlement(intent, row);
            self->Complete(request, [callback, settlement] { callback(settlement); });
        });
        queries.push_back(std::move(query));
    }

    void DispatchNotifications()
    {
        // Swap before external callbacks: submitting more work or closing cannot invalidate this iteration.
        auto current = std::move(notifications);
        notifications.clear();
        for (auto& callback : current)
            InvokeHandler(callback);
    }

    void Stop(bool notify)
    {
        ready = false;
        if (pool)
            pool->Close();
        transactions.clear();
        queries.clear();
        // Dispose the closed pool before external handlers can reopen this owner.
        pool.reset();
        auto failures = std::move(pendingFailures);
        pendingFailures.clear();
        if (notify)
        {
            for (auto& entry : failures)
                notifications.push_back(std::move(entry.second));
            DispatchNotifications();
        }
        else
            notifications.clear();
    }
};

KnowledgeRepository::KnowledgeRepository() : _impl(std::make_shared<Impl>()) { }

KnowledgeRepository::~KnowledgeRepository()
{
    // Destruction has no external callbacks. Explicit owner-thread Close reports unresolved requests as Failed.
    _impl->Stop(false);
}

bool KnowledgeRepository::Initialize(std::string_view charactersConnectionInfo, std::uint8_t asyncThreads,
    std::uint8_t syncThreads, bool schemaAdmitted, bool transactionApiAdmitted)
{
    auto self = _impl;
    if (self->pool || self->polling || !schemaAdmitted || !transactionApiAdmitted || !asyncThreads || !syncThreads ||
        charactersConnectionInfo.empty())
        return false;

    self->pool = std::make_unique<KnowledgePool>();
    self->pool->SetConnectionInfo(charactersConnectionInfo, asyncThreads, syncThreads);
    if (self->pool->Open() != 0 || !self->pool->PrepareStatements())
    {
        self->Stop(false);
        return false;
    }
    std::array<std::uint8_t, StatementCount> const expected{7, 7, 7, 5, 3};
    for (std::uint32_t index = 0; index < StatementCount; ++index)
    {
        if (self->pool->GetPreparedStatementParamCount(index) != expected[index])
        {
            self->Stop(false);
            return false;
        }
    }
    self->ready = true;
    return true;
}

void KnowledgeRepository::SettleKnowledgeAsync(KnowledgeIntent intent, SettlementHandler callback)
{
    if (!callback)
        return;

    auto self = _impl;
    if (!ValidKnowledgeIntent(intent) || !self->AdmitsRequest())
    {
        self->InvokeHandler(callback, KnowledgeSettlement{
            ValidKnowledgeIntent(intent) ? KnowledgeWriteResult::Failed : KnowledgeWriteResult::Refused,
            intent, 0, 0});
        return;
    }
    auto const request = ++self->nextRequest;
    self->pendingFailures.emplace(request, [callback, intent]
    {
        callback({KnowledgeWriteResult::Failed, intent, 0, 0});
    });
    auto transaction = std::make_shared<KnowledgeTransaction>();
    for (auto index : {ClaimReceipt, InsertKnowledge, SettleReceipt})
    {
        auto statement = MakeStatement(*self->pool, index);
        BindReceiptTerms(*statement, intent);
        transaction->Append(statement.release());
    }
    auto commit = self->pool->AsyncCommitChecked(transaction);
    std::weak_ptr<Impl> const weak = self;
    commit.AfterComplete([weak, request, intent, callback](bool succeeded)
    {
        auto state = weak.lock();
        if (!state || !state->ready)
            return;

        if (succeeded)
            state->ReadSettlement(request, intent, callback);
        else
            state->Complete(request, [callback, intent]
            {
                callback({KnowledgeWriteResult::Failed, intent, 0, 0});
            });
    });
    self->transactions.push_back(std::move(commit));
}

void KnowledgeRepository::HasKnowledgeAsync(KnowledgeScope scope, std::uint64_t scopeId,
    std::uint32_t knowledgeId, LookupHandler callback)
{
    if (!callback)
        return;

    auto self = _impl;
    if (!knowledgeId || !ValidKnowledgeScope(scope, scopeId, scopeId) || !self->AdmitsRequest())
    {
        self->InvokeHandler(callback, false, false);
        return;
    }
    auto const request = ++self->nextRequest;
    self->pendingFailures.emplace(request, [callback] { callback(false, false); });
    auto statement = MakeStatement(*self->pool, ReadKnowledge);
    statement->SetArguments(scope, scopeId, knowledgeId);
    auto query = self->pool->AsyncQuery(statement.release());
    std::weak_ptr<Impl> const weak = self;
    query.WithPreparedCallback([weak, request, callback](PreparedQueryResult result)
    {
        auto state = weak.lock();
        if (!state || !state->ready)
            return;

        bool const succeeded = result && result->GetRowCount() == 1 && result->GetFieldCount() == 1;
        auto const count = succeeded ? result->Fetch()[0].Get<std::uint64_t>() : 0;
        state->Complete(request, [callback, count, succeeded] { callback(count == 1, succeeded && count <= 1); });
    });
    self->queries.push_back(std::move(query));
}

void KnowledgeRepository::Poll()
{
    auto self = _impl;
    if (!self->ready || self->polling)
        return;

    self->polling = true;
    auto transactions = std::move(self->transactions);
    self->transactions.clear();
    for (auto& callback : transactions)
        if (!callback.InvokeIfReady())
            self->transactions.push_back(std::move(callback));

    auto queries = std::move(self->queries);
    self->queries.clear();
    for (auto& callback : queries)
        if (!callback.InvokeIfReady())
            self->queries.push_back(std::move(callback));

    self->polling = false;
    self->DispatchNotifications();
}

std::uint64_t KnowledgeRepository::HandlerFailureCount() const
{
    return _impl->handlerFailures;
}

void KnowledgeRepository::Close()
{
    auto self = _impl;
    self->Stop(true);
}
} // namespace AutoWowStrategy
