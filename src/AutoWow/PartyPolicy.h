/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_PARTY_POLICY_H
#define AUTOWOW_PARTY_POLICY_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

class PlayerbotAI;

// Cohort parties and dungeon runs (AutoWow.Party.Enable / AutoWow.Dungeon.Enable, default 0).
//
// Formation (world thread, every CheckIntervalMs): ungrouped independent cohort bots of one faction, on
// one map, in the same zone or within HubYards, whose levels span at most LevelSpread, form parties of
// MinSize..MaxSize when they have a reason: a data-driven dungeon (AutoWow.Dungeon.List) brackets every
// member's level and the roster holds a tank and a healer (dungeon), else a group quest is in a member's
// log or offered in the zone at the member's level (group_quest). Roles by class; leader = the tank, else
// the highest level (lowest guid on ties). Loot: need before greed from uncommon.
//
// Party play: the leader keeps its own New-RPG loop (scheduler, quests, errands); followers take
// "+follow,-new rpg,-grind" with the leader as master and assist its fights (kill credit is shared,
// quests are pushed by the stock "auto share quest"). In a party, group quests (type Group, suggested
// players <= party size) and dungeon quests (dungeon parties) become acceptable. Role strategies: tank
// class stance/form strategy + "tank assist"; healer class heal strategy + "healer dps".
//
// Value-only (no world access, no floats in decisions, no RNG); stable orders (guid ascending).
namespace AutoWowParty
{
// 2: Party.questSig / questSinceMs (AutoWow.Unstick.V2); 3: Party recruit fields (AutoWow.Dungeon.Recruit);
// 4: Party.gatherMs / gatherPortaled (AutoWow.Dungeon.RecruitGatherPortalMs)
inline constexpr std::uint8_t kStateVersion = 4;

// Wire-stable (ledger `roles`); append only.
enum class Role : std::uint8_t
{
    Dps = 0,
    Tank = 1,
    Healer = 2
};

inline constexpr char const* RoleName(Role r)
{
    switch (r)
    {
        case Role::Tank: return "tank";
        case Role::Healer: return "healer";
        case Role::Dps: return "dps";
    }
    return "dps";
}

// Wire-stable (ledger `why`); append only.
enum class Reason : std::uint8_t
{
    None = 0,
    Dungeon = 1,
    GroupQuest = 2,
    Squad = 3  // AutoWow.Squad roster (EnsureSquad); never formed by FormParties
};

inline constexpr char const* ReasonName(Reason r)
{
    switch (r)
    {
        case Reason::Dungeon: return "dungeon";
        case Reason::GroupQuest: return "group_quest";
        case Reason::Squad: return "squad";
        case Reason::None: return "none";
    }
    return "none";
}

// Wire-stable ledger `party` reasons for a disband; append only.
enum class Disband : std::uint8_t
{
    None = 0,
    LevelDrift,
    Separated,
    MemberOffline,
    DungeonDone,
    NoPurpose,
    MaxAge,
    TooSmall,
    Disabled,
    Stalled,  // AutoWow.Unstick.V2: group-quest party without quest progress for StallMs, or its leader gave up
    Recruited, // AutoWow.Dungeon.RecruitWidePool: a member was recruited into a dungeon party
    UnsafeZone,    // a member is below the curated minimum level of the leader's zone
    SurvivalEscape // a member's hard-escape recovery retired an open-world group-quest party
};

inline constexpr char const* DisbandName(Disband d)
{
    switch (d)
    {
        case Disband::LevelDrift: return "level_drift";
        case Disband::Separated: return "separated";
        case Disband::MemberOffline: return "member_offline";
        case Disband::DungeonDone: return "dungeon_done";
        case Disband::NoPurpose: return "no_purpose";
        case Disband::MaxAge: return "max_age";
        case Disband::TooSmall: return "too_small";
        case Disband::Disabled: return "disabled";
        case Disband::Stalled: return "stalled";
        case Disband::Recruited: return "recruited";
        case Disband::UnsafeZone: return "unsafe_zone";
        case Disband::SurvivalEscape: return "survival_escape";
        case Disband::None: return "none";
    }
    return "none";
}

// ---- classes and roles (3.3.5 class ids) ------------------------------------------------------------
inline constexpr std::uint32_t kWarrior = 1, kPaladin = 2, kHunter = 3, kRogue = 4, kPriest = 5, kDeathKnight = 6,
                               kShaman = 7, kMage = 8, kWarlock = 9, kDruid = 11;

// Preference rank (lower = preferred), 0 = cannot fill the role.
inline constexpr std::uint32_t TankRank(std::uint32_t cls)
{
    switch (cls)
    {
        case kWarrior: return 1;
        case kPaladin: return 2;
        case kDruid: return 3;
        case kDeathKnight: return 4;
        default: return 0;
    }
}

inline constexpr std::uint32_t HealRank(std::uint32_t cls)
{
    switch (cls)
    {
        case kPriest: return 1;
        case kShaman: return 2;
        case kDruid: return 3;
        case kPaladin: return 4;
        default: return 0;
    }
}

struct Member
{
    std::uint32_t guid = 0;
    std::uint32_t level = 0;
    std::uint32_t cls = 0;
};

// Roles for a roster (same order as `members`): one tank (best TankRank, then higher level, then lower
// guid), one healer among the rest (HealRank, level, guid), everyone else dps.
inline std::vector<Role> AssignRoles(std::vector<Member> const& members)
{
    std::vector<Role> roles(members.size(), Role::Dps);
    auto pick = [&](auto rankOf, Role role)
    {
        std::size_t best = members.size();
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            if (roles[i] != Role::Dps || !rankOf(members[i].cls))
                continue;
            if (best == members.size())
            {
                best = i;
                continue;
            }
            Member const& a = members[i];
            Member const& b = members[best];
            std::uint32_t const ra = rankOf(a.cls), rb = rankOf(b.cls);
            if (ra < rb || (ra == rb && (a.level > b.level || (a.level == b.level && a.guid < b.guid))))
                best = i;
        }
        if (best != members.size())
            roles[best] = role;
    };
    pick(TankRank, Role::Tank);
    pick(HealRank, Role::Healer);
    return roles;
}

// Index of the leader: the tank, else the highest level (lower guid on ties).
inline std::size_t ChooseLeader(std::vector<Member> const& members, std::vector<Role> const& roles)
{
    for (std::size_t i = 0; i < roles.size(); ++i)
        if (roles[i] == Role::Tank)
            return i;
    std::size_t best = 0;
    for (std::size_t i = 1; i < members.size(); ++i)
        if (members[i].level > members[best].level ||
            (members[i].level == members[best].level && members[i].guid < members[best].guid))
            best = i;
    return best;
}

