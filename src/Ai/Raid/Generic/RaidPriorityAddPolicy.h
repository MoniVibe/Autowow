/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDPRIORITYADDPOLICY_H
#define PLAYERBOTS_RAIDPRIORITYADDPOLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace RaidPriorityAddPolicy
{
using Guid = std::uint64_t;

constexpr std::size_t kMaxDpsResponders = 2;
constexpr std::uint8_t kPrimaryInterruptRank = 1;

enum class AssignmentKind : std::uint8_t
{
    Interrupt,
    TankPickup,
    DpsFocus
};

enum class TargetDisposition : std::uint8_t
{
    NoAssignment,
    AcquireAssignedTarget,
    HoldAssignedTarget
};

struct RaidMember
{
    Guid guid = 0;
    bool canRespond = true;
    bool isHealer = false;
    bool isTank = false;
    bool isMainTank = false;
    bool ownsActiveBoss = false;
    bool isDps = false;
    bool isRanged = false;
    bool canInterrupt = false;
};

struct AddCandidate
{
    Guid guid = 0;
    Guid victimGuid = 0;
    bool threatBacked = false;
    bool isCasting = false;
    bool isElite = false;
};

struct Assignment
{
    Guid memberGuid = 0;
    Guid addGuid = 0;
    AssignmentKind kind = AssignmentKind::DpsFocus;
    std::uint8_t interruptRank = 0;
};

struct Plan
{
    Guid focusAddGuid = 0;
    Guid bossPressureReserveGuid = 0;
    std::vector<Assignment> assignments;

    Assignment const* FindAssignment(Guid memberGuid) const
    {
        auto const found = std::find_if(assignments.begin(), assignments.end(),
            [memberGuid](Assignment const& assignment) { return assignment.memberGuid == memberGuid; });
        return found == assignments.end() ? nullptr : &*found;
    }
};

inline char const* AssignmentKindName(AssignmentKind kind)
{
    switch (kind)
    {
        case AssignmentKind::Interrupt: return "interrupt";
        case AssignmentKind::TankPickup: return "tank_pickup";
        case AssignmentKind::DpsFocus: return "dps_focus";
    }
    return "unknown";
}

inline char const* InterruptAssignmentRankName(std::uint8_t rank)
{
    if (!rank)
        return "none";
    return rank == kPrimaryInterruptRank ? "primary" : "backup";
}

constexpr TargetDisposition ResolveTargetDisposition(Guid assignedTargetGuid, Guid currentTargetGuid)
{
    if (!assignedTargetGuid)
        return TargetDisposition::NoAssignment;
    return assignedTargetGuid == currentTargetGuid ? TargetDisposition::HoldAssignedTarget :
                                                     TargetDisposition::AcquireAssignedTarget;
}

namespace Detail
{
inline std::vector<RaidMember> NormalizeMembers(std::vector<RaidMember> members)
{
    std::sort(members.begin(), members.end(),
        [](RaidMember const& left, RaidMember const& right) { return left.guid < right.guid; });

    std::vector<RaidMember> normalized;
    for (RaidMember member : members)
    {
        if (!member.guid)
            continue;
        member.isTank = member.isTank || member.isMainTank;
        if (!normalized.empty() && normalized.back().guid == member.guid)
        {
            RaidMember& existing = normalized.back();
            existing.canRespond = existing.canRespond || member.canRespond;
            existing.isHealer = existing.isHealer || member.isHealer;
            existing.isTank = existing.isTank || member.isTank;
            existing.isMainTank = existing.isMainTank || member.isMainTank;
            existing.ownsActiveBoss = existing.ownsActiveBoss || member.ownsActiveBoss;
            existing.isDps = existing.isDps || member.isDps;
            existing.isRanged = existing.isRanged || member.isRanged;
            existing.canInterrupt = existing.canInterrupt || member.canInterrupt;
            continue;
        }
        normalized.push_back(member);
    }
    return normalized;
}

inline std::vector<AddCandidate> NormalizeAdds(std::vector<AddCandidate> adds)
{
    std::sort(adds.begin(), adds.end(),
        [](AddCandidate const& left, AddCandidate const& right) { return left.guid < right.guid; });

    std::vector<AddCandidate> normalized;
    for (AddCandidate const& add : adds)
    {
        if (!add.guid || !add.threatBacked)
            continue;
        if (!normalized.empty() && normalized.back().guid == add.guid)
        {
            AddCandidate& existing = normalized.back();
            if (add.victimGuid && (!existing.victimGuid || add.victimGuid < existing.victimGuid))
                existing.victimGuid = add.victimGuid;
            existing.isCasting = existing.isCasting || add.isCasting;
            existing.isElite = existing.isElite || add.isElite;
            continue;
        }
        normalized.push_back(add);
    }
    return normalized;
}

inline RaidMember const* FindMember(std::vector<RaidMember> const& members, Guid guid)
{
    auto const found = std::lower_bound(members.begin(), members.end(), guid,
        [](RaidMember const& member, Guid value) { return member.guid < value; });
    return found != members.end() && found->guid == guid ? &*found : nullptr;
}

struct AddPriority
{
    bool healerVictim = false;
    bool otherNonTankVictim = false;
    bool casting = false;
    bool elite = false;
    Guid guid = 0;
};

inline AddPriority GetPriority(AddCandidate const& add, std::vector<RaidMember> const& members)
{
    RaidMember const* victim = FindMember(members, add.victimGuid);
    bool const healerVictim = victim && victim->isHealer;
    bool const otherNonTankVictim = victim && !victim->isTank && !victim->isHealer;
    return {healerVictim, otherNonTankVictim, add.isCasting, add.isElite, add.guid};
}

inline bool Prefer(AddPriority const& candidate, AddPriority const& current)
{
    if (candidate.healerVictim != current.healerVictim)
        return candidate.healerVictim;
    if (candidate.otherNonTankVictim != current.otherNonTankVictim)
        return candidate.otherNonTankVictim;
    if (candidate.casting != current.casting)
        return candidate.casting;
    if (candidate.elite != current.elite)
        return candidate.elite;
    return candidate.guid < current.guid;
}

inline bool IsDpsResponder(RaidMember const& member)
{
    return member.canRespond && member.isDps && !member.isTank && !member.isHealer && !member.ownsActiveBoss;
}

inline bool IsInterruptResponder(RaidMember const& member)
{
    return member.canRespond && member.canInterrupt && !member.isHealer && !member.ownsActiveBoss;
}

inline std::uint8_t InterruptPreference(RaidMember const& member)
{
    if (IsDpsResponder(member))
        return 0;
    if (!member.isTank)
        return 1;
    return member.isMainTank ? 3 : 2;
}

inline bool IsCurrentAddVictim(Guid memberGuid, std::vector<AddCandidate> const& adds)
{
    return std::any_of(adds.begin(), adds.end(),
        [memberGuid](AddCandidate const& add) { return add.victimGuid == memberGuid; });
}

inline bool HasAssignment(Plan const& plan, Guid memberGuid)
{
    return plan.FindAssignment(memberGuid) != nullptr;
}
}

inline Plan BuildPlan(std::vector<RaidMember> members, std::vector<AddCandidate> adds,
    bool bossEngaged, bool bossHasVictim)
{
    members = Detail::NormalizeMembers(std::move(members));
    adds = Detail::NormalizeAdds(std::move(adds));

    std::sort(adds.begin(), adds.end(), [&members](AddCandidate const& left, AddCandidate const& right) {
        return Detail::Prefer(Detail::GetPriority(left, members), Detail::GetPriority(right, members));
    });

    Plan plan;
    if (adds.empty())
        return plan;

    plan.focusAddGuid = adds.front().guid;

    std::vector<AddCandidate const*> castingAdds;
    for (AddCandidate const& add : adds)
        if (add.isCasting)
            castingAdds.push_back(&add);
    std::sort(castingAdds.begin(), castingAdds.end(),
        [](AddCandidate const* left, AddCandidate const* right) { return left->guid < right->guid; });

    std::size_t const interruptResponderCount = static_cast<std::size_t>(std::count_if(
        members.begin(), members.end(), Detail::IsInterruptResponder));

    // During an active boss engagement, keep a safe ranged DPS on normal boss pressure whenever
    // doing so still leaves enough eligible interrupters to cover every observed active cast. A
    // non-interrupter is always a valid reserve because reserving it cannot reduce cast coverage.
    if (bossEngaged)
    {
        for (RaidMember const& member : members)
        {
            if (!Detail::IsDpsResponder(member) || !member.isRanged ||
                Detail::IsCurrentAddVictim(member.guid, adds))
            {
                continue;
            }
            if (!Detail::IsInterruptResponder(member) || interruptResponderCount > castingAdds.size())
            {
                plan.bossPressureReserveGuid = member.guid;
                break;
            }
        }
    }

    // Interrupt assignments are derived only from the currently observed cast bit, normal class
    // interrupt availability supplied on each member, raid role, and GUIDs. DPS responders are
    // preferred over tanks; target and member GUIDs provide permutation-stable tie breaking. One
    // primary is sufficient here: normal class actions remain authoritative for range, cooldown,
    // known spell, immunity, facing, resource, and GCD checks at execution time.
    std::vector<RaidMember const*> interrupters;
    for (RaidMember const& member : members)
    {
        if (Detail::IsInterruptResponder(member) && member.guid != plan.bossPressureReserveGuid)
            interrupters.push_back(&member);
    }
    std::sort(interrupters.begin(), interrupters.end(),
        [](RaidMember const* left, RaidMember const* right) {
            std::uint8_t const leftPreference = Detail::InterruptPreference(*left);
            std::uint8_t const rightPreference = Detail::InterruptPreference(*right);
            return leftPreference != rightPreference ? leftPreference < rightPreference : left->guid < right->guid;
        });

    std::size_t const interruptAssignmentCount = std::min(castingAdds.size(), interrupters.size());
    for (std::size_t index = 0; index < interruptAssignmentCount; ++index)
    {
        plan.assignments.push_back({interrupters[index]->guid, castingAdds[index]->guid,
            AssignmentKind::Interrupt, kPrimaryInterruptRank});
    }

    // A tank already victimized by an add is controlling that add and must not be pulled away to
    // acquire another. Off-tanks are used before the main tank; the main tank is available only
    // while no threat-backed boss currently has a victim.
    std::vector<RaidMember const*> tanks;
    for (RaidMember const& member : members)
    {
        if (!member.canRespond || member.isHealer || !member.isTank || member.ownsActiveBoss ||
            (member.isMainTank && bossHasVictim) || Detail::IsCurrentAddVictim(member.guid, adds) ||
            Detail::HasAssignment(plan, member.guid))
        {
            continue;
        }
        tanks.push_back(&member);
    }
    std::sort(tanks.begin(), tanks.end(), [](RaidMember const* left, RaidMember const* right) {
        return left->isMainTank != right->isMainTank ? !left->isMainTank : left->guid < right->guid;
    });

    std::size_t nextTank = 0;
    for (AddCandidate const& add : adds)
    {
        RaidMember const* victim = Detail::FindMember(members, add.victimGuid);
        // A victim outside the player roster is commonly a pet or totem. Do not peel a raid tank
        // off the boss for an add whose player ownership cannot be established.
        if (!victim || victim->isTank)
            continue;
        if (nextTank == tanks.size())
            break;
        plan.assignments.push_back({tanks[nextTank++]->guid, add.guid, AssignmentKind::TankPickup});
    }

    AddCandidate const& focus = adds.front();
    RaidMember const* directVictim = Detail::FindMember(members, focus.victimGuid);
    std::size_t dpsResponders = 0;
    if (directVictim && Detail::IsDpsResponder(*directVictim) &&
        !Detail::HasAssignment(plan, directVictim->guid))
    {
        plan.assignments.push_back({directVictim->guid, focus.guid, AssignmentKind::DpsFocus});
        ++dpsResponders;
    }

    for (RaidMember const& member : members)
    {
        if (dpsResponders == kMaxDpsResponders)
            break;
        if (!Detail::IsDpsResponder(member) || member.guid == plan.bossPressureReserveGuid ||
            (directVictim && member.guid == directVictim->guid) || Detail::HasAssignment(plan, member.guid))
        {
            continue;
        }
        plan.assignments.push_back({member.guid, focus.guid, AssignmentKind::DpsFocus});
        ++dpsResponders;
    }

    return plan;
}
}

#endif
