/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TRADE_POLICY_H
#define AUTOWOW_TRADE_POLICY_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Auction-house trade for independent AutoWoW bots (AutoWow.Trade.Enable, default 0; needs
// AutoWow.Errands.Enable). A town run whose town has an auctioneer adds an auction stop, and one with a
// mailbox adds a mail stop when the bot has mail to collect (ErrandsPolicy PlanStops, after every other
// stop). At the auctioneer the bot posts tradeables it does not need (stock item usage `ah`: quality
// white or better, not soulbound / bind-on-pickup, not needed for equipment, quests or its own skills;
// only greens and better, or trade goods / reagents / gems) at a price below the lowest competing
// buyout (never under FloorPct of the vendor price), else PriceMultPct of the vendor price; then buys at
// most one equippable upgrade (stock item usage equip/replace, the stat-weight check of quest rewards)
// and profession mats it lacks (stock item usage `skill`) with buyout, inside a budget. At the mailbox it
// takes the money and items of every delivered, non-COD mail (auction sales, expiries, won auctions).
// Real gold only: the core auction / mail handlers run in the world thread (PlayerbotWorldThreadProcessor)
// and charge deposits, cuts and prices themselves. Each result emits ledger `trade` (event 17).
//
// AutoWow.Ledger.Treasury (default 0; needs AutoWow.Ledger.Enable) adds `trade` action `fee` lines for
// copper bots pay to the world (flights, repairs, trainers); the reducer sums them with auction fees
// into a per-faction treasury counter (stage 1: telemetry, no gameplay effect).
//
// Value-only below the runtime section: integer copper, no floats in decisions, no RNG, stable orders
// (item entry, then item guid / auction id ascending).
namespace AutoWowTrade
{
// ---- ledger wire names (append only) ----------------------------------------------------------------
enum class Action : std::uint8_t
{
    Post = 0,     // listed a stack; gold = -deposit
    Buy = 1,      // bought out an auction; gold = -price (the item arrives by mail)
    Sold = 2,     // took a sale's money; price = winning bid, gold = bid + deposit - cut
    Expired = 3,  // took back an expired / cancelled listing's item
    Mail = 4,     // took any other mail (won auction item, outbid refund, ...)
    Fee = 5,      // AutoWow.Ledger.Treasury: copper paid to the world (kind)
    Tax = 6       // AutoWow.Guilds: vendor-income tax paid into the house guild bank; price = copper, gold = -copper
};

inline constexpr char const* ActionName(Action a)
{
    switch (a)
    {
        case Action::Post: return "post";
        case Action::Buy: return "buy";
        case Action::Sold: return "sold";
        case Action::Expired: return "expired";
        case Action::Mail: return "mail";
        case Action::Fee: return "fee";
        case Action::Tax: return "tax";
    }
    return "mail";
}

enum class FeeKind : std::uint8_t
{
    Flight = 0,
    Repair = 1,
    Train = 2
};

inline constexpr char const* FeeKindName(FeeKind k)
{
    switch (k)
    {
        case FeeKind::Flight: return "flight";
        case FeeKind::Repair: return "repair";
        case FeeKind::Train: return "train";
    }
    return "flight";
}

// Core item constants (ItemTemplate.h), mirrored for the value-only policy.
inline constexpr std::uint32_t kQualityUncommon = 2;
inline constexpr std::uint32_t kClassReagent = 5, kClassTradeGoods = 7, kClassGem = 3;

struct Params
{
    std::uint32_t priceMultPct = 300;    // AutoWow.Trade.PriceMultPct: no competing listing -> vendor price * this
    std::uint32_t floorPct = 150;        // AutoWow.Trade.FloorPct: never list below vendor price * this
    std::uint32_t undercutCopper = 1;    // per unit below the lowest competing buyout
    std::uint32_t bidPct = 90;           // starting bid = buyout * this
    std::uint32_t maxPosts = 6;          // AutoWow.Trade.MaxPosts per visit
    std::uint32_t depositBudgetPct = 50; // deposits of one visit <= money * this
    std::uint32_t buyBudgetPct = 50;     // AutoWow.Trade.BuyBudgetPct: purchases <= (money - reserve) * this
    std::uint32_t matUnitMaxPct = 400;   // AutoWow.Trade.MatUnitMaxPct: a mat unit costs <= vendor price * this
    std::uint32_t matCountMax = 20;      // units of one mat bought per visit
    std::uint32_t durationMin = 720;     // listing time (minutes; the core accepts 720 / 1440 / 2880)
};

// ---- posting ----------------------------------------------------------------------------------------
// A bag stack the bot may list. usageAh = stock item usage says `ah` (tradeable and not needed).
struct Holding
{
    std::uint32_t entry = 0;
    std::uint32_t guid = 0;       // item guid low: stable order within an entry
    std::uint32_t count = 0;
    std::uint32_t quality = 0;
    std::uint32_t itemClass = 0;
    std::uint32_t sellPrice = 0;  // vendor price per unit (copper)
    bool usageAh = false;
    std::uint32_t lowestOther = 0;  // lowest competing buyout per unit on this auction house (0 = none)
    std::uint32_t deposit = 0;      // core deposit for this stack and duration
};

[[nodiscard]] inline bool Postable(Holding const& h)
{
    if (!h.usageAh || !h.count || !h.sellPrice)
        return false;
    return h.quality >= kQualityUncommon || h.itemClass == kClassTradeGoods || h.itemClass == kClassReagent ||
           h.itemClass == kClassGem;
}

// Buyout per unit: undercut the lowest competing buyout, else vendor * PriceMultPct. 0 = do not list
// (the competition sits under the floor: vendoring pays better).
[[nodiscard]] inline std::uint64_t UnitPrice(Params const& p, std::uint32_t sellPrice, std::uint32_t lowestOther)
{
    std::uint64_t const floor = (std::uint64_t(sellPrice) * p.floorPct + 99) / 100;
    std::uint64_t unit = std::uint64_t(sellPrice) * p.priceMultPct / 100;
    if (lowestOther)
        unit = lowestOther > p.undercutCopper ? lowestOther - p.undercutCopper : 0;
    return unit >= floor && unit ? unit : 0;
}

struct Post
{
    std::uint32_t entry = 0;
    std::uint32_t guid = 0;
    std::uint32_t count = 0;
    std::uint32_t bid = 0;
    std::uint32_t buyout = 0;
    std::uint32_t deposit = 0;
};

inline constexpr std::uint64_t kMaxMoney = 0x7FFFFFFF;  // core MAX_MONEY_AMOUNT (int32 max)

// Listings for one visit: postable stacks by (entry, guid), priced, while the deposits fit the budget;
// at most MaxPosts.
[[nodiscard]] inline std::vector<Post> PlanPosts(Params const& p, std::vector<Holding> holdings, std::uint64_t money)
{
    std::sort(holdings.begin(), holdings.end(), [](Holding const& a, Holding const& b)
              { return a.entry != b.entry ? a.entry < b.entry : a.guid < b.guid; });
    std::uint64_t budget = money * p.depositBudgetPct / 100;
    std::vector<Post> out;
    for (Holding const& h : holdings)
    {
        if (out.size() >= p.maxPosts)
            break;
        if (!Postable(h) || h.deposit > budget)
            continue;
        std::uint64_t const buyout = UnitPrice(p, h.sellPrice, h.lowestOther) * h.count;
        if (!buyout || buyout > kMaxMoney)
            continue;
        std::uint64_t const bid = std::max<std::uint64_t>(1, buyout * p.bidPct / 100);
        budget -= h.deposit;
        out.push_back({h.entry, h.guid, h.count, static_cast<std::uint32_t>(bid), static_cast<std::uint32_t>(buyout),
                       h.deposit});
    }
    return out;
}

// ---- buying -----------------------------------------------------------------------------------------
enum class Want : std::uint8_t
{
    None = 0,
    Upgrade = 1,  // stock item usage equip / replace
    Mat = 2       // stock item usage skill (a profession reagent the bot lacks)
};

struct Listing
{
    std::uint32_t id = 0;         // auction id: stable, never reused
    std::uint32_t entry = 0;
    std::uint32_t count = 0;
    std::uint32_t buyout = 0;     // 0 = bid only (never bought)
    std::uint32_t sellPrice = 0;  // vendor price per unit
    Want want = Want::None;
};

struct Buy
{
    std::uint32_t id = 0;
    std::uint32_t entry = 0;
    std::uint32_t count = 0;
    std::uint32_t price = 0;
    Want want = Want::None;
};

// Copper one visit may spend on purchases: BuyBudgetPct of the money above the reserve.
[[nodiscard]] inline std::uint64_t BuyBudget(Params const& p, std::uint64_t money, std::uint64_t reserve)
{
    return money > reserve ? (money - reserve) * p.buyBudgetPct / 100 : 0;
}

// Purchases for one visit, within `budget`: the cheapest affordable upgrade (ties: lower auction id),
// then per mat entry (ascending) the cheapest units (per-unit price, then id) up to MatCountMax, never
// above vendor * MatUnitMaxPct per unit. Own listings are excluded by the caller.
[[nodiscard]] inline std::vector<Buy> PlanBuys(Params const& p, std::vector<Listing> listings, std::uint64_t budget)
{
    std::vector<Buy> out;
    Listing const* upgrade = nullptr;
    for (Listing const& l : listings)
        if (l.want == Want::Upgrade && l.buyout && l.buyout <= budget &&
            (!upgrade || l.buyout < upgrade->buyout || (l.buyout == upgrade->buyout && l.id < upgrade->id)))
            upgrade = &l;
    if (upgrade)
    {
        budget -= upgrade->buyout;
        out.push_back({upgrade->id, upgrade->entry, upgrade->count, upgrade->buyout, Want::Upgrade});
    }
    // Per-unit order without floats: a/ac < b/bc  <=>  a*bc < b*ac.
    std::sort(listings.begin(), listings.end(), [](Listing const& a, Listing const& b)
              {
                  if (a.entry != b.entry)
                      return a.entry < b.entry;
                  std::uint64_t const l = std::uint64_t(a.buyout) * std::max<std::uint32_t>(1, b.count);
                  std::uint64_t const r = std::uint64_t(b.buyout) * std::max<std::uint32_t>(1, a.count);
                  return l != r ? l < r : a.id < b.id;
              });
    std::uint32_t entry = 0, units = 0;
    for (Listing const& l : listings)
    {
        if (l.want != Want::Mat || !l.buyout || !l.count || !l.sellPrice)
            continue;
        if (l.entry != entry)
        {
            entry = l.entry;
            units = 0;
        }
        std::uint64_t const cap = std::uint64_t(l.sellPrice) * p.matUnitMaxPct / 100 * l.count;
        if (l.buyout > cap || l.buyout > budget || units + l.count > p.matCountMax)
            continue;
        budget -= l.buyout;
        units += l.count;
        out.push_back({l.id, l.entry, l.count, l.buyout, Want::Mat});
    }
    return out;
}

// ---- mail -------------------------------------------------------------------------------------------
// Core MailAuctionAnswers (AuctionHouseMgr.h).
inline constexpr std::uint32_t kAuctionOutbid = 0, kAuctionWon = 1, kAuctionSuccessful = 2, kAuctionExpired = 3,
                               kAuctionCancelledToBidder = 4, kAuctionCanceled = 5, kAuctionSalePending = 6;

struct AuctionMail
{
    bool ok = false;
    std::uint32_t entry = 0;
    std::uint32_t response = 0;
    std::uint32_t auctionId = 0;
    std::uint32_t count = 0;
};

namespace detail
{
// Colon-separated unsigned fields of `s` (decimal); false on anything else.
inline bool SplitU32(std::string_view s, std::uint32_t* out, std::size_t n)
{
    std::size_t k = 0;
    std::uint64_t v = 0;
    bool digit = false;
    for (std::size_t i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ':')
        {
            if (!digit || k >= n)
                return false;
            out[k++] = static_cast<std::uint32_t>(v);
            v = 0;
            digit = false;
            continue;
        }
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + std::uint64_t(s[i] - '0');
        if (v > 0xFFFFFFFFull)
            return false;
        digit = true;
    }
    return k == n;
}
}  // namespace detail

