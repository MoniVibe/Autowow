/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Supply role step (policy: AutoWow/SupplyPolicy.h; world side: AutoWow/AutoWowSupply.cpp). A
// configured rep or artisan never quests, grinds, runs errands or takes contracts: it lives at its capital
// home (mailbox), collects its mail, and the artisan trains, buys thread and crafts. Map thread. Lane F: the artisan
// keeps bag room (junk sold / destroyed, a bag bought) and adventures as an apprentice below ArtisanMinLevel.

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "AuctionHouseMgr.h"
#include "AutoWowOracleRuntime.h"
#include "Bag.h"
#include "BankPackets.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemPackets.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "SupplyPolicy.h"
#include "TradePolicy.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutoWowSupply
{
namespace
{
constexpr std::uint32_t kMaxStuck = 8;             // stuck walk windows before the portal fallback home
constexpr std::uint64_t kTaskTimeoutMs = 180000;   // a station trip that has not arrived is dropped
constexpr float kNearYards = 40.0f;                // direct move to a station object this close

enum class Task : std::uint8_t
{
    None = 0,
    Home = 1,
    Mailbox = 2,
    Trainer = 3,
    Thread = 4,
    Auction = 5,
    Sell = 6,
    Market = 7,  // AutoWow.Supply.Market: the bag-house rep lists house surplus / buys artisan materials
    Junk = 8,    // artisan make-room: sell the sellable junk at the vendor
    Bag = 9,     // artisan make-room: buy and wear a kPouch (no bag worn)
    Bank = 10,   // RepStore: the rep's bank stash / refill at the banker near home
    GearTrainer = 11,  // gear line artisan: its line's due recipes / ranks at the line trainer
    GearVendor = 12    // gear line artisan: its target's vendor reagents (thread, dye) at the line vendor
};

constexpr std::uint32_t kHearthstone = 6948;

constexpr std::uint32_t kSkillupBags = 3;  // Tiers: thread bought for this many skill-up bags

struct RoleState
{
    std::uint32_t version = kStateVersion;
    std::uint64_t nextMs = 0;       // next task decision
    std::uint64_t marketMs = 0;     // Market: next faction AH visit
    Task task = Task::None;
    std::uint64_t taskSinceMs = 0;
    std::uint32_t stuck = 0;
    std::uint32_t castSpell = 0;    // craft cast issued, output not yet counted
    std::uint32_t castItem = 0;
    std::uint32_t castBefore = 0;
    bool apprentice = false;    // last seen below ArtisanMinLevel (logs the switch once each way)
    bool craftBlocked = false;  // the last craft had no room for its product (make-room wants a slot)
    std::uint64_t bagGrantMs = 0;  // next bag grant request (at most one per OutfitCheckMs: a refusal is a row)
    std::uint8_t castLine = kNoLine;  // the gear line of the cast in flight (its craft row), else kNoLine
};

// Map threads; only role bots (bounded by the configured roles) are stored.
std::mutex gRoleLock;
std::unordered_map<std::uint32_t, RoleState> gRoleStates;

RoleState LoadRole(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gRoleLock);
    auto const it = gRoleStates.find(guid);
    return it == gRoleStates.end() ? RoleState{} : it->second;
}

void StoreRole(std::uint32_t guid, RoleState const& s)
{
    std::lock_guard<std::mutex> guard(gRoleLock);
    gRoleStates[guid] = s;
}

std::uint32_t LooseCount(Player* bot, std::uint32_t entry)
{
    // GetItemCount counts equipped items too: worn bags, and a gear line piece the artisan wears, are not stock.
    std::uint32_t n = bot->GetItemCount(entry, false);
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); worn && worn->GetEntry() == entry)
            --n;
    return n;
}

