/*
 * This file is part of the mod-playerbots module for AzerothCore.
 */

#include "ProbeResetControl.h"
#include "ProbeResetPolicy.h"

#include "CombatManager.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace AutoWowProbeReset
{
namespace
{
std::string JsonString(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                    out << "?";
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

std::string GuidArray(std::vector<uint32> const& guids)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t index = 0; index < guids.size(); ++index)
    {
        if (index)
            out << ',';
        out << guids[index];
    }
    out << ']';
    return out.str();
}

std::string Response(bool ok, char const* status, char const* code, std::string const& route,
                     uint32 targetMap, uint32 targetDifficulty, std::vector<uint32> const& roster,
                     std::vector<uint32> const& pending, uint32 memberGuid = 0,
                     uint32 staleCombatCleared = 0, uint32 groupsDisbanded = 0,
                     uint32 temporaryBindsCleared = 0, uint32 resurrected = 0,
                     uint32 staged = 0)
{
    std::ostringstream out;
    out << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"operation\":\"probe_reset\",\"status\":" << JsonString(status)
        << ",\"code\":" << JsonString(code)
        << ",\"route\":" << JsonString(route)
        << ",\"target_map\":" << targetMap
        << ",\"target_difficulty\":" << targetDifficulty
        << ",\"roster\":" << GuidArray(roster)
        << ",\"pending\":" << GuidArray(pending)
        << ",\"member_guid\":" << memberGuid
        << ",\"cleared_stale_combat\":" << staleCombatCleared
        << ",\"disbanded_groups\":" << groupsDisbanded
        << ",\"cleared_temporary_binds\":" << temporaryBindsCleared
        << ",\"resurrected\":" << resurrected
        << ",\"staged\":" << staged << '}';
    return out.str();
}

bool AtRoute(Player const* player, ExteriorRoute const& route)
{
    if (player->GetMapId() != route.map)
        return false;
    float const dx = player->GetPositionX() - route.x;
    float const dy = player->GetPositionY() - route.y;
    float const dz = player->GetPositionZ() - route.z;
    return dx * dx + dy * dy + dz * dz <= 1.0f;
}
}

