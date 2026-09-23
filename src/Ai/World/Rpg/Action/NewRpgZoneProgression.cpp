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
#include "GameObject.h"
#include "Transport.h"
#include "TransportCrossingPolicy.h"
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
std::unordered_map<std::uint32_t, AutoWowTransports::ChainState> gChains;  // AutoWow.Transports only
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

static AutoWowTransports::ChainState LoadChain(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gChains.find(guid);
    return it == gChains.end() ? AutoWowTransports::ChainState{} : it->second;
}

static void StoreChain(std::uint32_t guid, AutoWowTransports::ChainState const& c)
{
    std::lock_guard<std::mutex> guard(gLock);
    gChains[guid] = c;
}
}  // namespace AutoWowZoneProgression

void AutoWowTransports::LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Transports.Enable", false);
    Params& p = detail::gParams;
    p.approachYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.ApproachYards", 5);
    p.exitYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.ExitYards", 60);
    p.dockedYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.DockedYards", 8);
    p.joinYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.JoinYards", 300);
    p.stepTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.StepTimeoutMs", 900000);
    p.autoPortalAfter = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Transports.AutoPortalAfterFailures", 2);
    p.mode = TransportMode::Auto;
    std::string const mode = sConfigMgr->GetOption<std::string>("AutoWow.Transports.Mode", "auto");
    if (!ParseMode(mode, p.mode))
        LOG_ERROR("server.loading", "[Transports] AutoWow.Transports.Mode '{}' unknown; auto kept", mode);
    detail::gOverrides.clear();
    std::string const overrides = sConfigMgr->GetOption<std::string>("AutoWow.Transports.ModeOverrides", "");
    if (!overrides.empty() && !ParseModeOverrides(overrides, detail::gOverrides))
        LOG_ERROR("server.loading", "[Transports] AutoWow.Transports.ModeOverrides malformed; none applied");
    detail::gCrossings = DefaultCrossings();
    std::string const rows = sConfigMgr->GetOption<std::string>("AutoWow.Transports.Crossings", "");
    if (!rows.empty() && !ParseCrossings(rows, detail::gCrossings))
        LOG_ERROR("server.loading", "[Transports] AutoWow.Transports.Crossings malformed; built-in table kept");
    // Routes only a crossing can serve join the zone-progression table (runs after its LoadConfig).
    if (detail::gEnabled)
        for (AutoWowZoneProgression::Route const& r : ExtraRoutes())
            AutoWowZoneProgression::detail::gRoutes.push_back(r);
}

