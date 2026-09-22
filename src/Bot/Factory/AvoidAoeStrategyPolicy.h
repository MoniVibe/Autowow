/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AVOIDAOESTRATEGYPOLICY_H
#define PLAYERBOTS_AVOIDAOESTRATEGYPOLICY_H

namespace AiFactoryPolicy
{
constexpr bool ShouldEnableAvoidAoe(bool autoAvoidAoe, bool hasRealPlayerMaster, bool isTank, bool isHealer,
                                    bool isDps)
{
    return autoAvoidAoe && (hasRealPlayerMaster || (!isTank && (isHealer || isDps)));
}
}

#endif
