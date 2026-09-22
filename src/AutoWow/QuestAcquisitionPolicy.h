/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_ACQUISITION_POLICY_H
#define AUTOWOW_QUEST_ACQUISITION_POLICY_H

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Pure parser and deterministic candidate ranking for the bridge's narrow quest-acquisition lane.
// World lookup, travel-target mutation, and the normal quest-accept opcode remain in AutoWowBridge.
namespace AutoWowQuestAcquisition
{
inline bool IsEventBoundSpawnEligible(std::vector<std::int16_t> const& signedEventBindings,
                                      std::vector<std::uint16_t> const& activeEvents)
{
    if (signedEventBindings.empty())
        return true;

    for (std::int16_t const binding : signedEventBindings)
    {
        if (!binding)
            continue;
        std::uint16_t const eventId = static_cast<std::uint16_t>(
            binding > 0 ? binding : -static_cast<std::int32_t>(binding));
        bool const active = std::find(activeEvents.begin(), activeEvents.end(), eventId) !=
            activeEvents.end();
        if ((binding > 0 && active) || (binding < 0 && !active))
            return true;
    }
    return false;
}

enum class WireMode
{
    Automatic,
    ExplicitQuest,
    Acquire
};

struct WireRequest
{
    WireMode mode = WireMode::Automatic;
    std::uint32_t leaderGuid = 0;
    std::uint32_t questId = 0;
};

// A selected destination and ordinary TravelTarget describe intent only. Quest acquisition never
// forces the target: native expiry and destination validity must remain active lifecycle guards.
// One immediate action execution may be transiently useless; it is a kick, not the persistent
// activation contract.
struct MovementOrderFacts
{
    bool targetInstalled = false;
    bool travelStatus = false;
    bool forced = false;  // Observational only; quest acquisition must leave this false.
    bool travelStrategy = false;
    bool actionActivated = false;
};

enum class MovementActivationFailure : std::uint8_t
{
    None,
    TargetNotInstalled,
    TravelStatusNotActive,
    TravelStrategyMissing,
    ActionNotActivated
};

inline MovementActivationFailure ClassifyMovementActivation(MovementOrderFacts const& facts)
{
    if (!facts.targetInstalled)
        return MovementActivationFailure::TargetNotInstalled;
    if (!facts.travelStatus)
        return MovementActivationFailure::TravelStatusNotActive;
    if (!facts.travelStrategy)
        return MovementActivationFailure::TravelStrategyMissing;
    if (!facts.actionActivated)
        return MovementActivationFailure::ActionNotActivated;
    return MovementActivationFailure::None;
}

inline std::string_view MovementActivationFailureName(MovementActivationFailure failure)
{
    switch (failure)
    {
        case MovementActivationFailure::None: return "active";
        case MovementActivationFailure::TargetNotInstalled: return "target_not_installed";
        case MovementActivationFailure::TravelStatusNotActive: return "travel_status_not_active";
        case MovementActivationFailure::TravelStrategyMissing: return "travel_strategy_missing";
        case MovementActivationFailure::ActionNotActivated: return "action_not_activated";
        default: return "unknown";
    }
}

inline bool IsMovementOrderActive(MovementOrderFacts const& facts)
{
    return ClassifyMovementActivation(facts) == MovementActivationFailure::None;
}

// DoSpecificAction exposes only a bool through PlayerbotAI. Preserve that result explicitly so
// diagnostics can distinguish an action that was never attempted from one that was rejected,
// without changing the action itself or reaching into the engine's execution path.
enum class MovementActionResult : std::uint8_t
{
    NotAttempted,
    Accepted,
    Rejected
};

inline std::string_view MovementActionResultName(MovementActionResult result)
{
    switch (result)
    {
        case MovementActionResult::NotAttempted: return "not_attempted";
        case MovementActionResult::Accepted: return "accepted";
        case MovementActionResult::Rejected: return "rejected";
        default: return "unknown";
    }
}

// PathProbeResult::safe intentionally allows a normal/incomplete hybrid when it has made some
// progress. Acquisition diagnostics need a stricter, endpoint-complete disposition: q827 showed
// that a probe can be marked safe while its final point remains hundreds of yards away.
struct PathProbeFacts
{
    bool attempted = false;
    bool safe = false;
    bool normalPathType = false;
    std::uint32_t pathType = 0;
    std::size_t pointCount = 0;
    double endpointDistance = 0.0;
    double pathLength = 0.0;
};

enum class PathProbeDisposition : std::uint8_t
{
    NotAttempted,
    Unsafe,
    Incomplete,
    Complete
};

inline PathProbeDisposition ClassifyPathProbe(PathProbeFacts const& facts,
                                              double completeEndpointDistance = 2.5)
{
    if (!facts.attempted)
        return PathProbeDisposition::NotAttempted;
    if (!facts.safe || !facts.normalPathType || facts.pointCount < 2)
        return PathProbeDisposition::Unsafe;
    if (!std::isfinite(facts.endpointDistance) || facts.endpointDistance > completeEndpointDistance)
        return PathProbeDisposition::Incomplete;
    return PathProbeDisposition::Complete;
}

inline std::string_view PathProbeDispositionName(PathProbeDisposition disposition)
{
    switch (disposition)
    {
        case PathProbeDisposition::NotAttempted: return "not_attempted";
        case PathProbeDisposition::Unsafe: return "unsafe";
        case PathProbeDisposition::Incomplete: return "incomplete";
        case PathProbeDisposition::Complete: return "complete";
        default: return "unknown";
    }
}

inline bool IsPathEndpointComplete(PathProbeFacts const& facts)
{
    return ClassifyPathProbe(facts) == PathProbeDisposition::Complete;
}

struct MovementDiagnosticsFacts
{
    MovementOrderFacts order;
    MovementActionResult actionResult = MovementActionResult::NotAttempted;
    PathProbeFacts pathProbe;
    bool inCombat = false;
    bool canMove = false;
    bool canMoveAround = false;
    bool travelActivityAllowed = false;
    bool detailedMoveAllowed = false;
    bool inFlight = false;
    bool controlled = false;
    bool beingTeleported = false;
    bool canLoot = false;
    bool isMoving = false;
    bool waitingForNormalMove = false;
};

enum class MovementDiagnosticCategory : std::uint8_t
{
    NotAttempted,
    TargetNotActive,
    ActionGate,
    PathOrGround,
    PendingOrdinaryTravel
};

inline MovementDiagnosticCategory ClassifyMovementDiagnostics(MovementDiagnosticsFacts const& facts)
{
    // These are concrete gates used by the normal movement action. Keep isMoving and the
    // movement reservation out of this branch: they explain a legitimate pending travel state.
    if (facts.inCombat || !facts.canMove || !facts.canMoveAround || !facts.travelActivityAllowed ||
        facts.inFlight || facts.controlled || facts.beingTeleported || facts.canLoot)
        return MovementDiagnosticCategory::ActionGate;

    if (!IsMovementOrderActive(facts.order))
        return MovementDiagnosticCategory::TargetNotActive;

    PathProbeDisposition const pathDisposition = ClassifyPathProbe(facts.pathProbe);
    if (pathDisposition == PathProbeDisposition::Unsafe ||
        pathDisposition == PathProbeDisposition::Incomplete)
        return MovementDiagnosticCategory::PathOrGround;

    if (facts.actionResult != MovementActionResult::NotAttempted || facts.isMoving ||
        facts.waitingForNormalMove)
        return MovementDiagnosticCategory::PendingOrdinaryTravel;

    return MovementDiagnosticCategory::NotAttempted;
}

inline std::string_view MovementDiagnosticCategoryName(MovementDiagnosticCategory category)
{
    switch (category)
    {
        case MovementDiagnosticCategory::NotAttempted: return "not_attempted";
        case MovementDiagnosticCategory::TargetNotActive: return "target_not_active";
        case MovementDiagnosticCategory::ActionGate: return "action_gate";
        case MovementDiagnosticCategory::PathOrGround: return "path_or_ground";
        case MovementDiagnosticCategory::PendingOrdinaryTravel: return "pending_ordinary_travel";
        default: return "unknown";
    }
}

inline bool ParsePositiveUint32(std::string_view token, std::uint32_t& value)
{
    if (token.empty())
        return false;

    std::uint32_t parsed = 0;
    auto const result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size() || !parsed)
        return false;

