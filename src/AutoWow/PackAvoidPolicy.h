/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_PACK_AVOID_POLICY_H
#define AUTOWOW_PACK_AVOID_POLICY_H

#include <cstdint>
#include <vector>

#include "PackRisk.h"
#include "WalkingV2Policy.h"

// AutoWow.Survival.PackAvoid (default 0). soak-s21-full-r1 (LOWMOB_DEATHS_S21 cause 2): 66% of the tactics
// classes' deaths were multi-mob fights against 17% of their wins, 20 of the 22 deaths to a mob 2+ levels
// below were multi-mob, and 22 deaths had 4+ attackers - the "low" killer was the last hit of a pack.
// Pack-risk pull choice existed only for the tactics treatment arm (AutoWowTactics::ScorePull). With the
// flag on every solo independent bot (any class) scores each proactive grind / quest-kill candidate:
//   risk = w(candidate) + sum w(idle, not-fighting creatures friendly to the candidate within LinkYards),
//   w = TacticalPolicy MobWeight (rank x level delta x caster), centi mob-equivalents;
//   capacity = the tactics capacity of the bot's class (tools ready, hp, mana) x GearPct x CapacityPct,
//   GearPct = main-hand DPS / level expectation (AutoWowGear), clamped to [MinGearPct, 100] for the
//   weapon classes (warrior, paladin, hunter, rogue, death knight), 100 for the others;
//   a candidate with linked neighbours and risk > capacity is skipped; the rest sort lone-first (band).
// Travel.Safe chunk choice also counts packs: a mob by the path with neighbours within LinkYards is a
// threat whatever its level, weighted by the pack size. The tactics treatment arm keeps its own scorer.
//
// Value-only, integer centi / yards, no floats in decisions, no RNG.
namespace AutoWowPackAvoid
{
inline constexpr std::uint8_t kPolicyVersion = 1;

struct Params
{
    std::uint32_t linkYards = 12;    // AutoWow.Survival.PackAvoid.LinkYards
    std::uint32_t capacityPct = 100; // AutoWow.Survival.PackAvoid.CapacityPct
    std::uint32_t minGearPct = 25;   // AutoWow.Survival.PackAvoid.MinGearPct
};

// Classes whose damage is the weapon's (warrior 1, paladin 2, hunter 3, rogue 4, death knight 6).
[[nodiscard]] inline bool WeaponClass(std::uint32_t cls) { return cls == 1 || cls == 2 || cls == 3 || cls == 4 || cls == 6; }

[[nodiscard]] inline std::uint32_t GearPct(Params const& p, std::uint32_t cls, std::uint32_t curMilli,
                                           std::uint32_t expectedMilli)
{
    if (!WeaponClass(cls) || !expectedMilli)
        return 100;
    std::uint64_t const pct = std::uint64_t(curMilli) * 100 / expectedMilli;
    return pct >= 100 ? 100 : (pct < p.minGearPct ? p.minGearPct : static_cast<std::uint32_t>(pct));
}

[[nodiscard]] inline std::uint32_t Capacity(Params const& p, std::uint32_t classCapacity, std::uint32_t gearPct)
{
    return static_cast<std::uint32_t>(std::uint64_t(classCapacity) * gearPct / 100 * p.capacityPct / 100);
}

// Proactive avoidance: no escape ratio (that is the in-fight escape line); a pack over capacity is skipped.
[[nodiscard]] inline AutoWowPackRisk::Verdict Score(std::uint32_t candidateWeight, std::uint32_t neighbourSum,
                                                    std::uint32_t capacity)
{
    AutoWowPackRisk::Verdict v;
    v.risk = candidateWeight + neighbourSum;
    v.band = v.risk / 100;
    v.reject = neighbourSum && v.risk > capacity;
    return v;
}

// Travel.Safe path threat with packs: every mob whose aggro radius (+margin) reaches the path counts
// 1 + its neighbours within linkYards when it has any (a pack at any level), else 1 when it threatens on
// its own (WalkingV2Policy Threatens), else 0. Lone threatening mobs count as PathThreat counts them.
// ponytail: neighbours are any idle hostile of the scan (no faction link check); O(mobs^2) on ~tens.
[[nodiscard]] inline std::uint32_t PackPathThreat(std::vector<WalkingV2Policy::Point> const& path,
                                                  std::vector<WalkingV2Policy::Mob> const& mobs,
                                                  std::uint32_t botLevel, std::uint32_t linkYards)
{
    std::uint32_t n = 0;
    std::int64_t const link2 = std::int64_t(linkYards) * linkYards;
    for (std::size_t i = 0; i < mobs.size() && !path.empty(); ++i)
    {
        WalkingV2Policy::Mob const& m = mobs[i];
        std::int64_t const r = std::int64_t(m.aggroYards) + WalkingV2Policy::kAggroMarginYards;
        bool hit = WalkingV2Policy::Dist2ToSegment(m.x, m.y, path.front(), path.front()) <= r * r;
        for (std::size_t k = 1; !hit && k < path.size(); ++k)
            hit = WalkingV2Policy::Dist2ToSegment(m.x, m.y, path[k - 1], path[k]) <= r * r;
        if (!hit)
            continue;
        std::uint32_t neighbours = 0;
        for (std::size_t j = 0; j < mobs.size(); ++j)
        {
            std::int64_t const dx = std::int64_t(mobs[j].x) - m.x;
            std::int64_t const dy = std::int64_t(mobs[j].y) - m.y;
            neighbours += j != i && dx * dx + dy * dy <= link2 ? 1 : 0;
        }
        n += neighbours ? 1 + neighbours : (WalkingV2Policy::Threatens(m, botLevel) ? 1 : 0);
    }
    return n;
}

// ---- runtime (flag + params; AutoWowTactics::LoadConfig reads them, GrindTargetValue / NewRpgBaseAction
// consult them) -----------------------------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
inline Params const& Get() { return detail::gParams; }
}  // namespace AutoWowPackAvoid

#endif  // AUTOWOW_PACK_AVOID_POLICY_H
