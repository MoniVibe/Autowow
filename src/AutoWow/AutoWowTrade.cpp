/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Trade runtime (policy: AutoWow/TradePolicy.h). Decisions are made on the bot's map thread at
// the stop (the auction house is only written by the world thread, which never runs alongside the map
// updates); the core auction / mail handlers are thread-unsafe opcodes, so they run in the world thread
// (PlayerbotWorldThreadProcessor) and every ledger line reports the money change they actually made.

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "ErrandsPolicy.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GearUpgradePolicy.h"
#include "NoWhitePolicy.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "SupplyPolicy.h"
#include "TradePolicy.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutoWowTrade
{
namespace
{
// AutoWow.Auction.SeedThinSlots: per-faction-house daily seed budget (world thread writes under the lock; the map
// thread reads it to decide whether to queue a pass). Keyed by AuctionHouseEntry::houseId.
std::mutex gSeedLock;
std::map<std::uint32_t, SeedWindow> gSeedWindows;
// AutoWow.Gear.NoWhite: an auction gear weapon is ledger reason ah_weapon (the weapon floor's source b).
char const* GearReason(std::uint32_t item)
{
    ItemTemplate const* proto = AutoWowNoWhite::Enabled() ? sObjectMgr->GetItemTemplate(item) : nullptr;
    return proto && proto->Class == ITEM_CLASS_WEAPON ? "ah_weapon" : "ah_gear";
}

// gain != 0: an AutoWow.Gear.AuctionUpgrades purchase (ledger reason ah_gear, + gain; action stays buy).
void Emit(Player* bot, Action a, std::uint32_t item, std::uint32_t count, std::uint64_t price, std::int64_t gold,
          std::uint32_t ah, std::uint32_t gain = 0)
{
    LOG_INFO("playerbots", "[Trade] bot={} {} item={} count={} price={} gold={} ah={}", bot->GetName(),
             ActionName(a), item, count, price, gold, ah);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitTrade(bot, gain ? GearReason(item) : ActionName(a),
                                      LedgerFields(a, item, count, price, gold, ah, nullptr, gain));
}

bool Collectable(Mail const* m, time_t now)
{
    return m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && !m->COD && (m->money || m->HasItems());
}

bool Empty(Mail const* m, time_t now)
{
    return detail::gParams.deleteEmptyMail && m && m->state != MAIL_STATE_DELETED &&
           EmptyMail(m->deliver_time <= now, m->COD != 0, m->money, m->HasItems());
}

template <typename F>
void ForEachBagItem(Player* bot, F&& fn)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            fn(item);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pBag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* item = pBag->GetItemByPos(slot))
                    fn(item);
}

std::string UsageKey(uint32 entry, int32 randomPropertyId)
{
    return std::to_string(entry) + "," + std::to_string(randomPropertyId);
}

// AutoWow.Gear.AuctionUpgrades (map thread, read-only over the house): the buyout listings of others the stock
// "item upgrade" scorer rates an equip for the bot (EQUIP / REPLACE: class / spec weights, proficiency, level) that
// raise the item level of their slot (FindEquipSlot), planned by AutoWowGear::AhPlan within AhBudget of `money`.
std::vector<Buy> PlanAhGear(PlayerbotAI* botAI, Player* bot, AuctionHouseObject* ah, uint64 money)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const level = bot->GetLevel();
    Item const* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    bool const wields2h = mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip();
    std::vector<AutoWowGear::AhOffer> offers;
    for (auto const& [id, a] : ah->GetAuctions())
    {
        if (!a || a->owner == bot->GetGUID() || !a->buyout || !a->itemCount)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->RequiredLevel > level ||
            bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        Item const* aitem = sAuctionMgr->GetAItem(a->item_guid);
        ItemUsage const usage =
            context->GetValue<ItemUsage>("item upgrade", UsageKey(a->item_template, aitem ? aitem->GetItemRandomPropertyId() : 0))
                ->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;
        uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot >= EQUIPMENT_SLOT_END || (slot == EQUIPMENT_SLOT_OFFHAND && wields2h))
            continue;  // an off hand would push the two-hander out
        Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 const wornIlvl = worn ? worn->GetTemplate()->ItemLevel : 0;
        if (proto->ItemLevel <= wornIlvl)
            continue;
        offers.push_back({id, a->item_template, a->buyout, proto->ItemLevel - wornIlvl, slot,
                          proto->InventoryType == INVTYPE_2HWEAPON, static_cast<std::uint8_t>(proto->Quality)});
    }
    // AutoWow.Gear.NoWhite: never a grey / white weapon from the auction house.
    if (AutoWowNoWhite::Enabled())
        AutoWowNoWhite::DropWhiteWeapons(offers);
    uint64 const repair = context->GetValue<uint32>("repair cost")->Get();
    uint64 const budget = AutoWowGear::AhBudget(money, level, repair);
    std::vector<AutoWowGear::AhOffer> const plan =
        AutoWowGear::AhPlan(AutoWowGear::GetAh(), offers, level, bot->getClass() == CLASS_HUNTER, budget);
    LOG_INFO("playerbots", "[AhGear] bot={} scan offers={} planned={} budget={} item_cap={} money={} repair={} lvl={}",
             bot->GetName(), offers.size(), plan.size(), budget, AutoWowGear::AhItemCap(AutoWowGear::GetAh(), level),
             money, repair, level);
    std::vector<Buy> buys;
    for (AutoWowGear::AhOffer const& o : plan)
    {
        LOG_INFO("playerbots", "[AhGear] bot={} plan ah={} item={} slot={} price={} ilvl_gain={}", bot->GetName(), o.id,
                 o.item, static_cast<uint32>(o.slot), o.price, o.gain);
        if (AutoWowNoWhite::Enabled() && (AutoWowNoWhite::FloorSlotsOf(bot) & (1u << o.slot)))
            LOG_INFO("playerbots", "[NoWhite] bot={} source=ah slot={} item={} quality={} price={} ilvl_gain={} "
                     "floor={} lvl={}", bot->GetName(), static_cast<uint32>(o.slot), o.item,
                     static_cast<uint32>(o.quality), o.price, o.gain,
                     AutoWowNoWhite::FloorIlvl(AutoWowNoWhite::Get(), level), level);
        buys.push_back({o.id, o.item, 1, static_cast<uint32>(o.price), Want::Upgrade, o.gain});
    }
    return buys;
}

