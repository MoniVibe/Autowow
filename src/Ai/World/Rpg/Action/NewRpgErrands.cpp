/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Errands runtime (policy: AutoWow/ErrandsPolicy.h).

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowGuildsPolicy.h"
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
#include "SupplyPolicy.h"
#include "SelfCraftPolicy.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SquadPolicy.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestTravelWalk.h"
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
    if (it != towns.end() && it->id == id)
        return &*it;
    // AutoWow.Errands.Mounts: a ride site (trainer spawn guid: a creature guid, disjoint from innkeeper ids).
    for (Town const& site : detail::gMountSites)
        if (id && site.id == id)
            return &site;
    return nullptr;
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
            // AutoWow.Supply.Outfit: the gathering tools it sells for plain gold (catalogued always: the Supply
            // flags load after this catalog; nothing reads the bits with Outfit off).
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* vi : list->m_items)
                    for (std::size_t k = 0; vi && !vi->ExtendedCost && k < kTools; ++k)
                        if (vi->item == kToolItems[k])
                            n.tools |= static_cast<std::uint8_t>(1u << k);
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
    // AutoWow.Errands.Mounts: riding trainers (trainer-profession flag) and mount vendors are in the scan already.
    if (detail::gParams.mounts)
    {
        detail::gMountSites = BuildMountSites(npcs);
        for (std::size_t k = 0; k < kMountSites; ++k)
        {
            Town& site = detail::gMountSites[k];
            if (Map* map = site.id ? sMapMgr->FindMap(site.map, 0) : nullptr)
                site.zone = map->GetZoneId(PHASEMASK_NORMAL, float(site.x), float(site.y), float(site.z));
            LOG_INFO("server.loading", ">> [Mounts] site row={} trainer={} vendor={} spawn={} map={} zone={} ({},{}) "
                     "teams={} npcs={}", k, kSites[k].trainer, kSites[k].vendor, site.id, site.map, site.zone, site.x,
                     site.y, static_cast<uint32>(site.teams), site.npcs.size());
        }
    }
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
    // AutoWow.SelfCraft.Cooking: the bot's own cooked food worth eating counts (errands buy less).
    if (kind == KindFood && AutoWowSelfCraft::CookingOn())
        n += AutoWowSelfCraft::CookedFoodStock(bot);
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
            // AutoWow.Supply.OutfitGear: food / drink skip the trainer reserve (the food floor).
            bool const floor = AutoWowSupply::OutfitGear() && (kind == KindFood || kind == KindWater);
            uint64 const keep = pass && !floor ? reserve : 0;
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
            if (bought && floor)
            {
                s.done |= DoneFoodFloor;
                AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::FoodFloor, item,
                                          money > bot->GetMoney() ? money - bot->GetMoney() : 0);
            }
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

// ---- AutoWow.Supply.Outfit ----------------------------------------------------------------------------
// The skill line a trainer spell grants, 0 = none (TrainerAction GrantedSkillLine idiom).
uint32 GrantedSkill(SpellInfo const* info)
{
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
            if (SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(effect.TriggerSpell))
                return node->skill;
    SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(info->Id);
    return node ? node->skill : 0;
}

// Copper of the ranks of the bot's planned primaries (AutoWow.Professions plan) this tradeskill trainer can teach
// now; `skills` collects their skill lines. 0 without a plan.
uint64 PlannedRankCost(Player* bot, Trainer::Trainer* trainer, std::vector<uint32>* skills = nullptr)
{
    std::vector<uint32> const* plan = sPlayerbotAIConfig.GetAutoWowProfessionPlan(bot->GetGUID().GetCounter());
    if (!plan || !trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
        return 0;
    uint64 cost = 0;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        SpellInfo const* info = trainer->CanTeachSpell(bot, &spell) ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr;
        uint32 const skill = info ? GrantedSkill(info) : 0;
        if (!skill || std::find(plan->begin(), plan->end(), skill) == plan->end())
            continue;
        cost += spell.MoneyCost;
        if (skills)
            skills->push_back(skill);
    }
    return cost;
}

// Tool bits of a cohort bot missing a gathering tool: a known Mining / Skinning, plus the skill lines in
// `learning` (planned ranks the town's trainers teach before its tool stop). Any pick / knife of the squad lists
// counts, in bags or equipped (HasItemCount walks the equipment slots). 0 with Outfit off.
std::uint8_t BotMissingTools(Player* bot, std::vector<uint32> const& learning = {})
{
    if (!AutoWowSupply::Outfit() || !AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), bot->GetGUID().GetCounter()))
        return 0;
    auto holds = [bot](auto const& list)
    { return std::any_of(std::begin(list), std::end(list), [bot](uint32 item) { return bot->HasItemCount(item, 1); }); };
    std::uint8_t const held = static_cast<std::uint8_t>((holds(AutoWowSquad::kMiningPicks) ? ToolPick : 0) |
                                                        (holds(AutoWowSquad::kSkinningKnives) ? ToolKnife : 0));
    std::uint8_t wants = 0;
    for (std::size_t i = 0; i < kTools; ++i)
        if (bot->HasSkill(kToolSkills[i]) ||
            std::find(learning.begin(), learning.end(), kToolSkills[i]) != learning.end())
            wants |= static_cast<std::uint8_t>(1u << i);
    return MissingTools(wants, held);
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
    // AutoWow.Supply.Outfit: an unaffordable rank of a planned primary is work too (a treasury grant covers it).
    if (!work && unaffordable && AutoWowSupply::Outfit())
        work = PlannedRankCost(bot, trainer) > 0;
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
    if (AutoWowSupply::Outfit())
        if (std::uint8_t const missing = BotMissingTools(bot))
            for (Npc const& n : t.npcs)
                if ((n.teams & team) && (n.roles & RoleVendor))
                    f.tools |= static_cast<std::uint8_t>(n.tools & missing);
    if (AutoWowSupply::MailPickup())
        f.mailbox = std::any_of(t.npcs.begin(), t.npcs.end(), [](Npc const& n) { return (n.roles & RoleMailbox) != 0; });
    if (AutoWowGear::AuctionEnabled())
        f.auction = std::any_of(t.npcs.begin(), t.npcs.end(),
                                [team](Npc const& n) { return (n.teams & team) && (n.roles & RoleAuction); });
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

