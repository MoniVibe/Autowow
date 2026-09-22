/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidAddWavePolicy.h"

#include <algorithm>

#include "gtest/gtest.h"

namespace
{
using namespace RaidAddWavePolicy;

Add WaveAdd(Guid guid, float x, float y, bool controlled = false, std::uint8_t priority = 0)
{
    Add add;
    add.guid = guid;
    add.x = x;
    add.y = y;
    add.controlledBySafeTank = controlled;
    add.priority = priority;
    return add;
}

std::vector<Add> Adds(std::size_t count)
{
    std::vector<Add> adds;
    for (std::size_t index = 0; index < count; ++index)
        adds.push_back(WaveAdd(100 + index, static_cast<float>(index * 20), 0.0f));
    return adds;
}

Member Dps(Guid guid)
{
    Member member;
    member.guid = guid;
    member.isDps = true;
    return member;
}

Member Tank(Guid guid, std::size_t currentLoad = 0, std::size_t capacity = kDefaultTankAddCapacity,
    float x = 0.0f, float y = 0.0f)
{
    Member member;
    member.guid = guid;
    member.isTank = true;
    member.x = x;
    member.y = y;
    member.currentAddLoad = currentLoad;
    member.addCapacity = capacity;
    return member;
}

Add ManagedAdd(Guid guid, float x = 0.0f, float y = 0.0f)
{
    Add add = WaveAdd(guid, x, y);
    add.eligibleForTankAssignment = true;
    return add;
}

std::vector<Add> ManagedAdds(std::size_t count)
{
    std::vector<Add> adds;
    for (std::size_t index = 0; index < count; ++index)
        adds.push_back(ManagedAdd(1000 + index, static_cast<float>(index % 4), static_cast<float>(index / 4)));
    return adds;
}
}

TEST(RaidAddWavePolicy, EntryAndExitUseBoundedHysteresis)
{
    Decision belowEntry = Evaluate({}, {}, Adds(kEntryAddCount - 1));
    EXPECT_EQ(belowEntry.memory.state, State::Inactive);

    Decision entered = Evaluate(belowEntry.memory, {}, Adds(kEntryAddCount));
    EXPECT_EQ(entered.memory.state, State::Gather);
    EXPECT_TRUE(entered.enteredNewEpoch);
    EXPECT_EQ(entered.memory.waveId, 1u);

    Decision held = Evaluate(entered.memory, {}, Adds(kExitAddCount + 1));
    EXPECT_NE(held.memory.state, State::Inactive);
    EXPECT_EQ(held.memory.waveId, entered.memory.waveId);

    Decision exited = Evaluate(held.memory, {}, Adds(kExitAddCount));
    EXPECT_EQ(exited.memory.state, State::Inactive);
    EXPECT_EQ(exited.memory.waveId, entered.memory.waveId);
}

TEST(RaidAddWavePolicy, FocusAndAnchorAreStableAcrossPermutedSnapshots)
{
    std::vector<Add> adds = {
        WaveAdd(30, 1.0f, 0.0f, true, 1), WaveAdd(10, 0.0f, 0.0f, true, 2),
        WaveAdd(20, 2.0f, 0.0f, true, 3), WaveAdd(40, 3.0f, 0.0f, false, 0),
        WaveAdd(50, 30.0f, 0.0f, false, 0), WaveAdd(60, 50.0f, 0.0f, false, 0)};
    Decision first = Evaluate({}, {}, adds);
    ASSERT_EQ(first.memory.state, State::Burn);
    EXPECT_EQ(first.memory.anchorGuid, 10u);
    EXPECT_EQ(first.memory.focusGuid, 20u);

    std::reverse(adds.begin(), adds.end());
    Decision independentlyPermuted = Evaluate({}, {}, adds);
    EXPECT_EQ(independentlyPermuted.memory.anchorGuid, first.memory.anchorGuid);
    EXPECT_EQ(independentlyPermuted.memory.focusGuid, first.memory.focusGuid);
    adds[0].priority = 9; // Transient victim/cast ordering must not churn a valid focus.
    Decision permuted = Evaluate(first.memory, {}, adds);
    EXPECT_EQ(permuted.memory.anchorGuid, first.memory.anchorGuid);
    EXPECT_EQ(permuted.memory.focusGuid, first.memory.focusGuid);
    EXPECT_EQ(permuted.memory.waveId, first.memory.waveId);
}

