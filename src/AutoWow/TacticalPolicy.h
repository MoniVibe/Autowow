/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TACTICAL_POLICY_H
#define AUTOWOW_TACTICAL_POLICY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tactical combat layer (docs/TACTICAL_COMBAT_PLAN.md section 2). Pure, header-only, no core types:
// engagement assessment (load / capacity), tactic choice with hysteresis, and the per-bot A/B arm hash.
// Integer math only (loads and capacities are in centi mob-equivalents: a lone same-level normal = 100).
// Runtime adapter: TacticalRuntime.{h,cpp}. Flags: AutoWow.Tactics.Observe / .Enable (both default 0).
namespace AutoWowTactics
{
// Wire-stable (ledger `engage` tac0/tacs, `combat` cv=2 tac_ms); append only, ids never reused.
// id = 10 x Family + slot. Slots: 0 single, 1 alternate single (priest burst; reserved elsewhere),
// 2 multi, 3 emergency, 4 escape, 5-9 reserved. 0 = none / stock.
enum class TacticId : std::uint8_t
{
    None = 0,
    PriestWand = 10,       // single, sustain: shield + SW:P + wand to the end
    PriestBurst = 11,      // single, fast: stock nuke rotation while mana is high
    PriestMulti = 12,      // 2-3 mobs: DoT spread, shield/renew, Psychic Scream
    PriestEmergency = 13,  // low hp / fast death eta: heal-first, damage off except wand
    PriestEscape = 14,     // overwhelmed and tools spent: scream / fade / flee, one attempt per engagement
    WarlockSingle = 20,    // pet tanks, drain-tank (Drain Life / Life Tap discipline)
    WarlockMulti = 22,     // fear the add, DoT spread
    WarlockEmergency = 23,
    WarlockEscape = 24,
    MageSingle = 30,       // frost kite: nova + step back
    MageMulti = 32,        // polymorph the add, nova + cone
    MageEmergency = 33,
    MageEscape = 34,
    HunterSingle = 40,     // pet tanks, wing clip / disengage off melee
    HunterMulti = 42,      // trap the add
    HunterEmergency = 43,
    HunterEscape = 44,
    RogueSingle = 50,
    RogueMulti = 52,       // evasion + blade flurry, kidney the target
    RogueEmergency = 53,
    RogueEscape = 54,      // vanish / gouge + sprint
    WarriorSingle = 60,    // rage pacing, victory rush, hamstring runners
    WarriorMulti = 62,     // thunder clap + demo shout, intimidating shout
    WarriorEmergency = 63,
    WarriorEscape = 64,
    PaladinSingle = 70,
    PaladinMulti = 72,
    PaladinEmergency = 73,
    PaladinEscape = 74,
    DruidSingle = 80,
    DruidMulti = 82,       // bear form for adds
    DruidEmergency = 83,
    DruidEscape = 84,
    ShamanSingle = 90,
    ShamanMulti = 92,
    ShamanEmergency = 93,
    ShamanEscape = 94,
    DeathKnightSingle = 100,
    DeathKnightMulti = 102,
    DeathKnightEmergency = 103,
    DeathKnightEscape = 104
};

inline constexpr std::uint32_t kMaxTacticId = 128;  // telemetry array bound (ids < 128)

// Class family: the tens digit of its tactic ids. Wire-stable.
enum class Family : std::uint8_t
{
    None = 0,
    Priest = 1,
    Warlock = 2,
    Mage = 3,
    Hunter = 4,
    Rogue = 5,
    Warrior = 6,
    Paladin = 7,
    Druid = 8,
    Shaman = 9,
    DeathKnight = 10
};
inline constexpr std::uint32_t kFamilies = 11;  // array bound (Family values < 11)

inline constexpr std::uint32_t kSlotSingle = 0;
inline constexpr std::uint32_t kSlotMulti = 2;
inline constexpr std::uint32_t kSlotEmergency = 3;
inline constexpr std::uint32_t kSlotEscape = 4;
inline constexpr std::uint32_t SlotOf(TacticId id) { return static_cast<std::uint32_t>(id) % 10; }
inline constexpr Family FamilyOf(TacticId id) { return static_cast<Family>(static_cast<std::uint32_t>(id) / 10); }
inline constexpr TacticId TacticOf(Family f, std::uint32_t slot)
{
    return static_cast<TacticId>(static_cast<std::uint32_t>(f) * 10 + slot);
}

// Readiness bits (EngagementSnapshot::cds). Priest names; every other family reads bit 0/3 as its control
// tool (fear / stun / nova / shout ...) and bit 1/4 as its defensive cooldown (see kCdControl ...).
inline constexpr std::uint32_t kCdScream = 1u << 0;  // Psychic Scream known and off cooldown
inline constexpr std::uint32_t kCdShield = 1u << 1;  // PW:Shield known and no Weakened Soul
inline constexpr std::uint32_t kCdWand = 1u << 2;    // wand equipped
inline constexpr std::uint32_t kCdScreamKnown = 1u << 3;
inline constexpr std::uint32_t kCdShieldKnown = 1u << 4;
inline constexpr std::uint32_t kCdPet = 1u << 5;         // combat pet alive
inline constexpr std::uint32_t kCdEscape = 1u << 6;      // escape tool known and ready (vanish, blink, FD ...)
inline constexpr std::uint32_t kCdEscapeKnown = 1u << 7;
inline constexpr std::uint32_t kCdControl = kCdScream;
inline constexpr std::uint32_t kCdDefensive = kCdShield;
inline constexpr std::uint32_t kCdControlKnown = kCdScreamKnown;
inline constexpr std::uint32_t kCdDefensiveKnown = kCdShieldKnown;

// Creature rank as the core stores it (CreatureEliteType); static_assert'ed in the runtime adapter.
inline constexpr std::uint32_t kRankNormal = 0;
inline constexpr std::uint32_t kRankElite = 1;
inline constexpr std::uint32_t kRankRareElite = 2;
inline constexpr std::uint32_t kRankBoss = 3;
inline constexpr std::uint32_t kRankRare = 4;

struct LoadParams
{
    std::uint32_t eliteMulPct = 300;  // AutoWow.Tactics.Load.EliteMulPct
    std::uint32_t rareMulPct = 150;   // AutoWow.Tactics.Load.RareMulPct
    std::uint32_t lvlStepPct = 15;    // AutoWow.Tactics.Load.LvlStepPct (per level of delta)
    std::uint32_t casterMulPct = 120; // AutoWow.Tactics.Load.CasterMulPct
};

// w(m) = rankMul x clamp(1 + LvlStep*dlvl, 0.4, 2.0) x (caster ? CasterMul : 1), in centi mob-equivalents.
inline std::uint32_t MobWeight(std::uint32_t rank, std::int32_t lvlDelta, bool caster, LoadParams const& p)
{
    std::uint64_t rankMul = 100;
    if (rank == kRankElite || rank == kRankBoss)
        rankMul = p.eliteMulPct;
    else if (rank == kRankRareElite)
        rankMul = std::uint64_t(p.eliteMulPct) * p.rareMulPct / 100;
    else if (rank == kRankRare)
        rankMul = p.rareMulPct;
    std::int64_t lvl = 100 + std::int64_t(p.lvlStepPct) * lvlDelta;
    lvl = lvl < 40 ? 40 : (lvl > 200 ? 200 : lvl);
    std::uint64_t w = rankMul * std::uint64_t(lvl) / 100;
    if (caster)
        w = w * p.casterMulPct / 100;
    return static_cast<std::uint32_t>(w);
}

// The assessment value (plan 2.1). Filled by the runtime adapter every ReevalMs while in combat;
// deathEtaMs is derived by the EngagementTracker from its own HP samples.
struct EngagementSnapshot
{
    std::uint32_t attackers = 0;       // units attacking the bot
    std::uint32_t melee = 0;           // attackers in melee range of the bot
    std::uint32_t fearableMelee = 0;   // ... of which not immune to the control tool (priest: Psychic Scream)
    std::uint32_t casters = 0;         // mana-class creature attackers
    std::uint32_t addsNear = 0;        // idle hostiles within LinkRadius of any attacker
    std::uint32_t eliteN = 0;
    std::uint32_t rareN = 0;
    std::int32_t lvlDmax = 0;          // max(attacker level - bot level)
    std::uint32_t load = 0;            // sum of w(m) over attackers + addsNear (centi)
    std::uint32_t hpPct = 100;
    std::uint32_t manaPct = 100;
    std::uint32_t deathEtaMs = 0;      // 0 = not dropping / unknown
    std::uint32_t cds = 0;             // kCd* bits
    std::uint32_t feared = 0;          // attackers currently feared
    bool targetCaster = false;         // current target is a mana-class creature
    bool pvp = false;                  // a player is among the attackers
};

struct PriestParams
{
    std::uint32_t singleMax = 120;          // AutoWow.Tactics.Priest.SingleMaxPct (centi load)
    std::uint32_t base = 100;               // AutoWow.Tactics.Priest.BasePct (centi capacity)
    std::uint32_t screamBonus = 100;        // capacity per ready Psychic Scream (centi)
    std::uint32_t shieldBonus = 50;         // capacity per ready PW:Shield (centi)
    std::uint32_t escapeRatioPct = 160;     // AutoWow.Tactics.EscapeRatioPct
    std::uint32_t wandManaPct = 60;         // AutoWow.Tactics.Priest.WandManaPct (below: wand-cycle)
    std::uint32_t burstExitManaPct = 50;    // AutoWow.Tactics.Priest.BurstExitManaPct (burst -> wand)
    std::int32_t cheapLvlMax = 1;           // a normal mob at most this many levels above is "cheap"
    std::uint32_t shieldMinManaPct = 20;    // AutoWow.Tactics.Priest.ShieldMinManaPct (wand only)
    std::uint32_t renewHpPct = 70;          // AutoWow.Tactics.Priest.RenewHpPct
    std::uint32_t healHpPct = 45;           // AutoWow.Tactics.Priest.HealHpPct
    std::uint32_t multiHealBonusPct = 10;   // multi: heal thresholds + this
    std::uint32_t emergencyHpPct = 30;      // AutoWow.Tactics.Priest.EmergencyHpPct (enter)
    std::uint32_t emergencyExitHpPct = 45;  // AutoWow.Tactics.Priest.EmergencyExitHpPct (leave)
    std::uint32_t deathEtaMs = 6000;        // AutoWow.Tactics.DeathEtaMs
    std::uint32_t screamMinMelee = 2;       // AutoWow.Tactics.Priest.ScreamMinMelee
    std::uint32_t screamHpPct = 60;         // AutoWow.Tactics.Priest.ScreamHpPct
    std::uint32_t minDwellMs = 3000;        // AutoWow.Tactics.MinDwellMs (any downgrade / lateral switch)
    std::uint32_t escapeMaxMs = 8000;       // AutoWow.Tactics.EscapeMaxMs (then re-evaluate; never re-enter)
    LoadParams load;
};

// Capacity C = (base + scream + shield bonuses) x hp% x (0.5 + 0.5 mana%), centi.
inline std::uint32_t Capacity(EngagementSnapshot const& s, PriestParams const& p)
{
    std::uint64_t c = p.base;
    if (s.cds & kCdScream)
        c += p.screamBonus;
    if (s.cds & kCdShield)
        c += p.shieldBonus;
    c = c * s.hpPct / 100;
    c = c * (200 + std::uint64_t(s.manaPct) * 2) / 400;  // (0.5 + 0.5 mana%) without rounding drift
    return static_cast<std::uint32_t>(c);
}

// Escalation order by slot (every family): single 1, multi 2, emergency 3, escape 4. A switch to a higher
// severity is immediate, anything else waits MinDwellMs.
inline std::uint32_t Severity(TacticId id)
{
    std::uint32_t const slot = SlotOf(id);
    if (id == TacticId::None || slot > kSlotEscape)
        return 0;
    return slot <= 1 ? 1 : slot;
}

// Escalation immediate; downgrades and lateral switches need minDwellMs in the current tactic; leaving an
// escape (after its hold) is always allowed.
inline TacticId Hysteresis(TacticId want, TacticId current, std::uint32_t msInCurrent, std::uint32_t minDwellMs)
{
    if (current == TacticId::None || want == current)
        return want;
    if (SlotOf(current) == kSlotEscape || Severity(want) > Severity(current))
        return want;
    return msInCurrent >= minDwellMs ? want : current;
}

template <class Params>
inline bool InEmergency(EngagementSnapshot const& s, Params const& p, bool alreadyIn)
{
    bool const etaBad = s.deathEtaMs && s.deathEtaMs < p.deathEtaMs;
    if (alreadyIn)  // leave only above the (higher) exit threshold with a safe eta
        return etaBad || s.hpPct <= p.emergencyExitHpPct;
    return etaBad || s.hpPct < p.emergencyHpPct;
}

// Raw desire from the snapshot (thresholds carry their own enter/exit hysteresis; dwell is applied by
// ChoosePriest). escapeUsed: the one escape attempt of this engagement is spent.
// Escape = overwhelmed (load > C x EscapeRatio) AND losing (hp below the emergency exit line) AND the
// control tools the bot knows are all on cooldown. A bot that knows no tool yet (levels 1-5) never escapes.
inline TacticId DesiredPriest(EngagementSnapshot const& s, PriestParams const& p, TacticId current, bool escapeUsed)
{
    std::uint32_t const cap = Capacity(s, p);
    bool const toolsKnown = (s.cds & (kCdScreamKnown | kCdShieldKnown)) != 0;
    bool const toolsSpent = toolsKnown && !(s.cds & (kCdScream | kCdShield));
    if (!escapeUsed && s.attackers >= 2 && toolsSpent && s.hpPct < p.emergencyExitHpPct &&
        std::uint64_t(s.load) * 100 > std::uint64_t(cap) * p.escapeRatioPct)
        return TacticId::PriestEscape;
    if (InEmergency(s, p, current == TacticId::PriestEmergency))
        return TacticId::PriestEmergency;
    if (s.load > p.singleMax)
        return TacticId::PriestMulti;

    // Single target. Cheap fight (lone normal mob, not a caster, <= cheapLvlMax above): wand first even at
    // full mana. Otherwise burst while mana lasts, handing off to the wand below BurstExitManaPct.
    bool const wand = (s.cds & kCdWand) != 0;
    bool const cheap = !s.eliteN && !s.rareN && !s.targetCaster && s.lvlDmax <= p.cheapLvlMax;
    if (!wand)
        return TacticId::PriestBurst;
    if (cheap)
        return TacticId::PriestWand;
    std::uint32_t const burstFloor = current == TacticId::PriestBurst ? p.burstExitManaPct : p.wandManaPct;
    return s.manaPct >= burstFloor ? TacticId::PriestBurst : TacticId::PriestWand;
}

// Hysteresis wrapper: escalation is immediate; downgrades and lateral switches need MinDwellMs in the
// current tactic. Escape holds for EscapeMaxMs, then re-evaluates (escapeUsed keeps it from re-entering).
inline TacticId ChoosePriest(EngagementSnapshot const& s, PriestParams const& p, TacticId current,
                             std::uint32_t msInCurrent, bool escapeUsed)
{
    if (current == TacticId::PriestEscape && msInCurrent < p.escapeMaxMs)
        return current;
    TacticId const want = DesiredPriest(s, p, current, escapeUsed || current == TacticId::PriestEscape);
    return Hysteresis(want, current, msInCurrent, p.minDwellMs);
}

// ---- every other class family (single / multi / emergency / escape) ------------------------------
// Same ladder as the priest without the wand/burst split: the class character lives in the strategy's
// triggers and factor tables (TacticalClassStrategy.cpp), not here. Config: AutoWow.Tactics.<Class>.*.
struct ClassParams
{
    std::uint32_t singleMax = 120;          // .SingleMaxPct (centi load)
    std::uint32_t base = 100;               // .BasePct (centi capacity)
    std::uint32_t controlBonus = 100;       // capacity per ready control tool
    std::uint32_t defensiveBonus = 50;      // capacity per ready defensive cooldown
    std::uint32_t petBonus = 100;           // capacity while a combat pet is alive (hunter / warlock tank)
    std::uint32_t escapeRatioPct = 160;     // AutoWow.Tactics.EscapeRatioPct
    std::uint32_t healHpPct = 50;           // .HealHpPct: self-heal / defensive floor while fighting
    std::uint32_t lowManaPct = 25;          // .LowManaPct: mana recovery (evocation, life tap) while fighting
    std::uint32_t emergencyHpPct = 30;      // .EmergencyHpPct (enter)
    std::uint32_t emergencyExitHpPct = 45;  // .EmergencyExitHpPct (leave)
    std::uint32_t controlMinMelee = 2;      // .ControlMinMelee: area control needs this many controllable melee
    std::uint32_t controlHpPct = 60;        // .ControlHpPct: ... and hp below it (or >= 3 attackers)
    std::uint32_t deathEtaMs = 6000;        // AutoWow.Tactics.DeathEtaMs
    std::uint32_t minDwellMs = 3000;        // AutoWow.Tactics.MinDwellMs
    std::uint32_t escapeMaxMs = 8000;       // AutoWow.Tactics.EscapeMaxMs
    LoadParams load;
};

// C = (base + control + defensive + pet bonuses) x hp% x (0.5 + 0.5 mana%); mana% = 100 for non-mana classes.
inline std::uint32_t Capacity(EngagementSnapshot const& s, ClassParams const& p)
{
    std::uint64_t c = p.base;
    if (s.cds & kCdControl)
        c += p.controlBonus;
    if (s.cds & kCdDefensive)
        c += p.defensiveBonus;
    if (s.cds & kCdPet)
        c += p.petBonus;
    c = c * s.hpPct / 100;
    c = c * (200 + std::uint64_t(s.manaPct) * 2) / 400;
    return static_cast<std::uint32_t>(c);
}

// Escape = overwhelmed (load > C x EscapeRatio) AND losing (hp below the emergency exit line) AND >= 2
// attackers AND the control/defensive tools the bot knows are all on cooldown (a bot that knows none
// fights on). The escape tool itself (vanish, blink, feign death ...) is what the escape tactic spends.
inline TacticId DesiredClass(Family f, EngagementSnapshot const& s, ClassParams const& p, TacticId current,
                             bool escapeUsed)
{
    std::uint32_t const cap = Capacity(s, p);
    bool const toolsKnown = (s.cds & (kCdControlKnown | kCdDefensiveKnown)) != 0;
    bool const toolsSpent = toolsKnown && !(s.cds & (kCdControl | kCdDefensive));
    if (!escapeUsed && s.attackers >= 2 && toolsSpent && s.hpPct < p.emergencyExitHpPct &&
        std::uint64_t(s.load) * 100 > std::uint64_t(cap) * p.escapeRatioPct)
        return TacticOf(f, kSlotEscape);
    if (InEmergency(s, p, current == TacticOf(f, kSlotEmergency)))
        return TacticOf(f, kSlotEmergency);
    if (s.load > p.singleMax)
        return TacticOf(f, kSlotMulti);
    return TacticOf(f, kSlotSingle);
}

inline TacticId ChooseClass(Family f, EngagementSnapshot const& s, ClassParams const& p, TacticId current,
                            std::uint32_t msInCurrent, bool escapeUsed)
{
    bool const escaping = current == TacticOf(f, kSlotEscape);
    if (escaping && msInCurrent < p.escapeMaxMs)
        return current;
    TacticId const want = DesiredClass(f, s, p, current, escapeUsed || escaping);
    return Hysteresis(want, current, msInCurrent, p.minDwellMs);
}

// Wire-stable condition ids of the non-priest "tac ..." triggers (TacticalClassStrategy.h registers them).
enum class ClassTacticCondition : std::uint8_t
{
    Single = 0,
    Multi = 1,
    Emergency = 2,
    Escape = 3,
    Heal = 4,              // single/multi, hp < HealHpPct (+10 in multi)
    Control = 5,           // multi, control ready, >= ControlMinMelee controllable melee on the bot,
                           // (hp < ControlHpPct or >= 3 attackers), no idle adds near (area fear/stun/nova)
    ControlAdd = 6,        // multi, control ready, >= 2 attackers, no idle adds near (single-target cc on the add)
    EmergencyControl = 7,  // emergency/escape, control ready, >= 1 controllable melee on the bot
    MeleeOnMe = 8,         // single/multi, a melee attacker on the bot
    Runner = 9,            // single/multi, the current target flees (not feared) and is not snared
    LowMana = 10,          // single/multi, mana user below LowManaPct
    LifeTap = 11,          // single/multi, mana below LowManaPct and hp >= EmergencyExitHpPct + 15
    PetLow = 12,           // single/multi, combat pet alive below 40 % hp
    Kite = 13              // single/multi, the current target is rooted/frozen in melee range (step out)
};

// Live facts a trigger reads besides the snapshot (filled by the adapter from the bot / pet / target).
struct TriggerFacts
{
    std::uint32_t hpPct = 100;
    std::uint32_t manaPct = 100;
    bool usesMana = false;
    bool petLow = false;               // combat pet alive below 40 % hp
    bool targetFleeing = false;        // current target flees, not feared, not snared
    bool targetRootedInMelee = false;  // current target rooted/frozen within melee range
};

// Does a "tac ..." condition hold while the non-priest tactic `id` runs?
inline bool ConditionHolds(ClassTacticCondition c, TacticId id, EngagementSnapshot const& s, ClassParams const& p,
                           TriggerFacts const& f)
{
    if (id == TacticId::None || FamilyOf(id) == Family::Priest)
        return false;
    std::uint32_t const slot = SlotOf(id);
    bool const single = slot == kSlotSingle;
    bool const multi = slot == kSlotMulti;
    bool const emergency = slot == kSlotEmergency;
    bool const escape = slot == kSlotEscape;
    bool const fighting = single || multi;
    bool const controlReady = (s.cds & kCdControl) != 0;
    switch (c)
    {
        case ClassTacticCondition::Single: return single;
        case ClassTacticCondition::Multi: return multi;
        case ClassTacticCondition::Emergency: return emergency;
        case ClassTacticCondition::Escape: return escape;
        case ClassTacticCondition::Heal: return fighting && f.hpPct < p.healHpPct + (multi ? 10 : 0);
        case ClassTacticCondition::Control:
            return multi && controlReady && s.fearableMelee >= p.controlMinMelee &&
                   (f.hpPct < p.controlHpPct || s.attackers >= 3) && !s.addsNear;
        case ClassTacticCondition::ControlAdd: return multi && controlReady && s.attackers >= 2 && !s.addsNear;
        case ClassTacticCondition::EmergencyControl:
            return (emergency || escape) && controlReady && s.fearableMelee >= 1;
        case ClassTacticCondition::MeleeOnMe: return fighting && s.melee >= 1;
        case ClassTacticCondition::Runner: return fighting && f.targetFleeing;
        case ClassTacticCondition::LowMana: return fighting && f.usesMana && f.manaPct < p.lowManaPct;
        case ClassTacticCondition::LifeTap:
            return fighting && f.usesMana && f.manaPct < p.lowManaPct && f.hpPct >= p.emergencyExitHpPct + 15;
        case ClassTacticCondition::PetLow: return fighting && f.petLow;
        case ClassTacticCondition::Kite: return fighting && f.targetRootedInMelee;
        default: return false;
    }
}

// Core class id (CLASS_WARRIOR 1 ... CLASS_DRUID 11; static_assert'ed in the adapter) -> family.
inline Family FamilyOfClass(std::uint32_t classId)
{
    switch (classId)
    {
        case 1: return Family::Warrior;
        case 2: return Family::Paladin;
        case 3: return Family::Hunter;
        case 4: return Family::Rogue;
        case 5: return Family::Priest;
        case 6: return Family::DeathKnight;
        case 7: return Family::Shaman;
        case 8: return Family::Mage;
        case 9: return Family::Warlock;
        case 11: return Family::Druid;
        default: return Family::None;
    }
}

// AutoWow.Tactics.Classes: class names separated by ',' or ' ' ("priest,warrior", "all") -> bit (1 << class
// id) per class. Unknown names are ignored.
inline std::uint32_t ParseClassMask(std::string_view text)
{
    static constexpr std::pair<std::string_view, std::uint32_t> kNames[] = {
        {"warrior", 1}, {"paladin", 2}, {"hunter", 3}, {"rogue", 4}, {"priest", 5}, {"deathknight", 6},
        {"dk", 6}, {"shaman", 7}, {"mage", 8}, {"warlock", 9}, {"druid", 11}};
    std::uint32_t mask = 0;
    while (!text.empty())
    {
        std::size_t const cut = text.find_first_of(", ");
        std::string_view const word = text.substr(0, cut);
        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);
        if (word == "all")
            for (auto const& [name, id] : kNames)
                mask |= 1u << id;
        for (auto const& [name, id] : kNames)
            if (word == name)
                mask |= 1u << id;
    }
    return mask;
}

