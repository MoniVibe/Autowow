/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_CAMPAIGN_TRAVEL_POLICY_H
#define AUTOWOW_CAMPAIGN_TRAVEL_POLICY_H

#include <cstdint>
#include <limits>
#include <vector>

// Pure Phase A policy for choosing one evidence-backed campaign travel transition. The caller
// normalizes world data into these value types; this layer performs no lookup, movement, learning,
// payment, or other mutation.
namespace AutoWowCampaignTravel
{
inline constexpr std::uint32_t kInvalidMapId = std::numeric_limits<std::uint32_t>::max();
// Mirrors AzerothCore's finite MAP_HALFSIZE - 0.5 coordinate envelope without importing world code.
inline constexpr double kMaximumAbsCoordinate = 17066.166;

enum class LegKind : std::uint8_t
{
    WalkingApproach,
    AreaTriggerPortal,
    GameObjectPortal,
    Transport,
    Taxi,
    Teleport,
    Hearth,
    Unknown
};

struct Coordinates
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Endpoint
{
    std::uint32_t mapId = kInvalidMapId;
    Coordinates coordinates;
    bool mapValidated = false;
};

// A proof is directional and bound to the exact normalized endpoints. Reusing a forward proof for
// the reverse candidate therefore fails even when both endpoints are on the same map.
struct DirectedEdgeProof
{
    bool validated = false;
    std::uint32_t sourceMapId = kInvalidMapId;
    Coordinates sourceCoordinates;
    std::uint32_t destinationMapId = kInvalidMapId;
    Coordinates destinationCoordinates;
};

struct CheatFlags
{
    bool seedGold = false;
    bool travelCheat = false;
};

struct CandidateLeg
{
    // Stable normalizer-owned identity. It is a deterministic tie-break key, never a source index.
    std::uint64_t stableId = 0;
    LegKind kind = LegKind::Unknown;
    Endpoint source;
    Endpoint destination;
    std::uint64_t cost = 0;
    CheatFlags cheats;
    bool normalTransitionValidated = false;
    bool realDynamicTransport = false;
    bool allTaxiNodesKnown = false;
    DirectedEdgeProof directedEdge;
};

struct Request
{
    Endpoint source;
    std::uint64_t botMoney = 0;
    std::vector<CandidateLeg> candidates;
};

enum class DecisionKind : std::uint8_t
{
    Blocked,
    Transition
};

// Declaration order is the deterministic blocker precedence when all candidates are rejected.
enum class Blocker : std::uint8_t
{
    None,
    InvalidRequestSourceMap,
    InvalidRequestSourceCoordinates,
    NoCandidateLegs,
    InvalidCandidateMap,
    InvalidCandidateCoordinates,
    CandidateSourceMapMismatch,
    SeedGoldRejected,
    CheatRejected,
    TeleportRejected,
    HearthRejected,
    UnknownLegKind,
    CrossMapWalkingRejected,
    PortalNotValidated,
    TransportNotDynamic,
    DirectedEdgeProofMissing,
    DirectedEdgeProofMismatch,
    UnknownTaxiNodes,
    InsufficientMoney
};

struct Decision
{
    DecisionKind kind = DecisionKind::Blocked;
    Blocker blocker = Blocker::NoCandidateLegs;
    CandidateLeg transition;
    bool runtimeBoardingProofRequired = false;
};

Decision PlanNextTransition(Request const& request);
}  // namespace AutoWowCampaignTravel

#endif  // AUTOWOW_CAMPAIGN_TRAVEL_POLICY_H