// The bag stacks the bot may list at `house` (Postable with its stock item usage `ah`), each with the lowest
// competing buyout and its deposit.
std::vector<Holding> Holdings(PlayerbotAI* botAI, Player* bot, AuctionHouseEntry const* house,
                              std::map<uint32, uint32> const& lowest)
{
    Params const& p = detail::gParams;
    AiObjectContext* context = botAI->GetAiObjectContext();
    std::vector<Holding> holdings;
    ForEachBagItem(bot, [&](Item* item)
                   {
                       ItemTemplate const* proto = item->GetTemplate();
                       Holding h;
                       h.entry = proto->ItemId;
                       h.guid = item->GetGUID().GetCounter();
                       h.count = item->GetCount();
                       h.quality = proto->Quality;
                       h.itemClass = proto->Class;
                       h.sellPrice = proto->SellPrice;
                       Holding probe = h;
                       probe.usageAh = true;
                       if (!Postable(probe))
                           return;  // skip the usage lookup for what could never be listed
                       ItemUsage const usage =
                           context->GetValue<ItemUsage>("item usage",
                                                        UsageKey(h.entry, item->GetItemRandomPropertyId()))
                               ->Get();
                       h.usageAh = usage == ITEM_USAGE_AH;
                       // AutoWow.Auction.ListLoot: a BoE green / blue equippable the bot does not want (not an
                       // equip / replace upgrade) is listed instead of vendored, so gear catch-up scans find it.
                       if (!h.usageAh && detail::gParams.listLoot &&
                           ListLootEquip(proto->Quality, proto->InventoryType, proto->Bonding, item->IsSoulBound(),
                                         usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE))
                           h.usageAh = true;
                       auto const low = lowest.find(h.entry);
                       h.lowestOther = low == lowest.end() ? 0 : low->second;
                       h.deposit = AuctionHouseMgr::GetAuctionDeposit(house, p.durationMin * MINUTE, item, h.count);
                       holdings.push_back(h);
                   });
    return holdings;
}

