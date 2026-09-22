/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "../src/AutoWow/CampaignTravelPolicy.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace AutoWowCampaignTravel;

static_assert(std::is_same_v<decltype(&PlanNextTransition), Decision (*)(Request const&)>,
              "The planner contract must accept immutable value facts only");

Endpoint At(std::uint32_t mapId, double x, double y, double z)
{
    return {mapId, {x, y, z}, true};
}

DirectedEdgeProof ProofFor(CandidateLeg const& candidate)
{
    return {true, candidate.source.mapId, candidate.source.coordinates,
            candidate.destination.mapId, candidate.destination.coordinates};
}

CandidateLeg Leg(std::uint64_t stableId, LegKind kind, std::uint32_t sourceMap = 0,
                 std::uint32_t destinationMap = 1)
{
    CandidateLeg candidate;
    candidate.stableId = stableId;
    candidate.kind = kind;
    candidate.source = At(sourceMap, 10.0 + stableId, 20.0, 30.0);
    candidate.destination = At(destinationMap, 40.0 + stableId, 50.0, 60.0);
    candidate.normalTransitionValidated = true;
    candidate.realDynamicTransport = true;
    candidate.allTaxiNodesKnown = true;
    candidate.directedEdge = ProofFor(candidate);
    return candidate;
}

Request Plan(std::vector<CandidateLeg> candidates, std::uint64_t money = 100,
             std::uint32_t sourceMap = 0)
{
    return {At(sourceMap, 1.0, 2.0, 3.0), money, std::move(candidates)};
}

void ExpectTransition(Decision const& decision, std::uint64_t stableId, LegKind kind,
                      bool boardingProof = false)
{
    ASSERT_EQ(decision.kind, DecisionKind::Transition);
    EXPECT_EQ(decision.blocker, Blocker::None);
    EXPECT_EQ(decision.transition.stableId, stableId);
    EXPECT_EQ(decision.transition.kind, kind);
    EXPECT_EQ(decision.runtimeBoardingProofRequired, boardingProof);
}

void ExpectBlocked(Request const& request, Blocker blocker)
{
    Decision const decision = PlanNextTransition(request);
    EXPECT_EQ(decision.kind, DecisionKind::Blocked);
    EXPECT_EQ(decision.blocker, blocker);
    EXPECT_FALSE(decision.runtimeBoardingProofRequired);
}

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(CampaignTravelPolicy, AllowsSameMapWalkingOnly)
{
    CandidateLeg walking = Leg(10, LegKind::WalkingApproach, 0, 0);
    ExpectTransition(PlanNextTransition(Plan({walking})), 10, LegKind::WalkingApproach);

    walking.destination = At(1, 40.0, 50.0, 60.0);
    ExpectBlocked(Plan({walking}), Blocker::CrossMapWalkingRejected);
}

TEST(CampaignTravelPolicy, AllowsValidatedDirectedAreaTriggerAndGameObjectPortals)
{
    CandidateLeg areaTrigger = Leg(20, LegKind::AreaTriggerPortal);
    ExpectTransition(PlanNextTransition(Plan({areaTrigger})), 20, LegKind::AreaTriggerPortal);

    CandidateLeg gameObject = Leg(21, LegKind::GameObjectPortal);
    ExpectTransition(PlanNextTransition(Plan({gameObject})), 21, LegKind::GameObjectPortal);

    areaTrigger.normalTransitionValidated = false;
    ExpectBlocked(Plan({areaTrigger}), Blocker::PortalNotValidated);
}

TEST(CampaignTravelPolicy, AllowsOnlyRealDynamicTransportAndMarksBoardingProofRequired)
{
    CandidateLeg transport = Leg(30, LegKind::Transport);
    ExpectTransition(PlanNextTransition(Plan({transport})), 30, LegKind::Transport, true);

    transport.realDynamicTransport = false;
    ExpectBlocked(Plan({transport}), Blocker::TransportNotDynamic);
}

TEST(CampaignTravelPolicy, AllowsDirectedKnownTaxiAtTheExactFareBoundary)
{
    CandidateLeg taxi = Leg(40, LegKind::Taxi);
    taxi.cost = 75;
    ExpectTransition(PlanNextTransition(Plan({taxi}, 75)), 40, LegKind::Taxi);

    ExpectBlocked(Plan({taxi}, 74), Blocker::InsufficientMoney);

    taxi.allTaxiNodesKnown = false;
    ExpectBlocked(Plan({taxi}, 1000), Blocker::UnknownTaxiNodes);
}

