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

#include "AuctionHouseMgr.h"
#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Playerbots.h"
#include "SelfCraftPolicy.h"
#include "SquadPolicy.h"
#include "SupplyPolicy.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

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
std::array<Station, 2> gBagVendor;  // the kPouch vendor near each home (artisan make-room)
std::array<std::uint32_t, 2> gArtisan{};  // the bag house artisan per team
Recipe gRecipe;
std::uint32_t gBagItem = 0, gBoltItem = 0, gBagSell = 0, gBagSlots = 0, gThreadPrice = 0;
std::vector<std::uint32_t> gLearn;
// Catalog lines with tierCount > 0 (index = Line; the bags entries stay unused), read-only after LoadConfig.
std::array<std::size_t, kLineCount> gLineHouse{};
std::array<std::string, kLineCount> gLineHouseName;
std::array<std::array<std::uint32_t, 2>, kLineCount> gLineArtisan{};
std::array<std::array<Stations, 2>, kLineCount> gLineStations;
std::array<std::vector<std::uint32_t>, kLineCount> gLineLearn;
std::array<std::vector<std::uint32_t>, kLineCount> gLineRoute;

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
    // Tiers only (TierTick):
    std::array<std::uint32_t, kTierCount> clothRooms{};
    std::uint8_t product = kNoTier;
    std::uint32_t productWant = 0;  // members' want for the product tier (overlord)
    std::uint8_t goal = kNoTier;    // PickGoal (overlord): the tier whose cloth the squad gathers first
    std::uint8_t skillup = kNoTier;
    bool skillupBag = false;
    std::uint32_t artisanSkill = 0;
    std::array<std::uint32_t, kTierCount> surplusBags{};
    std::vector<MarketWant> buy;
};
struct LineState
{
    LineView v;
    std::uint64_t artisanWant = 0;
};
std::mutex gLock;
std::array<TeamState, 2> gTeams;
std::array<std::array<LineState, 2>, kLineCount> gLines;  // [line][team]
std::uint32_t gNextOrderId = 0;  // run-scoped, never reused (both teams)
std::unordered_set<std::uint32_t> gHeld;

// Raw materials (RouteRaw, lane G): kind 0 = ore -> AutoWow.Supply.House.Ore, 1 = leather -> .House.Leather.
constexpr std::size_t kRawKinds = 2, kRawItems = 3;
static_assert(std::size(kOre) == kRawItems && std::size(kLeather) == kRawItems);
constexpr std::uint32_t const* kRawLists[kRawKinds] = {kOre, kLeather};
constexpr char const* kRawKeys[kRawKinds] = {"Ore", "Leather"};
constexpr char const* kRawDefaultHouse[kRawKinds] = {"Smiths", "Tanners"};
std::array<std::size_t, kRawKinds> gRawHouse{};  // read-only after LoadConfig; houses.size() = kind off
std::array<std::string, kRawKinds> gRawHouseName;
std::array<std::array<std::array<std::uint32_t, kRawItems>, kRawKinds>, 2> gRawRooms{};  // [team][kind][item], gLock

// Outfit (AutoWow.Supply.Outfit): pending grant requests, guid -> need copper (gLock; map threads add, the world
// tick takes). Per-bot level windows and per-team hour budgets are world thread only (in memory, reset on restart).
std::unordered_map<std::uint32_t, std::uint64_t> gGrantRequests;
std::unordered_map<std::uint32_t, GrantWindow> gGrantWindows;
std::array<GrantBudget, 2> gGrantBudgets{};

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

// The member's bag want (SupplyPolicy Member) for a product bag of `slots`.
Member MemberOf(Player* m, std::uint32_t slots)
{
    Member out;
    out.guid = Low(m);
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Bag* bag = m->GetBagByPos(slot);
        if (!bag)
            ++out.empty;
        else if (GeneralBag(bag->GetTemplate()) && bag->GetBagSize() < slots)
            ++out.smaller;
    }
    ForEachLoose(m, [&](Item* item) {
        if (GeneralBag(item->GetTemplate()) && item->GetTemplate()->ContainerSlots >= slots)
            ++out.incoming;
    });
    for (Mail const* mail : m->GetMails())
        if (mail && mail->state != MAIL_STATE_DELETED)
            for (MailItemInfo const& mi : mail->items)
                if (GeneralBag(sObjectMgr->GetItemTemplate(mi.item_template)))
                    ++out.incoming;
    return out;
}

// A working role bot (an apprentice artisan is an ordinary member, ActiveRoleOf).
bool IsRole(Player* p) { return ActiveRoleOf(p).role != Role::None; }

// The house artisan when it works: online, in world and past its apprentice phase; else nullptr (the chain then
// treats it as offline: no feed, pay or deliveries while it adventures).
Player* Working(Player* art) { return art && art->IsInWorld() && IsRole(art) ? art : nullptr; }

// The online cohort members of the team (roles excluded), unranked.
std::vector<Member> Members(bool alliance, std::uint32_t slots)
{
    std::vector<Member> members;
    for (AutoWowGuilds::GuidRange const& r : AutoWowGuilds::Cohort())
        for (std::uint64_t g = r.lo; g <= r.hi; ++g)
        {
            Player* m = Online(static_cast<std::uint32_t>(g));
            if (m && m->IsInWorld() && (m->GetTeamId() == TEAM_ALLIANCE) == alliance && !IsRole(m))
                members.push_back(MemberOf(m, slots));
        }
    return members;
}

// The same, ranked by want.
std::vector<Member> RankedMembers(bool alliance, std::uint32_t slots) { return RankNeeds(Members(alliance, slots)); }
std::vector<Member> RankedMembers(bool alliance) { return RankedMembers(alliance, gBagSlots); }

std::uint32_t XpFor(std::uint32_t configured, Player* p)
{
    return WorkXp(configured, sObjectMgr->GetXPForLevel(p->GetLevel()));
}

void GrantXp(Player* p, std::uint32_t xp, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
             Line line = Line::Bags)
{
    std::uint32_t const lvl = p->GetLevel(), before = p->GetUInt32Value(PLAYER_XP);
    p->GiveXP(xp, nullptr);
    LOG_INFO("playerbots", "[Supply] xp player={} xp={} lvl={}->{} xp_bar={}->{}", p->GetName(), xp, lvl,
             p->GetLevel(), before, p->GetUInt32Value(PLAYER_XP));
    EmitLine(line, p, Reason::Xp, oid, item, count, xp, 0, Low(p));
}

// World thread: mail a donation (cloth to the bag house, or a catalog line's routed reagents to its house; one
// row per entry) and release the held stacks.
class DonateOperation : public PlayerbotOperation
{
public:
    DonateOperation(ObjectGuid bot, bool alliance, std::vector<std::uint32_t> guids, Line line = Line::Bags)
        : bot_(bot), alliance_(alliance), guids_(std::move(guids)), line_(line)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        bool ok = false;
        if (bot)
        {
            bool const bags = line_ == Line::Bags;
            std::size_t const house = bags ? gBagHouse : gLineHouse[static_cast<std::size_t>(line_)];
            std::uint32_t const gid = AutoWowGuilds::HouseGuildId(house, alliance_);
            std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
            // Tiers route wool and silk too; off, only linen stacks are ever picked, so the extra rows stay 0.
            std::vector<std::uint32_t> const entries = bags ? std::vector<std::uint32_t>(std::begin(kTierCloth),
                                                                                         std::end(kTierCloth))
                                                            : gLineRoute[static_cast<std::size_t>(line_)];
            std::vector<std::uint32_t> units(entries.size(), 0);
            for (std::uint32_t const g : guids_)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < units.size(); ++k)
                        if (item->GetEntry() == entries[k])
                            units[k] += item->GetCount();
            ok = rep && rep != Low(bot) &&
                 AutoWowGuilds::SendItems(Low(bot), rep, guids_, bags ? "AutoWoW cloth" : "AutoWoW materials");
            for (std::size_t k = 0; k < units.size(); ++k)
                if (units[k])
                    EmitLine(line_, bot, ok ? Reason::Donate : Reason::Refused, 0, entries[k], units[k], 0, Low(bot),
                             rep, ok ? nullptr : "donate");
            // AutoWow.Squad: a squad member's delivery (`squad` deliver row).
            for (std::size_t k = 0; ok && AutoWowSquad::Enabled() && k < units.size(); ++k)
                if (units[k])
                    AutoWowSquad::NoteDelivered(bot, entries[k], units[k], rep);
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
    Line line_;
};

