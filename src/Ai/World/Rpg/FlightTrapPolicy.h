/*
 * Flight-master trap exit (AutoWow.Travel.FlightTrapFix, default off).
 *
 * Root cause (soak-s17-full-r1, research/XP_LIMITS_S17.md): RPG_TRAVEL_FLIGHT had no failure exit. Its
 * action only returned MoveFarTo(flight master) and its status case only left on flight arrival, so a
 * bot whose walk to the flight master gave up (5,369 cohort `intent_replan_exhausted` give-ups aimed at
 * the Thelsamar, Silvermoon, Sepulcher, Thunder Bluff, Booty Bay, Tranquillien and Ironforge masters)
 * re-planned the same point every ~9 s forever. Four paths enter the status: the random pick, the
 * death-loop relocation, errands and zone progression; all four pick the master through
 * TravelMgr::GetNearestFlightMasterInfo.
 *
 * With the flag on:
 *   - the approach exits when the travel intent gives up, or when the status has run TimeoutMs without
 *     boarding (Decide);
 *   - the abandoned master (creature template entry) is blocked for this bot for CooldownMs, and
 *     GetNearestFlightMasterInfo skips blocked masters, so every entry path picks another master or none
 *     (errands and zone progression then walk their leg);
 *   - the bot returns to Idle.
 *
 * Pure: wrap-safe uint32 milliseconds (getMSTime), fixed-capacity book, index tie-breaks; no floats,
 * RNG or world access. The per-bot books below the policy are the runtime store (map threads -> lock).
 */
#ifndef PLAYERBOTS_FLIGHT_TRAP_POLICY_H
#define PLAYERBOTS_FLIGHT_TRAP_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace FlightTrapPolicy
{
inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::size_t kBookSize = 8;  // distinct masters one bot can have blocked at once

struct Entry
{
    std::uint32_t fmEntry = 0;  // creature template entry of the flight master; 0 = free slot
    std::uint32_t untilMs = 0;  // getMSTime() at which the block lapses
};

struct Book
{
    std::uint8_t version = kPolicyVersion;
    std::array<Entry, kBookSize> entries{};
};

// Wrap-safe: `untilMs` is still ahead of `nowMs` (valid for cooldowns below 2^31 ms).
[[nodiscard]] inline bool Pending(std::uint32_t untilMs, std::uint32_t nowMs)
{
    return static_cast<std::int32_t>(untilMs - nowMs) > 0;
}

[[nodiscard]] inline bool IsBlocked(Book const& book, std::uint32_t fmEntry, std::uint32_t nowMs)
{
    if (fmEntry == 0)
        return false;
    for (Entry const& e : book.entries)
        if (e.fmEntry == fmEntry && Pending(e.untilMs, nowMs))
            return true;
    return false;
}

// Blocks `fmEntry` until nowMs + cooldownMs. Slot order: the master's own slot, else the first free or
// lapsed slot, else the slot that lapses soonest (lowest index on ties).
inline void Block(Book& book, std::uint32_t fmEntry, std::uint32_t nowMs, std::uint32_t cooldownMs)
{
    if (fmEntry == 0)
        return;
    std::size_t slot = kBookSize;
    for (std::size_t i = 0; i < kBookSize && slot == kBookSize; ++i)
        if (book.entries[i].fmEntry == fmEntry)
            slot = i;
    for (std::size_t i = 0; i < kBookSize && slot == kBookSize; ++i)
        if (book.entries[i].fmEntry == 0 || !Pending(book.entries[i].untilMs, nowMs))
            slot = i;
    if (slot == kBookSize)
    {
        slot = 0;
        for (std::size_t i = 1; i < kBookSize; ++i)
            if (static_cast<std::uint32_t>(book.entries[i].untilMs - nowMs) <
                static_cast<std::uint32_t>(book.entries[slot].untilMs - nowMs))
                slot = i;
    }
    book.entries[slot] = {fmEntry, nowMs + cooldownMs};
}

enum class Exit : std::uint8_t
{
    Stay,      // keep approaching (or flying)
    GaveUp,    // the travel intent gave up on the master (ledger-style reason approach_gave_up)
    TimedOut   // the status ran timeoutMs without boarding (reason approach_timeout)
};

// Not in flight: a give-up exits at once, else the approach is bounded by timeoutMs (0 = no bound).
[[nodiscard]] inline Exit Decide(bool inFlight, bool approachGaveUp, std::uint32_t statusMs, std::uint32_t timeoutMs)
{
    if (inFlight)
        return Exit::Stay;
    if (approachGaveUp)
        return Exit::GaveUp;
    return timeoutMs != 0 && statusMs > timeoutMs ? Exit::TimedOut : Exit::Stay;
}

[[nodiscard]] inline char const* ExitName(Exit exit)
{
    switch (exit)
    {
        case Exit::GaveUp: return "approach_gave_up";
        case Exit::TimedOut: return "approach_timeout";
        default: return "stay";
    }
}
}  // namespace FlightTrapPolicy

// ---- runtime -----------------------------------------------------------------------------------------
namespace AutoWowFlightTrap
{
namespace detail
{
inline bool gEnabled = false;                // AutoWow.Travel.FlightTrapFix
inline std::uint32_t gTimeoutMs = 600000;    // AutoWow.Travel.FlightTrapFix.TimeoutMs
inline std::uint32_t gCooldownMs = 1800000;  // AutoWow.Travel.FlightTrapFix.CooldownMs
// ponytail: one global lock; touched only with the flag on (master lookup + abandon).
inline std::mutex gLock;
inline std::unordered_map<std::uint32_t, FlightTrapPolicy::Book> gBooks;  // persistent bot GUID counter
}  // namespace detail

inline bool Enabled() { return detail::gEnabled; }

inline bool IsBlocked(std::uint32_t botGuid, std::uint32_t fmEntry, std::uint32_t nowMs)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    auto const it = detail::gBooks.find(botGuid);
    return it != detail::gBooks.end() && FlightTrapPolicy::IsBlocked(it->second, fmEntry, nowMs);
}

inline void Block(std::uint32_t botGuid, std::uint32_t fmEntry, std::uint32_t nowMs)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    FlightTrapPolicy::Block(detail::gBooks[botGuid], fmEntry, nowMs, detail::gCooldownMs);
}
}  // namespace AutoWowFlightTrap

#endif  // PLAYERBOTS_FLIGHT_TRAP_POLICY_H
