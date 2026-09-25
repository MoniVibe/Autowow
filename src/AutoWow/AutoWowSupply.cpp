/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Supply runtime (policy: AutoWow/SupplyPolicy.h). The overlord, the rep -> artisan feed, delivery,
// pay and work XP run on the world thread (WorldUpdate; mail, the guild bank and XP are thread-unsafe core
// state and the world thread never runs alongside the map updates, AutoWowGuilds idiom). The role bots' own
// walking, mailbox, trainer, vendor, craft and auction steps run on their map threads (NewRpgSupply.cpp) and
// share the per-team view below under gLock.

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SupplyPolicy.h"
#include "Trainer.h"

namespace AutoWowSupply
{
namespace
{
// V1 product chain (3.3.5 ids, checked against item_template / trainer_spell): Linen Bag 4238 (6 slots) from
// spell 3755 (tailoring 45, trainer-taught); Bolt of Linen Cloth 2996 from spell 2963 (learned with the
// skill); Coarse Thread 2320 (trade-supplies vendors). Reagent counts are read from the spells at load.
constexpr std::uint32_t kBagSpell = 3755, kBoltSpell = 2963, kThreadItem = 2320;
constexpr std::size_t kMaxHeld = 4096;      // cloth stacks held back from sale at once
constexpr std::uint32_t kStationYards = 400;  // stations are searched this far from home

// Read-only after LoadConfig.
std::size_t gBagHouse = 0;
std::string gBagHouseName;
std::unordered_map<std::uint32_t, RoleInfo> gRoles;
std::array<Stations, 2> gStations;  // [0] alliance, [1] horde
std::array<std::uint32_t, 2> gArtisan{};  // the bag house artisan per team
Recipe gRecipe;
std::uint32_t gBagItem = 0, gBoltItem = 0, gBagSell = 0, gBagSlots = 0, gThreadPrice = 0;
std::vector<std::uint32_t> gLearn;

// Shared: the world thread writes, role bots' map threads and donors read / take.
struct TeamState
{
    std::uint32_t version = kStateVersion;
    std::uint32_t orderId = 0;
    std::uint32_t remaining = 0;
    std::uint32_t surplus = 0;
    std::uint32_t clothRoom = 0;
    std::uint64_t artisanWant = 0;
    bool artisanCanBag = false;
};
std::mutex gLock;
std::array<TeamState, 2> gTeams;
std::uint32_t gNextOrderId = 0;  // run-scoped, never reused (both teams)
std::unordered_set<std::uint32_t> gHeld;

// World thread only.
std::uint32_t gTickAcc = 0;
std::uint32_t gOverlordAcc = 0;
bool gFirstOverlord = true;

std::size_t T(bool alliance) { return alliance ? 0 : 1; }
ObjectGuid PlayerGuid(std::uint32_t low) { return ObjectGuid::Create<HighGuid::Player>(low); }
std::uint32_t Low(Player* p) { return static_cast<std::uint32_t>(p->GetGUID().GetCounter()); }
Player* Online(std::uint32_t guid) { return guid ? ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid)) : nullptr; }

// Backpack and equipped bags' contents (never the equipped bags themselves or the bank).
template <typename F>
void ForEachLoose(Player* p, F&& fn)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            fn(item);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = p->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                if (Item* item = b->GetItemByPos(slot))
                    fn(item);
}

std::vector<Stack> LooseStacks(Player* p, std::uint32_t entry)
{
    std::vector<Stack> out;
    if (p)
        ForEachLoose(p, [&](Item* item) {
            if (item->GetEntry() == entry)
                out.push_back({static_cast<std::uint32_t>(item->GetGUID().GetCounter()), item->GetCount()});
        });
    return out;
}

std::uint32_t Loose(Player* p, std::uint32_t entry)
{
    std::uint32_t n = 0;
    for (Stack const& s : LooseStacks(p, entry))
        n += s.count;
    return n;
}

