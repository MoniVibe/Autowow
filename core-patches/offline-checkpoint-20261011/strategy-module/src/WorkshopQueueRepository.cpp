/* AutoWoW original durable draft workshop queue. GPL v2 or later. */
#include "WorkshopQueueRepository.h"
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
enum WorkshopStatement : std::uint32_t { InsertDraft, ReadDraft, StatementCount };

class WorkshopConnection;

class WorkshopTransaction final : public Transaction<WorkshopConnection>
{
public:
    std::vector<SQLElementData> const& Queries() const { return m_queries; }
};

class WorkshopConnection final : private OwnedConnectionInfo, public MySQLConnection
{
public:
    explicit WorkshopConnection(MySQLConnectionInfo& info)
        : OwnedConnectionInfo(info), MySQLConnection(OwnedInfo()) { }
    WorkshopConnection(ProducerConsumerQueue<SQLOperation*>* queue, MySQLConnectionInfo& info)
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

    bool CommitChecked(WorkshopTransaction const& transaction)
    {
        // One immutable draft insert. Never replay an individual statement on another session.
        if (transaction.GetSize() != 1 || !m_Mysql)
            return false;

        struct Backend
        {
            WorkshopConnection& connection;
            WorkshopTransaction const& transaction;
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

        PrepareStatement(InsertDraft,
            "INSERT INTO `autowow_workshop_drafts` "
            "(`order_id`,`crafter_guid`,`recipient_guid`,`output_item_entry`,`recipe_id`,`profile_id`,"
            "`profile_version`,`kind`,`quantity`,`stage_count`,`duration_0_ms`,`duration_1_ms`,`duration_2_ms`,"
            "`duration_3_ms`,`duration_4_ms`) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON DUPLICATE KEY UPDATE `order_id`=`autowow_workshop_drafts`.`order_id`", CONNECTION_ASYNC);
        // Aggregate miss is distinguishable from native null on query failure.
        PrepareStatement(ReadDraft,
            "SELECT COUNT(*),MAX(`crafter_guid`),MAX(`recipient_guid`),MAX(`output_item_entry`),"
            "MAX(`recipe_id`),MAX(`profile_id`),MAX(`profile_version`),MAX(`kind`),MAX(`quantity`),"
            "MAX(`stage_count`),MAX(`duration_0_ms`),MAX(`duration_1_ms`),MAX(`duration_2_ms`),"
            "MAX(`duration_3_ms`),MAX(`duration_4_ms`),MAX(`draft_state`) "
            "FROM `autowow_workshop_drafts` WHERE `order_id`=?", CONNECTION_ASYNC);
    }
};

class WorkshopPool final : public ModuleDatabasePool
{
public:
    // Quarantined sessions may have no MYSQL handle; this pool never queues native Ping operations.
    void KeepAlive() = delete;

