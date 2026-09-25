/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Supply role step (policy: AutoWow/SupplyPolicy.h; world side: AutoWow/AutoWowSupply.cpp). A
// configured rep or artisan never quests, grinds, runs errands or takes contracts: it lives at its capital
// home (mailbox), collects its mail, and the artisan trains, buys thread and crafts. Map thread.

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "Bag.h"
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
    Sell = 6
};

struct RoleState
{
    std::uint32_t version = kStateVersion;
    std::uint64_t nextMs = 0;       // next task decision
    Task task = Task::None;
    std::uint64_t taskSinceMs = 0;
    std::uint32_t stuck = 0;
    std::uint32_t castSpell = 0;    // craft cast issued, output not yet counted
    std::uint32_t castItem = 0;
    std::uint32_t castBefore = 0;
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
    // GetItemCount counts equipped items too; the only equippable entry here is the bag.
    std::uint32_t n = bot->GetItemCount(entry, false);
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
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
std::uint64_t LearnCost(Player* bot, Trainer::Trainer* trainer, bool& affordable)
{
    affordable = false;
    std::uint64_t cost = 0;
    if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
        return 0;
    for (std::uint32_t const id : LearnSpells())
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
        case Task::Sell: return st.threadVendor.entry ? &st.threadVendor : nullptr;
        case Task::Auction: return st.auctioneer.entry ? &st.auctioneer : nullptr;
        default: return nullptr;
    }
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

    // A finished craft cast: count what it made.
    if (s.castSpell)
    {
        std::uint32_t const have = LooseCount(bot, s.castItem);
        if (have > s.castBefore)
            Emit(bot, Reason::Craft, view.orderId, s.castItem, have - s.castBefore, 0, guid, guid);
        s.castSpell = 0;
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

    std::int64_t const dx = std::int64_t(bot->GetPositionX()) - home.x, dy = std::int64_t(bot->GetPositionY()) - home.y;
    bool const atHome = dx * dx + dy * dy <= std::int64_t(p.homeYards) * p.homeYards;

    // Decide the next station trip.
    if (s.task == Task::None && now >= s.nextMs)
    {
        s.nextMs = now + p.tickMs;
        Task next = Task::None;
        if (AutoWowTrade::HasCollectableMail(bot) && st.mailbox.entry)
            next = Task::Mailbox;
        if (artisan)
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
        if (role.role == Role::Rep && next == Task::None && view.surplus)
            next = st.auctioneer.entry ? Task::Auction : Task::Sell;
        if (next == Task::None && !atHome)
            next = Task::Home;
        if (next != Task::None)
        {
            s.task = next;
            s.taskSinceMs = now;
            LOG_INFO("playerbots", "[Supply] bot={} role={} task={} money={} oid={} remaining={} surplus={}",
                     bot->GetName(), role.role == Role::Rep ? "rep" : "artisan", uint32(next), bot->GetMoney(),
                     view.orderId, view.remaining, view.surplus);
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
        Station const* station = StationFor(st, s.task);
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
                for (std::uint32_t const id : LearnSpells())
                    if (trainer)
                        for (Trainer::Spell const& sp : trainer->GetSpells())
                            if (sp.SpellId == id && trainer->CanTeachSpell(bot, &sp) && sp.MoneyCost <= bot->GetMoney())
                                trainer->TeachSpell(npc, bot, id);  // stock path: validity, reputation price, money
                if (m0 > bot->GetMoney())
                    AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Train, m0 - bot->GetMoney());
                LOG_INFO("playerbots", "[Supply] bot={} trained at={} spent={} tailoring={}/{} bag_recipe={}",
                         bot->GetName(), npc->GetEntry(), m0 - bot->GetMoney(), bot->GetSkillValue(SKILL_TAILORING),
                         bot->GetMaxSkillValue(SKILL_TAILORING), bot->HasSpell(BagSpell()));
                break;
            }
            case Task::Thread:
            {
                Creature* npc = target->ToCreature();
                VendorItemData const* list = npc->GetVendorItems();
                std::uint32_t const want = view.remaining * BagRecipe().threadPerBag;
                std::uint32_t const have = LooseCount(bot, ThreadItem());
                std::uint64_t const m0 = bot->GetMoney();
                for (uint32 i = 0; list && i < list->GetItemCount(); ++i)
                    if (VendorItem const* vi = list->GetItem(i); vi && vi->item == ThreadItem() && !vi->ExtendedCost)
                    {
                        for (std::uint32_t n = have; n < want && n < have + 60; ++n)
                        {
                            std::uint32_t const before = LooseCount(bot, ThreadItem());
                            bot->BuyItemFromVendorSlot(npc->GetGUID(), i, ThreadItem(), 1, NULL_BAG, NULL_SLOT);
                            if (LooseCount(bot, ThreadItem()) <= before)
                                break;  // money, bags or stock
                        }
                        break;
                    }
                LOG_INFO("playerbots", "[Supply] bot={} bought thread {}->{} spent={}", bot->GetName(), have,
                         LooseCount(bot, ThreadItem()), m0 - bot->GetMoney());
                break;
            }
            case Task::Auction:
            {
                std::vector<std::uint32_t> const bags = LooseGuids(bot, BagItem(), view.surplus);
                std::uint32_t const posted =
                    bags.empty() ? 0 : AutoWowTrade::PostStacks(bot, target->ToCreature(), bags);
                if (posted)
                {
                    Emit(bot, Reason::Surplus, view.orderId, BagItem(), posted, 0, guid, 0, "auction");
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
                std::uint32_t sold = 0;
                for (std::uint32_t const g : LooseGuids(bot, BagItem(), view.surplus))
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
                if (sold)
                    Emit(bot, Reason::Surplus, view.orderId, BagItem(), sold, bot->GetMoney() - m0, guid, 0, "vendor");
                ClearSurplus(role.alliance);
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
    // rolls the skill-up).
    if (artisan && atHome)
    {
        bool const canBag = bot->HasSpell(BagSpell());
        Recipe const& r = BagRecipe();
        Craft const c = NextCraft(r, canBag ? view.remaining : 0, canBag, LooseCount(bot, kLinen),
                                  LooseCount(bot, BoltItem()), LooseCount(bot, ThreadItem()), LooseCount(bot, BagItem()));
        std::uint32_t const spell = c == Craft::Bag ? BagSpell() : c == Craft::Bolt ? BoltSpell() : 0;
        std::uint32_t const item = c == Craft::Bag ? BagItem() : BoltItem();
        if (spell && bot->HasSpell(spell) && botAI->CanCastSpell(spell, bot, true))
        {
            std::uint32_t const before = LooseCount(bot, item);
            if (botAI->CastSpell(spell, bot))
            {
                s.castSpell = spell;
                s.castItem = item;
                s.castBefore = before;
            }
        }
    }
    StoreRole(guid, s);
    return true;
}
