/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_SURVIVAL_RECOVERY_H
#define AUTOWOW_SURVIVAL_RECOVERY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "TravelIntentPolicy.h"
#include "WalkingV2Policy.h"

class Corpse;
class Player;
class PlayerbotAI;

// soak-s16-full-r1 (94 cohort deaths / 25 min): 36 were repeat deaths within 4 min of the previous one,
// 25 of them within 41 yd of the previous death spot - the ghost reclaimed its body at the corpse, among
// the pack that killed it, at half health. Separately 11 bots froze: fixed x/y, every walk (errand and
// quest) ended intent_replan_exhausted - no navmesh route out of where they stood.
//
// Value-only above each runtime section and unit-tested (SurvivalRecoveryTest). Positions are integer
// yards (floor), times game-time ms; no floats, no RNG; ties break on the lowest candidate index.

// ---- AutoWow.Survival.SafeRevive (default 0) --------------------------------------------------------
// A ghost resurrects where it stands, anywhere within CORPSE_RECLAIM_RADIUS (39 yd, 3D) of its corpse
// (WorldSession::HandleReclaimCorpseOpcode). With the flag on, a solo autonomous bot within kPlanYards of
// its corpse picks the reclaim spot: the corpse and two rings of 8 bearings (17 / 34 yd), each ground-
// snapped and mmap-pathed; the reachable spot with the least aggro weight of idle hostiles wins, ties to
// the one nearest the graveyard and farthest from the killer. No zero-threat spot and SpiritDeaths
// deaths within WindowMs (this one included): the spirit healer instead. After a corpse revive the bot
// first walks kRetreatYards off the spot (same scoring), then holds the RPG in REST below the rest-gate
// hp/mana thresholds; any revive (corpse or spirit healer) holds proactive pulls until rested, through
// the rest gate's forced rest (as AutoWow.DeathLoop.V2), whether or not AutoWow.Survival.RestGate is on.
namespace AutoWowSafeRevive
{
using TravelIntentPolicy::DistanceYards;
using TravelIntentPolicy::Point;
using WalkingV2Policy::Mob;

inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::int32_t kReachYards = 34;         // CORPSE_RECLAIM_RADIUS (39) - 5, the corpse-run margin
inline constexpr std::int32_t kInnerRingYards = 17;
inline constexpr std::int32_t kRetreatYards = 45;       // post-revive walk off the kill spot (30-60 yd band)
inline constexpr std::uint32_t kPlanYards = 60;         // the ghost plans once this close (its grids are loaded)
inline constexpr std::uint32_t kAggroMarginYards = WalkingV2Policy::kAggroMarginYards;
inline constexpr std::uint32_t kAtSpotYards = 3;        // reached a chosen spot
inline constexpr std::uint64_t kApproachTimeoutMs = 90000;  // spot not reached: legacy corpse run for this death
inline constexpr std::uint64_t kRetreatTimeoutMs = 30000;

struct Params
{
    std::uint32_t spiritDeaths = 2;  // AutoWow.Survival.SafeRevive.SpiritDeaths (0 = never the spirit healer)
    std::uint64_t windowMs = 600000; // AutoWow.Survival.SafeRevive.WindowMs
};

// E, NE, N, NW, W, SW, S, SE (x1024).
inline constexpr std::array<WalkingV2Policy::Rotation, 8> kBearings{
    {{1024, 0}, {724, 724}, {0, 1024}, {-724, 724}, {-1024, 0}, {-724, -724}, {0, -1024}, {724, -724}}};
inline constexpr std::size_t kReviveCandidates = 1 + 2 * kBearings.size();
inline constexpr std::size_t kRetreatCandidates = 1 + kBearings.size();

[[nodiscard]] inline Point Offset(Point p, std::size_t bearing, std::int32_t yards)
{
    WalkingV2Policy::Rotation const b = kBearings[bearing % kBearings.size()];
    p.x += b.cos1024 * yards / 1024;
    p.y += b.sin1024 * yards / 1024;
    return p;
}

// k = 0: the corpse itself; 1..8: inner ring; 9..16: outer ring (bearing order).
[[nodiscard]] inline Point ReviveCandidate(Point const& corpse, std::size_t k)
{
    if (k == 0 || k >= kReviveCandidates)
        return corpse;
    return Offset(corpse, (k - 1) % kBearings.size(), k <= kBearings.size() ? kInnerRingYards : kReachYards);
}

// k = 0: stay; 1..8: kRetreatYards along each bearing.
[[nodiscard]] inline Point RetreatCandidate(Point const& from, std::size_t k)
{
    return k == 0 || k >= kRetreatCandidates ? from : Offset(from, k - 1, kRetreatYards);
}

// Aggro weight of one idle hostile: elite 6, at/above the bot's level 3, lower 1 (it still aggroes
// inside its own, level-scaled, radius).
[[nodiscard]] inline std::uint32_t MobWeight(Mob const& m, std::uint32_t botLevel)
{
    return m.elite ? 6 : m.level >= botLevel ? 3 : 1;
}

// Sum of the weights of the mobs whose aggro radius (+kAggroMarginYards) covers p, or that stand within
// minRadius of it (SafeRevive.V2: kThreatYardsV2; 0 = aggro radius only).
[[nodiscard]] inline std::uint32_t SpotThreat(Point const& p, std::vector<Mob> const& mobs, std::uint32_t botLevel,
                                              std::uint32_t minRadius = 0)
{
    std::uint32_t threat = 0;
    for (Mob const& m : mobs)
    {
        std::int64_t const dx = std::int64_t(m.x) - p.x;
        std::int64_t const dy = std::int64_t(m.y) - p.y;
        std::int64_t const r = std::max<std::int64_t>(std::int64_t(m.aggroYards) + kAggroMarginYards, minRadius);
        if (dx * dx + dy * dy <= r * r)
            threat += MobWeight(m, botLevel);
    }
    return threat;
}

struct Anchors
{
    std::optional<Point> grave;   // nearer is better
    std::optional<Point> killer;  // farther is better
    std::optional<Point> away;    // farther is better (the death spot, for the retreat)
};

// Tie-break after threat, lower is better (2D yards; an anchor on another map contributes nothing).
[[nodiscard]] inline std::int64_t Preference(Point const& p, Anchors const& a)
{
    auto yards = [&p](std::optional<Point> const& q) -> std::int64_t
    {
        if (!q)
            return 0;
        std::uint32_t const d = DistanceYards(p, *q);
        return d == 0xFFFFFFFFu ? 0 : d;
    };
    return yards(a.grave) - yards(a.killer) - yards(a.away);
}

struct Spot
{
    Point p;
    bool reachable = false;  // ground-snapped and pathed (the caller's world check)
};

inline constexpr std::size_t kNoSpot = static_cast<std::size_t>(-1);

struct Pick
{
    std::size_t index = kNoSpot;
    std::uint32_t threat = 0;
};

// Lowest (threat, preference, index) among the reachable spots.
[[nodiscard]] inline Pick PickSpot(std::vector<Spot> const& spots, std::vector<Mob> const& mobs,
                                   std::uint32_t botLevel, Anchors const& a, std::uint32_t minRadius = 0)
{
    Pick best;
    std::int64_t bestPref = 0;
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        if (!spots[i].reachable)
            continue;
        std::uint32_t const threat = SpotThreat(spots[i].p, mobs, botLevel, minRadius);
        std::int64_t const pref = Preference(spots[i].p, a);
        if (best.index == kNoSpot || threat < best.threat || (threat == best.threat && pref < bestPref))
        {
            best = {i, threat};
            bestPref = pref;
        }
    }
    return best;
}

