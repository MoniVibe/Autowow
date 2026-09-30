/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestAcquisitionPolicy.h"
#include "QuestGiverTravelPolicy.h"
#include "QuestGiverTravelFeedback.h"
#include "QuestGiverTravelLifecycle.h"
#include "QuestTravelWalk.h"
#include "AutonomousRpgTravelPolicy.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

TEST(AutonomousRpgTravelPolicyTest, LongHealthyWalkKeepsItsDestination)
{
    using namespace AutonomousRpgTravelPolicy;
    float nearest = std::numeric_limits<float>::max();
    uint32_t progressedAt = 0;
    uint32_t attempts = 0;
    uint32_t lastObservedAt = 0;
    for (uint32_t now = 1000; now <= 241000; now += 30000)
    {
        float const distance = 240.0f - static_cast<float>((now - 1000) / 30000) * 8.0f;
        EXPECT_FALSE(ObserveProgress(distance, nearest, progressedAt, attempts, lastObservedAt, now));
    }
    EXPECT_EQ(nearest, 176.0f);
}

TEST(AutonomousRpgTravelPolicyTest, OscillationEventuallyReplansAndCoolsDestination)
{
    using namespace AutonomousRpgTravelPolicy;
    float nearest = std::numeric_limits<float>::max();
    uint32_t progressedAt = 0;
    uint32_t attempts = 0;
    uint32_t lastObservedAt = 0;
    EXPECT_FALSE(ObserveProgress(100.0f, nearest, progressedAt, attempts, lastObservedAt, 1000));
    for (uint32_t now = 11000; now < 91000; now += 10000)
        EXPECT_FALSE(ObserveProgress(now % 20000 ? 98.0f : 101.0f,
                                     nearest, progressedAt, attempts, lastObservedAt, now));
    EXPECT_TRUE(ObserveProgress(99.0f, nearest, progressedAt, attempts, lastObservedAt, 91000));
    EXPECT_TRUE(IsCoolingDown(91000, 91000 + kFailedDestinationCooldownMs - 1));
    EXPECT_FALSE(IsCoolingDown(91000, 91000 + kFailedDestinationCooldownMs));
}

TEST(AutonomousRpgTravelPolicyTest, CombatPauseResetsObservationWindow)
{
    using namespace AutonomousRpgTravelPolicy;
    float nearest = std::numeric_limits<float>::max();
    uint32_t progressedAt = 0;
    uint32_t attempts = 0;
    uint32_t lastObservedAt = 0;
    EXPECT_FALSE(ObserveProgress(100.0f, nearest, progressedAt, attempts, lastObservedAt, 1000));
    EXPECT_FALSE(ObserveProgress(99.0f, nearest, progressedAt, attempts, lastObservedAt, 121000));
    EXPECT_EQ(progressedAt, 121000u);
    EXPECT_EQ(nearest, 99.0f);
    for (uint32_t now = 131000; now < 211000; now += 10000)
        EXPECT_FALSE(ObserveProgress(99.0f, nearest, progressedAt, attempts, lastObservedAt, now));
    EXPECT_TRUE(ObserveProgress(99.0f, nearest, progressedAt, attempts, lastObservedAt, 211000));
}

TEST(AutonomousRpgTravelPolicyTest, MillisecondWrapPreservesCooldownAndProgressWindow)
{
    using namespace AutonomousRpgTravelPolicy;
    uint32_t const beforeWrap = std::numeric_limits<uint32_t>::max() - 2000;
    EXPECT_TRUE(IsCoolingDown(beforeWrap, 2000));
    float nearest = std::numeric_limits<float>::max();
    uint32_t progressedAt = 0;
    uint32_t attempts = 0;
    uint32_t lastObservedAt = 0;
    EXPECT_FALSE(ObserveProgress(100.0f, nearest, progressedAt, attempts, lastObservedAt, beforeWrap));
    EXPECT_FALSE(ObserveProgress(99.0f, nearest, progressedAt, attempts, lastObservedAt, 2000));
    EXPECT_EQ(progressedAt, beforeWrap);
}

TEST(AutonomousRpgTravelPolicyTest, SafePartialRouteCommitsReachableEndpoint)
{
    using namespace AutonomousRpgTravelPolicy;
    // Observed near Naomini: coarse destination is farther away, while a safe
    // NORMAL|INCOMPLETE navmesh path supplies one grounded 24-yard leg.
    LocalPoint const coarse{-9090.2f, -396.3f, 72.9f};
    LocalPoint const reachable{-9063.324f, -437.899f, 72.588f};
    PartialSegmentFacts const facts{true, true, true, false, 24.016f,
                                    72.9f, 49.527f, 24.016f};
    PartialSegmentDecision const selected = SelectPartialSegmentEndpoint(facts, reachable);
    ASSERT_TRUE(selected.accepted);
    EXPECT_FLOAT_EQ(selected.destination.x, reachable.x);
    EXPECT_FLOAT_EQ(selected.destination.y, reachable.y);
    EXPECT_FLOAT_EQ(selected.destination.z, reachable.z);
    EXPECT_NE(selected.destination.x, coarse.x);
    EXPECT_GT(selected.progress, kMeaningfulProgressYards);
}

TEST(AutonomousRpgTravelPolicyTest, UnsafeOrStalledPartialRouteCannotBecomeDestination)
{
    using namespace AutonomousRpgTravelPolicy;
    LocalPoint const endpoint{-9063.324f, -437.899f, 72.588f};
    PartialSegmentFacts facts{true, true, true, false, 24.0f, 72.9f, 49.5f, 24.0f};
    facts.safeNavmesh = false;
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
    facts.safeNavmesh = true;
    facts.normalIncompleteOnly = false; // no-path, shortcut, or other rejected type
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
    facts.normalIncompleteOnly = true;
    facts.endpointToCoarse = 70.0f; // less than five yards toward the coarse target
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
    facts.endpointToCoarse = 49.5f;
    facts.sameZone = false;
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
    facts.sameZone = true;
    facts.coolingDown = true;
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
    facts.coolingDown = false;
    facts.distanceToEndpoint = 9.0f; // would instantly finish GO_GRIND
    EXPECT_FALSE(SelectPartialSegmentEndpoint(facts, endpoint).accepted);
}

TEST(AutonomousRpgTravelPolicyTest, OnlyCompleteGroundedRouteIsCommitted)
{
    using namespace AutonomousRpgTravelPolicy;
    EXPECT_TRUE(IsCompleteLocalRoute(true, false, false, false, false, false, 2.0f));
    EXPECT_FALSE(IsCompleteLocalRoute(true, true, false, false, false, false, 2.0f));
    EXPECT_FALSE(IsCompleteLocalRoute(true, false, false, false, true, false, 2.0f));
    EXPECT_FALSE(IsCompleteLocalRoute(true, false, false, false, false, true, 2.0f));
    EXPECT_FALSE(IsCompleteLocalRoute(true, false, false, false, false, false, 15.0f));
}

TEST(AutonomousRpgTravelPolicyTest, IndependentArmingHasOneRpgMovementOwnerAndNoTeleport)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/AutoWowBridge.cpp", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    std::string const bridge{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    size_t const begin = bridge.find("bool ArmIndependentBot()");
    size_t const end = bridge.find("bool CreateParty()", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    std::string const arming = bridge.substr(begin, end - begin);
    EXPECT_NE(arming.find("AutoWowPolicy::SetNoTeleport(m_request.botGuid, true)"), std::string::npos);
    EXPECT_NE(arming.find("+grind,+new rpg,-rpg"), std::string::npos);
}

TEST(AutonomousRpgTravelPolicyTest, PendingAutonomousWalkCannotFallThroughToRandomNudge)
{
    auto const module = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream baseInput(module / "src/Ai/World/Rpg/Action/NewRpgBaseAction.cpp", std::ios::binary);
    std::ifstream actionInput(module / "src/Ai/World/Rpg/Action/NewRpgAction.cpp", std::ios::binary);
    ASSERT_TRUE(baseInput.is_open());
    ASSERT_TRUE(actionInput.is_open());
    std::string const base{std::istreambuf_iterator<char>(baseInput), std::istreambuf_iterator<char>()};
    std::string const action{std::istreambuf_iterator<char>(actionInput), std::istreambuf_iterator<char>()};
    EXPECT_NE(base.find("if (watchDestination)\n    {\n        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))\n            return true;"),
              std::string::npos);
    for (std::string const& name : {"NewRpgGoGrindAction", "NewRpgGoCampAction"})
    {
        size_t const begin = action.find("bool " + name + "::Execute");
        ASSERT_NE(begin, std::string::npos);
        size_t const end = action.find("\nbool ", begin + 1);
        ASSERT_NE(end, std::string::npos);
        std::string const body = action.substr(begin, end - begin);
        size_t const scopedHold = body.find("if (IsAutoWowTravelBot())\n            return false;");
        size_t const randomNudge = body.find("return MoveRandomNear(10.0f);");
        ASSERT_NE(scopedHold, std::string::npos);
        ASSERT_NE(randomNudge, std::string::npos);
        EXPECT_LT(scopedHold, randomNudge);
    }
}