// One tick of a crossing chain (AutoWow.Transports). Walks to each crossing, takes it by the game's own
// mechanic (areatrigger / GameObject use / riding the transport), then walks to the hub. True when it
// acted or holds the bot (waiting on a dock, riding); the caller consumes the RPG tick either way.
static bool ChainStep(Player* bot, PlayerbotAI* botAI, AutoWowZoneProgression::BotState& s,
                      AutoWowTransports::ChainState& c, std::uint64_t now, WorldPosition const& hub)
{
    using namespace AutoWowTransports;
    NewRpgInfo& info = botAI->rpgInfo;
    Params const& p = detail::gParams;
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    std::vector<Crossing> const chain = ChainFor(detail::gCrossings, team, s.route.from, s.route.to);

    // Walk toward `pos`: the New RPG long walk while far, an exact MovePoint for the last yards.
    auto walkTo = [&](std::uint32_t map, float x, float y, float z) -> bool
    {
        WorldPosition const pos(map, x, y, z);
        if (bot->GetMapId() == map && bot->GetExactDist2d(x, y) < 40.0f)
        {
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, x, y, z);
            return true;
        }
        if (info.GetStatus() == RPG_GO_GRIND && std::get<NewRpgInfo::GoGrind>(info.data).pos == pos)
            return true;
        ++s.reissues;
        info.ChangeToGoGrind(pos);
        return true;
    };

    if (c.leg >= chain.size())
    {
        c.legs.Open(Leg::Walk, now);
        return walkTo(hub.GetMapId(), hub.GetPositionX(), hub.GetPositionY(), hub.GetPositionZ());
    }
    Crossing const& x = chain[c.leg];
    std::int32_t const bx = static_cast<std::int32_t>(bot->GetPositionX());
    std::int32_t const by = static_cast<std::int32_t>(bot->GetPositionY());
    std::int64_t const approach2 = std::int64_t(p.approachYards) * p.approachYards;
    std::int64_t const exit2 = std::int64_t(p.exitYards) * p.exitYards;
    std::int64_t const docked2 = std::int64_t(p.dockedYards) * p.dockedYards;

    Obs o;
    o.atApproach = bot->GetMapId() == x.map && Dist2(x.x, x.y, bx, by) <= approach2;
    o.atExit = bot->GetMapId() == x.exitMap && Dist2(x.exitX, x.exitY, bx, by) <= exit2;
    bool const portal = x.via == Via::Transport &&
                        UsePortal(ModeFor(detail::gOverrides, p.mode, x.object), c.failedRides, p.autoPortalAfter);
    bool const ride = x.via == Via::Transport && !portal;
    Transport* ship = nullptr;  // this crossing's transport docked at the boarding stop
    if (ride)
    {
        Transport* const t = bot->GetTransport();
        o.onTransport = t && t->GetEntry() == x.object;
        if (o.onTransport)
            o.dockedExit = bot->GetMapId() == x.exitMap &&
                           Dist2(x.exitStopX, x.exitStopY, static_cast<std::int32_t>(t->GetPositionX()),
                                 static_cast<std::int32_t>(t->GetPositionY())) <= docked2;
        else if (bot->GetMapId() == x.map)
            for (Transport* candidate : bot->GetMap()->GetAllTransports())
                if (candidate->GetEntry() == x.object &&
                    Dist2(x.stopX, x.stopY, static_cast<std::int32_t>(candidate->GetPositionX()),
                          static_cast<std::int32_t>(candidate->GetPositionY())) <= docked2)
                {
                    ship = candidate;
                    o.dockedHere = true;
                    break;
                }
    }

    Step next = ride ? NextTransportStep(c.step, o) : NextObjectStep(o);
    bool const stuck = next == c.step && StepStuck(p, c.step, c.stepAt, now);
    if (stuck)
        next = Step::Approach;  // stuck step: restart this crossing
    if (next != c.step || stuck)
    {
        if (ride && FailedRide(c.step, next, stuck))
            ++c.failedRides;
        LOG_INFO("playerbots", "[Transports] bot={} to={} leg={} via={} obj={} portal={} fails={} step={}->{}",
                 bot->GetName(), s.route.to, c.leg, static_cast<std::uint32_t>(x.via), x.object, portal,
                 c.failedRides, StepName(c.step), StepName(next));
        c.step = next;
        c.stepAt = now;
    }

    switch (c.step)
    {
        case Step::Approach:
            c.legs.Open(Leg::Walk, now);
            return walkTo(x.map, float(x.x), float(x.y), float(x.z));
        case Step::Use:
        {
            c.legs.Open(portal ? Leg::Portal : LegOf(x.via), now);
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            if (bot->IsNonMeleeSpellCast(false) || bot->IsBeingTeleported())
                return true;  // the object's teleport spell is casting / a relocation is under way
            if (portal)
                // Flagged dock-to-dock portal (TransportMode): relocate onto the destination dock.
                bot->TeleportTo(x.exitMap, float(x.exitX), float(x.exitY), float(x.exitZ), bot->GetOrientation());
            else if (x.via == Via::AreaTrigger)
            {
                WorldPacket packet(CMSG_AREATRIGGER);
                packet << x.object;
                packet.rpos(0);
                bot->GetSession()->HandleAreaTriggerOpcode(packet);
            }
            else if (GameObject* go = bot->FindNearestGameObject(x.object, INTERACTION_DISTANCE * 2))
            {
                WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
                packet << go->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(packet);
            }
            return true;
        }
        case Step::Wait:
        case Step::Ride:
            c.legs.Open(Leg::Transport, now);
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            return true;  // hold: no wandering off the dock / deck
        case Step::Board:
            c.legs.Open(Leg::Transport, now);
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            // Straight onto the deck at dock height; the core makes the bot a passenger once the deck is
            // under it (PlayerbotAI transport check, Map::GetTransportForPos).
            if (ship && !bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, ship->GetPositionX(), ship->GetPositionY(), float(x.z),
                                                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            return true;
        case Step::Disembark:
            c.legs.Open(Leg::Transport, now);
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, float(x.exitX), float(x.exitY), float(x.exitZ),
                                                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            return true;
        case Step::Done:
            ++c.leg;
            c.step = Step::Approach;
            c.stepAt = now;
            c.failedRides = 0;
            s.reissues = 0;
            c.legs.Open(Leg::Walk, now);
            return true;
    }
    return false;
}

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

