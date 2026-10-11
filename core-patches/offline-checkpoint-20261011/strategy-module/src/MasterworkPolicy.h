/* AutoWoW original native masterwork eligibility policy. GPL v2 or later. */
#ifndef AUTOWOW_MASTERWORK_POLICY_H
#define AUTOWOW_MASTERWORK_POLICY_H

#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
struct MasterworkFacts
{
    std::uint64_t craftEventId = 0;
    std::uint64_t crafterGuid = 0;
    std::uint64_t itemGuid = 0;
    std::uint32_t skillAtCraft = 0;
    std::uint32_t itemPowerBudget = 0;
    std::uint32_t outputCount = 0;
    bool confirmedSelfCraft = false;
    bool recipeAndSpecialtyVerified = false;
    bool cityWorkshop = false;
    bool eligibleUnstackableGear = false;
    bool propertySlotsEmpty = false;
    bool existingRandomProperty = false;
    bool alreadyMasterworked = false;
};

// One existing client-known property bundle, not arbitrary server-only stats or two property IDs.
// The catalogue adapter validates all component effects, item family, scaling and client data compatibility.
struct MasterworkProfile
{
    std::int32_t nativePropertyId = 0;
    std::uint32_t minimumSkill = 0;
    std::uint32_t powerCost = 0;
    std::uint8_t affixCount = 0;
    bool catalogueValidated = false;
};

struct MasterworkDecision
{
    bool eligible = false;
    std::int32_t nativePropertyId = 0;
    std::uint8_t affixCount = 0;
};

// Proposal only; immutable event/item/skill/provenance and one-time application require durable integration.
// Do not alter existing items, grant skills, overwrite enchants or change every copy of an ItemTemplate.
inline MasterworkDecision PlanMasterwork(MasterworkFacts const& facts, MasterworkProfile const& profile)
{
    if (!facts.craftEventId || !facts.crafterGuid || !facts.itemGuid || facts.outputCount != 1 ||
        !facts.confirmedSelfCraft || !facts.recipeAndSpecialtyVerified || !facts.cityWorkshop ||
        !facts.eligibleUnstackableGear || !facts.propertySlotsEmpty || facts.existingRandomProperty ||
        facts.alreadyMasterworked || !profile.catalogueValidated || !profile.nativePropertyId ||
        profile.nativePropertyId < std::numeric_limits<std::int16_t>::min() ||
        profile.nativePropertyId > std::numeric_limits<std::int16_t>::max() ||
        !profile.minimumSkill || facts.skillAtCraft < profile.minimumSkill || !profile.powerCost ||
        profile.powerCost > facts.itemPowerBudget || profile.affixCount < 1 || profile.affixCount > 2)
        return {};

    return {true, profile.nativePropertyId, profile.affixCount};
}
} // namespace AutoWowStrategy

#endif