namespace
{
using AutoWowQuestGiverTravel::PathFacts;
using AutoWowQuestGiverTravel::StepKind;
using AutoWowQuestGiverTravel::Waypoint;

AutoWowQuestGiverTravel::EscapeCandidateFacts EscapeCandidate(float distanceReduction,
                                                              float routeLength,
                                                              std::size_t ringOrder)
{
    AutoWowQuestGiverTravel::EscapeCandidateFacts candidate;
    candidate.point = {1, 10.0f + static_cast<float>(ringOrder) * 8.0f,
                       20.0f + static_cast<float>(ringOrder) * 4.0f, 0.0f};
    candidate.cell = {1, static_cast<std::int32_t>(ringOrder), 2, 0};
    candidate.quantized = true;
    candidate.sameMap = true;
    candidate.finite = true;
    candidate.validHeight = true;
    candidate.freshlyStrictComplete = true;
    candidate.routeLength = routeLength;
    candidate.distanceReduction = distanceReduction;
    candidate.pathType = PATHFIND_NORMAL;
    candidate.ringOrder = ringOrder;
    return candidate;
}

AutoWowQuestGiverTravel::TravelMgrReanchorCandidateFacts TravelMgrCandidate(
    std::size_t routeIndex, float directDistance, float pathLength, bool valid = true,
    float targetDistanceReduction = 1.0f)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate;
    candidate.routeIndex = routeIndex;
    candidate.nodeKind = TravelMgrRouteNodeKind::Walk;
    candidate.point = {1, 10.0f + static_cast<float>(routeIndex), 20.0f, 0.0f};
    candidate.sameMap = true;
    candidate.directDistance = directDistance;
    candidate.targetDistanceReduction = targetDistanceReduction;
    candidate.freshProbe = valid
        ? PathFacts{true, true, true, PATHFIND_NORMAL, 4, 0.5f, pathLength}
        : PathFacts{true, false, true, PATHFIND_NORMAL, 4, 0.5f, pathLength};
    return candidate;
}

AutoWowQuestGiverTravel::TravelMgrReanchorCandidateFacts TravelMgrSafeIncompleteCandidate(
    std::size_t routeIndex, float directDistance, float pathLength,
    float targetDistanceReduction = 1.0f)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate =
        TravelMgrCandidate(routeIndex, directDistance, pathLength, true, targetDistanceReduction);
    candidate.freshProbe = {true,
                            true,
                            true,
                            PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                            3,
                            4.0f,
                            pathLength,
                            true,
                            false};
    return candidate;
}

AutoWowQuestGiverTravel::HearthRecoveryFacts ReadyHearthRecoveryFacts()
{
    AutoWowQuestGiverTravel::HearthRecoveryFacts facts;
    facts.sessionPendingOrJustExhausted = true;
    facts.botAlive = true;
    facts.boundedLocalEscapeRecoveryExhausted = true;
    facts.strongOffNavmeshEvidence = true;
    facts.hearthstoneUseful = true;
    facts.hearthstoneAvailable = true;
    return facts;
}
}

TEST(QuestGiverTravelPolicyTest, StrongOffNavmeshEvidenceRejectsGenericPathFailures)
{
    using namespace AutoWowQuestGiverTravel;

    OffNavmeshProbeFacts genericNoPath{
        true, PATHFIND_NOPATH, "ground_line_missing_floor", kRequiredStrongOffNavmeshProbes};
    EXPECT_FALSE(IsStrongOffNavmeshProbe(genericNoPath));
    EXPECT_FALSE(HasStrongOffNavmeshEvidence(genericNoPath));

    OffNavmeshProbeFacts farFromPolyWithoutGroundFailure{
        true, PATHFIND_FARFROMPOLY, "ground_line_blocked", kRequiredStrongOffNavmeshProbes};
    EXPECT_FALSE(IsStrongOffNavmeshProbe(farFromPolyWithoutGroundFailure));
    EXPECT_FALSE(HasStrongOffNavmeshEvidence(farFromPolyWithoutGroundFailure));

    OffNavmeshProbeFacts oneStrongProbe{
        true, PATHFIND_FARFROMPOLY, "ground_line_too_short", 1U};
    EXPECT_TRUE(IsStrongOffNavmeshProbe(oneStrongProbe));
    EXPECT_FALSE(HasStrongOffNavmeshEvidence(oneStrongProbe));

    OffNavmeshProbeFacts missingFloorEvidence{
        true, PATHFIND_FARFROMPOLY, "ground_line_missing_floor", kRequiredStrongOffNavmeshProbes};
    EXPECT_TRUE(IsStrongOffNavmeshProbe(missingFloorEvidence));
    EXPECT_TRUE(HasStrongOffNavmeshEvidence(missingFloorEvidence));
}

TEST(QuestGiverTravelPolicyTest, HearthRecoveryRequiresTheExactBoundedEscapeTerminalObservation)
{
    using namespace AutoWowQuestGiverTravel;
    EXPECT_TRUE(IsBoundedLocalEscapeRecoveryExhausted("escape_ring", "no_progress"));
    EXPECT_FALSE(IsBoundedLocalEscapeRecoveryExhausted("blocked", "no_progress"));
    EXPECT_FALSE(IsBoundedLocalEscapeRecoveryExhausted("escape_ring", "rejected"));
    EXPECT_FALSE(IsBoundedLocalEscapeRecoveryExhausted("local_detour", "no_progress"));
}

TEST(QuestGiverTravelPolicyTest, HearthRecoveryClassifierCoversEverySafetyGate)
{
    using namespace AutoWowQuestGiverTravel;
    HearthRecoveryFacts const ready = ReadyHearthRecoveryFacts();
    EXPECT_EQ(EvaluateHearthRecovery(ready), HearthRecoveryDecision::Requested);
    EXPECT_EQ(HearthRecoveryDecisionName(HearthRecoveryDecision::Requested), "requested");

    struct Gate
    {
        char const* name;
        void (*disable)(HearthRecoveryFacts&);
    };
    std::array<Gate, 9> const gates{
        Gate{"session", [](HearthRecoveryFacts& facts) {
                 facts.sessionPendingOrJustExhausted = false;
             }},
        Gate{"alive", [](HearthRecoveryFacts& facts) { facts.botAlive = false; }},
        Gate{"combat", [](HearthRecoveryFacts& facts) { facts.inCombat = true; }},
        Gate{"flight", [](HearthRecoveryFacts& facts) { facts.inFlight = true; }},
        Gate{"teleport", [](HearthRecoveryFacts& facts) { facts.beingTeleported = true; }},
        Gate{"campaign", [](HearthRecoveryFacts& facts) { facts.campaignTravelOwned = true; }},
        Gate{"bounded_recovery", [](HearthRecoveryFacts& facts) {
                 facts.boundedLocalEscapeRecoveryExhausted = false;
             }},
        Gate{"off_navmesh", [](HearthRecoveryFacts& facts) {
                 facts.strongOffNavmeshEvidence = false;
             }},
        Gate{"useful", [](HearthRecoveryFacts& facts) { facts.hearthstoneUseful = false; }},
    };

    for (Gate const& gate : gates)
    {
        HearthRecoveryFacts facts = ready;
        gate.disable(facts);
        EXPECT_EQ(EvaluateHearthRecovery(facts),
                  gate.name == std::string_view{"useful"}
                      ? HearthRecoveryDecision::Unavailable
                      : HearthRecoveryDecision::NotEligible)
            << gate.name;
    }

    HearthRecoveryFacts unavailable = ready;
    unavailable.hearthstoneAvailable = false;
    EXPECT_EQ(EvaluateHearthRecovery(unavailable), HearthRecoveryDecision::Unavailable);
    EXPECT_EQ(HearthRecoveryDecisionName(HearthRecoveryDecision::Unavailable), "unavailable");
}

TEST(QuestGiverTravelPolicyTest, HearthRecoveryAllowsAtMostOneAttemptAndThenReportsCooldown)
{
    using namespace AutoWowQuestGiverTravel;
    HearthRecoveryFacts facts = ReadyHearthRecoveryFacts();
    EXPECT_EQ(EvaluateHearthRecovery(facts), HearthRecoveryDecision::Requested);

    ++facts.hearthstoneAttempts;
    EXPECT_EQ(facts.hearthstoneAttempts, kMaxHearthRecoveryAttempts);
    EXPECT_EQ(EvaluateHearthRecovery(facts), HearthRecoveryDecision::Cooldown);
    EXPECT_EQ(HearthRecoveryDecisionName(HearthRecoveryDecision::Cooldown), "cooldown");

    // The cooldown remains authoritative even if another caller tries to re-arm all other gates.
    facts.hearthstoneUseful = true;
    facts.hearthstoneAvailable = true;
    EXPECT_EQ(EvaluateHearthRecovery(facts), HearthRecoveryDecision::Cooldown);
}

