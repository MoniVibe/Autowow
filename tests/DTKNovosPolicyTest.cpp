/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/Ai/Dungeon/DTK/DTKNovosPolicy.h"

#include "gtest/gtest.h"

TEST(DungeonDTKNovosPolicy, KeepsEncounterLogicActiveForTheWholeShieldAura)
{
    EXPECT_TRUE(DTKNovosPolicy::IsShieldPhase(true, true));
    EXPECT_FALSE(DTKNovosPolicy::IsShieldPhase(false, true));
    EXPECT_FALSE(DTKNovosPolicy::IsShieldPhase(true, false));
    EXPECT_FALSE(DTKNovosPolicy::IsShieldPhase(false, false));
}
