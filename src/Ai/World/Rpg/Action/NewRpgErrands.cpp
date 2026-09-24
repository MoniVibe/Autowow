/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Errands runtime (policy: AutoWow/ErrandsPolicy.h).

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "ErrandsPolicy.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "MapMgr.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "TradePolicy.h"
#include "TravelNode.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowErrands
{
namespace
{
// Bot AI updates run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;

constexpr uint32 kHearthstoneItem = 6948;
constexpr uint32 kHearthstoneSpell = 8690;
constexpr uint32 kProfessionSkills[] = {SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                        SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
                                        SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING,
                                        SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING};

BotState LoadState(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    return it == gStates.end() ? BotState{} : it->second;
}

void StoreState(std::uint32_t guid, BotState const& s)
{
    std::lock_guard<std::mutex> guard(gLock);
    gStates[guid] = s;
}

// Teams a creature faction does not attack (TravelMgr flight-master idiom).
std::uint8_t TeamsOf(uint32 faction)
{
    FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(faction);
    if (!f)
        return 0;
    std::uint8_t teams = 0;
    if (!(f->hostileMask & FACTION_MASK_ALLIANCE))
        teams |= kAlliance;
    if (!(f->hostileMask & FACTION_MASK_HORDE))
        teams |= kHorde;
    return teams;
}

std::uint8_t TeamOf(Player* bot) { return bot->GetTeamId() == TEAM_ALLIANCE ? kAlliance : kHorde; }

std::int32_t Yd(float v) { return static_cast<std::int32_t>(v); }

Town const* FindTown(std::uint32_t id)
{
    std::vector<Town> const& towns = detail::gTowns;
    auto const it = std::lower_bound(towns.begin(), towns.end(), id,
                                     [](Town const& t, std::uint32_t v) { return t.id < v; });
    return it != towns.end() && it->id == id ? &*it : nullptr;
}

// World init, flag on: every innkeeper / repairer / vendor / flight master / trainer spawn on the
// random-bot maps, clustered into towns (ErrandsPolicy BuildTowns).
void BuildCatalog()
{
    uint32 const start = getMSTime();
    uint32 const roleFlags = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_VENDOR_MASK |
                             UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_TRAINER_CLASS |
                             UNIT_NPC_FLAG_TRAINER_PROFESSION |
                             (AutoWowTrade::Enabled() ? uint32(UNIT_NPC_FLAG_AUCTIONEER) : 0u);
    std::vector<Npc> npcs;
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
        if (!ct)
            continue;
        uint32 npcflag = 0, unitFlags = 0, dynamicFlags = 0;
        ObjectMgr::ChooseCreatureFlags(ct, npcflag, unitFlags, dynamicFlags, &data);  // spawn overrides
        if (!(npcflag & roleFlags))
            continue;
        if (std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), data.mapid) ==
            sPlayerbotAIConfig.randomBotMaps.end())
            continue;
        Npc n;
        n.spawn = guid;
        n.entry = data.id;
        n.map = data.mapid;
        n.x = Yd(data.posX);
        n.y = Yd(data.posY);
        n.z = Yd(data.posZ);
        n.teams = TeamsOf(ct->faction);
        if (!n.teams)
            continue;
        if (npcflag & UNIT_NPC_FLAG_INNKEEPER)
        {
            n.roles |= RoleInn;
            if (Map* map = sMapMgr->FindMap(data.mapid, 0))
                n.zone = map->GetZoneId(PHASEMASK_NORMAL, data.posX, data.posY, data.posZ);
        }
        if (npcflag & UNIT_NPC_FLAG_REPAIR)
            n.roles |= RoleRepair;
        if (npcflag & UNIT_NPC_FLAG_TRAINER_CLASS)
            n.roles |= RoleClassTrainer;
        if (npcflag & UNIT_NPC_FLAG_TRAINER_PROFESSION)
            n.roles |= RoleTradeTrainer;
        if (AutoWowTrade::Enabled() && (npcflag & UNIT_NPC_FLAG_AUCTIONEER))
            n.roles |= RoleAuction;
        if (npcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
        {
            n.roles |= RoleFlight;
            n.nodeAlliance = sObjectMgr->GetNearestTaxiNode(data.posX, data.posY, data.posZ, data.mapid, TEAM_ALLIANCE);
            n.nodeHorde = sObjectMgr->GetNearestTaxiNode(data.posX, data.posY, data.posZ, data.mapid, TEAM_HORDE);
        }
        if (npcflag & UNIT_NPC_FLAG_VENDOR_MASK)
        {
            n.roles |= RoleVendor;
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* vi : list->m_items)
                    for (std::size_t k = 0; vi && !vi->ExtendedCost && k < kKinds; ++k)
                        if (IsTierItem(static_cast<Kind>(k), vi->item))
                        {
                            n.items.push_back(vi->item);
                            n.sells |= 1u << k;
                        }
            std::sort(n.items.begin(), n.items.end());
            n.items.erase(std::unique(n.items.begin(), n.items.end()), n.items.end());
            // AutoWow.Gear.Upgrades: plain-gold white / green weapons and armor (the bot filters by use).
            if (AutoWowGear::Enabled())
                if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                {
                    for (VendorItem const* vi : list->m_items)
                    {
                        ItemTemplate const* proto = vi && !vi->ExtendedCost ? sObjectMgr->GetItemTemplate(vi->item) : nullptr;
                        if (proto && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
                            proto->InventoryType != INVTYPE_NON_EQUIP && proto->Quality <= ITEM_QUALITY_UNCOMMON)
                            n.gear.push_back(vi->item);
                    }
                    std::sort(n.gear.begin(), n.gear.end());
                    n.gear.erase(std::unique(n.gear.begin(), n.gear.end()), n.gear.end());
                }
        }
        npcs.push_back(std::move(n));
    }
    // AutoWow.Trade: mailboxes join the towns too (usable by both teams; a hostile town is never chosen).
    if (AutoWowTrade::Enabled())
        for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        {
            GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!gt || gt->type != GAMEOBJECT_TYPE_MAILBOX || (guid & kGoSpawnBit) ||
                std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(),
                          data.mapid) == sPlayerbotAIConfig.randomBotMaps.end())
                continue;
            Npc n;
            n.spawn = guid | kGoSpawnBit;
            n.entry = data.id;
            n.map = data.mapid;
            n.x = Yd(data.posX);
            n.y = Yd(data.posY);
            n.z = Yd(data.posZ);
            n.teams = kAlliance | kHorde;
            n.roles = RoleMailbox;
            npcs.push_back(std::move(n));
        }
    std::size_t const scanned = npcs.size();
    detail::gTowns = BuildTowns(std::move(npcs), detail::gParams.townRadius);
    LOG_INFO("server.loading", ">> [Errands] {} towns from {} service npcs in {} ms", detail::gTowns.size(), scanned,
             GetMSTimeDiffToNow(start));
}

uint32 ZoneOfArea(uint32 areaId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    return area ? (area->zone ? area->zone : area->ID) : 0;
}