TEST(QuestGiverTravelPolicyTest, QuestAcquisitionStrategyYieldsInCombatAndRestoresOnceAfterward)
{
    using namespace AutoWowQuestGiverTravel;
    QuestAcquisitionStrategyFacts facts;
    facts.sessionPending = true;
    facts.botAlive = true;
    facts.nonCombatGatesClear = true;
    facts.role = QuestAcquisitionStrategyRole::Leader;
    facts.hasTravel = false;
    facts.hasFollow = true;
    facts.hasGrind = true;
    facts.hasRandomMove = true;
    facts.hasNewRpg = true;

    std::uint32_t strategyMutations = 0;
    auto evaluateAndApply = [&strategyMutations](QuestAcquisitionStrategyFacts const& current)
    {
        if (EvaluateQuestAcquisitionStrategy(current) == QuestAcquisitionStrategyDecision::Restore)
            ++strategyMutations;
    };

    facts.inCombat = true;
    EXPECT_EQ(EvaluateQuestAcquisitionStrategy(facts), QuestAcquisitionStrategyDecision::Yield);
    evaluateAndApply(facts);
    EXPECT_EQ(strategyMutations, 0U);

    // Combat ends: the first safe tick is allowed to restore the leader-owned movement profile.
    facts.inCombat = false;
    EXPECT_EQ(EvaluateQuestAcquisitionStrategy(facts), QuestAcquisitionStrategyDecision::Restore);
    evaluateAndApply(facts);
    EXPECT_EQ(strategyMutations, 1U);

    // The next safe tick sees the exact profile and must not reapply ChangeStrategy.
    facts.hasTravel = true;
    facts.hasFollow = false;
    facts.hasGrind = false;
    facts.hasRandomMove = false;
    facts.hasNewRpg = false;
    EXPECT_EQ(EvaluateQuestAcquisitionStrategy(facts),
              QuestAcquisitionStrategyDecision::AlreadyOwned);
    evaluateAndApply(facts);
    EXPECT_EQ(strategyMutations, 1U);

    // The same combat gate also protects followers from strategy thrash.
    facts.role = QuestAcquisitionStrategyRole::Follower;
    facts.inCombat = true;
    EXPECT_EQ(EvaluateQuestAcquisitionStrategy(facts), QuestAcquisitionStrategyDecision::Yield);
}

TEST(QuestGiverTravelPolicyTest, CohesiveAcquisitionRejectsFollowerPrerequisiteMismatch)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<PartyQuestEligibilityFacts> const leaderEligibleFollowerBlocked{
        PartyQuestEligibilityFacts{true, true, true, true, true, true},
        PartyQuestEligibilityFacts{true, false, false, true, true, true}};
    EXPECT_FALSE(IsCommonPartyQuestEligible(leaderEligibleFollowerBlocked));

    std::vector<PartyQuestEligibilityFacts> const fullPartyEligible{
        PartyQuestEligibilityFacts{true, true, true, true, true, true},
        PartyQuestEligibilityFacts{true, true, true, true, true, true},
        // An unloaded giver defers only the exact live-object check; common prerequisites still
        // pass and the route may be selected for later runtime giver validation.
        PartyQuestEligibilityFacts{true, true, true, true, false, false}};
    EXPECT_TRUE(IsCommonPartyQuestEligible(fullPartyEligible));
    EXPECT_FALSE(IsCommonPartyQuestEligible({}));

    std::vector<PartyQuestEligibilityFacts> const exactGiverMismatch{
        PartyQuestEligibilityFacts{true, true, true, true, true, true},
        PartyQuestEligibilityFacts{true, true, true, true, true, false}};
    EXPECT_FALSE(IsCommonPartyQuestEligible(exactGiverMismatch));
}

TEST(QuestGiverTravelPolicyTest, Q827NormalIncompletePathMakesAStagedStep)
{
    PathFacts const q827{true, true, true, PATHFIND_NORMAL | PATHFIND_INCOMPLETE, 6, 386.181f, 20.0264f};
    EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(q827), StepKind::PartialPath);
}

TEST(QuestGiverTravelPolicyTest, Q14170IncompleteOnlyFrontierIsEligibleForNomination)
{
    // The observed incomplete-only corridor is eligible to nominate a bounded frontier point.
    // Its exact candidate still needs a fresh complete re-probe before any MoveTo.
    PathFacts const q14170{true, false, false, PATHFIND_INCOMPLETE, 20, 462.293f, 76.3146f};
    EXPECT_TRUE(AutoWowQuestGiverTravel::IsBoundedPartial(q14170));
    EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(q14170), StepKind::PartialPath);
}

TEST(QuestGiverTravelPolicyTest, RejectsUnsafePathTypeAndMissingPointsForPartialStep)
{
    PathFacts const unsafePathType{true, false, true, PATHFIND_NORMAL | PATHFIND_NOPATH,
                                   2, 386.181f, 4.01523f};
    EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(unsafePathType), StepKind::TravelMgrSegment);

    PathFacts const missingPoints{true, false, true, PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                                  1, 386.181f, 4.01523f};
    EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(missingPoints), StepKind::TravelMgrSegment);
}

TEST(QuestGiverTravelPolicyTest, IncompleteFrontierRejectsForbiddenPathFlags)
{
    for (std::uint32_t const forbidden : {
             static_cast<std::uint32_t>(PATHFIND_SHORTCUT),
             static_cast<std::uint32_t>(PATHFIND_NOPATH),
             static_cast<std::uint32_t>(PATHFIND_NOT_USING_PATH),
             static_cast<std::uint32_t>(PATHFIND_SHORT),
             static_cast<std::uint32_t>(PATHFIND_FARFROMPOLY)})
    {
        PathFacts const rejected{true, true, false, PATHFIND_INCOMPLETE | forbidden,
                                 20, 462.293f, 76.3146f};
        EXPECT_FALSE(AutoWowQuestGiverTravel::IsBoundedPartial(rejected)) << forbidden;
        EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(rejected), StepKind::TravelMgrSegment);
    }
}

TEST(QuestGiverTravelPolicyTest, PartialCandidateMoveRequiresCompleteReprobe)
{
    PathFacts const incompleteOnly{true, true, false, PATHFIND_INCOMPLETE, 4, 0.5f, 12.0f};
    EXPECT_FALSE(AutoWowQuestGiverTravel::IsCompleteReprobe(incompleteOnly));

    PathFacts const unsafeNormal{true, false, true, PATHFIND_NORMAL, 4, 0.5f, 12.0f};
    EXPECT_FALSE(AutoWowQuestGiverTravel::IsCompleteReprobe(unsafeNormal));

    PathFacts const ungroundedEndpoint{true, true, true, PATHFIND_NORMAL, 4, 3.0f, 12.0f};
    EXPECT_FALSE(AutoWowQuestGiverTravel::IsCompleteReprobe(ungroundedEndpoint));

    PathFacts const complete{true, true, true, PATHFIND_NORMAL, 4, 0.5f, 12.0f};
    EXPECT_TRUE(AutoWowQuestGiverTravel::IsCompleteReprobe(complete));
}

TEST(QuestGiverTravelPolicyTest, ValidatedGroundLineRequiresExplicitProof)
{
    using namespace AutoWowQuestGiverTravel;
    PathFacts const validatedGroundLine{
        true, true, false, PATHFIND_BLANK, 4, 0.25f, 8.0f, true, true};
    EXPECT_TRUE(IsComplete(validatedGroundLine));
    EXPECT_TRUE(IsCompleteReprobe(validatedGroundLine));

    PathFacts identicalWithoutProof = validatedGroundLine;
    identicalWithoutProof.validatedGroundLine = false;
    EXPECT_FALSE(IsComplete(identicalWithoutProof));

    PathFacts forbiddenFlags = validatedGroundLine;
    forbiddenFlags.pathType = PATHFIND_NOPATH;
    EXPECT_FALSE(IsComplete(forbiddenFlags));
}

TEST(QuestGiverTravelPolicyTest, CompletePartialSelectorPrefersFarthestValidCandidate)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{
        start,
        {1, 20.0f, 0.0f, 0.0f},
        {1, 80.0f, 0.0f, 0.0f},
        {1, 110.0f, 0.0f, 0.0f}};
    PathFacts const complete{true, true, true, PATHFIND_NORMAL, 4, 0.5f, 80.0f};
    std::vector<PathFacts> const reprobes{PathFacts{}, complete, complete, complete};

    std::optional<std::size_t> const selected =
        SelectCompletePartialWaypoint(path, start, reprobes);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 3u);
}

TEST(QuestGiverTravelPolicyTest, CompletePartialSelectorBacktracksToNearerValidCandidate)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{
        start,
        {1, 20.0f, 0.0f, 0.0f},
        {1, 80.0f, 0.0f, 0.0f},
        {1, 110.0f, 0.0f, 0.0f}};
    PathFacts const complete{true, true, true, PATHFIND_NORMAL, 4, 0.5f, 80.0f};
    PathFacts const fartherInvalid{true, true, true, PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                                   4, 0.5f, 80.0f};
    std::vector<PathFacts> const reprobes{PathFacts{}, complete, complete, fartherInvalid};

    std::optional<std::size_t> const selected =
        SelectCompletePartialWaypoint(path, start, reprobes);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 2u);
}

TEST(QuestGiverTravelPolicyTest, TargetAwarePartialSelectorRejectsPathRelativeTargetRegression)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    Waypoint const target{1, 10.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{start, {1, 30.0f, 0.0f, 0.0f}};

    ASSERT_EQ(SelectPartialWaypointCandidates(path, start).size(), 1u);
    EXPECT_TRUE(SelectTargetAwarePartialWaypointCandidates(path, start, target).empty());
}

TEST(QuestGiverTravelPolicyTest, TargetAwarePartialSelectorChoosesFarthestEligibleCandidate)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    Waypoint const target{1, 200.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{
        start,
        {1, 20.0f, 0.0f, 0.0f},
        {1, 80.0f, 0.0f, 0.0f},
        {1, 110.0f, 0.0f, 0.0f}};
    PathFacts const complete{true, true, true, PATHFIND_NORMAL, 4, 0.5f, 80.0f};
    std::vector<PathFacts> const reprobes{PathFacts{}, complete, complete, complete};

    std::optional<std::size_t> const selected = SelectTargetAwareCompletePartialWaypoint(
        path, start, target, reprobes);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 3u);
}

