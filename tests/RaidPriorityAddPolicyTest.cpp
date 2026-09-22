/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidPriorityAddPolicy.h"

#include <algorithm>

#include "gtest/gtest.h"

namespace
{
using namespace RaidPriorityAddPolicy;

RaidMember Tank(Guid guid, bool mainTank = false)
{
    RaidMember member;
    member.guid = guid;
    member.isTank = true;
    member.isMainTank = mainTank;
    return member;
}

RaidMember BossOwnerTank(Guid guid, bool mainTank = false)
{
    RaidMember member = Tank(guid, mainTank);
    member.ownsActiveBoss = true;
    return member;
}

RaidMember Healer(Guid guid)
{
    RaidMember member;
    member.guid = guid;
    member.isHealer = true;
    member.isRanged = true;
    return member;
}

RaidMember Dps(Guid guid, bool ranged)
{
    RaidMember member;
    member.guid = guid;
    member.isDps = true;
    member.isRanged = ranged;
    return member;
}

AddCandidate Add(Guid guid, Guid victimGuid = 0, bool casting = false, bool elite = false,
    bool threatBacked = true)
{
    return {guid, victimGuid, threatBacked, casting, elite};
}

std::vector<Assignment> AssignmentsOfKind(Plan const& plan, AssignmentKind kind)
{
    std::vector<Assignment> result;
    for (Assignment const& assignment : plan.assignments)
        if (assignment.kind == kind)
            result.push_back(assignment);
    return result;
}

void ExpectSamePlan(Plan const& left, Plan const& right)
{
    EXPECT_EQ(left.focusAddGuid, right.focusAddGuid);
    EXPECT_EQ(left.bossPressureReserveGuid, right.bossPressureReserveGuid);
    ASSERT_EQ(left.assignments.size(), right.assignments.size());
    for (std::size_t index = 0; index < left.assignments.size(); ++index)
    {
        EXPECT_EQ(left.assignments[index].memberGuid, right.assignments[index].memberGuid);
        EXPECT_EQ(left.assignments[index].addGuid, right.assignments[index].addGuid);
        EXPECT_EQ(left.assignments[index].kind, right.assignments[index].kind);
    }
}
}

TEST(RaidPriorityAddPolicy, PriorityIsHealerThenOtherNonTankThenCastingThenEliteThenGuid)
{
    std::vector<RaidMember> const members = {Tank(1), Healer(10), Dps(20, false), Dps(21, false)};
    std::vector<AddCandidate> adds = {
        Add(500, 0, false, true),
        Add(400, 0, true, false),
        Add(300, 20, false, false),
        Add(200, 10, false, false)
    };

    EXPECT_EQ(BuildPlan(members, adds, true, true).focusAddGuid, 200u);
    adds.erase(adds.begin() + 3);
    EXPECT_EQ(BuildPlan(members, adds, true, true).focusAddGuid, 300u);
    adds.erase(adds.begin() + 2);
    EXPECT_EQ(BuildPlan(members, adds, true, true).focusAddGuid, 400u);

    adds.push_back(Add(350, 0, true, false));
    EXPECT_EQ(BuildPlan(members, adds, true, true).focusAddGuid, 350u);
}

TEST(RaidPriorityAddPolicy, IgnoresCandidatesWithoutObservableThreatBacking)
{
    std::vector<RaidMember> const members = {Healer(10), Dps(20, false)};
    Plan const plan = BuildPlan(members,
        {Add(1, 10, true, true, false), Add(2, 0, false, false, true)}, false, false);

    EXPECT_EQ(plan.focusAddGuid, 2u);
    for (Assignment const& assignment : plan.assignments)
        EXPECT_NE(assignment.addGuid, 1u);
    EXPECT_EQ(BuildPlan(members, {Add(1, 10, true, true, false)}, false, false).focusAddGuid, 0u);
}