void BagSlots(Player* bot, uint32& used, uint32& total)
{
    used = 0;
    total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* pBag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag));
        if (!pBag)
            continue;
        ItemTemplate const* proto = pBag->GetTemplate();
        if (proto->Class != ITEM_CLASS_CONTAINER || proto->SubClass != ITEM_SUBCLASS_CONTAINER)
            continue;  // quivers / profession bags hold no general loot
        total += pBag->GetBagSize();
        used += pBag->GetBagSize() - pBag->GetFreeSlots();
    }
}

uint32 BagFree(Player* bot)
{
    uint32 used = 0, total = 0;
    BagSlots(bot, used, total);
    return total - std::min(used, total);
}

uint32 EquippedDurabilityPct(Player* bot)
{
    uint64 cur = 0, max = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (uint32 const m = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            {
                max += m;
                cur += item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            }
    return DurabilityPct(cur, max);
}

RangedAmmo AmmoOf(Player* bot)
{
    Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    ItemTemplate const* proto = ranged ? ranged->GetTemplate() : nullptr;
    if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        return AmmoNone;
    if (proto->SubClass == ITEM_SUBCLASS_WEAPON_BOW || proto->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW)
        return AmmoArrows;
    return proto->SubClass == ITEM_SUBCLASS_WEAPON_GUN ? AmmoBullets : AmmoNone;
}

uint32 Stock(Player* bot, Kind kind)
{
    TierSpan const t = TiersOf(kind);
    uint32 n = 0;
    for (std::size_t k = 0; k < t.size; ++k)
        n += bot->GetItemCount(t.data[k].item, false);
    return n;
}

// AutoWow.Survival.KeepConsumables: vendor value (copper) of the grey items the stock "autowow-gray" sell
// mode may sell (trade goods, reagents, recipes and quest items are kept, as it keeps them).
uint64 GreyCopper(Player* bot)
{
    uint64 copper = 0;
    auto add = [&copper](Item const* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->Quality == ITEM_QUALITY_POOR && proto->Class != ITEM_CLASS_TRADE_GOODS &&
            proto->Class != ITEM_CLASS_REAGENT && proto->Class != ITEM_CLASS_RECIPE && proto->Class != ITEM_CLASS_QUEST)
            copper += uint64(proto->SellPrice) * item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag const* pBag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                add(pBag->GetItemByPos(slot));
    return copper;
}

// Friendly living vendor within `yards` among the bot's "nearest npcs" (nearest; lower guid on ties).
Creature* NearestVendor(Player* bot, PlayerbotAI* botAI, uint32 yards)
{
    Creature* best = nullptr;
    float bestDist = 0.0f;
    for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get())
    {
        Creature* c = botAI->GetCreature(guid);
        if (!c || !c->IsAlive() || !c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) || bot->IsHostileTo(c))
            continue;
        float const d = bot->GetExactDist2d(c);
        if (d > float(yards))
            continue;
        if (!best || d < bestDist || (d == bestDist && c->GetGUID().GetCounter() < best->GetGUID().GetCounter()))
        {
            best = c;
            bestDist = d;
        }
    }
    return best;
}

// AutoWow.Survival.KeepConsumables restock at one vendor: two passes over the kinds (PassTarget: to Low,
// then to Target; the class-trainer reserve only in pass 1, so food comes before training). Purchases are
// counted by the stock change: the core BuyItemFromVendorSlot returns `maxcount != 0` (false after a
// successful buy from an unlimited slot), which stopped the plain loop after its first pack.
void KeepBuy(Player* bot, Creature* npc, Stop const& st, BotState& s, Params const& p)
{
    VendorItemData const* list = npc->GetVendorItems();
    uint64 const reserve = (s.needs & NeedClassTrain) ? ClassTrainBudgetCopper(bot->GetLevel()) : 0;
    for (std::uint32_t pass = 0; list && pass < 2; ++pass)
        for (std::size_t k = 0; k < kKinds; ++k)
        {
            uint32 const item = s.buyItems[k];
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!(st.buyKinds & (1u << k)) || !proto)
                continue;
            uint32 slot = list->GetItemCount();
            for (uint32 i = 0; i < list->GetItemCount(); ++i)
                if (VendorItem const* vi = list->GetItem(i); vi && vi->item == item && !vi->ExtendedCost)
                {
                    slot = i;
                    break;
                }
            Kind const kind = static_cast<Kind>(k);
            uint32 const have = Stock(bot, kind);
            uint32 const target = PassTarget(p, kind, pass);
            if (slot == list->GetItemCount() || have >= target)
                continue;
            uint64 const money = bot->GetMoney();
            uint64 const keep = pass ? reserve : 0;
            PackBuy const b = PacksToBuy(target - have, proto->BuyCount, proto->BuyPrice, money > keep ? money - keep : 0);
            uint32 bought = 0;
            while (bought < b.packs)
            {
                uint32 const before = bot->GetItemCount(item, false);
                bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
                if (bot->GetItemCount(item, false) <= before)
                    break;  // bags full, out of money after the reputation price, vendor stock
                ++bought;
            }
            if (money > bot->GetMoney())
                s.spent += money - bot->GetMoney();
            if (bought)
                s.done |= DoneRestocked;
            if (bought < b.packs || (pass && b.shortOfMoney))
            {
                s.done |= DoneSkipped;
                LOG_INFO("playerbots", "[Errands] bot={} skip item={} pass={} bought={}/{} short_of_money={} money={}",
                         bot->GetName(), item, pass, bought, b.packs, b.shortOfMoney, bot->GetMoney());
            }
        }
}

bool HearthReady(Player* bot)
{
    return bot->HasItemCount(kHearthstoneItem, 1, false) && !bot->HasSpellCooldown(kHearthstoneSpell);
}

bool HearthBoundAt(Player* bot, Town const& t)
{
    std::int64_t const r = detail::gParams.townRadius;
    return bot->m_homebindMapId == t.map && Dist2(Yd(bot->m_homebindX), Yd(bot->m_homebindY), t.x, t.y) <= r * r;
}

Npc const* TownFlightMaster(Town const& t, std::uint8_t team)
{
    for (Npc const& n : t.npcs)
        if ((n.roles & RoleFlight) && (n.teams & team))
            return &n;
    return nullptr;
}

uint32 NodeFor(Npc const& fm, std::uint8_t team) { return team == kAlliance ? fm.nodeAlliance : fm.nodeHorde; }

// Taxi path from the nearest flight master on the bot's map to a node the bot knows.
bool FlightTo(Player* bot, uint32 node, TravelMgr::FlightMasterInfo const*& fm, std::vector<uint32>& path)
{
    fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!node || !fm || !fm->taxiNodeId || fm->taxiNodeId == node || !bot->m_taxi.IsTaximaskNodeKnown(node))
        return false;
    path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, node);
    return !path.empty();
}