// World thread: mail a raw-material donation (ore / leather, RouteRaw) to its kind's house rep, one `supply` row per
// entry (line "raw"), and release the held stacks.
class RawDonateOperation : public PlayerbotOperation
{
public:
    RawDonateOperation(ObjectGuid bot, bool alliance, std::size_t kind, std::vector<std::uint32_t> guids)
        : bot_(bot), alliance_(alliance), kind_(kind), guids_(std::move(guids))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        bool ok = false;
        if (bot)
        {
            std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gRawHouse[kind_], alliance_);
            std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
            std::array<std::uint32_t, kRawItems> units{};
            for (std::uint32_t const g : guids_)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < kRawItems; ++k)
                        if (item->GetEntry() == kRawLists[kind_][k])
                            units[k] += item->GetCount();
            ok = rep && rep != Low(bot) && AutoWowGuilds::SendItems(Low(bot), rep, guids_, "AutoWoW materials");
            for (std::size_t k = 0; k < kRawItems; ++k)
            {
                if (!units[k])
                    continue;
                Reason const r = ok ? Reason::Donate : Reason::Refused;
                LOG_INFO("playerbots", "[Supply] player={} {} house={} item={} count={} from={} to={}{}", bot->GetName(),
                         ReasonName(r), gRawHouseName[kind_], kRawLists[kind_][k], units[k], Low(bot), rep,
                         ok ? "" : " op=donate");
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::EmitSupply(bot, ReasonName(r),
                                                   LedgerFields(gRawHouseName[kind_], 0, kRawLists[kind_][k], units[k], 0,
                                                                Low(bot), rep, ok ? nullptr : "donate") +
                                                       ",\"line\":\"raw\"");
                if (ok && AutoWowSquad::Enabled())
                    AutoWowSquad::NoteDelivered(bot, kRawLists[kind_][k], units[k], rep);
            }
        }
        std::lock_guard<std::mutex> guard(gLock);
        for (std::uint32_t const g : guids_)
            gHeld.erase(g);
        return ok;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyRawDonate"; }

private:
    ObjectGuid bot_;
    bool alliance_;
    std::size_t kind_;
    std::vector<std::uint32_t> guids_;
};

// World thread (RouteRaw): each raw item's donor room at its kind's house rep, under RawCap.
void RawTick(bool alliance)
{
    Params const& p = detail::gParams;
    std::array<std::array<std::uint32_t, kRawItems>, kRawKinds> rooms{};
    for (std::size_t k = 0; k < kRawKinds; ++k)
    {
        std::uint32_t const gid = gRawHouse[k] < AutoWowGuilds::Houses().size()
                                      ? AutoWowGuilds::HouseGuildId(gRawHouse[k], alliance) : 0;
        Player* rep = Online(gid ? AutoWowGuilds::RepOf(gid) : 0);
        if (rep && !rep->IsInWorld())
            rep = nullptr;
        bool const repReady = rep && AutoWowGuilds::RepFreeSlots(rep) > 0;
        for (std::size_t i = 0; i < kRawItems; ++i)
            rooms[k][i] = ClothRoom(HeldUnits(rep, kRawLists[k][i]), p.rawCap, repReady);
    }
    std::lock_guard<std::mutex> guard(gLock);
    gRawRooms[T(alliance)] = rooms;
}

std::vector<std::uint32_t> Guids(std::vector<Stack> const& stacks, std::uint32_t maxStacks)
{
    std::vector<Stack> sorted = stacks;
    std::sort(sorted.begin(), sorted.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < sorted.size() && out.size() < maxStacks; ++i)
        out.push_back(sorted[i].guid);
    return out;
}

void TierTick(bool alliance, bool overlord);

// One world tick of a team's chain.
void TeamTick(bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    if (p.tiers)
        return TierTick(alliance, overlord);
    std::size_t const t = T(alliance);
    std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gBagHouse, alliance);
    std::uint32_t const repGuid = gid ? AutoWowGuilds::RepOf(gid) : 0;
    Player* rep = Online(repGuid);
    Player* art = Working(Online(gArtisan[t]));
    if (rep && !rep->IsInWorld())
        rep = nullptr;
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

std::uint32_t SellOf(std::uint32_t item)
{
    ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
    return t ? t->SellPrice : 0;
}

std::uint32_t BuyOf(std::uint32_t item)
{
    ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
    return t ? t->BuyPrice / std::max<std::uint32_t>(1, t->BuyCount) : 0;
}

