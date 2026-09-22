/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#ifndef MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AutoWowBossApproachPolicy
{
inline constexpr float kCohesionRadius = 36.0f;
inline constexpr float kSlotTolerance = 6.0f;
inline constexpr std::uint32_t kMaxStalledPolls = 4;

enum class Role
{
    MainTank,
    Tank,
    Melee,
    Ranged,
    Healer
};

enum class Phase
{
    Blocked,
    Advancing,
    Assembling,
    Ready,
    Stalled
};

struct MemberFacts
{
    std::uint64_t guid = 0;
    bool alive = false;
    bool mainTank = false;
    bool tank = false;
    bool healer = false;
    bool ranged = false;
};

struct FormationSlot
{
    std::uint64_t memberGuid = 0;
    Role role = Role::Melee;
    std::uint32_t roleIndex = 0;
    float trailing = 0.0f;
    float lateral = 0.0f;
};

inline char const* RoleName(Role role)
{
    switch (role)
    {
        case Role::MainTank: return "main_tank";
        case Role::Tank: return "tank";
        case Role::Melee: return "melee";
        case Role::Ranged: return "ranged";
        case Role::Healer: return "healer";
        default: return "unknown";
    }
}

inline char const* PhaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::Blocked: return "blocked";
        case Phase::Advancing: return "advancing";
        case Phase::Assembling: return "assembling";
        case Phase::Ready: return "ready";
        case Phase::Stalled: return "stalled";
        default: return "unknown";
    }
}

inline FormationSlot MakeSlot(std::uint64_t guid, Role role, std::uint32_t roleIndex)
{
    FormationSlot slot{guid, role, roleIndex, 0.0f, 0.0f};
    std::uint32_t const row = roleIndex / 2u;
    float const side = (roleIndex % 2u) ? 1.0f : -1.0f;

    switch (role)
    {
        case Role::MainTank:
            break;
        case Role::Tank:
            slot.trailing = 3.0f + 2.5f * static_cast<float>(row);
            slot.lateral = side * (3.0f + 2.0f * static_cast<float>(row));
            break;
        case Role::Melee:
            slot.trailing = 6.0f + 2.5f * static_cast<float>(row);
            slot.lateral = side * (4.0f + 2.5f * static_cast<float>(row));
            break;
        case Role::Ranged:
            slot.trailing = 13.0f + 2.5f * static_cast<float>(row);
            slot.lateral = side * (7.0f + 3.0f * static_cast<float>(row));
            break;
        case Role::Healer:
            slot.trailing = 16.0f + 2.5f * static_cast<float>(row);
            slot.lateral = side * (9.0f + 3.0f * static_cast<float>(row));
            break;
    }
    return slot;
}

// Slots are chosen from role facts only. Input order is deliberately ignored so that a raid and a
// party receive the same layout for the same GUID/role set, independent of GroupReference order.
inline std::vector<FormationSlot> BuildFormationSlots(std::vector<MemberFacts> members)
{
    std::sort(members.begin(), members.end(), [](MemberFacts const& left, MemberFacts const& right)
    {
        return left.guid < right.guid;
    });

    std::uint64_t mainTankGuid = 0;
    for (MemberFacts const& member : members)
    {
        if (member.alive && member.tank && member.mainTank)
        {
            mainTankGuid = member.guid;
            break;
        }
    }
    if (!mainTankGuid)
    {
        for (MemberFacts const& member : members)
        {
            if (member.alive && member.tank)
            {
                mainTankGuid = member.guid;
                break;
            }
        }
    }

    std::vector<FormationSlot> slots;
    slots.reserve(members.size());
    auto appendRole = [&slots, &members](std::uint64_t guid, Role role, std::uint32_t roleIndex)
    {
        (void)members;
        slots.push_back(MakeSlot(guid, role, roleIndex));
    };

    if (mainTankGuid)
        appendRole(mainTankGuid, Role::MainTank, 0);

    auto appendCategory = [&members, &slots, mainTankGuid](Role role, auto predicate)
    {
        std::uint32_t roleIndex = 0;
        for (MemberFacts const& member : members)
        {
            if (!member.alive || member.guid == mainTankGuid || !predicate(member))
                continue;
            slots.push_back(MakeSlot(member.guid, role, roleIndex++));
        }
    };

    appendCategory(Role::Tank, [](MemberFacts const& member) { return member.tank; });
    appendCategory(Role::Melee, [](MemberFacts const& member)
    {
        return !member.tank && !member.healer && !member.ranged;
    });
    appendCategory(Role::Ranged, [](MemberFacts const& member)
    {
        return !member.tank && !member.healer && member.ranged;
    });
    appendCategory(Role::Healer, [](MemberFacts const& member) { return !member.tank && member.healer; });
    return slots;
}

struct CohesionFacts
{
    std::size_t rosterMembers = 0;
    std::size_t livingMembers = 0;
    std::size_t onlinePlayerbots = 0;
    std::size_t tankMembers = 0;
    std::size_t sameContext = 0;
    std::size_t transientFree = 0;
    std::size_t withinCohesion = 0;
    std::size_t slotReady = 0;
    std::size_t exactRangeAndLos = 0;
    bool targetVisible = false;
    bool pathsSafe = true;
    bool aggroSafe = true;
    bool unknownHazard = false;
    bool movementIssued = false;
    bool progressMade = false;
    std::uint32_t stalledPolls = 0;
};

struct Decision
{
    Phase phase = Phase::Blocked;
    std::string reason = "uninitialized";
};

inline Decision Evaluate(CohesionFacts const& facts,
                         float cohesionRadius = kCohesionRadius,
                         float slotTolerance = kSlotTolerance,
                         std::uint32_t maxStalledPolls = kMaxStalledPolls)
{
    (void)cohesionRadius;
    (void)slotTolerance;

    if (!facts.rosterMembers)
        return {Phase::Blocked, "empty_roster"};
    if (facts.livingMembers < 2)
        return {Phase::Blocked, "insufficient_living_cohort"};
    if (!facts.tankMembers)
        return {Phase::Blocked, "no_tank_forward_probe"};
    if (!facts.targetVisible)
        return {Phase::Blocked, "no_eligible_boss"};
    if (facts.livingMembers != facts.rosterMembers)
        return {Phase::Assembling, "dead_party_member"};
    if (facts.onlinePlayerbots != facts.rosterMembers)
        return {Phase::Assembling, "member_unavailable"};
    if (facts.sameContext != facts.rosterMembers)
        return {Phase::Assembling, "member_wrong_map_or_instance"};
    if (facts.transientFree != facts.rosterMembers)
        return {Phase::Assembling, "member_teleporting_or_falling"};
    if (!facts.pathsSafe)
        return {Phase::Stalled, "formation_path_rejected"};
    if (!facts.aggroSafe)
        return {Phase::Stalled, facts.unknownHazard ? "hazard_visibility_or_script_unknown" : "predicted_aggro_or_assist"};
    if (facts.stalledPolls >= maxStalledPolls && !facts.progressMade)
        return {Phase::Stalled, "cohort_no_progress"};
    if (facts.slotReady != facts.rosterMembers || facts.withinCohesion != facts.rosterMembers)
        return {facts.movementIssued && facts.progressMade ? Phase::Advancing : Phase::Assembling,
                facts.movementIssued ? "cohort_assembling" : "formation_not_ready"};
    if (facts.exactRangeAndLos != facts.rosterMembers)
        return {Phase::Assembling, "cohort_not_in_exact_engage_range"};
    return {Phase::Ready, "cohort_ready"};
}
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_BOSS_APPROACH_POLICY_H
