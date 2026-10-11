/* AutoWoW physical caravan custody proposals. GPL v2 or later. */
#ifndef AUTOWOW_CARAVAN_POLICY_H
#define AUTOWOW_CARAVAN_POLICY_H

#include "BankPolicy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace AutoWowStrategy
{
enum class CaravanState : std::uint8_t { Reserved, Travelling, Halted, ReadyToUnload, Delivered, Returned, Lost };

struct CaravanLot
{
    std::uint64_t itemGuid = 0;
    std::uint32_t itemEntry = 0;
    std::uint64_t quantity = 0;
    std::array<std::uint8_t, 32> metadataFingerprint{};
};

struct CaravanTerms
{
    std::uint64_t shipmentId = 0;
    std::uint64_t contractId = 0;
    std::uint32_t contractVersion = 0;
    BankOwner source;
    BankOwner destination;
    std::uint32_t routeId = 0;
    std::uint32_t routeVersion = 0;
    std::uint32_t lastCheckpoint = 0;
    std::uint64_t carrierGuid = 0;
    std::vector<CaravanLot> cargo;
};

struct CaravanCursor
{
    CaravanState state = CaravanState::Reserved;
    CaravanTerms boundTerms;
    std::uint64_t revision = 0;
    std::uint64_t lastEventId = 0;
    std::uint32_t checkpoint = 0;
};

struct CaravanFacts
{
    std::uint64_t expectedRevision = 0;
    std::uint64_t eventId = 0;
    std::uint64_t shipmentId = 0;
    std::uint32_t routeId = 0;
    std::uint32_t routeVersion = 0;
    std::uint64_t carrierGuid = 0;
    bool manifestCustodyVerified = false;
    bool sourceStockHeld = false;
    bool autonomousCarrierAndMotionVerified = false;
    bool nativeCheckpointVerified = false;
    bool partiesAndRouteAuthorized = false;
};

struct CaravanDecision
{
    bool accepted = false;
    CaravanCursor cursor;
};

inline bool ValidCaravanTerms(CaravanTerms const& t)
{
    if (!t.shipmentId || !t.contractId || !t.contractVersion || !ValidOwner(t.source) ||
        !ValidOwner(t.destination) || SameOwner(t.source, t.destination) ||
        t.source.faction != t.destination.faction || !t.routeId || !t.routeVersion ||
        !t.lastCheckpoint || !t.carrierGuid || t.cargo.empty() || t.cargo.size() > 64)
        return false;
    std::uint64_t units = 0;
    for (std::size_t i = 0; i < t.cargo.size(); ++i)
    {
        auto const& lot = t.cargo[i];
        bool digestPresent = false;
        for (auto byte : lot.metadataFingerprint)
            digestPresent = digestPresent || byte != 0;
        if (!lot.itemGuid || !lot.itemEntry || !lot.quantity || !digestPresent ||
            lot.quantity > std::numeric_limits<std::uint64_t>::max() - units)
            return false;
        units += lot.quantity;
        for (std::size_t j = 0; j < i; ++j)
            if (t.cargo[j].itemGuid == lot.itemGuid)
                return false;
    }
    return true;
}

inline bool SameCaravanTerms(CaravanTerms const& a, CaravanTerms const& b)
{
    if (a.shipmentId != b.shipmentId || a.contractId != b.contractId || a.contractVersion != b.contractVersion ||
        !SameOwner(a.source, b.source) || !SameOwner(a.destination, b.destination) || a.routeId != b.routeId ||
        a.routeVersion != b.routeVersion || a.lastCheckpoint != b.lastCheckpoint || a.carrierGuid != b.carrierGuid ||
        a.cargo.size() != b.cargo.size())
        return false;
    for (std::size_t i = 0; i < a.cargo.size(); ++i)
        if (a.cargo[i].itemGuid != b.cargo[i].itemGuid || a.cargo[i].itemEntry != b.cargo[i].itemEntry ||
            a.cargo[i].quantity != b.cargo[i].quantity ||
            a.cargo[i].metadataFingerprint != b.cargo[i].metadataFingerprint)
            return false;
    return true;
}

inline bool CaravanActionAllowed(CaravanTerms const& t, CaravanCursor const& c, CaravanFacts const& f)
{
    if (!ValidCaravanTerms(t) || f.shipmentId != t.shipmentId || f.routeId != t.routeId ||
        f.routeVersion != t.routeVersion || f.carrierGuid != t.carrierGuid || !f.manifestCustodyVerified ||
        !f.partiesAndRouteAuthorized || f.expectedRevision != c.revision || f.eventId <= c.lastEventId ||
        c.revision == std::numeric_limits<std::uint64_t>::max() || c.checkpoint > t.lastCheckpoint)
        return false;
    if (c.state == CaravanState::Reserved)
        return !c.boundTerms.shipmentId && !c.revision && !c.lastEventId && !c.checkpoint;
    if (!SameCaravanTerms(c.boundTerms, t) || !c.revision || !c.lastEventId)
        return false;
    switch (c.state)
    {
        case CaravanState::Travelling:
            return c.checkpoint < t.lastCheckpoint;
        case CaravanState::Halted:
        case CaravanState::Returned:
        case CaravanState::Lost:
            return true;
        case CaravanState::ReadyToUnload:
        case CaravanState::Delivered:
            return c.checkpoint == t.lastCheckpoint;
        default:
            return false;
    }
}

inline CaravanDecision CaravanAccepted(CaravanCursor next, CaravanFacts const& f)
{
    ++next.revision;
    next.lastEventId = f.eventId;
    return {true, next};
}

inline CaravanDecision PlanCaravanStart(CaravanTerms const& t, CaravanCursor const& c, CaravanFacts const& f)
{
    if (!CaravanActionAllowed(t, c, f) || c.state != CaravanState::Reserved || !f.sourceStockHeld ||
        !f.autonomousCarrierAndMotionVerified || !f.nativeCheckpointVerified)
        return {};
    auto next = c;
    next.boundTerms = t;
    next.state = CaravanState::Travelling;
    return CaravanAccepted(next, f);
}

// Sequential authoritative checkpoint observation; no clock, distance estimate, proximity or teleport inference.
inline CaravanDecision PlanCaravanCheckpoint(CaravanTerms const& t, CaravanCursor const& c,
                                            CaravanFacts const& f, std::uint32_t checkpoint)
{
    if (!CaravanActionAllowed(t, c, f) || c.state != CaravanState::Travelling ||
        !f.autonomousCarrierAndMotionVerified || !f.nativeCheckpointVerified || checkpoint != c.checkpoint + 1)
        return {};
    auto next = c;
    next.checkpoint = checkpoint;
    if (checkpoint == t.lastCheckpoint)
        next.state = CaravanState::ReadyToUnload;
    return CaravanAccepted(next, f);
}

inline CaravanDecision PlanCaravanHalt(CaravanTerms const& t, CaravanCursor const& c, CaravanFacts const& f)
{
    if (!CaravanActionAllowed(t, c, f) || c.state != CaravanState::Travelling)
        return {};
    auto next = c;
    next.state = CaravanState::Halted;
    return CaravanAccepted(next, f);
}

inline CaravanDecision PlanCaravanResume(CaravanTerms const& t, CaravanCursor const& c, CaravanFacts const& f)
{
    if (!CaravanActionAllowed(t, c, f) || c.state != CaravanState::Halted ||
        !f.autonomousCarrierAndMotionVerified || c.checkpoint == t.lastCheckpoint)
        return {};
    auto next = c;
    next.state = CaravanState::Travelling;
    return CaravanAccepted(next, f);
}

// Disposition proof binds the full original manifest and actual recipient/return/loss, not a timer or death alone.
inline CaravanDecision PlanCaravanFinish(CaravanTerms const& t, CaravanCursor const& c, CaravanFacts const& f,
                                        CaravanState disposition, bool exactPhysicalDispositionVerified)
{
    if (!CaravanActionAllowed(t, c, f) || !exactPhysicalDispositionVerified ||
        c.state == CaravanState::Delivered || c.state == CaravanState::Returned || c.state == CaravanState::Lost ||
        c.state == CaravanState::Reserved)
        return {};
    if (disposition == CaravanState::Delivered)
    {
        if (c.state != CaravanState::ReadyToUnload)
            return {};
    }
    else if (disposition != CaravanState::Returned && disposition != CaravanState::Lost)
        return {};
    auto next = c;
    next.state = disposition;
    return CaravanAccepted(next, f);
}
}
#endif
