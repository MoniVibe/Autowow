/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_RAID_FIXTURE_CONTROL_H
#define AUTOWOW_RAID_FIXTURE_CONTROL_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Define.h"

// Exact-roster raid fixture control for the loopback AutoWow bridge. ParseWireRequest and the
// shape helpers are pure and may run on the bridge socket thread. Create, Status, and Leave inspect
// live world state and must run only through AutoWowBridgeOperation on the world thread.
namespace AutoWowRaid
{
inline constexpr std::array<std::size_t, 3> kSupportedRaidSizes = {10, 25, 40};
inline constexpr std::array<std::string_view, 12> kCanonicalStatusFields = {
    "ok", "operation", "leader_guid", "group_id", "group_type", "raid_size",
    "difficulty", "map", "instance_id", "roster", "roles_available", "proof"};

enum class WireOperation
{
    Create,
    Status,
    Leave
};

struct WireRequest
{
    WireOperation operation = WireOperation::Status;
    uint32 subjectGuid = 0;
    std::vector<uint32> memberGuids;
    uint32 difficulty = 0;
};

bool IsSupportedRaidSize(std::size_t size);
bool ValidateCreateShape(uint32 leaderGuid, std::vector<uint32> const& memberGuids,
                         uint32 difficulty, std::string& error);
bool IsSameRoster(std::vector<uint32> const& left, std::vector<uint32> const& right);
bool ParseWireRequest(std::string const& requestText, WireRequest& request, std::string& error);

std::string Create(uint32 leaderGuid, std::vector<uint32> const& memberGuids, uint32 difficulty);
std::string Status(uint32 leaderGuid);
std::string Leave(uint32 subjectGuid);
}

#endif  // AUTOWOW_RAID_FIXTURE_CONTROL_H