// AutoWow.Market.MailOrders (world thread, a house rep at its mailbox): each delivered COD mail, oldest id first, is
// accepted as an order fill (the house bank tops the rep's purse up to the COD; the core charges it and mails it to
// the seller), returned to the seller, or left for a visit with bag room (AutoWowSupply::CodAtRep decides).
void TakeCod(Player* bot, WorldSession* session, ObjectGuid mailbox, time_t now)
{
    std::vector<uint32> ids;
    for (Mail const* m : bot->GetMails())
        if (m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && m->COD && m->HasItems())
            ids.push_back(m->messageID);
    std::sort(ids.begin(), ids.end());
    for (uint32 id : ids)
    {
        Mail* m = bot->GetMail(id);
        if (!m || !m->COD || !m->HasItems())
            continue;
        uint32 entry = 0, units = 0;
        bool mixed = false, room = true;
        for (MailItemInfo const& mi : m->items)
        {
            Item* it = bot->GetMItem(mi.item_guid);
            mixed = mixed || (entry && mi.item_template != entry);
            entry = mi.item_template;
            units += it ? it->GetCount() : 0;
            ItemPosCountVec dest;
            room = room && it && bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) == EQUIP_ERR_OK;
        }
        uint32 const cod = m->COD;
        uint32 const seller = static_cast<uint32>(m->sender);
        char const* why = "mixed";
        AutoWowSupply::CodVerdict const v =
            mixed ? AutoWowSupply::CodVerdict::Return : AutoWowSupply::CodAtRep(bot, seller, entry, units, cod, room, &why);
        if (v == AutoWowSupply::CodVerdict::Wait)
            continue;
        if (v == AutoWowSupply::CodVerdict::Accept)
        {
            uint64 const m0 = bot->GetMoney();
            std::vector<MailItemInfo> const items = m->items;  // the handler erases taken items
            for (MailItemInfo const& mi : items)
            {
                WorldPacket packet(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
                packet << mailbox << id << mi.item_guid;
                session->HandleMailTakeItem(packet);
            }
            Emit(bot, Action::CodBuy, entry, units, cod, int64(bot->GetMoney()) - int64(m0), 0);
            continue;
        }
        WorldPacket packet(CMSG_MAIL_RETURN_TO_SENDER, 8 + 4 + 8);
        packet << mailbox << id << ObjectGuid::Create<HighGuid::Player>(seller);
        session->HandleMailReturnToSender(packet);
        LOG_INFO("playerbots", "[Trade] bot={} cod_return mail={} seller={} item={} count={} cod={} why={}",
                 bot->GetName(), id, seller, entry, units, cod, why);
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitTrade(bot, ActionName(Action::CodReturn),
                                          LedgerFields(Action::CodReturn, entry, units, cod, 0, 0, why));
    }
}

// World thread: the listings and purchases planned at the auctioneer.
class AuctionOperation : public PlayerbotOperation
{
public:
    AuctionOperation(ObjectGuid bot, ObjectGuid auctioneer, std::vector<Post> posts, std::vector<Buy> buys)
        : bot_(bot), auctioneer_(auctioneer), posts_(std::move(posts)), buys_(std::move(buys))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
            return false;
        WorldSession* session = bot->GetSession();
        for (Post const& post : posts_)
        {
            ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(post.guid);
            Item* item = bot->GetItemByGuid(itemGuid);
            if (!item || item->GetCount() != post.count)
                continue;  // used, split or moved since the plan
            uint64 const m0 = bot->GetMoney();
            WorldPacket packet(CMSG_AUCTION_SELL_ITEM, 8 + 4 + 8 + 4 + 4 + 4 + 4);
            packet << auctioneer_ << uint32(1) << itemGuid << uint32(post.count) << uint32(post.bid)
                   << uint32(post.buyout) << uint32(detail::gParams.durationMin);
            session->HandleAuctionSellItem(packet);
            if (bot->GetItemByGuid(itemGuid))
            {
                LOG_INFO("playerbots", "[Trade] bot={} post rejected item={} count={} buyout={}", bot->GetName(),
                         post.entry, post.count, post.buyout);
                continue;
            }
            Emit(bot, Action::Post, post.entry, post.count, post.buyout, int64(bot->GetMoney()) - int64(m0), 0);
        }
        for (Buy const& buy : buys_)
        {
            uint64 const m0 = bot->GetMoney();
            WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
            packet << auctioneer_ << uint32(buy.id) << uint32(buy.price);
            session->HandleAuctionPlaceBid(packet);
            if (bot->GetMoney() >= m0)
            {
                LOG_INFO("playerbots", "[Trade] bot={} buy rejected ah={} item={} price={}", bot->GetName(), buy.id,
                         buy.entry, buy.price);
                if (buy.gain)
                    LOG_INFO("playerbots", "[AhGear] bot={} rejected ah={} item={} price={} money={}", bot->GetName(),
                             buy.id, buy.entry, buy.price, m0);
                continue;
            }
            Emit(bot, Action::Buy, buy.entry, buy.count, buy.price, int64(bot->GetMoney()) - int64(m0), buy.id, buy.gain);
            if (buy.gain)
                LOG_INFO("playerbots", "[AhGear] bot={} bought ah={} item={} price={} ilvl_gain={} money={} lvl={}",
                         bot->GetName(), buy.id, buy.entry, buy.price, buy.gain, bot->GetMoney(), bot->GetLevel());
        }
        return true;
    }

    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowTradeAuction"; }

private:
    ObjectGuid bot_;
    ObjectGuid auctioneer_;
    std::vector<Post> posts_;
    std::vector<Buy> buys_;
};

