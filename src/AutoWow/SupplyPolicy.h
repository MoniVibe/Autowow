/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SUPPLYPOLICY_H
#define _PLAYERBOT_SUPPLYPOLICY_H

// AutoWow.Supply (docs/SUPPLY_CHAIN_PLAN.md lane B): the bag production chain. Adventurers route cloth to the
// bag house's rep, the rep feeds the house artisan (a tailor who stays home and crafts), the overlord (no body,
// one per team) sizes bag orders from the cohort's empty bag slots, the treasury pays the artisan per bag and
// the rep mails bags to the members with the most empty slots. Pure rules here (unit-tested); runtime in
// AutoWowSupply.cpp (world thread) and NewRpgSupply.cpp (the role bots' map-thread step). No free gold or
// items: every item and copper moves by mail, vendor, trainer, auction house or the guild bank.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "AutoWowGuildsPolicy.h"

class Player;

namespace AutoWowSupply
{
inline constexpr std::uint32_t kStateVersion = 1;  // RoleState / TeamState layout; bump on change

// Cloth routed to the bag house (item entries): linen, wool, silk. Only linen feeds the V1 recipe chain;
// wool and silk are stored for the next bags.
inline constexpr std::uint32_t kLinen = 2589, kWool = 2592, kSilk = 4306;
inline constexpr std::uint32_t kCloth[] = {kLinen, kWool, kSilk};
inline constexpr std::uint32_t kMaxMailStacks = 12;  // core MAX_MAIL_ITEMS

struct Params
{
    std::uint32_t tickMs = 30000;       // AutoWow.Supply.TickMs: role step and world ops stride
    std::uint32_t overlordMs = 300000;  // AutoWow.Supply.OverlordMs: need scan / order stride
    bool routeCloth = false;            // AutoWow.Supply.RouteCloth
    std::uint32_t clothCap = 200;       // AutoWow.Supply.ClothCap: house cloth stock above which routing stops
    std::uint32_t maxOrder = 6;         // AutoWow.Supply.MaxOrder: bags per order
    std::uint32_t bagPayPct = 200;      // AutoWow.Supply.BagPayPct: pay per bag = vendor sell value * this / 100
    std::uint32_t workXpPerItem = 0;    // AutoWow.Supply.WorkXpPerItem: 0 = XpForLevel(level) / kXpDivisor
    std::uint32_t repXpPerDeal = 0;     // AutoWow.Supply.RepXpPerDeal: 0 = the same rule
    std::uint32_t surplusKeep = 4;      // AutoWow.Supply.SurplusKeep: bags the rep keeps with no open need
    std::uint32_t homeYards = 25;       // AutoWow.Supply.HomeYards: "at home" radius
    std::uint32_t skillupCloth = 40;    // AutoWow.Supply.SkillupCloth: linen kept at an artisan still below
                                        // the bag recipe (bolts level the skill and are the bag's reagent)
};

// ---- bag need (overlord) ----

// One online cohort member of the team. empty = equipped bag slots 19-22 without a bag; smaller = equipped
// general bags with fewer slots than the product bag; incoming = unequipped general bags in its inventory
// or its mailbox (already delivered, not yet worn).
struct Member
{
    std::uint32_t guid = 0;
    std::uint32_t empty = 0;
    std::uint32_t smaller = 0;
    std::uint32_t incoming = 0;
};

[[nodiscard]] inline std::uint32_t Wants(Member const& m)
{
    std::uint32_t const want = m.empty + m.smaller;
    return want > m.incoming ? want - m.incoming : 0;
}

// Members that want a bag, most empty slots first, then most smaller bags, ties the lower guid.
[[nodiscard]] inline std::vector<Member> RankNeeds(std::vector<Member> members)
{
    members.erase(std::remove_if(members.begin(), members.end(), [](Member const& m) { return !Wants(m); }),
                  members.end());
    std::sort(members.begin(), members.end(), [](Member const& a, Member const& b)
              {
                  if (a.empty != b.empty)
                      return a.empty > b.empty;
                  if (a.smaller != b.smaller)
                      return a.smaller > b.smaller;
                  return a.guid < b.guid;
              });
    return members;
}

[[nodiscard]] inline std::uint32_t TotalWant(std::vector<Member> const& ranked)
{
    std::uint32_t n = 0;
    for (Member const& m : ranked)
        n += Wants(m);
    return n;
}

// The two-step chain: bolt = clothPerBolt linen; bag = boltsPerBag bolts + threadPerBag thread (read from the
// recipe spells at load; these are the 3.3.5 values).
struct Recipe
{
    std::uint32_t clothPerBolt = 2;
    std::uint32_t boltsPerBag = 3;
    std::uint32_t threadPerBag = 3;
};

// Bags the linen and bolts can make.
[[nodiscard]] inline std::uint32_t BagsFrom(Recipe const& r, std::uint32_t linen, std::uint32_t bolts)
{
    if (!r.clothPerBolt || !r.boltsPerBag)
        return 0;
    return (linen / r.clothPerBolt + bolts) / r.boltsPerBag;
}

// Order size: the want not already covered by finished bags (rep + artisan), within what the house's
// materials can make and MaxOrder.
[[nodiscard]] inline std::uint32_t OrderSize(std::uint32_t want, std::uint32_t finished, std::uint32_t craftable,
                                             std::uint32_t maxOrder)
{
    std::uint32_t const open = want > finished ? want - finished : 0;
    return std::min({open, craftable, maxOrder});
}

// ---- rep -> artisan feed ----

// Linen the artisan still needs for `remaining` bags beyond what it holds (linen, bolts, finished bags).
// Below the bag recipe (canBag false) it is the skill-up stock instead: SkillupCloth minus its linen.
[[nodiscard]] inline std::uint32_t LinenShort(Recipe const& r, std::uint32_t remaining, bool canBag,
                                              std::uint32_t linen, std::uint32_t bolts, std::uint32_t bags,
                                              std::uint32_t skillupCloth)
{
    if (!canBag)
        return skillupCloth > linen ? skillupCloth - linen : 0;
    std::uint32_t const toMake = remaining > bags ? remaining - bags : 0;
    std::uint64_t const need = std::uint64_t(toMake) * r.boltsPerBag * r.clothPerBolt;
    std::uint64_t const have = std::uint64_t(bolts) * r.clothPerBolt + linen;
    return need > have ? static_cast<std::uint32_t>(need - have) : 0;
}

struct Stack
{
    std::uint32_t guid = 0;  // item guid low
    std::uint32_t count = 0;
};

// Whole stacks (ascending guid) until `units` are covered, at most kMaxMailStacks. Empty when units is 0.
[[nodiscard]] inline std::vector<std::uint32_t> PickStacks(std::vector<Stack> stacks, std::uint32_t units)
{
    std::sort(stacks.begin(), stacks.end(), [](Stack const& a, Stack const& b) { return a.guid < b.guid; });
    std::vector<std::uint32_t> out;
    std::uint64_t got = 0;
    for (Stack const& s : stacks)
    {
        if (got >= units || out.size() >= kMaxMailStacks)
            break;
        out.push_back(s.guid);
        got += s.count;
    }
    return out;
}

// Copper the artisan lacks for the thread of `remaining` bags (at the vendor price) plus `extra` (a due
// trainer rank or recipe), beyond its purse.
[[nodiscard]] inline std::uint64_t CopperShort(Recipe const& r, std::uint32_t remaining, std::uint32_t thread,
                                               std::uint32_t threadPrice, std::uint64_t extra, std::uint64_t purse)
{
    std::uint64_t const needThread = std::uint64_t(remaining) * r.threadPerBag;
    std::uint64_t const want = (needThread > thread ? needThread - thread : 0) * threadPrice + extra;
    return want > purse ? want - purse : 0;
}

// ---- artisan craft ----

enum class Craft : std::uint8_t
{
    None = 0,
    Bolt = 1,
    Bag = 2
};

// Next cast: a bag while the order wants more than the finished ones and the reagents are in hand; else a
// bolt from spare linen (below the bag recipe every bolt is a skill-up and a future reagent; with the recipe
// only the bolts the order still lacks).
[[nodiscard]] inline Craft NextCraft(Recipe const& r, std::uint32_t remaining, bool canBag, std::uint32_t linen,
                                     std::uint32_t bolts, std::uint32_t thread, std::uint32_t bags)
{
    std::uint32_t const toMake = remaining > bags ? remaining - bags : 0;
    if (canBag && toMake && bolts >= r.boltsPerBag && thread >= r.threadPerBag)
        return Craft::Bag;
    if (linen < r.clothPerBolt)
        return Craft::None;
    if (!canBag)
        return Craft::Bolt;
    return std::uint64_t(bolts) < std::uint64_t(toMake) * r.boltsPerBag ? Craft::Bolt : Craft::None;
}

// ---- routing (adventurer sell stop) ----

// Cloth a donor mails: a cohort non-tailor, routing on, and house room left (ClothCap minus the house stock).
[[nodiscard]] inline bool RoutesCloth(bool routeCloth, bool inCohort, bool tailor, std::uint32_t room)
{
    return routeCloth && inCohort && !tailor && room > 0;
}

[[nodiscard]] inline std::uint32_t ClothRoom(std::uint32_t stock, std::uint32_t cap, bool repReady)
{
    return repReady && cap > stock ? cap - stock : 0;
}

// ---- delivery, pay, XP ----

struct Delivery
{
    std::uint32_t guid = 0;
    std::uint32_t bags = 0;
};

// The rep's bags to the ranked members in order, each up to its want.
[[nodiscard]] inline std::vector<Delivery> PlanDeliveries(std::vector<Member> const& ranked, std::uint32_t bags)
{
    std::vector<Delivery> out;
    for (Member const& m : ranked)
    {
        if (!bags)
            break;
        std::uint32_t const n = std::min(std::min(Wants(m), bags), kMaxMailStacks);
        if (!n)
            continue;
        out.push_back({m.guid, n});
        bags -= n;
    }
    return out;
}

// Treasury pay for delivered bags: vendor sell value * pct / 100 each.
[[nodiscard]] inline std::uint64_t BagPay(std::uint32_t sellPrice, std::uint32_t pct, std::uint32_t count)
{
    return std::uint64_t(sellPrice) * pct / 100 * count;
}

inline constexpr std::uint32_t kXpDivisor = 20;  // auto work XP: about one level per 20 items at that level

// XP per item (artisan) or per deal (rep): the configured value, else XpForLevel(level) / kXpDivisor (min 1).
[[nodiscard]] inline std::uint32_t WorkXp(std::uint32_t configured, std::uint32_t xpForLevel)
{
    if (configured)
        return configured;
    return std::max<std::uint32_t>(1, xpForLevel / kXpDivisor);
}

// Bags the rep sells with no open need, keeping SurplusKeep.
[[nodiscard]] inline std::uint32_t Surplus(std::uint32_t openWant, std::uint32_t repBags, std::uint32_t keep)
{
    return !openWant && repBags > keep ? repBags - keep : 0;
}

// ---- config parsing ----

// "map,x,y,z" (integers, yards). False (out untouched) on anything else.
struct Home
{
    std::uint32_t map = 0;
    std::int32_t x = 0, y = 0, z = 0;
    bool set = false;
};

inline bool ParseInt(std::string_view s, std::int32_t& out)
{
    bool const neg = !s.empty() && s.front() == '-';
    if (neg)
        s.remove_prefix(1);
    std::uint32_t v = 0;
    if (!AutoWowGuilds::detail::ParseU32(s, v) || v > 0x7FFFFFFFu)
        return false;
    out = neg ? -static_cast<std::int32_t>(v) : static_cast<std::int32_t>(v);
    return true;
}

inline bool ParseHome(std::string_view text, Home& out)
{
    std::vector<std::int32_t> v;
    bool ok = true;
    AutoWowGuilds::detail::Split(text, ',', [&](std::string_view token) {
        std::int32_t x = 0;
        ok = ok && ParseInt(token, x);
        v.push_back(x);
    });
    if (!ok || v.size() != 4 || v[0] < 0)
        return false;
    out = {static_cast<std::uint32_t>(v[0]), v[1], v[2], v[3], true};
    return true;
}

// ---- ledger `supply` (event 20) ----

// Wire-stable; append only.
enum class Reason : std::uint8_t
{
    Order = 0,    // overlord posted a craft order (count = bags)
    Donate = 1,   // adventurer -> rep cloth mail
    Feed = 2,     // rep -> artisan cloth mail, or treasury -> artisan copper (thread / trainer)
    Craft = 3,    // artisan finished a cast (item = product, count = units made)
    Deliver = 4,  // artisan -> rep or rep -> member bag mail
    Pay = 5,      // treasury -> artisan per delivered bag
    Surplus = 6,  // rep listed / sold spare bags
    Xp = 7,       // work XP (artisan per item, rep per deal); copper = XP amount
    Refused = 8,  // a movement not made; op names it
    Travel = 9    // role bot relocated home by portal fallback (op = other_map | stuck)
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Order: return "order";
        case Reason::Donate: return "donate";
        case Reason::Feed: return "feed";
        case Reason::Craft: return "craft";
        case Reason::Deliver: return "deliver";
        case Reason::Pay: return "pay";
        case Reason::Surplus: return "surplus";
        case Reason::Xp: return "xp";
        case Reason::Refused: return "refused";
        case Reason::Travel: return "travel";
    }
    return "refused";
}

