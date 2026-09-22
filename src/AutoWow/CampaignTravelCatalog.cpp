/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "CampaignTravelCatalog.h"

#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace AutoWowCampaignTravel
{
namespace
{
bool ValidEndpoint(Endpoint const& endpoint)
{
    Coordinates const& c = endpoint.coordinates;
    return endpoint.mapValidated && endpoint.mapId != kInvalidMapId && std::isfinite(c.x) &&
        std::isfinite(c.y) && std::isfinite(c.z) && std::fabs(c.x) <= kMaximumAbsCoordinate &&
        std::fabs(c.y) <= kMaximumAbsCoordinate && std::fabs(c.z) <= kMaximumAbsCoordinate;
}

double RadiusOrDefault(double radius)
{
    return std::isfinite(radius) && radius > 0.0 ? radius : kDefaultArrivalRadius;
}

std::uint64_t DistanceCost(Endpoint const& source, Endpoint const& destination)
{
    double const dx = source.coordinates.x - destination.coordinates.x;
    double const dy = source.coordinates.y - destination.coordinates.y;
    double const dz = source.coordinates.z - destination.coordinates.z;
    double const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    double const scaled = std::ceil(distance * 1000.0);
    return scaled >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ?
        std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(scaled);
}

CandidateLeg TransitionFacts(std::uint64_t stableId, LegKind kind, Endpoint const& source,
                             Endpoint const& destination)
{
    CandidateLeg facts;
    facts.stableId = stableId;
    facts.kind = kind;
    facts.source = source;
    facts.destination = destination;
    facts.normalTransitionValidated = true;
    facts.directedEdge = {true, source.mapId, source.coordinates,
                          destination.mapId, destination.coordinates};
    return facts;
}

ExecutableLeg WalkingApproach(std::uint64_t transitionStableId, Endpoint const& current,
                              Endpoint const& destination)
{
    CandidateLeg facts;
    facts.stableId = StableIdFor(LegKind::WalkingApproach,
                                 static_cast<std::uint32_t>(transitionStableId),
                                 static_cast<std::uint32_t>(transitionStableId >> 32));
    facts.kind = LegKind::WalkingApproach;
    facts.source = current;
    facts.destination = destination;
    facts.cost = DistanceCost(current, destination);
    return {facts, WalkPayload{destination}};
}

template <typename Payload>
bool AddApproachOrTransition(CatalogInput const& input, CandidateLeg facts, Payload payload,
                             double interactionRadius, std::vector<ExecutableLeg>& legs)
{
    if (facts.source.mapId != input.current.mapId ||
        facts.destination.mapId != input.finalDestination.mapId)
        return true;

    if (!IsWithin(input.current, facts.source, RadiusOrDefault(interactionRadius)))
        legs.push_back(WalkingApproach(facts.stableId, input.current, facts.source));
    else
        legs.push_back({std::move(facts), std::move(payload)});
    return true;
}

CatalogIssue ValidateUniqueAndPayloads(std::vector<ExecutableLeg> const& legs)
{
    std::set<std::uint64_t> identities;
    for (ExecutableLeg const& leg : legs)
    {
        if (!identities.insert(leg.policyFacts.stableId).second)
            return CatalogIssue::DuplicateStableId;
        if (!ValidateExecutableLeg(leg))
            return CatalogIssue::PayloadMismatch;
    }
    return CatalogIssue::None;
}
}  // namespace

std::uint64_t StableIdFor(LegKind kind, std::uint32_t primaryId, std::uint32_t secondaryId)
{
    return (static_cast<std::uint64_t>(kind) << 60) |
        ((static_cast<std::uint64_t>(secondaryId) & 0x0FFFFFFFULL) << 32) | primaryId;
}

bool SameEndpoint(Endpoint const& left, Endpoint const& right)
{
    return left.mapId == right.mapId && left.mapValidated == right.mapValidated &&
        left.coordinates.x == right.coordinates.x && left.coordinates.y == right.coordinates.y &&
        left.coordinates.z == right.coordinates.z;
}

bool IsWithin(Endpoint const& actual, Endpoint const& expected, double radius)
{
    if (!ValidEndpoint(actual) || !ValidEndpoint(expected) || actual.mapId != expected.mapId ||
        !std::isfinite(radius) || radius < 0.0)
        return false;

    double const dx = actual.coordinates.x - expected.coordinates.x;
    double const dy = actual.coordinates.y - expected.coordinates.y;
    double const dz = actual.coordinates.z - expected.coordinates.z;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

bool ValidateExecutableLeg(ExecutableLeg const& leg)
{
    CandidateLeg const& facts = leg.policyFacts;
    if (!ValidEndpoint(facts.source) || !ValidEndpoint(facts.destination) || facts.stableId == 0)
        return false;

    return std::visit(
        [&facts](auto const& payload)
        {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, WalkPayload>)
                return facts.kind == LegKind::WalkingApproach &&
                    facts.source.mapId == facts.destination.mapId &&
                    SameEndpoint(payload.endpoint, facts.destination);
            if constexpr (std::is_same_v<Payload, AreaTriggerPayload>)
                return facts.kind == LegKind::AreaTriggerPortal && payload.triggerId != 0;
            if constexpr (std::is_same_v<Payload, GameObjectPayload>)
                return facts.kind == LegKind::GameObjectPortal && payload.spawnId != 0 &&
                    payload.entry != 0 && payload.spellId != 0;
            if constexpr (std::is_same_v<Payload, TaxiPayload>)
                return facts.kind == LegKind::Taxi && payload.flightmasterSpawnId != 0 &&
                    payload.flightmasterEntry != 0 && payload.directedNodeIds.size() >= 2;
            if constexpr (std::is_same_v<Payload, TransportPayload>)
                return facts.kind == LegKind::Transport && payload.entry != 0 &&
                    payload.taxiPathId != 0 && payload.sourceStop != payload.destinationStop &&
                    SameEndpoint(payload.boardingPoint, facts.source);
            return false;
        },
        leg.payload);
}

CatalogResult BuildCatalog(CatalogInput const& input)
{
    CatalogResult result;
    result.request.source = input.current;
    result.request.botMoney = input.botMoney;

    if (!ValidEndpoint(input.current))
    {
        result.issue = CatalogIssue::InvalidCurrent;
        return result;
    }
    if (!ValidEndpoint(input.finalDestination))
    {
        result.issue = CatalogIssue::InvalidDestination;
        return result;
    }

    if (input.current.mapId == input.finalDestination.mapId)
    {
        if (!IsWithin(input.current, input.finalDestination, kDefaultArrivalRadius))
        {
            ExecutableLeg walk = WalkingApproach(
                StableIdFor(LegKind::WalkingApproach, input.finalDestination.mapId, 0),
                input.current, input.finalDestination);
            // WalkingApproach derives an approach identity from a transition identity. Replace that
            // identity here with the stable final-walk identity to keep it human-readable.
            walk.policyFacts.stableId = StableIdFor(LegKind::WalkingApproach,
                                                     input.finalDestination.mapId, 1);
            result.executableLegs.push_back(std::move(walk));
        }
    }

    for (AreaTriggerRecord const& record : input.areaTriggers)
    {
        if (!record.validated || record.triggerId == 0 || !ValidEndpoint(record.source) ||
            !ValidEndpoint(record.destination))
        {
            result.issue = CatalogIssue::InvalidAreaTrigger;
            return result;
        }
        CandidateLeg facts = TransitionFacts(StableIdFor(LegKind::AreaTriggerPortal, record.triggerId),
                                              LegKind::AreaTriggerPortal, record.source,
                                              record.destination);
        AddApproachOrTransition(input, std::move(facts), AreaTriggerPayload{record.triggerId},
                                record.interactionRadius, result.executableLegs);
    }

    for (GameObjectPortalRecord const& record : input.gameObjectPortals)
    {
        if (!record.directedSpellTargetValidated || record.spawnId == 0 || record.entry == 0 ||
            record.spellId == 0 || !ValidEndpoint(record.source) || !ValidEndpoint(record.destination))
        {
            result.issue = CatalogIssue::InvalidGameObjectPortal;
            return result;
        }
        CandidateLeg facts = TransitionFacts(
            StableIdFor(LegKind::GameObjectPortal, record.spawnId, record.spellId),
            LegKind::GameObjectPortal, record.source, record.destination);
        AddApproachOrTransition(input, std::move(facts),
                                GameObjectPayload{record.spawnId, record.entry, record.spellId},
                                record.interactionRadius, result.executableLegs);
    }

    for (TaxiRecord const& record : input.taxis)
    {
        if (input.taxiCheater)
        {
            result.issue = CatalogIssue::TaxiCheatEnabled;
            return result;
        }
        if (input.instantTaxi)
        {
            result.issue = CatalogIssue::InstantTaxiEnabled;
            return result;
        }
        if (record.flightmasterSpawnId == 0 || record.flightmasterEntry == 0 || record.pathId == 0 ||
            record.directedNodeIds.size() < 2 || !ValidEndpoint(record.source) ||
            !ValidEndpoint(record.destination))
        {
            result.issue = CatalogIssue::MissingFlightmaster;
            return result;
        }
        if (!record.allNodesKnown)
        {
            result.issue = CatalogIssue::UnknownTaxiNodes;
            return result;
        }
        if (input.botMoney < record.cost)
        {
            result.issue = CatalogIssue::InsufficientTaxiMoney;
            return result;
        }

        CandidateLeg facts = TransitionFacts(StableIdFor(LegKind::Taxi, record.pathId,
                                                          record.flightmasterSpawnId),
                                              LegKind::Taxi, record.source, record.destination);
        facts.cost = record.cost;
        facts.allTaxiNodesKnown = true;
        AddApproachOrTransition(input, std::move(facts),
                                TaxiPayload{record.flightmasterSpawnId, record.flightmasterEntry,
                                            record.directedNodeIds},
                                record.interactionRadius, result.executableLegs);
    }

    for (TransportRecord const& record : input.transports)
    {
        if (!record.dynamicMotionTransport)
        {
            result.issue = CatalogIssue::StaticTransport;
            return result;
        }
        if (!record.directedStopsValidated || record.entry == 0 || record.taxiPathId == 0 ||
            record.sourceStop == record.destinationStop || !ValidEndpoint(record.boardingPoint) ||
            !ValidEndpoint(record.destination))
        {
            result.issue = CatalogIssue::MalformedTransport;
            return result;
        }

        CandidateLeg facts = TransitionFacts(StableIdFor(LegKind::Transport, record.entry,
                                                          record.taxiPathId),
                                              LegKind::Transport, record.boardingPoint,
                                              record.destination);
        facts.realDynamicTransport = true;
        AddApproachOrTransition(input, std::move(facts),
                                TransportPayload{record.entry, record.taxiPathId,
                                                 record.sourceStop, record.destinationStop,
                                                 record.boardingPoint},
                                record.boardingRadius, result.executableLegs);
    }

    result.issue = ValidateUniqueAndPayloads(result.executableLegs);
    if (result.issue != CatalogIssue::None)
        result.executableLegs.clear();

    for (ExecutableLeg const& leg : result.executableLegs)
        result.request.candidates.push_back(leg.policyFacts);
    return result;
}

CatalogInput BuildRuntimeCatalogInput(Player const& player, Endpoint const& finalDestination)
{
    CatalogInput input;
    input.current = {player.GetMapId(),
                     {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()}, true};
    input.finalDestination = finalDestination;
    input.botMoney = player.GetMoney();
    input.taxiCheater = player.isTaxiCheater();
    input.instantTaxi = sWorld->getIntConfig(CONFIG_INSTANT_TAXI) != 0;

    for (auto const& [triggerId, destination] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* source = sObjectMgr->GetAreaTrigger(triggerId);
        if (!source || source->map != player.GetMapId())
            continue;

        input.areaTriggers.push_back(
            {triggerId,
             {source->map, {source->x, source->y, source->z}, true},
             {destination.target_mapId,
              {destination.target_X, destination.target_Y, destination.target_Z}, true},
             source->radius,
             true});
    }

    return input;
}
}  // namespace AutoWowCampaignTravel