// Subject of an auction-house mail: "entry:0:response:auctionId:count" (AuctionEntry::BuildAuctionMailSubject).
[[nodiscard]] inline AuctionMail ParseAuctionSubject(std::string_view subject)
{
    AuctionMail m;
    std::uint32_t f[5] = {};
    if (!detail::SplitU32(subject, f, 5) || f[1] != 0)
        return m;
    m.ok = true;
    m.entry = f[0];
    m.response = f[2];
    m.auctionId = f[3];
    m.count = f[4];
    return m;
}

// Winning bid of a sale mail body: "<16 hex guid>:bid:buyout:deposit:cut:delay:eta"
// (AuctionEntry::BuildAuctionMailBody). 0 = unparsable.
[[nodiscard]] inline std::uint32_t ParseSaleBid(std::string_view body)
{
    std::size_t const colon = body.find(':');
    if (colon == std::string_view::npos)
        return 0;
    std::uint32_t f[6] = {};
    return detail::SplitU32(body.substr(colon + 1), f, 6) ? f[0] : 0;
}

// Ledger action of a collected mail.
[[nodiscard]] inline Action MailAction(AuctionMail const& m)
{
    if (!m.ok)
        return Action::Mail;
    if (m.response == kAuctionSuccessful)
        return Action::Sold;
    if (m.response == kAuctionExpired || m.response == kAuctionCanceled)
        return Action::Expired;
    return Action::Mail;
}