// Tiers (AutoWow.Supply.Tiers): TeamTick over kTiers. The artisan's product is the highest bag tier it knows
// that members want (PickProduct); with no order left it levels on the cheapest non-grey recipe the house holds
// cloth for (PickSkillup). Every cloth routes under its own ClothCap. Market: the rep's AH wants.
void TierTick(bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    std::size_t const t = T(alliance);
    std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gBagHouse, alliance);
    std::uint32_t const repGuid = gid ? AutoWowGuilds::RepOf(gid) : 0;
    Player* rep = Online(repGuid);
    Player* art = Working(Online(gArtisan[t]));
    if (rep && !rep->IsInWorld())
        rep = nullptr;
    TeamState ts;
    {
        std::lock_guard<std::mutex> guard(gLock);
        ts = gTeams[t];
    }
    auto house = [&](std::uint32_t item) { return HeldUnits(rep, item) + HeldUnits(art, item); };

    std::array<std::uint32_t, kTierCount> stock{};
    for (std::size_t i = 0; i < kTierCount; ++i)
        stock[i] = HeldUnits(rep, kTiers[i].cloth);
    ts.clothRooms = ClothRooms(stock, p.clothCap, rep && AutoWowGuilds::RepFreeSlots(rep) > 0);
    ts.artisanSkill = art ? art->GetSkillValue(SKILL_TAILORING) : 0;

    // Artisan -> rep: finished bags of every tier, paid per bag, work XP per bag; product bags count down the order.
    if (art && rep && art != rep)
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            Tier const& tier = kTiers[i];
            std::vector<std::uint32_t> const bags = Guids(LooseStacks(art, tier.bag), kMaxMailStacks);
            if (bags.empty())
                continue;
            std::uint32_t const n = static_cast<std::uint32_t>(bags.size());
            if (!AutoWowGuilds::SendItems(Low(art), repGuid, bags, "AutoWoW bags"))
            {
                Emit(art, Reason::Refused, ts.orderId, tier.bag, n, 0, Low(art), repGuid, "deliver");
                continue;
            }
            Emit(art, Reason::Deliver, ts.orderId, tier.bag, n, 0, Low(art), repGuid);
            if (i == ts.product)
                ts.remaining -= std::min(ts.remaining, n);
            std::uint64_t const pay = BagPay(SellOf(tier.bag), p.bagPayPct, n);
            bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
            Emit(art, paid ? Reason::Pay : Reason::Refused, ts.orderId, tier.bag, n, pay, 0, Low(art),
                 paid ? nullptr : "pay");
            GrantXp(art, XpFor(p.workXpPerItem, art) * n, ts.orderId, tier.bag, n);
        }

    // Overlord: per-tier want and craftable, the product, the order and the per-tier surplus.
    std::array<std::uint32_t, kTierCount> wants{};
    if (overlord)
    {
        std::vector<ProductOption> opts(kTierCount);
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            Tier const& tier = kTiers[i];
            wants[i] = TotalWant(RankedMembers(alliance, tier.bagSlots));
            opts[i].known = art && art->HasSpell(tier.bagSpell);
            opts[i].want = wants[i];
            std::uint32_t const bags = BagsFrom(tier.recipe, house(tier.cloth), house(tier.bolt));
            opts[i].craftable = tier.extraPerBag ? std::min(bags, house(tier.extra) / tier.extraPerBag) : bags;
            ts.surplusBags[i] = Surplus(wants[i], Loose(rep, tier.bag), p.surplusKeep);
        }
        std::uint8_t const product = PickProduct(opts);
        ts.goal = PickGoal(opts);
        std::uint32_t n = 0;
        if (product != kNoTier)
        {
            Tier const& tier = kTiers[product];
            n = OrderSize(wants[product], house(tier.bag), opts[product].craftable, p.maxOrder);
        }
        if (n && (!ts.remaining || product != ts.product))
        {
            std::lock_guard<std::mutex> guard(gLock);
            ts.orderId = ++gNextOrderId;
        }
        ts.product = product;
        ts.productWant = product == kNoTier ? 0 : wants[product];
        ts.remaining = n;
        std::uint32_t surplus = 0;
        for (std::uint32_t const s : ts.surplusBags)
            surplus += s;
        ts.surplus = surplus;
        LOG_INFO("playerbots", "[Supply] overlord team={} gid={} rep={} artisan={} skill={} want={}/{}/{} "
                 "craftable={}/{}/{} product={} order={} oid={} surplus={} cloth_room={}/{}/{}",
                 alliance ? "alliance" : "horde", gid, repGuid, gArtisan[t], ts.artisanSkill, wants[0], wants[1],
                 wants[2], opts[0].craftable, opts[1].craftable, opts[2].craftable,
                 product == kNoTier ? -1 : int(product), n, ts.orderId, ts.surplus, ts.clothRooms[0],
                 ts.clothRooms[1], ts.clothRooms[2]);
        if (n)
            if (Player* who = art ? art : rep)
                Emit(who, Reason::Order, ts.orderId, kTiers[product].bag, n, 0, 0, gArtisan[t]);
    }

    // Skill-up recipe: the cheapest known non-grey recipe the house holds one cast of (none at the rank cap).
    std::vector<SkillupOption> skillups;
    if (art && ts.artisanSkill < art->GetMaxSkillValue(SKILL_TAILORING))
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            Tier const& tier = kTiers[i];
            Recipe const& r = tier.recipe;
            std::uint64_t const clothSell = SellOf(tier.cloth);
            std::uint64_t const houseCloth = std::uint64_t(house(tier.bolt)) * r.clothPerBolt + house(tier.cloth);
            skillups.push_back({tier.boltSpell, static_cast<std::uint8_t>(i), false, art->HasSpell(tier.boltSpell),
                                tier.boltGrey, house(tier.cloth) >= r.clothPerBolt, r.clothPerBolt * clothSell});
            skillups.push_back({tier.bagSpell, static_cast<std::uint8_t>(i), true, art->HasSpell(tier.bagSpell),
                                tier.bagGrey,
                                houseCloth >= std::uint64_t(r.boltsPerBag) * r.clothPerBolt &&
                                    house(tier.extra) >= tier.extraPerBag,
                                std::uint64_t(r.boltsPerBag) * r.clothPerBolt * clothSell +
                                    std::uint64_t(r.threadPerBag) * BuyOf(tier.thread) +
                                    std::uint64_t(tier.extraPerBag) * SellOf(tier.extra)});
        }
    int const pick = PickSkillup(ts.artisanSkill, skillups, true);
    ts.skillup = pick < 0 ? kNoTier : skillups[pick].tier;
    ts.skillupBag = pick >= 0 && skillups[pick].bag;

    // Rep -> artisan: the product order's cloth and extras, else one skill-up stock (SkillupCloth).
    auto feed = [&](std::uint32_t item, std::uint32_t units)
    {
        std::vector<std::uint32_t> const stacks = PickStacks(LooseStacks(rep, item), units);
        if (stacks.empty())
            return;
        std::uint32_t sent = 0;
        for (Stack const& s : LooseStacks(rep, item))
            if (std::find(stacks.begin(), stacks.end(), s.guid) != stacks.end())
                sent += s.count;
        bool const ok = AutoWowGuilds::SendItems(repGuid, Low(art), stacks, "AutoWoW materials");
        Emit(rep, ok ? Reason::Feed : Reason::Refused, ts.orderId, item, sent, 0, repGuid, Low(art),
             ok ? nullptr : "feed");
    };
    if (art && rep && art != rep)
    {
        if (ts.remaining && ts.product != kNoTier)
        {
            Tier const& tier = kTiers[ts.product];
            std::uint32_t const artBags = HeldUnits(art, tier.bag);
            feed(tier.cloth, LinenShort(tier.recipe, ts.remaining, true, HeldUnits(art, tier.cloth),
                                        HeldUnits(art, tier.bolt), artBags, 0));
            std::uint32_t const toMake = ts.remaining > artBags ? ts.remaining - artBags : 0;
            if (tier.extra)
                feed(tier.extra, Short(std::uint64_t(toMake) * tier.extraPerBag, HeldUnits(art, tier.extra)));
        }
        else if (ts.skillup != kNoTier)
        {
            Tier const& tier = kTiers[ts.skillup];
            feed(tier.cloth, Short(p.skillupCloth, HeldUnits(art, tier.cloth)));
            if (ts.skillupBag && tier.extra)
                feed(tier.extra, Short(tier.extraPerBag, HeldUnits(art, tier.extra)));
        }
    }
    if (art && ts.artisanWant)
    {
        bool const paid = gid && AutoWowGuilds::Pay(gid, art, ts.artisanWant);
        Emit(art, paid ? Reason::Feed : Reason::Refused, ts.orderId, 0, 0, ts.artisanWant, 0, Low(art),
             paid ? nullptr : "feed_copper");
        ts.artisanWant = 0;
    }

    // Market: the rep's listing float (AH deposits; refunded with a sale, lost on expiry) from the house bank.
    if (p.market && rep && gid && rep->GetMoney() < p.listFloat)
    {
        std::uint64_t const top = p.listFloat - rep->GetMoney();
        bool const paid = AutoWowGuilds::Pay(gid, rep, top);
        Emit(rep, paid ? Reason::Feed : Reason::Refused, 0, 0, 0, top, 0, repGuid, "float");
    }

    // Market: what the house lacks for the product it wants (members' open want, even when nothing is craftable
    // yet), else for the skill-up recipe (the stock-free pick when the house holds none).
    ts.buy.clear();
    if (p.market && art)
    {
        auto want = [&](std::uint32_t item, std::uint32_t units)
        {
            if (units)
                ts.buy.push_back({item, units, SellOf(item)});
        };
        if (ts.product != kNoTier && ts.productWant)
        {
            Tier const& tier = kTiers[ts.product];
            Recipe const& r = tier.recipe;
            std::uint32_t const open = ts.remaining ? ts.remaining : std::min(ts.productWant, p.maxOrder);
            std::uint32_t const toMake = open > house(tier.bag) ? open - house(tier.bag) : 0;
            want(tier.cloth, Short(std::uint64_t(toMake) * r.boltsPerBag * r.clothPerBolt,
                                   std::uint64_t(house(tier.bolt)) * r.clothPerBolt + house(tier.cloth)));
            if (tier.extra)
                want(tier.extra, Short(std::uint64_t(toMake) * tier.extraPerBag, house(tier.extra)));
        }
        else
        {
            int const target = pick >= 0 ? pick : PickSkillup(ts.artisanSkill, skillups, false);
            if (target >= 0)
            {
                Tier const& tier = kTiers[skillups[target].tier];
                want(tier.cloth, Short(p.skillupCloth, house(tier.cloth)));
                if (skillups[target].bag && tier.extra)
                    want(tier.extra, Short(tier.extraPerBag, house(tier.extra)));
            }
        }
    }

    // Rep -> members: its loose bags, best tier first, each member up to its want for that tier's slots (bags
    // given this tick count as incoming for the lower tiers).
    if (rep)
    {
        std::unordered_map<std::uint32_t, std::uint32_t> given;  // bounded by the cohort
        for (std::size_t i = kTierCount; i-- > 0;)
        {
            Tier const& tier = kTiers[i];
            std::vector<Stack> bags = LooseStacks(rep, tier.bag);
            if (bags.empty())
                continue;
            std::sort(bags.begin(), bags.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
            std::vector<Member> members = Members(alliance, tier.bagSlots);
            for (Member& m : members)
                if (auto const it = given.find(m.guid); it != given.end())
                    m.incoming += it->second;
            std::size_t next = 0;
            for (Delivery const& d : PlanDeliveries(RankNeeds(std::move(members)), static_cast<std::uint32_t>(bags.size())))
            {
                std::vector<std::uint32_t> give;
                for (; next < bags.size() && give.size() < d.bags; ++next)
                    give.push_back(bags[next].guid);
                if (give.empty())
                    break;
                std::uint32_t const n = static_cast<std::uint32_t>(give.size());
                if (!AutoWowGuilds::SendItems(repGuid, d.guid, give, "AutoWoW bag"))
                {
                    Emit(rep, Reason::Refused, ts.orderId, tier.bag, n, 0, repGuid, d.guid, "deliver");
                    continue;
                }
                given[d.guid] += n;
                Emit(rep, Reason::Deliver, ts.orderId, tier.bag, n, 0, repGuid, d.guid);
                GrantXp(rep, XpFor(p.repXpPerDeal, rep), ts.orderId, tier.bag, n);
            }
        }
    }

    std::lock_guard<std::mutex> guard(gLock);
    TeamState& out = gTeams[t];
    out.orderId = ts.orderId;
    out.remaining = ts.remaining;
    out.clothRooms = ts.clothRooms;
    out.artisanSkill = ts.artisanSkill;
    out.skillup = ts.skillup;
    out.skillupBag = ts.skillupBag;
    out.buy = ts.buy;
    if (overlord)
    {
        out.product = ts.product;
        out.productWant = ts.productWant;
        out.goal = ts.goal;
        out.surplus = ts.surplus;
        out.surplusBags = ts.surplusBags;
    }
    out.artisanWant = ts.artisanWant;  // no map update runs during the world tick: nothing set since the copy
}

// Vendor value of one cast of tier `i`'s reagents (copper): Route / Market at the sell value, Vendor at the
// buy price, Craft at its own tier's cost.
std::uint64_t CastCost(ProductLine const& l, std::size_t i, std::size_t depth = kMaxLineTiers)
{
    std::uint64_t cost = 0;
    for (Reagent const& r : l.tiers[i].reagents)
    {
        if (!r.item || !depth)
            continue;
        std::uint8_t const sub = r.source == Source::Craft ? TierOf(l, r.item) : kNoTier;
        std::uint64_t const unit = sub != kNoTier && sub != i ? CastCost(l, sub, depth - 1)
                                   : r.source == Source::Vendor ? BuyOf(r.item) : SellOf(r.item);
        cost += unit * r.count;
    }
    return cost;
}

