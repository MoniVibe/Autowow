/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef AUTOWOW_FIXTURE_FACTORY_CONTROL_H
#define AUTOWOW_FIXTURE_FACTORY_CONTROL_H

#include <string>

#include "Define.h"

// Isolated existing-character fixture initialization for the AutoWow bridge.
//
// The runtime surface is deliberately narrower than the normal random-bot reset path: it only
// resolves an already-online Playerbot, requires an explicit config allow-list entry, and delegates
// the supported level/spell/skill/talent/equipment work to PlayerbotFactory.
namespace AutoWowFixture
{
inline constexpr char kFixtureGuidsConfigKey[] = "AutoWow.FixtureGuids";
inline constexpr uint32 kMaxSpecIndexExclusive = 20; // MAX_SPECNO in PlayerbotAIConfig.h
inline constexpr uint32 kMinQuality = 0;
inline constexpr uint32 kMaxQuality = 5; // WotLK ItemQualities: poor through legendary

// Pure policy helpers kept available for contract tests without constructing a live world.
bool IsFixtureGuidAllowlisted(std::string const& configured, uint32 guid);
bool IsValidLevel(uint32 level, uint32 maxLevel);
bool IsValidSpecIndex(uint32 specIndex);
bool IsValidQuality(uint32 quality);

// These functions must be invoked on the world thread. AutoWowBridge dispatches them through
// PlayerbotWorldThreadProcessor, so they never inspect or mutate live Player objects from the
// bridge network thread.
std::string Init(uint32 guid, uint32 level, uint32 specIndex, uint32 quality);
std::string Status(uint32 guid);
}

#endif  // AUTOWOW_FIXTURE_FACTORY_CONTROL_H
