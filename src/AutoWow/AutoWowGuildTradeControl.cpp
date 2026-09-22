#include "AutoWowGuildTradeControl.h"

#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <array>
#include <sstream>
#include <string_view>

namespace AutoWowGuildTradeControl
{
namespace
{
constexpr std::string_view kSchema = "autowow.guild-trade.control.v1";
constexpr std::size_t kReceiptSlots = 64;

struct Receipt
{
    bool occupied = false;
    std::uint32_t sellerGuid = 0;
    std::uint32_t buyerGuid = 0;
    std::uint32_t itemGuid = 0;
    std::uint32_t itemEntry = 0;
    std::uint32_t quantity = 0;
    std::uint32_t priceCopper = 0;
    std::uint32_t sellerMoneyBefore = 0;
    std::uint32_t buyerMoneyBefore = 0;
    bool completed = false;
    std::string error;
};

std::array<Receipt, kReceiptSlots> receipts{};

Receipt* Find(std::uint32_t sellerGuid)
{
    for (Receipt& receipt : receipts)
        if (receipt.occupied && receipt.sellerGuid == sellerGuid)
            return &receipt;
    return nullptr;
}

Receipt* Free()
{
    for (Receipt& receipt : receipts)
        if (!receipt.occupied)
            return &receipt;
    return nullptr;
}

std::string Error(std::uint32_t sellerGuid, std::string_view error)
{
    std::ostringstream out;
    out << "{\"ok\":false,\"schema\":\"" << kSchema
        << "\",\"order\":\"guild-trade\",\"seller_guid\":" << sellerGuid
        << ",\"completed\":false,\"error\":\"guild-trade:" << error << "\"}";
    return out.str();
}

ReceiptFacts Observe(Receipt const& receipt)
{
    Player* seller = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(receipt.sellerGuid));
    Player* buyer = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(receipt.buyerGuid));
    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(receipt.itemGuid);
    Item* sellerItem = seller ? seller->GetItemByGuid(itemGuid) : nullptr;
    Item* buyerItem = buyer ? buyer->GetItemByGuid(itemGuid) : nullptr;
    return {!sellerItem, buyerItem && buyerItem->GetEntry() == receipt.itemEntry &&
             buyerItem->GetCount() == receipt.quantity,
            receipt.sellerMoneyBefore, seller ? seller->GetMoney() : 0,
            receipt.buyerMoneyBefore, buyer ? buyer->GetMoney() : 0, receipt.priceCopper};
}

std::string Render(Receipt& receipt, std::string_view order)
{
    ReceiptFacts const facts = Observe(receipt);
    receipt.completed = receipt.error.empty() && ReceiptComplete(facts);
    if (!receipt.completed && receipt.error.empty())
        receipt.error = "postcondition_not_observed";

    std::ostringstream out;
    out << "{\"ok\":" << (receipt.completed ? "true" : "false")
        << ",\"schema\":\"" << kSchema << "\",\"order\":\"" << order
        << "\",\"seller_guid\":" << receipt.sellerGuid
        << ",\"buyer_guid\":" << receipt.buyerGuid
        << ",\"item_guid\":" << receipt.itemGuid
        << ",\"item_entry\":" << receipt.itemEntry
        << ",\"quantity\":" << receipt.quantity
        << ",\"price_copper\":" << receipt.priceCopper
        << ",\"state\":\"" << (receipt.completed ? "completed" : "failed")
        << "\",\"completed\":" << (receipt.completed ? "true" : "false")
        << ",\"proof\":\"exact_item_owner_and_money_deltas\""
        << ",\"item_absent_from_seller\":" << (facts.itemAbsentFromSeller ? "true" : "false")
        << ",\"exact_item_owned_by_buyer\":" << (facts.exactItemOwnedByBuyer ? "true" : "false")
        << ",\"seller_money_before\":" << facts.sellerMoneyBefore
        << ",\"seller_money_after\":" << facts.sellerMoneyAfter
        << ",\"buyer_money_before\":" << facts.buyerMoneyBefore
        << ",\"buyer_money_after\":" << facts.buyerMoneyAfter;
    if (!receipt.completed)
        out << ",\"error\":\"guild-trade:" << receipt.error << '\"';
    out << '}';
    return out.str();
}
}

std::string GuardError(GuardFacts const& f, std::uint32_t sellerGuid,
                       std::uint32_t buyerGuid, std::uint32_t itemGuid,
                       std::uint32_t itemEntry, std::uint32_t quantity,
                       std::uint32_t priceCopper)
{
    if (!sellerGuid || !buyerGuid || sellerGuid == buyerGuid) return "distinct_exact_bots_required";
    if (!itemGuid || !itemEntry) return "exact_item_required";
    if (quantity != 1) return "whole_single_item_stack_required";
    if (!priceCopper || priceCopper > kMaximumPriceCopper) return "price_out_of_bounds";
    if (!f.sellerOnline || !f.buyerOnline) return "bot_not_online";
    if (!f.sellerPlayerbot || !f.buyerPlayerbot) return "playerbot_required";
    if (!f.sellerAlive || !f.buyerAlive) return "bot_dead";
    if (f.sellerInCombat || f.buyerInCombat) return "in_combat";
    if (!f.sameFaction) return "faction_mismatch";
    if (!f.sellerGuildId || !f.buyerGuildId || f.sellerGuildId == f.buyerGuildId)
        return "different_actual_guilds_required";
    if (!f.nearby) return "not_nearby";
    if (f.sellerAlreadyTrading || f.buyerAlreadyTrading) return "already_trading";
    if (!f.itemExists) return "item_instance_missing";
    if (!f.itemEntryMatches) return "item_entry_mismatch";
    if (f.stackQuantity != quantity) return "exact_whole_stack_quantity_required";
    if (!f.itemTradable || f.itemBoundToBuyer) return "item_not_tradable";
    if (!f.buyerCanStore) return "buyer_inventory_full";
    if (!f.sellerCanReceiveMoney) return "seller_money_cap";
    if (f.buyerMoney < priceCopper) return "buyer_money_insufficient";
    return {};
}

