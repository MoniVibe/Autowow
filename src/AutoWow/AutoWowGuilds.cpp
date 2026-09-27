/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Guilds runtime (policy: AutoWow/AutoWowGuildsPolicy.h). Guilds, the guild bank and mail are
// thread-unsafe core state, so everything here runs on the world thread (bot login is a world-thread
// PlayerbotOperation; map-thread callers queue one, see QueueTax). The world thread never runs alongside
// the map updates, so touching a player's money here is safe (AutoWowTrade idiom).

#include <algorithm>
#include <array>
#include <memory>

#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "TradePolicy.h"
#include "World.h"
#include "WorldSession.h"

namespace AutoWowGuilds
{
void Levy(Player* p, std::uint32_t guildId, std::uint64_t copper);  // defined below Balance
namespace
{
struct HouseRuntime
{
    House house;
    std::array<std::string, 2> guildName;  // by TeamId (0 alliance, 1 horde); empty = too long, skipped
    std::array<std::uint32_t, 2> rep{};    // configured rep guid-low, 0 = lowest cohort member
    std::array<std::uint32_t, 2> artisan{};  // pinned AutoWow.Supply artisan guid-low, 0 = none
    std::array<std::uint32_t, 2> guildId{};  // resolved guild id (world thread), 0 = not yet
};

// Read-only after LoadConfig except guildId (world thread only). gDefs[i] is gHouses[i].house.
std::vector<House> gDefs;
std::vector<HouseRuntime> gHouses;
std::vector<GuidRange> gCohort;
std::uint32_t gTaxPct = 10;
bool gLevy = false;  // AutoWow.Guilds.Levy

ObjectGuid PlayerGuid(std::uint32_t low) { return ObjectGuid::Create<HighGuid::Player>(low); }
std::uint32_t Low(Player* p) { return static_cast<std::uint32_t>(p->GetGUID().GetCounter()); }
std::size_t TeamIndex(Player* p) { return p->GetTeamId() == TEAM_ALLIANCE ? 0 : 1; }

HouseRuntime* HouseOfGuild(std::uint32_t guildId, std::size_t* team = nullptr)
{
    if (!guildId)
        return nullptr;
    for (HouseRuntime& h : gHouses)
        for (std::size_t t = 0; t < 2; ++t)
            if (h.guildId[t] == guildId)
            {
                if (team)
                    *team = t;
                return &h;
            }
    return nullptr;
}

void Emit(Player* p, Reason r, HouseRuntime const& h, std::uint32_t gid, std::uint64_t copper, char const* op = nullptr)
{
    Guild* g = sGuildMgr->GetGuildById(gid);
    std::uint64_t const bal = g ? g->GetTotalBankMoney() : 0;
    LOG_INFO("playerbots", "[Guilds] player={} {} house={} gid={} copper={} balance={}{}{}", p ? p->GetName() : "-",
             ReasonName(r), h.house.name, gid, copper, bal, op ? " op=" : "", op ? op : "");
    if (p && AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitGuild(p, ReasonName(r), LedgerFields(h.house.name, gid, copper, bal, op));
}

// The house guild of (house, team): cached id, else by name (it survives restarts in the core tables), else
// created with an online leader outside any guild (the rep when online, else `candidate`). 0 = not now.
Guild* EnsureGuild(HouseRuntime& h, std::size_t team, Player* candidate)
{
    if (h.guildName[team].empty())
        return nullptr;
    if (Guild* g = h.guildId[team] ? sGuildMgr->GetGuildById(h.guildId[team]) : nullptr)
        return g;
    if (Guild* g = sGuildMgr->GetGuildByName(h.guildName[team]))
    {
        h.guildId[team] = g->GetId();
        return g;
    }
    Player* leader = h.rep[team] ? ObjectAccessor::FindConnectedPlayer(PlayerGuid(h.rep[team])) : nullptr;
    if (!leader || leader->GetGuildId() || TeamIndex(leader) != team)
        leader = candidate;
    if (!leader || leader->GetGuildId() || TeamIndex(leader) != team)
        return nullptr;  // Guild::Create inserts the guild before AddMember can refuse a guilded leader
    Guild* g = new Guild();
    if (!g->Create(leader, h.guildName[team]))
    {
        LOG_ERROR("playerbots", "[Guilds] cannot create guild '{}' with leader {}", h.guildName[team], leader->GetName());
        delete g;
        return nullptr;
    }
    sGuildMgr->AddGuild(g);
    h.guildId[team] = g->GetId();
    Emit(leader, Reason::Created, h, g->GetId(), 0);
    return g;
}

// Adds guid to g unless already in it; a character in another guild is never moved, except a pinned one out of
// another of our house guilds (MovesToOwnHouse). online may be null.
void Join(HouseRuntime const& h, Guild* g, std::uint32_t guid, Player* online, bool pinned = false)
{
    ObjectGuid const og = PlayerGuid(guid);
    if (g->GetMember(og))
        return;
    std::uint32_t const current = online ? online->GetGuildId() : sCharacterCache->GetCharacterGuildIdByGuid(og);
    HouseRuntime* other = current ? HouseOfGuild(current) : nullptr;
    Guild* otherGuild = other ? sGuildMgr->GetGuildById(current) : nullptr;
    if (otherGuild && MovesToOwnHouse(pinned, true, otherGuild->GetLeaderGUID() == og))
    {
        otherGuild->DeleteMember(og);  // stock removal (an online member's guild id and rank are cleared)
        Emit(online, Reason::Moved, *other, current, 0);
    }
    else if (current)
    {
        Emit(online, Reason::SkipOtherGuild, h, current, 0);
        return;
    }
    if (g->AddMember(og))
        Emit(online, Reason::Joined, h, g->GetId(), 0);
}

// Keeps the core guild leader on RepOf when both are online (HandleSetLeader needs the leader's session).
void SyncLeader(Guild* g)
{
    std::uint32_t const rep = RepOf(g->GetId());
    if (!rep || PlayerGuid(rep) == g->GetLeaderGUID() || !g->GetMember(PlayerGuid(rep)))
        return;
    Player* leader = ObjectAccessor::FindConnectedPlayer(g->GetLeaderGUID());
    Player* repPlayer = ObjectAccessor::FindConnectedPlayer(PlayerGuid(rep));
    if (leader && repPlayer && leader->GetSession())
        g->HandleSetLeader(leader->GetSession(), repPlayer->GetName());
}

bool SendMail(std::uint32_t fromGuid, std::uint32_t toGuid, std::vector<std::uint32_t> const& itemGuids,
              std::uint32_t money, std::string const& subject, char const** refusalOut = nullptr, std::uint32_t cod = 0)
{
    Player* from = ObjectAccessor::FindConnectedPlayer(PlayerGuid(fromGuid));
    CharacterCacheEntry const* to = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(toGuid));
    char const* refusal = nullptr;
    if (!from || !to || fromGuid == toGuid)
        refusal = "no_sender_or_receiver";
    else if (itemGuids.size() > MAX_MAIL_ITEMS || (itemGuids.empty() && !money))
        refusal = "attachments";
    else if (Player::TeamIdForRace(to->Race) != from->GetTeamId())
        refusal = "other_team";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Guilds] mail {} -> {} refused: {}", fromGuid, toGuid, refusal);
        if (refusalOut)
            *refusalOut = refusal;
        return false;
    }
    Player* receiver = ObjectAccessor::FindConnectedPlayer(PlayerGuid(toGuid));
    std::uint32_t const receiverAccount = receiver ? receiver->GetSession()->GetAccountId() : to->AccountId;
    std::uint32_t mails = to->MailCount;
    if (receiver)
    {
        mails = 0;
        for (Mail const* m : receiver->GetMails())  // a deleted mail leaves the box at the receiver's next save
            if (m && m->state != MAIL_STATE_DELETED)
                ++mails;
    }
    if (mails > 100)
        refusal = "receiver_mailbox_full";

