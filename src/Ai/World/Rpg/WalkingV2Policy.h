/*
 * Chunked long walks for the committed travel intent (AutoWow.Walking.V2, default off; requires
 * AutoWow.TravelIntent.Enable).
 *
 * Root cause this replaces (soak-s14/s15: every zone_move arrived by portal, 33/52 town runs
 * travel_gave_up): MoveFarToIntent asked the navmesh for the WHOLE route to a 300-2000 yd goal. A
 * PathGenerator smooth path is capped at 74 points (4 yd steps, ~296 yd): a longer route fails
 * FindSmoothPath and comes back NOPATH (or SHORT), and a goal on a never-loaded grid has no end poly,
 * which for a Player source comes back NORMAL|NOT_USING_PATH. Both fail the mover's type check, so a
 * long walk could only advance through TravelMgr segments or 8-24 yd local detours - and the detours
 * are refused as 15 yd revisits after the first. Five failed replans (~10 s) gave up the intent, each
 * give-up burned a leg reissue, and eight reissues portalled the bot (walk_ms 36-160 s).
 *
 * Here a goal beyond kChunkYards is walked in chunks: the sub-goal is kChunkYards (then
 * kShortChunkYards) along the bearing to the goal, fanned 0, +-30, +-60, +-90 degrees in that order;
 * each is pathed on its own (well inside the smooth-path cap and the bot's loaded grids) and its real
 * mmap endpoint is admitted only if it brings the goal kMinGainYards closer. Goal-distance monotone
 * admission cannot bounce A -> B -> A, so it needs no revisit memory.
 *
 * Pure: integer yards, fixed rotation table, truncating integer division; no floats, RNG or world access.
 */
#ifndef PLAYERBOTS_WALKING_V2_POLICY_H
#define PLAYERBOTS_WALKING_V2_POLICY_H

#include "TravelIntentPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace WalkingV2Policy
{
using TravelIntentPolicy::DistanceYards;
using TravelIntentPolicy::Point;

inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::uint32_t kChunkYards = 120;      // first ring: a goal beyond it is walked in chunks
inline constexpr std::uint32_t kShortChunkYards = 60;  // second ring, for a blocked first ring
inline constexpr std::uint32_t kMinGainYards = 10;     // a chunk must bring the goal this much closer

struct Rotation
{
    std::int32_t cos1024;
    std::int32_t sin1024;
};

// 0, +30, -30, +60, -60, +90, -90 degrees (x1024, rounded).
inline constexpr std::array<Rotation, 7> kRotations{{
    {1024, 0}, {887, 512}, {887, -512}, {512, 887}, {512, -887}, {0, 1024}, {0, -1024}}};
inline constexpr std::size_t kChunkProbes = kRotations.size() * 2;

// Probe k (0 .. kChunkProbes-1): ring kChunkYards for k < 7, kShortChunkYards after, rotated by
// kRotations[k % 7] off the here -> goal bearing. `here` itself when here == goal or maps differ.
[[nodiscard]] inline Point ChunkProbe(Point const& here, Point const& goal, std::size_t k)
{
    std::uint32_t const len = DistanceYards(here, goal);
    if (len == 0 || len == 0xFFFFFFFFu || k >= kChunkProbes)
        return here;
    std::int64_t const radius = k < kRotations.size() ? kChunkYards : kShortChunkYards;
    Rotation const r = kRotations[k % kRotations.size()];
    std::int64_t const dx = static_cast<std::int64_t>(goal.x) - here.x;
    std::int64_t const dy = static_cast<std::int64_t>(goal.y) - here.y;
    std::int64_t const denom = static_cast<std::int64_t>(len) * 1024;
    Point p = here;
    p.x = static_cast<std::int32_t>(here.x + (dx * r.cos1024 - dy * r.sin1024) * radius / denom);
    p.y = static_cast<std::int32_t>(here.y + (dx * r.sin1024 + dy * r.cos1024) * radius / denom);
    return p;
}

// Chunking applies only beyond the first ring; nearer goals keep the ordinary intent planning.
[[nodiscard]] inline bool UseChunks(Point const& here, Point const& goal)
{
    std::uint32_t const len = DistanceYards(here, goal);
    return len != 0xFFFFFFFFu && len > kChunkYards;
}

// Admission of a chunk endpoint (the pathed end, not the probe): a real step, not dangerous, and
// kMinGainYards closer to the goal than the bot is now.
[[nodiscard]] inline bool AdmitChunk(Point const& here, Point const& candidate, Point const& goal, bool candidateDanger)
{
    if (candidateDanger || DistanceYards(here, candidate) < TravelIntentPolicy::kMinStepYards)
        return false;
    std::uint64_t const from = DistanceYards(here, goal);
    std::uint64_t const to = DistanceYards(candidate, goal);
    return to + kMinGainYards <= from;
}

// ---- AutoWow.Travel.Safe (default 0): aggro-aware chunk choice --------------------------------------
// soak-s16-full-r1: the fan committed the first goal-monotone chunk even when its path ran through a
// camp. With the flag on, each admissible chunk's mmap path (its point list) is scored by the idle
// hostile mobs whose aggro radius (+kAggroMarginYards) reaches it; a clear chunk is taken in fan order,
// else the least threatened one (earlier on ties). Same purity as above: integer yards, no floats.
inline constexpr std::uint8_t kSafePolicyVersion = 1;
inline constexpr std::uint32_t kAggroMarginYards = 5;

// An idle hostile creature near the walk (the caller filters: alive, not in combat, hostile, not civilian).
struct Mob
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t level = 0;
    bool elite = false;
    std::uint32_t aggroYards = 0;  // Creature::GetAggroRange(bot), truncated
};

