/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_WARSONG_FIXTURE_CONTROL_H
#define AUTOWOW_WARSONG_FIXTURE_CONTROL_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Define.h"

// Controlled, exact-roster Warsong Gulch fixture operations for the localhost AutoWow bridge.
// Queue, Status, and Leave are world-thread-only. The bridge parser may call the two pure roster
// helpers from its network thread.
namespace AutoWowWarsong
{
inline constexpr std::size_t kRosterSize = 20;
inline constexpr std::size_t kFactionSize = 10;
inline constexpr std::array<std::string_view, 7> kCanonicalStatusRootFields = {
    "ok", "instance_id", "battleground_state", "roster", "score", "flags", "winner"};
inline constexpr std::array<std::string_view, 14> kCanonicalStatusPlayerFields = {
    "guid", "faction", "team", "instance_id", "queue_state", "battleground_state", "kills",
    "deaths", "honorable_kills", "damage", "healing", "flag_state", "flag_captures", "flag_returns"};

enum class WireOperation
{
    Queue,
    Status,
    Leave
};

bool ValidateRosterShape(std::vector<uint32> const& roster, std::string& error);
bool IsSameRoster(std::vector<uint32> const& left, std::vector<uint32> const& right);
bool ParseWireRequest(std::string const& requestText, WireOperation& operation,
                      std::vector<uint32>& roster, std::string& error);

std::string Queue(std::vector<uint32> const& roster);
std::string Status(std::vector<uint32> const& roster);
std::string Leave(std::vector<uint32> const& roster);
}

#endif  // AUTOWOW_WARSONG_FIXTURE_CONTROL_H
