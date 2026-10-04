/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Gear.NoWhite runtime (NoWhitePolicy.h): the world pass for sources (a) own bags / bank, (c) hand-me-downs and
// (d) smith orders, plus the per-bot source track the errands read for (b) auction and (f) vendor last resort.

#include <algorithm>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "ErrandsPolicy.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "NoWhitePolicy.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SquadPolicy.h"
#include "SupplyPolicy.h"
#include "TradePolicy.h"
#include "WeaponOrderPolicy.h"

namespace AutoWowNoWhite
{
namespace
{
inline constexpr std::array<std::uint8_t, 3> kSlots = {kSlotMainHand, kSlotOffHand, kSlotRanged};

struct State
{
    Track track;
    std::array<std::uint32_t, 3> oid{};  // open smith order per weapon slot (kSlots order), 0 = none
};

std::mutex gLock;
std::unordered_map<std::uint32_t, State> gStates;  // floor-due bots only
std::uint32_t gAcc = 0;

std::uint32_t Low(Player* p) { return static_cast<std::uint32_t>(p->GetGUID().GetCounter()); }

bool Role(std::uint32_t guid) { return AutoWowSupply::RoleOf(guid).role != AutoWowSupply::Role::None; }

// An errand bot may get its weapon at the auction house (b): no role, the errand auction path on.
bool ErrandBot(Player* bot)
{
    return !Role(Low(bot)) && AutoWowErrands::Enabled() && AutoWowGear::AuctionEnabled() && AutoWowTrade::Enabled();
}

bool DualWield(Player* bot) { return bot->CanDualWield(); }

bool OffHandFree(Player* bot) { return !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND); }

// The stock "item upgrade" scorer rates the piece an equip (EQUIP / REPLACE) for `m` (as the gear flow and AH gear).
bool Upgrade(PlayerbotAI* ai, Player* m, ItemTemplate const* proto)
{
    if (!ai || !proto || proto->RequiredLevel > m->GetLevel() || m->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    ItemUsage const u =
        ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", std::to_string(proto->ItemId))->Get();
    return u == ITEM_USAGE_EQUIP || u == ITEM_USAGE_REPLACE;
}

// The weapon slot of the bot `proto` may go to among `slots` (mask), else 0xFF.
std::uint8_t SlotFor(Player* bot, ItemTemplate const* proto, std::uint32_t slots)
{
    if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        return 0xFF;
    for (std::uint8_t const slot : kSlots)
        if ((slots & (1u << slot)) && Fits(proto->InventoryType, slot, DualWield(bot), OffHandFree(bot)))
            return slot;
    return 0xFF;
}

void Ledger(Player* bot, char const* reason, std::uint8_t slot, ItemTemplate const* proto, std::uint32_t from,
            std::uint32_t oid)
{
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitErrand(bot, reason,
                                       LedgerFields(slot, proto ? proto->ItemId : 0, proto ? proto->ItemLevel : 0,
                                                    proto ? proto->Quality : 0, FloorIlvl(Get(), bot->GetLevel()),
                                                    from, oid));
}

bool FromBank(std::uint16_t pos)
{
    std::uint8_t const bag = static_cast<std::uint8_t>(pos >> 8), slot = static_cast<std::uint8_t>(pos & 0xFF);
    return bag == INVENTORY_SLOT_BAG_0 ? slot >= BANK_SLOT_ITEM_START && slot < BANK_SLOT_ITEM_END
                                       : bag >= BANK_SLOT_BAG_START && bag < BANK_SLOT_BAG_END;
}

// (a) Own weapons in the backpack, bags, bank and bank bags; the best per due slot is swapped in. Returns the slots
// that got one.
std::uint32_t EquipOwned(Player* bot, PlayerbotAI* ai, std::uint32_t due)
{
    struct Owned
    {
        Item* item;
        std::uint16_t pos;
    };
    std::vector<Owned> owned;
    auto add = [&](Item* item, std::uint8_t bag, std::uint8_t slot)
    {
        if (item && item->GetTemplate()->Class == ITEM_CLASS_WEAPON)
            owned.push_back({item, static_cast<std::uint16_t>((bag << 8) | slot)});
    };
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);
    for (std::uint8_t slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);
    for (std::uint8_t bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (std::uint32_t slot = 0; slot < b->GetBagSize(); ++slot)
                add(b->GetItemByPos(slot), bag, static_cast<std::uint8_t>(slot));
    for (std::uint8_t bag = BANK_SLOT_BAG_START; bag < BANK_SLOT_BAG_END; ++bag)
        if (Bag* b = bot->GetBagByPos(bag))
            for (std::uint32_t slot = 0; slot < b->GetBagSize(); ++slot)
                add(b->GetItemByPos(slot), bag, static_cast<std::uint8_t>(slot));
    std::uint32_t got = 0;
    uint32 const level = bot->GetLevel();
    for (std::uint8_t const slot : kSlots)
    {
        if (!(due & (1u << slot)))
            continue;
        Worn const worn = WornOf(bot, slot);
        std::vector<Candidate> cands;
        std::vector<std::size_t> index;
        for (std::size_t k = 0; k < owned.size(); ++k)
        {
            ItemTemplate const* proto = owned[k].item->GetTemplate();
            if (!Fits(proto->InventoryType, slot, DualWield(bot), OffHandFree(bot)) ||
                !Improves(proto->Quality, proto->ItemLevel, worn) || !Upgrade(ai, bot, proto))
                continue;
            cands.push_back({Low(bot), static_cast<std::uint32_t>(owned[k].item->GetGUID().GetCounter()), proto->ItemId,
                             static_cast<std::uint8_t>(proto->Quality), proto->ItemLevel});
            index.push_back(k);
        }
        std::size_t const pick = PickCandidate(Get(), level, worn, cands);
        if (pick == kNone)
            continue;
        Owned const& o = owned[index[pick]];
        ItemTemplate const* proto = o.item->GetTemplate();
        ObjectGuid const itemGuid = o.item->GetGUID();
        bot->SwapItem(o.pos, static_cast<std::uint16_t>((INVENTORY_SLOT_BAG_0 << 8) | slot));
        Item const* now = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        bool const equipped = now && now->GetGUID() == itemGuid;
        LOG_INFO("playerbots", "[NoWhite] bot={} source=bag slot={} item={} ilvl={} quality={} from_bank={} "
                 "worn_ilvl={} worn_quality={} floor={} equipped={} lvl={}", bot->GetName(), uint32(slot),
                 proto->ItemId, proto->ItemLevel, proto->Quality, FromBank(o.pos), worn.ilvl, uint32(worn.quality), FloorIlvl(Get(), level),
                 equipped, level);
        if (!equipped)
            continue;
        got |= 1u << slot;
        Ledger(bot, "weapon_floor", slot, proto, Low(bot), 0);
        // the swapped-out piece may have taken a slot an owned weapon held: rescan on the next pass
        break;
    }
    return got;
}

// A loose, tradeable green+ weapon a non-role member may give away: no upgrade for itself, not held for a donation.
struct Giveable
{
    Player* holder;
    Item* item;
};

std::vector<Giveable> GiveablesOf(Player* holder)
{
    std::vector<Giveable> out;
    PlayerbotAI* const ai = PlayerbotsMgr::instance().GetPlayerbotAI(holder);
    auto consider = [&](Item* item)
    {
        if (!item)
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_WEAPON || proto->Quality < kQualityGreen || !item->CanBeTraded(true) ||
            AutoWowSupply::HeldForDonation(static_cast<std::uint32_t>(item->GetGUID().GetCounter())) ||
            Upgrade(ai, holder, proto))
            return;
        out.push_back({holder, item});
    };
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(holder->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (std::uint8_t bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* b = holder->GetBagByPos(bag))
            for (std::uint32_t slot = 0; slot < b->GetBagSize(); ++slot)
                consider(b->GetItemByPos(slot));
    return out;
}

// (c) The best giveable weapon for one due slot, mailed (gear flow subject). Returns true when a mail went out.
bool HandDown(Player* bot, PlayerbotAI* ai, std::uint32_t due, std::vector<Giveable>& pool)
{
    uint32 const level = bot->GetLevel();
    for (std::uint8_t const slot : kSlots)
    {
        if (!(due & (1u << slot)))
            continue;
        Worn const worn = WornOf(bot, slot);
        std::vector<Candidate> cands;
        std::vector<std::size_t> index;
        for (std::size_t k = 0; k < pool.size(); ++k)
        {
            if (pool[k].holder == bot)
                continue;
            ItemTemplate const* proto = pool[k].item->GetTemplate();
            if (!Fits(proto->InventoryType, slot, DualWield(bot), OffHandFree(bot)) ||
                !Improves(proto->Quality, proto->ItemLevel, worn) || !Upgrade(ai, bot, proto))
                continue;
            cands.push_back({Low(pool[k].holder), static_cast<std::uint32_t>(pool[k].item->GetGUID().GetCounter()),
                             proto->ItemId, static_cast<std::uint8_t>(proto->Quality), proto->ItemLevel});
            index.push_back(k);
        }
        std::size_t const pick = PickCandidate(Get(), level, worn, cands);
        if (pick == kNone)
            continue;
        Giveable const g = pool[index[pick]];
        ItemTemplate const* proto = g.item->GetTemplate();
        std::uint32_t const from = Low(g.holder), itemGuid = cands[pick].itemGuid;
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(index[pick]));  // the item leaves the pool either way
        char const* why = nullptr;
        bool const sent = AutoWowGuilds::SendItems(from, Low(bot), {itemGuid}, AutoWowGear::kFlowSubject, &why);
        LOG_INFO("playerbots", "[NoWhite] bot={} source=handdown slot={} item={} ilvl={} quality={} from={} "
                 "worn_ilvl={} worn_quality={} floor={} result={} lvl={}", bot->GetName(), uint32(slot), proto->ItemId,
                 proto->ItemLevel, proto->Quality, from, worn.ilvl, uint32(worn.quality), FloorIlvl(Get(), level),
                 sent ? "sent" : why ? why : "refused", level);
        if (!sent)
            continue;
        Ledger(bot, "handdown_weapon", slot, proto, from, 0);
        return true;
    }
    return false;
}

// (d) One smith order per due slot without one.
void Order(Player* bot, std::uint32_t due, State& st)
{
    uint32 const level = bot->GetLevel();
    for (std::size_t k = 0; k < kSlots.size(); ++k)
    {
        std::uint8_t const slot = kSlots[k];
        if (!(due & (1u << slot)) || st.oid[k])
            continue;
        AutoWowWeaponOrder::Order o;
        o.guid = Low(bot);
        o.slot = slot;
        o.cls = bot->getClass();
        o.level = level;
        o.minIlvl = FloorIlvl(Get(), level);
        o.alliance = bot->GetTeamId() == TEAM_ALLIANCE;
        st.oid[k] = AutoWowWeaponOrder::Post(o);
        Worn const worn = WornOf(bot, slot);
        LOG_INFO("playerbots", "[NoWhite] bot={} source=smith slot={} oid={} min_ilvl={} worn_ilvl={} worn_quality={} "
                 "cls={} lvl={}", bot->GetName(), uint32(slot), st.oid[k], o.minIlvl, worn.ilvl, uint32(worn.quality),
                 uint32(o.cls), level);
        if (st.oid[k])
            Ledger(bot, "smith_order", slot, nullptr, 0, st.oid[k]);
    }
}

void CancelOrders(State& st, std::uint32_t due)
{
    for (std::size_t k = 0; k < kSlots.size(); ++k)
        if (st.oid[k] && !(due & (1u << kSlots[k])))
        {
            AutoWowWeaponOrder::Cancel(st.oid[k]);
            st.oid[k] = 0;
        }
}

bool Member(Player* p, std::vector<AutoWowGuilds::GuidRange> const& cohort, std::vector<std::uint32_t> const& squad)
{
    std::uint32_t const g = Low(p);
    for (AutoWowGuilds::GuidRange const& r : cohort)
        if (g >= r.lo && g <= r.hi)
            return true;
    return std::binary_search(squad.begin(), squad.end(), g) || Role(g);
}
}  // namespace

