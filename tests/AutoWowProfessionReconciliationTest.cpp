#include "../src/AutoWow/AutoWowProfessionReconciliation.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace
{
using AutoWowProfessionReconciliation::ObservedPrimarySkills;
using AutoWowProfessionReconciliation::ProfessionPair;
using AutoWowProfessionReconciliation::ReconcilePrimarySkills;
using AutoWowProfessionReconciliation::ShouldHonorStoredPair;

constexpr std::uint16_t kHerbalism = 182;
constexpr std::uint16_t kTailoring = 197;
constexpr std::uint16_t kEnchanting = 333;

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

std::string_view FunctionBody(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};

    std::size_t const finish = source.find(end, begin + start.size());
    if (finish == std::string::npos)
        return std::string_view(source).substr(begin);

    return std::string_view(source).substr(begin, finish - begin);
}
}

TEST(AutoWowProfessionReconciliation, StalePairBecomesExactlyRequestedPair)
{
    ObservedPrimarySkills observed;
    observed.skills[0] = kTailoring;
    observed.skills[1] = kEnchanting;
    observed.count = 2;

    EXPECT_TRUE(ReconcilePrimarySkills(observed, {kHerbalism, kTailoring}));
    ASSERT_EQ(observed.count, 2U);
    EXPECT_EQ(observed.skills[0], kHerbalism);
    EXPECT_EQ(observed.skills[1], kTailoring);
}

TEST(AutoWowProfessionReconciliation, RepeatedInvocationDoesNotChurnExactPair)
{
    ObservedPrimarySkills observed;
    observed.skills[0] = kHerbalism;
    observed.skills[1] = kTailoring;
    observed.count = 2;
    ObservedPrimarySkills const before = observed;

    EXPECT_FALSE(ReconcilePrimarySkills(observed, {kHerbalism, kTailoring}));
    EXPECT_EQ(observed.count, before.count);
    EXPECT_EQ(observed.skills, before.skills);
}

TEST(AutoWowProfessionReconciliation, InvalidPairFailsClosedWithoutChangingObservedSkills)
{
    ObservedPrimarySkills observed;
    observed.skills[0] = kEnchanting;
    observed.count = 1;
    ObservedPrimarySkills const before = observed;

    EXPECT_FALSE(ReconcilePrimarySkills(observed, {0, kTailoring}));
    EXPECT_EQ(observed.count, before.count);
    EXPECT_EQ(observed.skills, before.skills);
}

TEST(AutoWowProfessionReconciliation, ManagedExplicitPairSurvivesNativeCatalogPolicy)
{
    // Herbalism + Tailoring is valid for the Oracle campaign but is not a weighted random pair.
    EXPECT_TRUE(ShouldHonorStoredPair(true, true, false));

    // Ordinary/random bots still require native catalog membership.
    EXPECT_FALSE(ShouldHonorStoredPair(false, true, false));
    EXPECT_TRUE(ShouldHonorStoredPair(false, true, true));
    EXPECT_FALSE(ShouldHonorStoredPair(true, false, false));
}

TEST(AutoWowProfessionReconciliation, ProductionHookIsManagedAndPrimaryOnly)
{
    std::string const factory = ReadSource(ModuleRoot() / "src/Bot/Factory/PlayerbotFactory.cpp");
    ASSERT_FALSE(factory.empty());
    std::string_view const body = FunctionBody(
        factory, "bool PlayerbotFactory::ReconcilePrimaryTradeSkills", "void PlayerbotFactory::InitTradeSpecializations");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("AutoWowOracleRuntime::IsManagedBot"), std::string_view::npos);
    EXPECT_NE(body.find("AutoWowProfessionReconciliation::ReconcilePrimarySkills"), std::string_view::npos);
    EXPECT_NE(body.find("IsPrimaryTradeSkill"), std::string_view::npos);
    EXPECT_NE(body.find("bot->SetSkill(skill, 0, 0, 0)"), std::string_view::npos);
    for (std::string_view const forbidden : {"ClearSkills()", "ClearEverything()", "ClearInventory()",
                                              "ClearAllItems()", "ResetQuests()", "resetTalents"})
        EXPECT_EQ(body.find(forbidden), std::string_view::npos) << forbidden;

    std::string_view const initTradeSkills = FunctionBody(
        factory, "void PlayerbotFactory::InitTradeSkills()", "bool PlayerbotFactory::ReconcilePrimaryTradeSkills");
    ASSERT_FALSE(initTradeSkills.empty());
    EXPECT_NE(initTradeSkills.find("AutoWowProfessionReconciliation::ShouldHonorStoredPair"),
              std::string_view::npos);
    EXPECT_NE(initTradeSkills.find("maxPrimaryTradeSkills >= 2 && !hasStoredProfessionPair"),
              std::string_view::npos);

    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    std::size_t const pairApply = bridge.find("factory.ReconcilePrimaryTradeSkills(firstSkill, secondSkill)");
    std::size_t const pairInit = bridge.find("factory.InitSkills()", pairApply);
    ASSERT_NE(pairApply, std::string::npos);
    EXPECT_NE(pairInit, std::string::npos);
    EXPECT_LT(pairApply, pairInit);
    std::size_t const pairSave = bridge.find("bot->SaveToDB(false, false)", pairInit);
    EXPECT_NE(pairSave, std::string::npos);
    EXPECT_LT(pairInit, pairSave);
    EXPECT_NE(bridge.find("default: return {0, 0};"), std::string::npos);
}
