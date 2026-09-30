/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/Ai/Base/ReleasedCorpseApproachPolicy.h"
#include "../src/Ai/Base/Actions/ReleasedCorpseApproachPolicy.h"

#include "gtest/gtest.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::string ReadGroupedReleasedIngressBranch()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/Ai/Base/Actions/ReleaseSpiritAction.cpp");
    std::string const source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    std::size_t const functionStart = source.find("bool RepopAction::Execute");
    std::size_t const branchStart = source.find(
        "if (IsReleasedGroupedInstanceCorpse(bot))", functionStart);
    std::size_t const ordinaryDeathStart = source.find(
        "GraveyardStruct const* graveyard = GetGrave(", branchStart);
    std::size_t const helperStart = source.find("constexpr uint32 IngressRejectedPathTypes");
    std::size_t const helperEnd = source.find(
        "ReleasedCorpseApproachPolicy::IngressPortalSelection FindIngressPortal", helperStart);
    if (functionStart == std::string::npos || branchStart == std::string::npos ||
        ordinaryDeathStart == std::string::npos || helperStart == std::string::npos ||
        helperEnd == std::string::npos)
    {
        return {};
    }
    return source.substr(helperStart, helperEnd - helperStart) +
        source.substr(branchStart, ordinaryDeathStart - branchStart);
}
}

TEST(ReleasedCorpseApproachPolicy, OrdersExactPointThenFourFixedEightPointRings)
{
    using namespace ReleasedCorpseApproachPolicy;
    auto const candidates = BuildCandidates(100.0f, 200.0f);

    static_assert(MaxProbeCount == 33);
    ASSERT_EQ(candidates.size(), 33u);
    EXPECT_EQ(candidates[0].probeIndex, 0u);
    EXPECT_FLOAT_EQ(candidates[0].radius, 0.0f);
    EXPECT_FLOAT_EQ(candidates[0].x, 100.0f);
    EXPECT_FLOAT_EQ(candidates[0].y, 200.0f);

    EXPECT_EQ(candidates[1].probeIndex, 1u);
    EXPECT_FLOAT_EQ(candidates[1].radius, 2.0f);
    EXPECT_EQ(candidates[1].angleIndex, 0u);
    EXPECT_NEAR(candidates[1].x, 102.0f, 0.0001f);
    EXPECT_NEAR(candidates[1].y, 200.0f, 0.0001f);

    EXPECT_FLOAT_EQ(candidates[9].radius, 4.0f);
    EXPECT_FLOAT_EQ(candidates[17].radius, 8.0f);
    EXPECT_FLOAT_EQ(candidates[25].radius, 12.0f);
    EXPECT_EQ(candidates[32].probeIndex, 32u);
    EXPECT_EQ(candidates[32].angleIndex, 7u);
}

TEST(ReleasedCorpseApproachPolicy, NeverEvaluatesMoreThanThirtyThreeCandidates)
{
    using namespace ReleasedCorpseApproachPolicy;
    auto const candidates = BuildCandidates(0.0f, 0.0f);
    std::size_t evaluations = 0;

    Selection const selection = Select(candidates, 10.0f, 0,
        [&evaluations](Candidate const&)
        {
            ++evaluations;
            return Evaluation{};
        });

    EXPECT_FALSE(selection.found);
    EXPECT_EQ(selection.probesEvaluated, MaxProbeCount);
    EXPECT_EQ(evaluations, MaxProbeCount);
}

TEST(ReleasedCorpseApproachPolicy, RejectsMateriallyDifferentVerticalLevel)
{
    using namespace ReleasedCorpseApproachPolicy;
    auto const candidates = BuildCandidates(0.0f, 0.0f);
    std::vector<std::size_t> visited;

    Selection const selection = Select(candidates, 50.0f, 0,
        [&visited](Candidate const& candidate)
        {
            visited.push_back(candidate.probeIndex);
            Evaluation evaluation{true, true, true, 55.01f};
            if (candidate.probeIndex == 1)
                evaluation.endpointZ = 55.0f;
            return evaluation;
        });

    ASSERT_TRUE(selection.found);
    ASSERT_EQ(visited.size(), 2u);
    EXPECT_EQ(selection.candidate.probeIndex, 1u);
    EXPECT_FLOAT_EQ(selection.endpointZ, 55.0f);
}

