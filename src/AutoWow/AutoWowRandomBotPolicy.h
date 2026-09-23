/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWRANDOMBOTPOLICY_H
#define _PLAYERBOT_AUTOWOWRANDOMBOTPOLICY_H

// Pure random-bot setup rules for AutoWoW realms (no world access; unit-tested).

#include <cstdint>
#include <string_view>

namespace AutoWowRandomBotPolicy
{
// RandomPlayerbotMgr forces every random bot's PvP flag to sWorld->IsPvPRealm() at login and on
// refresh/revive. On a PvP realm that flags bots everywhere, overriding the core zone rules
// (hostile/contested territory flags, own territory does not). With
// AutoWow.PvpRealmZoneRules.Enable the forced write is skipped on PvP realms only; PvE realms keep
// the legacy SetPvP(false), and flag off keeps the legacy write on every realm type.
inline constexpr bool ShouldForceRandomBotPvpFlag(bool pvpRealm, bool zoneRulesEnabled)
{
    return !(pvpRealm && zoneRulesEnabled);
}

// Membership in a comma/whitespace separated decimal GUID list (AutoWow.OracleRuntime.BotGuids,
// AutoWow.FixtureGuids, AutoWow.MicroScenarioGuids). Fail-safe for an exclusion list: any token that
// is not a plain decimal number makes the whole list match, so a typo never exposes a listed bot.
inline bool GuidListContains(std::string_view list, std::uint32_t guid)
{
    std::size_t i = 0;
    while (i < list.size())
    {
        char const c = list[i];
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++i;
            continue;
        }
        std::uint64_t value = 0;
        std::size_t digits = 0;
        for (; i < list.size() && list[i] >= '0' && list[i] <= '9'; ++i, ++digits)
        {
            value = value * 10 + static_cast<std::uint64_t>(list[i] - '0');
            if (value > 0xFFFFFFFFull)
                return true;
        }
        bool const tokenEnds = i == list.size() || list[i] == ',' || list[i] == ' ' || list[i] == '\t' ||
                               list[i] == '\r' || list[i] == '\n';
        if (!digits || !tokenEnds)
            return true;
        if (value == guid)
            return true;
    }
    return false;
}

// AutoWow.Soak.RerollAboveMaxLevel: a random-pool bot logging in above RandomBotMaxLevel (legacy
// high-level pool entries whose randomize timers lie past the soak) is re-rolled once through the
// normal RandomizeFirst level path. Never for non-random bots or listed Oracle/fixture/micro GUIDs.
inline constexpr bool ShouldRerollAboveMaxLevel(bool enabled, bool randomBot, bool listedGuid,
                                                std::uint32_t level, std::uint32_t maxLevel)
{
    return enabled && randomBot && !listedGuid && level > maxLevel;
}
}  // namespace AutoWowRandomBotPolicy

#endif