// Trailing fields of the ledger `trade` line (AutoWowQuestLedger.h documents them). gold = signed
// copper change of the bot's money; ah = auction id (0 = unknown); kind only on `fee`.
inline std::string LedgerFields(Action a, std::uint32_t item, std::uint32_t count, std::uint64_t price,
                                std::int64_t gold, std::uint32_t ah, char const* kind = nullptr)
{
    std::string out = ",\"action\":\"";
    out += ActionName(a);
    out += "\",\"item\":" + std::to_string(item) + ",\"count\":" + std::to_string(count) +
           ",\"price\":" + std::to_string(price) + ",\"gold\":" + std::to_string(gold) +
           ",\"ah\":" + std::to_string(ah);
    if (kind)
    {
        out += ",\"kind\":\"";
        out += kind;
        out += "\"";
    }
    return out;
}

// ---- runtime (AutoWowTrade.cpp) ---------------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;
inline bool gTreasury = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }
inline bool TreasuryEnabled() { return detail::gTreasury; }

void LoadConfig();
}  // namespace AutoWowTrade

// World types for the runtime entry points (AutoWowTrade.cpp); kept out of the value-only section.
class Creature;
class GameObject;
class Player;
class PlayerbotAI;

namespace AutoWowTrade
{
// The bot has a delivered, non-COD mail with money or items.
bool HasCollectableMail(Player* bot);
// At the auctioneer (map thread): plan posts and buys (ErrandsPolicy reserve: `reserve` copper kept),
// queue them for the world thread.
void VisitAuctioneer(PlayerbotAI* botAI, Player* bot, Creature* auctioneer, std::uint64_t reserve);
// At the mailbox (map thread): queue the collection for the world thread.
void VisitMailbox(Player* bot, GameObject* mailbox);
// AutoWow.Ledger.Treasury: `fee` line for copper the bot just paid (no-op when off or copper is 0).
void NoteFee(Player* bot, FeeKind kind, std::uint64_t copper);
}  // namespace AutoWowTrade

#endif  // AUTOWOW_TRADE_POLICY_H
