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
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "AhBrokerPolicy.h"
#include "AuctionHouseMgr.h"
#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GearUpgradePolicy.h"
#include "Item.h"
#include "ItemUsageValue.h"
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
#include "WeaponOrderPolicy.h"
#include "TradePolicy.h"
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
// GearBootstrap: a gear line's default radius (Engineering 466 / 491, Mining 381 / 458 yards; the role step walks to
// any station within its 600-yard town radius). The nearest station wins, so a line found within 400 is unchanged.
constexpr std::uint32_t kBootstrapYards = 600;

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
// Gear lines (lane V), read-only after LoadConfig: each line's equipment recipes best first (RankGearRecipes) and the
// AutoWow.Supply.PriorityGuids list.
std::array<std::vector<std::uint8_t>, kLineCount> gGearRank;
std::vector<std::uint32_t> gPriority;
std::array<std::vector<std::uint32_t>, kLineCount> gGearYield;  // per recipe: items per cast (GearYield)
std::array<std::array<std::uint8_t, 2>, kLineCount> gLineSpec{};
// WeaponOrders (world thread only): order id -> first seen (ms), and per team the open order of a (guid, slot) need.
std::unordered_map<std::uint32_t, std::uint64_t> gWeaponOrderSeen;
std::array<std::map<std::pair<std::uint32_t, std::uint8_t>, std::uint32_t>, 2> gWeaponOrderOf;  // AutoWow.Supply.Spec.<House>.<Team> (ScanGearNeeds)

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
    // DirectRoutes (TierTick): the artisan donors mail directly (0 = the rep) and the tiers whose cloth it takes.
    std::uint32_t direct = 0;
    std::array<bool, kTierCount> directTier{};
    // DemandOnly (overlord): the members' bag want per tier (a skill-up's consumer).
    std::array<std::uint32_t, kTierCount> wants{};
    // cloth_gear on the bag house (TierTick): the cloth its open orders still lack per tier (GearClothShort).
    std::array<std::uint32_t, kTierCount> gearCloth{};
    // RouteBagExtra (TierTick): donor room per tier for its extra reagent (ExtraRoom; 0 off).
    std::array<std::uint32_t, kTierCount> extraRooms{};
};
struct LineState
{
    LineView v;
    std::uint64_t artisanWant = 0;
    // DirectRoutes (LineTick): the artisan donors mail directly (0 = the rep) and, per RouteItems, what it takes.
    std::uint32_t direct = 0;
    std::vector<bool> directItem;
    std::array<std::uint32_t, kMaxLineTiers> wants{};  // DemandOnly (overlord): want per tier (a skill-up's consumer)
    std::vector<GearOrder> orders;                     // gear lines (GearTick): the open order, ranked
    OrderPost post;                                    // OrderBackoffMs (GearTick): the order rows' backoff
};
std::mutex gLock;
std::array<TeamState, 2> gTeams;
std::array<std::array<LineState, 2>, kLineCount> gLines;  // [line][team]
std::uint32_t gNextOrderId = 0;  // run-scoped, never reused (both teams)
std::unordered_set<std::uint32_t> gHeld;
// MailOrders: the teams' open orders (gLock: the world tick rebuilds them, random sellers reserve units), and the COD
// fills in flight, (rep, seller, item) -> units / copper (world thread only; bounded, in memory: a fill pending at a
// restart is returned to its seller).
std::array<std::vector<MailOrder>, 2> gOrders;
std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, CodPending> gCodPending;
constexpr std::size_t kMaxCodPending = 4096;

// Finished-bag market: the world thread publishes live-member snapshots; map threads reserve exact auction identities
// under gLock before dispatch. Successful reservations remain coverage until their exact native item GUID appears in
// the representative's bags or mailbox. No Player, Item or AuctionEntry pointer crosses the queue.
enum class FinishedBagPurchaseState : std::uint8_t
{
    Queued = 0,
    Purchased = 1,
    Arrived = 2
};

struct FinishedBagPending
{
    FinishedBagBuy buy;
    std::uint32_t representative = 0;
    bool alliance = false;
    FinishedBagPurchaseState state = FinishedBagPurchaseState::Queued;
};

std::array<FinishedBagView, 2> gFinishedBagViews;
std::map<std::uint32_t, FinishedBagPending> gFinishedBagPending;  // auction id -> exact immutable purchase
constexpr std::size_t kMaxFinishedBagPending = 64;

// Raw materials (RouteRaw, lane G): kind 0 = ore -> AutoWow.Supply.House.Ore, 1 = leather -> .House.Leather, 2 = stone
// -> .House.Stone (lane AA; default Tinkers, not a default house: off unless AutoWow.Guilds.Houses names it).
constexpr std::size_t kRawKinds = 3, kRawItems = 3;
static_assert(std::size(kOre) == kRawItems && std::size(kLeather) == kRawItems && std::size(kStone) == kRawItems);
constexpr std::uint32_t const* kRawLists[kRawKinds] = {kOre, kLeather, kStone};
constexpr char const* kRawKeys[kRawKinds] = {"Ore", "Leather", "Stone"};
constexpr char const* kRawDefaultHouse[kRawKinds] = {"Smiths", "Tanners", "Tinkers"};
std::array<std::size_t, kRawKinds> gRawHouse{};  // read-only after LoadConfig; houses.size() = kind off
std::array<std::string, kRawKinds> gRawHouseName;
std::array<std::array<std::array<std::uint32_t, kRawItems>, kRawKinds>, 2> gRawRooms{};  // [team][kind][item], gLock

// Outfit (AutoWow.Supply.Outfit): pending grant requests, guid -> need copper (gLock; map threads add, the world
// tick takes). Per-bot level windows and per-team hour budgets are world thread only (in memory, reset on restart).
std::unordered_map<std::uint32_t, std::uint64_t> gGrantRequests;
std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> gGrantRooms;  // AutoWow.Errands.Mounts:
                                                                                          // guid -> room, budget/h
std::unordered_map<std::uint32_t, GrantWindow> gGrantWindows;
std::array<GrantBudget, 2> gGrantBudgets{};
std::array<GrantBudget, 2> gMountGrantBudgets{};  // world thread only

// World thread only.
std::array<std::array<std::vector<GearNeed>, 2>, kLineCount> gGearNeeds;  // gear lines: the last scan's ranked needs
                                                                         // (served ones leave; bounded by recipients x 19)
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

// AutoWow.Gear.Flow: the player's item `guid` may leave by mail (not bound: the mail helper refuses it as bad_item).
bool Tradeable(Player* p, std::uint32_t guid)
{
    Item* item = p->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid));
    return item && item->CanBeTraded(true);
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

// RepStore: units of `entry` in the player's character bank (bank slots and bank bags); 0 with RepStore off.
std::uint32_t Banked(Player* p, std::uint32_t entry)
{
    std::uint32_t n = 0;
    if (!p || !detail::gParams.repStore)
        return 0;
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); item && item->GetEntry() == entry)
            n += item->GetCount();
    for (uint8 bag = BANK_SLOT_BAG_START; bag < BANK_SLOT_BAG_END; ++bag)
        if (Bag* b = p->GetBagByPos(bag))
            for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                if (Item* item = b->GetItemByPos(slot); item && item->GetEntry() == entry)
                    n += item->GetCount();
    return n;
}

// Bags, bank (RepStore: the rep's stash counts toward the house caps) and mailbox.
std::uint32_t HeldUnits(Player* p, std::uint32_t entry) { return Loose(p, entry) + Banked(p, entry) + InMail(p, entry); }

// Mails in the player's box, the deleted ones (gone at its next save) not counted.
std::uint32_t LiveMails(Player* p)
{
    std::uint32_t n = 0;
    for (Mail const* m : p->GetMails())
        if (m && m->state != MAIL_STATE_DELETED)
            ++n;
    return n;
}

// Donor room at a rep: online with bag / bank room and (RepStore) fewer than RepMailCap mails (soak-s48-full-r1).
bool RepReady(Player* rep)
{
    return rep && AutoWowGuilds::RepFreeSlots(rep) > 0 &&
           (!detail::gParams.repStore || MailRoom(LiveMails(rep), detail::gParams.repMailCap));
}

// SendItems: nullptr = sent, else the refused row's op: "mailbox_full" (RepStore) when the receiver's box was at the
// core cap, else `op`.
char const* Send(std::uint32_t from, std::uint32_t to, std::vector<std::uint32_t> const& items,
                 std::string const& subject, char const* op)
{
    char const* why = nullptr;
    if (AutoWowGuilds::SendItems(from, to, items, subject, &why))
        return nullptr;
    return detail::gParams.repStore && why && std::string_view(why) == "receiver_mailbox_full" ? "mailbox_full" : op;
}

bool GeneralBag(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
           proto->InventoryType == INVTYPE_BAG && !proto->BagFamily && proto->ContainerSlots;
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
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(mi.item_template);
                    GeneralBag(proto) && proto->ContainerSlots >= slots)
                    ++out.incoming;
    return out;
}

// A working role bot (an apprentice artisan is an ordinary member, ActiveRoleOf).
bool IsRole(Player* p) { return ActiveRoleOf(p).role != Role::None; }

// The house artisan when it works: online, in world and past its apprentice phase; else nullptr (the chain then
// treats it as offline: no feed, pay or deliveries while it adventures).
Player* Working(Player* art) { return art && art->IsInWorld() && IsRole(art) ? art : nullptr; }

// DirectRoutes (world thread): the artisan works at home with bag room above its make-room target and mailbox room
// (ArtisanTakes): donors and its own deliveries skip the rep hop.
bool TakesDirect(Player* art)
{
    Params const& p = detail::gParams;
    Player* a = Working(art);
    if (!a)
        return false;
    Home const& home = RoleOf(Low(a)).home;
    std::int64_t const dx = std::int64_t(a->GetPositionX()) - home.x, dy = std::int64_t(a->GetPositionY()) - home.y;
    bool const atHome = home.set && a->GetMapId() == home.map && dx * dx + dy * dy <= std::int64_t(p.homeYards) * p.homeYards;
    return ArtisanTakes(true, atHome, a->GetFreeInventorySpace(), p.artisanFreeSlots,
                        MailRoom(LiveMails(a), p.repMailCap));
}

// The online configured rep of house `i` (a role bot), else nullptr.
Player* RoleRep(std::size_t i, bool alliance)
{
    std::uint32_t const gid = AutoWowGuilds::HouseGuildId(i, alliance);
    Player* rep = Online(gid ? AutoWowGuilds::RepOf(gid) : 0);
    return rep && rep->IsInWorld() && IsRole(rep) ? rep : nullptr;
}

// The online cohort members of the team (roles excluded), unranked. RepStore: plus every configured house rep of the
// team (Member.rep, ranked first; the bag-house rep keeps its own share and wears it, EquipOwnBags).
std::vector<Member> Members(bool alliance, std::uint32_t slots)
{
    std::vector<Member> members;
    for (AutoWowGuilds::GuidRange const& r : AutoWowGuilds::Cohort())
        for (std::uint64_t g = r.lo; g <= r.hi; ++g)
        {
            Player* m = Online(static_cast<std::uint32_t>(g));
            if (m && m->IsInWorld() && (m->GetTeamId() == TEAM_ALLIANCE) == alliance && !IsRole(m))
            {
                Member member = MemberOf(m, slots);
                member.priority = PriorityOf(gPriority, member.guid) != kNoPriority;
                members.push_back(member);
            }
        }
    for (std::size_t i = 0; detail::gParams.repStore && i < AutoWowGuilds::Houses().size(); ++i)
        if (Player* rep = RoleRep(i, alliance))
        {
            members.push_back(MemberOf(rep, slots));
            members.back().rep = true;
        }
    return members;
}

void AddFinishedBagMailCoverage(Player* holder, std::uint32_t recipient, FinishedBagCoverageSource source,
                                FinishedBagView& view)
{
    if (!holder)
        return;
    for (Mail const* mail : holder->GetMails())
        if (mail && mail->state != MAIL_STATE_DELETED)
            for (MailItemInfo const& mi : mail->items)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(mi.item_template); GeneralBag(proto))
                {
                    Player* recipientPlayer = recipient ? Online(recipient) : nullptr;
                    if ((recipient && (!recipientPlayer || recipientPlayer->CanUseItem(proto) != EQUIP_ERR_OK)) ||
                        (!recipient && (proto->Bonding == BIND_QUEST_ITEM || holder->HasQuestForItem(proto->ItemId))))
                        continue;
                    FinishedBagCoverage coverage{recipient, proto->ContainerSlots, 1, source, {}};
                    if (!recipient)
                        for (FinishedBagNeed const& need : view.needs)
                            if (Player* member = Online(need.recipient);
                                member && member->CanUseItem(proto) == EQUIP_ERR_OK &&
                                std::find(coverage.usableRecipients.begin(), coverage.usableRecipients.end(),
                                          need.recipient) == coverage.usableRecipients.end())
                                coverage.usableRecipients.push_back(need.recipient);
                    if (recipient || !coverage.usableRecipients.empty())
                        view.coverage.push_back(std::move(coverage));
                }
}

FinishedBagView SnapshotFinishedBags(bool alliance, Player* representative,
                                     std::unordered_set<std::uint32_t> const& excludedAuctions = {},
                                     bool includeRepresentativeLoose = true)
{
    FinishedBagView out;
    std::vector<FinishedBagMember> members;
    std::vector<Player*> recipients;
    for (AutoWowGuilds::GuidRange const& range : AutoWowGuilds::Cohort())
        for (std::uint64_t raw = range.lo; raw <= range.hi; ++raw)
        {
            Player* member = Online(static_cast<std::uint32_t>(raw));
            std::uint32_t const guid = member ? Low(member) : 0;
            if (!member || !member->IsInWorld() || (member->GetTeamId() == TEAM_ALLIANCE) != alliance ||
                IsRole(member) || PriorityOf(gPriority, guid) == kNoPriority)
                continue;
            FinishedBagMember facts;
            facts.guid = guid;
            facts.level = member->GetLevel();
            facts.classMask = member->getClassMask();
            facts.raceMask = member->getRaceMask();
            facts.priority = true;
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            {
                std::size_t const i = slot - INVENTORY_SLOT_BAG_START;
                Bag* bag = member->GetBagByPos(slot);
                facts.replaceable[i] = !bag || GeneralBag(bag->GetTemplate());
                facts.equippedSlots[i] = bag && facts.replaceable[i] ? bag->GetBagSize() : 0;
            }
            members.push_back(facts);
            recipients.push_back(member);
        }
    out.needs = FinishedBagNeeds(members);
    for (Player* member : recipients)
    {
        std::uint32_t const guid = Low(member);
        ForEachLoose(member,
                     [&](Item* item)
                     {
                         if (GeneralBag(item->GetTemplate()) && !item->IsNotEmptyBag() &&
                             member->CanUseItem(item->GetTemplate()) == EQUIP_ERR_OK)
                             out.coverage.push_back({guid, item->GetTemplate()->ContainerSlots, item->GetCount(),
                                                     FinishedBagCoverageSource::RecipientLoose});
                     });
        AddFinishedBagMailCoverage(member, guid, FinishedBagCoverageSource::RecipientMail, out);
    }
    if (representative)
    {
        if (includeRepresentativeLoose)
            ForEachLoose(representative,
                         [&](Item* item)
                         {
                             ItemTemplate const* proto = item->GetTemplate();
                             if (GeneralBag(proto) && !item->IsNotEmptyBag() && item->CanBeTraded(true) &&
                                 proto->Bonding != BIND_QUEST_ITEM && !representative->HasQuestForItem(proto->ItemId))
                             {
                                 FinishedBagCoverage coverage{0,
                                                              proto->ContainerSlots,
                                                              item->GetCount(),
                                                              FinishedBagCoverageSource::RepresentativeLoose,
                                                              {}};
                                 for (FinishedBagNeed const& need : out.needs)
                                     if (Player* member = Online(need.recipient);
                                         member && member->CanUseItem(proto) == EQUIP_ERR_OK &&
                                         std::find(coverage.usableRecipients.begin(), coverage.usableRecipients.end(),
                                                   need.recipient) == coverage.usableRecipients.end())
                                         coverage.usableRecipients.push_back(need.recipient);
                                 if (!coverage.usableRecipients.empty())
                                     out.coverage.push_back(std::move(coverage));
                             }
                         });
        AddFinishedBagMailCoverage(representative, 0, FinishedBagCoverageSource::RepresentativeMail, out);
    }
    std::lock_guard<std::mutex> guard(gLock);
    for (auto const& [auction, pending] : gFinishedBagPending)
        if (pending.alliance == alliance && pending.state != FinishedBagPurchaseState::Arrived &&
            !excludedAuctions.count(auction))
            for (std::uint32_t const recipient : pending.buy.recipients)
                out.coverage.push_back(
                    {recipient, pending.buy.listing.slots, 1, FinishedBagCoverageSource::InFlight, {}, auction});
    return out;
}

bool FinishedBagItemVisible(Player* representative, std::uint64_t rawGuid)
{
    bool found = false;
    ForEachLoose(representative, [&](Item* item) { found = found || item->GetGUID().GetRawValue() == rawGuid; });
    if (found)
        return true;
    for (Mail const* mail : representative->GetMails())
        if (mail && mail->state != MAIL_STATE_DELETED)
            for (MailItemInfo const& mi : mail->items)
                if (mi.item_guid == ObjectGuid(rawGuid).GetCounter())
                    return true;
    return false;
}

void ClearFinishedBagPending(std::uint32_t auction)
{
    std::lock_guard<std::mutex> guard(gLock);
    gFinishedBagPending.erase(auction);
    for (FinishedBagView& view : gFinishedBagViews)
        ClearFinishedBagCoverage(view, auction);
}

void UpdateFinishedBagRecipients(std::uint32_t auction, std::vector<std::uint32_t> const& recipients)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gFinishedBagPending.find(auction);
    if (it == gFinishedBagPending.end())
        return;
    it->second.buy.recipients = recipients;
    for (FinishedBagView& view : gFinishedBagViews)
        ClearFinishedBagCoverage(view, auction);
    FinishedBagView& view = gFinishedBagViews[T(it->second.alliance)];
    for (std::uint32_t const recipient : recipients)
        view.coverage.push_back(
            {recipient, it->second.buy.listing.slots, 1, FinishedBagCoverageSource::InFlight, {}, auction});
}

void MarkFinishedBagPurchased(std::uint32_t auction)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gFinishedBagPending.find(auction);
    if (it != gFinishedBagPending.end())
        it->second.state = FinishedBagPurchaseState::Purchased;
}

void FinishedBagReceipt(Player* representative, FinishedBagListing const& listing, std::uint32_t recipient,
                        char const* result)
{
    LOG_INFO("playerbots",
             "[Supply] finished_bag team={} rep={} auction={} item={} item_guid={} slots={} count={} "
             "buyout={} recipient={} result={}",
             representative->GetTeamId() == TEAM_ALLIANCE ? "alliance" : "horde", Low(representative),
             listing.auctionId, listing.item, listing.itemGuid, listing.slots, listing.count, listing.buyout, recipient,
             result);
}

