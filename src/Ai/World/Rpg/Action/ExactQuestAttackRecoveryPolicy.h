#ifndef PLAYERBOTS_EXACT_QUEST_ATTACK_RECOVERY_POLICY_H
#define PLAYERBOTS_EXACT_QUEST_ATTACK_RECOVERY_POLICY_H

namespace ExactQuestAttackRecoveryPolicy
{
enum class Rejection
{
    SourceMismatch,
    TargetUnavailable,
    Flight,
    RaidClaim,
    Friendly,
    InvalidAttackTarget,
    Vehicle,
    DungeonPull,
    AlreadyEngaged,
    BlockedLineOfSight,
    OtherAttackGuard
};

struct Facts
{
    bool sourceMatched = false;
    bool targetAvailable = false;
    bool flight = false;
    bool raidClaimAllows = false;
    bool friendly = false;
    bool validAttackTarget = false;
    bool vehicleAllows = false;
    bool dungeonPullReady = false;
    bool alreadyEngaged = false;
    bool lineOfSight = false;
};

// Called only after the native AttackAction rejects the exact bound creature. Distance is
// intentionally absent: AttackAction does not reject ranged initiation based on distance.
[[nodiscard]] inline Rejection Classify(Facts const& facts)
{
    if (!facts.sourceMatched)
        return Rejection::SourceMismatch;
    if (!facts.targetAvailable)
        return Rejection::TargetUnavailable;
    if (!facts.raidClaimAllows)
        return Rejection::RaidClaim;
    if (facts.flight)
        return Rejection::Flight;
    if (facts.friendly)
        return Rejection::Friendly;
    if (!facts.validAttackTarget)
        return Rejection::InvalidAttackTarget;
    if (!facts.vehicleAllows)
        return Rejection::Vehicle;
    if (!facts.dungeonPullReady)
        return Rejection::DungeonPull;
    if (facts.alreadyEngaged)
        return Rejection::AlreadyEngaged;
    if (!facts.lineOfSight)
        return Rejection::BlockedLineOfSight;
    return Rejection::OtherAttackGuard;
}

[[nodiscard]] inline bool ShouldApproach(Rejection reason)
{
    return reason == Rejection::BlockedLineOfSight;
}
}

#endif
