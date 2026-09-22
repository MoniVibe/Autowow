/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "CampaignTravelSession.h"

#include <algorithm>
#include <set>

namespace AutoWowCampaignTravel
{
bool IsActive(SessionState state)
{
    return state == SessionState::Planning || state == SessionState::ExecutingSelectedLeg ||
        state == SessionState::AwaitingExpectedObservation;
}

bool IsTerminal(SessionState state)
{
    return state == SessionState::Arrived || state == SessionState::Blocked ||
        state == SessionState::Cancelled;
}

bool CampaignTravelMailbox::Submit(CampaignTravelIntent const& intent)
{
    if (intent.sequence == 0)
        return false;

    std::lock_guard<std::mutex> guard(mutex);
    if (pendingIntent && pendingIntent->sequence > intent.sequence)
        return false;
    if (!pendingIntent && publishedSnapshot.sequence > intent.sequence)
        return false;
    pendingIntent = intent;
    return true;
}

std::optional<CampaignTravelIntent> CampaignTravelMailbox::Consume()
{
    std::lock_guard<std::mutex> guard(mutex);
    std::optional<CampaignTravelIntent> copy = pendingIntent;
    pendingIntent.reset();
    return copy;
}

void CampaignTravelMailbox::Publish(CampaignTravelSnapshot const& snapshot)
{
    std::lock_guard<std::mutex> guard(mutex);
    if (snapshot.sequence >= publishedSnapshot.sequence)
        publishedSnapshot = snapshot;
}

CampaignTravelSnapshot CampaignTravelMailbox::ReadSnapshot() const
{
    std::lock_guard<std::mutex> guard(mutex);
    return publishedSnapshot;
}

bool CampaignTravelMailbox::HasWork() const
{
    std::lock_guard<std::mutex> guard(mutex);
    return pendingIntent.has_value() || IsActive(publishedSnapshot.state);
}

void CampaignTravelSession::Begin(CampaignTravelIntent const& newIntent, std::uint32_t /*nowMs*/)
{
    intent = newIntent;
    selectedLeg.reset();
    observedPosition = {};
    legStartedMs = 0;
    taxiFlightObserved = false;
    transportBoardingObserved = false;
    blocker = SessionBlocker::None;
    catalogIssue = CatalogIssue::None;
    policyBlocker = AutoWowCampaignTravel::Blocker::None;

    if (intent.sequence == 0 || intent.legTimeoutMs == 0 ||
        !IsWithin(intent.finalDestination, intent.finalDestination, 0.0))
    {
        Block(SessionBlocker::InvalidIntent);
        return;
    }

    state = SessionState::Planning;
}

bool CampaignTravelSession::Cancel(std::uint64_t sequence)
{
    if (sequence == 0 || sequence != intent.sequence || !IsActive(state))
        return false;
    selectedLeg.reset();
    state = SessionState::Cancelled;
    blocker = SessionBlocker::None;
    return true;
}

bool CampaignTravelSession::Plan(CatalogResult const& catalog, std::uint32_t nowMs)
{
    if (state != SessionState::Planning)
        return false;
    if (catalog.issue != CatalogIssue::None)
    {
        Block(SessionBlocker::CatalogRejected, catalog.issue);
        return false;
    }

    std::set<std::uint64_t> identities;
    for (ExecutableLeg const& leg : catalog.executableLegs)
    {
        if (!identities.insert(leg.policyFacts.stableId).second || !ValidateExecutableLeg(leg))
        {
            Block(SessionBlocker::PayloadMismatch, CatalogIssue::PayloadMismatch);
            return false;
        }
    }

    Decision const decision = PlanNextTransition(catalog.request);
    if (decision.kind != DecisionKind::Transition)
    {
        Block(SessionBlocker::PolicyRejected, CatalogIssue::None, decision.blocker);
        return false;
    }

    auto const match = std::find_if(
        catalog.executableLegs.begin(), catalog.executableLegs.end(),
        [&decision](ExecutableLeg const& leg)
        {
            return leg.policyFacts.stableId == decision.transition.stableId &&
                leg.policyFacts.kind == decision.transition.kind;
        });
    if (match == catalog.executableLegs.end())
    {
        Block(SessionBlocker::PayloadMismatch, CatalogIssue::PayloadMismatch);
        return false;
    }

    selectedLeg = *match;
    legStartedMs = nowMs;
    taxiFlightObserved = false;
    transportBoardingObserved = false;
    state = SessionState::ExecutingSelectedLeg;
    return true;
}

bool CampaignTravelSession::MarkExecutionIssued(std::uint64_t stableId, std::uint32_t nowMs)
{
    if (state != SessionState::ExecutingSelectedLeg || !selectedLeg ||
        selectedLeg->policyFacts.stableId != stableId)
        return false;
    state = SessionState::AwaitingExpectedObservation;
    if (legStartedMs == 0)
        legStartedMs = nowMs;
    return true;
}

void CampaignTravelSession::BlockRuntime(SessionBlocker reason)
{
    if (reason == SessionBlocker::None)
        reason = SessionBlocker::HandlerRejected;
    Block(reason);
}

void CampaignTravelSession::Observe(CampaignTravelObservation const& observation, std::uint32_t nowMs)
{
    if (!IsActive(state))
        return;

    observedPosition = observation.position;
    if (IsWithin(observation.position, intent.finalDestination, kDefaultArrivalRadius))
    {
        selectedLeg.reset();
        state = SessionState::Arrived;
        return;
    }

    if (!selectedLeg)
        return;
    if (TimedOut(nowMs))
    {
        Block(SessionBlocker::Timeout);
        return;
    }

    CandidateLeg const& facts = selectedLeg->policyFacts;
    if (observation.position.mapId != facts.source.mapId &&
        observation.position.mapId != facts.destination.mapId)
    {
        Block(SessionBlocker::UnexpectedMap);
        return;
    }

    switch (facts.kind)
    {
        case LegKind::WalkingApproach:
            if (IsWithin(observation.position, facts.destination, kDefaultArrivalRadius))
                CompleteLeg(observation);
            break;
        case LegKind::AreaTriggerPortal:
        case LegKind::GameObjectPortal:
            if (state == SessionState::AwaitingExpectedObservation &&
                IsWithin(observation.position, facts.destination, kDefaultArrivalRadius))
                CompleteLeg(observation);
            break;
        case LegKind::Taxi:
            taxiFlightObserved = taxiFlightObserved || observation.inTaxiFlight;
            if (state == SessionState::AwaitingExpectedObservation && taxiFlightObserved &&
                !observation.inTaxiFlight &&
                IsWithin(observation.position, facts.destination, kDefaultArrivalRadius))
                CompleteLeg(observation);
            break;
        case LegKind::Transport:
        {
            auto const* payload = std::get_if<TransportPayload>(&selectedLeg->payload);
            bool const matchingTransport = payload && observation.onTransport &&
                observation.transportEntry == payload->entry &&
                observation.transportTaxiPathId == payload->taxiPathId;
            transportBoardingObserved = transportBoardingObserved || matchingTransport;
            if (state == SessionState::AwaitingExpectedObservation && transportBoardingObserved &&
                !observation.onTransport &&
                IsWithin(observation.position, facts.destination, kDefaultArrivalRadius))
                CompleteLeg(observation);
            break;
        }
        default:
            Block(SessionBlocker::PayloadMismatch, CatalogIssue::PayloadMismatch);
            break;
    }
}

ExecutableLeg const* CampaignTravelSession::GetSelectedLeg() const
{
    return selectedLeg ? &*selectedLeg : nullptr;
}

CampaignTravelSnapshot CampaignTravelSession::Snapshot() const
{
    CampaignTravelSnapshot snapshot;
    snapshot.sequence = intent.sequence;
    snapshot.purpose = intent.purpose;
    snapshot.state = state;
    snapshot.blocker = blocker;
    snapshot.catalogIssue = catalogIssue;
    snapshot.policyBlocker = policyBlocker;
    snapshot.observedPosition = observedPosition;
    snapshot.taxiFlightObserved = taxiFlightObserved;
    snapshot.transportBoardingObserved = transportBoardingObserved;
    if (selectedLeg)
    {
        snapshot.selectedStableId = selectedLeg->policyFacts.stableId;
        snapshot.selectedKind = selectedLeg->policyFacts.kind;
    }
    return snapshot;
}

void CampaignTravelSession::CompleteLeg(CampaignTravelObservation const& observation)
{
    observedPosition = observation.position;
    selectedLeg.reset();
    taxiFlightObserved = false;
    transportBoardingObserved = false;
    legStartedMs = 0;
    state = IsWithin(observation.position, intent.finalDestination, kDefaultArrivalRadius) ?
        SessionState::Arrived : SessionState::Planning;
}

void CampaignTravelSession::Block(SessionBlocker reason, CatalogIssue issue,
                                  AutoWowCampaignTravel::Blocker policyReason)
{
    selectedLeg.reset();
    state = SessionState::Blocked;
    blocker = reason;
    catalogIssue = issue;
    policyBlocker = policyReason;
}

bool CampaignTravelSession::TimedOut(std::uint32_t nowMs) const
{
    return selectedLeg && static_cast<std::uint32_t>(nowMs - legStartedMs) >= intent.legTimeoutMs;
}
}  // namespace AutoWowCampaignTravel
