/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ZONE_PROGRESSION_POLICY_H
#define AUTOWOW_ZONE_PROGRESSION_POLICY_H

#include <algorithm>
#include <cmath>
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
inline constexpr std::uint8_t kStateVersion = 2;  // 2: BotState.noFlight (AutoWow.Unstick.V2)

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

// AutoWow.ZoneProgression.HighRoutes (default 0): L20-60 quest hubs per faction and continent. A hub
// serves bots whose level lies in its band; `stage` orders the leveling tiers (1: 20-30 ... 4: 50-60).
// Coordinates = the world DB spawn of `npc` (the hub's innkeeper, else its flight master when the hub
// has no inn; inn 0 = no hearth bind). Every npc's faction template is friendly or neutral to `team`
// (FactionTemplate.dbc enemy mask; tests/ZoneProgressionPolicyTest.cpp keeps the checked table).
struct Hub
{
    std::uint32_t team = 0;  // 1 alliance, 2 horde
    std::uint32_t stage = 0;
    std::uint32_t zone = 0;
    std::uint32_t minLevel = 0;
    std::uint32_t maxLevel = 0;
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t inn = 0;
    std::uint32_t npc = 0;
};

inline std::vector<Hub> DefaultHubs()
{
    return {
        // Alliance, Eastern Kingdoms.
        {1, 1, 10, 20, 28, 0, -10516, -1161, 28, 6790, 6790},     // Duskwood: Darkshire
        {1, 1, 11, 21, 28, 0, -3828, -832, 10, 1464, 1464},       // Wetlands: Menethil Harbor
        {1, 1, 267, 23, 30, 0, -857, -571, 11, 2352, 2352},       // Hillsbrad: Southshore
        {1, 2, 33, 30, 38, 0, -11340, -219, 75, 0, 24366},        // Stranglethorn: Rebel Camp (FM)
        {1, 2, 45, 30, 40, 0, -1240, -2514, 22, 0, 2835},       // Arathi: Refuge Pointe (FM)
        {1, 3, 33, 38, 45, 0, -14458, 495, 15, 6807, 6807},       // Stranglethorn: Booty Bay
        {1, 3, 47, 40, 48, 0, 400, -2120, 132, 7744, 7744},       // Hinterlands: Aerie Peak
        {1, 3, 51, 45, 50, 0, -6559, -1169, 310, 0, 2941},        // Searing Gorge: Thorium Point (FM)
        {1, 3, 4, 47, 54, 0, -11110, -3437, 79, 0, 8609},         // Blasted Lands: Nethergarde Keep (FM)
        {1, 4, 46, 50, 58, 0, -8365, -2737, 186, 0, 2299},        // Burning Steppes: Morgan's Vigil (FM)
        {1, 4, 28, 51, 58, 0, 928, -1429, 65, 0, 12596},          // Western Plaguelands: Chillwind Camp (FM)
        {1, 4, 139, 54, 60, 0, 2301, -5345, 91, 16256, 16256},    // Eastern Plaguelands: Light's Hope
        // Alliance, Kalimdor.
        {1, 1, 331, 20, 28, 1, 2781, -433, 117, 6738, 6738},      // Ashenvale: Astranaar
        {1, 1, 406, 20, 27, 1, 2730, 1498, 238, 16458, 16458},    // Stonetalon: Stonetalon Peak
        {1, 2, 405, 28, 38, 1, 256, 1254, 192, 11103, 11103},     // Desolace: Nijel's Point
        {1, 2, 15, 35, 43, 1, -3616, -4471, 14, 6272, 6272},      // Dustwallow: Theramore
        {1, 3, 440, 40, 48, 1, -7159, -3842, 9, 7733, 7733},      // Tanaris: Gadgetzan
        {1, 3, 357, 41, 48, 1, -4491, -778, -40, 0, 4319},        // Feralas: Thalanaar (FM)
        {1, 3, 16, 45, 52, 1, 2718, -3881, 102, 0, 12577},        // Azshara: Talrendis Point (FM)
        {1, 4, 490, 48, 55, 1, -6111, -1140, -187, 0, 10583},     // Un'Goro: Marshal's Refuge (FM)
        {1, 4, 361, 48, 55, 1, 6204, -1951, 572, 0, 12578},       // Felwood: Talonbranch Glade (FM)
        {1, 4, 618, 53, 60, 1, 6695, -4673, 722, 11118, 11118},   // Winterspring: Everlook
        {1, 4, 1377, 55, 60, 1, -6868, 730, 46, 15174, 15174},    // Silithus: Cenarion Hold
        // Horde, Eastern Kingdoms.
        {2, 1, 267, 20, 30, 0, -6, -942, 57, 2388, 2388},         // Hillsbrad: Tarren Mill
        {2, 2, 45, 30, 38, 0, -912, -3525, 73, 9501, 9501},       // Arathi: Hammerfall
        {2, 2, 33, 30, 38, 0, -12434, 212, 2, 5814, 5814},        // Stranglethorn: Grom'gol
        {2, 2, 8, 35, 43, 0, -10487, -3259, 21, 6930, 6930},      // Swamp of Sorrows: Stonard
        {2, 2, 3, 36, 44, 0, -6650, -2149, 245, 9356, 9356},      // Badlands: Kargath
        {2, 3, 33, 38, 45, 0, -14458, 495, 15, 6807, 6807},       // Stranglethorn: Booty Bay
        {2, 3, 47, 42, 48, 0, -622, -4583, 12, 14731, 14731},     // Hinterlands: Revantusk Village
        {2, 3, 51, 45, 50, 0, -6559, -1100, 310, 0, 3305},        // Searing Gorge: Thorium Point (FM)
        {2, 4, 46, 50, 58, 0, -7504, -2191, 165, 0, 13177},       // Burning Steppes: Flame Crest (FM)
        {2, 4, 139, 53, 60, 0, 2301, -5345, 91, 16256, 16256},    // Eastern Plaguelands: Light's Hope
        // Horde, Kalimdor.
        {2, 1, 406, 20, 27, 1, 894, 928, 106, 7731, 7731},        // Stonetalon: Sun Rock Retreat
        {2, 1, 331, 20, 30, 1, 2342, -2567, 103, 12196, 12196},   // Ashenvale: Splintertree Post
        {2, 1, 400, 25, 33, 1, -5478, -2460, 89, 11116, 11116},   // Thousand Needles: Freewind Post
        {2, 2, 405, 30, 38, 1, -1593, 3150, 47, 11106, 11106},    // Desolace: Shadowprey Village
        {2, 2, 15, 35, 43, 1, -3166, -2912, 35, 24208, 24208},    // Dustwallow: Brackenwall Village
        {2, 3, 440, 40, 48, 1, -7159, -3842, 9, 7733, 7733},      // Tanaris: Gadgetzan
        {2, 3, 357, 41, 48, 1, -4460, 243, 39, 7737, 7737},       // Feralas: Camp Mojache
        {2, 3, 16, 45, 52, 1, 3664, -4390, 113, 0, 8610},         // Azshara: Valormok (FM)
        {2, 4, 490, 48, 55, 1, -6111, -1140, -187, 0, 10583},     // Un'Goro: Marshal's Refuge (FM)
        {2, 4, 361, 48, 55, 1, 5065, -339, 367, 0, 11900},        // Felwood: Bloodvenom Post (FM)
        {2, 4, 618, 53, 60, 1, 6695, -4673, 722, 11118, 11118},   // Winterspring: Everlook
        {2, 4, 1377, 55, 60, 1, -6868, 730, 46, 15174, 15174},    // Silithus: Cenarion Hold
    };
}