void FinishedBagTick(bool alliance)
{
    Player* representative = RoleRep(gBagHouse, alliance);
    if (!representative)
    {
        std::lock_guard<std::mutex> guard(gLock);
        gFinishedBagViews[T(alliance)] = FinishedBagView{};
        return;
    }

    {
        std::lock_guard<std::mutex> guard(gLock);
        for (auto& [auction, pending] : gFinishedBagPending)
            if (pending.alliance == alliance && pending.state == FinishedBagPurchaseState::Purchased &&
                FinishedBagItemVisible(representative, pending.buy.listing.itemGuid))
                pending.state = FinishedBagPurchaseState::Arrived;
    }

    // Exclude the loose bags from this delivery view: they are the candidates, not pre-existing coverage. Recipient
    // stock/mail, representative auction mail and other in-flight purchases still suppress duplicate delivery.
    FinishedBagView const deliveryView = SnapshotFinishedBags(alliance, representative, {}, false);
    std::vector<FinishedBagHeld> held;
    ForEachLoose(representative,
                 [&](Item* item)
                 {
                     ItemTemplate const* proto = item->GetTemplate();
                     bool const ordinary = GeneralBag(proto);
                     bool const tradeable = ordinary && !item->IsNotEmptyBag() && item->CanBeTraded(true) &&
                                            proto->Bonding != BIND_QUEST_ITEM &&
                                            !representative->HasQuestForItem(proto->ItemId);
                     FinishedBagHeld bag{item->GetEntry(),
                                         static_cast<std::uint32_t>(item->GetGUID().GetCounter()),
                                         proto ? proto->ContainerSlots : 0,
                                         ordinary,
                                         tradeable,
                                         {}};
                     if (tradeable)
                         for (FinishedBagNeed const& need : deliveryView.needs)
                             if (Player* recipient = Online(need.recipient);
                                 recipient && recipient->CanUseItem(proto) == EQUIP_ERR_OK &&
                                 std::find(bag.usableRecipients.begin(), bag.usableRecipients.end(), need.recipient) ==
                                     bag.usableRecipients.end())
                                 bag.usableRecipients.push_back(need.recipient);
                     held.push_back(std::move(bag));
                 });
    for (FinishedBagDelivery const& delivery : PlanFinishedBagDeliveries(std::move(held), deliveryView))
    {
        char const* why =
            Send(Low(representative), delivery.recipient, {delivery.itemGuid}, "AutoWoW finished bag", "bag_market");
        LOG_INFO("playerbots",
                 "[Supply] finished_bag_delivery team={} rep={} item={} item_guid={} slots={} "
                 "recipient={} result={}",
                 alliance ? "alliance" : "horde", Low(representative), delivery.item, delivery.itemGuid, delivery.slots,
                 delivery.recipient, why ? why : "sent");
        if (!why)
        {
            {
                std::lock_guard<std::mutex> guard(gLock);
                for (auto it = gFinishedBagPending.begin(); it != gFinishedBagPending.end(); ++it)
                    if (it->second.representative == Low(representative) &&
                        ObjectGuid(it->second.buy.listing.itemGuid).GetCounter() == delivery.itemGuid)
                    {
                        std::uint32_t const auction = it->first;
                        gFinishedBagPending.erase(it);
                        for (FinishedBagView& view : gFinishedBagViews)
                            ClearFinishedBagCoverage(view, auction);
                        break;
                    }
            }
            Emit(representative, Reason::Deliver, 0, delivery.item, 1, 0, Low(representative), delivery.recipient,
                 "bag_market", delivery.recipient);
        }
    }

    FinishedBagView view = SnapshotFinishedBags(alliance, representative);
    std::lock_guard<std::mutex> guard(gLock);
    gFinishedBagViews[T(alliance)] = std::move(view);
}

// RepStore (world thread): a rep wears its own loose general bags in its empty bag slots (BagsToWear), with the stock
// swap the equip action uses. The bags are house output it holds (delivered by mail or kept), never created.
void EquipOwnBags(Player* rep)
{
    std::vector<uint8> empty;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!rep->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            empty.push_back(slot);
    if (empty.empty())
        return;
    std::vector<LooseBag> bags;
    ForEachLoose(rep,
                 [&](Item* item)
                 {
                     if (GeneralBag(item->GetTemplate()) && !item->IsNotEmptyBag() &&
                         !ReservedFinishedBagItem(rep, item->GetGUID().GetRawValue()))
                         bags.push_back({static_cast<std::uint32_t>(item->GetGUID().GetCounter()),
                                         item->GetTemplate()->ContainerSlots});
                 });
    std::size_t k = 0;
    for (std::uint32_t const g : BagsToWear(std::move(bags), static_cast<std::uint32_t>(empty.size())))
    {
        Item* bag = rep->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g));
        if (!bag || k >= empty.size())
            continue;
        uint8 const slot = empty[k++];
        std::uint32_t const entry = bag->GetEntry();
        rep->SwapItem(static_cast<uint16>((bag->GetBagSlot() << 8) | bag->GetSlot()),
                      static_cast<uint16>((INVENTORY_SLOT_BAG_0 << 8) | slot));
        LOG_INFO("playerbots", "[Supply] rep bag bot={} item={} slot={} worn={} free={}", rep->GetName(), entry, slot,
                 rep->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != nullptr, rep->GetFreeInventorySpace());
    }
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

// The items a bag-house donation carries (a row each): the tier cloths, plus (RouteBagExtra) the tiers' extra reagents.
std::vector<std::uint32_t> BagEntries()
{
    std::vector<std::uint32_t> out(std::begin(kTierCloth), std::end(kTierCloth));
    if (detail::gParams.routeBagExtra)
        for (Tier const& t : kTiers)
            if (t.extra && std::find(out.begin(), out.end(), t.extra) == out.end())
                out.push_back(t.extra);
    return out;
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
        if (bot && detail::gParams.directRoutes)
            ok = ExecuteDirect(bot);
        else if (bot)
        {
            bool const bags = line_ == Line::Bags;
            std::size_t const house = bags ? gBagHouse : gLineHouse[static_cast<std::size_t>(line_)];
            std::uint32_t const gid = AutoWowGuilds::HouseGuildId(house, alliance_);
            std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
            // Tiers route wool and silk too; off, only linen stacks are ever picked, so the extra rows stay 0.
            std::vector<std::uint32_t> const entries = bags ? BagEntries() : gLineRoute[static_cast<std::size_t>(line_)];
            std::vector<std::uint32_t> units(entries.size(), 0);
            for (std::uint32_t const g : guids_)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < units.size(); ++k)
                        if (item->GetEntry() == entries[k])
                            units[k] += item->GetCount();
            char const* const why = rep && rep != Low(bot)
                ? Send(Low(bot), rep, guids_, bags ? "AutoWoW cloth" : "AutoWoW materials", "donate") : "donate";
            ok = !why;
            for (std::size_t k = 0; k < units.size(); ++k)
                if (units[k])
                    EmitLine(line_, bot, ok ? Reason::Donate : Reason::Refused, 0, entries[k], units[k], 0, Low(bot),
                             rep, why);
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
    // DirectRoutes: the stacks the house artisan takes now (TeamState / LineState direct) go to it, the rest to the
    // rep; one mail each, a donate row per entry with its receiver (the donation credit), squad deliveries noted.
    bool ExecuteDirect(Player* bot)
    {
        bool const bags = line_ == Line::Bags;
        std::size_t const li = static_cast<std::size_t>(line_), t = T(alliance_);
        std::uint32_t const gid = AutoWowGuilds::HouseGuildId(bags ? gBagHouse : gLineHouse[li], alliance_);
        std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
        std::vector<std::uint32_t> const entries = bags ? BagEntries() : gLineRoute[li];
        std::vector<bool> takes(entries.size(), false);
        std::uint32_t art = 0;
        {
            std::lock_guard<std::mutex> guard(gLock);
            art = bags ? gTeams[t].direct : gLines[li][t].direct;
            for (std::size_t k = 0; art && k < entries.size(); ++k)
                takes[k] = bags ? k < kTierCount && gTeams[t].directTier[k]
                                : k < gLines[li][t].directItem.size() && gLines[li][t].directItem[k];
        }
        if (art && !TakesDirect(Online(art)))
            art = 0;  // left home / filled up since the tick: the rep takes it all
        std::vector<std::uint32_t> toArt, toRep;
        for (std::uint32_t const g : guids_)
        {
            Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g));
            std::size_t k = 0;
            while (item && k < entries.size() && item->GetEntry() != entries[k])
                ++k;
            (art && item && k < entries.size() && takes[k] ? toArt : toRep).push_back(g);
        }
        bool ok = false;
        auto mail = [&](std::uint32_t to, std::vector<std::uint32_t> const& guids)
        {
            if (guids.empty())
                return;
            std::vector<std::uint32_t> units(entries.size(), 0);
            for (std::uint32_t const g : guids)
                if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(g)))
                    for (std::size_t k = 0; k < units.size(); ++k)
                        if (item->GetEntry() == entries[k])
                            units[k] += item->GetCount();
            char const* const why = to && to != Low(bot)
                ? Send(Low(bot), to, guids, bags ? "AutoWoW cloth" : "AutoWoW materials", "donate") : "donate";
            ok = ok || !why;
            for (std::size_t k = 0; k < units.size(); ++k)
            {
                if (!units[k])
                    continue;
                EmitLine(line_, bot, why ? Reason::Refused : Reason::Donate, 0, entries[k], units[k], 0, Low(bot), to, why);
                if (!why && AutoWowSquad::Enabled())
                    AutoWowSquad::NoteDelivered(bot, entries[k], units[k], to);
            }
        };
        mail(art, toArt);
        mail(rep, toRep);
        return ok;
    }

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
            char const* const why = rep && rep != Low(bot) ? Send(Low(bot), rep, guids_, "AutoWoW materials", "donate")
                                                           : "donate";
            ok = !why;
            for (std::size_t k = 0; k < kRawItems; ++k)
            {
                if (!units[k])
                    continue;
                Reason const r = ok ? Reason::Donate : Reason::Refused;
                LOG_INFO("playerbots", "[Supply] player={} {} house={} item={} count={} from={} to={}{}{}",
                         bot->GetName(), ReasonName(r), gRawHouseName[kind_], kRawLists[kind_][k], units[k], Low(bot),
                         rep, ok ? "" : " op=", ok ? "" : why);
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::EmitSupply(bot, ReasonName(r),
                                                   LedgerFields(gRawHouseName[kind_], 0, kRawLists[kind_][k], units[k], 0,
                                                                Low(bot), rep, why) +
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
        bool const repReady = RepReady(rep);
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
bool GearOrderUses(bool alliance, std::size_t house, std::uint32_t item);
void DirectBags(bool alliance, Player* art, Player* rep, std::uint32_t repGuid, std::uint32_t gid, TeamState& ts);

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
    ts.clothRoom = ClothRoom(stock, p.clothCap, RepReady(rep));

    // Artisan -> rep: finished bags, paid per bag from the treasury, work XP per bag.
    if (art && rep && art != rep)
        if (std::vector<std::uint32_t> const bags = Guids(LooseStacks(art, gBagItem), kMaxMailStacks); !bags.empty())
        {
            std::uint32_t const n = static_cast<std::uint32_t>(bags.size());
            char const* const why = Send(Low(art), repGuid, bags, "AutoWoW bags", "deliver");
            if (!why)
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
                Emit(art, Reason::Refused, ts.orderId, gBagItem, n, 0, Low(art), repGuid, why);
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
            char const* const why = Send(repGuid, Low(art), stacks, "AutoWoW linen", "feed");
            Emit(rep, why ? Reason::Refused : Reason::Feed, ts.orderId, kLinen, units, 0, repGuid, Low(art), why);
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
                Emit(who, Reason::Order, ts.orderId, gBagItem, n, 0, 0, gArtisan[t], nullptr,
                     p.demandOnly ? std::int64_t(ranked.empty() ? 0 : ranked.front().guid) : -1);
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
            if (d.guid == repGuid)
                continue;  // RepStore: the rep's own share stays with it (worn at the next tick, EquipOwnBags)
            if (char const* const why = Send(repGuid, d.guid, give, "AutoWoW bag", "deliver"))
            {
                Emit(rep, Reason::Refused, ts.orderId, gBagItem, static_cast<std::uint32_t>(give.size()), 0, repGuid,
                     d.guid, why);
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
    ts.clothRooms = ClothRooms(stock, p.clothCap, RepReady(rep));
    ts.artisanSkill = art ? art->GetSkillValue(SKILL_TAILORING) : 0;
    ts.gearCloth = {};
    if (std::size_t const g = static_cast<std::size_t>(Line::ClothGear); LineOn(Line::ClothGear) && gLineHouse[g] == gBagHouse)
    {
        std::vector<GearOrder> orders;
        {
            std::lock_guard<std::mutex> guard(gLock);
            orders = gLines[g][t].orders;
        }
        ts.gearCloth = GearClothShort(GearTable(LineOf(Line::ClothGear)), orders, house);
    }

    // DirectRoutes: artisan -> ranked members first (one hop instead of two); what is left goes to the rep below.
    if (p.directRoutes && art && gid)
        DirectBags(alliance, art, rep, repGuid, gid, ts);

    // Artisan -> rep: finished bags of every tier, paid per bag, work XP per bag; product bags count down the order.
    // RepStore: every tier in one mail (at most kMaxMailStacks bags); off: one mail per tier.
    if (art && rep && art != rep)
    {
        std::array<std::vector<std::uint32_t>, kTierCount> picks;
        auto ship = [&]()
        {
            std::vector<std::uint32_t> all;
            for (std::vector<std::uint32_t> const& g : picks)
                all.insert(all.end(), g.begin(), g.end());
            if (all.empty())
                return;
            char const* const why = Send(Low(art), repGuid, all, "AutoWoW bags", "deliver");
            for (std::size_t i = 0; i < kTierCount; ++i)
            {
                Tier const& tier = kTiers[i];
                std::uint32_t const n = static_cast<std::uint32_t>(picks[i].size());
                picks[i].clear();
                if (!n)
                    continue;
                if (why)
                {
                    Emit(art, Reason::Refused, ts.orderId, tier.bag, n, 0, Low(art), repGuid, why);
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
        };
        std::size_t used = 0;
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            picks[i] = Guids(LooseStacks(art, kTiers[i].bag), static_cast<std::uint32_t>(kMaxMailStacks - used));
            if (p.repStore)
                used += picks[i].size();
            else
                ship();
        }
        ship();
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
            {
                // DemandOnly: the order names its first consumer (the top-ranked member wanting the product tier).
                std::int64_t consumer = -1;
                if (p.demandOnly)
                {
                    std::vector<Member> const top = RankedMembers(alliance, kTiers[product].bagSlots);
                    consumer = top.empty() ? 0 : top.front().guid;
                }
                Emit(who, Reason::Order, ts.orderId, kTiers[product].bag, n, 0, 0, gArtisan[t], nullptr, consumer);
            }
        ts.wants = wants;
    }

    // Skill-up recipe: the cheapest known non-grey recipe the house holds one cast of (none at the rank cap). DemandOnly:
    // one whose output has a consumer first (a bag some member wants; a bolt a wanted bag or the house's gear order uses).
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
            if (p.demandOnly)
            {
                skillups[skillups.size() - 2].consumer = ts.wants[i] > 0 || GearOrderUses(alliance, gBagHouse, tier.bolt);
                skillups.back().consumer = ts.wants[i] > 0;
            }
        }
    int const pick = PickSkillupFor(ts.artisanSkill, skillups, true, p.demandOnly);
    ts.skillup = pick < 0 ? kNoTier : skillups[pick].tier;
    ts.skillupBag = pick >= 0 && skillups[pick].bag;

    // Rep -> artisan: the product order's cloth and extras, else one skill-up stock (SkillupCloth). RepStore: all in
    // one mail (at most kMaxMailStacks stacks); off: one mail per item.
    std::vector<std::uint32_t> feedStacks;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> fed;  // (item, units) of feedStacks
    auto flushFeed = [&]()
    {
        if (feedStacks.empty())
            return;
        char const* const why = Send(repGuid, Low(art), feedStacks, "AutoWoW materials", "feed");
        for (auto const& [item, sent] : fed)
            Emit(rep, why ? Reason::Refused : Reason::Feed, ts.orderId, item, sent, 0, repGuid, Low(art), why);
        feedStacks.clear();
        fed.clear();
    };
    auto feed = [&](std::uint32_t item, std::uint32_t units)
    {
        std::vector<std::uint32_t> stacks = PickStacks(LooseStacks(rep, item), units);
        stacks.resize(std::min<std::size_t>(stacks.size(), kMaxMailStacks - feedStacks.size()));
        if (stacks.empty())
            return;
        std::uint32_t sent = 0;
        for (Stack const& s : LooseStacks(rep, item))
            if (std::find(stacks.begin(), stacks.end(), s.guid) != stacks.end())
                sent += s.count;
        feedStacks.insert(feedStacks.end(), stacks.begin(), stacks.end());
        fed.push_back({item, sent});
        if (!p.repStore)
            flushFeed();
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
        flushFeed();
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
    if ((p.market || p.mailOrders) && art)
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
        // cloth_gear orders: each cloth at least what they lack.
        // ponytail: max of the bag and gear wants, not their sum; sum them if both lines starve on one cloth.
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            auto const it = std::find_if(ts.buy.begin(), ts.buy.end(),
                                         [&](MarketWant const& w) { return w.item == kTiers[i].cloth; });
            if (it != ts.buy.end())
                it->units = std::max(it->units, ts.gearCloth[i]);
            else
                want(kTiers[i].cloth, ts.gearCloth[i]);
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
                if (d.guid == repGuid)
                    continue;  // RepStore: the rep's own share stays with it (worn at the next tick, EquipOwnBags)
                if (char const* const why = Send(repGuid, d.guid, give, "AutoWoW bag", "deliver"))
                {
                    Emit(rep, Reason::Refused, ts.orderId, tier.bag, n, 0, repGuid, d.guid, why);
                    continue;
                }
                given[d.guid] += n;
                Emit(rep, Reason::Deliver, ts.orderId, tier.bag, n, 0, repGuid, d.guid);
                GrantXp(rep, XpFor(p.repXpPerDeal, rep), ts.orderId, tier.bag, n);
            }
        }
    }

    // DirectRoutes: the artisan takes its product / skill-up tier's cloth itself while TakesDirect, under ClothCap of
    // the house stock (rep + artisan).
    if (p.directRoutes)
    {
        bool const takes = TakesDirect(art);
        ts.direct = takes ? Low(art) : 0;
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            ts.directTier[i] = takes && ((i == ts.product && ts.remaining) || i == ts.skillup);
            if (ts.directTier[i])
                ts.clothRooms[i] =
                    ClothRoom(HeldUnits(rep, kTiers[i].cloth) + HeldUnits(art, kTiers[i].cloth), p.clothCap, true);
        }
    }

    // RouteBagExtra: each tier's extra reagent (Heavy Leather) the wanted bags the house cloth makes still lack.
    if (p.routeBagExtra)
        for (std::size_t i = 0; i < kTierCount; ++i)
        {
            Tier const& tier = kTiers[i];
            ts.extraRooms[i] = ExtraRoom(tier, art && art->HasSpell(tier.bagSpell), ts.wants[i],
                                         BagsFrom(tier.recipe, house(tier.cloth), house(tier.bolt)),
                                         tier.extra ? house(tier.extra) : 0, RepReady(rep));
        }

    std::lock_guard<std::mutex> guard(gLock);
    TeamState& out = gTeams[t];
    out.extraRooms = ts.extraRooms;
    out.orderId = ts.orderId;
    out.remaining = ts.remaining;
    out.clothRooms = ts.clothRooms;
    out.direct = ts.direct;
    out.directTier = ts.directTier;
    out.artisanSkill = ts.artisanSkill;
    out.skillup = ts.skillup;
    out.skillupBag = ts.skillupBag;
    out.buy = ts.buy;
    out.gearCloth = ts.gearCloth;
    if (overlord)
    {
        out.product = ts.product;
        out.productWant = ts.productWant;
        out.goal = ts.goal;
        out.surplus = ts.surplus;
        out.surplusBags = ts.surplusBags;
        out.wants = ts.wants;
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
            sm.mana = m->GetMaxPower(POWER_MANA) > 0;
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
    ProductLine const L = ActiveLine(line);
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
    bool const repReady = RepReady(rep);
    v.rooms.assign(route.size(), 0);
    for (std::size_t i = 0; i < route.size(); ++i)
        v.rooms[i] = ClothRoom(HeldUnits(rep, route[i]), p.herbCap, repReady);
    v.artisanSkill = art ? art->GetSkillValue(L.skillLine) : 0;
    std::array<bool, kMaxLineTiers> known{};
    for (std::size_t i = 0; i < L.tierCount; ++i)
        known[i] = art && art->HasSpell(L.tiers[i].spell);

    // The need: RankStock, PotionTiers a need per family (RankStockTiers).
    auto rankStock = [&]()
    {
        std::vector<StockMember> const members = StockMembers(alliance, L);
        return p.potionTiers ? RankStockTiers(L, members, known, p.potionTarget)
                             : RankStock(L, members, known, p.potionTarget);
    };

    // The artisan's current target (the order, else the skill-up recipe) and its casts: a Craft reagent of it
    // stays with the artisan.
    std::uint8_t const target = v.remaining && v.product != kNoTier ? v.product : v.skillup;
    std::uint32_t const targetCasts = v.remaining && v.product != kNoTier ? v.remaining : p.skillupCasts;

    // DirectRoutes: artisan -> members below PotionTarget first (one hop instead of two), from the stacks it may ship.
    if (p.directRoutes && art && gid)
    {
        std::vector<StockNeed> const need = rankStock();
        for (std::size_t i = L.tierCount; i-- > 0;)
        {
            LineTier const& tier = L.tiers[i];
            std::vector<Stack> const stacks = LooseStacks(art, tier.product);
            std::vector<std::uint32_t> const free = SellStacks(stacks, CraftReserve(L, target, targetCasts, tier.product));
            std::vector<Stack> avail;
            for (Stack const& st : stacks)
                if (std::find(free.begin(), free.end(), st.guid) != free.end())
                    avail.push_back(st);
            for (StackDelivery const& d : PlanStackDeliveries(need, static_cast<std::uint8_t>(i), std::move(avail)))
            {
                if (char const* const why = Send(Low(art), d.guid, d.stacks, "AutoWoW supplies", "deliver"))
                {
                    EmitLine(line, art, Reason::Refused, v.orderId, tier.product, d.units, 0, Low(art), d.guid, why);
                    continue;
                }
                EmitLine(line, art, Reason::Deliver, v.orderId, tier.product, d.units, 0, Low(art), d.guid);
                if (i == v.product)
                    v.remaining -= std::min(v.remaining, d.units);
                std::uint64_t const pay = BagPay(SellOf(tier.product), p.potionPayPct, d.units);
                bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
                EmitLine(line, art, paid ? Reason::Pay : Reason::Refused, v.orderId, tier.product, d.units, pay, 0,
                         Low(art), paid ? nullptr : "pay");
                GrantXp(art, XpFor(p.workXpPerItem, art) * d.units, v.orderId, tier.product, d.units, line);
                if (rep)
                    GrantXp(rep, XpFor(p.repXpPerDeal, rep), v.orderId, tier.product, d.units, line);
            }
        }
    }

    // Artisan -> rep: finished products of every tier, paid per unit, work XP per unit; product units count
    // down the order. RepStore: every tier in one mail (at most kMaxMailStacks stacks); off: one mail per tier.
    if (art && rep && art != rep)
    {
        std::array<std::vector<std::uint32_t>, kMaxLineTiers> picks;
        std::array<std::uint32_t, kMaxLineTiers> units{};
        auto ship = [&]()
        {
            std::vector<std::uint32_t> all;
            for (std::vector<std::uint32_t> const& g : picks)
                all.insert(all.end(), g.begin(), g.end());
            if (all.empty())
                return;
            char const* const why = Send(Low(art), repGuid, all, "AutoWoW goods", "deliver");
            for (std::size_t i = 0; i < L.tierCount; ++i)
            {
                LineTier const& tier = L.tiers[i];
                std::uint32_t const n = units[i];
                bool const any = !picks[i].empty();
                picks[i].clear();
                units[i] = 0;
                if (!any)
                    continue;
                if (why)
                {
                    EmitLine(line, art, Reason::Refused, v.orderId, tier.product, n, 0, Low(art), repGuid, why);
                    continue;
                }
                EmitLine(line, art, Reason::Deliver, v.orderId, tier.product, n, 0, Low(art), repGuid);
                if (i == v.product)
                    v.remaining -= std::min(v.remaining, n);
                std::uint64_t const pay = BagPay(SellOf(tier.product), p.potionPayPct, n);
                bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
                EmitLine(line, art, paid ? Reason::Pay : Reason::Refused, v.orderId, tier.product, n, pay, 0,
                         Low(art), paid ? nullptr : "pay");
                GrantXp(art, XpFor(p.workXpPerItem, art) * n, v.orderId, tier.product, n, line);
            }
        };
        std::size_t used = 0;
        for (std::size_t i = 0; i < L.tierCount; ++i)
        {
            LineTier const& tier = L.tiers[i];
            std::vector<Stack> const stacks = LooseStacks(art, tier.product);
            picks[i] = SellStacks(stacks, CraftReserve(L, target, targetCasts, tier.product));
            picks[i].resize(std::min<std::size_t>(picks[i].size(), kMaxMailStacks - used));
            for (Stack const& s : stacks)
                if (std::find(picks[i].begin(), picks[i].end(), s.guid) != picks[i].end())
                    units[i] += s.count;
            if (p.repStore)
                used += picks[i].size();
            else
                ship();
        }
        ship();
    }

    // Need scan (the overlord's order; every tick for the rep's deliveries).
    std::vector<StockNeed> ranked;
    if (overlord || rep)
        ranked = rankStock();
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
        std::uint8_t const product = p.potionTiers ? PickLineProduct(L, opts) : PickProduct(opts);
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
        if (p.potionTiers)
        {
            // Every tier (the line above shows the first three): product item:want/craftable/known/surplus.
            std::string tiers;
            for (std::size_t i = 0; i < L.tierCount; ++i)
                tiers += (i ? " " : "") + std::to_string(L.tiers[i].product) + ":" + std::to_string(wants[i]) + "/" +
                         std::to_string(opts[i].craftable) + "/" + (known[i] ? "1" : "0") + "/" +
                         std::to_string(v.surplus[i]);
            LOG_INFO("playerbots", "[Supply] overlord tiers line={} team={} skill={} skillup_last={} tiers={}", L.name,
                     alliance ? "alliance" : "horde", v.artisanSkill, v.skillup == kNoTier ? -1 : int(v.skillup), tiers);
        }
        if (n)
            if (Player* who = art ? art : rep)
            {
                std::int64_t consumer = -1;
                if (p.demandOnly)
                {
                    auto const top = std::find_if(ranked.begin(), ranked.end(),
                                                  [&](StockNeed const& s) { return s.tier == product; });
                    consumer = top == ranked.end() ? 0 : top->guid;
                }
                EmitLine(line, who, Reason::Order, v.orderId, L.tiers[product].product, n, 0, 0, gLineArtisan[li][t],
                         nullptr, consumer);
            }
        ts.wants = wants;
    }

    // Skill-up recipe: the cheapest known non-grey recipe the house holds one cast of (none at the rank cap).
    std::vector<SkillupOption> skillups;
    if (art && v.artisanSkill < art->GetMaxSkillValue(L.skillLine))
        for (std::size_t i = 0; i < L.tierCount; ++i)
            skillups.push_back({L.tiers[i].spell, static_cast<std::uint8_t>(i), false, known[i], L.tiers[i].grey,
                                Casts(L, i, house) > 0, CastCost(L, i), ts.wants[i] > 0});
    int const pick = PickSkillupFor(v.artisanSkill, skillups, true, p.demandOnly);
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
    // RepStore: every lack in one mail (at most kMaxMailStacks stacks); off: one mail per item.
    if (art && rep && art != rep)
    {
        std::vector<std::uint32_t> feedStacks;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> fed;  // (item, units) of feedStacks
        auto flushFeed = [&]()
        {
            if (feedStacks.empty())
                return;
            char const* const why = Send(repGuid, Low(art), feedStacks, "AutoWoW materials", "feed");
            for (auto const& [item, sent] : fed)
                EmitLine(line, rep, why ? Reason::Refused : Reason::Feed, v.orderId, item, sent, 0, repGuid, Low(art),
                         why);
            feedStacks.clear();
            fed.clear();
        };
        for (Lack const& k : lacks)
        {
            if (k.source == Source::Vendor)
                continue;
            std::vector<std::uint32_t> stacks = PickStacks(LooseStacks(rep, k.item), k.units);
            stacks.resize(std::min<std::size_t>(stacks.size(), kMaxMailStacks - feedStacks.size()));
            if (stacks.empty())
                continue;
            std::uint32_t sent = 0;
            for (Stack const& s : LooseStacks(rep, k.item))
                if (std::find(stacks.begin(), stacks.end(), s.guid) != stacks.end())
                    sent += s.count;
            feedStacks.insert(feedStacks.end(), stacks.begin(), stacks.end());
            fed.push_back({k.item, sent});
            if (!p.repStore)
                flushFeed();
        }
        flushFeed();
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
    if ((p.market || p.mailOrders) && art)
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
                if (char const* const why = Send(repGuid, d.guid, d.stacks, "AutoWoW supplies", "deliver"))
                {
                    EmitLine(line, rep, Reason::Refused, v.orderId, tier.product, d.units, 0, repGuid, d.guid, why);
                    continue;
                }
                EmitLine(line, rep, Reason::Deliver, v.orderId, tier.product, d.units, 0, repGuid, d.guid);
                GrantXp(rep, XpFor(p.repXpPerDeal, rep), v.orderId, tier.product, d.units, line);
            }
        }

    // DirectRoutes: the artisan takes the routed reagents it can use now itself while TakesDirect, under HerbCap of the
    // house stock (rep + artisan).
    if (p.directRoutes)
    {
        bool const takes = TakesDirect(art);
        ts.direct = takes ? Low(art) : 0;
        ts.directItem.assign(route.size(), false);
        for (std::size_t k = 0; k < route.size() && k < v.rooms.size(); ++k)
        {
            ts.directItem[k] = takes && UsableNow(L, route[k], v.artisanSkill);
            if (ts.directItem[k])
                v.rooms[k] = ClothRoom(HeldUnits(rep, route[k]) + HeldUnits(art, route[k]), p.herbCap, true);
        }
    }

    std::lock_guard<std::mutex> guard(gLock);
    gLines[li][t].direct = ts.direct;
    gLines[li][t].directItem = ts.directItem;
    if (overlord)
        gLines[li][t].wants = ts.wants;
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

