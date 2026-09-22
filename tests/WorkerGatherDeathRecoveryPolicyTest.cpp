/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "WorkerGatherDeathRecoveryPolicy.h"

#include "gtest/gtest.h"

namespace
{
using AutoWowGather::DeathRecoveryAction;
using AutoWowGather::DeathRecoveryObservation;
using AutoWowGather::DeathRecoveryPhase;
using AutoWowGather::DeathRecoveryState;
using AutoWowGather::DeathRecoveryTransition;

DeathRecoveryObservation Dead(bool ghost, bool corpseAvailable, bool corpseNear,
                              bool routeInProgress, std::uint64_t now)
{
    return {true, false, ghost, corpseAvailable, corpseNear, routeInProgress, now};
}

DeathRecoveryTransition Plan(DeathRecoveryState const& state, DeathRecoveryObservation observation)
{
    return AutoWowGather::EvaluateDeathRecovery(state, observation);
}
}

TEST(WorkerGatherDeathRecoveryPolicy, ReleasesOnceThenUsesBackoff)
{
    DeathRecoveryState state;
    DeathRecoveryTransition transition = Plan(state, Dead(false, true, false, false, 100));
    EXPECT_EQ(transition.action, DeathRecoveryAction::ReleaseSpirit);
    EXPECT_EQ(transition.next.phase, DeathRecoveryPhase::ReleasePending);
    EXPECT_EQ(transition.next.releaseAttempts, 1u);

    transition = Plan(transition.next, Dead(false, true, false, false, 101));
    EXPECT_EQ(transition.action, DeathRecoveryAction::Wait);
    EXPECT_EQ(transition.next.releaseAttempts, 1u);

    transition = Plan(transition.next, Dead(false, true, false, false, 103));
    EXPECT_EQ(transition.action, DeathRecoveryAction::ReleaseSpirit);
    EXPECT_EQ(transition.next.releaseAttempts, 2u);
}

TEST(WorkerGatherDeathRecoveryPolicy, GhostWalkAndReclaimAreEachBounded)
{
    DeathRecoveryState state;
    DeathRecoveryTransition transition = Plan(state, Dead(true, true, false, false, 200));
    EXPECT_EQ(transition.action, DeathRecoveryAction::WalkToCorpse);
    EXPECT_EQ(transition.next.routeAttempts, 1u);

    transition = Plan(transition.next, Dead(true, true, false, true, 201));
    EXPECT_EQ(transition.action, DeathRecoveryAction::Wait);
    EXPECT_EQ(transition.next.routeAttempts, 1u);

    transition = Plan(transition.next, Dead(true, true, true, false, 215));
    EXPECT_EQ(transition.action, DeathRecoveryAction::ReclaimCorpse);
    EXPECT_EQ(transition.next.reclaimAttempts, 1u);

    transition = Plan(transition.next, Dead(true, true, true, false, 216));
    EXPECT_EQ(transition.action, DeathRecoveryAction::Wait);
    EXPECT_EQ(transition.next.reclaimAttempts, 1u);
}

TEST(WorkerGatherDeathRecoveryPolicy, AliveObservationCompletesRecoveryAndNewDeathGetsFreshBudget)
{
    DeathRecoveryState state;
    DeathRecoveryTransition transition = Plan(state, Dead(false, true, false, false, 300));
    ASSERT_EQ(transition.action, DeathRecoveryAction::ReleaseSpirit);

    transition = Plan(transition.next, {true, true, false, false, false, false, 304});
    EXPECT_EQ(transition.action, DeathRecoveryAction::None);
    EXPECT_FALSE(transition.next.active);
    EXPECT_EQ(transition.next.phase, DeathRecoveryPhase::Recovered);

    transition = Plan(transition.next, Dead(false, true, false, false, 400));
    EXPECT_EQ(transition.action, DeathRecoveryAction::ReleaseSpirit);
    EXPECT_EQ(transition.next.releaseAttempts, 1u);
    EXPECT_EQ(transition.next.routeAttempts, 0u);
    EXPECT_EQ(transition.next.reclaimAttempts, 0u);
}

TEST(WorkerGatherDeathRecoveryPolicy, RouteAndReclaimAttemptLimitsFailClosed)
{
    DeathRecoveryState state;
    state.active = true;
    state.phase = DeathRecoveryPhase::CorpseRoutePending;
    state.startedAtSeconds = 500;
    state.nextActionAtSeconds = 500;
    state.routeAttempts = AutoWowGather::MaxCorpseRouteAttempts;

    DeathRecoveryTransition transition = Plan(state, Dead(true, true, false, false, 500));
    EXPECT_EQ(transition.action, DeathRecoveryAction::Wait);
    EXPECT_EQ(transition.next.phase, DeathRecoveryPhase::Blocked);
    EXPECT_EQ(transition.next.event, AutoWowGather::DeathRecoveryEvent::Blocked);

    state = {};
    state.active = true;
    state.phase = DeathRecoveryPhase::ReclaimPending;
    state.startedAtSeconds = 600;
    state.nextActionAtSeconds = 600;
    state.reclaimAttempts = AutoWowGather::MaxReclaimAttempts;
    transition = Plan(state, Dead(true, true, true, false, 600));
    EXPECT_EQ(transition.next.phase, DeathRecoveryPhase::Blocked);
}

TEST(WorkerGatherDeathRecoveryPolicy, NonWorkerIsStrictNoOp)
{
    DeathRecoveryState state;
    DeathRecoveryObservation observation = Dead(false, true, false, false, 700);
    observation.explicitWorker = false;
    DeathRecoveryTransition const transition = Plan(state, observation);
    EXPECT_EQ(transition.action, DeathRecoveryAction::None);
    EXPECT_EQ(transition.next.eventSequence, 0u);
    EXPECT_EQ(transition.next.phase, DeathRecoveryPhase::Inactive);
}
