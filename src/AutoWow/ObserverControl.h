/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_OBSERVER_CONTROL_H
#define AUTOWOW_OBSERVER_CONTROL_H

#include <string>

#include "Define.h"

// Isolated AutoWow observer-camera control surface (Observer Camera v0).
//
// It operates ONLY on an allow-listed, real (non-bot), GM-protected observer character and:
//   - never touches Playerbots quest, recovery, movement, or targeting logic;
//   - never writes character quest/XP/money/item/objective data;
//   - reads a watched leader's live position and relocates the observer through the same
//     world-thread Player::TeleportTo path the bridge already uses for rally/route.
//
// Every function here MUST be invoked on the world thread (the AutoWow bridge already
// dispatches through PlayerbotWorldThreadProcessor, so this holds by construction).
namespace AutoWowObserver
{
    // sub is one of: watch, relocate, status, release, protect.
    // leaderGuid is only meaningful for "watch"; pass 0 otherwise.
    // Returns a JSON response using the same {"ok":...} envelope as the rest of the bridge.
    std::string Dispatch(std::string const& sub, uint32 observerGuid, uint32 leaderGuid);
}

#endif  // AUTOWOW_OBSERVER_CONTROL_H
