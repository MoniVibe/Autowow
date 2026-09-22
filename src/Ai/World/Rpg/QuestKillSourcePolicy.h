#ifndef PLAYERBOTS_QUEST_KILL_SOURCE_POLICY_H
#define PLAYERBOTS_QUEST_KILL_SOURCE_POLICY_H

#include <cstdint>

#include "UnitDefines.h"

namespace QuestKillSourcePolicy
{
// A counted creature objective is not necessarily a kill. An authored creature that players
// cannot attack needs a spell/script interaction, which the kill-only executor cannot supply.
[[nodiscard]] inline bool CanUseKillExecutor(std::uint32_t unitFlags)
{
    constexpr std::uint32_t nonKillable = UNIT_FLAG_NON_ATTACKABLE |
        UNIT_FLAG_NOT_ATTACKABLE_1 | UNIT_FLAG_NON_ATTACKABLE_2 |
        UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_SELECTABLE;
    return (unitFlags & nonKillable) == 0;
}
}

#endif