// Zones below the first hub tier a bot may still stand in at L20+ (starter/10-20 zones and capitals),
// per team and continent. They and every lower-or-same-stage hub zone lead to each hub.
struct HubSource
{
    std::uint32_t team = 0;
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
};

inline std::vector<HubSource> DefaultHubSources()
{
    return {
        // Alliance EK: Dun Morogh, Elwynn, Loch Modan, Westfall, Redridge, Stormwind, Ironforge.
        {1, 0, 1}, {1, 0, 12}, {1, 0, 38}, {1, 0, 40}, {1, 0, 44}, {1, 0, 1519}, {1, 0, 1537},
        // Alliance Kalimdor: Teldrassil, Darkshore, Barrens, Darnassus.
        {1, 1, 141}, {1, 1, 148}, {1, 1, 17}, {1, 1, 1657},
        // Horde EK: Tirisfal, Silverpine, Undercity.
        {2, 0, 85}, {2, 0, 130}, {2, 0, 1497},
        // Horde Kalimdor: Durotar, Mulgore, Barrens, Orgrimmar, Thunder Bluff.
        {2, 1, 14}, {2, 1, 215}, {2, 1, 17}, {2, 1, 1637}, {2, 1, 1638},
    };
}

// Expands hubs into routes: every source zone and every hub zone of a lower-or-same stage (same team
// and continent, not the hub's own zone) leads to the hub, band = the hub's band. Order: hub table
// order, then source zones ascending (deterministic). Walkable only (same continent, crossing 0).
[[nodiscard]] inline std::vector<Route> HubRoutes(std::vector<Hub> const& hubs, std::vector<HubSource> const& sources)
{
    std::vector<Route> out;
    for (Hub const& h : hubs)
    {
        std::vector<std::uint32_t> from;
        for (HubSource const& s : sources)
            if (s.team == h.team && s.map == h.map)
                from.push_back(s.zone);
        for (Hub const& o : hubs)
            if (o.team == h.team && o.map == h.map && o.stage <= h.stage)
                from.push_back(o.zone);
        std::sort(from.begin(), from.end());
        from.erase(std::unique(from.begin(), from.end()), from.end());
        for (std::uint32_t zone : from)
            if (zone != h.zone)
                out.push_back(Route{h.team, zone, h.zone, h.minLevel, h.maxLevel, h.map, h.x, h.y, h.z, h.inn, false});
    }
    return out;
}

