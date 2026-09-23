/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_PACK_RISK_H
#define AUTOWOW_PACK_RISK_H

#include <cstdint>

// Pull selection risk (docs/TACTICAL_COMBAT_PLAN.md 2.4). Pure: the GrindTargetValue adapter feeds the
// candidate's weight plus the weights of idle hostiles within LinkRadius of it (social aggro), all as
// TacticalPolicy MobWeight() centi mob-equivalents. Patrol crossing is not scored yet (plan lane T3).
namespace AutoWowPackRisk
{
struct Verdict
{
    std::uint32_t risk = 0;  // candidate + linked neighbours, centi
    std::uint32_t band = 0;  // sort key: whole mob-equivalents (risk / 100)
    bool reject = false;     // risk > capacity x EscapeRatio: never pull
};

// neighbourSum excludes the candidate itself. capacity is TacticalPolicy Capacity() at pull time.
inline Verdict Score(std::uint32_t candidateWeight, std::uint32_t neighbourSum, std::uint32_t capacity,
                     std::uint32_t escapeRatioPct)
{
    Verdict v;
    v.risk = candidateWeight + neighbourSum;
    v.band = v.risk / 100;
    // A lone candidate is never rejected here: the level/elite screens already own single-mob legality.
    v.reject = neighbourSum && std::uint64_t(v.risk) * 100 > std::uint64_t(capacity) * escapeRatioPct;
    return v;
}

// Pre-pull readiness gate (plan 3.2): no proactive pull below these; 0 disables a threshold.
// Self-defence is never gated (attackers are returned before the grind selector consults this).
inline bool HoldPull(std::uint32_t hpPct, std::uint32_t manaPct, bool usesMana, std::uint32_t minHpPct,
                     std::uint32_t minManaPct)
{
    return (minHpPct && hpPct < minHpPct) || (usesMana && minManaPct && manaPct < minManaPct);
}
}  // namespace AutoWowPackRisk

#endif  // AUTOWOW_PACK_RISK_H