// AutoWow.Gear.AuctionUpgrades: integer average ItemLevel over the slots the core's average counts (no shirt,
// tabard, off hand, ranged); an empty slot counts 0.
uint32 AvgIlvl(Player* bot)
{
    uint32 sum = 0, count = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_OFFHAND ||
            slot == EQUIPMENT_SLOT_RANGED)
            continue;
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            sum += item->GetTemplate()->ItemLevel;
        ++count;
    }
    return count ? sum / count : 0;
}

// AutoWow.Gear.AuctionUpgrades, errands' end: the auction purchases came by the mail stop (the world thread took the
// mail after the map update that queued it); the stock equip action puts upgrades on. Logs each piece.
void EquipAhGear(Player* bot, PlayerbotAI* botAI, BotState& s)
{
    uint32 const avg0 = AvgIlvl(bot);
    botAI->DoSpecificAction("equip upgrades packet action", Event("autowow ahgear"), true);
    for (uint32 const item : s.ahGearItems)
    {
        if (!item)
            continue;
        bool equipped = false;
        for (uint8 e = EQUIPMENT_SLOT_START; e < EQUIPMENT_SLOT_END && !equipped; ++e)
            if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, e))
                equipped = worn->GetEntry() == item;
        if (equipped)
            s.done |= DoneAhGear;
        LOG_INFO("playerbots", "[AhGear] bot={} equip item={} equipped={} in_bags={} avg_ilvl={}->{} lvl={}",
                 bot->GetName(), item, equipped, bot->GetItemCount(item, false), avg0, AvgIlvl(bot), bot->GetLevel());
    }
    s.ahGearItems.fill(0);
}