std::vector<StockMember> StockMembers(bool alliance, ProductLine const& l)
{
    std::vector<StockMember> out;
    for (AutoWowGuilds::GuidRange const& r : AutoWowGuilds::Cohort())
        for (std::uint64_t g = r.lo; g <= r.hi; ++g)
        {
            std::uint32_t const guid = static_cast<std::uint32_t>(g);
            Player* m = Online(guid);
            if (!m || !m->IsInWorld() || (m->GetTeamId() == TEAM_ALLIANCE) != alliance || IsRole(m))
                continue;
            StockMember sm{guid, m->GetLevel(), {}};
            for (std::size_t i = 0; i < l.tierCount; ++i)
                sm.held[i] = HeldUnits(m, l.tiers[i].product);
            out.push_back(sm);
        }
    return out;
}

// A catalog line (tierCount > 0: single-step recipes, e.g. potions) over one team: TierTick's chain generalized.
// Route reagents come from adventurers via the rep, Vendor reagents the artisan buys, a Craft reagent (a lower
// tier's product) the artisan makes. Need = members' stock of their best usable tier under PotionTarget.
void LineTick(Line line, bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    ProductLine const& L = LineOf(line);
    std::size_t const li = static_cast<std::size_t>(line), t = T(alliance);
    std::uint32_t const gid = AutoWowGuilds::HouseGuildId(gLineHouse[li], alliance);
    std::uint32_t const repGuid = gid ? AutoWowGuilds::RepOf(gid) : 0;
    Player* rep = Online(repGuid);
    Player* art = Working(Online(gLineArtisan[li][t]));
    if (rep && !rep->IsInWorld())
        rep = nullptr;
    LineState ts;
    {
        std::lock_guard<std::mutex> guard(gLock);
        ts = gLines[li][t];
    }
    LineView& v = ts.v;
    auto house = [&](std::uint32_t item) { return HeldUnits(rep, item) + HeldUnits(art, item); };
    auto artHeld = [&](std::uint32_t item) { return HeldUnits(art, item); };

    // Donor room per routed reagent, each under HerbCap.
    std::vector<std::uint32_t> const& route = gLineRoute[li];
    bool const repReady = rep && AutoWowGuilds::RepFreeSlots(rep) > 0;
    v.rooms.assign(route.size(), 0);
    for (std::size_t i = 0; i < route.size(); ++i)
        v.rooms[i] = ClothRoom(HeldUnits(rep, route[i]), p.herbCap, repReady);
    v.artisanSkill = art ? art->GetSkillValue(L.skillLine) : 0;
    std::array<bool, kMaxLineTiers> known{};
    for (std::size_t i = 0; i < L.tierCount; ++i)
        known[i] = art && art->HasSpell(L.tiers[i].spell);

    // The artisan's current target (the order, else the skill-up recipe) and its casts: a Craft reagent of it
    // stays with the artisan.
    std::uint8_t const target = v.remaining && v.product != kNoTier ? v.product : v.skillup;
    std::uint32_t const targetCasts = v.remaining && v.product != kNoTier ? v.remaining : p.skillupCasts;

    // Artisan -> rep: finished products of every tier, paid per unit, work XP per unit; product units count
    // down the order.
    if (art && rep && art != rep)
        for (std::size_t i = 0; i < L.tierCount; ++i)
        {
            LineTier const& tier = L.tiers[i];
            std::vector<Stack> const stacks = LooseStacks(art, tier.product);
            std::vector<std::uint32_t> const ship =
                SellStacks(stacks, CraftReserve(L, target, targetCasts, tier.product));
            if (ship.empty())
                continue;
            std::uint32_t n = 0;
            for (Stack const& s : stacks)
                if (std::find(ship.begin(), ship.end(), s.guid) != ship.end())
                    n += s.count;
            if (!AutoWowGuilds::SendItems(Low(art), repGuid, ship, "AutoWoW goods"))
            {
                EmitLine(line, art, Reason::Refused, v.orderId, tier.product, n, 0, Low(art), repGuid, "deliver");
                continue;
            }
            EmitLine(line, art, Reason::Deliver, v.orderId, tier.product, n, 0, Low(art), repGuid);
            if (i == v.product)
                v.remaining -= std::min(v.remaining, n);
            std::uint64_t const pay = BagPay(SellOf(tier.product), p.potionPayPct, n);
            bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
            EmitLine(line, art, paid ? Reason::Pay : Reason::Refused, v.orderId, tier.product, n, pay, 0, Low(art),
                     paid ? nullptr : "pay");
            GrantXp(art, XpFor(p.workXpPerItem, art) * n, v.orderId, tier.product, n, line);
        }

    // Need scan (the overlord's order; every tick for the rep's deliveries).
    std::vector<StockNeed> ranked;
    if (overlord || rep)
        ranked = RankStock(L, StockMembers(alliance, L), known, p.potionTarget);
    if (overlord)
    {
        std::vector<ProductOption> opts(L.tierCount);
        std::array<std::uint32_t, kMaxLineTiers> wants{};
        for (std::size_t i = 0; i < L.tierCount; ++i)
        {
            wants[i] = TierWant(ranked, static_cast<std::uint8_t>(i));
            opts[i] = {known[i], wants[i], Casts(L, i, house)};
            v.surplus[i] = Surplus(wants[i], Loose(rep, L.tiers[i].product), p.potionKeep);
        }
        std::uint8_t const product = PickProduct(opts);
        std::uint32_t const n = product == kNoTier ? 0 : OrderSize(wants[product], house(L.tiers[product].product),
                                                                   opts[product].craftable, p.potionMaxOrder);
        if (n && (!v.remaining || product != v.product))
        {
            std::lock_guard<std::mutex> guard(gLock);
            v.orderId = ++gNextOrderId;
        }
        v.product = product;
        v.productWant = product == kNoTier ? 0 : wants[product];
        v.remaining = n;
        LOG_INFO("playerbots", "[Supply] overlord line={} team={} gid={} rep={} artisan={} skill={} members_wanting={} "
                 "want={}/{}/{} craftable={}/{}/{} product={} order={} oid={} surplus={}/{}/{}", L.name,
                 alliance ? "alliance" : "horde", gid, repGuid, gLineArtisan[li][t], v.artisanSkill, ranked.size(),
                 wants[0], wants[1], wants[2], L.tierCount > 0 ? opts[0].craftable : 0,
                 L.tierCount > 1 ? opts[1].craftable : 0, L.tierCount > 2 ? opts[2].craftable : 0,
                 product == kNoTier ? -1 : int(product), n, v.orderId, v.surplus[0], v.surplus[1], v.surplus[2]);
        if (n)
            if (Player* who = art ? art : rep)
                EmitLine(line, who, Reason::Order, v.orderId, L.tiers[product].product, n, 0, 0, gLineArtisan[li][t]);
    }

    // Skill-up recipe: the cheapest known non-grey recipe the house holds one cast of (none at the rank cap).
    std::vector<SkillupOption> skillups;
    if (art && v.artisanSkill < art->GetMaxSkillValue(L.skillLine))
        for (std::size_t i = 0; i < L.tierCount; ++i)
            skillups.push_back({L.tiers[i].spell, static_cast<std::uint8_t>(i), false, known[i], L.tiers[i].grey,
                                Casts(L, i, house) > 0, CastCost(L, i)});
    int const pick = PickSkillup(v.artisanSkill, skillups, true);
    v.skillup = pick < 0 ? kNoTier : skillups[pick].tier;

    // What the artisan lacks for its casts: the order's (toMake beyond the finished units it holds), else
    // SkillupCasts of the skill-up recipe. Route / Market reagents come from the rep, Vendor ones it buys.
    std::vector<Lack> lacks;
    std::uint32_t toMake = 0;
    if (v.remaining && v.product != kNoTier)
    {
        std::uint32_t const held = HeldUnits(art, L.tiers[v.product].product);
        toMake = v.remaining > held ? v.remaining - held : 0;
        lacks = Lacks(L, v.product, toMake, artHeld);
    }
    else if (v.skillup != kNoTier)
        lacks = Lacks(L, v.skillup, p.skillupCasts, artHeld);
    v.vendor.clear();
    for (Lack const& k : lacks)
        if (k.source == Source::Vendor)
            v.vendor.push_back({k.item, k.units + HeldUnits(art, k.item), BuyOf(k.item)});
    if (art && rep && art != rep)
        for (Lack const& k : lacks)
        {
            if (k.source == Source::Vendor)
                continue;
            std::vector<std::uint32_t> const stacks = PickStacks(LooseStacks(rep, k.item), k.units);
            if (stacks.empty())
                continue;
            std::uint32_t sent = 0;
            for (Stack const& s : LooseStacks(rep, k.item))
                if (std::find(stacks.begin(), stacks.end(), s.guid) != stacks.end())
                    sent += s.count;
            bool const ok = AutoWowGuilds::SendItems(repGuid, Low(art), stacks, "AutoWoW materials");
            EmitLine(line, rep, ok ? Reason::Feed : Reason::Refused, v.orderId, k.item, sent, 0, repGuid, Low(art),
                     ok ? nullptr : "feed");
        }
    if (art && ts.artisanWant)
    {
        bool const paid = gid && AutoWowGuilds::Pay(gid, art, ts.artisanWant);
        EmitLine(line, art, paid ? Reason::Feed : Reason::Refused, v.orderId, 0, 0, ts.artisanWant, 0, Low(art),
                 paid ? nullptr : "feed_copper");
        ts.artisanWant = 0;
    }

    // Market: what the house (rep + artisan) lacks for the product members want (even when nothing is
    // craftable yet), else for the skill-up recipe (the stock-free pick when the house holds none).
    v.buy.clear();
    if (p.market && art)
    {
        std::vector<Lack> short_;
        if (v.product != kNoTier && v.productWant)
        {
            std::uint32_t const open = v.remaining ? v.remaining : std::min(v.productWant, p.potionMaxOrder);
            std::uint32_t const finished = house(L.tiers[v.product].product);
            short_ = Lacks(L, v.product, open > finished ? open - finished : 0, house);
        }
        else
        {
            int const want = pick >= 0 ? pick : PickSkillup(v.artisanSkill, skillups, false);
            if (want >= 0)
                short_ = Lacks(L, skillups[want].tier, p.skillupCasts, house);
        }
        for (Lack const& k : short_)
            if (k.source != Source::Vendor)
                v.buy.push_back({k.item, k.units, SellOf(k.item)});
    }

    // Rep -> members: its products, best tier first, lowest stock first (whole stacks); rep XP per deal.
    if (rep)
        for (std::size_t i = L.tierCount; i-- > 0;)
        {
            LineTier const& tier = L.tiers[i];
            std::vector<Stack> const stacks = LooseStacks(rep, tier.product);
            if (stacks.empty())
                continue;
            for (StackDelivery const& d : PlanStackDeliveries(ranked, static_cast<std::uint8_t>(i), stacks))
            {
                if (!AutoWowGuilds::SendItems(repGuid, d.guid, d.stacks, "AutoWoW supplies"))
                {
                    EmitLine(line, rep, Reason::Refused, v.orderId, tier.product, d.units, 0, repGuid, d.guid,
                             "deliver");
                    continue;
                }
                EmitLine(line, rep, Reason::Deliver, v.orderId, tier.product, d.units, 0, repGuid, d.guid);
                GrantXp(rep, XpFor(p.repXpPerDeal, rep), v.orderId, tier.product, d.units, line);
            }
        }

    std::lock_guard<std::mutex> guard(gLock);
    LineView& out = gLines[li][t].v;
    out.orderId = v.orderId;
    out.remaining = v.remaining;
    out.rooms = v.rooms;
    out.artisanSkill = v.artisanSkill;
    out.skillup = v.skillup;
    out.buy = v.buy;
    out.vendor = v.vendor;
    if (overlord)
    {
        out.product = v.product;
        out.productWant = v.productWant;
        out.surplus = v.surplus;
    }
    gLines[li][t].artisanWant = ts.artisanWant;  // no map update runs during the world tick
}

