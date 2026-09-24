/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ZONE_PROGRESSION_POLICY_H
#define AUTOWOW_ZONE_PROGRESSION_POLICY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Zone progression for independent AutoWoW bots (AutoWow.ZoneProgression.Enable, default 0).
// Random bots graduate by teleport (RandomPlayerbotMgr::RandomTeleportForLevel); independent bots had
// no graduation and stalled in their L1-10 parent zone. Here a bot graduates when its level reaches
// the zone bracket max minus a margin, or when its quest log has had nothing actionable for N
// consecutive checks (and its level fits a route), then travels to the route's hub inn by NORMAL means
// only: a known flight path (source node learned at the flight master), else a walk (New RPG GoGrind /
// MoveFarTo, no teleport for AutoWow travel bots). At the inn it binds its hearthstone and then learns
// the zone's flight master. Each finished or abandoned move emits ledger `zone_move`.
//
// Value-only below the runtime section: integer yards / game-time ms, no floats in decisions, no RNG,
// stable route order (table order; deterministic spread by guid modulo candidate count).
namespace AutoWowZoneProgression
{
inline constexpr std::uint8_t kStateVersion = 1;

// Route team: 0 any, 1 alliance, 2 horde (core TeamId + 1).
struct Route
{
    std::uint32_t team = 0;
    std::uint32_t from = 0;      // current zone id
    std::uint32_t to = 0;        // destination zone id
    std::uint32_t minLevel = 0;  // inclusive level band the route serves
    std::uint32_t maxLevel = 0;
    std::uint32_t map = 0;       // hub (inn) position
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t inn = 0;       // innkeeper creature entry at the hub (hearth bind)
    bool crossing = false;       // sea/continent between: walking cannot reach, needs a flight path
};

// Built-in table (world DB innkeeper spawns). Known gaps (no route): Bloodmyst -> Darkshore (Exodar
// boat), Ghostlands -> Eastern Kingdoms (Silvermoon orb / Undercity), zeppelins; Teldrassil ->
// Darkshore works only by flight (Rut'theran -> Auberdine), the Auberdine boat is not ridden.
inline std::vector<Route> DefaultRoutes()
{
    return {
        // Alliance 1-10 -> 10-20
        {1, 12, 40, 9, 14, 0, -10653, 1166, 34, 8931, false},    // Elwynn -> Westfall (Sentinel Hill)
        {1, 12, 44, 15, 20, 0, -9224, -2158, 64, 6727, false},   // Elwynn -> Redridge (Lakeshire)
        {1, 1, 38, 9, 18, 0, -5378, -2974, 323, 6734, false},    // Dun Morogh -> Loch Modan (Thelsamar)
        {1, 141, 148, 9, 18, 1, 6406, 515, 8, 6737, true},       // Teldrassil -> Darkshore (Auberdine)
        {1, 3524, 3525, 9, 18, 530, -2059, -11897, 45, 17553, false},  // Azuremyst -> Bloodmyst (Blood Watch)
        // Alliance 10-20 -> 20-30
        {1, 40, 10, 18, 30, 0, -10516, -1162, 28, 6790, false},  // Westfall -> Duskwood (Darkshire)
        {1, 44, 10, 18, 30, 0, -10516, -1162, 28, 6790, false},  // Redridge -> Duskwood
        {1, 38, 11, 18, 30, 0, -3828, -832, 10, 1464, false},    // Loch Modan -> Wetlands (Menethil)
        {1, 148, 331, 18, 30, 1, 2781, -433, 116, 6738, false},  // Darkshore -> Ashenvale (Astranaar)
        // Horde 1-10 -> 10-20
        {2, 14, 17, 9, 20, 1, -408, -2646, 96, 3934, false},     // Durotar -> Barrens (Crossroads)
        {2, 215, 17, 9, 20, 1, -408, -2646, 96, 3934, false},    // Mulgore -> Barrens
        {2, 85, 130, 9, 18, 0, 510, 1636, 126, 6739, false},     // Tirisfal -> Silverpine (Sepulcher)
        {2, 3430, 3433, 9, 20, 530, 7553, -6898, 96, 16542, false},  // Eversong -> Ghostlands (Tranquillien)
        // Horde 10-20 -> 20-30
        {2, 17, 406, 18, 28, 1, 893, 927, 106, 7731, false},     // Barrens -> Stonetalon (Sun Rock)
        {2, 130, 267, 18, 30, 0, -6, -943, 57, 2388, false},     // Silverpine -> Hillsbrad (Tarren Mill)
    };
}

// Config override AutoWow.ZoneProgression.Routes: ';'-separated routes, each
// "team,from,to,minLevel,maxLevel,map,x,y,z,inn,crossing" (integers; crossing 0|1). Replaces the
// built-in table. False (out untouched) on any malformed entry.
[[nodiscard]] inline bool ParseRoutes(std::string_view text, std::vector<Route>& out)
{
    std::vector<Route> parsed;
    while (!text.empty())
    {
        std::size_t const end = text.find(';');
        std::string_view entry = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!entry.empty() && entry.front() == ' ')
            entry.remove_prefix(1);
        if (entry.empty())
            continue;
        std::int64_t v[11] = {};
        std::size_t n = 0;
        while (n < 11)
        {
            std::size_t const comma = entry.find(',');
            std::string_view tok = entry.substr(0, comma);
            while (!tok.empty() && tok.front() == ' ')
                tok.remove_prefix(1);
            while (!tok.empty() && tok.back() == ' ')
                tok.remove_suffix(1);
            bool neg = !tok.empty() && tok.front() == '-';
            if (neg)
                tok.remove_prefix(1);
            if (tok.empty() || tok.size() > 9)
                return false;
            std::int64_t value = 0;
            for (char ch : tok)
            {
                if (ch < '0' || ch > '9')
                    return false;
                value = value * 10 + (ch - '0');
            }
            v[n++] = neg ? -value : value;
            if (comma == std::string_view::npos)
            {
                entry = {};
                break;
            }
            entry = entry.substr(comma + 1);
        }
        if (n != 11 || !entry.empty())
            return false;
        for (std::size_t k : {0, 1, 2, 3, 4, 5, 9, 10})
            if (v[k] < 0)
                return false;
        if (v[0] > 2 || v[10] > 1 || v[3] > v[4])
            return false;
        parsed.push_back(Route{static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[1]),
                               static_cast<std::uint32_t>(v[2]), static_cast<std::uint32_t>(v[3]),
                               static_cast<std::uint32_t>(v[4]), static_cast<std::uint32_t>(v[5]),
                               static_cast<std::int32_t>(v[6]), static_cast<std::int32_t>(v[7]),
                               static_cast<std::int32_t>(v[8]), static_cast<std::uint32_t>(v[9]), v[10] == 1});
    }
    out = std::move(parsed);
    return true;
}

