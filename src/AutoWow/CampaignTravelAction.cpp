/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "CampaignTravelAction.h"

#include "CampaignTravelCatalog.h"
#include "CampaignTravelSession.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"
#include "Transport.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cmath>
#include <optional>
#include <variant>

namespace
{
using namespace AutoWowCampaignTravel;

Endpoint CurrentEndpoint(Player const& player)
{
    return {player.GetMapId(),
            {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()}, true};
}

CampaignTravelObservation Observe(Player const& player)
{
    CampaignTravelObservation observation;
    observation.position = CurrentEndpoint(player);
    observation.inTaxiFlight = player.IsInFlight();

    Transport const* transport = player.GetTransport();
    observation.onTransport = transport && player.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    if (observation.onTransport)
    {
        observation.transportEntry = transport->GetEntry();
        GameObjectTemplate const* info = transport->GetGOInfo();
        if (info && info->type == GAMEOBJECT_TYPE_MO_TRANSPORT)
            observation.transportTaxiPathId = info->moTransport.taxiPathId;
    }
    return observation;
}

bool MatchesDestination(AreaTriggerTeleport const& teleport, Endpoint const& destination)
{
    constexpr double epsilon = 0.01;
    return teleport.target_mapId == destination.mapId &&
        std::fabs(teleport.target_X - destination.coordinates.x) <= epsilon &&
        std::fabs(teleport.target_Y - destination.coordinates.y) <= epsilon &&
        std::fabs(teleport.target_Z - destination.coordinates.z) <= epsilon;
}

GameObject* FindGameObject(Player* player, GameObjectPayload const& payload)
{
    auto range = player->GetMap()->GetGameObjectBySpawnIdStore().equal_range(payload.spawnId);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        GameObject* object = itr->second;
        if (object && object->IsInWorld() && object->isSpawned() && object->GetEntry() == payload.entry)
            return object;
    }
    return nullptr;
}

std::uint32_t PortalSpellId(GameObjectTemplate const* info)
{
    if (!info)
        return 0;
    if (info->type == GAMEOBJECT_TYPE_SPELLCASTER)
        return info->spellcaster.spellId;
    if (info->type == GAMEOBJECT_TYPE_GOOBER)
        return info->goober.spellId;
    return 0;
}

Creature* FindFlightmaster(Player* player, TaxiPayload const& payload)
{
    auto range = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(payload.flightmasterSpawnId);
    for (auto itr = range.first; itr != range.second; ++itr)
    {
        Creature* creature = itr->second;
        if (creature && creature->IsInWorld() && creature->IsAlive() &&
            creature->GetEntry() == payload.flightmasterEntry &&
            (creature->GetNpcFlags() & UNIT_NPC_FLAG_FLIGHTMASTER))
            return creature;
    }
    return nullptr;
}

bool ValidateTaxiPath(Player const& player, TaxiPayload const& payload, std::uint64_t& totalCost)
{
    if (player.isTaxiCheater() || sWorld->getIntConfig(CONFIG_INSTANT_TAXI) != 0 ||
        payload.directedNodeIds.size() < 2)
        return false;

    totalCost = 0;
    for (std::uint32_t node : payload.directedNodeIds)
        if (!player.m_taxi.IsTaximaskNodeKnown(node))
            return false;

    for (std::size_t i = 1; i < payload.directedNodeIds.size(); ++i)
    {
        auto source = sTaxiPathSetBySource.find(payload.directedNodeIds[i - 1]);
        if (source == sTaxiPathSetBySource.end())
            return false;
        auto destination = source->second.find(payload.directedNodeIds[i]);
        if (destination == source->second.end() || !destination->second)
            return false;
        totalCost += destination->second->price;
    }
    return player.GetMoney() >= totalCost;
}
}  // namespace

bool CampaignTravelAction::isUseful()
{
    return botAI && botAI->HasCampaignTravelWork();
}

bool CampaignTravelAction::Execute(Event /*event*/)
{
    using namespace AutoWowCampaignTravel;

    if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->IsAlive())
    {
        if (botAI && botAI->campaignTravelSession)
        {
            botAI->campaignTravelSession->BlockRuntime(SessionBlocker::BotUnavailable);
            botAI->campaignTravelMailbox->Publish(botAI->campaignTravelSession->Snapshot());
        }
        return false;
    }

    CampaignTravelSession& session = *botAI->campaignTravelSession;
    CampaignTravelMailbox& mailbox = *botAI->campaignTravelMailbox;
    std::uint32_t const now = getMSTime();

    std::optional<CampaignTravelIntent> pending = mailbox.Consume();
    if (pending)
        session.Begin(*pending, now);

    session.Observe(Observe(*bot), now);
    if (session.GetState() == SessionState::Planning)
    {
        CatalogInput const input = BuildRuntimeCatalogInput(*bot, session.GetIntent().finalDestination);
        session.Plan(BuildCatalog(input), now);
    }

    bool const handled = IsActive(session.GetState()) || IsTerminal(session.GetState());
    if (IsActive(session.GetState()))
        ExecuteSelectedLeg();

    mailbox.Publish(session.Snapshot());
    return handled;
}