// ---- gear lines (lane V: Products cloth_gear / leather_gear; SupplyPolicy.h kTailorGear / kLeatherGear) ----

// World thread: an enabled gear line of house `house` has an open order entry with `item` as a direct reagent (a bolt
// skill-up then has a consumer, DemandOnly).
bool GearOrderUses(bool alliance, std::size_t house, std::uint32_t item)
{
    std::lock_guard<std::mutex> guard(gLock);
    for (ProductLine const& L : kCatalog)
    {
        std::size_t const li = static_cast<std::size_t>(L.id);
        if (!L.gearCount || !LineOn(L.id) || gLineHouse[li] != house)
            continue;
        RecipeTable const G = GearTable(L);  // SmithBars: the table the orders index
        for (GearOrder const& o : gLines[li][T(alliance)].orders)
            for (std::size_t k = 0; o.recipe < G.tierCount && k < kMaxReagents; ++k)
                if (G.tiers[o.recipe].reagents[k].item == item)
                    return true;
    }
    return false;
}

// The team's gear recipients: AutoWow.Supply.PriorityGuids, the cohort and the squad roster, online bots of the team
// that are no working role, each once (ascending guid).
std::vector<Player*> GearRecipients(bool alliance)
{
    std::vector<std::uint32_t> guids = gPriority;
    for (AutoWowGuilds::GuidRange const& r : AutoWowGuilds::Cohort())
        for (std::uint64_t g = r.lo; g <= r.hi; ++g)
            guids.push_back(static_cast<std::uint32_t>(g));
    for (std::uint32_t const g : AutoWowSquad::Roster(alliance))
        guids.push_back(g);
    std::sort(guids.begin(), guids.end());
    guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
    std::vector<Player*> out;
    for (std::uint32_t const g : guids)
    {
        Player* m = Online(g);
        if (m && m->IsInWorld() && (m->GetTeamId() == TEAM_ALLIANCE) == alliance && !IsRole(m) &&
            PlayerbotsMgr::instance().GetPlayerbotAI(m))
            out.push_back(m);
    }
    return out;
}

// Summed item level of the worn equipment (shirt and tabard left out; an empty slot adds 0).
std::uint32_t IlvlSum(Player* m)
{
    std::uint32_t n = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD)
            if (Item* it = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                n += it->GetTemplate()->ItemLevel;
    return n;
}

// The stock "item upgrade" value (the one quest rewards and the errand gear shop use) rates the piece EQUIP (empty slot)
// or REPLACE (beats the worn piece for this bot's class / spec weights, armor type), at its level.
bool GearUpgrade(PlayerbotAI* ai, Player* m, ItemTemplate const* proto)
{
    if (!ai || !proto || proto->RequiredLevel > m->GetLevel() || m->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    ItemUsage const u =
        ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", std::to_string(proto->ItemId))->Get();
    return u == ITEM_USAGE_EQUIP || u == ITEM_USAGE_REPLACE;
}

// ---- ammo (lane AA Tinkers: Products eng, NeedRule::AmmoStock / Consumer::LoadAmmo) ----

std::uint32_t AmmoDamage(ItemTemplate const* proto)
{
    return proto ? static_cast<std::uint32_t>(proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) : 0;
}

bool Bullet(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ITEM_SUBCLASS_BULLET;
}

bool GunHunter(Player* m)
{
    Item const* ranged = m->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    return m->getClass() == CLASS_HUNTER && ranged && ranged->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
           ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_GUN;
}

// WantsAmmo for the recipient: its bullets in bags and mailbox, its loaded ammo (0 when none of it is left).
bool AmmoWanted(Player* m, ItemTemplate const* proto)
{
    std::uint32_t held = 0;
    ForEachLoose(m, [&](Item* item) {
        if (Bullet(item->GetTemplate()))
            held += item->GetCount();
    });
    for (Mail const* mail : m->GetMails())
        if (mail && mail->state != MAIL_STATE_DELETED)
            for (MailItemInfo const& mi : mail->items)
                if (Bullet(sObjectMgr->GetItemTemplate(mi.item_template)))
                    if (Item* item = m->GetMItem(mi.item_guid))
                        held += item->GetCount();
    std::uint32_t const loaded = m->GetUInt32Value(PLAYER_AMMO_ID);
    std::uint32_t const loadedDamage =
        loaded && m->GetItemCount(loaded, false) ? AmmoDamage(sObjectMgr->GetItemTemplate(loaded)) : 0;
    return WantsAmmo(m->getClass() == CLASS_HUNTER, GunHunter(m), Bullet(proto), m->GetLevel(), proto->RequiredLevel,
                     AmmoDamage(proto), loadedDamage, held, detail::gParams.ammoTarget);
}

// The recipient still wants the product: shot (AmmoWanted) or an equipment upgrade (GearUpgrade).
bool Consumes(PlayerbotAI* ai, Player* m, ItemTemplate const* proto)
{
    if (proto && proto->InventoryType == INVTYPE_AMMO)
        return AmmoWanted(m, proto);
    return GearUpgrade(ai, m, proto);
}

// Consumer::LoadAmmo (world thread, maps idle): a gun hunter loads the strongest house shot in its bags usable at its
// level (LoadsAmmo). The stock ammo equip never fires (ItemUsageValue::QueryItemUsageForAmmo returns NONE for every
// class), so a delivery would otherwise sit in the bags.
void LoadAmmo(Player* m, RecipeTable const& G)
{
    if (!GunHunter(m))
        return;
    ItemTemplate const* best = nullptr;
    for (std::size_t i = 0; i < G.tierCount; ++i)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(G.tiers[i].product);
        if (Bullet(proto) && proto->RequiredLevel <= m->GetLevel() && m->GetItemCount(proto->ItemId, false) &&
            (!best || AmmoDamage(proto) > AmmoDamage(best)))
            best = proto;
    }
    std::uint32_t const loaded = m->GetUInt32Value(PLAYER_AMMO_ID);
    if (!best || best->ItemId == loaded ||
        !LoadsAmmo(AmmoDamage(best), AmmoDamage(sObjectMgr->GetItemTemplate(loaded)),
                   loaded ? m->GetItemCount(loaded, false) : 0))
        return;
    m->SetAmmo(best->ItemId);
    LOG_INFO("playerbots", "[Supply] ammo load bot={} item={} was={} loaded={}", m->GetName(), best->ItemId, loaded,
             m->GetUInt32Value(PLAYER_AMMO_ID) == best->ItemId);
}

// Lane tinkers2 (EngGuns): the tool categories (Spell TotemCategory) a recipe's cast needs.
std::vector<std::uint32_t> ToolCategories(std::uint32_t spell)
{
    std::vector<std::uint32_t> out;
    if (SpellInfo const* s = sSpellMgr->GetSpellInfo(spell))
        for (std::uint32_t const c : s->TotemCategory)
            if (c)
                out.push_back(c);
    return out;
}

// Vendor value of a gear line piece: its sell value; shot sells for 0: its vendor lot price (one cast's 200) instead.
std::uint32_t PieceValue(std::uint32_t item)
{
    ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
    return !t ? 0 : t->SellPrice ? t->SellPrice : t->BuyPrice;
}

// Overlord (world thread, maps idle): each recipient's equipment slots the best known product of the line upgrades
// (recipes best first, gGearRank), skipping slots a line product is already on its way to (loose or in the mailbox).
// ponytail: a product whose item level is not above the worn piece's is never asked about (each stock value call
// creates a scratch item); a lower-level piece with better weights is missed. Ask always if that matters.
std::vector<GearNeed> ScanGearNeeds(std::size_t li, bool alliance, std::vector<bool> const& known)
{
    RecipeTable const G = GearTable(kCatalog[li]);
    std::vector<GearNeed> out;
    if (std::none_of(known.begin(), known.end(), [](bool k) { return k; }))
        return out;
    for (Player* m : GearRecipients(alliance))
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(m);
        std::uint32_t done = 0;  // slot mask: incoming, then served by this scan
        auto incoming = [&](std::uint32_t entry)
        {
            ItemTemplate const* proto = TierOf(G, entry) != kNoTier ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            // Shot on its way counts in AmmoWanted's bullets instead.
            if (proto && proto->InventoryType != INVTYPE_NON_EQUIP && proto->InventoryType != INVTYPE_AMMO)
                if (uint8 const slot = ai->FindEquipSlot(proto, NULL_SLOT, true); slot < EQUIPMENT_SLOT_END)
                    done |= 1u << slot;
        };
        ForEachLoose(m, [&](Item* item) { incoming(item->GetEntry()); });
        for (Mail const* mail : m->GetMails())
            if (mail && mail->state != MAIL_STATE_DELETED)
                for (MailItemInfo const& mi : mail->items)
                    incoming(mi.item_template);
        std::uint32_t const ilvl = IlvlSum(m), prio = PriorityOf(gPriority, Low(m));
        for (std::uint8_t const r : gGearRank[li])
        {
            if (G.tiers[r].family == kFamilyBridge || !SpecAllows(G.tiers[r].spec, gLineSpec[li][T(alliance)]))
                continue;  // paid skill-up output goes to house stock / sale, never a member gear order
            ItemTemplate const* proto = r < known.size() && known[r] ? sObjectMgr->GetItemTemplate(G.tiers[r].product)
                                                                     : nullptr;
            if (!proto || proto->RequiredLevel > m->GetLevel() || m->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (proto->InventoryType == INVTYPE_AMMO)
            {
                // Shot (lane AA): one need per recipient, the best known shot it wants (kAmmoSlot).
                if (!(done >> kAmmoSlot & 1u) && AmmoWanted(m, proto))
                {
                    done |= 1u << kAmmoSlot;
                    out.push_back({Low(m), r, kAmmoSlot, prio, ilvl});
                }
                continue;
            }
            uint8 const slot = ai->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot >= EQUIPMENT_SLOT_END || (done >> slot & 1u))
                continue;
            Item const* worn = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if ((worn && worn->GetTemplate()->ItemLevel >= proto->ItemLevel) || !GearUpgrade(ai, m, proto))
                continue;
            done |= 1u << slot;
            out.push_back({Low(m), r, slot, prio, ilvl});
        }
    }
    return out;
}