// World thread: the rep's market buyouts, funded by its house bank (bank -> rep -> AH seller); the copper a
// rejected bid did not spend goes back to the bank. The auction house is the faction house the rep stands at.
class MarketBuyOperation : public PlayerbotOperation
{
public:
    MarketBuyOperation(ObjectGuid bot, ObjectGuid auctioneer, std::vector<MarketListing> buys)
        : bot_(bot), auctioneer_(auctioneer), buys_(std::move(buys))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || buys_.empty())
            return false;
        std::uint32_t const gid = AutoWowGuilds::HouseGuildOf(bot);
        std::uint64_t total = 0;
        for (MarketListing const& b : buys_)
            total += b.buyout;
        if (!gid || !AutoWowGuilds::Pay(gid, bot, total))
        {
            Emit(bot, Reason::Refused, 0, buys_.front().item, 0, total, 0, Low(bot), "buy");
            return false;
        }
        std::uint64_t spent = 0;
        for (MarketListing const& b : buys_)
        {
            std::uint64_t const m0 = bot->GetMoney();
            WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
            packet << auctioneer_ << uint32(b.id) << uint32(b.buyout);
            bot->GetSession()->HandleAuctionPlaceBid(packet);
            if (bot->GetMoney() >= m0)
            {
                Emit(bot, Reason::Refused, 0, b.item, b.count, b.buyout, Low(bot), 0, "buy");
                continue;
            }
            spent += m0 - bot->GetMoney();
            Emit(bot, Reason::Buy, 0, b.item, b.count, m0 - bot->GetMoney(), Low(bot), 0);
        }
        std::uint64_t const back = std::min<std::uint64_t>(total > spent ? total - spent : 0, bot->GetMoney());
        if (back)
            AutoWowGuilds::Deposit(bot, back);
        return spent != 0;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyMarketBuy"; }

private:
    ObjectGuid bot_;
    ObjectGuid auctioneer_;
    std::vector<MarketListing> buys_;
};

// World thread: the rep cancels its own listings of wanted items (stock cancel handler: the item comes back by mail,
// the deposit stays spent). One sold or bid on since the visit is left alone.
class MarketCancelOperation : public PlayerbotOperation
{
public:
    MarketCancelOperation(ObjectGuid bot, ObjectGuid auctioneer, std::vector<MarketListing> cancels)
        : bot_(bot), auctioneer_(auctioneer), cancels_(std::move(cancels))
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
            return false;
        Creature* npc = bot->GetNPCIfCanInteractWith(auctioneer_, UNIT_NPC_FLAG_AUCTIONEER);
        AuctionHouseObject* ah = npc ? sAuctionMgr->GetAuctionsMap(npc->GetFaction()) : nullptr;
        if (!ah)
            return false;
        bool any = false;
        for (MarketListing const& c : cancels_)
        {
            AuctionEntry const* a = ah->GetAuction(c.id);
            if (!a || a->owner != bot->GetGUID() || a->bidder)
                continue;
            WorldPacket packet(CMSG_AUCTION_REMOVE_ITEM, 8 + 4);
            packet << auctioneer_ << uint32(c.id);
            bot->GetSession()->HandleAuctionRemoveItem(packet);
            bool const gone = !ah->GetAuction(c.id);
            Emit(bot, gone ? Reason::Cancel : Reason::Refused, 0, c.item, c.count, c.buyout, Low(bot), Low(bot),
                 gone ? nullptr : "cancel");
            any = any || gone;
        }
        return any;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyMarketCancel"; }

private:
    ObjectGuid bot_;
    ObjectGuid auctioneer_;
    std::vector<MarketListing> cancels_;
};

// The stations near `home`: the trainer teaching `trainerSpell`, the vendor selling `vendorItem` (no extended
// cost), the auctioneer and the mailbox, each the nearest (ties the lower spawn id).
void FindStations(Stations& st, bool alliance, Home const& home, std::uint32_t trainerSpell, std::uint32_t vendorItem)
{
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
                    if (s.SpellId == trainerSpell)
                    {
                        consider(0, st.trainer, spawn, data.id, data.posX, data.posY, data.posZ);
                        break;
                    }
        if ((npcflag & UNIT_NPC_FLAG_VENDOR))
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* vi : list->m_items)
                    if (vi && vi->item == vendorItem && !vi->ExtendedCost)
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
}

void BuildStations(bool alliance, Home const& home)
{
    Stations& st = gStations[T(alliance)];
    FindStations(st, alliance, home, kBagSpell, kThreadItem);
    Stations bags;
    FindStations(bags, alliance, home, 0, kPouch);
    gBagVendor[T(alliance)] = bags.threadVendor;
    LOG_INFO("server.loading", "[Supply] {} home map={} ({},{}) stations: mailbox={} trainer={} thread_vendor={} "
             "auctioneer={} bag_vendor={} (item {})", alliance ? "alliance" : "horde", home.map, home.x, home.y,
             st.mailbox.entry, st.trainer.entry, st.threadVendor.entry, st.auctioneer.entry, gBagVendor[T(alliance)].entry,
             kPouch);
}

// Reagent count of `item` in `spell` (0 = not a reagent).
std::uint32_t ReagentCount(SpellInfo const* s, std::uint32_t item)
{
    for (std::size_t i = 0; s && i < MAX_SPELL_REAGENTS; ++i)
        if (s->Reagent[i] > 0 && std::uint32_t(s->Reagent[i]) == item)
            return s->ReagentCount[i];
    return 0;
}