// The town's gear the stock "item upgrade" value rates an equip for this bot (class / spec weights, armor
// type, proficiency, level), as AutoWowGear offers. Ranged slots are not shopped (AutoWow.Supply.OutfitGear: a
// hunter's bow / gun / crossbow is, for its floor).
std::vector<AutoWowGear::Offer> GearOffers(Player* bot, PlayerbotAI* botAI, Town const& town, std::uint8_t team)
{
    std::vector<AutoWowGear::Offer> offers;
    bool const dualWield = bot->CanDualWield();
    bool const hunterRanged = AutoWowSupply::OutfitGear() && bot->getClass() == CLASS_HUNTER;
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
                if (hunterRanged && (proto->SubClass == ITEM_SUBCLASS_WEAPON_BOW ||
                                     proto->SubClass == ITEM_SUBCLASS_WEAPON_GUN ||
                                     proto->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW))
                {
                    o.slot = EQUIPMENT_SLOT_RANGED;
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

// PlanGear with AutoWow.Supply.OutfitGear on: the floor weapons (AutoWowGear FloorWeapons: own gold, then the
// grant room) go first, flagged in s.floorGear and bought before training (in.gearFirst); the regular list shops
// against the floor main hand with what own gold is left. Returns the floor weapons' copper (grant request share).
uint64 PlanFloorGear(Player* bot, PlayerbotAI* botAI, Town const& town, std::uint8_t team, BotState& s, PlanInput& in)
{
    AutoWowSupply::Params const& sp = AutoWowSupply::detail::gParams;
    uint32 const level = bot->GetLevel();
    s.lastGearLevel = level;
    s.gearItems.fill(0);
    s.gearNpcs.fill(0);
    std::vector<AutoWowGear::Offer> const offers = GearOffers(bot, botAI, town, team);
    uint32 mainHandMilli = EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND);
    uint32 const rangedMilli = EquippedDpsMilli(bot, EQUIPMENT_SLOT_RANGED);
    uint64 const money = bot->GetMoney();
    uint64 const room = AutoWowSupply::GrantCapCopper(sp.outfitMaxCopper, true, sp.outfitGearCopper, level);
    bool dualWield = bot->CanDualWield();
    std::vector<AutoWowGear::Offer> list = AutoWowGear::FloorWeapons(
        AutoWowGear::Get(), offers, level, mainHandMilli, bot->getClass() == CLASS_HUNTER, rangedMilli, money, room);
    uint64 floorCopper = 0;
    for (AutoWowGear::Offer const& o : list)
    {
        floorCopper += o.price;
        LOG_INFO("playerbots", "[Outfit] bot={} weapon_floor plan item={} npc={} slot={} price={} dps_milli={} "
                 "cur_milli={} floor_milli={} money={} grant_room={} lvl={}", bot->GetName(), o.item, o.npc,
                 static_cast<uint32>(o.slot), o.price, o.dpsMilli,
                 o.slot == EQUIPMENT_SLOT_RANGED ? rangedMilli : mainHandMilli,
                 AutoWowGear::ExpectedDpsMilli(level) * AutoWowGear::Get().farBelowPct / 100, money, room, level);
        if (o.slot == EQUIPMENT_SLOT_MAINHAND)
        {
            mainHandMilli = o.dpsMilli;
            dualWield = dualWield && !o.twoHand;
        }
    }
    s.floorGear = static_cast<std::uint8_t>((1u << list.size()) - 1);
    in.gearFirst = !list.empty();
    std::vector<AutoWowGear::Offer> const rest = AutoWowGear::ShoppingList(
        AutoWowGear::Get(), offers, mainHandMilli, EquippedDpsMilli(bot, EQUIPMENT_SLOT_OFFHAND), dualWield,
        EmptyArmorSlots(bot), AutoWowGear::Spendable(money - std::min(money, floorCopper), level));
    list.insert(list.end(), rest.begin(), rest.end());
    for (std::size_t k = 0; k < list.size() && k < AutoWowGear::kMaxPicks; ++k)
    {
        s.gearItems[k] = list[k].item;
        s.gearNpcs[k] = list[k].npc;
        in.gearNpcs.push_back(list[k].npc);
        LOG_INFO("playerbots", "[Gear] bot={} plan item={} npc={} slot={} price={} dps_milli={} armor={} money={}",
                 bot->GetName(), list[k].item, list[k].npc, static_cast<uint32>(list[k].slot), list[k].price,
                 list[k].dpsMilli, list[k].armor, money);
    }
    std::sort(in.gearNpcs.begin(), in.gearNpcs.end());
    in.gearNpcs.erase(std::unique(in.gearNpcs.begin(), in.gearNpcs.end()), in.gearNpcs.end());
    return floorCopper;
}

// AutoWow.Supply.Outfit, arrival: the missing tools the town sells into the plan, and a grant request when the
// bot's money is short of them plus its planned trainer ranks here (+ kGrantBufferCopper).
// AutoWow.Supply.OutfitGear: floorCopper (floor weapons + food / drink stack) joins the one request.
void PlanOutfit(Player* bot, Town const& town, std::uint8_t team, PlanInput& in, uint64 floorCopper = 0)
{
    if (floorCopper)
        LOG_INFO("playerbots", "[Outfit] bot={} floor copper={} money={} lvl={}", bot->GetName(), floorCopper,
                 bot->GetMoney(), bot->GetLevel());
    std::vector<uint32> learning;
    uint64 ranks = 0;
    for (uint32 const spawn : in.trainers)
        for (Npc const& n : town.npcs)
            if (n.spawn == spawn)
                ranks += PlannedRankCost(bot, sObjectMgr->GetTrainer(n.entry), &learning);
    std::uint8_t sold = 0;
    for (Npc const& n : town.npcs)
        if ((n.teams & team) && (n.roles & RoleVendor))
            sold |= n.tools;
    in.tools = static_cast<std::uint8_t>(BotMissingTools(bot, learning) & sold);
    uint64 tools = 0;
    for (std::size_t i = 0; i < kTools; ++i)
        if (in.tools & (1u << i))
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(kToolItems[i]))
                tools += proto->BuyPrice;
    uint64 const need = tools + ranks + (ranks ? AutoWowSupply::kGrantBufferCopper : 0) + floorCopper;
    if (!need)
        return;
    if (need > bot->GetMoney())
        AutoWowSupply::RequestGrant(bot, need);
    LOG_INFO("playerbots", "[Outfit] bot={} plan tools={} tool_copper={} rank_copper={} need={} money={} grant={}",
             bot->GetName(), static_cast<uint32>(in.tools), tools, ranks, need, bot->GetMoney(),
             need > bot->GetMoney());
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

// A town walk starts only with an exact complete local proof or a fresh reachable TravelMgr prefix.
// The latter retains long waypoint-routed towns without treating a local detour as town connectivity.
bool TownWalkRouteAvailable(Player* bot, Town const& t)
{
    WorldPosition const dest(t.map, float(t.x), float(t.y), float(t.z));
    if (AutoWowQuestGiverTravel::SelectCompleteWalkProbe(bot, dest))
        return true;
    return AutoWowQuestGiverTravel::SelectTravelMgrWalkProbe(bot, dest).has_value();
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
Assessment AssessBot(Player* bot, BotState const& s, std::uint64_t nowMs)
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
    if (sPlayerbotAIConfig.autoWowProfessionsTrainRuns)
        if (std::vector<uint32> const* plan = sPlayerbotAIConfig.GetAutoWowProfessionPlan(bot->GetGUID().GetCounter()))
        {
            bool needed = false;
            for (uint32 const skill : *plan)
                needed |= PlannedTrainNeeded(bot->HasSkill(skill), bot->GetPureSkillValue(skill),
                                             bot->GetPureMaxSkillValue(skill), o.level);
            o.trainRunDue = TrainRunDue(needed, nowMs, s.nextTrainRunMs);
        }
    o.hearthElsewhere = ZoneOfArea(bot->m_homebindAreaId) != bot->GetZoneId();
    TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    o.unknownFlightPath =
        fm && fm->taxiNodeId && fm->zoneId == bot->GetZoneId() && !bot->m_taxi.IsTaximaskNodeKnown(fm->taxiNodeId);
    if (AutoWowGear::Enabled())
        o.gear = AutoWowGear::GearDue(AutoWowGear::Get(), o.level, s.lastGearLevel, money,
                                      EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND));
    // AutoWow.Supply.OutfitGear: a main hand (hunter: or ranged) under the floor is urgent whatever the purse.
    if (AutoWowGear::Enabled() && AutoWowSupply::OutfitGear() &&
        AutoWowGear::FloorDue(AutoWowGear::Get(), o.level, s.lastGearLevel,
                              EquippedDpsMilli(bot, EQUIPMENT_SLOT_MAINHAND), o.cls == kClassHunter,
                              EquippedDpsMilli(bot, EQUIPMENT_SLOT_RANGED)))
        o.gear.soft = o.gear.urgent = true;
    // AutoWow.Gear.AuctionUpgrades: gear far under the ilvl curve with gold for it -> an auction town, once per level.
    if (AutoWowGear::AuctionEnabled() && AutoWowTrade::Enabled())
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            o.ahGearDue = AutoWowGear::AhRunDue(AutoWowGear::GetAh(), o.level, s.lastAhGearLevel, money,
                                                ai->GetAiObjectContext()->GetValue<uint32>("repair cost")->Get(),
                                                AvgIlvl(bot));
    if (AutoWowSupply::Outfit())
    {
        o.missingTools = BotMissingTools(bot);
        o.toolRunDue = OutfitRunDue(o.missingTools, nowMs, s.nextOutfitMs);
    }
    if (AutoWowSupply::MailPickup())
    {
        o.supplyMail = AutoWowSupply::HasSupplyMail(bot);
        o.mailRunDue = MailRunDue(o.supplyMail, nowMs, s.nextMailMs);
    }
    return Assess(p, o);
}

// AutoWow.Professions.TrainRuns: a trade trainer of the town teaches the bot a planned rank now (and the
// arrival would train there: TrainerHasWork).
bool TownTeachesPlan(Player* bot, Town const& t, std::uint8_t team)
{
    uint64 const money = bot->GetMoney();
    for (Npc const& n : t.npcs)
    {
        bool unaffordable = false;
        if ((n.teams & team) && (n.roles & RoleTradeTrainer) && PlannedRankCost(bot, sObjectMgr->GetTrainer(n.entry)) &&
            TrainerHasWork(bot, n.entry, money, unaffordable))
            return true;
    }
    return false;
}

