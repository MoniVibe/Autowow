/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_SQUAD_POLICY_H
#define AUTOWOW_SQUAD_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "ContractsPolicy.h"
#include "SupplyPolicy.h"

class Player;

// Gatherer squad (docs/SUPPLY_CHAIN_PLAN.md "Gatherer squad", AutoWow.Squad.Enable, default 0). A configured
// roster per team (AutoWow.Squad.Alliance / .Horde) puts the supply overlord's material demand before levelling:
// the world thread ranks the demand (AutoWowSupply::MaterialDemand: routing room = house stock target minus rep
// stock, for materials the house can use now), picks the nearest level-safe source cluster of the top workable
// material near the squad leader (humanoids whose loot has the cloth, skinnable beasts whose skinning loot has the
// leather, herb / ore node spawns whose lock the squad's gatherers meet) and runs a stint there: the members walk to
// the anchor together (the leader waits for stragglers), hunt / gather within the leash (GrindTargetValue narrows
// proactive targets, the gathering detour takes the nodes, the stock loot / skinning strategies loot), until the
// squad holds the wanted units, the demand is gone, the anchor turns dangerous or StintMs passes. Materials reach
// the house reps through the ordinary errand sell stop routing (AutoWowSupply::RouteCloth). While a workable
// demand exists the squad holds its tier: zone progression does not graduate its members. No demand: the members
// quest like everyone else. The roster is one core party (AutoWowParty::EnsureSquad, reason squad). Ledger
// `squad` (event 22).
//
// Value-only: integer yards and game-time ms, no floats in decisions, no RNG, no strings in decisions; stable
// orders (count, then material table order; distance, then spawns, then spawn id).
namespace AutoWowSquad
{
inline constexpr std::uint8_t kStateVersion = 2;  // 2: LevelWindow zone cooldown (cooldownZone, anchorZone)
inline constexpr std::size_t kMaxMembers = 10;    // per team across all material crews
inline constexpr std::size_t kMaxCrewMembers = 5;  // native ordinary-group capacity
inline constexpr std::size_t kTeamCount = 2;
inline constexpr std::size_t kKindCount = 4;
inline constexpr std::size_t kCrewCount = kTeamCount * kKindCount;

struct GuidSpan
{
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
};

// Expands a parsed provenance roster atomically. The destination changes only when every span fits the cap.
[[nodiscard]] inline bool ExpandGuidSpansBounded(std::vector<GuidSpan> const& spans, std::size_t cap,
                                                 std::vector<std::uint32_t>& out)
{
    std::vector<std::uint32_t> candidate;
    for (GuidSpan const& span : spans)
    {
        if (!span.lo || span.hi < span.lo)
            return false;
        std::uint64_t const count = std::uint64_t(span.hi) - std::uint64_t(span.lo) + 1;
        if (count > cap || candidate.size() > cap - static_cast<std::size_t>(count))
            return false;
        for (std::uint64_t guid = span.lo; guid <= span.hi; ++guid)
            candidate.push_back(static_cast<std::uint32_t>(guid));
    }
    std::sort(candidate.begin(), candidate.end());
    candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
    out.swap(candidate);
    return true;
}

using AutoWowContracts::Cluster;
using AutoWowContracts::Spawn;

// Wire-stable (ledger `kind`); append only.
enum class Kind : std::uint8_t
{
    Cloth = 0,
    Herb = 1,
    Ore = 2,
    Leather = 3
};

inline constexpr char const* KindName(Kind k)
{
    switch (k)
    {
        case Kind::Cloth: return "cloth";
        case Kind::Herb: return "herb";
        case Kind::Ore: return "ore";
        case Kind::Leather: return "leather";
    }
    return "cloth";
}

[[nodiscard]] inline constexpr std::size_t KindIndex(Kind kind) { return static_cast<std::size_t>(kind); }

[[nodiscard]] inline constexpr std::size_t CrewIndex(std::size_t team, Kind kind)
{
    return team * kKindCount + KindIndex(kind);
}

[[nodiscard]] inline constexpr std::size_t CrewTeam(std::size_t crew) { return crew / kKindCount; }
[[nodiscard]] inline constexpr Kind CrewKind(std::size_t crew) { return static_cast<Kind>(crew % kKindCount); }

// An opt-in material-crew layout is accepted whole or rejected whole. A guid has one primary crew; secondary
// learned professions are untouched. Empty crews are valid. Stable slot order is team, then wire-stable Kind.
enum class CrewLayoutError : std::uint8_t
{
    None = 0,
    ZeroGuid,
    DuplicateGuid,
    CrewOverCap,
    TeamOverCap
};

struct CrewLayout
{
    CrewLayoutError error = CrewLayoutError::None;
    std::array<std::vector<std::uint32_t>, kCrewCount> crews;
    std::array<std::vector<std::uint32_t>, kTeamCount> teams;