// World thread: take the money and items of every collectable mail, oldest message id first.
class MailOperation : public PlayerbotOperation
{
public:
    MailOperation(ObjectGuid bot, ObjectGuid mailbox) : bot_(bot), mailbox_(mailbox) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
            return false;
        WorldSession* session = bot->GetSession();
        time_t const now = GameTime::GetGameTime().count();
        if (AutoWowSupply::MailOrders() &&
            AutoWowSupply::RoleOf(static_cast<std::uint32_t>(bot->GetGUID().GetCounter())).role == AutoWowSupply::Role::Rep)
            TakeCod(bot, session, mailbox_, now);
        std::vector<uint32> ids;
        for (Mail const* m : bot->GetMails())
            if (Collectable(m, now))
                ids.push_back(m->messageID);
        std::sort(ids.begin(), ids.end());
        for (uint32 id : ids)
        {
            Mail* m = bot->GetMail(id);
            if (!Collectable(m, now))
                continue;
            AuctionMail const am = m->messageType == MAIL_AUCTION ? ParseAuctionSubject(m->subject) : AuctionMail{};
            Action const action = MailAction(am);
            uint32 const bid = action == Action::Sold ? ParseSaleBid(m->body) : 0;
            uint32 const saleDeposit = action == Action::Sold ? ParseSaleDeposit(m->body) : 0;
            uint64 const m0 = bot->GetMoney();
            if (m->money)
            {
                WorldPacket packet(CMSG_MAIL_TAKE_MONEY, 8 + 4);
                packet << mailbox_ << id;
                session->HandleMailTakeMoney(packet);
            }
            std::vector<MailItemInfo> const items = m->items;  // the handler erases taken items
            uint32 firstEntry = 0, taken = 0, count = 0;
            for (MailItemInfo const& mi : items)
            {
                Item* it = bot->GetMItem(mi.item_guid);
                uint32 const stack = it ? it->GetCount() : 0;
                WorldPacket packet(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
                packet << mailbox_ << id << mi.item_guid;
                session->HandleMailTakeItem(packet);
                Mail const* after = bot->GetMail(id);
                bool const gone = !after || std::none_of(after->items.begin(), after->items.end(),
                                                         [&](MailItemInfo const& x) { return x.item_guid == mi.item_guid; });
                if (!gone)
                    continue;  // bags full: stays in the mail for the next visit
                if (!firstEntry)
                    firstEntry = mi.item_template;
                ++taken;
                count += stack;
            }
            int64 const gold = int64(bot->GetMoney()) - int64(m0);
            if (!gold && !taken)
                continue;
            Emit(bot, action, am.ok ? am.entry : firstEntry, am.ok ? am.count : count, bid, gold,
                 am.ok ? am.auctionId : 0);
            if (action == Action::Sold && AutoWowSupply::Market())
                // A rep's sale proceeds (bid - cut) -> its house bank; the refunded deposit stays its listing float.
                AutoWowSupply::OnAuctionSold(bot, am.entry, am.count, gold - int64(saleDeposit));
        }
        // DeleteEmptyMail: the emptied mails go (stock delete handler, as the client does), oldest id first.
        std::vector<uint32> empty;
        for (Mail const* m : bot->GetMails())
            if (Empty(m, now))
                empty.push_back(m->messageID);
        std::sort(empty.begin(), empty.end());
        for (uint32 id : empty)
        {
            WorldPacket packet(CMSG_MAIL_DELETE, 8 + 4 + 4);
            packet << mailbox_ << id << uint32(0);  // mailTemplateId
            session->HandleMailDelete(packet);
        }
        if (!empty.empty())
            LOG_INFO("playerbots", "[Trade] bot={} deleted empty mails={} left={}", bot->GetName(), empty.size(),
                     bot->GetMailSize());
        return true;
    }

    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowTradeMail"; }

private:
    ObjectGuid bot_;
    ObjectGuid mailbox_;
};

// World thread: AutoWow.Auction.SeedThinSlots. Count each thin accessory / off-slot's buyout listings in the bot's
// level band on the faction house, and for the thin ones create BoE green items (sRandomItemMgr's per-level
// equipment index, filtered to green BoE of the slot and band) and list them for sale, owned by the bot, within
// the per-house daily cap. The one place in the module where items are created from nothing (owner-approved).
class SeedOperation : public PlayerbotOperation
{
public:
    SeedOperation(ObjectGuid bot, ObjectGuid auctioneer, std::uint32_t band)
        : bot_(bot), auctioneer_(auctioneer), band_(band)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        Creature* auctioneer = bot ? ObjectAccessor::GetCreature(*bot, auctioneer_) : nullptr;
        if (!bot || !bot->IsInWorld() || !auctioneer)
            return false;
        AuctionHouseEntry const* house =
            AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        if (!house || !ah || house->houseId == uint32(AuctionHouseId::Neutral))
            return false;
        Params const& p = detail::gParams;
        std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
        std::uint64_t const day = now / kSeedDayMs;

        // Buyout listings of each seeded slot in this band, and the item ids already listed there (never seed a
        // duplicate the scanners already see).
        std::array<std::uint32_t, kSeedSlots + 1> count{};
        std::set<std::uint32_t> listed;
        for (auto const& [id, a] : ah->GetAuctions())
        {
            if (!a || !a->buyout || !a->itemCount)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
            if (!proto)
                continue;
            SeedSlot const slot = SeedSlotOf(proto->InventoryType);
            if (slot == SeedSlot::None || SeedBandOf(proto->RequiredLevel) != band_)
                continue;
            ++count[static_cast<std::size_t>(slot)];
            listed.insert(a->item_template);
        }

