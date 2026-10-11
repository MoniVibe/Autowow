/* AutoWoW conserved supply contract proposals. GPL v2 or later. */
#ifndef AUTOWOW_SUPPLY_CONTRACT_POLICY_H
#define AUTOWOW_SUPPLY_CONTRACT_POLICY_H

#include "BankPolicy.h"
#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
enum class SupplyState : std::uint8_t { Draft, Funded, Completed, Closed };

struct SupplyTerms
{
    std::uint64_t contractId = 0;
    std::uint32_t version = 0;
    BankOwner buyer;
    BankOwner supplier;
    std::uint32_t itemEntry = 0;
    std::uint64_t units = 0;
    std::uint64_t unitCopper = 0;
    std::uint64_t deadlineUtc = 0;
    std::uint32_t profileId = 0;
};

struct SupplyCursor
{
    SupplyState state = SupplyState::Draft;
    SupplyTerms boundTerms;
    std::uint64_t revision = 0;
    std::uint64_t lastEventId = 0;
    std::uint64_t heldCopper = 0;
    std::uint64_t deliveredUnits = 0;
    std::uint64_t shipmentId = 0;
    std::uint64_t transitUnits = 0;
};

struct SupplySnapshot
{
    SupplyCursor cursor;
    BankBalance buyerPayment;
    BankBalance supplierPayment;
};

// Verified adapter facts, never values accepted from a client. Event IDs are durable per-contract sequence IDs.
struct SupplyFacts
{
    std::uint64_t expectedRevision = 0;
    std::uint64_t eventId = 0;
    std::uint64_t nowUtc = 0;
    bool buyerAuthorized = false;
    bool supplierAuthorized = false;
    bool exactSnapshotAndClockVerified = false;
};

// Exact receipt/claim lookup includes original native GUIDs, counts, metadata and current buyer custody.
struct SupplyReceipt
{
    std::uint64_t contractId = 0;
    std::uint32_t contractVersion = 0;
    std::uint32_t itemEntry = 0;
    std::uint32_t profileId = 0;
    std::uint64_t shipmentId = 0;
    std::uint64_t units = 0;
    bool exactNativeReceiptVerified = false;
    bool uniqueClaimVerified = false;
    bool qualityAndRoleVerified = false;
    bool nativeArrivalVerified = false;
    std::uint64_t nativeReceivedUtc = 0;
};

struct SupplyDecision
{
    bool accepted = false;
    SupplySnapshot next;
};

inline bool SameSupplyTerms(SupplyTerms const& a, SupplyTerms const& b)
{
    return a.contractId == b.contractId && a.version == b.version && SameOwner(a.buyer, b.buyer) &&
           SameOwner(a.supplier, b.supplier) && a.itemEntry == b.itemEntry && a.units == b.units &&
           a.unitCopper == b.unitCopper && a.deadlineUtc == b.deadlineUtc && a.profileId == b.profileId;
}

inline bool ValidSupplyTerms(SupplyTerms const& t)
{
    return t.contractId && t.version && ValidOwner(t.buyer) && ValidOwner(t.supplier) &&
           t.buyer.kind == BankOwnerKind::Guild && t.supplier.kind == BankOwnerKind::Guild &&
           !SameOwner(t.buyer, t.supplier) && t.buyer.faction == t.supplier.faction && t.itemEntry &&
           t.units && t.unitCopper && t.deadlineUtc && t.profileId &&
           t.units <= std::numeric_limits<std::uint64_t>::max() / t.unitCopper;
}

inline bool ValidSupplySnapshot(SupplyTerms const& t, SupplySnapshot const& s)
{
    if (!ValidSupplyTerms(t) || !ValidBalance(s.buyerPayment) || !ValidBalance(s.supplierPayment) ||
        !SameOwner(t.buyer, s.buyerPayment.owner) || !SameOwner(t.supplier, s.supplierPayment.owner) ||
        s.buyerPayment.asset.kind != BankAssetKind::Copper || s.supplierPayment.asset.kind != BankAssetKind::Copper)
        return false;
    auto const& c = s.cursor;
    if (c.deliveredUnits > t.units || c.transitUnits > t.units - c.deliveredUnits ||
        (c.shipmentId == 0) != (c.transitUnits == 0) || c.heldCopper > s.buyerPayment.reserved)
        return false;
    if (c.state == SupplyState::Draft)
        return !c.boundTerms.contractId && !c.revision && !c.lastEventId && !c.heldCopper &&
               !c.deliveredUnits && !c.shipmentId && !c.transitUnits;
    if (!SameSupplyTerms(c.boundTerms, t) || !c.revision || !c.lastEventId)
        return false;
    switch (c.state)
    {
        case SupplyState::Funded:
            return c.deliveredUnits < t.units && c.heldCopper == (t.units - c.deliveredUnits) * t.unitCopper;
        case SupplyState::Completed:
            return c.deliveredUnits == t.units && !c.heldCopper && !c.transitUnits;
        case SupplyState::Closed:
            return c.deliveredUnits < t.units && !c.heldCopper && !c.transitUnits;
        default:
            return false;
    }
}

