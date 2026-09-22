#include "gtest/gtest.h"

#include "../src/AutoWow/ProbeResetPolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}

TEST(ProbeResetPolicyTest, RefusesActiveCombatButAllowsOnlyFlagOnlyStaleCombat)
{
    using namespace AutoWowProbeReset;
    EXPECT_EQ(ClassifyCombat({true, false, false, false, false}), CombatPolicy::ClearStaleFlag);
    EXPECT_EQ(ClassifyCombat({false, false, false, false, false}), CombatPolicy::Ready);
    EXPECT_EQ(ClassifyCombat({true, true, false, false, false}), CombatPolicy::RefuseActive);
    EXPECT_EQ(ClassifyCombat({true, false, true, false, false}), CombatPolicy::RefuseActive);
    EXPECT_EQ(ClassifyCombat({true, false, false, true, false}), CombatPolicy::RefuseActive);
    EXPECT_EQ(ClassifyCombat({true, false, false, false, true}), CombatPolicy::RefuseActive);
}

TEST(ProbeResetPolicyTest, RefusesForeignAndSpecialGroups)
{
    using namespace AutoWowProbeReset;
    EXPECT_TRUE(MayDisbandGroup({false, false, false, false}));
    EXPECT_FALSE(MayDisbandGroup({false, false, false, true}));
    EXPECT_FALSE(MayDisbandGroup({true, false, false, false}));
    EXPECT_FALSE(MayDisbandGroup({false, true, false, false}));
    EXPECT_FALSE(MayDisbandGroup({false, false, true, false}));
}

TEST(ProbeResetPolicyTest, ClearsOnlyTemporaryTargetBindAndRefusesPermanent)
{
    using namespace AutoWowProbeReset;
    EXPECT_EQ(ClassifyTargetBind(false, false), BindPolicy::Ready);
    EXPECT_EQ(ClassifyTargetBind(true, false), BindPolicy::ClearTemporary);
    EXPECT_EQ(ClassifyTargetBind(true, true), BindPolicy::RefusePermanent);
}

TEST(ProbeResetPolicyTest, ResolvesOnlyExactAllowlistedExteriorRoutes)
{
    using namespace AutoWowProbeReset;
    ExteriorRoute const* uk = ResolveExteriorRoute("uk-exterior");
    ASSERT_NE(uk, nullptr);
    EXPECT_EQ(uk->map, 571u);
    EXPECT_FLOAT_EQ(uk->x, 1257.11f);
    EXPECT_FLOAT_EQ(uk->y, -4854.48f);
    EXPECT_FLOAT_EQ(uk->z, 37.248f);
    EXPECT_FLOAT_EQ(uk->orientation, 0.274f);
    EXPECT_EQ(ResolveExteriorRoute("UK-exterior"), nullptr);
    EXPECT_EQ(ResolveExteriorRoute("uk-entry"), nullptr);
    EXPECT_EQ(ResolveExteriorRoute("uk-first-pack"), nullptr);
    EXPECT_EQ(ResolveExteriorRoute("interior"), nullptr);
}

TEST(ProbeResetPolicyTest, LiveControlUsesNormalCoreMutationApisWithoutSql)
{
    std::string const source = ReadSource(ModuleRoot() / "src/AutoWow/ProbeResetControl.cpp");
    EXPECT_NE(source.find("player->ClearInCombat()"), std::string::npos);
    EXPECT_NE(source.find("player->ResurrectPlayer(1.0f, false)"), std::string::npos);
    EXPECT_NE(source.find("player->SpawnCorpseBones()"), std::string::npos);
    EXPECT_NE(source.find("active_combat_at_staging"), std::string::npos);
    EXPECT_NE(source.find("group->Disband()"), std::string::npos);
    EXPECT_NE(source.find("PlayerUnbindInstance"), std::string::npos);
    EXPECT_NE(source.find("player->TeleportTo"), std::string::npos);
    EXPECT_EQ(source.find("CharacterDatabase"), std::string::npos);
    EXPECT_EQ(source.find("DELETE FROM"), std::string::npos);
    EXPECT_EQ(source.find("UPDATE "), std::string::npos);
}
