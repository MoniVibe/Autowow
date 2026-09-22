/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef _PLAYERBOT_DUNGEON_ENCOUNTER_COMPLETION_POLICY_H
#define _PLAYERBOT_DUNGEON_ENCOUNTER_COMPLETION_POLICY_H

#include <cstdint>
#include <limits>

namespace DungeonEncounterCompletion
{
// DungeonEncounter.dbc encounterIndex indexes the completed-encounter mask. InstanceScript boss
// state slots are script-private Data values and are deliberately not an input to this policy.
inline bool IsComplete(std::uint32_t completedEncounterMask, std::uint32_t dbcEncounterIndex)
{
    return dbcEncounterIndex < std::numeric_limits<std::uint32_t>::digits &&
        (completedEncounterMask & (std::uint32_t(1) << dbcEncounterIndex)) != 0;
}
}

#endif
