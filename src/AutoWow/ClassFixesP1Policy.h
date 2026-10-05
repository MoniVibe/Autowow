/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_CLASS_FIXES_P1_POLICY_H
#define AUTOWOW_CLASS_FIXES_P1_POLICY_H

#include <cstdint>
#include <vector>

// AutoWow.Combat.ClassFixesP1 (default 0): multi-mob kits of the four worst solo classes (S110-S116 died-per-
// fight with adds vs solo: rogue 42/8.5%, warrior 34/9.2%, druid 30/4.7%; shaman 84% of combat time mana-
// starved). Pure, header-only, integer math, no core types; the strategy (ClassFixesP1Strategy.cpp) fills
// Facts from the bot and maps action names onto Gate. Every condition needs a solo bot (no group, no instance)
// and every combat condition needs combat; outside that every gate is open (stock / tactical behaviour).
namespace AutoWowClassFixesP1
{
// Wire-stable condition ids of the "fix p1 ..." triggers; append only, ids never reused.
enum class Cond : std::uint8_t
{
    Adds = 0,           // >= 2 attackers on the bot
    ManyAdds = 1,       // >= ManyAdds attackers (single-target cc on an add)
    AddsOrHurt = 2,     // >= 2 attackers or hp < HurtHpPct (rogue Evasion)
    CasterKidney = 3,   // current target is a caster and >= KidneyCombo combo points
    RageStarved = 4,    // rage < StarvedRage (warrior Bloodrage at the pull)
    MaelstromHeal = 5,  // Maelstrom Weapon x5 and hp < MaelstromHealHpPct (instant Healing Wave)
    BearHold = 6,       // feral cat: enter bear at >= 2 attackers or hp < BearEnterHpPct; stay while >= 2 or
                        // hp < BearExitHpPct
    BearHurt = 7,       // in bear and hp < BearEnterHpPct (Frenzied Regeneration)
    WarriorControl = 8, // >= ManyAdds attackers, or >= 2 and hp < HurtHpPct (Intimidating Shout)
    Retaliation = 9,    // battle stance and (>= ManyAdds attackers, or >= 2 and hp < RetaliationHpPct)
    AoeOk = 10,         // >= 2 attackers and no attacker sits in a damage-breakable cc
    WaterShield = 11    // Water Shield known and missing (in or out of combat)
};

// Actions the strategy's multiplier gates (names -> Gate live in ClassFixesP1Strategy.cpp).
enum class Gate : std::uint8_t
{
    None = 0,
    LightningShield = 1,  // shaman: Water Shield replaces it once known
    ManaTotem = 2,        // shaman: Magma (27% base mana / 20 s), Call of the Elements (3-4 totems a pull)
    FireNova = 3,         // shaman
    EarthShock = 4,       // shaman: only above ShockManaPct
    FlameShock = 5,       // shaman: only above FlameShockManaPct
    HealCast = 6,         // shaman: cast-time heals only instant (Maelstrom x5) or below EmergencyHpPct
    CatForm = 7,          // druid: no Cat Form while BearHold holds (stops the cat/bear flap), unless escaping
    AoeDamage = 8         // all: no cleave / blade flurry / swipe / whirlwind while an attacker is in breakable cc
};

struct Facts
{
    bool solo = false;             // no group, not in a dungeon / raid / battleground / arena
    bool inCombat = false;
    std::uint32_t attackers = 0;   // units attacking the bot
    std::uint32_t hpPct = 100;
    std::uint32_t manaPct = 100;
    std::uint32_t rage = 0;        // rage points (0-100)
    std::uint32_t comboPoints = 0;
    std::uint32_t maelstrom = 0;   // Maelstrom Weapon stacks
    bool targetCaster = false;     // current target is a mage-class creature
    bool inBear = false;           // bear / dire bear form
    bool battleStance = false;
    bool knowsWaterShield = false;
    bool waterShieldUp = false;
    bool cced = false;             // an attacker sits in a damage-breakable cc (blind, gouge, fear, sap ...)
    bool escaping = false;         // the tactical layer runs an escape tactic
    bool tacticMulti = false;      // ... a multi tactic (druid: its "tac multi" casts Bear Form too)
};

struct Params
{
    std::uint32_t manyAdds = 3;             // AutoWow.Combat.ClassFixesP1.ManyAdds
    std::uint32_t hurtHpPct = 50;           // .HurtHpPct
    std::uint32_t kidneyCombo = 3;          // .KidneyCombo
    std::uint32_t starvedRage = 10;         // .StarvedRage
    std::uint32_t maelstromHealHpPct = 55;  // .MaelstromHealHpPct
    std::uint32_t bearEnterHpPct = 45;      // .BearEnterHpPct
    std::uint32_t bearExitHpPct = 60;       // .BearExitHpPct
    std::uint32_t retaliationHpPct = 60;    // .RetaliationHpPct
    std::uint32_t shockManaPct = 50;        // .ShockManaPct (Earth Shock)
    std::uint32_t flameShockManaPct = 30;   // .FlameShockManaPct
    std::uint32_t emergencyHpPct = 30;      // .EmergencyHpPct (cast-time heals allowed below)
};

inline bool BearHold(Facts const& f, Params const& p)
{
    std::uint32_t const line = f.inBear ? p.bearExitHpPct : p.bearEnterHpPct;
    return f.attackers >= 2 || f.hpPct < line;
}

inline bool Holds(Cond c, Facts const& f, Params const& p)
{
    if (!f.solo)
        return false;
    if (c == Cond::WaterShield)
        return f.knowsWaterShield && !f.waterShieldUp;
    if (!f.inCombat)
        return false;
    bool const adds = f.attackers >= 2;
    switch (c)
    {
        case Cond::Adds: return adds;
        case Cond::ManyAdds: return f.attackers >= p.manyAdds;
        case Cond::AddsOrHurt: return adds || f.hpPct < p.hurtHpPct;
        case Cond::CasterKidney: return f.targetCaster && f.comboPoints >= p.kidneyCombo;
        case Cond::RageStarved: return f.rage < p.starvedRage;
        case Cond::MaelstromHeal: return f.maelstrom >= 5 && f.hpPct < p.maelstromHealHpPct;
        case Cond::BearHold: return BearHold(f, p);
        case Cond::BearHurt: return f.inBear && f.hpPct < p.bearEnterHpPct;
        case Cond::WarriorControl: return f.attackers >= p.manyAdds || (adds && f.hpPct < p.hurtHpPct);
        case Cond::Retaliation:
            return f.battleStance && (f.attackers >= p.manyAdds || (adds && f.hpPct < p.retaliationHpPct));
        case Cond::AoeOk: return adds && !f.cced;
        default: return false;
    }
}

// May a gated action run? Open (true) for a grouped bot, and for the combat gates out of combat.
inline bool Allowed(Gate g, Facts const& f, Params const& p)
{
    if (!f.solo)
        return true;
    switch (g)
    {
        case Gate::LightningShield: return !f.knowsWaterShield;
        case Gate::ManaTotem: return false;
        case Gate::FireNova: return false;
        case Gate::EarthShock: return !f.inCombat || f.manaPct >= p.shockManaPct;
        case Gate::FlameShock: return !f.inCombat || f.manaPct >= p.flameShockManaPct;
        case Gate::HealCast: return !f.inCombat || f.maelstrom >= 5 || f.hpPct < p.emergencyHpPct;
        case Gate::CatForm: return !f.inCombat || f.escaping || (!BearHold(f, p) && !f.tacticMulti);
        case Gate::AoeDamage: return !f.cced;
        default: return true;
    }
}

// Single-target cc on an add (rogue Blind / Gouge): the attacker that is not the current target, not already
// in cc, with the most health (it would be fought last); ties -> lowest guid counter. -1 = none.
struct AddCandidate
{
    std::uint32_t guid = 0;
    std::uint32_t hpPct = 0;
    bool currentTarget = false;
    bool cced = false;
};

inline std::int32_t PickAdd(std::vector<AddCandidate> const& c)
{
    std::int32_t best = -1;
    for (std::size_t i = 0; i < c.size(); ++i)
    {
        if (c[i].currentTarget || c[i].cced)
            continue;
        if (best < 0 || c[i].hpPct > c[best].hpPct || (c[i].hpPct == c[best].hpPct && c[i].guid < c[best].guid))
            best = static_cast<std::int32_t>(i);
    }
    return best;
}

// The classes and talent tabs the kit covers (core class id; tab = AiFactory::GetPlayerSpecTab): warrior
// Arms / Fury, every rogue, Enhancement shaman, Feral druid (the caller also requires the cat strategy).
inline bool Covers(std::uint32_t classId, std::uint32_t tab)
{
    switch (classId)
    {
        case 1: return tab != 2;   // warrior, not Protection
        case 4: return true;       // rogue
        case 7: return tab == 1;   // shaman Enhancement
        case 11: return tab == 1;  // druid Feral
        default: return false;
    }
}
}  // namespace AutoWowClassFixesP1

#endif