        std::vector<Post> posts;  // for the ledger rows after the listings go in
        {
            std::lock_guard<std::mutex> guard(gSeedLock);
            SeedWindow& w = gSeedWindows[house->houseId];
            std::uint32_t room = std::min(SeedRoom(w, day, p.seedDailyCap), p.seedPerVisit);
            std::uint32_t made = 0;
            for (std::uint8_t s = 1; s <= kSeedSlots && room; ++s)
            {
                std::uint32_t const need = SeedCount(count[s], p.seedThinThreshold, room);
                for (std::uint32_t k = 0; k < need; ++k)
                {
                    std::uint32_t const item = PickCandidate(static_cast<SeedSlot>(s), listed);
                    if (!item)
                        break;
                    if (!ListCreated(bot, ah, house, item, posts))
                        break;
                    listed.insert(item);  // one per item id per pass
                    --room;
                    ++made;
                }
            }
            if (made)
                NoteSeed(w, day, made);
        }
        for (Post const& post : posts)
        {
            LOG_INFO("playerbots", "[Seed] bot={} house={} band={} item={} buyout={}", bot->GetName(), house->houseId,
                     band_, post.entry, post.buyout);
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::EmitTrade(bot, "seed",
                                              LedgerFields(Action::Post, post.entry, 1, post.buyout, 0, 0, "seed"));
        }
        return true;
    }

    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowTradeSeed"; }

