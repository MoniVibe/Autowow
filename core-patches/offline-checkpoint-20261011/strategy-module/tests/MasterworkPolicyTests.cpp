/* AutoWoW native masterwork policy tests. GPL v2 or later. Not runtime source. */
#ifdef NDEBUG
#error "Masterwork policy tests require assertions enabled"
#endif

#include "MasterworkPolicy.h"

#include <cassert>
#include <limits>

int main()
{
    using namespace AutoWowStrategy;
    MasterworkFacts facts{1, 1001, 2001, 300, 10, 1, true, true, true, true, true, false, false};
    // Synthetic property IDs test policy arithmetic only; no native catalogue admission is implied.
    MasterworkProfile profile{1, 300, 10, 1, true};
    auto const first = PlanMasterwork(facts, profile);
    assert(first.eligible && first.nativePropertyId == 1 && first.affixCount == 1);
    facts.skillAtCraft = 299;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.skillAtCraft = 375;
    profile.minimumSkill = 375;
    profile.affixCount = 2;
    assert(PlanMasterwork(facts, profile).eligible);
    profile.powerCost = 11;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.powerCost = 10;
    facts.itemPowerBudget = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.itemPowerBudget = 10;
    facts.craftEventId = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.craftEventId = 1;
    facts.crafterGuid = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.crafterGuid = 1001;
    facts.itemGuid = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.itemGuid = 2001;
    facts.outputCount = 2;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.outputCount = 1;
    facts.confirmedSelfCraft = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.confirmedSelfCraft = true;
    facts.recipeAndSpecialtyVerified = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.recipeAndSpecialtyVerified = true;
    facts.cityWorkshop = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.cityWorkshop = true;
    facts.eligibleUnstackableGear = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.eligibleUnstackableGear = true;
    facts.propertySlotsEmpty = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.propertySlotsEmpty = true;
    facts.existingRandomProperty = true;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.existingRandomProperty = false;
    facts.alreadyMasterworked = true;
    assert(!PlanMasterwork(facts, profile).eligible);
    facts.alreadyMasterworked = false;
    profile.catalogueValidated = false;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.catalogueValidated = true;
    profile.affixCount = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.affixCount = 3;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.affixCount = 2;
    profile.minimumSkill = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.minimumSkill = 375;
    profile.powerCost = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.powerCost = 10;
    profile.nativePropertyId = 0;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.nativePropertyId = 32768;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.nativePropertyId = -32769;
    assert(!PlanMasterwork(facts, profile).eligible);
    profile.nativePropertyId = std::numeric_limits<std::int16_t>::max();
    assert(PlanMasterwork(facts, profile).eligible);
    profile.nativePropertyId = -1;
    assert(PlanMasterwork(facts, profile).eligible);
    assert(facts.skillAtCraft == 375 && !facts.alreadyMasterworked);
}