// Class trainer valid for the bot, or (profession-planned bot only) a tradeskill trainer, with at least
// one spell it can teach now. `unaffordable`: a teachable spell costs more than `money`.
bool TrainerHasWork(Player* bot, uint32 entry, uint64 money, bool& unaffordable)
{
    unaffordable = false;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
    if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
        return false;
    if (trainer->GetTrainerType() == Trainer::Type::Tradeskill)
    {
        if (!sPlayerbotAIConfig.GetAutoWowProfessionPlan(bot->GetGUID().GetCounter()))
            return false;  // the stock trainer action has no learn filter without a plan
    }
    else if (trainer->GetTrainerType() != Trainer::Type::Class)
        return false;
    bool work = false;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (!trainer->CanTeachSpell(bot, &spell))
            continue;
        if (spell.MoneyCost <= money)
            work = true;
        else
            unaffordable = true;
    }
    return work;
}

TownFacts FactsOf(Player* bot, Town const& t, std::uint8_t team)
{
    TownFacts f;
    f.sells = t.sells;
    f.inBotZone = t.zone == bot->GetZoneId();
    uint64 const money = bot->GetMoney();
    for (Npc const& n : t.npcs)
    {
        if (!(n.teams & team) || !(n.roles & (RoleClassTrainer | RoleTradeTrainer)))
            continue;
        bool unaffordable = false;
        if (!TrainerHasWork(bot, n.entry, money, unaffordable))
            continue;
        f.classTrainer |= (n.roles & RoleClassTrainer) != 0;
        f.tradeTrainer |= (n.roles & RoleTradeTrainer) != 0;
    }
    if (Npc const* fm = TownFlightMaster(t, team))
        f.unknownFlightMaster = !bot->m_taxi.IsTaximaskNodeKnown(NodeFor(*fm, team));
    if (AutoWowGear::Enabled())
        f.gearVendor = std::any_of(t.npcs.begin(), t.npcs.end(),
                                   [team](Npc const& n) { return (n.teams & team) && !n.gear.empty(); });
    return f;
}

// ---- AutoWow.Gear.Upgrades ---------------------------------------------------------------------------
uint32 TemplateDpsMilli(ItemTemplate const* proto)
{
    if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        return 0;
    // ponytail: template damage truncated to whole points (1.0-2.0 starter daggers stay 1-2); enough to rank.
    return AutoWowGear::DpsMilli(static_cast<uint32>(std::max(0.0f, proto->Damage[0].DamageMin)),
                                 static_cast<uint32>(std::max(0.0f, proto->Damage[0].DamageMax)), proto->Delay);
}

uint32 EquippedDpsMilli(Player* bot, uint8 slot)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    return item ? TemplateDpsMilli(item->GetTemplate()) : 0;
}

// Empty equipment slots vendor armor may fill (no jewellery / trinkets / shirt / tabard: vendors sell none
// worth it). The off hand counts only for a bot that does not dual wield and holds no two-hander.
uint32 EmptyArmorSlots(Player* bot)
{
    static constexpr uint8 kSlots[] = {EQUIPMENT_SLOT_HEAD,  EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,
                                       EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS,      EQUIPMENT_SLOT_FEET,
                                       EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS,    EQUIPMENT_SLOT_BACK};
    uint32 mask = 0;
    for (uint8 slot : kSlots)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            mask |= 1u << slot;
    Item* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    bool const twoHander = mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
    if (!bot->CanDualWield() && !twoHander && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
        mask |= 1u << EQUIPMENT_SLOT_OFFHAND;
    return mask;
}

// The town's gear the stock "item upgrade" value rates an equip for this bot (class / spec weights, armor
// type, proficiency, level), as AutoWowGear offers. Ranged slots are not shopped.
std::vector<AutoWowGear::Offer> GearOffers(Player* bot, PlayerbotAI* botAI, Town const& town, std::uint8_t team)
{
    std::vector<AutoWowGear::Offer> offers;
    bool const dualWield = bot->CanDualWield();
    for (Npc const& n : town.npcs)
    {
        if (!(n.teams & team))
            continue;
        for (uint32 const item : n.gear)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto || proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            ItemUsage const usage =
                botAI->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", std::to_string(item))->Get();
            if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                continue;
            AutoWowGear::Offer o;
            o.item = item;
            o.npc = n.spawn;
            o.price = proto->BuyPrice;
            o.armor = proto->Armor;
            if (proto->Class == ITEM_CLASS_WEAPON)
            {
                o.weapon = true;
                o.dpsMilli = TemplateDpsMilli(proto);
                o.twoHand = proto->InventoryType == INVTYPE_2HWEAPON;
                bool const main = proto->InventoryType == INVTYPE_WEAPON ||
                                  proto->InventoryType == INVTYPE_WEAPONMAINHAND || o.twoHand;
                bool const off = dualWield && (proto->InventoryType == INVTYPE_WEAPON ||
                                               proto->InventoryType == INVTYPE_WEAPONOFFHAND);
                if (main)
                {
                    o.slot = EQUIPMENT_SLOT_MAINHAND;
                    offers.push_back(o);
                }
                if (off)
                {
                    o.slot = EQUIPMENT_SLOT_OFFHAND;
                    offers.push_back(o);
                }
                continue;
            }
            uint8 const slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot >= EQUIPMENT_SLOT_END)
                continue;
            o.slot = slot;
            offers.push_back(o);
        }
    }
    return offers;
}

// Arrival with NeedGear: the run's shopping list (AutoWowGear ShoppingList) into the state, its vendors
// into the plan. The level is spent either way (one shopping per level).
void PlanGear(Player* bot, PlayerbotAI* botAI, Town const& town, std::uint8_t team, BotState& s, PlanInput& in)
{
    s.lastGearLevel = bot->GetLevel();
    s.gearItems.fill(0);
    s.gearNpcs.fill(0);
    std::vector<AutoWowGear::Offer> const list = AutoWowGear::ShoppingList(
        AutoWowGear::Get(), GearOffers(bot, botAI, town, team), EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND),
        EquippedDpsMilli(bot, EQUIPMENT_SLOT_OFFHAND), bot->CanDualWield(), EmptyArmorSlots(bot),
        AutoWowGear::Spendable(bot->GetMoney(), bot->GetLevel()));
    for (std::size_t k = 0; k < list.size() && k < AutoWowGear::kMaxPicks; ++k)
    {
        s.gearItems[k] = list[k].item;
        s.gearNpcs[k] = list[k].npc;
        in.gearNpcs.push_back(list[k].npc);
        LOG_INFO("playerbots", "[Gear] bot={} plan item={} npc={} slot={} price={} dps_milli={} armor={} money={}",
                 bot->GetName(), list[k].item, list[k].npc, static_cast<uint32>(list[k].slot), list[k].price,
                 list[k].dpsMilli, list[k].armor, bot->GetMoney());
    }
    std::sort(in.gearNpcs.begin(), in.gearNpcs.end());
    in.gearNpcs.erase(std::unique(in.gearNpcs.begin(), in.gearNpcs.end()), in.gearNpcs.end());
}

