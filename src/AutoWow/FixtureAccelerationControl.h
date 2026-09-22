/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef AUTOWOW_FIXTURE_ACCELERATION_CONTROL_H
#define AUTOWOW_FIXTURE_ACCELERATION_CONTROL_H

#include <cstdint>
#include <string>

namespace AutoWowFixtureAcceleration
{
inline constexpr char kMicroScenarioGuidsConfigKey[] = "AutoWow.MicroScenarioGuids";
inline constexpr std::uint32_t kFixturePacingPercent = 1000;
inline constexpr float kFixturePacingMultiplier = 10.0F;

bool IsValidPacingPercent(std::uint32_t pacingPercent);
bool IsValidExactTarget(std::uint32_t entry, std::uint64_t stableSpawnId);

// These functions must be invoked on the world thread. The bridge dispatches them through the
// same world-thread queue used by fixture-init and the other fixture controls.
std::string Accelerate(std::uint32_t guid, std::uint32_t pacingPercent);
std::string Disable(std::uint32_t guid);
std::string KillExact(std::uint32_t guid, std::uint32_t entry, std::uint64_t stableSpawnId);
}

#endif // AUTOWOW_FIXTURE_ACCELERATION_CONTROL_H
