/* AutoWoW native bank policy tests. GPL v2 or later. Not part of runtime source collection. */
#ifdef NDEBUG
#error "Bank policy tests require assertions enabled"
#endif

#include "BankPolicy.h"

#include <cassert>
#include <limits>

int main()
{
    using namespace AutoWowStrategy;
    BankOwner const faction{BankOwnerKind::Faction, SocietyFaction::Alliance, 0};
    BankOwner const smiths{BankOwnerKind::Guild, SocietyFaction::Alliance, 11};
    BankOwner const adventurers{BankOwnerKind::Guild, SocietyFaction::Alliance, 12};
    BankAsset const copperOre{BankAssetKind::Item, 2770};
    BankAsset const silverOre{BankAssetKind::Item, 2775};
    BankAsset const money{BankAssetKind::Copper, 0};
    BankBalance const treasury{faction, copperOre, 100, 20, 200};
    BankBalance const guild{smiths, copperOre, 10, 0, 100};
    assert(ValidOwner(faction) && ValidOwner(smiths));
    assert(!ValidOwner({BankOwnerKind::Guild, SocietyFaction::Alliance, 0}));
    assert(!ValidOwner({BankOwnerKind::Faction, SocietyFaction::Alliance, 11}));
    assert(!ValidOwner({BankOwnerKind::Faction, static_cast<SocietyFaction>(99), 0}));
    assert(!ValidOwner({static_cast<BankOwnerKind>(99), SocietyFaction::Alliance, 0}));
    assert(ValidAsset(copperOre) && ValidAsset(money));
    assert(!ValidAsset({BankAssetKind::Item, 0}));
    assert(!ValidAsset({BankAssetKind::Copper, 2770}));
    assert(!ValidAsset({static_cast<BankAssetKind>(99), 0}));

    auto const moved = PlanBankTransfer(treasury, guild, 25, true, true);
    assert(moved.accepted && moved.source.available == 75 && moved.destination.available == 35);
    assert(moved.source.reserved == 20 && treasury.available == 100 && guild.available == 10);
    assert(!PlanBankTransfer(treasury, guild, 101, true, true).accepted);
    assert(!PlanBankTransfer(treasury, guild, 91, true, true).accepted);
    assert(!PlanBankTransfer(treasury, guild, 0, true, true).accepted);
    assert(!PlanBankTransfer(treasury, guild, 1, false, true).accepted);
    assert(!PlanBankTransfer(treasury, guild, 1, true, false).accepted);
    assert(!PlanBankTransfer(treasury, treasury, 1, true, true).accepted);
    auto foreign = guild;
    foreign.owner.faction = SocietyFaction::Horde;
    assert(!PlanBankTransfer(treasury, foreign, 1, true, true).accepted);
    foreign = guild;
    foreign.asset = silverOre;
    assert(!PlanBankTransfer(treasury, foreign, 1, true, true).accepted);
    foreign = guild;
    foreign.reserved = 90;
    assert(!PlanBankTransfer(treasury, foreign, 1, true, true).accepted);
    foreign.reserved = 91;
    assert(!ValidBalance(foreign));
    std::uint64_t const maximum = std::numeric_limits<std::uint64_t>::max();
    BankBalance const full{smiths, copperOre, maximum, 0, maximum};
    assert(ValidBalance(full) && !HasRoom(full, 1));
    auto malformed = full;
    malformed.reserved = 1;
    assert(!ValidBalance(malformed));

    BankDealSnapshot sale;
    sale.deal = {1, BankDealState::Proposed, 1000, 20, 600, 0, 0};
    sale.sellerGoods = {smiths, copperOre, 50, 5, 100};
    sale.buyerGoods = {adventurers, copperOre, 1, 0, 100};
    sale.buyerPayment = {adventurers, money, 1000, 10, 2000};
    sale.sellerPayment = {smiths, money, 100, 0, 2000};
    assert(ValidDealSnapshot(sale));
    auto const held = PlanBankDeal(sale, BankDealAction::Reserve, 100, true, true, false);
    assert(held.accepted && held.next.deal.state == BankDealState::Reserved);
    assert(held.next.sellerGoods.available == 30 && held.next.sellerGoods.reserved == 25);
    assert(held.next.buyerPayment.available == 400 && held.next.buyerPayment.reserved == 610);
    assert(held.next.buyerGoods.available == 1 && held.next.sellerPayment.available == 100);
    assert(!PlanBankDeal(held.next, BankDealAction::Reserve, 101, true, true, false).accepted);
    auto const settled = PlanBankDeal(held.next, BankDealAction::Settle, 102, true, true, false);
    assert(settled.accepted && settled.next.deal.state == BankDealState::Settled);
    assert(settled.next.sellerGoods.reserved == 5 && settled.next.buyerPayment.reserved == 10);
    assert(settled.next.buyerGoods.available == 21 && settled.next.sellerPayment.available == 700);
    assert(settled.next.sellerGoods.available + settled.next.sellerGoods.reserved +
           settled.next.buyerGoods.available == 56);
    assert(settled.next.buyerPayment.available + settled.next.buyerPayment.reserved +
           settled.next.sellerPayment.available == 1110);
    assert(!PlanBankDeal(settled.next, BankDealAction::Settle, 103, true, true, false).accepted);
    assert(!PlanBankDeal(settled.next, BankDealAction::Cancel, 103, true, true, true).accepted);
    assert(!PlanBankDeal(sale, BankDealAction::Settle, 100, true, true, false).accepted);
    assert(!PlanBankDeal(sale, BankDealAction::Reserve, 1000, true, true, false).accepted);
    assert(!PlanBankDeal(sale, BankDealAction::Reserve, 100, false, true, false).accepted);
    assert(!PlanBankDeal(sale, BankDealAction::Reserve, 100, true, false, false).accepted);
    assert(!PlanBankDeal(sale, BankDealAction::Reserve, 0, true, true, false).accepted);
    assert(!PlanBankDeal(sale, static_cast<BankDealAction>(99), 100, true, true, false).accepted);
    assert(!PlanBankDeal(held.next, BankDealAction::Settle, 1000, true, true, false).accepted);
    assert(!PlanBankDeal(held.next, BankDealAction::Settle, 102, false, true, false).accepted);
    assert(!PlanBankDeal(held.next, BankDealAction::Cancel, 102, true, true, false).accepted);
    auto const cancelled = PlanBankDeal(held.next, BankDealAction::Cancel, 1001, false, false, true);
    assert(cancelled.accepted && cancelled.next.deal.state == BankDealState::Cancelled);
    assert(cancelled.next.sellerGoods.available == 50 && cancelled.next.sellerGoods.reserved == 5);
    assert(cancelled.next.buyerPayment.available == 1000 && cancelled.next.buyerPayment.reserved == 10);
    assert(!PlanBankDeal(cancelled.next, BankDealAction::Reserve, 100, true, true, false).accepted);
    assert(PlanBankDeal(sale, BankDealAction::Cancel, 100, false, false, true).accepted);

    auto corrupt = held.next;
    corrupt.deal.offeredHeld = 19;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = held.next;
    corrupt.sellerGoods.reserved = 19;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = held.next;
    corrupt.buyerGoods.available = 81;
    assert(!PlanBankDeal(corrupt, BankDealAction::Settle, 102, true, true, false).accepted);
    assert(PlanBankDeal(corrupt, BankDealAction::Cancel, 102, false, false, true).accepted);
    corrupt = sale;
    corrupt.deal.id = 0;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = sale;
    corrupt.deal.offeredQuantity = 0;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = sale;
    corrupt.sellerPayment.owner = faction;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = sale;
    corrupt.buyerGoods.asset = silverOre;
    assert(!ValidDealSnapshot(corrupt));
    corrupt = sale;
    corrupt.deal.state = static_cast<BankDealState>(99);
    assert(!ValidDealSnapshot(corrupt));
    auto barter = sale;
    barter.buyerPayment.asset = silverOre;
    barter.sellerPayment.asset = silverOre;
    assert(PlanBankDeal(barter, BankDealAction::Reserve, 100, true, true, false).accepted);
    barter.buyerPayment.asset = copperOre;
    barter.sellerPayment.asset = copperOre;
    assert(!ValidDealSnapshot(barter));
}