    // Same checks as the stock send-mail handler; any bad attachment refuses the whole mail.
    std::vector<Item*> items;
    for (std::size_t i = 0; i < itemGuids.size() && !refusal; ++i)
    {
        Item* item = from->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuids[i]));
        if (!item || item->IsNotEmptyBag() || !item->CanBeTraded(true) ||
            (item->IsBoundAccountWide() && item->IsSoulBound() && from->GetSession()->GetAccountId() != receiverAccount) ||
            item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
            std::find(items.begin(), items.end(), item) != items.end())
            refusal = "bad_item";
        else
            items.push_back(item);
    }

    // The rep's postage is the house's; everyone else pays their own.
    std::uint32_t const postage = Postage(static_cast<std::uint32_t>(items.size()));
    std::uint32_t const houseGuild = HouseGuildOf(from);
    Guild* bank = houseGuild && RepOf(houseGuild) == fromGuid ? sGuildMgr->GetGuildById(houseGuild) : nullptr;
    std::uint64_t const fromCost = std::uint64_t(money) + (bank ? 0 : postage);
    if (!refusal && from->GetMoney() < fromCost)
        refusal = "sender_money";
    if (!refusal && bank)
        Levy(from, houseGuild, postage);
    if (!refusal && bank && !PayAllowed(bank->GetTotalBankMoney(), postage))
        refusal = "bank_postage";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Guilds] mail {} -> {} refused: {}", fromGuid, toGuid, refusal);
        if (refusalOut)
            *refusalOut = refusal;
        if (bank && std::string_view(refusal) == "bank_postage")
            if (HouseRuntime* h = HouseOfGuild(houseGuild))
                Emit(from, Reason::Refused, *h, houseGuild, postage, ReasonName(Reason::Postage));
        return false;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (bank && !bank->ModifyBankMoney(trans, postage, false))
        return false;  // checked above; nothing appended yet that the (dropped) transaction would commit
    if (fromCost)
        from->ModifyMoney(-int32(fromCost));
    MailDraft draft(subject, "");
    for (Item* item : items)
    {
        item->SetNotRefundable(from);
        from->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        item->DeleteFromInventoryDB(trans);
        if (item->GetState() == ITEM_UNCHANGED)
            item->FSetState(ITEM_CHANGED);
        item->SetOwnerGUID(PlayerGuid(toGuid));
        item->SaveToDB(trans);
        draft.AddItem(item);
    }
    std::uint32_t const delay = !items.empty() && from->GetSession()->GetAccountId() != receiverAccount ?
        sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY) : 0;
    draft.AddMoney(money).AddCOD(cod).SendMailTo(trans, MailReceiver(receiver, toGuid), MailSender(from), MAIL_CHECK_MASK_COPIED,
                                     delay);
    from->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    LOG_INFO("playerbots", "[Guilds] mail {} -> {} items={} money={} postage={} postage_from={}", from->GetName(), toGuid,
             items.size(), money, postage, bank ? "guild_bank" : "sender");
    if (bank)
        if (HouseRuntime* h = HouseOfGuild(houseGuild))
            Emit(from, Reason::Postage, *h, houseGuild, postage);
    return true;
}

