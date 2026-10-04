/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.ZoneProgression runtime (policy: AutoWow/ZoneProgressionPolicy.h), with the AutoWow.Transports
// crossing chains and the AutoWow.Gathering.Detours step (GatherDetourPolicy.h).

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "ContractsPolicy.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathLoopBreaker.h"
#include "ErrandsPolicy.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "GossipHelloAction.h"
#include "GatherDetourPolicy.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "NewRpgBaseAction.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RestGate.h"
#include "GameObject.h"
#include "Transport.h"
#include "TransportCrossingPolicy.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "SquadPolicy.h"
#include "UnstickPolicy.h"
#include "World.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowZoneProgression
{
namespace
{
// Bot AI updates run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;
std::vector<RoadPoint> const gRoads = DefaultRoads();  // read-only after static init
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
    p.portalAfterMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.PortalAfterMs", 1200000);
    detail::gRoutes = DefaultRoutes();
    // AutoWow.ZoneProgression.HighRoutes (default 0): append the L20-60 hub routes (Routes still replaces all).
    if (sConfigMgr->GetOption<bool>("AutoWow.ZoneProgression.HighRoutes", false))
        for (Route const& r : HubRoutes(DefaultHubs(), DefaultHubSources()))
            detail::gRoutes.push_back(r);
    // AutoWow.ZoneProgression.Outland (default 0): the L58-70 Outland ladder (Azeroth bands stop at 57).
    detail::gOutland = sConfigMgr->GetOption<bool>("AutoWow.ZoneProgression.Outland", false);
    p.outlandPortalAfterMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.ZoneProgression.OutlandPortalAfterMs", 2400000);
    if (detail::gOutland)
        AddOutland(detail::gRoutes);
    // Native Northrend admission is independently rollout-gated and depends on the existing Outland ladder.
    detail::gNorthrend = detail::gOutland &&
        sConfigMgr->GetOption<bool>("AutoWow.ZoneProgression.Northrend", false);
    if (detail::gNorthrend)
        AddNorthrend(detail::gRoutes, static_cast<std::uint32_t>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
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

bool Active(std::uint32_t guid) { return LoadState(guid).phase != Phase::None; }

bool MaterialDriverAdmissionBlocked(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    return it != gStates.end() && ShouldDeferMaterialDriver(true, it->second);
}

void CancelTrip(std::uint32_t guid, std::uint64_t nowMs)
{
    if (!Enabled())
        return;
    BotState s;
    s.cooldownUntilMs = nowMs + detail::gParams.cooldownMs;
    std::lock_guard<std::mutex> guard(gLock);
    gStates[guid] = s;
    gChains.erase(guid);
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
    // AutoWow.ZoneProgression.Outland: the Dark Portal chains of the Outland entry routes.
    if (AutoWowZoneProgression::detail::gOutland)
        for (Crossing const& c : OutlandCrossings())
            detail::gCrossings.push_back(c);
    if (AutoWowZoneProgression::detail::gNorthrend)
        for (Crossing const& c : NorthrendCrossings())
            detail::gCrossings.push_back(c);
    // Routes only a crossing can serve join the zone-progression table (runs after its LoadConfig).
    if (detail::gEnabled)
        for (AutoWowZoneProgression::Route const& r : ExtraRoutes())
            AutoWowZoneProgression::detail::gRoutes.push_back(r);
}

// One tick of a crossing chain (AutoWow.Transports). Walks to each crossing, takes it by the game's own
// mechanic (areatrigger / GameObject use / riding the transport), then walks to the hub. True when it
// acted or holds the bot (waiting on a dock, riding); the caller consumes the RPG tick either way.
// `walk` = NewRpgBaseAction::WalkLeg (one tick of the no-teleport long walk; true = stuck this tick).
using WalkFn = std::function<bool(WorldPosition const&)>;

enum class ArrivalResult : std::uint8_t
{
    Yield = 0,
    Held = 1,
    Complete = 2,
    Failed = 3
};

static bool ArrivalRequired(AutoWowZoneProgression::BotState const& s,
                            std::vector<AutoWowTransports::Crossing> const& chain)
{
    using namespace AutoWowZoneProgression;
    return AutoWowTransports::RequiresArrivalService(OutlandEnabled() && IsOutlandEntry(s.route),
                                                     s.trigger == AutoWowZoneProgression::Trigger::DeathLoop,
                                                     !chain.empty(),
                                                     chain.empty() ? 0 : chain.back().object);
}

static bool ArrivalControlBlocked(Player* bot, PlayerbotAI* botAI)
{
    return !botAI->IsAutoWowIndependentParty() || botAI->IsRealPlayer() || botAI->HasRealPlayerMaster() ||
           botAI->IsAutoWowPaused() || bot->GetGroup() != nullptr;
}

static bool ArrivalReceiptBlocked(Player* bot, PlayerbotAI* botAI)
{
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    return !bot->IsAlive() || bot->IsInCombat() || ArrivalControlBlocked(bot, botAI) ||
           AutoWowOracleRuntime::IsManagedBot(guid) || !bot->GetMap() || bot->GetMap()->Instanceable() ||
           (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid));
}

static bool ArrivalStepTimedOut(AutoWowTransports::ChainState const& c, std::uint64_t now)
{
    return c.arrivalStepAt && now >= c.arrivalStepAt &&
           now - c.arrivalStepAt > AutoWowTransports::detail::gParams.stepTimeoutMs;
}

static bool HoldArrivalClock(AutoWowTransports::ChainState& c, std::uint64_t now)
{
    return AutoWowTransports::HoldArrivalTimer(c, now);
}

static void ResumeArrivalClock(AutoWowTransports::ChainState& c, std::uint64_t now,
                               AutoWowZoneProgression::BotState* trip = nullptr)
{
    std::uint64_t const heldMs = AutoWowTransports::ResumeArrivalTimer(c, now);
    if (trip && trip->startMs)
        trip->startMs += heldMs;
}

static AutoWowTransports::ArrivalReceipt ObserveArrivalFlight(
    Player* bot, AutoWowTransports::ArrivalSpec const& spec, AutoWowTransports::ChainState& c,
    std::uint64_t now, AutoWowZoneProgression::BotState* trip = nullptr)
{
    using namespace AutoWowTransports;
    ArrivalReceipt const receipt = EvaluateArrivalReceipt(
        spec, c.arrivalPhase, c.arrivalFlightObserved, bot->IsInFlight(), bot->m_taxi.GetTaxiSource(),
        bot->m_taxi.GetTaxiDestination(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ());
    if (receipt == ArrivalReceipt::FlightStarted || receipt == ArrivalReceipt::FlightActive)
        ResumeArrivalClock(c, now, trip);
    if (receipt == ArrivalReceipt::FlightStarted)
    {
        c.arrivalPhase = ArrivalPhase::AwaitFlightEnd;
        c.arrivalStepAt = now;
        c.arrivalFlightObserved = true;
    }
    return receipt;
}

// Post-Dark-Portal native faction service. WalkLeg owns the prepared navmesh approach. At the NPC the
// server-prepared, condition-filtered quest ride wins; otherwise the exact DB scripted taxi path is used.
// Neither branch changes quest state, taxi masks, rewards, XP, items, or position directly.
static ArrivalResult ArrivalServiceStep(Player* bot, PlayerbotAI* botAI,
                                        AutoWowZoneProgression::BotState& s,
                                        AutoWowTransports::ChainState& c, std::uint64_t now,
                                        WalkFn const& walk)
{
    using namespace AutoWowTransports;
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    ArrivalSpec const* spec = ArrivalForTeam(team);
    if (!spec)
        return ArrivalResult::Failed;

    if (c.arrivalPhase == ArrivalPhase::Complete)
        return ArrivalResult::Complete;
    if (c.arrivalPhase == ArrivalPhase::Failed)
        return ArrivalResult::Failed;

    if (ArrivalControlBlocked(bot, botAI))
    {
        HoldArrivalClock(c, now);
        return ArrivalResult::Yield;
    }

    std::int32_t const bx = static_cast<std::int32_t>(std::floor(bot->GetPositionX()));
    std::int32_t const by = static_cast<std::int32_t>(std::floor(bot->GetPositionY()));
    if (c.arrivalPhase == ArrivalPhase::None)
    {
        if (!OwnsArrivalPosition(*spec, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                 bot->GetPositionZ()))
        {
            c.arrivalPhase = ArrivalPhase::Failed;
            LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=source_context team={} map={} pos=({}, {})",
                     bot->GetName(), team, bot->GetMapId(), bx, by);
            return ArrivalResult::Failed;
        }
        c.arrivalPhase = ArrivalPhase::ApproachService;
        c.arrivalStepAt = now;
        c.arrivalExpectedSource = spec->sourceNode;
        c.arrivalExpectedDestination = spec->destinationNode;
        LOG_INFO("playerbots", "[Transports] bot={} arrival_service npc={} path={} nodes={}->{}",
                 bot->GetName(), spec->npc, spec->path, spec->sourceNode, spec->destinationNode);
    }

    if (bot->GetTransport() || bot->IsBeingTeleported() || bot->IsNonMeleeSpellCast(false) ||
        bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED))
    {
        HoldArrivalClock(c, now);
        return ArrivalResult::Held;
    }
    ResumeArrivalClock(c, now, &s);

    if (ArrivalStepTimedOut(c, now))
    {
        ArrivalPhase const timedOutPhase = c.arrivalPhase;
        c.arrivalPhase = ArrivalPhase::Failed;
        LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=step_timeout phase={} attempts={}",
                 bot->GetName(), static_cast<std::uint32_t>(timedOutPhase), c.arrivalAttempts);
        return ArrivalResult::Failed;
    }

    if (c.arrivalPhase == ArrivalPhase::AwaitFlightStart)
        return ArrivalResult::Held;

    if (c.arrivalPhase == ArrivalPhase::AwaitFlightEnd)
    {
        TaxiNodesEntry const* landing = sTaxiNodesStore.LookupEntry(spec->destinationNode);
        bool const landingData = landing && landing->map_id == spec->map &&
                                 std::fabs(landing->x - spec->destinationX) <= 1.0f &&
                                 std::fabs(landing->y - spec->destinationY) <= 1.0f &&
                                 std::fabs(landing->z - spec->destinationZ) <= 1.0f;
        ArrivalReceipt const receipt = landingData
            ? EvaluateArrivalReceipt(*spec, c.arrivalPhase, c.arrivalFlightObserved, false, 0, 0,
                                     bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())
            : ArrivalReceipt::None;
        if (receipt == ArrivalReceipt::Landed)
        {
            c.arrivalPhase = ArrivalPhase::Complete;
            c.legs.Open(Leg::Walk, now);
            LOG_INFO("playerbots", "[Transports] bot={} arrival_landed path={} destination={} map={} pos=({}, {})",
                     bot->GetName(), spec->path, spec->destinationNode, bot->GetMapId(), bx, by);
            return ArrivalResult::Complete;
        }
        return ArrivalResult::Held;
    }

    TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(spec->path);
    TaxiNodesEntry const* source = sTaxiNodesStore.LookupEntry(spec->sourceNode);
    TaxiNodesEntry const* destination = sTaxiNodesStore.LookupEntry(spec->destinationNode);
    bool const pathValid = path && source && destination &&
        NativeArrivalPathMatches(*spec, path->ID, path->from, path->to) && source->map_id == spec->map &&
        destination->map_id == spec->map && std::fabs(source->x - spec->sourceX) <= 1.0f &&
        std::fabs(source->y - spec->sourceY) <= 1.0f && std::fabs(source->z - spec->sourceZ) <= 1.0f &&
        std::fabs(destination->x - spec->destinationX) <= 1.0f &&
        std::fabs(destination->y - spec->destinationY) <= 1.0f &&
        std::fabs(destination->z - spec->destinationZ) <= 1.0f;
    float const serviceDistance = bot->GetExactDist2d(spec->serviceX, spec->serviceY);
    Creature* service = bot->FindNearestCreature(spec->npc, static_cast<float>(kArrivalServiceSearchYards));
    bool const serviceValid = service && service->GetEntry() == spec->npc && service->GetMapId() == spec->map &&
        service->IsAlive() && service->IsFriendlyTo(bot) && service->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) && source &&
        service->GetExactDist2d(spec->serviceX, spec->serviceY) <= kArrivalSourceNodeYards &&
        service->GetExactDist2d(source->x, source->y) <= kArrivalSourceNodeYards;
    bool const landingBypass = AtDarkPortalExit(*spec, bot->GetMapId(), bot->GetPositionX(),
                                                bot->GetPositionY(), bot->GetPositionZ());

    auto activateNative = [&](char const* kind) -> ArrivalResult
    {
        ++c.arrivalAttempts;
        if (!bot->ActivateTaxiPathTo(spec->path, spec->spell))
        {
            ++s.reissues;
            LOG_WARN("playerbots", "[Transports] bot={} arrival_ride={}_refused path={} spell={} fare={} attempt={}",
                     bot->GetName(), kind, spec->path, spec->spell, path->price, c.arrivalAttempts);
            return ArrivalResult::Held;
        }
        c.arrivalPhase = ArrivalPhase::AwaitFlightStart;
        c.arrivalStepAt = now;
        c.legs.Open(Leg::Flight, now);
        ArrivalReceipt const immediate = ObserveArrivalFlight(bot, *spec, c, now, &s);
        LOG_INFO("playerbots", "[Transports] bot={} arrival_ride={} path={} spell={} fare={} nodes={}->{}",
                 bot->GetName(), kind, spec->path, spec->spell, path->price, spec->sourceNode,
                 spec->destinationNode);
        if (immediate == ArrivalReceipt::FlightStarted)
            LOG_INFO("playerbots", "[Transports] bot={} arrival_flight_started path={} nodes={}->{} menu=false immediate=true",
                     bot->GetName(), spec->path, spec->sourceNode, spec->destinationNode);
        return ArrivalResult::Held;
    };

    // Both stock stair approaches terminate at the same unsafe navmesh floor boundary. From the exact
    // portal exit only, admit the same scripted native flight after validating the loaded faction service
    // at its source node. The native flight generator starts at the player's current position.
    if (landingBypass)
    {
        if (!pathValid || !serviceValid)
        {
            c.arrivalPhase = ArrivalPhase::Failed;
            LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=landing_bypass_context npc={} path={}",
                     bot->GetName(), spec->npc, spec->path);
            return ArrivalResult::Failed;
        }
        return activateNative("landing_native");
    }

    if (!service)
    {
        if (serviceDistance <= INTERACTION_DISTANCE * 3.0f)
        {
            c.arrivalPhase = ArrivalPhase::Failed;
            LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=service_missing npc={}",
                     bot->GetName(), spec->npc);
            return ArrivalResult::Failed;
        }
        if (walk(WorldPosition(spec->map, spec->serviceX, spec->serviceY, spec->serviceZ)))
            ++s.reissues;
        return ArrivalResult::Held;
    }
    if (!serviceValid)
    {
        c.arrivalPhase = ArrivalPhase::Failed;
        LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=service_invalid npc={}",
                 bot->GetName(), spec->npc);
        return ArrivalResult::Failed;
    }
    if (bot->GetExactDist(service) >= INTERACTION_DISTANCE)
    {
        if (walk(WorldPosition(service->GetMapId(), service->GetPositionX(), service->GetPositionY(),
                               service->GetPositionZ())))
            ++s.reissues;
        return ArrivalResult::Held;
    }
    if (bot->isMoving())
        return ArrivalResult::Held;
    if (bot->GetNPCIfCanInteractWith(service->GetGUID(), UNIT_NPC_FLAG_NONE) != service)
    {
        c.arrivalPhase = ArrivalPhase::Failed;
        LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=service_not_interactable npc={}",
                 bot->GetName(), spec->npc);
        return ArrivalResult::Failed;
    }

    GossipHelloAction gossip(botAI);
    bool const opened = gossip.Execute(service->GetGUID(), -1, true);
    GossipMenuItem const* item = nullptr;
    GossipMenu const* menu = nullptr;
    if (opened && bot->PlayerTalkClass)
    {
        menu = &bot->PlayerTalkClass->GetGossipMenu();
        item = menu->GetItem(spec->menuItem);
    }
    bool const offered = menu && item &&
        SelectArrivalRide(*spec, menu->GetSenderGUID() == service->GetGUID(), menu->GetMenuId(), spec->menuItem,
                          item->OptionType, item->IsCoded) == ArrivalRide::PreparedMenu;
    if (offered && gossip.SelectPrepared(service->GetGUID(), spec->menu, spec->menuItem,
                                         GOSSIP_OPTION_GOSSIP, true))
    {
        c.arrivalPhase = ArrivalPhase::AwaitFlightStart;
        c.arrivalStepAt = now;
        c.arrivalUsedMenu = true;
        ++c.arrivalAttempts;
        c.legs.Open(Leg::Flight, now);
        ArrivalReceipt const immediate = ObserveArrivalFlight(bot, *spec, c, now, &s);
        LOG_INFO("playerbots", "[Transports] bot={} arrival_ride=menu npc={} menu={} item={} expected_path={}",
                 bot->GetName(), spec->npc, spec->menu, spec->menuItem, spec->path);
        if (immediate == ArrivalReceipt::FlightStarted)
            LOG_INFO("playerbots", "[Transports] bot={} arrival_flight_started path={} nodes={}->{} menu=true immediate=true",
                     bot->GetName(), spec->path, spec->sourceNode, spec->destinationNode);
        return ArrivalResult::Held;
    }

    if (!pathValid || bot->GetExactDist2d(source->x, source->y) > static_cast<float>(kArrivalSourceNodeYards))
    {
        c.arrivalPhase = ArrivalPhase::Failed;
        LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=native_path_invalid path={} nodes={}->{}",
                 bot->GetName(), spec->path, spec->sourceNode, spec->destinationNode);
        return ArrivalResult::Failed;
    }

    return activateNative("native");
}

