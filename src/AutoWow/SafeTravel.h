/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_SAFE_TRAVEL_H
#define AUTOWOW_SAFE_TRAVEL_H

#include <cstdint>

#include "ErrandsPolicy.h"
#include "PlayerbotAIConfig.h"
#include "ZoneProgressionPolicy.h"

// AutoWow.Travel.Safe (default 0). soak-s16-full-r1: a travel leg (town run, zone graduation) ran with
// the ordinary non-combat strategies, so grind could pull and loot could detour mid-trip, and the next
// walk tick stood a low-hp bot up from eating. With the flag on, while a trip is under way the bot pulls
// nothing proactively (GrindTargetValue; attackers are still fought), loots nothing optional (loot
// triggers), and WalkLeg does not resume below the rest-gate thresholds (AutoWowRestGate::HoldTravel).
// Chunk choice (WalkingV2Policy PathThreat / PickChunk) and the errands rescue leg (ErrandsPolicy
// RescueLeg) carry the rest of the flag.
namespace AutoWowSafeTravel
{
// An errand run or a zone-progression trip is under way for this bot (false with the flag off).
inline bool OnTravelLeg(std::uint32_t guid)
{
    return sPlayerbotAIConfig.autoWowTravelSafe &&
           ((AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)) ||
            (AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid)));
}
}  // namespace AutoWowSafeTravel

#endif  // AUTOWOW_SAFE_TRAVEL_H
