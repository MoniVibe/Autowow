/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SurvivalRecovery.h"

#include <cmath>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AutoWowQuestLedger.h"
#include "CellImpl.h"
#include "Config.h"
#include "Corpse.h"
#include "Creature.h"
#include "DeathLoopBreaker.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "GatheringWorkerState.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RestGate.h"
#include "TravelMgr.h"

static_assert(AutoWowSafeRevive::kReachYards + 5 == CORPSE_RECLAIM_RADIUS, "reach tracks the core reclaim radius");

namespace
{
constexpr std::uint32_t kHearthstoneItem = 6948;
constexpr std::uint32_t kHearthstoneSpell = 8690;
constexpr std::size_t kMaxBots = 2048;  // hard cap; beyond it new bots are not tracked

using TravelIntentPolicy::MakePoint;
using TravelIntentPolicy::Point;

struct ReviveState
{
    AutoWowDeathLoop::BotState deaths;   // recent deaths (DeathLoop's value functions; no danger areas used)
    std::optional<Point> pendingKiller;  // noted by the kill hook, consumed by the death hook
    std::optional<Point> killer;         // this death's killer
    Point deathSpot;
    bool planned = false;
    AutoWowSafeRevive::Target plan;
    std::uint64_t planMs = 0;
    bool retreatPending = false;
    bool retreating = false;
    float rx = 0.0f;
    float ry = 0.0f;
    float rz = 0.0f;
    std::uint64_t retreatMs = 0;
    bool restPending = false;
    bool relocateOnRes = false;  // V2: this death's plan is a forced spirit healer
    bool relocate = false;       // V2: revived after it, one relocation owed
    bool pendingPvp = false;     // RestSafe: killed by a player (or its pet), noted by the kill hook
    bool pvpDeath = false;       // RestSafe: this death was PvP
};

// AutoWow.Survival.RestSafe: one rest episode (below the rest-gate thresholds, out of combat).
struct RestState
{
    bool planned = false;
    bool moving = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint64_t moveMs = 0;
};

// Deaths, revives and bot AI updates run on map threads. Touched only with a flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, ReviveState> gRevive;
std::unordered_map<std::uint32_t, AutoWowUnstick::BotState> gUnstick;
std::unordered_map<std::uint32_t, RestState> gRest;

std::uint64_t NowMs()
{
    auto const now = GameTime::GetGameTimeMS().count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

std::uint32_t GuidOf(Player const* player) { return static_cast<std::uint32_t>(player->GetGUID().GetCounter()); }

Point Here(Player const* bot)
{
    return MakePoint(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
}

// Autonomous bots only (as the death-loop breaker): no real players, no bots following a real player,
// no gather workers (own recovery state machine).
PlayerbotAI* AutonomousBotAI(Player* player)
{
    PlayerbotAI* ai = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    if (!ai || ai->IsRealPlayer() || ai->HasRealPlayerMaster() || AutoWowGather::IsExplicitWorker(GuidOf(player)))
        return nullptr;
    return ai;
}

bool OpenWorld(Player const* bot)
{
    Map const* map = bot->GetMap();
    return map && !map->Instanceable();
}

bool Solo(Player const* bot)
{
    Group const* group = bot->GetGroup();
    return !group || group->GetMembersCount() <= 1;
}

template <typename T>
T* FindOrCreate(std::unordered_map<std::uint32_t, T>& states, std::uint32_t guid)
{
    auto it = states.find(guid);
    if (it == states.end())
    {
        if (states.size() >= kMaxBots)
            return nullptr;
        it = states.emplace(guid, T{}).first;
    }
    return &it->second;
}

template <typename T>
T* Find(std::unordered_map<std::uint32_t, T>& states, std::uint32_t guid)
{
    auto const it = states.find(guid);
    return it == states.end() ? nullptr : &it->second;
}

// Idle hostile creatures around the bot (as NewRpgBaseAction ScanTravelMobs; the ghost's own grids).
std::vector<WalkingV2Policy::Mob> ScanIdleHostiles(Player* bot, float radius)
{
    std::list<Creature*> found;
    Acore::AllWorldObjectsInRange check(bot, radius);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, radius);
    std::vector<WalkingV2Policy::Mob> mobs;
    for (Creature* c : found)
    {
        if (!c || !c->IsInWorld() || !c->IsAlive() || c->IsInCombat() || c->IsCivilian() ||
            c->HasReactState(REACT_PASSIVE) || !c->IsHostileTo(bot))
            continue;
        WalkingV2Policy::Mob m;
        m.x = static_cast<std::int32_t>(std::floor(c->GetPositionX()));
        m.y = static_cast<std::int32_t>(std::floor(c->GetPositionY()));
        m.level = c->GetLevel();
        m.elite = c->isElite();
        m.aggroYards = static_cast<std::uint32_t>(std::max(0.0f, c->GetAggroRange(bot)));
        mobs.push_back(m);
    }
    return mobs;
}

// SafeRevive.V2: every hostile around the ghost - idle or in combat where it stands, dead ones whose
// respawn is due within kRespawnSoonMs at their home position.
std::vector<WalkingV2Policy::Mob> ScanHostilesV2(Player* bot, float radius)
{
    std::list<Creature*> found;
    Acore::AllWorldObjectsInRange check(bot, radius);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, radius);
    std::int64_t const nowSec = static_cast<std::int64_t>(GameTime::GetGameTime().count());
    std::vector<WalkingV2Policy::Mob> mobs;
    for (Creature* c : found)
    {
        if (!c || !c->IsInWorld() || c->IsCivilian() || c->HasReactState(REACT_PASSIVE) || !c->IsHostileTo(bot))
            continue;
        bool const alive = c->IsAlive();
        if (!alive && !AutoWowSafeRevive::RespawnSoon(static_cast<std::int64_t>(c->GetRespawnTime()), nowSec))
            continue;
        Position const& at = alive ? static_cast<Position const&>(*c) : c->GetHomePosition();
        WalkingV2Policy::Mob m;
        m.x = static_cast<std::int32_t>(std::floor(at.GetPositionX()));
        m.y = static_cast<std::int32_t>(std::floor(at.GetPositionY()));
        m.level = c->GetLevel();
        m.elite = c->isElite();
        m.aggroYards = static_cast<std::uint32_t>(std::max(0.0f, c->GetAggroRange(bot)));
        mobs.push_back(m);
    }
    return mobs;
}

// AutoWow.Survival.RestSafe: live hostile players (not GMs) within radius of the bot.
std::vector<Player*> HostilePlayers(Player* bot, float radius)
{
    std::list<Player*> found;
    Acore::AnyPlayerInObjectRangeCheck check(bot, radius, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, radius);
    std::vector<Player*> out;
    for (Player* p : found)
        if (p && p != bot && p->IsInWorld() && p->IsHostileTo(bot))
            out.push_back(p);
    return out;
}

// Appends the hostile players as threats; returns how many.
std::size_t AppendPlayerThreats(Player* bot, float radius, std::vector<WalkingV2Policy::Mob>& mobs)
{
    std::vector<Player*> const players = HostilePlayers(bot, radius);
    for (Player* p : players)
        mobs.push_back(AutoWowRestSafe::PlayerThreat(static_cast<std::int32_t>(std::floor(p->GetPositionX())),
                                                     static_cast<std::int32_t>(std::floor(p->GetPositionY())),
                                                     p->GetLevel()));
    return players.size();
}

// Ground-snapped (height search from zHint + 10 down 50 yd) and mmap-reachable from where the bot stands
// on a complete path whose end lies within kAtSpotYards of the point.
bool ReachableSpot(Player* bot, Point const& p, float zHint, G3D::Vector3& out)
{
    float const x = static_cast<float>(p.x);
    float const y = static_cast<float>(p.y);
    float const z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, zHint + 10.0f, true, 50.0f);
    if (!std::isfinite(z) || z <= INVALID_HEIGHT)
        return false;
    PathGenerator path(bot);
    path.SetSlopeCheck(true);
    path.CalculatePath(x, y, z);
    if (path.GetPathType() & ~PATHFIND_NORMAL)
        return false;
    out = path.GetActualEndPosition();
    return std::hypot(out.x - x, out.y - y) <= float(AutoWowSafeRevive::kAtSpotYards);
}