TEST(ReleasedCorpseApproachPolicy, ReturnsNoCandidateWhenEveryProbeFails)
{
    using namespace ReleasedCorpseApproachPolicy;
    auto const candidates = BuildCandidates(-10.0f, 25.0f);

    Selection const selection = Select(candidates, 7.0f, FirstFallbackProbe,
        [](Candidate const& candidate)
        {
            Evaluation evaluation;
            evaluation.floorValid = true;
            evaluation.navmeshReachable = candidate.probeIndex % 2 == 0;
            evaluation.corpseLineOfSight = candidate.probeIndex % 2 != 0;
            evaluation.endpointZ = 7.0f;
            return evaluation;
        });

    EXPECT_FALSE(selection.found);
    EXPECT_EQ(selection.probesEvaluated, MaxProbeCount - FirstFallbackProbe);
}

TEST(ReleasedCorpseApproachPolicy, SelectsNearestExactMapIngressDeterministically)
{
    using namespace ReleasedCorpseApproachPolicy;
    std::array<IngressPortalCandidate, 6> const candidates = {{
        {90, 571, 600, 400.0f, true},
        {70, 571, 600, 100.0f, true},
        {60, 571, 600, 100.0f, true},
        {10, 1, 600, 1.0f, true},
        {20, 571, 576, 1.0f, true},
        {30, 571, 600, 0.0f, false},
    }};

    IngressPortalSelection const selected = SelectIngressPortal(candidates, 571, 600);
    ASSERT_TRUE(selected.found);
    EXPECT_EQ(selected.candidate.triggerId, 60u);
    EXPECT_FLOAT_EQ(selected.candidate.distanceSquared, 100.0f);
}

TEST(ReleasedCorpseApproachPolicy, RejectsMissingOrWrongMapIngress)
{
    using namespace ReleasedCorpseApproachPolicy;
    std::array<IngressPortalCandidate, 4> const candidates = {{
        {0, 571, 600, 1.0f, true},
        {10, 571, 600, -1.0f, true},
        {20, 571, 576, 1.0f, true},
        {30, 1, 600, 1.0f, true},
    }};

    IngressPortalSelection const selected = SelectIngressPortal(candidates, 571, 600);
    EXPECT_FALSE(selected.found);
}

TEST(ReleasedCorpseIngressApproachPolicy, SphereCandidatesAreBoundedAndDeterministic)
{
    using namespace ReleasedCorpseApproachPolicy;
    IngressTriggerVolume const sphere{100.0f, -50.0f, 20.0f, 8.0f,
                                      0.0f, 0.0f, 0.0f, 1.25f};
    auto const first = BuildIngressApproachCandidates(sphere);
    auto const second = BuildIngressApproachCandidates(sphere);

    static_assert(MaxIngressApproachCandidates == 33);
    ASSERT_EQ(first.size(), MaxIngressApproachCandidates);
    ASSERT_EQ(second.size(), first.size());
    EXPECT_EQ(first.front().candidateIndex, 0u);
    EXPECT_FLOAT_EQ(first.front().x, sphere.x);
    EXPECT_FLOAT_EQ(first.front().y, sphere.y);
    EXPECT_FLOAT_EQ(first.front().z, sphere.z);

    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index].candidateIndex, index);
        EXPECT_TRUE(IsInsideIngressTrigger(sphere, first[index].x, first[index].y,
                                           first[index].z, IngressTriggerSafetyMargin));
        EXPECT_EQ(second[index].candidateIndex, first[index].candidateIndex);
        EXPECT_FLOAT_EQ(second[index].x, first[index].x);
        EXPECT_FLOAT_EQ(second[index].y, first[index].y);
        EXPECT_FLOAT_EQ(second[index].z, first[index].z);
    }
}

TEST(ReleasedCorpseIngressApproachPolicy, OrientedBoxGridIsBoundedAndDeterministic)
{
    using namespace ReleasedCorpseApproachPolicy;
    constexpr float orientation = 1.0471975512f;
    IngressTriggerVolume const box{25.0f, 40.0f, 12.0f, 0.0f,
                                   12.0f, 6.0f, 4.0f, orientation};
    auto const first = BuildIngressApproachCandidates(box);
    auto const second = BuildIngressApproachCandidates(box);

    ASSERT_EQ(first.size(), 25u);
    ASSERT_EQ(second.size(), first.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index].candidateIndex, index);
        EXPECT_TRUE(IsInsideIngressTrigger(box, first[index].x, first[index].y,
                                           first[index].z, IngressTriggerSafetyMargin));
        EXPECT_EQ(second[index].candidateIndex, first[index].candidateIndex);
        EXPECT_FLOAT_EQ(second[index].x, first[index].x);
        EXPECT_FLOAT_EQ(second[index].y, first[index].y);
        EXPECT_FLOAT_EQ(second[index].z, first[index].z);
    }

    // A world-axis point outside the unrotated width can still be inside after local rotation.
    float const localX = 5.0f;
    float const rotatedX = box.x + localX * std::cos(orientation);
    float const rotatedY = box.y + localX * std::sin(orientation);
    EXPECT_TRUE(IsInsideIngressTrigger(box, rotatedX, rotatedY, box.z));
}

