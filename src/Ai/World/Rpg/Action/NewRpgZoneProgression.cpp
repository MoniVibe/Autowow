/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.ZoneProgression runtime (policy: AutoWow/ZoneProgressionPolicy.h).

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowZoneProgression
{
namespace
{
// Bot AI updates run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.ZoneProgression.Enable", false);
    Params& p = detail::gParams;
    p.levelMargin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.LevelMargin", 2);
    p.stallChecks = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.StallChecks", 10);
    p.checkIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.CheckIntervalMs", 60000);
    p.travelTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.TravelTimeoutMs", 3600000);
    p.maxReissues = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.MaxReissues", 8);
    p.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.GiveUpCooldownMs", 1800000);
    detail::gRoutes = DefaultRoutes();
    std::string const routes = sConfigMgr->GetOption<std::string>("AutoWow.ZoneProgression.Routes", "");
    if (!routes.empty() && !ParseRoutes(routes, detail::gRoutes))
        LOG_ERROR("server.loading", "[ZoneProgression] AutoWow.ZoneProgression.Routes malformed; built-in table kept");
}

// Snapshot/replace under the lock; the state is small and value-only.
static BotState LoadState(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    return it == gStates.end() ? BotState{} : it->second;
}

static void StoreState(std::uint32_t guid, BotState const& s)
{
    std::lock_guard<std::mutex> guard(gLock);
    gStates[guid] = s;
}
}  // namespace AutoWowZoneProgression

