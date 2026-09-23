/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TRANSPORT_CROSSING_POLICY_H
#define AUTOWOW_TRANSPORT_CROSSING_POLICY_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ZoneProgressionPolicy.h"

// Real crossings for zone progression (AutoWow.Transports.Enable, default 0; needs
// AutoWow.ZoneProgression.Enable). A route whose hub lies across water / a continent and has no known
// flight path runs a CHAIN: walk -> crossing -> walk -> ... -> walk to the hub. A crossing is either a
// travel object (an areatrigger the bot walks into, or a teleport GameObject the bot uses: Darnassus
// -> Rut'theran, the Silvermoon / Undercity Orbs of Translocation) or a real MO_TRANSPORT (boat,
// zeppelin): the bot waits on the dock, walks onto the deck while the ship is docked (the core makes it
// a passenger), stays aboard, and steps off onto the destination dock when the ship docks there.
// Travel objects and rides move the bot by the game's own trigger / spell / transport. The one AutoWow
// relocation is the flagged dock-to-dock portal fallback for transports (TransportMode, leg "portal").
//
// Value-only: integer yards, game-time ms, table order, no RNG. Positions come from the world DB /
// DBC (provenance per row in DefaultCrossings).
namespace AutoWowTransports
{
inline constexpr std::uint8_t kStateVersion = 1;

// How a crossing is taken. Wire-stable (config), append only.
enum class Via : std::uint8_t
{
    AreaTrigger = 0,  // walk into areatrigger `object` (areatrigger_teleport row)
    GameObject = 1,   // use teleport GameObject `object` (goober / spellcaster)
    Transport = 2     // ride MO_TRANSPORT GameObject entry `object`
};

// One crossing of a route chain. Rows of the same (team, from, to) run in `seq` order.
struct Crossing
{
    std::uint32_t team = 0;  // 0 any, 1 alliance, 2 horde (as ZoneProgression Route)
    std::uint32_t from = 0;  // route key: from zone id
    std::uint32_t to = 0;    // route key: to zone id
    std::uint32_t seq = 0;
    Via via = Via::AreaTrigger;
    std::uint32_t object = 0;
    std::uint32_t map = 0;  // approach: trigger / object spot, or the dock point to wait on (land)
    std::int32_t x = 0, y = 0, z = 0;
    std::int32_t stopX = 0, stopY = 0;  // transport: its docked stop on `map` (TaxiPathNode delay node)
    std::uint32_t exitMap = 0;          // arrival: teleport landing, or the destination dock (land)
    std::int32_t exitX = 0, exitY = 0, exitZ = 0;
    std::int32_t exitStopX = 0, exitStopY = 0;  // transport: its docked stop on exitMap
};

// Built-in crossings. Provenance: areatrigger 527 = AreaTrigger.dbc pos + areatrigger_teleport target;
// orb 184502 = gameobject spawn, landing = the Undercity orb 184503 spawn (spell 35376 lands beside it);
// transport stops = TaxiPathNode.dbc delay nodes of the transport's taxi path; dock points = a creature /
// gameobject spawn standing on that dock (zeppelin masters, dock guards, pier banner).
inline std::vector<Crossing> DefaultCrossings()
{
    return {
        // Teldrassil -> Darkshore: Darnassus portal to Rut'theran, then the Moonspray (path 293).
        {1, 141, 148, 0, Via::AreaTrigger, 527, 1, 9947, 2630, 1318, 0, 0, 1, 8786, 967, 30, 0, 0},
        {1, 141, 148, 1, Via::Transport, 176244, 1, 8558, 1014, 6, 8534, 1018, 1, 6577, 775, 6, 6595, 770},
        // Bloodmyst -> Darkshore: Elune's Blessing from Valaar's Berth (path 503, map 530 -> 1).
        {1, 3525, 148, 0, Via::Transport, 181646, 530, -4270, -11333, 6, -4265, -11317, 1, 6551, 925, 6, 6550, 938},
        // Wetlands -> Dustwallow: the Lady Mehley, Menethil -> Theramore (path 292, map 0 -> 1).
        {1, 11, 15, 0, Via::Transport, 176231, 0, -3904, -605, 5, -3905, -586, 1, -3999, -4736, 5, -4016, -4741},
        // Ghostlands -> Hillsbrad: Silvermoon Orb of Translocation to Undercity, then walk.
        {2, 3433, 267, 0, Via::GameObject, 184502, 530, 10032, -7000, 61, 0, 0, 0, 1806, 349, 71, 0, 0},
        // Barrens -> Stranglethorn: the Iron Eagle, Orgrimmar tower -> Grom'gol (path 285, map 1 -> 0).
        {2, 17, 33, 0, Via::Transport, 175080, 1, 1354, -4643, 54, 1361, -4631, 0, -12441, 215, 31, -12464, 232},
    };
}

// Routes that exist only because a crossing serves them (appended to the zone-progression table when
// the flag is on; Teldrassil -> Darkshore is already in the base table).
inline std::vector<AutoWowZoneProgression::Route> ExtraRoutes()
{
    return {
        {1, 3525, 148, 15, 22, 1, 6406, 515, 8, 6737, true},      // Bloodmyst -> Darkshore (Auberdine)
        {1, 11, 15, 30, 40, 1, -3616, -4471, 14, 6272, true},     // Wetlands -> Dustwallow (Theramore)
        {2, 3433, 267, 18, 30, 0, -6, -943, 57, 2388, true},      // Ghostlands -> Hillsbrad (Tarren Mill)
        {2, 17, 33, 28, 40, 0, -12434, 212, 2, 5814, true},       // Barrens -> Stranglethorn (Grom'gol)
    };
}

// Config override AutoWow.Transports.Crossings: ';'-separated rows of 18 integers
// "team,from,to,seq,via,object,map,x,y,z,stopX,stopY,exitMap,exitX,exitY,exitZ,exitStopX,exitStopY".
// Replaces the built-in table. False (out untouched) on any malformed row.
[[nodiscard]] inline bool ParseCrossings(std::string_view text, std::vector<Crossing>& out)
{
    std::vector<Crossing> parsed;
    while (!text.empty())
    {
        std::size_t const end = text.find(';');
        std::string_view entry = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!entry.empty() && entry.front() == ' ')
            entry.remove_prefix(1);
        if (entry.empty())
            continue;
        std::int64_t v[18] = {};
        std::size_t n = 0;
        while (n < 18)
        {
            std::size_t const comma = entry.find(',');
            std::string_view tok = entry.substr(0, comma);
            while (!tok.empty() && tok.front() == ' ')
                tok.remove_prefix(1);
            while (!tok.empty() && tok.back() == ' ')
                tok.remove_suffix(1);
            bool const neg = !tok.empty() && tok.front() == '-';
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
        if (n != 18 || !entry.empty())
            return false;
        for (std::size_t k : {0, 1, 2, 3, 4, 5, 6, 12})
            if (v[k] < 0)
                return false;
        if (v[0] > 2 || v[4] > 2 || v[5] == 0)
            return false;
        auto const u = [&](std::size_t k) { return static_cast<std::uint32_t>(v[k]); };
        auto const i = [&](std::size_t k) { return static_cast<std::int32_t>(v[k]); };
        parsed.push_back(Crossing{u(0), u(1), u(2), u(3), static_cast<Via>(v[4]), u(5), u(6), i(7), i(8), i(9),
                                  i(10), i(11), u(12), i(13), i(14), i(15), i(16), i(17)});
    }
    out = std::move(parsed);
    return true;
}

// The chain serving a route, in seq order (ties: table order). Empty = no crossing chain.
[[nodiscard]] inline std::vector<Crossing> ChainFor(std::vector<Crossing> const& table, std::uint32_t team,
                                                    std::uint32_t fromZone, std::uint32_t toZone)
{
    std::vector<Crossing> chain;
    for (Crossing const& c : table)
        if (c.from == fromZone && c.to == toZone && (c.team == 0 || c.team == team))
            chain.push_back(c);
    std::stable_sort(chain.begin(), chain.end(), [](Crossing const& a, Crossing const& b) { return a.seq < b.seq; });
    return chain;
}

// Integer squared 2D distance (yards).
[[nodiscard]] inline std::int64_t Dist2(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
{
    std::int64_t const dx = std::int64_t(ax) - bx;
    std::int64_t const dy = std::int64_t(ay) - by;
    return dx * dx + dy * dy;
}

// Where a bot joins its chain: the LAST crossing whose approach is on the bot's map within `nearYards`
// (a Teldrassil bot already down in Rut'theran skips the Darnassus portal); else the first crossing.
[[nodiscard]] inline std::uint32_t StartLeg(std::vector<Crossing> const& chain, std::uint32_t map, std::int32_t x,
                                            std::int32_t y, std::int32_t nearYards)
{
    std::uint32_t start = 0;
    std::int64_t const r2 = std::int64_t(nearYards) * nearYards;
    for (std::uint32_t k = 0; k < chain.size(); ++k)
        if (chain[k].map == map && Dist2(chain[k].x, chain[k].y, x, y) <= r2)
            start = k;
    return start;
}

// Mode choice with crossings: a known flight path still wins; a same-map walk without a crossing next;
// else a chain when one serves the route.
[[nodiscard]] inline AutoWowZoneProgression::Mode SelectMode(bool flightPath, bool sameMap, bool crossing,
                                                             bool chain)
{
    AutoWowZoneProgression::Mode const base = AutoWowZoneProgression::SelectMode(flightPath, sameMap, crossing);
    if (base == AutoWowZoneProgression::Mode::Unreachable && chain)
        return AutoWowZoneProgression::Mode::Chain;
    return base;
}

// ---- per-crossing step machine ---------------------------------------------------------------
// Wire-stable (logged), append only.
enum class Step : std::uint8_t
{
    Approach = 0,   // walking to the trigger / object / dock
    Use = 1,        // travel object: at the spot, trigger / use it
    Wait = 2,       // transport: on the dock, ship not docked here
    Board = 3,      // transport: ship docked here, walking onto the deck
    Ride = 4,       // transport: a passenger; do nothing
    Disembark = 5,  // transport: ship docked at the destination, walking onto the dock
    Done = 6        // at the crossing's exit: next crossing, or the final walk
};

inline constexpr char const* StepName(Step s)
{
    switch (s)
    {
        case Step::Approach: return "approach";
        case Step::Use: return "use";
        case Step::Wait: return "wait";
        case Step::Board: return "board";
        case Step::Ride: return "ride";
        case Step::Disembark: return "disembark";
        case Step::Done: return "done";
    }
    return "approach";
}

// What the runtime observed this tick (all booleans, computed with integer yards).
struct Obs
{
    bool atApproach = false;   // on `map` within reach of the trigger / object / dock point
    bool atExit = false;       // on `exitMap` within the arrival radius of the exit point
    bool onTransport = false;  // a passenger of this crossing's transport
    bool dockedHere = false;   // this crossing's transport stands at its boarding stop
    bool dockedExit = false;   // the bot's transport stands at the destination stop
};

// Travel object: approach, use, and done once the game has put the bot at the exit.
[[nodiscard]] inline Step NextObjectStep(Obs const& o)
{
    if (o.atExit)
        return Step::Done;
    return o.atApproach ? Step::Use : Step::Approach;
}

// Transport boarding machine. A missed ship falls back to Wait; falling off mid-sea restarts the
// crossing (Approach); stepping off anywhere on the exit side counts as done (the walk leg follows).
[[nodiscard]] inline Step NextTransportStep(Step s, Obs const& o)
{
    switch (s)
    {
        case Step::Approach:
        case Step::Use:
        case Step::Wait:
        case Step::Board:
            if (o.onTransport)
                return Step::Ride;
            if (!o.atApproach && s != Step::Board)
                return Step::Approach;
            return o.dockedHere ? Step::Board : Step::Wait;
        case Step::Ride:
            if (!o.onTransport)
                return o.atExit ? Step::Done : Step::Approach;
            return o.dockedExit ? Step::Disembark : Step::Ride;
        case Step::Disembark:
            if (!o.onTransport)
                return o.atExit ? Step::Done : Step::Approach;
            return o.dockedExit ? Step::Disembark : Step::Ride;  // ship left before we stepped off
        case Step::Done:
            return Step::Done;
    }
    return Step::Approach;
}

// ---- leg log (ledger `zone_move` "legs") -------------------------------------------------------
// Wire-stable leg names, append only.
enum class Leg : std::uint8_t
{
    Walk = 0,
    Flight = 1,
    TravelObject = 2,
    Transport = 3,
    Portal = 4  // fallback relocation dock -> dock (owner ruling 2026-09-24), see TransportMode
};

inline constexpr char const* LegName(Leg l)
{
    switch (l)
    {
        case Leg::Walk: return "walk";
        case Leg::Flight: return "flight";
        case Leg::TravelObject: return "travel_object";
        case Leg::Transport: return "transport";
        case Leg::Portal: return "portal";
    }
    return "walk";
}

// How a Via::Transport crossing is taken (AutoWow.Transports.Mode, per-entry ModeOverrides). Owner
// ruling 2026-09-24: a dock-to-dock portal is acceptable when riding is unreliable. Real = always ride;
// Portal = at the boarding dock the bot is relocated to the destination dock (no ship); Auto = ride,
// and switch that crossing to the portal after `autoPortalAfter` failed rides (missed boarding past the
// step timeout, or fell off mid-sea). Travel objects (triggers / orbs) are always real.
enum class TransportMode : std::uint8_t
{
    Real = 0,
    Portal = 1,
    Auto = 2
};

[[nodiscard]] inline bool ParseMode(std::string_view s, TransportMode& out)
{
    if (s == "real")
        out = TransportMode::Real;
    else if (s == "portal")
        out = TransportMode::Portal;
    else if (s == "auto")
        out = TransportMode::Auto;
    else
        return false;
    return true;
}

struct ModeOverride
{
    std::uint32_t object = 0;  // transport GameObject entry
    TransportMode mode = TransportMode::Auto;
};

// AutoWow.Transports.ModeOverrides: ';'-separated "entry=real|portal|auto". False (out untouched) on a
// malformed entry.
[[nodiscard]] inline bool ParseModeOverrides(std::string_view text, std::vector<ModeOverride>& out)
{
    std::vector<ModeOverride> parsed;
    while (!text.empty())
    {
        std::size_t const end = text.find(';');
        std::string_view entry = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!entry.empty() && entry.front() == ' ')
            entry.remove_prefix(1);
        while (!entry.empty() && entry.back() == ' ')
            entry.remove_suffix(1);
        if (entry.empty())
            continue;
        std::size_t const eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq > 9)
            return false;
        std::uint32_t object = 0;
        for (char ch : entry.substr(0, eq))
        {
            if (ch < '0' || ch > '9')
                return false;
            object = object * 10 + static_cast<std::uint32_t>(ch - '0');
        }
        ModeOverride o{object, TransportMode::Auto};
        if (!object || !ParseMode(entry.substr(eq + 1), o.mode))
            return false;
        parsed.push_back(o);
    }
    out = std::move(parsed);
    return true;
}

