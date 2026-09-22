/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/RaidFixtureControl.h"
#include "../src/AutoWow/PortalAdmissionPolicy.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
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

std::vector<uint32> MembersForSize(std::size_t size)
{
    std::vector<uint32> members;
    for (uint32 guid = 1002; members.size() + 1 < size; ++guid)
        members.push_back(guid);
    return members;
}

std::string CreateWire(std::size_t size, uint32 difficulty)
{
    std::ostringstream wire;
    wire << "raid create 1001";
    for (uint32 guid : MembersForSize(size))
        wire << ' ' << guid;
    wire << " difficulty=" << difficulty;
    return wire.str();
}
}  // namespace

TEST(RaidFixtureControl, CreateShapeAllowsOnlyExactTenTwentyFiveAndForty)
{
    for (std::size_t size : AutoWowRaid::kSupportedRaidSizes)
    {
        std::string error;
        EXPECT_TRUE(AutoWowRaid::ValidateCreateShape(1001, MembersForSize(size), 0, error)) << size;
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(AutoWowRaid::ValidateCreateShape(1001, MembersForSize(size), 1, error)) << size;
    }

    for (std::size_t size : {9u, 11u, 24u, 26u, 39u, 41u})
    {
        std::string error;
        EXPECT_FALSE(AutoWowRaid::ValidateCreateShape(1001, MembersForSize(size), 0, error)) << size;
        EXPECT_EQ(error, "raid_requires_exactly_10_25_or_40_total_guids");
    }
}

TEST(RaidFixtureControl, CreateShapeRequiresPositiveUniqueLeaderAndMembersAndDifficulty)
{
    std::string error;
    std::vector<uint32> members = MembersForSize(10);
    members.front() = 1001;
    EXPECT_FALSE(AutoWowRaid::ValidateCreateShape(1001, members, 0, error));
    EXPECT_EQ(error, "raid_guids_must_be_unique");

    members = MembersForSize(10);
    members.back() = 0;
    EXPECT_FALSE(AutoWowRaid::ValidateCreateShape(1001, members, 0, error));
    EXPECT_EQ(error, "raid_guids_must_be_positive");

    members = MembersForSize(10);
    EXPECT_FALSE(AutoWowRaid::ValidateCreateShape(0, members, 0, error));
    EXPECT_EQ(error, "raid_guids_must_be_positive");
    EXPECT_FALSE(AutoWowRaid::ValidateCreateShape(1001, members, 2, error));
    EXPECT_EQ(error, "raid_difficulty_must_be_0_or_1");
}

TEST(RaidFixtureControl, CanonicalWireParsesCreateStatusAndLeave)
{
    for (std::size_t size : AutoWowRaid::kSupportedRaidSizes)
    {
        for (uint32 difficulty : {0u, 1u})
        {
            AutoWowRaid::WireRequest request;
            std::string error;
            EXPECT_TRUE(AutoWowRaid::ParseWireRequest(CreateWire(size, difficulty), request, error));
            EXPECT_EQ(request.operation, AutoWowRaid::WireOperation::Create);
            EXPECT_EQ(request.subjectGuid, 1001u);
            EXPECT_EQ(request.memberGuids, MembersForSize(size));
            EXPECT_EQ(request.difficulty, difficulty);
            EXPECT_TRUE(error.empty());
        }
    }

    AutoWowRaid::WireRequest status;
    std::string error;
    EXPECT_TRUE(AutoWowRaid::ParseWireRequest("raid status 1001", status, error));
    EXPECT_EQ(status.operation, AutoWowRaid::WireOperation::Status);
    EXPECT_EQ(status.subjectGuid, 1001u);

    AutoWowRaid::WireRequest leave;
    EXPECT_TRUE(AutoWowRaid::ParseWireRequest("raid leave 1009", leave, error));
    EXPECT_EQ(leave.operation, AutoWowRaid::WireOperation::Leave);
    EXPECT_EQ(leave.subjectGuid, 1009u);
}

TEST(RaidFixtureControl, DashedAliasesParseAndMalformedWireFailsClosed)
{
    AutoWowRaid::WireRequest request;
    std::string error;
    EXPECT_TRUE(AutoWowRaid::ParseWireRequest("raid-status 1001", request, error));
    EXPECT_EQ(request.operation, AutoWowRaid::WireOperation::Status);
    EXPECT_TRUE(AutoWowRaid::ParseWireRequest("raid-leave 1002", request, error));
    EXPECT_EQ(request.operation, AutoWowRaid::WireOperation::Leave);

    EXPECT_FALSE(AutoWowRaid::ParseWireRequest("raid status 1001 1002", request, error));
    EXPECT_FALSE(AutoWowRaid::ParseWireRequest("raid create 1001 1002 difficulty=0 1003", request, error));
    EXPECT_EQ(error, "raid create difficulty must be the final argument");
    EXPECT_FALSE(AutoWowRaid::ParseWireRequest("raid create 1001 difficulty=2", request, error));
    EXPECT_FALSE(AutoWowRaid::ParseWireRequest("raid create 1001 1002", request, error));
}

