/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_GATHER_DETOUR_POLICY_H
#define AUTOWOW_GATHER_DETOUR_POLICY_H

#include <cstdint>
#include <vector>

// Gathering detours for independent questing bots (AutoWow.Gathering.Detours, default 0). The stock
// "gather" strategy only queues nodes and the loot strategy only walks to loot within
// AiPlayerbot.LootDistance (15 yd), so a questing bot almost never reached a herb/ore node and the
// faction profession KPI (ledger skill_up) stayed near zero. With the flag on, a bot that knows
// Herbalism or Mining walks to the nearest visible node within DetourYards whose lock skill it meets,
// when idle-able (alive, out of combat, not on a zone-progression / errand / flight leg, not resting),
// then yields to the stock loot pipeline (open, gather, skill-up) and resumes its RPG status. Skinning
// is stock: the gather
// strategy queues skinnable corpses (the bot's own kills lie within the loot range) and LootObject checks
// the skill against the corpse level. Emits nothing new (skill_up already logs).
//
// Value-only: integer yards and game-time ms, no floats in decisions, no RNG; ties by node guid.
namespace AutoWowGatherDetour
{
inline constexpr std::uint8_t kStateVersion = 1;
inline constexpr std::uint32_t kSkillHerbalism = 182;  // SKILL_HERBALISM
inline constexpr std::uint32_t kSkillMining = 186;     // SKILL_MINING

struct Params
{
    std::uint32_t yards = 60;          // AutoWow.Gathering.DetourYards
    std::uint32_t maxDz = 20;          // AutoWow.Gathering.DetourMaxDz (yards above/below the bot)
    std::uint32_t timeoutMs = 45000;   // AutoWow.Gathering.DetourTimeoutMs (per node, then skipped)
};

// The bot's gathering ability. 0 skill = not learned.
struct Skills
{
    std::uint32_t herbalism = 0;
    std::uint32_t mining = 0;
    bool miningPick = false;
};

// A visible gathering node, relative to the bot (integer yards).
struct Node
{
    std::uint64_t guid = 0;
    std::uint32_t skill = 0;     // lock skill (LootObject::skillId)
    std::uint32_t reqSkill = 0;  // lock skill value (LootObject::reqSkillValue)
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t dz = 0;
};

[[nodiscard]] inline bool HasGatherSkill(Skills const& s) { return s.herbalism > 0 || s.mining > 0; }

// A detour may start or continue only while the bot is free: not in combat and not on a critical leg
// (zone-progression trip, errand run, flight, rest).
[[nodiscard]] inline bool MayDetour(Skills const& s, bool inCombat, bool criticalLeg)
{
    return HasGatherSkill(s) && !inCombat && !criticalLeg;
}

[[nodiscard]] inline std::int64_t Dist2(Node const& n)
{
    return std::int64_t(n.dx) * n.dx + std::int64_t(n.dy) * n.dy + std::int64_t(n.dz) * n.dz;
}

// Herb or ore the bot can gather now (skill learned, lock value <= skill, a pick for ore) and within
// reach (3D distance <= yards, height difference <= maxDz).
[[nodiscard]] inline bool Eligible(Params const& p, Skills const& s, Node const& n)
{
    std::uint32_t have = 0;
    if (n.skill == kSkillHerbalism)
        have = s.herbalism;
    else if (n.skill == kSkillMining)
        have = s.miningPick ? s.mining : 0;
    if (!have || n.reqSkill > have)
        return false;
    std::int64_t const reach = p.yards;
    std::int64_t const dz = n.dz < 0 ? -std::int64_t(n.dz) : n.dz;
    return dz <= p.maxDz && Dist2(n) <= reach * reach;
}

// Nearest eligible node not in `skip` (lowest guid on ties); -1 = none.
[[nodiscard]] inline int PickNode(Params const& p, Skills const& s, std::vector<Node> const& nodes,
                                  std::vector<std::uint64_t> const& skip)
{
    int best = -1;
    for (std::size_t k = 0; k < nodes.size(); ++k)
    {
        Node const& n = nodes[k];
        if (!Eligible(p, s, n))
            continue;
        bool skipped = false;
        for (std::uint64_t g : skip)
            skipped |= g == n.guid;
        if (skipped)
            continue;
        if (best < 0 || Dist2(n) < Dist2(nodes[best]) ||
            (Dist2(n) == Dist2(nodes[best]) && n.guid < nodes[best].guid))
            best = static_cast<int>(k);
    }
    return best;
}

// Per-bot state (fixed size, value-only). skip = the last nodes that timed out (ring, never retried
// while they stay in it).
inline constexpr std::size_t kSkipSlots = 4;
struct BotState
{
    std::uint8_t version = kStateVersion;
    std::uint64_t target = 0;  // node guid (raw), 0 = no detour
    std::uint64_t startMs = 0;
    std::uint64_t skip[kSkipSlots] = {};
    std::uint32_t skipNext = 0;
};

inline std::vector<std::uint64_t> SkipList(BotState const& s)
{
    std::vector<std::uint64_t> out;
    for (std::uint64_t g : s.skip)
        if (g)
            out.push_back(g);
    return out;
}

inline void Begin(BotState& s, std::uint64_t guid, std::uint64_t nowMs)
{
    s.target = guid;
    s.startMs = nowMs;
}

// The detour ends: gathered / gone (skip = false) or given up (skip = true: the node joins the ring).
inline void End(BotState& s, bool skip)
{
    if (skip && s.target)
    {
        s.skip[s.skipNext % kSkipSlots] = s.target;
        s.skipNext = (s.skipNext + 1) % kSkipSlots;
    }
    s.target = 0;
    s.startMs = 0;
}

[[nodiscard]] inline bool TimedOut(Params const& p, BotState const& s, std::uint64_t nowMs)
{
    return s.target && nowMs >= s.startMs && nowMs - s.startMs > p.timeoutMs;
}

// ---- runtime (NewRpgZoneProgression.cpp) --------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }

void LoadConfig();
}  // namespace AutoWowGatherDetour

#endif  // AUTOWOW_GATHER_DETOUR_POLICY_H
