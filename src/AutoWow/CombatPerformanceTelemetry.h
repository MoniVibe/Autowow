/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
#define AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

class Unit;

// Headless, server-side combat contribution telemetry. The accumulator is deliberately value-only so
// its reset and bounded-window semantics can be tested without constructing a worldserver Unit.
// Runtime entry points are called from map threads (UnitScript callbacks, MapUpdate.Threads) and the
// world thread (AutoWow bridge); both runtime stores are mutex-guarded.
namespace AutoWowCombatPerformanceTelemetry
{
inline constexpr char kSchema[] = "autowow.combat-counters.v1";
inline constexpr std::uint32_t kVersion = 1;

// A combat session is bounded to fifteen minutes and is retired after thirty seconds without a
// combat event. The next event starts a fresh window. These are intentionally explicit rather than
// trying to infer a group-wide encounter boundary from individual combat flags.
inline constexpr std::uint64_t kMaxWindowMs = 15ULL * 60ULL * 1000ULL;
inline constexpr std::uint64_t kIdleTimeoutMs = 30ULL * 1000ULL;
inline constexpr std::uint64_t kThreatSampleIntervalMs = 1000ULL;
inline constexpr std::size_t kMaxTrackedBots = 256;

struct CounterSnapshot
{
    // `available` means the server hook-backed feature is present. `tracked` distinguishes a bot
    // that has produced an event since login from a bot with an empty window.
    bool available = true;
    bool tracked = false;
    bool active = false;

    std::uint64_t windowStartUnixMs = 0;
    std::uint64_t windowDurationMs = 0;
    std::uint64_t maxWindowMs = kMaxWindowMs;
    std::uint64_t idleTimeoutMs = kIdleTimeoutMs;

    // Damage is the amount exposed by UnitScript::OnDamage. AzerothCore does not expose a single
    // post-absorb/post-resist after-hook here, so consumers should treat it as server event damage,
    // not as a client combat-log reconstruction.
    std::uint64_t damageDone = 0;
    std::uint64_t effectiveHealing = 0;
    bool overhealingAvailable = false;
    std::uint64_t overhealing = 0;
    std::uint64_t damageTaken = 0;
    std::uint32_t deaths = 0;

    std::uint32_t combatEntries = 0;
    std::uint32_t combatExits = 0;

    std::uint32_t threatSamples = 0;
    std::uint32_t maxThreatenedByMe = 0;
    std::uint32_t maxOwnersTargetingBot = 0;
    float maxThreat = 0.0f;
};

class RollingCounters
{
public:
    void RecordDamageDone(std::uint64_t nowMs, std::uint64_t amount);
    void RecordEffectiveHealing(std::uint64_t nowMs, std::uint64_t amount);
    void RecordDamageTaken(std::uint64_t nowMs, std::uint64_t amount);
    void RecordDeath(std::uint64_t nowMs);
    void RecordCombatEntry(std::uint64_t nowMs);
    void RecordCombatExit(std::uint64_t nowMs);
    void RecordThreatSample(std::uint64_t nowMs, std::uint32_t threatenedByMe,
                            std::uint32_t ownersTargetingBot, float highestThreat);

    // Tick may reset state; it is called by the world-thread UnitScript update hook. Snapshot is
    // const and never performs cleanup, preserving the bridge's read-only behavior.
    void Tick(std::uint64_t nowMs);
    CounterSnapshot Snapshot(std::uint64_t nowMs) const;

    [[nodiscard]] std::uint64_t LastActivityMs() const { return lastActivityMs; }

private:
    bool ShouldReset(std::uint64_t nowMs) const;
    void StartWindow(std::uint64_t nowMs);
    void ResetWindow();
    void Touch(std::uint64_t nowMs);

    bool active = false;
    std::uint64_t windowStartMs = 0;
    std::uint64_t lastActivityMs = 0;
    std::uint64_t lastThreatSampleMs = 0;
    bool hasThreatSample = false;

