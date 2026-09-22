/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CampaignTravelPolicy.h"

#include <cmath>
#include <tuple>

namespace AutoWowCampaignTravel
{
namespace
{
bool ValidCoordinates(Coordinates const& coordinates)
{
    return std::isfinite(coordinates.x) && std::isfinite(coordinates.y) &&
        std::isfinite(coordinates.z) && std::fabs(coordinates.x) <= kMaximumAbsCoordinate &&
        std::fabs(coordinates.y) <= kMaximumAbsCoordinate &&
        std::fabs(coordinates.z) <= kMaximumAbsCoordinate;
}

bool SameCoordinates(Coordinates const& left, Coordinates const& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

Blocker ValidateDirectedEdge(CandidateLeg const& candidate)
{
    DirectedEdgeProof const& proof = candidate.directedEdge;
    if (!proof.validated)
        return Blocker::DirectedEdgeProofMissing;

    if (proof.sourceMapId != candidate.source.mapId ||
        proof.destinationMapId != candidate.destination.mapId ||
        !SameCoordinates(proof.sourceCoordinates, candidate.source.coordinates) ||
        !SameCoordinates(proof.destinationCoordinates, candidate.destination.coordinates))
        return Blocker::DirectedEdgeProofMismatch;

    return Blocker::None;
}

Blocker ValidateCandidate(CandidateLeg const& candidate, Request const& request)
{
    if (!candidate.source.mapValidated || !candidate.destination.mapValidated ||
        candidate.source.mapId == kInvalidMapId || candidate.destination.mapId == kInvalidMapId)
        return Blocker::InvalidCandidateMap;
    if (!ValidCoordinates(candidate.source.coordinates) ||
        !ValidCoordinates(candidate.destination.coordinates))
        return Blocker::InvalidCandidateCoordinates;
    if (candidate.source.mapId != request.source.mapId)
        return Blocker::CandidateSourceMapMismatch;
    if (candidate.cheats.seedGold)
        return Blocker::SeedGoldRejected;
    if (candidate.cheats.travelCheat)
        return Blocker::CheatRejected;

    switch (candidate.kind)
    {
        case LegKind::WalkingApproach:
            return candidate.source.mapId == candidate.destination.mapId ?
                Blocker::None : Blocker::CrossMapWalkingRejected;
        case LegKind::AreaTriggerPortal:
        case LegKind::GameObjectPortal:
            if (!candidate.normalTransitionValidated)
                return Blocker::PortalNotValidated;
            return ValidateDirectedEdge(candidate);
        case LegKind::Transport:
            if (!candidate.realDynamicTransport)
                return Blocker::TransportNotDynamic;
            return ValidateDirectedEdge(candidate);
        case LegKind::Taxi:
        {
            Blocker const edgeBlocker = ValidateDirectedEdge(candidate);
            if (edgeBlocker != Blocker::None)
                return edgeBlocker;
            if (!candidate.allTaxiNodesKnown)
                return Blocker::UnknownTaxiNodes;
            if (request.botMoney < candidate.cost)
                return Blocker::InsufficientMoney;
            return Blocker::None;
        }
        case LegKind::Teleport:
            return Blocker::TeleportRejected;
        case LegKind::Hearth:
            return Blocker::HearthRejected;
        case LegKind::Unknown:
            return Blocker::UnknownLegKind;
    }

    return Blocker::UnknownLegKind;
}

std::uint8_t KindRank(LegKind kind)
{
    switch (kind)
    {
        case LegKind::WalkingApproach:
            return 0;
        case LegKind::AreaTriggerPortal:
            return 1;
        case LegKind::GameObjectPortal:
            return 2;
        case LegKind::Transport:
            return 3;
        case LegKind::Taxi:
            return 4;
        case LegKind::Teleport:
            return 5;
        case LegKind::Hearth:
            return 6;
        case LegKind::Unknown:
            return 7;
    }

    return 7;
}

auto SelectionKey(CandidateLeg const& candidate)
{
    return std::tuple(
        candidate.cost, KindRank(candidate.kind), candidate.stableId,
        candidate.source.mapId, candidate.destination.mapId,
        candidate.source.coordinates.x, candidate.source.coordinates.y,
        candidate.source.coordinates.z, candidate.destination.coordinates.x,
        candidate.destination.coordinates.y, candidate.destination.coordinates.z);
}
}  // namespace

Decision PlanNextTransition(Request const& request)
{
    if (!request.source.mapValidated || request.source.mapId == kInvalidMapId)
        return {DecisionKind::Blocked, Blocker::InvalidRequestSourceMap, {}, false};
    if (!ValidCoordinates(request.source.coordinates))
        return {DecisionKind::Blocked, Blocker::InvalidRequestSourceCoordinates, {}, false};
    if (request.candidates.empty())
        return {DecisionKind::Blocked, Blocker::NoCandidateLegs, {}, false};

    CandidateLeg const* selected = nullptr;
    Blocker selectedBlocker = Blocker::None;
    for (CandidateLeg const& candidate : request.candidates)
    {
        Blocker const blocker = ValidateCandidate(candidate, request);
        if (blocker != Blocker::None)
        {
            if (selectedBlocker == Blocker::None || blocker < selectedBlocker)
                selectedBlocker = blocker;
            continue;
        }

        if (!selected || SelectionKey(candidate) < SelectionKey(*selected))
            selected = &candidate;
    }

    if (!selected)
        return {DecisionKind::Blocked, selectedBlocker, {}, false};

    return {DecisionKind::Transition, Blocker::None, *selected,
            selected->kind == LegKind::Transport};
}
}  // namespace AutoWowCampaignTravel