std::optional<Point> GraveAnchor(Player* bot)
{
    GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    if (!grave || grave->Map != bot->GetMapId())
        return std::nullopt;
    return MakePoint(grave->Map, grave->x, grave->y, grave->z);
}

std::string PointText(std::optional<Point> const& p)
{
    return p ? "(" + std::to_string(p->x) + "," + std::to_string(p->y) + ")" : "-";
}
}  // namespace

namespace AutoWowSafeRevive
{
std::optional<Target> PlanFor(Player* bot, Corpse* corpse)
{
    if (!Enabled() || !bot || !corpse || !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) || corpse->GetMapId() != bot->GetMapId() ||
        !AutonomousBotAI(bot) || !Solo(bot) || !OpenWorld(bot))
        return std::nullopt;
    std::uint32_t const guid = GuidOf(bot);
    // An escalated death-loop death takes the spirit healer through its own path.
    if (AutoWowDeathLoop::Enabled() && AutoWowDeathLoop::WantsSpiritHealer(guid))
        return std::nullopt;
    std::uint64_t const nowMs = NowMs();
    std::optional<Point> killer;
    Point deathSpot;
    std::uint32_t recentDeaths = 0;
    bool pvpDeath = false;
    {
        std::lock_guard<std::mutex> guard(gLock);
        ReviveState* s = Find(gRevive, guid);
        if (!s)
            return std::nullopt;  // death not seen with the flag on
        if (s->planned)
        {
            if (s->plan.plan == Plan::ReviveAt && nowMs - s->planMs > kApproachTimeoutMs &&
                bot->GetExactDist2d(s->plan.x, s->plan.y) > float(kAtSpotYards))
            {
                s->plan.plan = Plan::None;
                LOG_INFO("playerbots", "[SafeRevive] bot={} spot not reached in {} ms: legacy corpse run", bot->GetName(),
                         kApproachTimeoutMs);
            }
            return s->plan.plan == Plan::None ? std::nullopt : std::optional<Target>(s->plan);
        }
        killer = s->killer;
        deathSpot = s->deathSpot;
        recentDeaths = AutoWowDeathLoop::RecentDeaths(s->deaths, nowMs, detail::gParams.windowMs);
        pvpDeath = AutoWowRestSafe::Enabled() && s->pvpDeath;
    }
    if (bot->GetExactDist2d(corpse) > float(kPlanYards))
        return std::nullopt;

