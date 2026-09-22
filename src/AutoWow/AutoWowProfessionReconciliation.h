/*
 * Small, allocation-free policy for reconciling an explicit primary profession pair.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_PROFESSION_RECONCILIATION_H
#define MOD_PLAYERBOTS_AUTOWOW_PROFESSION_RECONCILIATION_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace AutoWowProfessionReconciliation
{
using SkillId = std::uint16_t;

inline constexpr std::size_t kMaxPrimarySkills = 14;

struct ProfessionPair
{
    SkillId first = 0;
    SkillId second = 0;
};

struct ObservedPrimarySkills
{
    std::array<SkillId, kMaxPrimarySkills> skills{};
    std::size_t count = 0;
};

inline bool IsValid(ProfessionPair const requested) noexcept
{
    return requested.first != 0 && requested.second != 0 && requested.first != requested.second;
}

// Native random/class catalogs remain authoritative for ordinary bots. An AutoWow-managed bot may
// carry an explicit pair that is intentionally absent from those weighted catalogs.
inline bool ShouldHonorStoredPair(bool autoWowManaged, bool validStoredPair,
                                  bool presentInNativeCatalog) noexcept
{
    return validStoredPair && (autoWowManaged || presentInNativeCatalog);
}

inline bool Contains(ObservedPrimarySkills const& observed, SkillId skill) noexcept
{
    for (std::size_t index = 0; index < observed.count; ++index)
        if (observed.skills[index] == skill)
            return true;
    return false;
}

// Returns true only when a mutation is needed. On mutation, the model becomes exactly the
// requested pair; a second call therefore returns false without changing the model.
inline bool ReconcilePrimarySkills(ObservedPrimarySkills& observed,
                                   ProfessionPair const requested) noexcept
{
    if (!IsValid(requested) ||
        (observed.count == 2 && Contains(observed, requested.first) &&
         Contains(observed, requested.second)))
        return false;

    observed = {};
    observed.skills[0] = requested.first;
    observed.skills[1] = requested.second;
    observed.count = 2;
    return true;
}
}  // namespace AutoWowProfessionReconciliation

#endif  // MOD_PLAYERBOTS_AUTOWOW_PROFESSION_RECONCILIATION_H
