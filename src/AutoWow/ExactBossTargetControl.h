/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_EXACT_BOSS_TARGET_CONTROL_H
#define AUTOWOW_EXACT_BOSS_TARGET_CONTROL_H

#include <string>

#include "Define.h"

// Pure parsing and eligibility policy for the bridge's exact creature-entry and player engage
// orders. Live world lookup and attack intent stay in AutoWowBridgeOperation on the world thread.
namespace AutoWowExactBossTarget
{
inline constexpr float kMaxTargetDistance = 100.0f;

enum class EngageMode
{
    Nearby,
    CreatureEntry,
    PlayerGuid
};

struct WireRequest
{
    EngageMode mode = EngageMode::Nearby;
    uint32 leaderGuid = 0;
    uint32 creatureEntry = 0;
    uint32 playerGuid = 0;
};

struct TargetFacts
{
    bool inWorld = false;
    bool sameMap = false;
    bool sameInstance = false;
    bool alive = false;
    bool hostile = false;
    bool attackable = false;
    float distance = 0.0f;
};

// Accepts the existing broad form (`engage <leader-guid>`), exact creature form
// (`engage <leader-guid> entry <creature-entry>`), and exact player form
// (`engage <leader-guid> player <player-guid>`). Every other shape fails closed.
bool ParseWireRequest(std::string const& requestText, WireRequest& request, std::string& error);

// Pure policy shared by target discovery and whole-party preflight.
bool ValidateTargetFacts(TargetFacts const& facts, std::string& error);
}

#endif  // AUTOWOW_EXACT_BOSS_TARGET_CONTROL_H