// Flight leg toward `toZone`: nearest flight master on the bot's map, a destination node in toZone the
// bot already knows (lowest node id), and a taxi path between them. The source node is learned at the
// flight master by NewRpgTravelFlightAction.
static bool FindZoneFlight(Player* bot, std::uint32_t toZone, uint32& fmEntry, WorldPosition& fmPos,
                           std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!fm || !fm->taxiNodeId || fm->pos.GetMapId() != bot->GetMapId() || fm->zoneId == toZone)
        return false;
    std::vector<uint32> nodes = sTravelMgr.GetFlightNodesInZone(toZone, bot->GetTeamId(), fm->taxiNodeId);
    std::sort(nodes.begin(), nodes.end());
    for (uint32 node : nodes)
    {
        if (!bot->m_taxi.IsTaximaskNodeKnown(node))
            continue;
        path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, node);
        if (!path.empty())
        {
            fmEntry = fm->templateEntry;
            fmPos = fm->pos;
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::ZoneProgressionStep()
{
    using namespace AutoWowZoneProgression;
    using AutoWowZoneProgression::BotState;  // other AutoWow policies also name BotState
    using AutoWowZoneProgression::Mode;
    using AutoWowZoneProgression::Params;
    using AutoWowZoneProgression::Phase;
    using AutoWowZoneProgression::Route;
    using AutoWowZoneProgression::Trigger;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive() || bot->IsInFlight() || bot->IsInCombat() || !botAI->IsAutoWowIndependentParty() ||
        AutoWowOracleRuntime::IsManagedBot(guid) || !bot->GetMap() || bot->GetMap()->Instanceable())
        return false;

    Params const& p = detail::gParams;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    BotState s = LoadState(guid);
    NewRpgInfo& info = botAI->rpgInfo;

    auto finish = [&](bool arrived)
    {
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitZoneMove(bot, TriggerName(s.trigger),
                LedgerFields(s.fromZone, s.route.to, now >= s.startMs ? now - s.startMs : 0, arrived, s.mode));
        LOG_INFO("playerbots", "[ZoneProgression] bot={} {} from={} to={} lvl={} trigger={} mode={} ms={}",
                 bot->GetName(), arrived ? "arrived" : "gave_up", s.fromZone, s.route.to, bot->GetLevel(),
                 TriggerName(s.trigger), ModeName(s.mode), now - s.startMs);
    };

    if (s.phase == Phase::None)
    {
        if (now < s.nextCheckMs || now < s.cooldownUntilMs)
            return false;
        s.nextCheckMs = now + p.checkIntervalMs;
        uint32 const zone = bot->GetZoneId();
        std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
        Route const* route = PickRoute(detail::gRoutes, team, zone, bot->GetLevel(), guid);
        s.stall = NextStall(s.stall, CheckRpgStatusAvailable(RPG_DO_QUEST));
        auto const bracket = sPlayerbotAIConfig.zoneBrackets.find(zone);
        std::uint32_t const zoneMax = bracket == sPlayerbotAIConfig.zoneBrackets.end() ? 0 : bracket->second.second;
        Trigger const trigger = Evaluate(p, bot->GetLevel(), zoneMax, s.stall, route != nullptr);
        if (trigger == Trigger::None)
        {
            StoreState(guid, s);
            return false;
        }
        s.phase = Phase::Travel;
        s.route = *route;
        s.trigger = trigger;
        s.fromZone = zone;
        s.startMs = now;
        s.reissues = 0;
        s.stall = 0;
        s.mode = Mode::Unreachable;
        LOG_INFO("playerbots", "[ZoneProgression] bot={} graduate from={} to={} lvl={} trigger={}", bot->GetName(),
                 zone, route->to, bot->GetLevel(), TriggerName(trigger));
    }

    WorldPosition const target(s.route.map, float(s.route.x), float(s.route.y), float(s.route.z));

    if (s.phase == Phase::Travel)
    {
        if (bot->GetMapId() == s.route.map && bot->GetExactDist2d(target.GetPositionX(), target.GetPositionY()) < 15.0f)
        {
            // At the hub inn: bind the hearthstone there (the innkeeper's own effect), then the flight master.
            if (Creature* inn = bot->FindNearestCreature(s.route.inn, 30.0f))
                bot->SetHomebind(WorldLocation(inn->GetMapId(), inn->GetPositionX(), inn->GetPositionY(),
                                               inn->GetPositionZ(), inn->GetOrientation()),
                                 inn->GetAreaId());
            finish(true);
            s.phase = Phase::LearnFp;
            s.reissues = 0;
            info.ChangeToIdle();
            StoreState(guid, s);
            return true;
        }
        if (TravelExhausted(p, s, now))
        {
            finish(false);
            s = BotState{};
            s.cooldownUntilMs = now + p.cooldownMs;
            StoreState(guid, s);
            info.ChangeToIdle();
            return true;
        }
        NewRpgStatus const status = info.GetStatus();
        bool const ours = (status == RPG_GO_GRIND && std::get<NewRpgInfo::GoGrind>(info.data).pos == target) ||
                          status == RPG_TRAVEL_FLIGHT;
        if (ours)
            return false;
        ++s.reissues;
        uint32 fmEntry = 0;
        WorldPosition fmPos;
        std::vector<uint32> path;
        bool const flight = bot->GetZoneId() != s.route.to && FindZoneFlight(bot, s.route.to, fmEntry, fmPos, path);
        Mode const mode = SelectMode(flight, bot->GetMapId() == s.route.map, s.route.crossing && bot->GetZoneId() != s.route.to);
        if (mode != Mode::Unreachable)
            s.mode = mode;
        StoreState(guid, s);
        if (mode == Mode::Flight)
            info.ChangeToTravelFlight(fmEntry, fmPos, path);
        else if (mode == Mode::Walk)
            info.ChangeToGoGrind(target);
        else
            return false;  // counted; the next tick retries until TravelExhausted
        return true;
    }

    // Phase::LearnFp: walk to the destination zone's nearest flight master and learn its node.
    TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    bool const need = fm && fm->taxiNodeId && fm->zoneId == s.route.to && fm->pos.GetMapId() == bot->GetMapId() &&
                      !bot->m_taxi.IsTaximaskNodeKnown(fm->taxiNodeId);
    if (!need || s.reissues > p.maxReissues)
    {
        s = BotState{};
        s.nextCheckMs = now + p.checkIntervalMs;
        StoreState(guid, s);
        return false;
    }
    if (bot->GetExactDist2d(fm->pos.GetPositionX(), fm->pos.GetPositionY()) < INTERACTION_DISTANCE * 3)
    {
        if (Creature* master = bot->FindNearestCreature(fm->templateEntry, INTERACTION_DISTANCE * 3))
            bot->GetSession()->SendLearnNewTaxiNode(master);
        s = BotState{};
        s.nextCheckMs = now + p.checkIntervalMs;
        StoreState(guid, s);
        info.ChangeToIdle();
        return true;
    }
    NewRpgStatus const status = info.GetStatus();
    if (status == RPG_GO_GRIND && std::get<NewRpgInfo::GoGrind>(info.data).pos == fm->pos)
        return false;
    ++s.reissues;
    StoreState(guid, s);
    info.ChangeToGoGrind(fm->pos);
    return true;
}
