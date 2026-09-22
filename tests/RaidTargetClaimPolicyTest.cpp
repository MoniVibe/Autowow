/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidTargetClaimValue.h"

#include "gtest/gtest.h"

TEST(RaidTargetClaimPolicy, LowerAuthorityCannotReplaceLiveTriageClaim)
{
    EXPECT_FALSE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterBaseline));
    EXPECT_FALSE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::GenericTriage));
}

TEST(RaidTargetClaimPolicy, HigherAuthorityAndSameOwnerCanPreempt)
{
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterEmergency));
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, true, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterBaseline));
}

TEST(RaidTargetClaimPolicy, StickySameOwnerCannotChurnButEmergencyStillPreempts)
{
    EXPECT_FALSE(RaidTargetClaimPolicy::CanAcquire(true, true, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::GenericTriage, false));
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, true, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterEmergency, false));
}

TEST(RaidTargetClaimPolicy, BossOwnershipPreemptsGenericTriageButNotEncounterEmergency)
{
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::BossOwnership));
    EXPECT_FALSE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::BossOwnership, RaidTargetAuthority::GenericTriage));
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, false, false,
        RaidTargetAuthority::BossOwnership, RaidTargetAuthority::EncounterEmergency));
}

TEST(RaidTargetClaimPolicy, SameTargetCooperatesWithoutUnnecessaryRetarget)
{
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(true, false, true,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterBaseline));
    EXPECT_TRUE(RaidTargetClaimPolicy::CanAcquire(false, false, false,
        RaidTargetAuthority::GenericTriage, RaidTargetAuthority::EncounterBaseline));
}

TEST(RaidTargetClaimPolicy, ClaimExpiryIsWrapSafe)
{
    EXPECT_FALSE(RaidTargetClaimPolicy::IsExpired(1000, 1200));
    EXPECT_TRUE(RaidTargetClaimPolicy::IsExpired(1200, 1200));
    EXPECT_TRUE(RaidTargetClaimPolicy::IsExpired(1201, 1200));
    EXPECT_TRUE(RaidTargetClaimPolicy::IsExpired(1000, 0));
}

TEST(RaidTargetClaimPolicy, LeaseBridgesOrdinaryCombatDecisionIntervals)
{
    // Dynamic raid-trigger refreshes can span several ordinary combat decisions. The live proof
    // still oscillated with a 1500 ms lease, so retain enough headroom to bridge that interval.
    EXPECT_GE(RaidTargetClaimPolicy::kClaimLifetimeMs, 3000u);
    EXPECT_FALSE(RaidTargetClaimPolicy::IsExpired(
        1500u, 1500u + RaidTargetClaimPolicy::kClaimLifetimeMs));
}
