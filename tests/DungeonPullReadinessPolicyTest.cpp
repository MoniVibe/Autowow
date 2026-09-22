/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonPullReadinessPolicy.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using DungeonPullReadiness::Decision;
using DungeonPullReadiness::Facts;
using DungeonPullReadiness::MemberFacts;
using DungeonPullReadiness::Requirements;
using DungeonPullReadiness::Result;

MemberFacts AvailableMember(bool tank, bool healer, bool dps, float distance)
{
    return {true, true, true, true, true, false, tank, healer, dps, distance};
}

Facts ReadyFivePlayerFacts()
{
    return {true, false, 20.0f,
        {AvailableMember(true, false, false, 0.0f), AvailableMember(false, true, false, 12.0f),
            AvailableMember(false, false, true, 18.0f), AvailableMember(false, false, true, 25.0f),
            AvailableMember(false, false, true, 50.0f)}};
}

void ExpectSameResult(Result const& left, Result const& right)
{
    EXPECT_EQ(left.decision, right.decision);
    EXPECT_EQ(left.availableTanks, right.availableTanks);
    EXPECT_EQ(left.readyTanks, right.readyTanks);
    EXPECT_EQ(left.availableHealers, right.availableHealers);
    EXPECT_EQ(left.readyHealers, right.readyHealers);
    EXPECT_EQ(left.availableDps, right.availableDps);
    EXPECT_EQ(left.readyDps, right.readyDps);
    EXPECT_EQ(left.requiredReadyDps, right.requiredReadyDps);
}

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}  // namespace

TEST(DungeonPullReadinessPolicy, FivePlayerGroupRequiresCeilSixtyPercentOfAvailableDps)
{
    Facts facts = ReadyFivePlayerFacts();
    Result result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.decision, Decision::Ready);
    EXPECT_EQ(result.availableDps, 3u);
    EXPECT_EQ(result.readyDps, 2u);
    EXPECT_EQ(result.requiredReadyDps, 2u);

    facts.members[3].distanceToInitiator = 50.0f;
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::DpsShareNotReady);
}

TEST(DungeonPullReadinessPolicy, TenPlayerGroupRequiresFourOfSixAvailableDps)
{
    Facts facts = {true, false, 25.0f,
        {AvailableMember(true, false, false, 0.0f), AvailableMember(true, false, false, 10.0f),
            AvailableMember(false, true, false, 15.0f), AvailableMember(false, true, false, 20.0f)}};
    for (int index = 0; index < 6; ++index)
        facts.members.push_back(AvailableMember(false, false, true, index < 4 ? 30.0f : 45.0f));

    Result const result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.decision, Decision::Ready);
    EXPECT_EQ(result.availableDps, 6u);
    EXPECT_EQ(result.readyDps, 4u);
    EXPECT_EQ(result.requiredReadyDps, 4u);
}

TEST(DungeonPullReadinessPolicy, HealerNinetyTwoPointEightYardsBehindFails)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.members[1].distanceToInitiator = 92.8f;
    Result const result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.decision, Decision::HealerNotReady);
    EXPECT_EQ(result.availableHealers, 1u);
    EXPECT_EQ(result.readyHealers, 0u);
}

TEST(DungeonPullReadinessPolicy, IntentionalTargetSeventyYardsAheadFails)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.targetDistance = 70.0f;
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::TargetOutsideSupportEnvelope);
}

TEST(DungeonPullReadinessPolicy, ExactDpsCeilBoundaryIsStable)
{
    Facts facts = {true, false, DungeonPullReadiness::DefaultSupportRadius,
        {AvailableMember(true, false, false, 0.0f), AvailableMember(false, true, false, 10.0f)}};
    for (int index = 0; index < 5; ++index)
        facts.members.push_back(AvailableMember(false, false, true,
            index < 3 ? DungeonPullReadiness::DefaultSupportRadius : 40.0f));

    Result const result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.requiredReadyDps, 3u);
    EXPECT_EQ(result.readyDps, 3u);
    EXPECT_EQ(result.decision, Decision::Ready);
}