// ---- dungeons (AutoWow.Dungeon.List) ----------------------------------------------------------------
inline constexpr std::uint8_t kAlliance = 1, kHorde = 2;

struct DungeonDef
{
    std::uint32_t map = 0;
    std::uint32_t minLevel = 0;
    std::uint32_t maxLevel = 0;
    std::uint8_t teams = 0;
};

// "map:min:max:A|H|AH,..." in priority order; malformed entries are skipped.
inline std::vector<DungeonDef> ParseDungeons(std::string_view text)
{
    std::vector<DungeonDef> out;
    std::size_t i = 0;
    while (i <= text.size())
    {
        std::size_t const end = std::min(text.find(',', i), text.size());
        std::string_view entry = text.substr(i, end - i);
        i = end + 1;
        std::uint32_t nums[3] = {};
        std::size_t field = 0;
        bool ok = true;
        std::uint8_t teams = 0;
        for (char const c : entry)
        {
            if (c == ' ')
                continue;
            if (c == ':')
            {
                ++field;
                continue;
            }
            if (field < 3 && c >= '0' && c <= '9' && nums[field] < 100000)
                nums[field] = nums[field] * 10 + static_cast<std::uint32_t>(c - '0');
            else if (field == 3 && (c == 'A' || c == 'a'))
                teams |= kAlliance;
            else if (field == 3 && (c == 'H' || c == 'h'))
                teams |= kHorde;
            else
                ok = false;
        }
        if (ok && field == 3 && nums[0] && nums[1] && nums[1] <= nums[2] && teams)
            out.push_back({nums[0], nums[1], nums[2], teams});
        if (end == text.size())
            break;
    }
    return out;
}

// First listed dungeon for the team that brackets [minLevel, maxLevel]; nullptr when none.
inline DungeonDef const* SelectDungeon(std::vector<DungeonDef> const& defs, std::uint8_t team, std::uint32_t minLevel,
                                       std::uint32_t maxLevel)
{
    for (DungeonDef const& d : defs)
        if ((d.teams & team) && minLevel >= d.minLevel && maxLevel <= d.maxLevel)
            return &d;
    return nullptr;
}

// ---- quests -------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kQuestTypeNormal = 0, kQuestTypeGroup = 1, kQuestTypeDungeon = 81;

// A quest worth a party (formation signal): a Group-type quest or one suggesting two or more players.
inline constexpr bool IsGroupQuest(std::uint32_t type, std::uint32_t suggested)
{
    return type == kQuestTypeGroup || (type == kQuestTypeNormal && suggested >= 2);
}

// Quest capability inside a party of `partySize` (replaces the solo-only rule of IsQuestCapableDoing).
inline constexpr bool QuestCapableInParty(std::uint32_t type, std::uint32_t suggested, std::uint32_t partySize,
                                          bool dungeonParty)
{
    std::uint32_t const need = suggested ? suggested : 1;
    if (type == kQuestTypeNormal || type == kQuestTypeGroup)
        return need <= partySize;
    if (type == kQuestTypeDungeon)
        return dungeonParty;
    return false;
}

// ---- formation ---------------------------------------------------------------------------------------
struct Candidate
{
    std::uint32_t guid = 0;
    std::uint32_t level = 0;
    std::uint32_t cls = 0;
    std::uint8_t team = 0;
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t zoneMinLevel = 0;  // curated zone bracket low; 0 = unknown, so do not reject
    bool groupQuest = false;  // a group quest in its log or offered in its zone at its level
    bool canTank = false;     // PlanRecruit with RequireTank: warrior, paladin, or druid with a bear form
};

// Both members can safely follow either candidate as leader. Unknown zone brackets remain admissible.
[[nodiscard]] inline bool ZoneLevelCompatible(Candidate const& a, Candidate const& b)
{
    return (!a.zoneMinLevel || b.level >= a.zoneMinLevel) &&
           (!b.zoneMinLevel || a.level >= b.zoneMinLevel);
}

struct FormParams
{
    std::uint32_t levelSpread = 3;
    std::uint32_t hubYards = 300;
    std::uint32_t minSize = 2;
    std::uint32_t maxSize = 5;
    bool dungeons = false;
    std::uint32_t dungeonMinSize = 3;
    bool requireTankHealer = true;
};

struct Plan
{
    std::vector<std::uint32_t> guids;  // ascending
    std::vector<Role> roles;           // same order
    std::uint32_t leader = 0;
    Reason reason = Reason::None;
    std::uint32_t dungeonMap = 0;
    std::int32_t tankDelta = 0;  // PlanRecruit: the tank's level minus the dungeon's max level (no tank: 0)
};

inline bool Near(Candidate const& a, Candidate const& b, std::uint32_t hubYards)
{
    if (a.team != b.team || a.map != b.map)
        return false;
    if (a.zone && a.zone == b.zone)
        return true;
    std::int64_t const dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy <= std::int64_t(hubYards) * hubYards;
}