// Cheapest reachable friendly town: the nearest CandidateTowns on the bot's map by straight line, plus
// the hearth town. Zones far above the bot's level are skipped unless reached by hearthstone.
// `trainOnly` (AutoWow.Professions.TrainRuns): only towns that teach the bot a planned rank now count.
// `auctionOnly` (AutoWow.Gear.AuctionUpgrades): only towns with a usable auctioneer count.
Town const* ChooseTown(Player* bot, std::uint8_t team, Leg& leg, WalkBackoffTable& walkBackoffs,
                       std::uint64_t nowMs, std::uint32_t needs, bool trainOnly = false,
                       bool auctionOnly = false)
{
    Params const& p = detail::gParams;
    std::int32_t const bx = Yd(bot->GetPositionX()), by = Yd(bot->GetPositionY());
    std::vector<std::pair<std::int64_t, Town const*>> nearby;
    Town const* hearthTown = nullptr;
    for (Town const& t : detail::gTowns)
    {
        if (!(t.teams & team) || t.map != bot->GetMapId())
            continue;
        if (trainOnly && !TownTeachesPlan(bot, t, team))
            continue;
        if (auctionOnly && std::none_of(t.npcs.begin(), t.npcs.end(), [team](Npc const& n)
                                        { return (n.teams & team) && (n.roles & RoleAuction); }))
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
    std::uint32_t const sourceZone = bot->GetZoneId();
    std::size_t walkCandidates = 0;
    std::vector<Candidate> cands;
    for (auto const& [d2, t] : nearby)
    {
        LegInput in = TownLeg(bot, *t, team);
        Leg l = ChooseLeg(p, in);
        auto const bracket = sPlayerbotAIConfig.zoneBrackets.find(t->zone);
        std::uint32_t const low = bracket == sPlayerbotAIConfig.zoneBrackets.end() ? 0 : bracket->second.first;
        if (l != Leg::Hearth && ZoneTooHigh(p, level, low))
            continue;
        if (l == Leg::Walk)
        {
            bool const withinProbeBudget = walkCandidates++ < kWalkBackoffs;
            bool const held = TownWalkBackedOff(walkBackoffs, t->id, sourceZone, needs, nowMs);
            bool const probeAvailable = withinProbeBudget && !held;
            in.walkRouteAvailable = probeAvailable && TownWalkRouteAvailable(bot, *t);
            if (!in.walkRouteAvailable && !held && probeAvailable)
            {
                RememberTownWalkFailure(walkBackoffs, t->id, sourceZone, needs, nowMs, nowMs + p.cooldownMs);
                LOG_INFO("playerbots", "[Errands] bot={} walk_denied town={} source_zone={} needs={} retry_ms={}",
                         bot->GetName(), t->id, sourceZone, needs, p.cooldownMs);
            }
            l = ChooseLeg(p, in);  // a denied walk may still use a known taxi
        }
        std::uint32_t const cost = LegCostMs(p, l, in);
        cands.push_back({t->id, l, detour ? DetourCostMs(cost, hasAuction(*t), p.auctionDetourMs) : cost});
    }
    Candidate const* best = PickTown(cands);
    if (!best)
        return nullptr;
    leg = best->leg;
    return FindTown(best->town);
}

// ---- AutoWow.Errands.Mounts ---------------------------------------------------------------------------------
std::uint8_t KnownRidingTier(Player* bot)
{
    for (std::size_t k = kRidingTiers; k-- > 0;)
        if (bot->HasSpell(kRiding[k].spell))
            return static_cast<std::uint8_t>(k + 1);
    return 0;
}

// Tier of the best mount the bot knows (the stock CollectMountData read: active SPELL_AURA_MOUNTED spells).
std::uint8_t KnownMountTier(Player* bot)
{
    int32 ground = 0;
    bool flying = false;
    for (auto const& [id, ps] : bot->GetSpellMap())
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info || !ps || ps->State == PLAYERSPELL_REMOVED || !ps->Active || info->IsPassive() ||
            info->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;
        if (info->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
            info->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
            flying = true;
        else
            ground = std::max({ground, info->Effects[1].BasePoints, info->Effects[2].BasePoints});
    }
    return MountTierOf(ground, flying);
}

// Mount items in the bags the bot may learn now (bought on a ride run, or a quest reward / drop): used, which
// learns them (Player::CastItemUseSpell's learning case consumes the item). Returns the mounts learned.
uint32 UseHeldMounts(Player* bot)
{
    std::vector<Item*> items;
    auto consider = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && proto->RequiredSkill == kRidingSkill &&
            (proto->Spells[0].SpellId == 483 || proto->Spells[0].SpellId == 55884) && proto->Spells[1].SpellId > 0 &&
            !bot->HasSpell(uint32(proto->Spells[1].SpellId)) && bot->CanUseItem(item) == EQUIP_ERR_OK)
            items.push_back(item);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag const* pBag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                consider(pBag->GetItemByPos(slot));
    uint32 learned = 0;
    for (Item* item : items)
    {
        uint32 const entry = item->GetEntry();
        uint32 const spell = uint32(item->GetTemplate()->Spells[1].SpellId);
        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(item, targets, 1, 0);  // the item is gone once learned
        bool const ok = bot->HasSpell(spell);
        learned += ok ? 1 : 0;
        LOG_INFO("playerbots", "[Mounts] bot={} use item={} spell={} learned={} riding={} lvl={}", bot->GetName(),
                 entry, spell, ok, bot->GetPureSkillValue(kRidingSkill), bot->GetLevel());
    }
    return learned;
}

// Plain-gold riding-skill items a vendor lists (world DB: the racial mounts; reputation-gated ones left out).
std::vector<MountOffer> MountOffers(uint32 vendorEntry)
{
    std::vector<MountOffer> out;
    if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(vendorEntry))
        for (VendorItem const* vi : list->m_items)
            if (ItemTemplate const* proto = vi && !vi->ExtendedCost ? sObjectMgr->GetItemTemplate(vi->item) : nullptr;
                proto && proto->RequiredSkill == kRidingSkill && !proto->RequiredReputationFaction)
                out.push_back({vi->item, static_cast<std::uint64_t>(std::max<int32>(0, proto->BuyPrice)),
                               proto->RequiredSkillRank, proto->AllowableRace});
    return out;
}