    [[nodiscard]] bool Valid() const { return error == CrewLayoutError::None; }
};

[[nodiscard]] inline CrewLayout BuildCrewLayout(std::array<std::vector<std::uint32_t>, kCrewCount> const& configured)
{
    CrewLayout out;
    std::vector<std::uint32_t> seen;
    for (std::size_t crew = 0; crew < kCrewCount; ++crew)
    {
        if (configured[crew].size() > kMaxCrewMembers)
        {
            out = {};
            out.error = CrewLayoutError::CrewOverCap;
            return out;
        }
        for (std::uint32_t const guid : configured[crew])
        {
            if (!guid)
            {
                out = {};
                out.error = CrewLayoutError::ZeroGuid;
                return out;
            }
            if (std::find(seen.begin(), seen.end(), guid) != seen.end())
            {
                out = {};
                out.error = CrewLayoutError::DuplicateGuid;
                return out;
            }
            seen.push_back(guid);
            out.crews[crew].push_back(guid);
            out.teams[CrewTeam(crew)].push_back(guid);
        }
        std::sort(out.crews[crew].begin(), out.crews[crew].end());
    }
    for (auto& team : out.teams)
    {
        if (team.size() > kMaxMembers)
        {
            out = {};
            out.error = CrewLayoutError::TeamOverCap;
            return out;
        }
        std::sort(team.begin(), team.end());
    }
    return out;
}

// Never reuse a stint id in this process. Exhaustion fails closed rather than wrapping to an old id.
[[nodiscard]] inline bool NextStintId(std::uint32_t& last, std::uint32_t& next)
{
    if (last == std::numeric_limits<std::uint32_t>::max())
    {
        next = 0;
        return false;
    }
    next = ++last;
    return true;
}

// The materials the squad can source (3.3.5 item ids, checked against the world DB loot tables), tier order
// within a kind: the order ties are broken in.
struct Material
{
    std::uint32_t item = 0;
    Kind kind = Kind::Cloth;
};

inline constexpr Material kMaterials[] = {
    {2589, Kind::Cloth},   {2592, Kind::Cloth},   {4306, Kind::Cloth},                        // linen, wool, silk
    {2447, Kind::Herb},    {765, Kind::Herb},     {2450, Kind::Herb},   {2453, Kind::Herb},   // peacebloom,
                                                                                               // silverleaf, briarthorn,
                                                                                               // bruiseweed
    {2770, Kind::Ore},     {2771, Kind::Ore},     {2772, Kind::Ore},                          // copper, tin, iron ore
    {2934, Kind::Leather}, {2318, Kind::Leather}, {2319, Kind::Leather},                      // ruined scraps, light,
                                                                                               // medium leather
    {2835, Kind::Ore}};                                                                         // rough stone
inline constexpr std::size_t kMaterialCount = std::size(kMaterials);
inline constexpr std::size_t kNoMaterial = kMaterialCount;

[[nodiscard]] inline std::size_t MaterialIndex(std::uint32_t item)
{
    for (std::size_t i = 0; i < kMaterialCount; ++i)
        if (kMaterials[i].item == item)
            return i;
    return kNoMaterial;
}

// Profession tools (the lists LootObject::IsLootPossible checks).
inline constexpr std::uint32_t kMiningPicks[] = {756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893};
inline constexpr std::uint32_t kSkinningKnives[] = {7005, 40772, 40893, 12709, 19901};

struct Params
{
    std::uint32_t tickMs = 30000;          // AutoWow.Squad.TickMs: world tick stride
    std::uint32_t stintMs = 1800000;       // AutoWow.Squad.StintMs
    std::uint32_t levelAbove = 1;          // AutoWow.Squad.LevelAbove: source max level <= squad avg level + this
    std::uint32_t levelBelow = 255;        // AutoWow.Squad.LevelBelow: source min level >= avg - this (grey ok)
    std::uint32_t zoneLevelMargin = 3;     // AutoWow.Squad.ZoneLevelMargin: anchor zone bracket low <= avg + this
    std::uint32_t searchYards = 2500;      // AutoWow.Squad.SearchYards: sources this far from the leader
    std::uint32_t clusterYards = 120;      // AutoWow.Squad.ClusterYards
    std::uint32_t minClusterSpawns = 5;    // AutoWow.Squad.MinClusterSpawns
    std::uint32_t leashYards = 150;        // AutoWow.Squad.LeashYards
    std::uint32_t togetherYards = 60;      // AutoWow.Squad.TogetherYards: the leader waits for members beyond it
    std::uint32_t minDemand = 5;           // AutoWow.Squad.MinDemand: smaller wants are not worth a stint
    std::uint32_t minDropPct = 5;          // AutoWow.Squad.MinDropPct: loot rows below this chance are no source
    std::uint32_t cooldownMs = 600000;     // AutoWow.Squad.CooldownMs: the last anchor rests this long
    std::uint32_t searchRetryMs = 60000;   // AutoWow.Squad.SearchRetryMs: no-stint search stride
    std::uint32_t holdLogMs = 600000;      // `hold` rows per member at most this often
    // AutoWow.Squad.LevelWindow (default 0 = off; LevelWindowOn documents the ON behaviour).
    std::uint32_t levelWindow = 0;         // AutoWow.Squad.LevelWindow: zone bracket low <= level + this
    std::uint32_t deathCluster = 3;        // AutoWow.Squad.DeathCluster: recent deaths (death-loop window)
    std::uint32_t dangerZoneMs = 3600000;  // AutoWow.Squad.DangerZoneMs: a death-cluster anchor zone rests this long
    std::uint32_t benchMs = 1800000;       // AutoWow.Squad.BenchMs: a bench lasts this long (no re-evaluation)
};

// ---- demand ----------------------------------------------------------------------------------------------

struct Want
{
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    Kind kind = Kind::Cloth;
    bool first = false;  // MaterialNeed.first: the bag artisan's current tier
};

// The supply needs the squad can source (items in kMaterials, at least MinDemand units; an item listed twice keeps
// its larger count and either first flag): first ones, then largest, ties by material table order (linen before
// wool, copper before tin).
[[nodiscard]] inline std::vector<Want> RankDemand(std::vector<AutoWowSupply::MaterialNeed> const& needs,
                                                  std::uint32_t minDemand)
{
    std::vector<Want> out;
    for (AutoWowSupply::MaterialNeed const& n : needs)
    {
        std::size_t const m = MaterialIndex(n.item);
        if (m == kNoMaterial || !n.count || n.count < minDemand)
            continue;
        auto it = std::find_if(out.begin(), out.end(), [&](Want const& w) { return w.item == n.item; });
        if (it == out.end())
            out.push_back({n.item, n.count, kMaterials[m].kind, n.first});
        else
        {
            it->count = std::max(it->count, n.count);
            it->first = it->first || n.first;
        }
    }
    std::sort(out.begin(), out.end(), [](Want const& a, Want const& b)
              {
                  if (a.first != b.first)
                      return a.first;
                  return a.count != b.count ? a.count > b.count : MaterialIndex(a.item) < MaterialIndex(b.item);
              });
    return out;
}

[[nodiscard]] inline std::vector<Want> RankDemandForKind(std::vector<AutoWowSupply::MaterialNeed> const& needs,
                                                         std::uint32_t minDemand, Kind kind)
{
    std::vector<Want> out = RankDemand(needs, minDemand);
    out.erase(std::remove_if(out.begin(), out.end(), [kind](Want const& w) { return w.kind != kind; }), out.end());
    return out;
}

[[nodiscard]] inline bool Demanded(std::vector<Want> const& ranked, std::uint32_t item)
{
    return std::any_of(ranked.begin(), ranked.end(), [&](Want const& w) { return w.item == item; });
}

// ---- level safety and skills ----------------------------------------------------------------------------

// The squad's best gathering skill per profession; 0 = nobody has it with its tool.
struct Skills
{
    std::uint32_t herbalism = 0;
    std::uint32_t mining = 0;
    std::uint32_t skinning = 0;
    bool anySkill = false;  // AutoWow.Gather.AnySkill: a learned skill works any source (no requirement gate)
};

// Integer mean of the members' levels (0 for none).
[[nodiscard]] inline std::uint32_t AvgLevel(std::vector<std::uint32_t> const& levels)
{
    if (levels.empty())
        return 0;
    std::uint64_t sum = 0;
    for (std::uint32_t const l : levels)
        sum += l;
    return static_cast<std::uint32_t>(sum / levels.size());
}

// Skinning skill a corpse of `level` needs (3.3.5 Spell::EffectSkinning).
[[nodiscard]] inline constexpr std::uint32_t SkinReq(std::uint32_t level)
{
    return level < 10 ? 0 : level < 20 ? (level - 10) * 10 : level * 5;
}

// Can the squad work a source of `kind` whose requirement is `req` (node lock skill; skinning skill)?
[[nodiscard]] inline bool CanWork(Kind kind, Skills const& s, std::uint32_t req)
{
    switch (kind)
    {
        case Kind::Cloth: return true;
        case Kind::Herb: return s.herbalism && (s.anySkill || s.herbalism >= req);
        case Kind::Ore: return s.mining && (s.anySkill || s.mining >= req);
        case Kind::Leather: return s.skinning && (s.anySkill || s.skinning >= req);
    }
    return false;
}

[[nodiscard]] inline constexpr bool NodeKind(Kind k) { return k == Kind::Herb || k == Kind::Ore; }

// The contracts cluster machinery with the squad's window: level range [avg - LevelBelow, avg + LevelAbove].
[[nodiscard]] inline AutoWowContracts::Params ClusterParams(Params const& p)
{
    AutoWowContracts::Params c;
    c.levelBelow = p.levelBelow;
    c.levelAbove = p.levelAbove;
    c.levelGapMin = 0;
    c.searchYards = p.searchYards;
    c.clusterYards = p.clusterYards;
    c.leashYards = p.leashYards;
    c.minClusterSpawns = std::max<std::uint32_t>(1, p.minClusterSpawns);
    return c;
}

// The item's indexed sources on the leader's map the squad can work: cloth any; leather when a skinner meets the
// corpse's skinning requirement (its max level); herb / ore when a gatherer meets the node lock (reqOf(entry)).
// Node spawns carry level 1..1, so the level window never rejects them (their zone bracket is checked at pick).
template <typename ReqOf>
[[nodiscard]] std::vector<Spawn> Workable(std::vector<Spawn> const& mapSources, Kind kind, Skills const& s,
                                          ReqOf&& reqOf)
{
    std::vector<Spawn> out;
    for (Spawn const& sp : mapSources)
    {
        std::uint32_t const req = kind == Kind::Leather ? SkinReq(sp.maxLevel) : NodeKind(kind) ? reqOf(sp.entry) : 0;
        if (CanWork(kind, s, req))
            out.push_back(sp);
    }
    return out;
}

// Nearest source first: the ranked clusters re-sorted by distance to the leader, then more spawns, then lower
// center spawn id.
[[nodiscard]] inline std::vector<Cluster> RankNearest(std::vector<Cluster> clusters)
{
    std::sort(clusters.begin(), clusters.end(), [](Cluster const& a, Cluster const& b)
              {
                  if (a.dist2 != b.dist2)
                      return a.dist2 < b.dist2;
                  return a.spawns != b.spawns ? a.spawns > b.spawns : a.center < b.center;
              });
    return clusters;
}

// ---- team stint state -----------------------------------------------------------------------------------

enum class Phase : std::uint8_t
{
    None = 0,  // no stint: the members quest
    Stint = 1  // working a source
};

// Ledger `squad` reason. Wire-stable; append only.
enum class Reason : std::uint8_t
{
    Stint = 0,    // a stint started (material, target, anchor)
    Gather = 1,   // a stint ended; count = units gathered, cause = done|demand_met|danger|timeout|stuck|empty|benched
    Deliver = 2,  // a member mailed squad materials to a house rep (errand sell stop routing)
    Hold = 3,     // zone progression deferred for a member (tier hold)
    Release = 4,  // no workable demand left: the squad quests
    Bench = 5     // LevelWindow: a member sits the stint out; cause = route|deaths
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Stint: return "stint";
        case Reason::Gather: return "gather";
        case Reason::Deliver: return "deliver";
        case Reason::Hold: return "hold";
        case Reason::Release: return "release";
        case Reason::Bench: return "bench";
    }
    return "stint";
}