TEST(RaidAddWavePolicy, GatherAssignsStableSafeTankAnchorAndStackResponders)
{
    Member firstResponder = Dps(10);
    Member looseAddResponder = Dps(20);
    looseAddResponder.hasLooseAddTarget = true;
    std::vector<Add> adds = {
        WaveAdd(100, 0.0f, 0.0f, true), WaveAdd(200, 20.0f, 0.0f),
        WaveAdd(201, 22.0f, 0.0f), WaveAdd(202, 24.0f, 0.0f),
        WaveAdd(203, 40.0f, 0.0f), WaveAdd(204, 60.0f, 0.0f)};

    Decision first = Evaluate({}, {looseAddResponder, firstResponder}, adds);
    ASSERT_EQ(first.memory.state, State::Gather);
    EXPECT_EQ(first.memory.anchorGuid, 100u);
    EXPECT_EQ(first.gatherAnchorGuid, 100u);
    ASSERT_EQ(first.stackResponders.size(), 2u);
    EXPECT_EQ(first.stackResponders[0], firstResponder.guid);
    EXPECT_EQ(first.stackResponders[1], looseAddResponder.guid);
    EXPECT_TRUE(IsStackResponder(first, firstResponder.guid));
    EXPECT_TRUE(IsStackResponder(first, looseAddResponder.guid));

    // A newly controlled candidate has a denser neighborhood, but the still-safe established
    // anchor must not churn and send the raid across the room.
    adds[1].controlledBySafeTank = true;
    Decision held = Evaluate(first.memory, {firstResponder, looseAddResponder}, adds);
    EXPECT_EQ(held.memory.state, State::Gather);
    EXPECT_EQ(held.gatherAnchorGuid, first.gatherAnchorGuid);
}

TEST(RaidAddWavePolicy, GatherStackExcludesProtectedRolesAndClaims)
{
    Member eligible = Dps(10);
    Member healer = Dps(20);
    healer.isHealer = true;
    Member bossOwner = Dps(30);
    bossOwner.ownsBoss = true;
    Member reserved = Dps(40);
    reserved.reservedForBossPressure = true;
    Member emergency = Dps(50);
    emergency.hasEmergencyClaim = true;
    Member tank = Dps(60);
    tank.isTank = true;

    std::vector<Add> adds = {
        WaveAdd(100, 0.0f, 0.0f, true), WaveAdd(101, 20.0f, 0.0f),
        WaveAdd(102, 40.0f, 0.0f), WaveAdd(103, 60.0f, 0.0f),
        WaveAdd(104, 80.0f, 0.0f), WaveAdd(105, 100.0f, 0.0f)};
    Decision decision = Evaluate({}, {emergency, bossOwner, eligible, reserved, healer, tank}, adds);

    ASSERT_EQ(decision.memory.state, State::Gather);
    ASSERT_EQ(decision.stackResponders.size(), 1u);
    EXPECT_EQ(decision.stackResponders.front(), eligible.guid);
    for (Member const& excluded : {emergency, bossOwner, reserved, healer, tank})
        EXPECT_FALSE(IsStackResponder(decision, excluded.guid));
}

TEST(RaidAddWavePolicy, GatherWithoutSafeTankAnchorAssignsNoMovement)
{
    Decision decision = Evaluate({}, {Dps(10)}, Adds(kEntryAddCount));

    ASSERT_EQ(decision.memory.state, State::Gather);
    EXPECT_EQ(decision.memory.anchorGuid, 0u);
    EXPECT_EQ(decision.gatherAnchorGuid, 0u);
    EXPECT_TRUE(decision.stackResponders.empty());
    EXPECT_FALSE(IsStackResponder(decision, 10u));
}

