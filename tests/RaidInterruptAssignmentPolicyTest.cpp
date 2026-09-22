/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidPriorityAddPolicy.h"

#include <algorithm>
#include <set>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace RaidPriorityAddPolicy;

RaidMember Member(Guid guid, bool dps, bool tank, bool healer, bool ranged, bool canInterrupt)
{
    RaidMember member;
    member.guid = guid;
    member.isDps = dps;
    member.isTank = tank;
    member.isHealer = healer;
    member.isRanged = ranged;
    member.canInterrupt = canInterrupt;
    return member;
}

AddCandidate Add(Guid guid, bool casting)
{
    AddCandidate add;
    add.guid = guid;
    add.threatBacked = true;
    add.isCasting = casting;
    return add;
}

std::vector<Assignment> Interrupts(Plan const& plan)
{
    std::vector<Assignment> assignments;
    for (Assignment const& assignment : plan.assignments)
        if (assignment.kind == AssignmentKind::Interrupt)
            assignments.push_back(assignment);
    return assignments;
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
        EXPECT_EQ(left.assignments[index].interruptRank, right.assignments[index].interruptRank);
    }
}
}

TEST(RaidInterruptAssignmentPolicy, DistributesThreeCastingAddsAcrossDistinctInterruptersByGuid)
{
    std::vector<RaidMember> const members = {
        Member(30, true, false, false, false, true),
        Member(10, true, false, false, false, true),
        Member(20, true, false, false, false, true)
    };
    std::vector<Assignment> const interrupts = Interrupts(BuildPlan(members,
        {Add(8335, true), Add(8310, true), Add(8316, true)}, false, false));

    ASSERT_EQ(interrupts.size(), 3u);
    EXPECT_EQ(interrupts[0].memberGuid, 10u);
    EXPECT_EQ(interrupts[0].addGuid, 8310u);
    EXPECT_EQ(interrupts[1].memberGuid, 20u);
    EXPECT_EQ(interrupts[1].addGuid, 8316u);
    EXPECT_EQ(interrupts[2].memberGuid, 30u);
    EXPECT_EQ(interrupts[2].addGuid, 8335u);
    for (Assignment const& assignment : interrupts)
    {
        EXPECT_EQ(assignment.interruptRank, kPrimaryInterruptRank);
        EXPECT_STREQ(InterruptAssignmentRankName(assignment.interruptRank), "primary");
    }
}

TEST(RaidInterruptAssignmentPolicy, NeverAssignsAHealerEvenWhenTheClassCanInterrupt)
{
    Plan const plan = BuildPlan({
        Member(1, false, false, true, true, true),
        Member(2, true, false, false, false, true)
    }, {Add(100, true)}, false, false);

    std::vector<Assignment> const interrupts = Interrupts(plan);
    ASSERT_EQ(interrupts.size(), 1u);
    EXPECT_EQ(interrupts[0].memberGuid, 2u);
    EXPECT_EQ(plan.FindAssignment(1), nullptr);
}

TEST(RaidInterruptAssignmentPolicy, PrefersDpsOverLowerGuidTank)
{
    std::vector<Assignment> const interrupts = Interrupts(BuildPlan({
        Member(1, false, true, false, false, true),
        Member(99, true, false, false, false, true)
    }, {Add(100, true)}, false, false));

    ASSERT_EQ(interrupts.size(), 1u);
    EXPECT_EQ(interrupts[0].memberGuid, 99u);
}

TEST(RaidInterruptAssignmentPolicy, InsufficientInterruptersCoverLowestGuidCastsWithoutDuplicates)
{
    std::vector<Assignment> const interrupts = Interrupts(BuildPlan({
        Member(10, true, false, false, false, true),
        Member(20, true, false, false, false, true)
    }, {Add(300, true), Add(100, true), Add(200, true)}, false, false));

    ASSERT_EQ(interrupts.size(), 2u);
    EXPECT_EQ(interrupts[0].addGuid, 100u);
    EXPECT_EQ(interrupts[1].addGuid, 200u);
    std::set<Guid> assignedMembers;
    for (Assignment const& assignment : interrupts)
        EXPECT_TRUE(assignedMembers.insert(assignment.memberGuid).second);
}

TEST(RaidInterruptAssignmentPolicy, OneMemberNeverReceivesMultipleSimultaneousCastAssignments)
{
    std::vector<Assignment> const interrupts = Interrupts(BuildPlan({
        Member(10, true, false, false, false, true),
        Member(20, true, false, false, false, true),
        Member(30, false, true, false, false, true)
    }, {Add(100, true), Add(200, true), Add(300, true)}, false, false));

    std::set<Guid> assignedMembers;
    for (Assignment const& assignment : interrupts)
        EXPECT_TRUE(assignedMembers.insert(assignment.memberGuid).second);
    EXPECT_EQ(assignedMembers.size(), interrupts.size());
}

TEST(RaidInterruptAssignmentPolicy, PreservesRangedBossReserveWhenCoverageAllows)
{
    Plan const plan = BuildPlan({
        Member(10, true, false, false, true, true),
        Member(20, true, false, false, true, true),
        Member(30, true, false, false, true, true)
    }, {Add(100, true), Add(200, true)}, true, true);

    EXPECT_EQ(plan.bossPressureReserveGuid, 10u);
    std::vector<Assignment> const interrupts = Interrupts(plan);
    ASSERT_EQ(interrupts.size(), 2u);
    EXPECT_EQ(interrupts[0].memberGuid, 20u);
    EXPECT_EQ(interrupts[1].memberGuid, 30u);
}

TEST(RaidInterruptAssignmentPolicy, PlanIsPermutationStable)
{
    std::vector<RaidMember> members = {
        Member(40, false, true, false, false, true),
        Member(20, true, false, false, true, true),
        Member(10, true, false, false, true, false),
        Member(30, true, false, false, false, true),
        Member(5, false, false, true, true, true)
    };
    std::vector<AddCandidate> adds = {Add(300, true), Add(100, true), Add(200, true)};
    Plan const expected = BuildPlan(members, adds, true, true);

    std::reverse(members.begin(), members.end());
    std::rotate(adds.begin(), adds.begin() + 1, adds.end());
    ExpectSamePlan(expected, BuildPlan(members, adds, true, true));

    std::rotate(members.begin(), members.begin() + 2, members.end());
    std::reverse(adds.begin(), adds.end());
    ExpectSamePlan(expected, BuildPlan(members, adds, true, true));
}

TEST(RaidInterruptAssignmentPolicy, CastEndReleasesResponderBackToNormalFocus)
{
    std::vector<RaidMember> const members = {
        Member(10, true, false, false, false, true),
        Member(20, true, false, false, false, true)
    };

    Plan const casting = BuildPlan(members, {Add(100, true)}, false, false);
    Assignment const* active = casting.FindAssignment(10);
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->kind, AssignmentKind::Interrupt);

    Plan const ended = BuildPlan(members, {Add(100, false)}, false, false);
    EXPECT_TRUE(Interrupts(ended).empty());
    Assignment const* resumed = ended.FindAssignment(10);
    ASSERT_NE(resumed, nullptr);
    EXPECT_EQ(resumed->kind, AssignmentKind::DpsFocus);
    EXPECT_EQ(resumed->interruptRank, 0u);
}
