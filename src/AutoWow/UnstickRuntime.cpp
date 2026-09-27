/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Unstick.V2 runtime (policy: UnstickPolicy.h). Per-bot tables are mutex-guarded (bot AI and unit hooks
// run on map threads) and touched only with the flag on; the lock is never held while calling into the core.

#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ThreatManager.h"
#include "UnstickPolicy.h"
#include "WorldSession.h"

namespace AutoWowUnstickV2
{
namespace
{
constexpr std::uint64_t kCombatCheckMs = 5000;

struct Combat
{
    std::uint64_t combatSinceMs = 0;  // 0 = not in combat at the last look
    std::uint64_t activityMs = 0;     // last damage dealt / taken / healing
    std::uint64_t nextCheckMs = 0;
};

std::mutex gLock;
std::unordered_map<std::uint32_t, XpWatch> gXp;
std::unordered_map<std::uint32_t, std::uint64_t> gGaveUpMs;
std::unordered_map<std::uint32_t, AgeBook> gAges;
std::unordered_map<std::uint32_t, Combat> gCombat;
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Unstick.V2", false);
    Params& p = detail::gParams;
    p.grindMaxYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.GrindMaxYards", 1000);
    p.noXpMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.NoXpMs", 1800000);
    p.logTrimAbove = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.LogTrimAbove", 15);
    p.logTrimStaleMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.LogTrimStaleMs", 3600000);
    p.combatStallMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.CombatStallMs", 180000);
    p.partyStallMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.StallMs", 1200000);
    p.strandSkipMin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.StrandSkipGuidMin", 72385);
    p.strandSkipMax = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Unstick.StrandSkipGuidMax", 72394);
    if (detail::gEnabled)
        LOG_INFO("server.loading", ">> AutoWow.Unstick.V2: grind_max={} no_xp_ms={} trim_above={} trim_stale_ms={} "
                 "combat_stall_ms={} party_stall_ms={} strand_skip={}-{}", p.grindMaxYards, p.noXpMs, p.logTrimAbove,
                 p.logTrimStaleMs, p.combatStallMs, p.partyStallMs, p.strandSkipMin, p.strandSkipMax);
}

bool XpStalled(Player* bot, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    return NoteXp(gXp[bot->GetGUID().GetCounter()], bot->GetLevel(), bot->GetUInt32Value(PLAYER_XP), nowMs,
                  detail::gParams.noXpMs);
}

void NoteGaveUp(std::uint32_t guid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    gGaveUpMs[guid] = nowMs;
}

bool GaveUpSince(std::uint32_t guid, std::uint64_t sinceMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gGaveUpMs.find(guid);
    return it != gGaveUpMs.end() && it->second >= sinceMs;
}

std::vector<QuestAge> ObserveAges(std::uint32_t guid, std::vector<QuestSchedulerPolicy::Observation> const& log,
                                  std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    AgeBook& book = gAges[guid];
    book.Observe(log, nowMs);
    return book.entries;
}

void NoteCombatActivity(std::uint32_t guid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    gCombat[guid].activityMs = nowMs;
}

void CombatWatch(Player* bot, std::uint64_t nowMs)
{
    Params const& p = detail::gParams;
    if (!p.combatStallMs)
        return;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    bool const inCombat = bot->IsAlive() && bot->IsInCombat();
    std::uint64_t since = 0;
    std::uint64_t activity = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!inCombat)
        {
            auto const it = gCombat.find(guid);
            if (it != gCombat.end())
                it->second.combatSinceMs = 0;
            return;
        }
        Combat& c = gCombat[guid];
        if (!c.combatSinceMs)
            c.combatSinceMs = nowMs ? nowMs : 1;
        if (nowMs < c.nextCheckMs)
            return;
        c.nextCheckMs = nowMs + kCombatCheckMs;
        since = c.combatSinceMs;
        activity = c.activityMs;
    }
    if (!CombatStalled(since, activity, nowMs, p.combatStallMs))
        return;
    Map* map = bot->GetMap();
    if (!map || bot->InBattleground() || map->IsBattlegroundOrArena() || AutoWowOracleRuntime::IsManagedBot(guid))
        return;
    if (InstanceMap* instance = map->ToInstanceMap())
        if (InstanceScript* script = instance->GetInstanceScript(); script && script->IsEncounterInProgress())
            return;  // a boss fight: the dungeon's own wipe / reset rules apply
    Unit const* victim = bot->GetVictim();
    LOG_INFO("playerbots", "[Unstick] combat_stall bot={} lvl={} combat_ms={} idle_ms={} attackers={} victim={} map={} "
             "zone={} x={} y={}", bot->GetName(), bot->GetLevel(), nowMs - since,
             nowMs - std::max(since, activity), bot->getAttackers().size(), victim ? victim->GetEntry() : 0,
             bot->GetMapId(), bot->GetZoneId(), std::int32_t(bot->GetPositionX()), std::int32_t(bot->GetPositionY()));
    bot->GetThreatMgr().RemoveMeFromThreatLists();
    bot->CombatStopWithPets(true);
    std::lock_guard<std::mutex> guard(gLock);
    gCombat[guid] = Combat{};
}

std::uint32_t TaxiFare(std::vector<std::uint32_t> const& nodes)
{
    std::uint32_t fare = 0;
    for (std::size_t k = 1; k < nodes.size(); ++k)
    {
        std::uint32_t path = 0;
        std::uint32_t cost = 0;
        sObjectMgr->GetTaxiPath(nodes[k - 1], nodes[k], path, cost);
        if (!path)
            return 0;
        fare += cost;
    }
    return fare;
}

char const* TaxiFailReason(Player* bot, std::vector<std::uint32_t> const& nodes, Creature* npc)
{
    if (nodes.size() < 2)
        return "short_path";
    if (bot->GetSession()->isLogingOut() || bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_STUNNED) ||
        bot->HasUnitState(UNIT_STATE_ROOT))
        return "busy";
    if (bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "disable_move";
    if (npc && bot->IsMounted())
        return "mounted";
    if (npc && bot->IsInDisallowedMountForm())
        return "shapeshift";
    if (npc && bot->IsNonMeleeSpellCast(false))
        return "casting";
    if (!sTaxiNodesStore.LookupEntry(nodes[0]))
        return "no_node";
    std::uint32_t const fare = TaxiFare(nodes);
    if (!fare)
        return "no_segment";
    if (!sObjectMgr->GetTaxiMountDisplayId(nodes[0], bot->GetTeamId(true), npc == nullptr))
        return "no_mount";
    if (bot->GetMoney() < fare)
        return "money";
    return "unknown";
}
}  // namespace AutoWowUnstickV2