// Per-bot A/B arm: 1 = treatment, 0 = control. Pure hash of the guid counter so it is reproducible
// across restarts and independent of login order (murmur3 fmix32).
inline std::uint32_t Hash32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

inline std::uint8_t ArmOf(std::uint32_t guidCounter, std::uint32_t salt, std::uint32_t armPct)
{
    return (Hash32(guidCounter ^ (salt * 0x9e3779b9u)) % 100) < armPct ? 1 : 0;
}

// ---- adapter tables (strings live here, never in the policy above) ------------------------------
// "action:factor,action:factor" -> (action, factor permille). Factor is a decimal with up to three
// fraction digits ("0", "0.5", "2"). Malformed entries are skipped; order preserved.
using FactorTable = std::vector<std::pair<std::string, std::uint32_t>>;

inline FactorTable ParseFactors(std::string_view text)
{
    FactorTable out;
    while (!text.empty())
    {
        std::size_t const comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        std::size_t const colon = item.rfind(':');
        if (colon == std::string_view::npos)
            continue;
        std::string_view name = item.substr(0, colon);
        std::string_view num = item.substr(colon + 1);
        while (!name.empty() && name.front() == ' ')
            name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ')
            name.remove_suffix(1);
        std::uint32_t whole = 0, frac = 0, fracDigits = 0;
        bool dot = false, ok = !num.empty();
        for (char ch : num)
        {
            if (ch == ' ')
                continue;
            if (ch == '.' && !dot)
                dot = true;
            else if (ch >= '0' && ch <= '9' && !dot && whole < 100000)
                whole = whole * 10 + std::uint32_t(ch - '0');
            else if (ch >= '0' && ch <= '9' && dot)
            {
                if (fracDigits < 3)
                {
                    frac = frac * 10 + std::uint32_t(ch - '0');
                    ++fracDigits;
                }
            }
            else
                ok = false;
        }
        if (!ok || name.empty())
            continue;
        while (fracDigits < 3)
        {
            frac *= 10;
            ++fracDigits;
        }
        out.emplace_back(std::string(name), whole * 1000 + frac);
    }
    return out;
}

// Permille factor of an action in a table; 1000 (unchanged) when absent.
inline std::uint32_t FactorOf(FactorTable const& table, std::string_view action)
{
    for (auto const& [name, permille] : table)
        if (name == action)
            return permille;
    return 1000;
}
}  // namespace AutoWowTactics

#endif  // AUTOWOW_TACTICAL_POLICY_H
