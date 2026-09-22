/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonEncounterCompletionPolicy.h"

#include "gtest/gtest.h"

TEST(DungeonEncounterCompletionPolicy, CompletedEncounterMaskBitIsAuthoritative)
{
    EXPECT_TRUE(DungeonEncounterCompletion::IsComplete(1u << 2, 2));
    EXPECT_FALSE(DungeonEncounterCompletion::IsComplete(1u << 2, 1));
}

TEST(DungeonEncounterCompletionPolicy, InternalBossDataSlotMustNotCompleteSameNumberedDbcEncounter)
{
    // DTK demonstrates why no InstanceScript boss-state value belongs here: internal Data slot 2
    // is NOVOS_CRYSTALS, while DBC encounter index 2 is Dred. A DONE slot 2 cannot set this mask bit.
    std::uint32_t const completedMaskAfterNovos = 1u << 1;
    EXPECT_FALSE(DungeonEncounterCompletion::IsComplete(completedMaskAfterNovos, 2));
}

TEST(DungeonEncounterCompletionPolicy, OutOfRangeIndexFailsClosed)
{
    EXPECT_FALSE(DungeonEncounterCompletion::IsComplete(~std::uint32_t(0), 32));
}
