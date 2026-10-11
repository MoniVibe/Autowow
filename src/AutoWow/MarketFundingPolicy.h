/*
 * This file is part of the mod-playerbots project.
 * Released under the GNU GPL v2 license, or (at your option) any later version.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace AutoWowSupply
{
[[nodiscard]] inline std::uint64_t MarketTopUp(std::uint64_t cost, std::uint64_t cash, std::uint64_t keep)
{
    std::uint64_t const available = cash > keep ? cash - keep : 0;
    return cost > available ? cost - available : 0;
}

[[nodiscard]] inline std::uint64_t MarketRefund(std::uint64_t topUp, std::uint64_t cost,
                                               std::uint64_t spent, std::uint64_t cash)
{
    return std::min({topUp, cost > spent ? cost - spent : 0, cash});
}
}  // namespace AutoWowSupply