bool CampaignTravelAction::ExecuteSelectedLeg()
{
    using namespace AutoWowCampaignTravel;

    CampaignTravelSession& session = *botAI->campaignTravelSession;
    ExecutableLeg const* selected = session.GetSelectedLeg();
    if (!selected)
    {
        session.BlockRuntime(SessionBlocker::PayloadMismatch);
        return false;
    }

    CandidateLeg const& facts = selected->policyFacts;
    Endpoint const current = CurrentEndpoint(*bot);
    if (current.mapId != facts.source.mapId && current.mapId != facts.destination.mapId)
    {
        session.BlockRuntime(SessionBlocker::UnexpectedMap);
        return false;
    }

    bool const awaitingObservation =
        session.GetState() == SessionState::AwaitingExpectedObservation;
    if (awaitingObservation && facts.kind != LegKind::WalkingApproach &&
        facts.kind != LegKind::Transport)
        return true;

    auto moveTo = [this](Endpoint const& endpoint)
    {
        return MoveTo(endpoint.mapId, static_cast<float>(endpoint.coordinates.x),
                      static_cast<float>(endpoint.coordinates.y),
                      static_cast<float>(endpoint.coordinates.z), false, false, true, false,
                      MovementPriority::MOVEMENT_NORMAL, true);
    };

    switch (facts.kind)
    {
        case LegKind::WalkingApproach:
        {
            auto const* payload = std::get_if<WalkPayload>(&selected->payload);
            if (!payload || !SameEndpoint(payload->endpoint, facts.destination))
            {
                session.BlockRuntime(SessionBlocker::PayloadMismatch);
                return false;
            }
            if (!IsWithin(current, payload->endpoint, kDefaultArrivalRadius))
                moveTo(payload->endpoint);
            session.MarkExecutionIssued(facts.stableId, getMSTime());
            return true;
        }
        case LegKind::AreaTriggerPortal:
        {
            auto const* payload = std::get_if<AreaTriggerPayload>(&selected->payload);
            if (!payload)
            {
                session.BlockRuntime(SessionBlocker::PayloadMismatch);
                return false;
            }
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(payload->triggerId);
            AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(payload->triggerId);
            if (!trigger || !teleport || trigger->map != facts.source.mapId ||
                !MatchesDestination(*teleport, facts.destination))
            {
                session.BlockRuntime(SessionBlocker::MissingRuntimeObject);
                return false;
            }
            double const radius = trigger->radius > 0.0f ? trigger->radius : kDefaultArrivalRadius;
            if (!IsWithin(current, facts.source, radius))
                return moveTo(facts.source);

            WorldPacket packet(CMSG_AREATRIGGER);
            packet << payload->triggerId;
            packet.rpos(0);
            bot->GetSession()->HandleAreaTriggerOpcode(packet);
            return session.MarkExecutionIssued(facts.stableId, getMSTime());
        }
        case LegKind::GameObjectPortal:
        {
            auto const* payload = std::get_if<GameObjectPayload>(&selected->payload);
            if (!payload)
            {
                session.BlockRuntime(SessionBlocker::PayloadMismatch);
                return false;
            }
            GameObject* object = FindGameObject(bot, *payload);
            if (!object || PortalSpellId(object->GetGOInfo()) != payload->spellId)
            {
                session.BlockRuntime(SessionBlocker::MissingRuntimeObject);
                return false;
            }
            if (!bot->IsWithinDistInMap(object, INTERACTION_DISTANCE))
                return MoveTo(object, INTERACTION_DISTANCE);

            WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
            packet << object->GetGUID();
            bot->GetSession()->HandleGameObjectUseOpcode(packet);
            return session.MarkExecutionIssued(facts.stableId, getMSTime());
        }
        case LegKind::Taxi:
        {
            auto const* payload = std::get_if<TaxiPayload>(&selected->payload);
            if (!payload)
            {
                session.BlockRuntime(SessionBlocker::PayloadMismatch);
                return false;
            }
            std::uint64_t totalCost = 0;
            if (!ValidateTaxiPath(*bot, *payload, totalCost))
            {
                session.BlockRuntime(SessionBlocker::HandlerRejected);
                return false;
            }
            Creature* flightmaster = FindFlightmaster(bot, *payload);
            if (!flightmaster)
            {
                session.BlockRuntime(SessionBlocker::MissingRuntimeObject);
                return false;
            }
            if (!bot->IsWithinDistInMap(flightmaster, INTERACTION_DISTANCE))
                return MoveTo(flightmaster, INTERACTION_DISTANCE);
            if (!bot->GetNPCIfCanInteractWith(flightmaster->GetGUID(), UNIT_NPC_FLAG_FLIGHTMASTER))
            {
                session.BlockRuntime(SessionBlocker::HandlerRejected);
                return false;
            }

            WorldPacket packet(CMSG_ACTIVATETAXIEXPRESS);
            packet << flightmaster->GetGUID();
            packet << static_cast<std::uint32_t>(payload->directedNodeIds.size());
            for (std::uint32_t node : payload->directedNodeIds)
                packet << node;
            packet.rpos(0);
            bot->GetSession()->HandleActivateTaxiExpressOpcode(packet);
            return session.MarkExecutionIssued(facts.stableId, getMSTime());
        }
        case LegKind::Transport:
        {
            auto const* payload = std::get_if<TransportPayload>(&selected->payload);
            if (!payload || !SameEndpoint(payload->boardingPoint, facts.source))
            {
                session.BlockRuntime(SessionBlocker::PayloadMismatch);
                return false;
            }
            GameObjectTemplate const* transport = sObjectMgr->GetGameObjectTemplate(payload->entry);
            if (!transport || transport->type != GAMEOBJECT_TYPE_MO_TRANSPORT ||
                transport->moTransport.taxiPathId != payload->taxiPathId)
            {
                session.BlockRuntime(SessionBlocker::MissingRuntimeObject);
                return false;
            }
            if (!IsWithin(current, payload->boardingPoint, kDefaultArrivalRadius))
                return moveTo(payload->boardingPoint);
            if (awaitingObservation)
                return true;
            return session.MarkExecutionIssued(facts.stableId, getMSTime());
        }
        default:
            session.BlockRuntime(SessionBlocker::PayloadMismatch);
            return false;
    }
}
