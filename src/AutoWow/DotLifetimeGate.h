/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_DOT_LIFETIME_GATE_H
#define AUTOWOW_DOT_LIFETIME_GATE_H

#include <cstdint>
#include <string_view>

class PlayerbotAI;
class Unit;

// C5 DoT/debuff lifetime gate (AutoWow.Combat.DotLifetimeGate, default 0). Stops a bot from applying or
// refreshing a damage-over-time / debuff on a target that will die before it pays off. Pure predicate
// below (unit-tested); Allows() is the runtime adapter used by the debuff trigger/action sites.
namespace AutoWowDotLifetimeGate
{
// Wire-stable; append only.
enum class Kind : std::uint8_t
{
    None = 0,    // not gated
    Dot = 1,     // value accrues over the aura duration
    Debuff = 2   // value is immediate (mark, curses, armor/attack debuffs): only the HP floor applies
};

enum class Verdict : std::uint8_t
{
    Allow = 0,
    SkipLowHp = 1,           // trash below the HP floor
    SkipShortTtk = 2,        // estimated TTK shorter than MinDurationPct of the DoT duration
    SkipRefreshOutlives = 3  // refresh while the running aura already outlasts the estimated TTK
};

// Closed spell-name table (lower-case, as the strategies name them). Evaluated only when the gate is on.
inline Kind Classify(std::string_view spell)
{
    static constexpr std::string_view kDots[] = {
        "corruption", "curse of agony", "curse of doom", "immolate", "unstable affliction", "siphon life",
        "seed of corruption", "shadow word: pain", "vampiric touch", "devouring plague", "moonfire",
        "insect swarm", "rake", "rip", "lacerate", "serpent sting", "rupture", "garrote", "rend",
        "frost fever", "blood plague", "icy touch", "plague strike", "flame shock", "living bomb"};
    static constexpr std::string_view kDebuffs[] = {
        "hunter's mark", "curse of the elements", "curse of weakness", "curse of exhaustion",
        "curse of tongues", "sunder armor", "faerie fire", "faerie fire (feral)", "expose armor"};
    for (std::string_view s : kDots)
        if (s == spell)
            return Kind::Dot;
    for (std::string_view s : kDebuffs)
        if (s == spell)
            return Kind::Debuff;
    return Kind::None;
}

struct Params
{
    std::uint32_t minTargetHpPct = 10;   // AutoWow.Combat.DotMinTargetHpPct
    std::uint32_t minDurationPct = 50;   // AutoWow.Combat.DotMinDurationPct
    std::uint32_t highHpPerLevel = 150;  // AutoWow.Combat.DotHighHpPerLevel (abs HP floor = bot level x this)
};

struct Input
{
    Kind kind = Kind::None;
    std::uint32_t hpPct = 100;       // target current HP percent (floor)
    std::uint64_t hp = 0;            // target current HP
    bool bossOrElite = false;        // creature rank elite/rare-elite/boss, dungeon/world boss, or not a creature
    std::uint32_t botLevel = 0;
    std::uint64_t estTtkMs = 0;      // 0 = unknown
    std::uint32_t durationMs = 0;    // full aura duration; 0 = unknown
    std::uint32_t remainingMs = 0;   // > 0 = refresh of the bot's own running aura
};

inline Verdict Evaluate(Input const& in, Params const& p)
{
    if (in.kind == Kind::None || in.bossOrElite)
        return Verdict::Allow;
    // High-HP exception to the HP floor only: 10% of a big pool is still a lot of health. The TTK rules
    // below still apply, because the TTK estimate already accounts for the absolute HP.
    bool const highHp = p.highHpPerLevel && in.hp >= std::uint64_t(in.botLevel) * p.highHpPerLevel;
    if (!highHp && in.hpPct < p.minTargetHpPct)
        return Verdict::SkipLowHp;
    if (in.kind != Kind::Dot || !in.estTtkMs)
        return Verdict::Allow;
    if (in.remainingMs && in.estTtkMs <= in.remainingMs)
        return Verdict::SkipRefreshOutlives;
    if (in.durationMs && in.estTtkMs * 100 < std::uint64_t(in.durationMs) * p.minDurationPct)
        return Verdict::SkipShortTtk;
    return Verdict::Allow;
}

inline constexpr std::uint64_t kObservedDpsMinMs = 2000;

// TTK estimate: the target's observed HP loss since the bot engaged it (everyone's damage, i.e. group
// DPS; assumes the target was at full HP when engaged), floored by the bot's own recent DPS. 0 = unknown.
inline std::uint64_t EstimateTtkMs(std::uint64_t hp, std::uint64_t maxHp, std::uint64_t engagedMs,
                                   std::uint32_t botRecentDps)
{
    std::uint64_t dps = botRecentDps;
    if (engagedMs >= kObservedDpsMinMs && maxHp > hp)
    {
        std::uint64_t const observed = (maxHp - hp) * 1000 / engagedMs;
        if (observed > dps)
            dps = observed;
    }
    return dps ? hp * 1000 / dps : 0;
}

// Runtime adapter. True when the flag is off, the spell is not in the table, or the predicate allows.
// World/map-thread (bot AI update). A skip is counted into the bot's combat telemetry (dot_skips).
bool Allows(PlayerbotAI* botAI, Unit* target, std::string_view spell);

namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }
}  // namespace AutoWowDotLifetimeGate

#endif  // AUTOWOW_DOT_LIFETIME_GATE_H