static AutoWowTransports::LoadedTransportFacts LoadedFacts(AutoWowTransports::NorthrendTransportSpec const& spec,
                                                           Transport const* transport)
{
    using namespace AutoWowTransports;
    LoadedTransportFacts f;
    if (!transport)
        return f;
    f.entry = transport->GetEntry();
    GameObjectTemplate const* info = transport->GetGOInfo();
    f.motionTransport = info && info->type == GAMEOBJECT_TYPE_MO_TRANSPORT;
    f.taxiPath = f.motionTransport ? info->moTransport.taxiPathId : 0;
    if (spec.taxiPath >= sTaxiPathNodesByPath.size())
        return f;
    TaxiPathNodeList const& nodes = sTaxiPathNodesByPath[spec.taxiPath];
    if (spec.sourceStop < nodes.size() && nodes[spec.sourceStop])
    {
        TaxiPathNodeEntry const* n = nodes[spec.sourceStop];
        f.sourceNodePresent = true;
        f.sourceIndex = n->index;
        f.sourceMap = n->mapid;
        f.sourceDelay = n->delay;
        f.sourceX = n->x;
        f.sourceY = n->y;
        f.sourceZ = n->z;
    }
    if (spec.destinationStop < nodes.size() && nodes[spec.destinationStop])
    {
        TaxiPathNodeEntry const* n = nodes[spec.destinationStop];
        f.destinationNodePresent = true;
        f.destinationIndex = n->index;
        f.destinationMap = n->mapid;
        f.destinationDelay = n->delay;
        f.destinationX = n->x;
        f.destinationY = n->y;
        f.destinationZ = n->z;
    }
    return f;
}