// Units of `entry` waiting in the player's mailbox (delivered or still in transit).
std::uint32_t InMail(Player* p, std::uint32_t entry)
{
    std::uint32_t n = 0;
    if (p)
        for (Mail const* m : p->GetMails())
            if (m && m->state != MAIL_STATE_DELETED)
                for (MailItemInfo const& mi : m->items)
                    if (mi.item_template == entry)
                        if (Item* item = p->GetMItem(mi.item_guid))
                            n += item->GetCount();
    return n;
}

std::uint32_t HeldUnits(Player* p, std::uint32_t entry) { return Loose(p, entry) + InMail(p, entry); }

bool GeneralBag(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER;
}

// The member's bag want (SupplyPolicy Member).
Member MemberOf(Player* m)
{
    Member out;
    out.guid = Low(m);
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Bag* bag = m->GetBagByPos(slot);
        if (!bag)
            ++out.empty;
        else if (GeneralBag(bag->GetTemplate()) && bag->GetBagSize() < gBagSlots)
            ++out.smaller;
    }
    ForEachLoose(m, [&](Item* item) {
        if (GeneralBag(item->GetTemplate()) && item->GetTemplate()->ContainerSlots >= gBagSlots)
            ++out.incoming;
    });
    for (Mail const* mail : m->GetMails())
        if (mail && mail->state != MAIL_STATE_DELETED)
            for (MailItemInfo const& mi : mail->items)
                if (GeneralBag(sObjectMgr->GetItemTemplate(mi.item_template)))
                    ++out.incoming;
    return out;
}

bool IsRole(std::uint32_t guid) { return gRoles.count(guid) != 0; }

// The online cohort members of the team (roles excluded), ranked by want.
std::vector<Member> RankedMembers(bool alliance)
{
    std::vector<Member> members;
    for (AutoWowGuilds::GuidRange const& r : AutoWowGuilds::Cohort())
        for (std::uint64_t g = r.lo; g <= r.hi; ++g)
        {
            std::uint32_t const guid = static_cast<std::uint32_t>(g);
            if (IsRole(guid))
                continue;
            Player* m = Online(guid);
            if (m && m->IsInWorld() && (m->GetTeamId() == TEAM_ALLIANCE) == alliance)
                members.push_back(MemberOf(m));
        }
    return RankNeeds(std::move(members));
}

std::uint32_t XpFor(std::uint32_t configured, Player* p)
{
    return WorkXp(configured, sObjectMgr->GetXPForLevel(p->GetLevel()));
}

void GrantXp(Player* p, std::uint32_t xp, std::uint32_t oid, std::uint32_t item, std::uint32_t count)
{
    std::uint32_t const lvl = p->GetLevel(), before = p->GetUInt32Value(PLAYER_XP);
    p->GiveXP(xp, nullptr);
    LOG_INFO("playerbots", "[Supply] xp player={} xp={} lvl={}->{} xp_bar={}->{}", p->GetName(), xp, lvl,
             p->GetLevel(), before, p->GetUInt32Value(PLAYER_XP));
    Emit(p, Reason::Xp, oid, item, count, xp, 0, Low(p));
}

// World thread: mail a cloth donation (one row per cloth entry) and release the held stacks.
class DonateOperation : public PlayerbotOperation
{
public:
    DonateOperation(ObjectGuid bot, bool alliance, std::vector<std::uint32_t> guids)
        : bot_(bot), alliance_(alliance), guids_(std::move(guids))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        bool ok = false;
        if (bot)
        {
            std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gBagHouse, alliance_);
            std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
            std::array<std::uint32_t, std::size(kCloth)> units{};
            for (std::uint32_t const g : guids_)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < units.size(); ++k)
                        if (item->GetEntry() == kCloth[k])
                            units[k] += item->GetCount();
            ok = rep && rep != Low(bot) && AutoWowGuilds::SendItems(Low(bot), rep, guids_, "AutoWoW cloth");
            for (std::size_t k = 0; k < units.size(); ++k)
                if (units[k])
                    Emit(bot, ok ? Reason::Donate : Reason::Refused, 0, kCloth[k], units[k], 0, Low(bot), rep,
                         ok ? nullptr : "donate");
        }
        std::lock_guard<std::mutex> guard(gLock);
        for (std::uint32_t const g : guids_)
            gHeld.erase(g);
        return ok;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyDonate"; }