LegInput TownLeg(Player* bot, Town const& t, std::uint8_t team)
{
    LegInput in;
    std::int32_t const bx = Yd(bot->GetPositionX()), by = Yd(bot->GetPositionY());
    in.sameMap = bot->GetMapId() == t.map;
    in.walkYards = Yards(bx, by, t.x, t.y);
    in.hearthHere = HearthBoundAt(bot, t);
    in.hearthReady = HearthReady(bot);
    if (Npc const* dest = TownFlightMaster(t, team))
    {
        TravelMgr::FlightMasterInfo const* fm = nullptr;
        std::vector<uint32> path;
        if (FlightTo(bot, NodeFor(*dest, team), fm, path))
        {
            std::int32_t const fx = Yd(fm->pos.GetPositionX()), fy = Yd(fm->pos.GetPositionY());
            in.flight = true;
            in.fmYards = Yards(bx, by, fx, fy);
            in.flyYards = Yards(fx, fy, dest->x, dest->y);
            in.tailYards = Yards(dest->x, dest->y, t.x, t.y);
        }
    }
    return in;
}

// Known taxi node of the bot's team nearest (x, y) on `map`, other than `exclude`. 0 = none.
uint32 NearestKnownNode(Player* bot, uint32 map, std::int32_t x, std::int32_t y, uint32 exclude)
{
    uint32 best = 0;
    std::int64_t bestD2 = 0;
    std::uint32_t const teamIdx = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;  // MountCreatureID[0] horde, [1] alliance
    for (uint32 id = 1; id < sTaxiNodesStore.GetNumRows(); ++id)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(id);
        if (!node || node->map_id != map || id == exclude || !node->MountCreatureID[teamIdx] ||
            !bot->m_taxi.IsTaximaskNodeKnown(id))
            continue;
        std::int64_t const d2 = Dist2(Yd(node->x), Yd(node->y), x, y);
        if (!best || d2 < bestD2)
        {
            best = id;
            bestD2 = d2;
        }
    }
    return best;
}

// Needs of the bot now (ErrandsPolicy Assess).
Assessment AssessBot(Player* bot, BotState const& s)
{
    Params const& p = detail::gParams;
    Obs o;
    uint32 used = 0, total = 0;
    BagSlots(bot, used, total);
    o.bagUsedPct = BagUsedPct(used, total);
    o.durabilityPct = EquippedDurabilityPct(bot);
    o.cls = bot->getClass();
    o.level = bot->GetLevel();
    o.ammo = AmmoOf(bot);
    uint64 const money = bot->GetMoney();
    uint64 restockCopper = 0;
    uint64 onePackCopper = 0;  // AutoWow.Survival.KeepConsumables only
    std::uint32_t const kinds = KindsFor(o.cls, o.level, o.ammo);
    for (std::size_t k = 0; k < kKinds; ++k)
    {
        if (!(kinds & (1u << k)))
            continue;
        Kind const kind = static_cast<Kind>(k);
        o.have[k] = Stock(bot, kind);
        if (o.have[k] >= LowOf(p, kind))
            continue;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(BestTier(kind, o.level, nullptr)))
        {
            restockCopper += uint64(TargetOf(p, kind) - o.have[k]) * proto->BuyPrice /
                             std::max<uint32>(1, proto->BuyCount);
            if (p.keepConsumables)
                onePackCopper += proto->BuyPrice;
        }
    }
    o.restockAffordable = money * 4 >= restockCopper;
    if (p.keepConsumables)
        o.restockAffordable = KeepAffordable(money, GreyCopper(bot), onePackCopper);
    o.classTrainDue = ClassTrainDue(o.level, s.lastClassTrainLevel, money);
    if (sPlayerbotAIConfig.GetAutoWowProfessionPlan(bot->GetGUID().GetCounter()))
        for (uint32 skill : kProfessionSkills)
            if (bot->HasSkill(skill) &&
                ProfessionRankDue(bot->GetPureSkillValue(skill), bot->GetPureMaxSkillValue(skill), o.level))
                o.profTrainDue = true;
    o.hearthElsewhere = ZoneOfArea(bot->m_homebindAreaId) != bot->GetZoneId();
    TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    o.unknownFlightPath =
        fm && fm->taxiNodeId && fm->zoneId == bot->GetZoneId() && !bot->m_taxi.IsTaximaskNodeKnown(fm->taxiNodeId);
    if (AutoWowGear::Enabled())
        o.gear = AutoWowGear::GearDue(AutoWowGear::Get(), o.level, s.lastGearLevel, money,
                                      EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND));
    return Assess(p, o);
}