    value = parsed;
    return true;
}

inline std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

inline bool ParseWireRequest(std::string_view text, WireRequest& request, std::string& error)
{
    request = {};
    error.clear();

    std::istringstream input{std::string(text)};
    std::string command;
    std::string leader;
    std::string selector;
    std::string extra;
    if (!(input >> command >> leader) || LowerAscii(command) != "quest" ||
        !ParsePositiveUint32(leader, request.leaderGuid))
    {
        error = "quest requires a positive numeric leader GUID";
        return false;
    }

    if (!(input >> selector))
    {
        request.mode = WireMode::Automatic;
        return true;
    }

    if (input >> extra)
    {
        error = "quest accepts exactly one selector (quest-id|acquire)";
        return false;
    }

    if (LowerAscii(selector) == "acquire")
    {
        request.mode = WireMode::Acquire;
        return true;
    }

    if (!ParsePositiveUint32(selector, request.questId))
    {
        error = "quest selector must be a positive quest id or acquire";
        return false;
    }

    request.mode = WireMode::ExplicitQuest;
    return true;
}

struct Candidate
{
    std::size_t sourceIndex = 0;
    std::uint32_t questId = 0;
    std::int32_t giverEntry = 0;
    std::uint32_t mapId = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double distance = 0.0;
    bool activeQuest = false;
    bool coreEligible = false;
    bool partySuitable = false;
    bool supported = false;
    std::uint64_t liveGiverGuid = 0;
    // DB-backed creature/gameobject identity used while its grid is unloaded. This is not an
    // interaction permission; the session must still bind a matching runtime object at arrival.
    std::uint64_t persistentSpawnGuid = 0;
};

