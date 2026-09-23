/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TACTICAL_RUNTIME_H
#define AUTOWOW_TACTICAL_RUNTIME_H

#include <cstdint>

#include "TacticalPolicy.h"

class Player;
class PlayerbotAI;
class SpellInfo;

// Runtime adapter of the tactical combat layer (docs/TACTICAL_COMBAT_PLAN.md). All default off:
//   AutoWow.Tactics.Observe (T1): assess every solo priest engagement, label the tactic it WOULD run, emit
//     one ledger `engage` line per engagement and the `combat` cv=2 tac_ms/arm fields. No behaviour change.
// Eligible = priest, not grouped, not in a dungeon/raid/battleground/arena. Per-bot state is keyed by guid
// counter under one mutex (bots update on map threads); never held while calling other subsystems.
namespace AutoWowTactics
{
namespace detail
{
inline bool gObserve = false;
inline bool gEnable = false;  // T2 master; tracking also runs under it
}  // namespace detail

inline bool Tracking() { return detail::gObserve || detail::gEnable; }
inline bool Enabled() { return detail::gEnable; }

// Reads AutoWow.Tactics.*. Called once at world init.
void LoadConfig();

// ---- T1: observation (bot AI tick / hook side) ---------------------------------------------------
// Bot AI tick. No-op unless Tracking(); re-evaluates every ReevalMs and on every combat-state change.
void Update(PlayerbotAI* botAI);
// A creature died to the bot's (or its pet's) killing blow.
void NoteKill(std::uint32_t botGuid);
// A non-triggered spell cast by a player (filtered to tracked priests inside).
void NoteCast(Player* player, SpellInfo const* spellInfo);
void Forget(std::uint32_t botGuid);

}  // namespace AutoWowTactics

#endif  // AUTOWOW_TACTICAL_RUNTIME_H
