/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bag.h"
#include "ObjectGuid.h"
#include "TestMap.h"
#include "TestPlayer.h"
#include "WorldMock.h"
#include "WorldSession.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace playerbots::item_usage_detail
{
uint32 SmallestEquippedBagSize(Player const& player);
}

namespace
{
using namespace testing;

class BagPlayer : public TestPlayer
{
public:
    using TestPlayer::TestPlayer;

    void AttachBag(uint8 slot, Bag* bag)
    {
        m_items[slot] = bag;
        bag->SetSlot(slot);
    }
};

class TestBag : public Bag
{
public:
    void ForceInitValues(ObjectGuid::LowType guid, uint32 size)
    {
        Object::_Create(guid, 0, HighGuid::Item);
        SetUInt32Value(CONTAINER_FIELD_NUM_SLOTS, size);
    }
};

class ItemUsageBagTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestMap::EnsureDBC();
        originalWorld = sWorld.release();
        worldMock = new NiceMock<WorldMock>();
        sWorld.reset(worldMock);
        static std::string emptyString;
        ON_CALL(*worldMock, GetDataPath()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*worldMock, GetRealmName()).WillByDefault(ReturnRef(emptyString));
        ON_CALL(*worldMock, GetDefaultDbcLocale()).WillByDefault(Return(LOCALE_enUS));
        ON_CALL(*worldMock, getRate(_)).WillByDefault(Return(1.0f));
        ON_CALL(*worldMock, getBoolConfig(_)).WillByDefault(Return(false));
        ON_CALL(*worldMock, getIntConfig(_)).WillByDefault(Return(0));
        ON_CALL(*worldMock, getFloatConfig(_)).WillByDefault(Return(0.0f));
        ON_CALL(*worldMock, GetPlayerSecurityLimit()).WillByDefault(Return(SEC_PLAYER));

        session = new WorldSession(1, "bag-test", 0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, 0,
                                   LOCALE_enUS, 0, false, false, 0);
        session->InitRBACDataForTest();
        player = new BagPlayer(session);
        player->ForceInitValues();
        session->SetPlayer(player);
        player->SetSession(session);
    }

    void TearDown() override
    {
        // Match the core mock fixtures: leak the isolated session/player/bags to avoid database work in destructors.
        IWorld* currentWorld = sWorld.release();
        delete currentWorld;
        sWorld.reset(originalWorld);
        originalWorld = nullptr;
        worldMock = nullptr;
        session = nullptr;
        player = nullptr;
    }

    void Attach(std::initializer_list<uint32> sizes)
    {
        uint8 slot = INVENTORY_SLOT_BAG_START;
        ObjectGuid::LowType guid = 100;
        for (uint32 const size : sizes)
        {
            TestBag* bag = new TestBag();
            bag->ForceInitValues(guid++, size);
            player->AttachBag(slot++, bag);
        }
    }

    IWorld* originalWorld = nullptr;
    NiceMock<WorldMock>* worldMock = nullptr;
    WorldSession* session = nullptr;
    BagPlayer* player = nullptr;
};

TEST_F(ItemUsageBagTest, FirstBagSlotParticipatesInSmallestSelection)
{
    Attach({6, 10, 12, 14});
    EXPECT_EQ(playerbots::item_usage_detail::SmallestEquippedBagSize(*player), 6u);
}

TEST_F(ItemUsageBagTest, EmptyFirstBagSlotIsTheMinimum)
{
    Attach({});
    uint8 slot = INVENTORY_SLOT_BAG_START + 1;
    ObjectGuid::LowType guid = 200;
    for (uint32 const size : {6u, 10u, 12u})
    {
        TestBag* bag = new TestBag();
        bag->ForceInitValues(guid++, size);
        player->AttachBag(slot++, bag);
    }
    EXPECT_EQ(playerbots::item_usage_detail::SmallestEquippedBagSize(*player), 0u);
}

TEST_F(ItemUsageBagTest, FindsTheSmallestAcrossAllFourNativeBags)
{
    Attach({14, 12, 10, 8});
    EXPECT_EQ(playerbots::item_usage_detail::SmallestEquippedBagSize(*player), 8u);
}
}  // namespace