// Destination: the routes leaving `fromZone` for the bot's team whose band holds `level`, in table
// order; the bot takes candidate (guid % count) so a cohort spreads deterministically. nullptr = none.
[[nodiscard]] inline Route const* PickRoute(std::vector<Route> const& routes, std::uint32_t team,
                                            std::uint32_t fromZone, std::uint32_t level, std::uint32_t guid)
{
    std::vector<Route const*> fit;
    for (Route const& r : routes)
        if (r.from == fromZone && (r.team == 0 || r.team == team) && level >= r.minLevel && level <= r.maxLevel)
            fit.push_back(&r);
    return fit.empty() ? nullptr : fit[guid % fit.size()];
}

// Death-loop escape hub (AutoWow.DeathLoop.EscapeViaZoneProgression): among the routes for the bot's
// team whose band holds `level`, whatever zone they leave, the nearest destination hub other than the
// bot's zone. Hubs on the bot's map first (by squared distance), then other maps; ties keep table
// order. soak-s13-full-r1: a L13 in Duskwood (no route leaves zone 10) -> Sentinel Hill. nullptr = none.
[[nodiscard]] inline Route const* PickEscapeRoute(std::vector<Route> const& routes, std::uint32_t team,
                                                  std::uint32_t level, std::uint32_t zone, std::uint32_t map,
                                                  std::int32_t x, std::int32_t y)
{
    Route const* best = nullptr;
    bool bestSameMap = false;
    std::int64_t bestDist2 = 0;
    for (Route const& r : routes)
    {
        if (r.to == zone || (r.team != 0 && r.team != team) || level < r.minLevel || level > r.maxLevel)
            continue;
        bool const sameMap = r.map == map;
        std::int64_t const dx = std::int64_t(r.x) - x;
        std::int64_t const dy = std::int64_t(r.y) - y;
        std::int64_t const dist2 = sameMap ? dx * dx + dy * dy : 0;
        if (!best || (sameMap && !bestSameMap) || (sameMap == bestSameMap && dist2 < bestDist2))
        {
            best = &r;
            bestSameMap = sameMap;
            bestDist2 = dist2;
        }
    }
    return best;
}