// Effective mode of a transport entry: the first override for it, else the global mode.
[[nodiscard]] inline TransportMode ModeFor(std::vector<ModeOverride> const& overrides, TransportMode global,
                                           std::uint32_t object)
{
    for (ModeOverride const& o : overrides)
        if (o.object == object)
            return o.mode;
    return global;
}

// True when this transport crossing is taken by portal now.
[[nodiscard]] inline bool UsePortal(TransportMode mode, std::uint32_t failedRides, std::uint32_t autoPortalAfter)
{
    return mode == TransportMode::Portal || (mode == TransportMode::Auto && failedRides >= autoPortalAfter);
}

inline constexpr Leg LegOf(Via v) { return v == Via::Transport ? Leg::Transport : Leg::TravelObject; }

inline constexpr std::uint32_t kMaxLegs = 12;

// Fixed-size leg log: open a leg with Open(), the previous one closes at that moment.
struct LegLog
{
    Leg kind[kMaxLegs] = {};
    std::uint32_t ms[kMaxLegs] = {};
    std::uint32_t count = 0;
    std::uint64_t openAt = 0;  // game ms the last leg opened
    bool open = false;

    void Close(std::uint64_t now)
    {
        if (open && count)
            ms[count - 1] = static_cast<std::uint32_t>(std::min<std::uint64_t>(now >= openAt ? now - openAt : 0,
                                                                              0xFFFFFFFFu));
        open = false;
    }

