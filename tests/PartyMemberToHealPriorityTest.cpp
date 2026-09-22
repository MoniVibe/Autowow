/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under the terms of Version 2
 * of the License.
 */

#include "PartyMemberToHeal.h"

#include "gtest/gtest.h"

namespace
{
using PartyMemberToHealPolicy::CalculatePriority;
using PartyMemberToHealPolicy::CanSelectWithIncomingHeal;
using PartyMemberToHealPolicy::CandidatePriority;
using PartyMemberToHealPolicy::IsSelectable;
using PartyMemberToHealPolicy::ShouldReplace;

constexpr float CriticalHealth = 25.0f;
constexpr float LowHealth = 45.0f;
constexpr float MediumHealth = 65.0f;

CandidatePriority Priority(float healthPct, float distance, bool explicitMainTank, bool tank)
{
    return CalculatePriority(healthPct, distance, 40.0f, CriticalHealth, LowHealth,
                             MediumHealth, explicitMainTank, tank);
}
}

TEST(PartyMemberToHealPriority, CriticalDpsBeatsNoncriticalExplicitMainTank)
{
    EXPECT_TRUE(ShouldReplace(Priority(24.0f, 10.0f, false, false),
                              Priority(25.0f, 10.0f, true, true)));
}

TEST(PartyMemberToHealPriority, ExplicitMainTankThenTankWinWithinSameUrgencyBand)
{
    CandidatePriority const dps = Priority(40.0f, 1.0f, false, false);
    CandidatePriority const tank = Priority(44.0f, 30.0f, false, true);
    CandidatePriority const explicitMainTank = Priority(44.0f, 35.0f, true, true);

    EXPECT_TRUE(ShouldReplace(tank, dps));
    EXPECT_TRUE(ShouldReplace(explicitMainTank, tank));
}

TEST(PartyMemberToHealPriority, NearbyNearlyDeadAllyBeatsHealthierDistantTankInCriticalBand)
{
    CandidatePriority const nearbyAlly = Priority(14.5f, 1.0f, false, false);
    CandidatePriority const distantTank = Priority(18.9f, 56.0f, true, true);

    EXPECT_TRUE(ShouldReplace(nearbyAlly, distantTank));
    EXPECT_FALSE(ShouldReplace(distantTank, nearbyAlly));
}

TEST(PartyMemberToHealPriority, HealthAndDistanceBreakTiesWithinSameBandAndRole)
{
    EXPECT_TRUE(ShouldReplace(Priority(30.0f, 10.0f, false, false),
                              Priority(32.0f, 10.0f, false, false)));
    EXPECT_TRUE(ShouldReplace(Priority(30.0f, 10.0f, false, false),
                              Priority(30.0f, 20.0f, false, false)));
}

TEST(PartyMemberToHealPriority, StableTieKeepsEarlierCandidate)
{
    CandidatePriority const earlierCandidate = Priority(50.0f, 10.0f, false, false);
    CandidatePriority const laterCandidate = Priority(50.0f, 10.0f, false, false);

    EXPECT_FALSE(ShouldReplace(laterCandidate, earlierCandidate));
}

TEST(PartyMemberToHealPriority, OriginalMaximumScoreStillRejectsHealthyFarTarget)
{
    EXPECT_TRUE(IsSelectable(Priority(60.0f, 10.0f, false, false)));
    EXPECT_FALSE(IsSelectable(Priority(75.0f, 50.0f, true, true)));
}

TEST(PartyMemberToHealPriority, RaidIncomingHealReservationBlocksOrdinaryTarget)
{
    EXPECT_FALSE(CanSelectWithIncomingHeal(40.0f, true, true, CriticalHealth, MediumHealth));
    EXPECT_TRUE(CanSelectWithIncomingHeal(40.0f, true, false, CriticalHealth, MediumHealth));
}

TEST(PartyMemberToHealPriority, CriticalRaidTargetCanOverrideIncomingHealReservation)
{
    EXPECT_TRUE(CanSelectWithIncomingHeal(24.0f, true, true, CriticalHealth, MediumHealth));
}

TEST(PartyMemberToHealPriority, NonRaidEmergencyReservationBehaviorIsPreserved)
{
    EXPECT_TRUE(CanSelectWithIncomingHeal(40.0f, false, true, CriticalHealth, MediumHealth));
    EXPECT_FALSE(CanSelectWithIncomingHeal(70.0f, false, true, CriticalHealth, MediumHealth));
}
