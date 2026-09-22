/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonTransitionPolicy.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace DungeonTransitionPolicy;

MemberFacts Member(std::uint64_t id, bool leader, std::uint32_t mapId = 0,
    std::uint32_t instanceId = 0, AdmissionGuard guard = AdmissionGuard::Allowed)
{
    return {id, leader, mapId, instanceId, guard};
}

Request Party(std::vector<MemberFacts> members, std::uint64_t nowMs = 1000)
{
    Request request;
    request.members = std::move(members);
    request.targetMapId = 33;
    request.targetInstanceId = 7001;
    request.nowMs = nowMs;
    return request;
}

PendingAttempt Pending(Decision const& decision)
{
    return {true, decision.memberId, 33, 7001, decision.attempt, decision.retryAtMs};
}
}

TEST(DungeonTransitionPolicy, AdmitsLeaderThenOneFollowerAtATimeInGuidOrder)
{
    Request request = Party({Member(30, false), Member(20, false), Member(99, true)});

    Decision const leader = Evaluate(request);
    ASSERT_EQ(leader.status, Status::AdmitMember);
    EXPECT_EQ(leader.memberId, 99u);

    request.members[2].mapId = 33;
    request.members[2].instanceId = 7001;
    request.pending = Pending(leader);
    Decision const firstFollower = Evaluate(request);
    ASSERT_EQ(firstFollower.status, Status::AdmitMember);
    EXPECT_EQ(firstFollower.memberId, 20u);

    request.members[1].mapId = 33;
    request.members[1].instanceId = 7001;
    request.pending = Pending(firstFollower);
    Decision const secondFollower = Evaluate(request);
    ASSERT_EQ(secondFollower.status, Status::AdmitMember);
    EXPECT_EQ(secondFollower.memberId, 30u);
}

TEST(DungeonTransitionPolicy, RequiresExactTargetMapAndInstanceBeforeAdvancing)
{
    Request wrongMap = Party({Member(10, true, 34, 7001), Member(20, false)});
    EXPECT_EQ(Evaluate(wrongMap).status, Status::SplitInstance);

    Request wrongInstance = Party({Member(10, true, 33, 7002), Member(20, false)});
    EXPECT_EQ(Evaluate(wrongInstance).status, Status::SplitInstance);

    Request exact = Party({Member(10, true, 33, 7001), Member(20, false)});
    Decision const result = Evaluate(exact);
    EXPECT_EQ(result.status, Status::AdmitMember);
    EXPECT_EQ(result.memberId, 20u);
}

TEST(DungeonTransitionPolicy, PendingAttemptSuppressesDuplicateAdmissionUntilDue)
{
    Request request = Party({Member(10, true), Member(20, false)}, 1000);
    Decision const issued = Evaluate(request);
    ASSERT_EQ(issued.status, Status::AdmitMember);
    ASSERT_EQ(issued.attempt, 1u);
    EXPECT_EQ(issued.retryAtMs, 3000u);

    request.pending = Pending(issued);
    request.nowMs = 2999;
    Decision const waiting = Evaluate(request);
    EXPECT_EQ(waiting.status, Status::AwaitingRetry);
    EXPECT_EQ(waiting.memberId, 10u);
    EXPECT_EQ(waiting.attempt, 1u);
}

TEST(DungeonTransitionPolicy, PermanentAndRetryableGuardsRemainDistinct)
{
    Request retryable = Party({Member(10, true, 0, 0, AdmissionGuard::RetryableBlocked), Member(20, false)});
    Decision const retryableResult = Evaluate(retryable);
    EXPECT_EQ(retryableResult.status, Status::RetryableGuard);
    EXPECT_EQ(retryableResult.memberId, 10u);
    EXPECT_EQ(retryableResult.retryAtMs, 3000u);

    Request permanent = Party({Member(10, true, 0, 0, AdmissionGuard::PermanentlyBlocked), Member(20, false)});
    Decision const permanentResult = Evaluate(permanent);
    EXPECT_EQ(permanentResult.status, Status::PermanentlyBlocked);
    EXPECT_EQ(permanentResult.memberId, 10u);
    EXPECT_EQ(permanentResult.retryAtMs, 0u);
}