// Trailing fields: house, oid (team order id, run-scoped, never reused; 0 = none), item, count, copper, from,
// to (guid-lows; 0 = the treasury / no player), and op when given. team is the row's own field.
inline std::string LedgerFields(std::string_view house, std::uint32_t oid, std::uint32_t item, std::uint32_t count,
                                std::uint64_t copper, std::uint32_t from, std::uint32_t to, char const* op = nullptr)
{
    std::string out = ",\"house\":\"";
    out += house;
    out += "\",\"oid\":" + std::to_string(oid) + ",\"item\":" + std::to_string(item) +
           ",\"count\":" + std::to_string(count) + ",\"copper\":" + std::to_string(copper) +
           ",\"from\":" + std::to_string(from) + ",\"to\":" + std::to_string(to);
    if (op)
    {
        out += ",\"op\":\"";
        out += op;
        out += "\"";
    }
    return out;
}

// ---- runtime (AutoWowSupply.cpp). ----
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}
inline bool Enabled() { return detail::gEnabled; }

enum class Role : std::uint8_t
{
    None = 0,
    Rep = 1,
    Artisan = 2
};

// A role bot's static facts (read-only after LoadConfig).
struct RoleInfo
{
    Role role = Role::None;
    bool alliance = false;
    bool bagHouse = false;  // its house makes the V1 product (only that artisan crafts)
    Home home;
};