Worn WornOf(Player* bot, std::uint8_t slot)
{
    Worn w;
    if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
    {
        ItemTemplate const* proto = item->GetTemplate();
        w.present = true;
        w.weapon = proto->Class == ITEM_CLASS_WEAPON;
        w.quality = static_cast<std::uint8_t>(proto->Quality);
        w.ilvl = proto->ItemLevel;
    }
    return w;
}

std::uint32_t FloorSlotsOf(Player* bot)
{
    if (!Enabled() || !bot)
        return 0;
    return FloorSlots(Get(), bot->GetLevel(), WornOf(bot, kSlotMainHand), WornOf(bot, kSlotOffHand),
                      WornOf(bot, kSlotRanged), bot->getClass() == CLASS_HUNTER, DualWield(bot));
}

void MarkAuctionTried(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    if (it != gStates.end())
        it->second.track.tried |= SrcAuction;
}

bool VendorWhiteAllowed(Player* bot, std::uint8_t slot)
{
    Track t;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gStates.find(Low(bot));
        if (it == gStates.end())
            return false;
        t = it->second.track;
    }
    return VendorLastResort(Get(), t, ErrandBot(bot), WornOf(bot, slot));
}

// World thread, maps idle: per faction the members (cohort, squad roster, supply roles) in ascending guid; each one
// with a weapon slot under the floor takes its next source step (NextStep). Hand-me-downs: at most MaxMails per pass.
void WorldUpdate(std::uint32_t diff)
{
    Params const& p = Get();
    gAcc += diff;
    if (gAcc < p.tickMs)
        return;
    gAcc = 0;
    std::vector<AutoWowGuilds::GuidRange> const& cohort = AutoWowGuilds::Cohort();
    std::array<std::vector<Player*>, 2> members;  // [alliance]
    {
        std::array<std::vector<std::uint32_t>, 2> squad = {AutoWowSquad::Roster(false), AutoWowSquad::Roster(true)};
        for (auto& r : squad)
            std::sort(r.begin(), r.end());
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, pl] : ObjectAccessor::GetPlayers())
        {
            bool const alliance = pl && pl->GetTeamId() == TEAM_ALLIANCE;
            if (pl && pl->IsInWorld() && PlayerbotsMgr::instance().GetPlayerbotAI(pl) &&
                Member(pl, cohort, squad[alliance]))
                members[alliance].push_back(pl);
        }
    }
    std::uint32_t mails = 0;
    std::lock_guard<std::mutex> guard(gLock);
    for (bool const alliance : {true, false})
    {
        std::vector<Player*>& team = members[alliance];
        std::sort(team.begin(), team.end(), [](Player* a, Player* b) { return Low(a) < Low(b); });
        std::vector<Giveable> pool;
        bool pooled = false;
        for (Player* bot : team)
        {
            std::uint32_t const g = Low(bot);
            std::uint32_t due = FloorSlotsOf(bot);
            if (!due)
            {
                if (auto const it = gStates.find(g); it != gStates.end())
                {
                    CancelOrders(it->second, 0);
                    gStates.erase(it);
                }
                continue;
            }
            if (!bot->IsAlive())
                continue;  // no swaps / mail for a corpse: the pass is not counted
            PlayerbotAI* const ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            State& st = gStates[g];
            CancelOrders(st, due);
            Advance(p, st.track, bot->GetLevel());
            bool const errandBot = ErrandBot(bot);
            for (bool more = true; more && due;)
            {
                more = false;
                switch (NextStep(p, st.track, errandBot))
                {
                    case Step::Bag:
                        // one swap per call (positions move): at most one per weapon slot
                        for (std::size_t k = 0; k < kSlots.size() && EquipOwned(bot, ai, due); ++k)
                            due = FloorSlotsOf(bot);
                        st.track.tried |= SrcBag;
                        more = true;
                        break;
                    case Step::HandDown:
                        if (mails >= p.maxMails)
                            break;  // the pass's mail budget is spent: this step waits for the next pass
                        if (!pooled)
                        {
                            for (Player* holder : team)
                                if (!Role(Low(holder)))
                                    for (Giveable const& gv : GiveablesOf(holder))
                                        pool.push_back(gv);
                            pooled = true;
                        }
                        st.track.tried |= SrcHandDown;
                        if (HandDown(bot, ai, due, pool))
                        {
                            // a weapon is in the mail: no smith order until the sources start over (RetryPasses)
                            ++mails;
                            st.track.tried |= SrcSmith;
                            break;
                        }
                        more = true;
                        break;
                    case Step::SmithOrder:
                        Order(bot, due, st);
                        st.track.tried |= SrcSmith;
                        break;
                    case Step::WaitAuction:
                    case Step::Idle:
                        break;
                }
            }
        }
    }
}
}  // namespace AutoWowNoWhite
