/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef AUTOWOW_CAMPAIGN_TRAVEL_CATALOG_H
#define AUTOWOW_CAMPAIGN_TRAVEL_CATALOG_H

#include "CampaignTravelPolicy.h"

#include <cstdint>
#include <variant>
#include <vector>

class Player;

namespace AutoWowCampaignTravel
{
struct WalkPayload
{
    Endpoint endpoint;
};

struct AreaTriggerPayload
{
    std::uint32_t triggerId = 0;
};

struct GameObjectPayload
{
    std::uint32_t spawnId = 0;
    std::uint32_t entry = 0;
    std::uint32_t spellId = 0;
};

struct TaxiPayload
{
    std::uint32_t flightmasterSpawnId = 0;
    std::uint32_t flightmasterEntry = 0;
    std::vector<std::uint32_t> directedNodeIds;
};

struct TransportPayload
{
    std::uint32_t entry = 0;
    std::uint32_t taxiPathId = 0;
    std::uint32_t sourceStop = 0;
    std::uint32_t destinationStop = 0;
    Endpoint boardingPoint;
};

using ExecutablePayload = std::variant<WalkPayload, AreaTriggerPayload, GameObjectPayload,
                                       TaxiPayload, TransportPayload>;

struct ExecutableLeg
{
    CandidateLeg policyFacts;
    ExecutablePayload payload = WalkPayload{};
};

struct AreaTriggerRecord
{
    std::uint32_t triggerId = 0;
    Endpoint source;
    Endpoint destination;
    double interactionRadius = 0.0;
    bool validated = false;
};

struct GameObjectPortalRecord
{
    std::uint32_t spawnId = 0;
    std::uint32_t entry = 0;
    std::uint32_t spellId = 0;
    Endpoint source;
    Endpoint destination;
    double interactionRadius = 0.0;
    bool directedSpellTargetValidated = false;
};

struct TaxiRecord
{
    std::uint32_t pathId = 0;
    std::uint32_t flightmasterSpawnId = 0;
    std::uint32_t flightmasterEntry = 0;
    Endpoint source;
    Endpoint destination;
    std::vector<std::uint32_t> directedNodeIds;
    std::uint64_t cost = 0;
    double interactionRadius = 0.0;
    bool allNodesKnown = false;
};

struct TransportRecord
{
    std::uint32_t entry = 0;
    std::uint32_t taxiPathId = 0;
    std::uint32_t sourceStop = 0;
    std::uint32_t destinationStop = 0;
    Endpoint boardingPoint;
    Endpoint destination;
    double boardingRadius = 0.0;
    bool dynamicMotionTransport = false;
    bool directedStopsValidated = false;
};

struct CatalogInput
{
    Endpoint current;
    Endpoint finalDestination;
    std::uint64_t botMoney = 0;
    bool taxiCheater = false;
    bool instantTaxi = false;
    std::vector<AreaTriggerRecord> areaTriggers;
    std::vector<GameObjectPortalRecord> gameObjectPortals;
    std::vector<TaxiRecord> taxis;
    std::vector<TransportRecord> transports;
};

enum class CatalogIssue : std::uint8_t
{
    None,
    InvalidCurrent,
    InvalidDestination,
    DuplicateStableId,
    PayloadMismatch,
    InvalidAreaTrigger,
    InvalidGameObjectPortal,
    TaxiCheatEnabled,
    InstantTaxiEnabled,
    UnknownTaxiNodes,
    InsufficientTaxiMoney,
    MissingFlightmaster,
    MalformedTransport,
    StaticTransport
};

struct CatalogResult
{
    Request request;
    std::vector<ExecutableLeg> executableLegs;
    CatalogIssue issue = CatalogIssue::None;
};

inline constexpr double kDefaultArrivalRadius = 4.0;

std::uint64_t StableIdFor(LegKind kind, std::uint32_t primaryId, std::uint32_t secondaryId = 0);
bool SameEndpoint(Endpoint const& left, Endpoint const& right);
bool IsWithin(Endpoint const& actual, Endpoint const& expected, double radius);
bool ValidateExecutableLeg(ExecutableLeg const& leg);
CatalogResult BuildCatalog(CatalogInput const& input);

// Map-thread-only normalizer. It copies authoritative area-trigger records and player value facts;
// it never stores a Player or world-object pointer in the returned catalog input.
CatalogInput BuildRuntimeCatalogInput(Player const& player, Endpoint const& finalDestination);
}  // namespace AutoWowCampaignTravel

#endif  // AUTOWOW_CAMPAIGN_TRAVEL_CATALOG_H