// GearBootstrap (world thread, overlord): no need of the line has a recipe the artisan knows. The needs of the recipes
// it can reach (Smiths: every rank its level can train; other lines: its rank or the next trainable rank) but does not
// know; the first one only
// skill blocks gets a skill-up order (PlanGearSkillup): SkillupCasts casts of the cheapest known recipe of the line's
// own skill (Engineering's smelts level Mining) still below grey and not already stocked (held < SkillupCasts). Logged.
// ClimbCastable: `castable` (per recipe: the house casts it now) steers each pick (PreferCastable); empty = off.
std::vector<GearOrder> BootstrapGearOrder(Line line, bool alliance, Player* art, std::vector<bool> const& known,
                                          std::vector<std::uint32_t> const& held, std::vector<bool> const& castable)
{
    Params const& p = detail::gParams;
    ProductLine const& L = LineOf(line);
    RecipeTable const G = GearTable(L);
    std::uint32_t const skill = art->GetSkillValue(L.skillLine), cap = art->GetMaxSkillValue(L.skillLine);
    std::uint32_t const reach = line == Line::MailGear ? BootstrapDemandHorizon(G, cap, art->GetLevel())
                                                        : ReachSkill(cap, art->GetLevel());
    std::vector<bool> blocked(G.tierCount, false);
    std::vector<SkillupOption> options;
    std::vector<bool> owns(G.tierCount, false);  // GearSkillupRestock: of the line's own skill
    for (std::size_t i = 0; i < G.tierCount; ++i)
    {
        LineTier const& tier = G.tiers[i];
        blocked[i] = !known[i] && tier.skill <= reach;
        bool own = false;
        SkillLineAbilityMapBounds const b = sSpellMgr->GetSkillLineAbilityMapBounds(tier.spell);
        for (auto it = b.first; it != b.second && !own; ++it)
            own = it->second->SkillLine == L.skillLine;
        owns[i] = own;
        std::uint64_t cost = 0;  // one cast's reagents at their vendor value
        for (Reagent const& r : tier.reagents)
            if (r.item)
                cost += std::uint64_t(r.source == Source::Vendor ? BuyOf(r.item) : SellOf(r.item)) * r.count;
        options.push_back({tier.spell, static_cast<std::uint8_t>(i), false,
                           known[i] && own && held[i] < p.skillupCasts, tier.grey, true, cost, true});
    }
    std::vector<GearNeed> const need = RankGearNeeds(ScanGearNeeds(static_cast<std::size_t>(line), alliance, blocked));
    std::vector<GearOrder> out;
    if (!castable.empty())
        options = PreferCastable(std::move(options), castable, skill);
    if (skill < cap)
        out = PlanGearSkillup(G, need, skill, options, p.skillupCasts);
    // GearSkillupRestock: every option stocked (none picked) -> again without the stock gate, sized past the artisan's own
    // finished units of the pick.
    if (out.empty() && skill < cap && p.gearSkillupRestock)
    {
        std::vector<std::uint32_t> mine(G.tierCount, 0);
        for (std::size_t i = 0; i < G.tierCount; ++i)
        {
            options[i].known = known[i] && owns[i];
            mine[i] = CastUnits(HeldUnits(art, G.tiers[i].product), GearYield(line, static_cast<std::uint8_t>(i)));
        }
        if (!castable.empty())
            options = PreferCastable(std::move(options), castable, skill);
        out = PlanGearSkillupRestock(G, need, skill, options, mine, p.skillupCasts);
        if (!out.empty())
            LOG_INFO("playerbots", "[Supply] gear bootstrap restock line={} team={} artisan={} skillup={} units={} mine={}",
                     L.name, alliance ? "alliance" : "horde", Low(art), G.tiers[out.front().recipe].spell,
                     out.front().units, mine[out.front().recipe]);
    }
    // ClimbSkillup: no blocked member need at all (an outgrown table) and still below the reach: climb anyway.
    if (out.empty() && need.empty() && p.climbSkillup && skill < cap)
    {
        if (ClimbPastStock())
        {
            // No stock gate, sized past the artisan's own finished units of the pick (PlanClimbSkillupPastStock).
            std::vector<std::uint32_t> mine(G.tierCount, 0);
            for (std::size_t i = 0; i < G.tierCount; ++i)
            {
                options[i].known = known[i] && owns[i];
                mine[i] = CastUnits(HeldUnits(art, G.tiers[i].product), GearYield(line, static_cast<std::uint8_t>(i)));
            }
            if (!castable.empty())
                options = PreferCastable(std::move(options), castable, skill);
            out = PlanClimbSkillupPastStock(skill, std::min(reach, cap), options, mine, p.skillupCasts);
        }
        else
            out = PlanClimbSkillup(skill, std::min(reach, cap), options, p.skillupCasts);
        if (!out.empty())
            LOG_INFO("playerbots", "[Supply] gear bootstrap climb line={} team={} artisan={} skill={}/{} skillup={} casts={}",
                     L.name, alliance ? "alliance" : "horde", Low(art), skill, cap, G.tiers[out.front().recipe].spell,
                     out.front().units);
    }
    LOG_INFO("playerbots", "[Supply] gear bootstrap line={} team={} artisan={} skill={}/{} reach={} blocked={} "
             "top_recipe={} consumer={} skillup={} casts={}", L.name, alliance ? "alliance" : "horde", Low(art), skill,
             cap, reach, need.size(), need.empty() ? 0 : G.tiers[need.front().recipe].spell,
             out.empty() ? 0 : out.front().consumer, out.empty() ? 0 : G.tiers[out.front().recipe].spell,
             out.empty() ? 0 : out.front().units);
    return out;
}

// WeaponOrders (lane smithfocus2; world thread, overlord): the team's pending AutoWow.Gear.NoWhite weapon orders as smith
// gear needs. An order whose bot's worn weapon reached its floor elsewhere, or older than WeaponOrderTimeoutMs, is
// cancelled; an online bot's order maps to the best recipe the artisan knows that the stock scorer rates an upgrade for
// that slot (PickWeaponRecipe), else it waits. `[Supply] weapon_order` logs and `supply` rows (line mail_gear, op
// weapon_order / weapon_fill / weapon_cancel_*) record each step.
std::vector<GearNeed> WeaponOrderNeeds(std::size_t li, bool alliance, std::vector<bool> const& known, Player* who)
{
    Params const& p = detail::gParams;
    RecipeTable const G = GearTable(kCatalog[li]);
    std::size_t const t = T(alliance);
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    std::vector<GearNeed> out;
    gWeaponOrderOf[t].clear();
    for (AutoWowWeaponOrder::Order const& o : AutoWowWeaponOrder::Pending(alliance))
    {
        auto const seen = gWeaponOrderSeen.try_emplace(o.id, now).first->second;
        Player* m = Online(o.guid);
        Item const* worn = m && o.slot < EQUIPMENT_SLOT_END ? m->GetItemByPos(INVENTORY_SLOT_BAG_0, o.slot) : nullptr;
        std::uint32_t const wornIlvl = worn ? worn->GetTemplate()->ItemLevel : 0;
        WeaponOrderVerdict const verdict = JudgeWeaponOrder(wornIlvl, o.minIlvl, seen, now, p.weaponOrderTimeoutMs);
        if (verdict != WeaponOrderVerdict::Keep)
        {
            char const* const why = verdict == WeaponOrderVerdict::Better ? "weapon_cancel_better" : "weapon_cancel_timeout";
            AutoWowWeaponOrder::Cancel(o.id);
            gWeaponOrderSeen.erase(o.id);
            LOG_INFO("playerbots", "[Supply] weapon_order cancel={} oid={} bot={} slot={} worn_ilvl={} min_ilvl={} age_ms={}",
                     why, o.id, o.guid, uint32(o.slot), wornIlvl, o.minIlvl, now - seen);
            if (who)
                EmitLine(Line::MailGear, who, Reason::Refused, o.id, 0, 1, 0, 0, o.guid, why, o.guid);
            continue;
        }
        PlayerbotAI* ai = m && m->IsInWorld() ? PlayerbotsMgr::instance().GetPlayerbotAI(m) : nullptr;
        if (!ai)
            continue;  // offline: the order waits (its timeout still runs)
        std::vector<WeaponCandidate> ranked;
        for (std::uint8_t const r : gGearRank[li])
        {
            ItemTemplate const* proto = r < G.tierCount ? sObjectMgr->GetItemTemplate(G.tiers[r].product) : nullptr;
            if (!proto || proto->Class != ITEM_CLASS_WEAPON)
                continue;
            WeaponCandidate c;
            c.recipe = r;
            c.slot = ai->FindEquipSlot(proto, NULL_SLOT, true);
            c.reqLevel = proto->RequiredLevel;
            c.ilvl = proto->ItemLevel;
            c.known = r < known.size() && known[r];
            c.bridge = G.tiers[r].family == kFamilyBridge;
            c.specOk = SpecAllows(G.tiers[r].spec, gLineSpec[li][t]);
            c.usable = m->CanUseItem(proto) == EQUIP_ERR_OK;
            // The stock scorer only when the cheap gates pass (each call creates a scratch item).
            c.upgrade = c.known && !c.bridge && c.slot == o.slot && c.reqLevel <= m->GetLevel() && c.usable &&
                        GearUpgrade(ai, m, proto);
            ranked.push_back(c);
        }
        std::uint8_t const recipe = PickWeaponRecipe(ranked, o.slot, m->GetLevel(), o.minIlvl);
        bool const first = gWeaponOrderSeen[o.id] == now;
        if (recipe == kNoTier)
        {
            if (first)
                LOG_INFO("playerbots", "[Supply] weapon_order wait oid={} bot={} slot={} min_ilvl={}: no known upgrade recipe",
                         o.id, o.guid, uint32(o.slot), o.minIlvl);
            continue;
        }
        gWeaponOrderOf[t][{o.guid, o.slot}] = o.id;
        out.push_back({o.guid, recipe, o.slot, 0, IlvlSum(m)});
        if (first)
        {
            LOG_INFO("playerbots", "[Supply] weapon_order take oid={} bot={} slot={} recipe={} item={} min_ilvl={}", o.id,
                     o.guid, uint32(o.slot), G.tiers[recipe].spell, G.tiers[recipe].product, o.minIlvl);
            if (who)
                EmitLine(Line::MailGear, who, Reason::Order, o.id, G.tiers[recipe].product, 1, 0, 0, o.guid,
                         "weapon_order", o.guid);
        }
    }
    return out;
}

// WeaponOrders: a piece mailed to a need with an open weapon order fills it.
void FillWeaponOrder(std::size_t t, Player* from, GearNeed const& n, std::uint32_t product)
{
    auto const it = gWeaponOrderOf[t].find({n.guid, n.slot});
    if (it == gWeaponOrderOf[t].end())
        return;
    std::uint32_t const oid = it->second;
    gWeaponOrderOf[t].erase(it);
    gWeaponOrderSeen.erase(oid);
    bool const filled = AutoWowWeaponOrder::Fill(oid);
    LOG_INFO("playerbots", "[Supply] weapon_order fill oid={} bot={} slot={} item={} from={} queued={}", oid, n.guid,
             uint32(n.slot), product, Low(from), filled);
    EmitLine(Line::MailGear, from, Reason::Deliver, oid, product, 1, 0, Low(from), n.guid, "weapon_fill", n.guid);
}

// A gear line over one team (world thread). Overlord: the need scan and the order (top GearMaxOrder needs, plus
// RepStockPerItem per wanted recipe for the rep, less the finished pieces the house holds). Every tick: the artisan's
// finished pieces go straight to their named consumers (still an upgrade), the rest to the rep's stock (each piece paid
// GearPayPct of its vendor value from the house bank, work XP); the rep's stock goes to the needs left (deal XP); the
// artisan's target (PickGearTarget), the rep's feed of what it lacks, its vendor list and copper.
void GearTick(Line line, bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    ProductLine const& L = LineOf(line);
    RecipeTable const G = GearTable(L);
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
    // Finished units of recipe r in order units (casts: a shot cast makes 200; a piece is one).
    auto castUnits = [&](std::uint8_t r, std::uint32_t items) { return CastUnits(items, GearYield(line, r)); };
    v.artisanSkill = art ? art->GetSkillValue(L.skillLine) : 0;
    std::vector<GearNeed>& needs = gGearNeeds[li][t];

    if (overlord)
    {
        std::vector<bool> known(G.tierCount, false);
        std::vector<std::uint32_t> held(G.tierCount, 0);
        for (std::size_t i = 0; i < G.tierCount; ++i)
        {
            known[i] = art && art->HasSpell(G.tiers[i].spell);
            held[i] = castUnits(static_cast<std::uint8_t>(i), house(G.tiers[i].product));
        }
        needs = RankGearNeeds(ScanGearNeeds(li, alliance, known));
        if (WeaponOrders() && line == Line::MailGear)
            needs = MergeWeaponNeeds(needs, WeaponOrderNeeds(li, alliance, known, art ? art : rep));
        std::vector<GearOrder> orders = PlanGearOrders(needs, held, p.gearMaxOrder, p.repStockPerItem);
        if (GearBootstrap() && needs.empty() && art)
        {
            // ClimbCastable: per recipe, the house (rep + artisan) holds what one cast takes now (Casts > 0).
            std::vector<bool> castable;
            if (ClimbCastable())
                for (std::size_t i = 0; i < G.tierCount; ++i)
                    castable.push_back(Casts(G, i, house) > 0);
            orders = BootstrapGearOrder(line, alliance, art, known, held, castable);
        }
        if (!orders.empty() && (ts.orders.empty() || orders.front().recipe != ts.orders.front().recipe))
        {
            std::lock_guard<std::mutex> guard(gLock);
            v.orderId = ++gNextOrderId;
        }
        ts.orders = orders;
        std::uint32_t units = 0;
        for (GearOrder const& o : orders)
            units += o.units;
        LOG_INFO("playerbots", "[Supply] overlord line={} team={} gid={} rep={} artisan={} skill={} needs={} "
                 "top_consumer={} orders={} units={} oid={}", L.name, alliance ? "alliance" : "horde", gid, repGuid,
                 gLineArtisan[li][t], v.artisanSkill, needs.size(), needs.empty() ? 0 : needs.front().guid,
                 orders.size(), units, v.orderId);
        // OrderBackoffMs: an unchanged order's rows back off (0: every scan, as before).
        std::uint64_t const nowMs = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
        if (Player* who = art ? art : rep;
            who && OrderPostDue(ts.post, OrderSig(v.orderId, orders), nowMs, p.orderBackoffMs))
            for (GearOrder const& o : orders)
                EmitLine(line, who, Reason::Order, v.orderId, G.tiers[o.recipe].product, o.units, 0, 0,
                         gLineArtisan[li][t], nullptr, o.consumer);
    }

    // The artisan's pieces shipped: pay, work XP, the order counts down.
    auto paid = [&](std::uint8_t r, std::uint32_t n)
    {
        std::uint32_t const product = G.tiers[r].product;
        std::uint64_t const pay = BagPay(PieceValue(product), p.gearPayPct, n);
        bool const ok = pay && gid && AutoWowGuilds::Pay(gid, art, pay);
        EmitLine(line, art, ok ? Reason::Pay : Reason::Refused, v.orderId, product, n, pay, 0, Low(art),
                 ok ? nullptr : "pay");
        GrantXp(art, XpFor(p.workXpPerItem, art) * n, v.orderId, product, n, line);
        for (GearOrder& o : ts.orders)
            if (o.recipe == r)
            {
                o.units -= std::min(o.units, n);
                break;
            }
    };
    auto pieces = [&](Player* from)
    {
        std::vector<GearItem> out;
        for (std::uint8_t const r : gGearRank[li])
            for (Stack const& st : LooseStacks(from, G.tiers[r].product))
                if (!AutoWowGear::FlowEnabled() || Tradeable(from, st.guid))  // a worn-then-bound piece never mails
                    out.push_back({r, st.guid});
        return out;
    };
    // `from`'s pieces to the ranked needs, one each (re-asked: still an upgrade); every planned need leaves the list
    // (delivered, refused or outgrown: the next scan asks again).
    auto deliver = [&](Player* from, bool fromArtisan)
    {
        if (!from || needs.empty())
            return;
        std::vector<bool> planned(needs.size(), false);
        for (GearDelivery const& d : PlanGearDeliveries(needs, pieces(from)))
        {
            GearNeed const& n = needs[d.need];
            std::uint32_t const product = G.tiers[n.recipe].product;
            planned[d.need] = true;
            Player* m = Online(n.guid);
            if (!m || !m->IsInWorld() ||
                !Consumes(PlayerbotsMgr::instance().GetPlayerbotAI(m), m, sObjectMgr->GetItemTemplate(product)))
                continue;
            if (char const* const why = Send(Low(from), n.guid, {d.item}, "AutoWoW gear", "deliver"))
            {
                EmitLine(line, from, Reason::Refused, v.orderId, product, 1, 0, Low(from), n.guid, why, n.guid);
                continue;
            }
            EmitLine(line, from, Reason::Deliver, v.orderId, product, 1, 0, Low(from), n.guid, nullptr, n.guid);
            if (WeaponOrders() && line == Line::MailGear)
                FillWeaponOrder(t, from, n, product);
            if (fromArtisan)
                paid(n.recipe, 1);
            else
                GrantXp(from, XpFor(p.repXpPerDeal, from), v.orderId, product, 1, line);
        }
        std::vector<GearNeed> left;
        for (std::size_t k = 0; k < needs.size(); ++k)
            if (!planned[k])
                left.push_back(needs[k]);
        needs = std::move(left);
    };
    deliver(art, true);
    // The artisan's other pieces -> the rep's stock (consumer 0), one mail.
    if (art && rep && art != rep)
    {
        std::vector<GearItem> left = pieces(art);
        left.resize(std::min<std::size_t>(left.size(), kMaxMailStacks));
        std::vector<std::uint32_t> guids;
        for (GearItem const& g : left)
            guids.push_back(g.guid);
        char const* const why = guids.empty() ? nullptr : Send(Low(art), repGuid, guids, "AutoWoW gear", "deliver");
        for (std::uint8_t const r : gGearRank[li])
        {
            std::uint32_t const n = static_cast<std::uint32_t>(
                std::count_if(left.begin(), left.end(), [&](GearItem const& g) { return g.recipe == r; }));
            if (!n)
                continue;
            EmitLine(line, art, why ? Reason::Refused : Reason::Deliver, v.orderId, G.tiers[r].product, n, 0, Low(art),
                     repGuid, why, 0);
            if (!why)
                paid(r, n);
        }
    }
    deliver(rep, false);
    // Consumer::LoadAmmo (lane AA): the team's gun hunters load delivered house shot.
    if (L.consumer == Consumer::LoadAmmo)
        for (Player* m : GearRecipients(alliance))
            LoadAmmo(m, G);

    // The artisan's target: the first order entry still short that the house can cast now, else the first short one.
    std::vector<std::uint32_t> toMake(ts.orders.size(), 0), casts(ts.orders.size(), 0);
    for (std::size_t i = 0; i < ts.orders.size(); ++i)
    {
        std::uint32_t const finished = castUnits(ts.orders[i].recipe, artHeld(G.tiers[ts.orders[i].recipe].product));
        toMake[i] = ts.orders[i].units > finished ? ts.orders[i].units - finished : 0;
        casts[i] = Casts(G, ts.orders[i].recipe, house);
    }
    int const target = PickGearTarget(toMake, casts);
    v.product = target < 0 ? kNoTier : ts.orders[static_cast<std::size_t>(target)].recipe;
    v.remaining = target < 0 ? 0 : ts.orders[static_cast<std::size_t>(target)].units;
    v.productWant = v.remaining;
    std::uint32_t const make = target < 0 ? 0 : toMake[static_cast<std::size_t>(target)];

    // Rep -> artisan (one mail, at most kMaxMailStacks stacks): an intermediate the rep holds (routed Medium Leather, a
    // bolt) for the target's direct Craft reagents first, then what the target still lacks (Route / Market reagents).
    // Vendor reagents the artisan buys (v.vendor).
    v.vendor.clear();
    if (art && make)
    {
        std::vector<std::uint32_t> feedStacks;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> fed;  // (item, units)
        auto feed = [&](std::uint32_t item, std::uint32_t units)
        {
            if (!rep || art == rep)
                return;
            std::vector<std::uint32_t> stacks = PickStacks(LooseStacks(rep, item), units);
            stacks.resize(std::min<std::size_t>(stacks.size(), kMaxMailStacks - feedStacks.size()));
            std::uint32_t sent = 0;
            for (Stack const& st : LooseStacks(rep, item))
                if (std::find(stacks.begin(), stacks.end(), st.guid) != stacks.end())
                    sent += st.count;
            if (!sent)
                return;
            feedStacks.insert(feedStacks.end(), stacks.begin(), stacks.end());
            fed.push_back({item, sent});
        };
        for (Reagent const& r : G.tiers[v.product].reagents)
            if (r.item && r.source == Source::Craft)
                feed(r.item, Short(std::uint64_t(make) * r.count, artHeld(r.item)));
        std::vector<std::pair<std::uint32_t, std::uint32_t>> const intermediates = fed;
        auto have = [&](std::uint32_t item)
        {
            std::uint32_t n = artHeld(item);
            for (auto const& [it, u] : intermediates)
                if (it == item)
                    n += u;
            return n;
        };
        for (Lack const& k : Lacks(G, v.product, make, have))
        {
            if (k.source == Source::Vendor)
                v.vendor.push_back({k.item, k.units + artHeld(k.item), BuyOf(k.item)});
            else
                feed(k.item, k.units);
        }
        // CrossHouseFeed (lane smithsupply): stone the own rep could not cover, from the team's other house reps whose
        // own open orders do not use it (S110: the Tinkers reps hold the routed Coarse / Heavy Stone, the Smiths rep none).
        // ponytail: stone only (the stranded stock the data shows); widen to bars once a donor house stocks them.
        // SmithBars (lane smithbars): the smith's routed ore too (the Tinkers reps hold the House.Ore stock).
        bool const oreToo = SmithBars() && line == Line::MailGear;
        if (CrossHouseFeed())
            for (Lack const& k : Lacks(G, v.product, make, have))
            {
                bool const stone = std::find(std::begin(kStone), std::end(kStone), k.item) != std::end(kStone);
                bool const ore = oreToo && std::find(std::begin(kOre), std::end(kOre), k.item) != std::end(kOre);
                if (k.source == Source::Vendor || (!stone && !ore))
                    continue;
                std::uint32_t left = k.units;
                for (auto const& [item, sent] : fed)
                    if (item == k.item)
                        left -= std::min(left, sent);
                for (std::size_t h = 0; left && h < AutoWowGuilds::Houses().size(); ++h)
                {
                    Player* donor = h == gLineHouse[li] ? nullptr : RoleRep(h, alliance);
                    if (!donor || donor == art)
                        continue;
                    std::uint32_t const units =
                        CrossFeedUnits(left, Loose(donor, k.item), GearOrderUses(alliance, h, k.item));
                    if (!units)
                        continue;
                    std::vector<std::uint32_t> stacks = PickStacks(LooseStacks(donor, k.item), units);
                    stacks.resize(std::min<std::size_t>(stacks.size(), kMaxMailStacks));
                    std::uint32_t sent = 0;
                    for (Stack const& st : LooseStacks(donor, k.item))
                        if (std::find(stacks.begin(), stacks.end(), st.guid) != stacks.end())
                            sent += st.count;
                    if (!sent)
                        continue;
                    char const* const why = Send(Low(donor), Low(art), stacks, "AutoWoW materials", "feed");
                    EmitLine(line, donor, why ? Reason::Refused : Reason::Feed, v.orderId, k.item, sent, 0, Low(donor),
                             Low(art), why ? why : "cross_feed");
                    if (!why)
                        left -= std::min(left, sent);
                }
            }
        // A tool (kTools: Blacksmith Hammer) a known recipe's cast needs that the artisan lacks (bags or worn): one,
        // bought at the line vendor. GearTable already removes disabled Engineering gun rows, so an EngGuns-off line
        // does not acquire their hammer.
        for (std::size_t i = 0; i < G.tierCount; ++i)
            if (art->HasSpell(G.tiers[i].spell))
                for (std::uint32_t const c : ToolCategories(G.tiers[i].spell))
                    if (std::uint32_t const tool = ToolFor(c);
                        tool && !art->HasItemTotemCategory(c) &&
                        std::none_of(v.vendor.begin(), v.vendor.end(),
                                     [&](MarketWant const& w) { return w.item == tool; }))
                        v.vendor.push_back({tool, 1, BuyOf(tool)});
        if (!feedStacks.empty())
        {
            char const* const why = Send(repGuid, Low(art), feedStacks, "AutoWoW materials", "feed");
            for (auto const& [item, sent] : fed)
                EmitLine(line, rep, why ? Reason::Refused : Reason::Feed, v.orderId, item, sent, 0, repGuid, Low(art),
                         why);
        }
    }
    if (art && ts.artisanWant)
    {
        bool const ok = gid && AutoWowGuilds::Pay(gid, art, ts.artisanWant);
        EmitLine(line, art, ok ? Reason::Feed : Reason::Refused, v.orderId, 0, 0, ts.artisanWant, 0, Low(art),
                 ok ? nullptr : "feed_copper");
        ts.artisanWant = 0;
    }

    // Market-only reagents for the active gear target. Routed inputs keep their existing house assignment and vendor
    // inputs stay on the artisan trip; only Source::Market becomes a faction AH / MailOrders want (GearMarketRoute: the
    // Route ones the house lacks too, GearBuys).
    v.buy.clear();
    if ((p.market || p.mailOrders) && art && v.product != kNoTier && make)
        for (Lack const& k : Lacks(G, v.product, make, house))
            if (GearBuys(k.source, GearMarketRoute()))
                v.buy.push_back({k.item, k.units, SellOf(k.item)});

    std::lock_guard<std::mutex> guard(gLock);
    LineState& out = gLines[li][t];
    out.orders = ts.orders;
    out.post = ts.post;
    out.v.orderId = v.orderId;
    out.v.remaining = v.remaining;
    out.v.product = v.product;
    out.v.productWant = v.productWant;
    out.v.artisanSkill = v.artisanSkill;
    out.v.buy = v.buy;
    out.v.vendor = v.vendor;
    out.artisanWant = ts.artisanWant;  // no map update runs during the world tick
}