// Per-team state (fixed size, value-only). The world thread writes, members' map threads read a copy.
struct TeamState
{
    std::uint8_t version = kStateVersion;
    Phase phase = Phase::None;
    std::uint32_t id = 0;        // run-scoped stint id, never reused (both teams)
    std::uint32_t item = 0;
    Kind kind = Kind::Cloth;
    std::uint32_t target = 0;    // units wanted at stint start
    std::uint32_t gathered = 0;  // held units gained since the stint started (deliveries never lower it)
    std::uint32_t lastHeld = 0;  // the members' held units at the last tick
    std::uint32_t leader = 0;    // lowest online roster guid
    std::uint32_t map = 0;       // anchor
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t anchorSpawn = 0;
    std::array<std::uint32_t, AutoWowContracts::kMaxEntries> entries = {};  // creature (or node) entries
    std::uint8_t entryCount = 0;
    std::uint64_t startMs = 0;
    std::uint64_t nextSearchMs = 0;
    bool together = true;  // every alive member on the leader's map within TogetherYards of it
    bool holding = false;  // a workable demand exists: zone progression holds the members
    std::uint32_t cooldownMap = 0;  // anchor of the last ended stint
    std::int32_t cooldownX = 0;
    std::int32_t cooldownY = 0;
    std::uint64_t cooldownUntilMs = 0;
    std::uint32_t anchorZone = 0;           // LevelWindow: zone of the running stint's anchor (0 = unknown)
    std::uint32_t cooldownZone = 0;         // LevelWindow: the last death-cluster anchor zone
    std::uint64_t zoneCooldownUntilMs = 0;  // ... skipped by the search until then
};