TEST(DungeonTransitionPolicy, RetriesUseCappedBackoffAndStopAtTheAttemptLimit)
{
    Request request = Party({Member(10, true), Member(20, false)}, 1000);
    request.retries = {3, 2000, 3000};

    Decision first = Evaluate(request);
    EXPECT_EQ(first.retryAtMs, 3000u);
    request.pending = Pending(first);
    request.nowMs = first.retryAtMs;

    Decision second = Evaluate(request);
    ASSERT_EQ(second.status, Status::AdmitMember);
    EXPECT_EQ(second.attempt, 2u);
    EXPECT_EQ(second.retryAtMs, 6000u);
    request.pending = Pending(second);
    request.nowMs = second.retryAtMs;

    Decision third = Evaluate(request);
    ASSERT_EQ(third.status, Status::AdmitMember);
    EXPECT_EQ(third.attempt, 3u);
    EXPECT_EQ(third.retryAtMs, 9000u);
    request.pending = Pending(third);
    request.nowMs = third.retryAtMs;

    Decision const exhausted = Evaluate(request);
    EXPECT_EQ(exhausted.status, Status::RetryExhausted);
    EXPECT_EQ(exhausted.memberId, 10u);
    EXPECT_EQ(exhausted.attempt, 3u);
}

TEST(DungeonTransitionPolicy, AnyMemberInAnotherInstanceFailsTheParty)
{
    Request request = Party({
        Member(10, true, 33, 7001), Member(30, false, 44, 8001), Member(20, false)});
    Decision const result = Evaluate(request);
    EXPECT_EQ(result.status, Status::SplitInstance);
    EXPECT_EQ(result.memberId, 30u);
}

TEST(DungeonTransitionPolicy, MemberOrderPermutationDoesNotChangeTheDecision)
{
    std::vector<MemberFacts> members = {
        Member(40, false), Member(10, true, 33, 7001), Member(20, false), Member(30, false)};
    Decision const expected = Evaluate(Party(members));
    ASSERT_EQ(expected.status, Status::AdmitMember);
    ASSERT_EQ(expected.memberId, 20u);

    std::reverse(members.begin(), members.end());
    Decision const reversed = Evaluate(Party(members));
    EXPECT_EQ(reversed.status, expected.status);
    EXPECT_EQ(reversed.memberId, expected.memberId);
    EXPECT_EQ(reversed.attempt, expected.attempt);
    EXPECT_EQ(reversed.retryAtMs, expected.retryAtMs);

    std::rotate(members.begin(), members.begin() + 1, members.end());
    Decision const rotated = Evaluate(Party(members));
    EXPECT_EQ(rotated.status, expected.status);
    EXPECT_EQ(rotated.memberId, expected.memberId);
    EXPECT_EQ(rotated.attempt, expected.attempt);
    EXPECT_EQ(rotated.retryAtMs, expected.retryAtMs);
}

TEST(DungeonTransitionPolicy, CompletesOnlyWhenEveryMemberMatchesTheExactTarget)
{
    Request request = Party({Member(10, true, 33, 7001), Member(20, false, 33, 7001)});
    EXPECT_EQ(Evaluate(request).status, Status::Complete);
}

TEST(DungeonTransitionPolicy, InvalidIdentityAndPendingStateFailClosed)
{
    EXPECT_EQ(Evaluate(Party({Member(10, true), Member(10, false)})).status, Status::InvalidInput);

    Request stale = Party({Member(10, true), Member(20, false)});
    stale.pending = {true, 99, 33, 7001, 1, 3000};
    EXPECT_EQ(Evaluate(stale).status, Status::InvalidInput);
}