struct LiveGiverFacts
{
    std::uint64_t liveGiverGuid = 0;
    std::int32_t signedEntry = 0;
    std::uint32_t liveEntry = 0;
    bool isCreature = false;
    bool isGameObject = false;
    bool inWorld = false;
    bool alive = false;
    bool offersQuest = false;
};

struct PersistentGiverBindingFacts
{
    std::uint64_t persistentSpawnGuid = 0;
    std::uint64_t expectedRuntimeGiverGuid = 0;
    std::uint64_t runtimeGiverGuid = 0;
    std::uint64_t runtimeSpawnId = 0;
    std::uint32_t persistentMapId = 0;
    std::uint32_t runtimeMapId = 0;
    std::int32_t signedEntry = 0;
    std::uint32_t runtimeEntry = 0;
    bool runtimeIsCreature = false;
    bool runtimeIsGameObject = false;
};

inline bool IsMatchingPersistentGiver(PersistentGiverBindingFacts const& facts)
{
    if (!facts.persistentSpawnGuid || !facts.runtimeGiverGuid ||
        facts.runtimeSpawnId != facts.persistentSpawnGuid ||
        facts.persistentMapId != facts.runtimeMapId || !facts.signedEntry || !facts.runtimeEntry)
        return false;
    if (facts.expectedRuntimeGiverGuid &&
        facts.expectedRuntimeGiverGuid != facts.runtimeGiverGuid)
        return false;

    std::uint32_t const expectedEntry = facts.signedEntry > 0
        ? static_cast<std::uint32_t>(facts.signedEntry)
        : static_cast<std::uint32_t>(-static_cast<std::int64_t>(facts.signedEntry));
    if (facts.runtimeEntry != expectedEntry)
        return false;

    return facts.signedEntry > 0
        ? facts.runtimeIsCreature && !facts.runtimeIsGameObject
        : !facts.runtimeIsCreature && facts.runtimeIsGameObject;
}

enum class GiverResolutionFailure : std::uint8_t
{
    None,
    Missing,
    TypeOrEntryMismatch,
    NotInWorld,
    Dead,
    QuestUnavailable
};

inline GiverResolutionFailure ClassifyGiverResolution(LiveGiverFacts const& facts)
{
    if (!facts.liveGiverGuid)
        return GiverResolutionFailure::Missing;

    if (!facts.signedEntry || !facts.liveEntry || facts.isCreature == facts.isGameObject)
        return GiverResolutionFailure::TypeOrEntryMismatch;

    std::uint32_t const expectedEntry = facts.signedEntry > 0
        ? static_cast<std::uint32_t>(facts.signedEntry)
        : static_cast<std::uint32_t>(-static_cast<std::int64_t>(facts.signedEntry));
    if (facts.liveEntry != expectedEntry)
        return GiverResolutionFailure::TypeOrEntryMismatch;

    if (!facts.inWorld)
        return GiverResolutionFailure::NotInWorld;
    if (facts.isCreature && !facts.alive)
        return GiverResolutionFailure::Dead;
    if (!facts.offersQuest)
        return GiverResolutionFailure::QuestUnavailable;
    return GiverResolutionFailure::None;
}

inline std::string_view GiverResolutionFailureName(GiverResolutionFailure failure)
{
    switch (failure)
    {
        case GiverResolutionFailure::None: return "resolved";
        case GiverResolutionFailure::Missing: return "missing";
        case GiverResolutionFailure::TypeOrEntryMismatch: return "type_or_entry_mismatch";
        case GiverResolutionFailure::NotInWorld: return "not_in_world";
        case GiverResolutionFailure::Dead: return "dead";
        case GiverResolutionFailure::QuestUnavailable: return "quest_unavailable";
        default: return "unknown";
    }
}

inline bool IsEligibleLiveGiver(LiveGiverFacts const& facts)
{
    return ClassifyGiverResolution(facts) == GiverResolutionFailure::None;
}

inline bool IsEligible(Candidate const& candidate)
{
    return candidate.questId && candidate.giverEntry && !candidate.activeQuest &&
        candidate.coreEligible && candidate.partySuitable && candidate.supported &&
        (candidate.liveGiverGuid || candidate.persistentSpawnGuid) &&
        std::isfinite(candidate.distance) && candidate.distance >= 0.0 &&
        std::isfinite(candidate.x) && std::isfinite(candidate.y) && std::isfinite(candidate.z);
}

inline std::optional<std::size_t> SelectCandidate(std::vector<Candidate> const& candidates)
{
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        Candidate const& candidate = candidates[index];
        if (!IsEligible(candidate))
            continue;

        if (!selected)
        {
            selected = index;
            continue;
        }

        Candidate const& current = candidates[*selected];
        if (std::tie(candidate.distance, candidate.questId, candidate.giverEntry, candidate.mapId,
                     candidate.x, candidate.y, candidate.z, candidate.sourceIndex) <
            std::tie(current.distance, current.questId, current.giverEntry, current.mapId,
                     current.x, current.y, current.z, current.sourceIndex))
            selected = index;
    }
    return selected;
}
}  // namespace AutoWowQuestAcquisition

#endif  // AUTOWOW_QUEST_ACQUISITION_POLICY_H