// Wire-stable log names; append only.
enum class Plan : std::uint8_t
{
    None = 0,          // legacy corpse run for this death
    ReviveAt = 1,
    SpiritHealer = 2
};

inline constexpr char const* PlanName(Plan p)
{
    switch (p)
    {
        case Plan::None: return "none";
        case Plan::ReviveAt: return "revive_at";
        case Plan::SpiritHealer: return "spirit_healer";
    }
    return "none";
}

// recentDeaths: deaths within WindowMs, this one included.
[[nodiscard]] inline Plan Decide(Pick const& best, std::uint32_t recentDeaths, Params const& p)
{
    bool const found = best.index != kNoSpot;
    if (found && best.threat == 0)
        return Plan::ReviveAt;
    if (p.spiritDeaths && recentDeaths >= p.spiritDeaths)
        return Plan::SpiritHealer;
    return found ? Plan::ReviveAt : Plan::None;
}

// ---- AutoWow.Survival.SafeRevive.V2 (default 0; needs SafeRevive) ------------------------------------
// soak-s21-full-r1 (LOWMOB_DEATHS_S21 cause 3): 76 plans for 177 deaths; all 44 revive_at plans at threat
// 0, 13 of them at deaths 3-4 in the window (past SpiritDeaths 2) - a zero-threat spot overrode the spirit
// healer - and 57% of repeat deaths came within 4 min: the threat saw only idle hostiles in aggro range.
// V2: SpiritDeaths deaths in the window take the spirit healer whatever the spot threat, and after that
// res one relocation (the death-loop one: zone-progression escape hub with EscapeViaZoneProgression, else
// a flight); spot threat counts every hostile within kThreatYardsV2 - idle, in combat, or dead with its
// respawn due within kRespawnSoonMs (at its home position).
inline constexpr std::uint32_t kThreatYardsV2 = 60;
inline constexpr std::uint64_t kRespawnSoonMs = 120000;

