/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_PERSISTENT_CORPSE_APPROACH_POLICY_H
#define PLAYERBOTS_PERSISTENT_CORPSE_APPROACH_POLICY_H

#include <cmath>

namespace PersistentCorpseApproachPolicy
{
constexpr float MinimumProgress = 2.0f;

enum class Decision
{
    None,
    DirectDestination,
    PartialEndpoint
};

struct Facts
{
    bool sameMap = false;
    bool directPathSafe = false;
    bool partialPathTypeAllowed = false;
    bool hasEndpoint = false;
    bool pathGrounded = false;
    float sourceToDestination = 0.0f;
    float sourceToEndpoint = 0.0f;
    float endpointToDestination = 0.0f;
};

inline Decision Select(Facts const& facts, float minimumProgress = MinimumProgress)
{
    if (!facts.sameMap || !std::isfinite(minimumProgress) || minimumProgress <= 0.0f ||
        !std::isfinite(facts.sourceToDestination) || facts.sourceToDestination <= 0.0f)
        return Decision::None;

    if (facts.directPathSafe)
        return Decision::DirectDestination;

    if (!facts.partialPathTypeAllowed || !facts.hasEndpoint || !facts.pathGrounded ||
        !std::isfinite(facts.sourceToEndpoint) || !std::isfinite(facts.endpointToDestination) ||
        facts.sourceToEndpoint < minimumProgress ||
        facts.endpointToDestination + minimumProgress > facts.sourceToDestination)
        return Decision::None;

    return Decision::PartialEndpoint;
}
}

#endif
