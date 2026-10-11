/* AutoWoW original discovery settlement policy. GPL v2 or later. */
#ifndef AUTOWOW_KNOWLEDGE_SETTLEMENT_POLICY_H
#define AUTOWOW_KNOWLEDGE_SETTLEMENT_POLICY_H

#include "KnowledgePolicy.h"

namespace AutoWowStrategy
{
enum class KnowledgeWriteResult : std::uint8_t
{
    Granted,
    AlreadyEarned,
    Refused,
    Failed
};

struct KnowledgeSettlement
{
    KnowledgeWriteResult result = KnowledgeWriteResult::Failed;
    KnowledgeIntent intent;
    std::uint64_t originalEventId = 0;
    std::uint64_t originalContributorGuid = 0;
};

// A successful aggregate query returns exactly one row even when receiptCount is zero.
struct KnowledgeReceiptView
{
    bool querySucceeded = false;
    std::uint64_t receiptCount = 0;
    KnowledgeScope scope = KnowledgeScope::Character;
    std::uint64_t scopeId = 0;
    std::uint8_t outcome = 0;
    std::uint64_t originalEventId = 0;
    std::uint64_t originalContributorGuid = 0;
    DiscoverySource originalSource = DiscoverySource::DungeonCompletion;
    std::uint32_t originalSourceFamilyId = 0;
};

inline bool ValidKnowledgeIntent(KnowledgeIntent const& intent)
{
    return intent.eligible && intent.eventId && intent.sourceFamilyId && intent.knowledgeId &&
        ValidKnowledgeSource(intent.source) && ValidKnowledgeScope(intent.scope, intent.scopeId, intent.characterGuid);
}

inline KnowledgeSettlement ClassifyKnowledgeSettlement(KnowledgeIntent const& intent, KnowledgeReceiptView const& row)
{
    KnowledgeSettlement result{KnowledgeWriteResult::Failed, intent, 0, 0};
    if (!ValidKnowledgeIntent(intent))
    {
        result.result = KnowledgeWriteResult::Refused;
        return result;
    }

    if (!row.querySucceeded || row.receiptCount != 1)
        return result;

    if (row.scope != intent.scope || row.scopeId != intent.scopeId)
    {
        result.result = KnowledgeWriteResult::Refused;
        return result;
    }

    if (!row.originalEventId || !row.originalContributorGuid || !row.originalSourceFamilyId ||
        !ValidKnowledgeSource(row.originalSource))
        return result;

    bool const originalMatches = row.originalEventId == intent.eventId &&
        row.originalContributorGuid == intent.characterGuid && row.originalSource == intent.source &&
        row.originalSourceFamilyId == intent.sourceFamilyId;
    if ((row.outcome == 1 && originalMatches) || (row.outcome == 2 && !originalMatches))
    {
        result.result = originalMatches ? KnowledgeWriteResult::Granted : KnowledgeWriteResult::AlreadyEarned;
        result.originalEventId = row.originalEventId;
        result.originalContributorGuid = row.originalContributorGuid;
    }
    return result;
}
} // namespace AutoWowStrategy

#endif
