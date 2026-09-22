/*
 * World-thread-only native gate for Oracle-owned Playerbot actions.
 *
 * The registry is intentionally bounded and has no ownership of live game objects. Callers must
 * invoke it from the world thread; it is not a cross-thread synchronization primitive.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_OWNERSHIP_GATE_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_OWNERSHIP_GATE_H

#include "AutoWowOracleContract.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AutoWowOracleRuntime
{
inline constexpr std::size_t kMaxOracleOwnedBots = AutoWowOracle::kMaxBotLeases;
static_assert(kMaxOracleOwnedBots <= AutoWowOracle::kMaxBotLeases);

// Claim replaces any existing claim for the same bot (preemption). A zero bot, decision, or expiry
// is invalid. Expiry is exclusive: the claim is live only while now < expires.
bool Claim(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
           AutoWowOracle::Tick expires) noexcept;

// Gather claims bind native authority to the full published source identity. An unrelated Oracle
// decision for the same bot cannot activate the fixed-target gather executor.
bool ClaimGather(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
                 AutoWowOracle::GatherSourceReference const& source,
                 AutoWowOracle::Tick expires) noexcept;

// Release removes the current claim for botGuid and reports whether one was present.
bool Release(AutoWowOracle::Guid botGuid) noexcept;

// Ownership queries reap expired entries before looking up the requested claim.
bool Owns(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
          AutoWowOracle::Tick now) noexcept;
bool OwnsGatherSource(AutoWowOracle::Guid botGuid, std::uint64_t decisionId,
                      AutoWowOracle::GatherSourceReference const& source,
                      AutoWowOracle::Tick now) noexcept;
bool OwnsGatherSource(AutoWowOracle::Guid botGuid,
                      AutoWowOracle::GatherSourceReference const& source,
                      AutoWowOracle::Tick now) noexcept;
bool IsOwned(AutoWowOracle::Guid botGuid, AutoWowOracle::Tick now) noexcept;

// Stable event source tag required by NewRpgDoQuestAction for an owned bot.
std::string_view TaggedEventSource() noexcept;

// Current world/server monotonic tick used by the native action gate.
AutoWowOracle::Tick CurrentTick() noexcept;

// Strict, whole-parameter decimal parser for the event decision id. It accepts digits only and
// leaves out unchanged on failure; zero is not a valid decision id.
bool ParseDecisionId(std::string_view parameter, AutoWowOracle::DecisionId& out) noexcept;
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_OWNERSHIP_GATE_H
