/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonGatePolicy.h"

#include <iterator>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using namespace DungeonGate;

Step Row(std::uint32_t order, std::uint32_t keyItem = 0, bool optional = false)
{
    return {48, 7, order, Kind::UseGo, 21118 + order, 32930 + order, 0.0f, 0.0f, 0.0f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, keyItem, optional, 1000};
}

auto const noKey = [](std::uint32_t) { return false; };
}

TEST(DungeonGatePolicy, SelectsFirstNotDoneRow)
{
    std::vector<Step> const rows = {Row(0), Row(1), Row(2)};
    std::vector<bool> const done = {true, false, false};
    EXPECT_EQ(SelectStep(rows, {}, false, noKey, [&](std::size_t i) { return bool(done[i]); }), 1u);
    EXPECT_EQ(SelectStep(rows, {}, false, noKey, [](std::size_t) { return true; }), NoStep);
}

TEST(DungeonGatePolicy, EnforcesOrderAndEvaluatesLazily)
{
    // A later row that already reads done never runs ahead of an earlier unfinished one, and done is
    // not evaluated past the selected row.
    std::vector<Step> const rows = {Row(0), Row(1), Row(2)};
    std::vector<std::size_t> evaluated;
    std::size_t const selected = SelectStep(rows, {}, false, noKey, [&](std::size_t i)
    {
        evaluated.push_back(i);
        return i != 0;
    });
    EXPECT_EQ(selected, 0u);
    EXPECT_EQ(evaluated, std::vector<std::size_t>({0}));
}

TEST(DungeonGatePolicy, LatchedDoneAndSkippedRowsAreNotReevaluated)
{
    std::vector<Step> const rows = {Row(0), Row(1), Row(2, 0, true)};
    std::vector<StepRuntime> runtime(3);
    runtime[0].done = true;
    runtime[2].skipped = true;
    std::vector<std::size_t> evaluated;
    auto isDone = [&](std::size_t i)
    {
        evaluated.push_back(i);
        return i == 1;
    };
    EXPECT_EQ(SelectStep(rows, runtime, false, noKey, isDone), NoStep);
    EXPECT_EQ(evaluated, std::vector<std::size_t>({1}));
}

TEST(DungeonGatePolicy, KeyRowsSkippedUnlessHeldOrBypassed)
{
    std::vector<Step> const rows = {Row(0, 5397), Row(1)};
    auto notDone = [](std::size_t) { return false; };
    std::vector<std::uint32_t> asked;
    auto holds = [&](std::uint32_t item)
    {
        asked.push_back(item);
        return item == 5397;
    };
    // Skipped only when no party member holds the key and bypass is off.
    EXPECT_EQ(SelectStep(rows, {}, false, noKey, notDone), 1u);
    EXPECT_EQ(SelectStep(rows, {}, false, holds, notDone), 0u);
    EXPECT_EQ(asked, std::vector<std::uint32_t>({5397}));
    EXPECT_EQ(SelectStep(rows, {}, true, noKey, notDone), 0u);

    // Rows without a key never ask who holds one.
    asked.clear();
    EXPECT_EQ(SelectStep(std::vector<Step>{Row(1)}, {}, false, holds, notDone), 0u);
    EXPECT_TRUE(asked.empty());
}

TEST(DungeonGatePolicy, TimeoutSkipsOptionalAndRetriesRequired)
{
    StepRuntime runtime;
    runtime.acts = 1;
    runtime.firstActMs = 5;
    EXPECT_EQ(Evaluate(Row(0, 0, true), runtime, 999), Wait::Hold);
    EXPECT_EQ(Evaluate(Row(0, 0, true), runtime, 1000), Wait::Skip);
    EXPECT_EQ(Evaluate(Row(0), runtime, 1000), Wait::Retry);

    // Not acted yet: no timeout, act.
    EXPECT_EQ(Evaluate(Row(0), StepRuntime{}, 5000), Wait::Act);

    // repeat bounds the uses of use/gossip/loot/item rows; other kinds act every scan.
    Step gong = Row(0);
    gong.repeat = 2;
    EXPECT_EQ(Evaluate(gong, runtime, 10), Wait::Act);
    Step kill = Row(0);
    kill.kind = Kind::KillSet;
    EXPECT_EQ(Evaluate(kill, runtime, 10), Wait::Act);
    Step loot = Row(0);
    loot.kind = Kind::LootGo;
    EXPECT_EQ(Evaluate(loot, runtime, 10), Wait::Hold);
    Step useItem = Row(0);
    useItem.kind = Kind::UseItemOnGo;
    EXPECT_EQ(Evaluate(useItem, runtime, 10), Wait::Hold);
}