static AutoWowTransports::CurrentTransportFacts CurrentFacts(Transport const* transport)
{
    using namespace AutoWowTransports;
    CurrentTransportFacts f;
    MotionTransport const* motion = transport ? transport->ToMotionTransport() : nullptr;
    if (!motion || !motion->GetPeriod())
        return f;
    std::uint32_t const timer = motion->GetPathProgress() % motion->GetPeriod();
    for (KeyFrame const& frame : motion->GetKeyFrames())
    {
        if (!frame.Node || !frame.IsStopFrame())
            continue;
        bool const stopped = frame.ArriveTime > frame.DepartureTime
                                 ? timer >= frame.ArriveTime || timer < frame.DepartureTime
                                 : timer >= frame.ArriveTime && timer < frame.DepartureTime;
        if (!stopped)
            continue;
        f.stoppedFrame = true;
        f.nodeIndex = frame.Node->index;
        f.map = motion->GetMapId();
        f.x = motion->GetPositionX();
        f.y = motion->GetPositionY();
        f.z = motion->GetPositionZ();
        return f;
    }
    return f;
}

static bool ChainStep(Player* bot, PlayerbotAI* botAI, AutoWowZoneProgression::BotState& s,
                      AutoWowTransports::ChainState& c, std::uint64_t now, WorldPosition const& hub,
                      WalkFn const& walk)
{
    using namespace AutoWowTransports;
    NewRpgInfo& info = botAI->rpgInfo;
    Params const& p = detail::gParams;
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    std::vector<Crossing> const chain = ChainFor(detail::gCrossings, team, s.route.from, s.route.to);
    bool const northrendOwned = AutoWowZoneProgression::IsNorthrendEntry(s.route);

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
        if (walk(pos))
            ++s.reissues;
        return true;
    };

    if (c.leg >= chain.size())
    {
        if (northrendOwned && !PassageComplete(c))
            return true;
        // A first Outland entry cannot open the terminal army walk until its exact native arrival
        // taxi has been observed landing. ArrivalServiceStep owns this state before ChainStep runs.
        if (!TerminalWalkAllowed(ArrivalRequired(s, chain), c.arrivalPhase))
            return true;
        c.legs.Open(Leg::Walk, now);
        return walkTo(hub.GetMapId(), hub.GetPositionX(), hub.GetPositionY(), hub.GetPositionZ());
    }
    Crossing const& x = chain[c.leg];
    NorthrendTransportSpec const* native = northrendOwned && x.via == Via::Transport
                                               ? NorthrendTransportFor(team, x.object)
                                               : nullptr;
    float const approachX = native ? native->sourceX : static_cast<float>(x.x);
    float const approachY = native ? native->sourceY : static_cast<float>(x.y);
    float const approachZ = native ? native->sourceZ : static_cast<float>(x.z);
    float const exitX = native ? native->destinationX : static_cast<float>(x.exitX);
    float const exitY = native ? native->destinationY : static_cast<float>(x.exitY);
    float const exitZ = native ? native->destinationZ : static_cast<float>(x.exitZ);
    std::int32_t const bx = static_cast<std::int32_t>(bot->GetPositionX());
    std::int32_t const by = static_cast<std::int32_t>(bot->GetPositionY());
    std::int64_t const approach2 = std::int64_t(p.approachYards) * p.approachYards;
    std::int64_t const exit2 = std::int64_t(p.exitYards) * p.exitYards;
    std::int64_t const docked2 = std::int64_t(p.dockedYards) * p.dockedYards;
    auto const within = [](float ax, float ay, float bx, float by, float yards)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return dx * dx + dy * dy <= yards * yards;
    };

    Obs o;
    o.atApproach = bot->GetMapId() == x.map &&
        (native ? within(approachX, approachY, bot->GetPositionX(), bot->GetPositionY(), float(p.approachYards))
                : Dist2(x.x, x.y, bx, by) <= approach2);
    o.atExit = bot->GetMapId() == x.exitMap &&
        (native ? within(exitX, exitY, bot->GetPositionX(), bot->GetPositionY(), float(p.exitYards))
                : Dist2(x.exitX, x.exitY, bx, by) <= exit2);
    bool const portal = x.via == Via::Transport &&
                        UsePortal(EffectiveMode(northrendOwned, x.via, detail::gOverrides, p.mode, x.object),
                                  c.failedRides, p.autoPortalAfter);
    bool const ride = x.via == Via::Transport && !portal;
    Transport* ship = nullptr;  // this crossing's transport docked at the boarding stop
    bool exactTransport = false;
    bool atSourceStop = false;
    std::uint64_t transportGuid = 0;
    if (ride)
    {
        Transport* const t = bot->GetTransport();
        auto const matches = [&](Transport const* candidate)
        {
            if (!candidate || candidate->GetEntry() != x.object)
                return false;
            return !northrendOwned || (native && LoadedTransportMatches(*native, LoadedFacts(*native, candidate)));
        };
        o.onTransport = matches(t);
        if (o.onTransport)
        {
            if (native)
            {
                CurrentTransportFacts const current = CurrentFacts(t);
                atSourceStop = AtNativeStop(*native, false, current, float(p.dockedYards));
                o.dockedExit = AtNativeStop(*native, true, current, float(p.dockedYards));
            }
            else
            {
                atSourceStop = bot->GetMapId() == x.map &&
                    Dist2(x.stopX, x.stopY, static_cast<std::int32_t>(t->GetPositionX()),
                          static_cast<std::int32_t>(t->GetPositionY())) <= docked2;
                o.dockedExit = bot->GetMapId() == x.exitMap &&
                    Dist2(x.exitStopX, x.exitStopY, static_cast<std::int32_t>(t->GetPositionX()),
                          static_cast<std::int32_t>(t->GetPositionY())) <= docked2;
            }
            exactTransport = native && native->passage != NorthrendPassageId::None;
            transportGuid = exactTransport ? t->GetGUID().GetRawValue() : 0;
        }
        else if (bot->GetMapId() == x.map)
            for (Transport* candidate : bot->GetMap()->GetAllTransports())
            {
                bool const stoppedHere = native
                    ? AtNativeStop(*native, false, CurrentFacts(candidate), float(p.dockedYards))
                    : Dist2(x.stopX, x.stopY, static_cast<std::int32_t>(candidate->GetPositionX()),
                            static_cast<std::int32_t>(candidate->GetPositionY())) <= docked2;
                if (matches(candidate) && stoppedHere)
                {
                    ship = candidate;
                    o.dockedHere = true;
                    break;
                }
            }
    }

    if (native && native->passage != NorthrendPassageId::None)
    {
        bool const before = PassageComplete(c);
        DisembarkFacts const disembark{bot->GetTransport() != nullptr, bot->IsInFlight(),
                                       bot->IsBeingTeleported(), bot->GetMapId(), bot->GetPositionX(),
                                       bot->GetPositionY(), bot->GetPositionZ()};
        ObservePassage(c, PassageObservation{native->passage, transportGuid, exactTransport, atSourceStop,
                                             o.dockedExit,
                                             exactTransport && bot->GetMapId() == native->destinationMap,
                                             PhysicalDisembark(*native, disembark, float(p.exitYards))});
        if (!before && PassageComplete(c))
            LOG_INFO("playerbots", "[Transports] bot={} passage_id={} transport_guid={} entry={} path={} "
                                   "stops={}->{} receipt=complete",
                     bot->GetName(), static_cast<std::uint32_t>(native->passage), c.passageTransportGuid,
                     native->entry, native->taxiPath, native->sourceStop, native->destinationStop);
    }

    Step next = ride ? NextTransportStep(c.step, o) : NextObjectStep(o);
    if (native && native->passage != NorthrendPassageId::None && next == Step::Done && !PassageComplete(c))
        next = Step::Disembark;
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
        bool const boardDiagnostic = northrendOwned && team == kGromgolOrgrimmar.team && native == &kGromgolOrgrimmar &&
                                     x.object == kGromgolOrgrimmar.entry &&
                                     s.route.to == AutoWowZoneProgression::kBoreanZone &&
                                     (next == Step::Board || c.step == Step::Board);
        if (boardDiagnostic && next == Step::Board && ship)
        {
            LoadedTransportFacts const candidateFacts = LoadedFacts(*native, ship);
            CurrentTransportFacts const candidateCurrent = CurrentFacts(ship);
            LOG_INFO("playerbots",
                     "[Transports] bot={} boarding_diag phase=issued step={}->{} candidate_guid={} entry={} path={} "
                     "node={} stopped={} ship=({},{},{}) bot=({},{},{}) target=({},{},{}) moving={}",
                     bot->GetName(), StepName(c.step), StepName(next), ship->GetGUID().GetRawValue(), ship->GetEntry(),
                     candidateFacts.taxiPath, candidateCurrent.nodeIndex, candidateCurrent.stoppedFrame,
                     ship->GetPositionX(), ship->GetPositionY(), ship->GetPositionZ(), bot->GetPositionX(),
                     bot->GetPositionY(), bot->GetPositionZ(), ship->GetPositionX(), ship->GetPositionY(), approachZ,
                     bot->isMoving());
        }
        else if (boardDiagnostic && c.step == Step::Board)
        {
            Transport* const raw = bot->GetTransport();
            LoadedTransportFacts const rawFacts = raw ? LoadedFacts(*native, raw) : LoadedTransportFacts{};
            bool const rawMatch = raw && raw->GetEntry() == x.object && LoadedTransportMatches(*native, rawFacts);
            Transport* const candidate = ship ? ship : (rawMatch ? raw : nullptr);
            CurrentTransportFacts const candidateCurrent = CurrentFacts(candidate);
            if (raw)
            {
                LOG_INFO("playerbots",
                         "[Transports] bot={} boarding_diag phase=evaluated step={}->{} bot=({},{},{}) moving={} "
                         "raw_guid={} raw_entry={} raw_match={} candidate_guid={} node={} stopped={} ship=({},{},{}) "
                         "raw_path={}",
                         bot->GetName(), StepName(c.step), StepName(next), bot->GetPositionX(), bot->GetPositionY(),
                         bot->GetPositionZ(), bot->isMoving(), raw->GetGUID().GetRawValue(), raw->GetEntry(), rawMatch,
                         candidate ? candidate->GetGUID().GetRawValue() : std::uint64_t{0}, candidateCurrent.nodeIndex,
                         candidateCurrent.stoppedFrame, candidate ? candidate->GetPositionX() : 0.0f,
                         candidate ? candidate->GetPositionY() : 0.0f, candidate ? candidate->GetPositionZ() : 0.0f,
                         rawFacts.taxiPath);
                if (!rawMatch)
                    LOG_INFO("playerbots",
                             "[Transports] bot={} boarding_diag phase=raw_rejected raw_guid={} motion={} path={} "
                             "src=({},{},{},{},{},{},{}) dst=({},{},{},{},{},{},{})",
                             bot->GetName(), raw->GetGUID().GetRawValue(), rawFacts.motionTransport, rawFacts.taxiPath,
                             rawFacts.sourceNodePresent, rawFacts.sourceIndex, rawFacts.sourceMap, rawFacts.sourceDelay,
                             rawFacts.sourceX, rawFacts.sourceY, rawFacts.sourceZ, rawFacts.destinationNodePresent,
                             rawFacts.destinationIndex, rawFacts.destinationMap, rawFacts.destinationDelay,
                             rawFacts.destinationX, rawFacts.destinationY, rawFacts.destinationZ);
            }
            else
            {
                Transport* const positional = bot->GetMap()->GetTransportForPos(
                    bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot);
                LoadedTransportFacts const positionalFacts =
                    positional ? LoadedFacts(*native, positional) : LoadedTransportFacts{};
                bool const positionalMatch = positional && positional->GetEntry() == x.object &&
                                             LoadedTransportMatches(*native, positionalFacts);
                LOG_INFO("playerbots",
                         "[Transports] bot={} boarding_diag phase=evaluated step={}->{} bot=({},{},{}) moving={} "
                         "raw_present=false candidate_guid={} node={} stopped={} ship=({},{},{}) positional_guid={} "
                         "positional_entry={} positional_path={} positional_match={}",
                         bot->GetName(), StepName(c.step), StepName(next), bot->GetPositionX(), bot->GetPositionY(),
                         bot->GetPositionZ(), bot->isMoving(),
                         candidate ? candidate->GetGUID().GetRawValue() : std::uint64_t{0}, candidateCurrent.nodeIndex,
                         candidateCurrent.stoppedFrame, candidate ? candidate->GetPositionX() : 0.0f,
                         candidate ? candidate->GetPositionY() : 0.0f, candidate ? candidate->GetPositionZ() : 0.0f,
                         positional ? positional->GetGUID().GetRawValue() : std::uint64_t{0},
                         positional ? positional->GetEntry() : 0, positionalFacts.taxiPath, positionalMatch);
            }
        }
        c.step = next;
        c.stepAt = now;
    }

    switch (c.step)
    {
        case Step::Approach:
            c.legs.Open(Leg::Walk, now);
            return walkTo(x.map, approachX, approachY, approachZ);
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
                // AutoWow.ZoneProgression.Outland: the Dark Portal trigger is a box 3.8 yd deep (no radius, no
                // tolerance in Player::IsInAreaTriggerRadius); a bot stopped inside approachYards but outside
                // the box steps onto the trigger point.
                if (AutoWowZoneProgression::OutlandEnabled() && !bot->isMoving() &&
                    bot->GetExactDist2d(float(x.x), float(x.y)) > 1.0f)
                    bot->GetMotionMaster()->MovePoint(0, float(x.x), float(x.y), float(x.z));
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
                bot->GetMotionMaster()->MovePoint(0, ship->GetPositionX(), ship->GetPositionY(), approachZ,
                                                  FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            return true;
        case Step::Disembark:
            c.legs.Open(Leg::Transport, now);
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, exitX, exitY, exitZ,
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
        // AutoWow.Unstick.V2: a fare the bot cannot pay is no flight (soak-s51: 361 of 389 graduation flights
        // gave up, ActivateTaxiPathTo refusing bots with 1-424 copper a 530-copper fare, nine re-issues each).
        if (!path.empty() && AutoWowUnstickV2::Enabled() && bot->GetMoney() < AutoWowUnstickV2::TaxiFare(path))
            continue;
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
                                 AutoWowTransports::ChainState& c, std::uint64_t now, WorldPosition const& target,
                                 WalkFn const& walk)
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
        bool const acted = ChainStep(bot, botAI, s, c, now, target, walk);
        store();
        return acted;
    }
    if (info.GetStatus() == RPG_TRAVEL_FLIGHT)
        return false;  // the flight status owns the leg; landing returns the bot to Idle
    if (s.mode == Mode::Walk)
    {
        AutoWowZoneProgression::NoteWalkTick(s, walk(target));
        store();
        return true;
    }
    ++s.reissues;
    uint32 fmEntry = 0;
    WorldPosition fmPos;
    std::vector<uint32> path;
    bool const flight = !s.noFlight && bot->GetZoneId() != s.route.to &&
                        FindZoneFlight(bot, s.route.to, fmEntry, fmPos, path);
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    std::vector<AutoWowTransports::Crossing> const chain =
        AutoWowTransports::ChainFor(AutoWowTransports::detail::gCrossings, team, s.route.from, s.route.to);
    Mode const mode = AutoWowZoneProgression::IsNorthrendEntry(s.route) && !chain.empty()
                          ? Mode::Chain
                          : AutoWowTransports::SelectMode(flight, bot->GetMapId() == s.route.map,
                                                         s.route.crossing && bot->GetZoneId() != s.route.to,
                                                         !chain.empty());
    if (mode != Mode::Unreachable)
        s.mode = mode;
    if (mode == Mode::Chain)
    {
        c.leg = AutoWowTransports::StartLeg(chain, bot->GetMapId(), static_cast<std::int32_t>(bot->GetPositionX()),
                                            static_cast<std::int32_t>(bot->GetPositionY()),
                                            static_cast<std::int32_t>(AutoWowTransports::detail::gParams.joinYards));
        c.step = AutoWowTransports::Step::Approach;
        c.stepAt = now;
        if (AutoWowZoneProgression::IsNorthrendEntry(s.route))
        {
            AutoWowTransports::NorthrendTransportSpec const* passage =
                AutoWowTransports::NorthrendPassageFor(team);
            c.passage = passage ? passage->passage : AutoWowTransports::NorthrendPassageId::None;
        }
        LOG_INFO("playerbots", "[Transports] bot={} chain to={} crossings={} start_leg={}", bot->GetName(),
                 s.route.to, chain.size(), c.leg);
        bool const acted = ChainStep(bot, botAI, s, c, now, target, walk);
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
        walk(target);
    else
        return false;  // counted; the next tick retries until TravelExhausted
    return true;
}

