/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TACTICAL_CLASS_TABLES_H
#define AUTOWOW_TACTICAL_CLASS_TABLES_H

#include <cstdint>
#include <string_view>

#include "TacticalPolicy.h"

// Adapter tables of the non-priest tactical families (strings live here, never in TacticalPolicy.h's
// policy core). Pure data, shared by the runtime (TacticalRuntime.cpp), the strategies
// (TacticalClassStrategy.cpp) and the contract test. A family with an empty `key` has no tactics yet and is
// never tracked, whatever AutoWow.Tactics.Classes says.
//   key:       AutoWow.Tactics.<key>.* config prefix
//   factors:   default AutoWow.Tactics.<key>.Factors.{Single,Multi,Emergency,Escape} ("action:factor,...")
//   control:   control tools, first known wins for the immunity check; ready = any known + off cooldown
//   defensive: defensive cooldowns (capacity bonus, `engage` shield_n)
//   escape:    escape tools (kCdEscape readiness)
//   healHpPct: default .HealHpPct
namespace AutoWowTactics
{
struct ClassTable
{
    std::string_view key;
    std::string_view factors[4];
    std::string_view control[3];
    std::string_view defensive[3];
    std::string_view escape[3];
    std::uint32_t healHpPct = 50;
};

inline constexpr std::string_view kClassSlotKeys[4] = {"Single", "Multi", "Emergency", "Escape"};
inline constexpr std::uint32_t kClassSlots[4] = {kSlotSingle, kSlotMulti, kSlotEmergency, kSlotEscape};

// Index into ClassTable::factors for a slot; 4 = the slot has no table (priest burst / reserved).
inline constexpr std::uint32_t FactorIndexOfSlot(std::uint32_t slot)
{
    return slot == kSlotSingle ? 0 : slot == kSlotMulti ? 1 : slot == kSlotEmergency ? 2 : slot == kSlotEscape ? 3 : 4;
}

// Indexed by Family. Priest keeps its own tables (TacticalRuntime.cpp, AutoWow.Tactics.Priest.*).
inline constexpr ClassTable kClassTables[kFamilies] = {
    {},  // None
    {},  // Priest
    {},  // Warlock
    {},  // Mage
    {},  // Hunter
    {"Rogue",
     {"rupture:0,expose armor:0,feint:0",
      "rupture:0,expose armor:0,feint:0,slice and dice:0.5",
      "rupture:0,expose armor:0,feint:0,slice and dice:0",
      "rupture:0,expose armor:0,feint:0,slice and dice:0,sinister strike:0,eviscerate:0,"
         "backstab:0,mutilate:0,kick:0,melee:0,reach melee:0"},
     {"gouge", "kidney shot", "blind"},
     {"evasion"},
     {"vanish", "sprint"},
     50},  // Rogue
    {"Warrior",
     {"sunder armor:0,heroic strike:0.5,rend on attacker:0",
      "sunder armor:0,heroic strike:0,rend on attacker:0,rend:0.5",
      "sunder armor:0,heroic strike:0,cleave:0,bloodrage:0,rend:0,rend on attacker:0,charge:0,"
         "mocking blow:0",
      "sunder armor:0,heroic strike:0,cleave:0,bloodrage:0,rend:0,rend on attacker:0,charge:0,"
         "mocking blow:0,overpower:0,mortal strike:0,execute:0,slam:0,whirlwind:0,bloodthirst:0,"
         "melee:0,reach melee:0"},
     {"intimidating shout"},
     {"shield wall", "retaliation", "last stand"},
     {},
     50},  // Warrior
    {},  // Paladin
    {},  // Druid
    {},  // Shaman
    {},  // DeathKnight
};
}  // namespace AutoWowTactics

#endif  // AUTOWOW_TACTICAL_CLASS_TABLES_H