// AutoWow.DeathLoop.V2: the same candidates as PickEscapeRoute (team, band holds `level`, not the bot's
// zone) but the lowest level band first (minLevel, then maxLevel) - back toward easier zones rather than
// the nearest same-level hub - then hubs on the bot's map by squared distance, then table order.
[[nodiscard]] inline Route const* PickLowEscapeRoute(std::vector<Route> const& routes, std::uint32_t team,
                                                     std::uint32_t level, std::uint32_t zone, std::uint32_t map,
                                                     std::int32_t x, std::int32_t y)
{
    Route const* best = nullptr;
    bool bestSameMap = false;
    std::int64_t bestDist2 = 0;
    for (Route const& r : routes)
    {
        if (r.to == zone || (r.team != 0 && r.team != team) || level < r.minLevel || level > r.maxLevel)
            continue;
        bool const sameMap = r.map == map;
        std::int64_t const dx = std::int64_t(r.x) - x;
        std::int64_t const dy = std::int64_t(r.y) - y;
        std::int64_t const dist2 = sameMap ? dx * dx + dy * dy : 0;
        bool take = !best;
        if (!take && r.minLevel != best->minLevel)
            take = r.minLevel < best->minLevel;
        else if (!take && r.maxLevel != best->maxLevel)
            take = r.maxLevel < best->maxLevel;
        else if (!take && sameMap != bestSameMap)
            take = sameMap;
        else if (!take)
            take = dist2 < bestDist2;
        if (take)
        {
            best = &r;
            bestSameMap = sameMap;
            bestDist2 = dist2;
        }
    }
    return best;
}

// Wire-stable ledger reason names; append only.
enum class Trigger : std::uint8_t
{
    None = 0,
    Level = 1,     // level >= zone bracket max - margin
    NoQuests = 2,  // nothing actionable in the quest log for N consecutive checks
    DeathLoop = 3  // AutoWow.DeathLoop.EscapeViaZoneProgression: escape from an over-level zone
};

inline constexpr char const* TriggerName(Trigger t)
{
    switch (t)
    {
        case Trigger::None: return "none";
        case Trigger::Level: return "level";
        case Trigger::NoQuests: return "no_quests";
        case Trigger::DeathLoop: return "death_loop";
    }
    return "none";
}

struct Params
{
    std::uint32_t levelMargin = 2;           // AutoWow.ZoneProgression.LevelMargin
    std::uint32_t stallChecks = 10;          // AutoWow.ZoneProgression.StallChecks (0 = stall rule off)
    std::uint32_t checkIntervalMs = 60000;   // AutoWow.ZoneProgression.CheckIntervalMs
    std::uint32_t travelTimeoutMs = 3600000; // AutoWow.ZoneProgression.TravelTimeoutMs
    std::uint32_t maxReissues = 8;           // AutoWow.ZoneProgression.MaxReissues
    std::uint32_t cooldownMs = 1800000;      // AutoWow.ZoneProgression.GiveUpCooldownMs
    std::uint32_t portalAfterMs = 1200000;   // AutoWow.ZoneProgression.PortalAfterMs (0 = time rule off)
};

// zoneMax = high end of the current zone's bracket (0 = unbracketed: level rule off). hasRoute = a
// route fits the bot's team/zone/level (the level band of the route is the no-quests level gate).
[[nodiscard]] inline Trigger Evaluate(Params const& p, std::uint32_t level, std::uint32_t zoneMax,
                                      std::uint32_t stallCount, bool hasRoute)
{
    if (!hasRoute)
        return Trigger::None;
    if (zoneMax && level + p.levelMargin >= zoneMax)
        return Trigger::Level;
    if (p.stallChecks && stallCount >= p.stallChecks)
        return Trigger::NoQuests;
    return Trigger::None;
}

// Consecutive-check stall counter: a check with an actionable quest resets it.
[[nodiscard]] inline std::uint32_t NextStall(std::uint32_t stallCount, bool actionableQuest)
{
    return actionableQuest ? 0 : stallCount + 1;
}

enum class Mode : std::uint8_t
{
    Unreachable = 0,
    Walk = 1,
    Flight = 2,
    Chain = 3,  // AutoWow.Transports: walk -> travel object / transport -> walk (TransportCrossingPolicy.h)
    Portal = 4  // AutoWow.Transports (mode auto/portal): walk leg spent -> portal to the hub (owner ruling)
};

inline constexpr char const* ModeName(Mode m)
{
    switch (m)
    {
        case Mode::Unreachable: return "none";
        case Mode::Walk: return "walk";
        case Mode::Flight: return "flight";
        case Mode::Chain: return "chain";
        case Mode::Portal: return "portal";
    }
    return "none";
}