    std::uint64_t damageDone = 0;
    std::uint64_t effectiveHealing = 0;
    std::uint64_t damageTaken = 0;
    std::uint32_t deaths = 0;
    std::uint32_t combatEntries = 0;
    std::uint32_t combatExits = 0;
    std::uint32_t threatSamples = 0;
    std::uint32_t maxThreatenedByMe = 0;
    std::uint32_t maxOwnersTargetingBot = 0;
    float maxThreat = 0.0f;
};

// Runtime store and hook bridge. Thread-safe (the v1 store is mutex-guarded); callable from map threads.
void RecordDamageDone(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount);
void RecordEffectiveHealing(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount);
void RecordDamageTaken(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount);
void RecordDeath(std::uint32_t botGuid, std::uint64_t nowMs);
void RecordCombatEntry(std::uint32_t botGuid, std::uint64_t nowMs);
void RecordCombatExit(std::uint32_t botGuid, std::uint64_t nowMs);
void RecordThreatSample(std::uint32_t botGuid, std::uint64_t nowMs, std::uint32_t threatenedByMe,
                        std::uint32_t ownersTargetingBot, float highestThreat);

// The default snapshot is still `available=true`, but reports tracked=false/active=false when no
// event has reached the bounded store. It never allocates, resets, or removes state.
CounterSnapshot SnapshotFor(std::uint32_t botGuid);
void Forget(std::uint32_t botGuid);

// Registers the UnitScript event hooks during AddPlayerbotsScripts().
void AddAutoWowCombatPerformanceTelemetryScript();

// ---- AutoWow.CombatTelemetry.* (default off) -------------------------------------------------
// Everything gated by this flag is inert when it is 0: the hooks early-return on one cached bool and
// the v1 window above behaves exactly as before (combat.entries stays 0 for players).
//   C1: player-side combat entry. The core raises OnUnitEnterCombat only for creatures
//       (Creature.cpp), so v1 `combat.entries` was structurally 0 for bots; with the flag on,
//       PlayerScript::OnPlayerEnterCombat (CombatManager::UpdateOwnerCombatState, every
//       false->true transition) feeds RecordCombatEntry.
namespace detail
{
inline bool gTelemetryEnabled = false;
inline std::uint64_t gLogIntervalMs = 60000;
}

inline bool TelemetryEnabled() { return detail::gTelemetryEnabled; }

// AutoWow.CombatTelemetry.Reactivity (default 0): bot `combat` rows carry the cv=3/4 appendix.
// AutoWow.CombatTelemetry.Players (default 0): real (non-bot) player sessions get lifetime totals and
// `combat` rows too (always with the appendix, tagged "human":1; also one row at each combat end, so a
// training-dummy fight with no kill and no death still yields a row). Both need ...Enable = 1.
namespace detail
{
inline bool gReactivityEnabled = false;
inline bool gPlayersEnabled = false;
}
inline bool ReactivityEnabled() { return detail::gTelemetryEnabled && detail::gReactivityEnabled; }
inline bool PlayersEnabled() { return detail::gTelemetryEnabled && detail::gPlayersEnabled; }

// Reads the AutoWow.CombatTelemetry.* / AutoWow.Combat.* keys. Called once at world init.
void LoadConfig();

// ---- C2/C4: per-bot lifetime totals (AutoWow.CombatTelemetry.Enable) ----------------------------
// Unlike the v1 window these never retire: cumulative since login, cleared only on logout. Emitted as
// the ledger `combat` event (C6); consumers fold deltas between lines (scripts/combat-reduce.py).
// cv (kLifetimeSchemaVersion) bumps on any field meaning change; fields are append-only.
// cv=2 (kLifetimeSchemaVersionTactics) = every cv=1 field unchanged, then tac_ms=[[tactic id, ms], ...]
// (cumulative in-engagement ms per AutoWowTactics::TacticId, ascending id) and arm (0 control, 1 treatment).
// A bot emits cv=2 only once the tactical layer (AutoWow.Tactics.Observe/Enable) has credited it; otherwise
// its lines stay byte-identical cv=1.
// cv=3 / cv=4 (lane combatp0) = the cv=1 / cv=2 line unchanged, then the reactivity appendix below. Emitted
// only by bots with AutoWow.CombatTelemetry.Reactivity = 1 and by every human row
// (AutoWow.CombatTelemetry.Players = 1); otherwise lines stay byte-identical cv=1/cv=2.
//   busy_ms  cumulative in-combat ms the caster was locked by its own casts: union over non-triggered casts
//            of [cast start, max(cast start + effective GCD incl. haste, cast end / channel end)]
//            (busy share = d(busy_ms) / d(combat_ms)).
//   spells   this line's window (since the previous line): top-8 [spell id, casts], count desc, id asc.
//   rl_att   this window's reaction latency after a new attacker: [samples, p50 ms, p90 ms, misses].
//   rl_hp    same, after own HP crossed below 35%. Latency = event -> start of the next non-triggered cast;
//            a miss = no cast within kReactionExpiryMs.
//   human    1 on rows from a real (non-bot) player session.
inline constexpr std::uint32_t kLifetimeSchemaVersion = 1;
inline constexpr std::uint32_t kLifetimeSchemaVersionTactics = 2;
inline constexpr std::uint32_t kLifetimeSchemaVersionReactivity = 3;
inline constexpr std::uint32_t kLifetimeSchemaVersionTacticsReactivity = 4;
inline constexpr std::size_t kSpellSlots = 32;          // distinct spells per window; more -> not ranked
inline constexpr std::size_t kSpellTopN = 8;
inline constexpr std::size_t kReactionSamplesMax = 32;  // per kind per window; more -> dropped
inline constexpr std::uint64_t kReactionExpiryMs = 10000;
inline constexpr std::uint32_t kLowHpPct = 35;
inline constexpr std::uint32_t kGcdMinMs = 1000;        // core MIN_GCD / MAX_GCD
inline constexpr std::uint32_t kGcdMaxMs = 1500;

// Effective GCD as Spell::TriggerGlobalCooldown computes it, minus SPELLMOD_GLOBAL_COOLDOWN talent/glyph mods
// (ponytail: not read - ApplySpellMod can mutate the live Spell; rare mods only overstate busy a little).
// castSpeedPermille = UNIT_MOD_CAST_SPEED x 1000 (1000 = no haste); hasteApplies per the core's category-133 test.
inline std::uint32_t EffectiveGcdMs(std::int32_t startRecoveryTime, bool hasteApplies, std::uint32_t castSpeedPermille)
{
    if (startRecoveryTime <= 0)
        return 0;
    std::uint32_t gcd = static_cast<std::uint32_t>(startRecoveryTime);
    if (gcd < kGcdMinMs || gcd > kGcdMaxMs)
        return gcd;
    if (hasteApplies)
        gcd = static_cast<std::uint32_t>(std::uint64_t(gcd) * castSpeedPermille / 1000);
    return std::clamp(gcd, kGcdMinMs, kGcdMaxMs);
}

// Nearest-rank percentile (pct 1-100) of v[0..n); sorts v in place. 0 when n == 0.
std::uint32_t PercentileNearestRank(std::uint32_t* v, std::size_t n, std::uint32_t pct);

// Window spell histogram: fixed slots, top-N by count desc then spell id asc (deterministic).
class SpellHistogram
{
public:
    void Record(std::uint32_t spellId);
    // [[id,n],...] top kSpellTopN, then clears the window.
    std::string Drain();
    [[nodiscard]] std::size_t Distinct() const { return used; }

private:
    struct Slot
    {
        std::uint32_t id = 0;
        std::uint32_t count = 0;
    };
    Slot slots[kSpellSlots] = {};
    std::size_t used = 0;
};

// Event -> next-cast latency samples for one trigger kind; pending event survives window drains.
class ReactionLatency
{
public:
    void Event(std::uint64_t nowMs);       // first pending event wins until a cast answers it
    void Cast(std::uint64_t castStartMs);  // answers a pending event that happened at or before cast start
    void Expire(std::uint64_t nowMs);
    std::string Drain();                   // [n,p50,p90,miss], then clears the window
    [[nodiscard]] std::size_t Samples() const { return count; }

private:
    bool pending = false;
    std::uint64_t pendingMs = 0;
    std::uint32_t samples[kReactionSamplesMax] = {};
    std::size_t count = 0;
    std::uint32_t misses = 0;
};
inline constexpr std::size_t kTacticSlots = 128;  // AutoWowTactics::kMaxTacticId (families 1-10, ids < 110)
inline constexpr std::size_t kTtkTracked = 16;              // concurrently engaged creatures per bot
inline constexpr std::uint64_t kTtkEngageExpiryMs = 120000; // engagement forgotten after 2 min
inline constexpr std::size_t kTtkPendingMax = 64;           // TTK samples per emit line; rest -> ttk_drop
inline constexpr std::uint64_t kRecentDpsWindowMs = 60000;  // recent-DPS halves once combat ms exceeds it
inline constexpr std::uint64_t kRecentDpsMinMs = 5000;      // below this much combat, recent DPS unknown (0)
inline constexpr std::size_t kMaxLifetimeBots = 2048;       // hard cap; beyond it new bots are not tracked

// Raw Powers values (SharedDefines.h; static_assert'ed in the .cpp) so this header stays value-only.
inline constexpr std::uint8_t kPowerMana = 0;
inline constexpr std::uint8_t kPowerRage = 1;
inline constexpr std::uint8_t kPowerEnergy = 3;
inline constexpr std::uint32_t kStarvedManaPct = 15;   // AiPlayerbot.LowMana default
inline constexpr std::uint32_t kStarvedRage = 100;     // 10 rage (core stores rage x10)
inline constexpr std::uint32_t kStarvedEnergy = 20;

// Resource-starved = cannot afford a typical ability. Runic power / focus / others: not measured (false).
inline bool IsResourceStarved(std::uint8_t powerType, std::uint32_t current, std::uint32_t maximum)
{
    switch (powerType)
    {
        case kPowerMana:
            return maximum && std::uint64_t(current) * 100 < std::uint64_t(maximum) * kStarvedManaPct;
        case kPowerRage:
            return current < kStarvedRage;
        case kPowerEnergy:
            return current < kStarvedEnergy;
        default:
            return false;
    }
}

struct TtkSample
{
    std::uint32_t ms = 0;
    std::int32_t levelDelta = 0;  // creature level - bot level
};

struct LifetimeTotals
{
    std::uint64_t wallMs = 0;
    std::uint64_t combatMs = 0;
    std::uint64_t deadMs = 0;
    std::uint64_t starvedMs = 0;
    std::uint64_t damageDone = 0;
    std::uint64_t damageTaken = 0;
    std::uint64_t healing = 0;
    std::uint32_t fights = 0;
    std::uint32_t kills = 0;
    std::uint32_t deaths = 0;
    std::uint32_t casts = 0;
    std::uint32_t gcdCasts = 0;
    std::uint32_t dotSkips = 0;
    std::uint32_t ttkCount = 0;
    std::uint32_t ttkDropped = 0;
};

// Value-only per-bot accumulator (unit-tested). TTK = first damage by the bot (or its pet) on a creature
// to that creature's death by the bot's (or pet's) killing blow.
class LifetimeCounters
{
public:
    void Update(std::uint64_t diffMs, bool inCombat, bool dead, bool starved);
    void RecordFight();
    void RecordDamageDone(std::uint64_t nowMs, std::uint64_t amount, std::uint64_t creatureKey);
    void RecordDamageTaken(std::uint64_t amount);
    void RecordHealing(std::uint64_t amount);
    void RecordDeath();
    void RecordCast(bool onGcd);
    void RecordKill(std::uint64_t nowMs, std::uint64_t creatureKey, std::int32_t levelDelta);
    // Counts one skipped DoT/debuff opportunity; consecutive skips of the same key count once.
    void RecordDotSkip(std::uint64_t skipKey);
    // Tactical layer: ms spent in a tactic (id < kTacticSlots; others ignored) and the bot's A/B arm.
    // The first call switches this bot's `combat` lines to cv=2.
    void RecordTactic(std::uint8_t tacticId, std::uint32_t ms, std::uint8_t arm);