// Ledger `zone_move` + log line for a trip that arrived or was abandoned.
static void EmitMove(Player* bot, AutoWowZoneProgression::BotState const& s, AutoWowTransports::ChainState& chain,
                     std::uint64_t now, bool arrived, bool transports)
{
    using namespace AutoWowZoneProgression;
    if (AutoWowQuestLedger::Enabled() && transports)
    {
        // AutoWow.Transports: append the per-leg log (fields append-only, event id unchanged).
        chain.legs.Close(now);
        std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
        AutoWowTransports::NorthrendTransportSpec const* passage =
            IsNorthrendEntry(s.route) ? AutoWowTransports::NorthrendPassageFor(team) : nullptr;
        AutoWowQuestLedger::EmitZoneMove(
            bot, TriggerName(s.trigger),
            LedgerFields(s.fromZone, s.route.to, now >= s.startMs ? now - s.startMs : 0, arrived, s.mode) +
                AutoWowTransports::LegsField(chain.legs) + AutoWowTransports::PassageField(chain, passage));
    }
    else if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitZoneMove(bot, TriggerName(s.trigger),
            LedgerFields(s.fromZone, s.route.to, now >= s.startMs ? now - s.startMs : 0, arrived, s.mode));
    LOG_INFO("playerbots", "[ZoneProgression] bot={} {} from={} to={} lvl={} trigger={} mode={} ms={}",
             bot->GetName(), arrived ? "arrived" : "gave_up", s.fromZone, s.route.to, bot->GetLevel(),
             TriggerName(s.trigger), ModeName(s.mode), now - s.startMs);
}