// Parties from the candidates (guid-ascending seeds; each bot in at most one plan). A seed collects the
// compatible unassigned bots nearest its level (then guid); a dungeon plan takes the best tank and healer
// first, then fills with dps up to MaxSize while the level span stays within LevelSpread.
inline std::vector<Plan> FormParties(std::vector<Candidate> cands, FormParams const& p,
                                     std::vector<DungeonDef> const& defs)
{
    std::sort(cands.begin(), cands.end(), [](Candidate const& a, Candidate const& b) { return a.guid < b.guid; });
    std::vector<bool> used(cands.size(), false);
    std::vector<Plan> plans;
    std::uint32_t const maxSize = std::min<std::uint32_t>(std::max<std::uint32_t>(p.maxSize, 2), 5);
    for (std::size_t s = 0; s < cands.size(); ++s)
    {
        if (used[s])
            continue;
        Candidate const& seed = cands[s];
        std::vector<std::size_t> pool;
        for (std::size_t i = 0; i < cands.size(); ++i)
        {
            std::uint32_t const d = cands[i].level > seed.level ? cands[i].level - seed.level : seed.level - cands[i].level;
            if (!used[i] && d <= p.levelSpread && (i == s || Near(seed, cands[i], p.hubYards)))
                pool.push_back(i);
        }
        std::stable_sort(pool.begin(), pool.end(), [&](std::size_t a, std::size_t b)
        {
            auto dist = [&](std::size_t i)
            { return cands[i].level > seed.level ? cands[i].level - seed.level : seed.level - cands[i].level; };
            return dist(a) != dist(b) ? dist(a) < dist(b) : cands[a].guid < cands[b].guid;
        });
        // Greedy fill from the seed; `prefer` first (tank, healer), then everyone, level span bounded.
        auto build = [&](bool roleFirst, bool zoneSafe)
        {
            std::vector<std::size_t> pick{s};
            std::uint32_t lo = seed.level, hi = seed.level;
            auto tryAdd = [&](std::size_t i)
            {
                if (pick.size() >= maxSize || std::find(pick.begin(), pick.end(), i) != pick.end())
                    return false;
                for (std::size_t j : pick)
                    if (zoneSafe && !ZoneLevelCompatible(cands[j], cands[i]))
                        return false;
                std::uint32_t const l2 = std::min(lo, cands[i].level), h2 = std::max(hi, cands[i].level);
                if (h2 - l2 > p.levelSpread)
                    return false;
                pick.push_back(i);
                lo = l2;
                hi = h2;
                return true;
            };
            if (roleFirst)
            {
                auto has = [&](auto rankOf)
                {
                    for (std::size_t i : pick)
                        if (rankOf(cands[i].cls))
                            return true;
                    return false;
                };
                auto addBest = [&](auto rankOf)
                {
                    std::size_t best = cands.size();
                    for (std::size_t i : pool)
                        if (rankOf(cands[i].cls) && std::find(pick.begin(), pick.end(), i) == pick.end() &&
                            (best == cands.size() || rankOf(cands[i].cls) < rankOf(cands[best].cls)))
                            best = i;
                    if (best != cands.size())
                        tryAdd(best);
                };
                if (!has(TankRank))
                    addBest(TankRank);
                // The healer must be a different bot than the tank (a druid seed counts once).
                std::vector<Member> ms;
                for (std::size_t i : pick)
                    ms.push_back({cands[i].guid, cands[i].level, cands[i].cls});
                std::vector<Role> const r = AssignRoles(ms);
                if (std::find(r.begin(), r.end(), Role::Healer) == r.end())
                {
                    std::size_t best = cands.size();
                    for (std::size_t i : pool)
                        if (HealRank(cands[i].cls) && std::find(pick.begin(), pick.end(), i) == pick.end() &&
                            (best == cands.size() || HealRank(cands[i].cls) < HealRank(cands[best].cls)))
                            best = i;
                    if (best != cands.size())
                        tryAdd(best);
                }
            }
            for (std::size_t i : pool)
                tryAdd(i);
            return pick;
        };

        Plan plan;
        std::vector<std::size_t> pick;
        if (p.dungeons)
        {
            pick = build(true, false);
            std::vector<Member> ms;
            std::uint32_t lo = ~0u, hi = 0;
            for (std::size_t i : pick)
            {
                ms.push_back({cands[i].guid, cands[i].level, cands[i].cls});
                lo = std::min(lo, cands[i].level);
                hi = std::max(hi, cands[i].level);
            }
            std::vector<Role> const r = AssignRoles(ms);
            bool const composed = !p.requireTankHealer ||
                (std::find(r.begin(), r.end(), Role::Tank) != r.end() &&
                 std::find(r.begin(), r.end(), Role::Healer) != r.end());
            if (DungeonDef const* d = SelectDungeon(defs, seed.team, lo, hi);
                d && composed && pick.size() >= std::max(p.dungeonMinSize, p.minSize))
            {
                plan.reason = Reason::Dungeon;
                plan.dungeonMap = d->map;
            }
        }
        if (plan.reason == Reason::None)
        {
            pick = build(false, true);
            bool quest = false;
            for (std::size_t i : pick)
                quest = quest || cands[i].groupQuest;
            if (quest && pick.size() >= std::max<std::uint32_t>(p.minSize, 2))
                plan.reason = Reason::GroupQuest;
        }
        if (plan.reason == Reason::None)
            continue;
        std::sort(pick.begin(), pick.end());
        std::vector<Member> ms;
        for (std::size_t i : pick)
        {
            used[i] = true;
            plan.guids.push_back(cands[i].guid);
            ms.push_back({cands[i].guid, cands[i].level, cands[i].cls});
        }
        plan.roles = AssignRoles(ms);
        plan.leader = plan.guids[ChooseLeader(ms, plan.roles)];
        plans.push_back(std::move(plan));
    }
    return plans;
}

// ---- recruitment (AutoWow.Dungeon.Recruit) --------------------------------------------------------------
// FormParties needs MinSize bots of one faction sharing a zone / HubYards, a level window, a tank and a healer
// and a first-listed bracketing dungeon whose entrance is on their continent; a cohort spread over two
// continents almost never offers that (S62..S70 ledgers: 0 dungeon parties). Recruitment instead picks, per
// faction and sweep, the listed dungeon with the most eligible bots anywhere, and the runtime gathers them at
// its entrance (a walk when everyone is in walk range, else a logged portal).
struct EntranceSpot  // same order as the dungeon list
{
    std::uint32_t map = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool known = false;
};

struct RecruitParams
{
    std::uint32_t levelSpread = 4;
    std::uint32_t minSize = 4;
    std::uint32_t maxSize = 5;
    std::uint32_t walkYards = 6000;
    bool requireTank = false;  // AutoWow.Dungeon.RequireTank
    std::uint32_t tankOverLevel = 0;  // AutoWow.Dungeon.RecruitTankOverLevel
};

// A tank candidate above the window [.., hi] up to TankOverLevel levels over the dungeon's max level (0 = off).
inline bool OverLevelTank(Candidate const& c, std::uint32_t hi, DungeonDef const& d, RecruitParams const& p)
{
    return p.tankOverLevel && c.level > hi && c.level <= d.maxLevel + p.tankOverLevel;
}

inline bool InWalkRange(Candidate const& c, EntranceSpot const& e, std::uint32_t yards)
{
    std::int64_t const dx = c.x - e.x, dy = c.y - e.y;
    return c.map == e.map && dx * dx + dy * dy <= std::int64_t(yards) * yards;
}

