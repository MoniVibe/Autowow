/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWGUILDSPOLICY_H
#define _PLAYERBOT_AUTOWOWGUILDSPOLICY_H

// AutoWow.Guilds (docs/SUPPLY_CHAIN_PLAN.md lane A): profession-house guilds for the persistent cohort, the
// guild bank money as the house treasury, vendor-income tax and a mail helper. Pure rules here (unit-tested);
// runtime in AutoWowGuilds.cpp. No free gold or items: every movement is a real player <-> bank or mail transfer.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class Player;

namespace AutoWowGuilds
{
// ---- cohort guid list: "62955-63004,70001" (ascending or not; ranges inclusive) ----

struct GuidRange
{
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
};

// ponytail: RepOf scans the ranges guid by guid, so the whole list is capped at kMaxCohortGuids.
inline constexpr std::uint64_t kMaxCohortGuids = 65536;

namespace detail
{
inline bool ParseU32(std::string_view s, std::uint32_t& out)
{
    if (s.empty() || s.size() > 10)
        return false;
    std::uint64_t v = 0;
    for (char const c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > 0xFFFFFFFFull)
        return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

template <typename F>
inline void Split(std::string_view s, char sep, F&& fn)
{
    std::size_t start = 0;
    for (;;)
    {
        std::size_t const end = s.find(sep, start);
        fn(Trim(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)));
        if (end == std::string_view::npos)
            return;
        start = end + 1;
    }
}
}  // namespace detail

// False (out untouched) on any malformed token, a range with lo > hi or guid 0, or more than kMaxCohortGuids.
inline bool ParseGuidRanges(std::string_view text, std::vector<GuidRange>& out)
{
    std::vector<GuidRange> ranges;
    std::uint64_t total = 0;
    bool ok = !detail::Trim(text).empty();
    detail::Split(text, ',', [&](std::string_view token) {
        if (!ok)
            return;
        GuidRange r;
        std::size_t const dash = token.find('-');
        if (dash == std::string_view::npos)
            ok = detail::ParseU32(token, r.lo) && (r.hi = r.lo, true);
        else
            ok = detail::ParseU32(detail::Trim(token.substr(0, dash)), r.lo) &&
                 detail::ParseU32(detail::Trim(token.substr(dash + 1)), r.hi);
        ok = ok && r.lo && r.lo <= r.hi;
        if (ok)
        {
            total += std::uint64_t(r.hi) - r.lo + 1;
            ok = total <= kMaxCohortGuids;
            ranges.push_back(r);
        }
    });
    if (!ok)
        return false;
    out = std::move(ranges);
    return true;
}

inline bool InRanges(std::vector<GuidRange> const& ranges, std::uint32_t guid)
{
    for (GuidRange const& r : ranges)
        if (guid >= r.lo && guid <= r.hi)
            return true;
    return false;
}

// ---- houses: "Weavers:197,333;Smiths:186,164,202" (name: ASCII letters only, 1..16; skill line ids) ----

struct House
{
    std::string name;
    std::vector<std::uint32_t> skills;
};

// False (out untouched) on an empty list, a bad or duplicate name, or a house without a valid skill id.
inline bool ParseHouses(std::string_view text, std::vector<House>& out)
{
    std::vector<House> houses;
    bool ok = !detail::Trim(text).empty();
    detail::Split(text, ';', [&](std::string_view token) {
        if (!ok)
            return;
        std::size_t const colon = token.find(':');
        if (colon == std::string_view::npos)
        {
            ok = false;
            return;
        }
        House h;
        std::string_view const name = detail::Trim(token.substr(0, colon));
        ok = !name.empty() && name.size() <= 16;
        for (char const c : name)
            ok = ok && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        for (House const& other : houses)
            ok = ok && other.name != name;
        h.name = std::string(name);
        detail::Split(token.substr(colon + 1), ',', [&](std::string_view skill) {
            std::uint32_t id = 0;
            ok = ok && detail::ParseU32(skill, id) && id;
            if (ok)
                h.skills.push_back(id);
        });
        if (ok)
            houses.push_back(std::move(h));
    });
    if (!ok)
        return false;
    out = std::move(houses);
    return true;
}

// The house a cohort bot joins: the lowest-listed house holding any skill the bot has. No match = house 0
// (the first listed house takes the bots without a matching profession).
template <typename HasSkill>
inline std::size_t HouseFor(std::vector<House> const& houses, HasSkill&& hasSkill)
{
    for (std::size_t i = 0; i < houses.size(); ++i)
        for (std::uint32_t const skill : houses[i].skills)
            if (hasSkill(skill))
                return i;
    return 0;
}

// ---- guild names: pattern tokens {house} and {team} (Alliance | Horde) ----

inline constexpr std::size_t kMaxGuildNameLength = 24;  // characters guild.name VARCHAR(24)

// Empty when the result is empty or longer than the guild name column (the house is then skipped).
inline std::string GuildName(std::string_view pattern, std::string_view house, bool alliance)
{
    std::string out;
    for (std::size_t i = 0; i < pattern.size();)
    {
        if (pattern.substr(i, 7) == "{house}")
        {
            out += house;
            i += 7;
        }
        else if (pattern.substr(i, 6) == "{team}")
        {
            out += alliance ? "Alliance" : "Horde";
            i += 6;
        }
        else
            out.push_back(pattern[i++]);
    }
    return out.empty() || out.size() > kMaxGuildNameLength ? std::string() : out;
}

// ---- treasury ----

// Tax on vendor income: floor(sold * pct / 100); pct is clamped to 100.
inline constexpr std::uint64_t TaxCopper(std::uint64_t sold, std::uint32_t pct)
{
    return pct >= 100 ? sold : sold / 100 * pct + sold % 100 * pct / 100;
}

// A bank -> player payment goes through only in full.
inline constexpr bool PayAllowed(std::uint64_t balance, std::uint64_t copper) { return copper && balance >= copper; }

// Stock postage (WorldSession::HandleSendMail): 30 copper per attached item, 30 for a mail without items.
inline constexpr std::uint32_t kPostagePerItem = 30;
inline constexpr std::uint32_t Postage(std::uint32_t items) { return items ? kPostagePerItem * items : kPostagePerItem; }

// ---- ledger `guild` (event 19) ----

// Wire-stable; append only.
enum class Reason : std::uint8_t
{
    Created = 0,         // house guild created; bot = its first leader
    Joined = 1,          // bot added to its house guild
    SkipOtherGuild = 2,  // bot already in a guild that is not its house: never moved
    Tax = 3,             // vendor-income tax deposited (also a `trade` row, action tax)
    Pay = 4,             // bank -> player
    Deposit = 5,         // player -> bank
    Refused = 6,         // a movement not made; op names it
    Postage = 7,         // mail postage paid from the bank for the house rep
    Levy = 8             // faction levy: a short house bank drew copper from the richest same-team house bank
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Created: return "created";
        case Reason::Joined: return "joined";
        case Reason::SkipOtherGuild: return "skip_other_guild";
        case Reason::Tax: return "tax";
        case Reason::Pay: return "pay";
        case Reason::Deposit: return "deposit";
        case Reason::Refused: return "refused";
        case Reason::Postage: return "postage";
        case Reason::Levy: return "levy";
    }
    return "refused";
}

