/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DeathLoopBreaker.h"

#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>

#include "AutoWowBridge.h"
#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameTime.h"
#include "GatheringWorkerState.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TravelMgr.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowDeathLoop
{
namespace
{
// Deaths and bot AI updates run on map threads (MapUpdate.Threads). Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;

std::uint64_t NowMs()
{
    auto const now = GameTime::GetGameTimeMS().count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

std::uint32_t GuidOf(Player const* player) { return static_cast<std::uint32_t>(player->GetGUID().GetCounter()); }

std::int32_t Yards(float v) { return static_cast<std::int32_t>(std::floor(v)); }

// Autonomous bots only: real players, bots following a real player and gather workers (own recovery
// state machine) are never escalated.
PlayerbotAI* AutonomousBotAI(Player* player)
{
    PlayerbotAI* ai = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    if (!ai || ai->IsRealPlayer() || ai->HasRealPlayerMaster() || AutoWowGather::IsExplicitWorker(GuidOf(player)))
        return nullptr;
    return ai;
}

// Caller holds gLock. Bounded: beyond kMaxBots new bots are not tracked (existing state is kept).
BotState* FindOrCreate(std::uint32_t botGuid)
{
    auto it = gStates.find(botGuid);
    if (it == gStates.end())
    {
        if (gStates.size() >= kMaxBots)
            return nullptr;
        it = gStates.emplace(botGuid, BotState{}).first;
    }
    return &it->second;
}

BotState* Find(std::uint32_t botGuid)
{
    auto const it = gStates.find(botGuid);
    return it == gStates.end() ? nullptr : &it->second;
}

void NoteKillerLevel(Player* victim, std::uint32_t level)
{
    if (!AutonomousBotAI(victim))
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (BotState* s = FindOrCreate(GuidOf(victim)))
        s->pendingKillerLevel = level;
}

void HandleDeath(Player* bot)
{
    PlayerbotAI* ai = AutonomousBotAI(bot);
    // Open world only: battlegrounds/arenas and dungeon/raid deaths keep their own recovery policies.
    if (!ai || !bot->GetMap() || bot->GetMap()->Instanceable())
        return;

    Params const& p = detail::gParams;
    std::uint32_t const botGuid = GuidOf(bot);
    std::uint64_t const nowMs = NowMs();
    DeathSample const death{nowMs, bot->GetMapId(), Yards(bot->GetPositionX()), Yards(bot->GetPositionY())};
    std::uint32_t const botLevel = bot->GetLevel();
    std::uint32_t const zoneMinLevel = sTravelMgr.GetZoneBracketLow(bot->GetZoneId());

    std::uint32_t clusterDeaths = 0;
    std::uint32_t killerLevel = 0;
    DeathSample center;
    Decision decision;
    {
        std::lock_guard<std::mutex> guard(gLock);
        BotState* s = FindOrCreate(botGuid);
        if (!s)
            return;
        killerLevel = s->pendingKillerLevel;
        s->pendingKillerLevel = 0;
        if (HardEscapeEnabled())
            s->lastKillerLevel = killerLevel;
        clusterDeaths = RecordDeath(*s, death, p.windowMs, p.radiusYards, center);
        decision = Evaluate(p, clusterDeaths, botLevel, killerLevel, zoneMinLevel);
        if (!decision.Escalate())
            return;
        s->spiritHealer = true;
        s->relocate = decision.relocate;
        s->deferQuest = 0;
        bool const repeat = MarkDanger(*s, center, p.radiusYards, nowMs, nowMs + p.dangerCooldownMs);
        // AutoWow.DeathLoop.V2 (b): a second escalation in the same live area relocates in any bracket.
        if (V2Enabled() && repeat && !decision.relocate)
        {
            decision.relocate = true;
            s->relocate = true;
        }
    }

    // The quest the bot was working when it died there. Oracle-managed quests are deferred by the Oracle
    // pass itself (lease release + its `deferred` line); ordinary New-RPG quests are deferred here and the
    // RPG status is dropped so the bot does not resume the route back into the danger area.
    std::uint32_t deferredQuest = 0;
    bool const oracleManaged = AutoWowOracleRuntime::IsManagedBot(botGuid);
    if (auto const* doQuest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data); doQuest && doQuest->questId)
    {
        deferredQuest = doQuest->questId;
        if (oracleManaged)
        {
            std::lock_guard<std::mutex> guard(gLock);
            if (BotState* s = Find(botGuid))
                s->deferQuest = deferredQuest;
        }
        else
            DeferQuest(bot, deferredQuest);
    }
    if (!oracleManaged)
        ai->rpgInfo.ChangeToIdle();

    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitDeathLoop(bot, deferredQuest, TriggerName(decision.trigger),
                                          LedgerFields(clusterDeaths, killerLevel, zoneMinLevel, decision, center, p));
    LOG_INFO("playerbots",
             "[DeathLoop] bot={} trigger={} deaths={} lvl={} klvl={} zone={} zlow={} relocate={} quest={} oracle={} "
             "danger=({},{},{}) r={} cool_ms={}",
             bot->GetName(), TriggerName(decision.trigger), clusterDeaths, botLevel, killerLevel, bot->GetZoneId(),
             zoneMinLevel, decision.relocate, deferredQuest, oracleManaged, center.map, center.x, center.y,
             p.radiusYards, p.dangerCooldownMs);
}

constexpr std::uint32_t kHearthstoneItem = 6948;
constexpr std::uint32_t kHearthstoneSpell = 8690;
constexpr float kHearthMinYards = 100.0f;

// AutoWow.Survival.HardEscape (3), sampled every kHardCheckMs from the player update (in combat and dead
// too: the RPG status update that owns the relocation never ran for Taelorin, killed within seconds of
// every res). Autonomous, open-world, non-Oracle bots only.
void HardEscapeTick(Player* bot)
{
    if (!bot->GetMap() || bot->GetMap()->Instanceable() || bot->IsBeingTeleported() || bot->IsInFlight())
        return;
    std::uint64_t const nowMs = NowMs();
    std::uint32_t const guid = GuidOf(bot);
    std::uint32_t const level = bot->GetLevel();
    std::uint32_t const zone = bot->GetZoneId();
    std::uint32_t const zoneLow = ZoneMinLevel(zone);
    HardParams const& h = detail::gHardParams;
    bool const overZone = Overshoot(zoneLow, level, h.zoneGap);
    {
        std::lock_guard<std::mutex> guard(gLock);
        BotState const* s = Find(guid);
        if (!s ? !overZone : nowMs < s->hard.nextCheckMs)
            return;
    }
    PlayerbotAI* ai = AutonomousBotAI(bot);
    if (!ai || AutoWowOracleRuntime::IsManagedBot(guid))
        return;

    // Hearth: in the bags, off cooldown, bound over 100 yd away in a zone that fits the bot, out of combat.
    std::uint32_t homeZone = 0;
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->m_homebindAreaId))
        homeZone = area->zone ? area->zone : area->ID;
    std::uint32_t const homeLow = ZoneMinLevel(homeZone);
    bool const homeFits = homeZone && !Overshoot(homeLow, level, h.walkZoneMargin);
    bool const hearthUsable = homeFits && bot->IsAlive() && !bot->IsInCombat() &&
                              bot->HasItemCount(kHearthstoneItem, 1, false) &&
                              !bot->HasSpellCooldown(kHearthstoneSpell) &&
                              (bot->m_homebindMapId != bot->GetMapId() ||
                               bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY) > kHearthMinYards);

    HardAction action = HardAction::None;
    std::uint32_t deaths = 0;
    std::uint32_t killerLevel = 0;
    std::uint64_t stuckMs = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        BotState* s = FindOrCreate(guid);
        if (!s || nowMs < s->hard.nextCheckMs)
            return;
        s->hard.nextCheckMs = nowMs + kHardCheckMs;
        deaths = RecentDeaths(*s, nowMs, detail::gParams.windowMs);
        killerLevel = s->lastKillerLevel;
        bool const hard = HardCondition(h, level, zoneLow, deaths, killerLevel);
        stuckMs = s->hard.active && nowMs >= s->hard.sinceMs ? nowMs - s->hard.sinceMs : 0;
        action = HardStep(h, s->hard, nowMs, hard, bot->IsAlive(), bot->GetMapId(), Yards(bot->GetPositionX()),
                          Yards(bot->GetPositionY()), hearthUsable);
    }
    if (action == HardAction::None)
        return;

    bool ok = false;
    if (action == HardAction::Hearth)
    {
        ok = ai->DoSpecificAction("hearthstone", Event("autowow hard escape"), true);
        LOG_INFO("playerbots", "[HardEscape] bot={} action=hearth ok={} lvl={} zone={} zlow={} deaths={} klvl={} "
                 "stuck_ms={} home_zone={}",
                 bot->GetName(), ok, level, zone, zoneLow, deaths, killerLevel, stuckMs, homeZone);
        if (ok)
            return;
        // The hearth would not start: portal now (same bookkeeping as HardStep's portal).
        std::lock_guard<std::mutex> guard(gLock);
        if (BotState* s = Find(guid))
        {
            s->hard.active = false;
            s->hard.cooldownUntilMs = nowMs + h.cooldownMs;
        }
    }

    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    Place const home{bot->m_homebindMapId, Yards(bot->m_homebindX), Yards(bot->m_homebindY),
                     Yards(bot->m_homebindZ), homeZone};
    Place const to = HardPortalTarget(team, bot->GetMapId(), home, homeLow, level, h.walkZoneMargin);
    // Hard escape is an explicit, logged portal (owner ruling: portals are an acceptable fallback). It must
    // override the per-bot no-teleport policy of independent bots, like the zone-progression portal leg does;
    // honouring it left a L18 cohort bot looping 400+ deaths in Burning Steppes (soak-s23-full-r1).
    bool const noTeleport = false;
    (void)AutoWowPolicy::IsNoTeleport(guid);
    if (!noTeleport)
    {
        {
            // The loop is broken: forget the deaths (the death rule must not fire again at the target) and
            // any pending relocation; the danger areas stay.
            std::lock_guard<std::mutex> guard(gLock);
            if (BotState* s = Find(guid))
            {
                s->deathCount = 0;
                s->lastKillerLevel = 0;
                s->relocate = false;
            }
        }
        AutoWowZoneProgression::CancelTrip(guid, nowMs);
        ai->rpgInfo.ChangeToIdle();
        bot->GetMotionMaster()->Clear();
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        ok = bot->TeleportTo(to.map, float(to.x), float(to.y), float(to.z), bot->GetOrientation());
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitZoneMove(bot, "hard_escape",
                                             AutoWowZoneProgression::LedgerFields(
                                                 zone, to.zone, stuckMs, ok, AutoWowZoneProgression::Mode::Portal));
    }
    LOG_INFO("playerbots", "[HardEscape] bot={} action=portal ok={} no_teleport={} lvl={} zone={} zlow={} deaths={} "
             "klvl={} stuck_ms={} to=({},{},{},{}) to_zone={}",
             bot->GetName(), ok, noTeleport, level, zone, zoneLow, deaths, killerLevel, stuckMs, to.map, to.x, to.y,
             to.z, to.zone);
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.DeathLoop.Enable", false);
    Params& p = detail::gParams;
    p.deaths = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.Deaths", 3);
    p.windowMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.WindowMs", 1800000);
    p.radiusYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.RadiusYards", 60);
    p.killerLevelGap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.KillerLevelGap", 10);
    p.dangerCooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.DangerCooldownMs", 3600000);
    p.relocateLevelMargin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.RelocateLevelMargin", 5);
    detail::gEscape = sConfigMgr->GetOption<bool>("AutoWow.DeathLoop.EscapeViaZoneProgression", false);
    p.escapePortalDeaths = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DeathLoop.EscapePortalDeaths", 3);
    detail::gV2 = sConfigMgr->GetOption<bool>("AutoWow.DeathLoop.V2", false);
    detail::gHard = sConfigMgr->GetOption<bool>("AutoWow.Survival.HardEscape", false);
    HardParams& h = detail::gHardParams;
    h.walkZoneMargin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.WalkZoneMargin", 5);
    h.zoneGap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.ZoneGap", 10);
    h.deaths = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.Deaths", 4);
    h.killerGap = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.KillerGap", 10);
    h.clusterDeaths = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.ClusterDeaths", 8);
    h.stuckMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.StuckMs", 120000);
    h.moveYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.MoveYards", 100);
    h.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.HardEscape.CooldownMs", 900000);
}