[[nodiscard]] inline Plan DecideV2(Pick const& best, std::uint32_t recentDeaths, Params const& p)
{
    if (p.spiritDeaths && recentDeaths >= p.spiritDeaths)
        return Plan::SpiritHealer;
    return best.index != kNoSpot ? Plan::ReviveAt : Plan::None;
}

// A dead creature's spawn counts as a threat when it respawns within kRespawnSoonMs (times in seconds, as
// the core keeps them; respawnAt 0 = no respawn scheduled).
[[nodiscard]] inline bool RespawnSoon(std::int64_t respawnAtSec, std::int64_t nowSec)
{
    return respawnAtSec > 0 && respawnAtSec - nowSec <= static_cast<std::int64_t>(kRespawnSoonMs / 1000);
}

// ---- runtime (SurvivalRecovery.cpp) ------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline bool gV2 = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
inline bool V2Enabled() { return detail::gEnabled && detail::gV2; }
// V2: non-consuming inspection and consuming acceptance of the relocation owed after a forced
// spirit-healer resurrection. Both are false with V2 off.
bool RelocationPending(std::uint32_t botGuid);
bool TakeRelocation(std::uint32_t botGuid);

// World-space target of this death's plan (plan != None).
struct Target
{
    Plan plan = Plan::None;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Ghost: the plan for this death, made on the first call within kPlanYards of the corpse. nullopt =
// legacy corpse run (flag off, not eligible, too far to plan, a death-loop escalation, or abandoned).
std::optional<Target> PlanFor(Player* bot, Corpse* corpse);
// The ghost could not walk to its spot: legacy corpse run for the rest of this death.
void Abandon(std::uint32_t botGuid);

// ReviveFromCorpseAction gate: Legacy = no plan; Hold = not on the chosen spot yet; Reclaim = on it.
enum class Gate : std::uint8_t
{
    Legacy,
    Hold,
    Reclaim,
    Spirit
};
Gate ReclaimGate(Player* bot, Corpse* corpse);

// Alive, out of combat, RPG status update: Walk = retreat toward (x, y, z); Rest = hold the RPG in
// REST (below the rest-gate hp/mana thresholds); None = nothing pending.
enum class Step : std::uint8_t
{
    None,
    Walk,
    Rest
};
Step RecoveryStep(PlayerbotAI* botAI, float& x, float& y, float& z);
void EndRetreat(std::uint32_t botGuid);

// Forced rest after a revive (AutoWowRestGate reads and clears it). False with the flag off.
bool RestPending(std::uint32_t botGuid);
void ClearRestPending(std::uint32_t botGuid);
}  // namespace AutoWowSafeRevive