    Point const corpseAt = MakePoint(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(),
                                     corpse->GetPositionZ());
    bool const v2 = V2Enabled();
    std::vector<Mob> mobs = v2 ? ScanHostilesV2(bot, 150.0f) : ScanIdleHostiles(bot, 150.0f);
    // AutoWow.Survival.RestSafe: hostile players around the corpse are threats too.
    std::size_t const players = AutoWowRestSafe::Enabled() ? AppendPlayerThreats(bot, 150.0f, mobs) : 0;
    std::vector<Spot> spots(kReviveCandidates);
    std::vector<G3D::Vector3> ends(kReviveCandidates);
    std::uint32_t reachable = 0;
    for (std::size_t k = 0; k < kReviveCandidates; ++k)
    {
        spots[k].p = ReviveCandidate(corpseAt, k);
        // Reclaim is a 3D test against the corpse: keep 2 yd of margin.
        spots[k].reachable = ReachableSpot(bot, spots[k].p, corpse->GetPositionZ(), ends[k]) &&
                             corpse->GetExactDist(ends[k].x, ends[k].y, ends[k].z) <= float(CORPSE_RECLAIM_RADIUS - 2);
        reachable += spots[k].reachable ? 1 : 0;
    }
    Anchors const anchors{GraveAnchor(bot), killer, std::nullopt};
    Pick const best = PickSpot(spots, mobs, bot->GetLevel(), anchors, v2 ? kThreatYardsV2 : 0);
    Target t;
    Plan const base = v2 ? DecideV2(best, recentDeaths, detail::gParams) : Decide(best, recentDeaths, detail::gParams);
    // AutoWow.Survival.RestSafe: after a PvP death reclaim only on a zero-threat spot (players counted).
    t.plan = pvpDeath ? AutoWowRestSafe::PvpPlan(base, best) : base;
    if (pvpDeath)
        LOG_INFO("playerbots", "[RestSafe] pvp_plan bot={} base={} plan={} threat={} players={}", bot->GetName(),
                 PlanName(base), PlanName(t.plan), best.threat, players);
    if (t.plan == Plan::ReviveAt)
    {
        t.x = ends[best.index].x;
        t.y = ends[best.index].y;
        t.z = ends[best.index].z;
    }
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (ReviveState* s = Find(gRevive, guid))
        {
            s->planned = true;
            s->plan = t;
            s->planMs = nowMs;
            // A RestSafe PvP spirit healer does not relocate (the danger area keeps the bot off the spot).
            if (v2)
                s->relocateOnRes = base == Plan::SpiritHealer;
        }
    }
    LOG_INFO("playerbots",
             "[SafeRevive] bot={} plan={} deaths={} threat={} spot={} at=({},{}) corpse=({},{}) death=({},{}) killer={} "
             "grave={} mobs={} reachable={}/{}",
             bot->GetName(), PlanName(t.plan), recentDeaths, best.threat,
             best.index == kNoSpot ? -1 : static_cast<int>(best.index), static_cast<std::int32_t>(std::floor(t.x)),
             static_cast<std::int32_t>(std::floor(t.y)), corpseAt.x, corpseAt.y, deathSpot.x, deathSpot.y,
             PointText(killer), PointText(anchors.grave), mobs.size(), reachable, kReviveCandidates);
    return t.plan == Plan::None ? std::nullopt : std::optional<Target>(t);
}

