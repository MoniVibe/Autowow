/*
 * World-thread-only native gate for Oracle-owned Playerbot actions.
 */
#include "AutoWowOracleOwnershipGate.h"

#include "GameTime.h"

#include <array>
#include <charconv>

namespace AutoWowOracleRuntime
{
namespace
{
struct OwnershipSlot
{
    bool occupied = false;
    AutoWowOracle::Guid botGuid = 0;
    AutoWowOracle::DecisionId decisionId = 0;
    AutoWowOracle::Tick expires = 0;
    bool gatherBound = false;
    AutoWowOracle::GatherSourceReference gather;
};

std::array<OwnershipSlot, kMaxOracleOwnedBots> ownershipSlots{};
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

void ReapExpired(AutoWowOracle::Tick now) noexcept
{
    for (OwnershipSlot& slot : ownershipSlots)
        if (slot.occupied && now >= slot.expires)
            slot = {};
}

std::size_t FindBot(AutoWowOracle::Guid botGuid) noexcept
{
    for (std::size_t index = 0; index < ownershipSlots.size(); ++index)
        if (ownershipSlots[index].occupied && ownershipSlots[index].botGuid == botGuid)
            return index;
    return kNotFound;
}

std::size_t FindFree() noexcept
{
    for (std::size_t index = 0; index < ownershipSlots.size(); ++index)
        if (!ownershipSlots[index].occupied)
            return index;
    return kNotFound;
}
}

bool Claim(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
           AutoWowOracle::Tick expires) noexcept
{
    if (botGuid == 0 || decisionId == 0 || expires == 0)
        return false;

    ReapExpired(CurrentTick());
    std::size_t index = FindBot(botGuid);
    if (index == kNotFound)
        index = FindFree();
    if (index == kNotFound)
        return false;

    ownershipSlots[index] = {true, botGuid, decisionId, expires, false, {}};
    return true;
}

bool ClaimGather(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
                 AutoWowOracle::GatherSourceReference const& source,
                 AutoWowOracle::Tick expires) noexcept
{
    if (botGuid == 0 || decisionId == 0 || expires == 0 ||
        !AutoWowOracle::ValidGatherSourceReference(source))
        return false;

    ReapExpired(CurrentTick());
    std::size_t index = FindBot(botGuid);
    if (index == kNotFound)
        index = FindFree();
    if (index == kNotFound)
        return false;

    ownershipSlots[index] = {true, botGuid, decisionId, expires, true, source};
    return true;
}

bool Release(AutoWowOracle::Guid botGuid) noexcept
{
    if (botGuid == 0)
        return false;

    ReapExpired(CurrentTick());
    std::size_t const index = FindBot(botGuid);
    if (index == kNotFound)
        return false;

    ownershipSlots[index] = {};
    return true;
}

bool Owns(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
          AutoWowOracle::Tick now) noexcept
{
    if (botGuid == 0 || decisionId == 0)
        return false;

    ReapExpired(now);
    std::size_t const index = FindBot(botGuid);
    return index != kNotFound && ownershipSlots[index].decisionId == decisionId;
}

bool IsOwned(AutoWowOracle::Guid botGuid, AutoWowOracle::Tick now) noexcept
{
    if (botGuid == 0)
        return false;

    ReapExpired(now);
    return FindBot(botGuid) != kNotFound;
}

bool OwnsGatherSource(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
                      AutoWowOracle::GatherSourceReference const& source,
                      AutoWowOracle::Tick now) noexcept
{
    if (botGuid == 0 || decisionId == 0 || !AutoWowOracle::ValidGatherSourceReference(source))
        return false;

    ReapExpired(now);
    std::size_t const index = FindBot(botGuid);
    return index != kNotFound && ownershipSlots[index].decisionId == decisionId &&
        ownershipSlots[index].gatherBound &&
        AutoWowOracle::SameGatherSourceReference(ownershipSlots[index].gather, source);
}

bool OwnsGatherSource(AutoWowOracle::Guid botGuid,
                      AutoWowOracle::GatherSourceReference const& source,
                      AutoWowOracle::Tick now) noexcept
{
    if (botGuid == 0 || !AutoWowOracle::ValidGatherSourceReference(source))
        return false;

    ReapExpired(now);
    std::size_t const index = FindBot(botGuid);
    return index != kNotFound && ownershipSlots[index].gatherBound &&
        AutoWowOracle::SameGatherSourceReference(ownershipSlots[index].gather, source);
}

std::string_view TaggedEventSource() noexcept
{
    return "autowow.oracle";
}

AutoWowOracle::Tick CurrentTick() noexcept
{
    return static_cast<AutoWowOracle::Tick>(GameTime::GetGameTimeMS().count());
}

bool ParseDecisionId(std::string_view parameter, AutoWowOracle::DecisionId& out) noexcept
{
    if (parameter.empty())
        return false;

    for (char const character : parameter)
        if (character < '0' || character > '9')
            return false;

    AutoWowOracle::DecisionId parsed = 0;
    auto const result = std::from_chars(parameter.data(), parameter.data() + parameter.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != parameter.data() + parameter.size() || parsed == 0)
        return false;

    out = parsed;
    return true;
}
}
