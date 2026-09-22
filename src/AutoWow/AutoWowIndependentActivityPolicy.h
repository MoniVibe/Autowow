#ifndef AUTOWOW_INDEPENDENT_ACTIVITY_POLICY_H
#define AUTOWOW_INDEPENDENT_ACTIVITY_POLICY_H

#include <cstdint>

// Independent AutoWoW bots should not inherit background-bot rotation or multi-second
// idle reaction jitter. This policy leaves combat, casting, and explicit waits to their owners.
namespace AutoWowIndependentActivityPolicy
{
constexpr bool ShouldForceActivity(bool independent, bool paused) noexcept
{
    return independent && !paused;
}

constexpr bool ShouldUseFastReaction(bool independent, bool paused,
                                     bool inCombat, bool combatState) noexcept
{
    return ShouldForceActivity(independent, paused) && !inCombat && !combatState;
}

constexpr std::uint32_t NonCombatReactDelay(std::uint32_t configuredBase) noexcept
{
    return configuredBase < 100U ? 100U : configuredBase > 250U ? 250U : configuredBase;
}
}

#endif