TEST(RaidFixtureControl, StatusContractNamesExactRosterLocationGroupAndRoles)
{
    constexpr std::array<std::string_view, 12> expected = {
        "ok", "operation", "leader_guid", "group_id", "group_type", "raid_size",
        "difficulty", "map", "instance_id", "roster", "roles_available", "proof"};
    EXPECT_EQ(AutoWowRaid::kCanonicalStatusFields, expected);

    std::string const source = ReadSource(ModuleRoot() / "src/AutoWow/RaidFixtureControl.cpp");
    ASSERT_FALSE(source.empty());
    for (std::string_view field : expected)
        EXPECT_NE(source.find("\\\"" + std::string(field) + "\\\""), std::string::npos) << field;

    for (std::string_view proof : {"exact_roster", "exact_leader", "exact_group_type",
                                   "exact_difficulty", "no_foreign_members", "same_map",
                                   "same_instance", "all_online_playerbots"})
        EXPECT_NE(source.find(proof), std::string::npos) << proof;

    for (std::string_view role : {"tank", "healer", "ranged_dps", "melee_dps"})
        EXPECT_NE(source.find(role), std::string::npos) << role;
}

TEST(RaidFixtureControl, RuntimeControlIsOrdinaryValidatedAndOwnerScoped)
{
    std::string const source = ReadSource(ModuleRoot() / "src/AutoWow/RaidFixtureControl.cpp");
    ASSERT_FALSE(source.empty());

    for (std::string_view required : {
             "GroupInviteOperation", "GroupConvertToRaidOperation", "SetRaidDifficulty",
             "autowow_league_member", "ObjectAccessor::FindPlayer", "GetPlayerbotAI", "IsRealPlayer",
             "raid_cross_faction_roster_not_allowed", "raid_foreign_group_member",
             "raid_exact_roster_verification_failed", "fixture.groupId", "OwnedGroup()",
             "group->RemoveMember", "group->Disband", "RollBackCreatedGroup"})
        EXPECT_NE(source.find(required), std::string::npos) << required;

    EXPECT_EQ(source.find("TeleportTo("), std::string::npos);
    EXPECT_EQ(source.find("new Group"), std::string::npos);
}