inline void Issue(TeamState& s, Want const& w, Cluster const& c, std::uint32_t map, std::uint32_t leader,
                  std::uint32_t id, std::uint32_t held, std::uint64_t nowMs)
{
    s.phase = Phase::Stint;
    s.id = id;
    s.item = w.item;
    s.kind = w.kind;
    s.target = w.count;
    s.gathered = 0;
    s.lastHeld = held;
    s.leader = leader;
    s.map = map;
    s.x = c.x;
    s.y = c.y;
    s.z = c.z;
    s.anchorSpawn = c.center;
    s.entries = {};
    s.entryCount = static_cast<std::uint8_t>(std::min(c.entries.size(), AutoWowContracts::kMaxEntries));
    for (std::size_t k = 0; k < s.entryCount; ++k)
        s.entries[k] = c.entries[k];
    s.startMs = nowMs;
    s.holding = true;
}

// The members hold `held` units of the stint item now; only a rise counts (a delivery lowers held, not progress).
inline void NoteHeld(TeamState& s, std::uint32_t held)
{
    if (held > s.lastHeld)
        s.gathered += held - s.lastHeld;
    s.lastHeld = held;
}

// Why the running stint ends now ("" = it goes on). Done beats every other cause.
[[nodiscard]] inline char const* EndCause(Params const& p, TeamState const& s, std::uint64_t nowMs, bool stillDemanded,
                                         bool membersOnAnchorMap, bool danger, bool stuck)
{
    if (s.phase != Phase::Stint)
        return "";
    if (s.gathered >= s.target)
        return "done";
    if (!stillDemanded)
        return "demand_met";
    if (!membersOnAnchorMap)
        return "empty";
    if (danger)
        return "danger";
    if (stuck)
        return "stuck";
    if (nowMs >= s.startMs && nowMs - s.startMs >= p.stintMs)
        return "timeout";
    return "";
}

