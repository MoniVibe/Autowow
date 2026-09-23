#include "../src/AutoWow/AutoWowTrainPolicy.h"
#include "../src/AutoWow/AutoWowQuestLedger.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowTrainPolicy;
using V = std::vector<std::uint32_t>;

TEST(AutoWowTrainPolicy, ParsesRenderedAssignments)
{
    AssignmentMap map;
    ASSERT_TRUE(ParseAssignments("62955:186,164;62956:182,171;62957:393;", map));
    ASSERT_EQ(map.size(), 3u);
    EXPECT_EQ(map.at(62955), (V{186, 164}));
    EXPECT_EQ(map.at(62956), (V{182, 171}));
    EXPECT_EQ(map.at(62957), (V{393}));

    ASSERT_TRUE(ParseAssignments("", map));
    EXPECT_TRUE(map.empty());
}

TEST(AutoWowTrainPolicy, MalformedAssignmentsFailClosed)
{
    AssignmentMap map;
    for (char const* bad : {"62955", "62955:", ":186", "0:186", "62955:186,164,202", "62955:186,186",
                            "62955:129", "62955:999", "62955:186;62955:164", "62955: 186", "x:186",
                            "62955:186,", "99999999999:186", "62955:186;;62956:18a"})
    {
        map[1] = {186};
        EXPECT_FALSE(ParseAssignments(bad, map)) << bad;
        EXPECT_TRUE(map.empty()) << bad;
    }
}

TEST(AutoWowTrainPolicy, ParsesSecondaries)
{
    V out;
    ASSERT_TRUE(ParseSecondaries("129,185,356", out));
    EXPECT_EQ(out, (V{129, 185, 356}));
    ASSERT_TRUE(ParseSecondaries("", out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(ParseSecondaries("129,186", out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(ParseSecondaries("129,129", out));
}

TEST(AutoWowTrainPolicy, LearnFilterOnlyStartsPlannedProfessions)
{
    V const plan{186, 164};
    V const secondaries{129, 185, 356};
    // Class spell / recipe: no skill line granted.
    EXPECT_TRUE(AllowSkillGrant(0, false, plan, secondaries));
    // Non-profession line (e.g. a weapon or riding skill) is not the filter's business.
    EXPECT_TRUE(AllowSkillGrant(762, false, plan, secondaries));
    // Planned primaries and secondaries start.
    EXPECT_TRUE(AllowSkillGrant(186, false, plan, secondaries));
    EXPECT_TRUE(AllowSkillGrant(164, false, plan, secondaries));
    EXPECT_TRUE(AllowSkillGrant(185, false, plan, secondaries));
    // Off-plan primary at a trainer with a free slot is refused.
    EXPECT_FALSE(AllowSkillGrant(182, false, plan, secondaries));
    EXPECT_FALSE(AllowSkillGrant(333, false, plan, secondaries));
    // Off-plan secondary refused when not in the secondary list.
    EXPECT_FALSE(AllowSkillGrant(356, false, plan, V{129, 185}));
    // A line the bot already has (rank-up) always passes, planned or not.
    EXPECT_TRUE(AllowSkillGrant(182, true, plan, secondaries));
}

TEST(AutoWowTrainPolicy, ProfessionSkillLinesAreTheFourteen)
{
    int count = 0;
    for (std::uint32_t skill = 0; skill < 1000; ++skill)
        count += IsProfessionSkillLine(skill) ? 1 : 0;
    EXPECT_EQ(count, 14);
}

TEST(AutoWowTrainPolicy, SkillUpEventIdIsElevenAndWireStable)
{
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::SkillUp), 11);
    EXPECT_STREQ(AutoWowQuestLedger::EventName(AutoWowQuestLedger::Event::SkillUp), "skill_up");
    EXPECT_FALSE(AutoWowQuestLedger::SkillUpEnabled());
}

TEST(AutoWowTrainPolicy, FormatsSkillUpWithTrailingSkillFields)
{
    AutoWowQuestLedger::Row row;
    row.ev = AutoWowQuestLedger::Event::SkillUp;
    row.ms = 1234;
    row.bot = 62955;
    row.team = 0;
    row.level = 6;
    row.map = 0;
    row.zone = 12;
    row.x = -8913;
    row.y = -136;
    row.skill = 186;
    row.skillOld = 12;
    row.skillNew = 13;
    row.skillMax = 75;
    row.cause = "update";
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("soak", row),
              "{\"v\":1,\"run\":\"soak\",\"ms\":1234,\"ev\":\"skill_up\",\"bot\":62955,\"team\":0,\"lvl\":6,"
              "\"quest\":0,\"map\":0,\"zone\":12,\"x\":-8913,\"y\":-136,\"c\":[0,0,0,0],\"i\":[0,0,0,0,0,0],"
              "\"reason\":\"\",\"phase\":\"\",\"skill\":186,\"old\":12,\"new\":13,\"max\":75,\"cause\":\"update\"}");

    // Other events never carry the skill fields.
    row.ev = AutoWowQuestLedger::Event::Accepted;
    EXPECT_EQ(AutoWowQuestLedger::FormatLine("soak", row).find("\"skill\""), std::string::npos);
}
}  // namespace
