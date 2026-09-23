#include "../src/AutoWow/AutoWowCohortPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowCohortPolicy;

CreateArgs Valid()
{
    CreateArgs args;
    EXPECT_TRUE(ParseCreateArgs("COHORT_HU_01 1 1 0 Aldermund", args));
    return args;
}

TEST(AutoWowCohortPolicy, ParsesExactlyFiveTokens)
{
    CreateArgs args;
    ASSERT_TRUE(ParseCreateArgs("  COHORT_TA_01\t6 11 1  Hoofmere ", args));
    EXPECT_EQ(args.account, "COHORT_TA_01");
    EXPECT_EQ(args.race, 6u);
    EXPECT_EQ(args.cls, 11u);
    EXPECT_EQ(args.gender, 1u);
    EXPECT_EQ(args.name, "Hoofmere");

    EXPECT_FALSE(ParseCreateArgs("", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 1 1 0", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 1 1 0 Name extra", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 human 1 0 Name", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 1 -1 0 Name", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 1 1 0x1 Name", args));
    EXPECT_FALSE(ParseCreateArgs("COHORT_HU_01 1 1111 0 Name", args));
}

TEST(AutoWowCohortPolicy, PlayableRaceAndClassIds)
{
    for (std::uint32_t race : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u})
        EXPECT_TRUE(IsPlayableRaceId(race)) << race;
    for (std::uint32_t race : {0u, 9u, 12u, 255u})
        EXPECT_FALSE(IsPlayableRaceId(race)) << race;

    for (std::uint32_t cls : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u})
        EXPECT_TRUE(IsPlayableClassId(cls)) << cls;
    for (std::uint32_t cls : {0u, 10u, 12u, 255u})
        EXPECT_FALSE(IsPlayableClassId(cls)) << cls;
}

TEST(AutoWowCohortPolicy, ArgumentRefusals)
{
    EXPECT_EQ(CheckCreateArgs(Valid(), "rndbot"), CreateRefusal::None);

    CreateArgs args = Valid();
    args.race = 9;
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::BadRace);

    args = Valid();
    args.cls = 10;
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::BadClass);

    args = Valid();
    args.gender = 2;
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::BadGender);

    args = Valid();
    args.account = "RNDBOT17";  // accounts are stored upper-case; prefix match ignores case
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::RndbotAccount);
    args.account = "rndbot";
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::RndbotAccount);
    args.account = "COHORT_RNDBOT";
    EXPECT_EQ(CheckCreateArgs(args, "rndbot"), CreateRefusal::None);
    args.account = "RNDBOT17";
    EXPECT_EQ(CheckCreateArgs(args, ""), CreateRefusal::None);

    EXPECT_STREQ(CreateRefusalName(CreateRefusal::RndbotAccount), "rndbot_account");
}

TEST(AutoWowCohortPolicy, IndependentMaintenanceEligibility)
{
    EXPECT_TRUE(IsIndependentMaintenanceEligible(false, false, true));
    EXPECT_FALSE(IsIndependentMaintenanceEligible(true, false, true));   // random pool keeps its own rules
    EXPECT_FALSE(IsIndependentMaintenanceEligible(false, true, true));   // Oracle arm keeps its own rules
    EXPECT_FALSE(IsIndependentMaintenanceEligible(false, false, false)); // not independent (e.g. alt bot)
}

TEST(AutoWowCohortPolicy, IndependentMaintenanceModes)
{
    EXPECT_FALSE(IndependentAutoTalents(0, true));
    EXPECT_FALSE(IndependentAutoSpells(0, true));

    EXPECT_TRUE(IndependentAutoTalents(1, true));
    EXPECT_FALSE(IndependentAutoSpells(1, true));

    EXPECT_TRUE(IndependentAutoTalents(2, true));
    EXPECT_TRUE(IndependentAutoSpells(2, true));

    EXPECT_FALSE(IndependentAutoTalents(2, false));
    EXPECT_FALSE(IndependentAutoSpells(2, false));
}
}  // namespace
