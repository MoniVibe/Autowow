/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Unstick.V2 runtime (policy: UnstickPolicy.h). Per-bot tables are mutex-guarded (bot AI and unit hooks
// run on map threads) and touched only with the flag on; the lock is never held while calling into the core.

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "AiObjectContext.h"
#include "AutoWowGuildsPolicy.h"
#include "AutoWowOracleRuntime.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GameTime.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
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
    std::uint64_t incomingMs = 0;     // incoming damage is productive progress
    std::uint64_t nextCheckMs = 0;
    ProductiveCombatWatch productive;
    ProductiveCombatWatch staleTarget;
    ProactiveRetryBackoff proactiveRetry;
};

std::mutex gLock;
std::unordered_map<std::uint32_t, XpWatch> gXp;
std::unordered_map<std::uint32_t, std::uint64_t> gGaveUpMs;
std::unordered_map<std::uint32_t, AgeBook> gAges;
std::unordered_map<std::uint32_t, Combat> gCombat;
std::vector<AutoWowGuilds::GuidRange> gCohort;
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
    gCohort.clear();
    std::string const cohort =
        sConfigMgr->GetOption<std::string>("AutoWow.Guilds.CohortGuids", "62955-63004");
    if (!AutoWowGuilds::ParseGuidRanges(cohort, gCohort))
        LOG_ERROR("server.loading", ">> AutoWow.Unstick.V2: invalid cohort guid list '{}'; productive watchdog off",
                  cohort);
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

void NoteCombatIncomingDamage(std::uint32_t guid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gLock);
    Combat& c = gCombat[guid];
    c.activityMs = nowMs;
    c.incomingMs = nowMs;
}

bool ProactiveRetryBlocked(Player* bot, PlayerbotAI* ai, std::uint64_t target)
{
    if (!bot || !ai || !target)
        return false;

    std::uint32_t const guid = bot->GetGUID().GetCounter();
    Map* const map = bot->GetMap();
    ProductiveCombatScope const scope{
        bot->IsAlive(),
        !bot->GetGroup(),
        map && !map->Instanceable(),
        AutoWowGuilds::InRanges(gCohort, guid),
        !ai->GetMaster(),
        AutoWowOracleRuntime::IsManagedBot(guid),
        ai->IsAutoWowPaused(),
        ai->IsRealPlayer() || ai->HasRealPlayerMaster(),
    };
    if (!ProductiveCombatEligible(scope) || bot->InBattleground() || (map && map->IsBattlegroundOrArena()))
        return false;

    auto const gameNow = GameTime::GetGameTimeMS().count();
    std::uint64_t const nowMs = gameNow > 0 ? static_cast<std::uint64_t>(gameNow) : 0;
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gCombat.find(guid);
    if (it == gCombat.end())
        return false;
    if (!ProactiveRetryBlocked(it->second.proactiveRetry, target, nowMs))
    {
        if (nowMs >= it->second.proactiveRetry.untilMs)
            it->second.proactiveRetry = {};
        return false;
    }
    return true;
}

void Forget(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    gXp.erase(guid);
    gGaveUpMs.erase(guid);
    gAges.erase(guid);
    gCombat.erase(guid);
}