private:
    // The lowest-id green BoE item of `slot` in this band that is not already listed / seeded this pass. 0 = none.
    std::uint32_t PickCandidate(SeedSlot slot, std::set<std::uint32_t> const& taken) const
    {
        std::uint32_t best = 0;
        std::uint32_t const lo = band_ * 10u + 1u, hi = band_ * 10u + 10u;
        auto consider = [&](InventoryType invType)
        {
            for (std::uint32_t level = lo; level <= hi; ++level)
                for (std::uint32_t const item : sRandomItemMgr.GetEquipmentNew(level, invType))
                {
                    if ((best && item >= best) || taken.count(item))
                        continue;
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                    if (!proto || proto->Quality != kQualityUncommon || !proto->SellPrice ||
                        proto->Bonding == kBindOnPickup || proto->Bonding == kBindQuestItem ||
                        SeedSlotOf(proto->InventoryType) != slot || SeedBandOf(proto->RequiredLevel) != band_)
                        continue;
                    best = item;
                }
        };
        for (InventoryType const invType : InvTypesOf(slot))
            if (invType != INVTYPE_NON_EQUIP)
                consider(invType);
        return best;
    }

    // The core InventoryTypes a seed category covers (padded with INVTYPE_NON_EQUIP = ignore).
    static std::array<InventoryType, 3> InvTypesOf(SeedSlot slot)
    {
        switch (slot)
        {
            case SeedSlot::Trinket: return {INVTYPE_TRINKET, INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP};
            case SeedSlot::Ring:    return {INVTYPE_FINGER, INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP};
            case SeedSlot::Neck:    return {INVTYPE_NECK, INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP};
            case SeedSlot::Back:    return {INVTYPE_CLOAK, INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP};
            case SeedSlot::OffHand: return {INVTYPE_SHIELD, INVTYPE_HOLDABLE, INVTYPE_NON_EQUIP};
            case SeedSlot::Ranged:  return {INVTYPE_RANGED, INVTYPE_THROWN, INVTYPE_RANGEDRIGHT};
            default:                return {INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP, INVTYPE_NON_EQUIP};
        }
    }

    // Create one `item` and add it to the house as a buyout listing owned by the bot (AhBot pattern: no deposit
    // charged, the item is created from nothing). Appends the listing to `posts`. False = could not create / price.
    bool ListCreated(Player* bot, AuctionHouseObject* ah, AuctionHouseEntry const* house, std::uint32_t item,
                     std::vector<Post>& posts)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto)
            return false;
        std::uint64_t const buyout = UnitPrice(detail::gParams, proto->SellPrice, 0);
        if (!buyout || buyout > kMaxMoney)
            return false;
        Item* created = Item::CreateItem(item, 1, bot);
        if (!created)
            return false;
        AuctionEntry* entry = new AuctionEntry;
        entry->Id = sObjectMgr->GenerateAuctionID();
        entry->houseId = AuctionHouseId(house->houseId);
        entry->item_guid = created->GetGUID();
        entry->item_template = item;
        entry->itemCount = 1;
        entry->owner = bot->GetGUID();
        entry->startbid = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, buyout * detail::gParams.bidPct / 100));
        entry->bidder = ObjectGuid::Empty;
        entry->bid = 0;
        entry->buyout = static_cast<std::uint32_t>(buyout);
        entry->expire_time = GameTime::GetGameTime().count() + detail::gParams.seedDurationMin * MINUTE;
        entry->deposit = 0;
        entry->auctionHouseEntry = house;
        ah->AddAuction(entry);
        sAuctionMgr->AddAItem(created);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        created->SaveToDB(trans);
        entry->SaveToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        posts.push_back({item, created->GetGUID().GetCounter(), 1, entry->startbid, entry->buyout, 0});
        return true;
    }

    ObjectGuid bot_;
    ObjectGuid auctioneer_;
    std::uint32_t band_;
};
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Trade.Enable", false);
    detail::gTreasury = sConfigMgr->GetOption<bool>("AutoWow.Ledger.Treasury", false);
    Params& p = detail::gParams;
    p.priceMultPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Trade.PriceMultPct", 300);
    p.floorPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Trade.FloorPct", 150);
    p.maxPosts = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Trade.MaxPosts", 6);
    p.buyBudgetPct = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Trade.BuyBudgetPct", 50));
    p.matUnitMaxPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Trade.MatUnitMaxPct", 400);
    p.deleteEmptyMail = sConfigMgr->GetOption<bool>("AutoWow.Trade.DeleteEmptyMail", true);
    p.randomSellers = sConfigMgr->GetOption<bool>("AutoWow.Market.RandomSellers", false);
    p.sellerCapPerHour = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Market.SellerCapPerHour", 6);
    p.sellerScanMs = std::max<std::uint32_t>(1000, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Market.SellerScanMs", 60000));
    p.sellerYards = std::max<std::uint32_t>(10, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Market.SellerYards", 120));
    p.listLoot = sConfigMgr->GetOption<bool>("AutoWow.Auction.ListLoot", false);
    p.seedThinSlots = sConfigMgr->GetOption<bool>("AutoWow.Auction.SeedThinSlots", false);
    p.seedDailyCap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Auction.DailyCap", 20);
    p.seedThinThreshold = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Auction.ThinThreshold", 3));
    p.seedPerVisit = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Auction.SeedPerVisit", 3));
    if (p.randomSellers)
        LOG_INFO("server.loading", "[Trade] market random sellers on: cap={}/h scan_ms={} yards={}", p.sellerCapPerHour,
                 p.sellerScanMs, p.sellerYards);
    if (p.listLoot)
        LOG_INFO("server.loading", "[Trade] auction list-loot on: unneeded BoE green / blue gear is listed, not vendored");
    if (p.seedThinSlots)
        LOG_INFO("server.loading", "[Trade] auction thin-slot seeding on: cap={}/day/house threshold={} per_visit={}",
                 p.seedDailyCap, p.seedThinThreshold, p.seedPerVisit);
}

bool HasCollectableMail(Player* bot)
{
    time_t const now = GameTime::GetGameTime().count();
    for (Mail const* m : bot->GetMails())
        if (Collectable(m, now))
            return true;
    return false;
}

bool HasCollectableMailWithRoom(Player* bot)
{
    time_t const now = GameTime::GetGameTime().count();
    bool const codRep = AutoWowSupply::MailOrders() && bot->GetFreeInventorySpace() > 0 &&
                        AutoWowSupply::RoleOf(static_cast<std::uint32_t>(bot->GetGUID().GetCounter())).role ==
                            AutoWowSupply::Role::Rep;
    for (Mail const* m : bot->GetMails())
    {
        if (codRep && m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && m->COD && m->HasItems())
            return true;  // MailOrders: an order fill to accept or return
        if (!Collectable(m, now))
            continue;
        if (m->money)
            return true;
        for (MailItemInfo const& mi : m->items)
        {
            Item* it = bot->GetMItem(mi.item_guid);
            ItemPosCountVec dest;
            if (it ? bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) == EQUIP_ERR_OK
                   : bot->GetFreeInventorySpace() > 0)  // mail items not loaded: any free slot
                return true;
        }
    }
    return false;
}

bool HasEmptyMail(Player* bot)
{
    time_t const now = GameTime::GetGameTime().count();
    for (Mail const* m : bot->GetMails())
        if (Empty(m, now))
            return true;
    return false;
}