// AutoWow.Transports variant of the Travel-phase reissue: the same flight / walk choice, plus crossing
// chains and the per-leg log. Stores both states.
static bool TransportsTravelStep(Player* bot, PlayerbotAI* botAI, AutoWowZoneProgression::BotState& s,
                                 AutoWowTransports::ChainState& c, std::uint64_t now, WorldPosition const& target)
{
    using AutoWowZoneProgression::Mode;
    uint32 const guid = bot->GetGUID().GetCounter();
    NewRpgInfo& info = botAI->rpgInfo;
    auto store = [&]()
    {
        AutoWowZoneProgression::StoreState(guid, s);
        AutoWowZoneProgression::StoreChain(guid, c);
    };
    if (s.mode == Mode::Chain)
    {
        bool const acted = ChainStep(bot, botAI, s, c, now, target);
        store();
        return acted;
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
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    std::vector<AutoWowTransports::Crossing> const chain =
        AutoWowTransports::ChainFor(AutoWowTransports::detail::gCrossings, team, s.route.from, s.route.to);
    Mode const mode = AutoWowTransports::SelectMode(flight, bot->GetMapId() == s.route.map,
                                                    s.route.crossing && bot->GetZoneId() != s.route.to, !chain.empty());
    if (mode != Mode::Unreachable)
        s.mode = mode;
    if (mode == Mode::Chain)
    {
        c.leg = AutoWowTransports::StartLeg(chain, bot->GetMapId(), static_cast<std::int32_t>(bot->GetPositionX()),
                                            static_cast<std::int32_t>(bot->GetPositionY()),
                                            static_cast<std::int32_t>(AutoWowTransports::detail::gParams.joinYards));
        c.step = AutoWowTransports::Step::Approach;
        c.stepAt = now;
        LOG_INFO("playerbots", "[Transports] bot={} chain to={} crossings={} start_leg={}", bot->GetName(),
                 s.route.to, chain.size(), c.leg);
        bool const acted = ChainStep(bot, botAI, s, c, now, target);
        store();
        return acted;
    }
    if (mode == Mode::Flight)
        c.legs.Open(AutoWowTransports::Leg::Flight, now);
    else if (mode == Mode::Walk)
        c.legs.Open(AutoWowTransports::Leg::Walk, now);
    store();
    if (mode == Mode::Flight)
        info.ChangeToTravelFlight(fmEntry, fmPos, path);
    else if (mode == Mode::Walk)
        info.ChangeToGoGrind(target);
    else
        return false;  // counted; the next tick retries until TravelExhausted
    return true;
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

    bool const transports = AutoWowTransports::Enabled();
    AutoWowTransports::ChainState chain = transports ? LoadChain(guid) : AutoWowTransports::ChainState{};

    auto finish = [&](bool arrived)
    {
        if (AutoWowQuestLedger::Enabled() && transports)
        {
            // AutoWow.Transports: append the per-leg log (fields append-only, event id unchanged).
            chain.legs.Close(now);
            AutoWowQuestLedger::EmitZoneMove(bot, TriggerName(s.trigger),
                LedgerFields(s.fromZone, s.route.to, now >= s.startMs ? now - s.startMs : 0, arrived, s.mode) +
                    AutoWowTransports::LegsField(chain.legs));
        }
        else if (AutoWowQuestLedger::Enabled())
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
        if (transports)
        {
            chain = AutoWowTransports::ChainState{};
            StoreChain(guid, chain);
        }
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
        if (transports)
            return TransportsTravelStep(bot, botAI, s, chain, now, target);
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