// Cheapest reachable friendly town: the nearest CandidateTowns on the bot's map by straight line, plus
// the hearth town. Zones far above the bot's level are skipped unless reached by hearthstone.
Town const* ChooseTown(Player* bot, std::uint8_t team, Leg& leg)
{
    Params const& p = detail::gParams;
    std::int32_t const bx = Yd(bot->GetPositionX()), by = Yd(bot->GetPositionY());
    std::vector<std::pair<std::int64_t, Town const*>> nearby;
    Town const* hearthTown = nullptr;
    for (Town const& t : detail::gTowns)
    {
        if (!(t.teams & team) || t.map != bot->GetMapId())
            continue;
        nearby.emplace_back(Dist2(bx, by, t.x, t.y), &t);
        if (!hearthTown && HearthBoundAt(bot, t))
            hearthTown = &t;
    }
    // ponytail: straight-line prefilter; a town behind a mountain can lose to a slightly farther one.
    std::size_t const keep = std::min<std::size_t>(nearby.size(), p.candidateTowns);
    std::partial_sort(nearby.begin(), nearby.begin() + keep, nearby.end(), [](auto const& a, auto const& b)
                      { return a.first != b.first ? a.first < b.first : a.second->id < b.second->id; });
    // AutoWow.Errands.AuctionDetourMs: the nearest auction town joins the candidates even when the
    // straight-line prefilter dropped it (capitals sit farther than the village inns).
    bool const detour = p.auctionDetourMs && AutoWowTrade::Enabled();
    auto const hasAuction = [team](Town const& t)
    {
        return std::any_of(t.npcs.begin(), t.npcs.end(),
                           [team](Npc const& n) { return (n.teams & team) && (n.roles & RoleAuction); });
    };
    std::pair<std::int64_t, Town const*> auctionTown{0, nullptr};
    if (detour)
        for (std::size_t i = keep; i < nearby.size(); ++i)
            if ((!auctionTown.second || nearby[i].first < auctionTown.first) && hasAuction(*nearby[i].second))
                auctionTown = nearby[i];
    nearby.resize(keep);
    if (auctionTown.second)
        nearby.push_back(auctionTown);
    if (hearthTown && std::none_of(nearby.begin(), nearby.end(), [&](auto const& e) { return e.second == hearthTown; }))
        nearby.emplace_back(0, hearthTown);
    std::uint32_t const level = bot->GetLevel();
    std::vector<Candidate> cands;
    for (auto const& [d2, t] : nearby)
    {
        LegInput const in = TownLeg(bot, *t, team);
        Leg const l = ChooseLeg(p, in);
        auto const bracket = sPlayerbotAIConfig.zoneBrackets.find(t->zone);
        std::uint32_t const low = bracket == sPlayerbotAIConfig.zoneBrackets.end() ? 0 : bracket->second.first;
        if (l != Leg::Hearth && ZoneTooHigh(p, level, low))
            continue;
        std::uint32_t const cost = LegCostMs(p, l, in);
        cands.push_back({t->id, l, detour ? DetourCostMs(cost, hasAuction(*t), p.auctionDetourMs) : cost});
    }
    Candidate const* best = PickTown(cands);
    if (!best)
        return nullptr;
    leg = best->leg;
    return FindTown(best->town);
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Errands.Enable", false);
    Params& p = detail::gParams;
    p.checkIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.CheckIntervalMs", 60000);
    p.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.CooldownMs", 900000);
    p.bagSoftPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.BagSoftPct", 70);
    p.bagUrgentPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.BagUrgentPct", 90);
    p.durabilitySoftPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.DurabilitySoftPct", 50);
    p.durabilityUrgentPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.DurabilityUrgentPct", 25);
    p.foodLow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.FoodLow", 5);
    p.foodTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.FoodTarget", 20);
    p.waterLow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.WaterLow", 5);
    p.waterTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.WaterTarget", 20);
    p.ammoLow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.AmmoLow", 200);
    p.ammoTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.AmmoTarget", 1000);
    p.reagentLow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.ReagentLow", 1);
    p.reagentTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.ReagentTarget", 5);
    p.townRadius = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.TownRadius", 180);
    p.hearthMinYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.HearthMinYards", 800);
    p.maxWalkYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.MaxWalkYards", 4000);
    p.travelTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.TravelTimeoutMs", 1200000);
    p.auctionDetourMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.AuctionDetourMs", 0);
    p.keepConsumables = sConfigMgr->GetOption<bool>("AutoWow.Survival.KeepConsumables", false);
    p.sellDetourYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.KeepConsumables.SellDetourYards", 30);
    // AutoWow.Gear.Upgrades (GearUpgradePolicy.h): read before the catalog (vendors list their gear with it on).
    AutoWowGear::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Gear.Upgrades", false);
    AutoWowGear::Params& g = AutoWowGear::detail::gParams;
    g.upgradePct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.UpgradePct", 150);
    g.farBelowPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.FarBelowPct", 50);
    g.armorSpendPct = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.ArmorSpendPct", 25));
    g.maxArmorBuys = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.MaxArmorBuys", 4);
    if (detail::gEnabled)
        BuildCatalog();
}

bool Active(std::uint32_t guid) { return LoadState(guid).phase != Phase::None; }
}  // namespace AutoWowErrands

