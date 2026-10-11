/*
 * AutoWoW strategic server module. Original custom implementation.
 * Released under GNU GPL v2, or (at your option) any later version.
 */

#ifndef AUTOWOW_STRATEGY_POLICY_H
#define AUTOWOW_STRATEGY_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>

namespace AutoWowStrategy
{
struct IncomeWindow
{
    bool valid = false;
    std::uint64_t firstPeriod = 0;
    std::uint64_t lastPeriod = 0;
    std::uint64_t skippedPeriods = 0;
};

// Caller supplies a persisted last-settled cursor. This policy never creates items or advances that cursor.
inline IncomeWindow PlanIncome(std::uint64_t lastSettled, std::uint64_t unixSeconds,
                               std::uint32_t periodSeconds, std::uint32_t maxCatchUp)
{
    if (!periodSeconds || !maxCatchUp)
        return {};

    std::uint64_t const completed = unixSeconds / periodSeconds;
    if (completed < lastSettled)
        return {};

    if (completed == lastSettled)
        return {true, 0, 0, 0};

    std::uint64_t const due = completed - lastSettled;
    std::uint64_t const admitted = std::min(due, static_cast<std::uint64_t>(maxCatchUp));
    return {true, completed - admitted + 1, completed, due - admitted};
}

enum class GloryReason
{
    Eligible,
    Disabled,
    Self,
    SameAccount,
    Friendly,
    Unqualified,
    Duel,
    Arena,
    GreyTarget,
    NoCredit,
    RepeatedOpponent,
    PairCooldown,
    DailyCap,
    ClockBackwards
};

inline char const* GloryReasonName(GloryReason reason)
{
    switch (reason)
    {
        case GloryReason::Eligible: return "eligible";
        case GloryReason::Disabled: return "disabled";
        case GloryReason::Self: return "self";
        case GloryReason::SameAccount: return "same_account";
        case GloryReason::Friendly: return "friendly";
        case GloryReason::Unqualified: return "unqualified";
        case GloryReason::Duel: return "duel";
        case GloryReason::Arena: return "arena";
        case GloryReason::GreyTarget: return "grey_target";
        case GloryReason::NoCredit: return "no_credit";
        case GloryReason::RepeatedOpponent: return "repeated_opponent";
        case GloryReason::PairCooldown: return "pair_cooldown";
        case GloryReason::DailyCap: return "daily_cap";
        case GloryReason::ClockBackwards: return "clock_backwards";
    }

    return "unqualified";
}

struct GloryFacts
{
    bool enabled = false;
    bool self = false;
    bool sameAccount = false;
    bool bothHeadless = false;
    bool hostile = false;
    bool sameContext = false;
    bool duel = false;
    bool arena = false;
    bool noCredit = false;
    std::uint8_t victimLevel = 0;
    std::uint8_t greyLevel = 0;
};

struct GloryLimits
{
    std::uint32_t dailyPoints = 50;
    std::uint32_t pairCooldownSeconds = 600;
};

struct GloryDecision
{
    GloryReason reason = GloryReason::Unqualified;
    std::uint32_t candidatePoints = 0;
};

// Pair counts are unordered: A killing B then B killing A consumes the same repeated-opponent allowance.
// Headless bots may share an account. They still obey pair and daily limits; humans cannot use that exception.
inline GloryDecision EvaluateGlory(GloryFacts const& facts, GloryLimits const& limits,
                                   std::uint32_t pairCount, std::uint32_t pointsToday,
                                   std::uint64_t nowSeconds, std::uint64_t lastPairSeconds)
{
    if (!facts.enabled)
        return {GloryReason::Disabled, 0};
    if (facts.self)
        return {GloryReason::Self, 0};
    if (facts.sameAccount && !facts.bothHeadless)
        return {GloryReason::SameAccount, 0};
    if (!facts.hostile)
        return {GloryReason::Friendly, 0};
    if (!facts.sameContext || !facts.victimLevel)
        return {GloryReason::Unqualified, 0};
    if (facts.duel)
        return {GloryReason::Duel, 0};
    if (facts.arena)
        return {GloryReason::Arena, 0};
    if (facts.victimLevel <= facts.greyLevel)
        return {GloryReason::GreyTarget, 0};
    if (facts.noCredit)
        return {GloryReason::NoCredit, 0};

    constexpr std::array<std::uint32_t, 3> points{10, 5, 2};
    if (pairCount >= points.size())
        return {GloryReason::RepeatedOpponent, 0};
    if (pointsToday >= limits.dailyPoints)
        return {GloryReason::DailyCap, 0};
    if (pairCount && nowSeconds < lastPairSeconds)
        return {GloryReason::ClockBackwards, 0};
    if (pairCount && nowSeconds - lastPairSeconds < limits.pairCooldownSeconds)
        return {GloryReason::PairCooldown, 0};

    return {GloryReason::Eligible, std::min(points[pairCount], limits.dailyPoints - pointsToday)};
}
} // namespace AutoWowStrategy

#endif