class TaxOperation : public PlayerbotOperation
{
public:
    TaxOperation(ObjectGuid bot, std::uint64_t copper) : bot_(bot), copper_(copper) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(bot_);
        if (!bot || !HouseGuildOf(bot))
            return false;
        // ponytail: the stop's later repair/buy may have spent the sale money; tax what is left, up to the due.
        std::uint64_t const copper = std::min<std::uint64_t>(copper_, bot->GetMoney());
        if (!copper || !Deposit(bot, copper, Reason::Tax))
            return false;
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitTrade(bot, AutoWowTrade::ActionName(AutoWowTrade::Action::Tax),
                                          AutoWowTrade::LedgerFields(AutoWowTrade::Action::Tax, 0, 0, copper,
                                                                     -std::int64_t(copper), 0));
        return true;
    }
    ObjectGuid GetBotGuid() const override { return bot_; }
    std::string GetName() const override { return "AutoWowGuildTax"; }

private:
    ObjectGuid bot_;
    std::uint64_t copper_;
};
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Guilds.Enable", false);
    gTaxPct = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Guilds.TaxPct", 10));
    gLevy = sConfigMgr->GetOption<bool>("AutoWow.Guilds.Levy", false);
    gDefs.clear();
    gHouses.clear();
    gCohort.clear();
    if (!detail::gEnabled)
        return;

    std::string const cohort = sConfigMgr->GetOption<std::string>("AutoWow.Guilds.CohortGuids", "62955-63004");
    if (!ParseGuidRanges(cohort, gCohort))
        LOG_ERROR("server.loading", "[Guilds] bad AutoWow.Guilds.CohortGuids '{}': no cohort", cohort);
    std::string const housesText = sConfigMgr->GetOption<std::string>(
        "AutoWow.Guilds.Houses", "Weavers:197,333;Smiths:186,164,202;Tanners:393,165;Brewers:182,171");
    std::vector<House> houses;
    if (!ParseHouses(housesText, houses))
        LOG_ERROR("server.loading", "[Guilds] bad AutoWow.Guilds.Houses '{}': no houses", housesText);
    std::string const pattern = sConfigMgr->GetOption<std::string>("AutoWow.Guilds.NamePattern", "{house} of the {team}");
    gDefs = houses;
    for (House& house : houses)
    {
        HouseRuntime h;
        for (std::size_t t = 0; t < 2; ++t)
        {
            h.guildName[t] = GuildName(pattern, house.name, t == 0);
            if (h.guildName[t].empty())
                LOG_ERROR("server.loading", "[Guilds] house {}: guild name from '{}' is empty or over {} characters",
                          house.name, pattern, kMaxGuildNameLength);
            h.rep[t] = sConfigMgr->GetOption<std::uint32_t>(
                "AutoWow.Guilds.Rep." + house.name + (t == 0 ? ".Alliance" : ".Horde"), 0, false);
        }
        h.house = std::move(house);
        gHouses.push_back(std::move(h));
    }
    LOG_INFO("server.loading", "[Guilds] enabled: {} houses, cohort ranges {}, tax {}%", gHouses.size(), gCohort.size(),
             gTaxPct);
}