// Station near a team's home (read-only after LoadConfig): creature entry (or mailbox go entry) + position.
struct Station
{
    std::uint32_t entry = 0;  // 0 = none found
    std::uint32_t map = 0;
    std::int32_t x = 0, y = 0, z = 0;
};

struct Stations
{
    Station mailbox, trainer, threadVendor, auctioneer;
};

// Shared per-team state (world thread writes, role bots' map threads read), copied out under the lock.
struct TeamView
{
    std::uint32_t version = kStateVersion;
    std::uint32_t orderId = 0;    // current order (0 = none yet)
    std::uint32_t remaining = 0;  // bags the order still wants delivered to the rep
    std::uint32_t surplus = 0;    // bags the rep should list / sell now
};

// Reads AutoWow.Supply.* (after AutoWowGuilds::LoadConfig; needs AutoWow.Guilds.Enable) and finds the stations.
void LoadConfig();
// World thread (PlayerbotsWorldScript::OnUpdate): overlord, feed, delivery, pay, XP, routing room.
void WorldUpdate(std::uint32_t diff);
RoleInfo RoleOf(std::uint32_t guid);
Stations const& StationsOf(bool alliance);
TeamView ViewOf(bool alliance);
Recipe const& BagRecipe();
std::uint32_t BagItem();
std::uint32_t BagSpell();
std::uint32_t BoltItem();
std::uint32_t BoltSpell();
std::uint32_t ThreadItem();
std::uint32_t ThreadPrice();  // copper per unit at the vendor (before reputation discount)
std::string const& BagHouseName();
std::vector<std::uint32_t> const& LearnSpells();  // trainer spell ids the artisan learns
// Map thread (artisan step): copper it lacks for due thread / training; the world tick tops it up.
void SetArtisanWant(bool alliance, std::uint64_t copper, bool canBag);
// Map thread: the rep's surplus listing / sale happened (the world tick recomputes it).
void ClearSurplus(bool alliance);
// Emit a `supply` row (any thread; no-op unless the player is a recorded bot) and a [Supply] log line.
void Emit(Player* p, Reason r, std::uint32_t oid, std::uint32_t item, std::uint32_t count, std::uint64_t copper,
          std::uint32_t from, std::uint32_t to, char const* op = nullptr);
// Errand sell stop (map thread), before any sell: a cohort non-tailor mails its cloth to the bag house rep of
// its team (queued to the world thread) and the stacks are held back from this stop's sales.
void RouteCloth(Player* bot);
// SellAction: true = the item (guid-low) is queued for a cloth donation (not sold).
bool HeldForDonation(std::uint32_t itemGuid);
}  // namespace AutoWowSupply

#endif