std::vector<std::uint32_t> LooseGuids(Player* bot, std::uint32_t entry, std::uint32_t max)
{
    std::vector<std::uint32_t> out;
    auto take = [&](Item* item)
    {
        if (item && item->GetEntry() == entry && out.size() < max)
            out.push_back(static_cast<std::uint32_t>(item->GetGUID().GetCounter()));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        take(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                take(b->GetItemByPos(slot));
    std::sort(out.begin(), out.end());
    return out;
}

// The artisan's due learn spells at `trainer` (teachable now): total cost, and whether any is affordable.
std::uint64_t LearnCost(Player* bot, Trainer::Trainer* trainer, bool& affordable,
                        std::vector<std::uint32_t> const& learn = LearnSpells())
{
    affordable = false;
    std::uint64_t cost = 0;
    if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
        return 0;
    for (std::uint32_t const id : learn)
        for (Trainer::Spell const& s : trainer->GetSpells())
            if (s.SpellId == id && trainer->CanTeachSpell(bot, &s))
            {
                cost += s.MoneyCost;
                affordable = affordable || s.MoneyCost <= bot->GetMoney();
            }
    return cost;
}

Station const* StationFor(Stations const& st, Task task)
{
    switch (task)
    {
        case Task::Mailbox: return st.mailbox.entry ? &st.mailbox : nullptr;
        case Task::Trainer: return st.trainer.entry ? &st.trainer : nullptr;
        case Task::Thread:
        case Task::Sell:
        case Task::Junk: return st.threadVendor.entry ? &st.threadVendor : nullptr;
        case Task::Auction:
        case Task::Market: return st.auctioneer.entry ? &st.auctioneer : nullptr;
        case Task::Bank: return st.banker.entry ? &st.banker : nullptr;
        default: return nullptr;
    }
}

// Tiers: the thread the artisan needs now (item, units): the product order's, else a bag skill-up's.
std::pair<std::uint32_t, std::uint32_t> TierThread(TeamView const& view, bool canBag)
{
    if (canBag && view.remaining && view.product != kNoTier)
        return {kTiers[view.product].thread, view.remaining * kTiers[view.product].recipe.threadPerBag};
    if (!view.remaining && view.skillup != kNoTier && view.skillupBag)
        return {kTiers[view.skillup].thread, kSkillupBags * kTiers[view.skillup].recipe.threadPerBag};
    return {ThreadItem(), 0};
}

bool HouseMaterial(std::uint32_t entry)
{
    for (Tier const& t : kTiers)
        if (entry == t.cloth || entry == t.bolt || entry == t.thread || (t.extra && entry == t.extra))
            return true;
    return false;
}

// A quest in the log (not yet rewarded) wants the item: an objective, a source drop, or the item it handed out
// (QuestWantsItem).
bool QuestNeeds(Player* bot, std::uint32_t entry)
{
    for (uint8 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        Quest const* q = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(i));
        QuestStatus const status = q ? bot->GetQuestStatus(q->GetQuestId()) : QUEST_STATUS_NONE;
        if ((status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE) &&
            QuestWantsItem(entry, q->GetSrcItemId(), q->RequiredItemId, q->ItemDrop))
            return true;
    }
    return false;
}

// The make-room view of the artisan's loose stacks (backpack and worn bags): house = its line's items, else the bag
// tiers' materials; plus its gear line's reagents, intermediates and pieces.
std::vector<BagStack> BagStacksOf(Player* bot, ProductLine const* line, ProductLine const* gear = nullptr)
{
    std::vector<BagStack> out;
    auto add = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return;
        BagStack b;
        b.guid = static_cast<std::uint32_t>(item->GetGUID().GetCounter());
        b.sellPrice = proto->SellPrice;
        b.house = (line ? LineItem(*line, proto->ItemId) : HouseMaterial(proto->ItemId)) ||
                  (gear && LineItem(GearTable(*gear), proto->ItemId));
        b.quest = QuestNeeds(bot, proto->ItemId);
        b.keep = proto->ItemId == kHearthstone || proto->TotemCategory || proto->Class == ITEM_CLASS_CONTAINER ||
                 (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE) ||
                 proto->HasFlag(ITEM_FLAG_NO_USER_DESTROY);
        out.push_back(b);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                add(b->GetItemByPos(slot));
    return out;
}

// RepStore: the rep's bank moves (item guid lows). Bags over 75% full: loose trade goods beyond RepKeep per item go
// to the bank (PlanStash, within its free slots); else the house's materials (the bag tiers' or its line's, and its
// gear line's) under RepKeep come back (PlanUnstash). Empty when nothing is due.
std::vector<std::uint32_t> BankMoves(Player* bot, ProductLine const* line, bool bagHouse, ProductLine const* gear = nullptr)
{
    std::vector<StashStack> loose, banked;
    std::uint32_t total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START, bankFree = 0;
    auto stack = [](Item* item)
    { return StashStack{static_cast<std::uint32_t>(item->GetGUID().GetCounter()), item->GetEntry(), item->GetCount()}; };
    auto material = [&](Item* item)
    {
        if (item && item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS && !item->IsSoulBound())
            loose.push_back(stack(item));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        material(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
        {
            total += b->GetBagSize();
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                material(b->GetItemByPos(slot));
        }
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            banked.push_back(stack(item));
        else
            ++bankFree;
    }
    for (uint8 bag = BANK_SLOT_BAG_START; bag < BANK_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
            {
                if (Item* item = b->GetItemByPos(slot))
                    banked.push_back(stack(item));
                else
                    ++bankFree;
            }
    std::uint32_t const keep = detail::gParams.repKeep, free = bot->GetFreeInventorySpace();
    if (StashDue(free, total))
        return PlanStash(std::move(loose), keep, bankFree);
    banked.erase(std::remove_if(banked.begin(), banked.end(), [&](StashStack const& b)
                                {
                                    return (line ? !LineItem(*line, b.item) : !(bagHouse && HouseMaterial(b.item))) &&
                                           !(gear && LineItem(GearTable(*gear), b.item));
                                }),
                 banked.end());
    return PlanUnstash(std::move(banked), loose, keep, free, total);
}

bool BagWorn(Player* bot)
{
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (bot->GetBagByPos(slot))
            return true;
    return false;
}

// Moves a loose `entry` bag into the first empty bag slot (the stock swap the equip action uses).
void WearBag(Player* bot, std::uint32_t entry)
{
    std::vector<std::uint32_t> const g = LooseGuids(bot, entry, 1);
    Item* bag = g.empty() ? nullptr : bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g[0]));
    for (uint8 slot = INVENTORY_SLOT_BAG_START; bag && slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            bot->SwapItem(static_cast<uint16>((bag->GetBagSlot() << 8) | bag->GetSlot()),
                          static_cast<uint16>((INVENTORY_SLOT_BAG_0 << 8) | slot));
            return;
        }
}

// Sells these stacks at `npc` through the stock sell handler; returns how many went.
std::uint32_t SellGuids(Player* bot, Creature* npc, std::vector<std::uint32_t> const& guids)
{
    std::uint32_t sold = 0;
    for (std::uint32_t const g : guids)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g));
        if (!item)
            continue;
        WorldPacket packet(CMSG_SELL_ITEM);
        packet << npc->GetGUID() << item->GetGUID() << uint32(item->GetCount());
        WorldPackets::Item::SellItem sell(std::move(packet));
        sell.Read();
        bot->GetSession()->HandleSellItemOpcode(sell);
        if (!bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
            ++sold;
    }
    return sold;
}

// Market: whole stacks the rep lists (ascending guid, at most kMaxMailStacks): each cloth tier beyond the next one
// above the artisan's reach (Listable) above SellKeep, and every non-house trade good. Nothing while the artisan's
// skill is unknown.
std::vector<std::uint32_t> MarketSellable(Player* bot, TeamView const& view)
{
    std::vector<std::uint32_t> out;
    if (!view.artisanSkill)
        return out;
    for (std::size_t i = 0; i < kTierCount; ++i)
    {
        Tier const& t = kTiers[i];
        if (!Listable(i, view.artisanSkill))
            continue;
        std::vector<Stack> stacks;
        for (std::uint32_t const g : LooseGuids(bot, t.cloth, 0xFFFFFFFFu))
            if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                stacks.push_back({g, item->GetCount()});
        for (std::uint32_t const g : SellStacks(stacks, detail::gParams.sellKeep))
            out.push_back(g);
    }
    auto take = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS && !HouseMaterial(proto->ItemId) && !item->IsSoulBound())
            out.push_back(static_cast<std::uint32_t>(item->GetGUID().GetCounter()));
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        take(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                take(b->GetItemByPos(slot));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > kMaxMailStacks)
        out.resize(kMaxMailStacks);
    return out;
}
// A catalog line rep's surplus: per tier, whole stacks (ascending guid) covering its surplus units.
std::vector<std::uint32_t> LineSurplusGuids(Player* bot, ProductLine const& l, LineView const& view)
{
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < l.tierCount; ++i)
    {
        if (!view.surplus[i])
            continue;
        std::vector<Stack> stacks;
        for (std::uint32_t const g : LooseGuids(bot, l.tiers[i].product, 0xFFFFFFFFu))
            if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                stacks.push_back({g, item->GetCount()});
        for (std::uint32_t const g : PickStacks(stacks, view.surplus[i]))
            if (out.size() < kMaxMailStacks)
                out.push_back(g);
    }
    return out;
}
}  // namespace
}  // namespace AutoWowSupply

