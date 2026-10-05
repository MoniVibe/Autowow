/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_COMBAT_REACTIVITY_H
#define AUTOWOW_COMBAT_REACTIVITY_H

#include <algorithm>
#include <cstdint>

// In-combat AI scheduling (lane combatp0). Value-only so the wake math is unit-tested headless; the
// flags are read once at world init by AutoWowCombatPerformanceTelemetry::LoadConfig().
//
// AutoWow.Combat.GcdWake (default 0). Without it an in-combat bot re-evaluates every react delay
// (ReactDelay x ReactMultiplier, 500 ms live) plus its 0-200 ms stagger, so the first tick after a GCD
// or cast ends lands 0-700 ms late. With it, while the bot is on the GCD or mid cast/channel, the next
// update is pulled in to (time left + kWakeSlackMs) whenever that is sooner than the react delay, with no
// stagger. Never later than the react delay, so movement/interrupt checks keep their old cadence.
//
// AutoWow.Combat.ReactMultiplier (default 5 = the upstream literal): in-combat react delay is
// ReactDelay x this (dynamic react delay, outside battlegrounds). Integer; 0 is read as 1.
namespace AutoWowCombatReactivity
{
inline constexpr std::uint32_t kWakeSlackMs = 50;
inline constexpr std::uint32_t kDefaultReactMultiplier = 5;

namespace detail
{
inline bool gGcdWake = false;
inline std::uint32_t gReactMultiplier = kDefaultReactMultiplier;
}

inline bool GcdWakeEnabled() { return detail::gGcdWake; }
inline std::uint32_t ReactMultiplier() { return detail::gReactMultiplier; }

// Delay until the next AI update. blockedMs = the larger of GCD remaining and cast/channel remaining
// (0 = not blocked). Returns reactDelay when not blocked or when the block outlasts reactDelay.
inline std::uint32_t WakeDelay(std::uint32_t reactDelay, std::uint32_t blockedMs)
{
    if (!blockedMs)
        return reactDelay;
    return std::min<std::uint64_t>(reactDelay, std::uint64_t(blockedMs) + kWakeSlackMs);
}
}

#endif  // AUTOWOW_COMBAT_REACTIVITY_H