TEST(ReleasedCorpseIngressApproachPolicy, CenterUnreachableUsesShortestReachableFallback)
{
    using namespace ReleasedCorpseApproachPolicy;
    IngressTriggerVolume const sphere{0.0f, 0.0f, 5.0f, 10.0f};
    auto const candidates = BuildIngressApproachCandidates(sphere);

    IngressApproachSelection const selection = SelectIngressApproach(candidates,
        [](IngressApproachCandidate const& candidate)
        {
            IngressApproachEvaluation evaluation;
            evaluation.floorValid = true;
            evaluation.pathReachable = candidate.candidateIndex != 0;
            evaluation.endpointInside = true;
            evaluation.endpointX = candidate.x;
            evaluation.endpointY = candidate.y;
            evaluation.endpointZ = candidate.z;
            evaluation.pathDistance = candidate.candidateIndex == 2 ? 5.0f :
                10.0f + static_cast<float>(candidate.candidateIndex);
            return evaluation;
        });

    ASSERT_TRUE(selection.found);
    EXPECT_EQ(selection.candidate.candidateIndex, 2u);
    EXPECT_FLOAT_EQ(selection.pathDistance, 5.0f);
    EXPECT_EQ(selection.probesEvaluated, candidates.size());
}

TEST(ReleasedCorpseIngressApproachPolicy, EqualPathDistancesUseStableCandidateIndex)
{
    using namespace ReleasedCorpseApproachPolicy;
    IngressTriggerVolume const sphere{0.0f, 0.0f, 5.0f, 10.0f};
    auto const candidates = BuildIngressApproachCandidates(sphere);

    IngressApproachSelection const selection = SelectIngressApproach(candidates,
        [](IngressApproachCandidate const& candidate)
        {
            IngressApproachEvaluation evaluation;
            evaluation.floorValid = true;
            evaluation.pathReachable = candidate.candidateIndex == 1 ||
                candidate.candidateIndex == 2;
            evaluation.endpointInside = true;
            evaluation.endpointX = candidate.x;
            evaluation.endpointY = candidate.y;
            evaluation.endpointZ = candidate.z;
            evaluation.pathDistance = 5.0f;
            return evaluation;
        });

    ASSERT_TRUE(selection.found);
    EXPECT_EQ(selection.candidate.candidateIndex, 1u);
}

TEST(ReleasedCorpseIngressApproachPolicy, ReturnsNoEndpointWhenNoCandidateIsViable)
{
    using namespace ReleasedCorpseApproachPolicy;
    IngressTriggerVolume const box{0.0f, 0.0f, 0.0f, 0.0f,
                                   8.0f, 4.0f, 6.0f, 0.25f};
    auto const candidates = BuildIngressApproachCandidates(box);

    IngressApproachSelection const selection = SelectIngressApproach(candidates,
        [](IngressApproachCandidate const& candidate)
        {
            IngressApproachEvaluation evaluation;
            evaluation.floorValid = candidate.candidateIndex % 2 == 0;
            evaluation.pathReachable = candidate.candidateIndex % 2 != 0;
            evaluation.endpointInside = true;
            evaluation.pathDistance = 1.0f;
            return evaluation;
        });

    EXPECT_FALSE(selection.found);
    EXPECT_EQ(selection.candidateCount, candidates.size());
    EXPECT_EQ(selection.probesEvaluated, candidates.size());
}

TEST(ReleasedCorpseIngressApproachPolicy, DungeonEntranceBoxProbeIsBoundedAndDeterministic)
{
    using namespace ReleasedCorpseApproachPolicy;
    IngressTriggerVolume const box{4775.32f, -2017.16f, 235.0f, 0.0f,
                                   12.0f, 13.0f, 12.0f, 0.0f};

    auto const first = BuildIngressApproachCandidates(box);
    auto const second = BuildIngressApproachCandidates(box);

    ASSERT_EQ(first.size(), 25u);
    ASSERT_EQ(second.size(), first.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index].candidateIndex, second[index].candidateIndex);
        EXPECT_FLOAT_EQ(first[index].x, second[index].x);
        EXPECT_FLOAT_EQ(first[index].y, second[index].y);
        EXPECT_FLOAT_EQ(first[index].z, second[index].z);
        EXPECT_TRUE(IsInsideIngressTrigger(box, first[index].x, first[index].y,
                                           first[index].z));
    }
}

