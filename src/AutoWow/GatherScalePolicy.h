/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERSCALEPOLICY_H
#define _PLAYERBOT_GATHERSCALEPOLICY_H

// Pure rules for the owner's 2026-09-28 gathering rule ("Warcraft redefined"): gathering is always allowed
// once the profession is known, the yield scales with the skill deficit, and hard gathers / orange crafts
// give more than one point. No world access; unit-tested (tests/GatherScalePolicyTest.cpp).
//
//   d = required skill - current skill (the node lock's skill; for a corpse the core's skinning red level).
//   AutoWow.Gather.AnySkill   : a known gatherer (skill >= 1) may work a node with d > 0 (core + bot gates).
//   AutoWow.Gather.YieldScale : every looted stack -> max(1, count * YieldPct(d) / 100); d > kScrapsAbove
//                               swaps a mapped material for its previous-tier material (ScrapItem).
//   AutoWow.Gather.MultiGain  : a gathering skill-up is GatherGain(d) points.
//   AutoWow.Craft.MultiGain   : an orange craft skill-up is one point more.
// Every gain is clamped to the rank max (CapGain). Integer math, no randomness: the core loot roll and the
// core skill-up roll stay the only rolls.

#include <cstdint>

namespace AutoWowGatherScale
{
inline constexpr std::uint32_t kSkillHerbalism = 182;
inline constexpr std::uint32_t kSkillMining = 186;
inline constexpr std::uint32_t kSkillSkinning = 393;

inline constexpr std::int32_t kYieldLossPctPerPoint = 2;  // 100% at d <= 0, 50% at d = 25, floor (1 unit) at d >= 50
inline constexpr std::int32_t kScrapsAbove = 50;          // d > 50: previous-tier material instead
inline constexpr std::int32_t kGainStep = 25;             // +1 point per 25 points of deficit
inline constexpr std::uint32_t kGainCap = 5;              // at most 5 points per gather

[[nodiscard]] inline constexpr bool IsGatherSkill(std::uint32_t skill)
{
    return skill == kSkillHerbalism || skill == kSkillMining || skill == kSkillSkinning;
}

// Bot-side mirror of the core gate: stock rule (req 0 or skill >= req), or AnySkill for a known gatherer.
[[nodiscard]] inline constexpr bool CanAttempt(bool anySkill, std::uint32_t skillId, std::uint32_t skill,
                                               std::uint32_t req)
{
    return !req || skill >= req || (anySkill && IsGatherSkill(skillId) && skill >= 1);
}

[[nodiscard]] inline constexpr std::int32_t Deficit(std::uint32_t req, std::uint32_t skill)
{
    return static_cast<std::int32_t>(req) - static_cast<std::int32_t>(skill);
}

// Skinning red level of a corpse (Spell::EffectSkinning / UpdateGatherSkill), the `req` of a skinning d.
[[nodiscard]] inline constexpr std::uint32_t SkinningRequired(std::uint32_t level)
{
    return level < 10 ? 0 : level < 20 ? (level - 10) * 10 : level * 5;
}

[[nodiscard]] inline constexpr std::uint32_t YieldPct(std::int32_t d)
{
    if (d <= 0)
        return 100;
    std::int32_t const pct = 100 - kYieldLossPctPerPoint * d;
    return pct > 0 ? static_cast<std::uint32_t>(pct) : 0;
}

// A looted stack at deficit d: full at d <= 0, never below one unit.
[[nodiscard]] inline constexpr std::uint32_t ScaledCount(std::uint32_t count, std::int32_t d)
{
    if (!count || d <= 0)
        return count;
    std::uint32_t const scaled = count * YieldPct(d) / 100;
    return scaled ? scaled : 1;
}

[[nodiscard]] inline constexpr bool Scraps(std::int32_t d) { return d > kScrapsAbove; }

// Previous-tier material of a gathered material (item_template ids checked against the world DB); 0 = none
// (tier-one materials, gems, motes, rare drops keep their item).
[[nodiscard]] inline constexpr std::uint32_t ScrapItem(std::uint32_t item)
{
    switch (item)
    {
        // ore
        case 2771: return 2770;    // Tin Ore -> Copper Ore
        case 3340: return 2770;    // Incendicite Ore -> Copper Ore
        case 2775: return 2771;    // Silver Ore -> Tin Ore
        case 2772: return 2771;    // Iron Ore -> Tin Ore
        case 2776: return 2772;    // Gold Ore -> Iron Ore
        case 3858: return 2772;    // Mithril Ore -> Iron Ore
        case 7911: return 3858;    // Truesilver Ore -> Mithril Ore
        case 10620: return 3858;   // Thorium Ore -> Mithril Ore
        case 11370: return 3858;   // Dark Iron Ore -> Mithril Ore
        case 23424: return 10620;  // Fel Iron Ore -> Thorium Ore
        case 23425: return 23424;  // Adamantite Ore -> Fel Iron Ore
        case 36909: return 23425;  // Cobalt Ore -> Adamantite Ore
        case 36912: return 36909;  // Saronite Ore -> Cobalt Ore
        case 36910: return 36912;  // Titanium Ore -> Saronite Ore
        // stone
        case 2836: return 2835;    // Coarse -> Rough Stone
        case 2838: return 2836;    // Heavy -> Coarse Stone
        case 7912: return 2838;    // Solid -> Heavy Stone
        case 12365: return 7912;   // Dense -> Solid Stone
        // herbs, skill 50-100 -> Silverleaf
        case 785:                  // Mageroyal
        case 2450:                 // Briarthorn
        case 2452:                 // Swiftthistle
        case 2453:                 // Bruiseweed
        case 3820: return 765;     // Stranglekelp
        // herbs, skill 115-170 -> Briarthorn
        case 3355:                 // Wild Steelbloom
        case 3356:                 // Kingsblood
        case 3357:                 // Liferoot
        case 3369:                 // Grave Moss
        case 3818:                 // Fadeleaf
        case 3821: return 2450;    // Goldthorn
        // herbs, skill 185-230 -> Kingsblood
        case 3358:                 // Khadgar's Whisker
        case 3819:                 // Wintersbite
        case 4625:                 // Firebloom
        case 8831:                 // Purple Lotus
        case 8836:                 // Arthas' Tears
        case 8838: return 3356;    // Sungrass
        // herbs, skill 235-300 -> Sungrass
        case 8839:                 // Blindweed
        case 8845:                 // Ghost Mushroom
        case 8846:                 // Gromsblood
        case 13463:                // Dreamfoil
        case 13464:                // Golden Sansam
        case 13465:                // Mountain Silversage
        case 13466:                // Plaguebloom
        case 13467:                // Icecap
        case 13468: return 8838;   // Black Lotus
        // leather
        case 783:                  // Light Hide
        case 2318: return 2934;    // Light Leather -> Ruined Leather Scraps
        case 4232:                 // Medium Hide
        case 2319: return 2318;    // Medium Leather -> Light Leather
        case 4235:                 // Heavy Hide
        case 4234: return 2319;    // Heavy Leather -> Medium Leather
        case 8169:                 // Thick Hide
        case 4304: return 4234;    // Thick Leather -> Heavy Leather
        case 8171:                 // Rugged Hide
        case 8170: return 4304;    // Rugged Leather -> Thick Leather
        case 21887: return 8170;   // Knothide Leather -> Rugged Leather
        case 33568: return 21887;  // Borean Leather -> Knothide Leather
        case 38425: return 33568;  // Heavy Borean Leather -> Borean Leather
        default: return 0;
    }
}

// Points of a gathering skill-up at deficit d: the core's base below d = 1, else 1 + d / 25, at most 5.
[[nodiscard]] inline constexpr std::uint32_t GatherGain(std::uint32_t base, std::int32_t d)
{
    if (d <= 0)
        return base;
    std::uint32_t const gain = 1 + static_cast<std::uint32_t>(d / kGainStep);
    std::uint32_t const capped = gain < kGainCap ? gain : kGainCap;
    return capped > base ? capped : base;
}

// Points of a crafting skill-up: one more for an orange recipe (current skill below its yellow level).
[[nodiscard]] inline constexpr std::uint32_t CraftGain(std::uint32_t base, std::uint32_t current, std::uint32_t yellow)
{
    return current < yellow ? base + 1 : base;
}

// Never past the rank max (the core clamps too; this keeps the reported gain honest).
[[nodiscard]] inline constexpr std::uint32_t CapGain(std::uint32_t gain, std::uint32_t current, std::uint32_t max)
{
    if (current >= max)
        return gain;  // the core's own no-op (UpdateSkillPro returns before applying)
    return current + gain > max ? max - current : gain;
}

// Runtime (AutoWowGatherScale.cpp): the loot / skill-gain / learn-log hooks, registered at script load.
void AddScripts();
}  // namespace AutoWowGatherScale

#endif
