/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/ProbePlacePolicy.h"

#include <string>

#include "gtest/gtest.h"

using namespace AutoWowProbePlace;

namespace
{
constexpr char kFixtures[] = "2, 21 36,72";
constexpr char kOracle[] = "7,21";

WireRequest Parse(std::string const& text, bool expectOk = true)
{
    WireRequest request;
    std::string error;
    EXPECT_EQ(ParseWireRequest(text, request, error), expectOk) << text << " -> " << error;
    return request;
}
}  // namespace

TEST(ProbePlacePolicy, GateIsDisabledByDefaultFlag)
{
    EXPECT_STREQ(GateGuid(false, kFixtures, kOracle, 36), "probe_disabled");
    EXPECT_EQ(GateGuid(true, kFixtures, kOracle, 36), nullptr);
    EXPECT_EQ(GateGuid(true, kFixtures, kOracle, 2), nullptr);
    EXPECT_EQ(GateGuid(true, kFixtures, kOracle, 72), nullptr);
}

TEST(ProbePlacePolicy, GateRefusesEverythingOutsideTheFixtureList)
{
    EXPECT_STREQ(GateGuid(true, kFixtures, kOracle, 0), "probe_guid_invalid");
    EXPECT_STREQ(GateGuid(true, kFixtures, kOracle, 37), "probe_guid_not_fixture");
    EXPECT_STREQ(GateGuid(true, kFixtures, kOracle, 7), "probe_guid_not_fixture");
    EXPECT_STREQ(GateGuid(true, "", kOracle, 36), "probe_guid_not_fixture");
    // Fail-closed allowlist: one malformed token (or a literal 0) refuses every GUID.
    EXPECT_STREQ(GateGuid(true, "36,abc", kOracle, 36), "probe_guid_not_fixture");
    EXPECT_STREQ(GateGuid(true, "36,0", kOracle, 36), "probe_guid_not_fixture");
    EXPECT_STREQ(GateGuid(true, "360", kOracle, 36), "probe_guid_not_fixture");
}

TEST(ProbePlacePolicy, GateRefusesCohortAndOracleEvenWhenListedAsFixture)
{
    std::string const withCohort = std::string(kFixtures) + ",62955,62980,63004,63005";
    EXPECT_STREQ(GateGuid(true, withCohort, kOracle, 62955), "probe_guid_is_cohort");
    EXPECT_STREQ(GateGuid(true, withCohort, kOracle, 62980), "probe_guid_is_cohort");
    EXPECT_STREQ(GateGuid(true, withCohort, kOracle, 63004), "probe_guid_is_cohort");
    EXPECT_EQ(GateGuid(true, withCohort, kOracle, 63005), nullptr);
    EXPECT_STREQ(GateGuid(true, kFixtures, kOracle, 21), "probe_guid_in_oracle_allowlist");
    // Fail-safe Oracle list: a malformed token excludes every GUID.
    EXPECT_STREQ(GateGuid(true, kFixtures, "7,x", 36), "probe_guid_in_oracle_allowlist");
    // Order is fixed: disabled beats cohort beats fixture beats oracle.
    EXPECT_STREQ(GateGuid(false, withCohort, kOracle, 62955), "probe_disabled");
    EXPECT_STREQ(GateGuid(true, "", "62955", 62955), "probe_guid_is_cohort");
}

TEST(ProbePlacePolicy, ParsesEveryVerb)
{
    WireRequest r = Parse("probe-login 36");
    EXPECT_EQ(r.verb, Verb::Login);
    EXPECT_EQ(r.guid, 36u);

    r = Parse("PROBE-SETLEVEL 36 8");
    EXPECT_EQ(r.verb, Verb::SetLevel);
    EXPECT_EQ(r.level, 8u);
    EXPECT_FALSE(r.hasSpec);
    EXPECT_EQ(r.quality, kDefaultQuality);

    r = Parse("probe-setlevel 36 12 3 1");
    EXPECT_TRUE(r.hasSpec);
    EXPECT_EQ(r.specIndex, 3u);
    EXPECT_EQ(r.quality, 1u);

    r = Parse("probe-place 36 358");
    EXPECT_EQ(r.verb, Verb::Place);
    EXPECT_EQ(r.questId, 358u);
    EXPECT_FALSE(r.dropOthers);
    EXPECT_TRUE(Parse("probe-place 36 358 drop-others").dropOthers);

    r = Parse("probe-status 36");
    EXPECT_EQ(r.verb, Verb::Status);
    EXPECT_EQ(r.questId, 0u);
    EXPECT_EQ(Parse("probe-status 36 358").questId, 358u);
}

TEST(ProbePlacePolicy, RejectsMalformedArguments)
{
    for (char const* bad : {"probe-login", "probe-login 0", "probe-login -3", "probe-login 36 37",
                            "probe-login 4294967296", "probe-setlevel 36", "probe-setlevel 36 0",
                            "probe-setlevel 36 8 3", "probe-setlevel 36 8 20 1", "probe-setlevel 36 8 3 6",
                            "probe-setlevel 36 8x", "probe-place 36", "probe-place 36 0",
                            "probe-place 36 358 drop", "probe-place 36 358 drop-others x",
                            "probe-status 36 358 1", "probe-status 36 q358", "probe-teleport 36"})
    {
        WireRequest request;
        request.guid = 99;
        std::string error;
        EXPECT_FALSE(ParseWireRequest(bad, request, error)) << bad;
        EXPECT_FALSE(error.empty()) << bad;
        EXPECT_EQ(request.guid, 99u) << "a rejected parse must not partially fill the request: " << bad;
    }
}