bool NewRpgBaseAction::SupplyStep()
{
    using namespace AutoWowSupply;
    uint32 const guid = bot->GetGUID().GetCounter();
    RoleInfo const role = RoleOf(guid);
    if (role.role == Role::None || AutoWowOracleRuntime::IsManagedBot(guid))
        return false;
    // Apprentice phase (ArtisanMinLevel): below it the artisan adventures like any cohort member (ActiveRoleOf, the
    // same gate as the party / self-craft / chain checks); at the level it goes home and trains.
    if (role.role == Role::Artisan)
    {
        bool const apprentice = ActiveRoleOf(bot).role == Role::None;
        RoleState a = LoadRole(guid);
        if (a.apprentice != apprentice)
        {
            if (apprentice)
                LOG_INFO("playerbots", "[Supply] apprentice bot={} level={} min_level={}: adventures until then",
                         bot->GetName(), bot->GetLevel(), detail::gParams.artisanMinLevel);
            else
                LOG_INFO("playerbots", "[Supply] apprentice bot={} level={} graduated: artisan, goes home to train",
                         bot->GetName(), bot->GetLevel());
            a.apprentice = apprentice;
            StoreRole(guid, a);
        }
        if (apprentice)
            return false;
    }
    // A role bot never falls through to quests / grind / errands / contracts. Combat, death, a flight, a
    // teleport or a cast in progress belong to other engines or finish on their own: hold this tick.
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported() ||
        bot->IsNonMeleeSpellCast(false) || !role.home.set)
        return true;

    Params const& p = detail::gParams;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    RoleState s = LoadRole(guid);
    NewRpgInfo& info = botAI->rpgInfo;
    if (info.GetStatus() != RPG_IDLE)
        info.ChangeToIdle();
    Home const& home = role.home;
    Stations const& st = StationsOf(role.alliance);
    TeamView const view = ViewOf(role.alliance);
    bool const artisan = role.role == Role::Artisan && role.bagHouse;
    // A catalog line's rep / artisan (AutoWow.Supply.Products, e.g. potions): its own view and stations.
    bool const lined = role.line != kNoLine;
    Line const lineId = lined ? static_cast<Line>(role.line) : Line::Bags;
    ProductLine const& L = LineOf(lineId);
    LineView const lview = lined ? LineViewOf(lineId, role.alliance) : LineView{};
    Stations const& lst = lined ? LineStationsOf(lineId, role.alliance) : st;
    // A gear line's artisan (lane V, Products cloth_gear / leather_gear): the line's view, stations and table.
    bool const geared = role.gear != kNoLine && role.role == Role::Artisan;
    Line const gearId = geared ? static_cast<Line>(role.gear) : Line::Bags;
    LineView const gview = geared ? LineViewOf(gearId, role.alliance) : LineView{};
    Stations const& gst = geared ? LineStationsOf(gearId, role.alliance) : st;
    RecipeTable const gtab = geared ? GearTable(LineOf(gearId)) : RecipeTable{};
    ProductLine const* const repGear = role.gear != kNoLine ? &LineOf(static_cast<Line>(role.gear)) : nullptr;

    // A finished craft cast: count what it made.
    if (s.castSpell)
    {
        std::uint32_t const have = LooseCount(bot, s.castItem);
        if (have > s.castBefore && s.castLine != kNoLine)
            EmitLine(static_cast<Line>(s.castLine), bot, Reason::Craft, gview.orderId, s.castItem, have - s.castBefore, 0,
                     guid, guid);
        else if (have > s.castBefore && lined)
            EmitLine(lineId, bot, Reason::Craft, lview.orderId, s.castItem, have - s.castBefore, 0, guid, guid);
        else if (have > s.castBefore)
            Emit(bot, Reason::Craft, view.orderId, s.castItem, have - s.castBefore, 0, guid, guid);
        s.castSpell = 0;
        s.castLine = kNoLine;
    }

    // Home is on another map: the portal fallback (owner ruling 2026-09-25: portals acceptable, logged).
    auto portal = [&](char const* why)
    {
        LOG_INFO("playerbots", "[Supply] bot={} travel=portal why={} from map={} ({},{}) to map={} ({},{})",
                 bot->GetName(), why, bot->GetMapId(), int32(bot->GetPositionX()), int32(bot->GetPositionY()),
                 home.map, home.x, home.y);
        Emit(bot, Reason::Travel, 0, 0, 0, 0, guid, guid, why);
        s.stuck = 0;
        s.task = Task::None;
        StoreRole(guid, s);
        bot->TeleportTo(home.map, float(home.x), float(home.y), float(home.z), bot->GetOrientation());
        return true;
    };
    if (bot->GetMapId() != home.map || bot->GetMap()->Instanceable())
        return portal("other_map");

    // A `supply` row of the bot's line (bags or its catalog line).
    auto emit = [&](Reason r, std::uint32_t item, std::uint32_t count, std::uint64_t copper, char const* op)
    {
        if (lined)
            EmitLine(lineId, bot, r, 0, item, count, copper, guid, 0, op);
        else
            Emit(bot, r, 0, item, count, copper, guid, 0, op);
    };
    bool const crafter = role.role == Role::Artisan && (artisan || lined || geared);

    std::int64_t const dx = std::int64_t(bot->GetPositionX()) - home.x, dy = std::int64_t(bot->GetPositionY()) - home.y;
    bool const atHome = dx * dx + dy * dy <= std::int64_t(p.homeYards) * p.homeYards;

    // Decide the next station trip.
    if (s.task == Task::None && now >= s.nextMs)
    {
        s.nextMs = now + p.tickMs;
        Task next = Task::None;
        // Make room (ArtisanFreeSlots; a slot while a craft has none): unsellable junk goes now, sellable junk on a
        // vendor trip, then a bag when none is worn (soak-s45-full-r1: a tailor with 16 backpack slots of quest
        // junk and no bag looped on its mailbox and never crafted).
        std::uint32_t const roomWant = crafter ? RoomTarget(p.artisanFreeSlots, s.craftBlocked) : 0;
        if (roomWant && bot->GetFreeInventorySpace() < roomWant)
        {
            RoomPlan const plan = PlanRoom(BagStacksOf(bot, lined ? &L : nullptr, geared ? &LineOf(gearId) : nullptr),
                                           bot->GetFreeInventorySpace(), roomWant);
            for (std::uint32_t const g : plan.destroy)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                {
                    std::uint32_t const entry = item->GetEntry(), count = item->GetCount();
                    LOG_INFO("playerbots", "[Supply] junk bot={} destroy item={} count={} free={} want={}", bot->GetName(),
                             entry, count, bot->GetFreeInventorySpace(), roomWant);
                    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                    emit(Reason::Junk, entry, count, 0, "destroy");
                }
            std::uint32_t const free = bot->GetFreeInventorySpace();
            Station const& bagVendor = BagVendorOf(role.alliance);
            std::uint32_t const bagPrice = PriceOf(kPouch);
            if (!plan.sell.empty() && (lined ? lst : st).threadVendor.entry)
                next = Task::Junk;
            else if (WantsBag(BagWorn(bot), free, roomWant) && bagVendor.entry && free)
            {
                if (bot->GetMoney() >= bagPrice)
                    next = Task::Bag;
                else if (Outfit() && !GrantPending(guid) && now >= s.bagGrantMs)
                {
                    s.bagGrantMs = now + p.outfitCheckMs;
                    RequestGrant(bot, bagPrice);  // lane O: the house bank pays the shortfall at the next tick
                    LOG_INFO("playerbots", "[Supply] bot={} bag grant requested item={} need={} money={}", bot->GetName(),
                             kPouch, bagPrice, bot->GetMoney());
                }
            }
        }
        // RepStore: the rep's bank stash (bags over 75% full) or refill (house materials under RepKeep) at the banker
        // near home, before the mailbox (soak-s48-full-r1: the Weavers hub, 16 backpack slots).
        if (next == Task::None && role.role == Role::Rep && RepStore() && (lined ? lst : st).banker.entry &&
            !BankMoves(bot, lined ? &L : nullptr, role.bagHouse, repGear).empty())
            next = Task::Bank;
        // Only a mail it can take something from (or, DeleteEmptyMail, emptied mail to delete: soak-s48-full-r1, 120
        // of them blocked the rep's box): full bags otherwise re-pick the mailbox every tick.
        if (next == Task::None && (AutoWowTrade::HasCollectableMailWithRoom(bot) || AutoWowTrade::HasEmptyMail(bot)) &&
            st.mailbox.entry)
            next = Task::Mailbox;
        if (artisan && Tiers())
        {
            // Tiers: the product tier's bag recipe, the product / skill-up thread.
            bool const canBag = view.product != kNoTier && bot->HasSpell(kTiers[view.product].bagSpell);
            bool learnAffordable = false;
            std::uint64_t const learnCost = st.trainer.entry
                ? LearnCost(bot, sObjectMgr->GetTrainer(st.trainer.entry), learnAffordable) : 0;
            auto const [threadItem, threadWant] = TierThread(view, canBag);
            std::uint32_t const thread = LooseCount(bot, threadItem);
            std::uint32_t const price = PriceOf(threadItem);
            std::uint32_t const remaining = canBag ? view.remaining : 0;
            std::uint64_t const want = std::uint64_t(Short(threadWant, thread)) * price + learnCost +
                                       (remaining ? AutoWowGuilds::Postage(remaining) : 0);
            SetArtisanWant(role.alliance, want > bot->GetMoney() ? want - bot->GetMoney() : 0, canBag);
            if (next == Task::None && learnAffordable)
                next = Task::Trainer;
            if (next == Task::None && thread < threadWant && bot->GetMoney() >= price && st.threadVendor.entry)
                next = Task::Thread;
        }
        else if (artisan)
        {
            bool const canBag = bot->HasSpell(BagSpell());
            bool learnAffordable = false;
            std::uint64_t const learnCost = st.trainer.entry
                ? LearnCost(bot, sObjectMgr->GetTrainer(st.trainer.entry), learnAffordable) : 0;
            std::uint32_t const thread = LooseCount(bot, ThreadItem());
            std::uint32_t const remaining = canBag ? view.remaining : 0;
            // Its own postage mails the bags to the rep (the rep's is the guild bank's).
            std::uint64_t const extra = learnCost + (remaining ? AutoWowGuilds::Postage(remaining) : 0);
            SetArtisanWant(role.alliance,
                           CopperShort(BagRecipe(), remaining, thread, ThreadPrice(), extra, bot->GetMoney()), canBag);
            if (next == Task::None && learnAffordable)
                next = Task::Trainer;
            if (next == Task::None && thread < remaining * BagRecipe().threadPerBag && bot->GetMoney() >= ThreadPrice() &&
                st.threadVendor.entry)
                next = Task::Thread;
        }
        if (lined && role.role == Role::Artisan)
        {
            // Line artisan: due trainer spells, Vendor reagents below what the view asks it to hold, postage
            // for its deliveries to the rep; the treasury tops up what its purse lacks.
            bool learnAffordable = false;
            std::uint64_t const learnCost = lst.trainer.entry
                ? LearnCost(bot, sObjectMgr->GetTrainer(lst.trainer.entry), learnAffordable, LineLearnSpells(lineId))
                : 0;
            std::uint64_t buy = 0, cheapest = 0;
            for (MarketWant const& w : lview.vendor)
                if (std::uint32_t const miss = Short(w.units, LooseCount(bot, w.item)))
                {
                    buy += std::uint64_t(miss) * w.sellPrice;  // sellPrice = the vendor price per unit here
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(w.item);
                    std::uint64_t const lot = proto ? proto->BuyPrice : 0;
                    cheapest = cheapest ? std::min(cheapest, lot) : lot;
                }
            std::uint64_t const want = buy + learnCost + AutoWowGuilds::Postage(L.tierCount);
            SetLineArtisanWant(lineId, role.alliance, want > bot->GetMoney() ? want - bot->GetMoney() : 0);
            if (next == Task::None && learnAffordable)
                next = Task::Trainer;
            if (next == Task::None && buy && bot->GetMoney() >= cheapest && lst.threadVendor.entry)
                next = Task::Thread;
        }
        if (geared)
        {
            // Gear line artisan: its line's due trainer spells, the target's vendor reagents below what the view asks
            // it to hold, postage for its pieces; the treasury tops up what its purse lacks (GearTick).
            bool learnAffordable = false;
            std::uint64_t const learnCost = gst.trainer.entry
                ? LearnCost(bot, sObjectMgr->GetTrainer(gst.trainer.entry), learnAffordable, LineLearnSpells(gearId))
                : 0;
            std::uint64_t buy = 0, cheapest = 0;
            for (MarketWant const& w : gview.vendor)
                if (std::uint32_t const miss = Short(w.units, LooseCount(bot, w.item)))
                {
                    buy += std::uint64_t(miss) * w.sellPrice;  // sellPrice = the vendor price per unit here
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(w.item);
                    std::uint64_t const lot = proto ? proto->BuyPrice : 0;
                    cheapest = cheapest ? std::min(cheapest, lot) : lot;
                }
            std::uint64_t const want = buy + learnCost + AutoWowGuilds::Postage(gview.remaining ? gview.remaining : 1);
            SetLineArtisanWant(gearId, role.alliance, want > bot->GetMoney() ? want - bot->GetMoney() : 0);
            if (next == Task::None && learnAffordable)
                next = Task::GearTrainer;
            if (next == Task::None && buy && bot->GetMoney() >= cheapest && gst.threadVendor.entry)
                next = Task::GearVendor;
        }
        std::uint32_t lineSurplus = 0;
        for (std::uint32_t const u : lview.surplus)
            lineSurplus += u;
        if (lined && role.role == Role::Rep && next == Task::None && lineSurplus)
            next = lst.auctioneer.entry ? Task::Auction : Task::Sell;
        if (lined && role.role == Role::Rep && Market() && next == Task::None && now >= s.marketMs &&
            lst.auctioneer.entry && !lview.buy.empty())
        {
            next = Task::Market;
            s.marketMs = now + p.tickMs;  // one visit (one BuyBudget) per tick
        }
        if (!lined && role.role == Role::Rep && next == Task::None && view.surplus)
            next = st.auctioneer.entry ? Task::Auction : Task::Sell;
        if (role.role == Role::Rep && role.bagHouse && Market() && next == Task::None && now >= s.marketMs &&
            st.auctioneer.entry && (!view.buy.empty() || !MarketSellable(bot, view).empty()))
        {
            next = Task::Market;
            s.marketMs = now + p.tickMs;  // one visit (one BuyBudget) per tick
        }
        if (next == Task::None && !atHome)
            next = Task::Home;
        if (next != Task::None)
        {
            s.task = next;
            s.taskSinceMs = now;
            LOG_INFO("playerbots", "[Supply] bot={} role={} task={} money={} oid={} remaining={} surplus={}",
                     bot->GetName(), role.role == Role::Rep ? "rep" : "artisan", uint32(next), bot->GetMoney(),
                     lined ? lview.orderId : view.orderId, lined ? lview.remaining : view.remaining,
                     lined ? lineSurplus : view.surplus);
        }
    }

    if (s.task == Task::Home)
    {
        if (atHome)
        {
            s.task = Task::None;
            s.stuck = 0;
        }
        else if (WalkLeg(WorldPosition(home.map, float(home.x), float(home.y), float(home.z))) && ++s.stuck > kMaxStuck)
            return portal("stuck");
        StoreRole(guid, s);
        return true;
    }

    if (s.task != Task::None)
    {
        Station const* station = s.task == Task::Bag
            ? (BagVendorOf(role.alliance).entry ? &BagVendorOf(role.alliance) : nullptr)
            : s.task == Task::GearTrainer ? StationFor(gst, Task::Trainer)
            : s.task == Task::GearVendor  ? StationFor(gst, Task::Thread)
                                          : StationFor(lined ? lst : st, s.task);
        if (!station || now - s.taskSinceMs > kTaskTimeoutMs)
        {
            LOG_INFO("playerbots", "[Supply] bot={} drop task={} (station={} timeout)", bot->GetName(), uint32(s.task),
                     station ? station->entry : 0);
            s.task = Task::None;
            StoreRole(guid, s);
            return true;
        }
        WorldObject* target = nullptr;
        if (s.task == Task::Mailbox)
            target = bot->FindNearestGameObject(station->entry, 60.0f);
        else if (Creature* c = bot->FindNearestCreature(station->entry, 60.0f); c && c->IsAlive())
            target = c;
        if (!target || !bot->IsWithinDistInMap(target, INTERACTION_DISTANCE - 0.5f))
        {
            if (target && bot->GetExactDist2d(target) < kNearYards)
            {
                if (!bot->isMoving())
                    bot->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                                                      target->GetPositionZ());
            }
            else
                WalkLeg(WorldPosition(station->map, float(station->x), float(station->y), float(station->z)));
            StoreRole(guid, s);
            return true;
        }

        bot->StopMoving();
        bot->SetFacingToObject(target);
        switch (s.task)
        {
            case Task::Mailbox:
                AutoWowTrade::VisitMailbox(bot, target->ToGameObject());
                break;
            case Task::Trainer:
            {
                Creature* npc = target->ToCreature();
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
                std::uint64_t const m0 = bot->GetMoney();
                for (std::uint32_t const id : lined ? LineLearnSpells(lineId) : LearnSpells())
                    if (trainer)
                        for (Trainer::Spell const& sp : trainer->GetSpells())
                            if (sp.SpellId == id && trainer->CanTeachSpell(bot, &sp) && sp.MoneyCost <= bot->GetMoney())
                                trainer->TeachSpell(npc, bot, id);  // stock path: validity, reputation price, money
                if (m0 > bot->GetMoney())
                    AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Train, m0 - bot->GetMoney());
                if (lined)
                    LOG_INFO("playerbots", "[Supply] bot={} trained at={} spent={} line={} skill={}/{}", bot->GetName(),
                             npc->GetEntry(), m0 - bot->GetMoney(), L.name, bot->GetSkillValue(L.skillLine),
                             bot->GetMaxSkillValue(L.skillLine));
                else
                    LOG_INFO("playerbots", "[Supply] bot={} trained at={} spent={} tailoring={}/{} bag_recipe={}",
                             bot->GetName(), npc->GetEntry(), m0 - bot->GetMoney(), bot->GetSkillValue(SKILL_TAILORING),
                             bot->GetMaxSkillValue(SKILL_TAILORING), bot->HasSpell(BagSpell()));
                break;
            }
            case Task::Thread:
            {
                Creature* npc = target->ToCreature();
                VendorItemData const* list = npc->GetVendorItems();
                if (lined)
                {
                    // Line artisan: each Vendor reagent up to what the view asks it to hold (a purchase is one
                    // vendor lot of BuyCount units; at most 60 purchases per item per visit).
                    std::uint64_t const m0 = bot->GetMoney();
                    for (MarketWant const& w : lview.vendor)
                        for (uint32 i = 0; list && i < list->GetItemCount(); ++i)
                            if (VendorItem const* vi = list->GetItem(i); vi && vi->item == w.item && !vi->ExtendedCost)
                            {
                                for (std::uint32_t k = 0; k < 60 && LooseCount(bot, w.item) < w.units; ++k)
                                {
                                    std::uint32_t const before = LooseCount(bot, w.item);
                                    bot->BuyItemFromVendorSlot(npc->GetGUID(), i, w.item, 1, NULL_BAG, NULL_SLOT);
                                    if (LooseCount(bot, w.item) <= before)
                                        break;  // money, bags or stock
                                }
                                break;
                            }
                    LOG_INFO("playerbots", "[Supply] bot={} line={} bought vendor reagents spent={}", bot->GetName(),
                             L.name, m0 - bot->GetMoney());
                    break;
                }
                // Tiers: the product / skill-up tier's thread (coarse or fine); off: Coarse Thread for the order.
                std::pair<std::uint32_t, std::uint32_t> const tierThread = Tiers()
                    ? TierThread(view, view.product != kNoTier && bot->HasSpell(kTiers[view.product].bagSpell))
                    : std::pair<std::uint32_t, std::uint32_t>{ThreadItem(), view.remaining * BagRecipe().threadPerBag};
                std::uint32_t const threadItem = tierThread.first;
                std::uint32_t const want = tierThread.second;
                std::uint32_t const have = LooseCount(bot, threadItem);
                std::uint64_t const m0 = bot->GetMoney();
                for (uint32 i = 0; list && i < list->GetItemCount(); ++i)
                    if (VendorItem const* vi = list->GetItem(i); vi && vi->item == threadItem && !vi->ExtendedCost)
                    {
                        for (std::uint32_t n = have; n < want && n < have + 60; ++n)
                        {
                            std::uint32_t const before = LooseCount(bot, threadItem);
                            bot->BuyItemFromVendorSlot(npc->GetGUID(), i, threadItem, 1, NULL_BAG, NULL_SLOT);
                            if (LooseCount(bot, threadItem) <= before)
                                break;  // money, bags or stock
                        }
                        break;
                    }
                LOG_INFO("playerbots", "[Supply] bot={} bought thread {}->{} spent={}", bot->GetName(), have,
                         LooseCount(bot, threadItem), m0 - bot->GetMoney());
                break;
            }
            case Task::GearTrainer:
            {
                Creature* npc = target->ToCreature();
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
                std::uint64_t const m0 = bot->GetMoney();
                for (std::uint32_t const id : LineLearnSpells(gearId))
                    if (trainer)
                        for (Trainer::Spell const& sp : trainer->GetSpells())
                            if (sp.SpellId == id && trainer->CanTeachSpell(bot, &sp) && sp.MoneyCost <= bot->GetMoney())
                                trainer->TeachSpell(npc, bot, id);  // stock path: validity, reputation price, money
                if (m0 > bot->GetMoney())
                    AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Train, m0 - bot->GetMoney());
                LOG_INFO("playerbots", "[Supply] bot={} trained at={} spent={} line={} skill={}/{}", bot->GetName(),
                         npc->GetEntry(), m0 - bot->GetMoney(), LineOf(gearId).name,
                         bot->GetSkillValue(LineOf(gearId).skillLine), bot->GetMaxSkillValue(LineOf(gearId).skillLine));
                break;
            }
            case Task::GearVendor:
            {
                // Each Vendor reagent up to what the view asks it to hold (a purchase is one vendor lot of BuyCount
                // units; at most 60 purchases per item per visit).
                Creature* npc = target->ToCreature();
                VendorItemData const* list = npc->GetVendorItems();
                std::uint64_t const m0 = bot->GetMoney();
                for (MarketWant const& w : gview.vendor)
                    for (uint32 i = 0; list && i < list->GetItemCount(); ++i)
                        if (VendorItem const* vi = list->GetItem(i); vi && vi->item == w.item && !vi->ExtendedCost)
                        {
                            for (std::uint32_t k = 0; k < 60 && LooseCount(bot, w.item) < w.units; ++k)
                            {
                                std::uint32_t const before = LooseCount(bot, w.item);
                                bot->BuyItemFromVendorSlot(npc->GetGUID(), i, w.item, 1, NULL_BAG, NULL_SLOT);
                                if (LooseCount(bot, w.item) <= before)
                                    break;  // money, bags or stock
                            }
                            break;
                        }
                LOG_INFO("playerbots", "[Supply] bot={} line={} bought vendor reagents spent={}", bot->GetName(),
                         LineOf(gearId).name, m0 - bot->GetMoney());
                break;
            }
            case Task::Auction:
            {
                if (lined)
                {
                    std::vector<std::uint32_t> const goods = LineSurplusGuids(bot, L, lview);
                    std::vector<AutoWowTrade::Post> planned;
                    if (!goods.empty())
                        AutoWowTrade::PostStacks(bot, target->ToCreature(), goods, &planned);
                    for (AutoWowTrade::Post const& post : planned)
                        EmitLine(lineId, bot, SurplusReason(), lview.orderId, post.entry, post.count, post.buyout, guid,
                                 0, "auction");
                    if (!planned.empty())
                    {
                        ClearLineSurplus(lineId, role.alliance);
                        break;
                    }
                    s.task = Task::Sell;
                    s.taskSinceMs = now;
                    StoreRole(guid, s);
                    return true;
                }
                if (Tiers())
                {
                    // Every tier's surplus bags; a row per posted bag item.
                    std::vector<std::uint32_t> bags;
                    for (std::size_t i = 0; i < kTierCount; ++i)
                        for (std::uint32_t const g : LooseGuids(bot, kTiers[i].bag, view.surplusBags[i]))
                            bags.push_back(g);
                    std::vector<AutoWowTrade::Post> planned;
                    if (!bags.empty())
                        AutoWowTrade::PostStacks(bot, target->ToCreature(), bags, &planned);
                    for (AutoWowTrade::Post const& post : planned)
                        Emit(bot, SurplusReason(), view.orderId, post.entry, post.count, post.buyout, guid, 0, "auction");
                    if (!planned.empty())
                    {
                        ClearSurplus(role.alliance);
                        break;
                    }
                    s.task = Task::Sell;
                    s.taskSinceMs = now;
                    StoreRole(guid, s);
                    return true;
                }
                std::vector<std::uint32_t> const bags = LooseGuids(bot, BagItem(), view.surplus);
                std::uint32_t const posted =
                    bags.empty() ? 0 : AutoWowTrade::PostStacks(bot, target->ToCreature(), bags);
                if (posted)
                {
                    Emit(bot, SurplusReason(), view.orderId, BagItem(), posted, 0, guid, 0, "auction");
                    ClearSurplus(role.alliance);
                    break;
                }
                // Priced out (competition under the vendor floor) or nothing to list: vendor instead.
                s.task = Task::Sell;
                s.taskSinceMs = now;
                StoreRole(guid, s);
                return true;
            }
            case Task::Sell:
            {
                Creature* npc = target->ToCreature();
                std::uint64_t const m0 = bot->GetMoney();
                std::vector<std::uint32_t> surplus;
                if (lined)
                    surplus = LineSurplusGuids(bot, L, lview);
                else if (Tiers())
                {
                    for (std::size_t i = 0; i < kTierCount; ++i)
                        for (std::uint32_t const g : LooseGuids(bot, kTiers[i].bag, view.surplusBags[i]))
                            surplus.push_back(g);
                }
                else
                    surplus = LooseGuids(bot, BagItem(), view.surplus);
                std::uint32_t const sold = SellGuids(bot, npc, surplus);
                if (sold && lined)
                    EmitLine(lineId, bot, SurplusReason(), lview.orderId, 0, sold, bot->GetMoney() - m0, guid, 0,
                             "vendor");  // mixed tiers (item 0; count = stacks)
                else if (sold)
                    Emit(bot, SurplusReason(), view.orderId, Tiers() ? 0 : BagItem(), sold, bot->GetMoney() - m0, guid,
                         0, "vendor");  // Tiers: mixed bag items (item 0)
                if (lined)
                    ClearLineSurplus(lineId, role.alliance);
                else
                    ClearSurplus(role.alliance);
                break;
            }
            case Task::Junk:
            {
                // Make-room sale: the sellable junk, re-planned here (bags may have changed on the way).
                std::vector<std::uint32_t> const sell = PlanRoom(BagStacksOf(bot, lined ? &L : nullptr,
                                                                             geared ? &LineOf(gearId) : nullptr),
                                                                 bot->GetFreeInventorySpace(),
                                                                 RoomTarget(p.artisanFreeSlots, s.craftBlocked)).sell;
                std::uint64_t const m0 = bot->GetMoney();
                std::uint32_t const sold = SellGuids(bot, target->ToCreature(), sell);
                LOG_INFO("playerbots", "[Supply] junk bot={} sold stacks={}/{} copper={} free={}", bot->GetName(), sold,
                         sell.size(), bot->GetMoney() - m0, bot->GetFreeInventorySpace());
                if (sold)
                    emit(Reason::Junk, 0, sold, bot->GetMoney() - m0, "vendor");  // mixed items (item 0; count = stacks)
                break;
            }
            case Task::Bag:
            {
                // Make-room bag: one kPouch with its own (granted) gold, worn at once; an outfit row like lane O's tools.
                Creature* npc = target->ToCreature();
                VendorItemData const* list = npc->GetVendorItems();
                std::uint64_t const m0 = bot->GetMoney();
                std::uint32_t const before = LooseCount(bot, kPouch);
                for (uint32 i = 0; list && i < list->GetItemCount(); ++i)
                    if (VendorItem const* vi = list->GetItem(i); vi && vi->item == kPouch && !vi->ExtendedCost)
                    {
                        bot->BuyItemFromVendorSlot(npc->GetGUID(), i, kPouch, 1, NULL_BAG, NULL_SLOT);
                        break;
                    }
                if (LooseCount(bot, kPouch) > before)
                {
                    WearBag(bot, kPouch);
                    EmitOutfit(bot, Reason::Outfit, kPouch, m0 > bot->GetMoney() ? m0 - bot->GetMoney() : 0);
                }
                else
                    EmitOutfit(bot, Reason::Refused, kPouch, PriceOf(kPouch), "outfit");
                LOG_INFO("playerbots", "[Supply] bot={} bag item={} worn={} free={} money={}", bot->GetName(), kPouch,
                         BagWorn(bot), bot->GetFreeInventorySpace(), bot->GetMoney());
                break;
            }
            case Task::Bank:
            {
                // The bank window: the stock auto-store handler moves each stack between bags and bank (CanUseBank
                // checks this banker; CanBankItem / CanStoreItem the room). A stack merged into another is gone.
                Creature* npc = target->ToCreature();
                bot->GetSession()->SendShowBank(npc->GetGUID());
                std::uint32_t deposited = 0, withdrawn = 0;
                for (std::uint32_t const g : BankMoves(bot, lined ? &L : nullptr, role.bagHouse, repGear))
                {
                    ObjectGuid const og = ObjectGuid::Create<HighGuid::Item>(g);
                    Item* item = bot->GetItemByGuid(og);
                    if (!item)
                        continue;
                    bool const fromBank = Player::IsBankPos(item->GetPos());
                    std::uint32_t const entry = item->GetEntry(), count = item->GetCount();
                    WorldPacket packet(CMSG_AUTOSTORE_BANK_ITEM, 2);
                    packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot());
                    WorldPackets::Bank::AutoStoreBankItem store(std::move(packet));
                    store.Read();
                    bot->GetSession()->HandleAutoStoreBankItemOpcode(store);
                    Item const* after = bot->GetItemByGuid(og);
                    if (after && Player::IsBankPos(after->GetPos()) == fromBank)
                        break;  // no room on the other side
                    if (fromBank)
                        ++withdrawn;
                    else
                        ++deposited;
                    LOG_INFO("playerbots", "[Supply] bank bot={} op={} item={} count={}", bot->GetName(),
                             fromBank ? "withdraw" : "deposit", entry, count);
                }
                LOG_INFO("playerbots", "[Supply] bank bot={} deposited={} withdrawn={} free={}", bot->GetName(), deposited,
                         withdrawn, bot->GetFreeInventorySpace());
                break;
            }
            case Task::Market:
            {
                // Faction AH only: the capital auctioneer near home. The neutral goblin houses (Booty Bay,
                // Gadgetzan) are the future cross-faction / smuggling channel, never used here; with two-side
                // auction interaction on, every house is neutral and the market stays shut.
                Creature* npc = target->ToCreature();
                AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(npc->GetFaction());
                AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(npc->GetFaction());
                if (!house || !ah || house->houseId == uint32(AuctionHouseId::Neutral))
                {
                    LOG_INFO("playerbots", "[Supply] bot={} market skipped: auctioneer {} is not a faction house",
                             bot->GetName(), npc->GetEntry());
                    break;
                }
                // A line rep only buys (its surplus goes through the Auction / Sell trips). Its own listings of a
                // wanted item come back first (PlanMarketCancels; the buyouts skip them).
                std::vector<MarketWant> wants = lined ? lview.buy : view.buy;
                std::vector<AutoWowTrade::Post> planned;
                if (std::vector<std::uint32_t> const sell = lined ? std::vector<std::uint32_t>{} : MarketSellable(bot, view);
                    !sell.empty())
                    AutoWowTrade::PostStacks(bot, npc, sell, &planned);
                for (AutoWowTrade::Post const& post : planned)
                    Emit(bot, Reason::List, 0, post.entry, post.count, post.buyout, guid, 0);
                std::vector<MarketListing> listings, own;
                if (!wants.empty())
                    for (auto const& [id, a] : ah->GetAuctions())
                        if (a && a->itemCount && std::any_of(wants.begin(), wants.end(), [&](MarketWant const& w)
                                                             { return w.item == a->item_template; }))
                        {
                            if (a->owner != bot->GetGUID() && a->buyout)
                                listings.push_back({id, a->item_template, a->itemCount, a->buyout});
                            else if (a->owner == bot->GetGUID() && !a->bidder)
                                own.push_back({id, a->item_template, a->itemCount, a->buyout});
                        }
                std::vector<MarketListing> cancels = PlanMarketCancels(std::move(own), wants);
                std::vector<MarketListing> buys = PlanMarketBuys(std::move(listings), wants,
                                                                 detail::gParams.buyMaxPct, detail::gParams.buyBudget);
                LOG_INFO("playerbots", "[Supply] bot={} market ah={} listed={} wants={} buys={} cancels={}",
                         bot->GetName(), house->houseId, planned.size(), wants.size(), buys.size(), cancels.size());
                QueueMarketCancels(bot, npc->GetGUID().GetRawValue(), std::move(cancels));
                QueueMarketBuys(bot, npc->GetGUID().GetRawValue(), std::move(buys));
                break;
            }
            default:
                break;
        }
        s.task = Task::None;
        s.nextMs = std::min(s.nextMs, now + 2000);  // the next trip (e.g. mailbox -> trainer) soon after
        StoreRole(guid, s);
        return true;
    }

    // At home, nothing to fetch: the artisan crafts (one cast at a time; the core consumes the reagents and
    // rolls the skill-up). The product needs room first (a free slot or a partial stack), else the next decision
    // makes room (RoomTarget: a slot at least).
    auto craft = [&](std::uint32_t spell, std::uint32_t item) -> bool
    {
        if (!spell || !bot->HasSpell(spell) || !botAI->CanCastSpell(spell, bot, true))
            return false;
        ItemPosCountVec dest;
        bool const room = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, 1) == EQUIP_ERR_OK;
        if (room == s.craftBlocked)
        {
            LOG_INFO("playerbots", "[Supply] bot={} craft item={} {} free={}", bot->GetName(), item,
                     room ? "has room again" : "blocked: no bag room", bot->GetFreeInventorySpace());
            s.craftBlocked = !room;
            if (!room)
                s.nextMs = std::min(s.nextMs, now);  // make room at once
        }
        if (!room)
            return false;
        std::uint32_t const before = LooseCount(bot, item);
        if (!botAI->CastSpell(spell, bot))
            return false;
        s.castSpell = spell;
        s.castItem = item;
        s.castBefore = before;
        return true;
    };
    // Gear line: the target order entry's piece while the view wants more than the finished ones in hand; NextCast makes
    // a short intermediate (a bolt, Medium Leather) first. False = nothing cast.
    auto gearCraft = [&]() -> bool
    {
        std::uint8_t const r = gview.product;
        if (!geared || r >= gtab.tierCount || gview.remaining <= LooseCount(bot, gtab.tiers[r].product) ||
            !bot->HasSpell(gtab.tiers[r].spell))
            return false;
        std::uint8_t const c = NextCast(gtab, r, [&](std::uint32_t item) { return LooseCount(bot, item); });
        if (c == kNoTier || !craft(gtab.tiers[c].spell, gtab.tiers[c].product))
            return false;
        s.castLine = static_cast<std::uint8_t>(gearId);
        return true;
    };
    // An open gear order the artisan works (DemandOnly: no consumer-less skill-up eats its reagents meanwhile).
    bool const gearOpen = geared && gview.product != kNoTier && gview.remaining;
    if (artisan && atHome && Tiers())
    {
        // Tiers: the product order first, else the skill-up recipe.
        Tier const* product = view.product != kNoTier ? &kTiers[view.product] : nullptr;
        Tier const* skillup = view.skillup != kNoTier ? &kTiers[view.skillup] : nullptr;
        bool const canBag = product && bot->HasSpell(product->bagSpell);
        auto hand = [&](Tier const* t)
        {
            return t ? Hand{LooseCount(bot, t->cloth), LooseCount(bot, t->bolt), LooseCount(bot, t->thread),
                            t->extra ? LooseCount(bot, t->extra) : 0, LooseCount(bot, t->bag)}
                     : Hand{};
        };
        std::uint32_t const remaining = canBag ? view.remaining : 0;
        Craft const c = NextTierCraft(product ? *product : kTiers[0], remaining, canBag, hand(product), skillup,
                                      view.skillupBag, hand(skillup));
        Tier const* tier = remaining ? product : skillup;
        std::uint32_t const spell = !tier ? 0 : c == Craft::Bag ? tier->bagSpell : c == Craft::Bolt ? tier->boltSpell : 0;
        std::uint32_t const item = !tier ? 0 : c == Craft::Bag ? tier->bag : tier->bolt;
        // The bag order first, then the gear order, then the skill-up (DemandOnly: none while a gear order is open).
        if ((remaining && c != Craft::None) || !gearCraft())
            if (remaining || !gearOpen || !DemandOnly())
                craft(spell, item);
    }
    else if (lined && role.role == Role::Artisan && atHome)
    {
        // Line artisan: the order's product while it wants more than the finished units in hand, else the
        // skill-up recipe; NextCast crafts a short Craft reagent (a lower tier) first.
        std::uint8_t const product = lview.product;
        bool const order = product != kNoTier && lview.remaining > LooseCount(bot, L.tiers[product].product) &&
                           bot->HasSpell(L.tiers[product].spell);
        std::uint8_t const goal = order ? product : lview.skillup;
        std::uint8_t const c =
            goal == kNoTier ? kNoTier : NextCast(L, goal, [&](std::uint32_t item) { return LooseCount(bot, item); });
        if (c != kNoTier)
            craft(L.tiers[c].spell, L.tiers[c].product);
    }
    else if (artisan && atHome)
    {
        bool const canBag = bot->HasSpell(BagSpell());
        Recipe const& r = BagRecipe();
        Craft const c = NextCraft(r, canBag ? view.remaining : 0, canBag, LooseCount(bot, kLinen),
                                  LooseCount(bot, BoltItem()), LooseCount(bot, ThreadItem()), LooseCount(bot, BagItem()));
        std::uint32_t const spell = c == Craft::Bag ? BagSpell() : c == Craft::Bolt ? BoltSpell() : 0;
        std::uint32_t const item = c == Craft::Bag ? BagItem() : BoltItem();
        if (c == Craft::Bag || !gearCraft())  // a bag order's cast first, the gear order before a bolt
            craft(spell, item);
    }
    else if (geared && atHome)
        gearCraft();
    StoreRole(guid, s);
    return true;
}
