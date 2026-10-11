/* AutoWoW actual native connection-description ownership checks. GPL v2 or later. No Open/DB call. */
#ifdef NDEBUG
#error "Connection ownership checks require assertions enabled"
#endif

#include "OwnedConnectionInfo.h"
#include "CheckedAsyncOperation.h"
#include "PCQueue.h"

#include <cassert>
#include <chrono>
#include <memory>

namespace
{
class FixtureConnection final : private AutoWowStrategy::OwnedConnectionInfo, public MySQLConnection
{
public:
    explicit FixtureConnection(MySQLConnectionInfo& info)
        : OwnedConnectionInfo(info), MySQLConnection(OwnedInfo()) { }

    FixtureConnection(ProducerConsumerQueue<SQLOperation*>* queue, MySQLConnectionInfo& info)
        : OwnedConnectionInfo(info), MySQLConnection(queue, OwnedInfo()) { }

    MySQLConnectionInfo& Description() { return OwnedInfo(); }

private:
    void DoPrepareStatements() override { }
};
}

int main()
{
    // These deliberately unused credentials/socket are never passed to native Open.
    MySQLConnectionInfo shared(".;/tmp/autowow-unused-fixture.sock;fixture-user;unused;fixture-db");
    {
        FixtureConnection first(shared);
        FixtureConnection second(shared);
        assert(&first.Description() != &shared && &second.Description() != &shared);
        assert(&first.Description() != &second.Description());
        // The exact mutation performed by native socket Open cannot affect the pool or a sibling.
        first.Description().host = "localhost";
        assert(shared.host == "." && second.Description().host == ".");
        assert(first.Description().port_or_socket == shared.port_or_socket);
        second.Description().database = "different-fixture-db";
        assert(shared.database == "fixture-db" && first.Description().database == "fixture-db");
        shared.user = "changed-source-user";
        assert(first.Description().user == "fixture-user" && second.Description().user == "fixture-user");
    }
    assert(shared.host == "." && shared.database == "fixture-db");
    std::unique_ptr<MySQLConnection> detached;
    {
        MySQLConnectionInfo shortLived(".;/tmp/autowow-unused-second.sock;fixture-user;unused;fixture-db");
        detached = std::make_unique<FixtureConnection>(shortLived);
    }
    auto const derived = dynamic_cast<FixtureConnection*>(detached.get());
    assert(derived && derived->Description().host == ".");
    assert(derived->Description().port_or_socket == "/tmp/autowow-unused-second.sock");
    detached.reset(); // Native destructor/Close completes before the copied description's base is destroyed.
    {
        ProducerConsumerQueue<SQLOperation*> queue;
        auto connection = std::make_unique<FixtureConnection>(&queue, shared);
        MySQLConnection* expected = connection.get();
        auto work = std::make_unique<AutoWowStrategy::CheckedAsyncOperation>([expected](MySQLConnection* bound)
        {
            // Actual DatabaseWorker binding, without Open, query or prepared statement execution.
            return bound == expected;
        }, [](MySQLConnection*) { assert(false); });
        auto future = work->GetFuture();
        queue.Push(work.release());
        assert(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready && future.get());
        queue.Shutdown();
        connection.reset(); // Joins the actual native worker before OwnedConnectionInfo is destroyed.
        assert(queue.Empty() && shared.host == ".");
    }
    {
        ProducerConsumerQueue<SQLOperation*> queue;
        auto connection = std::make_unique<FixtureConnection>(&queue, shared);
        queue.Shutdown();
        connection.reset(); // Idle native worker exits and joins without a connection or operation.
        assert(queue.Empty() && shared.host == ".");
    }
}