    // Same kind as the open leg: no-op (a reissued walk stays one leg). Full log: the last leg absorbs.
    void Open(Leg k, std::uint64_t now)
    {
        if (open && count && kind[count - 1] == k)
            return;
        if (count == kMaxLegs)
            return;
        Close(now);
        kind[count] = k;
        ms[count] = 0;
        ++count;
        openAt = now;
        open = true;
    }
};

// Trailing ledger field appended to zone_move (flag on only): ,"legs":[["walk",ms],...]
inline std::string LegsField(LegLog const& log)
{
    std::string out = ",\"legs\":[";
    for (std::uint32_t k = 0; k < log.count; ++k)
    {
        if (k)
            out += ',';
        out += "[\"";
        out += LegName(log.kind[k]);
        out += "\",";
        out += std::to_string(log.ms[k]);
        out += ']';
    }
    out += ']';
    return out;
}

// Per-bot chain progress (value-only; kept beside the zone-progression BotState).
struct ChainState
{
    std::uint8_t version = kStateVersion;
    std::uint32_t leg = 0;  // index into the route's chain; == chain size -> final walk to the hub
    Step step = Step::Approach;
    std::uint64_t stepAt = 0;  // game ms the current step began
    std::uint32_t failedRides = 0;  // this crossing; reset when the chain moves to the next crossing
    LegLog legs;
};