// The stint ends: its anchor rests CooldownMs; the next search may run now. Id, holding and roster facts stay.
inline void Finish(TeamState& s, Params const& p, std::uint64_t nowMs)
{
    s.cooldownMap = s.map;
    s.cooldownX = s.x;
    s.cooldownY = s.y;
    s.cooldownUntilMs = nowMs + p.cooldownMs;
    s.phase = Phase::None;
    s.item = 0;
    s.target = s.gathered = s.lastHeld = 0;
    s.anchorSpawn = 0;
    s.entries = {};
    s.entryCount = 0;
    s.nextSearchMs = nowMs;
}

[[nodiscard]] inline bool AnchorCooling(Params const& p, TeamState const& s, std::uint32_t map, std::int32_t x,
                                        std::int32_t y, std::uint64_t nowMs)
{
    return nowMs < s.cooldownUntilMs && map == s.cooldownMap &&
           AutoWowContracts::Within(x, y, s.cooldownX, s.cooldownY, p.clusterYards);
}

// ---- AutoWow.Squad.LevelWindow (default 0 = off) ---------------------------------------------------------
// soak-s62-full-r1 (1.5 h): 91 of 103 squad deaths were three members, none at a stint anchor: a L23 mage walking
// Hillsbrad -> the Tirisfal anchor through Alterac Mountains (bracket 30+; 32 deaths there, the death-loop escape
// dropped it back in Hillsbrad and the stint walked it in again while the leader waited for it), a L22 rogue dying
// on its walk through the Wetlands (24), and a L19 priest on another continent (35 deaths, 0 kills: the stint's
// target filter left it no grind target). Window on (LevelWindow > 0):
//   (1) an anchor's zone bracket starts at most LevelWindow above the squad average (ZoneLevelMargin otherwise),
//       the leader's straight line to it crosses no such zone, and a death-cluster zone is skipped while cooling;
//   (2) a member whose straight line to the anchor crosses a zone bracketed above its level + LevelWindow (its own
//       zone excepted), or with DeathCluster deaths in the death-loop window, is benched: its own loop runs, nobody
//       waits for it, no tier hold, no stint target filter (ledger `squad` bench, cause route|deaths);
//   (3) a member on another map than the anchor gets no stint target filter;
//   (4) a member within the leash of the anchor with DeathCluster recent deaths ends the stint (cause danger) and
//       the anchor zone rests DangerZoneMs;
//   (5) soak-s64-full-r1 (1.9 h, 128 squad deaths): benches cleared at every stint end and the benched Horde
//       leader re-anchored the next stint beside itself, so two members were re-benched every ~30 s (95 bench
//       rows) and kept dying where they stood (Hillsbrad bears). Now: a bench lasts BenchMs (not re-evaluated,
//       outlives stints); a newly benched member takes one death-loop escape trip to a level-fitting hub
//       (ZoneProgression, needs AutoWow.DeathLoop.EscapeViaZoneProgression); the leader is the lowest
//       unbenched member; more than half the online members benched ends the stint and holds the search
//       (gather / release cause benched).
[[nodiscard]] inline bool LevelWindowOn(Params const& p) { return p.levelWindow != 0; }