    // ---- cv=3/4 reactivity appendix (fed only when the caller emits it) ----
    // Polled state each unit update: a rise in attacker count / own HP dropping below kLowHpPct are events.
    void ObserveReactivity(std::uint64_t nowMs, std::uint32_t attackers, std::uint32_t hpPct);
    // One non-triggered cast observed at its cast() (= cast end): castMs = cast time, channelMs = channel
    // duration (0 if none), gcdMs = EffectiveGcdMs. Busy time counts only when inCombat.
    void RecordCastTiming(std::uint64_t nowMs, std::uint32_t spellId, std::uint32_t castMs, std::uint32_t channelMs,
                          std::uint32_t gcdMs, bool inCombat);
    // True once after an in-combat -> out-of-combat transition seen by Update() (human rows emit per fight).
    bool TakeCombatEnded();

    // C6. First call arms the interval (returns false); then true once per intervalMs (0 = never).
    bool EmitDue(std::uint64_t nowMs, std::uint64_t intervalMs);
    // Trailing fields for the ledger `combat` event (",\"cv\":1,..."); drains the pending TTK samples.
    // reactivity appends the cv=3/4 fields (and drains their window); human appends "human":1.
    std::string DrainEmitFields(std::uint32_t classId, bool reactivity = false, bool human = false);
    [[nodiscard]] std::uint64_t BusyMs() const { return busyMs; }

