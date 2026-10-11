/* AutoWoW settlement classification tests. GPL v2 or later. No database or runtime test is implied. */
#ifdef NDEBUG
#error "Knowledge settlement tests require assertions enabled"
#endif

#include "KnowledgeSettlementPolicy.h"

#include <cassert>

int main()
{
    using namespace AutoWowStrategy;
    KnowledgeIntent const intent{101, 36, 70001, 21, KnowledgeScope::Guild,
        DiscoverySource::DungeonCompletion, 9001, true};
    KnowledgeReceiptView row{true, 1, KnowledgeScope::Guild, 21, 1, 101, 70001,
        DiscoverySource::DungeonCompletion, 36};
    auto result = ClassifyKnowledgeSettlement(intent, row);
    assert(result.result == KnowledgeWriteResult::Granted);
    assert(result.originalEventId == 101);
    assert(result.originalContributorGuid == 70001);
    assert(result.intent.scopeId == 21);
    // An exact retry returns its recorded Granted result, not another mutation.
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Granted);

    row.originalContributorGuid = 70002;
    row.outcome = 2;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::AlreadyEarned);
    row.originalEventId = 99;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::AlreadyEarned);
    row.originalEventId = 101;
    row.originalContributorGuid = 70001;
    row.originalSource = DiscoverySource::QuestMilestone;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::AlreadyEarned);
    row.originalSource = DiscoverySource::DungeonCompletion;
    row.originalSourceFamilyId = 37;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::AlreadyEarned);
    row.originalSourceFamilyId = 36;
    row.originalContributorGuid = 70002;
    row.scopeId = 22;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Refused);
    row.scopeId = 21;
    row.scope = KnowledgeScope::Faction;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Refused);
    row.scope = KnowledgeScope::Guild;
    row.querySucceeded = false;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.querySucceeded = true;
    row.receiptCount = 0;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.receiptCount = 2;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.receiptCount = 1;
    row.outcome = 0;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.outcome = 1; // Stored Granted cannot match somebody else's first-earned record.
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.originalEventId = 101;
    row.originalContributorGuid = 70001;
    row.outcome = 2;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.outcome = 1;
    row.originalContributorGuid = 0;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.originalContributorGuid = 70001;
    row.originalEventId = 0;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.originalEventId = 101;
    row.originalSourceFamilyId = 0;
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);
    row.originalSourceFamilyId = 36;
    row.originalSource = static_cast<DiscoverySource>(99);
    assert(ClassifyKnowledgeSettlement(intent, row).result == KnowledgeWriteResult::Failed);

    auto invalid = intent;
    invalid.eligible = false;
    assert(ClassifyKnowledgeSettlement(invalid, row).result == KnowledgeWriteResult::Refused);
    invalid = intent;
    invalid.scope = static_cast<KnowledgeScope>(99);
    assert(ClassifyKnowledgeSettlement(invalid, row).result == KnowledgeWriteResult::Refused);
}
