/* AutoWoW physical caravan regressions. GPL v2 or later. */
#ifdef NDEBUG
#error "Caravan tests require assertions enabled"
#endif
#include "CaravanPolicy.h"
#include <cassert>
#include <iostream>
using namespace AutoWowStrategy;
namespace
{
CaravanTerms Terms()
{
    CaravanTerms t;
    t.shipmentId = 500; t.contractId = 1; t.contractVersion = 1;
    t.source = {BankOwnerKind::Guild, SocietyFaction::Alliance, 20};
    t.destination = {BankOwnerKind::Guild, SocietyFaction::Alliance, 10};
    t.routeId = 7; t.routeVersion = 1; t.lastCheckpoint = 3; t.carrierGuid = 70;
    t.cargo = {{800, 100, 2, {123}}, {801, 100, 1, {124}}};
    return t;
}
CaravanFacts Facts(CaravanCursor const& c)
{
    return {c.revision, c.lastEventId + 1, 500, 7, 1, 70, true, true, true, true, true};
}
void JourneyNeedsEvidence()
{
    auto const t = Terms();
    auto s = PlanCaravanStart(t, {}, Facts({}));
    assert(s.accepted && s.cursor.state == CaravanState::Travelling);
    auto c = s.cursor;
    assert(!PlanCaravanCheckpoint(t, c, Facts(c), 3).accepted);
    for (std::uint32_t checkpoint = 1; checkpoint <= 3; ++checkpoint)
    {
        auto f = Facts(c); f.nativeCheckpointVerified = false;
        assert(!PlanCaravanCheckpoint(t, c, f, checkpoint).accepted);
        auto step = PlanCaravanCheckpoint(t, c, Facts(c), checkpoint);
        assert(step.accepted); c = step.cursor;
    }
    assert(c.state == CaravanState::ReadyToUnload);
    assert(!PlanCaravanFinish(t, c, Facts(c), CaravanState::Delivered, false).accepted);
    auto received = PlanCaravanFinish(t, c, Facts(c), CaravanState::Delivered, true);
    assert(received.accepted && received.cursor.state == CaravanState::Delivered);
    assert(!PlanCaravanFinish(t, received.cursor, Facts(received.cursor), CaravanState::Delivered, true).accepted);
}
void ManifestAndCustody()
{
    auto t = Terms();
    auto bad = t; bad.cargo[1].itemGuid = bad.cargo[0].itemGuid;
    assert(!PlanCaravanStart(bad, {}, Facts({})).accepted);
    bad = t; bad.cargo[0].metadataFingerprint = {};
    assert(!PlanCaravanStart(bad, {}, Facts({})).accepted);
    auto f = Facts({}); f.sourceStockHeld = false;
    assert(!PlanCaravanStart(t, {}, f).accepted);
    f = Facts({}); f.nativeCheckpointVerified = false;
    assert(!PlanCaravanStart(t, {}, f).accepted);
    auto c = PlanCaravanStart(t, {}, Facts({})).cursor;
    f = Facts(c); f.carrierGuid = 71;
    assert(!PlanCaravanCheckpoint(t, c, f, 1).accepted);
    bad = t; bad.cargo[0].quantity = 1;
    assert(!PlanCaravanCheckpoint(bad, c, Facts(c), 1).accepted);
    f = Facts(c); f.manifestCustodyVerified = false;
    assert(!PlanCaravanCheckpoint(t, c, f, 1).accepted);
    f = Facts(c); f.eventId = c.lastEventId;
    assert(!PlanCaravanCheckpoint(t, c, f, 1).accepted);
}
void InterruptedDoesNotInventArrival()
{
    auto const t = Terms();
    auto c = PlanCaravanStart(t, {}, Facts({})).cursor;
    auto halted = PlanCaravanHalt(t, c, Facts(c));
    assert(halted.accepted && halted.cursor.state == CaravanState::Halted);
    auto f = Facts(halted.cursor); f.autonomousCarrierAndMotionVerified = false;
    assert(!PlanCaravanResume(t, halted.cursor, f).accepted);
    auto resumed = PlanCaravanResume(t, halted.cursor, Facts(halted.cursor));
    assert(resumed.accepted && resumed.cursor.checkpoint == 0);
    assert(!PlanCaravanFinish(t, resumed.cursor, Facts(resumed.cursor), CaravanState::Delivered, true).accepted);
    assert(!PlanCaravanFinish(t, resumed.cursor, Facts(resumed.cursor), CaravanState::Lost, false).accepted);
    auto lost = PlanCaravanFinish(t, resumed.cursor, Facts(resumed.cursor), CaravanState::Lost, true);
    assert(lost.accepted && lost.cursor.state == CaravanState::Lost);
    assert(!PlanCaravanResume(t, lost.cursor, Facts(lost.cursor)).accepted);
    auto returned = PlanCaravanFinish(t, halted.cursor, Facts(halted.cursor), CaravanState::Returned, true);
    assert(returned.accepted && returned.cursor.state == CaravanState::Returned);
}
}
int main()
{
    JourneyNeedsEvidence(); ManifestAndCustody(); InterruptedDoesNotInventArrival();
    std::cout << "PASS caravan groups3: ordered evidence, exact manifest/custody, interruption/disposition\n";
}
