/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherDetourPolicy.h"

#include <algorithm>

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowGatherDetour;

Node Herb(std::uint64_t guid, std::uint32_t req, std::int32_t dx, std::int32_t dy = 0, std::int32_t dz = 0)
{
    return Node{guid, kSkillHerbalism, req, dx, dy, dz};
}

Node Ore(std::uint64_t guid, std::uint32_t req, std::int32_t dx, std::int32_t dy = 0, std::int32_t dz = 0)
{
    return Node{guid, kSkillMining, req, dx, dy, dz};
}

TEST(GatherDetour, OnlySkillAppropriateNodes)
{
    Params p;
    Skills s;
    s.herbalism = 70;
    EXPECT_TRUE(Eligible(p, s, Herb(1, 70, 10)));   // req == skill
    EXPECT_FALSE(Eligible(p, s, Herb(1, 71, 10)));  // req above skill
    EXPECT_FALSE(Eligible(p, s, Ore(1, 1, 10)));    // mining not learned
    s.mining = 125;
    EXPECT_FALSE(Eligible(p, s, Ore(1, 65, 10)));   // no pick
    s.miningPick = true;
    EXPECT_TRUE(Eligible(p, s, Ore(1, 65, 10)));
    EXPECT_FALSE(Eligible(p, s, Ore(1, 150, 10)));
    EXPECT_FALSE(Eligible(p, s, Node{1, 393, 1, 10, 0, 0}));  // skinning / any other lock: not a detour
}

TEST(GatherDetour, WithinDetourYardsAndHeight)
{
    Params p;  // 60 yd, dz 20
    Skills s;
    s.herbalism = 300;
    EXPECT_TRUE(Eligible(p, s, Herb(1, 1, 60)));
    EXPECT_FALSE(Eligible(p, s, Herb(1, 1, 61)));
    EXPECT_FALSE(Eligible(p, s, Herb(1, 1, 43, 43)));  // 60.8 yd diagonal
    EXPECT_TRUE(Eligible(p, s, Herb(1, 1, 10, 0, -20)));
    EXPECT_FALSE(Eligible(p, s, Herb(1, 1, 10, 0, 21)));  // cliff top
    p.yards = 20;
    EXPECT_FALSE(Eligible(p, s, Herb(1, 1, 30)));
}

TEST(GatherDetour, PicksNearestThenLowestGuidAndHonoursSkip)
{
    Params p;
    Skills s;
    s.herbalism = 50;
    s.mining = 50;
    s.miningPick = true;
    std::vector<Node> const nodes = {Herb(9, 1, 30), Ore(4, 1, 0, 20), Herb(3, 1, -20), Herb(8, 80, 5), Ore(2, 1, 70)};
    EXPECT_EQ(PickNode(p, s, nodes, {}), 2);   // guid 3 and 4 tie at 20 yd; guid 8 needs 80; guid 2 is too far
    EXPECT_EQ(PickNode(p, s, nodes, {3}), 1);  // guid 3 skipped
    EXPECT_EQ(PickNode(p, s, nodes, {3, 4, 9}), -1);
    EXPECT_EQ(PickNode(p, s, {}, {}), -1);
}

TEST(GatherDetour, DetourDecision)
{
    Skills none;
    Skills herb;
    herb.herbalism = 1;
    EXPECT_FALSE(MayDetour(none, false, false));
    EXPECT_TRUE(MayDetour(herb, false, false));
    EXPECT_FALSE(MayDetour(herb, true, false));  // in combat
    EXPECT_FALSE(MayDetour(herb, false, true));  // zone-progression / errand / flight leg
}

TEST(GatherDetour, TimeoutSkipsTheNodeInARing)
{
    Params p;  // 45 s
    BotState s;
    EXPECT_FALSE(TimedOut(p, s, 1000000));
    Begin(s, 77, 1000);
    EXPECT_FALSE(TimedOut(p, s, 46000));
    EXPECT_TRUE(TimedOut(p, s, 46001));
    End(s, true);
    EXPECT_EQ(s.target, 0U);
    EXPECT_EQ(SkipList(s), std::vector<std::uint64_t>{77});
    Begin(s, 78, 0);
    End(s, false);  // gathered: not skipped
    EXPECT_EQ(SkipList(s).size(), 1U);
    for (std::uint64_t g = 100; g < 100 + kSkipSlots; ++g)
    {
        Begin(s, g, 0);
        End(s, true);
    }
    std::vector<std::uint64_t> const ring = SkipList(s);
    EXPECT_EQ(ring.size(), kSkipSlots);
    EXPECT_EQ(std::count(ring.begin(), ring.end(), 77U), 0);  // oldest dropped
}
}  // namespace
