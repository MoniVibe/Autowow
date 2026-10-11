/* AutoWoW original native treasury policy. GPL v2 or later. */
#ifndef AUTOWOW_TREASURY_POLICY_H
#define AUTOWOW_TREASURY_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
// Aggregate conservation projection for one item/faction, not a second spendable bank.
// BankPolicy supplies individual ownership; the persistence adapter must derive this view from those records.
struct ResourceStock
{
    std::uint64_t produced = 0;
    std::uint64_t treasury = 0;
    std::uint64_t reserved = 0;
    std::uint64_t guild = 0;
    std::uint64_t consumed = 0;
    std::uint64_t invested = 0;
    std::uint64_t overflow = 0;
};

inline bool Conserved(ResourceStock const& stock)
{
    std::uint64_t total = 0;
    for (std::uint64_t const value : std::array<std::uint64_t, 6>{
             stock.treasury, stock.reserved, stock.guild, stock.consumed, stock.invested, stock.overflow})
    {
        if (value > std::numeric_limits<std::uint64_t>::max() - total)
            return false;
        total += value;
    }

    return total == stock.produced;
}

enum class StockMove
{
    ReserveForGuild,
    DeliverToGuild,
    ConsumeByWorkshop,
    InvestInTerritory
};

struct StockDecision
{
    bool accepted = false;
    ResourceStock next;
    std::uint64_t credited = 0;
    std::uint64_t overflow = 0;
};

// Return a proposed state only. Durable event identity and atomic commit belong to the repository adapter.
inline StockDecision PlanProduction(ResourceStock const& prior, std::uint64_t income, std::uint64_t cap)
{
    if (!Conserved(prior) || prior.treasury > cap ||
        income > std::numeric_limits<std::uint64_t>::max() - prior.produced)
        return {};

    ResourceStock next = prior;
    std::uint64_t const credited = std::min(income, cap - prior.treasury);
    std::uint64_t const overflow = income - credited;
    next.produced += income;
    next.treasury += credited;
    next.overflow += overflow;
    return {true, next, credited, overflow};
}

inline StockDecision PlanTransfer(ResourceStock const& prior, StockMove move, std::uint64_t quantity)
{
    if (!quantity || !Conserved(prior))
        return {};

    ResourceStock next = prior;
    switch (move)
    {
        case StockMove::ReserveForGuild:
            if (quantity > next.treasury)
                return {};
            next.treasury -= quantity;
            next.reserved += quantity;
            break;
        case StockMove::DeliverToGuild:
            if (quantity > next.reserved)
                return {};
            next.reserved -= quantity;
            next.guild += quantity;
            break;
        case StockMove::ConsumeByWorkshop:
            if (quantity > next.guild)
                return {};
            next.guild -= quantity;
            next.consumed += quantity;
            break;
        case StockMove::InvestInTerritory:
            if (quantity > next.treasury)
                return {};
            next.treasury -= quantity;
            next.invested += quantity;
            break;
        default:
            return {};
    }

    // Conservation of a previously valid uint64 total also proves destination additions cannot overflow.
    return {Conserved(next), next, 0, 0};
}
} // namespace AutoWowStrategy

#endif