void Abandon(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (ReviveState* s = Find(gRevive, botGuid))
        s->plan.plan = Plan::None;
}

Gate ReclaimGate(Player* bot, Corpse* corpse)
{
    std::optional<Target> const t = PlanFor(bot, corpse);
    if (!t)
        return Gate::Legacy;
    if (t->plan == Plan::SpiritHealer)
        return Gate::Spirit;
    return bot->GetExactDist2d(t->x, t->y) <= float(kAtSpotYards) ? Gate::Reclaim : Gate::Hold;
}

Step RecoveryStep(PlayerbotAI* botAI, float& x, float& y, float& z)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!Enabled() || !bot || !bot->IsAlive() || bot->IsInCombat())
        return Step::None;
    std::uint32_t const guid = GuidOf(bot);
    std::uint64_t const nowMs = NowMs();
    bool planRetreat = false;
    std::optional<Point> killer;
    Point deathSpot;
    {
        std::lock_guard<std::mutex> guard(gLock);
        ReviveState* s = Find(gRevive, guid);
        if (!s)
            return Step::None;
        if (s->retreatPending)
        {
            s->retreatPending = false;
            planRetreat = true;
            killer = s->killer;
            deathSpot = s->deathSpot;
        }
    }

    // First tick after a corpse revive: kRetreatYards off the spot, clear of idle hostiles, toward the
    // graveyard and away from the death spot and the killer. Staying (candidate 0) wins only on less threat.
    if (planRetreat)
    {
        Point const from = Here(bot);
        std::vector<Mob> const mobs = ScanIdleHostiles(bot, 100.0f);
        std::vector<Spot> spots(kRetreatCandidates);
        std::vector<G3D::Vector3> ends(kRetreatCandidates);
        spots[0] = {from, true};
        for (std::size_t k = 1; k < kRetreatCandidates; ++k)
        {
            spots[k].p = RetreatCandidate(from, k);
            spots[k].reachable = ReachableSpot(bot, spots[k].p, bot->GetPositionZ(), ends[k]);
        }
        Anchors const anchors{GraveAnchor(bot), killer, deathSpot};
        Pick const best = PickSpot(spots, mobs, bot->GetLevel(), anchors);
        bool const walk = best.index != kNoSpot && best.index != 0;
        LOG_INFO("playerbots", "[SafeRevive] bot={} retreat={} threat={} here_threat={} to=({},{}) from=({},{}) mobs={}",
                 bot->GetName(), walk ? best.index : 0, best.threat, SpotThreat(from, mobs, bot->GetLevel()),
                 walk ? static_cast<std::int32_t>(std::floor(ends[best.index].x)) : from.x,
                 walk ? static_cast<std::int32_t>(std::floor(ends[best.index].y)) : from.y, from.x, from.y, mobs.size());
        if (walk)
        {
            std::lock_guard<std::mutex> guard(gLock);
            if (ReviveState* s = Find(gRevive, guid))
            {
                s->retreating = true;
                s->rx = ends[best.index].x;
                s->ry = ends[best.index].y;
                s->rz = ends[best.index].z;
                s->retreatMs = nowMs;
            }
        }
    }

    {
        std::lock_guard<std::mutex> guard(gLock);
        ReviveState* s = Find(gRevive, guid);
        if (!s)
            return Step::None;
        if (s->retreating)
        {
            if (nowMs - s->retreatMs > kRetreatTimeoutMs || bot->GetExactDist2d(s->rx, s->ry) <= float(kAtSpotYards))
                s->retreating = false;
            else
            {
                x = s->rx;
                y = s->ry;
                z = s->rz;
                return Step::Walk;
            }
        }
        if (!s->restPending)
            return Step::None;
    }
    // hp / mana below the rest-gate thresholds (sickness excluded: it outlasts any rest; pulls stay held
    // by the forced rest until it is gone).
    return AutoWowRestGate::HoldTravel(botAI) ? Step::Rest : Step::None;
}

