#include "RaidInterruptResponsePolicy.h"

#include <string_view>

#include "gtest/gtest.h"

using RaidInterruptResponsePolicy::GetActions;

TEST(RaidInterruptResponsePolicy, UsesExistingClassInterruptActions)
{
    EXPECT_EQ("counterspell", std::string_view(GetActions(CLASS_MAGE).primary));
    EXPECT_EQ("kick", std::string_view(GetActions(CLASS_ROGUE).primary));
    EXPECT_EQ("wind shear", std::string_view(GetActions(CLASS_SHAMAN).primary));
    EXPECT_EQ("mind freeze", std::string_view(GetActions(CLASS_DEATH_KNIGHT).primary));
}

TEST(RaidInterruptResponsePolicy, WarriorCanFallBackForEquipmentAndStance)
{
    auto const actions = GetActions(CLASS_WARRIOR);
    EXPECT_EQ("pummel", std::string_view(actions.primary));
    EXPECT_EQ("shield bash", std::string_view(actions.fallback));
}

TEST(RaidInterruptResponsePolicy, ClassesWithoutAnInterruptStayExplicitlyUnsupported)
{
    auto const actions = GetActions(CLASS_DRUID);
    EXPECT_EQ(nullptr, actions.primary);
    EXPECT_EQ(nullptr, actions.fallback);
}
