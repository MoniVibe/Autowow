/* AutoWoW native queue/task lifecycle checks. GPL v2 or later. No database connection admitted. */
#ifdef NDEBUG
#error "Asynchronous operation checks require assertions enabled"
#endif

#include "CheckedAsyncOperation.h"
#include "PCQueue.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace
{
using AutoWowStrategy::CheckedAsyncOperation;

bool Take(TransactionFuture& future)
{
    assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    return future.get();
}
}

int main()
{
    unsigned calls = 0;
    unsigned quarantines = 0;
    auto work = [&](MySQLConnection* connection)
    {
        assert(connection == nullptr); // These checks never instantiate a MYSQL connection or pool.
        ++calls;
        return true;
    };
    auto quarantine = [&](MySQLConnection*) { ++quarantines; };
    {
        ProducerConsumerQueue<SQLOperation*> queue;
        auto operation = std::make_unique<CheckedAsyncOperation>(work, quarantine);
        auto future = operation->GetFuture();
        assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        queue.Push(operation.release());
        SQLOperation* dispatched = nullptr;
        queue.WaitAndPop(dispatched);
        assert(dispatched);
        dispatched->SetConnection(nullptr);
        dispatched->call();
        dispatched->call();
        delete dispatched;
        assert(Take(future) && calls == 1 && quarantines == 0);
    }
    {
        auto operation = std::make_unique<CheckedAsyncOperation>([](MySQLConnection*) { return false; }, quarantine);
        auto future = operation->GetFuture();
        operation->call();
        operation.reset();
        assert(!Take(future) && quarantines == 0);
    }
    {
        auto operation = std::make_unique<CheckedAsyncOperation>([](MySQLConnection*) -> bool
        {
            throw std::runtime_error("injected work exception");
        }, quarantine);
        auto future = operation->GetFuture();
        operation->call();
        operation->call();
        operation.reset();
        assert(!Take(future) && quarantines == 1);
    }
    {
        auto operation = std::make_unique<CheckedAsyncOperation>([](MySQLConnection*) -> bool
        {
            throw std::runtime_error("injected work exception");
        }, [](MySQLConnection*) { throw std::runtime_error("injected cleanup exception"); });
        auto future = operation->GetFuture();
        operation->call();
        operation.reset();
        assert(!Take(future));
    }
    {
        auto operation = std::make_unique<CheckedAsyncOperation>(CheckedAsyncOperation::Work{}, quarantine);
        auto future = operation->GetFuture();
        operation->call();
        operation.reset();
        assert(!Take(future) && quarantines == 1);
    }
    {
        ProducerConsumerQueue<SQLOperation*> queue;
        auto operation = std::make_unique<CheckedAsyncOperation>(work, quarantine);
        auto cancelled = operation->GetFuture();
        queue.Push(operation.release());
        queue.Cancel();
        assert(!Take(cancelled) && calls == 1);
        queue.Reset();
        auto restarted = std::make_unique<CheckedAsyncOperation>(work, quarantine);
        auto future = restarted->GetFuture();
        queue.Push(restarted.release());
        SQLOperation* dispatched = nullptr;
        queue.WaitAndPop(dispatched);
        assert(dispatched);
        dispatched->call();
        delete dispatched;
        assert(Take(future) && calls == 2);
        queue.Shutdown();
    }
    {
        ProducerConsumerQueue<SQLOperation*> queue;
        auto operation = std::make_unique<CheckedAsyncOperation>(work, quarantine);
        auto future = operation->GetFuture();
        queue.Push(operation.release());
        queue.Shutdown();
        SQLOperation* dispatched = nullptr;
        queue.WaitAndPop(dispatched);
        assert(dispatched);
        dispatched->call();
        delete dispatched;
        dispatched = nullptr;
        queue.WaitAndPop(dispatched);
        assert(!dispatched && Take(future) && calls == 3);
    }
    {
        auto operation = std::make_unique<CheckedAsyncOperation>(work, quarantine);
        auto future = operation->GetFuture();
        operation.reset();
        assert(!Take(future) && calls == 3 && quarantines == 1);
    }
}