void EndRetreat(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (ReviveState* s = Find(gRevive, botGuid))
        s->retreating = false;
}

Diagnostic ReadDiagnostic(std::uint32_t botGuid)
{
    Diagnostic diagnostic;
    diagnostic.enabled = Enabled();
    diagnostic.v2Enabled = V2Enabled();

    std::lock_guard<std::mutex> guard(gLock);
    ReviveState const* state = Find(gRevive, botGuid);
    if (!state)
        return diagnostic;

    diagnostic.tracked = true;
    diagnostic.planned = state->planned;
    diagnostic.retreatPending = state->retreatPending;
    diagnostic.retreating = state->retreating;
    diagnostic.restPending = state->restPending;
    diagnostic.relocateOnRes = state->relocateOnRes;
    diagnostic.relocate = state->relocate;
    diagnostic.planMs = state->planMs;
    diagnostic.retreatMs = state->retreatMs;
    return diagnostic;
}

bool RestPending(std::uint32_t botGuid)
{
    if (!Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    ReviveState const* s = Find(gRevive, botGuid);
    return s && s->restPending;
}

void ClearRestPending(std::uint32_t botGuid)
{
    if (!Enabled())
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (ReviveState* s = Find(gRevive, botGuid))
        s->restPending = false;
}

bool RelocationPending(std::uint32_t botGuid)
{
    if (!V2Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    ReviveState const* s = Find(gRevive, botGuid);
    return s && s->relocate;
}

bool TakeRelocation(std::uint32_t botGuid)
{
    if (!V2Enabled())
        return false;
    std::lock_guard<std::mutex> guard(gLock);
    ReviveState* s = Find(gRevive, botGuid);
    if (!s || !s->relocate)
        return false;
    s->relocate = false;
    return true;
}
}  // namespace AutoWowSafeRevive

namespace AutoWowRestSafe
{
namespace
{
constexpr float kScanYards = 110.0f;  // kOuterYards + the widest threat radius

std::uint32_t PowerPct(std::uint64_t cur, std::uint64_t max) { return max ? static_cast<std::uint32_t>(cur * 100 / max) : 100; }

// Once per rest episode: a hostile covers the bot -> the best reachable candidate with less threat.
void PlanRest(Player* bot, std::uint32_t guid)
{
    Point const here = Here(bot);
    std::vector<Mob> mobs = ScanHostilesV2(bot, kScanYards);
    std::size_t const creatures = mobs.size();
    std::size_t const players = AppendPlayerThreats(bot, kScanYards, mobs);
    std::uint32_t const level = bot->GetLevel();
    std::uint32_t const hereThreat = AutoWowSafeRevive::SpotThreat(here, mobs, level);
    if (!hereThreat)
        return;
    std::vector<std::size_t> const order = MoveOrder(here, mobs, level);
    std::size_t pick = AutoWowSafeRevive::kNoSpot;
    std::size_t tried = 0;
    G3D::Vector3 end;
    for (std::size_t k : order)
    {
        ++tried;
        if (ReachableSpot(bot, Candidate(here, k), bot->GetPositionZ(), end))
        {
            pick = k;
            break;
        }
    }
    bool const found = pick != AutoWowSafeRevive::kNoSpot;
    LOG_INFO("playerbots",
             "[RestSafe] move bot={} {} hp={} mana={} here_threat={} threat={} spot={} to=({},{}) from=({},{},{}) "
             "mobs={} players={} tried={}/{}",
             bot->GetName(), found ? "go" : "stay", PowerPct(bot->GetHealth(), bot->GetMaxHealth()),
             bot->getPowerType() == POWER_MANA ? PowerPct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)) : 100,
             hereThreat, found ? AutoWowSafeRevive::SpotThreat(Candidate(here, pick), mobs, level) : hereThreat,
             found ? static_cast<int>(pick) : -1, found ? static_cast<std::int32_t>(std::floor(end.x)) : here.x,
             found ? static_cast<std::int32_t>(std::floor(end.y)) : here.y, here.mapId, here.x, here.y, creatures,
             players, tried, order.size());
    if (!found)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (RestState* s = Find(gRest, guid))
    {
        s->moving = true;
        s->x = end.x;
        s->y = end.y;
        s->z = end.z;
        s->moveMs = NowMs();
    }
}
}  // namespace

