/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IntegrationTestFixture.h"
#include "TacticalRuntime.h"

#include "DBCStores.h"
#include "gtest/gtest.h"

#include <vector>

namespace
{
using namespace AutoWowTactics;

constexpr std::uint32_t kTestDisplayId = 99010;
constexpr std::uint32_t kTestModelId = 99010;

class TacticalRuntimeLinkedAddTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();

        // Native LOS asks every Unit for collision data. The minimal core fixture has no display/model DBC rows,
        // so install an isolated pair for these native objects and remove the rows during teardown.
        ASSERT_EQ(sCreatureDisplayInfoStore.LookupEntry(kTestDisplayId), nullptr);
        ASSERT_EQ(sCreatureModelDataStore.LookupEntry(kTestModelId), nullptr);
        auto* display = new CreatureDisplayInfoEntry{};
        display->Displayid = kTestDisplayId;
        display->ModelId = kTestModelId;
        display->scale = 1.0f;
        auto* model = new CreatureModelDataEntry{};
        model->Id = kTestModelId;
        model->Scale = 1.0f;
        model->CollisionWidth = 1.0f;
        model->CollisionHeight = 2.0f;
        sCreatureDisplayInfoStore.SetEntry(kTestDisplayId, display);
        sCreatureModelDataStore.SetEntry(kTestModelId, model);
        _collisionEntriesInstalled = true;

        // The core fixture owns synthetic faction 90002 but leaves its assistance flag unset. Set and restore the
        // native flag so Creature::CanAssistTo exercises the same faction-response rule as the live assistance scan.
        _hostileFaction =
            const_cast<FactionTemplateEntry*>(sFactionTemplateStore.LookupEntry(TEST_FACTION_HOSTILE_TO_ALL));
        ASSERT_NE(_hostileFaction, nullptr);
        _originalFactionFlags = _hostileFaction->factionFlags;
        _hostileFaction->factionFlags |= FACTION_TEMPLATE_FLAG_RESPOND_TO_CALL_FOR_HELP;

        _bot = CreateTestPlayer(1, "TacticalBot", SEC_PLAYER);
        _bot->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, TEST_FACTION_HOSTILE_TO_MONSTERS);
        _bot->SetNativeDisplayId(kTestDisplayId);
        _bot->SetLevel(80);
        _bot->Relocate(0.0f, 0.0f, 0.0f);

        _attacker = MakeHostile(100, 99001, 3.0f);
        _candidate = MakeHostile(101, 99002, 5.0f);
    }

    void TearDown() override
    {
        if (_hostileFaction)
            _hostileFaction->factionFlags = _originalFactionFlags;
        IntegrationTestFixture::TearDown();
        if (_collisionEntriesInstalled)
        {
            sCreatureDisplayInfoStore.SetEntry(kTestDisplayId, nullptr);
            sCreatureModelDataStore.SetEntry(kTestModelId, nullptr);
        }
    }

    TestCreature* MakeHostile(ObjectGuid::LowType guid, std::uint32_t entry, float x)
    {
        TestCreature* creature = CreateTestCreature(guid, entry, TEST_FACTION_HOSTILE_TO_ALL);
        creature->SetNativeDisplayId(kTestDisplayId);
        creature->SetReactState(REACT_AGGRESSIVE);
        creature->Relocate(x, 0.0f, 0.0f);
        return creature;
    }

    static EngagementSnapshot Single()
    {
        EngagementSnapshot snapshot;
        snapshot.attackers = 1;
        snapshot.load = 100;
        snapshot.hpPct = 100;
        snapshot.manaPct = 100;
        return snapshot;
    }

    void Accumulate(EngagementSnapshot& snapshot, Creature* candidate, std::vector<Unit*> const& attackers,
                    std::uint32_t radius = 12)
    {
        detail::AccumulateAssistEligibleLinkedAdd(snapshot, candidate, attackers, _bot, 80, LoadParams{}, radius);
    }

    FactionTemplateEntry* _hostileFaction = nullptr;
    std::uint32_t _originalFactionFlags = 0;
    bool _collisionEntriesInstalled = false;
    TestPlayer* _bot = nullptr;
    TestCreature* _attacker = nullptr;
    TestCreature* _candidate = nullptr;
};

TEST_F(TacticalRuntimeLinkedAddTest, NativeAssistEligibleCandidateSelectsMulti)
{
    ASSERT_TRUE(_attacker->IsWithinLOSInMap(_candidate));
    ASSERT_TRUE(_candidate->CanAssistTo(_attacker, _bot));

    EngagementSnapshot snapshot = Single();
    Accumulate(snapshot, _candidate, {_attacker});

    EXPECT_EQ(snapshot.addsNear, 1u);
    EXPECT_EQ(snapshot.load, 200u);
    EXPECT_EQ(DesiredClass(Family::Warrior, snapshot, ClassParams{}, TacticId::None, false),
              TacticId::WarriorMulti);
}

TEST_F(TacticalRuntimeLinkedAddTest, NearbyHostileThatCannotAssistStaysSingle)
{
    _candidate->SetReactState(REACT_PASSIVE);
    ASSERT_TRUE(_attacker->IsWithinLOSInMap(_candidate));
    ASSERT_TRUE(_candidate->IsHostileTo(_bot));
    ASSERT_FALSE(_candidate->CanAssistTo(_attacker, _bot));

    EngagementSnapshot snapshot = Single();
    Accumulate(snapshot, _candidate, {_attacker});

    EXPECT_EQ(snapshot.addsNear, 0u);
    EXPECT_EQ(snapshot.load, 100u);
    EXPECT_EQ(DesiredClass(Family::Warrior, snapshot, ClassParams{}, TacticId::None, false),
              TacticId::WarriorSingle);
}

TEST_F(TacticalRuntimeLinkedAddTest, ExistingLinkRadiusStillExcludesDistantAssistant)
{
    _bot->Relocate(30.0f, 0.0f, 0.0f);
    _candidate->Relocate(30.0f, 0.0f, 0.0f);
    ASSERT_TRUE(_candidate->CanAssistTo(_attacker, _bot));

    EngagementSnapshot snapshot = Single();
    Accumulate(snapshot, _candidate, {_attacker});

    EXPECT_EQ(snapshot.addsNear, 0u);
    EXPECT_EQ(snapshot.load, 100u);
    EXPECT_EQ(DesiredClass(Family::Warrior, snapshot, ClassParams{}, TacticId::None, false),
              TacticId::WarriorSingle);
}

TEST_F(TacticalRuntimeLinkedAddTest, CandidateEligibleForTwoAttackersCountsOnce)
{
    TestCreature* secondAttacker = MakeHostile(102, 99003, 4.0f);
    ASSERT_TRUE(_candidate->CanAssistTo(_attacker, _bot));
    ASSERT_TRUE(_candidate->CanAssistTo(secondAttacker, _bot));

    EngagementSnapshot snapshot;
    snapshot.attackers = 2;
    snapshot.load = 200;
    snapshot.hpPct = 100;
    snapshot.manaPct = 100;
    Accumulate(snapshot, _candidate, {_attacker, secondAttacker});

    EXPECT_EQ(snapshot.addsNear, 1u);
    EXPECT_EQ(snapshot.load, 300u);
}
}  // namespace
