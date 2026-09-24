/*
 * Flight-master trap exit (FlightTrapPolicy.h, AutoWow.Travel.FlightTrapFix): per-bot block book and the
 * TravelFlight exit table. Pure: no world access, explicit getMSTime-style clocks.
 */

#include "FlightTrapPolicy.h"

#include "gtest/gtest.h"

#include <cstdint>

namespace
{
using namespace FlightTrapPolicy;

TEST(FlightTrapPolicyTest, BlockLastsExactlyTheCooldown)
{
    Book book;
    EXPECT_FALSE(IsBlocked(book, 3838, 1000));
    Block(book, 3838, 1000, 500);
    EXPECT_TRUE(IsBlocked(book, 3838, 1000));
    EXPECT_TRUE(IsBlocked(book, 3838, 1499));
    EXPECT_FALSE(IsBlocked(book, 3838, 1500));  // lapses at now + cooldown
    EXPECT_FALSE(IsBlocked(book, 1572, 1200));  // other masters untouched
}

TEST(FlightTrapPolicyTest, EntryZeroIsNeverBlocked)
{
    Book book;
    Block(book, 0, 1000, 500);
    EXPECT_FALSE(IsBlocked(book, 0, 1000));
    for (Entry const& e : book.entries)
        EXPECT_EQ(e.fmEntry, 0u);
}

TEST(FlightTrapPolicyTest, ClockWrapIsSafe)
{
    Book book;
    std::uint32_t const now = 0xFFFFFF00u;
    Block(book, 1387, now, 0x200);  // lapses past the uint32 wrap
    EXPECT_TRUE(IsBlocked(book, 1387, now));
    EXPECT_TRUE(IsBlocked(book, 1387, 0x000000F0u));
    EXPECT_FALSE(IsBlocked(book, 1387, 0x00000100u));
}

TEST(FlightTrapPolicyTest, ReblockRefreshesTheSameSlot)
{
    Book book;
    Block(book, 3838, 1000, 500);
    Block(book, 3838, 1400, 500);
    EXPECT_TRUE(IsBlocked(book, 3838, 1800));
    std::size_t slots = 0;
    for (Entry const& e : book.entries)
        slots += e.fmEntry == 3838 ? 1 : 0;
    EXPECT_EQ(slots, 1u);
}

TEST(FlightTrapPolicyTest, LapsedSlotIsReusedBeforeEviction)
{
    Book book;
    for (std::uint32_t i = 0; i < kBookSize; ++i)
        Block(book, 100 + i, 1000, i == 3 ? 10 : 1000);
    Block(book, 999, 1100, 1000);  // slot 3 lapsed at 1010
    EXPECT_EQ(book.entries[3].fmEntry, 999u);
    for (std::uint32_t i = 0; i < kBookSize; ++i)
        if (i != 3)
            EXPECT_TRUE(IsBlocked(book, 100 + i, 1100));
}

TEST(FlightTrapPolicyTest, FullBookEvictsTheSoonestLapseLowestIndexOnTies)
{
    Book book;
    for (std::uint32_t i = 0; i < kBookSize; ++i)
        Block(book, 100 + i, 1000, 5000 - (i == 2 || i == 5 ? 4000 : 0));  // slots 2 and 5 lapse first
    Block(book, 999, 1100, 1000);
    EXPECT_EQ(book.entries[2].fmEntry, 999u);
    EXPECT_EQ(book.entries[5].fmEntry, 105u);
    EXPECT_FALSE(IsBlocked(book, 102, 1100));
    EXPECT_TRUE(IsBlocked(book, 105, 1100));
}

TEST(FlightTrapPolicyTest, ExitTable)
{
    // In flight: never an exit, whatever the clock or the give-up says.
    EXPECT_EQ(Decide(true, true, 1u << 30, 1000), Exit::Stay);
    // Approach: a give-up exits at once.
    EXPECT_EQ(Decide(false, true, 0, 600000), Exit::GaveUp);
    // Approach: bounded by the timeout (strictly longer, as HasStatusPersisted).
    EXPECT_EQ(Decide(false, false, 600000, 600000), Exit::Stay);
    EXPECT_EQ(Decide(false, false, 600001, 600000), Exit::TimedOut);
    // Timeout 0 = unbounded.
    EXPECT_EQ(Decide(false, false, 1u << 30, 0), Exit::Stay);
    EXPECT_STREQ(ExitName(Exit::GaveUp), "approach_gave_up");
    EXPECT_STREQ(ExitName(Exit::TimedOut), "approach_timeout");
}

TEST(FlightTrapPolicyTest, RuntimeBooksArePerBot)
{
    std::uint32_t const saved = AutoWowFlightTrap::detail::gCooldownMs;
    AutoWowFlightTrap::detail::gCooldownMs = 1000;
    AutoWowFlightTrap::Block(900001, 3838, 5000);
    EXPECT_TRUE(AutoWowFlightTrap::IsBlocked(900001, 3838, 5999));
    EXPECT_FALSE(AutoWowFlightTrap::IsBlocked(900001, 3838, 6000));
    EXPECT_FALSE(AutoWowFlightTrap::IsBlocked(900002, 3838, 5500));  // another bot is free to use it
    AutoWowFlightTrap::detail::gCooldownMs = saved;
}
}  // namespace
