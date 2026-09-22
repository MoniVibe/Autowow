/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDFEARRESPONSEPOLICY_H
#define PLAYERBOTS_RAIDFEARRESPONSEPOLICY_H

#include "SharedDefines.h"

namespace RaidFearResponsePolicy
{
constexpr float kOnyxiaLandingWarningHealthPct = 45.0f;

inline bool SpellUsesFear(uint64 mechanicMask, bool appliesFearAura)
{
    return appliesFearAura || (mechanicMask & (1ULL << MECHANIC_FEAR)) != 0;
}

inline bool ShouldPrepare(bool inRaid, bool alive, bool warningActive, bool isPriest, bool isShaman)
{
    return inRaid && alive && warningActive && (isPriest || isShaman);
}

inline bool ShouldWarnForOnyxiaLanding(bool bossAlive, bool bossFlying, float bossHealthPct)
{
    return bossAlive && bossFlying && bossHealthPct > 0.0f && bossHealthPct <= kOnyxiaLandingWarningHealthPct;
}
}

#endif
