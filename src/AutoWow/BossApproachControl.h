/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#ifndef MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_CONTROL_H
#define MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_CONTROL_H

#include "BossApproachPolicy.h"

#include <cstddef>
#include <cstdint>
#include <string>

class Creature;
class Player;
class PlayerbotAI;

namespace AutoWowBossApproach
{
struct Result
{
    Creature* target = nullptr; // world-thread only; never serialized or retained by callers.
    bool ok = false;
    bool shouldEngage = false;
    bool engaged = false;
    AutoWowBossApproachPolicy::Phase phase = AutoWowBossApproachPolicy::Phase::Blocked;
    std::string reason = "uninitialized";
    std::uint32_t leaderGuid = 0;
    std::uint64_t targetGuid = 0;
    std::uint32_t targetEntry = 0;
    std::string targetName;
    float targetDistance = 0.0f;
    std::size_t rosterMembers = 0;
    std::size_t livingMembers = 0;
    std::size_t onlinePlayerbots = 0;
    std::size_t tankMembers = 0;
    std::size_t sameContext = 0;
    std::size_t transientFree = 0;
    std::size_t withinCohesion = 0;
    std::size_t slotReady = 0;
    std::size_t exactRangeAndLos = 0;
    float maxSlotDistance = 0.0f;
    float maxLeaderDistance = 0.0f;
    std::uint32_t stalledPolls = 0;
    std::uint32_t movementIssued = 0;
    std::uint32_t pathRejectedGuid = 0;
    std::size_t hazardsInCorridor = 0;
    std::size_t predictedHazards = 0;
    std::size_t unknownHazards = 0;
    std::size_t accidentalPullCount = 0;
    std::size_t socialAssistCandidates = 0;
    std::size_t tankProbeHazards = 0;
    std::size_t petHazards = 0;
    std::size_t intentionalExemptions = 0;
    std::size_t hazardObjectsScanned = 0;
    float closestHazardMargin = 0.0f;
    bool targetVisible = false;
    bool pathsSafe = true;
    bool aggroSafe = true;
    bool unknownHazardBlocks = false;
    bool pullAuthorized = false;
    bool leaderRouteAuthority = true;

    std::string Json() const;
};

// Run one world-thread approach tick. The leader is the sole route authority; followers never
// discover a different boss or issue an independent approach. statusOnly reads the last session
// facts and does not alter TravelTarget or movement state.
Result Run(Player* leader, PlayerbotAI* leaderAI, bool statusOnly = false);

// Remove only the temporary boss-approach travel intents and restore each member's prior
// non-combat travel/follow strategy. This is called immediately before normal attack intent.
void ReleaseMovement(Player* leader);

// Preserve a completed readiness snapshot for `boss status` without leaving temporary movement
// targets installed. A failed engage remains retryable on the next approach tick.
void MarkEngaged(Player* leader, bool success);
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_CONTROL_H
