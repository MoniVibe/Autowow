/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Supply-chain throughput steps (lane U; policy: AutoWow/TradePolicy.h, AutoWow/SupplyPolicy.h). Map thread.
//  - MailPickupStep (AutoWow.Supply.MailPickup): a cohort member with mail it can take walks to a mailbox within
//    MailPickupYards and collects it (not only on errand runs; soak-s49-full-r1: members met their bag mail only on
//    town runs).
//  - MarketSellerStep (AutoWow.Market.RandomSellers / MailOrders): a random bot lists its own `ah` loot at a faction
//    auctioneer within SellerYards, and at a mailbox fills its team's rep mail orders (COD) and takes its own mail
//    (COD payments, expired listings). Real items from its own bags, real gold both ways; SellerCapPerHour.

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowGuildsPolicy.h"
#include "AutoWowOracleRuntime.h"
#include "Bag.h"
#include "Creature.h"
#include "ErrandsPolicy.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SupplyPolicy.h"
#include "TradePolicy.h"

namespace
{
constexpr std::uint64_t kPickupScanMs = 10000;     // a member checks its mail / the nearest mailbox this often
constexpr std::uint64_t kTargetTimeoutMs = 60000;  // a walk that has not arrived is dropped
constexpr std::uint64_t kBackoffMs = 300000;       // after a dropped walk or a visit that did nothing
constexpr float kNearYards = 40.0f;                // direct move this close, the no-teleport mover beyond

enum class Target : std::uint8_t
{
    None = 0,
    Auctioneer = 1,
    Mailbox = 2
};

// Per bot (map threads; only bots that pass the population gate with a flag on are stored: bounded by the cohort
// plus the random-bot pool).
struct MarketState
{
    std::uint64_t nextScanMs = 0;
    Target kind = Target::None;
    ObjectGuid target;
    std::uint64_t sinceMs = 0;
    AutoWowTrade::SellerWindow window;
};

std::mutex gMarketLock;
std::unordered_map<std::uint32_t, MarketState> gMarket;

MarketState LoadMarket(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gMarketLock);
    auto const it = gMarket.find(guid);
    return it == gMarket.end() ? MarketState{} : it->second;
}

void StoreMarket(std::uint32_t guid, MarketState const& s)
{
    std::lock_guard<std::mutex> guard(gMarketLock);
    gMarket[guid] = s;
}

// Could list anything at all (TradePolicy Postable without the usage lookup): skips a walk for bags of greys.
bool HasPostableLoot(Player* bot)
{
    auto postable = [](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || !proto->SellPrice || item->IsSoulBound() || proto->Bonding == BIND_WHEN_PICKED_UP)
            return false;
        AutoWowTrade::Holding h;
        h.count = item->GetCount();
        h.quality = proto->Quality;
        h.itemClass = proto->Class;
        h.sellPrice = proto->SellPrice;
        h.usageAh = true;
        return AutoWowTrade::Postable(h);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (postable(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            return true;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                if (postable(b->GetItemByPos(slot)))
                    return true;
    return false;
}

std::uint64_t NowMs() { return static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count())); }
}  // namespace

// The walk to the state's target and the visit on arrival (`visit` returns whether it did anything; nothing backs
// the next scan off). True while the tick is consumed.
template <typename W, typename F>
static bool MarketTargetTick(W&& walkLeg, Player* bot, PlayerbotAI* botAI, std::uint32_t guid, MarketState& s,
                             std::uint64_t now, F&& visit)
{
    WorldObject* obj = s.target.IsEmpty() ? nullptr : ObjectAccessor::GetWorldObject(*bot, s.target);
    if (!obj || !obj->IsInWorld() || now - s.sinceMs > kTargetTimeoutMs)
    {
        LOG_INFO("playerbots", "[Market] bot={} drop target kind={} (gone / timeout)", bot->GetName(), uint32(s.kind));
        s.kind = Target::None;
        s.target.Clear();
        s.nextScanMs = now + kBackoffMs;
        StoreMarket(guid, s);
        return false;
    }
    if (bot->IsWithinDistInMap(obj, INTERACTION_DISTANCE - 0.5f))
    {
        bot->StopMoving();
        bot->SetFacingToObject(obj);
        if (!visit(obj))
            s.nextScanMs = now + kBackoffMs;
        s.kind = Target::None;
        s.target.Clear();
        StoreMarket(guid, s);
        return true;
    }
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE)
        botAI->rpgInfo.ChangeToIdle();
    if (bot->GetExactDist2d(obj) < kNearYards)
    {
        if (!bot->isMoving())
            bot->GetMotionMaster()->MovePoint(0, obj->GetPositionX(), obj->GetPositionY(), obj->GetPositionZ());
    }
    else
        walkLeg(WorldPosition(bot->GetMapId(), obj->GetPositionX(), obj->GetPositionY(), obj->GetPositionZ()));
    StoreMarket(guid, s);
    return true;
}

