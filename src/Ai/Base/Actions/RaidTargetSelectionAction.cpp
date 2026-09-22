/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidTargetSelectionAction.h"

#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

bool CooperativeRaidTargetAction::AttackWithRaidTargetClaim(
    Unit* target, std::string const& owner, RaidTargetAuthority authority, bool allowSameOwnerRetarget)
{
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    RaidTargetClaim& claim = AI_VALUE(RaidTargetClaim&, "raid target claim");
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    uint32 const nowMs = getMSTime();

    bool active = claim.target && !RaidTargetClaimPolicy::IsExpired(nowMs, claim.expiresAtMs) && currentTarget &&
        currentTarget->GetGUID() == claim.target;
    if (active)
    {
        Unit* claimedTarget = botAI->GetUnit(claim.target);
        active = claimedTarget && claimedTarget->IsAlive() && claimedTarget->IsInWorld();
    }
    if (!active)
        claim.Clear();

    bool const sameOwner = active && claim.owner == owner;
    bool const sameTarget = active && claim.target == target->GetGUID();
    if (!RaidTargetClaimPolicy::CanAcquire(
            active, sameOwner, sameTarget, claim.authority, authority, allowSameOwnerRetarget))
        return false;

    if (!active || sameOwner || static_cast<uint8>(authority) > static_cast<uint8>(claim.authority))
    {
        claim.target = target->GetGUID();
        claim.owner = owner;
        claim.authority = authority;
        claim.expiresAtMs = nowMs + RaidTargetClaimPolicy::kClaimLifetimeMs;
    }

    if (currentTarget == target)
        return false;
    return Attack(target);
}