TEST(RaidFixtureControl, BridgeQueuesRaidWorkAndRaidPortalsUseOnlyExteriorRelocation)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());

    for (std::string_view required : {
             "AutoWowRaid::ParseWireRequest", "AutoWowRaid::Create", "AutoWowRaid::Status",
             "AutoWowRaid::Leave", "PlayerbotWorldThreadProcessor::instance().QueueOperation",
             "{ \"ony-exterior\", 1, -4747.17f, -3753.27f, 49.8122f",
             "{ \"ony-portal\", 1, -4760.0f, -3752.8f, 49.43f, false }",
             "{ \"ony-boss-approach\", 249, -10.0f, -180.0f, -87.0f, true }",
             "{ \"voa-exterior\", 571, 5494.88f, 2839.81f, 420.811f",
             "{ \"voa-portal\", 571, 5494.88f, 2839.81f, 416.811f, false }",
             "{ \"voa-entrance-staging\", 624, -285.0f, -103.4f, 106.1f, true }",
             "{ \"voa-central-hub\", 624, -219.29f, -126.94f, 102.95f, true }",
             "{ \"voa-archavon-west-staging\", 624, -150.0f, -103.5f, 103.3f, true }",
             "{ \"voa-archavon-approach\", 624, 93.885f, -101.531f, 91.401f, true }",
             "{ \"voa-emalon-north-staging\", 624, -219.0f, -145.0f, 101.0f, true }",
             "{ \"voa-emalon-south-corridor\", 624, -218.85f, -196.20f, 97.59f, true }",
             "{ \"voa-emalon-approach\", 624, -221.8f, -243.8f, 96.8f, true }",
             "{ \"voa-koralon-south-staging\", 624, -218.9f, -82.0f, 100.0f, true }",
             "{ \"voa-koralon-first-warder\", 624, -218.8f, -65.0f, 97.6f, true }",
             "{ \"voa-koralon-second-warder\", 624, -218.9f, -3.0f, 97.7f, true }",
             "{ \"voa-koralon-approach\", 624, -218.5f, 62.0f, 96.8f, true }",
             "{ \"voa-toravon-east-staging\", 624, -219.0f, -175.0f, 98.5f, true }",
             "{ \"voa-toravon-arch-corner\", 624, -130.0f, -175.0f, 98.0f, true }",
             "{ \"voa-toravon-safe-staging\", 624, -80.0f, -175.0f, 98.0f, true }",
             "{ \"voa-toravon-first-warder\", 624, -43.2f, -185.0f, 97.6f, true }",
             "{ \"voa-toravon-second-warder\", 624, -43.2f, -201.0f, 97.6f, true }",
             "{ \"voa-toravon-approach\", 624, -43.3f, -247.0f, 96.8f, true }",
             "{ \"voa-portal\", 571, 624, 5258, 8.0f }",
             "command == \"advancepoint\"", "AutoWowRequestType::AdvancePoint",
             "Waypoint coordinateWaypoint", "hasCoordinateOrientation",
             "formationOrientation = m_request.coordinateOrientation"})
        EXPECT_NE(bridge.find(required), std::string::npos) << required;

    std::size_t const routeStart = bridge.find("bool RouteParty()");
    std::size_t const routeEnd = bridge.find("bool AdvanceDungeonParty()", routeStart);
    ASSERT_NE(routeStart, std::string::npos);
    ASSERT_NE(routeEnd, std::string::npos);
    std::string const route = bridge.substr(routeStart, routeEnd - routeStart);
    std::size_t const voaRoute = route.find("{ \"voa-");
    ASSERT_NE(voaRoute, std::string::npos);
    EXPECT_EQ(route.find("{ \"voa-", voaRoute + 1), std::string::npos);
    EXPECT_NE(route.find("{ \"voa-exterior\", 571"), std::string::npos);

    std::size_t const advanceStart = bridge.find("bool AdvanceDungeonParty()");
    std::size_t const advanceEnd = bridge.find("bool EngageNearby()", advanceStart);
    ASSERT_NE(advanceStart, std::string::npos);
    ASSERT_NE(advanceEnd, std::string::npos);
    std::string const advance = bridge.substr(advanceStart, advanceEnd - advanceStart);
    EXPECT_EQ(advance.find("TeleportTo("), std::string::npos);
    EXPECT_NE(advance.find("AutoWowDungeonWalkAction"), std::string::npos);
    EXPECT_NE(advance.find("AutoWowDungeonWalkAction walk(memberAI)"), std::string::npos);
    EXPECT_NE(advance.find("AutoWowAdvanceFormation::ForSlot"), std::string::npos);
    EXPECT_NE(advance.find("AutoWowRequestType::AdvancePoint ? 0.0f : 1.0f"), std::string::npos);
    EXPECT_NE(advance.find("plan.stationary"), std::string::npos);
    EXPECT_NE(advance.find("advance_formation_floor_rejected"), std::string::npos);
    EXPECT_NE(advance.find("GetGridTerrainData(destination.x, destination.y)"), std::string::npos);
    EXPECT_NE(advance.find("formationGround - waypoint->z) > 8.0f"), std::string::npos);
    EXPECT_NE(advance.find("IsBeingTeleported()"), std::string::npos);
    EXPECT_NE(advance.find("IsInAreaTriggerRadius(portalTrigger, 0.0f)"), std::string::npos);
    EXPECT_NE(advance.find("follower_transfer_pending"), std::string::npos);
    EXPECT_NE(advance.find("follower_repositioning"), std::string::npos);
    EXPECT_NE(advance.find("advance_portal_member_displaced"), std::string::npos);
    EXPECT_NE(advance.find("\\\"formation\\\":\\\""), std::string::npos);
    EXPECT_NE(advance.find("\"single_file\" : \"corridor_grid\""), std::string::npos);
    EXPECT_EQ(advance.find("walk.Walk(waypoint->map, waypoint->x, waypoint->y, waypoint->z)"), std::string::npos);
    EXPECT_NE(advance.find("started_members"), std::string::npos);
    EXPECT_EQ(advance.find("AutoWowDungeonWalkAction walk(leaderAI)"), std::string::npos);

    std::string const walk = ReadSource(ModuleRoot() / "src/AutoWow/DungeonPathWalkAction.h");
    ASSERT_FALSE(walk.empty());
    EXPECT_NE(walk.find("AutoWowDungeonPath::Probe"), std::string::npos);
    EXPECT_NE(walk.find("WalkPrepared"), std::string::npos);
    EXPECT_NE(walk.find("MoveSplinePath"), std::string::npos);
    EXPECT_EQ(walk.find("false, false, false, true"), std::string::npos);
    EXPECT_EQ(walk.find("TeleportTo("), std::string::npos);

    std::string const navigator =
        ReadSource(ModuleRoot() / "src/Ai/Dungeon/Generic/DungeonNavigator.cpp");
    ASSERT_FALSE(navigator.empty());
    EXPECT_NE(navigator.find("AutoWowDungeonWalkAction walk(botAI)"), std::string::npos);
    EXPECT_NE(navigator.find("walk.WalkPrepared(selectedPreparedProbe)"), std::string::npos);
    EXPECT_NE(navigator.find("if (!map || !map->IsDungeon())"), std::string::npos);
    EXPECT_EQ(navigator.find("!map->IsNonRaidDungeon() || map->IsRaid()"), std::string::npos);

    std::string const playerbotAI = ReadSource(ModuleRoot() / "src/Bot/PlayerbotAI.cpp");
    ASSERT_FALSE(playerbotAI.empty());
    EXPECT_NE(playerbotAI.find("bot->GetMap()->IsDungeon()"), std::string::npos);
    EXPECT_EQ(playerbotAI.find("bot->GetMap()->IsNonRaidDungeon()"), std::string::npos);

    EXPECT_NE(advance.find("advance_path_rejected"), std::string::npos);
    EXPECT_NE(advance.find("plans.reserve"), std::string::npos);
    EXPECT_NE(advance.find("WalkPrepared(plan.probe)"), std::string::npos);
}

