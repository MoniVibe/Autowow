/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
#define AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H

#include <cstddef>
#include <cstdint>

class Unit;

// Headless, server-side combat contribution telemetry. The accumulator is deliberately value-only so
// its reset and bounded-window semantics can be tested without constructing a worldserver Unit.
// Runtime entry points are world-thread-only: UnitScript callbacks and the AutoWow bridge operation
// both execute there, so this slice does not add a lock or cross-thread live-object access.
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

// Runtime store and hook bridge. All functions below are world-thread-only.
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
}

#endif  // AUTOWOW_COMBAT_PERFORMANCE_TELEMETRY_H
