/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_WORKERGATHERDEATHRECOVERYPOLICY_H
#define PLAYERBOTS_WORKERGATHERDEATHRECOVERYPOLICY_H

#include <cstdint>

namespace AutoWowGather
{
// This policy only decides when the existing player-facing release, corpse-route, and reclaim
// actions may be asked to run. It never performs movement, resurrects a player, repairs gear, or
// writes character state.
enum class DeathRecoveryPhase : std::uint8_t
{
    Inactive = 0,
    AwaitingRelease,
    ReleasePending,
    CorpseRoutePending,
    ReclaimPending,
    Recovered,
    Blocked
};

enum class DeathRecoveryAction : std::uint8_t
{
    None = 0,
    ReleaseSpirit,
    WalkToCorpse,
    ReclaimCorpse,
    Wait
};

enum class DeathRecoveryEvent : std::uint8_t
{
    None = 0,
    DeathDetected,
    ReleaseIssued,
    ReleaseWaiting,
    CorpseRouteIssued,
    CorpseRouteWaiting,
    CorpseNear,
    ReclaimIssued,
    ReclaimWaiting,
    Recovered,
    Blocked
};

enum class DeathRecoveryIdleReason : std::uint8_t
{
    NotDead = 0,
    AwaitingRelease,
    ReleaseBackoff,
    CorpseUnavailable,
    CorpseRouteInProgress,
    CorpseRouteBackoff,
    ReclaimBackoff,
    AttemptLimit
};

struct DeathRecoveryState
{
    bool active = false;
    DeathRecoveryPhase phase = DeathRecoveryPhase::Inactive;
    DeathRecoveryEvent event = DeathRecoveryEvent::None;
    DeathRecoveryIdleReason idleReason = DeathRecoveryIdleReason::NotDead;
    std::uint32_t releaseAttempts = 0;
    std::uint32_t routeAttempts = 0;
    std::uint32_t reclaimAttempts = 0;
    std::uint64_t startedAtSeconds = 0;
    std::uint64_t nextActionAtSeconds = 0;
    std::uint64_t eventSequence = 0;
};

struct DeathRecoveryObservation
{
    bool explicitWorker = false;
    bool alive = false;
    bool ghost = false;
    bool corpseAvailable = false;
    bool corpseNear = false;
    bool routeInProgress = false;
    std::uint64_t nowSeconds = 0;
};

struct DeathRecoveryTransition
{
    DeathRecoveryState next;
    DeathRecoveryAction action = DeathRecoveryAction::None;
    std::uint32_t delaySeconds = 0;
};

constexpr std::uint32_t MaxReleaseAttempts = 2;
constexpr std::uint32_t MaxCorpseRouteAttempts = 8;
constexpr std::uint32_t MaxReclaimAttempts = 4;
constexpr std::uint32_t ReleaseRetrySeconds = 3;
constexpr std::uint32_t CorpseRouteRetrySeconds = 15;
constexpr std::uint32_t ReclaimRetrySeconds = 20;
constexpr std::uint32_t MissingCorpseTimeoutSeconds = 60;

inline char const* DeathRecoveryPhaseName(DeathRecoveryPhase phase)
{
    switch (phase)
    {
        case DeathRecoveryPhase::AwaitingRelease: return "awaiting_release";
        case DeathRecoveryPhase::ReleasePending: return "release_pending";
        case DeathRecoveryPhase::CorpseRoutePending: return "corpse_route_pending";
        case DeathRecoveryPhase::ReclaimPending: return "reclaim_pending";
        case DeathRecoveryPhase::Recovered: return "recovered";
        case DeathRecoveryPhase::Blocked: return "blocked";
        case DeathRecoveryPhase::Inactive: default: return "inactive";
    }
}

inline char const* DeathRecoveryEventName(DeathRecoveryEvent event)
{
    switch (event)
    {
        case DeathRecoveryEvent::DeathDetected: return "worker_death_detected";
        case DeathRecoveryEvent::ReleaseIssued: return "worker_release_issued";
        case DeathRecoveryEvent::ReleaseWaiting: return "worker_release_waiting";
        case DeathRecoveryEvent::CorpseRouteIssued: return "worker_corpse_route_issued";
        case DeathRecoveryEvent::CorpseRouteWaiting: return "worker_corpse_route_waiting";
        case DeathRecoveryEvent::CorpseNear: return "worker_corpse_near";
        case DeathRecoveryEvent::ReclaimIssued: return "worker_reclaim_issued";
        case DeathRecoveryEvent::ReclaimWaiting: return "worker_reclaim_waiting";
        case DeathRecoveryEvent::Recovered: return "worker_death_recovered";
        case DeathRecoveryEvent::Blocked: return "worker_death_recovery_blocked";
        case DeathRecoveryEvent::None: default: return "none";
    }
}

inline char const* DeathRecoveryIdleReasonName(DeathRecoveryIdleReason reason)
{
    switch (reason)
    {
        case DeathRecoveryIdleReason::AwaitingRelease: return "awaiting_release";
        case DeathRecoveryIdleReason::ReleaseBackoff: return "release_backoff";
        case DeathRecoveryIdleReason::CorpseUnavailable: return "corpse_unavailable";
        case DeathRecoveryIdleReason::CorpseRouteInProgress: return "corpse_route_in_progress";
        case DeathRecoveryIdleReason::CorpseRouteBackoff: return "corpse_route_backoff";
        case DeathRecoveryIdleReason::ReclaimBackoff: return "reclaim_backoff";
        case DeathRecoveryIdleReason::AttemptLimit: return "attempt_limit";
        case DeathRecoveryIdleReason::NotDead: default: return "not_dead";
    }
}

namespace detail
{
inline bool IsDue(DeathRecoveryState const& state, std::uint64_t nowSeconds)
{
    return nowSeconds >= state.nextActionAtSeconds;
}

inline bool ElapsedAtLeast(DeathRecoveryState const& state, std::uint64_t nowSeconds,
                           std::uint32_t seconds)
{
    return nowSeconds >= state.startedAtSeconds &&
        nowSeconds - state.startedAtSeconds >= seconds;
}

inline void SetEvent(DeathRecoveryState& state, DeathRecoveryEvent event,
                     DeathRecoveryIdleReason idleReason)
{
    if (state.event != event || state.idleReason != idleReason)
        ++state.eventSequence;
    state.event = event;
    state.idleReason = idleReason;
}

inline DeathRecoveryTransition Wait(DeathRecoveryState state, DeathRecoveryEvent event,
                                    DeathRecoveryIdleReason reason, std::uint32_t delaySeconds)
{
    SetEvent(state, event, reason);
    DeathRecoveryTransition transition;
    transition.next = state;
    transition.action = DeathRecoveryAction::Wait;
    transition.delaySeconds = delaySeconds;
    return transition;
}

inline DeathRecoveryTransition Block(DeathRecoveryState state)
{
    state.active = true;
    state.phase = DeathRecoveryPhase::Blocked;
    state.nextActionAtSeconds = 0;
    SetEvent(state, DeathRecoveryEvent::Blocked, DeathRecoveryIdleReason::AttemptLimit);
    DeathRecoveryTransition transition;
    transition.next = state;
    transition.action = DeathRecoveryAction::Wait;
    transition.delaySeconds = CorpseRouteRetrySeconds;
    return transition;
}
}

// Pure, deterministic recovery planner. The returned state reserves at most one native action
// for the current observation, which makes repeated AI ticks idempotent. The caller must assign
// transition.next before invoking the existing playerbot action named by transition.action.
inline DeathRecoveryTransition EvaluateDeathRecovery(
    DeathRecoveryState const& current, DeathRecoveryObservation const& observation)
{
    DeathRecoveryTransition transition;
    transition.next = current;

    if (!observation.explicitWorker)
        return transition;

    if (observation.alive)
    {
        if (current.active && current.phase != DeathRecoveryPhase::Recovered)
        {
            transition.next.active = false;
            transition.next.phase = DeathRecoveryPhase::Recovered;
            transition.next.nextActionAtSeconds = 0;
            detail::SetEvent(transition.next, DeathRecoveryEvent::Recovered,
                             DeathRecoveryIdleReason::NotDead);
        }
        return transition;
    }

    if (!transition.next.active || transition.next.phase == DeathRecoveryPhase::Recovered)
    {
        std::uint64_t const previousEventSequence = transition.next.eventSequence;
        transition.next = DeathRecoveryState{};
        transition.next.eventSequence = previousEventSequence;
        transition.next.active = true;
        transition.next.startedAtSeconds = observation.nowSeconds;
        transition.next.nextActionAtSeconds = observation.nowSeconds;
        transition.next.phase = observation.ghost ? DeathRecoveryPhase::CorpseRoutePending
                                                  : DeathRecoveryPhase::AwaitingRelease;
        detail::SetEvent(transition.next, DeathRecoveryEvent::DeathDetected,
                         observation.ghost ? DeathRecoveryIdleReason::CorpseRouteBackoff
                                            : DeathRecoveryIdleReason::AwaitingRelease);
    }

    if (!observation.ghost)
    {
        transition.next.phase = DeathRecoveryPhase::AwaitingRelease;
        if (transition.next.releaseAttempts >= MaxReleaseAttempts)
            return detail::Block(transition.next);

        if (!detail::IsDue(transition.next, observation.nowSeconds))
            return detail::Wait(transition.next, DeathRecoveryEvent::ReleaseWaiting,
                                DeathRecoveryIdleReason::ReleaseBackoff, ReleaseRetrySeconds);

        ++transition.next.releaseAttempts;
        transition.next.phase = DeathRecoveryPhase::ReleasePending;
        transition.next.nextActionAtSeconds = observation.nowSeconds + ReleaseRetrySeconds;
        detail::SetEvent(transition.next, DeathRecoveryEvent::ReleaseIssued,
                         DeathRecoveryIdleReason::AwaitingRelease);
        transition.action = DeathRecoveryAction::ReleaseSpirit;
        return transition;
    }

    if (!observation.corpseAvailable)
    {
        if (detail::ElapsedAtLeast(transition.next, observation.nowSeconds,
                                   MissingCorpseTimeoutSeconds))
            return detail::Block(transition.next);

        return detail::Wait(transition.next, DeathRecoveryEvent::CorpseRouteWaiting,
                            DeathRecoveryIdleReason::CorpseUnavailable, 1);
    }

    // A release observation or a completed corpse walk opens the next phase immediately. The
    // backoff belongs to the action that was just issued, not to a later phase transition.
    if (transition.next.phase == DeathRecoveryPhase::ReleasePending ||
        transition.next.phase == DeathRecoveryPhase::AwaitingRelease)
    {
        transition.next.phase = DeathRecoveryPhase::CorpseRoutePending;
        transition.next.nextActionAtSeconds = observation.nowSeconds;
    }

    if (observation.corpseNear)
    {
        if (transition.next.phase != DeathRecoveryPhase::ReclaimPending)
            transition.next.nextActionAtSeconds = observation.nowSeconds;
        transition.next.phase = DeathRecoveryPhase::ReclaimPending;
        if (transition.next.reclaimAttempts >= MaxReclaimAttempts)
            return detail::Block(transition.next);

        if (!detail::IsDue(transition.next, observation.nowSeconds))
            return detail::Wait(transition.next, DeathRecoveryEvent::ReclaimWaiting,
                                DeathRecoveryIdleReason::ReclaimBackoff, ReclaimRetrySeconds);

        ++transition.next.reclaimAttempts;
        transition.next.nextActionAtSeconds = observation.nowSeconds + ReclaimRetrySeconds;
        detail::SetEvent(transition.next, DeathRecoveryEvent::ReclaimIssued,
                         DeathRecoveryIdleReason::ReclaimBackoff);
        transition.action = DeathRecoveryAction::ReclaimCorpse;
        return transition;
    }

    transition.next.phase = DeathRecoveryPhase::CorpseRoutePending;
    if (observation.routeInProgress)
        return detail::Wait(transition.next, DeathRecoveryEvent::CorpseRouteWaiting,
                            DeathRecoveryIdleReason::CorpseRouteInProgress, 2);

    if (transition.next.routeAttempts >= MaxCorpseRouteAttempts)
        return detail::Block(transition.next);

    if (!detail::IsDue(transition.next, observation.nowSeconds))
        return detail::Wait(transition.next, DeathRecoveryEvent::CorpseRouteWaiting,
                            DeathRecoveryIdleReason::CorpseRouteBackoff, CorpseRouteRetrySeconds);

    ++transition.next.routeAttempts;
    transition.next.nextActionAtSeconds = observation.nowSeconds + CorpseRouteRetrySeconds;
    detail::SetEvent(transition.next, DeathRecoveryEvent::CorpseRouteIssued,
                     DeathRecoveryIdleReason::CorpseRouteBackoff);
    transition.action = DeathRecoveryAction::WalkToCorpse;
    return transition;
}
}

#endif
