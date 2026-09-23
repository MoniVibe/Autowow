/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWCOHORTPOLICY_H
#define _PLAYERBOT_AUTOWOWCOHORTPOLICY_H

// Pure rules for the persistent bot cohort (docs/COHORT_PLAN.md C1/C2). No world access; unit-tested.

#include <cstdint>
#include <string>
#include <string_view>

namespace AutoWowCohortPolicy
{
// ---- C1: console `.autowow cohort create <account> <raceId> <classId> <gender> <name>` ----

struct CreateArgs
{
    std::string account;
    std::uint32_t race = 0;
    std::uint32_t cls = 0;
    std::uint32_t gender = 0;
    std::string name;
};

enum class CreateRefusal : std::uint8_t
{
    None = 0,
    BadRace,        // not a 3.3.5 playable race id
    BadClass,       // not a 3.3.5 playable class id
    BadGender,      // only 0 (male) / 1 (female)
    RndbotAccount,  // account name carries the random-bot pool prefix
};

// 3.3.5 playable ids: races 1-8,10,11 (9 = goblin is NPC-only); classes 1-9,11 (10 unused).
inline constexpr std::uint32_t kPlayableRaceMask = 0x6FFu;
inline constexpr std::uint32_t kPlayableClassMask = 0x5FFu;

inline constexpr bool IsPlayableRaceId(std::uint32_t race)
{
    return race >= 1 && race <= 11 && ((kPlayableRaceMask >> (race - 1)) & 1u);
}

inline constexpr bool IsPlayableClassId(std::uint32_t cls)
{
    return cls >= 1 && cls <= 11 && ((kPlayableClassMask >> (cls - 1)) & 1u);
}

// Exactly five whitespace-separated tokens; race/class/gender are plain decimal (at most 3 digits).
inline bool ParseCreateArgs(std::string_view args, CreateArgs& out)
{
    std::string_view tokens[5];
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < args.size())
    {
        while (i < args.size() && (args[i] == ' ' || args[i] == '\t'))
            ++i;
        if (i == args.size())
            break;
        std::size_t const start = i;
        while (i < args.size() && args[i] != ' ' && args[i] != '\t')
            ++i;
        if (count == 5)
            return false;
        tokens[count++] = args.substr(start, i - start);
    }
    if (count != 5)
        return false;

    std::uint32_t numbers[3] = {};
    for (std::size_t t = 0; t < 3; ++t)
    {
        std::string_view const token = tokens[t + 1];
        if (token.empty() || token.size() > 3)
            return false;
        for (char const c : token)
        {
            if (c < '0' || c > '9')
                return false;
            numbers[t] = numbers[t] * 10 + static_cast<std::uint32_t>(c - '0');
        }
    }

    out.account = std::string(tokens[0]);
    out.race = numbers[0];
    out.cls = numbers[1];
    out.gender = numbers[2];
    out.name = std::string(tokens[4]);
    return true;
}

inline constexpr bool StartsWithIgnoreAsciiCase(std::string_view text, std::string_view prefix)
{
    if (prefix.size() > text.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        char a = text[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

// Argument-only refusals, checked before any database or world lookup. An empty random-bot prefix
// never matches (no pool configured). Race/class *combination* legality is the runtime PlayerInfo
// check (RandomPlayerbotFactory::IsValidRaceClassCombination), not this function.
inline constexpr CreateRefusal CheckCreateArgs(CreateArgs const& args, std::string_view rndbotPrefix)
{
    if (!IsPlayableRaceId(args.race))
        return CreateRefusal::BadRace;
    if (!IsPlayableClassId(args.cls))
        return CreateRefusal::BadClass;
    if (args.gender > 1)
        return CreateRefusal::BadGender;
    if (!rndbotPrefix.empty() && StartsWithIgnoreAsciiCase(args.account, rndbotPrefix))
        return CreateRefusal::RndbotAccount;
    return CreateRefusal::None;
}

inline constexpr char const* CreateRefusalName(CreateRefusal refusal)
{
    switch (refusal)
    {
        case CreateRefusal::None: return "none";
        case CreateRefusal::BadRace: return "race_invalid";
        case CreateRefusal::BadClass: return "class_invalid";
        case CreateRefusal::BadGender: return "gender_invalid";
        case CreateRefusal::RndbotAccount: return "rndbot_account";
    }
    return "unknown";
}

// ---- C2: AutoWow.Independent.AutoMaintenance (0 = legacy, 1 = talents, 2 = talents + spells) ----

// Only bots that the random pool and the Oracle allowlist do not already maintain, and only while
// in AutoWoW independent mode (masterless solo autonomy, e.g. the cohort). Never gear/items/money.
inline constexpr bool IsIndependentMaintenanceEligible(bool randomBot, bool oracleManaged, bool independent)
{
    return !randomBot && !oracleManaged && independent;
}

inline constexpr bool IndependentAutoTalents(std::uint32_t mode, bool eligible)
{
    return mode >= 1 && eligible;
}

inline constexpr bool IndependentAutoSpells(std::uint32_t mode, bool eligible)
{
    return mode >= 2 && eligible;
}
}  // namespace AutoWowCohortPolicy

#endif
