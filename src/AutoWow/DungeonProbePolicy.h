/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_DUNGEON_PROBE_POLICY_H
#define AUTOWOW_DUNGEON_PROBE_POLICY_H

// Dungeon completability probe (AutoWow.DungeonProbe.Enable, default 0; runtime DungeonProbeRuntime.cpp).
//
// Probe parties are TEST INSTRUMENTS, not faction members (owner ruling 2026-09-27): magic level, gear, spec,
// teleports and resurrections are allowed for them and never for cohort / role / squad bots. Per party the
// world thread loops the dungeon queue: Prepare (level band, fixture gear, fixed spec, pet, fresh party) ->
// Enter (probe teleport into the instance, leader first) -> Inside (the dungeon navigator leads; wipes are
// resurrected at the instance entrance up to MaxWipes; stuck / timeout end the run) -> Exit (homebind,
// disband, unbind) -> next dungeon. Ledger `dprobe` (event 24) rows per run event.
//
// Value-only (no world access, no floats in decisions, no RNG); stable orders (guid ascending).

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AutoWowDungeonProbe
{
inline constexpr std::uint8_t kStateVersion = 1;
inline constexpr std::uint32_t kMaxPartySize = 5;
inline constexpr std::uint32_t kMinProbeQuality = 2;  // uncommon: the last rung of the gear ladder

// ---- config parsing ----------------------------------------------------------------------------------------
struct PartyDef
{
    std::string name;                  // [A-Za-z0-9_-], at most 32 chars (ledger `party`)
    std::vector<std::uint32_t> guids;  // ascending, distinct, non-zero, 1..5
};

namespace detail
{
inline bool ParseU32(std::string_view s, std::uint32_t& out)
{
    if (s.empty() || s.size() > 10)
        return false;
    std::uint64_t v = 0;
    for (char const c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + std::uint64_t(c - '0');
    }
    if (v > 0xFFFFFFFFull)
        return false;
    out = std::uint32_t(v);
    return true;
}

inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

inline std::vector<std::string_view> Split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (true)
    {
        std::size_t const end = std::min(s.find(sep, i), s.size());
        out.push_back(Trim(s.substr(i, end - i)));
        if (end == s.size())
            return out;
        i = end + 1;
    }
}

inline bool ValidName(std::string_view n)
{
    if (n.empty() || n.size() > 32)
        return false;
    for (char const c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}
}  // namespace detail

// "name:g,g,g,g,g;name2:..." in config order. A malformed party (bad name, bad / zero / repeated guid, 0 or more
// than five guids, a duplicate name, a guid already in an earlier party) is skipped whole.
inline std::vector<PartyDef> ParseParties(std::string_view text)
{
    std::vector<PartyDef> out;
    for (std::string_view entry : detail::Split(text, ';'))
    {
        if (entry.empty())
            continue;
        std::size_t const colon = entry.find(':');
        if (colon == std::string_view::npos)
            continue;
        PartyDef def;
        std::string_view const name = detail::Trim(entry.substr(0, colon));
        bool ok = detail::ValidName(name);
        def.name = std::string(name);
        for (std::string_view g : detail::Split(entry.substr(colon + 1), ','))
        {
            std::uint32_t guid = 0;
            if (!detail::ParseU32(g, guid) || !guid)
                ok = false;
            else
                def.guids.push_back(guid);
        }
        std::sort(def.guids.begin(), def.guids.end());
        ok = ok && !def.guids.empty() && def.guids.size() <= kMaxPartySize &&
             std::adjacent_find(def.guids.begin(), def.guids.end()) == def.guids.end();
        for (PartyDef const& earlier : out)
        {
            ok = ok && earlier.name != def.name;
            for (std::uint32_t const g : def.guids)
                ok = ok && !std::binary_search(earlier.guids.begin(), earlier.guids.end(), g);
        }
        if (ok)
            out.push_back(std::move(def));
    }
    return out;
}

// "36,43,389": dungeon map ids in queue order; malformed / zero skipped, first occurrence kept.
inline std::vector<std::uint32_t> ParseQueue(std::string_view text)
{
    std::vector<std::uint32_t> out;
    for (std::string_view t : detail::Split(text, ','))
    {
        std::uint32_t map = 0;
        if (detail::ParseU32(t, map) && map && std::find(out.begin(), out.end(), map) == out.end())
            out.push_back(map);
    }
    return out;
}

struct LevelOverride
{
    std::uint32_t map = 0;
    std::uint32_t level = 0;
};

// "36:26,43:24": per-map recommended max level overrides; malformed / zero skipped, last one wins.
inline std::vector<LevelOverride> ParseLevels(std::string_view text)
{
    std::vector<LevelOverride> out;
    for (std::string_view t : detail::Split(text, ','))
    {
        std::size_t const colon = t.find(':');
        LevelOverride o;
        if (colon == std::string_view::npos || !detail::ParseU32(detail::Trim(t.substr(0, colon)), o.map) ||
            !detail::ParseU32(detail::Trim(t.substr(colon + 1)), o.level) || !o.map || !o.level)
            continue;
        out.erase(std::remove_if(out.begin(), out.end(), [&o](LevelOverride const& e) { return e.map == o.map; }),
                  out.end());
        out.push_back(o);
    }
    return out;
}

// ---- level band --------------------------------------------------------------------------------------------
// Recommended max level per classic 5-man (upper end of the Blizzard recommended range). AutoWow.DungeonProbe.Levels
// overrides or extends it (TBC / WotLK / raids later).
inline constexpr std::array<LevelOverride, 19> kRecommendedMaxLevel = {{
    {389, 18},  // Ragefire Chasm
    {43, 24},   // Wailing Caverns
    {36, 26},   // The Deadmines
    {33, 30},   // Shadowfang Keep
    {48, 32},   // Blackfathom Deeps
    {34, 32},   // The Stockade
    {90, 38},   // Gnomeregan
    {47, 38},   // Razorfen Kraul
    {189, 45},  // Scarlet Monastery
    {129, 46},  // Razorfen Downs
    {70, 51},   // Uldaman
    {209, 54},  // Zul'Farrak
    {349, 55},  // Maraudon
    {109, 60},  // Sunken Temple
    {230, 60},  // Blackrock Depths
    {229, 60},  // Blackrock Spire
    {429, 60},  // Dire Maul
    {329, 60},  // Stratholme
    {289, 60},  // Scholomance
}};

// The probe level for a dungeon: recommended max + levelOver, capped at maxPlayerLevel. 0 = unknown map.
inline std::uint32_t BandLevel(std::uint32_t map, std::uint32_t levelOver, std::vector<LevelOverride> const& overrides,
                               std::uint32_t maxPlayerLevel)
{
    std::uint32_t rec = 0;
    for (LevelOverride const& o : kRecommendedMaxLevel)
        if (o.map == map)
            rec = o.level;
    for (LevelOverride const& o : overrides)
        if (o.map == map)
            rec = o.level;
    if (!rec || !maxPlayerLevel)
        return 0;
    return std::min(rec + levelOver, maxPlayerLevel);
}

// ---- fixture ------------------------------------------------------------------------------------------------
// Fixed role spec per class (AiPlayerbot.PremadeSpecName index): warrior prot pve, priest holy pve, mage frost pve,
// rogue combat pve, hunter bm pve. -1 = the bot's stored spec (other classes).
inline constexpr std::int32_t SpecFor(std::uint32_t cls)
{
    switch (cls)
    {
        case 1: return 2;  // warrior: prot pve
        case 5: return 1;  // priest: holy pve
        case 8: return 2;  // mage: frost pve
        case 4: return 1;  // rogue: combat pve
        case 3: return 0;  // hunter: bm pve
        default: return -1;
    }
}

// Exact-quality gear ladder: `best` down to uncommon (epic does not exist for every slot at every level; the
// fixture factory refuses a partial loadout, so the next rung is tried).
inline std::vector<std::uint32_t> QualityLadder(std::uint32_t best)
{
    std::vector<std::uint32_t> out;
    for (std::uint32_t q = std::min<std::uint32_t>(best, 5); q >= kMinProbeQuality; --q)
        out.push_back(q);
    return out;
}

// ---- run ---------------------------------------------------------------------------------------------------
// Wire-stable ledger `end`; append only.
enum class End : std::uint8_t
{
    None = 0,
    Completed,
    Abandoned,
    Stuck,
    Timeout,
    WipedOut,
    EnterFailed,
    PrepareFailed
};

inline constexpr char const* EndName(End e)
{
    switch (e)
    {
        case End::Completed: return "completed";
        case End::Abandoned: return "abandoned";
        case End::Stuck: return "stuck";
        case End::Timeout: return "timeout";
        case End::WipedOut: return "wiped_out";
        case End::EnterFailed: return "enter_failed";
        case End::PrepareFailed: return "prepare_failed";
        case End::None: return "";
    }
    return "";
}

// No progress for stuckMs = one stuck event. Progress = the leader moved at least `yards` (integer yards, 3D) from
// the anchor, or the caller saw a boss kill / party combat. A stuck event re-anchors, so events are stuckMs apart.
struct StuckTracker
{
    std::int32_t x = 0, y = 0, z = 0;
    std::uint64_t sinceMs = 0;
    bool armed = false;
};

inline bool NoteProgress(StuckTracker& t, std::int32_t x, std::int32_t y, std::int32_t z, bool progressed,
                         std::uint64_t nowMs, std::uint32_t yards, std::uint64_t stuckMs)
{
    std::int64_t const dx = x - t.x, dy = y - t.y, dz = z - t.z;
    if (!t.armed || progressed || dx * dx + dy * dy + dz * dz >= std::int64_t(yards) * yards)
    {
        t = {x, y, z, nowMs, true};
        return false;
    }
    if (nowMs < t.sinceMs + stuckMs)
        return false;
    t.sinceMs = nowMs;
    return true;
}

struct InsideFacts
{
    std::uint32_t size = 0;
    std::uint32_t online = 0;
    std::uint32_t alive = 0;
    bool anyInCombat = false;       // a living member in combat
    bool leaderOnDungeonMap = true;  // alive leader left the dungeon map = abandoned
    std::uint32_t mask = 0;
    std::uint32_t allMask = 0;
    std::uint32_t wipes = 0;         // wipes so far (this one excluded)
    std::uint32_t stucks = 0;        // stuck events so far (incl. one raised this tick)
    std::uint64_t nowMs = 0;
    std::uint64_t enteredMs = 0;
    std::uint64_t quietDeadSinceMs = 0;  // some member dead and nobody in combat since (0 = not)
};

struct InsideParams
{
    std::uint32_t maxWipes = 3;
    std::uint32_t stuckLimit = 2;
    std::uint64_t runTimeoutMs = 3600000;
    std::uint64_t reviveGraceMs = 30000;
};

enum class Verdict : std::uint8_t
{
    Continue,
    Revive,   // probe magic: resurrect the dead members at the leader
    Wiped,    // resurrect everyone at the entrance and continue the same instance
    Finish    // see End
};

// Fixed decision order: offline -> completed -> wipe -> timeout -> stuck -> revive.
inline Verdict DecideInside(InsideFacts const& f, InsideParams const& p, End& end)
{
    end = End::None;
    if (f.online < f.size || !f.leaderOnDungeonMap)
    {
        end = End::Abandoned;
        return Verdict::Finish;
    }
    if (f.allMask && (f.mask & f.allMask) == f.allMask)
    {
        end = End::Completed;
        return Verdict::Finish;
    }
    if (!f.alive)
    {
        if (f.wipes + 1 > p.maxWipes)
        {
            end = End::WipedOut;
            return Verdict::Finish;
        }
        return Verdict::Wiped;
    }
    if (f.nowMs >= f.enteredMs + p.runTimeoutMs)
    {
        end = End::Timeout;
        return Verdict::Finish;
    }
    if (p.stuckLimit && f.stucks >= p.stuckLimit)
    {
        end = End::Stuck;
        return Verdict::Finish;
    }
    if (f.quietDeadSinceMs && f.nowMs >= f.quietDeadSinceMs + p.reviveGraceMs)
        return Verdict::Revive;
    return Verdict::Continue;
}

// One DungeonEncounter record of the dungeon. A kill-credit record counts only when that creature has a static
// spawn on the dungeon map; a boss summoned only by a script event (RFK Grubbis 7361, escort) can never be
// reached by the navigator, so a full clear could never be scored. Spell / script credit records always count.
struct EncounterRecord
{
    std::uint32_t index = 0;
    bool killCredit = false;
    bool staticSpawn = false;
};

// Full-clear mask: every encounter index with at least one countable record.
inline std::uint32_t ClearableMask(std::vector<EncounterRecord> const& records)
{
    std::uint32_t mask = 0;
    for (EncounterRecord const& r : records)
        if (r.index < 32 && (!r.killCredit || r.staticSpawn))
            mask |= 1u << r.index;
    return mask;
}

// Index of the next undone encounter (lowest bit of all & ~mask); -1 = none.
inline std::int32_t NextEncounter(std::uint32_t mask, std::uint32_t allMask)
{
    for (std::uint32_t i = 0; i < 32; ++i)
        if ((allMask >> i & 1u) && !(mask >> i & 1u))
            return std::int32_t(i);
    return -1;
}

inline std::uint32_t Bits(std::uint32_t v)
{
    std::uint32_t n = 0;
    for (; v; v &= v - 1)
        ++n;
    return n;
}

// ---- ledger ------------------------------------------------------------------------------------------------
// One run's record. Every `dprobe` row carries the same trailing fields (the reducer folds the `run` row).
struct RunRecord
{
    std::string party;
    std::uint32_t rid = 0;    // run-scoped probe run id, never reused
    std::uint32_t map = 0;
    std::uint32_t level = 0;  // probe level used
    std::uint32_t quality = 0;  // lowest gear quality any member got
    std::uint32_t instance = 0;
    std::uint32_t mask = 0;
    std::uint32_t allMask = 0;
    std::uint32_t wipes = 0;
    std::uint32_t revives = 0;
    std::uint32_t stucks = 0;
    std::vector<std::uint32_t> members;  // ascending
    std::vector<std::uint32_t> deaths;   // same order
    std::uint64_t startMs = 0;           // prepare start
    End end = End::None;
};

struct StuckPoint
{
    std::uint32_t map = 0;
    std::int32_t x = 0, y = 0, z = 0;  // leader, integer yards
    std::int32_t next = -1;           // next undone encounter index
    std::uint32_t boss = 0;           // its credit entry (0 = unknown)
};

inline std::string List(std::vector<std::uint32_t> const& v)
{
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? "," : "") + std::to_string(v[i]);
    return out + "]";
}

