#ifndef PLAYERBOTS_RAIDINTERRUPTRESPONSEPOLICY_H
#define PLAYERBOTS_RAIDINTERRUPTRESPONSEPOLICY_H

#include "SharedDefines.h"

namespace RaidInterruptResponsePolicy
{
struct ActionNames
{
    char const* primary = nullptr;
    char const* fallback = nullptr;
};

// These are ordinary class actions already exposed by each Playerbots class context. The shared
// raid layer only tries them after legitimately selecting a casting priority add as current target.
inline ActionNames GetActions(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            return {"pummel", "shield bash"};
        case CLASS_PALADIN:
            return {"hammer of justice", nullptr};
        case CLASS_HUNTER:
            return {"silencing shot", nullptr};
        case CLASS_ROGUE:
            return {"kick", nullptr};
        case CLASS_PRIEST:
            return {"silence", nullptr};
        case CLASS_DEATH_KNIGHT:
            return {"mind freeze", nullptr};
        case CLASS_SHAMAN:
            return {"wind shear", nullptr};
        case CLASS_MAGE:
            return {"counterspell", nullptr};
        case CLASS_WARLOCK:
            return {"spell lock", nullptr};
        default:
            return {};
    }
}
}

#endif
