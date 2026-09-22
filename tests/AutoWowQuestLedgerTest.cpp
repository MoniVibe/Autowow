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
}  // namespace