// Bracket margin of an anchor zone above the squad average level.
[[nodiscard]] inline std::uint32_t ZoneMargin(Params const& p) { return p.levelWindow ? p.levelWindow : p.zoneLevelMargin; }

// The zone's bracket starts more than margin above level (zoneLow 0 = unknown: never).
[[nodiscard]] inline bool ZoneTooHigh(std::uint32_t zoneLow, std::uint32_t level, std::uint32_t margin)
{
    return zoneLow > level + margin;
}

// (2): the member sits the stint out; "route" beats "deaths"; "" = it takes part.
[[nodiscard]] inline char const* BenchCause(Params const& p, bool routeCrossesHighZone, std::uint32_t recentDeaths)
{
    if (!LevelWindowOn(p))
        return "";
    if (routeCrossesHighZone)
        return "route";
    return p.deathCluster && recentDeaths >= p.deathCluster ? "deaths" : "";
}

// (4): a member at (map, x, y) with recentDeaths makes the running stint's anchor a death cluster.
[[nodiscard]] inline bool AnchorDeathCluster(Params const& p, TeamState const& s, std::uint32_t map, std::int32_t x,
                                             std::int32_t y, std::uint32_t recentDeaths)
{
    return LevelWindowOn(p) && p.deathCluster && recentDeaths >= p.deathCluster && s.phase == Phase::Stint &&
           map == s.map && AutoWowContracts::Within(x, y, s.x, s.y, p.leashYards);
}

