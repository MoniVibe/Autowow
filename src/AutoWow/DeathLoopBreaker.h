/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_DEATH_LOOP_BREAKER_H
#define AUTOWOW_DEATH_LOOP_BREAKER_H

#include <cstddef>
#include <cstdint>
#include <string>

class Map;
class Player;

// Death-loop breaker (AutoWow.DeathLoop.Enable, default 0). A bot that keeps dying at the same spot
// (N deaths within WindowMs within RadiusYards), or is killed by something far above its level, is
// escalated: it takes the spirit-healer resurrection instead of the corpse run, the death cluster
// becomes a per-bot danger area for DangerCooldownMs (NewRpg grind/travel/quest picks skip it, the
// quest it was doing there is deferred with reason `death_loop_area`), and when its zone's level
// bracket starts well above the bot it is sent to a level-appropriate zone by the ordinary RPG
// flight-travel status (no teleport). Each escalation emits the ledger `death_loop` event.
//
// Everything below the runtime section is value-only and unit-tested. Positions are integer yards
// (floor), times are game-time ms; no floats, no RNG, fixed-size per-bot state, deterministic slot
// choice (lowest index on ties).
namespace AutoWowDeathLoop
{
inline constexpr std::size_t kMaxDeaths = 8;       // recent deaths remembered per bot (oldest dropped)
inline constexpr std::size_t kMaxDangerAreas = 4;  // concurrent danger areas per bot
inline constexpr std::size_t kMaxBots = 2048;      // hard cap; beyond it new bots are not tracked

struct Params
{
    std::uint32_t deaths = 3;                   // AutoWow.DeathLoop.Deaths (0 = cluster rule off)
    std::uint64_t windowMs = 1800000;           // AutoWow.DeathLoop.WindowMs
    std::uint32_t radiusYards = 60;             // AutoWow.DeathLoop.RadiusYards
    std::uint32_t killerLevelGap = 10;          // AutoWow.DeathLoop.KillerLevelGap (0 = rule off)
    std::uint64_t dangerCooldownMs = 3600000;   // AutoWow.DeathLoop.DangerCooldownMs
    std::uint32_t relocateLevelMargin = 5;      // AutoWow.DeathLoop.RelocateLevelMargin
    std::uint32_t escapePortalDeaths = 3;       // AutoWow.DeathLoop.EscapePortalDeaths (0 = never at once)
};

struct DeathSample
{
    std::uint64_t ms = 0;
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct DangerArea
{
    std::uint64_t untilMs = 0;  // 0 = free slot
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t radius = 0;
};

// AutoWow.Survival.HardEscape (3) episode: the bot has been in the hard condition since sinceMs without
// moving moveYards from the anchor (alive samples only).
struct HardState
{
    bool active = false;
    std::uint64_t sinceMs = 0;
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool hearthTried = false;
    std::uint64_t retryAtMs = 0;        // after a hearth attempt: the portal no earlier than this
    std::uint64_t cooldownUntilMs = 0;  // after a portal
    std::uint64_t nextCheckMs = 0;      // runtime throttle
};

// Per-bot state. deaths[0..deathCount) is oldest first.
struct BotState
{
    DeathSample deaths[kMaxDeaths] = {};
    std::uint32_t deathCount = 0;
    DangerArea danger[kMaxDangerAreas] = {};
    std::uint32_t pendingKillerLevel = 0;  // noted by the kill hook, consumed by the death hook
    bool spiritHealer = false;             // escalated death: spirit healer instead of corpse run
    bool relocate = false;                 // one relocation attempt pending
    std::uint32_t deferQuest = 0;          // Oracle-managed quest to defer at the next Oracle pass
    bool restPending = false;              // V2: spirit-healer res taken, full rest before any pull
    std::uint32_t lastKillerLevel = 0;     // HardEscape: killer level of the latest death
    HardState hard;                        // HardEscape (3)
};

// Wire-stable ledger reason names; append only.
enum class Trigger : std::uint8_t
{
    None = 0,
    Cluster = 1,
    LevelGap = 2,
    ClusterAndLevelGap = 3
};

inline constexpr char const* TriggerName(Trigger t)
{
    switch (t)
    {
        case Trigger::None: return "none";
        case Trigger::Cluster: return "cluster";
        case Trigger::LevelGap: return "level_gap";
        case Trigger::ClusterAndLevelGap: return "cluster_level_gap";
    }
    return "none";
}

struct Decision
{
    Trigger trigger = Trigger::None;
    bool relocate = false;
    [[nodiscard]] bool Escalate() const { return trigger != Trigger::None; }
};

inline bool Within(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by, std::uint32_t radius)
{
    std::int64_t const dx = std::int64_t(ax) - bx;
    std::int64_t const dy = std::int64_t(ay) - by;
    return dx * dx + dy * dy <= std::int64_t(radius) * radius;
}

// Forgets deaths outside the window (or from the future: a clock step back), appends `death`
// (dropping the oldest when full) and returns how many remembered deaths, including this one, lie on
// the same map within `radius`. `center` is their integer centroid (truncating division).
inline std::uint32_t RecordDeath(BotState& s, DeathSample const& death, std::uint64_t windowMs, std::uint32_t radius,
                                 DeathSample& center)
{
    std::uint32_t kept = 0;
    for (std::uint32_t k = 0; k < s.deathCount; ++k)
    {
        DeathSample const& d = s.deaths[k];
        if (d.ms <= death.ms && death.ms - d.ms <= windowMs)
            s.deaths[kept++] = d;
    }
    s.deathCount = kept;
    if (s.deathCount == kMaxDeaths)
    {
        for (std::size_t k = 1; k < kMaxDeaths; ++k)
            s.deaths[k - 1] = s.deaths[k];
        --s.deathCount;
    }
    s.deaths[s.deathCount++] = death;

    std::uint32_t n = 0;
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (std::uint32_t k = 0; k < s.deathCount; ++k)
    {
        DeathSample const& d = s.deaths[k];
        if (d.map != death.map || !Within(d.x, d.y, death.x, death.y, radius))
            continue;
        ++n;
        sx += d.x;
        sy += d.y;
    }
    center = death;
    center.x = static_cast<std::int32_t>(sx / n);
    center.y = static_cast<std::int32_t>(sy / n);
    return n;
}

// The zone's level bracket starts more than `margin` above the bot (zoneMinLevel 0 = unknown: never).
inline bool Overshoot(std::uint32_t zoneMinLevel, std::uint32_t botLevel, std::uint32_t margin)
{
    return zoneMinLevel && zoneMinLevel > botLevel + margin;
}

// Deaths remembered within windowMs of nowMs (any spot; a death from the future is not counted).
inline std::uint32_t RecentDeaths(BotState const& s, std::uint64_t nowMs, std::uint64_t windowMs)
{
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < s.deathCount; ++k)
        if (s.deaths[k].ms <= nowMs && nowMs - s.deaths[k].ms <= windowMs)
            ++n;
    return n;
}

// AutoWow.DeathLoop.EscapeViaZoneProgression: the escape trip takes the portal leg at once (no walk
// through the zone that keeps killing the bot) after portalDeaths recent deaths.
inline bool EscapePortalNow(std::uint32_t recentDeaths, std::uint32_t portalDeaths)
{
    return portalDeaths && recentDeaths >= portalDeaths;
}

// zoneMinLevel = low end of the curated level bracket of the zone the bot died in (0 = unknown).
inline Decision Evaluate(Params const& p, std::uint32_t clusterDeaths, std::uint32_t botLevel,
                         std::uint32_t killerLevel, std::uint32_t zoneMinLevel)
{
    Decision out;
    bool const cluster = p.deaths && clusterDeaths >= p.deaths;
    bool const levelGap = p.killerLevelGap && killerLevel >= botLevel + p.killerLevelGap;
    if (cluster && levelGap)
        out.trigger = Trigger::ClusterAndLevelGap;
    else if (cluster)
        out.trigger = Trigger::Cluster;
    else if (levelGap)
        out.trigger = Trigger::LevelGap;
    out.relocate = out.Escalate() && Overshoot(zoneMinLevel, botLevel, p.relocateLevelMargin);
    return out;
}

// Marks a danger area until untilMs. Reuses the slot of an area overlapping the center (same map,
// center within that area's radius), else the first free or expired slot, else the one expiring
// first (lowest index on ties). True when a live overlapping area was reused: a repeat escalation in
// the same area (AutoWow.DeathLoop.V2 relocates then even when the zone bracket fits).
inline bool MarkDanger(BotState& s, DeathSample const& center, std::uint32_t radius, std::uint64_t nowMs,
                       std::uint64_t untilMs)
{
    std::size_t slot = kMaxDangerAreas;
    std::size_t free = kMaxDangerAreas;
    std::size_t soonest = 0;
    for (std::size_t k = 0; k < kMaxDangerAreas; ++k)
    {
        DangerArea const& a = s.danger[k];
        bool const live = a.untilMs > nowMs;
        if (live && a.map == center.map && Within(a.x, a.y, center.x, center.y, a.radius))
        {
            slot = k;
            break;
        }
        if (!live && free == kMaxDangerAreas)
            free = k;
        if (a.untilMs < s.danger[soonest].untilMs)
            soonest = k;
    }
    bool const repeat = slot != kMaxDangerAreas;
    if (slot == kMaxDangerAreas)
        slot = free != kMaxDangerAreas ? free : soonest;
    s.danger[slot] = DangerArea{untilMs, center.map, center.x, center.y, radius};
    return repeat;
}

inline bool IsDangerous(BotState const& s, std::uint32_t map, std::int32_t x, std::int32_t y, std::uint64_t nowMs)
{
    for (DangerArea const& a : s.danger)
        if (a.untilMs > nowMs && a.map == map && Within(a.x, a.y, x, y, a.radius))
            return true;
    return false;
}

// Trailing fields of the ledger `death_loop` line (AutoWowQuestLedger.h documents them).
inline std::string LedgerFields(std::uint32_t clusterDeaths, std::uint32_t killerLevel, std::uint32_t zoneMinLevel,
                                Decision const& d, DeathSample const& center, Params const& p)
{
    std::string out;
    auto field = [&out](char const* name, std::int64_t value)
    {
        out += ",\"";
        out += name;
        out += "\":";
        out += std::to_string(value);
    };
    field("deaths", clusterDeaths);
    field("klvl", killerLevel);
    field("zlow", zoneMinLevel);
    field("relocate", d.relocate ? 1 : 0);
    field("dx", center.x);
    field("dy", center.y);
    field("dr", p.radiusYards);
    field("cool_ms", static_cast<std::int64_t>(p.dangerCooldownMs));
    return out;
}

// ---- AutoWow.Survival.HardEscape (default 0; needs Enable) -------------------------------------------
// soak-s22-full-r1: cohort hunter Taelorin (L18) died 183 times in 55 min in Burning Steppes (bracket
// 51-60). The V2 escape from Westfall took the lowest-band hub, Loch Modan, across the continent; the walk
// beelined through Redridge / Burning Steppes; every later relocation found no flight and never moved.
struct HardParams
{
    std::uint32_t walkZoneMargin = 5;   // AutoWow.Survival.HardEscape.WalkZoneMargin: zone too high to cross
    std::uint32_t zoneGap = 10;         // AutoWow.Survival.HardEscape.ZoneGap: stuck in a zone this far above
    std::uint32_t deaths = 4;           // AutoWow.Survival.HardEscape.Deaths (0 = death rule off)
    std::uint32_t killerGap = 10;       // AutoWow.Survival.HardEscape.KillerGap
    std::uint64_t stuckMs = 120000;     // AutoWow.Survival.HardEscape.StuckMs: no movement for this long
    std::uint32_t moveYards = 100;      // AutoWow.Survival.HardEscape.MoveYards: farther = it is moving
    std::uint64_t cooldownMs = 900000;  // AutoWow.Survival.HardEscape.CooldownMs after a portal
};
inline constexpr std::uint64_t kHardCheckMs = 5000;         // runtime sampling stride
inline constexpr std::uint64_t kHardHearthRetryMs = 30000;  // a hearth that did not move the bot -> portal

// The hard condition: the zone's bracket starts more than zoneGap above the bot, or deaths recent deaths
// with the latest killer killerGap or more above it.
inline bool HardCondition(HardParams const& h, std::uint32_t level, std::uint32_t zoneLow, std::uint32_t recentDeaths,
                          std::uint32_t killerLevel)
{
    return Overshoot(zoneLow, level, h.zoneGap) ||
           (h.deaths && recentDeaths >= h.deaths && h.killerGap && killerLevel >= level + h.killerGap);
}

enum class HardAction : std::uint8_t
{
    None = 0,
    Hearth = 1,
    Portal = 2
};

inline constexpr char const* HardActionName(HardAction a)
{
    switch (a)
    {
        case HardAction::None: return "none";
        case HardAction::Hearth: return "hearth";
        case HardAction::Portal: return "portal";
    }
    return "none";
}

// One sample. Not hard: the episode ends. Dead: the clock runs, the anchor holds (ghost walks and the
// graveyard do not count as movement). Alive and moved moveYards off the anchor (or another map): a new
// episode. stuckMs in the hard condition without moving: the hearthstone once when usable (a bound, ready
// hearth in a level-fitting zone, out of combat), then after kHardHearthRetryMs still here: the portal,
// which ends the episode and starts cooldownMs.
inline HardAction HardStep(HardParams const& h, HardState& s, std::uint64_t nowMs, bool hard, bool alive,
                           std::uint32_t map, std::int32_t x, std::int32_t y, bool hearthUsable)
{
    if (!hard)
    {
        s.active = false;
        return HardAction::None;
    }
    if (nowMs < s.cooldownUntilMs || !alive)
        return HardAction::None;
    if (!s.active || s.map != map || !Within(s.x, s.y, x, y, h.moveYards))
    {
        s.active = true;
        s.sinceMs = nowMs;
        s.map = map;
        s.x = x;
        s.y = y;
        s.hearthTried = false;
        s.retryAtMs = 0;
        return HardAction::None;
    }
    if (nowMs < s.sinceMs || nowMs - s.sinceMs < h.stuckMs || nowMs < s.retryAtMs)
        return HardAction::None;
    if (hearthUsable && !s.hearthTried)
    {
        s.hearthTried = true;
        s.retryAtMs = nowMs + kHardHearthRetryMs;
        return HardAction::Hearth;
    }
    s.active = false;
    s.cooldownUntilMs = nowMs + h.cooldownMs;
    return HardAction::Portal;
}

// Portal target: the bind point when its zone fits the bot (bracket low <= level + walkZoneMargin),
// else the faction capital of the bot's continent (world DB game_tele). team: 1 alliance, 2 horde.
struct Place
{
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t zone = 0;
};

inline Place CapitalFor(std::uint32_t team, std::uint32_t map)
{
    if (team == 1)
        return map == 1 ? Place{1, 9949, 2284, 1341, 1657}    // Darnassus
                        : Place{0, -8833, 628, 94, 1519};     // Stormwind
    return map == 0 ? Place{0, 1584, 240, -52, 1497}          // Undercity
                    : Place{1, 1629, -4373, 31, 1637};        // Orgrimmar
}

inline Place HardPortalTarget(std::uint32_t team, std::uint32_t map, Place const& home, std::uint32_t homeZoneLow,
                              std::uint32_t level, std::uint32_t margin)
{
    return home.zone && !Overshoot(homeZoneLow, level, margin) ? home : CapitalFor(team, map);
}

// ---- runtime (DeathLoopBreaker.cpp) -------------------------------------------------------------
// Per-bot state is mutex-guarded (deaths and bot AI updates run on map threads). Every query below
// returns false/0 when the flag is off.
namespace detail
{
inline bool gEnabled = false;
inline bool gEscape = false;
inline bool gV2 = false;
inline bool gHard = false;
inline Params gParams;
inline HardParams gHardParams;
}
inline bool Enabled() { return detail::gEnabled; }
// AutoWow.DeathLoop.EscapeViaZoneProgression (default 0; needs Enable and AutoWow.ZoneProgression.Enable):
// the relocation (and an over-level zone, even without a death) becomes a zone-progression trip to the
// nearest level-appropriate hub (ZoneProgression PickEscapeRoute), ledger zone_move reason death_loop.
inline bool EscapeEnabled() { return detail::gEnabled && detail::gEscape; }

// AutoWow.DeathLoop.V2 (default 0; needs Enable; FAIR_FIGHT_DEATHS_S14 cause 3):
//   (a) GrindTargetValue skips proactive targets standing in the bot's live danger areas (self-defence
//       unchanged; before, only travel/quest POIs were filtered);
//   (b) the second escalation in the same live area relocates even when the zone bracket fits; with
//       EscapeViaZoneProgression the hub is the lowest-band level fit (ZoneProgression PickLowEscapeRoute);
//   (c) after the spirit-healer res of an escalated death the bot rests fully (AutoWowRestGate thresholds
//       and no Resurrection Sickness) before any proactive pull, whether or not the rest gate flag is on.
inline bool V2Enabled() { return detail::gEnabled && detail::gV2; }

// AutoWow.Survival.HardEscape (default 0; needs Enable; conf/playerbots.conf.dist documents it):
//   (1) the death-loop escape hub is the nearest one whose straight line from the bot crosses no zone
//       bracketed above the bot (ZoneProgression PickSafeEscapeRoute) - not the lowest band anywhere;
//   (2) Walking V2 chunks never end in (or pass through) such a zone other than the bot's own;
//   (3) a bot stuck far over its level (or dying there) that does not move for StuckMs hearths home, else
//       is portalled to its bind point / a capital (ledger zone_move reason hard_escape).
inline bool HardEscapeEnabled() { return detail::gEnabled && detail::gHard; }

// Zone (AreaTable parent) at (x, y) on `map` from the terrain grid's area map; 0 = none / off map.
std::uint32_t ZoneAt(Map* map, float x, float y);
// Low end of the zone's curated bracket (TravelMgr), else its AreaTable level; 0 = unknown.
std::uint32_t ZoneMinLevel(std::uint32_t zoneId);

void LoadConfig();
void AddScripts();

bool WantsSpiritHealer(std::uint32_t botGuid);
bool IsDangerous(std::uint32_t botGuid, std::uint32_t map, float x, float y);
// Consumes the pending relocation (one attempt per escalation).
bool TakeRelocation(std::uint32_t botGuid);
// Deaths of this bot within WindowMs of now (0 with the flag off). Escape portal rule input.
std::uint32_t RecentDeaths(std::uint32_t botGuid);
// Consumes the pending Oracle deferral when it names questId.
bool TakeQuestDeferral(std::uint32_t botGuid, std::uint32_t questId);
// V2 (c): forced rest pending after a spirit-healer res; cleared by the rest gate once rested.
bool RestPending(std::uint32_t botGuid);
void ClearRestPending(std::uint32_t botGuid);
// Non-Oracle deferral: lowPriorityQuest + ledger `deferred` reason `death_loop_area`. Bot thread only.
void DeferQuest(Player* bot, std::uint32_t questId);
}  // namespace AutoWowDeathLoop

#endif  // AUTOWOW_DEATH_LOOP_BREAKER_H
