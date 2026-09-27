/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWTRAINPOLICY_H
#define _PLAYERBOT_AUTOWOWTRAINPOLICY_H

// Pure rules for cohort professions (docs/PROFESSIONS_PLAN.md P1). No world access; unit-tested.
//
// AutoWow.Professions.Assignments wire format (rendered by scripts/cohort/profession-assign.ps1 from
// the cohort manifest v2):  "<guid>:<skill>[,<skill>];<guid>:<skill>[,<skill>];..."
//   - guid: character guid-low, decimal, non-zero, each guid at most once.
//   - skill: one or two distinct primary profession skill line ids (SkillLine.dbc).
//   - ';' separates entries; empty entries (a trailing ';') are ignored; no whitespace.
// Any malformed entry rejects the whole value (fail closed: no bot gets a plan).
// AutoWow.Professions.Secondaries: "<skill>[,<skill>...]" from {129 First Aid, 185 Cooking, 356 Fishing}.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AutoWowTrainPolicy
{
inline constexpr bool IsPrimaryProfessionSkillLine(std::uint32_t skill)
{
    switch (skill)
    {
        case 164:  // Blacksmithing
        case 165:  // Leatherworking
        case 171:  // Alchemy
        case 182:  // Herbalism
        case 186:  // Mining
        case 197:  // Tailoring
        case 202:  // Engineering
        case 333:  // Enchanting
        case 393:  // Skinning
        case 755:  // Jewelcrafting
        case 773:  // Inscription
            return true;
        default:
            return false;
    }
}

inline constexpr bool IsSecondaryProfessionSkillLine(std::uint32_t skill)
{
    return skill == 129 || skill == 185 || skill == 356;
}

// The 14 profession lines the faction KPI sums.
inline constexpr bool IsProfessionSkillLine(std::uint32_t skill)
{
    return IsPrimaryProfessionSkillLine(skill) || IsSecondaryProfessionSkillLine(skill);
}

using AssignmentMap = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;

namespace detail
{
// Non-empty decimal, at most 10 digits, fits uint32.
inline bool ParseU32(std::string_view text, std::uint32_t& out)
{
    if (text.empty() || text.size() > 10)
        return false;
    std::uint64_t value = 0;
    for (char const c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFull)
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

// Comma list of distinct ids, each accepted by `allowed`, count in [1, maxCount].
inline bool ParseSkillList(std::string_view text, bool (*allowed)(std::uint32_t), std::size_t maxCount,
                           std::vector<std::uint32_t>& out)
{
    out.clear();
    std::size_t start = 0;
    while (true)
    {
        std::size_t const comma = text.find(',', start);
        std::string_view const token =
            text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        std::uint32_t skill = 0;
        if (!ParseU32(token, skill) || !allowed(skill) || out.size() == maxCount)
            return false;
        for (std::uint32_t const seen : out)
            if (seen == skill)
                return false;
        out.push_back(skill);
        if (comma == std::string_view::npos)
            return true;
        start = comma + 1;
    }
}

inline bool AllowPrimary(std::uint32_t skill) { return IsPrimaryProfessionSkillLine(skill); }
inline bool AllowSecondary(std::uint32_t skill) { return IsSecondaryProfessionSkillLine(skill); }
}  // namespace detail

// Empty text = no assignments (valid). On failure `out` is left empty.
inline bool ParseAssignments(std::string_view text, AssignmentMap& out)
{
    out.clear();
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const semi = text.find(';', start);
        std::string_view const entry =
            text.substr(start, semi == std::string_view::npos ? std::string_view::npos : semi - start);
        if (!entry.empty())
        {
            std::size_t const colon = entry.find(':');
            std::uint32_t guid = 0;
            std::vector<std::uint32_t> skills;
            if (colon == std::string_view::npos || !detail::ParseU32(entry.substr(0, colon), guid) || !guid ||
                out.count(guid) ||
                !detail::ParseSkillList(entry.substr(colon + 1), detail::AllowPrimary, 2, skills))
            {
                out.clear();
                return false;
            }
            out.emplace(guid, std::move(skills));
        }
        if (semi == std::string_view::npos)
            break;
        start = semi + 1;
    }
    return true;
}

// Empty text = no secondaries (valid). On failure `out` is left empty.
inline bool ParseSecondaries(std::string_view text, std::vector<std::uint32_t>& out)
{
    if (text.empty())
    {
        out.clear();
        return true;
    }
    if (detail::ParseSkillList(text, detail::AllowSecondary, 3, out))
        return true;
    out.clear();
    return false;
}

inline bool Contains(std::vector<std::uint32_t> const& list, std::uint32_t skill)
{
    for (std::uint32_t const s : list)
        if (s == skill)
            return true;
    return false;
}

// Learn filter for a planned bot at a trainer. `grantedSkill` is the skill line the trainer spell
// grants (0 = none: a class spell or a recipe). Only the first learn of a profession line the bot
// does not have yet is filtered: rank-ups and recipes of a line it already has always pass (their
// cost is still paid in gold). Without this the core teaches whatever primary the trainer offers
// into a free slot (Trainer::CanTeachSpell only checks the free primary count).
inline bool AllowSkillGrant(std::uint32_t grantedSkill, bool alreadyHasSkill,
                            std::vector<std::uint32_t> const& primaries, std::vector<std::uint32_t> const& secondaries)
{
    if (!grantedSkill || !IsProfessionSkillLine(grantedSkill) || alreadyHasSkill)
        return true;
    return Contains(primaries, grantedSkill) || Contains(secondaries, grantedSkill);
}

// The 11 primary lines of IsPrimaryProfessionSkillLine, ascending (the order a bot's lines are checked).
inline constexpr std::uint32_t kPrimaryProfessionSkillLines[] = {164, 165, 171, 182, 186, 197,
                                                                 202, 333, 393, 755, 773};

// AutoWow.Professions.DropOffPlan: the primaries a planned bot unlearns at login, in `known` order —
// every known primary line not in its plan. A configured supply artisan keeps all of its lines (its
// house recipes are learned outside the plan).
inline std::vector<std::uint32_t> OffPlanPrimaries(std::vector<std::uint32_t> const& known,
                                                   std::vector<std::uint32_t> const& plan, bool artisan)
{
    std::vector<std::uint32_t> drop;
    if (artisan)
        return drop;
    for (std::uint32_t const skill : known)
        if (IsPrimaryProfessionSkillLine(skill) && !Contains(plan, skill))
            drop.push_back(skill);
    return drop;
}

// "AutoWow.Supply.Artisan.<House>.<Alliance|Horde>" names an artisan guid; the ".Learn[.<line>]"
// recipe-list keys under the same prefix do not.
inline bool IsArtisanConfigKey(std::string_view key)
{
    constexpr std::string_view prefix = "AutoWow.Supply.Artisan.";
    if (key.substr(0, prefix.size()) != prefix)
        return false;
    std::string_view const rest = key.substr(prefix.size());
    std::size_t const dot = rest.find('.');
    if (dot == std::string_view::npos || dot == 0 || rest.substr(0, dot) == "Learn")
        return false;
    std::string_view const team = rest.substr(dot + 1);
    return team == "Alliance" || team == "Horde";
}
}  // namespace AutoWowTrainPolicy

#endif