// AutoWow.ZoneProgression.Outland (default 0): the L58-70 Outland ladder. soak-s76: 14 cohort bots at L58-67
// and none on map 530 - no hub band reached past 60, so a L61+ bot had no route anywhere and ground grey
// Silithus / Winterspring mobs. Hubs = world DB innkeeper spawns on map 530 (stages continue DefaultHubs).
inline constexpr std::uint32_t kOutlandMinLevel = 58;
inline constexpr std::uint32_t kOutlandMaxLevel = 70;
inline constexpr std::uint32_t kOutlandMap = 530;
inline constexpr std::uint32_t kHellfireZone = 3483;

inline std::vector<Hub> OutlandHubs()
{
    return {
        {1, 5, 3483, 58, 63, 530, -709, 2739, 95, 16826, 16826},   // Hellfire: Honor Hold
        {2, 5, 3483, 58, 63, 530, 191, 2611, 87, 16602, 16602},    // Hellfire: Thrallmar
        {1, 6, 3521, 60, 64, 530, 282, 6098, 133, 18251, 18251},   // Zangarmarsh: Telredor
        {2, 6, 3521, 60, 64, 530, 228, 7934, 25, 18245, 18245},    // Zangarmarsh: Zabra'jin
        {1, 7, 3519, 62, 65, 530, -2918, 4021, 1, 19296, 19296},   // Terokkar: Allerian Stronghold
        {2, 7, 3519, 62, 65, 530, -2622, 4450, 36, 18957, 18957},  // Terokkar: Stonebreaker Hold
        {1, 8, 3518, 64, 67, 530, -2761, 7300, 44, 18914, 18914},  // Nagrand: Telaar
        {2, 8, 3518, 64, 67, 530, -1230, 7161, 57, 18913, 18913},  // Nagrand: Garadar
        {1, 9, 3522, 65, 68, 530, 2098, 6904, 183, 19495, 19495},  // Blade's Edge: Sylvanaar
        {2, 9, 3522, 65, 68, 530, 2404, 5951, 152, 19470, 19470},  // Blade's Edge: Thunderlord Stronghold
        {1, 10, 3523, 67, 70, 530, 3062, 3702, 143, 19571, 19571}, // Netherstorm: Area 52
        {2, 10, 3523, 67, 70, 530, 3062, 3702, 143, 19571, 19571}, // Netherstorm: Area 52
        {1, 10, 3520, 67, 70, 530, -4084, 2187, 108, 19352, 19352}, // Shadowmoon: Wildhammer Stronghold
        {2, 10, 3520, 67, 70, 530, -2965, 2562, 79, 19319, 19319},  // Shadowmoon: Shadowmoon Village
    };
}

// Outland zones a bot may stand in outside the hub zones: Shattrath City.
inline std::vector<HubSource> OutlandSources() { return {{1, 530, 3703}, {2, 530, 3703}}; }

// Azeroth zones an Outland entry trip leaves from: every DefaultHubs zone and DefaultHubSources zone of the
// team (both continents). Ascending (map, zone), deterministic.
[[nodiscard]] inline std::vector<HubSource> OutlandEntryZones(std::uint32_t team)
{
    std::vector<HubSource> out;
    for (HubSource const& s : DefaultHubSources())
        if (s.team == team)
            out.push_back(s);
    for (Hub const& h : DefaultHubs())
        if (h.team == team)
            out.push_back(HubSource{team, h.map, h.zone});
    std::sort(out.begin(), out.end(), [](HubSource const& a, HubSource const& b)
              { return a.map != b.map ? a.map < b.map : a.zone < b.zone; });
    out.erase(std::unique(out.begin(), out.end(), [](HubSource const& a, HubSource const& b)
                          { return a.map == b.map && a.zone == b.zone; }),
              out.end());
    return out;
}

// The Outland routes: entry routes (every OutlandEntryZones zone -> the team's Hellfire hub, band 58-70,
// crossing: the Dark Portal chain, AutoWow.Transports OutlandCrossings) then the in-Outland HubRoutes.
[[nodiscard]] inline std::vector<Route> OutlandRoutes()
{
    std::vector<Hub> const hubs = OutlandHubs();
    std::vector<Route> out;
    for (Hub const& h : hubs)
        if (h.zone == kHellfireZone)
            for (HubSource const& s : OutlandEntryZones(h.team))
                out.push_back(Route{h.team, s.zone, h.zone, kOutlandMinLevel, kOutlandMaxLevel, h.map, h.x, h.y, h.z,
                                    h.inn, true});
    for (Route const& r : HubRoutes(hubs, OutlandSources()))
        out.push_back(r);
    return out;
}

// Applies the ladder to a route table: Azeroth hub bands stop at 57 (a L58+ bot's only way on is Outland),
// then the Outland routes are appended.
inline void AddOutland(std::vector<Route>& routes)
{
    for (Route& r : routes)
        if ((r.map == 0 || r.map == 1) && r.minLevel < kOutlandMinLevel && r.maxLevel >= kOutlandMinLevel)
            r.maxLevel = kOutlandMinLevel - 1;
    for (Route const& r : OutlandRoutes())
        routes.push_back(r);
}