void CombatWatch(Player* bot, std::uint64_t nowMs)
{
    Params const& p = detail::gParams;
    if (!p.combatStallMs)
        return;

    std::uint32_t const guid = bot->GetGUID().GetCounter();
    bool const inCombat = bot->IsAlive() && bot->IsInCombat();
    Map* const map = bot->GetMap();
    PlayerbotAI* const ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    Unit* const victim = bot->GetVictim();
    Unit* const currentTarget = ai && ai->GetAiObjectContext()
                                    ? ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()
                                    : nullptr;
    Group const* const group = bot->GetGroup();
    ProductiveCombatScope const scope{
        bot->IsAlive(),
        !group,
        map && !map->Instanceable(),
        AutoWowGuilds::InRanges(gCohort, guid),
        ai && !ai->GetMaster(),
        AutoWowOracleRuntime::IsManagedBot(guid),
        ai && ai->IsAutoWowPaused(),
        !ai || ai->IsRealPlayer() || ai->HasRealPlayerMaster(),
    };
    bool const productiveEligible = ProductiveCombatEligible(scope) && victim && victim->ToCreature();
    bool const sameLiveCreatureTarget = victim && victim == currentTarget && victim->ToCreature() &&
                                        victim->IsAlive() && victim->IsInWorld() &&
                                        victim->GetMapId() == bot->GetMapId() && bot->IsValidAttackTarget(victim);
    StaleTargetScope const staleScope{
        scope,
        bot->InBattleground() || (map && map->IsBattlegroundOrArena()),
        inCombat,
        ai && ai->GetState() == BOT_STATE_COMBAT,
        sameLiveCreatureTarget,
        bot->IsNonMeleeSpellCast(false),
    };
    bool const staleTargetObserved = StaleTargetObserved(staleScope);
    bool const staleTargetEligible = StaleTargetEligible(staleScope);
    ProductiveCombatSample productiveSample;
    if (productiveEligible || staleTargetObserved)
    {
        productiveSample.target = victim->GetGUID().GetRawValue();
        productiveSample.targetHp = victim->GetHealth();
        productiveSample.x = static_cast<std::int32_t>(std::lround(bot->GetPositionX()));
        productiveSample.y = static_cast<std::int32_t>(std::lround(bot->GetPositionY()));
        productiveSample.z = static_cast<std::int32_t>(std::lround(bot->GetPositionZ()));
        productiveSample.targetMaxHp = victim->GetMaxHealth();
    }

    std::uint64_t since = 0;
    std::uint64_t activity = 0;
    std::uint64_t productiveSince = 0;
    std::uint64_t staleTargetSince = 0;
    bool inactivityStalled = false;
    bool productiveStalled = false;
    bool staleTargetStalled = false;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!inCombat && !staleTargetObserved)
        {
            auto const it = gCombat.find(guid);
            if (it != gCombat.end())
            {
                it->second.combatSinceMs = 0;
                it->second.productive = ProductiveCombatWatch{};
                it->second.staleTarget = ProductiveCombatWatch{};
            }
            return;
        }

        Combat& c = gCombat[guid];
        if (nowMs < c.nextCheckMs)
            return;
        c.nextCheckMs = nowMs + kCombatCheckMs;
        if (inCombat)
        {
            c.staleTarget = ProductiveCombatWatch{};
            if (!c.combatSinceMs)
                c.combatSinceMs = nowMs ? nowMs : 1;
            since = c.combatSinceMs;
            activity = c.activityMs;
            inactivityStalled = CombatStalled(since, activity, nowMs, p.combatStallMs);
            if (productiveEligible)
            {
                productiveSample.incomingMs = c.incomingMs;
                productiveStalled =
                    ProductiveCombatStalled(c.productive, productiveSample, nowMs, p.combatStallMs);
                productiveSince = c.productive.sinceMs;
            }
            else
                c.productive = ProductiveCombatWatch{};
        }
        else
        {
            c.combatSinceMs = 0;
            c.productive = ProductiveCombatWatch{};
            staleTargetStalled =
                ProductiveCombatStalled(c.staleTarget, productiveSample, nowMs, p.combatStallMs);
            staleTargetSince = c.staleTarget.sinceMs;
        }
    }

    if (!inactivityStalled && !productiveStalled && !staleTargetStalled)
        return;
    if (staleTargetStalled && !staleTargetEligible)
        return;  // preserve the active cast; the already-expired timer is checked again when it ends

    // Preserve the original inactivity-watchdog exclusions. The productive watchdog has already applied
    // the stricter solo, masterless, cohort, open-world and paused/user-control gates above.
    if (!map || bot->InBattleground() || map->IsBattlegroundOrArena() || AutoWowOracleRuntime::IsManagedBot(guid))
        return;
    if (InstanceMap* instance = map->ToInstanceMap())
        if (InstanceScript* script = instance->GetInstanceScript(); script && script->IsEncounterInProgress())
            return;  // a boss fight: the dungeon's own wipe / reset rules apply

    char const* const cause =
        staleTargetStalled ? "precombat_target" : productiveStalled ? "unproductive" : "inactive";
    std::uint64_t const failedTargetRaw = staleTargetStalled && victim ? victim->GetGUID().GetRawValue() : 0;
    if (failedTargetRaw)
    {
        // Unit updates and this bot's AI decisions are serialized on its map thread. Install before the target
        // transition anyway, so any admission query after this callback observes the backoff immediately.
        std::lock_guard<std::mutex> guard(gLock);
        gCombat[guid].proactiveRetry = StartProactiveRetryBackoff(failedTargetRaw, nowMs);
    }
    LOG_INFO("playerbots", "[Unstick] combat_stall bot={} cause={} lvl={} combat_ms={} idle_ms={} attackers={} "
             "victim={} victim_guid={} backoff_ms={} map={} zone={} x={} y={}", bot->GetName(), cause,
             bot->GetLevel(), inCombat ? nowMs - since : 0,
             nowMs - (staleTargetStalled ? staleTargetSince
                                         : productiveStalled ? productiveSince : std::max(since, activity)),
             bot->getAttackers().size(),
             victim ? victim->GetEntry() : 0, victim ? victim->GetGUID().GetCounter() : 0,
             staleTargetStalled ? kProactiveRetryBackoffMs : 0, bot->GetMapId(),
             bot->GetZoneId(),
             std::int32_t(bot->GetPositionX()), std::int32_t(bot->GetPositionY()));

    if (staleTargetStalled && ai)
    {
        // Match native DropTargetAction without a broad AI reset or any movement/credit mutation.
        AiObjectContext* const context = ai->GetAiObjectContext();
        context->GetValue<Unit*>("current target")->Set(nullptr);
        auto* const pullTarget = context->GetValue<ObjectGuid>("pull target");
        if (ShouldClearMatchingPull(pullTarget->Get().GetRawValue(), failedTargetRaw))
            pullTarget->Set(ObjectGuid::Empty);
        auto* const pullStrategyTarget = context->GetValue<ObjectGuid>("pull strategy target");
        if (ShouldClearMatchingPull(pullStrategyTarget->Get().GetRawValue(), failedTargetRaw))
            pullStrategyTarget->Set(ObjectGuid::Empty);
        bot->SetTarget(ObjectGuid::Empty);
        bot->SetSelection(ObjectGuid::Empty);
        bot->AttackStop();
        ai->ChangeEngine(BOT_STATE_NON_COMBAT);
    }
    else
    {
        bot->GetThreatMgr().RemoveMeFromThreatLists();
        if (productiveStalled && ai)
        {
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
            bot->SetTarget(ObjectGuid::Empty);
            bot->SetSelection(ObjectGuid::Empty);
            bot->AttackStop();
        }
        bot->CombatStopWithPets(true);
    }

    std::lock_guard<std::mutex> guard(gLock);
    ProactiveRetryBackoff const retry = gCombat[guid].proactiveRetry;
    gCombat[guid] = Combat{};
    gCombat[guid].proactiveRetry = retry;
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
    if (bot->GetSession()->IsLoggingOut() || bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_STUNNED) ||
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