// The recruit plan for `team` (empty guids = none). Over the listed dungeons open to the team with a known
// entrance and every level window [lo, lo + LevelSpread] inside a dungeon's band, the window holding the most
// candidates wins (ties: list order, then the lower window); it needs MinSize candidates and a healer-capable
// class (tanks are preferred, not required: probe parties cleared with dps-heavy rosters). Roster: the best
// healer (HealRank), then the best tank among the rest (TankRank), then dps up to MaxSize; every pick prefers a
// bot in walk range of the entrance, then the higher level, then the lower guid. Leader = the tank, else the
// highest level. RequireTank (S71: 9/9 recruited runs with a tank completed, 15/15 without failed): a window
// also needs a tank-capable candidate (Candidate::canTank) other than its healer; the tank is picked first,
// then the healer among the rest. `why` (optional): "no_window" (no MinSize window with a healer) or "no_tank"
// (such windows exist, none with a tank) when there is no plan, else "". TankOverLevel N > 0: when the window
// has no tank of its own, a tank-capable candidate above the window up to N levels over the dungeon's max level
// fills the tank slot (S74: the free Alliance tanks are L63-66, over every listed band); the lowest level first,
// then TankRank, walk range, guid. It does not count toward the window size.
inline Plan PlanRecruit(std::vector<Candidate> cands, std::uint8_t team, std::vector<DungeonDef> const& defs,
                        std::vector<EntranceSpot> const& entrances, RecruitParams const& p, char const** why = nullptr)
{
    std::sort(cands.begin(), cands.end(), [](Candidate const& a, Candidate const& b) { return a.guid < b.guid; });
    std::uint32_t const minSize = std::max<std::uint32_t>(p.minSize, 2);
    std::uint32_t const maxSize = std::min<std::uint32_t>(std::max(p.maxSize, minSize), 5);
    std::size_t best = defs.size();
    std::uint32_t bestLo = 0, bestHi = 0, bestCount = 0;
    bool windowSeen = false;
    for (std::size_t di = 0; di < defs.size() && di < entrances.size(); ++di)
    {
        DungeonDef const& d = defs[di];
        if (!(d.teams & team) || !entrances[di].known)
            continue;
        for (std::uint32_t lo = d.minLevel; lo <= std::min<std::uint32_t>(d.maxLevel, 255); ++lo)
        {
            std::uint32_t const hi = std::min(lo + p.levelSpread, d.maxLevel);
            std::uint32_t count = 0, tanks = 0, heals = 0, both = 0, over = 0;
            for (Candidate const& c : cands)
                if (c.team == team && c.level >= lo && c.level <= hi)
                {
                    ++count;
                    bool const h = HealRank(c.cls) != 0;
                    heals += h;
                    tanks += c.canTank;
                    both += h && c.canTank;
                }
                else if (c.team == team && c.canTank && OverLevelTank(c, hi, d, p))
                    ++over;
            if (count < minSize || !heals)
                continue;
            windowSeen = true;
            // A tank and a healer that are two bots (one hybrid cannot be both); an over-level tank is never
            // the window's healer.
            if (p.requireTank && !over && (!tanks || (tanks == 1 && heals == 1 && both == 1)))
                continue;
            if (count > bestCount)
            {
                best = di;
                bestLo = lo;
                bestHi = hi;
                bestCount = count;
            }
        }
    }
    Plan plan;
    if (why)
        *why = best != defs.size() ? "" : windowSeen ? "no_tank" : "no_window";
    if (best == defs.size())
        return plan;
    EntranceSpot const& e = entrances[best];
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < cands.size(); ++i)
        if (cands[i].team == team && cands[i].level >= bestLo && cands[i].level <= bestHi)
            pool.push_back(i);
    auto better = [&](std::size_t a, std::size_t b)
    {
        bool const wa = InWalkRange(cands[a], e, p.walkYards), wb = InWalkRange(cands[b], e, p.walkYards);
        if (wa != wb)
            return wa;
        if (cands[a].level != cands[b].level)
            return cands[a].level > cands[b].level;
        return cands[a].guid < cands[b].guid;
    };
    std::vector<std::size_t> pick;
    std::vector<Role> roles;
    // rankOf(candidate index): lower = preferred, 0 = cannot fill the role.
    auto take = [&](auto rankOf, Role role)
    {
        std::size_t sel = cands.size();
        for (std::size_t i : pool)
        {
            if (!rankOf(i) || std::find(pick.begin(), pick.end(), i) != pick.end())
                continue;
            if (sel == cands.size() || rankOf(i) < rankOf(sel) || (rankOf(i) == rankOf(sel) && better(i, sel)))
                sel = i;
        }
        if (sel != cands.size())
        {
            pick.push_back(sel);
            roles.push_back(role);
        }
    };
    auto healRank = [&](std::size_t i) { return HealRank(cands[i].cls); };
    auto tankRank = [&](std::size_t i)
    { return !p.requireTank || cands[i].canTank ? TankRank(cands[i].cls) : 0u; };
    // RequireTank with a tank and a healer that are two window bots: the tank first. Otherwise the healer first
    // (the window's only tank-capable bot may be its only healer; an over-level tank then fills the slot).
    std::uint32_t tanks = 0, heals = 0, both = 0;
    for (std::size_t i : pool)
    {
        bool const h = HealRank(cands[i].cls) != 0;
        heals += h;
        tanks += cands[i].canTank;
        both += h && cands[i].canTank;
    }
    if (p.requireTank && tanks && !(tanks == 1 && heals == 1 && both == 1))
    {
        take(tankRank, Role::Tank);
        take(healRank, Role::Healer);
    }
    else
    {
        take(healRank, Role::Healer);
        take(tankRank, Role::Tank);
    }
    // TankOverLevel: no tank in the window -> the closest-level over-level tank.
    if (std::find(roles.begin(), roles.end(), Role::Tank) == roles.end())
    {
        std::size_t sel = cands.size();
        for (std::size_t i = 0; i < cands.size(); ++i)
        {
            Candidate const& c = cands[i];
            if (c.team != team || !c.canTank || !OverLevelTank(c, bestHi, defs[best], p))
                continue;
            if (sel == cands.size())
            {
                sel = i;
                continue;
            }
            Candidate const& b = cands[sel];
            if (c.level != b.level ? c.level < b.level
                : TankRank(c.cls) != TankRank(b.cls) ? TankRank(c.cls) < TankRank(b.cls)
                                                     : better(i, sel))
                sel = i;
        }
        if (sel != cands.size())
        {
            // Tank first: it takes a roster slot ahead of the dps.
            pick.insert(pick.begin(), sel);
            roles.insert(roles.begin(), Role::Tank);
        }
    }
    std::vector<std::size_t> rest;
    for (std::size_t i : pool)
        if (std::find(pick.begin(), pick.end(), i) == pick.end())
            rest.push_back(i);
    std::sort(rest.begin(), rest.end(), better);
    for (std::size_t i = 0; i < rest.size() && pick.size() < maxSize; ++i)
    {
        pick.push_back(rest[i]);
        roles.push_back(Role::Dps);
    }
    // Guid ascending, roles alongside.
    std::vector<std::size_t> order(pick.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return cands[pick[a]].guid < cands[pick[b]].guid; });
    std::vector<Member> ms;
    for (std::size_t i : order)
    {
        plan.guids.push_back(cands[pick[i]].guid);
        plan.roles.push_back(roles[i]);
        ms.push_back({cands[pick[i]].guid, cands[pick[i]].level, cands[pick[i]].cls});
    }
    plan.leader = plan.guids[ChooseLeader(ms, plan.roles)];
    plan.reason = Reason::Dungeon;
    plan.dungeonMap = defs[best].map;
    for (std::size_t i = 0; i < plan.roles.size(); ++i)
        if (plan.roles[i] == Role::Tank)
            plan.tankDelta = std::int32_t(ms[i].level) - std::int32_t(defs[best].maxLevel);
    return plan;
}