    TransactionCallback AsyncCommitChecked(std::shared_ptr<WorkshopTransaction> transaction)
    {
        auto task = std::make_unique<CheckedAsyncOperation>(
            [transaction = std::move(transaction)](MySQLConnection* native)
            {
                auto connection = dynamic_cast<WorkshopConnection*>(native);
                return connection && connection->CommitChecked(*transaction);
            },
            [](MySQLConnection* native)
            {
                if (auto connection = dynamic_cast<WorkshopConnection*>(native))
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
        return new WorkshopConnection(info);
    }

    MySQLConnection* CreateConnection(ProducerConsumerQueue<SQLOperation*>* queue,
        MySQLConnectionInfo& info) override
    {
        _operations = queue;
        return new WorkshopConnection(queue, info);
    }
};

using WorkshopPreparedStatement = PreparedStatement<WorkshopConnection>;

std::unique_ptr<WorkshopPreparedStatement> MakeStatement(WorkshopPool const& pool, WorkshopStatement index)
{
    return std::make_unique<WorkshopPreparedStatement>(index, pool.GetPreparedStatementParamCount(index));
}

bool ValidDraft(WorkshopDraft const& draft)
{
    return draft.recipientGuid && draft.outputItemEntry && draft.order.quantity <= 1000 &&
        ValidWorkshopTerms(draft.order, draft.profile);
}

bool SameDraftTerms(WorkshopDraft const& a, WorkshopDraft const& b)
{
    auto const& x = a.order;
    auto const& y = b.order;
    return x.orderId == y.orderId && x.crafterGuid == y.crafterGuid && x.recipeId == y.recipeId &&
        x.profileId == y.profileId && x.profileVersion == y.profileVersion && x.kind == y.kind &&
        x.quantity == y.quantity && a.recipientGuid == b.recipientGuid && a.outputItemEntry == b.outputItemEntry &&
        a.profile.stageCount == b.profile.stageCount && a.profile.stageDurationMs == b.profile.stageDurationMs;
}

WorkshopDraftView DecodeDraft(std::uint64_t id, PreparedQueryResult result)
{
    if (!result || result->GetRowCount() != 1 || result->GetFieldCount() != 16)
        return {};
    auto const* fields = result->Fetch();
    auto const count = fields[0].Get<std::uint64_t>();
    if (!count)
        return {true, false, {}};
    if (count != 1 || fields[15].Get<std::uint8_t>() != 0)
        return {};
    WorkshopDraft draft;
    draft.order = {id, fields[1].Get<std::uint64_t>(), fields[4].Get<std::uint32_t>(),
        fields[5].Get<std::uint32_t>(), fields[6].Get<std::uint32_t>(),
        static_cast<WorkshopKind>(fields[7].Get<std::uint8_t>()), fields[8].Get<std::uint32_t>()};
    draft.recipientGuid = fields[2].Get<std::uint64_t>();
    draft.outputItemEntry = fields[3].Get<std::uint32_t>();
    draft.profile = {draft.order.profileId, draft.order.profileVersion, draft.order.recipeId,
        draft.order.kind, fields[9].Get<std::uint8_t>(), {}, true};
    for (std::size_t i = 0; i < draft.profile.stageDurationMs.size(); ++i)
        draft.profile.stageDurationMs[i] = fields[10 + i].Get<std::uint64_t>();
    if (!ValidDraft(draft))
        return {};
    // Persistence is not fresh native recipe/timing/station admission for a future craft adapter.
    draft.profile.timingCatalogueValidated = false;
    return {true, true, draft};
}
} // namespace

struct WorkshopQueueRepository::Impl : std::enable_shared_from_this<WorkshopQueueRepository::Impl>
{
    // Bounded callbacks retain immutable scalars only. No gameplay pointers or speculative knowledge cache.
    static constexpr std::size_t MaximumPending = 128;
    std::unique_ptr<WorkshopPool> pool;
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

