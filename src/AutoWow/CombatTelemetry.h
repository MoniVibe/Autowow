/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_COMBAT_TELEMETRY_H
#define AUTOWOW_COMBAT_TELEMETRY_H

#include <cstdint>
#include <string>
#include <vector>

#include "CombatPerformanceTelemetry.h"

class Player;
class PlayerbotAI;

// Read-only combat witness for the AutoWow bridge. Build() must run on the world thread because it
// reads live Playerbot and Unit state. The top-level v0 contract remains instantaneous, while its
// nested counters payload is backed by the versioned server-side UnitScript hooks in
// CombatPerformanceTelemetry and is still read-only when queried.
namespace AutoWowCombatTelemetry
{
inline constexpr char kSchema[] = "autowow.combat-telemetry.v0";
inline constexpr std::uint32_t kVersion = 0;

struct UnitView
{
    bool present = false;
    std::uint32_t guid = 0;
    std::uint32_t entry = 0;
    std::string name;
    bool isPlayer = false;
    bool alive = false;
    std::uint32_t health = 0;
    std::uint32_t maxHealth = 0;
    float healthPct = 0.0f;
};

struct ThreatLink
{
    UnitView source;
    UnitView victim;
    float threat = 0.0f;
    bool sourceTargetsBot = false;
};

struct Snapshot
{
    std::uint32_t guid = 0;
    std::string name;
    bool alive = false;
    std::string deathState = "unknown";
    bool inCombat = false;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;

    std::uint32_t health = 0;
    std::uint32_t maxHealth = 0;
    float healthPct = 0.0f;
    std::uint32_t mana = 0;
    std::uint32_t maxMana = 0;
    float manaPct = 0.0f;
    std::uint32_t powerType = 0;
    std::uint32_t power = 0;
    std::uint32_t maxPower = 0;

    std::uint32_t roleMask = 0;
    bool roleTank = false;
    bool roleHealer = false;
    bool roleDps = false;
    bool mainTank = false;
    bool explicitMainTank = false;
    bool assistTank = false;
    std::uint32_t groupTanks = 0;

    std::uint32_t groupMembers = 0;
    std::uint32_t groupLeaderGuid = 0;
    UnitView victim;
    UnitView aiTarget;
    UnitView healerTarget;
    std::string healerIntent = "not_healer";

    std::uint32_t threatenedByMeCount = 0;
    std::uint32_t ownersTargetingBot = 0;
    bool threatLinksTruncated = false;
    std::vector<ThreatLink> threatLinks;

    std::uint32_t activeSpellId = 0;
    std::uint32_t lastSpellId = 0;
    std::uint32_t lastSpellTargetGuid = 0;
    std::int64_t lastSpellTime = 0;
    AutoWowCombatPerformanceTelemetry::CounterSnapshot counters;
    // Kept as compatibility aliases for existing consumers of the v0 instantaneous shape.
    bool recentCountersAvailable = true;
    std::uint32_t recentCountersWindowMs = 0;
};

// Serialize a contract snapshot. This is kept separate from live-state collection so the v0 JSON
// shape can be pinned by a small contract test without constructing a worldserver Player.
std::string Serialize(Snapshot const& snapshot);

// Collect and serialize a current world-thread snapshot. This function is read-only.
std::string Build(Player* bot, PlayerbotAI* botAI);
}

#endif  // AUTOWOW_COMBAT_TELEMETRY_H