TEST(QuestGiverTravelPolicyTest, TargetAwarePartialSelectorIsDeterministic)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    Waypoint const target{1, 100.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{
        start,
        {1, 20.0f, 0.0f, 0.0f},
        {1, 80.0f, 0.0f, 0.0f},
        {1, 140.0f, 0.0f, 0.0f},
        {1, 220.0f, 0.0f, 0.0f}};

    std::vector<std::size_t> const first =
        SelectTargetAwarePartialWaypointCandidates(path, start, target);
    std::vector<std::size_t> const second =
        SelectTargetAwarePartialWaypointCandidates(path, start, target);
    EXPECT_EQ(first, second);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0], 2u);
    EXPECT_EQ(first[1], 1u);
}

TEST(QuestGiverTravelPolicyTest, TargetAwarePartialSelectorFailsClosedWithoutEligibleCandidate)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    Waypoint const target{1, 100.0f, 0.0f, 0.0f};
    Waypoint const nonFinite{1, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    std::vector<Waypoint> const invalidPath{
        start,
        {2, 20.0f, 0.0f, 0.0f},
        nonFinite,
        {1, 140.0f, 0.0f, 0.0f}};
    EXPECT_TRUE(SelectTargetAwarePartialWaypointCandidates(invalidPath, start, target).empty());

    Waypoint const crossMapTarget{2, 100.0f, 0.0f, 0.0f};
    EXPECT_TRUE(SelectTargetAwarePartialWaypointCandidates(
                    {start, {1, 20.0f, 0.0f, 0.0f}}, start, crossMapTarget)
                    .empty());
}

TEST(QuestGiverTravelPolicyTest, CompletePartialSelectorReturnsNoneWhenAllCandidatesFail)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{start, {1, 20.0f, 0.0f, 0.0f}, {1, 80.0f, 0.0f, 0.0f}};
    PathFacts const invalid{true, true, true, PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                            4, 0.5f, 80.0f};
    std::vector<PathFacts> const reprobes{PathFacts{}, invalid, invalid};

    EXPECT_FALSE(SelectCompletePartialWaypoint(path, start, reprobes).has_value());
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorSkipsUnreachablePrefixAndChoosesFarthestSuffix)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<TravelMgrReanchorCandidateFacts> const candidates{
        TravelMgrCandidate(0, 100.0f, 20.0f, false),
        TravelMgrCandidate(1, 24.0f, 12.0f, false),
        TravelMgrCandidate(2, 29.0f, 14.0f),
        TravelMgrCandidate(3, 34.0f, 18.0f)};

    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate(candidates, 40.0f);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 3u);
    EXPECT_EQ(candidates[*selected].routeIndex, 3u);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorStopsAtTransitionBoundary)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts transition = TravelMgrCandidate(2, 10.0f, 8.0f);
    transition.nodeKind = TravelMgrRouteNodeKind::Transition;
    std::vector<TravelMgrReanchorCandidateFacts> const candidates{
        TravelMgrCandidate(0, 20.0f, 10.0f), TravelMgrCandidate(1, 30.0f, 12.0f), transition,
        TravelMgrCandidate(3, 20.0f, 10.0f)};

    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate(candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 1u);
    EXPECT_EQ(candidates[*selected].routeIndex, 1u);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorHonorsFiniteScanCap)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<TravelMgrReanchorCandidateFacts> candidates;
    for (std::size_t routeIndex = 0;
         routeIndex < kMaxTravelMgrReanchorScanPoints + 4U; ++routeIndex)
        candidates.push_back(TravelMgrCandidate(routeIndex, 10.0f, 8.0f));

    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate(candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, kMaxTravelMgrReanchorScanPoints - 1U);
    EXPECT_EQ(candidates[*selected].routeIndex, kMaxTravelMgrReanchorScanPoints - 1U);
}

TEST(QuestGiverTravelPolicyTest, ErrandsTopologyAcceptsOnlyBoundedSameMapAllWalkRouteToExactTown)
{
    using namespace AutoWowQuestGiverTravel;
    WorldPosition const town(1, 300.0f, 40.0f, 12.0f);
    std::vector<PathNodePoint> const route{
        {WorldPosition(1, 0.0f, 0.0f, 10.0f), NODE_PREPATH, 0},
        {WorldPosition(1, 150.0f, 20.0f, 11.0f), NODE_NODE, 0},
        {town, NODE_PATH, 0}};

    EXPECT_EQ(ClassifyErrandsWalkTopology(route, town), ErrandsWalkTopology::AllWalk);
    EXPECT_EQ(ClassifyErrandsWalkTopology(route, town, 2U), ErrandsWalkTopology::TooLong);
}

TEST(QuestGiverTravelPolicyTest, ErrandsTopologyRejectsReachablePrefixBeforeTransportOrMapBoundary)
{
    using namespace AutoWowQuestGiverTravel;
    WorldPosition const town(1, 300.0f, 40.0f, 12.0f);
    std::vector<PathNodePoint> transportRoute{
        {WorldPosition(1, 0.0f, 0.0f, 10.0f), NODE_PREPATH, 0},
        {WorldPosition(1, 100.0f, 10.0f, 10.0f), NODE_PATH, 0},
        {WorldPosition(1, 120.0f, 12.0f, 10.0f), NODE_TRANSPORT, 176310},
        {town, NODE_PATH, 0}};
    EXPECT_EQ(ClassifyErrandsWalkTopology(transportRoute, town), ErrandsWalkTopology::Transition);

    transportRoute[2] = {WorldPosition(0, 120.0f, 12.0f, 10.0f), NODE_PATH, 0};
    EXPECT_EQ(ClassifyErrandsWalkTopology(transportRoute, town), ErrandsWalkTopology::MapChange);
}

TEST(QuestGiverTravelPolicyTest, ErrandsTopologyRejectsRouteThatDoesNotReachExactTown)
{
    using namespace AutoWowQuestGiverTravel;
    WorldPosition const town(1, 300.0f, 40.0f, 12.0f);
    std::vector<PathNodePoint> const localPrefix{
        {WorldPosition(1, 0.0f, 0.0f, 10.0f), NODE_PREPATH, 0},
        {WorldPosition(1, 100.0f, 10.0f, 10.0f), NODE_PATH, 0}};

    EXPECT_EQ(ClassifyErrandsWalkTopology(localPrefix, town), ErrandsWalkTopology::WrongEndpoint);

    std::vector<PathNodePoint> wrongElevation = localPrefix;
    wrongElevation.back().point = WorldPosition(1, 300.0f, 40.0f, 30.0f);
    EXPECT_EQ(ClassifyErrandsWalkTopology(wrongElevation, town), ErrandsWalkTopology::WrongEndpoint);

    std::vector<PathNodePoint> malformed = localPrefix;
    malformed.back().point = WorldPosition(
        1, std::numeric_limits<float>::quiet_NaN(), 40.0f, 12.0f);
    EXPECT_EQ(ClassifyErrandsWalkTopology(malformed, town), ErrandsWalkTopology::InvalidCoordinates);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorTieBreaksByDirectThenPathLength)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<TravelMgrReanchorCandidateFacts> const candidates{
        TravelMgrCandidate(7, 40.0f, 20.0f), TravelMgrCandidate(7, 30.0f, 20.0f),
        TravelMgrCandidate(7, 30.0f, 10.0f), TravelMgrCandidate(6, 5.0f, 5.0f)};

    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate(candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 2u);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorFailsClosedWhenAllCandidatesAreInvalid)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts crossMap = TravelMgrCandidate(1, 10.0f, 8.0f);
    crossMap.sameMap = false;
    TravelMgrReanchorCandidateFacts tooFar = TravelMgrCandidate(2, 151.0f, 8.0f);
    TravelMgrReanchorCandidateFacts nonFinite = TravelMgrCandidate(3, 10.0f, 8.0f);
    nonFinite.point.x = std::numeric_limits<float>::quiet_NaN();
    std::vector<TravelMgrReanchorCandidateFacts> const candidates{
        TravelMgrCandidate(0, 10.0f, 8.0f, false), crossMap, tooFar, nonFinite};

    EXPECT_FALSE(SelectTravelMgrReanchorCandidate(candidates).has_value());
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorAllowsTheQuestSpecificLongBoundedSegment)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate = TravelMgrCandidate(4, 235.0f, 240.0f);
    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate({candidate}, 300.0f);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 0u);
}

TEST(QuestGiverTravelPolicyTest, SafeIncompleteTravelMgrFrontierIsExecutableButNotComplete)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate = TravelMgrSafeIncompleteCandidate(4, 145.0f, 70.0f);

    EXPECT_FALSE(IsCompleteReprobe(candidate.freshProbe));
    EXPECT_TRUE(IsSafeIncompleteTravelMgrProbe(candidate.freshProbe));
    EXPECT_TRUE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));
    EXPECT_EQ(SelectTravelMgrSegmentCandidate({candidate}, 300.0f).value(), 0U);
}

TEST(QuestGiverTravelPolicyTest, SafeIncompleteTravelMgrFrontierRequiresGroundAndForwardProof)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate = TravelMgrSafeIncompleteCandidate(4, 145.0f, 70.0f);

    candidate.freshProbe.validHeight = false;
    EXPECT_FALSE(IsSafeIncompleteTravelMgrProbe(candidate.freshProbe));
    EXPECT_FALSE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));

    candidate = TravelMgrSafeIncompleteCandidate(4, 145.0f, 70.0f, 0.0f);
    EXPECT_FALSE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));

    candidate = TravelMgrSafeIncompleteCandidate(4, 145.0f, 70.0f);
    candidate.freshProbe.pathType |= PATHFIND_NOPATH;
    EXPECT_FALSE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));
}