// Why a cohort bot is not recruitable at a sweep (the runtime classifies, first match in this order; the recruit
// log counts every reason). Wire-stable log names; append only before Count.
enum class RecruitOut : std::uint8_t
{
    Free = 0,
    Escort,    // mid escort event
    Instance,  // on an instance / battleground map (mid-dungeon)
    Flight,    // on a taxi or being teleported
    Dead,
    Supply,    // supply artisan / rep role
    Squad,     // AutoWow.Squad roster
    Run,       // in a recruited party, or a formation party mid dungeon run
    Cooldown,  // within RecruitCooldownMs of its last recruited run
    Paused,    // another runtime holds the bot (AutoWow pause)
    Group,     // a core group this registry does not own
    // Pullable with AutoWow.Dungeon.RecruitWidePool (excluded without it):
    Party,     // a formation party (group quest / proximity dungeon) not in a run: dissolved when recruited
    Combat,    // questing / grinding fight
    ZoneMove,  // zone graduation trip: cancelled when recruited
    Errand,    // vendor / auction / trainer / mail trip
    Count
};

inline constexpr char const* RecruitOutName(RecruitOut o)
{
    constexpr char const* names[] = {"free", "escort", "instance", "flight", "dead", "supply", "squad", "run",
                                     "cooldown", "paused", "group", "party", "combat", "zone_move", "errand"};
    return std::size_t(o) < std::size(names) ? names[std::size_t(o)] : "unknown";
}

// Recruitable: free, or (wide pool) only questing / grinding / on an errand, a zone trip or in a formation party.
inline constexpr bool RecruitTakes(RecruitOut o, bool widePool)
{
    return o == RecruitOut::Free || (widePool && o >= RecruitOut::Party && o < RecruitOut::Count);
}

// " out={free=9,combat=3,...}" (non-zero counts, enum order) for the recruit log line.
inline std::string RecruitOutFields(std::uint32_t const (&counts)[std::size_t(RecruitOut::Count)])
{
    std::string out = " out={";
    bool first = true;
    for (std::size_t i = 0; i < std::size_t(RecruitOut::Count); ++i)
        if (counts[i])
        {
            out += (first ? "" : ",") + std::string(RecruitOutName(RecruitOut(i))) + "=" + std::to_string(counts[i]);
            first = false;
        }
    return out + "}";
}

// ---- recruit gather (AutoWow.Dungeon.RecruitGatherPortalMs) -----------------------------------------------
// S75: two ZF runs entered with the leader while two members sat at flight paths far away; the Inside grace
// (MemberGraceMs, 180 s) then abandoned 0/5. From the gather start (the party reaches the entrance: Stage) a
// member not arrived after GatherPortalMs is portaled to the entrance once (its taxi ended first); the missing-
// member grace only counts it after that portal. A member that cannot come (offline, or dead outside the
// dungeon after the deadline) is dropped, and the run goes on while its tank and healer remain (KeepAfterDrop).
struct GatherFacts
{
    bool online = true;
    bool alive = true;
    bool inDungeon = false;  // on the dungeon's map
    bool arrived = false;    // in the dungeon, or at the entrance (within StageYards on its map)
    bool portaled = false;   // the gather portal was already tried
    bool runDeathWitnessed = false;  // corpse map matches this run and a live member witnesses its instance
};

enum class GatherAct : std::uint8_t
{
    None = 0,
    Portal,
    Drop
};

inline GatherAct DecideGather(GatherFacts const& f, bool deadlinePassed)
{
    if (!f.online)
        return GatherAct::Drop;
    if (f.arrived || !deadlinePassed)
        return GatherAct::None;
    if (!f.alive)
        return f.inDungeon || f.runDeathWitnessed ? GatherAct::None : GatherAct::Drop;
    return f.portaled ? GatherAct::None : GatherAct::Portal;
}

// The roster may run on without the dropped members: at least three left, and every tank / healer role the
// party had is still filled.
inline bool KeepAfterDrop(std::vector<Role> const& before, std::vector<Role> const& after)
{
    auto has = [](std::vector<Role> const& v, Role r) { return std::find(v.begin(), v.end(), r) != v.end(); };
    return after.size() >= 3 && (!has(before, Role::Tank) || has(after, Role::Tank)) &&
           (!has(before, Role::Healer) || has(after, Role::Healer));
}

// ---- keeping a party ---------------------------------------------------------------------------------
struct KeepFacts
{
    std::uint32_t online = 0;         // members online
    std::uint32_t size = 0;           // roster size
    std::uint32_t minLevel = 0;
    std::uint32_t maxLevel = 0;
    std::uint64_t separatedMs = 0;    // how long some member has been off the leader's map / beyond SeparatedYards
    std::uint64_t purposelessMs = 0;  // how long the party has had no reason (group quest / dungeon)
    std::uint64_t ageMs = 0;
    bool inDungeonRun = false;
    bool unsafeZone = false;         // some member is below the leader zone's curated minimum
};

struct KeepParams
{
    std::uint32_t levelSpread = 3;
    std::uint64_t separatedMs = 600000;
    std::uint64_t purposelessMs = 1800000;
    std::uint64_t maxAgeMs = 7200000;
};

// Disband reason (None = keep). A dungeon run in progress keeps the party (its own end conditions apply).
inline Disband ShouldDisband(KeepFacts const& f, KeepParams const& p)
{
    if (f.online < f.size)
        return Disband::MemberOffline;
    if (f.size < 2)
        return Disband::TooSmall;
    if (f.inDungeonRun)
        return Disband::None;
    if (f.unsafeZone)
        return Disband::UnsafeZone;
    if (f.maxLevel - f.minLevel > p.levelSpread)
        return Disband::LevelDrift;
    if (f.separatedMs >= p.separatedMs)
        return Disband::Separated;
    if (f.purposelessMs >= p.purposelessMs)
        return Disband::NoPurpose;
    if (f.ageMs >= p.maxAgeMs)
        return Disband::MaxAge;
    return Disband::None;
}

// ---- group combat roles -------------------------------------------------------------------------------
// A hostile attacking a party member (the tank's view).
struct Threat
{
    std::uint32_t mob = 0;       // creature guid counter
    Role victimRole = Role::Dps;
    std::uint32_t victimHpPct = 100;
    std::uint32_t yards = 0;     // tank to mob
};