// A usable flight path (reachable flight master, known destination node in the target zone, taxi
// path) wins; else a walk when the hub is on the bot's map and no sea lies between; else unreachable.
[[nodiscard]] inline Mode SelectMode(bool flightPath, bool sameMap, bool crossing)
{
    if (flightPath)
        return Mode::Flight;
    if (sameMap && !crossing)
        return Mode::Walk;
    return Mode::Unreachable;
}

// Road waypoints per route (world DB playerbots_travelnode positions, on the route's map), in travel
// order. A straight hub walk cut through the wrong zone (soak-s11: Elwynn -> Westfall bot died in Duskwood
// Raven Hill) or into cave/tunnel dead ends; a road keeps each leg short and on the roads.
struct RoadPoint
{
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

inline std::vector<RoadPoint> DefaultRoads()
{
    return {
        // Elwynn -> Westfall: Goldshire, Westbrook Garrison bridge, Jansen Stead.
        {12, 40, -9473, 56, 61}, {12, 40, -9634, 674, 53}, {12, 40, -9789, 986, 29},
        // Dun Morogh -> Loch Modan: Coldridge Valley/Pass, Kharanos, the east road, South Gate.
        {1, 38, -6342, 482, 382}, {1, 38, -6123, 79, 417}, {1, 38, -5584, -486, 401}, {1, 38, -5351, -1041, 395},
        {1, 38, -5525, -1354, 399}, {1, 38, -5721, -1891, 401}, {1, 38, -5620, -2205, 422},
        {1, 38, -5537, -2392, 401},
        // Durotar -> Barrens: Sen'jin, Razor Hill, Razormane Grounds, Southfury bridge, Far Watch Post.
        {14, 17, -814, -4921, 19}, {14, 17, 313, -4759, 10}, {14, 17, 238, -4318, 38}, {14, 17, 421, -3843, 24},
        {14, 17, 261, -3677, 48},
        // Mulgore -> Barrens: Bloodhoof, Ravaged Caravan, Red Rocks.
        {215, 17, -2307, -389, -9}, {215, 17, -1911, -727, 2}, {215, 17, -1040, -1078, 28},
        // Tirisfal -> Silverpine: Brill, Cold Hearth Manor, Malden's Orchard, Valgan's Field.
        {85, 130, 2268, 299, 34}, {85, 130, 2120, 621, 35}, {85, 130, 1421, 1027, 52}, {85, 130, 919, 1160, 47},
        // Eversong -> Ghostlands: Falconwing Square, East Sanctum, Elrendar Crossing.
        {3430, 3433, 9513, -6838, 17}, {3430, 3433, 8693, -7059, 48}, {3430, 3433, 7983, -6930, 60},
        // Azuremyst -> Bloodmyst: Azure Watch, Moongraze Woods, Fairbridge Strand, Kessel's Crossing.
        {3524, 3525, -4173, -12499, 45}, {3524, 3525, -3801, -12706, 11}, {3524, 3525, -2921, -12446, 5},
        {3524, 3525, -2694, -12138, 14},
    };
}

[[nodiscard]] inline std::vector<RoadPoint> RoadFor(std::vector<RoadPoint> const& roads, std::uint32_t from,
                                                    std::uint32_t to)
{
    std::vector<RoadPoint> out;
    for (RoadPoint const& p : roads)
        if (p.from == from && p.to == to)
            out.push_back(p);
    return out;
}

inline std::int64_t RoadDist2(RoadPoint const& p, std::int32_t x, std::int32_t y)
{
    std::int64_t const dx = std::int64_t(p.x) - x;
    std::int64_t const dy = std::int64_t(p.y) - y;
    return dx * dx + dy * dy;
}

// Join: the nearest road point (lowest index on ties). Empty road -> 0 (== size: walk to the hub).
[[nodiscard]] inline std::uint32_t JoinRoad(std::vector<RoadPoint> const& road, std::int32_t x, std::int32_t y)
{
    std::uint32_t best = 0;
    for (std::uint32_t k = 1; k < road.size(); ++k)
        if (RoadDist2(road[k], x, y) < RoadDist2(road[best], x, y))
            best = k;
    return best;
}

// Skips every road point already within reachYards; returns the index to walk to (== size: the hub).
[[nodiscard]] inline std::uint32_t AdvanceRoad(std::vector<RoadPoint> const& road, std::uint32_t wp, std::int32_t x,
                                               std::int32_t y, std::uint32_t reachYards)
{
    std::int64_t const reach2 = std::int64_t(reachYards) * reachYards;
    while (wp < road.size() && RoadDist2(road[wp], x, y) <= reach2)
        ++wp;
    return wp;
}

// Portal fallback for a walk leg (AutoWow.Transports, mode auto/portal; owner ruling 2026-09-24): the
// walk used up its stuck budget, or PortalAfterMs passed without the bot reaching the destination zone.
[[nodiscard]] inline bool PortalFallback(bool allowed, Mode mode, bool inDestZone, std::uint32_t reissues,
                                         std::uint32_t maxReissues, std::uint64_t travelMs, std::uint32_t portalAfterMs)
{
    if (!allowed || inDestZone || (mode != Mode::Walk && mode != Mode::Unreachable))
        return false;
    return reissues >= maxReissues || (portalAfterMs && travelMs >= portalAfterMs);
}

enum class Phase : std::uint8_t
{
    None = 0,     // evaluating triggers
    Travel = 1,   // heading to the route hub (inn)
    LearnFp = 2   // arrived and bound; walking to the destination zone's flight master
};

// Per-bot state (fixed size, value-only).
struct BotState
{
    std::uint8_t version = kStateVersion;
    Phase phase = Phase::None;
    std::uint64_t nextCheckMs = 0;
    std::uint32_t stall = 0;
    Route route;
    Trigger trigger = Trigger::None;
    Mode mode = Mode::Unreachable;
    std::uint32_t fromZone = 0;
    std::uint64_t startMs = 0;
    std::uint32_t reissues = 0;
    std::uint64_t cooldownUntilMs = 0;
    bool roadJoined = false;  // road index chosen for this travel
    std::uint32_t wp = 0;     // next road point (== road size: the hub)
};

// Starts (or restarts) a death_loop trip from `zone` to `hub`. The route leaves the bot's actual zone
// (no road table applies: straight to the hub). portal = take the portal leg at once (the Travel phase
// portals while mode is Portal and reissues <= MaxReissues). Check timers are kept.
inline void BeginEscape(BotState& s, Route const& hub, std::uint32_t zone, std::uint64_t nowMs, bool portal)
{
    s.phase = Phase::Travel;
    s.route = hub;
    s.route.from = zone;
    s.trigger = Trigger::DeathLoop;
    s.fromZone = zone;
    s.startMs = nowMs;
    s.reissues = 0;
    s.stall = 0;
    s.mode = portal ? Mode::Portal : Mode::Unreachable;
    s.roadJoined = false;
    s.wp = 0;
}

// True when the travel leg is spent: past the timeout or out of reissues.
[[nodiscard]] inline bool TravelExhausted(Params const& p, BotState const& s, std::uint64_t nowMs)
{
    return (nowMs >= s.startMs && nowMs - s.startMs > p.travelTimeoutMs) || s.reissues > p.maxReissues;
}

// One tick of a committed walk leg. Only a reported stuck (the mover's no-progress window) consumes a
// reissue and re-opens the flight/chain/walk choice; an ordinary tick costs nothing. soak-s10-zoneprog-r1
// charged every tick whose status was not the GO_GRIND leg, and GO_GRIND dropped inter-zone hubs at once,
// so all 9 trips hit maxReissues in 2-37 s.
inline void NoteWalkTick(BotState& s, bool stuck)
{
    if (!stuck)
        return;
    ++s.reissues;
    s.mode = Mode::Unreachable;
}

// Trailing fields of the ledger `zone_move` line (AutoWowQuestLedger.h documents them).
inline std::string LedgerFields(std::uint32_t fromZone, std::uint32_t toZone, std::uint64_t travelMs, bool arrived,
                                Mode mode)
{
    std::string out = ",\"from\":" + std::to_string(fromZone) + ",\"to\":" + std::to_string(toZone) +
                      ",\"travel_ms\":" + std::to_string(travelMs) + ",\"arrived\":" + (arrived ? "true" : "false") +
                      ",\"mode\":\"" + ModeName(mode) + "\"";
    return out;
}

// ---- runtime (NewRpgZoneProgression.cpp) --------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
inline std::vector<Route> gRoutes;
}
inline bool Enabled() { return detail::gEnabled; }

void LoadConfig();
// A graduation (travel or flight-path learning) is under way for this bot. Town runs wait for it.
bool Active(std::uint32_t guid);
}  // namespace AutoWowZoneProgression

#endif  // AUTOWOW_ZONE_PROGRESSION_POLICY_H