TEST(QuestGiverTravelPolicyTest, SafeIncompleteTravelMgrDetourUsesDestinationDirectedRouteProgress)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts candidate = TravelMgrSafeIncompleteCandidate(4, 145.0f, 70.0f, -2.0f);
    candidate.routeDistanceReduction = 18.0f;

    // The local detour moves away from the finisher in straight-line space, but it advances on
    // the freshly selected destination-directed route. That routed proof is admissible.
    EXPECT_TRUE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));
    EXPECT_EQ(SelectTravelMgrSegmentCandidate({candidate}, 300.0f).value(), 0U);

    candidate.routeDistanceReduction = 0.0f;
    EXPECT_FALSE(IsUsableTravelMgrSegmentCandidate(candidate, 300.0f));
}

TEST(QuestGiverTravelPolicyTest, TravelMgrSegmentSelectorUsesFrontierBeforeTransitionAndStaysBounded)
{
    using namespace AutoWowQuestGiverTravel;
    TravelMgrReanchorCandidateFacts afterTransition =
        TravelMgrSafeIncompleteCandidate(5, 280.0f, 90.0f);
    TravelMgrReanchorCandidateFacts transition = TravelMgrCandidate(3, 120.0f, 30.0f);
    transition.nodeKind = TravelMgrRouteNodeKind::Transition;

    std::vector<TravelMgrReanchorCandidateFacts> const candidates{
        TravelMgrSafeIncompleteCandidate(1, 145.0f, 45.0f),
        TravelMgrSafeIncompleteCandidate(2, 250.0f, 80.0f), transition, afterTransition};
    std::optional<std::size_t> const selected =
        SelectTravelMgrSegmentCandidate(candidates, 300.0f);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 1U);
    EXPECT_EQ(candidates[*selected].routeIndex, 2U);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrReanchorRequiresFiniteTargetProgress)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<TravelMgrReanchorCandidateFacts> const mixed{
        TravelMgrCandidate(2, 20.0f, 12.0f, true, 4.0f),
        TravelMgrCandidate(9, 30.0f, 16.0f, true, -3.0f)};
    std::optional<std::size_t> const selected = SelectTravelMgrReanchorCandidate(mixed);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(mixed[*selected].routeIndex, 2U);

    std::vector<TravelMgrReanchorCandidateFacts> const regressive{
        TravelMgrCandidate(4, 20.0f, 12.0f, true, -1.0f),
        TravelMgrCandidate(5, 30.0f, 16.0f, true, 0.0f)};
    EXPECT_FALSE(SelectTravelMgrReanchorCandidate(regressive).has_value());

    TravelMgrReanchorCandidateFacts nonFinite = TravelMgrCandidate(6, 20.0f, 12.0f);
    nonFinite.targetDistanceReduction = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(SelectTravelMgrReanchorCandidate({nonFinite}).has_value());
}

TEST(QuestGiverTravelPolicyTest, CompletePartialSelectorRejectsForbiddenFlags)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{start, {1, 80.0f, 0.0f, 0.0f}};
    for (std::uint32_t const forbidden : {
             static_cast<std::uint32_t>(PATHFIND_INCOMPLETE),
             static_cast<std::uint32_t>(PATHFIND_NOPATH),
             static_cast<std::uint32_t>(PATHFIND_NOT_USING_PATH),
             static_cast<std::uint32_t>(PATHFIND_SHORT),
             static_cast<std::uint32_t>(PATHFIND_FARFROMPOLY)})
    {
        PathFacts const forbiddenFacts{true, true, true, PATHFIND_NORMAL | forbidden,
                                       4, 0.5f, 80.0f};
        EXPECT_FALSE(SelectCompletePartialWaypoint(
                         path, start, std::vector<PathFacts>{PathFacts{}, forbiddenFacts})
                         .has_value())
            << forbidden;
    }
}

TEST(QuestGiverTravelPolicyTest, TinyUnsafeInitialPathProducesBoundedNoProgressFeedback)
{
    using namespace AutoWowQuestGiverTravel;
    PathFacts const tinyUnsafe{true, false, true, PATHFIND_NORMAL | PATHFIND_INCOMPLETE,
                               2, 386.181f, 4.01523f};
    EXPECT_EQ(SelectStep(tinyUnsafe), StepKind::PartialPath);

    AutoWowQuestAcquisition::MovementOrderFacts const inactiveOrder{
        true, true, false, false, false};
    EXPECT_EQ(AutoWowQuestAcquisition::ClassifyMovementActivation(inactiveOrder),
              AutoWowQuestAcquisition::MovementActivationFailure::TravelStrategyMissing);
    EXPECT_FALSE(AutoWowQuestAcquisition::IsMovementOrderActive(inactiveOrder));

    constexpr std::uint32_t botGuid = 9827;
    ClearMovementFeedback(botGuid);
    RecordMovementFeedback(botGuid, StepKind::PartialPath,
                           MovementFeedbackResult::NoProgress, false, 386.181f);
    MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    EXPECT_EQ(feedback->sequence, 1U);
    EXPECT_EQ(feedback->observations, 1U);
    EXPECT_EQ(feedback->movementAttempts, 0U);
    EXPECT_EQ(feedback->noProgressChecks, 1U);
    ClearMovementFeedback(botGuid);
}

TEST(QuestGiverTravelPolicyTest, ControlledStagedEntryKeepsSafetyGatesDeterministic)
{
    using namespace AutoWowQuestGiverTravel;
    StagedEntryFacts safe{true, true, true, false, false, false, true, false};
    EXPECT_EQ(EvaluateStagedEntry(safe), StagedEntryDecision::Execute);

    safe.moving = true;
    EXPECT_EQ(EvaluateStagedEntry(safe), StagedEntryDecision::Deferred);

    for (StagedEntryFacts blocked : {
             StagedEntryFacts{false, true, true, false, false, false, true, false},
             StagedEntryFacts{true, false, true, false, false, false, true, false},
             StagedEntryFacts{true, true, false, false, false, false, true, false},
             StagedEntryFacts{true, true, true, true, false, false, true, false},
             StagedEntryFacts{true, true, true, false, true, false, true, false},
             StagedEntryFacts{true, true, true, false, false, false, false, false},
             StagedEntryFacts{true, true, true, false, false, false, true, true}})
    {
        EXPECT_EQ(EvaluateStagedEntry(blocked), StagedEntryDecision::NoProgress);
    }
}

TEST(QuestGiverTravelPolicyTest, CompleteSafePathUsesTheGiverEndpoint)
{
    PathFacts const complete{true, true, true, PATHFIND_NORMAL, 6, 0.5f, 386.0f};
    EXPECT_EQ(AutoWowQuestGiverTravel::SelectStep(complete), StepKind::Direct);
}

TEST(QuestGiverTravelPolicyTest, PartialWaypointSelectionIsDeterministicAndBounded)
{
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{
        start,
        {1, 1.0f, 0.0f, 0.0f},
        {1, 20.0f, 0.0f, 0.0f},
        {1, 80.0f, 0.0f, 0.0f},
        {1, 140.0f, 0.0f, 0.0f},
    };
    std::optional<std::size_t> const selected =
        AutoWowQuestGiverTravel::SelectPartialWaypoint(path, start);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 3u);
}

TEST(QuestGiverTravelPolicyTest, InvalidOrEmptyPartialPathFailsClosed)
{
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    EXPECT_FALSE(AutoWowQuestGiverTravel::SelectPartialWaypoint({start}, start).has_value());
    EXPECT_FALSE(AutoWowQuestGiverTravel::SelectPartialWaypoint(
        {start, {2, 20.0f, 0.0f, 0.0f}}, start).has_value());
}

TEST(QuestGiverTravelPolicyTest, RejectsAnUnboundedFirstPointInsteadOfFallingBack)
{
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const path{start, {1, 250.0f, 0.0f, 0.0f}};
    EXPECT_FALSE(AutoWowQuestGiverTravel::SelectPartialWaypoint(
        path, start, 2.0f, 120.0f).has_value());
}

TEST(QuestGiverTravelPolicyTest, LocalDetourSelectsBestSafeProgressCandidate)
{
    using AutoWowQuestGiverTravel::LocalDetourCandidateFacts;
    std::vector<LocalDetourCandidateFacts> const candidates{
        {true, true, true, 12.0f, 4.0f},
        {true, true, true, 24.0f, 12.0f},
        {true, true, true, 8.0f, 7.0f}};
    std::optional<std::size_t> const selected =
        AutoWowQuestGiverTravel::SelectLocalDetour(candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 1u);
}

TEST(QuestGiverTravelPolicyTest, LocalDetourRejectsUnsafeIncompleteAndNonProgressCandidates)
{
    using AutoWowQuestGiverTravel::LocalDetourCandidateFacts;
    std::vector<LocalDetourCandidateFacts> const candidates{
        {false, true, true, 8.0f, 8.0f},
        {true, false, true, 8.0f, 8.0f},
        {true, true, false, 8.0f, 8.0f},
        {true, true, true, 40.0f, 8.0f},
        {true, true, true, 8.0f, 0.25f}};
    EXPECT_FALSE(AutoWowQuestGiverTravel::SelectLocalDetour(candidates).has_value());
    EXPECT_FALSE(AutoWowQuestGiverTravel::SelectLocalDetour({}).has_value());
}

