/* AutoWoW knowledge eligibility tests. GPL v2 or later. Source-only; assertions have not run. */
#ifdef NDEBUG
#error "Knowledge policy tests require assertions enabled"
#endif

#include "KnowledgePolicy.h"

#include <cassert>
#include <cstdint>
#include <limits>

namespace
{
using namespace AutoWowStrategy;

KnowledgeFacts GuildFacts()
{
    return {101, 36, 70001, 21, KnowledgeScope::Guild, DiscoverySource::DungeonCompletion,
        true, true, true, false};
}

KnowledgeProfile GuildProfile()
{
    return {36, 9001, KnowledgeScope::Guild, DiscoverySource::DungeonCompletion, true};
}

void EligibleFactsRetainImmutableIdentity()
{
    auto const intent = PlanKnowledge(GuildFacts(), GuildProfile());
    assert(intent.eligible);
    assert(intent.eventId == 101);
    assert(intent.sourceFamilyId == 36);
    assert(intent.characterGuid == 70001);
    assert(intent.scopeId == 21);
    assert(intent.scope == KnowledgeScope::Guild);
    assert(intent.source == DiscoverySource::DungeonCompletion);
    assert(intent.knowledgeId == 9001);
}

void MissingAndInvalidIdentitiesRefuse()
{
    auto facts = GuildFacts();
    facts.eventId = 0;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.sourceFamilyId = 0;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.characterGuid = 0;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.scopeId = 0;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.scopeId = std::numeric_limits<std::uint32_t>::max();
    assert(PlanKnowledge(facts, GuildProfile()).eligible);
    facts.scopeId += 1;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    auto profile = GuildProfile();
    profile.knowledgeId = 0;
    assert(!PlanKnowledge(GuildFacts(), profile).eligible);
    facts = GuildFacts();
    facts.scope = static_cast<KnowledgeScope>(99);
    profile = GuildProfile();
    profile.scope = facts.scope;
    assert(!PlanKnowledge(facts, profile).eligible);
    facts = GuildFacts();
    facts.source = static_cast<DiscoverySource>(99);
    profile = GuildProfile();
    profile.source = facts.source;
    assert(!PlanKnowledge(facts, profile).eligible);
}

void InvalidCompletionAndEntitlementRefuse()
{
    auto facts = GuildFacts();
    facts.confirmedCompletion = false;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.participationVerified = false;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.scopeEntitled = false;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    facts = GuildFacts();
    facts.administrativeOrProbe = true;
    assert(!PlanKnowledge(facts, GuildProfile()).eligible);
    auto profile = GuildProfile();
    profile.catalogueValidated = false;
    assert(!PlanKnowledge(GuildFacts(), profile).eligible);
    profile = GuildProfile();
    profile.sourceFamilyId = 37;
    assert(!PlanKnowledge(GuildFacts(), profile).eligible);
    profile = GuildProfile();
    profile.scope = KnowledgeScope::Character;
    assert(!PlanKnowledge(GuildFacts(), profile).eligible);
    profile = GuildProfile();
    profile.source = DiscoverySource::QuestMilestone;
    assert(!PlanKnowledge(GuildFacts(), profile).eligible);
}

void CharacterAndFactionScopesAreExplicit()
{
    auto facts = GuildFacts();
    auto profile = GuildProfile();
    facts.scope = profile.scope = KnowledgeScope::Character;
    facts.scopeId = facts.characterGuid;
    assert(PlanKnowledge(facts, profile).eligible);
    facts.scopeId = 70002;
    assert(!PlanKnowledge(facts, profile).eligible);
    facts.scope = profile.scope = KnowledgeScope::Faction;
    facts.scopeId = static_cast<std::uint64_t>(KnowledgeFaction::Alliance);
    assert(PlanKnowledge(facts, profile).eligible);
    facts.scopeId = static_cast<std::uint64_t>(KnowledgeFaction::Horde);
    assert(PlanKnowledge(facts, profile).eligible);
    facts.scopeId = 3;
    assert(!PlanKnowledge(facts, profile).eligible);
    facts.scopeId = 0;
    assert(!PlanKnowledge(facts, profile).eligible);
    facts = GuildFacts();
    profile = GuildProfile();
    facts.source = profile.source = DiscoverySource::QuestMilestone;
    assert(PlanKnowledge(facts, profile).eligible);
}

void RefusalDoesNotLeakAnAwardIntent()
{
    auto facts = GuildFacts();
    facts.confirmedCompletion = false;
    auto const intent = PlanKnowledge(facts, GuildProfile());
    assert(!intent.eligible);
    assert(intent.eventId == 0);
    assert(intent.scopeId == 0);
    assert(intent.knowledgeId == 0);
    assert(intent.characterGuid == 0);
}

void LaterMembershipCannotRewriteCapturedScope()
{
    auto facts = GuildFacts();
    auto const earned = PlanKnowledge(facts, GuildProfile());
    facts.scopeId = 22;
    auto const later = PlanKnowledge(facts, GuildProfile());
    assert(earned.scopeId == 21);
    assert(later.scopeId == 22);
    // Different captured scopes require repository event-term arbitration; this function cannot settle either.
    assert(earned.eventId == later.eventId);
    assert(earned.characterGuid == later.characterGuid);
}

void ScalarWidthIsPreserved()
{
    auto facts = GuildFacts();
    facts.eventId = std::numeric_limits<std::uint64_t>::max();
    facts.characterGuid = std::numeric_limits<std::uint64_t>::max();
    auto const intent = PlanKnowledge(facts, GuildProfile());
    assert(intent.eligible);
    assert(intent.eventId == facts.eventId);
    assert(intent.characterGuid == facts.characterGuid);
}
} // namespace

int main()
{
    EligibleFactsRetainImmutableIdentity();
    MissingAndInvalidIdentitiesRefuse();
    InvalidCompletionAndEntitlementRefuse();
    CharacterAndFactionScopesAreExplicit();
    RefusalDoesNotLeakAnAwardIntent();
    LaterMembershipCannotRewriteCapturedScope();
    ScalarWidthIsPreserved();
}
