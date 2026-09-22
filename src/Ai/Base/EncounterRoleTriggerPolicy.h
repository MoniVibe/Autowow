/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENCOUNTERROLETRIGGERPOLICY_H
#define PLAYERBOTS_ENCOUNTERROLETRIGGERPOLICY_H

namespace EncounterRoleTriggerPolicy
{
constexpr unsigned int kMaxRespondersPerPriorityRaidAdd = 2;
constexpr unsigned int kMinimumBossPressureDps = 1;

constexpr bool ShouldTargetOnyxiaWhelps(bool isHealer, bool isTank, bool isRanged, bool onyxiaIsFlying)
{
    // In phase two, tanks and melee DPS control the ground adds. Ranged DPS must keep
    // damaging flying Onyxia or the encounter stalls at the air-phase threshold.
    return onyxiaIsFlying && !isHealer && (isTank || !isRanged);
}

constexpr bool ShouldPressureFlyingOnyxia(bool isHealer, bool isDps, bool isRanged, bool onyxiaIsFlying)
{
    return onyxiaIsFlying && !isHealer && isDps && isRanged;
}

constexpr bool ShouldHandlePriorityRaidAdd(bool isHealer, bool isMainTank, bool isTank, bool isDps,
    bool isRanged, bool addIsCasting, bool addAttacksHealer, bool addAttacksSelf)
{
    if (isHealer || isMainTank)
        return false;
    if (addAttacksSelf)
        return true;
    if (isTank || (isDps && !isRanged))
        return true;
    return isDps && isRanged && (addIsCasting || addAttacksHealer);
}

constexpr bool PreferBossPressureReserve(bool candidateIsRanged, unsigned int candidateGuid,
    bool currentIsRanged, unsigned int currentGuid)
{
    return candidateIsRanged != currentIsRanged ? candidateIsRanged : candidateGuid < currentGuid;
}

constexpr unsigned int PriorityRaidHelperSlot(unsigned int addGuid, unsigned int helperCount,
    unsigned int responderOffset)
{
    return helperCount ? (addGuid % helperCount + responderOffset) % helperCount : 0;
}

constexpr unsigned int PriorityRaidHelperSlots(bool directVictimAssigned)
{
    return kMaxRespondersPerPriorityRaidAdd - (directVictimAssigned ? 1u : 0u);
}

constexpr int PriorityRaidAddScore(bool isCasting, bool attacksHealer, bool attacksNonTank, bool isElite)
{
    return (isCasting ? 1000 : 0) + (attacksHealer ? 600 : 0) + (attacksNonTank ? 300 : 0) +
        (isElite ? 400 : 0);
}

constexpr bool PreferPriorityRaidAdd(bool candidateAttacksSelf, int candidateScore, float candidateDistance,
    unsigned int candidateGuid, bool currentAttacksSelf, int currentScore, float currentDistance,
    unsigned int currentGuid)
{
    return (candidateAttacksSelf && !currentAttacksSelf) ||
        (candidateAttacksSelf == currentAttacksSelf && candidateScore > currentScore) ||
        (candidateAttacksSelf == currentAttacksSelf && candidateScore == currentScore &&
            candidateDistance < currentDistance) ||
        (candidateAttacksSelf == currentAttacksSelf && candidateScore == currentScore &&
            candidateDistance == currentDistance && candidateGuid < currentGuid);
}

constexpr bool ShouldPreemptPriorityRaidAdd(bool candidateAttacksSelf, int candidateScore,
    bool currentAttacksSelf, int currentScore)
{
    if (candidateAttacksSelf != currentAttacksSelf)
        return candidateAttacksSelf;
    return candidateScore > currentScore;
}

constexpr bool IsNotBehindIngvar(bool isBehindCurrentTarget)
{
    return !isBehindCurrentTarget;
}
}

#endif
