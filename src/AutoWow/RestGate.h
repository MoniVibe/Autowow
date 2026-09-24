/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_REST_GATE_H
#define AUTOWOW_REST_GATE_H

#include <cstdint>

#include "PackRisk.h"

class PlayerbotAI;

// Universal rest gate for solo independent AutoWoW bots (AutoWow.Survival.RestGate, default 0).
// soak-s14-full-r1: the priest pre-pull gate (AutoWow.Tactics, treatment arm only) took priests from
// 9.28 to 1.36 deaths/h; every other solo bot still pulled the next mob right after a fight or a rez.
// With the flag on, GrindTargetValue returns no proactive target (legacy grind and the strict quest-kill
// pick) while hp < MinHpPct, mana < MinManaPct (mana users) or Resurrection Sickness lasts; the "food"
// strategy eats/drinks up to the thresholds when the bot has food/drink, otherwise it just regenerates.
// Self-defence (attackers) is never gated. Same threshold rule as the tactics gate (AutoWowPackRisk::
// HoldPull). Value-only below the runtime section.
namespace AutoWowRestGate
{
inline constexpr std::uint32_t kResurrectionSicknessAura = 15007;

struct Params
{
    std::uint32_t minHpPct = 70;    // AutoWow.Survival.RestGate.MinHpPct (0 = no hp threshold)
    std::uint32_t minManaPct = 60;  // AutoWow.Survival.RestGate.MinManaPct (0 = no mana threshold)
};

// No proactive pull now: below a threshold, or resurrection sickness still on.
[[nodiscard]] inline bool Hold(Params const& p, std::uint32_t hpPct, std::uint32_t manaPct, bool usesMana, bool sick)
{
    return sick || AutoWowPackRisk::HoldPull(hpPct, manaPct, usesMana, p.minHpPct, p.minManaPct);
}

// Eat / drink triggers (out of combat): below the matching pull threshold.
[[nodiscard]] inline bool NeedHealth(Params const& p, std::uint32_t hpPct) { return p.minHpPct && hpPct < p.minHpPct; }

[[nodiscard]] inline bool NeedMana(Params const& p, std::uint32_t manaPct, bool usesMana)
{
    return usesMana && p.minManaPct && manaPct < p.minManaPct;
}

// ---- runtime (RestGate.cpp) ----------------------------------------------------------------------
// Eligible: independent party bot, not grouped (or a group of one), alive, open world (no dungeon,
// raid, battleground or arena). Every query is false for anyone else.
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }

// Reads AutoWow.Survival.RestGate.*. Called once at world init.
void LoadConfig();
bool HoldProactivePull(PlayerbotAI* botAI);
bool NeedsRestHealth(PlayerbotAI* botAI);
bool NeedsRestMana(PlayerbotAI* botAI);
}  // namespace AutoWowRestGate

#endif  // AUTOWOW_REST_GATE_H