// Trailing fields: house (name, letters only), gid, copper, balance_after (guild bank copper), and on
// refused op (the refused reason's name). team is the row's own field.
inline std::string LedgerFields(std::string_view house, std::uint32_t gid, std::uint64_t copper,
                                std::uint64_t balanceAfter, char const* op = nullptr)
{
    std::string out = ",\"house\":\"";
    out += house;
    out += "\",\"gid\":" + std::to_string(gid) + ",\"copper\":" + std::to_string(copper) +
           ",\"balance_after\":" + std::to_string(balanceAfter);
    if (op)
    {
        out += ",\"op\":\"";
        out += op;
        out += "\"";
    }
    return out;
}

// ---- runtime (AutoWowGuilds.cpp). World thread only unless noted; guids are guid-lows. ----
namespace detail
{
inline bool gEnabled = false;
}
inline bool Enabled() { return detail::gEnabled; }

void LoadConfig();
// Bot login (PlayerbotHolder::OnBotLogin): a cohort bot joins its house guild, a configured rep its own
// house; creates the house guild when missing (the logging-in player, or the rep when online, leads it).
void OnLogin(Player* player);
// The player's guild id when it is one of the house guilds, else 0.
std::uint32_t HouseGuildOf(Player* player);
// The house rep (logistics hub): AutoWow.Guilds.Rep.<House>.<Team>, else the lowest cohort guid in the
// guild. 0 = unknown guild / no member yet. May be offline.
std::uint32_t RepOf(std::uint32_t guildId);
std::uint64_t Balance(std::uint32_t guildId);
// player -> its house guild bank. Refused (logged) when short or not in a house.
bool Deposit(Player* from, std::uint64_t copper, Reason reason = Reason::Deposit);
// guild bank -> player. Refused (logged) when the bank is short.
bool Pay(std::uint32_t guildId, Player* to, std::uint64_t copper, Reason reason = Reason::Pay);
// Any thread (the errand sell stop, a map thread): queues the tax on `sold` copper to the world thread.
void QueueTax(Player* bot, std::uint64_t sold);
// Free slots in the rep's bags plus bank (bank slots and bank bags); the rep must be online.
std::uint32_t RepFreeSlots(Player* rep);
// Mail from an online sender to any same-team character. Items move out of the sender's inventory (whole
// stacks, at most 12). Postage is the sender's, except the rep of a house pays it from the guild bank.
bool SendMoney(std::uint32_t fromGuid, std::uint32_t toGuid, std::uint32_t copper, std::string const& subject);
bool SendItems(std::uint32_t fromGuid, std::uint32_t toGuid, std::vector<std::uint32_t> const& itemGuids,
               std::string const& subject);
// Read-only after LoadConfig (any thread): the houses in config order, a house's configured rep (0 = none
// configured) and the cohort guid ranges.
std::vector<House> const& Houses();
std::uint32_t ConfiguredRep(std::size_t house, bool alliance);
std::vector<GuidRange> const& Cohort();
// World thread: the guild id of (house, team), found by name when not cached; 0 = no such guild yet.
std::uint32_t HouseGuildId(std::size_t house, bool alliance);
}  // namespace AutoWowGuilds

#endif