// DirectRoutes (TierTick): the artisan's finished bags go straight to the ranked members, best tier first (the rep's
// own RepStore share stays for the shipment to it), each paid from the treasury and XP'd as a delivery to the rep;
// the rep earns its deal XP (it brokers and pays). Product bags count down the order.
void DirectBags(bool alliance, Player* art, Player* rep, std::uint32_t repGuid, std::uint32_t gid, TeamState& ts)
{
    Params const& p = detail::gParams;
    for (std::size_t i = kTierCount; i-- > 0;)
    {
        Tier const& tier = kTiers[i];
        std::vector<Stack> bags = LooseStacks(art, tier.bag);
        if (bags.empty())
            continue;
        std::sort(bags.begin(), bags.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
        std::size_t next = 0;
        for (Delivery const& d :
             PlanDeliveries(RankNeeds(Members(alliance, tier.bagSlots)), static_cast<std::uint32_t>(bags.size())))
        {
            std::vector<std::uint32_t> give;
            for (; next < bags.size() && give.size() < d.bags; ++next)
                give.push_back(bags[next].guid);
            if (give.empty())
                break;
            if (d.guid == repGuid)
                continue;  // RepStore: the rep's own share rides with the artisan's shipment to it
            std::uint32_t const n = static_cast<std::uint32_t>(give.size());
            if (char const* const why = Send(Low(art), d.guid, give, "AutoWoW bag", "deliver"))
            {
                Emit(art, Reason::Refused, ts.orderId, tier.bag, n, 0, Low(art), d.guid, why);
                continue;
            }
            Emit(art, Reason::Deliver, ts.orderId, tier.bag, n, 0, Low(art), d.guid);
            if (i == ts.product)
                ts.remaining -= std::min(ts.remaining, n);
            std::uint64_t const pay = BagPay(SellOf(tier.bag), p.bagPayPct, n);
            bool const paid = pay && AutoWowGuilds::Pay(gid, art, pay);
            Emit(art, paid ? Reason::Pay : Reason::Refused, ts.orderId, tier.bag, n, pay, 0, Low(art),
                 paid ? nullptr : "pay");
            GrantXp(art, XpFor(p.workXpPerItem, art) * n, ts.orderId, tier.bag, n);
            if (rep)
                GrantXp(rep, XpFor(p.repXpPerDeal, rep), ts.orderId, tier.bag, n);
        }
    }
}

// MailOrders (world thread): the team's open orders, rebuilt every tick from the reps' market wants (what each house
// lacks now; units in its mailbox, COD fills included, already count as held), within each house bank less the COD
// its rep already owes on pending fills.
void OrderTick(bool alliance, bool overlord)
{
    Params const& p = detail::gParams;
    std::size_t const t = T(alliance);
    std::vector<std::pair<std::size_t, std::vector<MarketWant>>> wants;  // (house, wants)
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (LineOn(Line::Bags) && p.tiers)
            wants.push_back({gBagHouse, gTeams[t].buy});
        for (ProductLine const& L : kCatalog)
            if ((L.tierCount || L.gearCount) && LineOn(L.id))
                wants.push_back({gLineHouse[static_cast<std::size_t>(L.id)], gLines[static_cast<std::size_t>(L.id)][t].v.buy});
    }
    std::vector<MailOrder> orders;
    for (auto const& [house, w] : wants)
    {
        std::uint32_t const gid = AutoWowGuilds::HouseGuildId(house, alliance);
        std::uint32_t const rep = gid ? AutoWowGuilds::RepOf(gid) : 0;
        if (!rep || w.empty())
            continue;
        std::uint64_t owed = 0;
        for (auto const& [key, c] : gCodPending)
            if (std::get<0>(key) == rep)
                owed += c.cod;
        std::uint64_t const bank = AutoWowGuilds::Balance(gid);
        for (MailOrder const& o : PlanOrders(rep, w, p.buyMaxPct, bank > owed ? bank - owed : 0))
        {
            orders.push_back(o);
            if (overlord)
                LOG_INFO("playerbots", "[Supply] order team={} rep={} item={} units={} unit_price={} bank={} owed={}",
                         alliance ? "alliance" : "horde", rep, o.item, o.units, o.unitPrice, bank, owed);
        }
    }
    std::lock_guard<std::mutex> guard(gLock);
    gOrders[t] = std::move(orders);
}

// World thread (MailOrders): a random seller's order fill, mailed to the rep as cash on delivery at the order price
// (its own postage); the fill is recorded pending for the rep's acceptance. A `trade` cod_sell row.
class CodSellOperation : public PlayerbotOperation
{
public:
    CodSellOperation(ObjectGuid bot, std::uint32_t rep, std::uint32_t item, std::vector<std::uint32_t> guids,
                     std::uint32_t units, std::uint64_t cod)
        : bot_(bot), rep_(rep), item_(item), guids_(std::move(guids)), units_(units), cod_(cod)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !cod_ || cod_ > AutoWowTrade::kMaxMoney)
            return false;
        auto const key = std::make_tuple(rep_, Low(bot), item_);
        if (!gCodPending.count(key) && gCodPending.size() >= kMaxCodPending)
        {
            LOG_INFO("playerbots", "[Supply] cod_sell bot={} item={} refused: {} fills pending", bot->GetName(), item_,
                     gCodPending.size());
            return false;
        }
        std::uint64_t const m0 = bot->GetMoney();
        char const* why = nullptr;
        if (!AutoWowGuilds::SendItemsCod(Low(bot), rep_, guids_, static_cast<std::uint32_t>(cod_), "AutoWoW order", &why))
        {
            LOG_INFO("playerbots", "[Supply] cod_sell bot={} rep={} item={} count={} refused: {}", bot->GetName(), rep_,
                     item_, units_, why ? why : "send");
            return false;
        }
        CodPending& c = gCodPending[key];
        c.units += units_;
        c.cod += cod_;
        AutoWowTrade::EmitRow(bot, AutoWowTrade::Action::CodSell, item_, units_, cod_,
                              std::int64_t(bot->GetMoney()) - std::int64_t(m0));
        return true;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyCodSell"; }

private:
    ObjectGuid bot_;
    std::uint32_t rep_;
    std::uint32_t item_;
    std::vector<std::uint32_t> guids_;
    std::uint32_t units_;
    std::uint64_t cod_;
};

// World thread: the rep's market buyouts, funded by its house bank (bank -> rep -> AH seller); the copper a
// rejected bid did not spend goes back to the bank. The auction house is the faction house the rep stands at.
class MarketBuyOperation : public PlayerbotOperation
{
public:
    MarketBuyOperation(ObjectGuid bot, ObjectGuid auctioneer, std::vector<MarketListing> buys)
        : bot_(bot), auctioneer_(auctioneer), buys_(std::move(buys))
    {
    }

    MarketBuyOperation(ObjectGuid bot, ObjectGuid auctioneer, std::vector<FinishedBagBuy> buys)
        : bot_(bot), auctioneer_(auctioneer), bagBuys_(std::move(buys))
    {
    }

    bool Execute() override { return bagBuys_.empty() ? ExecuteMaterials() : ExecuteFinishedBags(); }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowSupplyMarketBuy"; }

private:
    bool ExecuteMaterials()
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

    bool ExecuteFinishedBags()
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        auto clearAll = [&]()
        {
            for (FinishedBagBuy const& buy : bagBuys_)
                ClearFinishedBagPending(buy.listing.auctionId);
        };
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
        {
            clearAll();
            return false;
        }
        Creature* npc = bot->GetNPCIfCanInteractWith(auctioneer_, UNIT_NPC_FLAG_AUCTIONEER);
        AuctionHouseEntry const* house =
            npc ? AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(npc->GetFaction()) : nullptr;
        AuctionHouseObject* ah = npc ? sAuctionMgr->GetAuctionsMap(npc->GetFaction()) : nullptr;
        std::uint32_t const expectedHouse =
            uint32(bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
        if (!house || !ah || house->houseId != expectedHouse)
        {
            clearAll();
            return false;
        }

        std::unordered_set<std::uint32_t> excluded;
        for (FinishedBagBuy const& buy : bagBuys_)
            excluded.insert(buy.listing.auctionId);
        FinishedBagView live = SnapshotFinishedBags(bot->GetTeamId() == TEAM_ALLIANCE, bot, excluded);
        std::vector<FinishedBagCoverage> planned;
        std::vector<FinishedBagBuy> accepted;
        std::uint64_t budget = detail::gParams.bagBuyBudget;
        std::sort(bagBuys_.begin(), bagBuys_.end(),
                  [](FinishedBagBuy const& a, FinishedBagBuy const& b)
                  {
                      if (a.listing.slots != b.listing.slots)
                          return a.listing.slots > b.listing.slots;
                      if (a.listing.buyout != b.listing.buyout)
                          return a.listing.buyout < b.listing.buyout;
                      return a.listing.auctionId < b.listing.auctionId;
                  });
        for (FinishedBagBuy queued : bagBuys_)
        {
            FinishedBagListing const& listing = queued.listing;
            AuctionEntry const* auction = ah->GetAuction(listing.auctionId);
            Item* auctionItem = auction ? sAuctionMgr->GetAItem(auction->item_guid) : nullptr;
            ItemTemplate const* proto = auctionItem ? auctionItem->GetTemplate() : nullptr;
            bool const exact = auction && auctionItem && auction->item_template == listing.item &&
                               auction->item_guid.GetRawValue() == listing.itemGuid &&
                               auction->itemCount == listing.count && auction->buyout == listing.buyout &&
                               auction->owner.GetRawValue() == listing.ownerGuid && auction->owner != bot->GetGUID() &&
                               GeneralBag(proto) && proto->ContainerSlots == listing.slots &&
                               proto->SellPrice == listing.sellPrice;
            std::vector<std::uint32_t> usableRecipients;
            if (exact)
                for (FinishedBagNeed const& need : live.needs)
                    if (Player* recipient = Online(need.recipient);
                        recipient && recipient->IsInWorld() &&
                        (recipient->GetTeamId() == TEAM_ALLIANCE) == (bot->GetTeamId() == TEAM_ALLIANCE) &&
                        !IsRole(recipient) && PriorityOf(gPriority, need.recipient) != kNoPriority &&
                        recipient->CanUseItem(proto) == EQUIP_ERR_OK &&
                        std::find(usableRecipients.begin(), usableRecipients.end(), need.recipient) ==
                            usableRecipients.end())
                        usableRecipients.push_back(need.recipient);
            std::vector<FinishedBagNeed> needs =
                exact ? UncoveredFinishedBagNeeds(live, listing.slots, planned, &usableRecipients)
                      : std::vector<FinishedBagNeed>{};
            bool const affordable = exact && listing.buyout <= budget &&
                                    std::uint64_t(listing.buyout) * 100 <=
                                        std::uint64_t(listing.sellPrice) * detail::gParams.buyMaxPct * listing.count;
            if (!affordable || listing.count > needs.size())
            {
                FinishedBagReceipt(bot, listing, 0, exact ? "demand_or_budget" : "stale");
                ClearFinishedBagPending(listing.auctionId);
                continue;
            }
            queued.recipients.clear();
            for (std::uint32_t i = 0; i < listing.count; ++i)
            {
                queued.recipients.push_back(needs[i].recipient);
                planned.push_back({needs[i].recipient, listing.slots, 1, FinishedBagCoverageSource::InFlight});
            }
            budget -= listing.buyout;
            UpdateFinishedBagRecipients(listing.auctionId, queued.recipients);
            accepted.push_back(std::move(queued));
        }
        if (accepted.empty())
            return false;

        std::uint64_t total = 0;
        for (FinishedBagBuy const& buy : accepted)
            total += buy.listing.buyout;
        std::uint32_t const guild = AutoWowGuilds::HouseGuildOf(bot);
        if (!guild || !AutoWowGuilds::Pay(guild, bot, total))
        {
            for (FinishedBagBuy const& buy : accepted)
            {
                FinishedBagReceipt(bot, buy.listing, buy.recipients.front(), "treasury");
                ClearFinishedBagPending(buy.listing.auctionId);
            }
            return false;
        }

        std::uint64_t spent = 0;
        for (FinishedBagBuy const& buy : accepted)
        {
            FinishedBagListing const& listing = buy.listing;
            AuctionEntry const* auction = ah->GetAuction(listing.auctionId);  // reacquire immediately before CMSG
            Item* auctionItem = auction ? sAuctionMgr->GetAItem(auction->item_guid) : nullptr;
            ItemTemplate const* proto = auctionItem ? auctionItem->GetTemplate() : nullptr;
            bool exact = auction && auctionItem && auction->item_template == listing.item &&
                         auction->item_guid.GetRawValue() == listing.itemGuid && auction->itemCount == listing.count &&
                         auction->buyout == listing.buyout && auction->owner.GetRawValue() == listing.ownerGuid &&
                         auction->owner != bot->GetGUID() && GeneralBag(proto) &&
                         proto->ContainerSlots == listing.slots && proto->SellPrice == listing.sellPrice;
            FinishedBagView immediate =
                SnapshotFinishedBags(bot->GetTeamId() == TEAM_ALLIANCE, bot, {listing.auctionId});
            std::vector<std::uint32_t> usableRecipients;
            if (exact)
                for (std::uint32_t const recipientGuid : buy.recipients)
                    if (Player* recipient = Online(recipientGuid);
                        recipient && recipient->IsInWorld() && !IsRole(recipient) &&
                        (recipient->GetTeamId() == TEAM_ALLIANCE) == (bot->GetTeamId() == TEAM_ALLIANCE) &&
                        PriorityOf(gPriority, recipientGuid) != kNoPriority &&
                        recipient->CanUseItem(proto) == EQUIP_ERR_OK)
                        usableRecipients.push_back(recipientGuid);
            std::vector<FinishedBagNeed> immediateNeeds =
                exact ? UncoveredFinishedBagNeeds(immediate, listing.slots, {}, &usableRecipients)
                      : std::vector<FinishedBagNeed>{};
            for (std::uint32_t const recipientGuid : buy.recipients)
            {
                auto const need = std::find_if(immediateNeeds.begin(), immediateNeeds.end(),
                                               [&](FinishedBagNeed const& n) { return n.recipient == recipientGuid; });
                if (need == immediateNeeds.end())
                {
                    exact = false;
                    break;
                }
                immediateNeeds.erase(need);  // one whole-lot unit consumes one distinct current slot
            }
            if (!exact || buy.recipients.size() != listing.count)
            {
                FinishedBagReceipt(bot, listing, buy.recipients.front(), "stale_or_demand_before_bid");
                ClearFinishedBagPending(listing.auctionId);
                continue;
            }
            std::uint64_t const moneyBefore = bot->GetMoney();
            WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
            packet << auctioneer_ << uint32(listing.auctionId) << uint32(listing.buyout);
            bot->GetSession()->HandleAuctionPlaceBid(packet);
            if (bot->GetMoney() >= moneyBefore)
            {
                FinishedBagReceipt(bot, listing, buy.recipients.front(), "native_refused");
                ClearFinishedBagPending(listing.auctionId);
                continue;
            }
            std::uint64_t const debit = moneyBefore - bot->GetMoney();
            spent += debit;
            MarkFinishedBagPurchased(listing.auctionId);
            FinishedBagReceipt(bot, listing, buy.recipients.front(), "purchased");
            Emit(bot, Reason::Buy, 0, listing.item, listing.count, debit, Low(bot), buy.recipients.front(),
                 "bag_market", buy.recipients.front());
        }
        std::uint64_t const back = std::min<std::uint64_t>(total > spent ? total - spent : 0, bot->GetMoney());
        if (back)
            AutoWowGuilds::Deposit(bot, back);
        return spent != 0;
    }