    // Damage per second over roughly the last minute of combat; 0 = unknown (too little combat).
    [[nodiscard]] std::uint32_t RecentDps() const;
    [[nodiscard]] LifetimeTotals const& Totals() const { return totals; }
    [[nodiscard]] std::size_t PendingTtk() const { return pendingCount; }
    // First-damage time of a live engagement with this creature; 0 = not engaged.
    [[nodiscard]] std::uint64_t EngagedAtMs(std::uint64_t creatureKey) const;

private:
    struct Engagement
    {
        std::uint64_t key = 0;
        std::uint64_t firstMs = 0;
    };

    LifetimeTotals totals;
    Engagement engaged[kTtkTracked] = {};
    TtkSample pending[kTtkPendingMax] = {};
    std::size_t pendingCount = 0;
    std::uint64_t recentDamage = 0;
    std::uint64_t recentCombatMs = 0;
    std::uint64_t lastDotSkipKey = 0;
    std::uint64_t lastEmitMs = 0;
    bool emitArmed = false;
    bool tacticsTracked = false;
    std::uint8_t tacticArm = 0;
    std::uint64_t tacticMs[kTacticSlots] = {};

    bool wasInCombat = false;
    bool combatEnded = false;
    std::uint64_t busyMs = 0;
    std::uint64_t busyUntilMs = 0;
    SpellHistogram spells;
    bool observed = false;
    std::uint32_t lastAttackers = 0;
    bool lastLowHp = false;
    ReactionLatency attackerLatency;
    ReactionLatency lowHpLatency;
};

// Runtime (world/map-thread; the store is mutex-guarded). No-ops unless TelemetryEnabled().
std::uint32_t RecentDpsFor(std::uint32_t botGuid);
void RecordDotSkip(std::uint32_t botGuid, std::uint64_t skipKey);
void RecordTactic(std::uint32_t botGuid, std::uint8_t tacticId, std::uint32_t ms, std::uint8_t arm);
}

#endif  // AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
