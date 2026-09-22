/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONTRANSITIONPOLICY_H
#define PLAYERBOTS_DUNGEONTRANSITIONPOLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace DungeonTransitionPolicy
{
enum class AdmissionGuard
{
    Allowed,
    RetryableBlocked,
    PermanentlyBlocked
};

struct MemberFacts
{
    std::uint64_t memberId = 0;
    bool isLeader = false;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    AdmissionGuard guard = AdmissionGuard::Allowed;
};

struct PendingAttempt
{
    bool active = false;
    std::uint64_t memberId = 0;
    std::uint32_t targetMapId = 0;
    std::uint32_t targetInstanceId = 0;
    std::uint32_t attempts = 0;
    std::uint64_t retryAtMs = 0;
};

struct RetryPolicy
{
    std::uint32_t maxAttempts = 3;
    std::uint64_t initialBackoffMs = 2000;
    std::uint64_t maxBackoffMs = 8000;
};

struct Request
{
    std::vector<MemberFacts> members;
    std::uint32_t targetMapId = 0;
    std::uint32_t targetInstanceId = 0;
    std::uint64_t nowMs = 0;
    PendingAttempt pending;
    RetryPolicy retries;
};

enum class Status
{
    AdmitMember,
    AwaitingRetry,
    RetryableGuard,
    PermanentlyBlocked,
    RetryExhausted,
    SplitInstance,
    Complete,
    InvalidInput
};

struct Decision
{
    Status status = Status::InvalidInput;
    std::uint64_t memberId = 0;
    std::uint32_t attempt = 0;
    std::uint64_t retryAtMs = 0;
};

enum class PartyReadinessStage
{
    ExteriorPortal,
    LeaderInside
};

enum class MemberReadiness
{
    Proceed,
    OmitReleasedCorpse,
    BlockParty
};

struct MemberReadinessFacts
{
    bool online = false;
    bool alive = false;
    bool ready = false;
    bool released = false;
    bool inFlight = false;
    bool hasActualCorpse = false;
    std::uint32_t corpseMapId = 0;
    // Zero means the corpse's instance could not be determined.
    std::uint32_t corpseInstanceId = 0;
};

struct MemberReadinessContext
{
    PartyReadinessStage stage = PartyReadinessStage::ExteriorPortal;
    std::uint32_t portalTargetMapId = 0;
    std::uint32_t leaderMapId = 0;
    std::uint32_t expectedInstanceId = 0;
};

inline MemberReadiness EvaluateMemberReadiness(
    MemberReadinessFacts const& member, MemberReadinessContext const& context)
{
    if (member.alive)
    {
        return member.online && member.ready && !member.inFlight
            ? MemberReadiness::Proceed
            : MemberReadiness::BlockParty;
    }

    if (!member.online || !member.released || member.inFlight || !member.hasActualCorpse)
        return MemberReadiness::BlockParty;

    std::uint32_t const requiredCorpseMap = context.stage == PartyReadinessStage::ExteriorPortal
        ? context.portalTargetMapId
        : context.leaderMapId;
    if (!requiredCorpseMap || member.corpseMapId != requiredCorpseMap)
        return MemberReadiness::BlockParty;

    // Exterior staging has no authoritative target instance yet. Once inside, compare only when
    // both the leader's expected instance and the corpse's instance are available.
    bool const instanceDataAvailable = context.stage == PartyReadinessStage::LeaderInside &&
        context.expectedInstanceId && member.corpseInstanceId;
    if (instanceDataAvailable && member.corpseInstanceId != context.expectedInstanceId)
        return MemberReadiness::BlockParty;

    return MemberReadiness::OmitReleasedCorpse;
}

namespace Detail
{
inline bool IsAtTarget(MemberFacts const& member, Request const& request)
{
    return member.mapId == request.targetMapId && member.instanceId == request.targetInstanceId;
}

inline bool CanonicalOrder(MemberFacts const& left, MemberFacts const& right)
{
    if (left.isLeader != right.isLeader)
        return left.isLeader;
    return left.memberId < right.memberId;
}

inline std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right)
{
    std::uint64_t const maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

inline std::uint64_t RetryDelay(RetryPolicy const& retries, std::uint32_t attempt)
{
    std::uint64_t delay = std::min(retries.initialBackoffMs, retries.maxBackoffMs);
    for (std::uint32_t step = 1; step < attempt && delay < retries.maxBackoffMs; ++step)
    {
        if (delay > retries.maxBackoffMs - delay)
            return retries.maxBackoffMs;
        delay = std::min(delay + delay, retries.maxBackoffMs);
    }
    return delay;
}

inline Decision Admit(std::uint64_t memberId, std::uint32_t attempt, Request const& request)
{
    return {Status::AdmitMember, memberId, attempt,
        SaturatingAdd(request.nowMs, RetryDelay(request.retries, attempt))};
}
}

inline Decision Evaluate(Request const& request)
{
    if (!request.targetMapId || !request.targetInstanceId || request.members.empty() ||
        !request.retries.maxAttempts || !request.retries.initialBackoffMs || !request.retries.maxBackoffMs)
        return {};

    std::vector<MemberFacts> members = request.members;
    std::sort(members.begin(), members.end(), Detail::CanonicalOrder);

    std::uint32_t leaderCount = 0;
    std::set<std::uint64_t> memberIds;
    for (MemberFacts const& member : members)
    {
        if (!member.memberId || !memberIds.insert(member.memberId).second)
            return {};
        leaderCount += member.isLeader ? 1u : 0u;
    }
    if (leaderCount != 1)
        return {};

    if (request.pending.active)
    {
        if (!request.pending.memberId || !request.pending.attempts ||
            request.pending.targetMapId != request.targetMapId ||
            request.pending.targetInstanceId != request.targetInstanceId)
            return {};

        auto const pendingMember = std::find_if(members.begin(), members.end(), [&](MemberFacts const& member)
        {
            return member.memberId == request.pending.memberId;
        });
        if (pendingMember == members.end())
            return {};
    }

    for (MemberFacts const& member : members)
    {
        if (member.instanceId && !Detail::IsAtTarget(member, request))
            return {Status::SplitInstance, member.memberId, 0, 0};
    }

    auto const next = std::find_if(members.begin(), members.end(), [&](MemberFacts const& member)
    {
        return !Detail::IsAtTarget(member, request);
    });
    if (next == members.end())
        return {Status::Complete, 0, 0, 0};

    if (request.pending.active && request.pending.memberId != next->memberId)
    {
        auto const pendingMember = std::find_if(members.begin(), members.end(), [&](MemberFacts const& member)
        {
            return member.memberId == request.pending.memberId;
        });
        if (!Detail::IsAtTarget(*pendingMember, request))
            return {};
    }

    if (next->guard == AdmissionGuard::PermanentlyBlocked)
        return {Status::PermanentlyBlocked, next->memberId, 0, 0};
    if (next->guard == AdmissionGuard::RetryableBlocked)
        return {Status::RetryableGuard, next->memberId, 0,
            Detail::SaturatingAdd(request.nowMs, Detail::RetryDelay(request.retries, 1))};

    if (!request.pending.active || request.pending.memberId != next->memberId)
        return Detail::Admit(next->memberId, 1, request);
    if (request.pending.attempts >= request.retries.maxAttempts)
        return {Status::RetryExhausted, next->memberId, request.pending.attempts, 0};
    if (request.nowMs < request.pending.retryAtMs)
        return {Status::AwaitingRetry, next->memberId, request.pending.attempts, request.pending.retryAtMs};

    return Detail::Admit(next->memberId, request.pending.attempts + 1, request);
}
}

#endif