    ObjectGuid bot_;
    ObjectGuid auctioneer_;
    std::vector<MarketListing> buys_;
    std::vector<FinishedBagBuy> bagBuys_;
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

// The stations near `home` (within `yards`): the trainer teaching `trainerSpell`, the vendor selling `vendorItem` (no
// extended cost; `vendorAll`: every item listed), the auctioneer, the banker, the mailbox, a forge and, when requested,
// an anvil, each the nearest (ties the lower spawn id).
void FindStations(Stations& st, bool alliance, Home const& home, std::uint32_t trainerSpell, std::uint32_t vendorItem,
                  std::vector<std::uint32_t> const* vendorAll = nullptr, std::uint32_t yards = kStationYards,
                  bool findAnvil = false, std::vector<std::uint32_t> const* trainerAll = nullptr)
{
    st = Stations{};
    std::array<std::int64_t, 7> best{};
    std::array<std::uint64_t, 7> bestSpawn{};
    std::int64_t const r2 = std::int64_t(yards) * yards;
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
            {
                auto teaches = [&](std::uint32_t id)
                {
                    return std::any_of(tr->GetSpells().begin(), tr->GetSpells().end(),
                                       [&](Trainer::Spell const& s) { return s.SpellId == id; });
                };
                bool const serves = trainerAll && !trainerAll->empty()
                    ? std::all_of(trainerAll->begin(), trainerAll->end(), teaches)
                    : teaches(trainerSpell);
                if (serves)
                    consider(0, st.trainer, spawn, data.id, data.posX, data.posY, data.posZ);
            }
        if ((npcflag & UNIT_NPC_FLAG_VENDOR) && vendorAll)
        {
            VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id);
            bool all = list && !vendorAll->empty();
            for (std::size_t k = 0; all && k < vendorAll->size(); ++k)
                all = std::any_of(list->m_items.begin(), list->m_items.end(), [&](VendorItem const* vi)
                                  { return vi && vi->item == (*vendorAll)[k] && !vi->ExtendedCost; });
            if (all)
                consider(1, st.threadVendor, spawn, data.id, data.posX, data.posY, data.posZ);
        }
        else if ((npcflag & UNIT_NPC_FLAG_VENDOR))
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(data.id))
                for (VendorItem const* vi : list->m_items)
                    if (vi && vi->item == vendorItem && !vi->ExtendedCost)
                    {
                        consider(1, st.threadVendor, spawn, data.id, data.posX, data.posY, data.posZ);
                        break;
                    }
        if (npcflag & UNIT_NPC_FLAG_AUCTIONEER)
            consider(2, st.auctioneer, spawn, data.id, data.posX, data.posY, data.posZ);
        if (npcflag & UNIT_NPC_FLAG_BANKER)
            consider(4, st.banker, spawn, data.id, data.posX, data.posY, data.posZ);
    }
    for (auto const& [spawn, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* gt = data.mapid == home.map ? sObjectMgr->GetGameObjectTemplate(data.id) : nullptr;
        if (gt && gt->type == GAMEOBJECT_TYPE_MAILBOX)
            consider(3, st.mailbox, spawn, data.id, data.posX, data.posY, data.posZ);
        if (gt && gt->type == GAMEOBJECT_TYPE_SPELL_FOCUS && gt->spellFocus.focusId == kForgeFocus)
            consider(5, st.forge, spawn, data.id, data.posX, data.posY, data.posZ);
        if (findAnvil && gt && gt->type == GAMEOBJECT_TYPE_SPELL_FOCUS && gt->spellFocus.focusId == kAnvilFocus)
            consider(6, st.anvil, spawn, data.id, data.posX, data.posY, data.posZ);
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
             "auctioneer={} bag_vendor={} (item {}) banker={}", alliance ? "alliance" : "horde", home.map, home.x,
             home.y, st.mailbox.entry, st.trainer.entry, st.threadVendor.entry, st.auctioneer.entry,
             gBagVendor[T(alliance)].entry, kPouch, st.banker.entry);
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
    std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> rooms;
    {
        std::lock_guard<std::mutex> guard(gLock);
        for (auto const& [guid, need] : gGrantRequests)
            reqs.push_back({guid, 0, need});
        gGrantRequests.clear();
        rooms.swap(gGrantRooms);
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
        // AutoWow.Errands.Mounts: a mount request widens the bot cap by its room and uses its own team budget.
        auto const room = rooms.find(r.guid);
        bool const mount = room != rooms.end();
        std::size_t const t = T(bot->GetTeamId() == TEAM_ALLIANCE);
        GrantBudget& budget = mount ? gMountGrantBudgets[t] : gGrantBudgets[t];
        GrantWindow& window = gGrantWindows[r.guid];
        // AutoWow.Supply.OutfitGear: the floors' room on top of both caps (off: the Outfit caps as they were).
        bool const gear = OutfitGear();
        GrantDecision const d =
            DecideGrant(r, bot->GetMoney(), window, budget, hour,
                        GrantCapCopper(p.outfitMaxCopper, gear, p.outfitGearCopper, r.level) +
                            (mount ? room->second.first : 0),
                        mount ? room->second.second
                              : p.outfitBudgetPerHour + (gear ? std::uint64_t(p.outfitGearBudgetPerHour) : 0));
        if (d.verdict == GrantVerdict::Covered)
            continue;
        if (d.verdict != GrantVerdict::Pay)
        {
            EmitOutfit(bot, Reason::Refused, 0, d.copper, GrantVerdictName(d.verdict));
            continue;
        }
        if (AutoWowGuilds::Pay(gid, bot, d.copper, AutoWowGuilds::Reason::Grant))
        {
            NoteGrant(window, budget, r.level, hour, d.copper);
            if (gear || mount)
                EmitOutfit(bot, Reason::Grant, 0, d.copper, mount ? "mount" : nullptr);
        }
    }
}