bool NewRpgBaseAction::ErrandsStep()
{
    using namespace AutoWowErrands;
    using AutoWowErrands::BotState;  // other AutoWow policies also name these
    using AutoWowErrands::Params;
    using AutoWowErrands::Phase;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive() || bot->IsInFlight() || bot->IsInCombat() || !botAI->IsAutoWowIndependentParty() ||
        AutoWowOracleRuntime::IsManagedBot(guid) || !bot->GetMap() || bot->GetMap()->Instanceable() ||
        bot->GetTransport())
        return false;

    Params const& p = detail::gParams;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    BotState s = LoadState(guid);
    NewRpgInfo& info = botAI->rpgInfo;
    std::uint8_t const team = TeamOf(bot);
    std::int32_t const bx = Yd(bot->GetPositionX()), by = Yd(bot->GetPositionY());

    auto finish = [&]()
    {
        Town const* town = FindTown(s.town);
        std::uint64_t const returnMs = s.phase == Phase::Return && now >= s.phaseMs ? now - s.phaseMs : 0;
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitErrand(bot, OutcomeName(s.outcome),
                                           LedgerFields(s, town ? town->zone : 0, returnMs));
        LOG_INFO("playerbots", "[Errands] bot={} {} town={} needs={} done={} spent={} sold={} leg={} hearth={} "
                 "travel_ms={} return_ms={}", bot->GetName(), OutcomeName(s.outcome), s.town, s.needs, s.done,
                 s.spent, s.sold, LegName(s.travelLeg), s.hearthUsed, s.travelMs, returnMs);
        s = AfterRun(p, s, now);
        StoreState(guid, s);
        info.ChangeToIdle();
        return true;
    };

    // AutoWow.Survival.KeepConsumables: sell the greys at a friendly vendor the bot passes (one detour of
    // at most SellDetourMs, then SellRetryMs before the next). No run under way, no flight or zone trip.
    if (p.keepConsumables && s.phase == Phase::None && now >= s.sellRetryMs &&
        !(AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid)) &&
        info.GetStatus() != RPG_TRAVEL_FLIGHT && GreyCopper(bot))
    {
        if (Creature* vendor = NearestVendor(bot, botAI, p.sellDetourYards))
        {
            if (!s.sellUntilMs)
                s.sellUntilMs = now + p.sellDetourMs;
            bool const inReach = bot->IsWithinDistInMap(vendor, INTERACTION_DISTANCE - 0.5f);
            if (inReach || now >= s.sellUntilMs)
            {
                if (inReach)
                {
                    bot->StopMoving();
                    uint64 const m0 = bot->GetMoney();
                    botAI->DoSpecificAction("sell", Event("autowow errands", "autowow-gray"), true);
                    LOG_INFO("playerbots", "[Errands] bot={} sold greys at passing vendor={} copper={}", bot->GetName(),
                             vendor->GetEntry(), bot->GetMoney() > m0 ? bot->GetMoney() - m0 : 0);
                }
                s.sellUntilMs = 0;
                s.sellRetryMs = now + p.sellRetryMs;
                StoreState(guid, s);
                return inReach;
            }
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, vendor->GetPositionX(), vendor->GetPositionY(),
                                                  vendor->GetPositionZ());
            StoreState(guid, s);
            return true;
        }
        if (s.sellUntilMs)
        {
            s.sellUntilMs = 0;
            StoreState(guid, s);
        }
    }

    if (s.phase == Phase::None)
    {
        if (now < s.nextCheckMs || now < s.cooldownUntilMs)
            return false;
        s.nextCheckMs = now + p.checkIntervalMs;
        StoreState(guid, s);
        // Busy: a zone graduation, a flight, an escort under way.
        if ((AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid)) ||
            info.GetStatus() == RPG_TRAVEL_FLIGHT)
            return false;
        if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
            if (quest->objectiveRuntime.phase == QuestActionPhase::EscortEvent)
                return false;
        Assessment const a = AssessBot(bot, s);
        if (!ShouldRun(a.needs, a.urgent))
            return false;
        Leg leg = Leg::None;
        Town const* town = ChooseTown(bot, team, leg);
        if (!town)
            return false;
        std::uint32_t const serves = Serves(FactsOf(bot, *town, team));
        if (!ShouldRun(a.needs & serves, a.urgent & serves))
            return false;
        s.phase = Phase::Travel;
        s.town = town->id;
        s.needs = a.needs & serves;
        s.leg = leg;
        s.travelLeg = leg;
        s.startMs = s.phaseMs = s.legMs = now;
        s.backMap = bot->GetMapId();
        s.backX = bx;
        s.backY = by;
        s.backZ = Yd(bot->GetPositionZ());
        LOG_INFO("playerbots", "[Errands] bot={} start town={} zone={} needs={} urgent={} leg={} lvl={}",
                 bot->GetName(), town->id, town->zone, s.needs, a.urgent & serves, LegName(leg), bot->GetLevel());
    }

    Town const* town = FindTown(s.town);
    if (!town)
    {
        s.outcome = Outcome::TravelGaveUp;
        return finish();
    }
    std::int64_t const arrive2 = std::int64_t(p.arriveYards) * p.arriveYards;
    WorldPosition const innPos(town->map, float(town->x), float(town->y), float(town->z));

    // One tick of a flight / walk leg toward `dest` (travel and return). True = the tick is consumed.
    auto legTick = [&](WorldPosition const& dest, uint32 destNode) -> bool
    {
        if (s.leg == Leg::Flight)
        {
            if (info.GetStatus() == RPG_TRAVEL_FLIGHT)
            {
                StoreState(guid, s);
                return false;  // the flight status owns the leg; landing returns the bot to Idle
            }
            if (s.legIssued)
            {
                s.leg = Leg::Walk;  // landed (or the taxi failed) short of the destination: walk the rest
                s.legIssued = false;
            }
            else
            {
                TravelMgr::FlightMasterInfo const* fm = nullptr;
                std::vector<uint32> path;
                if (FlightTo(bot, destNode, fm, path))
                {
                    s.legIssued = true;
                    StoreState(guid, s);
                    info.ChangeToTravelFlight(fm->templateEntry, fm->pos, path);
                    return true;
                }
                ++s.reissues;
                s.leg = Leg::Walk;
            }
        }
        if (s.leg == Leg::Walk && WalkLeg(dest))
            ++s.reissues;  // stuck: the no-progress window of the no-teleport mover
        StoreState(guid, s);
        return true;
    };

    if (s.phase == Phase::Travel)
    {
        if (bot->GetMapId() == town->map && Dist2(bx, by, town->x, town->y) <= arrive2)
        {
            // Arrived: plan the batch (ErrandsPolicy PlanStops).
            s.travelMs = now - s.startMs;
            s.durBefore = EquippedDurabilityPct(bot);
            s.bagFreeBefore = BagFree(bot);
            std::vector<uint32> available;
            for (Npc const& n : town->npcs)
                if (n.teams & team)
                    available.insert(available.end(), n.items.begin(), n.items.end());
            std::sort(available.begin(), available.end());
            std::array<std::uint32_t, kKinds> have{};
            for (std::size_t k = 0; k < kKinds; ++k)
                have[k] = Stock(bot, static_cast<Kind>(k));
            PlanInput in;
            in.team = team;
            for (RestockLine const& line :
                 RestockList(p, bot->getClass(), bot->GetLevel(), AmmoOf(bot), have, available))
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(line.item);
                if (proto && proto->RequiredLevel <= bot->GetLevel())
                    in.buyItems[line.kind] = line.item;
            }
            uint64 const money = bot->GetMoney();
            for (Npc const& n : town->npcs)
            {
                bool unaffordable = false;
                if ((n.teams & team) && (n.roles & (RoleClassTrainer | RoleTradeTrainer)) &&
                    TrainerHasWork(bot, n.entry, money, unaffordable))
                    in.trainers.push_back(n.spawn);
            }
            in.bind = !HearthBoundAt(bot, *town);
            if (Npc const* fm = TownFlightMaster(*town, team))
                in.learnFp = !bot->m_taxi.IsTaximaskNodeKnown(NodeFor(*fm, team));
            if (AutoWowTrade::Enabled())
            {
                // Mail when there is some to take, or after an auction visit (won items arrive at once).
                in.auction = std::any_of(town->npcs.begin(), town->npcs.end(), [team](Npc const& n)
                                         { return (n.teams & team) && (n.roles & RoleAuction); });
                in.mail = in.auction || AutoWowTrade::HasCollectableMail(bot);
            }
            if (AutoWowGear::Enabled() && (s.needs & NeedGear))
                PlanGear(bot, botAI, *town, team, s, in);
            s.plan = PlanStops(*town, in);
            s.buyItems = in.buyItems;
            s.stop = 0;
            s.phase = Phase::Errands;
            s.phaseMs = s.legMs = now;
            s.reissues = 0;
            LOG_INFO("playerbots", "[Errands] bot={} arrived town={} ms={} stops={} leg={} hearth={}", bot->GetName(),
                     town->id, s.travelMs, s.plan.count, LegName(s.travelLeg), s.hearthUsed);
            info.ChangeToIdle();
            StoreState(guid, s);
            return true;
        }
        if (LegExhausted(p, s, now, p.travelTimeoutMs))
        {
            // AutoWow.Travel.Safe: one rescue leg per run (ErrandsPolicy RescueLeg) before giving up.
            if (sPlayerbotAIConfig.autoWowTravelSafe && !s.rescued)
            {
                Town const* hearthTown = nullptr;
                if (HearthReady(bot))
                    for (Town const& t : detail::gTowns)
                        if ((t.teams & team) && HearthBoundAt(bot, t) && (s.needs & Serves(FactsOf(bot, t, team))))
                        {
                            hearthTown = &t;
                            break;
                        }
                Leg const rescue = RescueLeg(s.rescued, s.travelLeg, hearthTown != nullptr,
                                             TownLeg(bot, *town, team).flight);
                s.rescued = true;
                if (rescue != Leg::None)
                {
                    if (rescue == Leg::Hearth)
                        s.town = hearthTown->id;
                    s.leg = s.travelLeg = rescue;
                    s.legIssued = false;
                    s.reissues = 0;
                    s.phaseMs = s.legMs = now;
                    LOG_INFO("playerbots", "[Errands] bot={} rescue leg={} town={} at ({},{})", bot->GetName(),
                             LegName(rescue), s.town, bx, by);
                    StoreState(guid, s);
                    return true;
                }
            }
            s.outcome = Outcome::TravelGaveUp;
            return finish();
        }
        if (s.leg == Leg::None)
        {
            // (Re)choose: at start the hearth may fail, a flight may be gone.
            s.leg = ChooseLeg(p, TownLeg(bot, *town, team));
            s.legIssued = false;
            if (s.leg == Leg::None)
                ++s.reissues;
            else
                s.travelLeg = s.leg;
            StoreState(guid, s);
            return true;
        }
        if (s.leg == Leg::Hearth)
        {
            if (!s.legIssued)
            {
                bool cast = false;
                if (HearthReady(bot))
                {
                    if (bot->isMoving())
                    {
                        bot->StopMoving();
                        bot->GetMotionMaster()->Clear();
                    }
                    if (info.GetStatus() != RPG_IDLE)
                        info.ChangeToIdle();
                    cast = botAI->DoSpecificAction("hearthstone", Event("autowow errands"), true);
                }
                if (cast)
                {
                    s.legIssued = true;
                    s.hearthUsed = true;
                    s.legMs = now;
                }
                else
                {
                    ++s.reissues;
                    s.leg = Leg::None;
                }
                StoreState(guid, s);
                return true;
            }
            if (bot->IsNonMeleeSpellCast(false) || bot->IsBeingTeleported())
                return true;  // casting / relocating
            if (now - s.legMs > 20000)
            {
                // The cast ended without landing at the town (interrupted, or bound elsewhere): re-choose.
                ++s.reissues;
                s.leg = Leg::None;
                s.legIssued = false;
            }
            StoreState(guid, s);
            return true;
        }
        Npc const* fm = TownFlightMaster(*town, team);
        return legTick(innPos, fm ? NodeFor(*fm, team) : 0);
    }

    if (s.phase == Phase::Errands)
    {
        bool const timedOut = now - s.phaseMs > p.errandsTimeoutMs;
        if (timedOut || s.stop >= s.plan.count)
        {
            if (timedOut)
                s.outcome = Outcome::ErrandsTimeout;
            s.durAfter = EquippedDurabilityPct(bot);
            s.bagFreeAfter = BagFree(bot);
            bool const far = bot->GetMapId() != s.backMap ||
                             Dist2(bx, by, s.backX, s.backY) > std::int64_t(p.returnMinYards) * p.returnMinYards;
            if (!far)
                return finish();
            s.phase = Phase::Return;
            s.phaseMs = s.legMs = now;
            s.leg = Leg::None;
            s.legIssued = false;
            s.reissues = 0;
            StoreState(guid, s);
            return true;
        }
        Stop const& st = s.plan.stops[s.stop];
        if (now - s.legMs > p.stopTimeoutMs)
        {
            LOG_INFO("playerbots", "[Errands] bot={} skip stop spawn={} entry={} ops={} (timeout)", bot->GetName(),
                     st.spawn, st.entry, st.ops);
            ++s.stop;
            s.legMs = now;
            StoreState(guid, s);
            return true;
        }
        if (st.ops & OpMail)
        {
            // AutoWow.Trade mail stop (only planned with the flag on): a gameobject, not an npc.
            GameObject* mailbox = bot->FindNearestGameObject(st.entry, 60.0f);
            if (mailbox && bot->IsWithinDistInMap(mailbox, INTERACTION_DISTANCE - 0.5f))
            {
                AutoWowTrade::VisitMailbox(bot, mailbox);
                ++s.stop;
                s.legMs = now;
            }
            else
            {
                if (info.GetStatus() != RPG_IDLE)
                    info.ChangeToIdle();
                if (mailbox && bot->GetExactDist2d(mailbox) < 40.0f)
                {
                    if (!bot->isMoving())
                        bot->GetMotionMaster()->MovePoint(0, mailbox->GetPositionX(), mailbox->GetPositionY(),
                                                          mailbox->GetPositionZ());
                }
                else
                    WalkLeg(WorldPosition(town->map, float(st.x), float(st.y), float(st.z)));
            }
            StoreState(guid, s);
            return true;
        }
        Creature* npc = bot->FindNearestCreature(st.entry, 60.0f);
        if (npc && npc->IsAlive() && bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE - 0.5f))
        {
            ErrandsAtNpc(npc, st, s);
            ++s.stop;
            s.legMs = now;
            StoreState(guid, s);
            return true;
        }
        if (info.GetStatus() != RPG_IDLE)
            info.ChangeToIdle();
        if (npc && bot->GetExactDist2d(npc) < 40.0f)
        {
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
        }
        else
            WalkLeg(WorldPosition(town->map, float(st.x), float(st.y), float(st.z)));  // the stop timeout bounds it
        StoreState(guid, s);
        return true;
    }

    // Phase::Return: back to the pre-run position.
    if (bot->GetMapId() == s.backMap && Dist2(bx, by, s.backX, s.backY) <= arrive2)
        return finish();
    if (bot->GetMapId() != s.backMap || LegExhausted(p, s, now, p.returnTimeoutMs))
    {
        if (s.outcome == Outcome::Done)
            s.outcome = Outcome::ReturnGaveUp;
        return finish();
    }
    WorldPosition const back(s.backMap, float(s.backX), float(s.backY), float(s.backZ));
    uint32 backNode = 0;  // known node nearest the pre-run position (only while a flight may be issued)
    if (s.leg == Leg::None || (s.leg == Leg::Flight && !s.legIssued))
    {
        TravelMgr::FlightMasterInfo const* nearFm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        backNode = NearestKnownNode(bot, s.backMap, s.backX, s.backY, nearFm ? nearFm->taxiNodeId : 0);
    }
    if (s.leg == Leg::None)
    {
        LegInput in;
        in.walkYards = Yards(bx, by, s.backX, s.backY);
        TravelMgr::FlightMasterInfo const* fm = nullptr;
        std::vector<uint32> path;
        TaxiNodesEntry const* node = backNode ? sTaxiNodesStore.LookupEntry(backNode) : nullptr;
        if (node && FlightTo(bot, backNode, fm, path))
        {
            std::int32_t const fx = Yd(fm->pos.GetPositionX()), fy = Yd(fm->pos.GetPositionY());
            in.flight = true;
            in.fmYards = Yards(bx, by, fx, fy);
            in.flyYards = Yards(fx, fy, Yd(node->x), Yd(node->y));
            in.tailYards = Yards(Yd(node->x), Yd(node->y), s.backX, s.backY);
        }
        s.leg = ChooseLeg(p, in);
        s.legIssued = false;
        if (s.leg == Leg::None)
            ++s.reissues;
        StoreState(guid, s);
        return true;
    }
    return legTick(back, backNode);
}

