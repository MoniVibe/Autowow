/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWRANDOMBOTPOLICY_H
#define _PLAYERBOT_AUTOWOWRANDOMBOTPOLICY_H

// Pure random-bot setup rules for AutoWoW realms (no world access; unit-tested).

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
}  // namespace AutoWowRandomBotPolicy

#endif
