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

TEST(AutoWowQuestLedgerTest, CombatEventAppendsPreformattedFieldsAtQuestZero)
{
    Row row;
    row.ev = Event::Combat;
    row.ms = 5;
    row.bot = 62960;
    row.level = 7;
    row.extra = ",\"cv\":1,\"cls\":1";
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("r", row),
              "{\"v\":1,\"run\":\"r\",\"ms\":5,\"ev\":\"combat\",\"bot\":62960,\"team\":0,\"lvl\":7,"
              "\"quest\":0,\"map\":0,\"zone\":0,\"x\":0,\"y\":0,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"\",\"phase\":\"\",\"cv\":1,\"cls\":1}");
    // extra is ignored on every other event.
    row.ev = Event::Accepted;
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("r", row).find("cv"), std::string::npos);
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

TEST(AutoWowQuestLedgerTest, DiedAndPvpKillEventsAreAppendedWireStable)
{
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Died), "died");
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::PvpKill), "pvp_kill");
    EXPECT_EQ(static_cast<int>(Event::Died), 6);
    EXPECT_EQ(static_cast<int>(Event::PvpKill), 7);
    using AutoWowQuestLedger::KillerKind;
    EXPECT_STREQ(AutoWowQuestLedger::KillerKindName(KillerKind::Player), "player");
    EXPECT_STREQ(AutoWowQuestLedger::KillerKindName(KillerKind::Creature), "creature");
    EXPECT_STREQ(AutoWowQuestLedger::KillerKindName(KillerKind::Environment), "environment");
    EXPECT_STREQ(AutoWowQuestLedger::KillerKindName(KillerKind::Unknown), "unknown");
}

TEST(AutoWowQuestLedgerTest, FormatsDiedWithKillerFields)
{
    Row row;
    row.ev = Event::Died;
    row.ms = 5000;
    row.bot = 77;
    row.team = 1;
    row.level = 34;
    row.map = 0;
    row.zone = 33;
    row.x = -11500;
    row.y = 300;
    row.killer = AutoWowQuestLedger::KillerKind::Creature;
    row.killerId = 681;
    row.killerLevel = 36;
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("s2", row),
              "{\"v\":1,\"run\":\"s2\",\"ms\":5000,\"ev\":\"died\",\"bot\":77,\"team\":1,\"lvl\":34,"
              "\"quest\":0,\"map\":0,\"zone\":33,\"x\":-11500,\"y\":300,\"c\":[0,0,0,0],"
              "\"i\":[0,0,0,0,0,0],\"reason\":\"\",\"phase\":\"\",\"killer\":\"creature\",\"kid\":681,"
              "\"klvl\":36}");

    Row unknown;
    unknown.ev = Event::Died;
    std::string const line = AutoWowQuestLedger::FormatLine("", unknown);
    EXPECT_NE(line.find(",\"killer\":\"unknown\",\"kid\":0,\"klvl\":0}"), std::string::npos);
}

TEST(AutoWowQuestLedgerTest, FormatsPvpKillWithVictimFields)
{
    Row row;
    row.ev = Event::PvpKill;
    row.bot = 12;
    row.victim = 99;
    row.victimLevel = 40;
    row.honorable = true;
    std::string const line = AutoWowQuestLedger::FormatLine("", row);
    EXPECT_NE(line.find("\"ev\":\"pvp_kill\""), std::string::npos);
    EXPECT_NE(line.find(",\"phase\":\"\",\"victim\":99,\"vlvl\":40,\"honorable\":true}"), std::string::npos);
    row.honorable = false;
    EXPECT_NE(AutoWowQuestLedger::FormatLine("", row).find("\"honorable\":false}"), std::string::npos);

    // Other events never carry the death/kill fields.
    Row blocked;
    blocked.ev = Event::Blocked;
    blocked.victim = 99;
    blocked.killer = AutoWowQuestLedger::KillerKind::Player;
    std::string const b = AutoWowQuestLedger::FormatLine("", blocked);
    EXPECT_EQ(b.find("victim"), std::string::npos);
    EXPECT_EQ(b.find("killer"), std::string::npos);
}