private:
    ObjectGuid bot_;
    bool alliance_;
    std::vector<std::uint32_t> guids_;
};

std::vector<std::uint32_t> Guids(std::vector<Stack> const& stacks, std::uint32_t maxStacks)
{
    std::vector<Stack> sorted = stacks;
    std::sort(sorted.begin(), sorted.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < sorted.size() && out.size() < maxStacks; ++i)
        out.push_back(sorted[i].guid);
    return out;
}

// One world tick of a team's chain.
void TeamTick(bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    std::size_t const t = T(alliance);
    std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gBagHouse, alliance);
    std::uint32_t const repGuid = gid ? AutoWowGuilds::RepOf(gid) : 0;
    Player* rep = Online(repGuid);
    Player* art = Online(gArtisan[t]);
    if (rep && !rep->IsInWorld())
        rep = nullptr;
    if (art && !art->IsInWorld())
        art = nullptr;
    TeamState ts;
    {
        std::lock_guard<std::mutex> guard(gLock);
        ts = gTeams[t];
    }

    // Cloth room for donors: the rep's cloth (bags + mailbox) under ClothCap, while it has bag space.
    std::uint32_t stock = 0;
    for (std::uint32_t const c : kCloth)
        stock += HeldUnits(rep, c);
    ts.clothRoom = ClothRoom(stock, p.clothCap, rep && AutoWowGuilds::RepFreeSlots(rep) > 0);

    // Artisan -> rep: finished bags, paid per bag from the treasury, work XP per bag.
    if (art && rep && art != rep)
        if (std::vector<std::uint32_t> const bags = Guids(LooseStacks(art, gBagItem), kMaxMailStacks); !bags.empty())
        {
            std::uint32_t const n = static_cast<std::uint32_t>(bags.size());
            if (AutoWowGuilds::SendItems(Low(art), repGuid, bags, "AutoWoW bags"))
            {
                Emit(art, Reason::Deliver, ts.orderId, gBagItem, n, 0, Low(art), repGuid);
                ts.remaining -= std::min(ts.remaining, n);
                std::uint64_t const pay = BagPay(gBagSell, p.bagPayPct, n);
                bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
                Emit(art, paid ? Reason::Pay : Reason::Refused, ts.orderId, gBagItem, n, pay, 0, Low(art),
                     paid ? nullptr : "pay");
                GrantXp(art, XpFor(p.workXpPerItem, art) * n, ts.orderId, gBagItem, n);
            }
            else
                Emit(art, Reason::Refused, ts.orderId, gBagItem, n, 0, Low(art), repGuid, "deliver");
        }

    std::uint32_t const artLinen = HeldUnits(art, kLinen), artBolts = HeldUnits(art, gBoltItem), artBags = HeldUnits(art, gBagItem);

    // Rep -> artisan: linen for the order (or the skill-up stock), and the copper the artisan asked for.
    if (art && rep && art != rep)
    {
        std::uint32_t const shortLinen =
            LinenShort(gRecipe, ts.remaining, ts.artisanCanBag, artLinen, artBolts, artBags, p.skillupCloth);
        std::vector<std::uint32_t> const stacks = PickStacks(LooseStacks(rep, kLinen), shortLinen);
        if (!stacks.empty())
        {
            std::uint32_t units = 0;
            for (Stack const& s : LooseStacks(rep, kLinen))
                if (std::find(stacks.begin(), stacks.end(), s.guid) != stacks.end())
                    units += s.count;
            bool const ok = AutoWowGuilds::SendItems(repGuid, Low(art), stacks, "AutoWoW linen");
            Emit(rep, ok ? Reason::Feed : Reason::Refused, ts.orderId, kLinen, units, 0, repGuid, Low(art),
                 ok ? nullptr : "feed");
        }
    }
    if (art && ts.artisanWant)
    {
        bool const paid = gid && AutoWowGuilds::Pay(gid, art, ts.artisanWant);
        Emit(art, paid ? Reason::Feed : Reason::Refused, ts.orderId, 0, 0, ts.artisanWant, 0, Low(art),
             paid ? nullptr : "feed_copper");
        ts.artisanWant = 0;
    }

    // Overlord: need scan and order size.
    std::vector<Member> ranked;
    if (overlord || rep)
        ranked = RankedMembers(alliance);
    std::uint32_t const repBags = Loose(rep, gBagItem);
    if (overlord)
    {
        std::uint32_t const want = TotalWant(ranked);
        std::uint32_t const finished = HeldUnits(rep, gBagItem) + artBags;
        std::uint32_t const craftable = BagsFrom(gRecipe, HeldUnits(rep, kLinen) + artLinen, artBolts);
        std::uint32_t const n = OrderSize(want, finished, craftable, p.maxOrder);
        if (n && !ts.remaining)
        {
            std::lock_guard<std::mutex> guard(gLock);
            ts.orderId = ++gNextOrderId;
        }
        ts.remaining = n;
        ts.surplus = Surplus(want, repBags, p.surplusKeep);
        LOG_INFO("playerbots", "[Supply] overlord team={} gid={} rep={} artisan={} members_wanting={} want={} "
                 "finished={} craftable={} order={} oid={} surplus={} cloth_room={}", alliance ? "alliance" : "horde",
                 gid, repGuid, gArtisan[t], ranked.size(), want, finished, craftable, n, ts.orderId, ts.surplus,
                 ts.clothRoom);
        if (n)
            if (Player* who = art ? art : rep)
                Emit(who, Reason::Order, ts.orderId, gBagItem, n, 0, 0, gArtisan[t]);
    }

    // Rep -> members: the rep's loose bags to the ranked members; rep XP per deal.
    if (rep && repBags)
    {
        std::vector<Stack> bags = LooseStacks(rep, gBagItem);
        std::sort(bags.begin(), bags.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
        std::size_t next = 0;
        for (Delivery const& d : PlanDeliveries(ranked, repBags))
        {
            std::vector<std::uint32_t> give;
            for (; next < bags.size() && give.size() < d.bags; ++next)
                give.push_back(bags[next].guid);
            if (give.empty())
                break;
            if (!AutoWowGuilds::SendItems(repGuid, d.guid, give, "AutoWoW bag"))
            {
                Emit(rep, Reason::Refused, ts.orderId, gBagItem, static_cast<std::uint32_t>(give.size()), 0, repGuid,
                     d.guid, "deliver");
                continue;
            }
            Emit(rep, Reason::Deliver, ts.orderId, gBagItem, static_cast<std::uint32_t>(give.size()), 0, repGuid,
                 d.guid);
            GrantXp(rep, XpFor(p.repXpPerDeal, rep), ts.orderId, gBagItem, static_cast<std::uint32_t>(give.size()));
        }
    }

    std::lock_guard<std::mutex> guard(gLock);
    TeamState& out = gTeams[t];
    out.orderId = ts.orderId;
    out.remaining = ts.remaining;
    out.clothRoom = ts.clothRoom;
    if (overlord)
        out.surplus = ts.surplus;
    out.artisanWant = ts.artisanWant;  // no map update runs during the world tick: nothing set since the copy
}