TEST(RaidAddWavePolicy, GatherTransitionsToBurnOnlyAfterClusterAndThreatConsolidate)
{
    std::vector<Add> adds = {
        WaveAdd(10, 0.0f, 0.0f, true), WaveAdd(11, 2.0f, 0.0f),
        WaveAdd(12, 4.0f, 0.0f), WaveAdd(20, 30.0f, 0.0f),
        WaveAdd(21, 50.0f, 0.0f), WaveAdd(22, 70.0f, 0.0f)};
    Decision gather = Evaluate({}, {Dps(100)}, adds);
    ASSERT_EQ(gather.memory.state, State::Gather);
    EXPECT_TRUE(IsStackResponder(gather, 100u));

    adds[1].controlledBySafeTank = true;
    adds[2].controlledBySafeTank = true;
    Decision burn = Evaluate(gather.memory, {Dps(100)}, adds);
    EXPECT_EQ(burn.memory.state, State::Burn);
    EXPECT_EQ(burn.clustered, 3u);
    EXPECT_EQ(burn.controlled, 3u);
    EXPECT_TRUE(burn.stackResponders.empty());
    EXPECT_TRUE(IsResponder(burn, 100u));
}

TEST(RaidAddWavePolicy, BurnRequiresClusterAndConsolidatedTankThreat)
{
    std::vector<Add> clustered = {
        WaveAdd(10, 0.0f, 0.0f, true), WaveAdd(11, 2.0f, 0.0f, true),
        WaveAdd(12, 4.0f, 0.0f, true), WaveAdd(20, 30.0f, 0.0f),
        WaveAdd(21, 50.0f, 0.0f), WaveAdd(22, 70.0f, 0.0f)};
    Decision burn = Evaluate({}, {}, clustered);
    EXPECT_EQ(burn.memory.state, State::Burn);
    EXPECT_EQ(burn.clustered, 3u);
    EXPECT_EQ(burn.controlled, 3u);

    clustered[2].controlledBySafeTank = false;
    Decision gatherForThreat = Evaluate({}, {}, clustered);
    EXPECT_EQ(gatherForThreat.memory.state, State::Gather);

    clustered[2].controlledBySafeTank = true;
    clustered[2].x = 20.0f;
    Decision gatherForSpread = Evaluate({}, {}, clustered);
    EXPECT_EQ(gatherForSpread.memory.state, State::Gather);

    clustered[2].x = 4.0f;
    clustered[2].z = 20.0f;
    Decision gatherForVerticalSeparation = Evaluate({}, {}, clustered);
    EXPECT_EQ(gatherForVerticalSeparation.memory.state, State::Gather);
}

TEST(RaidAddWavePolicy, BurnRespondersExcludeReservedAndNonDpsRoles)
{
    Member eligible = Dps(10);
    Member healer = Dps(20);
    healer.isHealer = true;
    Member bossOwner = Dps(30);
    bossOwner.ownsBoss = true;
    Member reserved = Dps(40);
    reserved.reservedForBossPressure = true;
    Member emergency = Dps(50);
    emergency.hasEmergencyClaim = true;
    Member tank = Dps(60);
    tank.isTank = true;

    std::vector<Add> adds = {
        WaveAdd(100, 0.0f, 0.0f, true), WaveAdd(101, 1.0f, 0.0f, true),
        WaveAdd(102, 2.0f, 0.0f, true), WaveAdd(103, 20.0f, 0.0f),
        WaveAdd(104, 30.0f, 0.0f), WaveAdd(105, 40.0f, 0.0f)};
    Decision decision = Evaluate({}, {emergency, bossOwner, eligible, reserved, healer, tank}, adds);
    ASSERT_EQ(decision.memory.state, State::Burn);
    ASSERT_EQ(decision.responders.size(), 1u);
    EXPECT_EQ(decision.responders.front(), eligible.guid);
}

TEST(RaidAddWavePolicy, CcMarkedAddsAreExcludedFromAnchorFocusAndBurnCluster)
{
    std::vector<Add> adds = {
        WaveAdd(10, 0.0f, 0.0f, true, 1), WaveAdd(11, 1.0f, 0.0f, true, 1),
        WaveAdd(12, 2.0f, 0.0f, true, 1), WaveAdd(13, 3.0f, 0.0f, true, 9),
        WaveAdd(14, 30.0f, 0.0f), WaveAdd(15, 40.0f, 0.0f)};
    adds[3].ccMarked = true;
    Decision decision = Evaluate({}, {}, adds);
    EXPECT_EQ(decision.memory.state, State::Burn);
    EXPECT_EQ(decision.clustered, 3u);
    EXPECT_NE(decision.memory.anchorGuid, 13u);
    EXPECT_NE(decision.memory.focusGuid, 13u);
}