void OnLogin(Player* player)
{
    if (!player || gHouses.empty())
        return;
    std::uint32_t const guid = Low(player);
    std::size_t const team = TeamIndex(player);
    HouseRuntime* house = nullptr;
    for (HouseRuntime& h : gHouses)
        if (h.rep[team] == guid || h.artisan[team] == guid)
            house = &h;  // a configured rep / artisan joins its own house, whatever its professions
    bool const pinned = house != nullptr;
    if (!house && InRanges(gCohort, guid))
        house = &gHouses[HouseFor(gDefs, [player](std::uint32_t skill) { return player->HasSkill(skill); })];
    if (!house)
        return;
    Guild* g = EnsureGuild(*house, team, player);
    if (!g)
    {
        if (player->GetGuildId() && player->GetGuildId() != house->guildId[team])
            Emit(player, Reason::SkipOtherGuild, *house, player->GetGuildId(), 0);
        return;
    }
    Join(*house, g, guid, player, pinned);
    if (house->rep[team] && house->rep[team] != guid)
    {
        // The configured rep may be offline or not a bot: AddMember takes an offline character.
        CharacterCacheEntry const* rep = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(house->rep[team]));
        if (rep && Player::TeamIdForRace(rep->Race) == player->GetTeamId())
            Join(*house, g, house->rep[team], ObjectAccessor::FindConnectedPlayer(PlayerGuid(house->rep[team])));
    }
    SyncLeader(g);
}

std::uint32_t HouseGuildOf(Player* player)
{
    std::uint32_t const gid = player ? player->GetGuildId() : 0;
    return HouseOfGuild(gid) ? gid : 0;
}

std::string HouseNameOf(std::uint32_t guildId)
{
    HouseRuntime const* h = HouseOfGuild(guildId);
    return h ? h->house.name : std::string();
}

std::uint32_t RepOf(std::uint32_t guildId)
{
    std::size_t team = 0;
    HouseRuntime const* h = HouseOfGuild(guildId, &team);
    Guild* g = h ? sGuildMgr->GetGuildById(guildId) : nullptr;
    if (!g)
        return 0;
    if (h->rep[team])
        return h->rep[team];
    std::uint32_t best = 0;  // lowest over all ranges (config order is free)
    for (GuidRange const& r : gCohort)
        for (std::uint32_t guid = r.lo; guid <= r.hi && (!best || guid < best); ++guid)
            if (g->GetMember(PlayerGuid(guid)))
            {
                best = guid;
                break;
            }
    return best;
}

// AutoWow.Guilds.Levy: a house bank short of the copper draws the shortfall from the richest other house bank
// of the same team (a real bank-to-bank transfer; soak-s40-full-r1: the Weavers bank that funds the
// faction's bags held 57 copper while other houses held up to 998, and 56 feed mails were refused).
void Levy(Player* p, std::uint32_t guildId, std::uint64_t copper)
{
    std::size_t team = 0;
    HouseRuntime* h = HouseOfGuild(guildId, &team);
    Guild* g = h ? sGuildMgr->GetGuildById(guildId) : nullptr;
    if (!gLevy || !g || g->GetTotalBankMoney() >= copper)
        return;
    std::uint64_t const need = copper - g->GetTotalBankMoney();
    Guild* rich = nullptr;
    HouseRuntime* richHouse = nullptr;
    for (HouseRuntime& o : gHouses)
        if (Guild* og = o.guildId[team] && o.guildId[team] != guildId ? sGuildMgr->GetGuildById(o.guildId[team]) : nullptr)
            if (og->GetTotalBankMoney() >= need && (!rich || og->GetTotalBankMoney() > rich->GetTotalBankMoney()))
            {
                rich = og;
                richHouse = &o;
            }
    if (!rich)
        return;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (!rich->ModifyBankMoney(trans, need, false) || !g->ModifyBankMoney(trans, need, true))
        return;
    CharacterDatabase.CommitTransaction(trans);
    Emit(p, Reason::Levy, *richHouse, rich->GetId(), need);
    Emit(p, Reason::Levy, *h, guildId, need);
}

std::uint64_t Balance(std::uint32_t guildId)
{
    Guild* g = HouseOfGuild(guildId) ? sGuildMgr->GetGuildById(guildId) : nullptr;
    return g ? g->GetTotalBankMoney() : 0;
}

