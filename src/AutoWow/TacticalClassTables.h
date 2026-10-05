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
//   escapeRelaxed: extra escape tools read only with AutoWow.Tactics.EscapeRelaxed (kCdEscape readiness)
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
    std::string_view escapeRelaxed[3];
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
    {"Warlock",
     {"",
      "shadow bolt:0.5",
      "life tap:0,shadow bolt:0,incinerate:0,soul fire:0,immolate:0,immolate on attacker:0,"
         "corruption on attacker:0,curse of agony on attacker:0,drain soul:0,health funnel:0",
      "life tap:0,shadow bolt:0,incinerate:0,soul fire:0,immolate:0,immolate on attacker:0,"
         "corruption:0,corruption on attacker:0,curse of agony:0,curse of agony on attacker:0,"
         "drain soul:0,drain life:0,health funnel:0,shoot:0"},
     {"fear", "howl of terror"},
     {"death coil"},
     {"howl of terror"},
     70,
     {}},  // Warlock
    {"Mage",
     {"",
      "pyroblast:0,evocation:0",
      "pyroblast:0,fireball:0.5,arcane missiles:0,evocation:0,flamestrike:0,blizzard:0",
      "frostbolt:0,fireball:0,pyroblast:0,arcane missiles:0,fire blast:0,scorch:0,"
         "frostfire bolt:0,ice lance:0,arcane blast:0,arcane barrage:0,evocation:0,flamestrike:0,"
         "blizzard:0,cone of cold:0,shoot:0"},
     {"frost nova"},
     {"ice block", "ice barrier", "mana shield"},
     {},  // no escape tool: Blink + run-away died 10 of 11 times in S20; a mage fights on in emergency
     50,
     {}},  // Mage
    {"Hunter",
     {"",
      "",
      "aimed shot:0,volley:0",
      "auto shot:0,arcane shot:0,serpent sting:0,serpent sting on attacker:0,multi-shot:0,"
         "aimed shot:0,steady shot:0,raptor strike:0,mongoose bite:0,volley:0,hunter's mark:0,"
         "melee:0,reach melee:0"},
     {"freezing trap", "scare beast", "intimidation"},
     {"deterrence"},
     {"feign death", "disengage"},
     50,
     {}},  // Hunter
    {"Rogue",
     {"rupture:0,expose armor:0,feint:0",
      "rupture:0,expose armor:0,feint:0,slice and dice:0.5",
      "rupture:0,expose armor:0,feint:0,slice and dice:0",
      "rupture:0,expose armor:0,feint:0,slice and dice:0,sinister strike:0,eviscerate:0,"
         "backstab:0,mutilate:0,kick:0,melee:0,reach melee:0"},
     {"gouge", "kidney shot", "blind"},
     {"evasion"},
     {"vanish", "sprint"},
     50,
     {}},  // Rogue
    {"Warrior",
     {"sunder armor:0,heroic strike:0.5,rend on attacker:0",
      "sunder armor:0,heroic strike:0,rend on attacker:0,rend:0.5",
      "sunder armor:0,heroic strike:0,cleave:0,bloodrage:0,rend:0,rend on attacker:0,charge:0,"
         "mocking blow:0",
      "sunder armor:0,heroic strike:0,cleave:0,bloodrage:0,rend:0,rend on attacker:0,charge:0,"
         "mocking blow:0,overpower:0,mortal strike:0,execute:0,slam:0,whirlwind:0,bloodthirst:0,"
         "melee:0,reach melee:0"},
     {"intimidating shout"},
     {"retaliation", "last stand"},  // no Shield Wall: it needs a shield (read "ready" on 2H warriors)
     {},
     50,
     {}},  // Warrior
    {"Paladin",
     {"",
      "",
      "consecration:0,holy wrath:0",  // offence on: heals alone never killed the mob (S20: 0 kills, 180 s)
      "consecration:0,exorcism:0,holy wrath:0,crusader strike:0,divine storm:0,judgement:0,"
         "judgement of light:0,judgement of wisdom:0,hammer of wrath:0,melee:0,reach melee:0"},
     {"hammer of justice"},
     {"divine protection", "divine shield", "lay on hands"},
     {},  // no escape tool: bubble + run-away died 4 of 4 times in S20; bubble is an emergency heal window
     50,
     {}},  // Paladin
    {"Druid",
     {"",
      "wrath:0.3,starfire:0.3,moonfire:0.5",
      "wrath:0,starfire:0,moonfire:0,insect swarm:0,hurricane:0,moonfire on attacker:0,"
         "insect swarm on attacker:0",
      "wrath:0,starfire:0,moonfire:0,insect swarm:0,hurricane:0,moonfire on attacker:0,"
         "insect swarm on attacker:0,claw:0,shred:0,rake:0,rip:0,ferocious bite:0,maul:0,"
         "swipe (bear):0,mangle (bear):0,mangle (cat):0,lacerate:0,melee:0,reach melee:0"},
     {"bash", "entangling roots"},
     {"barkskin", "frenzied regeneration"},
     {"dash"},
     50,
     {"entangling roots"}},  // Druid (EscapeRelaxed: roots the target, cat form + Dash, flee)
    {"Shaman",
     {"magma totem:0,chain lightning:0.5,fire nova:0",
      "",
      "lightning bolt:0,chain lightning:0,lava burst:0,searing totem:0,magma totem:0,fire nova:0",
      "lightning bolt:0,chain lightning:0,lava burst:0,searing totem:0,magma totem:0,fire nova:0,"
         "stormstrike:0,lava lash:0,earth shock:0,flame shock:0,melee:0,reach melee:0"},
     {"stoneclaw totem", "earthbind totem"},
     {"shamanistic rage"},
     {"ghost wolf"},
     50,
     {"earthbind totem", "frost shock"}},  // Shaman (EscapeRelaxed: snare, Ghost Wolf, flee)
    {"DeathKnight",
     {"",
      "",
      "army of the dead:0,death and decay:0",
      "army of the dead:0,death and decay:0,blood boil:0,pestilence:0,death grip:0,obliterate:0,"
         "scourge strike:0,heart strike:0,frost strike:0,blood strike:0,plague strike:0,icy touch:0,"
         "howling blast:0,rune strike:0,death coil:0,melee:0,reach melee:0"},
     {"strangulate", "chains of ice"},
     {"icebound fortitude", "anti-magic shell"},
     {},
     50,
     {}},  // DeathKnight
};
}  // namespace AutoWowTactics

#endif  // AUTOWOW_TACTICAL_CLASS_TABLES_H