void BuildStations(bool alliance, Home const& home)
{
    Stations& st = gStations[T(alliance)];
    st = Stations{};
    std::array<std::int64_t, 4> best{};
    std::array<std::uint64_t, 4> bestSpawn{};
    std::int64_t const r2 = std::int64_t(kStationYards) * kStationYards;
    auto consider = [&](std::size_t k, Station& s, std::uint64_t spawn, std::uint32_t entry, float x, float y, float z)
    {
        std::int64_t const dx = std::int64_t(x) - home.x, dy = std::int64_t(y) - home.y, d2 = dx * dx + dy * dy;
        if (d2 > r2 || (s.entry && (d2 > best[k] || (d2 == best[k] && spawn > bestSpawn[k]))))
            return;
        best[k] = d2;
        bestSpawn[k] = spawn;
        s = {entry, home.map, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
    };
    std::uint32_t const hostileMask = alliance ? FACTION_MASK_ALLIANCE : FACTION_MASK_HORDE;
    for (auto const& [spawn, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != home.map)
            continue;
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
        FactionTemplateEntry const* f = ct ? sFactionTemplateStore.LookupEntry(ct->faction) : nullptr;
        if (!f || (f->hostileMask & hostileMask))
            continue;
        uint32 npcflag = 0, unitFlags = 0, dynamicFlags = 0;
        ObjectMgr::ChooseCreatureFlags(ct, npcflag, unitFlags, dynamicFlags, &data);
        if ((npcflag & UNIT_NPC_FLAG_TRAINER_PROFESSION))
            if (Trainer::Trainer* tr = sObjectMgr->GetTrainer(data.id))
                for (Trainer::Spell const& s : tr->GetSpells())
                    if (s.SpellId == kBagSpell)
                    {
                        consider(0, st.trainer, spawn, data.id, data.posX, data.posY, data.posZ);
                        break;
                    }
        if ((npcflag & UNIT_NPC_FLAG_VENDOR))
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* vi : list->m_items)
                    if (vi && vi->item == kThreadItem && !vi->ExtendedCost)
                    {
                        consider(1, st.threadVendor, spawn, data.id, data.posX, data.posY, data.posZ);
                        break;
                    }
        if (npcflag & UNIT_NPC_FLAG_AUCTIONEER)
            consider(2, st.auctioneer, spawn, data.id, data.posX, data.posY, data.posZ);
    }
    for (auto const& [spawn, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* gt = data.mapid == home.map ? sObjectMgr->GetGameObjectTemplate(data.id) : nullptr;
        if (gt && gt->type == GAMEOBJECT_TYPE_MAILBOX)
            consider(3, st.mailbox, spawn, data.id, data.posX, data.posY, data.posZ);
    }
    LOG_INFO("server.loading", "[Supply] {} home map={} ({},{}) stations: mailbox={} trainer={} thread_vendor={} "
             "auctioneer={}", alliance ? "alliance" : "horde", home.map, home.x, home.y, st.mailbox.entry,
             st.trainer.entry, st.threadVendor.entry, st.auctioneer.entry);
}

// Reagent count of `item` in `spell` (0 = not a reagent).
std::uint32_t ReagentCount(SpellInfo const* s, std::uint32_t item)
{
    for (std::size_t i = 0; s && i < MAX_SPELL_REAGENTS; ++i)
        if (s->Reagent[i] > 0 && std::uint32_t(s->Reagent[i]) == item)
            return s->ReagentCount[i];
    return 0;
}

std::uint32_t Output(SpellInfo const* s)
{
    for (std::size_t i = 0; s && i < MAX_SPELL_EFFECTS; ++i)
        if (s->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
            return s->Effects[i].ItemType;
    return 0;
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Supply.Enable", false);
    Params& p = detail::gParams;
    p.tickMs = std::max<std::uint32_t>(1000, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.TickMs", 30000));
    p.overlordMs = std::max<std::uint32_t>(p.tickMs, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OverlordMs", 300000));
    p.routeCloth = sConfigMgr->GetOption<bool>("AutoWow.Supply.RouteCloth", false);
    p.clothCap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ClothCap", 200);
    p.maxOrder = std::min<std::uint32_t>(kMaxMailStacks, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.MaxOrder", 6));
    p.bagPayPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BagPayPct", 200);
    p.workXpPerItem = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.WorkXpPerItem", 0);
    p.repXpPerDeal = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.RepXpPerDeal", 0);
    p.surplusKeep = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.SurplusKeep", 4);
    p.homeYards = std::max<std::uint32_t>(5, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.HomeYards", 25));
    p.skillupCloth = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.SkillupCloth", 40);
    gRoles.clear();
    gArtisan = {};
    gLearn.clear();
    {
        std::lock_guard<std::mutex> guard(gLock);
        gTeams = {};
        gHeld.clear();
    }
    if (!detail::gEnabled)
        return;
    auto disable = [](std::string const& why)
    {
        LOG_ERROR("server.loading", "[Supply] disabled: {}", why);
        detail::gEnabled = false;
    };
    if (!AutoWowGuilds::Enabled())
        return disable("needs AutoWow.Guilds.Enable");

    gBagHouseName = sConfigMgr->GetOption<std::string>("AutoWow.Supply.House", "Weavers");
    std::vector<AutoWowGuilds::House> const& houses = AutoWowGuilds::Houses();
    gBagHouse = houses.size();
    for (std::size_t i = 0; i < houses.size(); ++i)
        if (houses[i].name == gBagHouseName)
            gBagHouse = i;
    if (gBagHouse == houses.size())
        return disable("AutoWow.Supply.House '" + gBagHouseName + "' is not an AutoWow.Guilds.Houses house");

    SpellInfo const* bag = sSpellMgr->GetSpellInfo(kBagSpell);
    SpellInfo const* bolt = sSpellMgr->GetSpellInfo(kBoltSpell);
    gBagItem = Output(bag);
    gBoltItem = Output(bolt);
    ItemTemplate const* bagProto = sObjectMgr->GetItemTemplate(gBagItem);
    ItemTemplate const* threadProto = sObjectMgr->GetItemTemplate(kThreadItem);
    gRecipe.clothPerBolt = ReagentCount(bolt, kLinen);
    gRecipe.boltsPerBag = ReagentCount(bag, gBoltItem);
    gRecipe.threadPerBag = ReagentCount(bag, kThreadItem);
    if (!bagProto || !threadProto || !gBoltItem || !gRecipe.clothPerBolt || !gRecipe.boltsPerBag)
        return disable("bag recipe spells / items missing");
    gBagSell = bagProto->SellPrice;
    gBagSlots = bagProto->ContainerSlots;
    gThreadPrice = threadProto->BuyPrice / std::max<std::uint32_t>(1, threadProto->BuyCount);

    std::string const learn = sConfigMgr->GetOption<std::string>("AutoWow.Supply.Artisan.Learn", "3911,3912,3913,12181,3755");
    AutoWowGuilds::detail::Split(learn, ',', [](std::string_view s) {
        std::uint32_t id = 0;
        if (AutoWowGuilds::detail::ParseU32(s, id) && id)
            gLearn.push_back(id);
    });

    for (bool const alliance : {true, false})
    {
        std::string const team = alliance ? "Alliance" : "Horde";
        Home home;
        std::string const homeText = sConfigMgr->GetOption<std::string>(
            "AutoWow.Supply.Home." + team, alliance ? "0,-8813,650,95" : "1,1660,-4436,18");
        if (!ParseHome(homeText, home))
            LOG_ERROR("server.loading", "[Supply] bad AutoWow.Supply.Home.{} '{}': no home, {} roles idle", team,
                      homeText, team);
        else
            BuildStations(alliance, home);
        for (std::size_t i = 0; i < houses.size(); ++i)
        {
            RoleInfo info;
            info.alliance = alliance;
            info.bagHouse = i == gBagHouse;
            info.home = home;
            if (std::uint32_t const rep = AutoWowGuilds::ConfiguredRep(i, alliance))
            {
                info.role = Role::Rep;
                gRoles[rep] = info;
            }
            std::uint32_t const artisan = sConfigMgr->GetOption<std::uint32_t>(
                "AutoWow.Supply.Artisan." + houses[i].name + "." + team, 0, false);
            if (artisan)
            {
                info.role = Role::Artisan;
                gRoles[artisan] = info;
                if (info.bagHouse)
                    gArtisan[T(alliance)] = artisan;
            }
        }
    }
    LOG_INFO("server.loading", "[Supply] enabled: house={} bag={} ({} slots, sell {}) bolt={} recipe cloth/bolt={} "
             "bolts/bag={} thread/bag={} thread_price={} roles={} artisan A={} H={} learn={}", gBagHouseName, gBagItem,
             gBagSlots, gBagSell, gBoltItem, gRecipe.clothPerBolt, gRecipe.boltsPerBag, gRecipe.threadPerBag,
             gThreadPrice, gRoles.size(), gArtisan[0], gArtisan[1], gLearn.size());
}

void WorldUpdate(std::uint32_t diff)
{
    if (!detail::gEnabled)
        return;
    Params const& p = detail::gParams;
    gTickAcc += diff;
    if (gTickAcc < p.tickMs)
        return;
    gOverlordAcc += gTickAcc;
    gTickAcc = 0;
    bool const overlord = gFirstOverlord || gOverlordAcc >= p.overlordMs;
    if (overlord)
    {
        gOverlordAcc = 0;
        gFirstOverlord = false;
    }
    TeamTick(true, overlord);
    TeamTick(false, overlord);
}

RoleInfo RoleOf(std::uint32_t guid)
{
    auto const it = gRoles.find(guid);
    return it == gRoles.end() ? RoleInfo{} : it->second;
}

Stations const& StationsOf(bool alliance) { return gStations[T(alliance)]; }

TeamView ViewOf(bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    TeamState const& s = gTeams[T(alliance)];
    TeamView v;
    v.orderId = s.orderId;
    v.remaining = s.remaining;
    v.surplus = s.surplus;
    return v;
}

Recipe const& BagRecipe() { return gRecipe; }
std::uint32_t BagItem() { return gBagItem; }
std::uint32_t BagSpell() { return kBagSpell; }
std::uint32_t BoltItem() { return gBoltItem; }
std::uint32_t BoltSpell() { return kBoltSpell; }
std::uint32_t ThreadItem() { return kThreadItem; }
std::uint32_t ThreadPrice() { return gThreadPrice; }
std::string const& BagHouseName() { return gBagHouseName; }
std::vector<std::uint32_t> const& LearnSpells() { return gLearn; }

void SetArtisanWant(bool alliance, std::uint64_t copper, bool canBag)
{
    std::lock_guard<std::mutex> guard(gLock);
    gTeams[T(alliance)].artisanWant = copper;
    gTeams[T(alliance)].artisanCanBag = canBag;
}

void ClearSurplus(bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    gTeams[T(alliance)].surplus = 0;
}

void Emit(Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count, std::uint64_t copper,
          std::uint32_t from, std::uint32_t to, char const* op)
{
    LOG_INFO("playerbots", "[Supply] player={} {} house={} oid={} item={} count={} copper={} from={} to={}{}{}",
             p ? p->GetName() : "-", ReasonName(r), gBagHouseName, oid, item, count, copper, from, to,
             op ? " op=" : "", op ? op : "");
    if (p && AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSupply(p, ReasonName(r), LedgerFields(gBagHouseName, oid, item, count, copper, from, to, op));
}

void RouteCloth(Player* bot)
{
    Params const& p = detail::gParams;
    if (!detail::gEnabled || !bot)
        return;
    std::uint32_t const guid = Low(bot);
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    std::uint32_t room = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        room = gTeams[T(alliance)].clothRoom;
    }
    if (!RoutesCloth(p.routeCloth, AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) && !IsRole(guid),
                     bot->HasSkill(SKILL_TAILORING), room))
        return;
    std::vector<Stack> stacks;
    for (std::uint32_t const c : kCloth)
        for (Stack const& s : LooseStacks(bot, c))
            stacks.push_back(s);
    std::vector<std::uint32_t> pick = PickStacks(stacks, room);
    if (pick.empty())
        return;
    std::uint32_t units = 0;
    for (Stack const& s : stacks)
        if (std::find(pick.begin(), pick.end(), s.guid) != pick.end())
            units += s.count;
    {
        std::lock_guard<std::mutex> guard(gLock);
        TeamState& ts = gTeams[T(alliance)];
        if (!ts.clothRoom || gHeld.size() + pick.size() > kMaxHeld)
            return;
        ts.clothRoom -= std::min(ts.clothRoom, units);
        gHeld.insert(pick.begin(), pick.end());
    }
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<DonateOperation>(bot->GetGUID(), alliance, std::move(pick)));
}

bool HeldForDonation(std::uint32_t itemGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    return gHeld.count(itemGuid) != 0;
}
}  // namespace AutoWowSupply