// An Outland entry trip (hub on map 530, crossing): its crossing chain may take the portal fallback like a
// walk leg (FallbackMode), on its own clock (AutoWow.ZoneProgression.OutlandPortalAfterMs).
[[nodiscard]] inline bool IsOutlandEntry(Route const& r) { return r.map == kOutlandMap && r.crossing && r.to == kHellfireZone; }

// AutoWow.ZoneProgression.Northrend (default 0): native admission to the two faction Borean hubs.
// Admission is deliberately separate from Borean's quest bracket (installed default 68-75): once the
// configured floor is reached, passage stays eligible through the server's configured maximum level.
inline constexpr std::uint32_t kNorthrendMinLevel = 68;
inline constexpr std::uint32_t kNorthrendMap = 571;
inline constexpr std::uint32_t kBoreanZone = 3537;
inline constexpr std::uint32_t kStormwindZone = 1519;
inline constexpr std::uint32_t kOrgrimmarZone = 1637;

[[nodiscard]] inline Hub NorthrendArrivalHub(std::uint32_t team)
{
    if (team == 1)
        return {1, 11, kBoreanZone, 68, 75, kNorthrendMap, 2282, 5210, 12, 25245, 25245};
    return {2, 11, kBoreanZone, 68, 75, kNorthrendMap, 2803, 6154, 85, 25278, 25278};
}

// Capital plus every zone in the existing Outland frontier. Ascending (map, zone), deterministic.
[[nodiscard]] inline std::vector<HubSource> NorthrendEntryZones(std::uint32_t team)
{
    std::vector<HubSource> out = {{team, team == 1 ? 0U : 1U, team == 1 ? kStormwindZone : kOrgrimmarZone}};
    for (Hub const& h : OutlandHubs())
        if (h.team == team)
            out.push_back({team, h.map, h.zone});
    for (HubSource const& s : OutlandSources())
        if (s.team == team)
            out.push_back(s);
    std::sort(out.begin(), out.end(), [](HubSource const& a, HubSource const& b)
              { return a.map != b.map ? a.map < b.map : a.zone < b.zone; });
    out.erase(std::unique(out.begin(), out.end(), [](HubSource const& a, HubSource const& b)
                          { return a.map == b.map && a.zone == b.zone; }),
              out.end());
    return out;
}

[[nodiscard]] inline std::vector<Route> NorthrendEntryRoutes(std::uint32_t maxPlayerLevel,
                                                             std::uint32_t admissionMinLevel = kNorthrendMinLevel)
{
    std::vector<Route> out;
    if (maxPlayerLevel < admissionMinLevel)
        return out;
    for (std::uint32_t team : {1U, 2U})
    {
        Hub const h = NorthrendArrivalHub(team);
        for (HubSource const& s : NorthrendEntryZones(team))
            out.push_back(Route{team, s.zone, h.zone, admissionMinLevel, maxPlayerLevel, h.map, h.x, h.y, h.z,
                                h.inn, true});
    }
    return out;
}

// Append admission without mutating the shared Outland bands used by death-loop escape selection.
inline void AddNorthrend(std::vector<Route>& routes, std::uint32_t maxPlayerLevel,
                         std::uint32_t admissionMinLevel = kNorthrendMinLevel)
{
    if (maxPlayerLevel < admissionMinLevel)
        return;
    for (Route const& r : NorthrendEntryRoutes(maxPlayerLevel, admissionMinLevel))
        routes.push_back(r);
}

[[nodiscard]] inline bool IsNorthrendEntry(Route const& r)
{
    return r.map == kNorthrendMap && r.crossing && r.to == kBoreanZone;
}

// AutoWow.ZoneProgression.Northrend2 (default 0; needs Northrend): soak-s107b - 23 of 30 L70+ cohort bots sat
// in Eastern Kingdoms at ~950 XP/bot-h. Admission left only from a capital or Outland, so a L68+ bot that
// wandered or hearthed anywhere else had no route (unstick planned an empty chain: mode none in ~1.3 s).
// Northrend2 adds (a) a staging trip from any Azeroth zone to the team's chain start, (b) Horde chain starts
// Stranglethorn (Grom'gol zeppelin) and Durotar (Orgrimmar zeppelin tower), (c) the Outland start through
// the faction city portal instead of the Dark Portal walk (TransportCrossingPolicy Northrend2Crossings),
// (d) the L68-80 Northrend hub ladder, (e) death-loop escape kept on the bot's own map.
inline constexpr std::uint32_t kStranglethornZone = 33;
inline constexpr std::uint32_t kDurotarZone = 14;

// Chain starts: NorthrendEntryZones plus the Horde Grom'gol / Durotar starts. Ascending (map, zone).
[[nodiscard]] inline std::vector<HubSource> Northrend2EntryZones(std::uint32_t team)
{
    std::vector<HubSource> out = NorthrendEntryZones(team);
    if (team == 2)
    {
        out.push_back({2, 0, kStranglethornZone});
        out.push_back({2, 1, kDurotarZone});
    }
    std::sort(out.begin(), out.end(), [](HubSource const& a, HubSource const& b)
              { return a.map != b.map ? a.map < b.map : a.zone < b.zone; });
    return out;
}

