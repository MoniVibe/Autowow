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
}

TEST(DungeonGatePolicy, SelectsFirstNotDoneRow)
{
    std::vector<Step> const rows = {Row(0), Row(1), Row(2)};
    std::vector<bool> const done = {true, false, false};
    EXPECT_EQ(SelectStep(rows, {}, false, [&](std::size_t i) { return bool(done[i]); }), 1u);
    EXPECT_EQ(SelectStep(rows, {}, false, [](std::size_t) { return true; }), NoStep);
}

TEST(DungeonGatePolicy, EnforcesOrderAndEvaluatesLazily)
{
    // A later row that already reads done never runs ahead of an earlier unfinished one, and done is
    // not evaluated past the selected row.
    std::vector<Step> const rows = {Row(0), Row(1), Row(2)};
    std::vector<std::size_t> evaluated;
    std::size_t const selected = SelectStep(rows, {}, false, [&](std::size_t i)
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
    EXPECT_EQ(SelectStep(rows, runtime, false, isDone), NoStep);
    EXPECT_EQ(evaluated, std::vector<std::size_t>({1}));
}

TEST(DungeonGatePolicy, KeyRowsSkippedUnlessBypassed)
{
    std::vector<Step> const rows = {Row(0, 5397), Row(1)};
    auto notDone = [](std::size_t) { return false; };
    EXPECT_EQ(SelectStep(rows, {}, false, notDone), 1u);
    EXPECT_EQ(SelectStep(rows, {}, true, notDone), 0u);
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

    // repeat bounds the uses of use/gossip rows; other kinds act every scan.
    Step gong = Row(0);
    gong.repeat = 2;
    EXPECT_EQ(Evaluate(gong, runtime, 10), Wait::Act);
    Step kill = Row(0);
    kill.kind = Kind::KillSet;
    EXPECT_EQ(Evaluate(kill, runtime, 10), Wait::Act);
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
