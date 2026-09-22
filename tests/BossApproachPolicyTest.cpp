/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#include "../src/AutoWow/BossApproachPolicy.h"

#include <algorithm>
#include <set>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowBossApproachPolicy;

CohesionFacts ReadyFacts()
{
    CohesionFacts facts;
    facts.rosterMembers = 5;
    facts.livingMembers = 5;
    facts.onlinePlayerbots = 5;
    facts.tankMembers = 1;
    facts.sameContext = 5;
    facts.transientFree = 5;
    facts.withinCohesion = 5;
    facts.slotReady = 5;
    facts.exactRangeAndLos = 5;
    facts.targetVisible = true;
    facts.pathsSafe = true;
    return facts;
}
}

TEST(BossApproachPolicy, RoleSlotsAreDeterministicAndPutTheMainTankFirst)
{
    std::vector<MemberFacts> input = {
        {40, true, false, false, true, false},
        {30, true, false, false, false, true},
        {20, true, false, false, false, false},
        {11, true, false, true, false, false},
        {10, true, true, true, false, false},
    };

    std::vector<FormationSlot> first = BuildFormationSlots(input);
    std::reverse(input.begin(), input.end());
    std::vector<FormationSlot> second = BuildFormationSlots(input);

    ASSERT_EQ(first.size(), 5u);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index].memberGuid, second[index].memberGuid);
        EXPECT_EQ(first[index].role, second[index].role);
        EXPECT_FLOAT_EQ(first[index].trailing, second[index].trailing);
        EXPECT_FLOAT_EQ(first[index].lateral, second[index].lateral);
    }

    EXPECT_EQ(first[0].memberGuid, 10u);
    EXPECT_EQ(first[0].role, Role::MainTank);
    EXPECT_LT(first[1].trailing, first[2].trailing);
    EXPECT_LT(first[2].trailing, first[3].trailing);
    EXPECT_LT(first[3].trailing, first[4].trailing);
}

TEST(BossApproachPolicy, FortyMemberRaidReceivesUniqueBoundedRoleSlots)
{
    std::vector<MemberFacts> input;
    for (std::uint64_t guid = 1; guid <= 40; ++guid)
        input.push_back({guid, true, guid == 1, guid <= 2, guid > 30, guid > 20 && guid <= 30});

    std::vector<FormationSlot> slots = BuildFormationSlots(input);
    std::set<std::uint64_t> guids;
    for (FormationSlot const& slot : slots)
    {
        guids.insert(slot.memberGuid);
        EXPECT_GE(slot.trailing, 0.0f);
        EXPECT_LE(slot.trailing, 70.0f);
    }
    EXPECT_EQ(slots.size(), 40u);
    EXPECT_EQ(guids.size(), 40u);
    EXPECT_EQ(slots.front().role, Role::MainTank);
}

TEST(BossApproachPolicy, ALeaderAloneCannotBecomeReady)
{
    CohesionFacts facts = ReadyFacts();
    facts.rosterMembers = 1;
    facts.livingMembers = 1;
    facts.onlinePlayerbots = 1;
    facts.sameContext = 1;
    facts.transientFree = 1;
    facts.withinCohesion = 1;
    facts.slotReady = 1;
    facts.exactRangeAndLos = 1;

    Decision const decision = Evaluate(facts);
    EXPECT_EQ(decision.phase, Phase::Blocked);
    EXPECT_EQ(decision.reason, "insufficient_living_cohort");
}

TEST(BossApproachPolicy, CohortWithoutLivingTankCannotPull)
{
    CohesionFacts facts = ReadyFacts();
    facts.tankMembers = 0;

    Decision const decision = Evaluate(facts);
    EXPECT_EQ(decision.phase, Phase::Blocked);
    EXPECT_EQ(decision.reason, "no_tank_forward_probe");
}

TEST(BossApproachPolicy, DeadOrTransientMembersHoldTheCohortAtTheGate)
{
    CohesionFacts facts = ReadyFacts();
    facts.livingMembers = 4;
    Decision dead = Evaluate(facts);
    EXPECT_EQ(dead.phase, Phase::Assembling);
    EXPECT_EQ(dead.reason, "dead_party_member");

    facts = ReadyFacts();
    facts.transientFree = 4;
    Decision transient = Evaluate(facts);
    EXPECT_EQ(transient.phase, Phase::Assembling);
    EXPECT_EQ(transient.reason, "member_teleporting_or_falling");
}

TEST(BossApproachPolicy, ProgressAndStallPhasesAreExplicit)
{
    CohesionFacts facts = ReadyFacts();
    facts.slotReady = 2;
    facts.withinCohesion = 2;
    facts.movementIssued = true;
    facts.progressMade = true;
    EXPECT_EQ(Evaluate(facts).phase, Phase::Advancing);

    facts.progressMade = false;
    facts.stalledPolls = kMaxStalledPolls;
    Decision const stalled = Evaluate(facts);
    EXPECT_EQ(stalled.phase, Phase::Stalled);
    EXPECT_EQ(stalled.reason, "cohort_no_progress");

    facts = ReadyFacts();
    EXPECT_EQ(Evaluate(facts).phase, Phase::Ready);
}