TEST(QuestGiverTravelPolicyTest, StrictLocalDetourRejectsNormalIncomplete)
{
    using namespace AutoWowQuestGiverTravel;
    LocalDetourCandidateFacts incomplete;
    incomplete.sameMap = true;
    incomplete.finite = true;
    incomplete.completeNormalPath = true;
    incomplete.routeLength = 8.0f;
    incomplete.distanceReduction = 8.0f;
    incomplete.pathType = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    EXPECT_FALSE(IsUsableLocalDetour(incomplete));
    EXPECT_FALSE(SelectLocalDetour({incomplete}).has_value());
}

TEST(QuestGiverTravelPolicyTest, LocalDetourGroundLineRequiresExplicitProof)
{
    using namespace AutoWowQuestGiverTravel;
    LocalDetourCandidateFacts groundLine;
    groundLine.sameMap = true;
    groundLine.finite = true;
    groundLine.completeNormalPath = false;
    groundLine.routeLength = 8.0f;
    groundLine.distanceReduction = 4.0f;
    groundLine.pathType = PATHFIND_BLANK;
    groundLine.validHeight = true;
    groundLine.validatedGroundLine = true;
    EXPECT_TRUE(IsUsableLocalDetour(groundLine));

    LocalDetourCandidateFacts identicalWithoutProof = groundLine;
    identicalWithoutProof.validatedGroundLine = false;
    EXPECT_FALSE(IsUsableLocalDetour(identicalWithoutProof));

    LocalDetourCandidateFacts forbiddenFlags = groundLine;
    forbiddenFlags.pathType = PATHFIND_NOPATH;
    EXPECT_FALSE(IsUsableLocalDetour(forbiddenFlags));
}

TEST(QuestGiverTravelPolicyTest, EscapeRingHasFixedFullCircleOrder)
{
    using namespace AutoWowQuestGiverTravel;
    Waypoint const start{1, 0.0f, 0.0f, 0.0f};
    Waypoint const target{1, 100.0f, 0.0f, 0.0f};
    std::vector<Waypoint> const first = BuildEscapeRing(start, target);
    std::vector<Waypoint> const second = BuildEscapeRing(start, target);
    ASSERT_EQ(kEscapeRingRadii.size(), 5U);
    EXPECT_FLOAT_EQ(kEscapeRingRadii[0], 4.0f);
    EXPECT_FLOAT_EQ(kEscapeRingRadii[1], 8.0f);
    EXPECT_FLOAT_EQ(kEscapeRingRadii[2], 16.0f);
    EXPECT_FLOAT_EQ(kEscapeRingRadii[3], 32.0f);
    EXPECT_FLOAT_EQ(kEscapeRingRadii[4], 48.0f);
    ASSERT_EQ(first.size(), kEscapeRingRadii.size() * kEscapeRingAngles.size());
    EXPECT_EQ(first.size(), 80U);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index].mapId, second[index].mapId);
        EXPECT_FLOAT_EQ(first[index].x, second[index].x);
        EXPECT_FLOAT_EQ(first[index].y, second[index].y);
        EXPECT_FLOAT_EQ(first[index].z, second[index].z);
    }
    EXPECT_EQ(first.front().mapId, 1U);
    EXPECT_NEAR(Distance2d(start, first.front()), kEscapeRingRadii.front(), 0.01f);
    for (std::size_t radiusIndex = 0; radiusIndex < kEscapeRingRadii.size(); ++radiusIndex)
    {
        std::size_t const firstAngleIndex = radiusIndex * kEscapeRingAngles.size();
        EXPECT_NEAR(Distance2d(start, first[firstAngleIndex]), kEscapeRingRadii[radiusIndex], 0.01f);
    }
}

TEST(QuestGiverTravelPolicyTest, EscapePrefersBestImprovingStrictCandidateDeterministically)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<EscapeCandidateFacts> const candidates{
        EscapeCandidate(4.0f, 12.0f, 0),
        EscapeCandidate(9.0f, 30.0f, 1),
        EscapeCandidate(9.0f, 10.0f, 2)};
    EscapeSelection const selected = SelectEscapeCandidate(candidates, {});
    ASSERT_TRUE(selected.index.has_value());
    EXPECT_EQ(*selected.index, 2U);
    EXPECT_EQ(selected.kind, EscapeSelectionKind::Improving);
}

TEST(QuestGiverTravelPolicyTest, EscapePermitsSidewaysThenLeastRegressiveFallback)
{
    using namespace AutoWowQuestGiverTravel;
    std::vector<EscapeCandidateFacts> const sideways{
        EscapeCandidate(0.0f, 14.0f, 0), EscapeCandidate(-3.0f, 10.0f, 1)};
    EscapeSelection const sidewaysSelection = SelectEscapeCandidate(sideways, {});
    ASSERT_TRUE(sidewaysSelection.index.has_value());
    EXPECT_EQ(*sidewaysSelection.index, 0U);
    EXPECT_EQ(sidewaysSelection.kind, EscapeSelectionKind::Sideways);

    std::vector<EscapeCandidateFacts> const regressive{
        EscapeCandidate(-12.0f, 14.0f, 0), EscapeCandidate(-3.0f, 10.0f, 1)};
    EscapeSelection const regressiveSelection = SelectEscapeCandidate(regressive, {});
    ASSERT_TRUE(regressiveSelection.index.has_value());
    EXPECT_EQ(*regressiveSelection.index, 1U);
    EXPECT_EQ(regressiveSelection.kind, EscapeSelectionKind::Regressive);
}

TEST(QuestGiverTravelPolicyTest, ShortRegressiveEscapeCandidateRemainsBoundedAndSelectable)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget budget;
    EscapeCandidateFacts shortRegressive = EscapeCandidate(-0.25f, 4.0f, 0);

    EXPECT_TRUE(IsUsableEscapeCandidate(shortRegressive, budget));
    EscapeSelection const selection = SelectEscapeCandidate({shortRegressive}, budget);
    ASSERT_TRUE(selection.index.has_value());
    EXPECT_EQ(*selection.index, 0U);
    EXPECT_EQ(selection.kind, EscapeSelectionKind::Regressive);

    EXPECT_TRUE(ConsumeEscapeCandidate(budget, shortRegressive));
    EXPECT_EQ(budget.attempts, 1U);
    EXPECT_FLOAT_EQ(budget.totalDistance, 4.0f);
    EXPECT_FLOAT_EQ(budget.totalRegression, 0.25f);
}

TEST(QuestGiverTravelPolicyTest, EscapeRejectsVisitedAndUnsafeCandidates)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeCandidateFacts visited = EscapeCandidate(10.0f, 10.0f, 0);
    std::vector<QuantizedCell> const visitedCells{visited.cell};
    EXPECT_FALSE(SelectEscapeCandidate({visited}, {}, visitedCells).index.has_value());

    EscapeCandidateFacts incomplete = EscapeCandidate(10.0f, 10.0f, 1);
    incomplete.pathType = PATHFIND_NORMAL | PATHFIND_INCOMPLETE;
    EscapeCandidateFacts unsafe = EscapeCandidate(10.0f, 10.0f, 2);
    unsafe.freshlyStrictComplete = false;
    EscapeCandidateFacts crossMap = EscapeCandidate(10.0f, 10.0f, 3);
    crossMap.sameMap = false;
    EscapeCandidateFacts invalidHeight = EscapeCandidate(10.0f, 10.0f, 4);
    invalidHeight.validHeight = false;
    EscapeCandidateFacts nonFinite = EscapeCandidate(10.0f, 10.0f, 5);
    nonFinite.finite = false;
    EXPECT_FALSE(SelectEscapeCandidate(
                     {incomplete, unsafe, crossMap, invalidHeight, nonFinite}, {})
                     .index.has_value());
}

TEST(QuestGiverTravelPolicyTest, EscapeGroundLineRequiresExplicitProof)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeCandidateFacts groundLine = EscapeCandidate(1.0f, 8.0f, 0);
    groundLine.pathType = PATHFIND_BLANK;
    groundLine.validatedGroundLine = true;
    EXPECT_TRUE(IsUsableEscapeCandidate(groundLine, {}));
    EXPECT_TRUE(SelectEscapeCandidate({groundLine}, {}).index.has_value());

    EscapeCandidateFacts identicalWithoutProof = groundLine;
    identicalWithoutProof.validatedGroundLine = false;
    EXPECT_FALSE(IsUsableEscapeCandidate(identicalWithoutProof, {}));

    EscapeCandidateFacts forbiddenFlags = groundLine;
    forbiddenFlags.pathType = PATHFIND_NOPATH;
    EXPECT_FALSE(IsUsableEscapeCandidate(forbiddenFlags, {}));
}

TEST(QuestGiverTravelPolicyTest, EscapeBudgetBoundsRegressionAndTotalDistance)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget regressionBudget;
    regressionBudget.totalRegression = kMaxEscapeTotalRegression - 2.0f;
    EscapeCandidateFacts regressive = EscapeCandidate(-3.0f, 10.0f, 0);
    EXPECT_FALSE(IsUsableEscapeCandidate(regressive, regressionBudget));
    EXPECT_FALSE(SelectEscapeCandidate({regressive}, regressionBudget).index.has_value());

    EscapeBudget distanceBudget;
    distanceBudget.totalDistance = kMaxEscapeTotalDistance - 5.0f;
    EscapeCandidateFacts tooFar = EscapeCandidate(2.0f, 10.0f, 0);
    EXPECT_FALSE(IsUsableEscapeCandidate(tooFar, distanceBudget));

    EscapeBudget consumed;
    EscapeCandidateFacts accepted = EscapeCandidate(-4.0f, 20.0f, 0);
    EXPECT_TRUE(ConsumeEscapeCandidate(consumed, accepted));
    EXPECT_EQ(consumed.attempts, 1U);
    EXPECT_FLOAT_EQ(consumed.totalDistance, 20.0f);
    EXPECT_FLOAT_EQ(consumed.totalRegression, 4.0f);
}

