/*
 * Pure dispatch authority for Oracle-managed quest work.
 *
 * This policy has no world, Playerbot, movement, quest mutation, or clock access. Runtime/action
 * adapters publish the current ownership facts; the result decides whether ordinary New-RPG may
 * run, an exact tagged Oracle event may run, or the action must fail closed.
 */
#ifndef MOD_PLAYERBOTS_ORACLE_QUEST_DISPATCH_POLICY_H
#define MOD_PLAYERBOTS_ORACLE_QUEST_DISPATCH_POLICY_H

#include <cstdint>

namespace AutoWowOracleQuestDispatchPolicy
{
using DecisionId = std::uint64_t;

enum class WorkKind : std::uint8_t
{
    Objective,
    Finisher
};

enum class Result : std::uint8_t
{
    Reject,
    Ordinary,
    OracleTagged
};

struct OwnershipState
{
    bool managed = false;
    bool leaseRequired = false;
    bool activeLease = false;
    bool gateOwned = false;
    DecisionId activeDecisionId = 0;
};

struct EventState
{
    bool tagged = false;
    bool decisionParsed = false;
    DecisionId decisionId = 0;
    WorkKind work = WorkKind::Objective;
    std::int32_t signedFinisherEntry = 0;
};

[[nodiscard]] inline constexpr OwnershipState Acquired(bool managed, DecisionId decisionId) noexcept
{
    return {managed, decisionId != 0, decisionId != 0, decisionId != 0, decisionId};
}

[[nodiscard]] inline constexpr OwnershipState TerminalRelease(OwnershipState state) noexcept
{
    state.leaseRequired = false;
    state.activeLease = false;
    state.gateOwned = false;
    state.activeDecisionId = 0;
    return state;
}

[[nodiscard]] inline constexpr Result Evaluate(OwnershipState const& ownership,
                                                EventState const& event) noexcept
{
    bool const authorityExpected = ownership.managed || ownership.leaseRequired ||
                                   ownership.activeLease || ownership.gateOwned;
    if (!authorityExpected)
        return event.tagged ? Result::Reject : Result::Ordinary;

    // This is the crucial initially-unleased state: configured Oracle bots already have a DoQuest
    // directive, but only the runtime may acquire authority and issue the first tagged step.
    if (!ownership.leaseRequired || !ownership.activeLease || !ownership.gateOwned)
        return Result::Reject;

    if (!event.tagged || !event.decisionParsed || event.decisionId == 0 ||
        event.decisionId != ownership.activeDecisionId)
        return Result::Reject;

    if (event.work == WorkKind::Finisher && event.signedFinisherEntry == 0)
        return Result::Reject;

    return Result::OracleTagged;
}

// Baseline New-RPG questgiver scanning/log organization is deliberately available only to an
// ordinary unmanaged dispatch. Managed, tagged, malformed, and initially-unleased events cannot
// enter that broad legacy maintenance path.
[[nodiscard]] inline constexpr bool AllowsLegacyQuestMaintenance(Result result) noexcept
{
    return result == Result::Ordinary;
}
}

#endif