// Independent, open-world, idle-able bot that zone progression may move. Keep the predicate and the
// relocation diagnostic sourced from one classifier so an exclusion can never become permission.
static AutoWowDeathLoop::RelocationBlock MovementBlock(Player* bot, PlayerbotAI* botAI)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive())
        return AutoWowDeathLoop::RelocationBlock::Dead;
    if (bot->IsInFlight())
        return AutoWowDeathLoop::RelocationBlock::Flight;
    if (bot->IsInCombat())
        return AutoWowDeathLoop::RelocationBlock::Combat;
    if (!botAI->IsAutoWowIndependentParty())
        return AutoWowDeathLoop::RelocationBlock::Controlled;
    if (AutoWowOracleRuntime::IsManagedBot(guid))
        return AutoWowDeathLoop::RelocationBlock::Oracle;
    if (!bot->GetMap())
        return AutoWowDeathLoop::RelocationBlock::NoMap;
    if (bot->GetMap()->Instanceable())
        return AutoWowDeathLoop::RelocationBlock::Instance;
    // AutoWow.Errands: a town run under way owns the bot (flag off: never true).
    if (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid))
        return AutoWowDeathLoop::RelocationBlock::Errand;
    return AutoWowDeathLoop::RelocationBlock::None;
}

static bool Movable(Player* bot, PlayerbotAI* botAI)
{
    return MovementBlock(bot, botAI) == AutoWowDeathLoop::RelocationBlock::None;
}

// Stricter admission ownership is local to Northrend. MovementBlock also serves existing grouped
// death-loop/escape behavior and must retain its S97 contract.
static bool NorthrendControlBlocked(Player* bot, PlayerbotAI* botAI)
{
    return botAI->IsRealPlayer() || botAI->HasRealPlayerMaster() || botAI->IsAutoWowPaused() ||
           bot->GetGroup() != nullptr;
}

static void NoteEscapeBlock(Player* bot, AutoWowDeathLoop::RelocationBlock reason)
{
    if (AutoWowDeathLoop::NoteRelocationBlock(bot->GetGUID().GetCounter(), reason))
        LOG_INFO("playerbots", "[DeathLoop] bot={} relocate_pending reason={}", bot->GetName(),
                 AutoWowDeathLoop::RelocationBlockName(reason));
}

// AutoWow.DeathLoop.EscapeViaZoneProgression: start a death_loop trip to the nearest level-appropriate hub
// (abandoning, with its zone_move, any trip under way), or move a running escape onto the portal leg once
// the bot has EscapePortalDeaths recent deaths (Transports mode auto/portal only). False when no hub fits.
static bool StartEscape(Player* bot, AutoWowZoneProgression::BotState& s, AutoWowTransports::ChainState& chain,
                        std::uint64_t now, bool transports)
{
    using namespace AutoWowZoneProgression;
    using AutoWowZoneProgression::Mode;
    using AutoWowZoneProgression::Phase;
    using AutoWowZoneProgression::Route;
    using AutoWowZoneProgression::Trigger;
    std::uint32_t const deaths = AutoWowDeathLoop::RecentDeaths(bot->GetGUID().GetCounter());
    bool const portal = transports &&
                        AutoWowTransports::detail::gParams.mode != AutoWowTransports::TransportMode::Real &&
                        AutoWowDeathLoop::EscapePortalNow(deaths, AutoWowDeathLoop::detail::gParams.escapePortalDeaths);
    if (s.phase == Phase::Travel && s.trigger == Trigger::DeathLoop)
    {
        if (portal && s.mode != Mode::Portal)
        {
            LOG_INFO("playerbots", "[ZoneProgression] bot={} escape to={} portal deaths={}", bot->GetName(),
                     s.route.to, deaths);
            s.mode = Mode::Portal;
            s.reissues = 0;
        }
        return true;
    }
    std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    std::int32_t const bx = static_cast<std::int32_t>(std::floor(bot->GetPositionX()));
    std::int32_t const by = static_cast<std::int32_t>(std::floor(bot->GetPositionY()));
    Map* const map = bot->GetMap();
    std::uint32_t const level = bot->GetLevel();
    std::uint32_t const zone = bot->GetZoneId();
    std::uint32_t const margin = AutoWowDeathLoop::detail::gHardParams.walkZoneMargin;
    auto const zoneAt = [map](std::int32_t x, std::int32_t y)
    { return AutoWowDeathLoop::ZoneAt(map, float(x), float(y)); };
    auto const danger = [level, margin](std::uint32_t z)
    { return AutoWowDeathLoop::Overshoot(AutoWowDeathLoop::ZoneMinLevel(z), level, margin); };
    auto const crossesDanger = [&](Route const& r)
    { return SegmentCrossesDanger(bx, by, r.x, r.y, kDangerStepYards, zone, zoneAt, danger); };
    Route const* hub = nullptr;
    if (AutoWowDeathLoop::HardEscapeEnabled())
    {
        // AutoWow.Survival.HardEscape (1): the nearest hub whose straight line crosses no zone bracketed
        // more than WalkZoneMargin above the bot (the bot's own zone excepted).
        hub = PickSafeEscapeRoute(detail::gRoutes, team, level, zone, bot->GetMapId(), bx, by, crossesDanger);
    }
    // AutoWow.DeathLoop.V2: back toward the lowest level band that fits, not the nearest same-level hub.
    else
        hub = AutoWowDeathLoop::V2Enabled()
                  ? PickLowEscapeRoute(detail::gRoutes, team, bot->GetLevel(), bot->GetZoneId(), bot->GetMapId(), bx, by)
                  : PickEscapeRoute(detail::gRoutes, team, bot->GetLevel(), bot->GetZoneId(), bot->GetMapId(), bx, by);
    Route const* const crossZoneHub = hub;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    hub = PickEscapeOrSafeSameZoneHub(crossZoneHub, detail::gRoutes, team, level, zone, bot->GetMapId(), bx, by,
                                      [&](Route const& r)
                                      {
                                          return crossesDanger(r) ||
                                                 AutoWowDeathLoop::IsDangerous(guid, r.map, float(r.x), float(r.y));
                                      });
    bool const sameZoneFallback = !crossZoneHub && hub != nullptr;
    if (!hub)
        return false;
    if (sameZoneFallback)
        LOG_INFO("playerbots", "[DeathLoop] bot={} same_zone_fallback zone={} hub=({}, {}) lvl={}", bot->GetName(),
                 zone, hub->x, hub->y, level);
    if (s.phase != Phase::None)
        EmitMove(bot, s, chain, now, false, transports);
    BeginEscape(s, *hub, bot->GetZoneId(), now, portal);
    if (transports)
        chain = AutoWowTransports::ChainState{};
    LOG_INFO("playerbots", "[ZoneProgression] bot={} escape from={} to={} lvl={} deaths={} mode={}", bot->GetName(),
             s.fromZone, s.route.to, bot->GetLevel(), deaths, ModeName(s.mode));
    return true;
}