// World thread, every tick with Outfit on: the pending grant requests, lowest level first (RankGrants), each paid
// whole from the bot's house bank or refused (DecideGrant; a short bank = Pay's own refused `guild` row).
void OutfitTick()
{
    std::vector<GrantRequest> reqs;
    {
        std::lock_guard<std::mutex> guard(gLock);
        for (auto const& [guid, need] : gGrantRequests)
            reqs.push_back({guid, 0, need});
        gGrantRequests.clear();
    }
    for (GrantRequest& r : reqs)
        if (Player* bot = Online(r.guid))
            r.level = bot->GetLevel();
    Params const& p = detail::gParams;
    std::uint64_t const hour =
        static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count())) / kGrantHourMs;
    for (GrantRequest const& r : RankGrants(std::move(reqs)))
    {
        Player* bot = Online(r.guid);
        std::uint32_t const gid = bot ? AutoWowGuilds::HouseGuildOf(bot) : 0;
        if (!gid)
            continue;  // logged out / not in a house: nothing to pay into
        GrantBudget& budget = gGrantBudgets[T(bot->GetTeamId() == TEAM_ALLIANCE)];
        GrantWindow& window = gGrantWindows[r.guid];
        GrantDecision const d =
            DecideGrant(r, bot->GetMoney(), window, budget, hour, p.outfitMaxCopper, p.outfitBudgetPerHour);
        if (d.verdict == GrantVerdict::Covered)
            continue;
        if (d.verdict != GrantVerdict::Pay)
        {
            EmitOutfit(bot, Reason::Refused, 0, d.copper, GrantVerdictName(d.verdict));
            continue;
        }
        if (AutoWowGuilds::Pay(gid, bot, d.copper, AutoWowGuilds::Reason::Grant))
            NoteGrant(window, budget, r.level, hour, d.copper);
    }
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
    p.tiers = sConfigMgr->GetOption<bool>("AutoWow.Supply.Tiers", false);
    p.market = sConfigMgr->GetOption<bool>("AutoWow.Supply.Market", false);
    p.buyMaxPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BuyMaxPct", 400);
    p.buyBudget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BuyBudget", 500);
    p.sellKeep = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.SellKeep", 60);
    p.listFloat = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ListFloat", 1000);
    p.routeHerbs = sConfigMgr->GetOption<bool>("AutoWow.Supply.RouteHerbs", false);
    p.herbCap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.HerbCap", 100);
    p.potionTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.PotionTarget", 5);
    p.potionPayPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.PotionPayPct", 200);
    p.potionMaxOrder = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.PotionMaxOrder", 20));
    p.potionKeep = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.PotionSurplusKeep", 20);
    p.skillupCasts = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.SkillupCasts", 10);
    p.routeRaw = sConfigMgr->GetOption<bool>("AutoWow.Supply.RouteRaw", false);
    p.rawCap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.RawCap", 100);
    p.outfit = sConfigMgr->GetOption<bool>("AutoWow.Supply.Outfit", false);
    p.outfitCheckMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OutfitCheckMs", 600000);
    p.outfitMaxCopper = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OutfitMaxCopper", 500);
    p.outfitBudgetPerHour = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OutfitBudgetPerHour", 5000);
    p.artisanFreeSlots = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ArtisanFreeSlots", 4);
    p.artisanMinLevel = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ArtisanMinLevel", 10);
    p.lines = 1;
    std::string const products = sConfigMgr->GetOption<std::string>("AutoWow.Supply.Products", "bags");
    if (!ParseProducts(products, p.lines))
        LOG_ERROR("server.loading", "[Supply] bad AutoWow.Supply.Products '{}': bags only", products);
    gRoles.clear();
    gArtisan = {};
    gBagVendor = {};
    gLearn.clear();
    gLineHouse = {};
    gLineArtisan = {};
    for (std::size_t i = 0; i < kLineCount; ++i)
    {
        gLineHouseName[i].clear();
        gLineLearn[i].clear();
        gLineRoute[i].clear();
    }
    {
        std::lock_guard<std::mutex> guard(gLock);
        gTeams = {};
        gLines = {};
        gHeld.clear();
        gRawRooms = {};
        gGrantRequests.clear();
    }
    gGrantWindows.clear();
    gGrantBudgets = {};
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
    // RouteRaw: each raw kind's house (a missing house turns that kind off).
    for (std::size_t k = 0; k < kRawKinds; ++k)
    {
        gRawHouseName[k] = sConfigMgr->GetOption<std::string>(std::string("AutoWow.Supply.House.") + kRawKeys[k],
                                                              kRawDefaultHouse[k]);
        gRawHouse[k] = houses.size();
        for (std::size_t i = 0; i < houses.size(); ++i)
            if (houses[i].name == gRawHouseName[k])
                gRawHouse[k] = i;
        if (p.routeRaw)
            LOG_INFO("server.loading", "[Supply] route raw {} -> house {}{} cap={}", kRawKeys[k], gRawHouseName[k],
                     gRawHouse[k] == houses.size() ? " (not a house: off)" : "", p.rawCap);
    }

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

    if (p.market && !p.tiers)
    {
        LOG_ERROR("server.loading", "[Supply] AutoWow.Supply.Market needs AutoWow.Supply.Tiers: market off");
        p.market = false;
    }
    // The static tier table must match the loaded spells (outputs, reagent counts) and bag templates.
    for (std::size_t i = 0; p.tiers && i < kTierCount; ++i)
    {
        Tier const& tier = kTiers[i];
        SpellInfo const* b = sSpellMgr->GetSpellInfo(tier.boltSpell);
        SpellInfo const* g = sSpellMgr->GetSpellInfo(tier.bagSpell);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tier.bag);
        bool const ok = Output(b) == tier.bolt && Output(g) == tier.bag &&
                        ReagentCount(b, tier.cloth) == tier.recipe.clothPerBolt &&
                        ReagentCount(g, tier.bolt) == tier.recipe.boltsPerBag &&
                        ReagentCount(g, tier.thread) == tier.recipe.threadPerBag &&
                        (!tier.extra || ReagentCount(g, tier.extra) == tier.extraPerBag) && proto &&
                        proto->ContainerSlots == tier.bagSlots;
        if (!ok)
        {
            LOG_ERROR("server.loading", "[Supply] tier {} (bolt spell {}, bag spell {}) does not match the loaded "
                      "spells / items: Tiers and Market off", i, tier.boltSpell, tier.bagSpell);
            p.tiers = p.market = false;
        }
    }
    if (p.tiers)
        // The artisan learns every tier recipe once its skill allows (CanTeachSpell checks the rank).
        for (Tier const& tier : kTiers)
            for (std::uint32_t const id : {tier.boltSpell, tier.bagSpell})
                if (std::find(gLearn.begin(), gLearn.end(), id) == gLearn.end())
                    gLearn.push_back(id);

    // Catalog lines with their own runtime (tierCount > 0): house, table check, learn list, routed reagents.
    for (std::size_t li = 0; li < kLineCount; ++li)
    {
        ProductLine const& L = kCatalog[li];
        if (!L.tierCount || !LineOn(L.id))
            continue;
        auto off = [&](std::string const& why)
        {
            LOG_ERROR("server.loading", "[Supply] line {} off: {}", L.name, why);
            p.lines &= static_cast<std::uint8_t>(~(1u << li));
        };
        std::string const key = std::string("AutoWow.Supply.House.") + L.key;
        gLineHouseName[li] = sConfigMgr->GetOption<std::string>(key, L.house);
        gLineHouse[li] = houses.size();
        for (std::size_t i = 0; i < houses.size(); ++i)
            if (houses[i].name == gLineHouseName[li])
                gLineHouse[li] = i;
        if (gLineHouse[li] == houses.size())
        {
            off(key + " '" + gLineHouseName[li] + "' is not an AutoWow.Guilds.Houses house");
            continue;
        }
        if (LineOn(Line::Bags) && gLineHouse[li] == gBagHouse)
        {
            off("its house is the bag house");
            continue;
        }
        bool ok = true;
        for (std::size_t i = 0; i < L.tierCount; ++i)
        {
            LineTier const& tier = L.tiers[i];
            SpellInfo const* s = sSpellMgr->GetSpellInfo(tier.spell);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tier.product);
            bool t = s && Output(s) == tier.product && proto && proto->RequiredLevel == tier.reqLevel;
            for (Reagent const& r : tier.reagents)
                t = t && (!r.item || (ReagentCount(s, r.item) == r.count && sObjectMgr->GetItemTemplate(r.item)));
            if (!t)
                LOG_ERROR("server.loading", "[Supply] line {} tier {} (spell {}, product {}) does not match the "
                          "loaded spells / items", L.name, i, tier.spell, tier.product);
            ok = ok && t;
        }
        if (!ok)
        {
            off("tier table mismatch");
            continue;
        }
        std::string const learn =
            sConfigMgr->GetOption<std::string>(std::string("AutoWow.Supply.Artisan.Learn.") + L.key, L.learn);
        AutoWowGuilds::detail::Split(learn, ',', [&](std::string_view s) {
            std::uint32_t id = 0;
            if (AutoWowGuilds::detail::ParseU32(s, id) && id)
                gLineLearn[li].push_back(id);
        });
        // The artisan learns every trainer-taught tier recipe once its skill allows (CanTeachSpell checks it).
        for (std::size_t i = 0; i < L.tierCount; ++i)
            if (L.tiers[i].skill > 1 &&
                std::find(gLineLearn[li].begin(), gLineLearn[li].end(), L.tiers[i].spell) == gLineLearn[li].end())
                gLineLearn[li].push_back(L.tiers[i].spell);
        gLineRoute[li] = RouteItems(L);
    }

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
        for (std::size_t li = 0; li < kLineCount && home.set; ++li)
        {
            ProductLine const& L = kCatalog[li];
            if (!L.tierCount || !LineOn(L.id))
                continue;
            // Trainer: the one teaching the first trainer-taught recipe; vendor: the first Vendor reagent's.
            std::uint32_t trainerSpell = 0, vendorItem = 0;
            for (std::size_t i = 0; i < L.tierCount; ++i)
            {
                if (!trainerSpell && L.tiers[i].skill > 1)
                    trainerSpell = L.tiers[i].spell;
                for (Reagent const& r : L.tiers[i].reagents)
                    if (!vendorItem && r.item && r.source == Source::Vendor)
                        vendorItem = r.item;
            }
            Stations& st = gLineStations[li][T(alliance)];
            FindStations(st, alliance, home, trainerSpell, vendorItem);
            LOG_INFO("server.loading", "[Supply] line {} {} stations: mailbox={} trainer={} vendor={} auctioneer={}",
                     L.name, alliance ? "alliance" : "horde", st.mailbox.entry, st.trainer.entry,
                     st.threadVendor.entry, st.auctioneer.entry);
        }
        for (std::size_t i = 0; i < houses.size(); ++i)
        {
            RoleInfo info;
            info.alliance = alliance;
            info.bagHouse = LineOn(Line::Bags) && i == gBagHouse;
            info.home = home;
            for (std::size_t li = 0; li < kLineCount; ++li)
                if (kCatalog[li].tierCount && LineOn(kCatalog[li].id) && gLineHouse[li] == i)
                    info.line = static_cast<std::uint8_t>(li);
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
                AutoWowGuilds::PinArtisan(i, alliance, artisan);  // joins this house at login, like a rep
                if (info.bagHouse)
                    gArtisan[T(alliance)] = artisan;
                if (info.line != kNoLine)
                    gLineArtisan[info.line][T(alliance)] = artisan;
            }
        }
    }
    LOG_INFO("server.loading", "[Supply] enabled: house={} bag={} ({} slots, sell {}) bolt={} recipe cloth/bolt={} "
             "bolts/bag={} thread/bag={} thread_price={} roles={} artisan A={} H={} learn={}", gBagHouseName, gBagItem,
             gBagSlots, gBagSell, gBoltItem, gRecipe.clothPerBolt, gRecipe.boltsPerBag, gRecipe.threadPerBag,
             gThreadPrice, gRoles.size(), gArtisan[0], gArtisan[1], gLearn.size());
    LOG_INFO("server.loading", "[Supply] artisan upkeep: free_slots={} min_level={} bag={} outfit={}",
             p.artisanFreeSlots, p.artisanMinLevel, kPouch, p.outfit);
    if (p.tiers)
        LOG_INFO("server.loading", "[Supply] tiers on: {} tiers, cloth cap {} per cloth; market={} buy_max_pct={} "
                 "buy_budget={} sell_keep={} list_float={}", kTierCount, p.clothCap, p.market, p.buyMaxPct, p.buyBudget,
                 p.sellKeep, p.listFloat);
    for (std::size_t li = 0; li < kLineCount; ++li)
        if (kCatalog[li].tierCount && LineOn(kCatalog[li].id))
            LOG_INFO("server.loading", "[Supply] line {} on: house={} artisan A={} H={} learn={} routed={} "
                     "route_herbs={} herb_cap={} target={} pay_pct={} max_order={} keep={} skillup_casts={}",
                     kCatalog[li].name, gLineHouseName[li], gLineArtisan[li][0], gLineArtisan[li][1],
                     gLineLearn[li].size(), gLineRoute[li].size(), p.routeHerbs, p.herbCap, p.potionTarget,
                     p.potionPayPct, p.potionMaxOrder, p.potionKeep, p.skillupCasts);
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
    // The overlord walks every enabled catalog line per team: bags on its bespoke chain, the rest on LineTick.
    if (LineOn(Line::Bags))
    {
        TeamTick(true, overlord);
        TeamTick(false, overlord);
    }
    for (ProductLine const& L : kCatalog)
        if (L.tierCount && LineOn(L.id))
        {
            LineTick(L.id, true, overlord);
            LineTick(L.id, false, overlord);
        }
    if (p.routeRaw)
    {
        RawTick(true);
        RawTick(false);
    }
    if (p.outfit)
        OutfitTick();
}