std::uint32_t ZoneAt(Map* map, float x, float y)
{
    GridTerrainData* const grid = map ? map->GetGridTerrainData(x, y) : nullptr;
    AreaTableEntry const* const area = grid ? sAreaTableStore.LookupEntry(grid->getArea(x, y)) : nullptr;
    return !area ? 0 : area->zone ? area->zone : area->ID;
}

std::uint32_t ZoneMinLevel(std::uint32_t zoneId)
{
    if (std::uint32_t const low = sTravelMgr.GetZoneBracketLow(zoneId))
        return low;
    AreaTableEntry const* const area = zoneId ? sAreaTableStore.LookupEntry(zoneId) : nullptr;
    return area && area->area_level > 0 ? static_cast<std::uint32_t>(area->area_level) : 0;
}

bool RestPending(std::uint32_t botGuid)
{
    if (!V2Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    BotState const* s = Find(botGuid);
    return s && s->restPending;
}

void ClearRestPending(std::uint32_t botGuid)
{
    if (!V2Enabled())
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (BotState* s = Find(botGuid))
        s->restPending = false;
}

std::uint32_t RecentDeaths(std::uint32_t botGuid)
{
    if (!Enabled())
        return 0;
    std::uint64_t const nowMs = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    BotState const* s = Find(botGuid);
    return s ? RecentDeaths(*s, nowMs, detail::gParams.windowMs) : 0;
}

bool WantsSpiritHealer(std::uint32_t botGuid)
{
    if (!Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    BotState const* s = Find(botGuid);
    return s && s->spiritHealer;
}

bool IsDangerous(std::uint32_t botGuid, std::uint32_t map, float x, float y)
{
    if (!Enabled())
        return false;
    std::uint64_t const nowMs = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    BotState const* s = Find(botGuid);
    return s && IsDangerous(*s, map, Yards(x), Yards(y), nowMs);
}

bool MarkDangerArea(std::uint32_t botGuid, std::uint32_t map, std::int32_t x, std::int32_t y, std::uint32_t radius,
                    std::uint64_t durationMs)
{
    if (!Enabled())
        return false;
    std::uint64_t const nowMs = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    BotState* s = FindOrCreate(botGuid);
    if (!s)
        return false;
    MarkDanger(*s, DeathSample{nowMs, map, x, y}, radius, nowMs, nowMs + durationMs);
    return true;
}

bool TakeRelocation(std::uint32_t botGuid)
{
    if (!Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    BotState* s = Find(botGuid);
    if (!s || !s->relocate)
        return false;
    s->relocate = false;
    return true;
}

bool TakeQuestDeferral(std::uint32_t botGuid, std::uint32_t questId)
{
    if (!Enabled() || !questId)
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    BotState* s = Find(botGuid);
    if (!s || s->deferQuest != questId)
        return false;
    s->deferQuest = 0;
    return true;
}

void DeferQuest(Player* bot, std::uint32_t questId)
{
    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!ai || !questId || !ai->lowPriorityQuest.insert(questId).second)
        return;
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, questId, "death_loop_area");
}

// Registered unconditionally (the flag is read after script registration); every hook early-returns
// on the cached flag.
class DeathLoopPlayerScript : public PlayerScript
{
public:
    DeathLoopPlayerScript()
        : PlayerScript("AutoWowDeathLoopPlayer", {
            PLAYERHOOK_ON_PVP_KILL,
            PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_PLAYER_RESURRECT,
            PLAYERHOOK_ON_LOGOUT
        })
    {
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!Enabled() || !killer || !killed)
            return;
        NoteKillerLevel(killed, killer == killed ? 0 : killer->GetLevel());  // self-kill: environment
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!Enabled() || !killer || !killed)
            return;
        Player* const owner = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        NoteKillerLevel(killed, owner ? owner->GetLevel() : killer->GetLevel());
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (Enabled() && player)
            HandleDeath(player);
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        if (!Enabled() || !player)
            return;
        std::lock_guard<std::mutex> guard(gLock);
        if (BotState* s = Find(GuidOf(player)))
        {
            // AutoWow.DeathLoop.V2 (c): the escalated death took the spirit healer: rest before pulling.
            if (V2Enabled() && s->spiritHealer)
                s->restPending = true;
            s->spiritHealer = false;
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!Enabled() || !player)
            return;
        std::lock_guard<std::mutex> guard(gLock);
        gStates.erase(GuidOf(player));
    }
};

