/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CORPSE_ROUTE_RETRY_POLICY_H
#define PLAYERBOTS_CORPSE_ROUTE_RETRY_POLICY_H

#include <algorithm>
#include <cstdint>

namespace CorpseRouteRetryPolicy
{
inline constexpr std::uint32_t MaxRouteFailures = 8;
inline constexpr std::uint32_t WaitingRetryDelayMs = 1000;
inline constexpr std::uint32_t BlockedRetryDelayMs = 30000;

struct DeathIdentity
{
    std::uint32_t corpseGuid = 0;
    std::int64_t ghostTime = 0;

    constexpr bool operator==(DeathIdentity const&) const = default;
};

struct RouteEndpoint
{
    std::uint32_t graveId = 0;
    std::uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] constexpr bool IsValid() const { return graveId != 0; }
    [[nodiscard]] constexpr bool IsLocalTo(std::uint32_t currentMapId) const
    {
        return IsValid() && mapId == currentMapId;
    }
};

enum class MovementObservation
{
    Ready,
    Duplicate,
    InProgress,
    Waiting,
    RouteStarted,
    RouteFailure,
    RecoverySucceeded
};

struct Transition
{
    bool blocked = false;
    bool emitTerminalReceipt = false;
    bool recoverySucceeded = false;
};

// Keep MoveTo's non-route false returns out of failure accounting. A call made while movement is
// restricted is waiting for the restriction to clear, just as duplicate and last-move delay gates
// are waiting for an already accepted movement request.
[[nodiscard]] constexpr MovementObservation ClassifyPreflight(bool movementAllowed, bool duplicateMove,
                                                              bool waitingForLastMove, bool isMoving)
{
    if (duplicateMove)
        return MovementObservation::Duplicate;
    if (!movementAllowed || waitingForLastMove)
        return MovementObservation::Waiting;
    if (isMoving)
        return MovementObservation::InProgress;
    return MovementObservation::Ready;
}

[[nodiscard]] constexpr std::uint32_t RetryDelayMs(std::uint32_t routeFailures)
{
    return std::max<std::uint32_t>(1, std::min(routeFailures, MaxRouteFailures)) * WaitingRetryDelayMs;
}

class State
{
public:
    // Returns true only when a new death starts. Repeated calls for the same corpse intentionally
    // ignore a different candidate endpoint: the first local grave remains pinned for this death.
    bool BeginDeath(DeathIdentity death, RouteEndpoint localGrave)
    {
        if (active_ && death_ == death)
            return false;

        Reset();
        active_ = true;
        death_ = death;
        endpoint_ = localGrave;
        return true;
    }

    Transition Observe(MovementObservation observation)
    {
        if (!active_)
            return {};
        if (blocked_)
            return {true, false, false};
        if (completed_)
            return {false, false, true};

        switch (observation)
        {
            case MovementObservation::RouteStarted:
                ++routeAttempts_;
                routeInProgress_ = true;
                break;
            case MovementObservation::RouteFailure:
                // A rejected submission is both an attempt and a failure. A route that was
                // accepted earlier already consumed its attempt when RouteStarted was observed;
                // discovering that it stopped short only consumes the matching failure here.
                if (!routeInProgress_)
                    ++routeAttempts_;
                ++routeFailures_;
                routeInProgress_ = false;
                if (routeFailures_ >= MaxRouteFailures)
                {
                    blocked_ = true;
                    bool const emit = !terminalReceiptEmitted_;
                    terminalReceiptEmitted_ = true;
                    return {true, emit, false};
                }
                break;
            case MovementObservation::RecoverySucceeded:
                completed_ = true;
                routeInProgress_ = false;
                return {false, false, true};
            case MovementObservation::Duplicate:
            case MovementObservation::InProgress:
            case MovementObservation::Waiting:
            case MovementObservation::Ready:
                break;
        }

        return {};
    }

    void Reset()
    {
        active_ = false;
        blocked_ = false;
        completed_ = false;
        routeInProgress_ = false;
        terminalReceiptEmitted_ = false;
        death_ = {};
        endpoint_ = {};
        routeAttempts_ = 0;
        routeFailures_ = 0;
    }

    [[nodiscard]] bool IsActive() const { return active_; }
    [[nodiscard]] bool IsBlocked() const { return blocked_; }
    [[nodiscard]] bool IsComplete() const { return completed_; }
    [[nodiscard]] bool IsRouteInProgress() const { return routeInProgress_; }
    [[nodiscard]] bool HasEmittedTerminalReceipt() const { return terminalReceiptEmitted_; }
    [[nodiscard]] RouteEndpoint const& Endpoint() const { return endpoint_; }
    [[nodiscard]] std::uint32_t RouteAttempts() const { return routeAttempts_; }
    [[nodiscard]] std::uint32_t RouteFailures() const { return routeFailures_; }

private:
    bool active_ = false;
    bool blocked_ = false;
    bool completed_ = false;
    bool routeInProgress_ = false;
    bool terminalReceiptEmitted_ = false;
    DeathIdentity death_;
    RouteEndpoint endpoint_;
    std::uint32_t routeAttempts_ = 0;
    std::uint32_t routeFailures_ = 0;
};
}

#endif
