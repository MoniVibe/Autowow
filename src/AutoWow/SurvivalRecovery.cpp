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
};

// Deaths, revives and bot AI updates run on map threads. Touched only with a flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, ReviveState> gRevive;
std::unordered_map<std::uint32_t, AutoWowUnstick::BotState> gUnstick;

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
    }
    if (bot->GetExactDist2d(corpse) > float(kPlanYards))
        return std::nullopt;

    Point const corpseAt = MakePoint(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(),
                                     corpse->GetPositionZ());
    std::vector<Mob> const mobs = ScanIdleHostiles(bot, 150.0f);
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
    Pick const best = PickSpot(spots, mobs, bot->GetLevel(), anchors);
    Target t;
    t.plan = Decide(best, recentDeaths, detail::gParams);
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
}  // namespace AutoWowSafeRevive

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
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (AutoWowSafeRevive::Enabled() && killer && killed)
            NoteKiller(killed, MakePoint(killer->GetMapId(), killer->GetPositionX(), killer->GetPositionY(),
                                         killer->GetPositionZ()));
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!AutoWowSafeRevive::Enabled() || !player || !AutonomousBotAI(player) || !OpenWorld(player))
            return;
        Point const at = Here(player);
        AutoWowDeathLoop::DeathSample const death{NowMs(), at.mapId, at.x, at.y};
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
    }

    void OnPlayerLogout(Player* player) override
    {
        if ((!AutoWowSafeRevive::Enabled() && !AutoWowUnstick::Enabled()) || !player)
            return;
        std::lock_guard<std::mutex> guard(gLock);
        gRevive.erase(GuidOf(player));
        gUnstick.erase(GuidOf(player));
    }

private:
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
