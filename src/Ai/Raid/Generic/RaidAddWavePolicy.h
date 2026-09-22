/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDADWAVEPOLICY_H
#define PLAYERBOTS_RAIDADWAVEPOLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace RaidAddWavePolicy
{
using Guid = std::uint64_t;

constexpr std::size_t kEntryAddCount = 6;
constexpr std::size_t kExitAddCount = 2;
constexpr std::size_t kMinimumBurnCluster = 3;
constexpr float kAoeRadius = 8.0f;
constexpr float kGatherStackDistance = 4.0f;
constexpr std::uint8_t kMaterialDangerPriority = 2;
// This is a deliberately conservative generic load bound. Encounter adapters may provide a
// tighter bound, but they must not silently raise it for a particular map or creature entry.
constexpr std::size_t kDefaultTankAddCapacity = 8;
constexpr float kTankAssignmentRadius = 45.0f;

enum class State : std::uint8_t
{
    Inactive,
    Gather,
    Burn
};

struct Member
{
    Guid guid = 0;
    bool canRespond = true;
    bool isDps = false;
    bool isTank = false;
    bool isHealer = false;
    bool ownsBoss = false;
    bool reservedForBossPressure = false;
    bool hasEmergencyClaim = false;
    bool safeForAdds = true;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::size_t currentAddLoad = 0;
    std::size_t addCapacity = kDefaultTankAddCapacity;
    // Per-bot RaidTargetClaim memory is imported by the runtime adapter when available. It is
    // only a preference; the policy still validates the add, tank, range, and capacity every pass.
    Guid stickyAddGuid = 0;
    // Informational policy input: a responder already kiting a loose add remains eligible so the
    // ordinary movement assignment can drag that add toward the safe-tank pack.
    bool hasLooseAddTarget = false;
};

struct Add
{
    Guid guid = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = true;
    bool ccMarked = false;
    bool controlledBySafeTank = false;
    Guid currentTankGuid = 0;
    // The runtime sets this only for observed hostile adds that are eligible for the generic tank
    // capacity gate. Keeping the default false preserves callers that only use the legacy cluster
    // policy and do not provide enough information to make a safe assignment.
    bool eligibleForTankAssignment = false;
    bool dangerousCast = false;
    std::uint8_t priority = 0;
    std::uint8_t dangerPriority = 0;
};

struct AddClaim
{
    std::uint64_t waveId = 0;
    Guid addGuid = 0;
    Guid tankGuid = 0;
};

struct Memory
{
    std::uint64_t waveId = 0;
    State state = State::Inactive;
    Guid anchorGuid = 0;
    Guid focusGuid = 0;
    Guid preemptGuid = 0;
    Guid replacementFromGuid = 0;
    std::vector<AddClaim> tankClaims;
};

struct Decision
{
    Memory memory;
    Guid responseTargetGuid = 0;
    std::size_t total = 0;
    std::size_t clustered = 0;
    std::size_t controlled = 0;
    bool enteredNewEpoch = false;
    bool focusChangedAfterInvalidation = false;
    bool temporaryPreemption = false;
    Guid gatherAnchorGuid = 0;
    std::size_t reportedTotal = 0;
    std::size_t eligibleAdds = 0;
    std::size_t unassignedAdds = 0;
    std::size_t overBudgetTanks = 0;
    bool addCapacityReady = true;
    std::vector<AddClaim> tankClaims;
    std::vector<Guid> stackResponders;
    std::vector<Guid> responders;
};

inline char const* StateName(State state)
{
    switch (state)
    {
        case State::Inactive: return "inactive";
        case State::Gather: return "gather";
        case State::Burn: return "burn";
    }
    return "unknown";
}

namespace Detail
{
inline float DistanceSquared(Add const& left, Add const& right)
{
    float const dx = left.x - right.x;
    float const dy = left.y - right.y;
    float const dz = left.z - right.z;
    return dx * dx + dy * dy + dz * dz;
}

inline float DistanceSquared(Add const& add, Member const& member)
{
    float const dx = add.x - member.x;
    float const dy = add.y - member.y;
    float const dz = add.z - member.z;
    return dx * dx + dy * dy + dz * dz;
}

inline Add const* FindAdd(std::vector<Add> const& adds, Guid guid)
{
    auto const found = std::lower_bound(adds.begin(), adds.end(), guid,
        [](Add const& add, Guid value) { return add.guid < value; });
    return found != adds.end() && found->guid == guid ? &*found : nullptr;
}

inline bool IsSafeTank(Member const& member)
{
    return member.guid && member.canRespond && member.isTank && member.safeForAdds && !member.isHealer &&
        !member.ownsBoss && !member.reservedForBossPressure && !member.hasEmergencyClaim && member.addCapacity;
}

inline AddClaim const* FindClaimForAdd(std::vector<AddClaim> const& claims, std::uint64_t waveId, Guid addGuid)
{
    auto const found = std::find_if(claims.begin(), claims.end(), [waveId, addGuid](AddClaim const& claim) {
        return claim.waveId == waveId && claim.addGuid == addGuid;
    });
    return found == claims.end() ? nullptr : &*found;
}

inline AddClaim const* FindClaimForTank(std::vector<AddClaim> const& claims, std::uint64_t waveId, Guid tankGuid,
    Guid addGuid = 0)
{
    auto const found = std::find_if(claims.begin(), claims.end(), [waveId, tankGuid, addGuid](AddClaim const& claim) {
        return claim.waveId == waveId && claim.tankGuid == tankGuid && (!addGuid || claim.addGuid == addGuid);
    });
    return found == claims.end() ? nullptr : &*found;
}

inline bool IsFocusEligible(Add const* add)
{
    return add && add->valid && !add->ccMarked;
}

inline bool IsResponder(Member const& member)
{
    return member.guid && member.canRespond && member.isDps && !member.isTank && !member.isHealer &&
        !member.ownsBoss && !member.reservedForBossPressure && !member.hasEmergencyClaim;
}
}

inline Decision Evaluate(Memory previous, std::vector<Member> members, std::vector<Add> adds,
    std::size_t reportedAddCount = 0)
{
    std::sort(members.begin(), members.end(), [](Member const& left, Member const& right) {
        return left.guid < right.guid;
    });
    std::sort(adds.begin(), adds.end(), [](Add const& left, Add const& right) { return left.guid < right.guid; });
    adds.erase(std::remove_if(adds.begin(), adds.end(), [](Add const& add) { return !add.guid || !add.valid; }),
        adds.end());
    adds.erase(std::unique(adds.begin(), adds.end(), [](Add const& left, Add const& right) {
        return left.guid == right.guid;
    }), adds.end());

    Decision decision;
    decision.memory = previous;
    decision.total = adds.size();
    decision.reportedTotal = std::max(reportedAddCount, decision.total);

    bool const wasActive = previous.state != State::Inactive;
    bool const shouldExit = wasActive && decision.total <= kExitAddCount;
    bool const shouldEnter = !wasActive && decision.total >= kEntryAddCount;
    if (shouldExit || (!wasActive && !shouldEnter))
    {
        decision.memory.state = State::Inactive;
        decision.memory.anchorGuid = 0;
        decision.memory.focusGuid = 0;
        decision.memory.preemptGuid = 0;
        decision.memory.replacementFromGuid = 0;
        decision.memory.tankClaims.clear();
        decision.tankClaims.clear();
        return decision;
    }

    if (shouldEnter)
    {
        ++decision.memory.waveId;
        if (!decision.memory.waveId)
            ++decision.memory.waveId;
        decision.memory.state = State::Gather;
        decision.memory.anchorGuid = 0;
        decision.memory.focusGuid = 0;
        decision.memory.preemptGuid = 0;
        decision.memory.replacementFromGuid = 0;
        decision.memory.tankClaims.clear();
        decision.enteredNewEpoch = true;
    }

    Add const* anchor = Detail::FindAdd(adds, decision.memory.anchorGuid);
    if (!anchor || anchor->ccMarked || !anchor->controlledBySafeTank)
    {
        anchor = nullptr;
        std::size_t bestCluster = 0;
        std::size_t bestControlled = 0;
        for (Add const& candidate : adds)
        {
            if (candidate.ccMarked || !candidate.controlledBySafeTank)
                continue;
            std::size_t cluster = 0;
            std::size_t controlled = 0;
            for (Add const& add : adds)
            {
                if (add.ccMarked || Detail::DistanceSquared(candidate, add) > kAoeRadius * kAoeRadius)
                    continue;
                ++cluster;
                if (add.controlledBySafeTank)
                    ++controlled;
            }
            if (!anchor || cluster > bestCluster || (cluster == bestCluster && controlled > bestControlled) ||
                (cluster == bestCluster && controlled == bestControlled && candidate.guid < anchor->guid))
            {
                anchor = &candidate;
                bestCluster = cluster;
                bestControlled = controlled;
            }
        }
        decision.memory.anchorGuid = anchor ? anchor->guid : 0;
    }

    if (anchor)
    {
        for (Add const& add : adds)
        {
            if (add.ccMarked || Detail::DistanceSquared(*anchor, add) > kAoeRadius * kAoeRadius)
                continue;
            ++decision.clustered;
            if (add.controlledBySafeTank)
                ++decision.controlled;
        }
    }

    bool const threatConsolidated = decision.controlled >= kMinimumBurnCluster &&
        decision.controlled * 2 >= decision.clustered;
    decision.memory.state = anchor && decision.clustered >= kMinimumBurnCluster && threatConsolidated ?
        State::Burn : State::Gather;

    struct TankState
    {
        Member const* member = nullptr;
        std::size_t load = 0;
    };

    std::vector<TankState> tanks;
    for (Member const& member : members)
    {
        if (!Detail::IsSafeTank(member))
            continue;

        tanks.push_back({&member, member.currentAddLoad});
        if (member.currentAddLoad > member.addCapacity)
            ++decision.overBudgetTanks;
    }

    auto findTank = [&tanks](Guid guid) -> TankState* {
        auto const found = std::find_if(tanks.begin(), tanks.end(), [guid](TankState const& tank) {
            return tank.member && tank.member->guid == guid;
        });
        return found == tanks.end() ? nullptr : &*found;
    };

    // The runtime cannot store an arbitrary vector in RaidTargetClaim, so it also supplies each
    // tank's current generic target as a sticky hint. Pure callers can carry the complete vector in
    // Memory; both paths are validated below before a claim is retained.
    std::vector<AddClaim> priorClaims = previous.tankClaims;
    if (!decision.enteredNewEpoch)
    {
        for (Member const& member : members)
        {
            if (!Detail::IsSafeTank(member) || !member.stickyAddGuid ||
                !Detail::FindAdd(adds, member.stickyAddGuid) ||
                Detail::FindClaimForAdd(priorClaims, decision.memory.waveId, member.stickyAddGuid))
            {
                continue;
            }
            priorClaims.push_back({decision.memory.waveId, member.stickyAddGuid, member.guid});
        }
    }

    for (Add const& add : adds)
    {
        if (add.ccMarked || !add.eligibleForTankAssignment)
            continue;

        ++decision.eligibleAdds;
        TankState* chosen = nullptr;
        bool currentOwner = false;

        if (add.currentTankGuid)
        {
            TankState* current = findTank(add.currentTankGuid);
            if (current && Detail::DistanceSquared(add, *current->member) <=
                    kTankAssignmentRadius * kTankAssignmentRadius &&
                current->load <= current->member->addCapacity)
            {
                chosen = current;
                currentOwner = true;
            }
        }

        if (!chosen)
        {
            AddClaim const* prior = Detail::FindClaimForAdd(priorClaims, decision.memory.waveId, add.guid);
            TankState* priorTank = prior ? findTank(prior->tankGuid) : nullptr;
            if (priorTank && prior->waveId == decision.memory.waveId &&
                Detail::DistanceSquared(add, *priorTank->member) <=
                    kTankAssignmentRadius * kTankAssignmentRadius &&
                priorTank->load < priorTank->member->addCapacity)
            {
                chosen = priorTank;
            }
        }

        if (!chosen)
        {
            for (TankState& candidate : tanks)
            {
                if (candidate.load >= candidate.member->addCapacity ||
                    Detail::DistanceSquared(add, *candidate.member) >
                        kTankAssignmentRadius * kTankAssignmentRadius)
                {
                    continue;
                }
                if (!chosen || candidate.load < chosen->load ||
                    (candidate.load == chosen->load && candidate.member->guid < chosen->member->guid))
                {
                    chosen = &candidate;
                }
            }
        }

        if (!chosen)
        {
            ++decision.unassignedAdds;
            continue;
        }

        decision.tankClaims.push_back({decision.memory.waveId, add.guid, chosen->member->guid});
        if (!currentOwner)
            ++chosen->load;
    }

    if (decision.reportedTotal > decision.total)
        decision.unassignedAdds += decision.reportedTotal - decision.total;
    decision.addCapacityReady = decision.overBudgetTanks == 0 && decision.unassignedAdds == 0;
    decision.memory.tankClaims = decision.tankClaims;
    if (decision.memory.state == State::Burn && !decision.addCapacityReady)
        decision.memory.state = State::Gather;

    Add const* focus = Detail::FindAdd(adds, decision.memory.focusGuid);
    if (!Detail::IsFocusEligible(focus))
    {
        Guid const lostPriorFocusGuid = decision.memory.focusGuid;
        focus = nullptr;
        for (Add const& candidate : adds)
        {
            if (candidate.ccMarked)
                continue;
            if (!focus || candidate.priority > focus->priority ||
                (candidate.priority == focus->priority && candidate.guid < focus->guid))
            {
                focus = &candidate;
            }
        }
        decision.memory.focusGuid = focus ? focus->guid : 0;
        decision.focusChangedAfterInvalidation = lostPriorFocusGuid && focus;
        if (decision.focusChangedAfterInvalidation)
            decision.memory.replacementFromGuid = lostPriorFocusGuid;
    }

    Add const* preempt = nullptr;
    for (Add const& candidate : adds)
    {
        if (candidate.ccMarked || candidate.guid == decision.memory.focusGuid || !candidate.dangerousCast ||
            candidate.dangerPriority < kMaterialDangerPriority)
        {
            continue;
        }
        if (!preempt || candidate.dangerPriority > preempt->dangerPriority ||
            (candidate.dangerPriority == preempt->dangerPriority && candidate.guid < preempt->guid))
        {
            preempt = &candidate;
        }
    }
    // Keep the previous preempt target until the caller observes that the persistent focus has
    // actually been reacquired. Trigger and action evaluation happen on separate scheduler passes.
    decision.memory.preemptGuid = preempt ? preempt->guid : previous.preemptGuid;
    decision.temporaryPreemption = preempt != nullptr;
    decision.responseTargetGuid = preempt ? preempt->guid : decision.memory.focusGuid;

    if (decision.memory.state == State::Gather && anchor)
        decision.gatherAnchorGuid = anchor->guid;

    if (decision.memory.state == State::Gather || decision.memory.state == State::Burn)
    {
        std::sort(members.begin(), members.end(), [](Member const& left, Member const& right) {
            return left.guid < right.guid;
        });
        for (Member const& member : members)
            if (Detail::IsResponder(member))
            {
                if (decision.memory.state == State::Gather && decision.gatherAnchorGuid)
                    decision.stackResponders.push_back(member.guid);
                else if (decision.memory.state == State::Burn)
                    decision.responders.push_back(member.guid);
            }
        decision.stackResponders.erase(
            std::unique(decision.stackResponders.begin(), decision.stackResponders.end()),
            decision.stackResponders.end());
        decision.responders.erase(
            std::unique(decision.responders.begin(), decision.responders.end()), decision.responders.end());
    }
    return decision;
}

inline Guid FindTankAssignment(Decision const& decision, Guid tankGuid, Guid currentTargetGuid = 0)
{
    AddClaim const* current = Detail::FindClaimForTank(
        decision.tankClaims, decision.memory.waveId, tankGuid, currentTargetGuid);
    if (current)
        return current->addGuid;

    AddClaim const* first = Detail::FindClaimForTank(decision.tankClaims, decision.memory.waveId, tankGuid);
    return first ? first->addGuid : 0;
}

inline bool MayReplaceTankTarget(Decision const& decision, Guid tankGuid, Guid currentTargetGuid,
    Guid requestedTargetGuid)
{
    if (!currentTargetGuid || currentTargetGuid == requestedTargetGuid)
        return true;

    // A still-valid claim is sticky. Once the current add disappears, is CC-marked, moves out of
    // safe capacity, or its tank becomes ineligible, it is absent from the new decision and may be
    // replaced by the deterministic next claim.
    return !Detail::FindClaimForTank(
        decision.tankClaims, decision.memory.waveId, tankGuid, currentTargetGuid);
}

inline bool IsStackResponder(Decision const& decision, Guid memberGuid)
{
    return decision.memory.state == State::Gather && decision.gatherAnchorGuid &&
        std::binary_search(decision.stackResponders.begin(), decision.stackResponders.end(), memberGuid);
}

inline bool IsResponder(Decision const& decision, Guid memberGuid)
{
    return std::binary_search(decision.responders.begin(), decision.responders.end(), memberGuid);
}

inline bool MayReplaceStickyTarget(Memory const& previous, Decision const& decision,
    Guid currentTargetGuid, Guid requestedTargetGuid)
{
    if (!currentTargetGuid || currentTargetGuid == requestedTargetGuid)
        return true;
    if (decision.enteredNewEpoch || decision.focusChangedAfterInvalidation)
        return true;
    if (decision.temporaryPreemption && requestedTargetGuid == decision.memory.preemptGuid)
        return true;
    return !decision.temporaryPreemption && requestedTargetGuid == decision.memory.focusGuid &&
        (previous.preemptGuid == currentTargetGuid || previous.replacementFromGuid == currentTargetGuid);
}
}

#endif
