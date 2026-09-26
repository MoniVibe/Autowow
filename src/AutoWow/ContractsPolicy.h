/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_CONTRACTS_POLICY_H
#define AUTOWOW_CONTRACTS_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct CreatureData;

// Hunt contracts for independent AutoWoW bots (AutoWow.Contracts.Enable, default 0). A bot whose quest
// scheduler has no live quest work (RPG Idle and DO_QUEST unavailable) is issued a contract by its
// faction ("faction_board"): kill Kills level-appropriate non-elite mobs of the best nearby spawn cluster.
// It walks to the cluster anchor (the ordinary no-teleport long walk), grinds only the contract entries
// within LeashYards of the anchor (GrindTargetValue), walks back when it drifts past the leash out of
// combat, and returns to its quest loop when the contract is done (Kills credited kills), expired
// (TimeoutMs, no credited kill for StallMs, or the anchor fell inside one of the bot's death-loop danger
// areas) or abandoned (quest work appeared, another trip took the bot, the walk got stuck). An errand run
// pauses both clocks. No reward in V1 (no free gold). Each issue and end emits ledger `contract` (event 18).
//
// Spawn index: world-DB creature spawns on the random-bot maps whose template is a normal-rank,
// attackable, non-critter/totem/trigger/civilian creature without service npc flags (gossip allowed),
// with the teams that may attack it (faction template not friendly to the team; a creature without loot
// only for the teams it is hostile to, as the grind filter skips it otherwise). Anchors in water are
// rejected at issue time (no map terrain at index time).
//
// Value-only: integer yards and game-time ms, no floats in decisions, no RNG, no strings in decisions;
// stable orders (spawn id / entry ascending on ties).
namespace AutoWowContracts
{
inline constexpr std::uint8_t kStateVersion = 2;  // 2: lastKillMs, pause
inline constexpr std::size_t kMaxEntries = 8;         // target entries per contract (most spawns first)
inline constexpr std::size_t kMaxCandidates = 512;    // nearest candidate spawns scored per search
inline constexpr std::size_t kMaxTrackedBots = 2048;  // hard cap; beyond it new bots get no contract
inline constexpr std::uint8_t kAlliance = 1;          // Spawn::teams bits
inline constexpr std::uint8_t kHorde = 2;

struct Params
{
    std::uint32_t kills = 15;              // AutoWow.Contracts.Kills
    std::uint32_t levelBelow = 3;          // AutoWow.Contracts.LevelBelow
    std::uint32_t levelAbove = 1;          // AutoWow.Contracts.LevelAbove
    std::uint32_t levelGapMin = 0;         // AutoWow.Contracts.LevelGapMin: max level at least this far below the bot
    std::uint32_t searchYards = 700;       // AutoWow.Contracts.SearchYards
    std::uint32_t clusterYards = 120;      // AutoWow.Contracts.ClusterYards
    std::uint32_t leashYards = 150;        // AutoWow.Contracts.LeashYards
    std::uint32_t timeoutMs = 1200000;     // AutoWow.Contracts.TimeoutMs
    std::uint32_t stallMs = 480000;        // AutoWow.Contracts.StallMs: no credited kill this long expires (0 = off)
    std::uint32_t cooldownMs = 300000;     // AutoWow.Contracts.CooldownMs (same anchor after an end)
    std::uint32_t minClusterSpawns = 6;    // AutoWow.Contracts.MinClusterSpawns
};

// One indexed creature spawn (world DB), integer yards.
struct Spawn
{
    std::uint32_t spawnId = 0;
    std::uint32_t entry = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t minLevel = 0;
    std::uint8_t maxLevel = 0;
    std::uint8_t teams = 0;  // kAlliance | kHorde: the teams that may hunt it
};

[[nodiscard]] inline std::int64_t Dist2(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
{
    std::int64_t const dx = std::int64_t(ax) - bx;
    std::int64_t const dy = std::int64_t(ay) - by;
    return dx * dx + dy * dy;
}

[[nodiscard]] inline bool Within(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by,
                                 std::uint32_t yards)
{
    return Dist2(ax, ay, bx, by) <= std::int64_t(yards) * yards;
}

// The spawn's whole level range lies in [botLevel - LevelBelow, botLevel + LevelAbove].
[[nodiscard]] inline bool InLevelWindow(Params const& p, std::uint32_t botLevel, Spawn const& s)
{
    std::uint32_t const low = botLevel > p.levelBelow ? botLevel - p.levelBelow : 1;
    // LevelGapMin wins over LevelAbove: soak-s35-full-r1 contract deaths were near-level fair fights lost
    // (7.3 deaths per contract bot-hour vs 3.0 off contract).
    std::uint32_t const high = p.levelGapMin ? (botLevel > p.levelGapMin ? botLevel - p.levelGapMin : 0)
                                             : botLevel + p.levelAbove;
    return s.minLevel >= low && s.maxLevel <= high;
}

// Spawns of the bot's map the bot's team may hunt, in its level window, within SearchYards of the bot:
// the kMaxCandidates nearest (ties by lower spawn id), in that order.
// ponytail: linear scan of the map's index per search (tens of thousands, one search per bot per minute
// at most); bucket the index by grid cell if it ever shows in a profile.
[[nodiscard]] inline std::vector<Spawn> Candidates(Params const& p, std::vector<Spawn> const& mapSpawns,
                                                   std::uint8_t team, std::uint32_t botLevel, std::int32_t bx,
                                                   std::int32_t by)
{
    std::vector<Spawn> out;
    for (Spawn const& s : mapSpawns)
        if ((s.teams & team) && InLevelWindow(p, botLevel, s) && Within(s.x, s.y, bx, by, p.searchYards))
            out.push_back(s);
    std::sort(out.begin(), out.end(),
              [bx, by](Spawn const& a, Spawn const& b)
              {
                  std::int64_t const da = Dist2(a.x, a.y, bx, by), db = Dist2(b.x, b.y, bx, by);
                  return da != db ? da < db : a.spawnId < b.spawnId;
              });
    if (out.size() > kMaxCandidates)
        out.resize(kMaxCandidates);
    return out;
}

// A candidate spawn scored as a cluster center: the candidates within ClusterYards of it (itself
// included) and their entries, most spawns first (ties by lower entry), at most kMaxEntries.
struct Cluster
{
    std::uint32_t center = 0;  // spawn id of the anchor spawn
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t spawns = 0;
    std::int64_t dist2 = 0;  // to the bot
    std::vector<std::uint32_t> entries;
};

// Every candidate with at least MinClusterSpawns candidates within ClusterYards, best first: more spawns,
// then nearer the bot, then lower center spawn id. O(n^2) over the capped candidate list.
[[nodiscard]] inline std::vector<Cluster> RankClusters(Params const& p, std::vector<Spawn> const& candidates,
                                                       std::int32_t bx, std::int32_t by)
{
    std::vector<Cluster> out;
    for (Spawn const& c : candidates)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> counts;  // (entry, spawns)
        std::uint32_t members = 0;
        for (Spawn const& m : candidates)
        {
            if (!Within(m.x, m.y, c.x, c.y, p.clusterYards))
                continue;
            ++members;
            auto it = std::find_if(counts.begin(), counts.end(), [&m](auto const& e) { return e.first == m.entry; });
            if (it == counts.end())
                counts.emplace_back(m.entry, 1);
            else
                ++it->second;
        }
        if (members < p.minClusterSpawns)
            continue;
        std::sort(counts.begin(), counts.end(),
                  [](auto const& a, auto const& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        Cluster cl;
        cl.center = c.spawnId;
        cl.x = c.x;
        cl.y = c.y;
        cl.z = c.z;
        cl.spawns = members;
        cl.dist2 = Dist2(c.x, c.y, bx, by);
        for (std::size_t k = 0; k < counts.size() && k < kMaxEntries; ++k)
            cl.entries.push_back(counts[k].first);
        out.push_back(std::move(cl));
    }
    std::sort(out.begin(), out.end(),
              [](Cluster const& a, Cluster const& b)
              {
                  if (a.spawns != b.spawns)
                      return a.spawns > b.spawns;
                  return a.dist2 != b.dist2 ? a.dist2 < b.dist2 : a.center < b.center;
              });
    return out;
}

// First ranked cluster whose anchor `skip` does not reject (danger area, over-level zone, cooldown);
// nullptr = none. `skip` is called best first and stops at the first accepted cluster.
template <typename Skip>
[[nodiscard]] Cluster const* PickCluster(std::vector<Cluster> const& ranked, Skip&& skip)
{
    for (Cluster const& c : ranked)
        if (!skip(c))
            return &c;
    return nullptr;
}

// ---- contract state ------------------------------------------------------------------------------
enum class Phase : std::uint8_t
{
    None = 0,    // no contract
    Travel = 1,  // walking to the anchor (or back inside the leash)
    Hunt = 2     // at the anchor: grinding the contract entries
};

// Ledger `contract` reason. Wire-stable; append only. Judge() returns Issued for "still running".
enum class Reason : std::uint8_t
{
    Issued = 0,
    Done = 1,
    Expired = 2,
    Abandoned = 3
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Issued: return "issued";
        case Reason::Done: return "done";
        case Reason::Expired: return "expired";
        case Reason::Abandoned: return "abandoned";
    }
    return "unknown";
}

// Per-bot state (fixed size, value-only).
struct BotState
{
    std::uint8_t version = kStateVersion;
    Phase phase = Phase::None;
    std::uint32_t id = 0;           // run-scoped contract id, never reused
    std::uint32_t map = 0;
    std::int32_t x = 0;             // anchor
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t anchorSpawn = 0;  // cluster center spawn id
    std::array<std::uint32_t, kMaxEntries> entries = {};
    std::uint8_t entryCount = 0;
    std::uint32_t kills = 0;
    std::uint32_t target = 0;
    std::uint64_t startMs = 0;
    std::uint64_t lastKillMs = 0;     // last credited kill (issue time before the first)
    bool paused = false;              // errand run in progress: clocks stopped since pauseSinceMs
    std::uint64_t pauseSinceMs = 0;
    std::uint32_t stuck = 0;          // stuck walk ticks this contract
    std::uint64_t nextSearchMs = 0;   // no issue search before
    std::uint64_t nextQuestMs = 0;    // next quest-work recheck while hunting
    std::uint32_t cooldownMap = 0;    // anchor of the last ended contract
    std::int32_t cooldownX = 0;
    std::int32_t cooldownY = 0;
    std::uint64_t cooldownUntilMs = 0;
};

inline void Issue(BotState& s, Params const& p, Cluster const& c, std::uint32_t map, std::uint32_t id,
                  std::uint64_t nowMs)
{
    s.phase = Phase::Travel;
    s.id = id;
    s.map = map;
    s.x = c.x;
    s.y = c.y;
    s.z = c.z;
    s.anchorSpawn = c.center;
    s.entries = {};
    s.entryCount = static_cast<std::uint8_t>(std::min(c.entries.size(), kMaxEntries));
    for (std::size_t k = 0; k < s.entryCount; ++k)
        s.entries[k] = c.entries[k];
    s.kills = 0;
    s.target = p.kills;
    s.startMs = nowMs;
    s.lastKillMs = nowMs;
    s.paused = false;
    s.pauseSinceMs = 0;
    s.stuck = 0;
}

[[nodiscard]] inline bool Accepts(BotState const& s, std::uint32_t entry)
{
    if (s.phase == Phase::None)
        return false;
    for (std::size_t k = 0; k < s.entryCount; ++k)
        if (s.entries[k] == entry)
            return true;
    return false;
}

// A credited kill of `entry`; true when it counted toward the contract.
inline bool NoteKill(BotState& s, std::uint32_t entry, std::uint64_t nowMs)
{
    if (!Accepts(s, entry) || s.kills >= s.target)
        return false;
    ++s.kills;
    s.lastKillMs = nowMs;
    return true;
}

// Why a running contract expires now ("" = it does not): danger, then timeout, then the no-kill
// watchdog (soak-s38: 17% of hunt minutes frozen, 22% walking outside the leash, 4 of 89 done).
[[nodiscard]] inline char const* ExpiredCause(Params const& p, BotState const& s, std::uint64_t nowMs,
                                             bool anchorDangerous)
{
    if (anchorDangerous)
        return "danger";
    if (nowMs >= s.startMs && nowMs - s.startMs >= p.timeoutMs)
        return "timeout";
    if (p.stallMs && nowMs >= s.lastKillMs && nowMs - s.lastKillMs >= p.stallMs)
        return "stall";
    return "";
}

// Done beats expired: a contract that reached its count ends done even past its deadline.
[[nodiscard]] inline Reason Judge(Params const& p, BotState const& s, std::uint64_t nowMs, bool anchorDangerous)
{
    if (s.phase == Phase::None)
        return Reason::Issued;
    if (s.kills >= s.target)
        return Reason::Done;
    if (*ExpiredCause(p, s, nowMs, anchorDangerous))
        return Reason::Expired;
    return Reason::Issued;
}

// An errand run (sell / repair / restock) stops the contract clocks; Resume shifts the timeout start and
// the last kill by the paused span (a kill credited during the errand stays at most `now`).
inline void Pause(BotState& s, std::uint64_t nowMs)
{
    if (s.paused)
        return;
    s.paused = true;
    s.pauseSinceMs = nowMs;
}

inline void Resume(BotState& s, std::uint64_t nowMs)
{
    if (!s.paused)
        return;
    std::uint64_t const span = nowMs >= s.pauseSinceMs ? nowMs - s.pauseSinceMs : 0;
    s.startMs += span;
    s.lastKillMs = std::min(s.lastKillMs + span, nowMs);
    s.paused = false;
    s.pauseSinceMs = 0;
}

// The contract ends (any reason): its anchor cools down for CooldownMs (a done pack respawns, a failed
// one is not re-issued at once); the next search may run now.
inline void Finish(BotState& s, Params const& p, std::uint64_t nowMs)
{
    BotState next;
    next.cooldownMap = s.map;
    next.cooldownX = s.x;
    next.cooldownY = s.y;
    next.cooldownUntilMs = nowMs + p.cooldownMs;
    next.nextSearchMs = nowMs;
    s = next;
}

// A cluster anchored within ClusterYards of the last ended contract's anchor (the same pack) waits out
// the cooldown.
[[nodiscard]] inline bool AnchorCooling(Params const& p, BotState const& s, std::uint32_t map, std::int32_t x,
                                        std::int32_t y, std::uint64_t nowMs)
{
    return nowMs < s.cooldownUntilMs && map == s.cooldownMap && Within(x, y, s.cooldownX, s.cooldownY, p.clusterYards);
}

// Travel <-> Hunt: arrive within ArriveYards of the anchor; leave the hunt beyond LeashYards.
inline constexpr std::uint32_t kArriveYards = 30;
[[nodiscard]] inline Phase NextPhase(Params const& p, BotState const& s, std::uint32_t map, std::int32_t bx,
                                     std::int32_t by)
{
    if (s.phase == Phase::None)
        return Phase::None;
    if (map != s.map)
        return Phase::Travel;
    if (s.phase == Phase::Travel)
        return Within(bx, by, s.x, s.y, kArriveYards) ? Phase::Hunt : Phase::Travel;
    return Within(bx, by, s.x, s.y, p.leashYards) ? Phase::Hunt : Phase::Travel;
}

// Something else moved the bot far off (zone-progression trip, flight, death relocation): farther than
// SearchYards + LeashYards from the anchor, a distance the contract's own walk never produces.
[[nodiscard]] inline bool Displaced(Params const& p, BotState const& s, std::int32_t bx, std::int32_t by)
{
    return s.phase != Phase::None && !Within(bx, by, s.x, s.y, p.searchYards + p.leashYards);
}

// Grind filter: a proactive target of a bot with a contract must be a contract entry within LeashYards
// of the anchor.
[[nodiscard]] inline bool HuntTarget(Params const& p, BotState const& s, std::uint32_t entry, std::uint32_t map,
                                     std::int32_t x, std::int32_t y)
{
    return Accepts(s, entry) && map == s.map && Within(x, y, s.x, s.y, p.leashYards);
}

// Ledger `contract` trailing fields (append only). `cause` (expired: timeout|stall|danger; abandoned: the
// abandon cause) is appended when given.
inline std::string LedgerFields(BotState const& s, std::uint64_t nowMs, char const* cause = "")
{
    return ",\"issuer\":\"faction_board\",\"cid\":" + std::to_string(s.id) + ",\"amap\":" + std::to_string(s.map) +
           ",\"ax\":" + std::to_string(s.x) + ",\"ay\":" + std::to_string(s.y) +
           ",\"anchor\":" + std::to_string(s.anchorSpawn) + ",\"entries\":" + std::to_string(s.entryCount) +
           ",\"kills\":" + std::to_string(s.kills) + ",\"target\":" + std::to_string(s.target) +
           ",\"dur_ms\":" + std::to_string(nowMs >= s.startMs ? nowMs - s.startMs : 0) +
           (*cause ? ",\"cause\":\"" + std::string(cause) + "\"" : std::string());
}

// ---- runtime (NewRpgContracts.cpp) ----------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }

// Reads the AutoWow.Contracts.* keys; with the flag on builds the spawn index (world init, after spawns).
void LoadConfig();
// Snapshot of the bot's contract (phase None = no contract). Flag off: always None.
BotState Snapshot(std::uint32_t guid);
// Kill credit (core kill hook): counts an accepted entry toward the holder's contract.
void CreditKill(std::uint32_t guid, std::uint32_t entry);
// The spawn index filter (any flag state): true + `out` filled when the world-DB spawn is huntable (random-bot
// map, normal rank, attackable, no service flags, some team may hunt it). AutoWow.Squad indexes its sources with it.
bool HuntSpawn(std::uint32_t spawnId, CreatureData const& data, Spawn& out);
}  // namespace AutoWowContracts

#endif  // AUTOWOW_CONTRACTS_POLICY_H