inline void CoolZone(TeamState& s, Params const& p, std::uint32_t zone, std::uint64_t nowMs)
{
    s.cooldownZone = zone;
    s.zoneCooldownUntilMs = nowMs + p.dangerZoneMs;
}

[[nodiscard]] inline bool ZoneCooling(TeamState const& s, std::uint32_t zone, std::uint64_t nowMs)
{
    return zone && zone == s.cooldownZone && nowMs < s.zoneCooldownUntilMs;
}

// (5): a bench started at nowMs holds until the returned time.
[[nodiscard]] inline std::uint64_t BenchUntil(Params const& p, std::uint64_t nowMs) { return nowMs + p.benchMs; }
[[nodiscard]] inline bool BenchActive(std::uint64_t untilMs, std::uint64_t nowMs) { return nowMs < untilMs; }

// (5): more than half of the online members benched: no stint.
[[nodiscard]] inline bool TooManyBenched(Params const& p, std::size_t benched, std::size_t members)
{
    return LevelWindowOn(p) && benched * 2 > members;
}

// (2) + (3): the stint as the member's target filter sees it (phase None = no filter). Window off: s unchanged.
[[nodiscard]] inline TeamState FilterView(Params const& p, TeamState s, std::uint32_t map, bool benched)
{
    if (LevelWindowOn(p) && (benched || map != s.map))
        s.phase = Phase::None;
    return s;
}

// Tier hold: a member does not graduate to the next zone while its squad holds, unless where it stands is
// dangerous (a death-loop area, or a zone bracketed above its level).
[[nodiscard]] inline bool Holds(TeamState const& s, bool dangerousHere) { return s.holding && !dangerousHere; }

// No straggler: every alive member on the leader's map lies within TogetherYards of the leader or nearer the anchor
// (ax, ay) than the leader (members walk ahead to the anchor; only those behind are waited for). Yards.
[[nodiscard]] inline bool Together(Params const& p, std::int32_t lx, std::int32_t ly, std::int32_t ax, std::int32_t ay,
                                   std::vector<std::array<std::int32_t, 2>> const& members)
{
    std::int64_t const lead = AutoWowContracts::Dist2(lx, ly, ax, ay);
    for (auto const& m : members)
        if (!AutoWowContracts::Within(m[0], m[1], lx, ly, p.togetherYards) &&
            AutoWowContracts::Dist2(m[0], m[1], ax, ay) > lead)
            return false;
    return true;
}