// Northrend hubs (map 571; world DB innkeeper spawns, zone = map-grid area of the spawn, faction checked in
// tests/ZoneProgressionPolicyTest.cpp). Stages continue OutlandHubs; Borean = the admission hub.
inline std::vector<Hub> NorthrendHubs()
{
    Hub const borA = NorthrendArrivalHub(1);
    Hub const borH = NorthrendArrivalHub(2);
    return {
        {1, 11, kBoreanZone, 68, 72, kNorthrendMap, borA.x, borA.y, borA.z, borA.inn, borA.npc},  // Valiance Keep
        {2, 11, kBoreanZone, 68, 72, kNorthrendMap, borH.x, borH.y, borH.z, borH.inn, borH.npc},  // Warsong Hold
        {1, 11, 495, 68, 72, 571, 599, -4928, 19, 23731, 23731},       // Howling Fjord: Valgarde
        {2, 11, 495, 68, 72, 571, 1873, -6218, 13, 24342, 24342},      // Howling Fjord: Vengeance Landing
        {1, 12, 65, 71, 75, 571, 3478, 2007, 65, 27052, 27052},        // Dragonblight: Stars' Rest
        {2, 12, 65, 71, 75, 571, 3830, 1485, 92, 26985, 26985},        // Dragonblight: Agmar's Hammer
        {1, 13, 394, 73, 75, 571, 3407, -2789, 202, 27066, 27066},     // Grizzly Hills: Amberpine Lodge
        {2, 13, 394, 73, 75, 571, 3256, -2202, 117, 27125, 27125},     // Grizzly Hills: Conquest Hold
        {1, 14, 66, 74, 77, 571, 5463, -2639, 307, 28791, 28791},      // Zul'Drak: The Argent Stand
        {2, 14, 66, 74, 77, 571, 5463, -2639, 307, 28791, 28791},      // Zul'Drak: The Argent Stand
        {1, 15, 3711, 76, 78, 571, 5566, 5764, -75, 28038, 28038},     // Sholazar: Nesingwary Base Camp
        {2, 15, 3711, 76, 78, 571, 5566, 5764, -75, 28038, 28038},     // Sholazar: Nesingwary Base Camp
        {1, 16, 67, 77, 80, 571, 6671, -200, 951, 29926, 29926},       // Storm Peaks: Frosthold
        {2, 16, 67, 77, 80, 571, 7850, -799, 1185, 29944, 29944},      // Storm Peaks: Grom'arsh Crash-Site
        {1, 16, 210, 77, 80, 571, 8605, 666, 550, 33970, 33970},       // Icecrown: Argent Tournament (Silver Covenant)
        {2, 16, 210, 77, 80, 571, 8425, 671, 550, 33971, 33971},       // Icecrown: Argent Tournament (Sunreavers)
    };
}

// Appended after AddNorthrend: the Horde Stranglethorn / Durotar admission routes, then the Northrend ladder.
inline void AddNorthrend2(std::vector<Route>& routes, std::uint32_t maxPlayerLevel)
{
    if (maxPlayerLevel < kNorthrendMinLevel)
        return;
    Hub const h = NorthrendArrivalHub(2);
    for (std::uint32_t zone : {kStranglethornZone, kDurotarZone})
        routes.push_back(Route{2, zone, h.zone, kNorthrendMinLevel, maxPlayerLevel, h.map, h.x, h.y, h.z, h.inn, true});
    for (Route const& r : HubRoutes(NorthrendHubs(), {}))
        routes.push_back(r);
}

// Staging points (world DB innkeeper spawns): Stormwind (Trade District inn), Orgrimmar (Gryshka), Grom'gol.
inline std::vector<Hub> NorthrendStagingHubs()
{
    return {
        {1, 0, kStormwindZone, kNorthrendMinLevel, 0, 0, -8868, 674, 98, 6740, 6740},
        {2, 0, kOrgrimmarZone, kNorthrendMinLevel, 0, 1, 1634, -4439, 16, 6929, 6929},
        {2, 0, kStranglethornZone, kNorthrendMinLevel, 0, 0, -12434, 212, 2, 5814, 5814},
    };
}

