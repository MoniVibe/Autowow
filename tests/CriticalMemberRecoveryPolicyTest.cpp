/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cstdint>

#include "CriticalMemberRecoveryPolicy.h"
#include "PartyMemberToHeal.h"

#include "gtest/gtest.h"

namespace
{
using CriticalMemberRecoveryPolicy::Action;
using CriticalMemberRecoveryPolicy::Decision;
using CriticalMemberRecoveryPolicy::Healer;
using CriticalMemberRecoveryPolicy::Reason;
using CriticalMemberRecoveryPolicy::Request;

constexpr float HealDistance = 38.5f;
constexpr float CriticalHealth = 25.0f;

Healer EligibleHealer(std::uint64_t guid)
{
    Healer healer;
    healer.guid = guid;
    healer.alive = true;
    healer.withinBoundedRecoveryRange = true;
    return healer;
}

Request DistantCriticalDpsRequest()
{
    Request request;
    request.target.guid = 8;
    request.target.healthPct = 7.8f;
    request.target.distanceToCurrentHealer = 91.4f;
    request.target.alive = true;
    request.healers = {EligibleHealer(26), EligibleHealer(41), EligibleHealer(68)};
    request.currentHealerGuid = 26;
    request.healDistance = HealDistance;
    request.criticalHealth = CriticalHealth;
    request.nowMs = 1000;
    return request;
}
}

TEST(CriticalMemberRecoveryPolicy, AutopsyDistanceIsCriticalAndBounded)
{
    Request const request = DistantCriticalDpsRequest();
    Decision const decision = CriticalMemberRecoveryPolicy::Evaluate(request);

    EXPECT_GT(91.4f, HealDistance * 2.0f);
    EXPECT_TRUE(CriticalMemberRecoveryPolicy::IsWithinBoundedRecoveryRange(91.4f, HealDistance));
    EXPECT_FALSE(CriticalMemberRecoveryPolicy::IsWithinBoundedRecoveryRange(96.26f, HealDistance));
    EXPECT_EQ(Action::HealerApproach, decision.action);
    EXPECT_EQ(8u, decision.claim.targetGuid);
    EXPECT_EQ(26u, decision.claim.claimantGuid);
    EXPECT_EQ(1000u + CriticalMemberRecoveryPolicy::kClaimLifetimeMs,
              decision.claim.expiresAtMs);
}

TEST(CriticalMemberRecoveryPolicy, AnEligibleHealerAlreadyInCastRangeClearsRemoteRecovery)
{
    Request request = DistantCriticalDpsRequest();
    request.healers[1].inCastRangeAndLos = true;

    Decision const decision = CriticalMemberRecoveryPolicy::Evaluate(request);
    EXPECT_EQ(Action::None, decision.action);
    EXPECT_EQ(Reason::InCastRangeAndLos, decision.reason);
}

TEST(CriticalMemberRecoveryPolicy, DerivedClaimIsIdempotentForTheSameTarget)
{
    Request request = DistantCriticalDpsRequest();
    Decision const first = CriticalMemberRecoveryPolicy::Evaluate(request);

    request.previousClaim = first.claim;
    Decision const second = CriticalMemberRecoveryPolicy::Evaluate(request);

    EXPECT_EQ(first.action, second.action);
    EXPECT_EQ(first.reason, second.reason);
    EXPECT_EQ(first.claim.targetGuid, second.claim.targetGuid);
    EXPECT_EQ(first.claim.claimantGuid, second.claim.claimantGuid);
    EXPECT_EQ(first.claim.expiresAtMs, second.claim.expiresAtMs);
}

TEST(CriticalMemberRecoveryPolicy, ActiveIncomingHealReservationBlocksASecondClaim)
{
    Request request = DistantCriticalDpsRequest();
    request.target.incomingHeal = true;

    Decision const activeReservation = CriticalMemberRecoveryPolicy::Evaluate(request);
    EXPECT_EQ(Action::None, activeReservation.action);
    EXPECT_EQ(Reason::IncomingHealReserved, activeReservation.reason);

    request.target.incomingHealIsStale = true;
    Decision const staleReservation = CriticalMemberRecoveryPolicy::Evaluate(request);
    EXPECT_EQ(Action::HealerApproach, staleReservation.action);
    EXPECT_EQ(26u, staleReservation.claim.claimantGuid);
}

TEST(CriticalMemberRecoveryPolicy, CriticalDpsBeatsNoncriticalTank)
{
    auto const criticalDps = PartyMemberToHealPolicy::CalculatePriority(
        7.8f, 91.4f, HealDistance, CriticalHealth, 45.0f, 65.0f, false, false);
    auto const noncriticalTank = PartyMemberToHealPolicy::CalculatePriority(
        60.0f, 10.0f, HealDistance, CriticalHealth, 45.0f, 65.0f, true, true);

    EXPECT_TRUE(PartyMemberToHealPolicy::ShouldReplace(criticalDps, noncriticalTank));
}

TEST(CriticalMemberRecoveryPolicy, CriticalBossTankIsNotSuppressedByBossOwnership)
{
    Request request = DistantCriticalDpsRequest();
    request.target.guid = 72;
    request.target.isTank = true;
    request.target.ownsBoss = true;
    request.currentHealerGuid = 26;

    Decision const decision = CriticalMemberRecoveryPolicy::Evaluate(request);
    EXPECT_EQ(Action::HealerApproach, decision.action);
    EXPECT_EQ(72u, decision.claim.targetGuid);
    EXPECT_EQ(26u, decision.claim.claimantGuid);
}

TEST(CriticalMemberRecoveryPolicy, ClearConditionsDoNotKeepARecoveryClaim)
{
    Request request = DistantCriticalDpsRequest();

    request.target.healthPct = CriticalHealth;
    EXPECT_EQ(Reason::AboveCriticalHealth, CriticalMemberRecoveryPolicy::Evaluate(request).reason);

    request = DistantCriticalDpsRequest();
    request.target.inCastRangeAndLos = true;
    EXPECT_EQ(Reason::InCastRangeAndLos, CriticalMemberRecoveryPolicy::Evaluate(request).reason);

    request = DistantCriticalDpsRequest();
    request.target.alive = false;
    EXPECT_EQ(Reason::Dead, CriticalMemberRecoveryPolicy::Evaluate(request).reason);
}

TEST(CriticalMemberRecoveryPolicy, BossOwnershipTankPickupAndAvoidanceCannotBePreempted)
{
    Request request = DistantCriticalDpsRequest();
    request.healers[0].ownsBoss = true;
    request.healers[1].hasTankPickup = true;
    request.healers[2].emergencyAvoidance = true;

    Decision const decision = CriticalMemberRecoveryPolicy::Evaluate(request);
    EXPECT_EQ(Action::None, decision.action);
    EXPECT_EQ(Reason::NoEligibleHealer, decision.reason);
}

TEST(CriticalMemberRecoveryPolicy, FailedApproachIsExplicitAndFailClosed)
{
    Request const request = DistantCriticalDpsRequest();
    Decision const attempt = CriticalMemberRecoveryPolicy::Evaluate(request);
    Decision const failed = CriticalMemberRecoveryPolicy::AfterHealerApproach(attempt, false);

    EXPECT_EQ(Action::NoPathOrMovementRejected, failed.action);
    EXPECT_EQ(Reason::NoPathOrMovementRejected, failed.reason);
    EXPECT_EQ(attempt.claim.targetGuid, failed.claim.targetGuid);
    EXPECT_EQ(attempt.claim.claimantGuid, failed.claim.claimantGuid);
}
