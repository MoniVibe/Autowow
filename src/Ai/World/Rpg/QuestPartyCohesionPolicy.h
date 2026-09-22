#ifndef PLAYERBOTS_QUEST_PARTY_COHESION_POLICY_H
#define PLAYERBOTS_QUEST_PARTY_COHESION_POLICY_H

#include <cstdint>

namespace AutoWowQuestPartyCohesion
{
// This policy governs only ordinary campaign travel.  It deliberately does not replace combat,
// loot, scripted escort, or corpse-recovery behavior with a formation rule.
enum class Action : std::uint8_t
{
    NotApplicable,
    AllowObjective,
    RejoinLeader,
    AllowIndependent,
    AllowException
};

enum class Reason : std::uint8_t
{
    NoParty,
    Leader,
    LeaderUnavailable,
    CrossMap,
    WithinRadius,
    OutsideRadius,
    SelfDead,
    SelfCombat,
    ObjectiveWorkWindow
};

struct Facts
{
    bool inParty = false;
    bool follower = false;
    bool leaderAvailable = false;
    bool leaderAlive = false;
    bool sameMap = false;
    bool selfAlive = true;
    bool selfInCombat = false;
    bool objectiveWorkWindow = false;
    float distanceToLeader = 0.0f;
};

struct Result
{
    Action action = Action::NotApplicable;
    Reason reason = Reason::NoParty;
};

// A 45-yard radius keeps a walking quest party together without forcing ranged members to stack
// during combat.  The check is intentionally one-sided: only a follower is made to rejoin; the
// leader never changes course because of a lagging member.
inline constexpr float kCampaignCohesionRadius = 45.0f;

[[nodiscard]] inline Result Evaluate(Facts const& facts)
{
    if (!facts.inParty)
        return {Action::NotApplicable, Reason::NoParty};
    if (!facts.follower)
        return {Action::NotApplicable, Reason::Leader};
    if (!facts.selfAlive)
        return {Action::AllowException, Reason::SelfDead};
    if (facts.selfInCombat)
        return {Action::AllowException, Reason::SelfCombat};
    if (facts.objectiveWorkWindow)
        return {Action::AllowException, Reason::ObjectiveWorkWindow};
    // Cohesion is a preference, not a dependency.  A follower that cannot currently share the
    // leader's map or observe a live leader must continue its own valid quest work rather than
    // wait forever or use a teleport-like shortcut to manufacture party alignment.
    if (!facts.leaderAvailable || !facts.leaderAlive)
        return {Action::AllowIndependent, Reason::LeaderUnavailable};
    if (!facts.sameMap)
        return {Action::AllowIndependent, Reason::CrossMap};
    if (facts.distanceToLeader <= kCampaignCohesionRadius)
        return {Action::AllowObjective, Reason::WithinRadius};
    return {Action::RejoinLeader, Reason::OutsideRadius};
}
}

#endif
