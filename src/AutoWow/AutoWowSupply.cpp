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
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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
    // Tiers only (TierTick):
    std::array<std::uint32_t, kTierCount> clothRooms{};
    std::uint8_t product = kNoTier;
    std::uint32_t productWant = 0;  // members' want for the product tier (overlord)
    std::uint8_t skillup = kNoTier;
    bool skillupBag = false;
    std::uint32_t artisanSkill = 0;
    std::array<std::uint32_t, kTierCount> surplusBags{};
    std::vector<MarketWant> buy;
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

bool IsRole(std::uint32_t guid) { return gRoles.count(guid) != 0; }

// The online cohort members of the team (roles excluded), unranked.
std::vector<Member> Members(bool alliance, std::uint32_t slots)
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
            // Tiers route wool and silk too; off, only linen stacks are ever picked, so the extra rows stay 0.
            std::array<std::uint32_t, std::size(kTierCloth)> units{};
            for (std::uint32_t const g : guids_)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < units.size(); ++k)
                        if (item->GetEntry() == kTierCloth[k])
                            units[k] += item->GetCount();
            ok = rep && rep != Low(bot) && AutoWowGuilds::SendItems(Low(bot), rep, guids_, "AutoWoW cloth");
            for (std::size_t k = 0; k < units.size(); ++k)
                if (units[k])
                    Emit(bot, ok ? Reason::Donate : Reason::Refused, 0, kTierCloth[k], units[k], 0, Low(bot), rep,
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
        out.surplus = ts.surplus;
        out.surplusBags = ts.surplusBags;
    }
    out.artisanWant = ts.artisanWant;  // no map update runs during the world tick: nothing set since the copy
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
    p.tiers = sConfigMgr->GetOption<bool>("AutoWow.Supply.Tiers", false);
    p.market = sConfigMgr->GetOption<bool>("AutoWow.Supply.Market", false);
    p.buyMaxPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BuyMaxPct", 400);
    p.buyBudget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BuyBudget", 500);
    p.sellKeep = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.SellKeep", 60);
    p.listFloat = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ListFloat", 1000);
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
    if (p.tiers)
        LOG_INFO("server.loading", "[Supply] tiers on: {} tiers, cloth cap {} per cloth; market={} buy_max_pct={} "
                 "buy_budget={} sell_keep={} list_float={}", kTierCount, p.clothCap, p.market, p.buyMaxPct, p.buyBudget,
                 p.sellKeep, p.listFloat);
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
        if (!RoutesCloth(p.routeCloth, AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) && !IsRole(guid),
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

std::uint32_t PriceOf(std::uint32_t item) { return BuyOf(item); }
std::uint32_t SellPriceOf(std::uint32_t item) { return SellOf(item); }

void QueueMarketBuys(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<MarketListing> buys)
{
    if (!Market() || !rep || buys.empty())
        return;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MarketBuyOperation>(rep->GetGUID(), ObjectGuid(auctioneerRawGuid), std::move(buys)));
}

void OnAuctionSold(Player* bot, std::uint32_t item, std::uint32_t count, std::int64_t gold)
{
    if (!Market() || !bot || gold <= 0 || RoleOf(Low(bot)).role != Role::Rep)
        return;
    std::uint64_t const copper = std::min<std::uint64_t>(std::uint64_t(gold), bot->GetMoney());
    bool const ok = copper && AutoWowGuilds::Deposit(bot, copper);
    Emit(bot, ok ? Reason::Sold : Reason::Refused, 0, item, count, copper, Low(bot), 0, ok ? nullptr : "sold");
}
}  // namespace AutoWowSupply
