/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONPULLREADINESSPOLICY_H
#define PLAYERBOTS_DUNGEONPULLREADINESSPOLICY_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace DungeonPullReadiness
{
constexpr float DefaultSupportRadius = 38.5f;
constexpr float DefaultDpsReadyShare = 0.60f;

enum class Decision
{
    Ready,
    SelfDefense,
    AlreadyEngaged,
    InvalidRequirements,
    TankNotReady,
    HealerNotReady,
    DpsShareNotReady,
    TargetOutsideSupportEnvelope
};

enum class Role
{
    None,
    Tank,
    Healer,
    Dps
};

struct Requirements
{
    float supportRadius = DefaultSupportRadius;
    float dpsReadyShare = DefaultDpsReadyShare;
};

struct MemberFacts
{
    bool online = false;
    bool alive = false;
    bool inWorld = false;
    bool sameMap = false;
    bool sameInstance = false;
    bool teleporting = false;
    bool tank = false;
    bool healer = false;
    bool dps = false;
    float distanceToInitiator = 0.0f;
};

struct Facts
{
    bool initiatorIsTank = false;
    bool targetThreatensGroupOrPet = false;
    float targetDistance = 0.0f;
    std::vector<MemberFacts> members;
    bool existingCombatOrThreat = false;
};

struct Result
{
    Decision decision = Decision::InvalidRequirements;
    std::size_t availableTanks = 0;
    std::size_t readyTanks = 0;
    std::size_t availableHealers = 0;
    std::size_t readyHealers = 0;
    std::size_t availableDps = 0;
    std::size_t readyDps = 0;
    std::size_t requiredReadyDps = 0;
};

inline bool IsAvailable(MemberFacts const& member)
{
    return member.online && member.alive && member.inWorld && member.sameMap && member.sameInstance &&
        !member.teleporting;
}

// Role predicates in the host can overlap while strategies are changing. Resolve them in a fixed
// order so each available member contributes to exactly one readiness class.
inline Role ClassifyRole(MemberFacts const& member)
{
    if (member.tank)
        return Role::Tank;
    if (member.healer)
        return Role::Healer;
    if (member.dps)
        return Role::Dps;
    return Role::None;
}

inline char const* ToString(Decision decision)
{
    switch (decision)
    {
        case Decision::Ready:
            return "Ready";
        case Decision::SelfDefense:
            return "SelfDefense";
        case Decision::AlreadyEngaged:
            return "AlreadyEngaged";
        case Decision::InvalidRequirements:
            return "InvalidRequirements";
        case Decision::TankNotReady:
            return "TankNotReady";
        case Decision::HealerNotReady:
            return "HealerNotReady";
        case Decision::DpsShareNotReady:
            return "DpsShareNotReady";
        case Decision::TargetOutsideSupportEnvelope:
            return "TargetOutsideSupportEnvelope";
    }
    return "InvalidRequirements";
}

inline Result Evaluate(Facts const& facts, Requirements const& requirements = Requirements{})
{
    Result result;

    // An already unavoidable pull must never be held by configuration or roster readiness.
    if (facts.targetThreatensGroupOrPet)
    {
        result.decision = Decision::SelfDefense;
        return result;
    }

    if (facts.existingCombatOrThreat)
    {
        result.decision = Decision::AlreadyEngaged;
        return result;
    }

    if (!std::isfinite(requirements.supportRadius) || requirements.supportRadius <= 0.0f ||
        !std::isfinite(requirements.dpsReadyShare) || requirements.dpsReadyShare <= 0.0f ||
        requirements.dpsReadyShare > 1.0f)
    {
        result.decision = Decision::InvalidRequirements;
        return result;
    }

    if (!facts.initiatorIsTank)
    {
        result.decision = Decision::TankNotReady;
        return result;
    }

    for (MemberFacts const& member : facts.members)
    {
        if (!IsAvailable(member))
            continue;

        bool const inSupport = std::isfinite(member.distanceToInitiator) &&
            member.distanceToInitiator >= 0.0f && member.distanceToInitiator <= requirements.supportRadius;
        switch (ClassifyRole(member))
        {
            case Role::Tank:
                ++result.availableTanks;
                result.readyTanks += inSupport ? 1u : 0u;
                break;
            case Role::Healer:
                ++result.availableHealers;
                result.readyHealers += inSupport ? 1u : 0u;
                break;
            case Role::Dps:
                ++result.availableDps;
                result.readyDps += inSupport ? 1u : 0u;
                break;
            case Role::None:
                break;
        }
    }

    // Keep the multiplication in float so exact decimal-style boundaries such as 5 * 0.60f
    // round to 3.0f before ceil instead of being widened with the input representation error.
    float const requiredDps = static_cast<float>(result.availableDps) * requirements.dpsReadyShare;
    result.requiredReadyDps = static_cast<std::size_t>(std::ceil(requiredDps));

    if (!result.readyHealers)
    {
        result.decision = Decision::HealerNotReady;
        return result;
    }
    if (result.readyDps < result.requiredReadyDps)
    {
        result.decision = Decision::DpsShareNotReady;
        return result;
    }
    if (!std::isfinite(facts.targetDistance) || facts.targetDistance < 0.0f ||
        facts.targetDistance > requirements.supportRadius)
    {
        result.decision = Decision::TargetOutsideSupportEnvelope;
        return result;
    }

    result.decision = Decision::Ready;
    return result;
}
}

#endif