TEST(DungeonPullReadinessPolicy, UnavailableOffInstanceAndTeleportingMembersLeaveDpsDenominator)
{
    Facts facts = {true, false, 15.0f,
        {AvailableMember(true, false, false, 0.0f), AvailableMember(false, true, false, 10.0f),
            AvailableMember(false, false, true, 20.0f)}};
    MemberFacts offline = AvailableMember(false, false, true, 100.0f);
    offline.online = false;
    MemberFacts dead = AvailableMember(false, false, true, 100.0f);
    dead.alive = false;
    MemberFacts outOfWorld = AvailableMember(false, false, true, 100.0f);
    outOfWorld.inWorld = false;
    MemberFacts offMap = AvailableMember(false, false, true, 100.0f);
    offMap.sameMap = false;
    MemberFacts offInstance = AvailableMember(false, false, true, 100.0f);
    offInstance.sameInstance = false;
    MemberFacts teleporting = AvailableMember(false, false, true, 100.0f);
    teleporting.teleporting = true;
    facts.members.insert(facts.members.end(), {offline, dead, outOfWorld, offMap, offInstance, teleporting});

    Result const result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.availableDps, 1u);
    EXPECT_EQ(result.requiredReadyDps, 1u);
    EXPECT_EQ(result.readyDps, 1u);
    EXPECT_EQ(result.decision, Decision::Ready);
}

TEST(DungeonPullReadinessPolicy, MissingTankAndHealerFailWithSpecificReasons)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.initiatorIsTank = false;
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::TankNotReady);

    facts = ReadyFivePlayerFacts();
    facts.members.erase(facts.members.begin() + 1);
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::HealerNotReady);
}

TEST(DungeonPullReadinessPolicy, InvalidAndNonfiniteRequirementsFailClosed)
{
    Facts const facts = ReadyFivePlayerFacts();
    for (Requirements const requirements : {
             Requirements{0.0f, 0.60f}, Requirements{-1.0f, 0.60f},
             Requirements{std::numeric_limits<float>::quiet_NaN(), 0.60f},
             Requirements{std::numeric_limits<float>::infinity(), 0.60f},
             Requirements{38.5f, 0.0f}, Requirements{38.5f, -0.1f}, Requirements{38.5f, 1.01f},
             Requirements{38.5f, std::numeric_limits<float>::quiet_NaN()},
             Requirements{38.5f, std::numeric_limits<float>::infinity()}})
    {
        EXPECT_EQ(DungeonPullReadiness::Evaluate(facts, requirements).decision, Decision::InvalidRequirements);
    }
}

TEST(DungeonPullReadinessPolicy, InvalidDistancesFailClosed)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.targetDistance = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::TargetOutsideSupportEnvelope);

    facts = ReadyFivePlayerFacts();
    facts.members[1].distanceToInitiator = std::numeric_limits<float>::infinity();
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::HealerNotReady);
}

TEST(DungeonPullReadinessPolicy, SelfDefenseOverridesReadinessAndInvalidConfiguration)
{
    Facts facts;
    facts.targetThreatensGroupOrPet = true;
    Requirements const invalid{std::numeric_limits<float>::quiet_NaN(), 0.0f};
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts, invalid).decision, Decision::SelfDefense);
}

TEST(DungeonPullReadinessPolicy, DeliberatePullFailsClosedWhenGroupIsAlreadyEngaged)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.existingCombatOrThreat = true;
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::AlreadyEngaged);

    facts.targetThreatensGroupOrPet = true;
    EXPECT_EQ(DungeonPullReadiness::Evaluate(facts).decision, Decision::SelfDefense);
}

TEST(DungeonPullReadinessPolicy, OverlappingRolesClassifyExclusivelyTankThenHealerThenDps)
{
    Facts facts = ReadyFivePlayerFacts();
    facts.members.push_back(AvailableMember(true, true, true, 5.0f));
    facts.members.push_back(AvailableMember(false, true, true, 5.0f));
    Result const result = DungeonPullReadiness::Evaluate(facts);
    EXPECT_EQ(result.availableTanks, 2u);
    EXPECT_EQ(result.availableHealers, 2u);
    EXPECT_EQ(result.availableDps, 3u);
}

TEST(DungeonPullReadinessPolicy, EvaluationIsPermutationStableAndIdempotent)
{
    Facts facts = ReadyFivePlayerFacts();
    Result const baseline = DungeonPullReadiness::Evaluate(facts);
    ExpectSameResult(baseline, DungeonPullReadiness::Evaluate(facts));

    std::reverse(facts.members.begin(), facts.members.end());
    ExpectSameResult(baseline, DungeonPullReadiness::Evaluate(facts));
    std::rotate(facts.members.begin(), facts.members.begin() + 2, facts.members.end());
    ExpectSameResult(baseline, DungeonPullReadiness::Evaluate(facts));
}