std::string Reset(std::string const& exteriorRoute, uint32 targetMap, uint32 targetDifficulty,
                  std::vector<uint32> const& roster)
{
    ExteriorRoute const* route = ResolveExteriorRoute(exteriorRoute);
    if (!route)
        return Response(false, "REFUSED", "exterior_route_not_allowlisted", exteriorRoute,
                        targetMap, targetDifficulty, roster, {});
    if (!targetMap || targetDifficulty >= MAX_DIFFICULTY || roster.empty())
        return Response(false, "REFUSED", "invalid_request", exteriorRoute,
                        targetMap, targetDifficulty, roster, {});

    std::set<uint32> rosterSet;
    for (uint32 guid : roster)
        if (!guid || !rosterSet.insert(guid).second)
            return Response(false, "REFUSED", "invalid_roster", exteriorRoute,
                            targetMap, targetDifficulty, roster, {}, guid);

    std::vector<Player*> players;
    std::vector<uint32> offline;
    players.reserve(roster.size());
    for (uint32 guid : roster)
    {
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!player)
            offline.push_back(guid);
        else
            players.push_back(player);
    }
    // Never partially reset a roster. Activation is owned by the caller; retry when every exact
    // requested member is online.
    if (!offline.empty())
        return Response(true, "PENDING", "members_not_online", exteriorRoute,
                        targetMap, targetDifficulty, roster, offline);

    std::vector<Group*> groups;
    std::vector<Player*> staleCombat;
    std::vector<Player*> temporaryBinds;
    std::vector<Player*> deadPlayers;
    std::vector<uint32> stagingCombat;
    bool const allAtRoute = std::all_of(players.begin(), players.end(),
        [route](Player const* player) { return AtRoute(player, *route); });
    for (Player* player : players)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        if (!player->IsAlive())
            deadPlayers.push_back(player);
        if (player->IsInFlight())
            return Response(false, "REFUSED", "in_flight", exteriorRoute,
                            targetMap, targetDifficulty, roster, {}, guid);
        if (player->IsBeingTeleported())
            return Response(false, "REFUSED", "teleport_in_progress", exteriorRoute,
                            targetMap, targetDifficulty, roster, {}, guid);
        if (player->GetCharmerGUID() || player->GetCharmGUID())
            return Response(false, "REFUSED", "charmed_or_charming", exteriorRoute,
                            targetMap, targetDifficulty, roster, {}, guid);

        CombatSignals const combat{
            player->IsInCombat(), player->GetVictim() != nullptr, !player->getAttackers().empty(),
            player->GetCombatManager().HasCombat(), player->IsNonMeleeSpellCast(false)};
        CombatPolicy const combatPolicy = ClassifyCombat(combat);
        if (combatPolicy == CombatPolicy::RefuseActive)
        {
            // Exterior staging can briefly aggro a nearby ambient creature. Do not clear or
            // relocate genuine combat, but let the bounded caller poll until it naturally ends.
            // Active combat anywhere other than the exact allowlisted staging point remains a
            // hard refusal.
            if (!allAtRoute)
                return Response(false, "REFUSED", "active_combat", exteriorRoute,
                                targetMap, targetDifficulty, roster, {}, guid);
            stagingCombat.push_back(guid);
        }
        if (combatPolicy == CombatPolicy::ClearStaleFlag)
            staleCombat.push_back(player);

        if (Group* group = player->GetGroup())
        {
            bool foreignMember = false;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (!rosterSet.contains(slot.guid.GetCounter()))
                    foreignMember = true;
            GroupSignals const groupState{
                group->isBGGroup(), group->isBFGroup(), group->isLFGGroup(), foreignMember};
            if (!MayDisbandGroup(groupState))
            {
                char const* code = groupState.battleground ? "battleground_group" :
                    groupState.battlefield ? "battlefield_group" :
                    groupState.lfg ? "lfg_group" : "foreign_group_member";
                return Response(false, "REFUSED", code, exteriorRoute,
                                targetMap, targetDifficulty, roster, {}, guid);
            }
            if (std::find(groups.begin(), groups.end(), group) == groups.end())
                groups.push_back(group);
        }

        InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(
            player->GetGUID(), targetMap, Difficulty(targetDifficulty));
        BindPolicy const bindPolicy = ClassifyTargetBind(bind != nullptr, bind && bind->perm);
        if (bindPolicy == BindPolicy::RefusePermanent)
            return Response(false, "REFUSED", "permanent_target_bind", exteriorRoute,
                            targetMap, targetDifficulty, roster, {}, guid);
        if (bindPolicy == BindPolicy::ClearTemporary)
            temporaryBinds.push_back(player);
    }

    if (!stagingCombat.empty())
        return Response(true, "PENDING", "active_combat_at_staging", exteriorRoute,
                        targetMap, targetDifficulty, roster, stagingCombat);

    // All refusal checks precede mutation so one unsafe member leaves the entire roster untouched.
    // A previous serial portal admission may have intentionally paused a follower. Reset is the
    // idempotent boundary between probe runs, so no roster member may carry that transient pause
    // into a later run (especially when that former follower becomes the next party leader).
    for (Player* player : players)
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player))
            ai->SetAutoWowPaused(false);
    for (Player* player : deadPlayers)
    {
        player->ResurrectPlayer(1.0f, false);
        player->SpawnCorpseBones();
    }
    for (Player* player : staleCombat)
        player->ClearInCombat();
    for (Group* group : groups)
        group->Disband();
    for (Player* player : temporaryBinds)
        sInstanceSaveMgr->PlayerUnbindInstance(
            player->GetGUID(), targetMap, Difficulty(targetDifficulty), true, player);

    std::vector<uint32> pending;
    uint32 staged = 0;
    for (Player* player : players)
    {
        if (AtRoute(player, *route))
            continue;
        if (!player->TeleportTo(route->map, route->x, route->y, route->z, route->orientation))
            return Response(false, "REFUSED", "exterior_staging_failed", exteriorRoute,
                            targetMap, targetDifficulty, roster, pending,
                            player->GetGUID().GetCounter(), staleCombat.size(), groups.size(),
                            temporaryBinds.size(), deadPlayers.size(), staged);
        pending.push_back(player->GetGUID().GetCounter());
        ++staged;
    }

    if (!pending.empty())
        return Response(true, "PENDING", "staging_pending", exteriorRoute,
                        targetMap, targetDifficulty, roster, pending, 0,
                        staleCombat.size(), groups.size(), temporaryBinds.size(),
                        deadPlayers.size(), staged);
    return Response(true, "READY", "ready", exteriorRoute,
                    targetMap, targetDifficulty, roster, {}, 0,
                    staleCombat.size(), groups.size(), temporaryBinds.size(),
                    deadPlayers.size(), staged);
}
} // namespace AutoWowProbeReset