RoleInfo RoleOf(std::uint32_t guid)
{
    auto const it = gRoles.find(guid);
    return it == gRoles.end() ? RoleInfo{} : it->second;
}

RoleInfo ActiveRoleOf(Player* p)
{
    if (!p)
        return {};
    RoleInfo r = RoleOf(Low(p));
    r.role = GatedRole(r.role, p->GetLevel(), detail::gParams.artisanMinLevel,
                       p->GetMap() && p->GetMap()->Instanceable());
    return r;
}

Stations const& StationsOf(bool alliance) { return gStations[T(alliance)]; }
Station const& BagVendorOf(bool alliance) { return gBagVendor[T(alliance)]; }

TeamView ViewOf(bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    TeamState const& s = gTeams[T(alliance)];
    TeamView v;
    v.orderId = s.orderId;
    v.remaining = s.remaining;
    v.surplus = s.surplus;
    v.product = s.product;
    v.skillup = s.skillup;
    v.skillupBag = s.skillupBag;
    v.artisanSkill = s.artisanSkill;
    v.surplusBags = s.surplusBags;
    v.buy = s.buy;
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
    EmitLine(Line::Bags, p, r, oid, item, count, copper, from, to, op);
}

void EmitLine(Line l, Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
              std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op)
{
    std::string const& house = l == Line::Bags ? gBagHouseName : gLineHouseName[static_cast<std::size_t>(l)];
    LOG_INFO("playerbots", "[Supply] player={} {} house={} oid={} item={} count={} copper={} from={} to={}{}{}",
             p ? p->GetName() : "-", ReasonName(r), house, oid, item, count, copper, from, to,
             op ? " op=" : "", op ? op : "");
    if (p && AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSupply(p, ReasonName(r),
                                       LedgerFields(house, oid, item, count, copper, from, to, op) + LineField(l));
}

// Errand sell stop: a cohort adventurer mails the line's routed reagents to its team's house rep, each within
// its own room (HerbCap); the stacks are held back from this stop's sales. Unlike cloth, holders of the line's
// profession route too: adventurers never craft it (6 of the 9 cohort herbalists are alchemists at skill 1).
static void RouteLine(Line l, Player* bot)
{
    Params const& p = detail::gParams;
    std::size_t const li = static_cast<std::size_t>(l);
    if (!LineOn(l) || !p.routeHerbs || !LineOf(l).tierCount || gLineRoute[li].empty())
        return;
    std::uint32_t const guid = Low(bot);
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    std::vector<std::uint32_t> rooms;
    {
        std::lock_guard<std::mutex> guard(gLock);
        rooms = gLines[li][T(alliance)].v.rooms;
    }
    std::uint32_t total = 0;
    for (std::uint32_t const r : rooms)
        total += r;
    if (rooms.size() != gLineRoute[li].size() ||
        !RoutesCloth(true, AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) && !IsRole(bot), false, total))
        return;
    std::vector<std::vector<Stack>> stacks;
    for (std::uint32_t const item : gLineRoute[li])
        stacks.push_back(LooseStacks(bot, item));
    RoutePlan plan = PlanRoute(stacks, rooms);
    if (plan.picks.empty())
        return;
    {
        std::lock_guard<std::mutex> guard(gLock);
        std::vector<std::uint32_t>& live = gLines[li][T(alliance)].v.rooms;
        if (live.size() != plan.units.size() || gHeld.size() + plan.picks.size() > kMaxHeld)
            return;
        for (std::size_t i = 0; i < live.size(); ++i)
            live[i] -= std::min(live[i], plan.units[i]);
        gHeld.insert(plan.picks.begin(), plan.picks.end());
    }
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<DonateOperation>(bot->GetGUID(), alliance, std::move(plan.picks), l));
}