TEST(DungeonPullReadinessPolicy, AttackGuardPrecedesEveryTargetEngineAndMovementMutation)
{
    std::string const source = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/AttackAction.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const targetValidity = source.find("if (!bot->IsValidAttackTarget(target))");
    std::size_t const guard = source.find("if (!DungeonPullReadiness::IsReady");
    ASSERT_NE(targetValidity, std::string::npos);
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(targetValidity, guard);

    for (std::string_view const mutation : {"bot->SetSelection", "\"current target\")->Set(target)",
             "\"available loot\")->Get()->Add", "GetMotionMaster()->Clear", "bot->StopMoving()",
             "ServerFacade::instance().SetFacingTo", "botAI->ChangeEngine", "bot->Attack(target"})
    {
        std::size_t const mutationPosition = source.find(mutation);
        ASSERT_NE(mutationPosition, std::string::npos) << mutation;
        EXPECT_LT(guard, mutationPosition) << mutation;
    }
}

TEST(DungeonPullReadinessPolicy, PullRequestGuardPrecedesEveryPullStateMutation)
{
    std::string const source = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/PullActions.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const requestStart = source.find("bool PullRequestAction::Execute(Event event)");
    std::size_t const pullStart = source.find("bool PullAction::Execute(Event event)");
    ASSERT_NE(requestStart, std::string::npos);
    ASSERT_NE(pullStart, std::string::npos);
    ASSERT_LT(requestStart, pullStart);

    std::string const requestSource = source.substr(requestStart, pullStart - requestStart);
    std::size_t const guard = requestSource.find("DungeonPullReadiness::IsReady");
    ASSERT_NE(guard, std::string::npos);

    for (std::string_view const mutation : {"posMap[\"pull\"] = pullPosition", "strategy->RequestPull(target)",
             "\"current target\")->Set(target)", "botAI->ChangeEngine(BOT_STATE_COMBAT)",
             "botAI->SetNextCheckDelay"})
    {
        std::size_t const mutationPosition = requestSource.find(mutation);
        ASSERT_NE(mutationPosition, std::string::npos) << mutation;
        EXPECT_LT(guard, mutationPosition) << mutation;
    }
}

TEST(DungeonPullReadinessPolicy, PullActionGuardPrecedesRangeMovementAndSpellInitiation)
{
    std::string const source = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/PullActions.cpp");
    ASSERT_FALSE(source.empty());

    std::size_t const pullStart = source.find("bool PullAction::Execute(Event event)");
    std::size_t const possibleStart = source.find("bool PullAction::isPossible()");
    ASSERT_NE(pullStart, std::string::npos);
    ASSERT_NE(possibleStart, std::string::npos);
    ASSERT_LT(pullStart, possibleStart);

    std::string const pullSource = source.substr(pullStart, possibleStart - pullStart);
    std::size_t const guard = pullSource.find("DungeonPullReadiness::IsReady");
    ASSERT_NE(guard, std::string::npos);

    for (std::string_view const mutation : {"strategy->RequestPull(target, false)", "bot->StopMoving()",
             "\"current target\")->Set(target)",
             "botAI->DoSpecificAction(strategy->GetPullActionName()"})
    {
        std::size_t searchFrom = 0;
        bool found = false;
        while (true)
        {
            std::size_t const mutationPosition = pullSource.find(mutation, searchFrom);
            if (mutationPosition == std::string::npos)
                break;
            found = true;
            EXPECT_LT(guard, mutationPosition) << mutation;
            searchFrom = mutationPosition + mutation.size();
        }
        EXPECT_TRUE(found) << mutation;
    }
}

TEST(DungeonPullReadinessPolicy, BothPullPathsUseDeliberatePullMode)
{
    std::string const source = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/PullActions.cpp");
    ASSERT_FALSE(source.empty());

    std::string const call = "DungeonPullReadiness::IsReady(botAI, bot, target, getName(), true)";
    std::size_t const first = source.find(call);
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(source.find(call, first + call.size()), std::string::npos);
}