bool NewRpgBaseAction::MailPickupStep()
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!botAI->IsAutoWowIndependentParty() || AutoWowOracleRuntime::IsManagedBot(guid) || !bot->IsAlive() ||
        bot->IsInCombat() || bot->IsInFlight() || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        bot->GetTransport() || AutoWowSupply::ActiveRoleOf(bot).role != AutoWowSupply::Role::None ||
        (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)))
        return false;  // role bots have their own mailbox task; an errand run has its own mail stop
    NewRpgInfo& info = botAI->rpgInfo;
    if (info.GetStatus() == RPG_TRAVEL_FLIGHT)
        return false;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        if (quest->objectiveRuntime.phase == QuestActionPhase::EscortEvent)
            return false;
    std::uint64_t const now = NowMs();
    MarketState s = LoadMarket(guid);
    if (s.kind == Target::None)
    {
        if (now < s.nextScanMs)
            return false;
        s.nextScanMs = now + kPickupScanMs;
        GameObject* box = AutoWowTrade::HasCollectableMailWithRoom(bot)
            ? bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, float(AutoWowSupply::detail::gParams.mailPickupYards))
            : nullptr;
        if (!box)
        {
            StoreMarket(guid, s);
            return false;
        }
        s.kind = Target::Mailbox;
        s.target = box->GetGUID();
        s.sinceMs = now;
        LOG_INFO("playerbots", "[Market] pickup bot={} mailbox={} dist={} supply_mail={}", bot->GetName(),
                 box->GetEntry(), uint32(bot->GetExactDist2d(box)), AutoWowSupply::HasSupplyMail(bot));
    }
    return MarketTargetTick([this](WorldPosition const& d) { WalkLeg(d); }, bot, botAI, guid, s, now,
                            [&](WorldObject* obj)
                            {
                                AutoWowTrade::VisitMailbox(bot, obj->ToGameObject());
                                return true;
                            });
}

bool NewRpgBaseAction::MarketSellerStep()
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (botAI->IsAutoWowIndependentParty() || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        AutoWowOracleRuntime::IsManagedBot(guid) ||
        (AutoWowGuilds::Enabled() && AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid)) ||
        AutoWowSupply::RoleOf(guid).role != AutoWowSupply::Role::None || !bot->IsAlive() || bot->IsInCombat() ||
        bot->IsInFlight() || !bot->GetMap() || bot->GetMap()->Instanceable() || bot->GetTransport())
        return false;
    AutoWowTrade::Params const& p = AutoWowTrade::detail::gParams;
    std::uint64_t const now = NowMs(), hour = now / AutoWowTrade::kSellerHourMs;
    MarketState s = LoadMarket(guid);
    std::uint32_t const room = AutoWowTrade::SellerRoom(s.window, hour, p.sellerCapPerHour);
    if (s.kind == Target::None)
    {
        if (now < s.nextScanMs)
            return false;
        s.nextScanMs = now + p.sellerScanMs;
        NewRpgStatus const status = botAI->rpgInfo.GetStatus();
        if (status != RPG_IDLE && status != RPG_WANDER_NPC && status != RPG_WANDER_RANDOM)
        {
            StoreMarket(guid, s);
            return false;  // questing / travelling: the next scan
        }
        float const yards = float(p.sellerYards);
        // AutoWow.Auction.ListLoot also sends a background bot to the auctioneer to list its unneeded BoE gear;
        // AutoWow.Auction.SeedThinSlots sends it even with no loot, to top up thin slots (its own daily cap throttles).
        bool const wantList = (AutoWowTrade::RandomSellers() || AutoWowTrade::ListLoot()) && room && HasPostableLoot(bot);
        if (wantList || AutoWowTrade::SeedThinSlots())
            for (ObjectGuid const& g : AI_VALUE(GuidVector, "possible new rpg targets"))  // nearest first
                if (Creature* c = ObjectAccessor::GetCreature(*bot, g);
                    c && c->IsAlive() && c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) && !c->IsHostileTo(bot) &&
                    bot->GetExactDist2d(c) <= yards)
                {
                    s.kind = Target::Auctioneer;
                    s.target = g;
                    break;
                }
        if (s.kind == Target::None &&
            ((room && AutoWowSupply::HoldsOrderedItem(bot)) || AutoWowTrade::HasCollectableMailWithRoom(bot)))
            if (GameObject* box = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, yards))
            {
                s.kind = Target::Mailbox;
                s.target = box->GetGUID();
            }
        if (s.kind == Target::None)
        {
            StoreMarket(guid, s);
            return false;
        }
        s.sinceMs = now;
        LOG_INFO("playerbots", "[Market] seller bot={} lvl={} target={} room={} money={}", bot->GetName(),
                 bot->GetLevel(), s.kind == Target::Auctioneer ? "auctioneer" : "mailbox", room, bot->GetMoney());
    }
    return MarketTargetTick([this](WorldPosition const& d) { WalkLeg(d); }, bot, botAI, guid, s, now,
                            [&](WorldObject* obj)
                            {
                                std::uint32_t n = 0, seeded = 0;
                                if (s.kind == Target::Auctioneer)
                                {
                                    n = AutoWowTrade::PostLoot(botAI, bot, obj->ToCreature(), room);
                                    // SeedThinSlots keeps its own per-house daily cap; it does not draw the hourly
                                    // seller cap below, so it is not folded into NoteSeller.
                                    seeded = AutoWowTrade::SeedThinSlotsAt(bot, obj->ToCreature());
                                }
                                else
                                {
                                    AutoWowTrade::VisitMailbox(bot, obj->ToGameObject());
                                    n = AutoWowSupply::FillOrders(bot, room);
                                }
                                AutoWowTrade::NoteSeller(s.window, hour, n);
                                return n != 0 || seeded != 0 || s.kind == Target::Mailbox;
                            });
}