TEST(QuestGiverTravelPolicyTest, EscapeExhaustionIsExplicitAndBounded)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget attemptsExhausted;
    attemptsExhausted.attempts = kMaxEscapeAttempts;
    EXPECT_TRUE(IsEscapeExhausted(attemptsExhausted));
    EXPECT_EQ(SelectEscapeCandidate({EscapeCandidate(10.0f, 10.0f, 0)}, attemptsExhausted).kind,
              EscapeSelectionKind::Exhausted);

    EscapeBudget terminal;
    terminal.exhausted = true;
    EXPECT_TRUE(IsEscapeExhausted(terminal));
    EXPECT_EQ(SelectEscapeCandidate({EscapeCandidate(10.0f, 10.0f, 0)}, terminal).kind,
              EscapeSelectionKind::Exhausted);
}

TEST(QuestGiverTravelPolicyTest, EscapeReplanRequiresMeaningfulProgressAndResetsOnlyEpochState)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget budget;
    ASSERT_TRUE(BeginInitialEscapeEpoch(budget, 100.0f));
    EscapeCandidateFacts accepted = EscapeCandidate(10.0f, 20.0f, 0);
    ASSERT_TRUE(ConsumeEscapeCandidate(budget, accepted));
    ExhaustEscapeEpoch(budget);
    ObserveEscapeProgress(budget, 92.0f);

    EXPECT_TRUE(HasMeaningfulEscapeProgress(budget));
    EXPECT_TRUE(CanBeginEscapeReplan(budget, 92.0f));
    ASSERT_TRUE(BeginEscapeReplan(budget, 92.0f));
    EXPECT_EQ(budget.epochCount, 2U);
    EXPECT_EQ(budget.attempts, 0U);
    EXPECT_EQ(budget.journeyAttempts, 1U);
    EXPECT_FLOAT_EQ(budget.totalDistance, 20.0f);
    EXPECT_FLOAT_EQ(budget.epochDistance, 0.0f);
    EXPECT_FLOAT_EQ(budget.epochStartRemainingDistance, 92.0f);
    EXPECT_FLOAT_EQ(budget.bestRemainingDistance, 92.0f);
    EXPECT_FALSE(budget.exhausted);
}

TEST(QuestGiverTravelPolicyTest, EscapeReplanRejectsStagnationAndSmallNoise)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget budget;
    ASSERT_TRUE(BeginInitialEscapeEpoch(budget, 100.0f));
    ExhaustEscapeEpoch(budget);
    ObserveEscapeProgress(budget, 97.0f);

    EXPECT_FALSE(HasMeaningfulEscapeProgress(budget));
    EXPECT_FALSE(CanBeginEscapeReplan(budget, 97.0f));
    EXPECT_FALSE(BeginEscapeReplan(budget, 97.0f));
    EXPECT_EQ(budget.epochCount, 1U);
    EXPECT_TRUE(budget.exhausted);

    ObserveEscapeProgress(budget, 92.0f);
    ObserveEscapeProgress(budget, 99.0f);
    EXPECT_FALSE(HasMeaningfulEscapeProgress(budget));
    EXPECT_FALSE(CanBeginEscapeReplan(budget, 99.0f));
}

TEST(QuestGiverTravelPolicyTest, EscapeReplanHonorsEpochAndJourneyWideCaps)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget epochs;
    ASSERT_TRUE(BeginInitialEscapeEpoch(epochs, 100.0f));
    ExhaustEscapeEpoch(epochs);
    EXPECT_TRUE(BeginEscapeReplan(epochs, 90.0f));
    ExhaustEscapeEpoch(epochs);
    EXPECT_TRUE(BeginEscapeReplan(epochs, 80.0f));
    ExhaustEscapeEpoch(epochs);
    EXPECT_FALSE(CanBeginEscapeReplan(epochs, 70.0f));
    EXPECT_FALSE(BeginEscapeReplan(epochs, 70.0f));
    EXPECT_EQ(epochs.epochCount, kMaxEscapeEpochs);

    EscapeBudget journeyAttempts;
    ASSERT_TRUE(BeginInitialEscapeEpoch(journeyAttempts, 100.0f));
    journeyAttempts.journeyAttempts = kMaxEscapeJourneyAttempts;
    ExhaustEscapeEpoch(journeyAttempts);
    EXPECT_FALSE(CanBeginEscapeReplan(journeyAttempts, 80.0f));

    EscapeBudget journeyDistance;
    ASSERT_TRUE(BeginInitialEscapeEpoch(journeyDistance, 100.0f));
    journeyDistance.totalDistance = kMaxEscapeTotalDistance;
    ExhaustEscapeEpoch(journeyDistance);
    EXPECT_FALSE(CanBeginEscapeReplan(journeyDistance, 80.0f));
}

TEST(QuestGiverTravelPolicyTest, EscapeReplanAndSelectionRemainDeterministic)
{
    using namespace AutoWowQuestGiverTravel;
    EscapeBudget first;
    EscapeBudget second;
    ASSERT_TRUE(BeginInitialEscapeEpoch(first, 100.0f));
    ASSERT_TRUE(BeginInitialEscapeEpoch(second, 100.0f));
    ExhaustEscapeEpoch(first);
    ExhaustEscapeEpoch(second);
    ObserveEscapeProgress(first, 88.0f);
    ObserveEscapeProgress(second, 88.0f);
    ASSERT_TRUE(BeginEscapeReplan(first, 88.0f));
    ASSERT_TRUE(BeginEscapeReplan(second, 88.0f));

    std::vector<EscapeCandidateFacts> const candidates{
        EscapeCandidate(8.0f, 22.0f, 0), EscapeCandidate(8.0f, 14.0f, 1),
        EscapeCandidate(4.0f, 8.0f, 2)};
    EscapeSelection const firstSelection = SelectEscapeCandidate(candidates, first);
    EscapeSelection const secondSelection = SelectEscapeCandidate(candidates, second);
    ASSERT_TRUE(firstSelection.index.has_value());
    ASSERT_TRUE(secondSelection.index.has_value());
    EXPECT_EQ(*firstSelection.index, *secondSelection.index);
    EXPECT_EQ(firstSelection.kind, secondSelection.kind);
    EXPECT_EQ(first.epochCount, second.epochCount);
    EXPECT_FLOAT_EQ(first.epochStartRemainingDistance, second.epochStartRemainingDistance);
    EXPECT_FLOAT_EQ(first.bestRemainingDistance, second.bestRemainingDistance);
}

TEST(QuestGiverTravelPolicyTest, LocalDetourTieBreakIsDeterministic)
{
    using AutoWowQuestGiverTravel::LocalDetourCandidateFacts;
    std::vector<LocalDetourCandidateFacts> const candidates{
        {true, true, true, 8.0f, 7.0f},
        {true, true, true, 8.0f, 7.0f},
        {true, true, true, 9.0f, 7.0f}};
    std::optional<std::size_t> const selected =
        AutoWowQuestGiverTravel::SelectLocalDetour(candidates);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, 0u);
}

TEST(QuestGiverTravelPolicyTest, LifecycleDecisionsAreExplicitAndBounded)
{
    using namespace AutoWowQuestGiverTravel;
    EXPECT_EQ(LifecycleDecisionName(LifecycleDecision::BlockedTimeout), "acquisition_timeout");
    EXPECT_EQ(LifecycleDecisionName(LifecycleDecision::BlockedMovement), "movement_retry_limit");
    EXPECT_EQ(LifecycleDecisionName(LifecycleDecision::BlockedNoProgress),
              "movement_no_progress_timeout");
    EXPECT_EQ(LifecycleDecisionName(LifecycleDecision::BlockedStaleGiver), "stale_giver");
    EXPECT_EQ(LifecycleDecisionName(LifecycleDecision::BlockedPartyArrival),
              "party_arrival_timeout");
}

TEST(QuestGiverTravelPolicyTest, LongHealthyRouteDoesNotSpendFailureBudgetOnSuccessfulSegments)
{
    using namespace AutoWowQuestGiverTravel;
    std::uint32_t const deadline = AcquisitionDeadlineMs(3878.58, 7.0f);
    EXPECT_GT(deadline, kBaseAcquisitionLifetimeMs);

    LifecycleFacts facts;
    facts.routeDeadlineMs = deadline;
    facts.elapsedMs = 0;
    facts.movementAttempts = 26; // q954 needs more than the old 24-attempt cap.
    facts.movementRejects = 0;
    facts.noProgressChecks = 0;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::Continue);

    facts.elapsedMs = deadline;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedTimeout);
}

TEST(QuestGiverTravelPolicyTest, FailureAndWaitBudgetsRemainWallClockBounded)
{
    using namespace AutoWowQuestGiverTravel;
    LifecycleFacts facts;
    facts.movementRejects = kMaxMovementRejects;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedMovement);

    facts = {};
    facts.noProgressChecks = kMaxNoProgressChecks;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedNoProgress);

    facts = {};
    facts.staleGiverWaitMs = kMaxStaleGiverWaitMs;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedStaleGiver);

    facts = {};
    facts.partyArrivalWaitMs = kMaxPartyArrivalWaitMs;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedPartyArrival);
}

