/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRITICALMEMBERRECOVERYPOLICY_H
#define PLAYERBOTS_CRITICALMEMBERRECOVERYPOLICY_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace CriticalMemberRecoveryPolicy
{
using Guid = std::uint64_t;

// The extension is deliberately bounded.  At HealDistance 38.5 this covers the observed 91.4 yd
// failure while refusing an unbounded chase or any encounter/map-specific distance.
constexpr float kMaxRecoveryRangeMultiplier = 2.5f;
constexpr std::uint32_t kClaimLifetimeMs = 2500;

enum class Action
{
    None,
    HealerApproach,
    NoPathOrMovementRejected
};

enum class Reason
{
    None,
    InvalidTarget,
    Dead,
    AboveCriticalHealth,
    InCastRangeAndLos,
    Superseded,
    OutsideBoundedRange,
    IncomingHealReserved,
    NoEligibleHealer,
    NotClaimant,
    HealerApproachAccepted,
    NoPathOrMovementRejected
};

struct Target
{
    Guid guid = 0;
    float healthPct = 100.0f;
    float distanceToCurrentHealer = 0.0f;
    bool alive = false;
    bool inCastRangeAndLos = false;
    bool incomingHeal = false;
    bool incomingHealIsStale = false;
    bool superseded = false;
    bool isTank = false;
    bool ownsBoss = false;
};

struct Healer
{
    Guid guid = 0;
    bool alive = false;
    bool withinBoundedRecoveryRange = false;
    bool inCastRangeAndLos = false;
    bool ownsBoss = false;
    bool hasTankPickup = false;
    bool emergencyAvoidance = false;
};

struct Claim
{
    Guid targetGuid = 0;
    Guid claimantGuid = 0;
    std::uint32_t expiresAtMs = 0;
};

struct Request
{
    Target target;
    std::vector<Healer> healers;
    Guid currentHealerGuid = 0;
    float healDistance = 0.0f;
    float criticalHealth = 25.0f;
    std::uint32_t nowMs = 0;
    Claim previousClaim;
};

struct Decision
{
    Action action = Action::None;
    Reason reason = Reason::None;
    Claim claim;
};

inline bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs)
{
    return expiresAtMs == 0 || static_cast<std::int32_t>(expiresAtMs - nowMs) <= 0;
}

inline bool IsCritical(Target const& target, float criticalHealth)
{
    return target.alive && target.healthPct < criticalHealth;
}

inline bool IsWithinBoundedRecoveryRange(float distance, float healDistance)
{
    return healDistance > 0.0f && distance >= 0.0f &&
           distance <= healDistance * kMaxRecoveryRangeMultiplier;
}

inline bool IsRecoveryCandidate(Target const& target, float healDistance, float criticalHealth)
{
    return target.guid != 0 && IsCritical(target, criticalHealth) &&
           IsWithinBoundedRecoveryRange(target.distanceToCurrentHealer, healDistance);
}

inline bool IsEligible(Healer const& healer)
{
    return healer.guid != 0 && healer.alive && healer.withinBoundedRecoveryRange &&
           !healer.ownsBoss &&
           !healer.hasTankPickup && !healer.emergencyAvoidance;
}

inline bool IsActiveClaim(Claim const& claim, Guid targetGuid, std::uint32_t nowMs)
{
    return claim.targetGuid == targetGuid && claim.claimantGuid != 0 &&
           !IsExpired(nowMs, claim.expiresAtMs);
}

inline Healer const* FindHealer(std::vector<Healer> const& healers, Guid guid)
{
    auto const found = std::find_if(
        healers.begin(), healers.end(),
        [guid](Healer const& healer) { return healer.guid == guid; });
    return found == healers.end() ? nullptr : &*found;
}

inline Healer const* SelectClaimant(Request const& request)
{
    // Retain an active claimant for the target's short lease.  If there is no live lease, the
    // smallest eligible healer GUID reacquires it.  All healers therefore derive the same claim
    // without a mutable global registry or per-tick claimant churn.
    if (IsActiveClaim(request.previousClaim, request.target.guid, request.nowMs))
    {
        Healer const* previous = FindHealer(request.healers, request.previousClaim.claimantGuid);
        if (previous && IsEligible(*previous))
            return previous;
    }

    Healer const* selected = nullptr;
    for (Healer const& healer : request.healers)
    {
        if (!IsEligible(healer) || (selected && selected->guid <= healer.guid))
            continue;
        selected = &healer;
    }
    return selected;
}

inline Decision Evaluate(Request const& request)
{
    Decision decision;
    decision.claim.targetGuid = request.target.guid;

    if (request.target.guid == 0)
    {
        decision.reason = Reason::InvalidTarget;
        return decision;
    }
    if (!request.target.alive)
    {
        decision.reason = Reason::Dead;
        return decision;
    }
    if (request.target.healthPct >= request.criticalHealth)
    {
        decision.reason = Reason::AboveCriticalHealth;
        return decision;
    }
    if (request.target.inCastRangeAndLos)
    {
        decision.reason = Reason::InCastRangeAndLos;
        return decision;
    }
    if (std::any_of(
            request.healers.begin(), request.healers.end(),
            [](Healer const& healer) { return IsEligible(healer) && healer.inCastRangeAndLos; }))
    {
        decision.reason = Reason::InCastRangeAndLos;
        return decision;
    }
    if (request.target.superseded)
    {
        decision.reason = Reason::Superseded;
        return decision;
    }
    if (!IsWithinBoundedRecoveryRange(
            request.target.distanceToCurrentHealer, request.healDistance))
    {
        decision.reason = Reason::OutsideBoundedRange;
        return decision;
    }
    if (request.target.incomingHeal && !request.target.incomingHealIsStale)
    {
        decision.reason = Reason::IncomingHealReserved;
        return decision;
    }

    Healer const* claimant = SelectClaimant(request);
    if (!claimant)
    {
        decision.reason = Reason::NoEligibleHealer;
        return decision;
    }

    decision.claim.claimantGuid = claimant->guid;
    decision.claim.expiresAtMs = request.nowMs + kClaimLifetimeMs;
    if (request.currentHealerGuid != claimant->guid)
    {
        decision.reason = Reason::NotClaimant;
        return decision;
    }

    decision.action = Action::HealerApproach;
    decision.reason = Reason::HealerApproachAccepted;
    return decision;
}

// ReachCombatTo owns the actual nav/path attempt.  If it rejects the bounded ordinary approach,
// this seam records a fail-closed outcome; the bidirectional non-tank rejoin is intentionally not
// added to this generic action because it would require a second movement owner.
inline Decision AfterHealerApproach(Decision const& attempt, bool movementStarted)
{
    if (movementStarted || attempt.action != Action::HealerApproach)
        return attempt;

    Decision failed = attempt;
    failed.action = Action::NoPathOrMovementRejected;
    failed.reason = Reason::NoPathOrMovementRejected;
    return failed;
}
}

#endif