TEST(ProbePlacePolicy, LevelValidationAllowsDownButKeepsDeathKnightFloor)
{
    EXPECT_EQ(ValidateLevel(1, 80, 5), nullptr);
    EXPECT_EQ(ValidateLevel(80, 80, 5), nullptr);
    EXPECT_STREQ(ValidateLevel(0, 80, 5), "probe_level_out_of_range");
    EXPECT_STREQ(ValidateLevel(81, 80, 5), "probe_level_out_of_range");
    EXPECT_STREQ(ValidateLevel(54, 80, kDeathKnightClassId), "probe_level_below_death_knight_floor");
    EXPECT_EQ(ValidateLevel(55, 80, kDeathKnightClassId), nullptr);
}

TEST(ProbePlacePolicy, StarterSelectionIsDeterministic)
{
    std::vector<StarterCandidate> c = {
        {true, 50, 1, true, true, true},     // gameobject on bot map
        {false, 40, 900, true, true, false}, // friendly creature elsewhere
        {false, 41, 800, true, true, true},  // friendly creature on bot map, higher spawn id
        {false, 42, 700, true, true, true},  // friendly creature on bot map, lowest spawn id -> chosen
        {false, 43, 5, true, false, true},   // hostile
        {false, 44, 1, false, true, true},   // instance map, never chosen
    };
    EXPECT_EQ(SelectStarter(c), 3u);
    std::vector<StarterCandidate> reversed(c.rbegin(), c.rend());
    EXPECT_EQ(reversed[SelectStarter(reversed)].spawnId, 700u);

    std::vector<StarterCandidate> none = {{false, 44, 1, false, true, true}};
    EXPECT_EQ(SelectStarter(none), none.size());
    EXPECT_EQ(SelectStarter({}), 0u);
}

TEST(ProbePlacePolicy, ResponseFormatsAreStable)
{
    EXPECT_EQ(FormatError(Verb::Place, 36, "probe_disabled"),
              "{\"ok\":false,\"operation\":\"probe_place\",\"guid\":36,\"error\":\"probe_disabled\"}");
    EXPECT_EQ(FormatLogin(36, "login_queued"),
              "{\"ok\":true,\"operation\":\"probe_login\",\"guid\":36,\"state\":\"login_queued\"}");

    StatusFacts s;
    s.guid = 36;
    s.level = 8;
    s.alive = true;
    s.position = {0, 130, 228, 1.0f, -2.5f, 3.25f, 0.0f};
    s.questLogCount = 2;
    EXPECT_EQ(FormatStatus(s),
              "{\"ok\":true,\"operation\":\"probe_status\",\"guid\":36,\"level\":8,\"xp\":0,\"alive\":true,"
              "\"in_combat\":false,\"position\":{\"map\":0,\"zone\":130,\"area\":228,\"x\":1.00,\"y\":-2.50,"
              "\"z\":3.25,\"o\":0.00},\"quest_log_count\":2,\"quest\":null}");

    s.hasQuest = true;
    s.quest.questId = 358;
    s.quest.status = 3;
    s.quest.slot = 4;
    s.quest.objectives = {{"creature", 0, 1941, 2, 8}, {"item", 1, 3162, 0, 5}};
    std::string const status = FormatStatus(s);
    EXPECT_NE(status.find("\"quest\":{\"id\":358,\"status\":\"incomplete\",\"rewarded\":false,\"slot\":4,"
                          "\"objectives\":[{\"kind\":\"creature\",\"index\":0,\"entry\":1941,\"current\":2,"
                          "\"required\":8},{\"kind\":\"item\",\"index\":1,\"entry\":3162,\"current\":0,"
                          "\"required\":5}]}}"),
              std::string::npos)
        << status;

    PlaceFacts p;
    p.guid = 36;
    p.questId = 358;
    p.candidates = 2;
    p.starter = {"creature", 1515, 44321, {0, 0, 0, 10.0f, 20.0f, 30.0f, 1.5f}};
    p.previousStatus = 6;
    p.previousRewarded = true;
    p.questItemsDestroyed = 3;
    p.droppedOthers = {354, 437};
    p.teleport = "done";
    EXPECT_EQ(FormatPlace(p),
              "{\"ok\":true,\"operation\":\"probe_place\",\"guid\":36,\"quest_id\":358,\"candidates\":2,"
              "\"starter\":{\"kind\":\"creature\",\"entry\":1515,\"spawn_id\":44321,\"position\":{\"map\":0,"
              "\"zone\":0,\"area\":0,\"x\":10.00,\"y\":20.00,\"z\":30.00,\"o\":1.50}},\"cleared\":{"
              "\"previous_status\":\"rewarded\",\"previous_rewarded\":true,\"quest_items_destroyed\":3},"
              "\"dropped_others\":[354,437],\"teleport\":\"done\",\"ledger\":\"contaminated:probe_setup\"}");
    EXPECT_EQ(JsonString("a\"b\\c\n"), "\"a\\\"b\\\\c?\"");
}