TEST(CampaignTravelPolicy, RejectsTeleportHearthAndUnknownLegKinds)
{
    ExpectBlocked(Plan({Leg(50, LegKind::Teleport)}), Blocker::TeleportRejected);
    ExpectBlocked(Plan({Leg(51, LegKind::Hearth)}), Blocker::HearthRejected);
    ExpectBlocked(Plan({Leg(52, LegKind::Unknown)}), Blocker::UnknownLegKind);
}

TEST(CampaignTravelPolicy, RejectsSeedGoldAndTravelCheats)
{
    CandidateLeg taxi = Leg(60, LegKind::Taxi);
    taxi.cheats.seedGold = true;
    ExpectBlocked(Plan({taxi}), Blocker::SeedGoldRejected);

    taxi.cheats.seedGold = false;
    taxi.cheats.travelCheat = true;
    ExpectBlocked(Plan({taxi}), Blocker::CheatRejected);
}

TEST(CampaignTravelPolicy, RejectsInvalidRequestAndCandidateMaps)
{
    Request invalidRequest = Plan({Leg(70, LegKind::Taxi)});
    invalidRequest.source.mapValidated = false;
    ExpectBlocked(invalidRequest, Blocker::InvalidRequestSourceMap);

    invalidRequest = Plan({Leg(70, LegKind::Taxi)});
    invalidRequest.source.mapId = kInvalidMapId;
    ExpectBlocked(invalidRequest, Blocker::InvalidRequestSourceMap);

    CandidateLeg invalidSource = Leg(71, LegKind::Taxi);
    invalidSource.source.mapValidated = false;
    ExpectBlocked(Plan({invalidSource}), Blocker::InvalidCandidateMap);

    CandidateLeg invalidDestination = Leg(72, LegKind::Taxi);
    invalidDestination.destination.mapId = kInvalidMapId;
    ExpectBlocked(Plan({invalidDestination}), Blocker::InvalidCandidateMap);
}

TEST(CampaignTravelPolicy, RejectsInvalidCoordinatesAndAcceptsTheFiniteEnvelopeBoundary)
{
    CandidateLeg boundary = Leg(80, LegKind::WalkingApproach, 0, 0);
    boundary.source.coordinates.x = -kMaximumAbsCoordinate;
    boundary.destination.coordinates.z = kMaximumAbsCoordinate;
    ExpectTransition(PlanNextTransition(Plan({boundary})), 80, LegKind::WalkingApproach);

    Request invalidRequest = Plan({boundary});
    invalidRequest.source.coordinates.x = std::numeric_limits<double>::infinity();
    ExpectBlocked(invalidRequest, Blocker::InvalidRequestSourceCoordinates);

    CandidateLeg nonFinite = boundary;
    nonFinite.source.coordinates.y = std::numeric_limits<double>::quiet_NaN();
    ExpectBlocked(Plan({nonFinite}), Blocker::InvalidCandidateCoordinates);

    CandidateLeg outOfRange = boundary;
    outOfRange.destination.coordinates.z = kMaximumAbsCoordinate + 0.001;
    ExpectBlocked(Plan({outOfRange}), Blocker::InvalidCandidateCoordinates);
}

TEST(CampaignTravelPolicy, RequiresTheCandidateToStartOnTheRequestSourceMap)
{
    CandidateLeg candidate = Leg(90, LegKind::Taxi, 1, 530);
    ExpectBlocked(Plan({candidate}, 100, 0), Blocker::CandidateSourceMapMismatch);
}

TEST(CampaignTravelPolicy, RequiresDirectedProofAndNeverInventsTheReverseEdge)
{
    CandidateLeg forward = Leg(100, LegKind::AreaTriggerPortal, 0, 1);
    ExpectTransition(PlanNextTransition(Plan({forward})), 100, LegKind::AreaTriggerPortal);

    CandidateLeg missing = forward;
    missing.directedEdge.validated = false;
    ExpectBlocked(Plan({missing}), Blocker::DirectedEdgeProofMissing);

    CandidateLeg reverse = forward;
    std::swap(reverse.source, reverse.destination);
    ExpectBlocked(Plan({reverse}, 100, 1), Blocker::DirectedEdgeProofMismatch);

    CandidateLeg sameMap = Leg(101, LegKind::GameObjectPortal, 0, 0);
    CandidateLeg sameMapReverse = sameMap;
    std::swap(sameMapReverse.source.coordinates, sameMapReverse.destination.coordinates);
    ExpectBlocked(Plan({sameMapReverse}), Blocker::DirectedEdgeProofMismatch);
}