TEST(RaidAddWavePolicy, DangerousCastTemporarilyPreemptsThenReturnsToPersistentFocus)
{
    std::vector<Add> adds = {
        WaveAdd(10, 0.0f, 0.0f, true, 5), WaveAdd(20, 1.0f, 0.0f, true, 1),
        WaveAdd(30, 2.0f, 0.0f, true), WaveAdd(40, 20.0f, 0.0f),
        WaveAdd(50, 30.0f, 0.0f), WaveAdd(60, 40.0f, 0.0f)};
    Decision initial = Evaluate({}, {}, adds);
    ASSERT_EQ(initial.memory.focusGuid, 10u);

    adds[1].dangerousCast = true;
    adds[1].dangerPriority = kMaterialDangerPriority;
    Decision preempted = Evaluate(initial.memory, {}, adds);
    EXPECT_TRUE(preempted.temporaryPreemption);
    EXPECT_EQ(preempted.responseTargetGuid, 20u);
    EXPECT_EQ(preempted.memory.focusGuid, 10u);

    adds[1].dangerousCast = false;
    Decision resumed = Evaluate(preempted.memory, {}, adds);
    EXPECT_FALSE(resumed.temporaryPreemption);
    EXPECT_EQ(resumed.responseTargetGuid, 10u);
    EXPECT_EQ(resumed.memory.focusGuid, 10u);
    EXPECT_TRUE(MayReplaceStickyTarget(preempted.memory, resumed, 20, 10));
    Decision resumedActionPass = Evaluate(resumed.memory, {}, adds);
    EXPECT_TRUE(MayReplaceStickyTarget(resumed.memory, resumedActionPass, 20, 10));
}

TEST(RaidAddWavePolicy, StickyFocusDoesNotRetargetBeforeInvalidationOrEpochChange)
{
    std::vector<Add> adds = {
        WaveAdd(10, 0.0f, 0.0f, true, 5), WaveAdd(20, 1.0f, 0.0f, true, 1),
        WaveAdd(30, 2.0f, 0.0f, true), WaveAdd(40, 20.0f, 0.0f),
        WaveAdd(50, 30.0f, 0.0f), WaveAdd(60, 40.0f, 0.0f)};
    Decision initial = Evaluate({}, {}, adds);
    adds[1].priority = 9;
    Decision transient = Evaluate(initial.memory, {}, adds);
    EXPECT_EQ(transient.memory.focusGuid, 10u);
    EXPECT_FALSE(MayReplaceStickyTarget(initial.memory, transient, 10, 20));

    adds.erase(adds.begin());
    Decision replaced = Evaluate(transient.memory, {}, adds);
    EXPECT_TRUE(replaced.focusChangedAfterInvalidation);
    EXPECT_EQ(replaced.memory.focusGuid, 20u);
    EXPECT_TRUE(MayReplaceStickyTarget(transient.memory, replaced, 10, 20));
    Decision replacementActionPass = Evaluate(replaced.memory, {}, adds);
    EXPECT_TRUE(MayReplaceStickyTarget(replaced.memory, replacementActionPass, 10, 20));

    Decision exited = Evaluate(replacementActionPass.memory, {}, Adds(kExitAddCount));
    Decision nextEpoch = Evaluate(exited.memory, {}, Adds(kEntryAddCount));
    EXPECT_GT(nextEpoch.memory.waveId, replacementActionPass.memory.waveId);
    EXPECT_TRUE(MayReplaceStickyTarget(exited.memory, nextEpoch, 20, nextEpoch.memory.focusGuid));
}