// The mob the tank should taunt: attacking a non-tank within `maxYards`; healer's attackers first, then
// the lowest-health victim, then the nearer mob, then the lower guid. 0 = none.
inline std::uint32_t PickTauntTarget(std::vector<Threat> const& threats, std::uint32_t maxYards)
{
    Threat const* best = nullptr;
    for (Threat const& t : threats)
    {
        if (t.victimRole == Role::Tank || t.yards > maxYards)
            continue;
        if (!best)
        {
            best = &t;
            continue;
        }
        bool const th = t.victimRole == Role::Healer, bh = best->victimRole == Role::Healer;
        if (th != bh ? th
                     : t.victimHpPct != best->victimHpPct ? t.victimHpPct < best->victimHpPct
                     : t.yards != best->yards             ? t.yards < best->yards
                                                          : t.mob < best->mob)
            best = &t;
    }
    return best ? best->mob : 0;
}

struct Wounded
{
    std::uint32_t guid = 0;
    Role role = Role::Dps;
    std::uint32_t hpPct = 100;
    bool alive = true;
};

// The member the healer heals now: alive, below `healPct`, lowest health; the tank first on equal
// health, then the lower guid. 0 = nobody (the healer may deal damage).
inline std::uint32_t PickHealTarget(std::vector<Wounded> const& party, std::uint32_t healPct)
{
    Wounded const* best = nullptr;
    for (Wounded const& w : party)
    {
        if (!w.alive || w.hpPct >= healPct)
            continue;
        if (!best || w.hpPct < best->hpPct ||
            (w.hpPct == best->hpPct && ((w.role == Role::Tank) != (best->role == Role::Tank) ? w.role == Role::Tank
                                                                                           : w.guid < best->guid)))
            best = &w;
    }
    return best ? best->guid : 0;
}

// Class spell names (stock PlayerbotAI::CastSpell names, highest known rank) in cast-preference order.
inline std::vector<char const*> TauntSpells(std::uint32_t cls)
{
    switch (cls)
    {
        case kWarrior: return {"taunt", "mocking blow"};
        case kPaladin: return {"hand of reckoning"};
        case kDruid: return {"growl"};
        case kDeathKnight: return {"dark command", "death grip"};
        default: return {};
    }
}

inline std::vector<char const*> HealSpells(std::uint32_t cls)
{
    switch (cls)
    {
        case kPriest: return {"flash heal", "heal", "lesser heal", "renew"};
        case kShaman: return {"lesser healing wave", "healing wave"};
        case kDruid: return {"regrowth", "healing touch", "rejuvenation"};
        case kPaladin: return {"flash of light", "holy light"};
        default: return {};
    }
}

// Strategy changes for a role (combat, non-combat). Class sibling strategies replace each other in the
// stock factories, so "+tank" swaps a warrior out of arms/fury; the runtime snapshots the lists first
// and restores them with StrategyUndo.
struct RoleStrategies
{
    std::string combat;
    std::string nonCombat;
};

inline RoleStrategies StrategiesFor(std::uint32_t cls, Role role)
{
    if (role == Role::Tank)
    {
        char const* stance = cls == kWarrior || cls == kPaladin ? "tank" : cls == kDruid ? "bear"
                             : cls == kDeathKnight               ? "blood"
                                                                 : nullptr;
        if (!stance)
            return {};
        return {std::string("+") + stance + ",+tank assist,+tank face,-dps assist", "+tank assist,-dps assist"};
    }
    if (role == Role::Healer)
    {
        char const* heal = cls == kPriest || cls == kPaladin ? "heal" : cls == kShaman || cls == kDruid ? "resto"
                                                                                                        : nullptr;
        if (!heal)
            return {};
        return {std::string("+") + heal + ",+healer dps", ""};
    }
    return {};
}

// "+a,-b" string that turns the `after` strategy list back into `before` (sorted, deterministic).
inline std::string StrategyUndo(std::vector<std::string> before, std::vector<std::string> after)
{
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::string out;
    auto add = [&](char sign, std::string const& name)
    {
        if (!out.empty())
            out += ',';
        out += sign;
        out += name;
    };
    for (std::string const& n : after)
        if (!std::binary_search(before.begin(), before.end(), n))
            add('-', n);
    for (std::string const& n : before)
        if (!std::binary_search(after.begin(), after.end(), n))
            add('+', n);
    return out;
}

// ---- dungeon run ---------------------------------------------------------------------------------------
// Wire-stable; append only.
enum class Phase : std::uint8_t
{
    None = 0,      // questing together
    Approach = 1,  // the leader walks to the entrance trigger, followers follow
    Stage = 2,     // every member steps into the trigger volume; DungeonTransition admits them
    Inside = 3,    // dungeon navigator clears encounters; followers follow
    Exit = 4       // hearthstone out (or the core boots the party after the disband)
};

// A death-loop escape may retire an ordinary open-world group-quest party or a recruited dungeon party
// before instance admission. Native/user/untracked groups never reach this tracked-party policy.
[[nodiscard]] inline bool ShouldLeaveForSurvival(Reason why, bool recruited, Phase phase, bool entered)
{
    if (why == Reason::GroupQuest)
        return phase == Phase::None;
    bool const preEntry = phase == Phase::None || phase == Phase::Approach || phase == Phase::Stage;
    return why == Reason::Dungeon && recruited && preEntry && !entered;
}

struct SurvivalLeaveLiveFacts
{
    bool exactOwnedGroup = false;
    bool allAutonomous = false;
    bool allOpenWorld = false;
};

// The request is made on a map thread, so its stored phase can lag physical admission. Revalidate the
// native roster and every member's current ownership/location immediately before the world thread acts.
[[nodiscard]] inline bool ShouldConsumeSurvivalLeave(Reason why, bool recruited, Phase phase, bool entered,
                                                     SurvivalLeaveLiveFacts const& live)
{
    return ShouldLeaveForSurvival(why, recruited, phase, entered) && live.exactOwnedGroup &&
           live.allAutonomous && live.allOpenWorld;
}

// Wire-stable ledger `dungeon` reasons; append only.
enum class RunEvent : std::uint8_t
{
    Entered = 0,
    BossKilled,
    Completed,
    Wiped,
    Abandoned,
    ApproachGaveUp,
    StageFailed,
    PortalFallback,
    Summary  // AutoWow.Dungeon.Recruit: one row when a recruited party is released
};

inline constexpr char const* RunEventName(RunEvent e)
{
    switch (e)
    {
        case RunEvent::Entered: return "entered";
        case RunEvent::BossKilled: return "boss_killed";
        case RunEvent::Completed: return "completed";
        case RunEvent::Wiped: return "wiped";
        case RunEvent::Abandoned: return "abandoned";
        case RunEvent::ApproachGaveUp: return "approach_gave_up";
        case RunEvent::StageFailed: return "stage_failed";
        case RunEvent::PortalFallback: return "portal_fallback";
        case RunEvent::Summary: return "summary";
    }
    return "unknown";
}

