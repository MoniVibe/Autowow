#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_EXACT_HANDOFF_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_EXACT_HANDOFF_POLICY_H

#include <cstdint>

namespace AutoWowOracleExactHandoff
{
// World-facing checks (spec membership and live object identity) are supplied by the runtime.
// A phase change alone is never proof that the same exact source remains authorized.
struct Facts
{
    bool pinned = false;
    bool sourceInObjective = false;
    bool runtimeTargetMatches = false;
    bool arrived = false;
    bool blocked = false;
    bool routeFailed = false;
    std::uint8_t oldPhase = 0;
    std::uint8_t newPhase = 0;
    std::uint32_t oldQuest = 0;
    std::uint32_t newQuest = 0;
    std::uint8_t oldFamily = 0;
    std::uint8_t newFamily = 0;
    std::uint8_t oldSlot = 0;
    std::uint8_t newSlot = 0;
    std::uint32_t routeEntry = 0;
    std::uint32_t selectedEntry = 0;
    std::uint64_t routeSpawn = 0;
    std::uint64_t selectedSpawn = 0;
    std::uint32_t routeMap = 0;
    std::uint32_t currentMap = 0;
    std::uint32_t routeInstance = 0;
    std::uint32_t currentInstance = 0;
    std::uint64_t oldDecision = 0;
    std::uint64_t routeDecision = 0;
};

[[nodiscard]] inline constexpr bool ExactSourceProof(Facts const& f) noexcept
{
    return f.pinned && f.sourceInObjective && f.runtimeTargetMatches && f.arrived &&
        !f.blocked && !f.routeFailed && f.oldQuest != 0 && f.oldQuest == f.newQuest &&
        f.oldFamily == f.newFamily && f.oldSlot == f.newSlot && f.routeEntry != 0 &&
        f.routeEntry == f.selectedEntry && f.routeSpawn != 0 &&
        f.routeSpawn == f.selectedSpawn && f.routeMap == f.currentMap &&
        f.routeInstance == f.currentInstance && f.oldDecision != 0 &&
        f.oldDecision == f.routeDecision;
}

[[nodiscard]] inline constexpr bool SameArrivedSource(Facts const& f,
    std::uint8_t travelPhase, std::uint8_t acquirePhase) noexcept
{
    return ExactSourceProof(f) && f.oldPhase == travelPhase &&
        f.newPhase == acquirePhase;
}

// Keep the objective target stable during one native Acquire step, including its post-step
// observation after a successful attack enters Engage. The next cadence still releases the
// changed-phase lease under the runtime's strict SameIntentIdentity rule.
[[nodiscard]] inline constexpr bool UsePinnedTarget(Facts const& f,
    std::uint8_t travelPhase, std::uint8_t acquirePhase,
    std::uint8_t engagePhase) noexcept
{
    return ExactSourceProof(f) &&
        ((f.oldPhase == travelPhase && f.newPhase == acquirePhase) ||
         (f.oldPhase == acquirePhase &&
             (f.newPhase == acquirePhase || f.newPhase == engagePhase)));
}

[[nodiscard]] inline constexpr std::uint64_t SelectLiveTarget(
    std::uint64_t genericTarget, Facts const& f, bool validPinnedTarget) noexcept
{
    return validPinnedTarget ? f.selectedSpawn : genericTarget;
}

[[nodiscard]] inline constexpr bool MayRestore(Facts const& f,
    std::uint8_t travelPhase, std::uint8_t acquirePhase,
    bool authoritativeRelease, bool newLeaseGranted,
    std::uint64_t newDecision, std::uint64_t newDecisionTarget) noexcept
{
    return SameArrivedSource(f, travelPhase, acquirePhase) &&
        authoritativeRelease && newLeaseGranted && newDecision != 0 &&
        newDecision != f.oldDecision && newDecisionTarget == f.selectedSpawn;
}
} // namespace AutoWowOracleExactHandoff

#endif
