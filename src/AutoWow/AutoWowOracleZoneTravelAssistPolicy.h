#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_ZONE_TRAVEL_ASSIST_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_ZONE_TRAVEL_ASSIST_POLICY_H

#include "OracleRouteExecutor.h"

#include <cstdint>

namespace AutoWowOracleZoneTravelAssist
{
inline constexpr std::uint64_t kCooldownSeconds = 60;
inline constexpr std::uint64_t kBindWaitSeconds = 10;

struct Facts
{
    bool enabled = false;
    bool managed = false;
    bool taggedLease = false;
    bool exactSpawn = false;
    bool alive = false;
    bool inCombat = false;
    bool inFlight = false;
    bool onTransport = false;
    bool teleporting = false;
    bool moving = false;
    bool sameMapAndInstance = false;
    bool sameZone = false;
    bool validDestination = false;
    bool outsideArrivalRadius = false;
    bool cooldownReady = false;
    AutoWowOracleRoute::RouteFailure failure = AutoWowOracleRoute::RouteFailure::None;
};

[[nodiscard]] inline constexpr bool RecoverableFailure(
    AutoWowOracleRoute::RouteFailure failure) noexcept
{
    using AutoWowOracleRoute::RouteFailure;
    return failure == RouteFailure::Stalled ||
        failure == RouteFailure::DeadlineExceeded ||
        failure == RouteFailure::TargetNotLoadedAfterGridProbe ||
        failure == RouteFailure::ExactLiveBindRequired;
}

[[nodiscard]] inline constexpr bool ShouldAssist(Facts const& facts) noexcept
{
    return facts.enabled && facts.managed && facts.taggedLease && facts.exactSpawn &&
        facts.alive && !facts.inCombat && !facts.inFlight && !facts.onTransport &&
        !facts.teleporting && !facts.moving && facts.sameMapAndInstance &&
        facts.sameZone && facts.validDestination && facts.outsideArrivalRadius &&
        facts.cooldownReady && RecoverableFailure(facts.failure);
}

enum class BindDecision : std::uint8_t
{
    Wait,
    EnterNativePhase,
    Block
};

// A teleport is only a change of position. The caller must load and bind the exact live spawn,
// then confirm interaction/arrival range before native quest work may resume.
[[nodiscard]] inline constexpr BindDecision DecidePostTeleportBind(
    bool teleporting, bool exactLiveIdentity, bool withinRadius,
    std::uint64_t elapsedSeconds) noexcept
{
    if (!teleporting && exactLiveIdentity && withinRadius)
        return BindDecision::EnterNativePhase;
    return elapsedSeconds < kBindWaitSeconds ? BindDecision::Wait : BindDecision::Block;
}
}  // namespace AutoWowOracleZoneTravelAssist

#endif
