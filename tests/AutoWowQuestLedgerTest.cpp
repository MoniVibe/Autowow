/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowQuestLedger.h"

#include "gtest/gtest.h"

namespace
{
using AutoWowQuestLedger::Event;
using AutoWowQuestLedger::Row;

TEST(AutoWowQuestLedgerTest, FormatsSchemaV1InStableFieldOrder)
{
    Row row;
    row.ev = Event::Blocked;
    row.ms = 1758600000123ull;
    row.bot = 4242;
    row.team = 1;
    row.level = 12;
    row.quest = 835;
    row.map = 1;
    row.zone = 14;
    row.x = -601;
    row.y = -4250;
    row.c[0] = 3;
    row.c[3] = 1;
    row.i[5] = 7;
    row.reason = "no_live_candidate";
    row.phase = "acquire_target";

    EXPECT_EQ(AutoWowQuestLedger::FormatLine("soak-01", row),
              "{\"v\":1,\"run\":\"soak-01\",\"ms\":1758600000123,\"ev\":\"blocked\",\"bot\":4242,"
              "\"team\":1,\"lvl\":12,\"quest\":835,\"map\":1,\"zone\":14,\"x\":-601,\"y\":-4250,"
              "\"c\":[3,0,0,1],\"i\":[0,0,0,0,0,7],\"reason\":\"no_live_candidate\","
              "\"phase\":\"acquire_target\"}");
}

TEST(AutoWowQuestLedgerTest, DefaultRowHasEmptyStringsAndZeroCounters)
{
    Row row;
    row.reason = nullptr;
    row.phase = nullptr;
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("", row),
              "{\"v\":1,\"run\":\"\",\"ms\":0,\"ev\":\"accepted\",\"bot\":0,\"team\":0,\"lvl\":0,"
              "\"quest\":0,\"map\":0,\"zone\":0,\"x\":0,\"y\":0,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"\",\"phase\":\"\"}");
}

TEST(AutoWowQuestLedgerTest, EscapesRunIdSoLineStaysOneJsonObject)
{
    Row row;
    std::string const line = AutoWowQuestLedger::FormatLine("a\"b\\c\nd", row);
    EXPECT_NE(line.find("\"run\":\"a\\\"b\\\\c\\u000ad\""), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(AutoWowQuestLedgerTest, EventNamesAreWireStable)
{
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Accepted), "accepted");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Rewarded), "rewarded");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Abandoned), "abandoned");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Blocked), "blocked");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Deferred), "deferred");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Contaminated), "contaminated");
    EXPECT_EQ(static_cast<int>(Event::Contaminated), 5);
    EXPECT_EQ(AutoWowQuestLedger::kSchemaVersion, 1u);
}

TEST(AutoWowQuestLedgerTest, DisabledByDefault)
{
    EXPECT_FALSE(AutoWowQuestLedger::Enabled());
}

TEST(AutoWowQuestLedgerTest, BlockedCountFieldIsTrailingAndOmittedWhenZero)
{
    Row row;
    row.ev = Event::Blocked;
    row.reason = "inventory_full";
    row.phase = "resolve_objective";
    std::string const plain = AutoWowQuestLedger::FormatLine("r", row);
    EXPECT_EQ(plain.find("\"n\":"), std::string::npos);
    row.n = 42;
    std::string const counted = AutoWowQuestLedger::FormatLine("r", row);
    EXPECT_EQ(counted, plain.substr(0, plain.size() - 1) + ",\"n\":42}");
}

TEST(AutoWowQuestLedgerTest, BlockedDedupeEmitsOnChangeAndHeartbeatsWithCount)
{
    using AutoWowQuestLedger::BlockedDecision;
    using AutoWowQuestLedger::BlockedDedupeState;
    using AutoWowQuestLedger::BlockedKey;
    using AutoWowQuestLedger::DedupeBlocked;
    constexpr std::uint64_t hb = 60000;
    BlockedDedupeState s;
    BlockedKey const full{835, "inventory_full", "resolve_objective"};

    BlockedDecision d = DedupeBlocked(s, full, 1000, hb);
    EXPECT_EQ(d.n, 1u);
    EXPECT_EQ(d.flushN, 0u);

    // ~2 Hz for just under a minute: all suppressed.
    std::uint32_t total = 1;
    std::uint64_t t = 1000;
    for (int k = 0; k < 119; ++k)
    {
        t += 500;
        d = DedupeBlocked(s, full, t, hb);
        EXPECT_EQ(d.n, 0u);
        ++total;
    }
    // Heartbeat at 60 s: one line standing for 119 suppressed + this one.
    d = DedupeBlocked(s, full, 61000, hb);
    ++total;
    EXPECT_EQ(d.n, 120u);

    // Two more repeats, then the phase changes: flush those two, then emit the new key.
    std::uint32_t emitted = 1 + 120;
    (void)DedupeBlocked(s, full, 61500, hb);
    (void)DedupeBlocked(s, full, 62000, hb);
    total += 2;
    BlockedKey const moved{835, "inventory_full", "travel_to_source"};
    d = DedupeBlocked(s, moved, 62500, hb);
    ++total;
    EXPECT_EQ(d.flushN, 2u);
    EXPECT_EQ(d.flushKey.quest, 835u);
    EXPECT_STREQ(d.flushKey.phase, "resolve_objective");
    EXPECT_EQ(d.n, 1u);
    emitted += d.flushN + d.n;
    EXPECT_EQ(emitted, total);  // sterile sum: sum(n) == occurrences

    // A different quest with no suppressed repeats: no flush line.
    d = DedupeBlocked(s, BlockedKey{836, "inventory_full", "travel_to_source"}, 63000, hb);
    EXPECT_EQ(d.flushN, 0u);
    EXPECT_EQ(d.n, 1u);
}

TEST(AutoWowQuestLedgerTest, BlockedDedupeComparesKeyByTextNotPointer)
{
    using AutoWowQuestLedger::BlockedDedupeState;
    using AutoWowQuestLedger::BlockedKey;
    using AutoWowQuestLedger::DedupeBlocked;
    BlockedDedupeState s;
    std::string const reasonCopy = "no_live_candidate";
    (void)DedupeBlocked(s, BlockedKey{1, "no_live_candidate", "acquire_target"}, 0, 60000);
    EXPECT_EQ(DedupeBlocked(s, BlockedKey{1, reasonCopy.c_str(), "acquire_target"}, 10, 60000).n, 0u);
}
}  // namespace
