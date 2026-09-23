/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
#define AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H

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
inline constexpr std::uint32_t kLifetimeSchemaVersion = 1;
inline constexpr std::uint32_t kLifetimeSchemaVersionTactics = 2;
inline constexpr std::size_t kTacticSlots = 32;  // AutoWowTactics::kMaxTacticId
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

    // C6. First call arms the interval (returns false); then true once per intervalMs (0 = never).
    bool EmitDue(std::uint64_t nowMs, std::uint64_t intervalMs);
    // Trailing fields for the ledger `combat` event (",\"cv\":1,..."); drains the pending TTK samples.
    std::string DrainEmitFields(std::uint32_t classId);

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
};

// Runtime (world/map-thread; the store is mutex-guarded). No-ops unless TelemetryEnabled().
std::uint32_t RecentDpsFor(std::uint32_t botGuid);
void RecordDotSkip(std::uint32_t botGuid, std::uint64_t skipKey);
void RecordTactic(std::uint32_t botGuid, std::uint8_t tacticId, std::uint32_t ms, std::uint8_t arm);
}

#endif  // AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