// Worth walking around: at or above the bot's level, or any elite.
[[nodiscard]] inline bool Threatens(Mob const& m, std::uint32_t botLevel) { return m.elite || m.level >= botLevel; }

// Squared 2D distance (truncated) from (px, py) to the segment a-b.
[[nodiscard]] inline std::int64_t Dist2ToSegment(std::int32_t px, std::int32_t py, Point const& a, Point const& b)
{
    std::int64_t const abx = std::int64_t(b.x) - a.x, aby = std::int64_t(b.y) - a.y;
    std::int64_t const apx = std::int64_t(px) - a.x, apy = std::int64_t(py) - a.y;
    std::int64_t const len2 = abx * abx + aby * aby;
    std::int64_t const t = apx * abx + apy * aby;
    if (len2 == 0 || t <= 0)
        return apx * apx + apy * apy;
    if (t >= len2)
    {
        std::int64_t const bpx = std::int64_t(px) - b.x, bpy = std::int64_t(py) - b.y;
        return bpx * bpx + bpy * bpy;
    }
    std::int64_t const cross = apx * aby - apy * abx;  // |cross| <= |ap| * |ab|: no overflow on map coords
    return cross * cross / len2;
}

// Threatening mobs whose aggro radius (+kAggroMarginYards) reaches the path polyline (one count per mob).
[[nodiscard]] inline std::uint32_t PathThreat(std::vector<Point> const& path, std::vector<Mob> const& mobs,
                                              std::uint32_t botLevel)
{
    std::uint32_t n = 0;
    for (Mob const& m : mobs)
    {
        if (!Threatens(m, botLevel) || path.empty())
            continue;
        std::int64_t const r = std::int64_t(m.aggroYards) + kAggroMarginYards;
        bool hit = Dist2ToSegment(m.x, m.y, path.front(), path.front()) <= r * r;
        for (std::size_t i = 1; !hit && i < path.size(); ++i)
            hit = Dist2ToSegment(m.x, m.y, path[i - 1], path[i]) <= r * r;
        n += hit ? 1 : 0;
    }
    return n;
}

struct ChunkChoice
{
    bool admissible = false;  // AdmitChunk, and no death-loop danger along its path
    std::uint32_t threat = 0;
};
inline constexpr std::size_t kNoChunk = static_cast<std::size_t>(-1);

// Fan order: the first admissible chunk with no threat, else the least threatened admissible one
// (earlier on ties); kNoChunk when none is admissible. The caller may stop probing at the first clear one.
[[nodiscard]] inline std::size_t PickChunk(std::vector<ChunkChoice> const& choices)
{
    std::size_t best = kNoChunk;
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        if (!choices[i].admissible)
            continue;
        if (choices[i].threat == 0)
            return i;
        if (best == kNoChunk || choices[i].threat < choices[best].threat)
            best = i;
    }
    return best;
}

// ---- AutoWow.Survival.HardEscape (default 0): zone-danger-aware chunks ---------------------------------
// soak-s22-full-r1: an L18 escape walk beelined NE through Redridge into Burning Steppes (bracket 51-60)
// and died 183 times. With the flag on, a chunk endpoint (and, with AutoWow.Travel.Safe, its path points)
// in a zone whose bracket low is more than `margin` above the bot is not admissible - the bot's own zone
// excepted, so a bot already inside can still walk out. zoneLow 0 = unknown: never.
[[nodiscard]] inline bool ZoneDanger(std::uint32_t pointZone, std::uint32_t botZone, std::uint32_t zoneLow,
                                     std::uint32_t botLevel, std::uint32_t margin)
{
    return pointZone && pointZone != botZone && zoneLow && zoneLow > botLevel + margin;
}
}  // namespace WalkingV2Policy

#endif
