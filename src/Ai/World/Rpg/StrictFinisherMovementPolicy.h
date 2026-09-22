/*
 * Pure provenance policy for deterministic exact-finisher movement.
 */
#ifndef PLAYERBOTS_STRICT_FINISHER_MOVEMENT_POLICY_H
#define PLAYERBOTS_STRICT_FINISHER_MOVEMENT_POLICY_H

#include <cstdint>

namespace StrictFinisherMovementPolicy
{
[[nodiscard]] inline constexpr bool IsEnabled(bool oracleFinisherAuthorized,
                                               std::uint64_t decisionId) noexcept
{
    return oracleFinisherAuthorized && decisionId != 0;
}

[[nodiscard]] inline constexpr bool UseQuestNoTeleport(bool oracleFinisherAuthorized,
                                                        std::uint64_t decisionId) noexcept
{
    return IsEnabled(oracleFinisherAuthorized, decisionId);
}

[[nodiscard]] inline constexpr bool AllowLegacyTeleportRecovery(bool oracleFinisherAuthorized,
                                                                 std::uint64_t decisionId) noexcept
{
    return !IsEnabled(oracleFinisherAuthorized, decisionId);
}

struct RouteIdentity
{
    std::uint64_t decisionId = 0;
    std::uint32_t questId = 0;
    std::uint64_t stableFinisherGuid = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return questId != 0 && stableFinisherGuid != 0;
    }
};

struct LastMovementFacts
{
    bool active = false;
    std::uint32_t issuedAtMs = 0;
    std::uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Provenance
{
    bool issued = false;
    RouteIdentity route;
    LastMovementFacts movement;
};

enum class Decision : std::uint8_t
{
    NoInheritedMovement,
    ContinueCurrentStrictRoute,
    RejectAndClearInherited
};

[[nodiscard]] inline constexpr bool SameRoute(RouteIdentity const& left,
                                               RouteIdentity const& right) noexcept
{
    return left.IsValid() && right.IsValid() && left.decisionId == right.decisionId &&
           left.questId == right.questId && left.stableFinisherGuid == right.stableFinisherGuid;
}

[[nodiscard]] inline constexpr bool SameMovement(LastMovementFacts const& left,
                                                  LastMovementFacts const& right) noexcept
{
    return left.issuedAtMs == right.issuedAtMs && left.mapId == right.mapId && left.x == right.x &&
           left.y == right.y && left.z == right.z;
}

[[nodiscard]] inline constexpr Decision Evaluate(RouteIdentity const& current,
                                                  LastMovementFacts const& observed,
                                                  Provenance const& provenance) noexcept
{
    if (!observed.active)
        return Decision::NoInheritedMovement;
    if (provenance.issued && SameRoute(current, provenance.route) &&
        SameMovement(observed, provenance.movement))
        return Decision::ContinueCurrentStrictRoute;
    return Decision::RejectAndClearInherited;
}
}

#endif
