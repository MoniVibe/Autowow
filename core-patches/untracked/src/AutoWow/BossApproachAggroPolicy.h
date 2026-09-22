/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#ifndef MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_AGGRO_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_AGGRO_POLICY_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace AutoWowBossApproachAggroPolicy
{
inline constexpr float kUnknownHoldMargin = 8.0f;
inline constexpr float kFormationFootprintBuffer = 3.0f;
inline constexpr float kPetFootprintBuffer = 4.0f;

struct HazardFacts
{
    std::uint64_t guid = 0;
    bool knownHostile = false;
    bool currentlyEngagedByCohort = false;
    bool canStartAttack = false;
    bool detectionUnknown = false;
    bool scriptedUnknown = false;
    bool socialAssistPossible = false;
    bool intentionalTarget = false;
    bool tankProbe = false;
    bool petFootprint = false;
    float distanceToCorridor = std::numeric_limits<float>::infinity();
    float aggroRadius = 0.0f; // Core Creature::GetAggroRange result for the nearest cohort unit.
    float combatReach = 0.0f;
    float footprintBuffer = 0.0f;
};

struct RiskResult
{
    bool safe = true;
    bool unknownBlocks = false;
    std::string reason = "clear";
    std::size_t hazardsInCorridor = 0;
    std::size_t predictedHazards = 0;
    std::size_t unknownHazards = 0;
    std::size_t accidentalPullCount = 0;
    std::size_t socialAssistCandidates = 0;
    std::size_t tankProbeHazards = 0;
    std::size_t petHazards = 0;
    std::size_t intentionalExemptions = 0;
    float closestMargin = std::numeric_limits<float>::infinity();
};

inline RiskResult Evaluate(std::vector<HazardFacts> const& hazards, bool pullAuthorized)
{
    RiskResult result;
    for (HazardFacts const& hazard : hazards)
    {
        if (!std::isfinite(hazard.distanceToCorridor))
            continue;

        bool const intentional = hazard.intentionalTarget && pullAuthorized;
        if (intentional)
        {
            ++result.intentionalExemptions;
            continue;
        }

        float const margin = hazard.distanceToCorridor -
            (hazard.aggroRadius + hazard.combatReach + hazard.footprintBuffer);
        result.closestMargin = std::min(result.closestMargin, margin);

        bool const corridorRelevant = margin <= kUnknownHoldMargin;
        if (!corridorRelevant)
            continue;
        ++result.hazardsInCorridor;

        bool const unknown = hazard.detectionUnknown || hazard.scriptedUnknown;
        if (unknown)
        {
            ++result.unknownHazards;
            if (margin <= kUnknownHoldMargin)
            {
                result.safe = false;
                result.unknownBlocks = true;
                result.reason = "hazard_visibility_or_script_unknown";
            }
        }

        bool const likelyAggro = hazard.knownHostile && (hazard.canStartAttack || margin <= 0.0f);
        if (likelyAggro && margin <= 0.0f)
        {
            ++result.predictedHazards;
            if (!hazard.currentlyEngagedByCohort)
                ++result.accidentalPullCount;
            if (hazard.socialAssistPossible)
                ++result.socialAssistCandidates;
            if (hazard.tankProbe)
                ++result.tankProbeHazards;
            if (hazard.petFootprint)
                ++result.petHazards;
            result.safe = false;
            if (!result.unknownBlocks)
                result.reason = "predicted_aggro_or_assist";
        }
    }

    if (result.safe && result.unknownHazards)
        result.reason = "unknown_near_corridor_but_outside_hold_margin";
    return result;
}
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_AGGRO_POLICY_H
