/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
std::string ReadObserverControlSource()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/AutoWow/ObserverControl.cpp");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string ReadRelocateSource()
{
    std::string const source = ReadObserverControlSource();
    std::size_t const functionStart = source.find("std::string Relocate");
    std::size_t const functionEnd = source.find("std::string Status", functionStart);
    if (functionStart == std::string::npos || functionEnd == std::string::npos)
        return {};
    return source.substr(functionStart, functionEnd - functionStart);
}
}  // namespace

TEST(ObserverControlPolicy, RelocateLiftsAboveValidatedFloorAndKeepsSafetyGuards)
{
    std::string const fullSource = ReadObserverControlSource();
    std::string const source = ReadRelocateSource();
    ASSERT_FALSE(fullSource.empty());
    ASSERT_FALSE(source.empty());

    for (std::string_view const required : {
             "IsProtected(observer)", "observer->GetGroup()", "observer->IsInCombat()",
             "leader->FindMap()", "MapMgr::IsValidMapCoord", "GetHeight(",
             "INVALID_HEIGHT", "std::isfinite", "CameraVerticalOffset",
             "MaxCameraVerticalOffset", "observer_floor_unavailable",
             "observer_floor_context_invalid", "observer_context_invalid",
             "leader_context_invalid", "camera_offset_z_yards",
             "camera_lift_yards", "cameraFloorZ + CameraVerticalOffset",
             "cameraZ > cameraFloorZ", "cameraZ"})
        EXPECT_NE(source.find(required), std::string::npos) << required;

    EXPECT_NE(fullSource.find("CameraVerticalOffset = 2.0f"), std::string::npos);
    EXPECT_NE(fullSource.find("MaxCameraVerticalOffset = 3.0f"), std::string::npos);
    EXPECT_LT(source.find("observer_floor_unavailable"),
              source.find("bool installedTemporaryBind"));
    EXPECT_NE(source.find("TeleportTo(leaderMap, cameraX, cameraY, cameraZ, lo"),
             std::string::npos);
    EXPECT_EQ(source.find("TeleportTo(leaderMap, cameraX, cameraY, lz, lo"),
              std::string::npos);
    EXPECT_EQ(source.find("leader->Set"), std::string::npos);
    EXPECT_EQ(source.find("leader->Teleport"), std::string::npos);
}
