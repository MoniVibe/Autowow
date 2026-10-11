/* AutoWoW workshop proposal regressions. GPL v2 or later. No native operations. */
#ifdef NDEBUG
#error "Workshop tests require assertions enabled"
#endif

#include "WorkshopPolicy.h"

#include <cassert>
#include <limits>

using namespace AutoWowStrategy;

namespace
{
WorkshopProfile Profile()
{
    return {10, 1, 100, WorkshopKind::Routine, 1, {1000, 0, 0, 0, 0}, true};
}

WorkshopOrder Order()
{
    return {1, 200, 100, 10, 1, WorkshopKind::Routine, 10};
}

WorkshopWorkFacts Work(WorkshopCursor const& cursor, std::uint64_t milliseconds)
{
    return {cursor.revision, cursor.lastSliceId + 1, milliseconds, milliseconds,
            true, true, true, true, true, true};
}

WorkshopCraftReceipt Receipt(WorkshopCursor const& cursor)
{
    return {1, 200, 100, 10, 1, cursor.revision, cursor.completedPieces + 1,
            300 + cursor.completedPieces, 400 + cursor.completedPieces,
            1, true, true, true, true};
}

void TestBatchRequiresTenIndependentCompletions()
{
    auto const order = Order();
    auto const profile = Profile();
    auto cursor = BeginWorkshopOrder(order, profile, true).cursor;
    for (std::uint32_t piece = 0; piece < 10; ++piece)
    {
        auto const ready = PlanWorkshopWork(order, profile, cursor, Work(cursor, 1000));
        assert(ready.result == WorkshopResult::ReadyToCraft && ready.cursor.completedPieces == piece);
        assert(ready.cursor.stageWorkedMs == 1000);
        auto const done = PlanWorkshopCraft(order, profile, ready.cursor, Receipt(ready.cursor));
        assert(done.cursor.completedPieces == piece + 1);
        assert(done.result == (piece == 9 ? WorkshopResult::Completed : WorkshopResult::Advanced));
        cursor = done.cursor;
    }
    assert(PlanWorkshopWork(order, profile, cursor, Work(cursor, 1000)).result == WorkshopResult::Completed);
}

void TestTimingAndPause()
{
    auto const order = Order();
    auto const profile = Profile();
    auto const initial = BeginWorkshopOrder(order, profile, true).cursor;
    auto const partial = PlanWorkshopWork(order, profile, initial, Work(initial, 400));
    assert(partial.result == WorkshopResult::Advanced && partial.cursor.stageWorkedMs == 400);
    auto blocked = Work(partial.cursor, 600);
    blocked.materialsReserved = false;
    auto const pause = PlanWorkshopWork(order, profile, partial.cursor, blocked);
    assert(pause.result == WorkshopResult::Paused && pause.cursor.stageWorkedMs == 400);
    assert(pause.cursor.lastSliceId == blocked.sliceId && pause.cursor.revision == partial.cursor.revision + 1);
    assert(PlanWorkshopWork(order, profile, pause.cursor, blocked).result == WorkshopResult::Refused);
    auto const resume = PlanWorkshopWork(order, profile, pause.cursor, Work(pause.cursor, 600));
    assert(resume.result == WorkshopResult::ReadyToCraft && resume.cursor.completedPieces == 0);
    auto gap = Work(partial.cursor, 600);
    gap.continuousObservation = false;
    assert(PlanWorkshopWork(order, profile, partial.cursor, gap).result == WorkshopResult::Paused);
    gap = Work(partial.cursor, 600);
    gap.sameClockSession = false;
    assert(PlanWorkshopWork(order, profile, partial.cursor, gap).result == WorkshopResult::Paused);
}

void TestLegendaryStagesAndExcessTime()
{
    auto order = Order();
    order.kind = WorkshopKind::Legendary;
    order.quantity = 1;
    auto profile = Profile();
    profile.kind = WorkshopKind::Legendary;
    profile.stageCount = 5;
    profile.stageDurationMs = {100, 200, 300, 400, 500};
    auto cursor = BeginWorkshopOrder(order, profile, true).cursor;
    assert(PlanWorkshopCraft(order, profile, cursor, Receipt(cursor)).result == WorkshopResult::Refused);
    for (std::uint8_t stage = 0; stage < 5; ++stage)
    {
        auto const next = PlanWorkshopWork(order, profile, cursor, Work(cursor, 10000));
        assert(next.cursor.completedPieces == 0);
        if (stage < 4)
            assert(next.result == WorkshopResult::Advanced && next.cursor.stageIndex == stage + 1 &&
                next.cursor.stageWorkedMs == 0);
        else
            assert(next.result == WorkshopResult::ReadyToCraft && next.cursor.stageWorkedMs == 500);
        cursor = next.cursor;
    }
    assert(PlanWorkshopCraft(order, profile, cursor, Receipt(cursor)).result == WorkshopResult::Completed);
}

void TestSettlementRefusalsAndReplay()
{
    auto const order = Order();
    auto const profile = Profile();
    auto const initial = BeginWorkshopOrder(order, profile, true).cursor;
    auto const ready = PlanWorkshopWork(order, profile, initial, Work(initial, 1000)).cursor;
    auto const laterTick = PlanWorkshopWork(order, profile, ready, Work(ready, 1000));
    assert(laterTick.result == WorkshopResult::ReadyToCraft && laterTick.cursor.revision == ready.revision &&
        laterTick.cursor.lastSliceId == ready.lastSliceId && laterTick.cursor.completedPieces == 0);
    assert(PlanWorkshopCraft(order, profile, laterTick.cursor, Receipt(ready)).result == WorkshopResult::Advanced);
    auto receipt = Receipt(ready);
    receipt.durableSettlementConfirmed = false;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    receipt.nativeReagentsConsumed = false;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    receipt.exactOutputVerified = false;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    receipt.outputCount = 10;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    receipt.crafterGuid = 201;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    receipt.unitOrdinal = 2;
    assert(PlanWorkshopCraft(order, profile, ready, receipt).result == WorkshopResult::Refused);
    receipt = Receipt(ready);
    auto const settled = PlanWorkshopCraft(order, profile, ready, receipt);
    assert(PlanWorkshopCraft(order, profile, settled.cursor, receipt).result == WorkshopResult::Refused);
    assert(ready.completedPieces == 0 && ready.stageWorkedMs == 1000);
}

void TestInvalidInputsAndOverflow()
{
    auto order = Order();
    auto profile = Profile();
    assert(BeginWorkshopOrder(order, profile, false).result == WorkshopResult::Refused);
    order.quantity = 0;
    assert(BeginWorkshopOrder(order, profile, true).result == WorkshopResult::Refused);
    order = Order();
    profile.timingCatalogueValidated = false;
    assert(BeginWorkshopOrder(order, profile, true).result == WorkshopResult::Refused);
    profile = Profile();
    profile.stageDurationMs[1] = 1;
    assert(BeginWorkshopOrder(order, profile, true).result == WorkshopResult::Refused);
    profile = Profile();
    order.kind = static_cast<WorkshopKind>(99);
    assert(BeginWorkshopOrder(order, profile, true).result == WorkshopResult::Refused);
    order = Order();
    auto cursor = BeginWorkshopOrder(order, profile, true).cursor;
    auto facts = Work(cursor, 100);
    facts.activeWorkMs = 101;
    assert(PlanWorkshopWork(order, profile, cursor, facts).result == WorkshopResult::Refused);
    cursor.revision = std::numeric_limits<std::uint64_t>::max();
    assert(PlanWorkshopWork(order, profile, cursor, Work(cursor, 100)).result == WorkshopResult::Refused);
    cursor = BeginWorkshopOrder(order, profile, true).cursor;
    cursor.completedPieces = 1;
    cursor.stageWorkedMs = 1;
    cursor.stageIndex = 1;
    assert(PlanWorkshopWork(order, profile, cursor, Work(cursor, 100)).result == WorkshopResult::Refused);
    profile.stageDurationMs[0] = 0;
    assert(BeginWorkshopOrder(order, profile, true).result == WorkshopResult::Refused);
}
}

int main()
{
    TestBatchRequiresTenIndependentCompletions();
    TestTimingAndPause();
    TestLegendaryStagesAndExcessTime();
    TestSettlementRefusalsAndReplay();
    TestInvalidInputsAndOverflow();
}