// A failed ride: a transport step restarted by the timeout, or the bot left the deck mid-sea.
[[nodiscard]] inline bool FailedRide(Step from, Step to, bool stuck)
{
    return to == Step::Approach && (stuck || from == Step::Ride || from == Step::Disembark);
}

struct Params
{
    TransportMode mode = TransportMode::Auto;  // AutoWow.Transports.Mode
    std::uint32_t autoPortalAfter = 2;         // AutoWow.Transports.AutoPortalAfterFailures
    std::uint32_t approachYards = 5;     // AutoWow.Transports.ApproachYards: at trigger / object / dock
    std::uint32_t exitYards = 60;         // AutoWow.Transports.ExitYards: arrived on the exit side
    std::uint32_t dockedYards = 8;        // AutoWow.Transports.DockedYards: ship at its stop
    std::uint32_t joinYards = 300;        // AutoWow.Transports.JoinYards: StartLeg radius
    std::uint32_t stepTimeoutMs = 900000; // AutoWow.Transports.StepTimeoutMs: stuck step -> restart crossing
};

// A step older than the timeout restarts the crossing (Approach); Ride is exempt (the ship moves on
// its own schedule; the zone-progression travel timeout still bounds the whole move).
[[nodiscard]] inline bool StepStuck(Params const& p, Step s, std::uint64_t stepAt, std::uint64_t now)
{
    return s != Step::Ride && s != Step::Done && now >= stepAt && now - stepAt > p.stepTimeoutMs;
}

// ---- runtime (NewRpgZoneProgression.cpp) --------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
inline std::vector<Crossing> gCrossings;
inline std::vector<ModeOverride> gOverrides;
}
inline bool Enabled() { return detail::gEnabled; }

void LoadConfig();
}  // namespace AutoWowTransports

#endif  // AUTOWOW_TRANSPORT_CROSSING_POLICY_H