Step RestStep(PlayerbotAI* botAI, float& x, float& y, float& z)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!Enabled() || !bot || bot->IsInFlight() || !AutonomousBotAI(bot))
        return Step::None;
    std::uint32_t const guid = GuidOf(bot);
    // Eligible (rest gate), out of combat, below the hp / mana thresholds.
    bool const need = AutoWowRestGate::HoldTravel(botAI);
    bool plan = false;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!need)
        {
            gRest.erase(guid);
            return Step::None;
        }
        // SafeRevive walks a corpse revive off the kill spot first.
        if (ReviveState const* r = Find(gRevive, guid); r && (r->retreatPending || r->retreating))
            return Step::None;
        RestState* s = FindOrCreate(gRest, guid);
        if (!s)
            return Step::None;
        plan = !s->planned;
        s->planned = true;
    }
    if (plan)
        PlanRest(bot, guid);

    std::uint64_t const nowMs = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    RestState* s = Find(gRest, guid);
    if (s && s->moving)
    {
        if (nowMs - s->moveMs > kMoveTimeoutMs || bot->GetExactDist2d(s->x, s->y) <= float(AutoWowSafeRevive::kAtSpotYards))
            s->moving = false;
        else
        {
            x = s->x;
            y = s->y;
            z = s->z;
            return Step::Walk;
        }
    }
    return Step::Rest;
}

void EndMove(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (RestState* s = Find(gRest, botGuid))
        s->moving = false;
}
}  // namespace AutoWowRestSafe

namespace AutoWowUnstick
{
void NoteReplanExhausted(std::uint32_t botGuid)
{
    if (!Enabled())
        return;
    std::uint64_t const nowMs = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    if (BotState* s = FindOrCreate(gUnstick, botGuid))
        NoteReplanExhausted(detail::gParams, *s, nowMs);
}

bool Step(PlayerbotAI* botAI, std::uint32_t destMap, float destX, float destY, float destZ)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!Enabled() || !bot || !bot->IsAlive() || bot->IsInCombat() || !AutonomousBotAI(bot) || !OpenWorld(bot))
        return false;
    Params const& p = detail::gParams;
    std::uint32_t const guid = GuidOf(bot);
    std::uint64_t const nowMs = NowMs();
    Point const here = Here(bot);
    Trigger trigger = Trigger::None;
    std::uint64_t stillMs = 0;
    std::uint32_t exhausted = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        BotState* s = FindOrCreate(gUnstick, guid);
        if (!s)
            return false;
        if (s->hearthMs)
        {
            if (nowMs >= s->hearthMs && nowMs - s->hearthMs < kHearthCastMs &&
                (bot->IsNonMeleeSpellCast(false) || bot->IsBeingTeleported()))
                return true;  // our hearthstone is casting / relocating
            s->hearthMs = 0;
        }
        trigger = Observe(p, *s, here, nowMs);
        stillMs = nowMs - s->anchorMs;
        exhausted = s->exhausted;
    }
    if (trigger == Trigger::None)
        return false;

    bool const hearthReady =
        bot->HasItemCount(kHearthstoneItem, 1, false) && !bot->HasSpellCooldown(kHearthstoneSpell);
    bool const hearthFar = bot->m_homebindMapId != bot->GetMapId() ||
                           bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY) > float(kHearthMinYards);
    // Navmesh probe: 8 bearings x kProbeYards, complete or partial mmap paths only (no far-from-poly ends).
    std::array<std::uint32_t, kProbes> escape{};
    std::uint32_t escapeMax = 0;
    bool hole = false;
    if (!hearthReady || !hearthFar)
    {
        for (std::size_t k = 0; k < kProbes; ++k)
        {
            Point const probe = AutoWowSafeRevive::Offset(here, k, kProbeYards);
            float const px = static_cast<float>(probe.x);
            float const py = static_cast<float>(probe.y);
            float const pz = bot->GetMap()->GetHeight(bot->GetPhaseMask(), px, py, bot->GetPositionZ() + 10.0f, true, 50.0f);
            if (!std::isfinite(pz) || pz <= INVALID_HEIGHT)
                continue;
            PathGenerator path(bot);
            path.SetSlopeCheck(true);
            path.CalculatePath(px, py, pz);
            if (path.GetPathType() & ~(PATHFIND_NORMAL | PATHFIND_INCOMPLETE))
                continue;
            G3D::Vector3 const end = path.GetActualEndPosition();
            escape[k] = static_cast<std::uint32_t>(bot->GetExactDist2d(end.x, end.y));
            escapeMax = std::max(escapeMax, escape[k]);
        }
        hole = NavmeshHole(escape, p.minEscapeYards);
    }

    Action action = Decide(hearthReady, hearthFar, hole);
    bool done = false;
    if (action == Action::Hearth)
    {
        done = botAI->DoSpecificAction("hearthstone", Event("autowow unstick"), true);
        if (!done)
            action = Action::None;
    }
    else if (action == Action::Portal)
    {
        GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
        if (grave)
        {
            bot->GetMotionMaster()->Clear();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "unstick_portal");
            done = bot->TeleportTo(grave->Map, grave->x, grave->y, grave->z, bot->GetOrientation());
        }
    }
    LOG_INFO("playerbots",
             "[Unstick] bot={} trigger={} action={} ok={} hole={} escape_max={} hearth_ready={} hearth_far={} map={} "
             "x={} y={} z={} zone={} area={} still_ms={} exhausted={} dest=({},{},{},{})",
             bot->GetName(), TriggerName(trigger), ActionName(action), done, hole, escapeMax, hearthReady, hearthFar,
             bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetZoneId(),
             bot->GetAreaId(), stillMs, exhausted, destMap, destX, destY, destZ);
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (BotState* s = Find(gUnstick, guid))
        {
            Acted(p, *s, nowMs);
            if (action == Action::Hearth && done)
                s->hearthMs = nowMs;
        }
    }
    if (done)
    {
        // A fresh start from the new spot: the old commitment and stuck counters were earned elsewhere.
        botAI->rpgInfo.travelIntent = {};
        botAI->rpgInfo.SetMoveFarTo(WorldPosition());
    }
    return done;
}
}  // namespace AutoWowUnstick

