/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GAME_OBJECT_LOCK_POLICY_H
#define PLAYERBOTS_GAME_OBJECT_LOCK_POLICY_H

#include "DBCStructure.h"
#include "SharedDefines.h"

namespace GameObjectLockPolicy
{
// These LockType values mean that the object can be opened through its ordinary
// interaction path. Keep this explicit: SkillByLockType() returning SKILL_NONE
// is not sufficient to distinguish ordinary opening from non-skill routes.
[[nodiscard]] inline bool IsOrdinaryOpenLockType(LockType lockType)
{
    switch (lockType)
    {
        case LOCKTYPE_OPEN:
        case LOCKTYPE_QUICK_OPEN:
        case LOCKTYPE_OPEN_KNEELING:
        case LOCKTYPE_OPEN_ATTACKING:
        case LOCKTYPE_SLOW_OPEN:
            return true;
        default:
            return false;
    }
}

// A null LockEntry represents an unlocked object at the policy boundary. For
// a non-null entry, an ordinary route must be explicitly encoded as a skill-key
// alternative; this rejects key, spell, profession-only, and other routes even
// when their LockType happens to map to SKILL_NONE.
[[nodiscard]] inline bool HasOrdinaryOpenAlternative(LockEntry const* lockInfo)
{
    if (!lockInfo)
        return true;

    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (lockInfo->Type[i] == LOCK_KEY_SKILL &&
            IsOrdinaryOpenLockType(static_cast<LockType>(lockInfo->Index[i])))
            return true;
    }

    return false;
}
}  // namespace GameObjectLockPolicy

#endif  // PLAYERBOTS_GAME_OBJECT_LOCK_POLICY_H
