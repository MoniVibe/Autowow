/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OnyBreathSignal.h"

#include "gtest/gtest.h"

TEST(OnyxiaBreathSignal, MovementAndArrivalTelemetryAreIdempotentPerSignal)
{
    ObjectGuid const botGuid = ObjectGuid::Create<HighGuid::Player>(5252);
    OnyxiaBreathSignal::Clear(botGuid);
    OnyxiaBreathSignal::Record(botGuid, 18564);

    EXPECT_TRUE(OnyxiaBreathSignal::MarkMove(botGuid, 18564));
    EXPECT_FALSE(OnyxiaBreathSignal::MarkMove(botGuid, 18564));
    EXPECT_TRUE(OnyxiaBreathSignal::MarkArrival(botGuid, 18564));
    EXPECT_FALSE(OnyxiaBreathSignal::MarkArrival(botGuid, 18564));

    OnyxiaBreathSignal::Record(botGuid, 18584);
    EXPECT_TRUE(OnyxiaBreathSignal::MarkMove(botGuid, 18584));
    EXPECT_TRUE(OnyxiaBreathSignal::MarkArrival(botGuid, 18584));
    EXPECT_FALSE(OnyxiaBreathSignal::MarkMove(botGuid, 18564));

    OnyxiaBreathSignal::Clear(botGuid);
}

TEST(OnyxiaBreathSignal, InstantEncounterCastRemainsVisibleToEveryBotTick)
{
    uint32 const instanceId = 4242;
    OnyxiaBreathSignal::ClearEncounter(instanceId);

    EXPECT_TRUE(OnyxiaBreathSignal::RecordEncounter(instanceId, 18596));
    EXPECT_EQ(OnyxiaBreathSignal::GetEncounterActive(instanceId), 18596u);
    EXPECT_FALSE(OnyxiaBreathSignal::RecordEncounter(instanceId, 18596));

    EXPECT_TRUE(OnyxiaBreathSignal::RecordEncounter(instanceId, 18617));
    EXPECT_EQ(OnyxiaBreathSignal::GetEncounterActive(instanceId), 18617u);

    OnyxiaBreathSignal::ClearEncounter(instanceId);
    EXPECT_EQ(OnyxiaBreathSignal::GetEncounterActive(instanceId), 0u);
}