AutoWowDeathLoop::RelocationAttempt NewRpgBaseAction::TryDeathLoopEscape()
{
    using AutoWowDeathLoop::RelocationAttempt;
    uint32 const guid = bot->GetGUID().GetCounter();
    AutoWowDeathLoop::RelocationBlock const block = MovementBlock(bot, botAI);
    if (block != AutoWowDeathLoop::RelocationBlock::None)
    {
        NoteEscapeBlock(bot, block);
        return RelocationAttempt::TemporaryBlocked;
    }
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    AutoWowZoneProgression::BotState s = AutoWowZoneProgression::LoadState(guid);
    bool const transports = AutoWowTransports::Enabled();
    AutoWowTransports::ChainState chain =
        transports ? AutoWowZoneProgression::LoadChain(guid) : AutoWowTransports::ChainState{};
    if (!StartEscape(bot, s, chain, now, transports))
    {
        NoteEscapeBlock(bot, AutoWowDeathLoop::RelocationBlock::NoRoute);
        return RelocationAttempt::NoRoute;
    }
    AutoWowZoneProgression::StoreState(guid, s);
    if (transports)
        AutoWowZoneProgression::StoreChain(guid, chain);
    NoteEscapeBlock(bot, AutoWowDeathLoop::RelocationBlock::None);
    botAI->rpgInfo.ChangeToIdle();
    return RelocationAttempt::Started;
}