bool VisitAuctioneer(PlayerbotAI* botAI, Player* bot, Creature* auctioneer, std::uint64_t reserve,
                     std::vector<std::uint32_t>* ahGear)
{
    Params const& p = detail::gParams;
    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!house || !ah)
        return false;
    AiObjectContext* context = botAI->GetAiObjectContext();
    // Read-only pass over the house (ascending auction id): lowest competing buyout per unit, and the
    // listings the bot wants (stock item usage: equip / replace = upgrade, skill = a mat it lacks).
    std::map<uint32, uint32> lowest;
    std::vector<Listing> listings;
    for (auto const& [id, a] : ah->GetAuctions())
    {
        if (!a || a->owner == bot->GetGUID() || !a->buyout || !a->itemCount)
            continue;
        uint32 const unit = a->buyout / a->itemCount;
        auto const [it, fresh] = lowest.emplace(a->item_template, unit);
        if (!fresh)
            it->second = std::min(it->second, unit);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
        if (!proto)
            continue;
        Item const* aitem = sAuctionMgr->GetAItem(a->item_guid);
        ItemUsage const usage =
            context->GetValue<ItemUsage>("item usage", UsageKey(a->item_template, aitem ? aitem->GetItemRandomPropertyId() : 0))
                ->Get();
        Want const want = (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE) ? Want::Upgrade
                          : usage == ITEM_USAGE_SKILL                                 ? Want::Mat
                                                                                      : Want::None;
        if (want != Want::None)
            listings.push_back({id, a->item_template, a->itemCount, a->buyout, proto->SellPrice, want});
    }
    std::vector<Holding> holdings = Holdings(botAI, bot, house, lowest);
    uint64 const money = bot->GetMoney();
    std::vector<Post> posts = PlanPosts(p, std::move(holdings), money);
    uint64 deposits = 0;
    for (Post const& post : posts)
        deposits += post.deposit;
    // AutoWow.Gear.AuctionUpgrades: the gear plan replaces PlanBuys' one cheapest upgrade and goes first; the mats
    // get the trade budget of what is left.
    std::vector<Buy> gear;
    bool ahGearScanned = false;
    if (AutoWowGear::AuctionEnabled())
    {
        gear = PlanAhGear(botAI, bot, ah, money > deposits ? money - deposits : 0);
        ahGearScanned = true;
        listings.erase(std::remove_if(listings.begin(), listings.end(),
                                      [](Listing const& l) { return l.want == Want::Upgrade; }),
                       listings.end());
        for (Buy const& b : gear)
        {
            deposits += b.price;  // committed copper from here on
            if (ahGear)
                ahGear->push_back(b.entry);
        }
    }
    // AutoWow.Survival.PotionFloor: potion lots after the gear, before the mats, the reserve kept in hand.
    std::vector<Buy> potions;
    if (AutoWowErrands::Enabled() && AutoWowErrands::detail::gParams.potionFloor)
    {
        std::vector<AutoWowErrands::PotionLot> lots;
        for (auto const& [id, a] : ah->GetAuctions())
            if (a && a->owner != bot->GetGUID() && a->buyout && a->itemCount &&
                AutoWowErrands::IsPotionItem(a->item_template))
                lots.push_back({id, a->item_template, a->itemCount, a->buyout});
        std::array<std::uint32_t, AutoWowErrands::kPotionFamilies> have{};
        for (AutoWowErrands::Potion const& t : AutoWowErrands::kPotions)
            have[t.family] += bot->GetItemCount(t.item, false);
        uint64 const left = money > deposits + reserve ? money - deposits - reserve : 0;
        for (AutoWowErrands::PotionLot const& l : AutoWowErrands::PlanPotionLots(
                 AutoWowErrands::detail::gParams, bot->getClass(), bot->GetLevel(), have, lots, left))
        {
            potions.push_back({l.id, l.item, l.count, static_cast<uint32>(l.buyout), Want::None});
            deposits += l.buyout;
            LOG_INFO("playerbots", "[PotionFloor] bot={} source=ah ah={} item={} count={} buyout={} money={} lvl={}",
                     bot->GetName(), l.id, l.item, l.count, l.buyout, money, bot->GetLevel());
        }
        if (lots.empty() && AutoWowErrands::PotionShort(AutoWowErrands::detail::gParams, bot->getClass(),
                                                        bot->GetLevel(), have,
                                                        AutoWowErrands::detail::gParams.potionTarget))
            LOG_INFO("playerbots", "[PotionFloor] bot={} source=ah lots=0 heal={} mana={} lvl={}", bot->GetName(),
                     have[AutoWowErrands::kPotionHeal], have[AutoWowErrands::kPotionMana], bot->GetLevel());
    }
    std::vector<Buy> buys = PlanBuys(p, std::move(listings), BuyBudget(p, money > deposits ? money - deposits : 0, reserve));
    buys.insert(buys.begin(), potions.begin(), potions.end());
    buys.insert(buys.begin(), gear.begin(), gear.end());
    LOG_INFO("playerbots", "[Trade] bot={} auctioneer={} posts={} buys={} money={} reserve={}", bot->GetName(),
             auctioneer->GetEntry(), posts.size(), buys.size(), money, reserve);
    if (posts.empty() && buys.empty())
        return ahGearScanned;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<AuctionOperation>(bot->GetGUID(), auctioneer->GetGUID(), std::move(posts), std::move(buys)));
    return ahGearScanned;
}

