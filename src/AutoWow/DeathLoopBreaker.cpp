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

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "GatheringWorkerState.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TravelMgr.h"

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

void AddScripts() { new DeathLoopPlayerScript(); }
}  // namespace AutoWowDeathLoop
