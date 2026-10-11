/* AutoWoW checked transaction fault regressions. GPL v2 or later. */
#ifdef NDEBUG
#error "Transaction tests require assertions enabled"
#endif

#include "CheckedTransactionPolicy.h"

#include <cassert>
#include <string>
#include <vector>

namespace
{
struct Session
{
    bool begin = true;
    bool commit = true;
    bool rollback = true;
    int failedStep = -1;
    std::vector<std::string> calls;

    bool Begin() { calls.emplace_back("begin"); return begin; }
    bool Step(std::size_t index)
    {
        calls.emplace_back("step" + std::to_string(index));
        return static_cast<int>(index) != failedStep;
    }
    bool Commit() { calls.emplace_back("commit"); return commit; }
    bool Rollback() { calls.emplace_back("rollback"); return rollback; }
    void Quarantine() { calls.emplace_back("quarantine"); }
};
}

int main()
{
    Session success;
    assert(AutoWowStrategy::ExecuteCheckedTransaction(3, success));
    assert((success.calls == std::vector<std::string>{"begin", "step0", "step1", "step2", "commit"}));

    Session empty;
    assert(!AutoWowStrategy::ExecuteCheckedTransaction(0, empty) && empty.calls.empty());

    Session beginFailure;
    beginFailure.begin = false;
    assert(!AutoWowStrategy::ExecuteCheckedTransaction(3, beginFailure));
    assert((beginFailure.calls == std::vector<std::string>{"begin", "quarantine"}));

    for (int step = 0; step < 3; ++step)
    {
        Session statementFailure;
        statementFailure.failedStep = step;
        assert(!AutoWowStrategy::ExecuteCheckedTransaction(3, statementFailure));
        assert(statementFailure.calls.back() == "rollback");
        assert(statementFailure.calls.size() == static_cast<std::size_t>(step + 3));
    }

    Session rollbackFailure;
    rollbackFailure.failedStep = 1;
    rollbackFailure.rollback = false;
    assert(!AutoWowStrategy::ExecuteCheckedTransaction(3, rollbackFailure));
    assert((rollbackFailure.calls ==
        std::vector<std::string>{"begin", "step0", "step1", "rollback", "quarantine"}));

    Session commitFailure;
    commitFailure.commit = false;
    assert(!AutoWowStrategy::ExecuteCheckedTransaction(3, commitFailure));
    assert((commitFailure.calls ==
        std::vector<std::string>{"begin", "step0", "step1", "step2", "commit", "quarantine"}));
}