// AutoWow.Survival.HardEscape (3): its own script so the death-loop hooks above stay as they were;
// registered unconditionally, early-returns on the cached flag.
class HardEscapePlayerScript : public PlayerScript
{
public:
    HardEscapePlayerScript() : PlayerScript("AutoWowHardEscapePlayer", {PLAYERHOOK_ON_UPDATE}) {}

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (HardEscapeEnabled() && player)
            HardEscapeTick(player);
        if (player && player->isDead() && !player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            ReleaseStaleCorpse(player);
    }

private:
    // AutoWow.Survival.CorpsePortal: an autonomous bot lying unreleased for kStaleCorpseMs is released to its
    // graveyard like a player (soak-s32-full-r1: bots that logged in dead idled the whole run; the
    // random-bot manager pass that used to release them does not reach the cohort reliably).
    static constexpr std::uint64_t kStaleCorpseMs = 60000;
    static void ReleaseStaleCorpse(Player* player)
    {
        // The core counts an unreleased corpse down from 6 min (m_deathTimer; 0 after a login dead).
        if (!sConfigMgr->GetOption<bool>("AutoWow.Survival.CorpsePortal", false) || !AutonomousBotAI(player) ||
            player->GetDeathTimer() + kStaleCorpseMs > 6 * MINUTE * IN_MILLISECONDS)
            return;
        LOG_INFO("playerbots", "[SafeRevive] bot={} release stale corpse", player->GetName());
        player->BuildPlayerRepop();
        player->RepopAtGraveyard();
    }
};

void AddScripts()
{
    new DeathLoopPlayerScript();
    new HardEscapePlayerScript();
}
}  // namespace AutoWowDeathLoop