std::uint32_t Output(SpellInfo const* s)
{
    for (std::size_t i = 0; s && i < MAX_SPELL_EFFECTS; ++i)
        if (s->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
            return s->Effects[i].ItemType;
    return 0;
}

// Items one cast of `s` creates (BasePoints + DieSides: 3.3.5 create-item spells roll a single die side), at least 1.
std::uint32_t Yield(SpellInfo const* s)
{
    for (std::size_t i = 0; s && i < MAX_SPELL_EFFECTS; ++i)
        if (s->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
            return static_cast<std::uint32_t>(
                std::max<std::int32_t>(1, s->Effects[i].BasePoints + s->Effects[i].DieSides));
    return 1;
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
    p.bagMarket = sConfigMgr->GetOption<bool>("AutoWow.Supply.BagMarket", false);
    p.bagBuyBudget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.BagBuyBudget", 0);
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
    p.outfitGear = sConfigMgr->GetOption<bool>("AutoWow.Supply.OutfitGear", false);
    p.outfitGearCopper = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OutfitGearCopper", 25);
    p.outfitGearBudgetPerHour = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OutfitGearBudgetPerHour", 50000);
    p.artisanFreeSlots = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ArtisanFreeSlots", 4);
    p.artisanMinLevel = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.ArtisanMinLevel", 10);
    p.repStore = sConfigMgr->GetOption<bool>("AutoWow.Supply.RepStore", true);
    p.repMailCap = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.RepMailCap", 80));
    p.repKeep = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.RepKeep", 60);
    p.directRoutes = sConfigMgr->GetOption<bool>("AutoWow.Supply.DirectRoutes", false);
    p.mailPickup = sConfigMgr->GetOption<bool>("AutoWow.Supply.MailPickup", false);
    p.mailPickupYards = std::max<std::uint32_t>(5, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.MailPickupYards", 40));
    p.mailRunMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.MailRunMs", 1800000);
    p.mailOrders = sConfigMgr->GetOption<bool>("AutoWow.Market.MailOrders", false);
    p.demandOnly = sConfigMgr->GetOption<bool>("AutoWow.Supply.DemandOnly", false);
    p.repStockPerItem = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.RepStockPerItem", 2);
    p.gearMaxOrder = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.GearMaxOrder", 4));
    p.gearPayPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.GearPayPct", 200);
    p.ammoTarget = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.AmmoTarget", 1000);
    p.gearBootstrap = sConfigMgr->GetOption<bool>("AutoWow.Supply.GearBootstrap", false);
    p.engGuns = sConfigMgr->GetOption<bool>("AutoWow.Supply.EngGuns", false);
    p.routeBagExtra = sConfigMgr->GetOption<bool>("AutoWow.Supply.RouteBagExtra", false);
    p.gearStockSell = sConfigMgr->GetOption<bool>("AutoWow.Supply.GearStockSell", false);
    p.gearSkillupRestock = sConfigMgr->GetOption<bool>("AutoWow.Supply.GearSkillupRestock", false);
    p.potionTiers = sConfigMgr->GetOption<bool>("AutoWow.Supply.PotionTiers", false);
    p.smithEndgame = sConfigMgr->GetOption<bool>("AutoWow.Supply.SmithEndgame", false);
    p.weaponOrders = sConfigMgr->GetOption<bool>("AutoWow.Supply.WeaponOrders", false);
    p.climbSkillup = sConfigMgr->GetOption<bool>("AutoWow.Supply.ClimbSkillup", false);
    p.weaponOrderTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.WeaponOrderTimeoutMs", 7200000);
    gWeaponOrderSeen.clear();
    gWeaponOrderOf = {};
    p.orderBackoffMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.OrderBackoffMs", 0);
    p.mineMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.MineMs", 0);
    p.mineCooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Supply.MineCooldownMs", 1800000);
    p.mineLootYield = sConfigMgr->GetOption<bool>("AutoWow.Supply.MineLootYield", false);
    p.crossHouseFeed = sConfigMgr->GetOption<bool>("AutoWow.Supply.CrossHouseFeed", false);
    p.smithBars = sConfigMgr->GetOption<bool>("AutoWow.Supply.SmithBars", false);
    p.climbPastStock = sConfigMgr->GetOption<bool>("AutoWow.Supply.ClimbPastStock", false);
    p.gearMarketRoute = sConfigMgr->GetOption<bool>("AutoWow.Supply.GearMarketRoute", false);
    p.potionLowBridge = sConfigMgr->GetOption<bool>("AutoWow.Supply.PotionLowBridge", false);
    p.climbCastable = sConfigMgr->GetOption<bool>("AutoWow.Supply.ClimbCastable", false);
    p.smithCopper = sConfigMgr->GetOption<bool>("AutoWow.Supply.SmithCopper", false);
    p.craftTrace = sConfigMgr->GetOption<bool>("AutoWow.Supply.CraftTrace", false);
    p.craftDismount = sConfigMgr->GetOption<bool>("AutoWow.Supply.CraftDismount", false);
    p.artisanBagHygiene = sConfigMgr->GetOption<bool>("AutoWow.Professions.ArtisanBagHygiene", false);
    p.houseBoE = sConfigMgr->GetOption<bool>("AutoWow.Professions.HouseBoE", false);
    p.vendorJunk = sConfigMgr->GetOption<bool>("AutoWow.Professions.VendorJunk", false);
    p.surplusToAuction = sConfigMgr->GetOption<bool>("AutoWow.Professions.SurplusToAuction", false);
    gLineSpec = {};
    gPriority.clear();
    {
        std::lock_guard<std::mutex> guard(gLock);
        gFinishedBagViews = {};
        gFinishedBagPending.clear();
    }
    std::string const priority = sConfigMgr->GetOption<std::string>("AutoWow.Supply.PriorityGuids", "");
    if (!ParseGuids(priority, gPriority))
        LOG_ERROR("server.loading", "[Supply] bad AutoWow.Supply.PriorityGuids '{}': no priority list", priority);
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
        gGearRank[i].clear();
        gGearYield[i].clear();
        gGearNeeds[i] = {};
    }
    {
        std::lock_guard<std::mutex> guard(gLock);
        gTeams = {};
        gLines = {};
        gHeld.clear();
        gRawRooms = {};
        gGrantRequests.clear();
        gGrantRooms.clear();
        gOrders = {};
    }
    gCodPending.clear();
    gGrantWindows.clear();
    gGrantBudgets = {};
    gMountGrantBudgets = {};
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
        ProductLine const L = ActiveLine(kCatalog[li].id);
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
    // Gear lines (lane V): house, table check (outputs, reagent counts, RequiredLevel), learn list (the ranks and every
    // trainer-taught recipe, less what the bag chain already teaches when it is the bag house) and the recipes best first.
    for (std::size_t li = 0; li < kLineCount; ++li)
    {
        ProductLine const& L = kCatalog[li];
        if ((L.need != NeedRule::GearSlots && L.need != NeedRule::AmmoStock) || !LineOn(L.id))
            continue;
        auto off = [&](std::string const& why)
        {
            LOG_ERROR("server.loading", "[Supply] line {} off: {}", L.name, why);
            p.lines &= static_cast<std::uint8_t>(~(1u << li));
        };
        if (!L.gearCount)
        {
            off("no recipe table yet");
            continue;
        }
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
        RecipeTable const G = GearTable(L);
        std::vector<std::uint32_t> ilvl(G.tierCount, 0);
        bool ok = true;
        for (std::size_t i = 0; i < G.tierCount; ++i)
        {
            LineTier const& tier = G.tiers[i];
            SpellInfo const* sp = sSpellMgr->GetSpellInfo(tier.spell);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tier.product);
            bool t = sp && Output(sp) == tier.product && proto && proto->RequiredLevel == tier.reqLevel;
            for (Reagent const& r : tier.reagents)
                t = t && (!r.item || (ReagentCount(sp, r.item) == r.count && sObjectMgr->GetItemTemplate(r.item)));
            // Every required tool must have a lawful vendor item. GearTable excludes disabled Engineering gun rows.
            for (std::uint32_t const c : ToolCategories(tier.spell))
                t = t && ToolFor(c) && sObjectMgr->GetItemTemplate(ToolFor(c));
            if (!t)
                LOG_ERROR("server.loading", "[Supply] line {} recipe {} (spell {}, product {}) does not match the "
                          "loaded spells / items", L.name, i, tier.spell, tier.product);
            ok = ok && t;
            ilvl[i] = proto && proto->InventoryType != INVTYPE_NON_EQUIP ? proto->ItemLevel : 0;
            gGearYield[li].push_back(Yield(sp));
        }
        if (!ok)
        {
            off("recipe table mismatch");
            continue;
        }
        std::string const learn =
            sConfigMgr->GetOption<std::string>(std::string("AutoWow.Supply.Artisan.Learn.") + L.key, L.learn);
        AutoWowGuilds::detail::Split(learn, ',', [&](std::string_view sv) {
            std::uint32_t id = 0;
            if (AutoWowGuilds::detail::ParseU32(sv, id) && id)
                gLineLearn[li].push_back(id);
        });
        // SmithEndgame: the Master / Grand Master ranks (the rank trainer trip teaches them).
        if (SmithEndgame() && L.id == Line::MailGear)
            for (std::uint32_t const id : kSmithRanks)
                if (std::find(gLineLearn[li].begin(), gLineLearn[li].end(), id) == gLineLearn[li].end())
                    gLineLearn[li].push_back(id);
        for (std::size_t i = 0; i < G.tierCount; ++i)
            if (G.tiers[i].skill > 1 &&
                std::find(gLineLearn[li].begin(), gLineLearn[li].end(), G.tiers[i].spell) == gLineLearn[li].end())
                gLineLearn[li].push_back(G.tiers[i].spell);
        if (LineOn(Line::Bags) && gLineHouse[li] == gBagHouse)
            gLineLearn[li].erase(std::remove_if(gLineLearn[li].begin(), gLineLearn[li].end(), [](std::uint32_t id)
                                                { return std::find(gLearn.begin(), gLearn.end(), id) != gLearn.end(); }),
                                 gLineLearn[li].end());
        gGearRank[li] = RankGearRecipes(G, ilvl);
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
            ProductLine const L = ActiveLine(kCatalog[li].id);
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
        for (std::size_t li = 0; li < kLineCount && home.set; ++li)
        {
            // Gear line: the trainer teaching its first trainer-taught recipe, the vendor selling every Vendor reagent.
            ProductLine const& L = kCatalog[li];
            if (!L.gearCount || !LineOn(L.id))
                continue;
            RecipeTable const G = GearTable(L);
            std::uint32_t trainerSpell = 0;
            std::vector<std::uint32_t> vendorAll;
            bool findAnvil = false;
            for (std::size_t i = 0; i < G.tierCount; ++i)
            {
                if (!trainerSpell && G.tiers[i].skill > 1)
                    trainerSpell = G.tiers[i].spell;
                if (SpellInfo const* sp = sSpellMgr->GetSpellInfo(G.tiers[i].spell);
                    sp && sp->RequiresSpellFocus == kAnvilFocus)
                    findAnvil = true;
                for (Reagent const& r : G.tiers[i].reagents)
                    if (r.item && r.source == Source::Vendor &&
                        std::find(vendorAll.begin(), vendorAll.end(), r.item) == vendorAll.end())
                        vendorAll.push_back(r.item);
                for (std::uint32_t const c : ToolCategories(G.tiers[i].spell))
                    if (std::uint32_t const tool = ToolFor(c);
                        tool && std::find(vendorAll.begin(), vendorAll.end(), tool) == vendorAll.end())
                        vendorAll.push_back(tool);
            }
            Stations& st = gLineStations[li][T(alliance)];
            // AutoWow.Supply.StationYards.<Key> (lane AA): the Engineering trainers stand 466 / 491 yards from the homes.
            // GearBootstrap: kBootstrapYards when unset (soak S53-S59 ran without the key: no Engineering trainer).
            std::uint32_t const yards = sConfigMgr->GetOption<std::uint32_t>(
                std::string("AutoWow.Supply.StationYards.") + L.key, GearBootstrap() ? kBootstrapYards : kStationYards,
                false);
            FindStations(st, alliance, home, trainerSpell, 0, &vendorAll, yards, findAnvil);
            LOG_INFO("server.loading", "[Supply] line {} {} stations: mailbox={} trainer={} vendor={} ({} items)",
                     L.name, alliance ? "alliance" : "horde", st.mailbox.entry, st.trainer.entry, st.threadVendor.entry,
                     vendorAll.size());
            // AutoWow.Supply.Spec.<House>.<Team> (lane smithfocus): the team's specialization rows (default none).
            // HouseBoE (Part 2): a specialization row is a Bind-on-Pickup craft, so order planning only admits the
            // team's spec rows (SpecAllows) under the flag -- off, the spec stays kSpecAny and the OFF path is
            // byte-identical (every shipped row is kSpecAny today, so this changes nothing observable until verified
            // BoP spec rows are authored into the gear tables).
            std::string const specKey = "AutoWow.Supply.Spec." + gLineHouseName[li] + "." + team;
            std::string const specText = sConfigMgr->GetOption<std::string>(specKey, "none", false);
            std::uint8_t parsedSpec = kSpecAny;
            if (!ParseSpec(specText, parsedSpec))
                LOG_ERROR("server.loading", "[Supply] bad {} '{}': none", specKey, specText);
            gLineSpec[li][T(alliance)] = HouseBoE() ? parsedSpec : kSpecAny;
            // AutoWow.Supply.MineSpot.<House>.<Team> (MineMs): where the artisan mines, on its home map.
            if (detail::gParams.mineMs)
            {
                std::string const mineKey = "AutoWow.Supply.MineSpot." + gLineHouseName[li] + "." + team;
                std::string const mineText = sConfigMgr->GetOption<std::string>(mineKey, "", false);
                if (!mineText.empty() && (!ParseHome(mineText, st.mine) || st.mine.map != home.map))
                {
                    LOG_ERROR("server.loading", "[Supply] bad {} '{}' (map,x,y,z on the home map): no mining", mineKey,
                              mineText);
                    st.mine = Home{};
                }
                else if (st.mine.set)
                    LOG_INFO("server.loading", "[Supply] line {} {} mine spot map={} ({},{}) stint_ms={} cooldown_ms={}",
                             L.name, alliance ? "alliance" : "horde", st.mine.map, st.mine.x, st.mine.y,
                             detail::gParams.mineMs, detail::gParams.mineCooldownMs);
            }
            if (L.id != Line::Engineering && L.id != Line::MailGear)
                continue;
            // A second trainer serves every configured learn spell absent from the primary (Engineering: Mining;
            // Smiths: Phantom Blade). One secondary trainer is the bounded model; otherwise the line turns off.
            Trainer::Trainer* tr = st.trainer.entry ? sObjectMgr->GetTrainer(st.trainer.entry) : nullptr;
            std::vector<std::uint32_t> missing;
            for (std::uint32_t const id : gLineLearn[li])
                if (!tr || std::none_of(tr->GetSpells().begin(), tr->GetSpells().end(),
                                        [&](Trainer::Spell const& sp) { return sp.SpellId == id; }))
                    missing.push_back(id);
            // SmithEndgame: the rank trainer (AutoWow.Supply.RankTrainer.<House>.<Team>; defaults are the Grand Master
            // trainers checked in the world DB: trainer 59 teaches every rank and every table recipe, faction templates
            // 1892 Valiance Expedition / 1981 Warsong Offensive) serves the spells no home trainer does.
            if (SmithEndgame() && L.id == Line::MailGear)
            {
                std::uint32_t const entry = sConfigMgr->GetOption<std::uint32_t>(
                    "AutoWow.Supply.RankTrainer." + gLineHouseName[li] + "." + team, alliance ? 26988 : 26981, false);
                std::uint64_t bestSpawn = 0;
                for (auto const& [spawn, data] : sObjectMgr->GetAllCreatureData())
                    if (data.id == entry && (!st.rankTrainer.entry || spawn < bestSpawn))
                    {
                        bestSpawn = spawn;
                        st.rankTrainer = {entry, data.mapid, static_cast<std::int32_t>(data.posX),
                                          static_cast<std::int32_t>(data.posY), static_cast<std::int32_t>(data.posZ)};
                    }
                Trainer::Trainer* rt = st.rankTrainer.entry ? sObjectMgr->GetTrainer(st.rankTrainer.entry) : nullptr;
                missing.erase(std::remove_if(missing.begin(), missing.end(), [&](std::uint32_t id)
                {
                    return rt && std::any_of(rt->GetSpells().begin(), rt->GetSpells().end(),
                                             [&](Trainer::Spell const& sp) { return sp.SpellId == id; });
                }), missing.end());
                LOG_INFO("server.loading", "[Supply] line {} {} rank trainer={} map={} ({},{}) teaches={}", L.name,
                         alliance ? "alliance" : "horde", st.rankTrainer.entry, st.rankTrainer.map, st.rankTrainer.x,
                         st.rankTrainer.y, rt ? "yes" : "no trainer");
            }
            if (!missing.empty())
            {
                Stations other;
                FindStations(other, alliance, home, 0, 0, nullptr, yards, false, &missing);
                st.trainer2 = other.trainer;
            }
            Trainer::Trainer* tr2 = st.trainer2.entry ? sObjectMgr->GetTrainer(st.trainer2.entry) : nullptr;
            bool const served = std::all_of(missing.begin(), missing.end(), [&](std::uint32_t id)
            {
                return tr2 && std::any_of(tr2->GetSpells().begin(), tr2->GetSpells().end(),
                                          [&](Trainer::Spell const& sp) { return sp.SpellId == id; });
            });
            if (!served)
            {
                LOG_ERROR("server.loading", "[Supply] line {} off: {} learn spells are not served by trainer {} or {}",
                          L.name, missing.size(), st.trainer.entry, st.trainer2.entry);
                p.lines &= static_cast<std::uint8_t>(~(1u << li));
                continue;
            }
            LOG_INFO("server.loading",
                     "[Supply] line {} {} stations: trainer2={} (missing={}) forge={} anvil={} yards={}", L.name,
                     alliance ? "alliance" : "horde", st.trainer2.entry, missing.size(), st.forge.entry,
                     st.anvil.entry, yards);
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
            for (std::size_t li = 0; li < kLineCount; ++li)
                if (kCatalog[li].gearCount && LineOn(kCatalog[li].id) && gLineHouse[li] == i)
                    info.gear = static_cast<std::uint8_t>(li);
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
                if (info.gear != kNoLine)
                    gLineArtisan[info.gear][T(alliance)] = artisan;
            }
        }
    }
    LOG_INFO("server.loading", "[Supply] enabled: house={} bag={} ({} slots, sell {}) bolt={} recipe cloth/bolt={} "
             "bolts/bag={} thread/bag={} thread_price={} roles={} artisan A={} H={} learn={}", gBagHouseName, gBagItem,
             gBagSlots, gBagSell, gBoltItem, gRecipe.clothPerBolt, gRecipe.boltsPerBag, gRecipe.threadPerBag,
             gThreadPrice, gRoles.size(), gArtisan[0], gArtisan[1], gLearn.size());
    LOG_INFO("server.loading", "[Supply] artisan upkeep: free_slots={} min_level={} bag={} outfit={}",
             p.artisanFreeSlots, p.artisanMinLevel, kPouch, p.outfit);
    LOG_INFO("server.loading", "[Supply] rep store: {} mail_cap={} keep={}", p.repStore, p.repMailCap, p.repKeep);
    LOG_INFO("server.loading", "[Supply] throughput: direct_routes={} mail_pickup={} (yards {}, run_ms {}) mail_orders={} "
             "(buy_max_pct {})", p.directRoutes, p.mailPickup, p.mailPickupYards, p.mailRunMs, p.mailOrders, p.buyMaxPct);
    if (p.tiers)
        LOG_INFO("server.loading", "[Supply] tiers on: {} tiers, cloth cap {} per cloth; market={} buy_max_pct={} "
                 "buy_budget={} sell_keep={} list_float={}", kTierCount, p.clothCap, p.market, p.buyMaxPct, p.buyBudget,
                 p.sellKeep, p.listFloat);
    LOG_INFO("server.loading", "[Supply] finished bag market={} budget={} buy_max_pct={} priority={}", p.bagMarket,
             p.bagBuyBudget, p.buyMaxPct, gPriority.size());
    for (std::size_t li = 0; li < kLineCount; ++li)
        if (kCatalog[li].gearCount && LineOn(kCatalog[li].id))
            LOG_INFO("server.loading", "[Supply] line {} on: house={} artisan A={} H={} learn={} recipes={} equipment={} "
                     "max_order={} rep_stock={} pay_pct={} priority={}", kCatalog[li].name, gLineHouseName[li],
                     gLineArtisan[li][0], gLineArtisan[li][1], gLineLearn[li].size(), GearTable(kCatalog[li]).tierCount,
                     gGearRank[li].size(), p.gearMaxOrder, p.repStockPerItem, p.gearPayPct, gPriority.size());
    if (LineOn(Line::Engineering))
        LOG_INFO("server.loading", "[Supply] line eng: ammo target={} (gun hunters), consumer=load ammo",
                 p.ammoTarget);
    if (LineOn(Line::Engineering) && EngGuns())
        LOG_INFO("server.loading", "[Supply] line eng: EngGuns on: {} recipes (anvil parts, Rough Boomstick, Bronze Tube "
                 "bridge), tools {}", kEngGuns, std::size(kTools));
    if (p.demandOnly)
        LOG_INFO("server.loading", "[Supply] demand only: consumer-first skill-ups, surplus sales are `waste`, "
                 "order / deliver rows name their consumer");
    if (p.routeBagExtra || p.gearStockSell || p.gearSkillupRestock)
        LOG_INFO("server.loading", "[Supply] craft flow: route_bag_extra={} (needs tiers {}) gear_stock_sell={} "
                 "(keep {}) gear_skillup_restock={}", p.routeBagExtra, p.tiers, p.gearStockSell, p.repStockPerItem,
                 p.gearSkillupRestock);
    for (std::size_t li = 0; li < kLineCount; ++li)
        if (kCatalog[li].tierCount && LineOn(kCatalog[li].id))
            LOG_INFO("server.loading", "[Supply] line {} on: house={} artisan A={} H={} learn={} routed={} "
                     "route_herbs={} herb_cap={} target={} pay_pct={} max_order={} keep={} skillup_casts={}",
                     kCatalog[li].name, gLineHouseName[li], gLineArtisan[li][0], gLineArtisan[li][1],
                     gLineLearn[li].size(), gLineRoute[li].size(), p.routeHerbs, p.herbCap, p.potionTarget,
                     p.potionPayPct, p.potionMaxOrder, p.potionKeep, p.skillupCasts);
    if (p.smithEndgame || p.orderBackoffMs || p.mineMs)
        LOG_INFO("server.loading", "[Supply] smithfocus: endgame={} (+{} smith rows, ranks {} / {}) order_backoff_ms={} "
                 "mine_ms={} mine_cooldown_ms={}", p.smithEndgame, kSmithEndgame, kSmithRanks[0], kSmithRanks[1],
                 p.orderBackoffMs, p.mineMs, p.mineCooldownMs);
    if (p.potionTiers && LineOn(Line::Potions))
        LOG_INFO("server.loading", "[Supply] line potions: PotionTiers on: {} tiers (+{}: mana potions, Greater Healing, "
                 "Elixir of Wisdom bridge), a need per family (mana users), heal products first",
                 ActiveLine(Line::Potions).tierCount, LineOf(Line::Potions).tierExtra);
    if (p.potionTiers && p.potionLowBridge && LineOn(Line::Potions))
        LOG_INFO("server.loading", "[Supply] line potions: PotionLowBridge on: +{} (Elixir of Minor Defense bridge, alchemy "
                 "1-95 from Silverleaf)", LineOf(Line::Potions).tierLow);
    if (p.climbPastStock || p.gearMarketRoute)
        LOG_INFO("server.loading", "[Supply] housegaps: climb_past_stock={} gear_market_route={}", p.climbPastStock,
                 p.gearMarketRoute);
    if (p.climbCastable || p.smithCopper || p.craftTrace)
        LOG_INFO("server.loading", "[Supply] hordehouses: climb_castable={} smith_copper={} (live: {}) craft_trace={}",
                 p.climbCastable, p.smithCopper, SmithCopper(), p.craftTrace);
    if (p.craftDismount)
        LOG_INFO("server.loading", "[Supply] hordehouses2: craft_dismount=true");
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
    // RepStore: every configured rep wears its own loose bags before the chains count and deliver.
    for (std::size_t i = 0; p.repStore && i < AutoWowGuilds::Houses().size(); ++i)
        for (bool const alliance : {true, false})
            if (Player* rep = RoleRep(i, alliance))
                EquipOwnBags(rep);
    if (BagMarket())
    {
        FinishedBagTick(true);
        FinishedBagTick(false);
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
    for (ProductLine const& L : kCatalog)
        if (L.gearCount && LineOn(L.id))
        {
            GearTick(L.id, true, overlord);
            GearTick(L.id, false, overlord);
        }
    if (p.routeRaw)
    {
        RawTick(true);
        RawTick(false);
    }
    if (p.outfit)
        OutfitTick();
    if (p.mailOrders)
    {
        OrderTick(true, overlord);
        OrderTick(false, overlord);
    }
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
          std::uint32_t from, std::uint32_t to, char const* op, std::int64_t consumer)
{
    EmitLine(Line::Bags, p, r, oid, item, count, copper, from, to, op, consumer);
}

void EmitLine(Line l, Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
              std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op, std::int64_t consumer)
{
    // DemandOnly: a deliver row names its consumer (the receiver, or 0 for a role bot: the rep's stock).
    if (consumer < 0 && detail::gParams.demandOnly && r == Reason::Deliver)
        consumer = RoleOf(to).role == Role::None ? to : 0;
    std::string const tail = consumer >= 0 ? ConsumerField(static_cast<std::uint32_t>(consumer)) : std::string();
    std::string const& house = l == Line::Bags ? gBagHouseName : gLineHouseName[static_cast<std::size_t>(l)];
    LOG_INFO("playerbots", "[Supply] player={} {} house={} oid={} item={} count={} copper={} from={} to={}{}{}{}",
             p ? p->GetName() : "-", ReasonName(r), house, oid, item, count, copper, from, to,
             op ? " op=" : "", op ? op : "", consumer >= 0 ? " consumer=" + std::to_string(consumer) : std::string());
    if (p && AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSupply(p, ReasonName(r),
                                       LedgerFields(house, oid, item, count, copper, from, to, op) + LineField(l) + tail);
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

// Errand sell stop (RouteBagExtra): a cohort adventurer mails the bag tiers' extra reagent (Heavy Leather) to the bag
// house rep, each tier within its own room (ExtraRoom); the stacks are held back from this stop's sales. Mirrors
// RouteRaw; a Donate row per item ("line":"bags").
static void RouteBagExtra(Player* bot)
{
    std::uint32_t const guid = Low(bot);
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    if (!AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) || IsRole(bot))
        return;
    std::array<std::uint32_t, kTierCount> rooms{};
    {
        std::lock_guard<std::mutex> guard(gLock);
        rooms = gTeams[T(alliance)].extraRooms;
    }
    std::vector<std::vector<Stack>> stacks;
    for (std::size_t i = 0; i < kTierCount; ++i)
        stacks.push_back(kTiers[i].extra ? LooseStacks(bot, kTiers[i].extra) : std::vector<Stack>{});
    RoutePlan plan = PlanRoute(stacks, std::vector<std::uint32_t>(rooms.begin(), rooms.end()));
    if (plan.picks.empty())
        return;
    {
        std::lock_guard<std::mutex> guard(gLock);
        std::array<std::uint32_t, kTierCount>& live = gTeams[T(alliance)].extraRooms;
        if (gHeld.size() + plan.picks.size() > kMaxHeld)
            return;
        for (std::size_t i = 0; i < kTierCount; ++i)
            live[i] -= std::min(live[i], plan.units[i]);
        gHeld.insert(plan.picks.begin(), plan.picks.end());
    }
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<DonateOperation>(bot->GetGUID(), alliance, std::move(plan.picks)));
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
    if (p.tiers && p.routeBagExtra)
        RouteBagExtra(bot);
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

FinishedBagView FinishedBagViewOf(bool alliance)
{
    std::lock_guard<std::mutex> guard(gLock);
    return gFinishedBagViews[T(alliance)];
}

void QueueFinishedBagBuys(Player* rep, std::uint64_t auctioneerRawGuid, std::vector<FinishedBagListing> listings)
{
    if (!BagMarket() || !rep || listings.empty())
        return;
    std::vector<FinishedBagBuy> buys;
    {
        std::lock_guard<std::mutex> guard(gLock);
        std::size_t const team = T(rep->GetTeamId() == TEAM_ALLIANCE);
        buys = PlanFinishedBagBuys(std::move(listings), gFinishedBagViews[team], Low(rep), detail::gParams.buyMaxPct,
                                   detail::gParams.bagBuyBudget);
        std::vector<FinishedBagBuy> reserved;
        for (FinishedBagBuy const& buy : buys)
        {
            if (gFinishedBagPending.size() >= kMaxFinishedBagPending ||
                gFinishedBagPending.count(buy.listing.auctionId))
                continue;
            gFinishedBagPending.emplace(
                buy.listing.auctionId,
                FinishedBagPending{buy, Low(rep), rep->GetTeamId() == TEAM_ALLIANCE, FinishedBagPurchaseState::Queued});
            for (std::uint32_t const recipient : buy.recipients)
                gFinishedBagViews[team].coverage.push_back(
                    {recipient, buy.listing.slots, 1, FinishedBagCoverageSource::InFlight, {}, buy.listing.auctionId});
            reserved.push_back(buy);
        }
        buys = std::move(reserved);
    }
    if (!buys.empty())
        PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<MarketBuyOperation>(rep->GetGUID(), ObjectGuid(auctioneerRawGuid), std::move(buys)));
}

bool ReservedFinishedBagItem(Player* representative, std::uint64_t itemRawGuid)
{
    if (!representative || !itemRawGuid)
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    return std::any_of(gFinishedBagPending.begin(), gFinishedBagPending.end(),
                       [&](auto const& row)
                       {
                           FinishedBagPending const& pending = row.second;
                           return pending.representative == Low(representative) &&
                                  pending.state != FinishedBagPurchaseState::Queued &&
                                  pending.buy.listing.itemGuid == itemRawGuid;
                       });
}

bool ReservedFinishedBagEntry(Player* representative, std::uint32_t item)
{
    if (!representative || !item)
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    return std::any_of(gFinishedBagPending.begin(), gFinishedBagPending.end(),
                       [&](auto const& row)
                       {
                           FinishedBagPending const& pending = row.second;
                           return pending.representative == Low(representative) &&
                                  pending.state != FinishedBagPurchaseState::Queued && pending.buy.listing.item == item;
                       });
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

std::uint32_t GearYield(Line l, std::uint8_t recipe)
{
    std::vector<std::uint32_t> const& y = gGearYield[static_cast<std::size_t>(l)];
    return recipe < y.size() ? y[recipe] : 1;
}
std::vector<std::uint32_t> const& LineLearnSpells(Line l) { return gLineLearn[static_cast<std::size_t>(l)]; }
std::uint8_t LineSpecOf(Line l, bool alliance) { return gLineSpec[static_cast<std::size_t>(l)][T(alliance)]; }

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
            // skill-up tier first, a tier below both no more unless a cloth_gear order lacks it (ClothDemandOf).
            for (std::size_t i = 0; i < kTierCount; ++i)
                if (ClothDemand const d = ClothDemandOf(i, ts.artisanSkill, ts.goal, ts.skillup, ts.gearCloth[i]);
                    d != ClothDemand::Done)
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
            ProductLine const A = ActiveLine(L.id);
            for (std::size_t i = 0; i < gLineRoute[li].size(); ++i)
                if (UsableNow(A, gLineRoute[li][i], v.artisanSkill))
                    out.push_back({gLineRoute[li][i], v.rooms[i]});
        }
    if (p.routeRaw)
        for (std::size_t k = 0; k < kRawKinds; ++k)
            if (gRawHouse[k] < AutoWowGuilds::Houses().size())
                for (std::size_t i = 0; i < kRawItems; ++i)
                    out.push_back({kRawLists[k][i], gRawRooms[t][k][i]});
    return out;
}