bool Deposit(Player* from, std::uint64_t copper, Reason reason)
{
    std::uint32_t const gid = HouseGuildOf(from);
    HouseRuntime* h = HouseOfGuild(gid);
    Guild* g = h ? sGuildMgr->GetGuildById(gid) : nullptr;
    if (!g)
        return false;
    std::uint64_t const before = g->GetTotalBankMoney();
    if (!copper || copper > from->GetMoney() || copper > 0x7FFFFFFFu)
    {
        Emit(from, Reason::Refused, *h, gid, copper, ReasonName(reason));
        return false;
    }
    // Stock deposit: bank money, player money, gold save and bank log in one transaction.
    g->HandleMemberDepositMoney(from->GetSession(), static_cast<std::uint32_t>(copper));
    bool const ok = g->GetTotalBankMoney() == before + copper;
    Emit(from, ok ? reason : Reason::Refused, *h, gid, copper, ok ? nullptr : ReasonName(reason));
    return ok;
}

bool Pay(std::uint32_t guildId, Player* to, std::uint64_t copper, Reason reason)
{
    HouseRuntime* h = HouseOfGuild(guildId);
    Guild* g = h ? sGuildMgr->GetGuildById(guildId) : nullptr;
    if (!g || !to)
        return false;
    Levy(to, guildId, copper);
    if (!PayAllowed(g->GetTotalBankMoney(), copper) || copper > 0x7FFFFFFFu ||
        to->GetMoney() + copper > MAX_MONEY_AMOUNT)
    {
        Emit(to, Reason::Refused, *h, guildId, copper, ReasonName(reason));
        return false;
    }
    // ponytail: straight bank -> player (HandleMemberWithdrawMoney would apply the payee's rank daily limit);
    // no guild bank log row, the ledger carries it.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (!g->ModifyBankMoney(trans, copper, false))
        return false;
    to->ModifyMoney(static_cast<std::int32_t>(copper));
    to->SaveGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    Emit(to, reason, *h, guildId, copper);
    return true;
}

void QueueTax(Player* bot, std::uint64_t sold)
{
    if (!detail::gEnabled || !bot || !bot->GetGuildId())
        return;
    std::uint64_t const tax = TaxCopper(sold, gTaxPct);
    if (tax)
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<TaxOperation>(bot->GetGUID(), tax));
}

std::uint32_t RepFreeSlots(Player* rep)
{
    if (!rep)
        return 0;
    std::uint32_t free = rep->GetFreeInventorySpace();
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        if (!rep->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
        if (Bag* bag = rep->GetBagByPos(slot))
            free += bag->GetFreeSlots();
    return free;
}

bool SendMoney(std::uint32_t fromGuid, std::uint32_t toGuid, std::uint32_t copper, std::string const& subject)
{
    return SendMail(fromGuid, toGuid, {}, copper, subject);
}

bool SendItems(std::uint32_t fromGuid, std::uint32_t toGuid, std::vector<std::uint32_t> const& itemGuids,
               std::string const& subject, char const** refusal)
{
    return SendMail(fromGuid, toGuid, itemGuids, 0, subject, refusal);
}

bool SendItemsCod(std::uint32_t fromGuid, std::uint32_t toGuid, std::vector<std::uint32_t> const& itemGuids,
                  std::uint32_t cod, std::string const& subject, char const** refusal)
{
    if (itemGuids.empty())
        return false;  // a COD mail carries items (the core client rule)
    return SendMail(fromGuid, toGuid, itemGuids, 0, subject, refusal, cod);
}

std::vector<House> const& Houses() { return gDefs; }

std::uint32_t ConfiguredRep(std::size_t house, bool alliance)
{
    return house < gHouses.size() ? gHouses[house].rep[alliance ? 0 : 1] : 0;
}

std::vector<GuidRange> const& Cohort() { return gCohort; }

void PinArtisan(std::size_t house, bool alliance, std::uint32_t guid)
{
    if (house < gHouses.size())
        gHouses[house].artisan[alliance ? 0 : 1] = guid;
}

std::uint32_t HouseGuildId(std::size_t house, bool alliance)
{
    if (house >= gHouses.size())
        return 0;
    HouseRuntime& h = gHouses[house];
    std::size_t const team = alliance ? 0 : 1;
    if (!h.guildId[team] && !h.guildName[team].empty())
        if (Guild* g = sGuildMgr->GetGuildByName(h.guildName[team]))
            h.guildId[team] = g->GetId();
    return h.guildId[team];
}
}  // namespace AutoWowGuilds
