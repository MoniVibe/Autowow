/* AutoWoW original native bank and deal policy. GPL v2 or later. */
#ifndef AUTOWOW_BANK_POLICY_H
#define AUTOWOW_BANK_POLICY_H

#include <array>
#include <cstdint>

namespace AutoWowStrategy
{
enum class SocietyFaction
{
    Alliance,
    Horde
};

enum class BankOwnerKind
{
    Faction,
    Guild
};

enum class BankAssetKind
{
    Item,
    Copper
};

struct BankOwner
{
    BankOwnerKind kind = BankOwnerKind::Faction;
    SocietyFaction faction = SocietyFaction::Alliance;
    std::uint32_t guildId = 0;
};

struct BankAsset
{
    BankAssetKind kind = BankAssetKind::Item;
    std::uint32_t itemEntry = 0;
};

inline bool ValidOwner(BankOwner const& owner)
{
    if (owner.faction != SocietyFaction::Alliance && owner.faction != SocietyFaction::Horde)
        return false;
    return (owner.kind == BankOwnerKind::Faction && owner.guildId == 0) ||
           (owner.kind == BankOwnerKind::Guild && owner.guildId != 0);
}

inline bool ValidAsset(BankAsset const& asset)
{
    return (asset.kind == BankAssetKind::Item && asset.itemEntry != 0) ||
           (asset.kind == BankAssetKind::Copper && asset.itemEntry == 0);
}

inline bool SameOwner(BankOwner const& left, BankOwner const& right)
{
    return left.kind == right.kind && left.faction == right.faction && left.guildId == right.guildId;
}

inline bool SameAsset(BankAsset const& left, BankAsset const& right)
{
    return left.kind == right.kind && left.itemEntry == right.itemEntry;
}

struct BankBalance
{
    BankOwner owner;
    BankAsset asset;
    std::uint64_t available = 0;
    std::uint64_t reserved = 0;
    std::uint64_t capacity = 0;
};

inline bool ValidBalance(BankBalance const& balance)
{
    return ValidOwner(balance.owner) && ValidAsset(balance.asset) && balance.available <= balance.capacity &&
           balance.reserved <= balance.capacity - balance.available;
}

inline bool HasRoom(BankBalance const& balance, std::uint64_t quantity)
{
    return ValidBalance(balance) && quantity <= balance.capacity - balance.available - balance.reserved;
}

struct BankTransferDecision
{
    bool accepted = false;
    BankBalance source;
    BankBalance destination;
};

// Proposal only. The adapter binds authority, identities, versions and one event in an atomic transaction.
inline BankTransferDecision PlanBankTransfer(BankBalance const& source, BankBalance const& destination,
                                            std::uint64_t quantity, bool sourceAuthorized,
                                            bool destinationAuthorized)
{
    if (!sourceAuthorized || !destinationAuthorized || !quantity || !ValidBalance(source) ||
        !ValidBalance(destination) || SameOwner(source.owner, destination.owner) ||
        source.owner.faction != destination.owner.faction || !SameAsset(source.asset, destination.asset) ||
        quantity > source.available || !HasRoom(destination, quantity))
        return {};

    BankBalance nextSource = source;
    BankBalance nextDestination = destination;
    nextSource.available -= quantity;
    nextDestination.available += quantity;
    return {true, nextSource, nextDestination};
}

enum class BankDealState
{
    Proposed,
    Reserved,
    Settled,
    Cancelled
};

enum class BankDealAction
{
    Reserve,
    Settle,
    Cancel
};

struct BankDeal
{
    std::uint64_t id = 0;
    BankDealState state = BankDealState::Proposed;
    std::uint64_t expiresAt = 0;
    std::uint64_t offeredQuantity = 0;
    std::uint64_t requestedQuantity = 0;
    std::uint64_t offeredHeld = 0;
    std::uint64_t requestedHeld = 0;
};

// Four distinct account/asset keys: seller goods, buyer goods, buyer payment, seller payment.
// Barter uses a different requested item; purchases use copper. Physical inventory is not a second balance.
struct BankDealSnapshot
{
    BankDeal deal;
    BankBalance sellerGoods;
    BankBalance buyerGoods;
    BankBalance buyerPayment;
    BankBalance sellerPayment;
};

inline bool ValidDealSnapshot(BankDealSnapshot const& snapshot)
{
    for (BankBalance const& balance : std::array<BankBalance, 4>{snapshot.sellerGoods, snapshot.buyerGoods,
                                                               snapshot.buyerPayment, snapshot.sellerPayment})
        if (!ValidBalance(balance))
            return false;

    BankDeal const& deal = snapshot.deal;
    if (!deal.id || !deal.expiresAt || !deal.offeredQuantity || !deal.requestedQuantity ||
        !SameOwner(snapshot.sellerGoods.owner, snapshot.sellerPayment.owner) ||
        !SameOwner(snapshot.buyerGoods.owner, snapshot.buyerPayment.owner) ||
        SameOwner(snapshot.sellerGoods.owner, snapshot.buyerGoods.owner) ||
        snapshot.sellerGoods.owner.faction != snapshot.buyerGoods.owner.faction ||
        !SameAsset(snapshot.sellerGoods.asset, snapshot.buyerGoods.asset) ||
        !SameAsset(snapshot.buyerPayment.asset, snapshot.sellerPayment.asset) ||
        SameAsset(snapshot.sellerGoods.asset, snapshot.buyerPayment.asset))
        return false;

    if (deal.state == BankDealState::Reserved)
        return deal.offeredHeld == deal.offeredQuantity && deal.requestedHeld == deal.requestedQuantity &&
               deal.offeredHeld <= snapshot.sellerGoods.reserved &&
               deal.requestedHeld <= snapshot.buyerPayment.reserved;

    return (deal.state == BankDealState::Proposed || deal.state == BankDealState::Settled ||
            deal.state == BankDealState::Cancelled) && !deal.offeredHeld && !deal.requestedHeld;
}

struct BankDealDecision
{
    bool accepted = false;
    BankDealSnapshot next;
};

// Durable deal rows own their reservations. Fresh authority and version checks are adapter requirements.
// Both legs settle together; transport/mail is a later, distinct physical-delivery state machine.
inline BankDealDecision PlanBankDeal(BankDealSnapshot const& prior, BankDealAction action,
                                    std::uint64_t now, bool sellerAuthorized, bool buyerAuthorized,
                                    bool cancellationAuthorized)
{
    if (!now || !ValidDealSnapshot(prior))
        return {};

    BankDealSnapshot next = prior;
    BankDeal& deal = next.deal;
    if (action == BankDealAction::Reserve)
    {
        if (deal.state != BankDealState::Proposed || now >= deal.expiresAt ||
            !sellerAuthorized || !buyerAuthorized || deal.offeredQuantity > next.sellerGoods.available ||
            deal.requestedQuantity > next.buyerPayment.available ||
            !HasRoom(next.buyerGoods, deal.offeredQuantity) ||
            !HasRoom(next.sellerPayment, deal.requestedQuantity))
            return {};
        next.sellerGoods.available -= deal.offeredQuantity;
        next.sellerGoods.reserved += deal.offeredQuantity;
        next.buyerPayment.available -= deal.requestedQuantity;
        next.buyerPayment.reserved += deal.requestedQuantity;
        deal.offeredHeld = deal.offeredQuantity;
        deal.requestedHeld = deal.requestedQuantity;
        deal.state = BankDealState::Reserved;
    }
    else if (action == BankDealAction::Settle)
    {
        if (deal.state != BankDealState::Reserved || now >= deal.expiresAt ||
            !sellerAuthorized || !buyerAuthorized || !HasRoom(next.buyerGoods, deal.offeredHeld) ||
            !HasRoom(next.sellerPayment, deal.requestedHeld))
            return {};
        next.sellerGoods.reserved -= deal.offeredHeld;
        next.buyerGoods.available += deal.offeredHeld;
        next.buyerPayment.reserved -= deal.requestedHeld;
        next.sellerPayment.available += deal.requestedHeld;
        deal.offeredHeld = 0;
        deal.requestedHeld = 0;
        deal.state = BankDealState::Settled;
    }
    else if (action == BankDealAction::Cancel)
    {
        if (!cancellationAuthorized ||
            (deal.state != BankDealState::Proposed && deal.state != BankDealState::Reserved))
            return {};
        next.sellerGoods.reserved -= deal.offeredHeld;
        next.sellerGoods.available += deal.offeredHeld;
        next.buyerPayment.reserved -= deal.requestedHeld;
        next.buyerPayment.available += deal.requestedHeld;
        deal.offeredHeld = 0;
        deal.requestedHeld = 0;
        deal.state = BankDealState::Cancelled;
    }
    else
        return {};

    return {ValidDealSnapshot(next), next};
}
} // namespace AutoWowStrategy

#endif
