/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef AUTOWOW_CAMPAIGN_TRAVEL_SESSION_H
#define AUTOWOW_CAMPAIGN_TRAVEL_SESSION_H

#include "CampaignTravelCatalog.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace AutoWowCampaignTravel
{
enum class Purpose : std::uint8_t
{
    Generic,
    QuestAcquire,
    QuestTurnIn
};

enum class SessionState : std::uint8_t
{
    Idle,
    Planning,
    ExecutingSelectedLeg,
    AwaitingExpectedObservation,
    Arrived,
    Blocked,
    Cancelled
};

enum class SessionBlocker : std::uint8_t
{
    None,
    InvalidIntent,
    CatalogRejected,
    PolicyRejected,
    PayloadMismatch,
    Timeout,
    UnexpectedMap,
    MissingRuntimeObject,
    HandlerRejected,
    BotUnavailable
};

struct CampaignTravelIntent
{
    std::uint64_t sequence = 0;
    Purpose purpose = Purpose::Generic;
    std::uint32_t questId = 0;
    std::int32_t targetEntry = 0;
    std::uint32_t targetSpawnId = 0;
    Endpoint finalDestination;
    std::uint32_t legTimeoutMs = 120000;
};

struct CampaignTravelObservation
{
    Endpoint position;
    bool inTaxiFlight = false;
    bool onTransport = false;
    std::uint32_t transportEntry = 0;
    std::uint32_t transportTaxiPathId = 0;
};

struct CampaignTravelSnapshot
{
    std::uint64_t sequence = 0;
    Purpose purpose = Purpose::Generic;
    SessionState state = SessionState::Idle;
    SessionBlocker blocker = SessionBlocker::None;
    CatalogIssue catalogIssue = CatalogIssue::None;
    Blocker policyBlocker = Blocker::None;
    std::uint64_t selectedStableId = 0;
    LegKind selectedKind = LegKind::Unknown;
    Endpoint observedPosition;
    bool taxiFlightObserved = false;
    bool transportBoardingObserved = false;
};

bool IsActive(SessionState state);
bool IsTerminal(SessionState state);

// The mailbox is the only cross-thread surface. Every method copies value objects; callers never
// receive a reference into its protected storage and the mutex is never held while world state is
// inspected or changed.
class CampaignTravelMailbox
{
public:
    bool Submit(CampaignTravelIntent const& intent);
    std::optional<CampaignTravelIntent> Consume();
    void Publish(CampaignTravelSnapshot const& snapshot);
    CampaignTravelSnapshot ReadSnapshot() const;
    bool HasWork() const;

private:
    mutable std::mutex mutex;
    std::optional<CampaignTravelIntent> pendingIntent;
    CampaignTravelSnapshot publishedSnapshot;
};

// Owned and mutated only by the bot's map/AI thread.
class CampaignTravelSession
{
public:
    void Begin(CampaignTravelIntent const& intent, std::uint32_t nowMs);
    bool Cancel(std::uint64_t sequence);
    bool Plan(CatalogResult const& catalog, std::uint32_t nowMs);
    bool MarkExecutionIssued(std::uint64_t stableId, std::uint32_t nowMs);
    void BlockRuntime(SessionBlocker blocker);
    void Observe(CampaignTravelObservation const& observation, std::uint32_t nowMs);

    CampaignTravelIntent const& GetIntent() const { return intent; }
    ExecutableLeg const* GetSelectedLeg() const;
    CampaignTravelSnapshot Snapshot() const;
    SessionState GetState() const { return state; }

private:
    void CompleteLeg(CampaignTravelObservation const& observation);
    void Block(SessionBlocker reason, CatalogIssue issue = CatalogIssue::None,
               AutoWowCampaignTravel::Blocker policyReason = AutoWowCampaignTravel::Blocker::None);
    bool TimedOut(std::uint32_t nowMs) const;

    CampaignTravelIntent intent;
    SessionState state = SessionState::Idle;
    SessionBlocker blocker = SessionBlocker::None;
    CatalogIssue catalogIssue = CatalogIssue::None;
    AutoWowCampaignTravel::Blocker policyBlocker = AutoWowCampaignTravel::Blocker::None;
    std::optional<ExecutableLeg> selectedLeg;
    Endpoint observedPosition;
    std::uint32_t legStartedMs = 0;
    bool taxiFlightObserved = false;
    bool transportBoardingObserved = false;
};
}  // namespace AutoWowCampaignTravel

#endif  // AUTOWOW_CAMPAIGN_TRAVEL_SESSION_H
