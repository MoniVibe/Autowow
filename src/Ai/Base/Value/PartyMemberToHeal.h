/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PARTYMEMBERTOHEAL_H
#define PLAYERBOTS_PARTYMEMBERTOHEAL_H

#include "PartyMemberValue.h"

namespace PartyMemberToHealPolicy
{
constexpr float kMaximumSelectableScore = 100.0f;

enum class UrgencyBand
{
    Critical,
    Low,
    Medium,
    Stable
};

enum class RolePriority
{
    ExplicitMainTank,
    Tank,
    Other
};

struct CandidatePriority
{
    UrgencyBand urgencyBand = UrgencyBand::Stable;
    RolePriority rolePriority = RolePriority::Other;
    float healthDistanceScore = kMaximumSelectableScore;
};

inline UrgencyBand CalculateUrgencyBand(float healthPct, float criticalHealth, float lowHealth,
                                        float mediumHealth)
{
    if (healthPct < criticalHealth)
        return UrgencyBand::Critical;
    if (healthPct < lowHealth)
        return UrgencyBand::Low;
    if (healthPct < mediumHealth)
        return UrgencyBand::Medium;
    return UrgencyBand::Stable;
}

inline float CalculateHealthDistanceScore(float healthPct, float distance2d, float healDistance)
{
    float const distancePenalty = distance2d > healDistance ? 30.0f : distance2d / 10.0f;
    return healthPct + distancePenalty;
}

// Urgency is intentionally compared before role: a critical DPS target must beat a noncritical
// tank. Critical targets are then ordered by health and distance so a nearly dead nearby ally is
// not sacrificed for a healthier distant tank. Ordinary bands retain main-tank/tank priority.
inline CandidatePriority CalculatePriority(float healthPct, float distance2d, float healDistance,
                                           float criticalHealth, float lowHealth, float mediumHealth,
                                           bool explicitMainTank, bool tank)
{
    RolePriority rolePriority = RolePriority::Other;
    if (explicitMainTank)
        rolePriority = RolePriority::ExplicitMainTank;
    else if (tank)
        rolePriority = RolePriority::Tank;

    return { CalculateUrgencyBand(healthPct, criticalHealth, lowHealth, mediumHealth), rolePriority,
             CalculateHealthDistanceScore(healthPct, distance2d, healDistance) };
}

inline CandidatePriority CalculateCompanionPriority(float healthPct, float criticalHealth,
                                                    float lowHealth, float mediumHealth)
{
    return { CalculateUrgencyBand(healthPct, criticalHealth, lowHealth, mediumHealth),
             RolePriority::Other, healthPct + 30.0f };
}

inline bool IsSelectable(CandidatePriority const& candidate)
{
    return candidate.healthDistanceScore < kMaximumSelectableScore;
}

// Strict comparisons keep the first candidate when every ordering key is equal.
inline bool ShouldReplace(CandidatePriority const& candidate, CandidatePriority const& current)
{
    if (candidate.urgencyBand != current.urgencyBand)
        return candidate.urgencyBand < current.urgencyBand;
    if (candidate.urgencyBand == UrgencyBand::Critical &&
        candidate.healthDistanceScore != current.healthDistanceScore)
    {
        return candidate.healthDistanceScore < current.healthDistanceScore;
    }
    if (candidate.rolePriority != current.rolePriority)
        return candidate.rolePriority < current.rolePriority;
    return candidate.healthDistanceScore < current.healthDistanceScore;
}

// Preserve the existing non-raid emergency behavior, but in raids honor another healer's cast
// reservation unless the target is already in the critical band.
inline bool CanSelectWithIncomingHeal(float healthPct, bool isRaid, bool hasIncomingHeal,
                                      float criticalHealth, float mediumHealth)
{
    return !hasIncomingHeal || healthPct < criticalHealth || (!isRaid && healthPct < mediumHealth);
}
}

class Pet;
class PlayerbotAI;
class Unit;

class PartyMemberToHeal : public PartyMemberValue
{
public:
    PartyMemberToHeal(PlayerbotAI* botAI, std::string const name = "party member to heal")
        : PartyMemberValue(botAI, name)
    {
    }

protected:
    Unit* Calculate() override;
    bool Check(Unit* player) override;
};

class PartyMemberToProtect : public PartyMemberValue
{
public:
    PartyMemberToProtect(PlayerbotAI* botAI, std::string const name = "party member to protect")
        : PartyMemberValue(botAI, name)
    {
    }

protected:
    Unit* Calculate() override;
};

class HealerLowMana : public PartyMemberValue
{
public:
    HealerLowMana(PlayerbotAI* botAI) : PartyMemberValue(botAI, "healer low mana") {}

protected:
    Unit* Calculate() override;
};

#endif