// Phase::None, a cohort bot once per MountsCheckMs: its next ride (NextRide) at its race's site when that site is
// reachable (ChooseLeg) and paid for (MountAfford: own gold after the class-trainer reserve, else one treasury grant
// request per window). True = a ride run started (state stored, Phase::Travel).
bool StartMountRun(Player* bot, BotState& s, std::uint64_t now, std::uint8_t team)
{
    Params const& p = detail::gParams;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!p.mounts || now < s.nextMountMs || !AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid))
        return false;
    s.nextMountMs = now + p.mountsCheckMs;
    UseHeldMounts(bot);
    uint32 const level = bot->GetLevel();
    std::uint8_t const known = KnownRidingTier(bot);
    std::uint8_t const mountTier = KnownMountTier(bot);
    Ride const r = NextRide(level, known, mountTier, p.mountsMaxTier);
    std::size_t const k = r.tier ? SiteFor(bot->getRace(), r.tier) : kMountSites;
    Town const* site = k < kMountSites && k < detail::gMountSites.size() && detail::gMountSites[k].id
                           ? &detail::gMountSites[k]
                           : nullptr;
    auto wait = [&](char const* why)
    {
        LOG_INFO("playerbots", "[Mounts] bot={} wait {} tier={} learn={} buy={} known={} mount_tier={} race={} "
                 "site={} map={} money={} lvl={}", bot->GetName(), why, static_cast<uint32>(r.tier), r.learn, r.buy,
                 static_cast<uint32>(known), static_cast<uint32>(mountTier), static_cast<uint32>(bot->getRace()),
                 site ? site->id : 0, bot->GetMapId(), bot->GetMoney(), level);
        StoreState(guid, s);
        return false;
    };
    if (!r.tier)
    {
        StoreState(guid, s);
        return false;
    }
    if (!site || !(site->teams & team))
        return wait("no_site");
    uint64 cost = 0;
    if (r.learn)
    {
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(kSites[k].trainer);
        Trainer::Spell const* spell = trainer ? trainer->GetSpell(kRiding[r.tier - 1].spell) : nullptr;
        if (!spell)
            return wait("no_trainer_spell");
        cost += spell->MoneyCost;
    }
    uint32 item = 0;
    if (r.buy)
    {
        std::vector<MountOffer> const offers = MountOffers(kSites[k].vendor);
        MountOffer const* o = CheapestMount(offers, kRiding[r.tier - 1].mountRank, 1u << (bot->getRace() - 1));
        if (!o)
            return wait("no_mount_offer");
        item = o->item;
        cost += o->price;
    }
    Leg const leg = ChooseLeg(p, TownLeg(bot, *site, team));
    if (leg == Leg::None)
        return wait("unreachable");
    bool const asked = s.mountGrantMs && now < s.mountGrantMs + p.mountsCheckMs;
    bool const grantable = AutoWowSupply::Outfit() && AutoWowGuilds::HouseGuildOf(bot);
    uint64 const money = bot->GetMoney();
    Afford const a = MountAfford(money, cost, ClassTrainBudgetCopper(level), grantable, asked);
    if (a.grantNeed)
    {
        AutoWowSupply::RequestGrant(bot, a.grantNeed, cost, p.mountsGrantBudgetPerHour);
        s.mountGrantMs = now;
        s.nextMountMs = now;  // the next errand check sees the tick's answer
        LOG_INFO("playerbots", "[Mounts] bot={} grant_request tier={} cost={} need={} money={} lvl={}", bot->GetName(),
                 static_cast<uint32>(r.tier), cost, a.grantNeed, money, level);
        StoreState(guid, s);
        return false;
    }
    if (!a.go)
    {
        if (asked && AutoWowSupply::GrantPending(guid))
            s.nextMountMs = now;
        return wait(asked ? "grant_short" : "unaffordable");
    }
    s.phase = Phase::Travel;
    s.town = site->id;
    s.needs = NeedRiding;
    s.leg = s.travelLeg = leg;
    s.walkAdmitted = false;  // mount-site walks use the same one-shot runtime admission
    s.townWalkAttempted = false;
    s.walkSourceZone = 0;
    s.startMs = s.phaseMs = s.legMs = now;
    s.backMap = bot->GetMapId();
    s.backX = Yd(bot->GetPositionX());
    s.backY = Yd(bot->GetPositionY());
    s.backZ = Yd(bot->GetPositionZ());
    s.rideTier = r.tier;
    s.rideLearn = r.learn;
    s.mountItem = item;
    LOG_INFO("playerbots", "[Mounts] bot={} start site={} zone={} tier={} learn={} item={} cost={} money={} "
             "granted={} leg={} lvl={}", bot->GetName(), site->id, site->zone, static_cast<uint32>(r.tier), r.learn,
             item, cost, money, asked, LegName(leg), level);
    StoreState(guid, s);
    return true;
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
    p.sellTradeGoods = sConfigMgr->GetOption<bool>("AutoWow.Errands.SellTradeGoods", false);
    p.keepConsumables = sConfigMgr->GetOption<bool>("AutoWow.Survival.KeepConsumables", false);
    p.sellDetourYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.KeepConsumables.SellDetourYards", 30);
    // AutoWow.Errands.Mounts: read before the catalog (the ride sites are built with it on).
    p.mounts = sConfigMgr->GetOption<bool>("AutoWow.Errands.Mounts", false);
    p.mountsCheckMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.MountsCheckMs", 600000);
    p.mountsMaxTier = std::min<std::uint32_t>(kRidingTiers,
                                              sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.MountsMaxTier", 2));
    p.mountsGrantBudgetPerHour =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.Errands.MountsGrantBudgetPerHour", 100000);
    // AutoWow.Gear.Upgrades (GearUpgradePolicy.h): read before the catalog (vendors list their gear with it on).
    AutoWowGear::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Gear.Upgrades", false);
    AutoWowGear::Params& g = AutoWowGear::detail::gParams;
    g.upgradePct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.UpgradePct", 150);
    g.farBelowPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.FarBelowPct", 50);
    g.armorSpendPct = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.ArmorSpendPct", 25));
    g.maxArmorBuys = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.MaxArmorBuys", 4);
    // AutoWow.Gear.AuctionUpgrades (GearUpgradePolicy.h; the auction stop needs AutoWow.Trade.Enable).
    AutoWowGear::detail::gAuctionEnabled = sConfigMgr->GetOption<bool>("AutoWow.Gear.AuctionUpgrades", false);
    AutoWowGear::AhParams& ah = AutoWowGear::detail::gAhParams;
    ah.maxBuys = std::min<std::uint32_t>(AutoWowGear::kAhMaxBuys,
                                         sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.AuctionMaxBuys", 3));
    ah.priceMult = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.AuctionPriceMult", 20);
    ah.ilvlPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.AuctionIlvlPct", 75);
    ah.minSpendPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.AuctionMinSpendPct", 50);
    // AutoWow.Gear.Flow (GearUpgradePolicy.h; the pass runs on the world thread, AutoWowSupply::GearFlowUpdate).
    AutoWowGear::detail::gFlowEnabled = sConfigMgr->GetOption<bool>("AutoWow.Gear.Flow", false);
    AutoWowGear::FlowParams& fl = AutoWowGear::detail::gFlowParams;
    fl.tickMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.FlowTickMs", 60000);
    fl.maxMails = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.FlowMaxMails", 10);
    fl.minQuality = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gear.FlowMinQuality", 1);
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

    auto retireTownWalk = [&](Town const& target)
    {
        WalkGoalKey const expected{target.map, target.x, target.y, target.z};
        bool const moveFarActive = info.moveFarPos != WorldPosition();
        TravelIntentPolicy::Point const moveFarPoint =
            TravelIntentPolicy::MakePoint(info.moveFarPos.GetMapId(), info.moveFarPos.GetPositionX(),
                                          info.moveFarPos.GetPositionY(), info.moveFarPos.GetPositionZ());
        WalkGoalKey const moveFar{moveFarPoint.mapId, moveFarPoint.x, moveFarPoint.y, moveFarPoint.z};
        TravelIntentPolicy::Intent const& intent = info.travelIntent;
        WalkGoalKey const intentGoal{intent.goal.mapId, intent.goal.x, intent.goal.y, intent.goal.z};
        if (!OwnsTownWalkGoal(expected, moveFarActive, moveFar, intent.active, intentGoal))
            return false;

        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        AI_VALUE(LastMovement&, "last movement").clear();
        info.travelIntent = {};
        info.SetMoveFarTo(WorldPosition());
        return true;
    };

    auto finish = [&]()
    {
        Town const* town = FindTown(s.town);
        if (ShouldRememberTownWalkFailure(s.outcome, s.townWalkAttempted) && town)
        {
            RememberTownWalkFailure(s.walkBackoffs, town->id, s.walkSourceZone, s.needs, now,
                                    now + p.cooldownMs);
            retireTownWalk(*town);
        }
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
        // AutoWow.Errands.Mounts: a paid-for riding rank / mount goes before the town needs (next tick travels).
        if (p.mounts && StartMountRun(bot, s, now, team))
            return true;
        Assessment const a = AssessBot(bot, s, now);
        if (a.urgent & NeedTool)
        {
            // AutoWow.Supply.Outfit only: a tool-due check spends the OutfitCheckMs window, run or not.
            s.nextOutfitMs = now + AutoWowSupply::detail::gParams.outfitCheckMs;
            StoreState(guid, s);
        }
        if (a.urgent & NeedMail)
        {
            // AutoWow.Supply.MailPickup only: a mail-due check spends the MailRunMs window, run or not.
            s.nextMailMs = now + AutoWowSupply::detail::gParams.mailRunMs;
            StoreState(guid, s);
        }
        if (a.urgent & NeedProfTrain)
        {
            // AutoWow.Professions.TrainRuns only: a train-due check spends the TrainRunCooldownMs window, run or not.
            s.nextTrainRunMs = now + sPlayerbotAIConfig.autoWowProfessionsTrainRunCooldownMs;
            StoreState(guid, s);
        }
        if (a.urgent & NeedAhGear)
        {
            // AutoWow.Gear.AuctionUpgrades only: an auction-run due check spends the level, run or not.
            s.lastAhGearLevel = bot->GetLevel();
            StoreState(guid, s);
        }
        if (!ShouldRun(a.needs, a.urgent))
            return false;
        Leg leg = Leg::None;
        // A run only a missing / capped planned profession asked for goes to a town that teaches it.
        bool const trainOnly = a.urgent == NeedProfTrain;
        // A run only the auction gear asked for goes to a town with an auctioneer.
        bool const auctionOnly = a.urgent == NeedAhGear;
        Town const* town =
            ChooseTown(bot, team, leg, s.walkBackoffs, now, a.needs, trainOnly, auctionOnly);
        StoreState(guid, s);  // retain failures; nextCheckMs bounds a no-town decision to one per check window
        if (trainOnly)
            LOG_INFO("playerbots", "[Professions] train_due bot={} level={} town={}", bot->GetName(), bot->GetLevel(),
                     town ? town->id : 0);
        if (a.urgent & NeedAhGear)
            LOG_INFO("playerbots", "[AhGear] run_due bot={} lvl={} avg_ilvl={} money={} only={} town={}", bot->GetName(),
                     bot->GetLevel(), AvgIlvl(bot), bot->GetMoney(), auctionOnly, town ? town->id : 0);
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
        s.walkAdmitted = leg == Leg::Walk;  // ChooseTown already proved this first walk handoff
        s.townWalkAttempted = false;
        s.walkSourceZone = leg == Leg::Walk ? bot->GetZoneId() : 0;
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
                s.walkAdmitted = false;
                s.townWalkAttempted = false;
                s.walkSourceZone = 0;
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
                s.walkAdmitted = false;
                s.townWalkAttempted = false;
                s.walkSourceZone = 0;
            }
        }
        if (s.phase == Phase::Travel && s.leg == Leg::Walk && !s.walkAdmitted)
        {
            StoreState(guid, s);
            return true;  // the next tick performs the one-shot town admission before any local segment
        }
        if (s.leg == Leg::Walk)
        {
            if (s.phase == Phase::Travel)
                s.townWalkAttempted = true;
            if (WalkLeg(dest))
                ++s.reissues;  // stuck: the no-progress window of the no-teleport mover
        }
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
            if (s.rideTier)
            {
                // AutoWow.Errands.Mounts: the ride site's stops only (ErrandsPolicy PlanRide).
                std::size_t const k = SiteFor(bot->getRace(), s.rideTier);
                s.plan = PlanRide(*town, s.rideLearn, s.mountItem != 0, k < kMountSites ? kSites[k].vendor : 0);
                s.stop = 0;
                s.phase = Phase::Errands;
                s.phaseMs = s.legMs = now;
                s.reissues = 0;
                LOG_INFO("playerbots", "[Mounts] bot={} arrived site={} ms={} stops={} leg={}", bot->GetName(), town->id,
                         s.travelMs, s.plan.count, LegName(s.travelLeg));
                info.ChangeToIdle();
                StoreState(guid, s);
                return true;
            }
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
            // AutoWow.Supply.OutfitGear: food (and drink for mana users, a mage too) topped up to Target on every
            // run; what own gold cannot pay joins the floor weapons in the grant request.
            uint64 floorCopper = 0;
            if (AutoWowSupply::OutfitGear())
                for (Kind const kind : {KindFood, KindWater})
                {
                    if (!(FloorKinds(bot->getClass(), bot->GetLevel(), AmmoOf(bot)) & (1u << kind)) ||
                        have[kind] >= TargetOf(p, kind))
                        continue;
                    if (!in.buyItems[kind])
                        if (uint32 const item = BestTier(kind, bot->GetLevel(), &available))
                            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                                proto && proto->RequiredLevel <= bot->GetLevel())
                                in.buyItems[kind] = item;
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(in.buyItems[kind]))
                        floorCopper += FloorCopper(TargetOf(p, kind) - have[kind], proto->BuyCount, proto->BuyPrice);
                }
            if (AutoWowGear::Enabled() && (s.needs & NeedGear))
            {
                if (AutoWowSupply::OutfitGear())
                    floorCopper += PlanFloorGear(bot, botAI, *town, team, s, in);
                else
                    PlanGear(bot, botAI, *town, team, s, in);
            }
            if (AutoWowSupply::Outfit())
                PlanOutfit(bot, *town, team, in, floorCopper);
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
        if (s.leg == Leg::Walk && !s.walkAdmitted)
        {
            s.walkSourceZone = bot->GetZoneId();  // capture the origin before any town-bound walk segment
            bool const held = TownWalkBackedOff(s.walkBackoffs, town->id, s.walkSourceZone, s.needs, now);
            if (!held && TownWalkRouteAvailable(bot, *town))
            {
                s.walkAdmitted = true;
            }
            else
            {
                RememberTownWalkFailure(s.walkBackoffs, town->id, s.walkSourceZone, s.needs, now,
                                        now + p.cooldownMs);
                retireTownWalk(*town);
                s.reissues = p.maxReissues + 1;  // enter the existing finite rescue/give-up path now
                LOG_INFO("playerbots", "[Errands] bot={} walk_denied_active town={} source_zone={} needs={} held={}",
                         bot->GetName(), town->id, s.walkSourceZone, s.needs, held);
            }
            StoreState(guid, s);
        }
        if (LegExhausted(p, s, now, p.travelTimeoutMs))
        {
            if (s.townWalkAttempted)
                retireTownWalk(*town);
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
                    {
                        s.town = hearthTown->id;
                        s.townWalkAttempted = false;
                        s.walkSourceZone = 0;
                    }
                    s.leg = s.travelLeg = rescue;
                    s.legIssued = false;
                    s.walkAdmitted = false;
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
            s.walkAdmitted = false;
            s.townWalkAttempted = false;
            s.walkSourceZone = 0;
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
                    s.walkAdmitted = false;
                    s.townWalkAttempted = false;
                    s.walkSourceZone = 0;
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
                s.walkAdmitted = false;
                s.townWalkAttempted = false;
                s.walkSourceZone = 0;
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
            if (s.ahGearItems[0])
                EquipAhGear(bot, botAI, s);
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
            s.walkAdmitted = false;
            s.townWalkAttempted = false;
            s.walkSourceZone = 0;
            s.reissues = 0;
            StoreState(guid, s);
            return true;
        }
        Stop const& st = s.plan.stops[s.stop];
        if (now - s.legMs > p.stopTimeoutMs)
        {
            LOG_INFO("playerbots", "[Errands] bot={} skip stop spawn={} entry={} ops={} (timeout)", bot->GetName(),
                     st.spawn, st.entry, st.ops);
            if ((st.ops & OpSell) && RetryTimedOutSeller(*town, team, s.plan, s.stop, s.sellerRetry))
            {
                Stop const& fallback = s.plan.stops[s.stop];
                s.legMs = now;
                s.legIssued = false;
                s.reissues = 0;
                if (info.GetStatus() != RPG_IDLE)
                    info.ChangeToIdle();
                LOG_INFO("playerbots", "[Errands] bot={} sell fallback spawn={} entry={} attempt={}", bot->GetName(),
                         fallback.spawn, fallback.entry, static_cast<uint32>(s.sellerRetry.count));
                StoreState(guid, s);
                return true;
            }
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
            // AutoWow.Supply.Outfit: a trainer / tool stop waits for the bot's pending grant (the world tick pays
            // or refuses it within AutoWow.Supply.TickMs; the stop timeout bounds the wait).
            // AutoWow.Supply.OutfitGear: the floors' food / weapon stops wait for it too.
            uint32 const grantOps = OpTrain | OpTool | (AutoWowSupply::OutfitGear() ? uint32(OpBuy | OpGear) : 0u);
            if ((st.ops & grantOps) && AutoWowSupply::Outfit() && AutoWowSupply::GrantPending(guid))
            {
                if (bot->isMoving())
                    bot->StopMoving();
                StoreState(guid, s);
                return true;
            }
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
    uint64 const sold0 = s.sold;

    // AutoWow.Supply.RouteCloth: a cohort non-tailor mails its cloth to the bag house rep instead of selling it
    // (the stacks are held back from this stop's sales; the mail runs on the world thread).
    if (AutoWowSupply::Enabled() && (st.ops & OpSell))
        AutoWowSupply::RouteCloth(bot);
    if (st.ops & OpSell)
    {
        uint64 const m0 = money();
        // Stock `rpg sell` event: vendor-usage items when any, else gray.
        botAI->DoSpecificAction("sell", Event("rpg action", AI_VALUE(bool, "can sell") ? "vendor" : "gray"), true);
        if (p.sellTradeGoods)
            botAI->DoSpecificAction("sell", Event("autowow errands", "autowow-trade"), true);
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
    // AutoWow.Guilds: TaxPct of this stop's vendor income goes to the bot's house guild bank (world thread).
    if (AutoWowGuilds::Enabled() && s.sold > sold0)
        AutoWowGuilds::QueueTax(bot, s.sold - sold0);
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
            // AutoWow.Supply.OutfitGear: food / drink skip the trainer reserve (the food floor).
            bool const floor = AutoWowSupply::OutfitGear() && (kind == KindFood || kind == KindWater);
            uint64 const keepCopper = floor ? 0 : reserve;
            uint64 const spendable = money() > keepCopper ? money() - keepCopper : 0;
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
            if (bought && floor)
            {
                s.done |= DoneFoodFloor;
                AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::FoodFloor, item, m0 > money() ? m0 - money() : 0);
            }
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
        // AutoWow.SelfCraft: `selfcraft learn` rows for the First Aid / Cooking lines, ranks and recipes.
        AutoWowSelfCraft::Known const known0 =
            AutoWowSelfCraft::Enabled() ? AutoWowSelfCraft::KnownOf(bot) : AutoWowSelfCraft::Known{};
        botAI->DoSpecificAction("trainer", Event("autowow errands", "learn"), true);
        if (AutoWowSelfCraft::Enabled())
            AutoWowSelfCraft::NoteLearned(bot, known0);
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
    // AutoWow.Supply.Outfit (only planned with the flag on): the missing tools this vendor sells, with the bot's own
    // gold (a grant may have topped it up); only for a skill it knows by now (the stock sell keeps a used tool).
    if (st.ops & OpTool)
    {
        VendorItemData const* list = npc->GetVendorItems();
        std::uint8_t const missing = BotMissingTools(bot);
        for (std::size_t i = 0; list && i < kTools; ++i)
        {
            uint32 const item = kToolItems[i];
            if (!(missing & (1u << i)))
                continue;
            uint32 slot = list->GetItemCount();
            for (uint32 v = 0; v < list->GetItemCount(); ++v)
                if (VendorItem const* vi = list->GetItem(v); vi && vi->item == item && !vi->ExtendedCost)
                {
                    slot = v;
                    break;
                }
            if (slot == list->GetItemCount())
                continue;
            uint64 const m0 = money();
            uint32 const before = bot->GetItemCount(item, false);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(item, false) > before)
            {
                uint64 const paid = m0 > money() ? m0 - money() : 0;
                s.spent += paid;
                s.done |= DoneTooled;
                AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::Outfit, item, paid);
            }
            else
            {
                s.done |= DoneSkipped;  // money short (grant refused), bags full: the core reports its own error
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::Refused, item, proto ? proto->BuyPrice : 0,
                                          "outfit");
            }
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
            // AutoWow.Supply.OutfitGear: a floor weapon skips the gear reserve (s.floorGear is 0 with the flag off).
            bool const floor = (s.floorGear >> k) & 1u;
            if (slot == list->GetItemCount() ||
                m0 < uint64(proto->BuyPrice) + (floor ? 0 : AutoWowGear::ReserveCopper(bot->GetLevel())))
            {
                s.done |= DoneSkipped;
                LOG_INFO("playerbots", "[Gear] bot={} skip item={} npc={} in_list={} money={}", bot->GetName(), item,
                         npc->GetEntry(), slot != list->GetItemCount(), m0);
                if (floor)
                    AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::Refused, item, proto->BuyPrice,
                                              "weapon_floor");
                continue;
            }
            uint32 const before = bot->GetItemCount(item, false);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(item, false) <= before)
            {
                if (floor)
                    AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::Refused, item, proto->BuyPrice,
                                              "weapon_floor");
                continue;  // bags full / stock: the core reports its own error
            }
            bought.push_back(item);
            if (m0 > money())
                s.spent += m0 - money();
            if (floor)
            {
                s.done |= DoneWeaponFloor;
                AutoWowSupply::EmitOutfit(bot, AutoWowSupply::Reason::WeaponFloor, item, m0 > money() ? m0 - money() : 0);
            }
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
    // AutoWow.Gear.AuctionUpgrades: the queued gear purchases are equipped at the errands' end (EquipAhGear).
    if (st.ops & OpAuction)
    {
        std::vector<uint32> ahGear;
        AutoWowTrade::VisitAuctioneer(botAI, bot, npc, ClassTrainBudgetCopper(bot->GetLevel()), &ahGear);
        for (std::size_t k = 0; k < ahGear.size() && k < s.ahGearItems.size(); ++k)
            s.ahGearItems[k] = ahGear[k];
    }
    // AutoWow.Errands.Mounts (only planned with the flag on): the rank through the trainer's own teach path (its
    // price, reputation discount and checks), then the mount bought unless held and learned by using it.
    if ((st.ops & OpRide) && s.rideTier)
    {
        uint32 const spell = kRiding[s.rideTier - 1].spell;
        uint64 const m0 = money();
        if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry()))
            trainer->TeachSpell(npc, bot, spell);
        uint64 const paid = m0 > money() ? m0 - money() : 0;
        bool const learned = bot->HasSpell(spell);
        s.spent += paid;
        s.done |= learned ? DoneRiding : DoneSkipped;
        if (paid)
            AutoWowTrade::NoteFee(bot, AutoWowTrade::FeeKind::Train, paid);
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitErrand(bot, "riding", MountLedgerFields(s.town, s.rideTier, "spell", spell, paid, learned));
        LOG_INFO("playerbots", "[Mounts] bot={} riding tier={} spell={} learned={} copper={} money={} skill={} lvl={}",
                 bot->GetName(), static_cast<uint32>(s.rideTier), spell, learned, paid, money(),
                 bot->GetPureSkillValue(kRidingSkill), bot->GetLevel());
    }
    if ((st.ops & OpMount) && s.mountItem)
    {
        uint64 const m0 = money();
        VendorItemData const* list = npc->GetVendorItems();
        if (list && !bot->HasItemCount(s.mountItem, 1))
            for (uint32 i = 0; i < list->GetItemCount(); ++i)
                if (VendorItem const* vi = list->GetItem(i); vi && vi->item == s.mountItem && !vi->ExtendedCost)
                {
                    bot->BuyItemFromVendorSlot(npc->GetGUID(), i, s.mountItem, 1, NULL_BAG, NULL_SLOT);
                    break;
                }
        uint64 const paid = m0 > money() ? m0 - money() : 0;
        s.spent += paid;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(s.mountItem);
        uint32 const mountSpell = proto && proto->Spells[1].SpellId > 0 ? uint32(proto->Spells[1].SpellId) : 0;
        UseHeldMounts(bot);
        bool const learned = mountSpell && bot->HasSpell(mountSpell);
        s.done |= learned ? DoneMount : DoneSkipped;
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitErrand(bot, "mount",
                                           MountLedgerFields(s.town, s.rideTier, "item", s.mountItem, paid, learned));
        LOG_INFO("playerbots", "[Mounts] bot={} mount tier={} item={} spell={} learned={} copper={} money={} lvl={}",
                 bot->GetName(), static_cast<uint32>(s.rideTier), s.mountItem, mountSpell, learned, paid, money(),
                 bot->GetLevel());
    }
}