// Errand sell stop (RouteRaw): a cohort adventurer mails its ore / leather to its kind's house rep, each item within
// its own room (RawCap); the stacks are held back from this stop's sales. Mirrors RouteLine.
static void RouteRaw(Player* bot)
{
    std::uint32_t const guid = Low(bot);
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    if (!AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) || IsRole(bot))
        return;
    for (std::size_t k = 0; k < kRawKinds; ++k)
    {
        if (gRawHouse[k] >= AutoWowGuilds::Houses().size())
            continue;
        std::vector<std::uint32_t> rooms(kRawItems, 0);
        {
            std::lock_guard<std::mutex> guard(gLock);
            std::copy(gRawRooms[T(alliance)][k].begin(), gRawRooms[T(alliance)][k].end(), rooms.begin());
        }
        std::vector<std::vector<Stack>> stacks;
        for (std::size_t i = 0; i < kRawItems; ++i)
            stacks.push_back(LooseStacks(bot, kRawLists[k][i]));
        RoutePlan plan = PlanRoute(stacks, rooms);
        if (plan.picks.empty())
            continue;
        {
            std::lock_guard<std::mutex> guard(gLock);
            std::array<std::uint32_t, kRawItems>& live = gRawRooms[T(alliance)][k];
            if (gHeld.size() + plan.picks.size() > kMaxHeld)
                return;
            for (std::size_t i = 0; i < kRawItems; ++i)
                live[i] -= std::min(live[i], plan.units[i]);
            gHeld.insert(plan.picks.begin(), plan.picks.end());
        }
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RawDonateOperation>(bot->GetGUID(), alliance, k, std::move(plan.picks)));
    }
}

void RouteCloth(Player* bot)
{
    Params const& p = detail::gParams;
    if (!detail::gEnabled || !bot)
        return;
    for (ProductLine const& L : kCatalog)
        if (L.tierCount)
            RouteLine(L.id, bot);
    if (p.routeRaw)
        RouteRaw(bot);
    if (!LineOn(Line::Bags))
        return;
    std::uint32_t const guid = Low(bot);
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    if (p.tiers)
    {
        // Every tier's cloth, each within its own room.
        std::array<std::uint32_t, kTierCount> rooms{};
        {
            std::lock_guard<std::mutex> guard(gLock);
            rooms = gTeams[T(alliance)].clothRooms;
        }
        std::uint32_t total = 0;
        for (std::uint32_t const r : rooms)
            total += r;
        if (!RoutesCloth(p.routeCloth, AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) && !IsRole(bot),
                         bot->HasSkill(SKILL_TAILORING), total))
            return;
        std::vector<std::uint32_t> pick;
        std::array<std::uint32_t, kTierCount> units{};
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            std::vector<Stack> const stacks = LooseStacks(bot, kTiers[i].cloth);
            for (std::uint32_t const g : PickStacks(stacks, rooms[i]))
            {
                if (pick.size() >= kMaxMailStacks)
                    break;
                pick.push_back(g);
                for (Stack const& s : stacks)
                    if (s.guid == g)
                        units[i] += s.count;
            }
        }
        if (pick.empty())
            return;
        {
            std::lock_guard<std::mutex> guard(gLock);
            TeamState& ts = gTeams[T(alliance)];
            if (gHeld.size() + pick.size() > kMaxHeld)
                return;
            for (std::size_t i = 0; i < kTierCount; ++i)
                ts.clothRooms[i] -= std::min(ts.clothRooms[i], units[i]);
            gHeld.insert(pick.begin(), pick.end());
        }
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<DonateOperation>(bot->GetGUID(), alliance, std::move(pick)));
        return;
    }
    std::uint32_t room = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        room = gTeams[T(alliance)].clothRoom;
    }
    if (!RoutesCloth(p.routeCloth, AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) && !IsRole(bot),
                     bot->HasSkill(SKILL_TAILORING), room))
        return;
    std::vector<Stack> stacks;
    PlayerbotAI* const selfAI = AutoWowSelfCraft::Enabled() ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    for (std::uint32_t const c : kCloth)
        for (Stack const& s : LooseStacks(bot, c))
        {
            // AutoWow.SelfCraft: the bandage cloth reserve stays with the bot.
            Item* item = selfAI ? bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(s.guid)) : nullptr;
            if (item && AutoWowSelfCraft::ReservedCloth(selfAI, bot, item))
                continue;
            stacks.push_back(s);
        }
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

std::uint32_t PriceOf(std::uint32_t item) { return BuyOf(item); }
std::uint32_t SellPriceOf(std::uint32_t item) { return SellOf(item); }

void QueueMarketBuys(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> buys)
{
    if (!Market() || !rep || buys.empty())
        return;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MarketBuyOperation>(rep->GetGUID(), ObjectGuid(auctioneerRawGuid), std::move(buys)));
}

void QueueMarketCancels(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> cancels)
{
    if (!Market() || !rep || cancels.empty())
        return;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MarketCancelOperation>(rep->GetGUID(), ObjectGuid(auctioneerRawGuid), std::move(cancels)));
}

void OnAuctionSold(Player* bot, std::uint32_t item, std::uint32_t count, std::int64_t gold)
{
    if (!Market() || !bot || gold <= 0 || RoleOf(Low(bot)).role != Role::Rep)
        return;
    std::uint64_t const copper = std::min<std::uint64_t>(std::uint64_t(gold), bot->GetMoney());
    bool const ok = copper && AutoWowGuilds::Deposit(bot, copper);
    Emit(bot, ok ? Reason::Sold : Reason::Refused, 0, item, count, copper, Low(bot), 0, ok ? nullptr : "sold");
}

LineView LineViewOf(Line l, bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    return gLines[static_cast<std::size_t>(l)][T(alliance)].v;
}

Stations const& LineStationsOf(Line l, bool alliance) { return gLineStations[static_cast<std::size_t>(l)][T(alliance)]; }
std::vector<std::uint32_t> const& LineLearnSpells(Line l) { return gLineLearn[static_cast<std::size_t>(l)]; }

void SetLineArtisanWant(Line l, bool alliance, std::uint64_t copper)
{
    std::lock_guard<std::mutex> guard(gLock);
    gLines[static_cast<std::size_t>(l)][T(alliance)].artisanWant = copper;
}

void ClearLineSurplus(Line l, bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    gLines[static_cast<std::size_t>(l)][T(alliance)].v.surplus = {};
}

std::vector<MaterialNeed> MaterialDemand(bool alliance)
{
    std::vector<MaterialNeed> out;
    if (!detail::gEnabled)
        return out;
    Params const& p = detail::gParams;
    std::size_t const t = T(alliance);
    std::lock_guard<std::mutex> guard(gLock);
    TeamState const& ts = gTeams[t];
    if (LineOn(Line::Bags) && p.routeCloth)
    {
        if (p.tiers)
        {
            // Cloth the bag artisan can bolt now (linen always; stock beyond its reach waits, lane C): its goal /
            // skill-up tier first, a tier below both no more (ClothDemandOf).
            for (std::size_t i = 0; i < kTierCount; ++i)
                if (ClothDemand const d = ClothDemandOf(i, ts.artisanSkill, ts.goal, ts.skillup); d != ClothDemand::Done)
                    out.push_back({kTiers[i].cloth, ts.clothRooms[i], d == ClothDemand::First});
        }
        else
            out.push_back({kLinen, ts.clothRoom});
    }
    if (p.routeHerbs)
        for (ProductLine const& L : kCatalog)
        {
            std::size_t const li = static_cast<std::size_t>(L.id);
            LineView const& v = gLines[li][t].v;
            if (!L.tierCount || !LineOn(L.id) || v.rooms.size() != gLineRoute[li].size())
                continue;
            for (std::size_t i = 0; i < gLineRoute[li].size(); ++i)
                if (UsableNow(L, gLineRoute[li][i], v.artisanSkill))
                    out.push_back({gLineRoute[li][i], v.rooms[i]});
        }
    if (p.routeRaw)
        for (std::size_t k = 0; k < kRawKinds; ++k)
            if (gRawHouse[k] < AutoWowGuilds::Houses().size())
                for (std::size_t i = 0; i < kRawItems; ++i)
                    out.push_back({kRawLists[k][i], gRawRooms[t][k][i]});
    return out;
}

void RequestGrant(Player* bot, std::uint64_t need)
{
    if (!Outfit() || !bot || !need)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    gGrantRequests[Low(bot)] = need;
}

bool GrantPending(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    return gGrantRequests.count(guid) != 0;
}

void EmitOutfit(Player* bot, Reason r, std::uint32_t item, std::uint64_t copper, char const* op)
{
    if (!bot)
        return;
    std::string const house = AutoWowGuilds::HouseNameOf(AutoWowGuilds::HouseGuildOf(bot));
    LOG_INFO("playerbots", "[Supply] player={} {} house={} item={} copper={} line=outfit{}{}", bot->GetName(),
             ReasonName(r), house, item, copper, op ? " op=" : "", op ? op : "");
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSupply(bot, ReasonName(r),
                                       LedgerFields(house, 0, item, item ? 1 : 0, copper, item ? Low(bot) : 0,
                                                    item ? 0 : Low(bot), op) +
                                           ",\"line\":\"outfit\"");
}
}  // namespace AutoWowSupply