bool NewRpgBaseAction::DeathLoopEscape()
{
    return TryDeathLoopEscape() == AutoWowDeathLoop::RelocationAttempt::Started;
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
    Params const& p = detail::gParams;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    BotState s = LoadState(guid);
    NewRpgInfo& info = botAI->rpgInfo;
    bool const transports = AutoWowTransports::Enabled();
    AutoWowTransports::ChainState chain = transports ? LoadChain(guid) : AutoWowTransports::ChainState{};

    if (transports && chain.version != AutoWowTransports::kStateVersion)
    {
        bool const tripActive = s.phase != Phase::None;
        bool const foreignMotion = bot->GetTransport() || bot->IsBeingTeleported() || bot->isMoving() ||
                                   info.GetStatus() != RPG_IDLE;
        if (!AutoWowTransports::CanResetChainVersion(chain.version, tripActive, foreignMotion))
            return false;  // preserve active/foreign or newer ownership; never clear its motion
        chain = AutoWowTransports::ChainState{};
        StoreChain(guid, chain);
    }

    if (s.phase != Phase::None && IsNorthrendEntry(s.route) && NorthrendControlBlocked(bot, botAI))
        return false;

    AutoWowDeathLoop::RelocationBlock const movementBlock = MovementBlock(bot, botAI);
    if (movementBlock == AutoWowDeathLoop::RelocationBlock::Flight && !ArrivalReceiptBlocked(bot, botAI) && transports &&
        (chain.arrivalPhase == AutoWowTransports::ArrivalPhase::AwaitFlightStart ||
         chain.arrivalPhase == AutoWowTransports::ArrivalPhase::AwaitFlightEnd))
    {
        std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
        AutoWowTransports::ArrivalSpec const* spec = AutoWowTransports::ArrivalForTeam(team);
        std::vector<AutoWowTransports::Crossing> const arrivalChain = AutoWowTransports::ChainFor(
            AutoWowTransports::detail::gCrossings, team, s.route.from, s.route.to);
        bool const owned = spec && ArrivalRequired(s, arrivalChain) &&
                           chain.arrivalExpectedSource == spec->sourceNode &&
                           chain.arrivalExpectedDestination == spec->destinationNode;
        AutoWowTransports::ArrivalReceipt const receipt =
            owned ? ObserveArrivalFlight(bot, *spec, chain, now, &s) : AutoWowTransports::ArrivalReceipt::None;
        if (receipt == AutoWowTransports::ArrivalReceipt::FlightStarted ||
            receipt == AutoWowTransports::ArrivalReceipt::FlightActive)
        {
            if (receipt == AutoWowTransports::ArrivalReceipt::FlightStarted)
            {
                LOG_INFO("playerbots", "[Transports] bot={} arrival_flight_started path={} nodes={}->{} menu={}",
                         bot->GetName(), spec->path, spec->sourceNode, spec->destinationNode,
                         chain.arrivalUsedMenu);
            }
            StoreChain(guid, chain);
            StoreState(guid, s);
            return true;
        }
    }
    if (!Movable(bot, botAI))
    {
        if (transports && HoldArrivalClock(chain, now))
            StoreChain(guid, chain);
        return false;
    }

    auto finish = [&](bool arrived)
    {
        EmitMove(bot, s, chain, now, arrived, transports);
        if (arrived && s.trigger == Trigger::DeathLoop)
            AutoWowDeathLoop::CompleteEscapeArrival(guid, true);
    };

    // AutoWow.DeathLoop.EscapeViaZoneProgression: a bot whose zone bracket starts more than
    // RelocateLevelMargin above it escapes to the nearest level hub even without an escalated death.
    if (AutoWowDeathLoop::EscapeEnabled() && s.phase == Phase::None && now >= s.nextCheckMs &&
        now >= s.cooldownUntilMs &&
        AutoWowDeathLoop::Overshoot(sTravelMgr.GetZoneBracketLow(bot->GetZoneId()), bot->GetLevel(),
                                    AutoWowDeathLoop::detail::gParams.relocateLevelMargin) &&
        StartEscape(bot, s, chain, now, transports))
    {
        StoreState(guid, s);
        if (transports)
            StoreChain(guid, chain);
    }

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
        Trigger trigger = Evaluate(p, bot->GetLevel(), zoneMax, s.stall, route != nullptr);
        // AutoWow.Unstick.V2: no trigger fired, but the bot has earned no XP for NoXpMs and no grind spot lies
        // within GrindMaxYards (soak-s49..s51 town trap) -> graduate: the zone's own route when one fits, else
        // the nearest hub whose band fits the level, leaving from the bot's zone (no road table, as an escape).
        Route unstick;
        if (trigger == Trigger::None && AutoWowUnstickV2::Enabled() && AutoWowUnstickV2::XpStalled(bot, now) &&
            SelectRandomGrindPos(bot) == WorldPosition())
        {
            Route const* hub = route;
            if (!hub)
                hub = AutoWowUnstickV2::PickUnstickRoute(
                    detail::gRoutes, team, bot->GetLevel(), p.levelMargin, zone, bot->GetMapId(),
                    static_cast<std::int32_t>(std::floor(bot->GetPositionX())),
                    static_cast<std::int32_t>(std::floor(bot->GetPositionY())));
            if (hub)
            {
                unstick = *hub;
                if (!route)
                    unstick.from = zone;
                route = &unstick;
                trigger = Trigger::Stuck;
            }
        }
        // AutoWow.Contracts: a running hunt contract defers graduation (the stall count keeps counting); the
        // bot graduates at the next check after the contract ends. Death-loop escapes do not wait.
        bool const huntingContract =
            (trigger == Trigger::Level || trigger == Trigger::NoQuests || trigger == Trigger::Stuck) &&
            AutoWowContracts::Enabled() && AutoWowContracts::Snapshot(guid).phase != AutoWowContracts::Phase::None;
        // AutoWow.Squad: a squad member holds its tier while its squad has workable material demand.
        bool const squadHold = (trigger == Trigger::Level || trigger == Trigger::NoQuests || trigger == Trigger::Stuck) &&
                                AutoWowSquad::Enabled() && AutoWowSquad::HoldsTier(bot);
        if (trigger == Trigger::None || huntingContract || squadHold)
        {
            StoreState(guid, s);
            return false;
        }
        if (route && IsNorthrendEntry(*route) && NorthrendControlBlocked(bot, botAI))
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
        s.roadJoined = false;
        s.wp = 0;
        LOG_INFO("playerbots", "[ZoneProgression] bot={} graduate from={} to={} lvl={} trigger={}", bot->GetName(),
                 zone, route->to, bot->GetLevel(), TriggerName(trigger));
        if (transports)
        {
            chain = AutoWowTransports::ChainState{};
            StoreChain(guid, chain);
        }
    }

    WorldPosition const target(s.route.map, float(s.route.x), float(s.route.y), float(s.route.z));

    // Next walk goal toward the hub: the route's road point (joined at the nearest, advanced within 20 yd),
    // the hub itself once past the road, in the destination zone, or off the route's map. Updates s.wp.
    auto walkTarget = [&]() -> WorldPosition
    {
        if (bot->GetMapId() != s.route.map || bot->GetZoneId() == s.route.to)
            return target;
        std::vector<RoadPoint> const road = RoadFor(gRoads, s.route.from, s.route.to);
        std::int32_t const bx = static_cast<std::int32_t>(std::floor(bot->GetPositionX()));
        std::int32_t const by = static_cast<std::int32_t>(std::floor(bot->GetPositionY()));
        if (!s.roadJoined)
        {
            // AutoWow.Walking.V2: never join a road point the bot is already past.
            s.wp = sPlayerbotAIConfig.autoWowWalkingV2 ? JoinRoadAhead(road, bx, by) : JoinRoad(road, bx, by);
            s.roadJoined = true;
        }
        s.wp = AdvanceRoad(road, s.wp, bx, by, 20);
        if (s.wp >= road.size())
            return target;
        RoadPoint const& r = road[s.wp];
        return WorldPosition(s.route.map, float(r.x), float(r.y), float(r.z));
    };

    if (s.phase == Phase::Travel)
    {
        std::uint32_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
        std::vector<AutoWowTransports::Crossing> const arrivalChain = transports
            ? AutoWowTransports::ChainFor(AutoWowTransports::detail::gCrossings, team, s.route.from, s.route.to)
            : std::vector<AutoWowTransports::Crossing>{};
        bool const arrivalRequired = transports && ArrivalRequired(s, arrivalChain);
        auto failTrip = [&]()
        {
            if (AutoWowUnstickV2::Enabled())
                AutoWowUnstickV2::NoteGaveUp(guid, now);
            finish(false);
            s = BotState{};
            s.cooldownUntilMs = now + p.cooldownMs;
            chain = AutoWowTransports::ChainState{};
            StoreState(guid, s);
            if (transports)
                StoreChain(guid, chain);
            info.ChangeToIdle();
        };

        bool const northrendEntry = IsNorthrendEntry(s.route);
        if (northrendEntry && s.mode == Mode::Portal)
        {
            LOG_WARN("playerbots", "[Transports] bot={} northrend_failed reason=portal_mode_blocked to={}",
                     bot->GetName(), s.route.to);
            failTrip();
            return true;
        }

        // Legacy initial-entry portal state cannot count as the new native arrival receipt.
        if (arrivalRequired && s.mode == Mode::Portal)
        {
            LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=portal_bypass_blocked to={}",
                     bot->GetName(), s.route.to);
            failTrip();
            return true;
        }
        if (arrivalRequired && chain.leg >= arrivalChain.size() &&
            chain.arrivalPhase != AutoWowTransports::ArrivalPhase::Complete)
        {
            ArrivalResult const arrival = ArrivalServiceStep(
                bot, botAI, s, chain, now, [this](WorldPosition const& pos) { return WalkLeg(pos); });
            StoreState(guid, s);
            StoreChain(guid, chain);
            if (arrival == ArrivalResult::Failed)
            {
                failTrip();
                return true;
            }
            if (arrival == ArrivalResult::Yield)
                return false;
            if (arrival == ArrivalResult::Held)
            {
                if (!chain.arrivalHeldAt && chain.arrivalPhase == AutoWowTransports::ArrivalPhase::ApproachService &&
                    TravelExhausted(p, s, now))
                {
                    LOG_WARN("playerbots", "[Transports] bot={} arrival_failed reason=travel_exhausted attempts={}",
                             bot->GetName(), chain.arrivalAttempts);
                    failTrip();
                }
                return true;
            }
        }

        PhysicalHubFacts const hubFacts{bot->GetTransport() != nullptr, bot->IsInFlight(),
                                            bot->IsBeingTeleported(), bot->GetMapId(), bot->GetPositionX(),
                                            bot->GetPositionY(), bot->GetPositionZ()};
        bool const atHub = northrendEntry ? AtPhysicalNorthrendHub(s.route, hubFacts)
                                          : AtRouteHub(s.route, hubFacts.map, hubFacts.x, hubFacts.y);
        if ((!arrivalRequired || chain.arrivalPhase == AutoWowTransports::ArrivalPhase::Complete) &&
            (!northrendEntry || AutoWowTransports::PassageComplete(chain)) && atHub)
        {
            // At the hub inn: bind the hearthstone there (the innkeeper's own effect), then the flight master.
            if (Creature* inn = bot->FindNearestCreature(s.route.inn, 30.0f))
                bot->SetHomebind(WorldLocation(inn->GetMapId(), inn->GetPositionX(), inn->GetPositionY(),
                                               inn->GetPositionZ(), inn->GetOrientation()),
                                 inn->GetAreaId());
            finish(true);
            if (northrendEntry)
            {
                // Passage admission ends here. Flight points remain ordinary earned world state.
                s = BotState{};
                s.nextCheckMs = now + p.checkIntervalMs;
                chain = AutoWowTransports::ChainState{};
                StoreState(guid, s);
                StoreChain(guid, chain);
                info.ChangeToIdle();
                return true;
            }
            s.phase = Phase::LearnFp;
            s.reissues = 0;
            info.ChangeToIdle();
            StoreState(guid, s);
            return true;
        }
        // AutoWow.Unstick.V2: the flight leg is back in Idle short of the destination zone without having flown
        // (refused taxi, master gone, approach given up): walk the rest of this trip (portal fallback applies).
        if (AutoWowUnstickV2::Enabled() && s.mode == Mode::Flight && !s.noFlight &&
            info.GetStatus() != RPG_TRAVEL_FLIGHT && !bot->IsInFlight() && bot->GetZoneId() != s.route.to)
        {
            LOG_INFO("playerbots", "[Unstick] bot={} flight_failed to={} money={} reissues={} -> walk", bot->GetName(),
                     s.route.to, bot->GetMoney(), s.reissues);
            s.noFlight = true;
            s.mode = Mode::Unreachable;
        }
        // AutoWow.Transports (mode auto/portal; owner ruling 2026-09-24): a spent walk leg portals to the hub.
        if (transports && !arrivalRequired && !northrendEntry)
        {
            bool const allowed = AutoWowTransports::detail::gParams.mode != AutoWowTransports::TransportMode::Real;
            // AutoWow.ZoneProgression.Outland: an entry trip's chain portals like a walk, on OutlandPortalAfterMs.
            bool const entry = OutlandEnabled() && IsOutlandEntry(s.route);
            bool const reached = PortalFallbackDestinationReached(s, bot->GetZoneId(), bot->GetMapId(),
                                                                   bot->GetPositionX(), bot->GetPositionY());
            bool const start = PortalFallback(allowed, FallbackMode(s.mode, entry), reached,
                                              s.reissues, p.maxReissues, now >= s.startMs ? now - s.startMs : 0,
                                              entry ? p.outlandPortalAfterMs : p.portalAfterMs);
            if (start || (s.mode == Mode::Portal && s.reissues <= p.maxReissues))
            {
                if (start)
                {
                    LOG_INFO("playerbots", "[ZoneProgression] bot={} portal fallback to={} after walk_ms={} reissues={}",
                             bot->GetName(), s.route.to, now - s.startMs, s.reissues);
                    s.mode = Mode::Portal;
                    s.reissues = 0;
                }
                chain.legs.Open(AutoWowTransports::Leg::Portal, now);
                if (!bot->IsBeingTeleported())
                {
                    ++s.reissues;
                    if (info.GetStatus() != RPG_IDLE)
                        info.ChangeToIdle();
                    bot->TeleportTo(target.GetMapId(), target.GetPositionX(), target.GetPositionY(),
                                    target.GetPositionZ(), bot->GetOrientation());
                }
                StoreState(guid, s);
                StoreChain(guid, chain);
                return true;
            }
        }
        if (TravelExhausted(p, s, now))
        {
            failTrip();
            return true;
        }
        if (transports)
            return TransportsTravelStep(bot, botAI, s, chain, now, target,
                                        [this, &target, &walkTarget](WorldPosition const& pos)
                                        { return WalkLeg(pos == target ? walkTarget() : pos); });
        NewRpgStatus const status = info.GetStatus();
        if (status == RPG_TRAVEL_FLIGHT)
            return false;  // the flight status owns the leg; landing returns the bot to Idle
        uint32 fmEntry = 0;
        WorldPosition fmPos;
        std::vector<uint32> path;
        // Flight is looked up only while no walk leg is committed (start, after landing, after a stuck).
        bool const flight = !s.noFlight && s.mode != Mode::Walk && bot->GetZoneId() != s.route.to &&
                            FindZoneFlight(bot, s.route.to, fmEntry, fmPos, path);
        Mode const mode = SelectMode(flight, bot->GetMapId() == s.route.map, s.route.crossing && bot->GetZoneId() != s.route.to);
        if (mode == Mode::Flight)
        {
            ++s.reissues;
            s.mode = mode;
            StoreState(guid, s);
            info.ChangeToTravelFlight(fmEntry, fmPos, path);
            return true;
        }
        if (mode == Mode::Unreachable)
        {
            ++s.reissues;  // counted; the next tick retries until TravelExhausted
            StoreState(guid, s);
            return false;
        }
        s.mode = Mode::Walk;
        NoteWalkTick(s, WalkLeg(walkTarget()));
        StoreState(guid, s);
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
    if (WalkLeg(fm->pos))
        ++s.reissues;
    StoreState(guid, s);
    return true;
}

