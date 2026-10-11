/* AutoWoW original discovery eligibility policy. GPL v2 or later. */
#ifndef AUTOWOW_KNOWLEDGE_POLICY_H
#define AUTOWOW_KNOWLEDGE_POLICY_H

#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
enum class KnowledgeScope : std::uint8_t
{
    Character = 1,
    Guild = 2,
    Faction = 3
};

enum class DiscoverySource : std::uint8_t
{
    DungeonCompletion = 1,
    QuestMilestone = 2
};

// Internal society identities, not native TeamId or faction-template values.
enum class KnowledgeFaction : std::uint8_t
{
    Alliance = 1,
    Horde = 2
};

struct KnowledgeFacts
{
    std::uint64_t eventId = 0;
    std::uint32_t sourceFamilyId = 0;
    std::uint64_t characterGuid = 0;
    std::uint64_t scopeId = 0;
    KnowledgeScope scope = KnowledgeScope::Character;
    DiscoverySource source = DiscoverySource::DungeonCompletion;
    bool confirmedCompletion = false;
    bool participationVerified = false;
    bool scopeEntitled = false;
    bool administrativeOrProbe = false;
};

struct KnowledgeProfile
{
    std::uint32_t sourceFamilyId = 0;
    std::uint32_t knowledgeId = 0;
    KnowledgeScope scope = KnowledgeScope::Character;
    DiscoverySource source = DiscoverySource::DungeonCompletion;
    bool catalogueValidated = false;
};

struct KnowledgeIntent
{
    std::uint64_t eventId = 0;
    std::uint32_t sourceFamilyId = 0;
    std::uint64_t characterGuid = 0;
    std::uint64_t scopeId = 0;
    KnowledgeScope scope = KnowledgeScope::Character;
    DiscoverySource source = DiscoverySource::DungeonCompletion;
    std::uint32_t knowledgeId = 0;
    bool eligible = false;
};

inline bool ValidKnowledgeSource(DiscoverySource source)
{
    return source == DiscoverySource::DungeonCompletion || source == DiscoverySource::QuestMilestone;
}

inline bool ValidKnowledgeScope(KnowledgeScope scope, std::uint64_t scopeId, std::uint64_t characterGuid)
{
    if (!scopeId || !characterGuid)
        return false;

    switch (scope)
    {
        case KnowledgeScope::Character:
            return scopeId == characterGuid;
        case KnowledgeScope::Guild:
            return scopeId <= std::numeric_limits<std::uint32_t>::max();
        case KnowledgeScope::Faction:
            return scopeId == static_cast<std::uint64_t>(KnowledgeFaction::Alliance) ||
                scopeId == static_cast<std::uint64_t>(KnowledgeFaction::Horde);
        default:
            return false;
    }
}

// A caller must verify actual completion, participation and scope entitlement at the earned event.
// This returns scalar facts only; it grants no unlock. Durable event terms and first-earn uniqueness belong
// to the repository, which must never derive scope again from a later guild membership or faction change.
inline KnowledgeIntent PlanKnowledge(KnowledgeFacts const& facts, KnowledgeProfile const& profile)
{
    if (!facts.eventId || !facts.sourceFamilyId || !profile.knowledgeId ||
        !ValidKnowledgeSource(facts.source) || !ValidKnowledgeScope(facts.scope, facts.scopeId, facts.characterGuid) ||
        facts.sourceFamilyId != profile.sourceFamilyId || facts.source != profile.source ||
        facts.scope != profile.scope || !profile.catalogueValidated || !facts.confirmedCompletion ||
        !facts.participationVerified || !facts.scopeEntitled || facts.administrativeOrProbe)
        return {};

    return {facts.eventId, facts.sourceFamilyId, facts.characterGuid, facts.scopeId, facts.scope, facts.source,
        profile.knowledgeId, true};
}
} // namespace AutoWowStrategy

#endif