// New encounter bits (ascending encounter index) between two completed-encounter masks.
inline std::vector<std::uint32_t> NewBosses(std::uint32_t before, std::uint32_t after)
{
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < 32; ++i)
        if ((after >> i & 1u) && !(before >> i & 1u))
            out.push_back(i);
    return out;
}

inline bool AllEncountersDone(std::uint32_t mask, std::uint32_t allMask) { return allMask && (mask & allMask) == allMask; }

// ---- ledger fields -------------------------------------------------------------------------------------
inline std::string GuidList(std::vector<std::uint32_t> const& guids)
{
    std::string out = "[";
    for (std::size_t i = 0; i < guids.size(); ++i)
        out += (i ? "," : "") + std::to_string(guids[i]);
    return out + "]";
}

// Trailing fields of the ledger `party` line (AutoWowQuestLedger.h documents them).
inline std::string PartyFields(std::uint32_t id, std::vector<std::uint32_t> const& guids, std::vector<Role> const& roles,
                               std::uint32_t leader, Reason why, std::uint32_t dungeonMap, std::uint64_t ageMs)
{
    std::string r = "[";
    for (std::size_t i = 0; i < roles.size(); ++i)
        r += std::string(i ? "," : "") + "\"" + RoleName(roles[i]) + "\"";
    r += "]";
    return ",\"pid\":" + std::to_string(id) + ",\"members\":" + GuidList(guids) + ",\"roles\":" + r +
           ",\"leader\":" + std::to_string(leader) + ",\"why\":\"" + ReasonName(why) + "\",\"dmap\":" +
           std::to_string(dungeonMap) + ",\"age_ms\":" + std::to_string(ageMs);
}

// Trailing fields of the ledger `dungeon` line.
inline std::string DungeonFields(std::uint32_t id, std::uint32_t map, std::uint32_t instance, std::int32_t encounter,
                                 std::uint32_t mask, std::uint32_t allMask, std::uint64_t durMs,
                                 std::vector<std::uint32_t> const& guids)
{
    return ",\"pid\":" + std::to_string(id) + ",\"dmap\":" + std::to_string(map) + ",\"inst\":" +
           std::to_string(instance) + ",\"enc\":" + std::to_string(encounter) + ",\"mask\":" + std::to_string(mask) +
           ",\"all\":" + std::to_string(allMask) + ",\"dur_ms\":" + std::to_string(durMs) + ",\"members\":" +
           GuidList(guids);
}

// Extra trailing fields of a recruited run's `summary` row (after DungeonFields): bosses / total over the
// clearable encounters, completed and wiped as 0|1, member deaths during the run.
inline std::string RunSummaryFields(std::uint32_t bosses, std::uint32_t total, bool completed, bool wiped,
                                    std::uint32_t deaths)
{
    return ",\"bosses\":" + std::to_string(bosses) + ",\"total\":" + std::to_string(total) + ",\"completed\":" +
           (completed ? "1" : "0") + ",\"wipes\":" + (wiped ? "1" : "0") + ",\"deaths\":" + std::to_string(deaths) +
           ",\"recruit\":1";
}

// ---- material-crew migration ---------------------------------------------------------------------------

// Material-crew driver repair is a world-thread-only, fail-closed maintenance operation. The caller folds
// every configured member's live facts into guardMask; equality with kMaterialDriverAllGuards is intentional,
// so an unknown newer guard/version cannot accidentally admit mutation.
inline constexpr std::uint8_t kMaterialDriverPolicyVersion = 1;

enum MaterialDriverGuard : std::uint32_t
{
    MaterialDriverPlayerAi = 1u << 0,
    MaterialDriverInWorld = 1u << 1,
    MaterialDriverAlive = 1u << 2,
    MaterialDriverIndependent = 1u << 3,
    MaterialDriverNotReal = 1u << 4,
    MaterialDriverNoRealMaster = 1u << 5,
    MaterialDriverMasterNull = 1u << 6,
    MaterialDriverNotPaused = 1u << 7,
    MaterialDriverNotOracle = 1u << 8,
    MaterialDriverNotProbe = 1u << 9,
    MaterialDriverNotWorker = 1u << 10,
    MaterialDriverOpenWorld = 1u << 11,
    MaterialDriverNotBattleground = 1u << 12,
    MaterialDriverNoTransport = 1u << 13,
    MaterialDriverNotFlying = 1u << 14,
    MaterialDriverNotCombat = 1u << 15,
    MaterialDriverNotTeleporting = 1u << 16,
    MaterialDriverNotCasting = 1u << 17,
    MaterialDriverNoMotion = 1u << 18,
    MaterialDriverNoZoneProgression = 1u << 19,
    MaterialDriverNoErrand = 1u << 20,
    MaterialDriverNoQuest = 1u << 21,
    MaterialDriverNoRecovery = 1u << 22,
    MaterialDriverNoTravelStrategy = 1u << 23,
    MaterialDriverRpgIdle = 1u << 24,
    MaterialDriverNoMoveFar = 1u << 25,
    MaterialDriverTravelIntentValid = 1u << 26,
    MaterialDriverTravelIntentIdle = 1u << 27,
    MaterialDriverNoCampaignTravel = 1u << 28
};

inline constexpr std::uint32_t kMaterialDriverAllGuards = (1u << 29) - 1u;
inline constexpr std::uint32_t kMaterialDriverCustodyGuards =
    MaterialDriverPlayerAi | MaterialDriverInWorld | MaterialDriverIndependent | MaterialDriverNotReal |
    MaterialDriverNoRealMaster | MaterialDriverMasterNull | MaterialDriverNotOracle | MaterialDriverNotProbe |
    MaterialDriverNotWorker;
inline constexpr std::uint32_t kMaterialDriverTargetGuards = kMaterialDriverAllGuards & ~kMaterialDriverCustodyGuards;

[[nodiscard]] inline constexpr bool MaterialDriverCustodyReady(std::uint32_t guardMask)
{
    return (guardMask & kMaterialDriverCustodyGuards) == kMaterialDriverCustodyGuards;
}

struct MaterialDriverFacts
{
    std::uint8_t version = kMaterialDriverPolicyVersion;
    bool materialMode = false;
    bool registeredSquad = false;
    bool configuredRosterExact = false;
    bool nativeRosterExact = false;
    bool registeredLeaderExact = false;
    bool nativeLeaderExact = false;
    bool ordinaryGroup = false;
    std::uint32_t guardMask = 0;
    bool strategiesExact = false;
};

enum class MaterialDriverDecision : std::uint8_t
{
    Disabled = 0,
    Defer,
    Noop,
    Repair
};