TEST(RaidPriorityAddPolicy, AssignsAtMostOneUncontrolledAddPerAvailableTank)
{
    std::vector<RaidMember> const members = {
        Tank(1), Tank(2), Tank(3, true), Tank(4), Healer(10), Dps(20, false)
    };
    std::vector<AddCandidate> const adds = {
        Add(100, 10),
        Add(101, 20),
        Add(102, 4),
        Add(103)
    };

    std::vector<Assignment> const pickups =
        AssignmentsOfKind(BuildPlan(members, adds, true, true), AssignmentKind::TankPickup);
    ASSERT_EQ(pickups.size(), 2u);
    EXPECT_EQ(pickups[0].memberGuid, 1u);
    EXPECT_EQ(pickups[0].addGuid, 100u);
    EXPECT_EQ(pickups[1].memberGuid, 2u);
    EXPECT_EQ(pickups[1].addGuid, 101u);
    EXPECT_NE(pickups[0].memberGuid, pickups[1].memberGuid);
    for (Assignment const& pickup : pickups)
    {
        EXPECT_NE(pickup.memberGuid, 3u);  // Main tank remains on the active boss.
        EXPECT_NE(pickup.memberGuid, 4u);  // This tank already controls add 102.
        EXPECT_NE(pickup.addGuid, 102u);
    }
}

TEST(RaidPriorityAddPolicy, MainTankCanPickUpOnlyWhenBossHasNoVictim)
{
    std::vector<RaidMember> const members = {Tank(1, true), Healer(10)};
    std::vector<AddCandidate> const adds = {Add(100, 10)};

    EXPECT_TRUE(AssignmentsOfKind(
        BuildPlan(members, adds, true, true), AssignmentKind::TankPickup).empty());

    std::vector<Assignment> const passiveBossPickups = AssignmentsOfKind(
        BuildPlan(members, adds, true, false), AssignmentKind::TankPickup);
    ASSERT_EQ(passiveBossPickups.size(), 1u);
    EXPECT_EQ(passiveBossPickups[0].memberGuid, 1u);
    EXPECT_EQ(passiveBossPickups[0].addGuid, 100u);
}

TEST(RaidPriorityAddPolicy, AirborneEngagedBossWithoutVictimStillReservesSafeRangedPressure)
{
    std::vector<RaidMember> const members = {
        Tank(1, true), Healer(10), Dps(20, true), Dps(30, false), Dps(40, false)
    };

    Plan const plan = BuildPlan(members, {Add(100, 10)}, true, false);

    EXPECT_EQ(plan.bossPressureReserveGuid, 20u);
    EXPECT_EQ(plan.FindAssignment(20), nullptr);
}

TEST(RaidPriorityAddPolicy, GenuineNoBossDoesNotReserveRangedDps)
{
    std::vector<RaidMember> const members = {
        Tank(1), Healer(10), Dps(20, true), Dps(30, false)
    };

    Plan const plan = BuildPlan(members, {Add(100, 10)}, false, false);

    EXPECT_EQ(plan.bossPressureReserveGuid, 0u);
    ASSERT_NE(plan.FindAssignment(20), nullptr);
    EXPECT_EQ(plan.FindAssignment(20)->kind, AssignmentKind::DpsFocus);
}

TEST(RaidPriorityAddPolicy, BossReservePreservesFullEmergencyAddDpsBudget)
{
    std::vector<RaidMember> const members = {
        Tank(1, true), Healer(10), Dps(20, true), Dps(30, true), Dps(40, false)
    };

    // DPS 30 is already defending itself from the priority add, so it is not a safe boss reserve.
    // DPS 20 stays on the engaged transition boss while DPS 30 and 40 retain the existing full
    // two-responder emergency add budget.
    Plan const plan = BuildPlan(members, {Add(100, 30)}, true, false);

    EXPECT_EQ(plan.bossPressureReserveGuid, 20u);
    std::vector<Assignment> const responders =
        AssignmentsOfKind(plan, AssignmentKind::DpsFocus);
    ASSERT_EQ(responders.size(), kMaxDpsResponders);
    EXPECT_EQ(responders[0].memberGuid, 30u);
    EXPECT_EQ(responders[1].memberGuid, 40u);
}