// Root cause of soak-s10-zoneprog-r1 (every trip gave up in 2-37 s): legs were handed to New RPG
// GO_GRIND, whose MoveFarTo for an autonomous bot (watchDestination) rejects any destination the local
// mmap cannot route to (an inter-zone hub is always beyond it) -> MarkTravelDestinationFailed + Idle on
// the first tick, and each re-issue burned the budget. The leg is now walked here, in Idle, with the
// quest-path no-teleport mover: TravelMgr re-anchored prepared walks and stepping stones, `stuck` only
// after the ordinary no-progress window. Returns true when this tick reported stuck (one reissue).
bool NewRpgBaseAction::WalkLeg(WorldPosition const& dest)
{
    if (botAI->rpgInfo.GetStatus() != RPG_IDLE)
        botAI->rpgInfo.ChangeToIdle();
    // AutoWow.Travel.Safe: rest first (below the rest-gate thresholds) - no move is issued, so an eating bot
    // stays seated; the tick is not stuck.
    if (sPlayerbotAIConfig.autoWowTravelSafe && AutoWowRestGate::HoldTravel(botAI))
        return false;
    bool stuck = false;
    MoveFarTo(dest, /*questNoTeleport*/ true, &stuck);
    return stuck;
}

// ---- AutoWow.Gathering.Detours runtime (policy: AutoWow/GatherDetourPolicy.h) ----------------------
namespace AutoWowGatherDetour
{
namespace
{
std::mutex gDetourLock;
std::unordered_map<std::uint32_t, BotState> gDetours;  // touched only with the flag on

BotState LoadDetour(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gDetourLock);
    auto const it = gDetours.find(guid);
    return it == gDetours.end() ? BotState{} : it->second;
}

void StoreDetour(std::uint32_t guid, BotState const& s)
{
    std::lock_guard<std::mutex> guard(gDetourLock);
    gDetours[guid] = s;
}

// Same pick list as LootObject::IsLootPossible.
bool HasMiningPick(Player* bot)
{
    for (uint32 item : {756u, 778u, 1819u, 1893u, 1959u, 2901u, 9465u, 20723u, 40772u, 40892u, 40893u})
        if (bot->HasItemCount(item, 1))
            return true;
    return false;
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Gathering.Detours", false);
    Params& p = detail::gParams;
    p.yards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gathering.DetourYards", 60);
    p.maxDz = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gathering.DetourMaxDz", 20);
    p.timeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Gathering.DetourTimeoutMs", 45000);
    p.anySkill = sConfigMgr->GetOption<bool>("AutoWow.Gather.AnySkill", false);
}
}  // namespace AutoWowGatherDetour

bool NewRpgBaseAction::GatherDetourStep()
{
    using namespace AutoWowGatherDetour;
    using AutoWowGatherDetour::BotState;
    using AutoWowGatherDetour::Params;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive() || bot->IsInFlight() || !botAI->IsAutoWowIndependentParty() ||
        AutoWowOracleRuntime::IsManagedBot(guid) || !bot->GetMap() || bot->GetMap()->Instanceable())
        return false;

    Params const& p = detail::gParams;
    Skills skills;
    if (bot->HasSkill(SKILL_HERBALISM))
        skills.herbalism = bot->GetSkillValue(SKILL_HERBALISM);
    if (bot->HasSkill(SKILL_MINING))
    {
        skills.mining = bot->GetSkillValue(SKILL_MINING);
        skills.miningPick = HasMiningPick(bot);
    }
    // Critical legs: a zone-progression trip, a town run, a flight, a rest (eating/drinking).
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    bool const critical = (AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid)) ||
                          (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)) ||
                          status == RPG_TRAVEL_FLIGHT || status == RPG_REST;
    if (!HasGatherSkill(skills))
        return false;
    BotState s = LoadDetour(guid);
    if (!MayDetour(skills, bot->IsInCombat(), critical))
    {
        if (s.target)
        {
            End(s, false);
            StoreDetour(guid, s);
        }
        return false;
    }

    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    if (TimedOut(p, s, now))
    {
        LOG_DEBUG("playerbots", "[GatherDetour] bot={} gave up node={}", bot->GetName(), s.target);
        End(s, true);
        StoreDetour(guid, s);
        return false;
    }

    bool fresh = false;
    if (!s.target)
    {
        std::int32_t const bx = static_cast<std::int32_t>(std::floor(bot->GetPositionX()));
        std::int32_t const by = static_cast<std::int32_t>(std::floor(bot->GetPositionY()));
        std::int32_t const bz = static_cast<std::int32_t>(std::floor(bot->GetPositionZ()));
        std::vector<Node> nodes;
        for (ObjectGuid const& g : AI_VALUE(GuidVector, "nearest game objects"))
        {
            GameObject* go = botAI->GetGameObject(g);
            if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY)
                continue;
            LootObject lo(bot, g);
            if (lo.IsEmpty() || (lo.skillId != SKILL_HERBALISM && lo.skillId != SKILL_MINING))
                continue;
            nodes.push_back(Node{g.GetRawValue(), lo.skillId, lo.reqSkillValue,
                                 static_cast<std::int32_t>(std::floor(go->GetPositionX())) - bx,
                                 static_cast<std::int32_t>(std::floor(go->GetPositionY())) - by,
                                 static_cast<std::int32_t>(std::floor(go->GetPositionZ())) - bz});
        }
        int const pick = PickNode(p, skills, nodes, SkipList(s));
        if (pick < 0)
            return false;
        Begin(s, nodes[pick].guid, now);
        StoreDetour(guid, s);
        fresh = true;
        LOG_DEBUG("playerbots", "[GatherDetour] bot={} node={} skill={} req={}", bot->GetName(), s.target,
                  nodes[pick].skill, nodes[pick].reqSkill);
    }

    ObjectGuid const target(s.target);
    GameObject* go = botAI->GetGameObject(target);
    if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY || LootObject(bot, target).IsEmpty())
    {
        End(s, false);  // gathered, or gone
        StoreDetour(guid, s);
        return false;
    }
    if (bot->GetExactDist(go) <= INTERACTION_DISTANCE - 2.0f)
    {
        // At the node: the stock loot strategy opens it (gather spell, skill-up); the RPG status resumes.
        AI_VALUE(LootObjectStack*, "available loot")->Add(target);
        return false;
    }
    if (fresh || !bot->isMoving())
        MoveTo(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), false, false, false, true);
    return true;
}