TEST(DungeonTransitionPolicy, LivingReadyMemberMayProceed)
{
    MemberReadinessFacts member;
    member.online = true;
    member.alive = true;
    member.ready = true;

    EXPECT_EQ(EvaluateMemberReadiness(member, {}), MemberReadiness::Proceed);

    member.ready = false;
    EXPECT_EQ(EvaluateMemberReadiness(member, {}), MemberReadiness::BlockParty);
}

TEST(DungeonTransitionPolicy, ExteriorPortalMayOmitReleasedMemberWithCorpseOnTargetDungeonMap)
{
    MemberReadinessFacts member;
    member.online = true;
    member.released = true;
    member.hasActualCorpse = true;
    member.corpseMapId = 33;

    MemberReadinessContext context;
    context.stage = PartyReadinessStage::ExteriorPortal;
    context.portalTargetMapId = 33;

    EXPECT_EQ(EvaluateMemberReadiness(member, context), MemberReadiness::OmitReleasedCorpse);
}

TEST(DungeonTransitionPolicy, LeaderInsideMayOmitReleasedMemberOnlyForMatchingCorpseAnchor)
{
    MemberReadinessFacts member;
    member.online = true;
    member.released = true;
    member.hasActualCorpse = true;
    member.corpseMapId = 33;
    member.corpseInstanceId = 7001;

    MemberReadinessContext context;
    context.stage = PartyReadinessStage::LeaderInside;
    context.leaderMapId = 33;
    context.expectedInstanceId = 7001;

    EXPECT_EQ(EvaluateMemberReadiness(member, context), MemberReadiness::OmitReleasedCorpse);

    member.corpseInstanceId = 7002;
    EXPECT_EQ(EvaluateMemberReadiness(member, context), MemberReadiness::BlockParty);

    member.corpseInstanceId = 0;
    EXPECT_EQ(EvaluateMemberReadiness(member, context), MemberReadiness::OmitReleasedCorpse);
}

TEST(DungeonTransitionPolicy, UnrelatedDeadAndUnavailableMembersStillBlockParty)
{
    MemberReadinessContext context;
    context.stage = PartyReadinessStage::ExteriorPortal;
    context.portalTargetMapId = 33;

    MemberReadinessFacts released;
    released.online = true;
    released.released = true;
    released.hasActualCorpse = true;
    released.corpseMapId = 33;

    MemberReadinessFacts unreleased = released;
    unreleased.released = false;
    EXPECT_EQ(EvaluateMemberReadiness(unreleased, context), MemberReadiness::BlockParty);

    MemberReadinessFacts offline = released;
    offline.online = false;
    EXPECT_EQ(EvaluateMemberReadiness(offline, context), MemberReadiness::BlockParty);

    MemberReadinessFacts inFlight;
    inFlight.online = true;
    inFlight.alive = true;
    inFlight.ready = true;
    inFlight.inFlight = true;
    EXPECT_EQ(EvaluateMemberReadiness(inFlight, context), MemberReadiness::BlockParty);

    MemberReadinessFacts missingCorpse = released;
    missingCorpse.hasActualCorpse = false;
    EXPECT_EQ(EvaluateMemberReadiness(missingCorpse, context), MemberReadiness::BlockParty);

    MemberReadinessFacts otherMapCorpse = released;
    otherMapCorpse.corpseMapId = 34;
    EXPECT_EQ(EvaluateMemberReadiness(otherMapCorpse, context), MemberReadiness::BlockParty);
}

TEST(DungeonTransitionPolicy, LeaderInsideRequiresCorpseOnLeaderMap)
{
    MemberReadinessFacts member;
    member.online = true;
    member.released = true;
    member.hasActualCorpse = true;
    member.corpseMapId = 34;
    member.corpseInstanceId = 7001;

    MemberReadinessContext context;
    context.stage = PartyReadinessStage::LeaderInside;
    context.leaderMapId = 33;
    context.expectedInstanceId = 7001;

    EXPECT_EQ(EvaluateMemberReadiness(member, context), MemberReadiness::BlockParty);
}