TEST(RaidFixtureControl, PortalAdmissionDistinguishesReadySplitAndDisplacedMembers)
{
    using AutoWowPortalAdmission::ClassifyMember;
    using AutoWowPortalAdmission::MemberLocation;

    EXPECT_EQ(ClassifyMember(624, 2, 624, 2, 571, false),
        MemberLocation::InsideExpectedInstance);
    EXPECT_EQ(ClassifyMember(571, 0, 624, 2, 571, true),
        MemberLocation::ReadyOutside);
    EXPECT_EQ(ClassifyMember(624, 3, 624, 2, 571, false),
        MemberLocation::SplitInstance);
    EXPECT_EQ(ClassifyMember(571, 0, 624, 2, 571, false),
        MemberLocation::Displaced);
    EXPECT_EQ(ClassifyMember(571, 0, 249, 7, 1, false),
        MemberLocation::Displaced);
}

TEST(RaidFixtureControl, ExteriorDungeonRoutesStackAtAuthoritativeIngressTriggerCenter)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());

    std::size_t const helperStart = bridge.find("AreaTrigger const* FindNonRaidDungeonExteriorTrigger(");
    std::size_t const routeStart = bridge.find("bool RouteParty()");
    std::size_t const routeEnd = bridge.find("bool AdvanceDungeonParty()", routeStart);
    ASSERT_NE(helperStart, std::string::npos);
    ASSERT_NE(routeStart, std::string::npos);
    ASSERT_NE(routeEnd, std::string::npos);

    std::string const helper = bridge.substr(helperStart, routeStart - helperStart);
    for (std::string_view required : {
             "IsNamedExteriorRoute(routeName)",
             "sObjectMgr->GetAllAreaTriggerTeleports()",
             "sObjectMgr->GetAreaTrigger(triggerId)",
             "sMapStore.LookupEntry(teleport.target_mapId)",
             "targetMap->IsNonRaidDungeon()",
             "AreaTriggerContainsPoint(*trigger, x, y, z)",
             "triggerId < best->entry"})
        EXPECT_NE(helper.find(required), std::string::npos) << required;

    std::string const route = bridge.substr(routeStart, routeEnd - routeStart);
    for (std::string_view required : {
             "FindNonRaidDungeonExteriorTrigger(",
             "exteriorTrigger ? exteriorTrigger->map : route->map",
             "exteriorTrigger ? exteriorTrigger->x : route->x",
             "exteriorTrigger ? exteriorTrigger->y : route->y",
             "exteriorTrigger ? exteriorTrigger->z : route->z",
             "member->TeleportTo(stagingMap, stagingX, stagingY, stagingZ, route->o)"})
        EXPECT_NE(route.find(required), std::string::npos) << required;

    // The ingress teleport's destination is intentionally never used by RouteParty. The normal
    // DungeonTransition action remains the only mechanism that admits the group to the instance.
    EXPECT_EQ(route.find("teleport.target_mapId"), std::string::npos);
    EXPECT_EQ(route.find("teleport.target_X"), std::string::npos);
    EXPECT_EQ(route.find("teleport.target_Y"), std::string::npos);
    EXPECT_EQ(route.find("teleport.target_Z"), std::string::npos);
}
