/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_GIVER_TRAVEL_LIFECYCLE_H
#define AUTOWOW_QUEST_GIVER_TRAVEL_LIFECYCLE_H

#include "QuestGiverTravelFeedback.h"
#include "QuestGiverTravelPolicy.h"

#include <cstdint>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace AutoWowQuestGiverTravel
{
inline GiverLookupState ClassifyGiverLookup(bool spawnDataExists, bool coarseGridLoaded,
                                            bool withinResolutionRange,
                                            bool liveObjectResolved)
{
    if (!spawnDataExists)
        return GiverLookupState::ConfirmedMissing;
    if (liveObjectResolved)
        return GiverLookupState::Resolved;
    if (!coarseGridLoaded || !withinResolutionRange)
        return GiverLookupState::GridUnloaded;
    return GiverLookupState::ConfirmedMissing;
}

enum class SessionMovementState : std::uint8_t
{
    Traveling,
    StaleGiver,
    LeaderParkedWaitingFollowers,
    CohesiveAtGiver,
    Terminal
};

inline std::string_view SessionMovementStateName(SessionMovementState state)
{
    switch (state)
    {
        case SessionMovementState::Traveling: return "traveling";
        case SessionMovementState::StaleGiver: return "stale_giver";
        case SessionMovementState::LeaderParkedWaitingFollowers:
            return "leader_parked_waiting_followers";
        case SessionMovementState::CohesiveAtGiver: return "cohesive_at_giver";
        case SessionMovementState::Terminal: return "terminal";
        default: return "terminal";
    }
}

struct SessionObservation
{
    std::uint32_t elapsedMs = 0;
    bool giverAvailable = true;
    bool leaderAtGiver = false;
    std::uint32_t expectedPartyMembers = 0;
    std::uint32_t arrivedPartyMembers = 0;
    bool allowAcceptance = false;
};

struct SessionDecision
{
    SessionMovementState state = SessionMovementState::Traveling;
    bool moveLeader = true;
    bool issueAcceptance = false;
    // Observation/status paths are never allowed to replace the installed target.
    bool replaceTarget = false;
};

class SessionLifecycle
{
public:
    SessionDecision Observe(SessionObservation const& observation)
    {
        SessionDecision giverDecision =
            ObserveGiverAvailability(observation.elapsedMs, observation.giverAvailable);
        if (!observation.giverAvailable || terminal_)
            return giverDecision;
        return ObserveParty(observation.elapsedMs, observation.leaderAtGiver,
                            observation.expectedPartyMembers, observation.arrivedPartyMembers,
                            observation.allowAcceptance);
    }

    SessionDecision ObserveGiverAvailability(std::uint32_t elapsedMs, bool giverAvailable)
    {
        ++observations_;
        if (terminal_)
            return {SessionMovementState::Terminal, false, false, false};

        if (!giverAvailable)
        {
            if (!staleGiverWaitStarted_)
            {
                staleGiverWaitStarted_ = true;
                staleGiverWaitStartedAt_ = elapsedMs;
            }
            partyArrivalWaitStarted_ = false;
            state_ = SessionMovementState::StaleGiver;
            return {state_, false, false, false};
        }

        staleGiverWaitStarted_ = false;
        if (state_ == SessionMovementState::StaleGiver)
            state_ = SessionMovementState::Traveling;
        return {state_, state_ == SessionMovementState::Traveling, false, false};
    }

    SessionDecision ObserveParty(std::uint32_t elapsedMs, bool leaderAtGiver,
                                 std::uint32_t expectedPartyMembers,
                                 std::uint32_t arrivedPartyMembers, bool allowAcceptance)
    {
        ++observations_;
        if (terminal_)
            return {SessionMovementState::Terminal, false, false, false};

        if (!leaderAtGiver)
        {
            partyArrivalWaitStarted_ = false;
            state_ = SessionMovementState::Traveling;
            return {state_, true, false, false};
        }

        if (!expectedPartyMembers || arrivedPartyMembers < expectedPartyMembers)
        {
            if (!partyArrivalWaitStarted_)
            {
                partyArrivalWaitStarted_ = true;
                partyArrivalWaitStartedAt_ = elapsedMs;
            }
            state_ = SessionMovementState::LeaderParkedWaitingFollowers;
            return {state_, false, false, false};
        }

        partyArrivalWaitStarted_ = false;
        state_ = SessionMovementState::CohesiveAtGiver;
        return {state_, false, allowAcceptance, false};
    }

    void MarkTerminal()
    {
        terminal_ = true;
        movementAccepted_ = false;
        state_ = SessionMovementState::Terminal;
    }

    void SetMovementAccepted(bool accepted)
    {
        movementAccepted_ = !terminal_ && accepted;
    }

    std::uint32_t StaleGiverWaitMs(std::uint32_t elapsedMs) const
    {
        return staleGiverWaitStarted_ && elapsedMs >= staleGiverWaitStartedAt_
            ? elapsedMs - staleGiverWaitStartedAt_
            : 0;
    }

    std::uint32_t PartyArrivalWaitMs(std::uint32_t elapsedMs) const
    {
        return partyArrivalWaitStarted_ && elapsedMs >= partyArrivalWaitStartedAt_
            ? elapsedMs - partyArrivalWaitStartedAt_
            : 0;
    }

    SessionMovementState State() const { return state_; }
    std::uint32_t Observations() const { return observations_; }
    bool IsTerminal() const { return terminal_; }
    bool MovementAccepted() const { return movementAccepted_; }

private:
    SessionMovementState state_ = SessionMovementState::Traveling;
    std::uint32_t observations_ = 0;
    bool terminal_ = false;
    bool movementAccepted_ = false;
    bool staleGiverWaitStarted_ = false;
    std::uint32_t staleGiverWaitStartedAt_ = 0;
    bool partyArrivalWaitStarted_ = false;
    std::uint32_t partyArrivalWaitStartedAt_ = 0;
};

// Production state seam shared by QuestAcquisitionSession and lifecycle tests. It owns the one
// creation-time deadline, installs that deadline into the real TravelTarget exactly once, consumes
// staged-mover outcomes, and emits the same terminal telemetry embedded by response and snapshot.
class QuestAcquisitionJourneyRuntime
{
public:
    QuestAcquisitionJourneyRuntime(std::uint64_t nowMs, double routeDistance, float movementSpeed)
        : deadline_(AbsoluteSessionDeadline::Create(nowMs, routeDistance, movementSpeed))
    {
    }

    template <typename TravelTargetT>
    bool InstallTarget(TravelTargetT& target, std::uint64_t nowMs)
    {
        if (targetInstalled_ || deadline_.IsExpired(nowMs))
            return false;
        std::uint32_t const remaining = deadline_.RemainingMs(nowMs);
        if (!remaining || !target.alignTimeLeft(remaining))
            return false;
        targetInstalled_ = true;
        nativeDeadlineAtMs_ = deadline_.DeadlineAtMs();
        nativeStatusDeadlineMs_ = target.getStatusDeadline();
        return true;
    }

    void ApplyStagedMoverOutcome(MovementFeedback const& feedback)
    {
        if (terminal_ || feedback.sequence <= movementSequence_)
            return;
        movementSequence_ = feedback.sequence;
        movementObservations_ = feedback.observations;
        movementAttempts_ = feedback.movementAttempts;
        movementRejects_ = feedback.rejectedAttempts;
        movementNoProgress_ = feedback.noProgressChecks;
        if (feedback.result == MovementFeedbackResult::Accepted)
        {
            movementAccepted_ = true;
            lifecycle_.SetMovementAccepted(true);
        }
        else if (feedback.result == MovementFeedbackResult::NoProgress ||
                 feedback.result == MovementFeedbackResult::Rejected ||
                 feedback.result == MovementFeedbackResult::Parked)
        {
            movementAccepted_ = false;
            lifecycle_.SetMovementAccepted(false);
        }
        // Deferred is an observation that the previous accepted step still owns the movement
        // reservation. Preserve its accepted state so a refresh cannot look like a failed route.
    }

    bool ExpireIfDue(std::uint64_t nowMs)
    {
        if (terminal_ || !deadline_.IsExpired(nowMs))
            return false;
        Block("acquisition_timeout");
        return true;
    }

    void Block(std::string reason)
    {
        terminal_ = true;
        blocked_ = true;
        terminalReason_ = std::move(reason);
        movementAccepted_ = false;
        lifecycle_.MarkTerminal();
    }

    void Accept(std::string reason)
    {
        terminal_ = true;
        blocked_ = false;
        terminalReason_ = std::move(reason);
        movementAccepted_ = false;
        lifecycle_.MarkTerminal();
    }

    std::string TelemetryJson(char const* surface) const
    {
        std::ostringstream out;
        out << "{\"surface\":\"" << surface << "\",\"state\":\""
            << (terminal_ ? (blocked_ ? "blocked" : "accepted") : "issued")
            << "\",\"reason\":\"" << terminalReason_
            << "\",\"movement_kick_accepted\":" << (movementAccepted_ ? "true" : "false")
            << ",\"movement_action_result\":\""
            << (terminal_ ? "not_attempted" : movementAccepted_ ? "accepted" : "rejected")
            << "\",\"movement_observations\":" << movementObservations_
            << ",\"movement_attempts\":" << movementAttempts_
            << ",\"movement_rejects\":" << movementRejects_
            << ",\"session_created_at_ms\":" << deadline_.CreatedAtMs()
            << ",\"session_deadline_at_ms\":" << deadline_.DeadlineAtMs()
            << ",\"session_duration_ms\":" << deadline_.DurationMs()
            << ",\"native_aligned_deadline_at_ms\":" << nativeDeadlineAtMs_ << '}';
        return out.str();
    }

    AbsoluteSessionDeadline const& Deadline() const { return deadline_; }
    SessionLifecycle& Lifecycle() { return lifecycle_; }
    SessionLifecycle const& Lifecycle() const { return lifecycle_; }
    std::uint64_t NativeDeadlineAtMs() const { return nativeDeadlineAtMs_; }
    std::uint32_t NativeStatusDeadlineMs() const { return nativeStatusDeadlineMs_; }
    std::uint32_t MovementAttempts() const { return movementAttempts_; }
    std::uint32_t MovementNoProgressChecks() const { return movementNoProgress_; }
    bool IsTerminal() const { return terminal_; }

private:
    AbsoluteSessionDeadline deadline_;
    SessionLifecycle lifecycle_;
    std::uint64_t nativeDeadlineAtMs_ = 0;
    std::uint64_t movementSequence_ = 0;
    std::uint32_t nativeStatusDeadlineMs_ = 0;
    std::uint32_t movementObservations_ = 0;
    std::uint32_t movementAttempts_ = 0;
    std::uint32_t movementRejects_ = 0;
    std::uint32_t movementNoProgress_ = 0;
    bool targetInstalled_ = false;
    bool movementAccepted_ = false;
    bool terminal_ = false;
    bool blocked_ = false;
    std::string terminalReason_ = "traveling_to_selected_giver";
};
}

#endif  // AUTOWOW_QUEST_GIVER_TRAVEL_LIFECYCLE_H
