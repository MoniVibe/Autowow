/* AutoWoW checked asynchronous result operation. GPL v2 or later. */
#ifndef AUTOWOW_CHECKED_ASYNC_OPERATION_H
#define AUTOWOW_CHECKED_ASYNC_OPERATION_H

#include "SQLOperation.h"

#include <functional>
#include <future>
#include <utility>

namespace AutoWowStrategy
{
// Uses the native worker's current connection. Closures retain immutable owned work, never gameplay pointers.
// Cancellation resolves false; execution exceptions attempt quarantine and still resolve false.
class CheckedAsyncOperation final : public SQLOperation
{
public:
    using Work = std::function<bool(MySQLConnection*)>;
    using Quarantine = std::function<void(MySQLConnection*)>;

    CheckedAsyncOperation(Work work, Quarantine quarantine)
        : _work(std::move(work)), _quarantine(std::move(quarantine)) { }

    ~CheckedAsyncOperation() override
    {
        if (!_resolved)
            Resolve(false);
    }

    TransactionFuture GetFuture() { return _result.get_future(); }

private:
    bool Execute() override
    {
        // The native worker dispatches once. An accidental repeated dispatch cannot repeat durable work.
        if (_resolved)
            return _success;
        bool success = false;
        try
        {
            success = _work && _work(m_conn);
        }
        catch (...)
        {
            try
            {
                if (_quarantine)
                    _quarantine(m_conn);
            }
            catch (...)
            {
                // Cleanup failure never changes the failed acknowledgement into success.
            }
        }
        Resolve(success);
        return success;
    }

    void Resolve(bool success)
    {
        _result.set_value(success);
        _success = success;
        _resolved = true;
    }

    Work _work;
    Quarantine _quarantine;
    TransactionPromise _result;
    bool _resolved = false;
    bool _success = false;
};
} // namespace AutoWowStrategy

#endif