inline bool SupplyActionAllowed(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f)
{
    return ValidSupplySnapshot(t, s) && f.exactSnapshotAndClockVerified && f.buyerAuthorized &&
           f.expectedRevision == s.cursor.revision && f.eventId > s.cursor.lastEventId && f.nowUtc &&
           s.cursor.revision != std::numeric_limits<std::uint64_t>::max();
}

inline SupplyDecision SupplyAccepted(SupplySnapshot next, SupplyFacts const& f)
{
    ++next.cursor.revision;
    next.cursor.lastEventId = f.eventId;
    return {true, next};
}

inline SupplyDecision PlanSupplyFund(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f)
{
    if (!SupplyActionAllowed(t, s, f) || s.cursor.state != SupplyState::Draft || f.nowUtc >= t.deadlineUtc)
        return {};
    auto const total = t.units * t.unitCopper;
    if (s.buyerPayment.available < total)
        return {};
    auto next = s;
    next.buyerPayment.available -= total;
    next.buyerPayment.reserved += total;
    next.cursor.state = SupplyState::Funded;
    next.cursor.boundTerms = t;
    next.cursor.heldCopper = total;
    return SupplyAccepted(next, f);
}

inline SupplyDecision PlanSupplyDispatch(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f,
                                        std::uint64_t shipmentId, std::uint64_t units, bool exactNativeStockHeld)
{
    if (!SupplyActionAllowed(t, s, f) || !f.supplierAuthorized || !exactNativeStockHeld || !shipmentId ||
        !units || s.cursor.state != SupplyState::Funded || s.cursor.shipmentId || f.nowUtc >= t.deadlineUtc ||
        units > t.units - s.cursor.deliveredUnits)
        return {};
    auto next = s;
    next.cursor.shipmentId = shipmentId;
    next.cursor.transitUnits = units;
    return SupplyAccepted(next, f);
}

inline SupplyDecision PlanSupplyDeliver(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f,
                                       SupplyReceipt const& r)
{
    if (!SupplyActionAllowed(t, s, f) || !f.supplierAuthorized || s.cursor.state != SupplyState::Funded ||
        r.contractId != t.contractId || r.contractVersion != t.version ||
        r.itemEntry != t.itemEntry || r.profileId != t.profileId || !r.units ||
        r.units > t.units - s.cursor.deliveredUnits || !r.exactNativeReceiptVerified ||
        !r.uniqueClaimVerified || !r.qualityAndRoleVerified || !r.shipmentId ||
        r.shipmentId != s.cursor.shipmentId || !r.nativeArrivalVerified || r.units > s.cursor.transitUnits ||
        !r.nativeReceivedUtc || r.nativeReceivedUtc > f.nowUtc || r.nativeReceivedUtc >= t.deadlineUtc)
        return {};
    auto const payment = r.units * t.unitCopper;
    if (payment > s.cursor.heldCopper || !HasRoom(s.supplierPayment, payment))
        return {};
    auto next = s;
    next.cursor.deliveredUnits += r.units;
    next.cursor.heldCopper -= payment;
    next.buyerPayment.reserved -= payment;
    next.supplierPayment.available += payment;
    if (r.shipmentId)
    {
        next.cursor.transitUnits -= r.units;
        if (!next.cursor.transitUnits)
            next.cursor.shipmentId = 0;
    }
    if (next.cursor.deliveredUnits == t.units)
        next.cursor.state = SupplyState::Completed;
    return SupplyAccepted(next, f);
}

// True only after exact native return/loss plus durable shipment reconciliation; neither disposition pays.
inline SupplyDecision PlanSupplyResolveCargo(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f,
                                            std::uint64_t shipmentId, bool exactReturnOrLossVerified)
{
    if (!SupplyActionAllowed(t, s, f) || !f.supplierAuthorized || s.cursor.state != SupplyState::Funded ||
        !shipmentId || shipmentId != s.cursor.shipmentId || !exactReturnOrLossVerified)
        return {};
    auto next = s;
    next.cursor.shipmentId = 0;
    next.cursor.transitUnits = 0;
    return SupplyAccepted(next, f);
}

// Buyer cancellation or observed expiry: outstanding cargo must be reconciled first.
inline SupplyDecision PlanSupplyClose(SupplyTerms const& t, SupplySnapshot const& s, SupplyFacts const& f)
{
    if (!SupplyActionAllowed(t, s, f) || s.cursor.state != SupplyState::Funded || s.cursor.transitUnits)
        return {};
    auto next = s;
    next.buyerPayment.available += next.cursor.heldCopper;
    next.buyerPayment.reserved -= next.cursor.heldCopper;
    next.cursor.heldCopper = 0;
    next.cursor.state = SupplyState::Closed;
    return SupplyAccepted(next, f);
}
}
#endif