TEST(DungeonGatePolicy, TableIsOrderedAndWellFormed)
{
    for (std::size_t i = 0; i < std::size(Steps); ++i)
    {
        Step const& step = Steps[i];
        EXPECT_TRUE(step.entry) << i;
        EXPECT_GE(step.radius, 5.0f) << i;  // arrival radius must exceed the navigator's 3 yd arrival
        EXPECT_GE(step.repeat, 1u) << i;
        EXPECT_TRUE(step.timeoutMs) << i;
        if (step.kind == Kind::UseGo || step.kind == Kind::Gossip || step.kind == Kind::Escort ||
            step.doneWhen == DoneWhen::GoUsed || step.doneWhen == DoneWhen::Escorting)
        {
            EXPECT_TRUE(step.spawnGuid || step.doneData) << i;
        }
        if (step.kind == Kind::LootGo)
        {
            // The loot row produces the key, so it must never be key-gated itself.
            EXPECT_TRUE(step.spawnGuid && step.doneValue) << i;
            EXPECT_EQ(step.keyItem, 0u) << i;
        }
        if (step.kind == Kind::UseItemOnGo)
            EXPECT_TRUE(step.spawnGuid && step.keyItem) << i;
        if (step.doneWhen == DoneWhen::Unlocked)
            EXPECT_TRUE(step.doneData) << i;
        if (i)
        {
            Step const& previous = Steps[i - 1];
            EXPECT_LT(std::tie(previous.mapId, previous.encounterIdx, previous.stepOrder),
                std::tie(step.mapId, step.encounterIdx, step.stepOrder)) << i;
        }
    }
    EXPECT_TRUE(MapHasSteps(109));
    EXPECT_FALSE(MapHasSteps(389));
    EXPECT_EQ(StepsFor(48, 7).size(), 9u);
    EXPECT_LT(GoalId(0), GoalId(1));
    EXPECT_TRUE(GoalId(0) & GoalIdBase);
}

TEST(DungeonGatePolicy, DeadminesIronCladDoorGatesSmiteThroughVanCleef)
{
    // Gilnid (idx2) and earlier stay ungated by the door; Mr. Smite..VanCleef (idx3-6) each loot the
    // Defias Gunpowder then fire the cannon, both done once the Iron Clad Door (30534) opens.
    EXPECT_TRUE(StepsFor(36, 2).empty());
    for (std::uint32_t encounter = 3; encounter <= 6; ++encounter)
    {
        std::vector<std::size_t> const rows = StepsFor(36, encounter);
        ASSERT_EQ(rows.size(), 2u) << encounter;
        Step const& loot = Steps[rows[0]];
        Step const& cannon = Steps[rows[1]];
        EXPECT_EQ(loot.kind, Kind::LootGo);
        EXPECT_EQ(loot.entry, 17155u);
        EXPECT_EQ(loot.spawnGuid, 26203u);
        EXPECT_EQ(loot.doneWhen, DoneWhen::Unlocked);
        EXPECT_EQ(loot.doneData, 30534u);
        EXPECT_EQ(loot.doneValue, 5397u);
        EXPECT_EQ(cannon.kind, Kind::UseItemOnGo);
        EXPECT_EQ(cannon.entry, 16398u);
        EXPECT_EQ(cannon.spawnGuid, 26205u);
        EXPECT_EQ(cannon.keyItem, loot.doneValue);
        EXPECT_EQ(cannon.doneWhen, DoneWhen::Unlocked);
        EXPECT_EQ(cannon.doneData, 30534u);
        EXPECT_EQ(cannon.doneValue, 0u);

        // No key held: loot first, cannon row key-gated; key held: cannon next without BypassKeys.
        std::vector<Step> const encounterRows = {loot, cannon};
        EXPECT_EQ(SelectStep(encounterRows, {}, false, noKey, [](std::size_t) { return false; }), 0u);
        auto held = [](std::uint32_t item) { return item == 5397; };
        EXPECT_EQ(SelectStep(encounterRows, {}, false, held, [](std::size_t i) { return i == 0; }), 1u);
        EXPECT_EQ(SelectStep(encounterRows, {}, false, noKey, [](std::size_t i) { return i == 0; }),
            NoStep);
    }
}

TEST(DungeonGatePolicy, SlopeFreeNavmeshPathWalksDirect)
{
    // Soak S55: the Gunpowder row was selected but only the slope-free probe reaches the chest from the
    // foundry exit; travel nodes are the last resort, not the answer to a slope-check artifact.
    EXPECT_EQ(SelectApproach(true, false), Approach::Direct);
    EXPECT_EQ(SelectApproach(false, true), Approach::Direct);
    EXPECT_EQ(SelectApproach(false, false), Approach::TravelNodes);
}

TEST(DungeonGatePolicy, PerScanGateLogsAreRateLimited)
{
    EXPECT_TRUE(ShouldLog(false, 0));
    EXPECT_FALSE(ShouldLog(true, 0));
    EXPECT_FALSE(ShouldLog(true, LogRepeatMs - 1));
    EXPECT_TRUE(ShouldLog(true, LogRepeatMs));
}