TEST(AutoWowQuestLedgerTest, HonorableKillMirrorsCoreRewardHonorGate)
{
    using AutoWowQuestLedger::IsHonorableKill;
    // Level 40 killer: core gray level is 30 (40 - 10 for levels 40..49).
    EXPECT_TRUE(IsHonorableKill(false, false, 30, 31, false));
    EXPECT_FALSE(IsHonorableKill(false, false, 30, 30, false));  // gray victim
    EXPECT_FALSE(IsHonorableKill(true, false, 30, 40, false));   // same faction
    EXPECT_TRUE(IsHonorableKill(true, true, 30, 40, false));     // same faction on an FFA realm
    EXPECT_FALSE(IsHonorableKill(false, false, 30, 40, true));   // honorless-target aura
}

TEST(AutoWowQuestLedgerTest, ProgressEventNameIsAppendOnly)
{
    EXPECT_EQ(static_cast<int>(Event::Progress), 9);  // 8 reserved for the combat lane
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Progress), "progress");
}

TEST(AutoWowQuestLedgerTest, DeathLoopEventIsAppendOnlyAndCarriesTrailingFields)
{
    EXPECT_EQ(static_cast<int>(Event::DeathLoop), 10);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::DeathLoop), "death_loop");
    Row row;
    row.ev = Event::DeathLoop;
    row.bot = 112;
    row.level = 18;
    row.quest = 4183;
    row.reason = "level_gap";
    row.extra = ",\"deaths\":1";
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("r", row),
              "{\"v\":1,\"run\":\"r\",\"ms\":0,\"ev\":\"death_loop\",\"bot\":112,\"team\":0,\"lvl\":18,"
              "\"quest\":4183,\"map\":0,\"zone\":0,\"x\":0,\"y\":0,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"level_gap\",\"phase\":\"\",\"deaths\":1}");
}

TEST(AutoWowQuestLedgerTest, ZoneMoveEventIsAppendOnlyAndCarriesTrailingFields)
{
    EXPECT_EQ(static_cast<int>(Event::ZoneMove), 13);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::ZoneMove), "zone_move");
    Row row;
    row.ev = Event::ZoneMove;
    row.bot = 62955;
    row.level = 10;
    row.zone = 40;
    row.reason = "level";
    row.extra = ",\"from\":12,\"to\":40,\"travel_ms\":5000,\"arrived\":true,\"mode\":\"walk\"";
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("r", row),
              "{\"v\":1,\"run\":\"r\",\"ms\":0,\"ev\":\"zone_move\",\"bot\":62955,\"team\":0,\"lvl\":10,"
              "\"quest\":0,\"map\":0,\"zone\":40,\"x\":0,\"y\":0,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"level\",\"phase\":\"\",\"from\":12,\"to\":40,\"travel_ms\":5000,\"arrived\":true,"
              "\"mode\":\"walk\"}");
}

TEST(AutoWowQuestLedgerTest, ErrandEventIsAppendOnlyAndCarriesTrailingFields)
{
    EXPECT_EQ(static_cast<int>(Event::Errand), 14);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(Event::Errand), "errand");
    Row row;
    row.ev = Event::Errand;
    row.bot = 62955;
    row.level = 12;
    row.zone = 12;
    row.reason = "done";
    row.extra = ",\"town\":3002,\"hearth\":false";
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("r", row),
              "{\"v\":1,\"run\":\"r\",\"ms\":0,\"ev\":\"errand\",\"bot\":62955,\"team\":0,\"lvl\":12,"
              "\"quest\":0,\"map\":0,\"zone\":12,\"x\":0,\"y\":0,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"done\",\"phase\":\"\",\"town\":3002,\"hearth\":false}");
}

TEST(AutoWowQuestLedgerTest, DiffProgressEmitsOnlyChangedKnownQuests)
{
    using AutoWowQuestLedger::DiffProgress;
    using AutoWowQuestLedger::QuestCounters;
    std::vector<QuestCounters> last;

    QuestCounters kill{170};
    QuestCounters collect{3361};
    EXPECT_TRUE(DiffProgress(last, {kill, collect}).empty());  // first sample: baseline only

    kill.c[0] = 2;
    EXPECT_EQ(DiffProgress(last, {kill, collect}), (std::vector<std::uint32_t>{170}));
    EXPECT_TRUE(DiffProgress(last, {kill, collect}).empty());  // unchanged

    collect.i[2] = 1;
    QuestCounters talk{233};
    EXPECT_EQ(DiffProgress(last, {collect, talk}), (std::vector<std::uint32_t>{3361}));  // 170 left the log
    kill.c[0] = 3;
    EXPECT_TRUE(DiffProgress(last, {kill}).empty());  // re-accepted 170 is a new baseline
}
}  // namespace