// Trailing fields of a `dprobe` row. enc = the encounter index of a boss_killed row, else -1.
inline std::string Fields(RunRecord const& r, std::uint64_t nowMs, std::int32_t enc = -1, StuckPoint const* s = nullptr)
{
    std::string out = ",\"party\":\"" + r.party + "\",\"rid\":" + std::to_string(r.rid) + ",\"dmap\":" +
                      std::to_string(r.map) + ",\"plvl\":" + std::to_string(r.level) + ",\"q\":" +
                      std::to_string(r.quality) + ",\"inst\":" + std::to_string(r.instance) + ",\"enc\":" +
                      std::to_string(enc) + ",\"mask\":" + std::to_string(r.mask) + ",\"all\":" +
                      std::to_string(r.allMask) + ",\"bosses\":" + std::to_string(Bits(r.mask & r.allMask)) +
                      ",\"total\":" + std::to_string(Bits(r.allMask)) + ",\"wipes\":" + std::to_string(r.wipes) +
                      ",\"revives\":" + std::to_string(r.revives) + ",\"stucks\":" + std::to_string(r.stucks) +
                      ",\"deaths\":" + List(r.deaths) + ",\"dur_ms\":" +
                      std::to_string(nowMs >= r.startMs ? nowMs - r.startMs : 0) + ",\"end\":\"" + EndName(r.end) +
                      "\",\"members\":" + List(r.members);
    if (s)
        out += ",\"smap\":" + std::to_string(s->map) + ",\"sx\":" + std::to_string(s->x) + ",\"sy\":" +
               std::to_string(s->y) + ",\"sz\":" + std::to_string(s->z) + ",\"next\":" + std::to_string(s->next) +
               ",\"boss\":" + std::to_string(s->boss);
    return out;
}

// ---- runtime (AutoWow/DungeonProbeRuntime.cpp) ----------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;                   // AutoWow.DungeonProbe.Enable and at least one accepted party
inline std::vector<std::uint32_t> gProbeGuids;  // accepted probe guids, ascending; filled once at world init
}
inline bool Enabled() { return detail::gEnabled; }
// An accepted probe party member (AutoWow.DungeonProbe.Parties, even with the flag off). Cohort / supply /
// squad / contract / party-formation features skip these bots. Read-only after world init (any thread).
inline bool IsProbeBot(std::uint32_t guid)
{
    return !detail::gProbeGuids.empty() &&
           std::binary_search(detail::gProbeGuids.begin(), detail::gProbeGuids.end(), guid);
}

// World init, after Guilds / Supply / Squad (a party with a cohort, role or squad guid is refused whole).
void LoadConfig();
// World thread (Playerbots.cpp WorldScript::OnUpdate).
void WorldUpdate(std::uint32_t diff);
}  // namespace AutoWowDungeonProbe

#endif  // AUTOWOW_DUNGEON_PROBE_POLICY_H