// The staging trip of a L68+ bot on an Azeroth continent outside every Northrend2 chain start: to the hearth
// zone when `hearthZone` (0 = hearthstone not ready) is a staging zone of the team, else the staging point
// on the bot's own continent (Alliance on Kalimdor: Stormwind, a crossing). false = no staging (out filled).
[[nodiscard]] inline bool NorthrendStagingRoute(std::uint32_t team, std::uint32_t map, std::uint32_t zone,
                                                std::uint32_t level, std::uint32_t maxPlayerLevel,
                                                std::uint32_t hearthZone, Route& out)
{
    if ((map != 0 && map != 1) || level < kNorthrendMinLevel || level > maxPlayerLevel)
        return false;
    for (HubSource const& s : Northrend2EntryZones(team))
        if (s.zone == zone)
            return false;
    std::vector<Hub> const hubs = NorthrendStagingHubs();
    Hub const* pick = nullptr;
    for (Hub const& h : hubs)
        if (h.team == team && hearthZone && h.zone == hearthZone)
            pick = pick ? pick : &h;
    for (Hub const& h : hubs)
        if (h.team == team && h.map == map)
            pick = pick ? pick : &h;
    for (Hub const& h : hubs)
        if (h.team == team)
            pick = pick ? pick : &h;
    if (!pick)
        return false;
    out = Route{team, zone, pick->zone, kNorthrendMinLevel, maxPlayerLevel, pick->map, pick->x, pick->y, pick->z,
                pick->inn, pick->map != map};
    return true;
}

// A staging trip arrives on entering its zone (a hearth lands at the bind spot, not at the hub inn).
[[nodiscard]] inline bool IsNorthrendStaging(Route const& r)
{
    if (r.minLevel < kNorthrendMinLevel)
        return false;
    std::vector<Hub> const hubs = NorthrendStagingHubs();
    return std::any_of(hubs.begin(), hubs.end(), [&](Hub const& h)
                       { return h.team == r.team && h.zone == r.to && h.map == r.map && h.inn == r.inn; });
}

