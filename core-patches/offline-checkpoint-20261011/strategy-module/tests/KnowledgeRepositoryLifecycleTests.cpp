/* AutoWoW admission/lifecycle regression source. GPL v2 or later. Unrun, no DB connection admitted. */
#ifdef NDEBUG
#error "Knowledge lifecycle tests require assertions enabled"
#endif

#include "KnowledgeRepository.h"

#include <cassert>

int main()
{
    using namespace AutoWowStrategy;
    KnowledgeRepository repository;
    // Every refusal occurs before pool construction/Open. This is not an isolated database test.
    assert(!repository.Initialize("not-a-connection", 1, 1, false, false));
    assert(!repository.Initialize("not-a-connection", 1, 1, true));
    assert(!repository.Initialize("not-a-connection", 0, 1, true, true));
    assert(!repository.Initialize("not-a-connection", 1, 0, true, true));
    assert(!repository.Initialize("", 1, 1, true, true));

    KnowledgeIntent const intent{101, 36, 70001, 21, KnowledgeScope::Guild,
        DiscoverySource::DungeonCompletion, 9001, true};
    unsigned settlements = 0;
    repository.SettleKnowledgeAsync(intent, [&](KnowledgeSettlement result)
    {
        ++settlements;
        assert(result.result == KnowledgeWriteResult::Failed);
        assert(result.intent.scopeId == 21);
        repository.Close();
    });
    assert(settlements == 1);
    auto invalid = intent;
    invalid.eligible = false;
    repository.SettleKnowledgeAsync(invalid, [&](KnowledgeSettlement result)
    {
        ++settlements;
        assert(result.result == KnowledgeWriteResult::Refused);
    });
    assert(settlements == 2);
    unsigned lookups = 0;
    repository.HasKnowledgeAsync(KnowledgeScope::Guild, 21, 9001, [&](bool present, bool succeeded)
    {
        ++lookups;
        assert(!present && !succeeded);
    });
    assert(lookups == 1);
    repository.Poll();
    repository.Close();
    repository.Close();
    assert(settlements == 2 && lookups == 1);
}
