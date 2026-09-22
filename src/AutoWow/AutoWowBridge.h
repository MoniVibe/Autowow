/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUTOWOWBRIDGE_H
#define PLAYERBOTS_AUTOWOWBRIDGE_H

#include "Common.h"

#include <atomic>
#include <limits>
#include <string>

namespace AutoWowPolicy
{
void SetNoTeleport(uint32 botGuid, bool enabled);
bool IsNoTeleport(uint32 botGuid);
}

class PlayerbotAI;

namespace AutoWowQuestAcquisitionBridge
{
// This is intentionally a small, pure ownership contract. The production tick method below is
// called from PlayerbotAI::UpdateAI on the map/world thread; these facts keep its gate ordering
// testable without constructing a live Player, TravelTarget, or world session.
enum class TickResult : uint8
{
    NoWork,
    Owned,
    Terminal
};

constexpr uint32 kNativeTickCadenceMs = 1000;

inline uint32 SaturatingMonotonicDeltaMs(uint64 previousMs, uint64 currentMs)
{
    if (currentMs <= previousMs)
        return 0;
    uint64 const deltaMs = currentMs - previousMs;
    return deltaMs > std::numeric_limits<uint32>::max()
        ? std::numeric_limits<uint32>::max()
        : static_cast<uint32>(deltaMs);
}

struct TickFacts
{
    bool hasPendingSession = false;
    bool sessionTerminal = false;
    bool terminalCleanupNeeded = false;
    bool botPaused = false;
    bool botAlive = false;
    bool inCombat = false;
    bool inFlight = false;
    bool beingTeleported = false;
    bool canMove = false;
    bool travelActivityAllowed = false;
    bool detailedMoveAllowed = false;
    bool campaignTravelOwned = false;
    bool targetInstalled = false;
    bool targetActive = false;
    bool targetWorkReady = false;
    bool cadenceDue = false;
    // Authoritative current Player::isMoving() state only. Historical movement feedback is
    // telemetry and must never keep this gate true after the live segment stops.
    bool movementInFlight = false;
};

inline TickResult DecideTick(TickFacts const& facts)
{
    if (!facts.hasPendingSession)
        return TickResult::NoWork;

    if (facts.sessionTerminal)
        return facts.terminalCleanupNeeded ? TickResult::Terminal : TickResult::NoWork;

    // These gates deliberately relinquish ownership so combat, death, teleport, and the
    // higher-priority campaign travel owner can run normally.
    if (facts.campaignTravelOwned || facts.botPaused || !facts.botAlive || facts.inCombat ||
        facts.inFlight || facts.beingTeleported || !facts.canMove ||
        !facts.travelActivityAllowed || !facts.detailedMoveAllowed)
        return TickResult::NoWork;

    // Once the party is cohesive at the giver, let the ordinary travel/work path call the
    // destination with performAcceptance=true. The native owner only owns the approach.
    if (facts.targetWorkReady)
        return TickResult::NoWork;

    // A pending session owns its exact target even between staged-mover attempts. The target
    // itself must still be present and active; production reconciles a missing/expired target
    // before evaluating this contract.
    if (!facts.targetInstalled || !facts.targetActive)
        return TickResult::NoWork;

    return TickResult::Owned;
}

inline bool ShouldDispatchStagedMover(TickFacts const& facts)
{
    return DecideTick(facts) == TickResult::Owned && facts.cadenceDue &&
        !facts.movementInFlight;
}

struct TickCadence
{
    uint32 remainingMs = 0;

    void Advance(uint32 elapsedMs)
    {
        remainingMs = elapsedMs >= remainingMs ? 0 : remainingMs - elapsedMs;
    }

    bool IsDue() const { return remainingMs == 0; }

    void Arm(uint32 delayMs = kNativeTickCadenceMs) { remainingMs = delayMs; }
};
}

// A deliberately small, localhost-only control surface for an external AutoWoW agent.
// Network threads only parse requests and wait for a response. All Playerbot and game-object
// access is deferred to PlayerbotWorldThreadProcessor and therefore runs on the world thread.
// Exact guild-trade requests use that same world-thread boundary and normal WorldSession trade
// handlers; the bridge never transfers their money or items directly.
class AutoWowBridge
{
public:
    using QuestAcquisitionTickResult = AutoWowQuestAcquisitionBridge::TickResult;

    static AutoWowBridge& instance()
    {
        static AutoWowBridge instance;
        return instance;
    }

    void Start();

    // Must be called by the owning PlayerbotAI update on the map/world thread. It is deliberately
    // not a queued bridge operation: the native AI tick is the movement clock after one initial
    // quest-acquire request. No caller should invoke this from a network thread.
    QuestAcquisitionTickResult TickQuestAcquisition(PlayerbotAI* botAI, uint32 elapsedMs);

private:
    AutoWowBridge() = default;
    ~AutoWowBridge() = default;

    AutoWowBridge(AutoWowBridge const&) = delete;
    AutoWowBridge& operator=(AutoWowBridge const&) = delete;

    void Run(uint16 port);

    std::atomic_bool started = false;
};

namespace AutoWowQuestAcquisitionBridgeTest
{
struct LifecycleScenarioResult
{
    bool targetInactive = false;
    bool unloadedGiverDidNotStartStaleWait = false;
    bool confirmedMissingGiverReachedBoundedTerminal = false;
    std::string response;
    std::string snapshot;
};

// Integration seam implemented in AutoWowBridge.cpp. It uses the production private session,
// destination, TravelTarget lifecycle, and response/snapshot builders.
LifecycleScenarioResult RunNativeExpiryScenario();
}

#endif