TEST(CampaignTravelPolicy, ChoosesLeastCostThenFixedKindAndStableIdentity)
{
    CandidateLeg walking = Leg(12, LegKind::WalkingApproach, 0, 0);
    walking.cost = 10;
    CandidateLeg lowerIdentityWalking = Leg(11, LegKind::WalkingApproach, 0, 0);
    lowerIdentityWalking.cost = 10;
    CandidateLeg portal = Leg(10, LegKind::AreaTriggerPortal);
    portal.cost = 10;
    CandidateLeg taxi = Leg(9, LegKind::Taxi);
    taxi.cost = 9;

    ExpectTransition(PlanNextTransition(Plan({walking, lowerIdentityWalking, portal, taxi})),
                     9, LegKind::Taxi);

    taxi.cost = 10;
    ExpectTransition(PlanNextTransition(Plan({walking, lowerIdentityWalking, portal, taxi})),
                     11, LegKind::WalkingApproach);
}

TEST(CampaignTravelPolicy, SelectionIsStableAcrossEveryCandidatePermutation)
{
    std::vector<CandidateLeg> candidates = {
        Leg(4, LegKind::Taxi), Leg(3, LegKind::Transport),
        Leg(2, LegKind::GameObjectPortal), Leg(1, LegKind::AreaTriggerPortal)};
    for (CandidateLeg& candidate : candidates)
        candidate.cost = 25;

    std::sort(candidates.begin(), candidates.end(),
              [](CandidateLeg const& left, CandidateLeg const& right)
              {
                  return left.stableId < right.stableId;
              });

    std::size_t permutationCount = 0;
    do
    {
        ExpectTransition(PlanNextTransition(Plan(candidates)), 1, LegKind::AreaTriggerPortal);
        ++permutationCount;
    } while (std::next_permutation(candidates.begin(), candidates.end(),
                 [](CandidateLeg const& left, CandidateLeg const& right)
                 {
                     return left.stableId < right.stableId;
                 }));

    EXPECT_EQ(permutationCount, 24u);
}

TEST(CampaignTravelPolicy, BlockerSelectionIsPermutationStable)
{
    CandidateLeg teleport = Leg(110, LegKind::Teleport);
    CandidateLeg taxi = Leg(111, LegKind::Taxi);
    taxi.allTaxiNodesKnown = false;
    CandidateLeg invalid = Leg(112, LegKind::WalkingApproach, 0, 0);
    invalid.destination.mapValidated = false;

    std::vector<CandidateLeg> candidates = {teleport, taxi, invalid};
    do
    {
        ExpectBlocked(Plan(candidates), Blocker::InvalidCandidateMap);
    } while (std::next_permutation(candidates.begin(), candidates.end(),
                 [](CandidateLeg const& left, CandidateLeg const& right)
                 {
                     return left.stableId < right.stableId;
                 }));
}

TEST(CampaignTravelPolicy, EmptyCandidateSetReturnsATypedBlocker)
{
    ExpectBlocked(Plan({}), Blocker::NoCandidateLegs);
}

TEST(CampaignTravelPolicy, PlanningDoesNotModifyInputFacts)
{
    CandidateLeg taxi = Leg(120, LegKind::Taxi);
    taxi.cost = 50;
    Request request = Plan({taxi}, 50);
    Request const before = request;

    ExpectTransition(PlanNextTransition(request), 120, LegKind::Taxi);
    ASSERT_EQ(request.candidates.size(), before.candidates.size());
    EXPECT_EQ(request.botMoney, before.botMoney);
    EXPECT_EQ(request.source.mapId, before.source.mapId);
    EXPECT_EQ(request.candidates[0].stableId, before.candidates[0].stableId);
    EXPECT_EQ(request.candidates[0].cost, before.candidates[0].cost);
    EXPECT_EQ(request.candidates[0].allTaxiNodesKnown, before.candidates[0].allTaxiNodesKnown);
    EXPECT_EQ(request.candidates[0].directedEdge.validated,
              before.candidates[0].directedEdge.validated);
}

TEST(CampaignTravelPolicy, PolicySourcesContainNoRuntimeMovementLearningOrMutationWiring)
{
    std::string const source =
        ReadSource(ModuleRoot() / "src/AutoWow/CampaignTravelPolicy.h") +
        ReadSource(ModuleRoot() / "src/AutoWow/CampaignTravelPolicy.cpp");
    ASSERT_FALSE(source.empty());

    for (std::string_view const forbidden : {
             "PlayerbotAI", "Player.h", "TravelMgr", "TaxiPathGraph", "WorldDatabase",
             "CharacterDatabase", "TeleportTo(", "MoveRandom", "MovePoint(", "AddPassenger(",
             "SetTaximaskNode(", "LearnTaxi", "SetMoney(", "ModifyMoney(", "SaveToDB(",
             "urand(", "frand(", "rand("})
        EXPECT_EQ(source.find(forbidden), std::string::npos) << forbidden;
}
