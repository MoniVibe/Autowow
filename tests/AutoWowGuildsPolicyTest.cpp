/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowGuildsPolicy.h"

#include <algorithm>

#include "AutoWowQuestLedger.h"
#include "TradePolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowGuilds;

TEST(AutoWowGuildsPolicy, ParsesCohortGuidRanges)
{
    std::vector<GuidRange> r;
    ASSERT_TRUE(ParseGuidRanges("62955-63004", r));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(InRanges(r, 62955));
    EXPECT_TRUE(InRanges(r, 63004));
    EXPECT_FALSE(InRanges(r, 62954));
    EXPECT_FALSE(InRanges(r, 63005));

    ASSERT_TRUE(ParseGuidRanges(" 10 - 12 , 70001 ", r));
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(InRanges(r, 11));
    EXPECT_TRUE(InRanges(r, 70001));
    EXPECT_FALSE(InRanges(r, 13));

    // Malformed input leaves the previous list untouched.
    EXPECT_FALSE(ParseGuidRanges("", r));
    EXPECT_FALSE(ParseGuidRanges("12-10", r));
    EXPECT_FALSE(ParseGuidRanges("0-5", r));
    EXPECT_FALSE(ParseGuidRanges("1-5,x", r));
    EXPECT_FALSE(ParseGuidRanges("1-5,", r));
    EXPECT_FALSE(ParseGuidRanges("1-65537", r));
    EXPECT_TRUE(ParseGuidRanges("1-65536", r));
    EXPECT_EQ(r.size(), 1u);
}

TEST(AutoWowGuildsPolicy, ParsesHousesAndPicksLowestListedMatch)
{
    std::vector<House> h;
    ASSERT_TRUE(ParseHouses("Weavers:197,333;Smiths:186,164,202;Tanners:393,165;Brewers:182,171", h));
    ASSERT_EQ(h.size(), 4u);
    EXPECT_EQ(h[1].name, "Smiths");
    EXPECT_EQ(h[1].skills, (std::vector<std::uint32_t>{186, 164, 202}));

    auto holds = [](std::vector<std::uint32_t> skills) {
        return [skills](std::uint32_t s) { return std::find(skills.begin(), skills.end(), s) != skills.end(); };
    };
    EXPECT_EQ(HouseFor(h, holds({393, 186})), 1u);  // skinning + mining: Smiths is listed before Tanners
    EXPECT_EQ(HouseFor(h, holds({171})), 3u);
    EXPECT_EQ(HouseFor(h, holds({197, 182})), 0u);
    EXPECT_EQ(HouseFor(h, holds({755})), 0u);      // no matching house: the first house
    EXPECT_EQ(HouseFor(h, holds({})), 0u);

    EXPECT_FALSE(ParseHouses("", h));
    EXPECT_FALSE(ParseHouses("Weavers", h));
    EXPECT_FALSE(ParseHouses("Weavers:", h));
    EXPECT_FALSE(ParseHouses("Weavers:197;Weavers:333", h));
    EXPECT_FALSE(ParseHouses("Iron Smiths:186", h));
    EXPECT_FALSE(ParseHouses("Smiths:186,0", h));
    EXPECT_EQ(h.size(), 4u);
}

TEST(AutoWowGuildsPolicy, GuildNamesFitTheColumn)
{
    EXPECT_EQ(GuildName("{house} of the {team}", "Weavers", true), "Weavers of the Alliance");
    EXPECT_EQ(GuildName("{house} of the {team}", "Brewers", false), "Brewers of the Horde");
    EXPECT_EQ(GuildName("{house} of the {team}", "Herbalists", true), "");  // 26 characters
    EXPECT_EQ(GuildName("{team} {house}", "Herbalists", true), "Alliance Herbalists");
    EXPECT_EQ(GuildName("", "Weavers", true), "");
}

TEST(AutoWowGuildsPolicy, TaxIsFlooredPercentOfSales)
{
    EXPECT_EQ(TaxCopper(0, 10), 0u);
    EXPECT_EQ(TaxCopper(9, 10), 0u);
    EXPECT_EQ(TaxCopper(10, 10), 1u);
    EXPECT_EQ(TaxCopper(1234, 10), 123u);
    EXPECT_EQ(TaxCopper(1234, 0), 0u);
    EXPECT_EQ(TaxCopper(1234, 150), 1234u);  // clamped to 100%
    EXPECT_EQ(TaxCopper(0xFFFFFFFFFFFFFFFFull, 50), 0x7FFFFFFFFFFFFFFFull);  // no overflow
}

TEST(AutoWowGuildsPolicy, PayRefusedWhenShortAndPostageIsStock)
{
    EXPECT_TRUE(PayAllowed(100, 100));
    EXPECT_FALSE(PayAllowed(99, 100));
    EXPECT_FALSE(PayAllowed(100, 0));
    EXPECT_EQ(Postage(0), 30u);
    EXPECT_EQ(Postage(1), 30u);
    EXPECT_EQ(Postage(12), 360u);
}

TEST(AutoWowGuildsPolicy, LedgerWireIsStable)
{
    EXPECT_STREQ(AutoWowQuestLedger::EventName(AutoWowQuestLedger::Event::Guild), "guild");
    EXPECT_EQ(static_cast<int>(AutoWowQuestLedger::Event::Guild), 19);
    EXPECT_STREQ(ReasonName(Reason::SkipOtherGuild), "skip_other_guild");
    EXPECT_STREQ(ReasonName(Reason::Postage), "postage");
    EXPECT_STREQ(ReasonName(Reason::Grant), "grant");  // AutoWow.Supply.Outfit
    EXPECT_EQ(static_cast<int>(Reason::Levy), 8);
    EXPECT_EQ(static_cast<int>(Reason::Grant), 9);
    EXPECT_EQ(LedgerFields("Weavers", 7, 50, 1050),
              ",\"house\":\"Weavers\",\"gid\":7,\"copper\":50,\"balance_after\":1050");
    EXPECT_EQ(LedgerFields("Weavers", 7, 50, 10, ReasonName(Reason::Pay)),
              ",\"house\":\"Weavers\",\"gid\":7,\"copper\":50,\"balance_after\":10,\"op\":\"pay\"");
    EXPECT_STREQ(AutoWowTrade::ActionName(AutoWowTrade::Action::Tax), "tax");
    EXPECT_STREQ(ReasonName(Reason::Moved), "moved");  // lane F: a pinned rep / artisan left another house guild
    EXPECT_EQ(static_cast<int>(Reason::Moved), 10);
}

TEST(AutoWowGuildsPolicy, PinnedMembersMoveOnlyOutOfOurHouseGuilds)
{
    // soak-s45-full-r1: the Brewers artisans sat in the Weavers guilds.
    EXPECT_TRUE(MovesToOwnHouse(true, true, false));
    EXPECT_FALSE(MovesToOwnHouse(false, true, false));  // an ordinary cohort member is never moved
    EXPECT_FALSE(MovesToOwnHouse(true, false, false));  // a guild that is not ours is never touched
    EXPECT_FALSE(MovesToOwnHouse(true, true, true));    // never that guild's leader
}
}  // namespace