bool ReceiptComplete(ReceiptFacts const& f)
{
    return f.itemAbsentFromSeller && f.exactItemOwnedByBuyer &&
        static_cast<std::uint64_t>(f.sellerMoneyAfter) ==
            static_cast<std::uint64_t>(f.sellerMoneyBefore) + f.priceCopper &&
        static_cast<std::uint64_t>(f.buyerMoneyAfter) + f.priceCopper == f.buyerMoneyBefore;
}

std::string Start(Player* seller, PlayerbotAI* sellerAI, std::uint32_t buyerGuid,
                  std::uint32_t itemGuid, std::uint32_t itemEntry,
                  std::uint32_t quantity, std::uint32_t priceCopper)
{
    std::uint32_t const sellerGuid = seller ? seller->GetGUID().GetCounter() : 0;
    if (Find(sellerGuid)) return Error(sellerGuid, "already_attempted");
    Player* buyer = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(buyerGuid));
    PlayerbotAI* buyerAI = buyer ? PlayerbotsMgr::instance().GetPlayerbotAI(buyer) : nullptr;
    Item* item = seller ? seller->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid)) : nullptr;
    ItemPosCountVec destination;
    bool const buyerCanStore = buyer && item &&
        buyer->CanStoreItem(NULL_BAG, NULL_SLOT, destination, item, false) == EQUIP_ERR_OK;
    GuardFacts const facts{
        seller && seller->IsInWorld(), buyer && buyer->IsInWorld(),
        sellerAI && !sellerAI->IsRealPlayer(), buyerAI && !buyerAI->IsRealPlayer(),
        seller && seller->IsAlive(), buyer && buyer->IsAlive(),
        seller && seller->IsInCombat(), buyer && buyer->IsInCombat(),
        seller && buyer && seller->GetTeamId() == buyer->GetTeamId(),
        seller && buyer && seller->IsWithinDistInMap(buyer, TRADE_DISTANCE, false),
        seller ? seller->GetGuildId() : 0, buyer ? buyer->GetGuildId() : 0,
        seller && seller->GetTradeData(), buyer && buyer->GetTradeData(), item != nullptr,
        item && item->GetEntry() == itemEntry, item && item->CanBeTraded(false, true),
        item && buyer && item->IsBindedNotWith(buyer), item ? item->GetCount() : 0,
        buyerCanStore,
        seller && seller->GetMoney() <= MAX_MONEY_AMOUNT - priceCopper,
        buyer ? buyer->GetMoney() : 0};
    std::string const guard = GuardError(facts, sellerGuid, buyerGuid, itemGuid,
                                         itemEntry, quantity, priceCopper);
    if (!guard.empty()) return Error(sellerGuid, guard);
    Receipt* receipt = Free();
    if (!receipt) return Error(sellerGuid, "receipt_capacity_exhausted");
    *receipt = {true, sellerGuid, buyerGuid, itemGuid, itemEntry, quantity, priceCopper,
                seller->GetMoney(), buyer->GetMoney(), false, {}};

    WorldPacket initiate;
    initiate << buyer->GetGUID();
    seller->GetSession()->HandleInitiateTradeOpcode(initiate);
    if (!seller->GetTradeData() || !buyer->GetTradeData() || seller->GetTrader() != buyer)
    {
        receipt->error = "native_initiate_rejected";
        return Render(*receipt, "guild-trade");
    }
    WorldPacket empty;
    seller->GetSession()->HandleBeginTradeOpcode(empty);
    WorldPacket setItem;
    setItem << std::uint8_t(0) << item->GetBagSlot() << item->GetSlot();
    seller->GetSession()->HandleSetTradeItemOpcode(setItem);
    WorldPacket setGold;
    setGold << priceCopper;
    buyer->GetSession()->HandleSetTradeGoldOpcode(setGold);
    if (seller->GetTradeData()->GetItem(TradeSlots(0)) != item ||
        buyer->GetTradeData()->GetMoney() != priceCopper)
    {
        WorldPacket cancel;
        seller->GetSession()->HandleCancelTradeOpcode(cancel);
        receipt->error = "native_offer_rejected";
        return Render(*receipt, "guild-trade");
    }
    WorldPacket acceptSeller;
    seller->GetSession()->HandleAcceptTradeOpcode(acceptSeller);
    WorldPacket acceptBuyer;
    buyer->GetSession()->HandleAcceptTradeOpcode(acceptBuyer);
    if (!ReceiptComplete(Observe(*receipt)) && seller->GetTradeData())
    {
        WorldPacket cancel;
        seller->GetSession()->HandleCancelTradeOpcode(cancel);
    }
    return Render(*receipt, "guild-trade");
}

std::string Status(std::uint32_t sellerGuid)
{
    Receipt* receipt = Find(sellerGuid);
    return receipt ? Render(*receipt, "guild-trade-status") : Error(sellerGuid, "receipt_missing");
}
}