void RequestGrant(Player* bot, std::uint64_t need, std::uint64_t room, std::uint64_t roomBudgetPerHour)
{
    if (!Outfit() || !bot || !need)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    gGrantRequests[Low(bot)] = need;
    if (room)
        gGrantRooms[Low(bot)] = {room, roomBudgetPerHour};
    else
        gGrantRooms.erase(Low(bot));
}

bool GrantPending(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    return gGrantRequests.count(guid) != 0;
}

bool HasSupplyMail(Player* bot)
{
    time_t const now = GameTime::GetGameTime().count();
    for (Mail const* m : bot->GetMails())
        if (m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && !m->COD && m->HasItems() &&
            m->messageType == MAIL_NORMAL &&
            (RoleOf(static_cast<std::uint32_t>(m->sender)).role != Role::None ||
             (AutoWowGear::FlowEnabled() && m->subject == AutoWowGear::kFlowSubject)))
            return true;
    return false;
}

namespace
{
std::uint32_t gFlowAcc = 0;

// Item level `proto` adds over what `m` wears in the slot it would take (FindEquipSlot, as the auction gear plan);
// 0 when not higher, or for an off hand under a two-hander.
std::uint32_t SlotGain(PlayerbotAI* ai, Player* m, ItemTemplate const* proto)
{
    uint8 const slot = ai->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot >= EQUIPMENT_SLOT_END)
        return 0;
    Item const* mh = m->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (slot == EQUIPMENT_SLOT_OFFHAND && mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON &&
        !m->CanTitanGrip())
        return 0;
    Item const* worn = m->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    std::uint32_t const wornIlvl = worn ? worn->GetTemplate()->ItemLevel : 0;
    return proto->ItemLevel > wornIlvl ? proto->ItemLevel - wornIlvl : 0;
}
}  // namespace

// AutoWow.Gear.Flow (world thread, maps idle): per faction the gear recipients in ascending guid, each holder's loose
// pieces in bag order; a piece that Flows goes to the member PickTaker names (the ilvl gain is the cheap filter, the
// stock scorer confirms), one mail per recipient per pass, at most FlowMaxMails per pass.
// ponytail: recipients x flowing pieces scorer calls per pass; a round-robin holder cursor if the pass shows in the
// world diff.
void GearFlowUpdate(std::uint32_t diff)
{
    AutoWowGear::FlowParams const& fp = AutoWowGear::GetFlow();
    gFlowAcc += diff;
    if (gFlowAcc < fp.tickMs)
        return;
    gFlowAcc = 0;
    std::uint32_t mails = 0;
    for (bool const alliance : {true, false})
    {
        std::vector<Player*> const members = GearRecipients(alliance);
        std::unordered_set<std::uint32_t> served;
        for (Player* holder : members)
        {
            PlayerbotAI* const holderAi = PlayerbotsMgr::instance().GetPlayerbotAI(holder);
            std::vector<Item*> pieces;
            ForEachLoose(holder, [&](Item* item) {
                ItemTemplate const* proto = item->GetTemplate();
                bool const gear = (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
                                  proto->InventoryType != INVTYPE_NON_EQUIP;
                bool const tradeable = item->CanBeTraded(true);
                if (!gear || !tradeable || proto->Quality < fp.minQuality)
                    return;  // cheap facts first: the holder's scorer only for candidates
                if (AutoWowGear::Flows(fp, proto->Quality, gear, tradeable, GearUpgrade(holderAi, holder, proto)))
                    pieces.push_back(item);
            });
            for (Item* item : pieces)
            {
                if (mails >= fp.maxMails)
                    return;
                ItemTemplate const* proto = item->GetTemplate();
                std::vector<AutoWowGear::FlowTaker> takers;
                for (Player* m : members)
                {
                    if (m == holder || served.count(Low(m)))
                        continue;
                    PlayerbotAI* const ai = PlayerbotsMgr::instance().GetPlayerbotAI(m);
                    std::uint32_t const gain = SlotGain(ai, m, proto);
                    if (gain && GearUpgrade(ai, m, proto))
                        takers.push_back({Low(m), gain});
                }
                std::size_t const k = AutoWowGear::PickTaker(takers);
                if (k == AutoWowGear::kNone)
                    continue;
                std::uint32_t const entry = proto->ItemId, to = takers[k].guid;
                char const* why = nullptr;
                bool const sent = AutoWowGuilds::SendItems(Low(holder), to,
                                                           {static_cast<std::uint32_t>(item->GetGUID().GetCounter())},
                                                           AutoWowGear::kFlowSubject, &why);
                LOG_INFO("playerbots", "[GearFlow] mail bot={} to={} item={} quality={} ilvl_gain={} result={}",
                         holder->GetName(), to, entry, proto->Quality, takers[k].gain, sent ? "sent" : why ? why : "refused");
                if (!sent)
                    continue;
                served.insert(to);
                ++mails;
            }
        }
    }
}

// ---- AutoWow.Gear.AhBroker runtime (AhBrokerPolicy.h) -------------------------------------------------------------
// The per-faction broker is the bag-house rep (already stationed at its capital auctioneer + mailbox). Its map-thread
// Task::Market visit drains the team's AH-gear request queue and buys each requester's selection with treasury gold
// (the shared MarketBuyOperation: Pay then buyout, won items mailed to the rep). The world-thread delivery half mails
// each won item to its requester COD; the requester's own mail stop pays it (AutoWowTrade::TakeBrokerCod).
namespace
{
std::mutex gBrokerLock;  // producer: a rep's map-thread buy; consumer: the world-thread delivery (maps idle, exclusive)
struct BrokerDelivery
{
    std::uint32_t requester = 0;
    std::uint32_t item = 0;       // entry (log / match)
    std::uint32_t itemGuid = 0;   // the won auction item; its guid survives into the rep's win mail and bags
    std::uint32_t cod = 0;        // AutoWowBroker::CodPrice(buyout)
    std::uint8_t team = 0;        // core TeamId
    std::uint32_t bornSec = 0;    // GameTime seconds at buy (the undelivered-timeout clock)
};
std::vector<BrokerDelivery> gBrokerDeliveries;   // gBrokerLock
struct BrokerRate { std::uint32_t windowSec = 0; std::uint32_t count = 0; };
std::array<BrokerRate, 2> gBrokerRate{};         // per team; touched only by that team's single rep (map thread)
std::uint32_t gBrokerAcc = 0;                    // world thread only
constexpr std::uint32_t kBrokerMaxPerVisit = 8;  // buys per auctioneer visit, over and above the per-minute window
constexpr std::int64_t kBrokerTimeoutSec = 1800; // 30 min: a never-delivered / stale request is dropped

// The requester's AH-gear offers from this faction house: a buyout listing it can use that raises the ilvl of the slot
// it fills. Lightweight (usability + FindEquipSlot + worn ilvl, as the gear-flow pass), no heavy stock scorer.
// ponytail: the rep reads another online player's equip cross-thread here, exactly as the finished-bag market reads
// recipient->CanUseItem at this same seam; acceptable per that precedent, the live soak watches it.
std::vector<AutoWowGear::AhOffer> BrokerOffers(Player* requester, AuctionHouseObject* ah)
{
    std::vector<AutoWowGear::AhOffer> offers;
    PlayerbotAI* const ai = PlayerbotsMgr::instance().GetPlayerbotAI(requester);
    if (!ai || !ah)
        return offers;
    std::uint32_t const level = requester->GetLevel();
    Item const* mh = requester->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    bool const wields2h = mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !requester->CanTitanGrip();
    for (auto const& [id, a] : ah->GetAuctions())
    {
        if (!a || a->owner == requester->GetGUID() || !a->buyout || !a->itemCount)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->item_template);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->RequiredLevel > level ||
            requester->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        uint8 const slot = ai->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot >= EQUIPMENT_SLOT_END || (slot == EQUIPMENT_SLOT_OFFHAND && wields2h))
            continue;
        Item const* worn = requester->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::uint32_t const wornIlvl = worn ? worn->GetTemplate()->ItemLevel : 0;
        if (proto->ItemLevel <= wornIlvl)
            continue;
        offers.push_back({id, a->item_template, a->buyout, proto->ItemLevel - wornIlvl, slot,
                          proto->InventoryType == INVTYPE_2HWEAPON, static_cast<std::uint8_t>(proto->Quality)});
    }
    return offers;
}
}  // namespace

void QueueBrokerBuys(Player* rep, std::uint64_t auctioneerRawGuid)
{
    if (!AutoWowBroker::Enabled() || !rep || !rep->IsInWorld() || !rep->GetSession())
        return;
    Creature* npc = rep->GetNPCIfCanInteractWith(ObjectGuid(auctioneerRawGuid), UNIT_NPC_FLAG_AUCTIONEER);
    AuctionHouseEntry const* house =
        npc ? AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(npc->GetFaction()) : nullptr;
    AuctionHouseObject* ah = npc ? sAuctionMgr->GetAuctionsMap(npc->GetFaction()) : nullptr;
    std::uint8_t const team = static_cast<std::uint8_t>(rep->GetTeamId());
    std::uint32_t const expected = uint32(team == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
    if (!house || !ah || house->houseId != expected)
        return;
    AutoWowBroker::Params const& bp = AutoWowBroker::Get();
    time_t const nowSec = GameTime::GetGameTime().count();
    BrokerRate& rate = gBrokerRate[T(team == TEAM_ALLIANCE)];
    if (static_cast<std::uint32_t>(nowSec) - rate.windowSec >= 60)
    {
        rate.windowSec = static_cast<std::uint32_t>(nowSec);
        rate.count = 0;
    }
    std::vector<MarketListing> buys;
    std::vector<BrokerDelivery> pend;
    std::uint32_t perVisit = 0;
    for (AutoWowBroker::Request const& req : AutoWowBroker::TeamRequests(team))
    {
        if (!AutoWowBroker::CanBuy(bp, rate.count) || perVisit >= kBrokerMaxPerVisit)
            break;
        Player* requester = Online(req.guid);
        if (!requester)
        {
            if (nowSec - static_cast<time_t>(req.filedSec) > kBrokerTimeoutSec)
            {
                LOG_INFO("playerbots", "[AhBroker] drop req={} reason=offline_stale", req.guid);
                AutoWowBroker::Forget(req.guid);
            }
            continue;  // offline but fresh: leave it for a later visit
        }
        if (requester->GetLevel() >= req.level + 2 || nowSec - static_cast<time_t>(req.filedSec) > kBrokerTimeoutSec)
        {
            LOG_INFO("playerbots", "[AhBroker] drop req={} reason={} lvl={}->{}", req.guid,
                     requester->GetLevel() >= req.level + 2 ? "outleveled" : "stale", req.level, requester->GetLevel());
            AutoWowBroker::Forget(req.guid);
            continue;
        }
        std::vector<AutoWowGear::AhOffer> const plan = AutoWowBroker::PlanForRequest(req, BrokerOffers(requester, ah));
        std::uint32_t bought = 0;
        for (AutoWowGear::AhOffer const& o : plan)
        {
            if (!AutoWowBroker::CanBuy(bp, rate.count) || perVisit >= kBrokerMaxPerVisit)
                break;
            std::uint32_t const itemGuid = [&]() -> std::uint32_t
            {
                AuctionEntry const* a = ah->GetAuction(o.id);
                return a ? static_cast<std::uint32_t>(a->item_guid.GetCounter()) : 0;
            }();
            if (!itemGuid)
                continue;  // listing gone since BrokerOffers
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(o.item);
            buys.push_back({o.id, o.item, 1, static_cast<std::uint32_t>(o.price)});
            pend.push_back({req.guid, o.item, itemGuid, static_cast<std::uint32_t>(AutoWowBroker::CodPrice(bp, o.price)),
                            team, static_cast<std::uint32_t>(nowSec)});
            LOG_INFO("playerbots", "[AhBroker] buy broker={} for={} item={} ilvl={} price={}", Low(rep), req.guid,
                     o.item, proto ? proto->ItemLevel : 0, o.price);
            ++rate.count;
            ++perVisit;
            ++bought;
        }
        if (bought)
            AutoWowBroker::Forget(req.guid);  // served this visit; a still-needy bot re-files next level
    }
    if (buys.empty())
        return;
    {
        std::lock_guard<std::mutex> guard(gBrokerLock);
        gBrokerDeliveries.insert(gBrokerDeliveries.end(), pend.begin(), pend.end());
    }
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MarketBuyOperation>(rep->GetGUID(), ObjectGuid(auctioneerRawGuid), std::move(buys)));
}

void AhBrokerUpdate(std::uint32_t diff)
{
    gBrokerAcc += diff;
    if (gBrokerAcc < AutoWowBroker::Get().tickMs)
        return;
    gBrokerAcc = 0;
    std::vector<BrokerDelivery> snap;
    {
        std::lock_guard<std::mutex> guard(gBrokerLock);
        snap = gBrokerDeliveries;
    }
    if (snap.empty())
        return;
    time_t const nowSec = GameTime::GetGameTime().count();
    std::unordered_set<std::uint32_t> done;  // itemGuids delivered or dropped this pass
    for (bool const alliance : {true, false})
    {
        Player* rep = RoleRep(gBagHouse, alliance);
        std::uint8_t const team = alliance ? TEAM_ALLIANCE : TEAM_HORDE;
        for (BrokerDelivery const& d : snap)
        {
            if (d.team != team || done.count(d.itemGuid))
                continue;
            bool const timedOut = nowSec - static_cast<time_t>(d.bornSec) > kBrokerTimeoutSec;
            bool loose = false;
            if (rep)
                ForEachLoose(rep, [&](Item* it)
                             { loose = loose || static_cast<std::uint32_t>(it->GetGUID().GetCounter()) == d.itemGuid; });
            Player* requester = Online(d.requester);
            if (rep && loose && requester)
            {
                char const* why = nullptr;
                bool const sent = AutoWowGuilds::SendItemsCod(Low(rep), d.requester, {d.itemGuid}, d.cod,
                                                              AutoWowBroker::kBrokerSubject, &why);
                LOG_INFO("playerbots", "[AhBroker] cod_sent broker={} to={} item={} price={} result={}", Low(rep),
                         d.requester, d.item, d.cod, sent ? "sent" : why ? why : "refused");
                if (sent)
                    done.insert(d.itemGuid);
                continue;  // not sent (e.g. mailbox full): keep it for the next pass
            }
            if (timedOut)
            {
                LOG_INFO("playerbots", "[AhBroker] drop req={} reason=undelivered item={}", d.requester, d.item);
                done.insert(d.itemGuid);
            }
        }
    }
    if (done.empty())
        return;
    std::lock_guard<std::mutex> guard(gBrokerLock);
    gBrokerDeliveries.erase(std::remove_if(gBrokerDeliveries.begin(), gBrokerDeliveries.end(),
                                           [&](BrokerDelivery const& d) { return done.count(d.itemGuid) != 0; }),
                            gBrokerDeliveries.end());
}

// The seller's tradeable loose stacks of `item` (whole stacks; the mail helper refuses bound items).
static std::vector<Stack> TradeableStacks(Player* bot, std::uint32_t item)
{
    std::vector<Stack> out;
    ForEachLoose(bot, [&](Item* it) {
        if (it->GetEntry() == item && it->CanBeTraded(true))
            out.push_back({static_cast<std::uint32_t>(it->GetGUID().GetCounter()), it->GetCount()});
    });
    return out;
}

bool HoldsOrderedItem(Player* bot)
{
    if (!MailOrders() || !bot)
        return false;
    std::vector<MailOrder> orders;
    {
        std::lock_guard<std::mutex> guard(gLock);
        orders = gOrders[T(bot->GetTeamId() == TEAM_ALLIANCE)];
    }
    for (MailOrder const& o : orders)
        if (o.units && o.rep != Low(bot) && !PickWithin(TradeableStacks(bot, o.item), o.units).empty())
            return true;
    return false;
}

std::uint32_t FillOrders(Player* bot, std::uint32_t maxMails)
{
    if (!MailOrders() || !bot || !maxMails)
        return 0;
    std::size_t const t = T(bot->GetTeamId() == TEAM_ALLIANCE);
    std::vector<MailOrder> orders;
    {
        std::lock_guard<std::mutex> guard(gLock);
        orders = gOrders[t];
    }
    std::uint32_t mails = 0;
    for (MailOrder const& o : orders)
    {
        if (mails >= maxMails)
            break;
        if (!o.units || o.rep == Low(bot))
            continue;
        std::vector<Stack> const stacks = TradeableStacks(bot, o.item);
        std::vector<std::uint32_t> pick = PickWithin(stacks, o.units);
        std::uint32_t units = 0;
        for (Stack const& st : stacks)
            if (std::find(pick.begin(), pick.end(), st.guid) != pick.end())
                units += st.count;
        if (!units)
            continue;
        {
            std::lock_guard<std::mutex> guard(gLock);
            auto const it = std::find_if(gOrders[t].begin(), gOrders[t].end(), [&](MailOrder const& x)
                                         { return x.rep == o.rep && x.item == o.item; });
            if (it == gOrders[t].end() || it->units < units)
                continue;  // another seller took the order since the copy
            it->units -= units;
        }
        LOG_INFO("playerbots", "[Supply] order fill bot={} rep={} item={} units={} unit_price={}", bot->GetName(), o.rep,
                 o.item, units, o.unitPrice);
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<CodSellOperation>(
            bot->GetGUID(), o.rep, o.item, std::move(pick), units, std::uint64_t(units) * o.unitPrice));
        ++mails;
    }
    return mails;
}

CodVerdict CodAtRep(Player* rep, std::uint32_t seller, std::uint32_t item, std::uint32_t units, std::uint64_t cod,
                    bool room, char const** why)
{
    auto const it = gCodPending.find(std::make_tuple(Low(rep), seller, item));
    CodPending const pending = it == gCodPending.end() ? CodPending{} : it->second;
    std::uint32_t const gid = AutoWowGuilds::HouseGuildOf(rep);
    std::uint64_t const funds = rep->GetMoney() + (gid ? AutoWowGuilds::Balance(gid) : 0);
    CodVerdict v = DecideCod(pending, units, cod, funds, room);
    if (v == CodVerdict::Wait)
        return v;
    // The house bank tops the rep's purse up to the COD (a guild `pay` row); a refused top-up returns the mail.
    if (v == CodVerdict::Accept && rep->GetMoney() < cod && !(gid && AutoWowGuilds::Pay(gid, rep, cod - rep->GetMoney())))
        v = CodVerdict::Return;
    if (why && v == CodVerdict::Return)
        *why = !units || units > pending.units || cod > pending.cod ? "unordered" : "budget";
    if (it != gCodPending.end())
    {
        it->second.units -= std::min(it->second.units, units);
        it->second.cod -= std::min(it->second.cod, cod);
        if (!it->second.units)
            gCodPending.erase(it);
    }
    return v;
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