[[nodiscard]] inline constexpr MaterialDriverDecision DecideMaterialDriver(MaterialDriverFacts const& facts)
{
    if (!facts.materialMode)
        return MaterialDriverDecision::Disabled;
    if (facts.version != kMaterialDriverPolicyVersion || !facts.registeredSquad || !facts.configuredRosterExact ||
        !facts.nativeRosterExact || !facts.registeredLeaderExact || !facts.nativeLeaderExact || !facts.ordinaryGroup ||
        facts.guardMask != kMaterialDriverAllGuards)
        return MaterialDriverDecision::Defer;
    return facts.strategiesExact ? MaterialDriverDecision::Noop : MaterialDriverDecision::Repair;
}

struct MaterialDriverDelta
{
    bool addGrind = false;
    bool addNewRpg = false;
    bool removeFollow = false;
    [[nodiscard]] constexpr bool Any() const { return addGrind || addNewRpg || removeFollow; }
};

[[nodiscard]] inline constexpr MaterialDriverDelta PlanMaterialDriverDelta(bool hasGrind, bool hasNewRpg,
                                                                           bool hasFollow)
{
    return {!hasGrind, !hasNewRpg, hasFollow};
}

[[nodiscard]] inline std::uint64_t MaterialDriverRosterHash(std::vector<std::uint32_t> const& roster)
{
    std::vector<std::uint32_t> sorted = roster;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t const guid : sorted)
    {
        hash ^= guid;
        hash *= 1099511628211ull;
    }
    return hash;
}

// The persisted legacy squad group may span successor material crews after a restart. Retirement is admitted only
// when the successors are an exact, exclusive partition of the expected legacy roster and at least two are nonempty.
[[nodiscard]] inline bool SameGuidSet(std::vector<std::uint32_t> left, std::vector<std::uint32_t> right)
{
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

[[nodiscard]] inline bool SplitSquadPlanMatches(std::vector<std::uint32_t> const& legacy,
                                                std::vector<std::vector<std::uint32_t>> const& successors)
{
    if (legacy.empty())
        return false;
    std::vector<std::uint32_t> joined;
    std::size_t nonempty = 0;
    for (auto const& roster : successors)
    {
        nonempty += roster.empty() ? 0 : 1;
        joined.insert(joined.end(), roster.begin(), roster.end());
    }
    std::vector<std::uint32_t> unique = joined;
    std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end())
        return false;
    return nonempty >= 2 && SameGuidSet(legacy, joined);
}

enum class SplitNativeShape : std::uint8_t
{
    Ungrouped = 0,
    ExactSuccessors,
    ExactLegacy,
    Foreign
};

enum class SplitRetireDecision : std::uint8_t
{
    Noop = 0,
    Defer,
    RetireOwned,
    RetireOrphan
};

[[nodiscard]] inline SplitRetireDecision DecideSplitRetire(bool planMatches, SplitNativeShape shape, bool ordinaryGroup,
                                                           bool safe, bool registeredOwnedSquad, bool orphan)
{
    if (!planMatches || !safe)
        return SplitRetireDecision::Defer;
    if (shape == SplitNativeShape::Ungrouped || shape == SplitNativeShape::ExactSuccessors)
        return SplitRetireDecision::Noop;
    if (shape != SplitNativeShape::ExactLegacy || !ordinaryGroup)
        return SplitRetireDecision::Defer;
    if (registeredOwnedSquad)
        return SplitRetireDecision::RetireOwned;
    return orphan ? SplitRetireDecision::RetireOrphan : SplitRetireDecision::Defer;
}

[[nodiscard]] inline bool ShouldDisbandOrphan(bool enabled, bool orphan, bool pendingSplit)
{
    return enabled && orphan && !pendingSplit;
}

// ---- runtime (AutoWow/PartyRuntime.cpp, leader walk in NewRpgParty.cpp) ------------------------------
namespace detail
{
inline bool gEnabled = false;   // AutoWow.Party.Enable
inline bool gDungeons = false;  // AutoWow.Dungeon.Enable (needs Party.Enable)
inline bool gRoles = false;     // AutoWow.Party.Roles
inline bool gRecruit = false;   // AutoWow.Dungeon.Recruit (needs Dungeon.Enable)
inline bool gRecruitWalk = false;  // AutoWow.Dungeon.RecruitStragglerWalk (needs Recruit)
}
inline bool RecruitWalkEnabled() { return detail::gRecruitWalk; }
inline bool Enabled() { return detail::gEnabled; }
inline bool RolesEnabled() { return detail::gRoles; }

// What a party leader's New-RPG tick does (NewRpgBaseAction::PartyStep).
struct LeaderOrder
{
    Phase phase = Phase::None;
    std::uint32_t map = 0;  // entrance (Approach)
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

void LoadConfig();
// World thread (Playerbots.cpp WorldScript::OnUpdate): formation, supervision, dungeon runs, ledger.
void WorldUpdate(std::uint32_t diff);
// Bot AI tick (map thread): tank taunt / healer heal overrides of a cohort party member in combat.
void CombatUpdate(PlayerbotAI* botAI);
// Roster size of the bot's cohort party (0 = none) and whether that party was formed for a dungeon.
std::uint32_t PartySize(std::uint32_t guid, bool* dungeonParty = nullptr);
// True when `guid` is a member of a recruited dungeon party (AutoWow.Dungeon.Recruit); any thread.
bool InRecruitedParty(std::uint32_t guid);
// Mutex-guarded registry snapshot: exact recruited Inside owner for this corpse map and witnessed instance.
bool OwnsDungeonRecovery(std::uint32_t guid, std::uint32_t corpseMap, std::uint32_t witnessGuid,
                         std::uint32_t witnessedInstance);
// Map-thread safe: queue retirement of guid's tracked open-world group-quest party on the world thread.
void RequestSurvivalLeave(std::uint32_t guid);
// True when `guid` leads a cohort party; fills its order.
bool GetLeaderOrder(std::uint32_t guid, LeaderOrder& out);
// One Approach walk tick of the leader (stuck = WalkLeg reported no progress).
void NoteApproachTick(std::uint32_t guid, bool stuck);
// Material-crews migration (world thread): retire only an exact safe legacy squad group which spans an exact
// successor partition. True means no such legacy group blocks successor EnsureSquad calls; false means defer.
bool RetireSplitSquad(std::vector<std::uint32_t> const& legacy,
                      std::vector<std::vector<std::uint32_t>> const& successors);
// AutoWow.Squad (world thread, any Party flag state): keep one team's squad roster (guid ascending) as one party
// (reason squad, leader = the lowest online guid, all dps, no follower mode: every member keeps its own New-RPG loop).
// Exempt from formation (its guids never join a formed party) and from supervision / the keep rules: a changed
// online roster or a broken core group dissolves it here and the online members (at least two) re-form it.
void EnsureSquad(std::vector<std::uint32_t> const& roster, bool materialCrew = false);
}  // namespace AutoWowParty

#endif  // AUTOWOW_PARTY_POLICY_H
