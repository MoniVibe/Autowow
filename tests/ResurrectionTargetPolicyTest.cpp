/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/Ai/Base/ResurrectionTargetPolicy.h"

#include "gtest/gtest.h"

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

TEST(ResurrectionTargetPolicy, PreservesMasterHealerTankOtherPriority)
{
    using namespace ResurrectionTargetPolicy;
    EXPECT_EQ(ClassifyRole(true, true, true), RolePriority::Master);
    EXPECT_EQ(ClassifyRole(false, true, true), RolePriority::Healer);
    EXPECT_EQ(ClassifyRole(false, false, true), RolePriority::Tank);
    EXPECT_EQ(ClassifyRole(false, false, false), RolePriority::Other);
    EXPECT_TRUE(HasHigherPriority(RolePriority::Master, RolePriority::Healer));
    EXPECT_TRUE(HasHigherPriority(RolePriority::Healer, RolePriority::Tank));
    EXPECT_TRUE(HasHigherPriority(RolePriority::Tank, RolePriority::Other));
}

TEST(ResurrectionTargetPolicy, ReleasedGhostRequiresValidatedCorpse)
{
    using namespace ResurrectionTargetPolicy;
    EXPECT_EQ(ClassifyTarget(true, true, false, true, false, CorpseValidation::Valid),
              TargetKind::ReleasedCorpse);
    EXPECT_EQ(ClassifyTarget(true, true, false, true, false, CorpseValidation::Missing),
              TargetKind::Invalid);
    EXPECT_EQ(ClassifyTarget(true, true, false, true, false, CorpseValidation::Bones),
              TargetKind::Invalid);
    EXPECT_EQ(ClassifyTarget(true, true, false, true, false, CorpseValidation::WrongOwner),
              TargetKind::Invalid);
    EXPECT_EQ(ClassifyTarget(true, true, false, true, false, CorpseValidation::WrongMap),
              TargetKind::Invalid);
}

TEST(ResurrectionTargetPolicy, UnreleasedBodyUsesUnitAndPendingRequestFailsClosed)
{
    using namespace ResurrectionTargetPolicy;
    EXPECT_EQ(ClassifyTarget(true, true, false, false, true, CorpseValidation::NotRequired),
              TargetKind::UnreleasedUnit);
    EXPECT_EQ(ClassifyTarget(true, true, true, false, true, CorpseValidation::NotRequired),
              TargetKind::Invalid);
    EXPECT_EQ(ClassifyTarget(true, true, false, false, false, CorpseValidation::NotRequired),
              TargetKind::Invalid);
}

TEST(ResurrectionTargetPolicy, GeometryAndCombatGatesFailClosed)
{
    using namespace ResurrectionTargetPolicy;
    EXPECT_TRUE(IsGeometryReady(true, true));
    EXPECT_TRUE(NeedsMovement(false, true));
    EXPECT_TRUE(NeedsMovement(true, false));
    EXPECT_FALSE(CanCastInCombat(true, false));
    EXPECT_TRUE(CanCastInCombat(true, true));
    EXPECT_TRUE(CanCastInCombat(false, false));
    EXPECT_TRUE(CanCastInCombat(false, true));
    EXPECT_FLOAT_EQ(SearchRadius(28.5f, false), 57.0f);
    EXPECT_FLOAT_EQ(SearchRadius(28.5f, true), 142.5f);
    EXPECT_FLOAT_EQ(SearchRadius(0.0f, true), 0.0f);
}

TEST(ResurrectionTargetPolicy, SourceUsesCoreCorpseContractWithoutDirectResurrection)
{
    std::filesystem::path const root = ModuleRoot();
    std::string const selection = ReadSource(root / "src/Ai/Base/Value/PartyMemberToResurrect.cpp");
    std::string const reach = ReadSource(root / "src/Ai/Base/Actions/ReachTargetActions.cpp");
    std::string const cast = ReadSource(root / "src/Ai/Base/Actions/GenericSpellActions.cpp");
    std::string const druid = ReadSource(root / "src/Ai/Class/Druid/Action/DruidActions.cpp");
    std::string const druidNonCombat =
        ReadSource(root / "src/Ai/Class/Druid/Strategy/GenericDruidNonCombatStrategy.cpp");
    std::string const druidCombat =
        ReadSource(root / "src/Ai/Class/Druid/Strategy/GenericDruidStrategy.cpp");
    std::string const healthTriggers = ReadSource(root / "src/Ai/Base/Trigger/HealthTriggers.cpp");
    std::string const accept = ReadSource(root / "src/Ai/Base/Actions/AcceptResurrectAction.cpp");
    std::string const playerScript = ReadSource(root / "src/Script/Playerbots.cpp");
    std::string const ownedSource =
        selection + reach + cast + druid + druidNonCombat + druidCombat + healthTriggers + accept + playerScript;

    EXPECT_NE(selection.find("gref->GetSource()"), std::string::npos);
    EXPECT_NE(selection.find("healer->GetMap()->GetCorpseByPlayer(dead->GetGUID())"), std::string::npos);
    EXPECT_NE(selection.find("HasResurrectionReservation"), std::string::npos);
    EXPECT_NE(selection.find("ResurrectionTargetPolicy::SearchRadius"), std::string::npos);
    EXPECT_NE(reach.find("GetAnchor(resolved)"), std::string::npos);
    EXPECT_NE(reach.find("MoveToLOS(anchor, true)"), std::string::npos);
    EXPECT_NE(reach.find("MoveTo(anchor, distance, MovementPriority::MOVEMENT_NORMAL)"), std::string::npos);
    EXPECT_NE(cast.find("targets.SetCorpseTarget(corpse)"), std::string::npos);
    EXPECT_NE(cast.find("CastSpellAction::Execute(event)"), std::string::npos);
    EXPECT_NE(cast.find("bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)"), std::string::npos);
    EXPECT_NE(druid.find("return ResurrectPartyMemberAction::isUseful();"), std::string::npos);
    EXPECT_NE(druidNonCombat.find("NextAction(\"revive\""), std::string::npos);
    EXPECT_EQ(druidNonCombat.find("NextAction(\"rebirth\""), std::string::npos);
    EXPECT_NE(druidCombat.find("combat party member dead"), std::string::npos);
    EXPECT_NE(druidCombat.find("NextAction(\"rebirth\""), std::string::npos);
    EXPECT_NE(cast.find("caster_combat={}"), std::string::npos);
    EXPECT_NE(cast.find("combat_res={}"), std::string::npos);
    EXPECT_NE(accept.find("[CorpseRescue] accept"), std::string::npos);
    EXPECT_NE(accept.find("delayed={}"), std::string::npos);
    EXPECT_NE(playerScript.find("PLAYERHOOK_ON_PLAYER_RESURRECT"), std::string::npos);
    EXPECT_NE(playerScript.find("[CorpseRescue] completed"), std::string::npos);
    EXPECT_NE(ownedSource.find("[CorpseRescue]"), std::string::npos);
    EXPECT_EQ(ownedSource.find("ResurrectPlayer("), std::string::npos);
    EXPECT_EQ(ownedSource.find("TeleportTo("), std::string::npos);
}
