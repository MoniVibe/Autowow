/* AutoWoW original earned boss-legacy policy. GPL v2 or later. */
#ifndef AUTOWOW_BOSS_LEGACY_POLICY_H
#define AUTOWOW_BOSS_LEGACY_POLICY_H

#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
// Percentage increments use basis points: 10000 is +100%, not a final multiplier of 100%.
// Primary attributes exclude stamina; maximum health has its own bonus to avoid double-counting it.
// The native adapter must exclude this layer from its baseline and reapply from durable totals.
struct BossLegacyPower
{
    std::uint32_t primaryAttributesBps = 0;
    std::uint32_t maximumHealthBps = 0;
    std::uint32_t damageAndHealingBps = 0;
    std::uint32_t fireResistance = 0;
};

struct BossLegacyProfile
{
    std::uint32_t bossFamilyId = 0;
    BossLegacyPower power;
    bool encounterCatalogueValidated = false;
};

struct BossLegacyFacts
{
    std::uint64_t characterGuid = 0;
    std::uint64_t encounterEventId = 0;
    std::uint64_t instanceSaveId = 0;
    std::uint32_t bossFamilyId = 0;
    bool nativeRaidEncounterCompleted = false;
    bool eligibleParticipationVerified = false;
    bool administrativeOrProbe = false;
    bool alreadyEarned = false;
};

struct BossLegacyDecision
{
    bool eligible = false;
    BossLegacyPower proposedTotal;
};

inline bool ValidBossLegacyPower(BossLegacyPower const& power)
{
    // Flat native resistance is signed. Percentage totals are converted with wide arithmetic by the adapter.
    return power.fireResistance <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
}

inline bool AddBossLegacyValue(std::uint32_t before, std::uint32_t earned, std::uint32_t& total)
{
    if (earned > std::numeric_limits<std::uint32_t>::max() - before)
        return false;

    total = before + earned;
    return true;
}

// Proposal only. The repository must atomically bind (character, boss family) to the event and new totals.
// Difficulty, guild changes and repeat kills do not create another identity for the same personal legacy.
inline BossLegacyDecision PlanBossLegacy(BossLegacyFacts const& facts, BossLegacyProfile const& profile,
    BossLegacyPower const& before)
{
    if (!facts.characterGuid || !facts.encounterEventId || !facts.instanceSaveId || !facts.bossFamilyId ||
        facts.bossFamilyId != profile.bossFamilyId || !facts.nativeRaidEncounterCompleted ||
        !facts.eligibleParticipationVerified || facts.administrativeOrProbe || facts.alreadyEarned ||
        !profile.encounterCatalogueValidated || !ValidBossLegacyPower(before) || !ValidBossLegacyPower(profile.power))
        return {};

    if (!profile.power.primaryAttributesBps && !profile.power.maximumHealthBps &&
        !profile.power.damageAndHealingBps && !profile.power.fireResistance)
        return {};

    BossLegacyPower total;
    if (!AddBossLegacyValue(before.primaryAttributesBps, profile.power.primaryAttributesBps,
            total.primaryAttributesBps) ||
        !AddBossLegacyValue(before.maximumHealthBps, profile.power.maximumHealthBps, total.maximumHealthBps) ||
        !AddBossLegacyValue(before.damageAndHealingBps, profile.power.damageAndHealingBps,
            total.damageAndHealingBps) ||
        !AddBossLegacyValue(before.fireResistance, profile.power.fireResistance, total.fireResistance) ||
        !ValidBossLegacyPower(total))
        return {};

    return {true, total};
}
} // namespace AutoWowStrategy

#endif