// ---- AutoWow.Survival.RestSafe (default 0) ----------------------------------------------------------
// S62-S65 deaths: fights started below 70% hp died 45% of the time (10% above), and 237 of those 279 deaths
// began < 60 s after the previous fight - the rest gate holds only self-initiated pulls, so the bot rested
// (or walked on) where it stood while patrols, respawns and hostile players came to it; only 17% of fatal
// fights had any rest before them. Two camping pairs caused 30 of 39 S65 PvP deaths (SafeRevive scanned
// creatures only).
//   (1) A rest-gate eligible bot (solo independent, open world, out of combat) below the rest-gate hp / mana
//       thresholds plans once per rest episode: when a hostile's aggro radius (+kAggroMarginYards; SafeRevive
//       V2 scan: idle, in combat, respawning soon) or a hostile player's kPlayerThreatYards covers it, it
//       walks to the reachable candidate (8 bearings at kInnerYards, then kOuterYards) with the least threat
//       (less than here), within kMoveTimeoutMs. Then the RPG holds in REST (the food strategy eats) until
//       rested. Combat or being rested ends the episode.
//   (2) SafeRevive's corpse plan counts hostile players too. After a death to a player (killer or its pet,
//       or a hostile player within kPvpNearYards at death) the ghost reclaims only on a zero-threat spot,
//       else takes the spirit healer, and the death spot becomes a death-loop danger area (PvpDangerYards
//       for PvpDangerMs; needs AutoWow.DeathLoop.Enable) that the RPG picks avoid. Needs SafeRevive.
namespace AutoWowRestSafe
{
using AutoWowSafeRevive::Pick;
using AutoWowSafeRevive::Plan;
using TravelIntentPolicy::Point;
using WalkingV2Policy::Mob;

inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::int32_t kInnerYards = 30;
inline constexpr std::int32_t kOuterYards = 60;  // farthest rest move
inline constexpr std::size_t kCandidates = 1 + 2 * AutoWowSafeRevive::kBearings.size();
inline constexpr std::uint32_t kPlayerThreatYards = 40;  // + kAggroMarginYards
inline constexpr std::uint32_t kPvpNearYards = 40;
inline constexpr std::uint64_t kMoveTimeoutMs = 30000;

struct Params
{
    std::uint32_t pvpDangerYards = 80;      // AutoWow.Survival.RestSafe.PvpDangerYards (0 = no danger area)
    std::uint64_t pvpDangerMs = 1800000;    // AutoWow.Survival.RestSafe.PvpDangerMs
};

// k = 0: stay; 1..8: kInnerYards along each bearing; 9..16: kOuterYards.
[[nodiscard]] inline Point Candidate(Point const& from, std::size_t k)
{
    std::size_t const n = AutoWowSafeRevive::kBearings.size();
    if (k == 0 || k >= kCandidates)
        return from;
    return AutoWowSafeRevive::Offset(from, (k - 1) % n, k <= n ? kInnerYards : kOuterYards);
}

// A hostile player as a threat: the heaviest weight (elite) over kPlayerThreatYards.
[[nodiscard]] inline Mob PlayerThreat(std::int32_t x, std::int32_t y, std::uint32_t level)
{
    Mob m;
    m.x = x;
    m.y = y;
    m.level = level;
    m.elite = true;
    m.aggroYards = kPlayerThreatYards;
    return m;
}

// Candidates worth trying, best first: less threat than here, by (threat, index). Empty when here is clear.
[[nodiscard]] inline std::vector<std::size_t> MoveOrder(Point const& here, std::vector<Mob> const& mobs,
                                                        std::uint32_t botLevel)
{
    std::uint32_t const hereThreat = AutoWowSafeRevive::SpotThreat(here, mobs, botLevel);
    std::vector<std::pair<std::uint32_t, std::size_t>> better;
    for (std::size_t k = 1; hereThreat && k < kCandidates; ++k)
    {
        std::uint32_t const t = AutoWowSafeRevive::SpotThreat(Candidate(here, k), mobs, botLevel);
        if (t < hereThreat)
            better.push_back({t, k});
    }
    std::sort(better.begin(), better.end());
    std::vector<std::size_t> out;
    for (auto const& b : better)
        out.push_back(b.second);
    return out;
}

// Corpse plan after a PvP death: reclaim only on a zero-threat spot, else the spirit healer.
[[nodiscard]] inline Plan PvpPlan(Plan base, Pick const& best)
{
    if (base == Plan::SpiritHealer)
        return base;
    return best.index != AutoWowSafeRevive::kNoSpot && best.threat == 0 ? Plan::ReviveAt : Plan::SpiritHealer;
}

// ---- runtime (SurvivalRecovery.cpp) ------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }

// NewRpgStatusUpdateAction: Walk = move toward (x, y, z); Rest = hold the RPG in REST; None = nothing
// (not below the thresholds, in combat, not eligible, or SafeRevive's post-revive retreat runs first).
enum class Step : std::uint8_t
{
    None,
    Walk,
    Rest
};
Step RestStep(PlayerbotAI* botAI, float& x, float& y, float& z);
// The move could not be issued: rest where it stands.
void EndMove(std::uint32_t botGuid);
}  // namespace AutoWowRestSafe