TEST(RaidAddWavePolicy, ReportedWaveOverloadHoldsBurnForTwoTanksAndElevenVisibleAdds)
{
    Decision decision = Evaluate({}, {Tank(10), Tank(20)}, ManagedAdds(11), 47);

    EXPECT_EQ(decision.total, 11u);
    EXPECT_EQ(decision.reportedTotal, 47u);
    EXPECT_EQ(decision.eligibleAdds, 11u);
    EXPECT_GE(decision.unassignedAdds, 36u);
    EXPECT_FALSE(decision.addCapacityReady);
    EXPECT_NE(decision.memory.state, State::Burn);
}

TEST(RaidAddWavePolicy, OverBudgetTankPreventsBurnEvenWithAConsolidatedCluster)
{
    std::vector<Add> adds;
    for (std::size_t index = 0; index < 6; ++index)
    {
        Add add = ManagedAdd(200 + index, static_cast<float>(index), 0.0f);
        add.controlledBySafeTank = index < 3;
        add.currentTankGuid = index < 3 ? 10 : 0;
        adds.push_back(add);
    }

    Decision decision = Evaluate({}, {Tank(10, 3, 2), Tank(20, 0, 4)}, adds);

    EXPECT_EQ(decision.overBudgetTanks, 1u);
    EXPECT_FALSE(decision.addCapacityReady);
    EXPECT_EQ(decision.memory.state, State::Gather);
}

TEST(RaidAddWavePolicy, TankClaimsRemainStableAcrossPermutation)
{
    std::vector<Member> members = {Tank(20, 0, 6), Tank(10, 0, 6)};
    std::vector<Add> adds = ManagedAdds(7);
    Decision first = Evaluate({}, members, adds);
    ASSERT_EQ(first.tankClaims.size(), adds.size());

    std::reverse(members.begin(), members.end());
    std::reverse(adds.begin(), adds.end());
    Decision permuted = Evaluate(first.memory, members, adds);
    ASSERT_EQ(permuted.tankClaims.size(), first.tankClaims.size());
    for (std::size_t index = 0; index < first.tankClaims.size(); ++index)
    {
        EXPECT_EQ(permuted.tankClaims[index].waveId, first.tankClaims[index].waveId);
        EXPECT_EQ(permuted.tankClaims[index].addGuid, first.tankClaims[index].addGuid);
        EXPECT_EQ(permuted.tankClaims[index].tankGuid, first.tankClaims[index].tankGuid);
    }
}

TEST(RaidAddWavePolicy, InvalidatedTankClaimReassignsWithoutRetainingTheUnsafeTank)
{
    std::vector<Member> members = {Tank(10, 0, 6), Tank(20, 0, 6)};
    std::vector<Add> adds = ManagedAdds(6);
    Decision first = Evaluate({}, members, adds);
    ASSERT_FALSE(first.tankClaims.empty());
    Guid const invalidatedAdd = first.tankClaims.front().addGuid;

    members.front().safeForAdds = false;
    Decision reassigned = Evaluate(first.memory, members, adds);
    auto const found = std::find_if(reassigned.tankClaims.begin(), reassigned.tankClaims.end(),
        [invalidatedAdd](AddClaim const& claim) { return claim.addGuid == invalidatedAdd; });
    ASSERT_NE(found, reassigned.tankClaims.end());
    EXPECT_EQ(found->tankGuid, 20u);
    EXPECT_TRUE(std::all_of(reassigned.tankClaims.begin(), reassigned.tankClaims.end(),
        [](AddClaim const& claim) { return claim.tankGuid != 10; }));
}

TEST(RaidAddWavePolicy, BossOwnerHealerReservationAndCcClaimsArePreserved)
{
    Member bossOwner = Tank(10);
    bossOwner.ownsBoss = true;
    Member safeTank = Tank(20);
    Member healer = Dps(30);
    healer.isHealer = true;
    Member reserved = Dps(40);
    reserved.reservedForBossPressure = true;

    std::vector<Add> adds = ManagedAdds(6);
    adds.back().ccMarked = true;
    Decision decision = Evaluate({}, {reserved, healer, bossOwner, safeTank}, adds);

    EXPECT_EQ(decision.eligibleAdds, 5u);
    EXPECT_EQ(decision.tankClaims.size(), 5u);
    EXPECT_TRUE(decision.addCapacityReady);
    for (AddClaim const& claim : decision.tankClaims)
        EXPECT_EQ(claim.tankGuid, safeTank.guid);
}
