/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_PULL_LEVEL_CAP_H
#define AUTOWOW_PULL_LEVEL_CAP_H

#include <cstdint>

// Pull level cap for solo independent AutoWoW bots (AutoWow.Survival.PullLevelCap, default 0).
// soak-s13-full-r1: 43 of 104 cohort deaths were L9-13 bots in their new zones killed by mobs +1..+3
// above them. With the flag on, GrindTargetValue (legacy grind and the strict quest-objective pick New
// RPG DO_QUEST delegates to) never proactively chooses a creature whose level, plus ElitePenalty for an
// elite, is more than MaxLevelAbove above the bot, unless it is a quest objective AND the bot is healthy
// (hp, and mana for mana users, >= HealthyPct) AND the target is alone (no idle hostile within
// AloneYards of it). Self-defence (attackers) is never capped. Skips and quest exceptions are counted
// (process-lifetime) and logged at most once a minute. Value-only below the runtime section.
namespace AutoWowPullCap
{
struct Params
{
    std::uint32_t maxLevelAbove = 2;  // AutoWow.Survival.PullLevelCap.MaxLevelAbove
    std::uint32_t elitePenalty = 3;   // AutoWow.Survival.PullLevelCap.ElitePenalty
    std::uint32_t healthyPct = 80;    // AutoWow.Survival.PullLevelCap.HealthyPct
    std::uint32_t aloneYards = 15;    // AutoWow.Survival.PullLevelCap.AloneYards
};

// The target (elite penalised) sits more than maxLevelAbove above the bot.
[[nodiscard]] inline bool OverCap(Params const& p, std::uint32_t botLevel, std::uint32_t targetLevel, bool elite)
{
    return targetLevel + (elite ? p.elitePenalty : 0) > botLevel + p.maxLevelAbove;
}

// An over-cap target is still pulled when it is a quest objective, the bot is healthy and the target
// has no idle hostile neighbour (neighbours = count within AloneYards, the target itself excluded).
[[nodiscard]] inline bool QuestException(Params const& p, bool questObjective, std::uint32_t hpPct,
                                         std::uint32_t manaPct, bool usesMana, std::uint32_t neighbours)
{
    bool const healthy = hpPct >= p.healthyPct && (!usesMana || manaPct >= p.healthyPct);
    return questObjective && healthy && neighbours == 0;
}

// ---- runtime (PullLevelCap.cpp) -----------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }
inline Params const& Get() { return detail::gParams; }

void LoadConfig();
// Counts one over-cap candidate (skipped, or pulled under the quest exception); logs the totals at most
// once per minute of game time. Thread-safe (map threads).
void Note(bool skipped);
}  // namespace AutoWowPullCap

#endif  // AUTOWOW_PULL_LEVEL_CAP_H