// ---- AutoWow.Survival.Unstick (default 0) -----------------------------------------------------------
// Observed on every MoveFarTo tick (the bot has an active travel / quest goal). Frozen = the bot stayed
// within RadiusYards for FrozenMs (the clock restarts when the goal was not observed for kMaxGapMs:
// combat, rest, another status), or ReplanExhausted intent_replan_exhausted give-ups within WindowMs.
// Then: the hearthstone when it is in the bags, off cooldown and bound kHearthMinYards away; else, when
// no mmap probe (8 bearings x kProbeYards) gets MinEscapeYards away - a navmesh hole - a portal to the
// nearest graveyard (ledger `contaminated` reason unstick_portal); else nothing. Every decision logs
// "[Unstick]" with the bot's coordinates (navmesh-hole map) and starts CooldownMs.
namespace AutoWowUnstick
{
using TravelIntentPolicy::DistanceYards;
using TravelIntentPolicy::Point;

inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::uint64_t kMaxGapMs = 60000;
inline constexpr std::uint32_t kHearthMinYards = 100;
inline constexpr std::int32_t kProbeYards = 30;
inline constexpr std::size_t kProbes = 8;
inline constexpr std::uint64_t kHearthCastMs = 20000;  // cast + teleport window held after the cast starts

struct Params
{
    std::uint32_t radiusYards = 10;           // AutoWow.Survival.Unstick.RadiusYards
    std::uint64_t frozenMs = 300000;          // AutoWow.Survival.Unstick.FrozenMs (0 = rule off)
    std::uint32_t replanExhausted = 5;        // AutoWow.Survival.Unstick.ReplanExhausted (0 = rule off)
    std::uint64_t windowMs = 900000;          // AutoWow.Survival.Unstick.WindowMs
    std::uint64_t cooldownMs = 600000;        // AutoWow.Survival.Unstick.CooldownMs
    std::uint32_t minEscapeYards = 8;         // AutoWow.Survival.Unstick.MinEscapeYards
};

struct BotState
{
    std::uint8_t version = kPolicyVersion;
    bool anchored = false;
    Point anchor;
    std::uint64_t anchorMs = 0;
    std::uint64_t lastSeenMs = 0;
    std::uint32_t exhausted = 0;
    std::uint64_t exhaustedSinceMs = 0;
    std::uint64_t cooldownUntilMs = 0;
    std::uint64_t hearthMs = 0;  // runtime: our hearthstone cast started (0 = none)
};

// Wire-stable log names; append only.
enum class Trigger : std::uint8_t
{
    None = 0,
    Frozen = 1,
    ReplanExhausted = 2
};

inline constexpr char const* TriggerName(Trigger t)
{
    switch (t)
    {
        case Trigger::None: return "none";
        case Trigger::Frozen: return "frozen";
        case Trigger::ReplanExhausted: return "replan_exhausted";
    }
    return "none";
}

enum class Action : std::uint8_t
{
    None = 0,
    Hearth = 1,
    Portal = 2
};

inline constexpr char const* ActionName(Action a)
{
    switch (a)
    {
        case Action::None: return "none";
        case Action::Hearth: return "hearth";
        case Action::Portal: return "portal";
    }
    return "none";
}

// One intent_replan_exhausted give-up; the count restarts when the first one is older than WindowMs.
inline void NoteReplanExhausted(Params const& p, BotState& s, std::uint64_t nowMs)
{
    if (!s.exhausted || nowMs < s.exhaustedSinceMs || nowMs - s.exhaustedSinceMs > p.windowMs)
    {
        s.exhausted = 0;
        s.exhaustedSinceMs = nowMs;
    }
    ++s.exhausted;
}

// One goal tick at `here`.
[[nodiscard]] inline Trigger Observe(Params const& p, BotState& s, Point const& here, std::uint64_t nowMs)
{
    if (s.anchored && (nowMs < s.lastSeenMs || nowMs - s.lastSeenMs > kMaxGapMs))
        s.anchored = false;  // goal paused (or a clock step back): the still clock restarts
    s.lastSeenMs = nowMs;
    if (!s.anchored || DistanceYards(s.anchor, here) > p.radiusYards)
    {
        s.anchored = true;
        s.anchor = here;
        s.anchorMs = nowMs;
    }
    if (nowMs < s.cooldownUntilMs)
        return Trigger::None;
    if (p.replanExhausted && s.exhausted >= p.replanExhausted && nowMs - s.exhaustedSinceMs <= p.windowMs)
        return Trigger::ReplanExhausted;
    if (p.frozenMs && nowMs - s.anchorMs >= p.frozenMs)
        return Trigger::Frozen;
    return Trigger::None;
}

// escapeYards[k]: how far probe k's mmap path gets from the bot (0 = no usable path).
[[nodiscard]] inline bool NavmeshHole(std::array<std::uint32_t, kProbes> const& escapeYards, std::uint32_t minEscapeYards)
{
    for (std::uint32_t y : escapeYards)
        if (y >= minEscapeYards)
            return false;
    return true;
}

[[nodiscard]] inline Action Decide(bool hearthReady, bool hearthFar, bool navmeshHole)
{
    if (hearthReady && hearthFar)
        return Action::Hearth;
    return navmeshHole ? Action::Portal : Action::None;
}

// A decision was taken (any action, none included): counters reset, cooldown starts.
inline void Acted(Params const& p, BotState& s, std::uint64_t nowMs)
{
    s.anchored = false;
    s.exhausted = 0;
    s.cooldownUntilMs = nowMs + p.cooldownMs;
}

// ---- runtime (SurvivalRecovery.cpp) ------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }

// TravelIntent give-up hook (reason intent_replan_exhausted).
void NoteReplanExhausted(std::uint32_t botGuid);
// MoveFarTo hook. True: the bot is unsticking (hearth cast under way, or just moved) - the caller returns.
bool Step(PlayerbotAI* botAI, std::uint32_t destMap, float destX, float destY, float destZ);
}  // namespace AutoWowUnstick

namespace AutoWowSurvivalRecovery
{
// Reads AutoWow.Survival.SafeRevive.*, AutoWow.Survival.RestSafe.* and AutoWow.Survival.Unstick.*. Called
// once at world init.
void LoadConfig();
void AddScripts();
}  // namespace AutoWowSurvivalRecovery

#endif  // AUTOWOW_SURVIVAL_RECOVERY_H