TEST(ReleasedCorpseIngressApproachPolicy,
     DtkExteriorGraveyardSelectsSafeInteriorEndpointForNormalReentry)
{
    using namespace ReleasedCorpseApproachPolicy;
    constexpr std::uint32_t exteriorMap = 571;
    constexpr std::uint32_t corpseMap = 600;
    constexpr std::uint32_t dtkEntranceTrigger = 4998;

    std::array<IngressPortalCandidate, 1> const portals = {{
        {dtkEntranceTrigger, exteriorMap, corpseMap, 100.0f, true},
    }};
    IngressPortalSelection const portal = SelectIngressPortal(portals, exteriorMap, corpseMap);
    ASSERT_TRUE(portal.found);
    EXPECT_EQ(portal.candidate.triggerId, dtkEntranceTrigger);

    IngressTriggerVolume const entrance{4775.32f, -2017.16f, 235.0f, 0.0f,
                                        12.0f, 13.0f, 12.0f, 0.0f};
    auto const candidates = BuildIngressApproachCandidates(entrance);
    ASSERT_EQ(candidates.size(), 25u);

    IngressApproachSelection const selection = SelectIngressApproach(candidates,
        [&entrance](IngressApproachCandidate const& candidate)
        {
            IngressApproachEvaluation evaluation;
            evaluation.floorValid = true;
            evaluation.pathReachable = true;
            evaluation.endpointInside = candidate.candidateIndex == 24;
            // The navmesh may stop at a different point in the same entrance volume than the
            // requested probe. That point is the safe movement endpoint used by the action.
            evaluation.endpointX = evaluation.endpointInside ? entrance.x + 0.5f : entrance.x + 7.0f;
            evaluation.endpointY = entrance.y - 0.5f;
            evaluation.endpointZ = entrance.z;
            evaluation.pathDistance = 80.0f;
            return evaluation;
        });

    ASSERT_TRUE(selection.found);
    EXPECT_EQ(selection.candidate.candidateIndex, 24u);
    EXPECT_EQ(selection.probesEvaluated, 25u);
    EXPECT_TRUE(IsInsideIngressTrigger(entrance, selection.endpointX,
                                       selection.endpointY, selection.endpointZ));
    EXPECT_NE(selection.endpointX, selection.candidate.x);
    EXPECT_NE(selection.endpointY, selection.candidate.y);
}

TEST(ReleasedCorpseIngressApproachSourceContract,
     GroupedIngressIsWalkOnlyStrictAndKeepsFiniteFailureBookkeeping)
{
    std::string const branch = ReadGroupedReleasedIngressBranch();
    ASSERT_FALSE(branch.empty());

    for (std::string_view const forbidden : {
             "TeleportTo(", "NearTeleportTo(", "PerformGraveyardTeleport(",
             "ResurrectPlayer(", "SpawnCorpseBones(", "HandleMoveWorldportAck(",
             "MoveRandom", "urand(", "frand("})
    {
        EXPECT_EQ(branch.find(forbidden), std::string::npos) << forbidden;
    }

    EXPECT_NE(branch.find("GetHeight("), std::string::npos);
    EXPECT_NE(branch.find("PathGenerator"), std::string::npos);
    EXPECT_EQ(branch.find("SetSlopeCheck("), std::string::npos);
    EXPECT_NE(branch.find("PATHFIND_INCOMPLETE"), std::string::npos);
    EXPECT_NE(branch.find("PATHFIND_FARFROMPOLY"), std::string::npos);
    EXPECT_NE(branch.find("IsInsideIngressTrigger"), std::string::npos);
    EXPECT_NE(branch.find("IsUsableIngressPath"), std::string::npos);
    EXPECT_EQ(branch.find("IngressEndpointTolerance"), std::string::npos);
    EXPECT_NE(branch.find("MoveTo(bot->GetMapId()"), std::string::npos);
    EXPECT_NE(branch.find("last area trigger"), std::string::npos);
    EXPECT_NE(branch.find("DoSpecificAction(\"area trigger\""), std::string::npos);
    EXPECT_NE(branch.find("RecordEntranceAttempt"), std::string::npos);
    EXPECT_NE(branch.find("phase=exhausted"), std::string::npos);
    EXPECT_NE(branch.find("reason=no_reachable_inside_candidate"), std::string::npos);
    EXPECT_NE(branch.find("candidate_count={}"), std::string::npos);
    EXPECT_NE(branch.find("probe_count={}"), std::string::npos);
}