std::uint32_t PostStacks(Player* bot, Creature* auctioneer, std::vector<std::uint32_t> const& itemGuids,
                         std::vector<Post>* planned)
{
    Params const& p = detail::gParams;
    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!house || !ah)
        return 0;
    std::vector<Post> posts;
    uint64 budget = bot->GetMoney();
    for (std::uint32_t const guid : itemGuids)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid));
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            continue;
        uint32 lowest = 0;
        for (auto const& [id, a] : ah->GetAuctions())
            if (a && a->owner != bot->GetGUID() && a->buyout && a->itemCount && a->item_template == proto->ItemId)
                lowest = lowest ? std::min(lowest, a->buyout / a->itemCount) : a->buyout / a->itemCount;
        uint64 const buyout = UnitPrice(p, proto->SellPrice, lowest) * item->GetCount();
        uint32 const deposit = AuctionHouseMgr::GetAuctionDeposit(house, p.durationMin * MINUTE, item, item->GetCount());
        if (!buyout || buyout > kMaxMoney || deposit > budget)
            continue;
        budget -= deposit;
        posts.push_back({proto->ItemId, guid, item->GetCount(),
                         static_cast<uint32>(std::max<uint64>(1, buyout * p.bidPct / 100)), static_cast<uint32>(buyout),
                         deposit});
    }
    if (planned)
        *planned = posts;
    if (!posts.empty())
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<AuctionOperation>(bot->GetGUID(), auctioneer->GetGUID(), posts, std::vector<Buy>{}));
    return static_cast<std::uint32_t>(posts.size());
}

void VisitMailbox(Player* bot, GameObject* mailbox)
{
    if (!HasCollectableMail(bot) && !HasEmptyMail(bot))
        return;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MailOperation>(bot->GetGUID(), mailbox->GetGUID()));
}

std::uint32_t PostLoot(PlayerbotAI* botAI, Player* bot, Creature* auctioneer, std::uint32_t maxPosts)
{
    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!maxPosts || !house || !ah || house->houseId == uint32(AuctionHouseId::Neutral))
        return 0;  // faction houses only (the goblin houses are the future cross-faction channel)
    std::map<uint32, uint32> lowest;
    for (auto const& [id, a] : ah->GetAuctions())
        if (a && a->owner != bot->GetGUID() && a->buyout && a->itemCount)
        {
            uint32 const unit = a->buyout / a->itemCount;
            auto const [it, fresh] = lowest.emplace(a->item_template, unit);
            if (!fresh)
                it->second = std::min(it->second, unit);
        }
    Params p = detail::gParams;
    p.maxPosts = std::min(p.maxPosts, maxPosts);
    std::vector<Post> posts = PlanPosts(p, Holdings(botAI, bot, house, lowest), bot->GetMoney());
    LOG_INFO("playerbots", "[Trade] seller bot={} auctioneer={} posts={} money={}", bot->GetName(),
             auctioneer->GetEntry(), posts.size(), bot->GetMoney());
    std::uint32_t const n = static_cast<std::uint32_t>(posts.size());
    if (n)
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<AuctionOperation>(
            bot->GetGUID(), auctioneer->GetGUID(), std::move(posts), std::vector<Buy>{}));
    return n;
}

std::uint32_t SeedThinSlotsAt(Player* bot, Creature* auctioneer)
{
    if (!detail::gParams.seedThinSlots || !bot || !auctioneer)
        return 0;
    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!house || house->houseId == uint32(AuctionHouseId::Neutral))
        return 0;  // faction houses only (the neutral goblin houses are the future cross-faction channel)
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    std::uint64_t const day = now / kSeedDayMs;
    std::uint32_t room = 0;
    {
        std::lock_guard<std::mutex> guard(gSeedLock);
        room = SeedRoom(gSeedWindows[house->houseId], day, detail::gParams.seedDailyCap);
    }
    if (!room)
        return 0;  // the house's daily cap is spent
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<SeedOperation>(bot->GetGUID(), auctioneer->GetGUID(), SeedBandOf(bot->GetLevel())));
    return std::min(room, detail::gParams.seedPerVisit);
}

void EmitRow(Player* bot, Action a, std::uint32_t item, std::uint32_t count, std::uint64_t price, std::int64_t gold,
             char const* kind)
{
    LOG_INFO("playerbots", "[Trade] bot={} {} item={} count={} price={} gold={}{}{}", bot->GetName(), ActionName(a),
             item, count, price, gold, kind ? " kind=" : "", kind ? kind : "");
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitTrade(bot, ActionName(a), LedgerFields(a, item, count, price, gold, 0, kind));
}

void NoteFee(Player* bot, FeeKind kind, std::uint64_t copper)
{
    if (!detail::gTreasury || !copper || !AutoWowQuestLedger::Enabled() || !bot)
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->IsAutoWowIndependentParty())
        return;  // the persistent cohort only (random bots are kept up by cheats)
    AutoWowQuestLedger::EmitTrade(bot, ActionName(Action::Fee),
                                  LedgerFields(Action::Fee, 0, 0, copper, -std::int64_t(copper), 0, FeeKindName(kind)));
}
}  // namespace AutoWowTrade
