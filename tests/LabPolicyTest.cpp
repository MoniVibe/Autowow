/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LabPolicy.h"

#include <cmath>

#include "gtest/gtest.h"

using namespace AutoWowLab;

TEST(LabPolicy, SpawnArgsParseAndBounds)
{
    SpawnArgs a;
    ASSERT_TRUE(ParseSpawnArgs("27260 3", 75, a));
    EXPECT_EQ(a.entry, 27260u);
    EXPECT_EQ(a.count, 3u);
    EXPECT_EQ(a.level, 75u);  // omitted level -> lab level
    ASSERT_TRUE(ParseSpawnArgs(" 27131 1 74 ", 75, a));
    EXPECT_EQ(a.level, 74u);
    EXPECT_FALSE(ParseSpawnArgs("", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("27260", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("27260 0", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("27260 9", 75, a));  // > kMaxSpawnCount
    EXPECT_FALSE(ParseSpawnArgs("27260 2 84", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("-5 2", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("27260 2 75 junk", 75, a));
    EXPECT_FALSE(ParseSpawnArgs("27260 2 x", 75, a));
}

TEST(LabPolicy, RingSlotsAreFixedAndEvenlySpaced)
{
    auto const [x0, y0] = RingOffset(0, 3, 18.0f, 0.0f);
    EXPECT_NEAR(x0, 18.0f, 1e-4);
    EXPECT_NEAR(y0, 0.0f, 1e-4);
    for (std::uint32_t n = 1; n <= kMaxSpawnCount; ++n)
        for (std::uint32_t i = 0; i < n; ++i)
        {
            auto const [dx, dy] = RingOffset(i, n, 18.0f, 1.0f);
            EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), 18.0f, 1e-3);
            auto const [ax, ay] = RingOffset(i, n, 18.0f, 1.0f);  // same slot -> same spot
            EXPECT_EQ(dx, ax);
            EXPECT_EQ(dy, ay);
        }
    auto const [x1, y1] = RingOffset(1, 2, 10.0f, 0.0f);  // opposite side for a pair
    EXPECT_NEAR(x1, -10.0f, 1e-4);
    EXPECT_NEAR(y1, 0.0f, 1e-4);
    auto const [zx, zy] = RingOffset(0, 0, 10.0f, 0.0f);
    EXPECT_EQ(zx, 0.0f);
    EXPECT_EQ(zy, 0.0f);
}

TEST(LabPolicy, KitIlvlMap)
{
    EXPECT_EQ(KitIlvlFor("4:79,1:89, 11:87", 4), 79u);
    EXPECT_EQ(KitIlvlFor("4:79,1:89, 11:87", 1), 89u);
    EXPECT_EQ(KitIlvlFor("4:79,1:89, 11:87", 11), 87u);
    EXPECT_EQ(KitIlvlFor("4:79,1:89", 8), 0u);  // missing class -> uncapped
    EXPECT_EQ(KitIlvlFor("", 4), 0u);
    EXPECT_EQ(KitIlvlFor("4:x9,4:80", 4), 80u);  // malformed item skipped
    EXPECT_EQ(KitIlvlFor("14:70", 4), 0u);
}

TEST(LabPolicy, MarkerParse)
{
    Marker m;
    ASSERT_TRUE(ParseMarker("1 16226.2 16257.0 13.2 1.5", m));
    EXPECT_EQ(m.map, 1u);
    EXPECT_NEAR(m.x, 16226.2f, 1e-3);
    EXPECT_NEAR(m.o, 1.5f, 1e-6);
    ASSERT_TRUE(ParseMarker("571 1 2 3", m));
    EXPECT_EQ(m.o, 0.0f);
    EXPECT_FALSE(ParseMarker("1 2 3", m));
}

TEST(LabPolicy, LowHpCrossingAndHelpers)
{
    EXPECT_TRUE(CrossesBelowPct(4000, 600, 10000, 35));    // 40% -> 34%
    EXPECT_FALSE(CrossesBelowPct(3400, 100, 10000, 35));   // already below
    EXPECT_FALSE(CrossesBelowPct(4000, 400, 10000, 35));   // 40% -> 36%
    EXPECT_TRUE(CrossesBelowPct(3500, 1, 10000, 35));      // exactly on the line -> below
    EXPECT_TRUE(CrossesBelowPct(3500, 9000, 10000, 35));   // overkill clamps to 0
    EXPECT_FALSE(CrossesBelowPct(100, 50, 0, 35));
    EXPECT_EQ(ScenarioPart({27260, 3, 75}), "27260x3@75");
    EXPECT_TRUE(InRadius(30.0f, 40.0f, 50.0f));
    EXPECT_FALSE(InRadius(30.0f, 41.0f, 50.0f));
    EXPECT_TRUE(IsLabName("LabRogue"));
    EXPECT_FALSE(IsLabName("Lab"));
    EXPECT_FALSE(IsLabName("Flacid"));
    EXPECT_FALSE(IsLabName("labrogue"));
}