TEST(RaidPriorityAddPolicy, LiveBossOwnerIsReservedEvenWhenStaticMainTankMetadataIsWrong)
{
    std::vector<RaidMember> const members = {
        BossOwnerTank(1), Tank(2, true), Tank(3), Healer(10), Dps(20, false)
    };

    std::vector<Assignment> const pickups = AssignmentsOfKind(
        BuildPlan(members, {Add(100, 10)}, true, true), AssignmentKind::TankPickup);
    ASSERT_EQ(pickups.size(), 1u);
    EXPECT_EQ(pickups[0].memberGuid, 3u);
    EXPECT_EQ(pickups[0].addGuid, 100u);
}

TEST(RaidPriorityAddPolicy, UnknownPetOrTotemVictimDoesNotConsumeAHealthyRaidTank)
{
    std::vector<RaidMember> const members = {BossOwnerTank(1), Tank(2), Healer(10), Dps(20, false)};

    EXPECT_TRUE(AssignmentsOfKind(
        BuildPlan(members, {Add(100, 999999)}, true, true), AssignmentKind::TankPickup).empty());
}

TEST(RaidPriorityAddPolicy, BossOwnerCannotBeAssignedInterruptOrDpsResponse)
{
    RaidMember owner = Dps(20, true);
    owner.canInterrupt = true;
    owner.ownsActiveBoss = true;
    RaidMember responder = Dps(21, false);
    responder.canInterrupt = true;

    Plan const plan = BuildPlan({owner, responder, Healer(10)}, {Add(100, 10, true)}, true, true);
    EXPECT_EQ(plan.FindAssignment(owner.guid), nullptr);
    ASSERT_NE(plan.FindAssignment(responder.guid), nullptr);
    EXPECT_EQ(plan.FindAssignment(responder.guid)->kind, AssignmentKind::Interrupt);
}

TEST(RaidPriorityAddPolicy, SharedFocusCapsDpsAndCountsDirectVictimWhileReservingSafeRanged)
{
    std::vector<RaidMember> const members = {
        Healer(1), Tank(2), Dps(20, true), Dps(30, true), Dps(40, false), Dps(50, true)
    };
    Plan const plan = BuildPlan(members, {Add(100, 30)}, true, true);

    EXPECT_EQ(plan.focusAddGuid, 100u);
    EXPECT_EQ(plan.bossPressureReserveGuid, 20u);
    EXPECT_EQ(plan.FindAssignment(1), nullptr);  // Healers never receive an attack assignment.

    std::vector<Assignment> const responders =
        AssignmentsOfKind(plan, AssignmentKind::DpsFocus);
    ASSERT_EQ(responders.size(), kMaxDpsResponders);
    EXPECT_EQ(responders[0].memberGuid, 30u);
    EXPECT_EQ(responders[1].memberGuid, 40u);
    for (Assignment const& responder : responders)
    {
        EXPECT_EQ(responder.addGuid, plan.focusAddGuid);
        EXPECT_NE(responder.memberGuid, plan.bossPressureReserveGuid);
        EXPECT_NE(responder.memberGuid, 1u);
    }
}

TEST(RaidPriorityAddPolicy, PlanIsStableAcrossMemberAndAddPermutations)
{
    std::vector<RaidMember> members = {
        Tank(1), Tank(2), Tank(3, true), Healer(10), Dps(20, true), Dps(30, false), Dps(40, true)
    };
    std::vector<AddCandidate> adds = {
        Add(300, 0, false, true), Add(100, 10), Add(200, 30, true, false)
    };
    Plan const expected = BuildPlan(members, adds, true, true);

    std::reverse(members.begin(), members.end());
    std::rotate(adds.begin(), adds.begin() + 1, adds.end());
    ExpectSamePlan(expected, BuildPlan(members, adds, true, true));

    std::rotate(members.begin(), members.begin() + 2, members.end());
    std::reverse(adds.begin(), adds.end());
    ExpectSamePlan(expected, BuildPlan(members, adds, true, true));
}

TEST(RaidPriorityAddPolicy, ExistingAssignmentResolvesToHoldInsteadOfAnotherAcquire)
{
    EXPECT_EQ(ResolveTargetDisposition(100, 0), TargetDisposition::AcquireAssignedTarget);
    EXPECT_EQ(ResolveTargetDisposition(100, 100), TargetDisposition::HoldAssignedTarget);
    EXPECT_EQ(ResolveTargetDisposition(0, 100), TargetDisposition::NoAssignment);
}