TEST(QuestGiverTravelPolicyTest, MovementFeedbackKeepsLatestResultAndHardRejectCount)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 9917;
    ClearMovementFeedback(botGuid);

    RecordMovementFeedback(botGuid, StepKind::PartialPath, MovementFeedbackResult::Accepted,
                           true, 310.0f);
    RecordMovementFeedback(botGuid, StepKind::PartialPath, MovementFeedbackResult::Rejected,
                           false, 310.0f);
    MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    EXPECT_EQ(feedback->observations, 2U);
    EXPECT_EQ(feedback->movementAttempts, 2U);
    EXPECT_EQ(feedback->rejectedAttempts, 1U);
    EXPECT_EQ(feedback->result, MovementFeedbackResult::Rejected);
    EXPECT_EQ(feedback->step, StepKind::PartialPath);

    RecordMovementFeedback(botGuid, StepKind::Blocked, MovementFeedbackResult::Parked,
                           false, 4.0f);
    feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    EXPECT_EQ(feedback->observations, 3U);
    EXPECT_EQ(feedback->movementAttempts, 2U);
    EXPECT_EQ(feedback->rejectedAttempts, 1U);
    EXPECT_EQ(feedback->result, MovementFeedbackResult::Parked);
    ClearMovementFeedback(botGuid);
    EXPECT_EQ(ReadMovementFeedback(botGuid), nullptr);
}

TEST(QuestGiverTravelPolicyTest, AcceptedPartialStepRefreshesAreDeferredNotHardRejects)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 9918;
    ClearMovementFeedback(botGuid);

    EXPECT_TRUE(IsMovementRefreshDeferred(true, false));
    EXPECT_TRUE(IsMovementRefreshDeferred(false, true));
    EXPECT_FALSE(IsMovementRefreshDeferred(false, false));

    RecordMovementFeedback(botGuid, StepKind::PartialPath, MovementFeedbackResult::Accepted,
                           true, 350.0f);
    QuestAcquisitionJourneyRuntime journey(1000, 386.181, 7.0f);
    MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    journey.ApplyStagedMoverOutcome(*feedback);
    EXPECT_TRUE(journey.Lifecycle().MovementAccepted());

    for (std::uint32_t refresh = 0; refresh < 6; ++refresh)
    {
        RecordMovementFeedback(botGuid, StepKind::Blocked, MovementFeedbackResult::Deferred,
                               true, 350.0f);
        feedback = ReadMovementFeedback(botGuid);
        ASSERT_NE(feedback, nullptr);
        journey.ApplyStagedMoverOutcome(*feedback);
        EXPECT_TRUE(journey.Lifecycle().MovementAccepted());
        EXPECT_EQ(feedback->movementAttempts, 1U);
        EXPECT_EQ(feedback->rejectedAttempts, 0U);

        LifecycleFacts facts;
        facts.movementAttempts = feedback->movementAttempts;
        facts.movementRejects = feedback->rejectedAttempts;
        EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::Continue);
    }

    // Once the reserved segment is complete, a new accepted partial step continues the route.
    RecordMovementFeedback(botGuid, StepKind::PartialPath, MovementFeedbackResult::Accepted,
                           true, 210.0f);
    feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    journey.ApplyStagedMoverOutcome(*feedback);
    EXPECT_TRUE(journey.Lifecycle().MovementAccepted());
    EXPECT_EQ(feedback->movementAttempts, 2U);
    EXPECT_EQ(feedback->rejectedAttempts, 0U);
    ClearMovementFeedback(botGuid);
}

TEST(QuestGiverTravelPolicyTest, NoProgressStillTerminatesExplicitlyAfterDeferredRefreshes)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 9919;
    ClearMovementFeedback(botGuid);

    RecordMovementFeedback(botGuid, StepKind::PartialPath, MovementFeedbackResult::Accepted,
                           true, 350.0f);
    for (std::uint32_t refresh = 0; refresh < 8; ++refresh)
        RecordMovementFeedback(botGuid, StepKind::Blocked, MovementFeedbackResult::Deferred,
                               true, 350.0f);

    for (std::uint32_t check = 0; check < kMaxNoProgressChecks - 1U; ++check)
        RecordMovementFeedback(botGuid, StepKind::TravelMgrSegment,
                               MovementFeedbackResult::NoProgress, false, 350.0f);

    MovementFeedback const* feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    EXPECT_EQ(feedback->movementAttempts, 1U);
    EXPECT_EQ(feedback->rejectedAttempts, 0U);
    EXPECT_EQ(feedback->noProgressChecks, kMaxNoProgressChecks - 1U);

    LifecycleFacts facts;
    facts.movementAttempts = feedback->movementAttempts;
    facts.movementRejects = feedback->rejectedAttempts;
    facts.noProgressChecks = feedback->noProgressChecks;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::Continue);

    // The final unsafe/no-route observation reaches an explicit no-progress terminal state even
    // when the refresh stream has not consumed the hard-rejection budget.
    RecordMovementFeedback(botGuid, StepKind::TravelMgrSegment,
                           MovementFeedbackResult::NoProgress, false, 350.0f);
    feedback = ReadMovementFeedback(botGuid);
    ASSERT_NE(feedback, nullptr);
    facts.noProgressChecks = feedback->noProgressChecks;
    EXPECT_EQ(EvaluateLifecycle(facts), LifecycleDecision::BlockedNoProgress);
    EXPECT_EQ(feedback->rejectedAttempts, 0U);
    EXPECT_EQ(facts.movementRejects, 0U);
    ClearMovementFeedback(botGuid);
}

TEST(QuestGiverTravelPolicyTest, TravelMgrDiagnosticsAreBoundedReadableAndClearedWithJourney)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 0xA117U;
    ClearMovementFeedback(botGuid);

    TravelMgrDiagnostics diagnostics;
    diagnostics.unattachedStartAllowed = true;
    diagnostics.pathPointCount = 1404959U;
    diagnostics.scanPointsConsidered = static_cast<std::uint32_t>(kMaxTravelMgrReanchorScanPoints);
    diagnostics.freshProbeAttempts = 7U;
    diagnostics.freshProbeAccepts = 0U;
    diagnostics.reason = TravelMgrDiagnosticReason::FreshProbeIncomplete;
    RecordTravelMgrDiagnostics(botGuid, diagnostics);

    TravelMgrDiagnostics const* observed = ReadTravelMgrDiagnostics(botGuid);
    ASSERT_NE(observed, nullptr);
    EXPECT_TRUE(observed->unattachedStartAllowed);
    EXPECT_EQ(observed->scanPointsConsidered, kMaxTravelMgrReanchorScanPoints);
    EXPECT_EQ(observed->freshProbeAttempts, 7U);
    EXPECT_EQ(observed->freshProbeAccepts, 0U);
    EXPECT_EQ(TravelMgrDiagnosticReasonName(observed->reason), "fresh_probe_incomplete");

    ClearMovementFeedback(botGuid);
    EXPECT_EQ(ReadTravelMgrDiagnostics(botGuid), nullptr);
}

TEST(QuestGiverTravelPolicyTest, StagedEntryDiagnosticsPreserveExactFactsAndDecision)
{
    using namespace AutoWowQuestGiverTravel;
    constexpr std::uint32_t botGuid = 0xA118U;
    ClearMovementFeedback(botGuid);

    StagedEntryFacts facts;
    facts.activityAllowed = true;
    facts.exactQuestAcquisitionTarget = true;
    facts.travelStatusActive = true;
    facts.inFlight = false;
    facts.flying = false;
    facts.moving = true;
    facts.canMoveAround = true;
    facts.lootPossible = false;
    StagedEntryDecision const decision = EvaluateStagedEntry(facts);
    ASSERT_EQ(decision, StagedEntryDecision::Deferred);

    RecordStagedEntryDiagnostics(botGuid, facts, decision);
    StagedEntryDiagnostics const* observed = ReadStagedEntryDiagnostics(botGuid);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(observed->decision, StagedEntryDecision::Deferred);
    EXPECT_EQ(observed->facts.activityAllowed, facts.activityAllowed);
    EXPECT_EQ(observed->facts.exactQuestAcquisitionTarget,
              facts.exactQuestAcquisitionTarget);
    EXPECT_EQ(observed->facts.travelStatusActive, facts.travelStatusActive);
    EXPECT_EQ(observed->facts.inFlight, facts.inFlight);
    EXPECT_EQ(observed->facts.flying, facts.flying);
    EXPECT_EQ(observed->facts.moving, facts.moving);
    EXPECT_EQ(observed->facts.canMoveAround, facts.canMoveAround);
    EXPECT_EQ(observed->facts.lootPossible, facts.lootPossible);
    EXPECT_EQ(StagedEntryDecisionName(observed->decision), "deferred");

    facts.moving = false;
    RecordStagedEntryDiagnostics(botGuid, facts, EvaluateStagedEntry(facts));
    observed = ReadStagedEntryDiagnostics(botGuid);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(observed->decision, StagedEntryDecision::Execute);
    EXPECT_FALSE(observed->facts.moving);

    ClearMovementFeedback(botGuid);
    EXPECT_EQ(ReadStagedEntryDiagnostics(botGuid), nullptr);
}
