/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDTARGETCLAIMVALUE_H
#define PLAYERBOTS_RAIDTARGETCLAIMVALUE_H

#include "Value.h"

enum class RaidTargetAuthority : uint8
{
    EncounterBaseline = 10,
    GenericTriage = 20,
    BossOwnership = 25,
    EncounterEmergency = 30
};

struct RaidTargetClaim
{
    ObjectGuid target;
    std::string owner;
    RaidTargetAuthority authority = RaidTargetAuthority::EncounterBaseline;
    uint32 expiresAtMs = 0;

    // Per-bot sticky memory for the deterministic generic add-wave policy. It deliberately survives
    // ordinary claim expiry/clear so a scheduler gap cannot manufacture a new wave epoch or focus.
    // The policy owns interpretation of these primitive fields; no encounter identity is stored.
    uint64 addWaveId = 0;
    uint8 addWaveState = 0;
    uint64 addWaveAnchorGuid = 0;
    uint64 addWaveFocusGuid = 0;
    uint64 addWavePreemptGuid = 0;
    uint64 addWaveReplacementFromGuid = 0;

    void Clear()
    {
        target.Clear();
        owner.clear();
        authority = RaidTargetAuthority::EncounterBaseline;
        expiresAtMs = 0;
    }
};

namespace RaidTargetClaimPolicy
{
// Keep a cooperative selection alive across ordinary combat/trigger decisions. Live raid proof
// showed that even 1500 ms could expire between dynamic trigger refreshes, allowing the same guard
// to be reacquired 112 times. Five seconds spans that scheduling gap without locking the target:
// an explicitly authorized same-owner transition may replace its assignment, a dead/invalid target
// clears the claim, and encounter-emergency authority can always preempt it through CanAcquire.
constexpr uint32 kClaimLifetimeMs = 5000;

inline bool IsExpired(uint32 nowMs, uint32 expiresAtMs)
{
    return expiresAtMs == 0 || static_cast<int32>(expiresAtMs - nowMs) <= 0;
}

inline bool CanAcquire(bool active, bool sameOwner, bool sameTarget,
    RaidTargetAuthority currentAuthority, RaidTargetAuthority requestedAuthority,
    bool allowSameOwnerRetarget = true)
{
    return !active || sameTarget || (sameOwner && allowSameOwnerRetarget) ||
        static_cast<uint8>(requestedAuthority) > static_cast<uint8>(currentAuthority);
}
}

class RaidTargetClaimValue : public ManualSetValue<RaidTargetClaim&>
{
public:
    RaidTargetClaimValue(PlayerbotAI* botAI, std::string const name = "raid target claim")
        : ManualSetValue<RaidTargetClaim&>(botAI, data, name)
    {
    }

private:
    RaidTargetClaim data;
};

#endif