    void ReadSubmittedDraft(std::uint64_t request, WorkshopDraft draft, DraftHandler callback)
    {
        if (!ready || pendingFailures.find(request) == pendingFailures.end())
            return;
        auto statement = MakeStatement(*pool, ReadDraft);
        statement->SetArguments(draft.order.orderId);
        auto query = pool->AsyncQuery(statement.release());
        std::weak_ptr<Impl> const weak = shared_from_this();
        query.WithPreparedCallback([weak, request, draft, callback](PreparedQueryResult result)
        {
            auto self = weak.lock();
            if (!self || !self->ready)
                return;
            auto const row = DecodeDraft(draft.order.orderId, result);
            auto const outcome = !row.querySucceeded || !row.present ? WorkshopDraftResult::Failed :
                SameDraftTerms(draft, row.draft) ? WorkshopDraftResult::Recorded : WorkshopDraftResult::Conflict;
            self->Complete(request, [callback, outcome] { callback(outcome); });
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

WorkshopQueueRepository::WorkshopQueueRepository() : _impl(std::make_shared<Impl>()) { }

WorkshopQueueRepository::~WorkshopQueueRepository()
{
    // Destruction has no external callbacks. Explicit owner-thread Close reports unresolved requests as Failed.
    _impl->Stop(false);
}

bool WorkshopQueueRepository::Initialize(std::string_view charactersConnectionInfo, std::uint8_t asyncThreads,
    std::uint8_t syncThreads, bool schemaAdmitted, bool transactionApiAdmitted)
{
    auto self = _impl;
    if (self->pool || self->polling || !schemaAdmitted || !transactionApiAdmitted || !asyncThreads || !syncThreads ||
        charactersConnectionInfo.empty())
        return false;

    self->pool = std::make_unique<WorkshopPool>();
    self->pool->SetConnectionInfo(charactersConnectionInfo, asyncThreads, syncThreads);
    if (self->pool->Open() != 0 || !self->pool->PrepareStatements())
    {
        self->Stop(false);
        return false;
    }
    std::array<std::uint8_t, StatementCount> const expected{15, 1};
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

void WorkshopQueueRepository::SubmitDraftAsync(WorkshopDraft draft, DraftHandler callback)
{
    if (!callback)
        return;
    auto self = _impl;
    if (!ValidDraft(draft) || !self->AdmitsRequest())
    {
        self->InvokeHandler(callback, ValidDraft(draft) ? WorkshopDraftResult::Failed : WorkshopDraftResult::Refused);
        return;
    }
    auto const request = ++self->nextRequest;
    self->pendingFailures.emplace(request, [callback] { callback(WorkshopDraftResult::Failed); });
    auto statement = MakeStatement(*self->pool, InsertDraft);
    auto const& order = draft.order;
    auto const& profile = draft.profile;
    statement->SetArguments(order.orderId, order.crafterGuid, draft.recipientGuid, draft.outputItemEntry,
        order.recipeId, order.profileId, order.profileVersion, order.kind, order.quantity, profile.stageCount,
        profile.stageDurationMs[0], profile.stageDurationMs[1], profile.stageDurationMs[2],
        profile.stageDurationMs[3], profile.stageDurationMs[4]);
    auto transaction = std::make_shared<WorkshopTransaction>();
    transaction->Append(statement.release());
    auto commit = self->pool->AsyncCommitChecked(transaction);
    std::weak_ptr<Impl> const weak = self;
    commit.AfterComplete([weak, request, draft, callback](bool succeeded)
    {
        auto state = weak.lock();
        if (!state || !state->ready)
            return;
        if (succeeded)
            state->ReadSubmittedDraft(request, draft, callback);
        else
            state->Complete(request, [callback] { callback(WorkshopDraftResult::Failed); });
    });
    self->transactions.push_back(std::move(commit));
}

void WorkshopQueueRepository::ReadDraftAsync(std::uint64_t orderId, LookupHandler callback)
{
    if (!callback)
        return;
    auto self = _impl;
    if (!orderId || !self->AdmitsRequest())
    {
        self->InvokeHandler(callback, WorkshopDraftView{});
        return;
    }
    auto const request = ++self->nextRequest;
    self->pendingFailures.emplace(request, [callback] { callback({}); });
    auto statement = MakeStatement(*self->pool, ReadDraft);
    statement->SetArguments(orderId);
    auto query = self->pool->AsyncQuery(statement.release());
    std::weak_ptr<Impl> const weak = self;
    query.WithPreparedCallback([weak, request, orderId, callback](PreparedQueryResult result)
    {
        auto state = weak.lock();
        if (!state || !state->ready)
            return;
        auto const row = DecodeDraft(orderId, result);
        state->Complete(request, [callback, row] { callback(row); });
    });
    self->queries.push_back(std::move(query));
}

void WorkshopQueueRepository::Poll()
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

std::uint64_t WorkshopQueueRepository::HandlerFailureCount() const
{
    return _impl->handlerFailures;
}

void WorkshopQueueRepository::Close()
{
    auto self = _impl;
    self->Stop(true);
}
} // namespace AutoWowStrategy