// Northrend2 death-loop escape: candidate routes whose hub lies on the bot's own map only.
[[nodiscard]] inline std::vector<Route> SameMapRoutes(std::vector<Route> const& routes, std::uint32_t map)
{
    std::vector<Route> out;
    for (Route const& r : routes)
        if (r.map == map)
            out.push_back(r);
    return out;
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

// Destination: the routes leaving `fromZone` for the bot's team whose band holds `level`. An eligible
// Northrend admission has deterministic priority; otherwise table order and guid spreading stay unchanged.
[[nodiscard]] inline Route const* PickRoute(std::vector<Route> const& routes, std::uint32_t team,
                                            std::uint32_t fromZone, std::uint32_t level, std::uint32_t guid)
{
    std::vector<Route const*> fit;
    for (Route const& r : routes)
    {
        if (r.from != fromZone || (r.team != 0 && r.team != team) || level < r.minLevel || level > r.maxLevel)
            continue;
        if (IsNorthrendEntry(r))
            return &r;
        fit.push_back(&r);
    }
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
        if (IsNorthrendEntry(r) || r.to == zone || (r.team != 0 && r.team != team) || level < r.minLevel ||
            level > r.maxLevel)
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
        if (IsNorthrendEntry(r) || r.to == zone || (r.team != 0 && r.team != team) || level < r.minLevel ||
            level > r.maxLevel)
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

// AutoWow.Survival.HardEscape: the straight line a -> b passes a danger zone. Samples every stepYards
// (Chebyshev spacing, integer interpolation, a excluded, b included); zoneAt(x, y) -> zone id (0 = none),
// danger(zone) -> too high for the bot. Samples in skipZone (the zone the bot already stands in) never count.
inline constexpr std::uint32_t kDangerStepYards = 50;
inline constexpr float kHubArrivalYards = 15.0f;

// Runtime arrival and same-zone fallback admission share the same exact 2D hub radius. Being in the
// destination zone alone never means the trip arrived.
[[nodiscard]] inline bool AtRouteHub(Route const& route, std::uint32_t map, float x, float y)
{
    if (map != route.map)
        return false;
    float const dx = float(route.x) - x;
    float const dy = float(route.y) - y;
    return dx * dx + dy * dy < kHubArrivalYards * kHubArrivalYards;
}

struct PhysicalHubFacts
{
    bool anyTransport = false;
    bool inFlight = false;
    bool teleporting = false;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

[[nodiscard]] inline bool AtPhysicalNorthrendHub(Route const& route, PhysicalHubFacts const& f,
                                                 float verticalYards = 3.0f)
{
    if (!IsNorthrendEntry(route) || f.anyTransport || f.inFlight || f.teleporting || f.map != route.map ||
        !std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z) || !std::isfinite(verticalYards) ||
        verticalYards < 0.0f)
        return false;
    float const dx = float(route.x) - f.x;
    float const dy = float(route.y) - f.y;
    return dx * dx + dy * dy < kHubArrivalYards * kHubArrivalYards &&
           std::fabs(float(route.z) - f.z) <= verticalYards;
}

template <class ZoneAt, class Danger>
[[nodiscard]] inline bool SegmentCrossesDanger(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by,
                                               std::uint32_t stepYards, std::uint32_t skipZone, ZoneAt const& zoneAt,
                                               Danger const& danger)
{
    std::int64_t const dx = std::int64_t(bx) - ax;
    std::int64_t const dy = std::int64_t(by) - ay;
    std::int64_t const n = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) / std::max<std::uint32_t>(stepYards, 1) + 1;
    for (std::int64_t i = 1; i <= n; ++i)
    {
        std::uint32_t const zone = zoneAt(static_cast<std::int32_t>(ax + dx * i / n),
                                          static_cast<std::int32_t>(ay + dy * i / n));
        if (zone && zone != skipZone && danger(zone))
            return true;
    }
    return false;
}

// AutoWow.Survival.HardEscape escape hub: the PickEscapeRoute candidates (team, band holds `level`, not the
// bot's zone), first the nearest on the bot's map whose straight line crosses no danger zone
// (crosses(route), evaluated lazily nearest first), else the nearest on another map (flight / portal leg),
// else the nearest on the bot's map anyway. Distance ties keep table order. soak-s22-full-r1: a L18 in
// Westfall took Loch Modan (lowest band) across Burning Steppes; here Duskwood.
// ponytail: straight-line distance stands in for the path length once the line is proven danger-free;
// a travel-node graph router is the upgrade if safe lines start detouring far.
template <class Crosses>
[[nodiscard]] inline Route const* PickSafeEscapeRoute(std::vector<Route> const& routes, std::uint32_t team,
                                                      std::uint32_t level, std::uint32_t zone, std::uint32_t map,
                                                      std::int32_t x, std::int32_t y, Crosses const& crosses)
{
    struct Candidate
    {
        Route const* route;
        bool sameMap;
        std::int64_t dist2;
    };
    std::vector<Candidate> fit;
    for (Route const& r : routes)
    {
        if (IsNorthrendEntry(r) || r.to == zone || (r.team != 0 && r.team != team) || level < r.minLevel ||
            level > r.maxLevel)
            continue;
        std::int64_t const dx = std::int64_t(r.x) - x;
        std::int64_t const dy = std::int64_t(r.y) - y;
        fit.push_back(Candidate{&r, r.map == map, r.map == map ? dx * dx + dy * dy : 0});
    }
    std::stable_sort(fit.begin(), fit.end(), [](Candidate const& a, Candidate const& b)
                     { return a.sameMap != b.sameMap ? a.sameMap : a.dist2 < b.dist2; });
    for (Candidate const& c : fit)
        if (c.sameMap && !crosses(*c.route))
            return c.route;
    for (Candidate const& c : fit)
        if (!c.sameMap)
            return c.route;
    return fit.empty() ? nullptr : fit.front().route;
}

// Death-loop-only last resort after the existing picker found no cross-zone hub: choose the nearest
// faction/level-fitting hub in the current zone and map, outside the arrival radius. This composition is
// used by StartEscape; ordinary level/no-quest routing keeps using PickRoute. Hub routes repeat for many
// source zones, so matching nonzero inn identities or exact coordinates are evaluated once in table order.
// Unlike PickSafeEscapeRoute, an unsafe candidate is never returned as a fallback.
template <class Unsafe>
[[nodiscard]] inline Route const* PickEscapeOrSafeSameZoneHub(Route const* crossZoneHub,
                                                              std::vector<Route> const& routes,
                                                              std::uint32_t team, std::uint32_t level,
                                                              std::uint32_t zone, std::uint32_t map, std::int32_t x,
                                                              std::int32_t y, Unsafe const& unsafe)
{
    if (crossZoneHub && !IsNorthrendEntry(*crossZoneHub))
        return crossZoneHub;
    struct Candidate
    {
        Route const* route;
        std::int64_t dist2;
    };
    std::vector<Candidate> fit;
    for (Route const& r : routes)
    {
        if (IsNorthrendEntry(r) || r.to != zone || r.map != map || (r.team != 0 && r.team != team) ||
            level < r.minLevel || level > r.maxLevel || AtRouteHub(r, map, float(x), float(y)))
            continue;
        bool const duplicate = std::any_of(fit.begin(), fit.end(), [&](Candidate const& c)
        {
            Route const& seen = *c.route;
            return (r.inn && r.inn == seen.inn) ||
                   (r.map == seen.map && r.x == seen.x && r.y == seen.y && r.z == seen.z);
        });
        if (duplicate)
            continue;
        std::int64_t const dx = std::int64_t(r.x) - x;
        std::int64_t const dy = std::int64_t(r.y) - y;
        fit.push_back(Candidate{&r, dx * dx + dy * dy});
    }
    std::stable_sort(fit.begin(), fit.end(),
                     [](Candidate const& a, Candidate const& b) { return a.dist2 < b.dist2; });
    for (Candidate const& c : fit)
        if (!unsafe(*c.route))
            return c.route;
    return nullptr;
}

// Wire-stable ledger reason names; append only.
enum class Trigger : std::uint8_t
{
    None = 0,
    Level = 1,     // level >= zone bracket max - margin
    NoQuests = 2,  // nothing actionable in the quest log for N consecutive checks
    DeathLoop = 3, // AutoWow.DeathLoop.EscapeViaZoneProgression: escape from an over-level zone
    Stuck = 4      // AutoWow.Unstick.V2: no grind spot in reach and no XP for NoXpMs (UnstickPolicy.h)
};

inline constexpr char const* TriggerName(Trigger t)
{
    switch (t)
    {
        case Trigger::None: return "none";
        case Trigger::Level: return "level";
        case Trigger::NoQuests: return "no_quests";
        case Trigger::DeathLoop: return "death_loop";
        case Trigger::Stuck: return "unstick";
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
    std::uint32_t outlandPortalAfterMs = 2400000;  // AutoWow.ZoneProgression.OutlandPortalAfterMs (entry trips)
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
    Portal = 4, // AutoWow.Transports (mode auto/portal): walk leg spent -> portal to the hub (owner ruling)
    Hearth = 5  // AutoWow.ZoneProgression.Northrend2: hearthstone to the staging zone
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
        case Mode::Hearth: return "hearth";
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

// AutoWow.Walking.V2: JoinRoad, then one point further when the bot already lies between the nearest
// point and the next (closer to the next than the nearest is). soak-s14: a Coldridge bot at
// (-6080,389) joined (-6342,482) behind it and spent its walk budget heading back into the valley.
[[nodiscard]] inline std::uint32_t JoinRoadAhead(std::vector<RoadPoint> const& road, std::int32_t x, std::int32_t y)
{
    std::uint32_t k = JoinRoad(road, x, y);
    if (k + 1 < road.size() && RoadDist2(road[k + 1], x, y) < RoadDist2(road[k + 1], road[k].x, road[k].y))
        ++k;
    return k;
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

// AutoWow.ZoneProgression.Outland: the mode PortalFallback judges. An Outland entry trip's crossing chain
// (Silithus -> Orgrimmar portal -> Dark Portal -> Thrallmar) counts as a walk leg; every other chain keeps
// its own give-up rule.
[[nodiscard]] inline Mode FallbackMode(Mode mode, bool outlandEntry)
{
    return outlandEntry && mode == Mode::Chain ? Mode::Walk : mode;
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
    bool noFlight = false;    // AutoWow.Unstick.V2: a flight leg of this trip fell back to idle; walk from now on
};

// Missing state is idle. An invalid stored version or an active trip owns movement and fails material-driver
// admission closed; callers must use the non-inserting runtime getter below rather than normalized LoadState.
[[nodiscard]] inline bool ShouldDeferMaterialDriver(bool tracked, BotState const& state)
{
    return tracked && (state.version != kStateVersion || state.phase != Phase::None);
}

// Starts (or restarts) a death_loop trip from `zone` to `hub`. The route leaves the bot's actual zone
// (no road table applies: straight to the hub). portal = take the portal leg at once (the Travel phase
// portals while mode is Portal and reissues <= MaxReissues). Check timers are kept.
inline void BeginEscape(BotState& s, Route const& hub, std::uint32_t zone, std::uint64_t nowMs, bool portal)
{
    s.phase = Phase::Travel;
    s.route = hub;
    s.route.from = zone;
    if (s.route.to == zone)
        s.route.crossing = false;
    s.trigger = Trigger::DeathLoop;
    s.fromZone = zone;
    s.startMs = nowMs;
    s.reissues = 0;
    s.stall = 0;
    s.mode = portal ? Mode::Portal : Mode::Unreachable;
    s.roadJoined = false;
    s.wp = 0;
}

// A same-zone death-loop trip is still travelling until it reaches the hub coordinates. Other trips
// keep the established destination-zone check that governs their portal fallback.
[[nodiscard]] inline bool PortalFallbackDestinationReached(BotState const& s, std::uint32_t zone, std::uint32_t map,
                                                            float x, float y)
{
    if (s.trigger == Trigger::DeathLoop && s.fromZone == s.route.to)
        return AtRouteHub(s.route, map, x, y);
    return zone == s.route.to;
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
inline bool gOutland = false;  // AutoWow.ZoneProgression.Outland
inline bool gNorthrend = false;  // AutoWow.ZoneProgression.Northrend
inline bool gNorthrend2 = false;  // AutoWow.ZoneProgression.Northrend2
inline Params gParams;
inline std::vector<Route> gRoutes;
}
inline bool Enabled() { return detail::gEnabled; }
inline bool OutlandEnabled() { return detail::gEnabled && detail::gOutland; }
inline bool NorthrendEnabled() { return detail::gEnabled && detail::gOutland && detail::gNorthrend; }
inline bool Northrend2Enabled() { return NorthrendEnabled() && detail::gNorthrend2; }

void LoadConfig();
// A graduation (travel or flight-path learning) is under way for this bot. Town runs wait for it.
bool Active(std::uint32_t guid);
// Read-only, non-inserting material-driver admission check. Invalid stored versions fail closed.
bool MaterialDriverAdmissionBlocked(std::uint32_t guid);
// AutoWow.Survival.HardEscape: drop any trip (and crossing chain) of this bot, no ledger line of its own
// (the caller emits zone_move reason hard_escape), and hold new ones for GiveUpCooldownMs. Flag off: no-op.
void CancelTrip(std::uint32_t guid, std::uint64_t nowMs);
}  // namespace AutoWowZoneProgression

#endif  // AUTOWOW_ZONE_PROGRESSION_POLICY_H