// One npc stop: its operations in bit order (sell, repair, buy, train, bind, flight path). Real gold.
void NewRpgBaseAction::ErrandsAtNpc(Creature* npc, AutoWowErrands::Stop const& st, AutoWowErrands::BotState& s)
{
    using namespace AutoWowErrands;
    Params const& p = detail::gParams;
    bot->StopMoving();
    bot->SetFacingToObject(npc);
    RESET_AI_VALUE(GuidVector, "nearest npcs");  // the stock sell / repair actions pick their npc from it
    auto money = [&]() { return uint64(bot->GetMoney()); };

    if (st.ops & OpSell)
    {
        uint64 const m0 = money();
        // Stock `rpg sell` event: vendor-usage items when any, else gray.
        botAI->DoSpecificAction("sell", Event("rpg action", AI_VALUE(bool, "can sell") ? "vendor" : "gray"), true);
        if (money() > m0)
        {
            s.sold += money() - m0;
            s.done |= DoneSold;
        }
    }
    if (st.ops & OpRepair)
    {
        uint64 const m0 = money();
        botAI->DoSpecificAction("repair", Event("autowow errands"), true);
        if (m0 > money())
        {
            s.spent += m0 - money();
            s.done |= DoneRepaired;
            AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Repair, m0 - money());
        }
    }
    // AutoWow.Survival.KeepConsumables: greys go at every vendor stop (the sell stop may be unreachable or
    // sell by item usage only), then the two-pass restock.
    if (p.keepConsumables && (st.ops & (OpSell | OpBuy)))
    {
        uint64 const m0 = money();
        botAI->DoSpecificAction("sell", Event("autowow errands", "autowow-gray"), true);
        if (money() > m0)
        {
            s.sold += money() - m0;
            s.done |= DoneSold;
        }
    }
    if ((st.ops & OpBuy) && p.keepConsumables)
        KeepBuy(bot, npc, st, s, p);
    else if (st.ops & OpBuy)
    {
        VendorItemData const* list = npc->GetVendorItems();
        // Keep a class-trainer budget when training is one of the needs (training comes after).
        uint64 const reserve = (s.needs & NeedClassTrain) ? ClassTrainBudgetCopper(bot->GetLevel()) : 0;
        for (std::size_t k = 0; list && k < kKinds; ++k)
        {
            uint32 const item = s.buyItems[k];
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!(st.buyKinds & (1u << k)) || !proto)
                continue;
            uint32 slot = list->GetItemCount();
            for (uint32 i = 0; i < list->GetItemCount(); ++i)
                if (VendorItem const* vi = list->GetItem(i); vi && vi->item == item && !vi->ExtendedCost)
                {
                    slot = i;
                    break;
                }
            Kind const kind = static_cast<Kind>(k);
            uint32 const have = Stock(bot, kind);
            uint32 const target = TargetOf(p, kind);
            if (slot == list->GetItemCount() || have >= target)
                continue;
            uint64 const spendable = money() > reserve ? money() - reserve : 0;
            // BuyPrice is per pack of BuyCount, before the reputation discount (an upper bound).
            PackBuy const b = PacksToBuy(target - have, proto->BuyCount, proto->BuyPrice, spendable);
            uint64 const m0 = money();
            uint32 bought = 0;
            while (bought < b.packs && bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT))
                ++bought;
            if (m0 > money())
                s.spent += m0 - money();
            if (bought)
                s.done |= DoneRestocked;
            if (b.shortOfMoney || bought < b.packs)
            {
                s.done |= DoneSkipped;
                LOG_INFO("playerbots", "[Errands] bot={} skip item={} bought={}/{} short_of_money={} money={}",
                         bot->GetName(), item, bought, b.packs, b.shortOfMoney, money());
            }
        }
    }
    if (st.ops & OpTrain)
    {
        bot->SetSelection(npc->GetGUID());
        uint64 const m0 = money();
        botAI->DoSpecificAction("trainer", Event("autowow errands", "learn"), true);
        if (m0 > money())
        {
            s.spent += m0 - money();
            s.done |= DoneTrained;
            AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Train, m0 - money());
        }
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (trainer && trainer->GetTrainerType() == Trainer::Type::Class)
            s.lastClassTrainLevel = bot->GetLevel();
        bool unaffordable = false;
        TrainerHasWork(bot, npc->GetEntry(), money(), unaffordable);
        if (unaffordable)
        {
            s.done |= DoneSkipped;
            LOG_INFO("playerbots", "[Errands] bot={} skip trainer={} ranks unaffordable money={}", bot->GetName(),
                     npc->GetEntry(), money());
        }
    }
    // AutoWow.Gear.Upgrades (only planned with the flag on): buy this vendor's planned pieces while the
    // gear reserve (trainer rank + food) stays in hand, then the stock equip action puts upgrades on.
    if (st.ops & OpGear)
    {
        VendorItemData const* list = npc->GetVendorItems();
        std::vector<uint32> bought;
        for (std::size_t k = 0; list && k < s.gearItems.size(); ++k)
        {
            uint32 const item = s.gearItems[k];
            ItemTemplate const* proto = item && s.gearNpcs[k] == st.spawn ? sObjectMgr->GetItemTemplate(item) : nullptr;
            if (!proto)
                continue;
            uint32 slot = list->GetItemCount();
            for (uint32 i = 0; i < list->GetItemCount(); ++i)
                if (VendorItem const* vi = list->GetItem(i); vi && vi->item == item && !vi->ExtendedCost)
                {
                    slot = i;
                    break;
                }
            uint64 const m0 = money();
            if (slot == list->GetItemCount() ||
                m0 < uint64(proto->BuyPrice) + AutoWowGear::ReserveCopper(bot->GetLevel()))
            {
                s.done |= DoneSkipped;
                LOG_INFO("playerbots", "[Gear] bot={} skip item={} npc={} in_list={} money={}", bot->GetName(), item,
                         npc->GetEntry(), slot != list->GetItemCount(), m0);
                continue;
            }
            uint32 const before = bot->GetItemCount(item, false);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(item, false) <= before)
                continue;  // bags full / stock: the core reports its own error
            bought.push_back(item);
            if (m0 > money())
                s.spent += m0 - money();
        }
        if (!bought.empty())
        {
            s.done |= DoneGeared;
            uint32 const mh0 = EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND);
            botAI->DoSpecificAction("equip upgrades packet action", Event("autowow gear"), true);
            for (uint32 const item : bought)
            {
                bool equipped = false;
                for (uint8 e = EQUIPMENT_SLOT_START; e < EQUIPMENT_SLOT_END && !equipped; ++e)
                    if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, e))
                        equipped = worn->GetEntry() == item;
                LOG_INFO("playerbots", "[Gear] bot={} bought item={} npc={} equipped={} mh_dps_milli={}->{} money={} "
                         "lvl={}", bot->GetName(), item, npc->GetEntry(), equipped, mh0,
                         EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND), money(), bot->GetLevel());
            }
        }
    }
    if (st.ops & OpBind)
    {
        // The innkeeper's own effect (as zone progression does at a hub inn).
        bot->SetHomebind(WorldLocation(npc->GetMapId(), npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(),
                                       npc->GetOrientation()),
                         npc->GetAreaId());
        s.done |= DoneBound;
    }
    if (st.ops & OpLearnFp)
    {
        bot->GetSession()->SendLearnNewTaxiNode(npc);
        uint32 const node = sObjectMgr->GetNearestTaxiNode(npc->GetPositionX(), npc->GetPositionY(),
                                                           npc->GetPositionZ(), npc->GetMapId(), bot->GetTeamId());
        if (node && bot->m_taxi.IsTaximaskNodeKnown(node))
            s.done |= DoneLearnedFp;
    }
    // AutoWow.Trade (only planned with the flag on): the class-trainer budget is kept out of purchases.
    if (st.ops & OpAuction)
        AutoWowTrade::VisitAuctioneer(botAI, bot, npc, ClassTrainBudgetCopper(bot->GetLevel()));
}