// ---- member step (map thread) ---------------------------------------------------------------------------
enum class Step : std::uint8_t
{
    Idle = 0,    // no stint for this member (none running, or it stands on another map): its own loop runs
    Wait = 1,    // the leader waits for stragglers before walking on
    Travel = 2,  // walk to the anchor (or back inside the leash)
    Hunt = 3     // at the anchor: hunt / gather within the leash
};

// Arrive within ArriveYards of the anchor; leave the hunt beyond LeashYards (the contracts idiom).
[[nodiscard]] inline Step MemberStep(Params const& p, TeamState const& s, bool isLeader, bool hunting, std::uint32_t map,
                                     std::int32_t bx, std::int32_t by)
{
    if (s.phase != Phase::Stint || map != s.map)
        return Step::Idle;
    if (AutoWowContracts::Within(bx, by, s.x, s.y, hunting ? p.leashYards : AutoWowContracts::kArriveYards))
        return Step::Hunt;
    return isLeader && !s.together ? Step::Wait : Step::Travel;
}

// Grind filter: during a stint a member's proactive target lies within LeashYards of the anchor and, for a creature
// source (cloth / leather), is one of the source entries. A node stint takes any target within the leash.
[[nodiscard]] inline bool HuntTarget(Params const& p, TeamState const& s, std::uint32_t entry, std::uint32_t map,
                                     std::int32_t x, std::int32_t y)
{
    if (s.phase != Phase::Stint || map != s.map || !AutoWowContracts::Within(x, y, s.x, s.y, p.leashYards))
        return false;
    if (NodeKind(s.kind))
        return true;
    for (std::size_t k = 0; k < s.entryCount; ++k)
        if (s.entries[k] == entry)
            return true;
    return false;
}

// Ledger `squad` trailing fields (append only): stint id, kind, item, count, target, gathered, anchor, duration,
// and the cause when given.
inline std::string LedgerFields(TeamState const& s, Kind kind, std::uint32_t item, std::uint32_t count,
                                std::uint64_t nowMs, char const* cause = "")
{
    return ",\"sid\":" + std::to_string(s.id) + ",\"kind\":\"" + KindName(kind) + "\",\"item\":" +
           std::to_string(item) + ",\"count\":" + std::to_string(count) + ",\"target\":" + std::to_string(s.target) +
           ",\"gathered\":" + std::to_string(s.gathered) + ",\"amap\":" + std::to_string(s.map) +
           ",\"ax\":" + std::to_string(s.x) + ",\"ay\":" + std::to_string(s.y) +
           ",\"anchor\":" + std::to_string(s.anchorSpawn) +
           ",\"dur_ms\":" + std::to_string(s.startMs && nowMs >= s.startMs ? nowMs - s.startMs : 0) +
           (*cause ? ",\"cause\":\"" + std::string(cause) + "\"" : std::string());
}

// ---- runtime (NewRpgSquad.cpp) --------------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }

// Reads AutoWow.Squad.* and, with the flag on, builds the source index (world init, after spawns).
void LoadConfig();
// World thread: the squad party, demand, stint start / end, together flag.
void WorldUpdate(std::uint32_t diff);
bool IsMember(std::uint32_t guid);
// The team's configured roster (guid ascending; empty with the flag off). World thread (read-only after LoadConfig).
std::vector<std::uint32_t> Roster(bool alliance);
// The member's team state (phase None for a non-member).
TeamState SnapshotOf(std::uint32_t guid);
// The stint as the member's grind target filter sees it on `map` (FilterView; LevelWindow off: SnapshotOf).
TeamState SnapshotFor(std::uint32_t guid, std::uint32_t map);
// Zone progression (map thread): true = the member holds its tier (a `hold` row at most every holdLogMs).
bool HoldsTier(Player* bot);
// Supply donation (world thread): a squad member's materials reached a house rep (`deliver` row).
void NoteDelivered(Player* bot, std::uint32_t item, std::uint32_t count, std::uint32_t rep);
}  // namespace AutoWowSquad

#endif  // AUTOWOW_SQUAD_POLICY_H
