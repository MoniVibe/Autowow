/* AutoWoW supply contract regressions. GPL v2 or later. */
#ifdef NDEBUG
#error "Supply tests require assertions enabled"
#endif
#include "SupplyContractPolicy.h"
#include <cassert>
#include <limits>
#include <iostream>
using namespace AutoWowStrategy;
namespace
{
SupplyTerms Terms()
{
    return {1, 1, {BankOwnerKind::Guild, SocietyFaction::Alliance, 10},
            {BankOwnerKind::Guild, SocietyFaction::Alliance, 20}, 100, 5, 10, 1000, 77};
}
SupplySnapshot Initial()
{
    auto const t = Terms();
    return {{}, {t.buyer, {BankAssetKind::Copper, 0}, 200, 25, 300},
            {t.supplier, {BankAssetKind::Copper, 0}, 0, 0, 300}};
}
SupplyFacts Facts(SupplySnapshot const& s)
{
    return {s.cursor.revision, s.cursor.lastEventId + 1, 100, true, true, true};
}
SupplyReceipt Receipt(std::uint64_t units, std::uint64_t shipment = 500)
{
    return {1, 1, 100, 77, shipment, units, true, true, true, shipment != 0, 100};
}
SupplySnapshot Intent()
{
    auto s = PlanSupplyFund(Terms(), Initial(), Facts(Initial())).next;
    return PlanSupplyDispatch(Terms(), s, Facts(s), 500, 5, true).next;
}
void PartialAndConserved()
{
    auto const t = Terms();
    auto s = PlanSupplyFund(t, Initial(), Facts(Initial())).next;
    assert(s.cursor.state == SupplyState::Funded && s.cursor.heldCopper == 50);
    assert(s.buyerPayment.available == 150 && s.buyerPayment.reserved == 75);
    s = PlanSupplyDispatch(t, s, Facts(s), 500, 5, true).next;
    auto a = PlanSupplyDeliver(t, s, Facts(s), Receipt(2));
    assert(a.accepted && a.next.cursor.deliveredUnits == 2 && a.next.cursor.heldCopper == 30);
    assert(a.next.supplierPayment.available == 20 && a.next.buyerPayment.reserved == 55);
    auto b = PlanSupplyDeliver(t, a.next, Facts(a.next), Receipt(3));
    assert(b.accepted && b.next.cursor.state == SupplyState::Completed);
    assert(b.next.buyerPayment.reserved == 25 && b.next.supplierPayment.available == 50);
    assert(b.next.buyerPayment.available + b.next.buyerPayment.reserved + b.next.supplierPayment.available == 225);
    assert(!PlanSupplyDeliver(t, b.next, Facts(b.next), Receipt(1)).accepted);
}
void TransitAndResolution()
{
    auto const t = Terms();
    auto s = PlanSupplyFund(t, Initial(), Facts(Initial())).next;
    auto d = PlanSupplyDispatch(t, s, Facts(s), 500, 3, true);
    assert(d.accepted && d.next.cursor.transitUnits == 3);
    assert(!PlanSupplyClose(t, d.next, Facts(d.next)).accepted);
    assert(!PlanSupplyDispatch(t, d.next, Facts(d.next), 501, 1, true).accepted);
    assert(!PlanSupplyDeliver(t, d.next, Facts(d.next), Receipt(1, 0)).accepted);
    auto receipt = Receipt(2, 500);
    receipt.nativeArrivalVerified = false;
    assert(!PlanSupplyDeliver(t, d.next, Facts(d.next), receipt).accepted);
    receipt.nativeArrivalVerified = true;
    auto paid = PlanSupplyDeliver(t, d.next, Facts(d.next), receipt);
    assert(paid.accepted && paid.next.cursor.transitUnits == 1 && paid.next.cursor.deliveredUnits == 2);
    assert(!PlanSupplyResolveCargo(t, paid.next, Facts(paid.next), 499, false).accepted);
    assert(!PlanSupplyResolveCargo(t, paid.next, Facts(paid.next), 500, false).accepted);
    auto returned = PlanSupplyResolveCargo(t, paid.next, Facts(paid.next), 500, true);
    assert(returned.accepted && !returned.next.cursor.transitUnits && returned.next.cursor.heldCopper == 30);
    auto closed = PlanSupplyClose(t, returned.next, Facts(returned.next));
    assert(closed.accepted && closed.next.buyerPayment.reserved == 25 && closed.next.buyerPayment.available == 180);
    assert(closed.next.supplierPayment.available == 20 && closed.next.cursor.state == SupplyState::Closed);
}
void Refusals()
{
    auto t = Terms();
    auto initial = Initial();
    auto f = Facts(initial);
    f.buyerAuthorized = false;
    assert(!PlanSupplyFund(t, initial, f).accepted);
    f = Facts(initial); f.nowUtc = 1000;
    assert(!PlanSupplyFund(t, initial, f).accepted);
    t.unitCopper = std::numeric_limits<std::uint64_t>::max();
    assert(!PlanSupplyFund(t, initial, Facts(initial)).accepted);
    t = Terms();
    auto s = Intent();
    f = Facts(s); f.expectedRevision = 0;
    assert(!PlanSupplyDeliver(t, s, f, Receipt(1)).accepted);
    f = Facts(s); f.eventId = s.cursor.lastEventId;
    assert(!PlanSupplyDeliver(t, s, f, Receipt(1)).accepted);
    f = Facts(s); f.nowUtc = 1000;
    auto late = Receipt(1); late.nativeReceivedUtc = 1000;
    assert(!PlanSupplyDeliver(t, s, f, late).accepted);
    auto r = Receipt(1); r.qualityAndRoleVerified = false;
    assert(!PlanSupplyDeliver(t, s, Facts(s), r).accepted);
    r = Receipt(1); r.exactNativeReceiptVerified = false;
    assert(!PlanSupplyDeliver(t, s, Facts(s), r).accepted);
    r = Receipt(1); r.uniqueClaimVerified = false;
    assert(!PlanSupplyDeliver(t, s, Facts(s), r).accepted);
    r = Receipt(1); r.profileId = 999;
    assert(!PlanSupplyDeliver(t, s, Facts(s), r).accepted);
    assert(!PlanSupplyDeliver(t, s, Facts(s), Receipt(6)).accepted);
    s.supplierPayment.capacity = 5;
    assert(!PlanSupplyDeliver(t, s, Facts(s), Receipt(1)).accepted);
    s = Intent();
    s.cursor.heldCopper = 25;
    assert(!PlanSupplyClose(t, s, Facts(s)).accepted);
    s = Intent();
    s.cursor.revision = std::numeric_limits<std::uint64_t>::max();
    assert(!PlanSupplyDeliver(t, s, Facts(s), Receipt(1, 0)).accepted);
}
void DelayedSettlementHonorsOriginalReceiptTime()
{
    auto const t = Terms();
    auto s = Intent();
    auto f = Facts(s); f.nowUtc = 1001;
    auto onTime = Receipt(2); onTime.nativeReceivedUtc = 999;
    assert(PlanSupplyDeliver(t, s, f, onTime).accepted);
    auto future = Receipt(2); future.nativeReceivedUtc = 1002;
    assert(!PlanSupplyDeliver(t, s, f, future).accepted);
    auto changed = t; changed.unitCopper = 11;
    assert(!PlanSupplyDeliver(changed, s, Facts(s), Receipt(1)).accepted);
}
void HandoffMustBeReservedBeforeNativeMutation()
{
    auto const t = Terms();
    auto s = PlanSupplyFund(t, Initial(), Facts(Initial())).next;
    // A native transfer outside this contract's registered intent cannot consume its payment hold.
    assert(!PlanSupplyDeliver(t, s, Facts(s), Receipt(1)).accepted);
}
}
int main()
{
    PartialAndConserved(); TransitAndResolution(); Refusals(); HandoffMustBeReservedBeforeNativeMutation();
    DelayedSettlementHonorsOriginalReceiptTime();
    std::cout << "PASS supply groups5: conservation, transit, refusals, pre-handoff intent, receipt-time settlement\n";
}