namespace AutoWowSurvivalRecovery
{
void LoadConfig()
{
    AutoWowSafeRevive::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.SafeRevive", false);
    AutoWowSafeRevive::Params& r = AutoWowSafeRevive::detail::gParams;
    r.spiritDeaths = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.SafeRevive.SpiritDeaths", 2);
    r.windowMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.SafeRevive.WindowMs", 600000);
    AutoWowSafeRevive::detail::gV2 = sConfigMgr->GetOption<bool>("AutoWow.Survival.SafeRevive.V2", false);

    AutoWowRestSafe::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.RestSafe", false);
    AutoWowRestSafe::Params& rs = AutoWowRestSafe::detail::gParams;
    rs.pvpDangerYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.RestSafe.PvpDangerYards", 80);
    rs.pvpDangerMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.RestSafe.PvpDangerMs", 1800000);

    AutoWowUnstick::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.Unstick", false);
    AutoWowUnstick::Params& u = AutoWowUnstick::detail::gParams;
    u.radiusYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.RadiusYards", 10);
    u.frozenMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.FrozenMs", 300000);
    u.replanExhausted = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.ReplanExhausted", 5);
    u.windowMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.WindowMs", 900000);
    u.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.CooldownMs", 600000);
    u.minEscapeYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.Unstick.MinEscapeYards", 8);
}

namespace
{
// Registered unconditionally (the flags are read after script registration); every hook early-returns
// on the cached flag.
class SurvivalRecoveryPlayerScript : public PlayerScript
{
public:
    SurvivalRecoveryPlayerScript()
        : PlayerScript("AutoWowSurvivalRecoveryPlayer", {
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
        if (AutoWowSafeRevive::Enabled() && killer && killed && killer != killed)
            NoteKiller(killed, Here(killer));
        if (AutoWowRestSafe::Enabled() && AutoWowSafeRevive::Enabled() && killer && killed && killer != killed)
            NotePvpKill(killed);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (AutoWowSafeRevive::Enabled() && killer && killed)
            NoteKiller(killed, MakePoint(killer->GetMapId(), killer->GetPositionX(), killer->GetPositionY(),
                                         killer->GetPositionZ()));
        // AutoWow.Survival.RestSafe: a player's pet (or charmed creature) kill is a PvP death.
        if (AutoWowRestSafe::Enabled() && AutoWowSafeRevive::Enabled() && killer && killed &&
            killer->GetCharmerOrOwnerPlayerOrPlayerItself())
            NotePvpKill(killed);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!AutoWowSafeRevive::Enabled() || !player || !AutonomousBotAI(player) || !OpenWorld(player))
            return;
        Point const at = Here(player);
        AutoWowDeathLoop::DeathSample const death{NowMs(), at.mapId, at.x, at.y};
        // AutoWow.Survival.RestSafe: a hostile player close by at death makes it a PvP death too.
        bool const restSafe = AutoWowRestSafe::Enabled();
        bool const playerNear =
            restSafe && !HostilePlayers(player, float(AutoWowRestSafe::kPvpNearYards)).empty();
        bool killedByPlayer = false;
        {
            std::lock_guard<std::mutex> guard(gLock);
            ReviveState* s = FindOrCreate(gRevive, GuidOf(player));
            if (!s)
                return;
            AutoWowDeathLoop::DeathSample center;
            AutoWowDeathLoop::RecordDeath(s->deaths, death, AutoWowSafeRevive::detail::gParams.windowMs, 0, center);
            s->killer = s->pendingKiller;
            s->pendingKiller.reset();
            s->deathSpot = at;
            s->planned = false;
            s->plan = {};
            s->retreatPending = false;
            s->retreating = false;
            s->restPending = false;
            s->relocateOnRes = false;
            s->relocate = false;
            if (restSafe)
            {
                killedByPlayer = s->pendingPvp;
                s->pvpDeath = killedByPlayer || playerNear;
                s->pendingPvp = false;
            }
        }
        if (!killedByPlayer && !playerNear)
            return;
        // The camped spot becomes a death-loop danger area: RPG grind / quest / travel picks skip it.
        AutoWowRestSafe::Params const& p = AutoWowRestSafe::detail::gParams;
        bool const marked = p.pvpDangerYards &&
                            AutoWowDeathLoop::MarkDangerArea(GuidOf(player), at.mapId, at.x, at.y, p.pvpDangerYards,
                                                             p.pvpDangerMs);
        LOG_INFO("playerbots", "[RestSafe] pvp_death bot={} killer_player={} player_near={} at=({},{},{}) danger={} r={} "
                 "cool_ms={}",
                 player->GetName(), killedByPlayer, playerNear, at.mapId, at.x, at.y, marked, p.pvpDangerYards,
                 p.pvpDangerMs);
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        if (!AutoWowSafeRevive::Enabled() || !player)
            return;
        std::lock_guard<std::mutex> guard(gLock);
        ReviveState* s = Find(gRevive, GuidOf(player));
        if (!s || !s->deaths.deathCount)
            return;
        // A reclaim on the chosen spot walks off it first; any revive rests before the next pull.
        s->retreatPending = s->planned && s->plan.plan == AutoWowSafeRevive::Plan::ReviveAt &&
                            player->GetExactDist2d(s->plan.x, s->plan.y) <= float(AutoWowSafeRevive::kAtSpotYards + 2);
        s->restPending = true;
        s->planned = false;
        s->plan = {};
        // V2: a forced spirit-healer res moves the bot away (NewRpgAction takes the relocation).
        if (AutoWowSafeRevive::V2Enabled())
        {
            s->relocate = s->relocateOnRes;
            s->relocateOnRes = false;
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if ((!AutoWowSafeRevive::Enabled() && !AutoWowUnstick::Enabled() && !AutoWowRestSafe::Enabled()) || !player)
            return;
        std::lock_guard<std::mutex> guard(gLock);
        gRevive.erase(GuidOf(player));
        gUnstick.erase(GuidOf(player));
        gRest.erase(GuidOf(player));
    }

private:
    static void NotePvpKill(Player* killed)
    {
        if (!AutonomousBotAI(killed) || !OpenWorld(killed))
            return;
        std::lock_guard<std::mutex> guard(gLock);
        if (ReviveState* s = FindOrCreate(gRevive, GuidOf(killed)))
            s->pendingPvp = true;
    }

    static void NoteKiller(Player* killed, Point const& at)
    {
        if (!AutonomousBotAI(killed) || !OpenWorld(killed))
            return;
        std::lock_guard<std::mutex> guard(gLock);
        if (ReviveState* s = FindOrCreate(gRevive, GuidOf(killed)))
            s->pendingKiller = at;
    }
};
}  // namespace

void AddScripts() { new SurvivalRecoveryPlayerScript(); }
}  // namespace AutoWowSurvivalRecovery
