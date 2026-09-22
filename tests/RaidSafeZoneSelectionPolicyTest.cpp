#include "RaidSafeZoneSelectionPolicy.h"

#include "gtest/gtest.h"

using RaidSafeZoneSelectionPolicy::Point;
using RaidSafeZoneSelectionPolicy::SelectClosest;
using RaidSafeZoneSelectionPolicy::SelectClosestToSharedAnchor;

TEST(RaidSafeZoneSelectionPolicy, OnyxiaRespondersChooseNearestShelterFromLivePositions)
{
    std::vector<Point> const northSouth{{-10.0f, -180.0f}, {-20.0f, -250.0f}};

    // Live positions immediately before the failed Deep Breath crossing.
    Point const teleane{-19.47f, -219.57f};
    Point const usta{-26.15f, -222.41f};
    Point const nathalis{-10.74f, -180.80f};

    EXPECT_EQ(1u, SelectClosest(teleane, northSouth));
    EXPECT_EQ(1u, SelectClosest(usta, northSouth));
    EXPECT_EQ(0u, SelectClosest(nathalis, northSouth));
}

TEST(RaidSafeZoneSelectionPolicy, SharedAnchorApiDelegatesToClosestSelection)
{
    std::vector<Point> const zones{{-10.0f, -180.0f}, {-20.0f, -250.0f}};
    Point const anchor{-43.85f, -213.60f};
    EXPECT_EQ(SelectClosest(anchor, zones), SelectClosestToSharedAnchor(anchor, zones));
}

TEST(RaidSafeZoneSelectionPolicy, EqualDistanceTieIsStable)
{
    std::vector<Point> const zones{{-10.0f, 0.0f}, {10.0f, 0.0f}};
    EXPECT_EQ(0u, SelectClosest({0.0f, 0.0f}, zones));
}

TEST(RaidSafeZoneSelectionPolicy, EmptyZoneListHasSafeDefaultIndex)
{
    EXPECT_EQ(0u, SelectClosest({1.0f, 2.0f}, {}));
}
