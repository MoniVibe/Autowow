/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDTARGETSELECTIONACTION_H
#define PLAYERBOTS_RAIDTARGETSELECTIONACTION_H

#include "AttackAction.h"
#include "RaidTargetClaimValue.h"

class CooperativeRaidTargetAction : public AttackAction
{
public:
    CooperativeRaidTargetAction(PlayerbotAI* botAI, std::string const name) : AttackAction(botAI, name) {}

protected:
    bool AttackWithRaidTargetClaim(Unit* target, std::string const& owner, RaidTargetAuthority authority,
        bool allowSameOwnerRetarget = true);
};

#endif
