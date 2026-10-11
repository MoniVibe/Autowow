/* AutoWoW original boss-legacy policy checks. GPL v2 or later. */
#ifdef NDEBUG
#error Boss legacy checks require assertions enabled
#endif

#include "../src/BossLegacyPolicy.h"
#include <cassert>

using namespace AutoWowStrategy;

int main()
{
    // Synthetic family ID; the actual encounter catalogue must supply a verified canonical family.
    BossLegacyFacts facts{1, 2, 3, 4, true, true, false, false};
    BossLegacyProfile profile{4, {2500, 10000, 2500, 75}, true};
    BossLegacyPower before;
    auto decision = PlanBossLegacy(facts, profile, before);
    assert(decision.eligible);
    assert(decision.proposedTotal.primaryAttributesBps == 2500);
    assert(decision.proposedTotal.maximumHealthBps == 10000);
    assert(decision.proposedTotal.damageAndHealingBps == 2500);
    assert(decision.proposedTotal.fireResistance == 75);

    auto rejectFacts = [&](BossLegacyFacts const& changed)
    {
        return !PlanBossLegacy(changed, profile, before).eligible;
    };
    auto changed = facts;
    changed.characterGuid = 0;
    assert(rejectFacts(changed));
    changed = facts;
    changed.encounterEventId = 0;
    assert(rejectFacts(changed));
    changed = facts;
    changed.instanceSaveId = 0;
    assert(rejectFacts(changed));
    changed = facts;
    changed.bossFamilyId = 0;
    assert(rejectFacts(changed));
    changed = facts;
    changed.bossFamilyId = 5;
    assert(rejectFacts(changed));
    changed = facts;
    changed.nativeRaidEncounterCompleted = false;
    assert(rejectFacts(changed));
    changed = facts;
    changed.eligibleParticipationVerified = false;
    assert(rejectFacts(changed));
    changed = facts;
    changed.administrativeOrProbe = true;
    assert(rejectFacts(changed));
    changed = facts;
    changed.alreadyEarned = true;
    assert(rejectFacts(changed));

    auto changedProfile = profile;
    changedProfile.encounterCatalogueValidated = false;
    assert(!PlanBossLegacy(facts, changedProfile, before).eligible);
    changedProfile = profile;
    changedProfile.bossFamilyId = 0;
    assert(!PlanBossLegacy(facts, changedProfile, before).eligible);
    changedProfile = profile;
    changedProfile.power = {};
    assert(!PlanBossLegacy(facts, changedProfile, before).eligible);

    // Nine +2% attributes/+5% health/+2% output increments plus the drafted end-boss reward, added not compounded.
    before = {1800, 4500, 1800, 0};
    decision = PlanBossLegacy(facts, profile, before);
    assert(decision.eligible);
    assert(decision.proposedTotal.primaryAttributesBps == 4300);
    assert(decision.proposedTotal.maximumHealthBps == 14500);
    assert(decision.proposedTotal.damageAndHealingBps == 4300);
    assert(decision.proposedTotal.fireResistance == 75);

    auto const maximum = std::numeric_limits<std::uint32_t>::max();
    before = {maximum - 2499, 0, 0, 0};
    assert(!PlanBossLegacy(facts, profile, before).eligible);
    before = {0, maximum - 9999, 0, 0};
    assert(!PlanBossLegacy(facts, profile, before).eligible);
    before = {0, 0, maximum - 2499, 0};
    assert(!PlanBossLegacy(facts, profile, before).eligible);
    before = {0, 0, 0, maximum};
    assert(!PlanBossLegacy(facts, profile, before).eligible);
    before = {0, 0, 0, static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) - 74};
    assert(!PlanBossLegacy(facts, profile, before).eligible);
    before = {};
    changedProfile = profile;
    changedProfile.power.fireResistance = maximum;
    assert(!PlanBossLegacy(facts, changedProfile, before).eligible);
    before = {maximum - 2500, maximum - 10000, maximum - 2500, 0};
    decision = PlanBossLegacy(facts, profile, before);
    assert(decision.eligible);
    assert(decision.proposedTotal.primaryAttributesBps == maximum);
    assert(decision.proposedTotal.maximumHealthBps == maximum);
    assert(decision.proposedTotal.damageAndHealingBps == maximum);
}
