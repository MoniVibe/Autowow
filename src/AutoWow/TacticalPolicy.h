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
// Namespaced by class family: priest 10-19 (warlock 20-29, ... reserved). 0 = none / stock.
enum class TacticId : std::uint8_t
{
    None = 0,
    PriestWand = 10,       // single, sustain: shield + SW:P + wand to the end
    PriestBurst = 11,      // single, fast: stock nuke rotation while mana is high
    PriestMulti = 12,      // 2-3 mobs: DoT spread, shield/renew, Psychic Scream
    PriestEmergency = 13,  // low hp / fast death eta: heal-first, damage off except wand
    PriestEscape = 14      // overwhelmed and tools spent: scream / fade / flee, one attempt per engagement
};

inline constexpr std::uint32_t kMaxTacticId = 32;  // telemetry array bound (ids < 32)

// Readiness bits (EngagementSnapshot::cds).
inline constexpr std::uint32_t kCdScream = 1u << 0;  // Psychic Scream known and off cooldown
inline constexpr std::uint32_t kCdShield = 1u << 1;  // PW:Shield known and no Weakened Soul
inline constexpr std::uint32_t kCdWand = 1u << 2;    // wand equipped
inline constexpr std::uint32_t kCdScreamKnown = 1u << 3;
inline constexpr std::uint32_t kCdShieldKnown = 1u << 4;

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
    std::uint32_t fearableMelee = 0;   // ... of which not immune to Psychic Scream
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

// Escalation order; a switch to a higher severity is immediate, anything else waits MinDwellMs.
inline std::uint32_t Severity(TacticId id)
{
    switch (id)
    {
        case TacticId::PriestWand:
        case TacticId::PriestBurst: return 1;
        case TacticId::PriestMulti: return 2;
        case TacticId::PriestEmergency: return 3;
        case TacticId::PriestEscape: return 4;
        default: return 0;
    }
}

inline bool InEmergency(EngagementSnapshot const& s, PriestParams const& p, bool alreadyIn)
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
    if (current == TacticId::None || want == current)
        return want;
    if (current == TacticId::PriestEscape || Severity(want) > Severity(current))
        return want;
    return msInCurrent >= p.minDwellMs ? want : current;
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
