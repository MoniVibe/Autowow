/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ENCOUNTER_TELEMETRY_H
#define AUTOWOW_ENCOUNTER_TELEMETRY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;

// Read-only, bounded encounter witness for the AutoWow bridge. Build() is called only by the
// bridge's existing world-thread operation and never retains live object addresses after returning.
namespace AutoWowEncounterTelemetry
{
inline constexpr char kSchema[] = "autowow.encounter-telemetry.v0";
inline constexpr std::uint32_t kVersion = 0;
inline constexpr float kEncounterRadius = 250.0f;
inline constexpr std::size_t kMaxRosterGuids = 40;
inline constexpr std::size_t kMaxMembers = 40;
inline constexpr std::size_t kMaxEncounterUnits = 64;

// Value-only input to the generic encounter filter. Multiple group-member grid searches may report
// the same creature; SelectCandidates merges those reports by GUID before sorting and capping them.
struct CandidateSignal
{
    std::uint64_t guid = 0;
    bool inWorld = false;
    bool alive = false;
    bool hostile = false;
    bool boss = false;
    bool engaged = false;
    float distance = 0.0f;
};

struct CandidateSelection
{
    std::vector<CandidateSignal> candidates;
    std::size_t eligibleBeforeCap = 0;
    bool truncated = false;
};

// Value-only per-member relationship facts used to aggregate a creature's ownership by the group.
struct MemberEngagementSignal
{
    std::uint64_t memberGuid = 0;
    bool threatens = false;
    bool targets = false;
    bool victimLink = false;
    bool combatLink = false;
};

struct ThreatAggregate
{
    std::uint32_t threateningMembers = 0;
    std::uint32_t targetingMembers = 0;
    std::uint32_t victimLinks = 0;
    std::uint32_t combatLinks = 0;
    std::uint32_t threateningOrTargetingMembers = 0;
    std::uint32_t involvedMembers = 0;
};

float Percent(std::uint32_t current, std::uint32_t maximum);
bool IsCandidateEligible(CandidateSignal const& candidate, float radius = kEncounterRadius);
CandidateSelection SelectCandidates(std::vector<CandidateSignal> candidates,
                                    std::size_t cap = kMaxEncounterUnits,
                                    float radius = kEncounterRadius);
ThreatAggregate AggregateThreat(std::vector<MemberEngagementSignal> signals);

// Collect and serialize one instantaneous group encounter snapshot. Read-only; world-thread only.
std::string Build(Player* leader, PlayerbotAI* leaderAI);
}

#endif  // AUTOWOW_ENCOUNTER_TELEMETRY_H
