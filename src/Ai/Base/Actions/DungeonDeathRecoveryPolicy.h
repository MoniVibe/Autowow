/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef _PLAYERBOT_DUNGEON_DEATH_RECOVERY_POLICY_H
#define _PLAYERBOT_DUNGEON_DEATH_RECOVERY_POLICY_H

#include <cstdint>

namespace DungeonDeathRecovery
{
constexpr std::uint8_t MaxEntranceAttempts = 4;
constexpr std::uint32_t MaxCorpseRunSeconds = 8 * 60;
constexpr std::uint32_t RetryBackoffSeconds = 5;

struct PartyRecoveryOwnerFacts
{
    bool exactRecruitedMember = false;
    bool exactWitnessMember = false;
    bool inside = false;
    std::uint32_t runMap = 0;
    std::uint32_t runInstance = 0;
    std::uint32_t corpseMap = 0;
    std::uint32_t witnessedInstance = 0;
};

// A released corpse has no persisted instance id. Ownership therefore needs a live member in the
// exact run instance as its witness; unknown or foreign instances fail closed.
constexpr bool HasExactPartyRecoveryOwner(PartyRecoveryOwnerFacts const& facts)
{
    return facts.exactRecruitedMember && facts.exactWitnessMember && facts.inside && facts.runMap == facts.corpseMap &&
           facts.runInstance != 0 && facts.runInstance == facts.witnessedInstance;
}

// A grouped dungeon player should leave a resurrectable body for an alive healer. Releasing
// immediately strands the ghost outside the instance and prevents the party from recovering.
constexpr bool ShouldWaitForHealer(bool grouped, bool sameInstanceDungeon, bool aliveHealerPresent)
{
    return grouped && sameInstanceDungeon && aliveHealerPresent;
}

enum class ReleasedRecoveryStep : std::uint8_t
{
    OrdinaryDeath,
    ApproachEntrance,
    ActivateEntrance,
    EntranceInProgress,
    RetryBackoff,
    ResumeCorpseApproach,
    Exhausted,
    MissingEntrance
};

struct ReleasedRecoveryFacts
{
    bool grouped = false;
    bool releasedGhost = false;
    bool corpseInDungeonOrRaid = false;
    bool onCorpseMap = false;
    bool ingressPortalAvailable = false;
    bool insideIngressPortal = false;
    bool movementOrTransferInProgress = false;
    bool retryBackoffActive = false;
    std::uint8_t entranceAttempts = 0;
    std::uint32_t elapsedSeconds = 0;
};

// This is deliberately a finite state machine. Ordinary world deaths never enter it, and a
// released instance ghost either reaches the normal corpse-reclaim path or stops issuing ingress
// attempts after the fixed attempt/time budget.
constexpr ReleasedRecoveryStep EvaluateReleasedRecovery(ReleasedRecoveryFacts const& facts)
{
    if (!facts.grouped || !facts.releasedGhost || !facts.corpseInDungeonOrRaid)
        return ReleasedRecoveryStep::OrdinaryDeath;

    if (facts.onCorpseMap)
        return ReleasedRecoveryStep::ResumeCorpseApproach;

    if (facts.entranceAttempts >= MaxEntranceAttempts ||
        facts.elapsedSeconds >= MaxCorpseRunSeconds)
        return ReleasedRecoveryStep::Exhausted;

    if (facts.movementOrTransferInProgress)
        return ReleasedRecoveryStep::EntranceInProgress;

    if (facts.retryBackoffActive)
        return ReleasedRecoveryStep::RetryBackoff;

    if (!facts.ingressPortalAvailable)
        return ReleasedRecoveryStep::MissingEntrance;

    return facts.insideIngressPortal ? ReleasedRecoveryStep::ActivateEntrance :
        ReleasedRecoveryStep::ApproachEntrance;
}
}

#endif
