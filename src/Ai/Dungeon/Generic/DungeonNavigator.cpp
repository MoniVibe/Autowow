/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonNavigator.h"

#include "AutoWow/DungeonPathSafety.h"
#include "AutoWow/DungeonPathWalkAction.h"
#include "AutoWowQuestLedger.h"
#include "CombatManager.h"
#include "Config.h"
#include "CreatureData.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DungeonEncounterActivationPolicy.h"
#include "DungeonEncounterCompletionPolicy.h"
#include "DungeonEncounterSelectionPolicy.h"
#include "DungeonNavigatorCombatPolicy.h"
#include "DungeonNavigatorConvoyPolicy.h"
#include "DungeonNavigatorPacingPolicy.h"
#include "DungeonProgressionInteractionPolicy.h"
#include "DungeonPullReadinessPolicy.h"
#include "DungeonRouteReconnectPolicy.h"
#include "DungeonSpellCreditBossPolicy.h"
#include "GameObjectLockPolicy.h"
#include "Group.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "TravelNode.h"
#include "WorldPacket.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr uint32 BlockedScanBackoffMs = 15 * IN_MILLISECONDS;
constexpr float TravelArrivalRadius = 3.0f;
// Keep each autonomous leader advance short enough for the ordinary combat engine to observe
// aggro and for nearby followers to enter assist combat before the navigator can spline through
// another trash pack. Route discovery still scans ahead, but movement itself is deliberately
// paced like a cautious player pull rather than one long body-pull through the corridor.
constexpr float TravelLookaheadDistance = 12.0f;
// Recast path lengths at a navmesh boundary can exceed the requested twelve-yard stride by a
// tiny floating-point amount. This tolerance applies only after the incomplete path and its
// actual endpoint have passed the ordinary safety probe; it does not enlarge full-point route
// selection or count the requested stored point as reached.
constexpr float TravelPartialPathTolerance = 0.5f;
constexpr uint32 TravelLookaheadPointLimit = 32;
// Stored routes can begin at an entrance or other stable graph anchor well behind the bot after
// a local-corridor replan. Attachment remains independently probed point-by-point, but needs a
// larger bounded prefix than ordinary movement lookahead to find the bot's physical route region.
constexpr uint32 TravelStoredWalkAttachmentPointLimit = 128;
constexpr float TravelNoProgressDistance = 1.5f;
constexpr uint8 TravelNoProgressRetryLimit = 3;
constexpr uint8 TravelNoProgressReplanLimit = 1;
constexpr float TravelNodeAttachmentRadius = 80.0f;
constexpr std::size_t TravelNodeAttachmentLimit = 8;
constexpr float TravelNodeReplanAttachmentRadius = 128.0f;
constexpr std::size_t TravelNodeReplanCandidateLimit = 8;
constexpr float PartyCohesionRadius = 45.0f;
constexpr uint32 PartyCohesionBackoffMs = 5 * IN_MILLISECONDS;
constexpr float ConvoyAdvanceDistance = 15.0f;
constexpr float ConvoyMinimumProgress = 1.5f;
constexpr float ConvoyMaximumPathLength = 45.0f;
constexpr float ConvoyPostCombatRejoinMaximumPathLength = 192.0f;
constexpr std::size_t ConvoyRoutePointLimit = 32;
constexpr std::size_t ConvoyBackwardReanchorPointLimit = 128;
constexpr std::size_t ConvoySharedRegroupSmallPrefix = 5;
constexpr std::size_t ConvoySharedRegroupMediumPrefix = 16;
constexpr uint32 ConvoyBackwardReanchorBackoffMs = 15 * IN_MILLISECONDS;
constexpr float ConvoySharedRegroupMinimumLeaderRetreat = 3.0f;
constexpr float ConvoySharedRegroupMaximumPathLength = 128.0f;
constexpr uint8 ConvoySharedRegroupAttemptLimit = 3;
constexpr float ConvoyGroundReattachMinimum = 8.0f;
// Collision fall-through can place a player far below the established WMO floor while retaining
// the correct XY. The correction remains same-XY and still requires a valid floor derived from the
// follower's assigned route slot plus a complete ordinary continuation path from that floor.
constexpr float ConvoyGroundReattachMaximum = 192.0f;
constexpr float ConvoyGroundProbeAboveRoute = 2.0f;
constexpr float ConvoyGroundProbeDepth = 12.0f;
constexpr float ConvoyRouteReattachMaximumHorizontal = 20.0f;
constexpr float ConvoyRouteGroundTolerance = 2.0f;
constexpr uint32 AmbiguousCombatStateRetentionMs = 5 * MINUTE * IN_MILLISECONDS;
constexpr float GroundReattachMinimum = 0.1f;
constexpr float GroundReattachMaximum = 2.0f;
constexpr float GroundReattachOffset = 0.05f;
constexpr float DirectHopMaximumHorizontal = 15.0f;  // ConvoyV2 leader navmesh-gap hop bounds
constexpr float DirectHopMaximumVertical = 5.5f;
constexpr uint8 DirectHopAttemptLimit = 2;
constexpr float PartyGroundAnchorRadius = 12.0f;
constexpr float PartyGroundAnchorVerticalTolerance = 1.5f;
constexpr float PartyRouteReanchorMinimumDrop = 3.0f;
constexpr float PartyRouteReanchorMaximumDrop = 24.0f;
constexpr std::size_t PartyGroundMinimumSupport = 2;

uint32 SuccessfulMoveRescanDelayMs()
{
    return DungeonNavigatorPacing::GetSuccessfulMoveBackoffMs(
        sPlayerbotAIConfig.maxWaitForMove);
}

// Leader guid -> {map, instance, encounter index} of its last selected encounter; read by the probe.
std::map<uint32, std::array<uint32, 3>> navigatorTargetEncounters;
std::mutex navigatorTargetEncountersMutex;

// AutoWow.DungeonNav.ConvoyV2 (default 0): follower settles on its best reachable point at or
// below an unreachable slot, and the shared-regroup terminal spends the real attempt budget and
// is released when the leader frontier moves or the member rejoins cohesion. Read once.
bool ConvoyV2Enabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AutoWow.DungeonNav.ConvoyV2", false);
    return enabled;
}

// AutoWow.DungeonNav.Gates (default 0): the DungeonGatePolicy.h step table drives scripted progression
// (gossip, escorts, gongs, fires, altars, kill sets, proxy kills). Read once.
bool GatesEnabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AutoWow.DungeonNav.Gates", false);
    return enabled;
}

// AutoWow.DungeonNav.Gates.BypassKeys (default 0): also run rows that name a key item. Read once.
bool GateBypassKeys()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AutoWow.DungeonNav.Gates.BypassKeys", false);
    return enabled;
}

constexpr uint32 GateHoldMs = 5 * IN_MILLISECONDS;
constexpr float GateCreatureSearchLimit = 150.0f;
constexpr float GateEscortDistance = 8.0f;
constexpr float GateEscortFollowDistance = 4.0f;
constexpr float GateAttackDistance = 30.0f;
constexpr float GateAttackApproachDistance = 20.0f;
constexpr float GateItemUseDistance = 4.0f;  // key item spells (Defias Gunpowder 6250) reach 5 yd
constexpr float GateItemUseApproachDistance = 3.0f;

GameObject* GateObject(Map* map, uint32 spawnGuid)
{
    auto const bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnGuid);
    return bounds.first != bounds.second ? bounds.first->second : nullptr;
}

Creature* GateCreature(Map* map, uint32 spawnGuid)
{
    auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnGuid);
    return bounds.first != bounds.second ? bounds.first->second : nullptr;
}

// Nearest live creature of the row entry to the row position: its exact spawn, or any spawn/summon
// within the row radius. searched is false when that cannot be known from here (grid not loaded or
// the bot too far to search).
Creature* NearestLiveGateCreature(Player* bot, Map* map, DungeonGate::Step const& step, bool& searched)
{
    searched = false;
    if (step.spawnGuid)
    {
        Creature* creature = GateCreature(map, step.spawnGuid);
        searched = creature || map->IsGridLoaded(step.x, step.y);
        return creature && creature->IsAlive() && creature->IsInWorld() ? creature : nullptr;
    }

    float const distance = bot->GetExactDist(step.x, step.y, step.z);
    if (!map->IsGridLoaded(step.x, step.y) || distance - step.radius > GateCreatureSearchLimit)
        return nullptr;

    searched = true;
    std::list<Creature*> found;
    bot->GetCreatureListWithEntryInGrid(found, step.entry, distance + step.radius);
    Creature* best = nullptr;
    float bestDistance = step.radius;
    for (Creature* creature : found)
    {
        if (!creature || !creature->IsAlive() || !creature->IsInWorld())
            continue;
        float const candidateDistance = creature->GetExactDist(step.x, step.y, step.z);
        if (candidateDistance > step.radius)
            continue;
        if (!best || candidateDistance < bestDistance ||
            (candidateDistance == bestDistance &&
                creature->GetGUID().GetCounter() < best->GetGUID().GetCounter()))
        {
            best = creature;
            bestDistance = candidateDistance;
        }
    }
    return best;
}

struct EncounterGoal
{
    uint32 spawnId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pathLength = 0.0f;
    bool travelNodes = false;
    std::size_t waypointIndex = 0;
    float finalX = 0.0f;
    float finalY = 0.0f;
    float finalZ = 0.0f;
};

struct SpellCreditBossFallbackGoal
{
    DungeonSpellCreditBoss::Binding binding;
    EncounterGoal goal;
};

struct SpellCreditBossDiagnosticKey
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 encounterId = 0;
    uint32 spawnId = 0;

    bool operator<(SpellCreditBossDiagnosticKey const& other) const
    {
        if (mapId != other.mapId)
            return mapId < other.mapId;
        if (instanceId != other.instanceId)
            return instanceId < other.instanceId;
        if (encounterId != other.encounterId)
            return encounterId < other.encounterId;
        return spawnId < other.spawnId;
    }
};

struct AmbiguousCombatKey
{
    uint32 memberGuid = 0;
    uint32 mapId = 0;
    uint32 instanceId = 0;

    bool operator<(AmbiguousCombatKey const& other) const
    {
        if (memberGuid != other.memberGuid)
            return memberGuid < other.memberGuid;
        if (mapId != other.mapId)
            return mapId < other.mapId;
        return instanceId < other.instanceId;
    }
};

struct AmbiguousCombatState
{
    uint32 firstSeen = 0;
    uint32 lastSeen = 0;
    bool recoveryIssued = false;
};

struct ConvoyCombatGate
{
    DungeonNavigatorCombat::Decision decision = DungeonNavigatorCombat::Decision::Allow;
    DungeonNavigatorCombat::Signals signals;
    uint32 ambiguousDurationMs = 0;
};

std::map<AmbiguousCombatKey, AmbiguousCombatState> ambiguousCombatStates;
std::mutex ambiguousCombatStatesMutex;

struct ConvoySharedRegroupState
{
    uint8 attempts = 0;
    uint8 waitScans = 0;
    bool active = false;
    std::size_t anchorRouteIndex = 0;
    DungeonRouteReconnect::SharedRegroupTerminalState terminalState;
    std::size_t terminalLeaderFrontier = 0;  // ConvoyV2 release key
};

std::map<DungeonNavigateNextEncounterAction const*,
    std::map<uint32, ConvoySharedRegroupState>> convoySharedRegroupAttemptStates;

ConvoySharedRegroupState& GetConvoySharedRegroupState(
    DungeonNavigateNextEncounterAction const* action, uint32 memberGuid)
{
    return convoySharedRegroupAttemptStates[action][memberGuid];
}

void EnsureConvoySharedRegroupContext(
    DungeonNavigateNextEncounterAction const* action,
    uint32 memberGuid,
    DungeonRouteReconnect::SharedRegroupContext const& context)
{
    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(action, memberGuid);
    if (state.terminalState.hasContext &&
        !DungeonRouteReconnect::SameSharedRegroupContext(state.terminalState.context, context))
    {
        state = {};
    }

    if (!state.terminalState.hasContext)
    {
        state.terminalState.hasContext = true;
        state.terminalState.context = context;
    }
}

uint8 GetConvoySharedRegroupAttempts(DungeonNavigateNextEncounterAction const* action,
                                     uint32 memberGuid)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return 0;
    auto const memberState = actionState->second.find(memberGuid);
    return memberState == actionState->second.end() ? 0 : memberState->second.attempts;
}

void SetConvoySharedRegroupAttempts(DungeonNavigateNextEncounterAction const* action,
                                    uint32 memberGuid, uint8 attempts)
{
    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(action, memberGuid);
    state.attempts = attempts;
    state.waitScans = 0;
    state.active = false;
}

void ActivateConvoySharedRegroup(DungeonNavigateNextEncounterAction const* action,
                                 uint32 memberGuid, std::size_t anchorRouteIndex)
{
    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(action, memberGuid);
    state.waitScans = 0;
    state.active = true;
    state.anchorRouteIndex = anchorRouteIndex;
}

uint8 ConsumeConvoySharedRegroupWait(DungeonNavigateNextEncounterAction const* action,
                                     uint32 memberGuid, uint8 waitLimit)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return 0;
    auto memberState = actionState->second.find(memberGuid);
    if (memberState == actionState->second.end() || !memberState->second.active ||
        memberState->second.waitScans >= waitLimit)
    {
        return 0;
    }
    return ++memberState->second.waitScans;
}

bool IsConvoySharedRegroupActive(DungeonNavigateNextEncounterAction const* action,
                                 uint32 memberGuid)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return false;
    auto const memberState = actionState->second.find(memberGuid);
    return memberState != actionState->second.end() && memberState->second.active;
}

bool IsConvoySharedRegroupTerminal(
    DungeonNavigateNextEncounterAction const* action,
    uint32 memberGuid,
    DungeonRouteReconnect::SharedRegroupContext const& context)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return false;
    auto const memberState = actionState->second.find(memberGuid);
    return memberState != actionState->second.end() &&
        DungeonRouteReconnect::IsSharedRegroupTerminal(
            memberState->second.terminalState, context);
}

std::size_t GetConvoySharedRegroupAnchor(DungeonNavigateNextEncounterAction const* action,
                                         uint32 memberGuid)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return 0;
    auto const memberState = actionState->second.find(memberGuid);
    return memberState == actionState->second.end() ? 0 : memberState->second.anchorRouteIndex;
}

void EraseConvoySharedRegroupAttempts(DungeonNavigateNextEncounterAction const* action,
                                      uint32 memberGuid)
{
    auto const actionState = convoySharedRegroupAttemptStates.find(action);
    if (actionState == convoySharedRegroupAttemptStates.end())
        return;
    actionState->second.erase(memberGuid);
    if (actionState->second.empty())
        convoySharedRegroupAttemptStates.erase(actionState);
}

void ClearConvoySharedRegroupAttempts(DungeonNavigateNextEncounterAction const* action)
{
    convoySharedRegroupAttemptStates.erase(action);
}

bool IsLiveCombatUnit(Unit const* subject, Unit const* other)
{
    return subject && other && other->IsAlive() && other->IsInWorld() &&
        other->GetMapId() == subject->GetMapId() &&
        other->GetInstanceId() == subject->GetInstanceId();
}

void AddCombatSignals(Unit* unit, bool pet, DungeonNavigatorCombat::Signals& signals)
{
    if (!unit)
        return;

    bool const combatFlag = unit->IsInCombat();
    if (pet)
        signals.petCombatFlag = signals.petCombatFlag || combatFlag;
    else
        signals.combatFlag = signals.combatFlag || combatFlag;

    signals.liveVictim = signals.liveVictim || IsLiveCombatUnit(unit, unit->GetVictim());
    for (Unit* attacker : unit->getAttackers())
    {
        if (IsLiveCombatUnit(unit, attacker))
        {
            signals.liveAttackers = true;
            break;
        }
    }
    signals.combatRelationships = signals.combatRelationships ||
        unit->GetCombatManager().HasCombat();
    for (auto const& threat : unit->GetThreatMgr().GetThreatenedByMeList())
    {
        ThreatReference* reference = threat.second;
        if (reference && reference->IsAvailable() &&
            IsLiveCombatUnit(unit, reference->GetOwner()))
        {
            signals.liveThreat = true;
            break;
        }
    }
    signals.combatCast = signals.combatCast ||
        (combatFlag && unit->IsNonMeleeSpellCast(false));
}

ConvoyCombatGate EvaluateConvoyCombat(Player* member, uint32 now)
{
    ConvoyCombatGate result;
    AddCombatSignals(member, false, result.signals);
    AddCombatSignals(member ? member->GetPet() : nullptr, true, result.signals);

    AmbiguousCombatKey const key{
        member ? member->GetGUID().GetCounter() : 0,
        member ? member->GetMapId() : 0,
        member ? member->GetInstanceId() : 0,
    };
    std::lock_guard<std::mutex> lock(ambiguousCombatStatesMutex);
    for (auto state = ambiguousCombatStates.begin(); state != ambiguousCombatStates.end();)
    {
        if (getMSTimeDiff(state->second.lastSeen, now) > AmbiguousCombatStateRetentionMs)
            state = ambiguousCombatStates.erase(state);
        else
            ++state;
    }

    DungeonNavigatorCombat::Decision const immediate =
        DungeonNavigatorCombat::Evaluate(result.signals, 0, false);
    if (immediate == DungeonNavigatorCombat::Decision::Allow ||
        immediate == DungeonNavigatorCombat::Decision::BlockActive)
    {
        ambiguousCombatStates.erase(key);
        result.decision = immediate;
        return result;
    }

    auto const state = ambiguousCombatStates.try_emplace(
        key, AmbiguousCombatState{now, now, false}).first;
    state->second.lastSeen = now;
    result.ambiguousDurationMs = getMSTimeDiff(state->second.firstSeen, now);
    result.decision = DungeonNavigatorCombat::Evaluate(
        result.signals, result.ambiguousDurationMs, state->second.recoveryIssued);
    if (result.decision == DungeonNavigatorCombat::Decision::RecoverAndAllow)
        state->second.recoveryIssued = true;
    return result;
}

void RecoverFlagOnlyCombat(Player* member)
{
    if (!member)
        return;
    member->AttackStop();
    if (PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member))
        memberAI->PetFollow();
}

bool IsWalkPoint(PathNodeType type)
{
    return type == NODE_PREPATH || type == NODE_PATH || type == NODE_NODE;
}

bool ProbeReachedStoredDestination(AutoWowDungeonPath::ProbeResult const& probe);

// Stored-walk cliff step (DungeonNavigatorConvoy::IsStoredWalkCliff) = DungeonPathSafety MaxSegmentVerticalDelta.
constexpr float StoredWalkCliffStep = 5.5f;

// Route-leg probe: the slope-checked probe, or with ConvoyV2 the slope-free one when
// DungeonNavigatorConvoy::UseSlopeFreeLeg picks it. ConvoyV2 off: exactly AutoWowDungeonPath::Probe.
AutoWowDungeonPath::ProbeResult ProbeLeg(Player const* player, float x, float y, float z)
{
    AutoWowDungeonPath::ProbeResult checked = AutoWowDungeonPath::Probe(player, x, y, z);
    if (!ConvoyV2Enabled())
        return checked;
    bool const checkedReached = ProbeReachedStoredDestination(checked);
    if (checked.safe && checkedReached)
        return checked;
    AutoWowDungeonPath::ProbeResult slopeFree = AutoWowDungeonPath::Probe(player, x, y, z, false);
    return DungeonNavigatorConvoy::UseSlopeFreeLeg(true, checked.safe, checkedReached, slopeFree.safe,
        ProbeReachedStoredDestination(slopeFree)) ? slopeFree : checked;
}

// ConvoyV2: the stored walk path has a cliff segment (DungeonNavigatorConvoy::IsStoredWalkCliff). The direct
// stored route skips it; the graph search walks it only when no cliff-free route reaches the goal.
bool StoredWalkPathHasCliff(TravelNodePath* path)
{
    std::vector<WorldPosition> const points = path->getPath();
    for (std::size_t index = 1; index < points.size(); ++index)
    {
        float const dx = points[index].GetPositionX() - points[index - 1].GetPositionX();
        float const dy = points[index].GetPositionY() - points[index - 1].GetPositionY();
        float const dz = points[index].GetPositionZ() - points[index - 1].GetPositionZ();
        if (DungeonNavigatorConvoy::IsStoredWalkCliff(std::sqrt(dx * dx + dy * dy), dz, StoredWalkCliffStep))
            return true;
    }
    return false;
}

struct ReplanWalkCandidateEvidence
{
    TravelNode* candidateNode = nullptr;
    float nodeDistance = 0.0f;
    std::size_t candidateRank = 0;
    std::size_t candidateLimit = TravelNodeReplanCandidateLimit;
    bool attachmentSafe = false;
    bool attachmentReached = false;
    float attachmentPathLength = 0.0f;
    bool allEdgesWalk = false;
    bool sameMap = false;
    char const* firstSpecialType = "unknown";
    int firstSpecialIndex = -1;
    uint32 firstSpecialEntry = 0;
    uint32 firstSpecialMap = 0;
    bool noSpecialPoint = false;
    bool accepted = false;
};

struct StoredWalkGraphRouteResult
{
    std::vector<PathNodePoint> route;
    std::vector<ReplanWalkCandidateEvidence> replanCandidates;
    bool replanAttempted = false;
};

char const* ReplanSpecialTypeName(TravelNodePathType type)
{
    switch (type)
    {
        case TravelNodePathType::portal:
            return "portal";
        case TravelNodePathType::transport:
            return "transport";
        case TravelNodePathType::flightPath:
            return "flight";
        case TravelNodePathType::teleportSpell:
            return "teleport";
        default:
            return "none";
    }
}

void RecordDirectReplanSpecial(TravelNode* startNode, uint32 mapId,
    ReplanWalkCandidateEvidence& evidence)
{
    evidence.firstSpecialMap = mapId;
    if (!startNode)
        return;

    for (auto const& [nextNode, path] : *startNode->getLinks())
    {
        if (!path || path->getPathType() == TravelNodePathType::walk)
            continue;

        evidence.firstSpecialType = ReplanSpecialTypeName(path->getPathType());
        evidence.firstSpecialIndex = 0;
        evidence.firstSpecialEntry = path->getPathObject();
        evidence.firstSpecialMap = nextNode ? nextNode->getMapId() : 0;
        return;
    }
}

std::vector<PathNodePoint> FindDirectStoredWalkRoute(WorldPosition start,
                                                     WorldPosition finish)
{
    std::vector<TravelNode*> startNodes =
        TravelNodeMap::instance().getNodes(start, TravelNodeAttachmentRadius);
    std::vector<TravelNode*> finishNodes =
        TravelNodeMap::instance().getNodes(finish, TravelNodeAttachmentRadius);
    if (startNodes.size() > TravelNodeAttachmentLimit)
        startNodes.resize(TravelNodeAttachmentLimit);
    if (finishNodes.size() > TravelNodeAttachmentLimit)
        finishNodes.resize(TravelNodeAttachmentLimit);

    TravelNodePath* bestPath = nullptr;
    float bestCost = std::numeric_limits<float>::infinity();
    for (TravelNode* startNode : startNodes)
    {
        for (TravelNode* finishNode : finishNodes)
        {
            if (!startNode || !finishNode || !startNode->hasCompletePathTo(finishNode))
                continue;

            TravelNodePath* path = startNode->getPathTo(finishNode);
            if (!path || path->getPathType() != TravelNodePathType::walk ||
                (ConvoyV2Enabled() && StoredWalkPathHasCliff(path)))
                continue;

            float const cost = start.distance(*startNode->getPosition()) + path->getDistance() +
                finish.distance(*finishNode->getPosition());
            if (cost < bestCost)
            {
                bestCost = cost;
                bestPath = path;
            }
        }
    }

    std::vector<PathNodePoint> route;
    if (!bestPath)
        return route;
    for (WorldPosition const& point : bestPath->getPath())
        route.push_back({point, NODE_PATH, 0});
    return route;
}

StoredWalkGraphRouteResult FindStoredWalkGraphRoute(WorldPosition start, WorldPosition finish,
    Player* bot, bool allowReplanRecovery)
{
    StoredWalkGraphRouteResult result;
    std::vector<TravelNode*> startNodes =
        TravelNodeMap::instance().getNodes(start, TravelNodeAttachmentRadius);
    std::vector<TravelNode*> finishNodes =
        TravelNodeMap::instance().getNodes(finish, TravelNodeAttachmentRadius);
    if (startNodes.size() > TravelNodeAttachmentLimit)
        startNodes.resize(TravelNodeAttachmentLimit);
    if (finishNodes.size() > TravelNodeAttachmentLimit)
        finishNodes.resize(TravelNodeAttachmentLimit);

    struct QueueEntry
    {
        float cost;
        TravelNode* node;
    };
    struct GreaterCost
    {
        bool operator()(QueueEntry const& left, QueueEntry const& right) const
        {
            if (left.cost != right.cost)
                return left.cost > right.cost;
            if (left.node->getX() != right.node->getX())
                return left.node->getX() > right.node->getX();
            if (left.node->getY() != right.node->getY())
                return left.node->getY() > right.node->getY();
            return left.node->getZ() > right.node->getZ();
        }
    };

    auto findGraphFromStartNode = [&](TravelNode* startNode, bool requireCompleteAttachment,
        AutoWowDungeonPath::ProbeResult* attachmentOutput, float& routeCost)
    {
        routeCost = std::numeric_limits<float>::infinity();
        std::vector<TravelNode*> nodes;
        if (!startNode || startNode->getMapId() != start.GetMapId())
            return nodes;

        AutoWowDungeonPath::ProbeResult const attachment = ProbeLeg(
            bot, startNode->getX(), startNode->getY(), startNode->getZ());
        if (attachmentOutput)
            *attachmentOutput = attachment;
        if (!attachment.safe || (requireCompleteAttachment &&
                !ProbeReachedStoredDestination(attachment)))
        {
            return nodes;
        }

        std::priority_queue<QueueEntry, std::vector<QueueEntry>, GreaterCost> queue;
        std::unordered_map<TravelNode*, float> distances;
        std::unordered_map<TravelNode*, TravelNode*> previous;
        distances[startNode] = 0.0f;
        queue.push({0.0f, startNode});

        while (!queue.empty())
        {
            QueueEntry const current = queue.top();
            queue.pop();
            auto const known = distances.find(current.node);
            if (known == distances.end() || current.cost > known->second)
                continue;

            std::vector<std::pair<TravelNode*, TravelNodePath*>> links;
            for (auto const& link : *current.node->getLinks())
                links.push_back(link);
            std::sort(links.begin(), links.end(), [](auto const& left, auto const& right)
            {
                if (left.first->getX() != right.first->getX())
                    return left.first->getX() < right.first->getX();
                if (left.first->getY() != right.first->getY())
                    return left.first->getY() < right.first->getY();
                return left.first->getZ() < right.first->getZ();
            });

            for (auto const& [nextNode, path] : links)
            {
                if (!nextNode || !path || nextNode->getMapId() != start.GetMapId() ||
                    path->getPathType() != TravelNodePathType::walk || !path->getComplete())
                {
                    continue;
                }
                float const nextCost = current.cost + DungeonNavigatorConvoy::StoredWalkLinkCost(
                    ConvoyV2Enabled() && StoredWalkPathHasCliff(path), path->getDistance());
                auto const nextKnown = distances.find(nextNode);
                if (nextKnown != distances.end() && nextKnown->second <= nextCost)
                    continue;
                distances[nextNode] = nextCost;
                previous[nextNode] = current.node;
                queue.push({nextCost, nextNode});
            }
        }

        float bestCost = std::numeric_limits<float>::infinity();
        for (TravelNode* finishNode : finishNodes)
        {
            auto const distance = distances.find(finishNode);
            if (!finishNode || distance == distances.end())
                continue;
            float const totalCost = start.distance(*startNode->getPosition()) + distance->second +
                finish.distance(*finishNode->getPosition());
            if (totalCost >= bestCost)
                continue;

            std::vector<TravelNode*> reversed = {finishNode};
            while (reversed.back() != startNode)
            {
                auto const prior = previous.find(reversed.back());
                if (prior == previous.end())
                {
                    reversed.clear();
                    break;
                }
                reversed.push_back(prior->second);
            }
            if (reversed.empty())
                continue;
            std::reverse(reversed.begin(), reversed.end());
            bestCost = totalCost;
            nodes = std::move(reversed);
        }

        routeCost = bestCost;
        return nodes;
    };

    auto buildRoute = [&](std::vector<TravelNode*> const& nodes, bool requireCurrentMap,
        bool& allEdgesWalk, bool& sameMap)
    {
        std::vector<PathNodePoint> route;
        allEdgesWalk = !nodes.empty();
        sameMap = true;
        for (std::size_t index = 1; index < nodes.size(); ++index)
        {
            TravelNode* const fromNode = nodes[index - 1];
            TravelNode* const toNode = nodes[index];
            TravelNodePath* const path = fromNode && toNode ? fromNode->getPathTo(toNode) : nullptr;
            if (!fromNode || !toNode || !path || path->getPathType() != TravelNodePathType::walk ||
                !path->getComplete())
            {
                allEdgesWalk = false;
                return std::vector<PathNodePoint>();
            }

            std::vector<WorldPosition> points = path->getPath();
            if (points.empty())
                points.push_back(*toNode->getPosition());
            for (WorldPosition const& point : points)
            {
                if (point.GetMapId() != start.GetMapId())
                {
                    sameMap = false;
                    if (requireCurrentMap)
                        return std::vector<PathNodePoint>();
                }
                route.push_back({point, NODE_PATH, 0});
            }
        }
        return route;
    };

    float bestCost = std::numeric_limits<float>::infinity();
    std::vector<TravelNode*> bestNodes;
    for (TravelNode* startNode : startNodes)
    {
        AutoWowDungeonPath::ProbeResult attachment;
        float routeCost = std::numeric_limits<float>::infinity();
        std::vector<TravelNode*> const nodes = findGraphFromStartNode(
            startNode, false, &attachment, routeCost);
        if (!nodes.empty() && routeCost < bestCost)
        {
            bestCost = routeCost;
            bestNodes = nodes;
        }
    }

    bool ignoredAllEdgesWalk = false;
    bool ignoredSameMap = false;
    result.route = buildRoute(bestNodes, false, ignoredAllEdgesWalk, ignoredSameMap);
    if (!result.route.empty() || !allowReplanRecovery)
        return result;

    result.replanAttempted = true;
    std::vector<TravelNode*> replanStartNodes = TravelNodeMap::instance().getNodes(
        start, TravelNodeReplanAttachmentRadius);
    if (replanStartNodes.size() > TravelNodeReplanCandidateLimit)
        replanStartNodes.resize(TravelNodeReplanCandidateLimit);

    for (std::size_t candidateRank = 0; candidateRank < replanStartNodes.size(); ++candidateRank)
    {
        TravelNode* const candidateNode = replanStartNodes[candidateRank];
        ReplanWalkCandidateEvidence evidence;
        evidence.candidateNode = candidateNode;
        evidence.candidateRank = candidateRank;
        evidence.candidateLimit = TravelNodeReplanCandidateLimit;
        evidence.nodeDistance = candidateNode ? start.distance(*candidateNode->getPosition()) :
            std::numeric_limits<float>::infinity();
        evidence.sameMap = candidateNode && candidateNode->getMapId() == start.GetMapId();
        bool const withinDistance = std::isfinite(evidence.nodeDistance) &&
            evidence.nodeDistance <= TravelNodeReplanAttachmentRadius;
        if (candidateNode)
            RecordDirectReplanSpecial(candidateNode, start.GetMapId(), evidence);
        else
            evidence.firstSpecialMap = start.GetMapId();

        AutoWowDungeonPath::ProbeResult attachment;
        float routeCost = std::numeric_limits<float>::infinity();
        std::vector<TravelNode*> const nodes = findGraphFromStartNode(
            candidateNode, true, &attachment, routeCost);
        evidence.attachmentSafe = attachment.safe;
        evidence.attachmentReached = ProbeReachedStoredDestination(attachment);
        evidence.attachmentPathLength = attachment.pathLength;

        bool allEdgesWalk = false;
        bool routeSameMap = true;
        std::vector<PathNodePoint> const candidateRoute = buildRoute(
            nodes, true, allEdgesWalk, routeSameMap);
        evidence.allEdgesWalk = allEdgesWalk && !candidateRoute.empty();
        evidence.sameMap = evidence.sameMap && routeSameMap;
        if (evidence.allEdgesWalk && evidence.sameMap)
        {
            evidence.firstSpecialType = "none";
            evidence.firstSpecialIndex = -1;
            evidence.firstSpecialEntry = 0;
            evidence.firstSpecialMap = start.GetMapId();
            evidence.noSpecialPoint = true;
        }

        DungeonRouteReconnect::ReplanWalkCandidateFacts const facts = {
            candidateRank,
            TravelNodeReplanCandidateLimit,
            withinDistance,
            evidence.sameMap,
            evidence.allEdgesWalk,
            evidence.attachmentSafe,
            evidence.attachmentReached,
            evidence.noSpecialPoint,
        };
        evidence.accepted = DungeonRouteReconnect::AdmitReplanWalkCandidate(facts);
        result.replanCandidates.push_back(evidence);
        if (evidence.accepted)
        {
            result.route = candidateRoute;
            return result;
        }
    }

    return result;
}

char const* StatusName(DungeonEncounterSelection::Status status)
{
    using DungeonEncounterSelection::Status;
    switch (status)
    {
        case Status::Selected: return "selected";
        case Status::NoCandidates: return "no_candidates";
        case Status::AllComplete: return "all_complete";
        case Status::Blocked: return "blocked";
        case Status::InvalidInput: return "invalid_input";
    }
    return "unknown";
}

char const* BlockedReasonName(DungeonEncounterSelection::BlockedReason reason)
{
    using DungeonEncounterSelection::BlockedReason;
    switch (reason)
    {
        case BlockedReason::None: return "none";
        case BlockedReason::InvalidKey: return "invalid_key";
        case BlockedReason::DuplicateKey: return "duplicate_key";
        case BlockedReason::CompletionUnknown: return "completion_unknown";
        case BlockedReason::AlreadyComplete: return "already_complete";
        case BlockedReason::PrerequisitesUnknown: return "prerequisites_unknown";
        case BlockedReason::PrerequisitesBlocked: return "prerequisites_blocked";
        case BlockedReason::ReachabilityUnknown: return "reachability_unknown";
        case BlockedReason::Unreachable: return "unreachable";
    }
    return "unknown";
}

char const* ActivationDecisionName(DungeonEncounterActivation::Decision decision)
{
    using DungeonEncounterActivation::Decision;
    switch (decision)
    {
        case Decision::Attack: return "attack";
        case Decision::NotArrived: return "not_arrived";
        case Decision::SpawnNotLoaded: return "spawn_not_loaded";
        case Decision::SpawnMismatch: return "spawn_mismatch";
        case Decision::Dead: return "dead";
        case Decision::NotInWorld: return "not_in_world";
        case Decision::NotHostile: return "not_hostile";
        case Decision::NotTargetable: return "not_targetable";
        case Decision::NoLineOfSight: return "no_line_of_sight";
    }
    return "unknown";
}

DungeonEncounterList const* GetEncounters(Map const* map)
{
    Difficulty const normalizedDifficulty = IsSharedDifficultyMap(map->GetId()) ?
        Difficulty(map->GetDifficulty() % 2) : map->GetDifficulty();

    // Keep parity with Map::UpdateEncounterState. V0 excludes raids, but retaining its ICC/RS
    // normalization here prevents this adapter from silently diverging if that scope changes.
    if ((map->GetId() == 631 || map->GetId() == 724) && map->IsHeroic())
    {
        return sObjectMgr->GetDungeonEncounterList(map->GetId(), !map->Is25ManRaid() ?
            RAID_DIFFICULTY_10MAN_NORMAL : RAID_DIFFICULTY_25MAN_NORMAL);
    }
    return sObjectMgr->GetDungeonEncounterList(map->GetId(), normalizedDifficulty);
}

bool IsEncounterComplete(InstanceScript const* script, uint32 encounterIndex)
{
    return DungeonEncounterCompletion::IsComplete(
        script->GetCompletedEncounterMask(), encounterIndex);
}

bool ProbeReachedStoredDestination(AutoWowDungeonPath::ProbeResult const& probe)
{
    return DungeonRouteReconnect::DestinationReached(
        probe.safe, probe.mode == "ground_line", (probe.pathType & PATHFIND_NORMAL) != 0,
        (probe.pathType & PATHFIND_INCOMPLETE) != 0);
}

bool MatchesCredit(CreatureData const& spawn, uint32 creditEntry)
{
    return spawn.id == creditEntry || spawn.id2 == creditEntry || spawn.id3 == creditEntry;
}

bool PreferGoal(EncounterGoal const& candidate, EncounterGoal const& current)
{
    return !current.spawnId || candidate.pathLength < current.pathLength ||
        (candidate.pathLength == current.pathLength && candidate.spawnId < current.spawnId);
}

SpellCreditBossFallbackGoal BuildSpellCreditBossFallback(uint32 mapId, uint32 activeSpawnMask,
    InstanceScript const* script,
    std::map<uint32, std::vector<DungeonEncounter const*>> const& encounterGroups)
{
    SpellCreditBossFallbackGoal result;
    if (!script)
        return result;

    std::vector<DungeonSpellCreditBoss::EncounterGroupFacts> fallbackEncounters;
    std::set<uint32> killCreditEntries;
    fallbackEncounters.reserve(encounterGroups.size());
    for (auto const& [encounterIndex, records] : encounterGroups)
    {
        bool hasKillCreatureCredit = false;
        for (DungeonEncounter const* record : records)
        {
            if (!record || record->creditType != ENCOUNTER_CREDIT_KILL_CREATURE)
                continue;
            hasKillCreatureCredit = true;
            killCreditEntries.insert(record->creditEntry);
        }
        fallbackEncounters.push_back(
            {mapId, encounterIndex, IsEncounterComplete(script, encounterIndex),
                hasKillCreatureCredit});
    }

    // Resolve every kill-credit record to its static spawn and all of that spawn's possible
    // entries before considering script-name fallback candidates. Completion is intentionally
    // irrelevant here: a known kill-credit boss remains reserved even after it is complete.
    std::set<uint32> killMappedSpawnIds;
    std::set<uint32> killMappedEntries;
    for (auto const& [spawnId, spawn] : sObjectMgr->GetAllCreatureData())
    {
        bool mapped = false;
        for (uint32 creditEntry : killCreditEntries)
        {
            if (MatchesCredit(spawn, creditEntry))
            {
                mapped = true;
                break;
            }
        }
        if (!mapped)
            continue;

        killMappedSpawnIds.insert(uint32(spawnId));
        if (spawn.id)
            killMappedEntries.insert(spawn.id);
        if (spawn.id2)
            killMappedEntries.insert(spawn.id2);
        if (spawn.id3)
            killMappedEntries.insert(spawn.id3);
    }

    std::vector<DungeonSpellCreditBoss::StaticSpawnFacts> fallbackSpawns;
    for (auto const& [spawnId, spawn] : sObjectMgr->GetAllCreatureData())
    {
        if (spawn.mapid != mapId || !(spawn.spawnMask & activeSpawnMask))
            continue;

        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(spawn.id);
        if (!creatureTemplate)
            continue;
        std::string const& scriptName =
            sObjectMgr->GetScriptName(creatureTemplate->ScriptID);
        uint32 const numericSpawnId = uint32(spawnId);
        bool const mappedByKillCredit = killMappedSpawnIds.count(numericSpawnId) ||
            killMappedEntries.count(spawn.id) || killMappedEntries.count(spawn.id2) ||
            killMappedEntries.count(spawn.id3);
        fallbackSpawns.push_back({mapId, uint32(spawn.spawnMask), numericSpawnId, spawn.id,
            scriptName, mappedByKillCredit});
    }

    result.binding = DungeonSpellCreditBoss::Select(
        mapId, activeSpawnMask, fallbackEncounters, fallbackSpawns);
    if (!result.binding.selected)
        return result;

    CreatureData const* spawn = sObjectMgr->GetCreatureData(result.binding.spawnId);
    if (!spawn || spawn->mapid != mapId || !(spawn->spawnMask & activeSpawnMask) ||
        spawn->id != result.binding.entry)
    {
        return {};
    }
    result.goal = {result.binding.spawnId, spawn->posX, spawn->posY, spawn->posZ};
    return result;
}

bool ShouldLogSpellCreditBossFallback(uint32 mapId, uint32 instanceId, uint32 encounterId,
    uint32 spawnId)
{
    static std::set<SpellCreditBossDiagnosticKey> loggedSelections;
    static std::mutex loggedSelectionsMutex;
    std::lock_guard<std::mutex> lock(loggedSelectionsMutex);
    return loggedSelections.insert({mapId, instanceId, encounterId, spawnId}).second;
}
}

void DungeonNavigateNextEncounterAction::ResetTravelRoute()
{
    travelRoute.clear();
    travelRouteEncounterId = 0;
    travelRouteSpawnId = 0;
    travelRouteMapId = 0;
    travelRouteInstanceId = 0;
    travelRouteNextIndex = 0;
    travelRouteInitialized = false;
    travelRouteBlocked = false;
    travelRouteBlockedReason = "none";
    lastTravelEncounterId = 0;
    lastTravelSpawnId = 0;
    lastTravelWaypointIndex = 0;
    travelNoProgressRetries = 0;
    travelNoProgressReplans = 0;
    walkRevalidationState = {};
    convoyBackwardReanchorFloors.clear();
    convoySettledRouteFloors.clear();
    directHopRouteIndex = DungeonRouteReconnect::NoSelection;
    directHopAttempts = 0;
    ClearConvoySharedRegroupAttempts(this);
}

void DungeonNavigateNextEncounterAction::ReplanTravelRoute()
{
    // Keep the exact encounter/map identity so the next scan rebuilds this route from the bot's
    // current physical position. ResetTravelRoute would erase that identity and also erase the
    // bounded replan count, allowing an accidental infinite rebuild loop.
    std::size_t const priorRoutePointCount = travelRoute.size();
    std::size_t const priorRouteIndex = travelRouteNextIndex;
    travelRoute.clear();
    // These two fields are scratch state for the immediately following replan-only build. The
    // route builder consumes and clears them before ordinary cursor processing resumes.
    travelRouteNextIndex = priorRouteIndex;
    travelRouteBlocked = false;
    travelRouteBlockedReason = "none";
    lastTravelEncounterId = 0;
    lastTravelSpawnId = 0;
    lastTravelWaypointIndex = priorRoutePointCount;
    travelNoProgressRetries = 0;
    convoyBackwardReanchorFloors.clear();
    convoySettledRouteFloors.clear();
    directHopRouteIndex = DungeonRouteReconnect::NoSelection;
    directHopAttempts = 0;
    ClearConvoySharedRegroupAttempts(this);
}

void DungeonNavigateNextEncounterAction::BlockTravelRoute(char const* reason)
{
    travelRoute.clear();
    travelRouteNextIndex = 0;
    travelRouteBlocked = true;
    travelRouteBlockedReason = reason;
    convoyBackwardReanchorFloors.clear();
    convoySettledRouteFloors.clear();
    ClearConvoySharedRegroupAttempts(this);
}

int32 GetDungeonNavigatorTargetEncounter(uint32 botGuid, uint32 mapId, uint32 instanceId)
{
    std::lock_guard<std::mutex> lock(navigatorTargetEncountersMutex);
    auto const target = navigatorTargetEncounters.find(botGuid);
    return target != navigatorTargetEncounters.end() && target->second[0] == mapId &&
        target->second[1] == instanceId ? int32(target->second[2]) : -1;
}

void DungeonNavigatorStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // This is route discovery only. Looting, recovery, and map-specific actions remain ahead of it.
    triggers.push_back(new TriggerNode(
        "timer", {NextAction("dungeon navigate next encounter", 1.0f)}));
}

bool DungeonNavigateNextEncounterAction::Execute(Event /*event*/)
{
    if (!bot || !bot->IsAlive() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->isMoving())
    {
        return false;
    }

    Group* group = bot->GetGroup();
    // Dungeon progression is a party-leader responsibility.  Failing closed for
    // groupless bots also prevents independently logged-in party members from
    // racing toward encounter spawns while their persistent group is restored.
    if (!group || !group->IsLeader(bot->GetGUID()))
        return false;

    uint32 const now = getMSTime();
    if (nextScanTime && int32(nextScanTime - now) > 0)
        return false;

    Map* map = bot->GetMap();
    if (map && travelRouteInitialized &&
        (travelRouteMapId != map->GetId() || travelRouteInstanceId != map->GetInstanceId()))
    {
        ResetTravelRoute();
    }

    // The encounter and travel-node machinery is shared by five-player dungeons
    // and raids.  Keep battlegrounds and world maps out, but allow raid instance
    // maps to use the same data-driven route executor instead of bespoke points.
    if (!map || !map->IsDungeon())
        return false;

    InstanceMap* instanceMap = map->ToInstanceMap();
    InstanceScript* script = instanceMap ? instanceMap->GetInstanceScript() : nullptr;
    if (!instanceMap || !script)
        return false;

    struct ConvoyPlan
    {
        Player* member = nullptr;
        PlayerbotAI* memberAI = nullptr;
        std::size_t routeIndex = 0;
        AutoWowDungeonPath::ProbeResult probe;
        bool backwardReanchor = false;
        std::size_t leaderFrontier = 0;
        std::size_t fromRouteIndex = 0;
        bool postCombatRejoin = false;
        bool sharedRegroup = false;
        bool leaderMove = false;
        uint8 sharedAttempt = 0;
    };

    // Encounter completion is observed after combat/loot, while the cached route still names
    // the defeated encounter until the selection pass below. Never let that stale frontier block
    // the party before the next encounter can be selected. Rejoin separated followers to the
    // stationary leader through a freshly proven ordinary path, without teleporting or skipping
    // any dungeon transition.
    bool postCombatConvoyRecovery = false;
    if (travelRouteInitialized && IsEncounterComplete(script, travelRouteEncounterId))
    {
        uint32 const completedEncounter = travelRouteEncounterId;
        postCombatConvoyRecovery = true;
        ResetTravelRoute();
        LOG_INFO("playerbots",
            "[DungeonNavigator] bot={} map={} recovery=convoy_post_combat_route_reset "
            "completed_encounter={}",
            bot->GetName(), map->GetId(), completedEncounter);
    }

    std::vector<Player*> followers;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
        {
            nextScanTime = now + PartyCohesionBackoffMs;
            return false;
        }
        if (member != bot)
            followers.push_back(member);
    }
    std::sort(followers.begin(), followers.end(), [](Player const* left, Player const* right)
    {
        return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
    });

    auto combatGateAllowsConvoy = [&](Player* member, float leaderDistance)
    {
        ConvoyCombatGate const gate = EvaluateConvoyCombat(member, now);
        if (gate.decision == DungeonNavigatorCombat::Decision::BlockActive ||
            gate.decision == DungeonNavigatorCombat::Decision::BlockAmbiguous)
        {
            nextScanTime = now + PartyCohesionBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} blocked=party_cohesion member={} distance={} "
                "combat={} pet_combat={} combat_gate={} ambiguous_ms={} victim={} attackers={} "
                "relationships={} threat={} cast={}",
                bot->GetName(), map->GetId(), member->GetName(), leaderDistance,
                gate.signals.combatFlag, gate.signals.petCombatFlag,
                gate.decision == DungeonNavigatorCombat::Decision::BlockActive ?
                    "active" : "ambiguous",
                gate.ambiguousDurationMs, gate.signals.liveVictim,
                gate.signals.liveAttackers, gate.signals.combatRelationships,
                gate.signals.liveThreat, gate.signals.combatCast);
            return false;
        }
        if (gate.decision == DungeonNavigatorCombat::Decision::RecoverAndAllow)
        {
            RecoverFlagOnlyCombat(member);
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} convoy_member={} recovery=stale_combat_lock "
                "action=stop_attack_pet_follow ambiguous_ms={} combat={} pet_combat={}",
                bot->GetName(), map->GetId(), member->GetName(),
                gate.ambiguousDurationMs, gate.signals.combatFlag,
                gate.signals.petCombatFlag);
        }
        return true;
    };

    if (!combatGateAllowsConvoy(bot, 0.0f))
        return false;

    bool const hasConvoyRoute = travelRouteInitialized && !travelRouteBlocked &&
        !travelRoute.empty() && travelRouteMapId == map->GetId() &&
        travelRouteInstanceId == map->GetInstanceId();
    std::size_t leaderFrontier = travelRouteNextIndex;
    if (hasConvoyRoute && lastTravelEncounterId == travelRouteEncounterId &&
        lastTravelSpawnId == travelRouteSpawnId)
    {
        leaderFrontier = std::max(leaderFrontier, lastTravelWaypointIndex);
    }
    if (hasConvoyRoute)
        leaderFrontier = std::min(leaderFrontier, travelRoute.size() - 1);

    std::vector<ConvoyPlan> convoyPlans;
    convoyPlans.reserve(followers.size());
    // A stationary follower that has fallen behind is sent only to an exact cached-route point.
    // Hold the leader for this scan so route-ordered convoy motion happens before further progress.
    for (std::size_t followerIndex = 0; followerIndex < followers.size(); ++followerIndex)
    {
        Player* member = followers[followerIndex];
        std::size_t const followerOrdinal = followerIndex + 1;
        std::size_t const maximumRouteIndex = hasConvoyRoute && leaderFrontier > followerOrdinal ?
            leaderFrontier - followerOrdinal : 0;

        // Player followers can very rarely retain ordinary XY progress while their server-side Z
        // falls below WMO collision. Detect that state against this follower's already-established
        // trailing route slot, not against a hard-coded map height. The correction is same-XY,
        // out-of-combat, bounded, and accepted only when the corrected source has a complete
        // ordinary continuation path to the assigned route point. It therefore cannot skip a door,
        // encounter, elevator, or any horizontal dungeon content.
        if (hasConvoyRoute)
        {
            DungeonNavigatorRoutePoint const& assignedPoint = travelRoute[maximumRouteIndex];
            bool const sameContext = member->GetMap() == map;
            float const ground = sameContext ? map->GetHeight(member->GetPhaseMask(),
                member->GetPositionX(), member->GetPositionY(),
                assignedPoint.z + ConvoyGroundProbeAboveRoute, true,
                ConvoyGroundProbeDepth) : INVALID_HEIGHT;
            bool const floorValid = ground > INVALID_HEIGHT && std::isfinite(ground);
            float const correction = floorValid ? ground - member->GetPositionZ() :
                std::numeric_limits<float>::infinity();
            bool continuationReached = false;
            float continuationLength = std::numeric_limits<float>::infinity();
            if (sameContext && floorValid && assignedPoint.mapId == map->GetId())
            {
                AutoWowDungeonPath::ProbeResult const continuation =
                    AutoWowDungeonPath::ProbeFrom(member, member->GetPositionX(),
                        member->GetPositionY(), ground, assignedPoint.x, assignedPoint.y,
                        assignedPoint.z);
                continuationReached = continuation.safe &&
                    ProbeReachedStoredDestination(continuation);
                continuationLength = continuation.pathLength;
            }

            bool const onTransport = member->GetTransport() || member->GetVehicle();
            if (DungeonNavigatorConvoy::CanGroundReattach(hasConvoyRoute, sameContext,
                    member->IsAlive(), member->IsInCombat(), member->IsBeingTeleported(),
                    onTransport, floorValid, correction, continuationReached,
                    ConvoyGroundReattachMinimum, ConvoyGroundReattachMaximum))
            {
                float const fromZ = member->GetPositionZ();
                if (MotionMaster* motion = member->GetMotionMaster())
                    motion->Clear();
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::Emit(member, AutoWowQuestLedger::Event::Contaminated, 0, "dungeon_ground_reattach");
                member->NearTeleportTo(member->GetPositionX(), member->GetPositionY(),
                    ground + GroundReattachOffset, member->GetOrientation());
                nextScanTime = now + PartyCohesionBackoffMs;
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} recovery=convoy_ground_reattach "
                    "member={} route_index={} from_z={} to_z={} correction={} "
                    "continuation_length={}",
                    bot->GetName(), map->GetId(), member->GetName(), maximumRouteIndex,
                    fromZ, ground + GroundReattachOffset, correction, continuationLength);
                return true;
            }

            // If the follower's XY lies inside a collision hole, the same-XY height query above
            // legitimately fails. Reattach only to this follower's exact trailing route slot,
            // bounded horizontally and vertically, and prove ordinary forward continuation from
            // that slot before changing position. The target is strictly behind leaderFrontier.
            std::size_t const continuationIndex =
                std::min(maximumRouteIndex + 1, leaderFrontier);
            if (continuationIndex < travelRoute.size())
            {
                DungeonNavigatorRoutePoint const& continuationPoint =
                    travelRoute[continuationIndex];
                bool const routeSameMap = assignedPoint.mapId == map->GetId() &&
                    continuationPoint.mapId == map->GetId();
                float const routeGround = routeSameMap ? map->GetHeight(member->GetPhaseMask(),
                    assignedPoint.x, assignedPoint.y,
                    assignedPoint.z + ConvoyGroundProbeAboveRoute, true,
                    ConvoyGroundProbeDepth) : INVALID_HEIGHT;
                bool const routeFloorValid = routeGround > INVALID_HEIGHT &&
                    std::isfinite(routeGround) &&
                    std::fabs(routeGround - assignedPoint.z) <= ConvoyRouteGroundTolerance;
                float const dx = assignedPoint.x - member->GetPositionX();
                float const dy = assignedPoint.y - member->GetPositionY();
                float const horizontalDistance = std::sqrt(dx * dx + dy * dy);
                float const routeCorrection = routeFloorValid ?
                    routeGround - member->GetPositionZ() :
                    std::numeric_limits<float>::infinity();
                bool routeContinuationReached = false;
                float routeContinuationLength = std::numeric_limits<float>::infinity();
                if (sameContext && routeFloorValid && routeSameMap &&
                    continuationIndex > maximumRouteIndex)
                {
                    AutoWowDungeonPath::ProbeResult const routeContinuation =
                        AutoWowDungeonPath::ProbeFrom(member, assignedPoint.x, assignedPoint.y,
                            routeGround, continuationPoint.x, continuationPoint.y,
                            continuationPoint.z);
                    routeContinuationReached = routeContinuation.safe &&
                        ProbeReachedStoredDestination(routeContinuation);
                    routeContinuationLength = routeContinuation.pathLength;
                }

                if (DungeonNavigatorConvoy::CanRouteSlotReattach(hasConvoyRoute, sameContext,
                        member->IsAlive(), member->IsInCombat(), member->IsBeingTeleported(),
                        onTransport, routeFloorValid, routeSameMap, maximumRouteIndex,
                        leaderFrontier, horizontalDistance, routeCorrection,
                        routeContinuationReached, ConvoyRouteReattachMaximumHorizontal,
                        ConvoyGroundReattachMinimum, ConvoyGroundReattachMaximum))
                {
                    float const fromX = member->GetPositionX();
                    float const fromY = member->GetPositionY();
                    float const fromZ = member->GetPositionZ();
                    if (MotionMaster* motion = member->GetMotionMaster())
                        motion->Clear();
                    if (AutoWowQuestLedger::Enabled())
                        AutoWowQuestLedger::Emit(member, AutoWowQuestLedger::Event::Contaminated, 0, "dungeon_ground_reattach");
                    member->NearTeleportTo(assignedPoint.x, assignedPoint.y,
                        routeGround + GroundReattachOffset, member->GetOrientation());
                    nextScanTime = now + PartyCohesionBackoffMs;
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} recovery=convoy_route_slot_reattach "
                        "member={} route_index={} continuation_index={} from_x={} from_y={} "
                        "from_z={} to_x={} to_y={} to_z={} horizontal={} correction={} "
                        "continuation_length={}",
                        bot->GetName(), map->GetId(), member->GetName(), maximumRouteIndex,
                        continuationIndex, fromX, fromY, fromZ, assignedPoint.x,
                        assignedPoint.y, routeGround + GroundReattachOffset,
                        horizontalDistance, routeCorrection, routeContinuationLength);
                    return true;
                }
            }
        }

        float const leaderDistance = bot->GetExactDist(member);
        if (member->IsBeingTeleported())
        {
            nextScanTime = now + PartyCohesionBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} blocked=party_cohesion member={} distance={} combat={} teleporting={}",
                bot->GetName(), map->GetId(), member->GetName(), bot->GetExactDist(member),
                member->IsInCombat(), member->IsBeingTeleported());
            return false;
        }
        if (!combatGateAllowsConvoy(member, leaderDistance))
            return false;
        uint32 const convoyMemberGuid = member->GetGUID().GetCounter();
        if (IsConvoySharedRegroupActive(this, convoyMemberGuid))
        {
            if (!member->isMoving() && leaderDistance <= ConvoyAdvanceDistance)
            {
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_rejoined "
                    "member={} anchor_route_index={} distance={}",
                    bot->GetName(), map->GetId(), member->GetName(),
                    GetConvoySharedRegroupAnchor(this, convoyMemberGuid), leaderDistance);
                EraseConvoySharedRegroupAttempts(this, convoyMemberGuid);
            }
            else
            {
                uint8 const waitScan = ConsumeConvoySharedRegroupWait(this, convoyMemberGuid,
                    ConvoySharedRegroupAttemptLimit);
                if (!waitScan)
                {
                    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(
                        this, convoyMemberGuid);
                    bool const enteredTerminal = state.terminalState.hasContext &&
                        DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                            state.terminalState, state.terminalState.context, true) ==
                        DungeonRouteReconnect::SharedRegroupTerminalDecision::EnterTerminal;
                    SetConvoySharedRegroupAttempts(this, convoyMemberGuid,
                        ConvoySharedRegroupAttemptLimit);
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    if (enteredTerminal)
                    {
                        state.terminalLeaderFrontier = leaderFrontier;
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} blocked=convoy_shared_regroup_terminal "
                            "member={} anchor_route_index={} distance={} moving={} attempts={} "
                            "attempt_limit={} wait_limit={} terminal=latched",
                            bot->GetName(), map->GetId(), member->GetName(),
                            GetConvoySharedRegroupAnchor(this, convoyMemberGuid), leaderDistance,
                            member->isMoving(), ConvoySharedRegroupAttemptLimit,
                            ConvoySharedRegroupAttemptLimit, ConvoySharedRegroupAttemptLimit);
                    }
                }
                else
                {
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_wait "
                        "member={} anchor_route_index={} distance={} moving={} wait_scan={} "
                        "wait_limit={}",
                        bot->GetName(), map->GetId(), member->GetName(),
                        GetConvoySharedRegroupAnchor(this, convoyMemberGuid), leaderDistance,
                        member->isMoving(), waitScan, ConvoySharedRegroupAttemptLimit);
                }
                return false;
            }
        }
        else if (!member->isMoving() && leaderDistance <= ConvoyAdvanceDistance)
        {
            EraseConvoySharedRegroupAttempts(this, convoyMemberGuid);
        }

        if (postCombatConvoyRecovery && !member->isMoving() &&
            leaderDistance > ConvoyAdvanceDistance)
        {
            PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (!memberAI)
            {
                nextScanTime = now + PartyCohesionBackoffMs;
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} blocked=convoy_missing_ai "
                    "member={} recovery=convoy_post_combat_rejoin",
                    bot->GetName(), map->GetId(), member->GetName());
                return false;
            }

            AutoWowDungeonPath::ProbeResult leaderProbe = ProbeLeg(
                member, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            bool const leaderPathReached = leaderProbe.safe &&
                ProbeReachedStoredDestination(leaderProbe);
            bool const sameContext = member->GetMap() == map;
            bool const onTransport = member->GetTransport() || member->GetVehicle();
            if (DungeonNavigatorConvoy::CanPostCombatRejoin(
                    postCombatConvoyRecovery, sameContext, member->IsAlive(),
                    member->IsInCombat(), member->IsBeingTeleported(), onTransport,
                    leaderPathReached, leaderProbe.pathLength, leaderDistance,
                    ConvoyAdvanceDistance, ConvoyPostCombatRejoinMaximumPathLength))
            {
                convoyPlans.push_back({member, memberAI, 0, std::move(leaderProbe), false,
                    leaderFrontier, 0, true});
                continue;
            }

            nextScanTime = now + PartyCohesionBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} blocked=convoy_post_combat_no_reachable_leader "
                "member={} distance={} path_safe={} path_reached={} probe={}",
                bot->GetName(), map->GetId(), member->GetName(), leaderDistance,
                leaderProbe.safe, leaderPathReached, AutoWowDungeonPath::Json(leaderProbe, false));
            return false;
        }

        bool const needsConvoyMove = hasConvoyRoute && !member->isMoving() &&
            leaderDistance > ConvoyAdvanceDistance;
        if (needsConvoyMove)
        {
            PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (!memberAI)
            {
                nextScanTime = now + PartyCohesionBackoffMs;
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} blocked=convoy_missing_ai member={}",
                    bot->GetName(), map->GetId(), member->GetName());
                return false;
            }

            DungeonNavigatorRoutePoint const& assignedPoint = travelRoute[maximumRouteIndex];
            float const assignedDistance = member->GetExactDist(
                assignedPoint.x, assignedPoint.y, assignedPoint.z);
            if (DungeonNavigatorConvoy::IsSettledAtAssignedPoint(
                    assignedPoint.mapId == map->GetId(), assignedDistance,
                    TravelArrivalRadius))
            {
                convoyBackwardReanchorFloors.erase(member->GetGUID().GetCounter());
                EraseConvoySharedRegroupAttempts(this, member->GetGUID().GetCounter());
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} convoy_member={} route_index={} "
                    "route_points={} movement=settled distance={}",
                    bot->GetName(), map->GetId(), member->GetName(), maximumRouteIndex,
                    travelRoute.size(), assignedDistance);
                continue;
            }
            std::size_t const scanBegin = maximumRouteIndex + 1 > ConvoyRoutePointLimit ?
                maximumRouteIndex + 1 - ConvoyRoutePointLimit : 0;
            std::vector<DungeonNavigatorConvoy::Candidate> candidates;
            std::vector<AutoWowDungeonPath::ProbeResult> probes;
            candidates.reserve(maximumRouteIndex - scanBegin + 1);
            probes.reserve(maximumRouteIndex - scanBegin + 1);
            for (std::size_t routeIndex = scanBegin; routeIndex <= maximumRouteIndex; ++routeIndex)
            {
                DungeonNavigatorRoutePoint const& point = travelRoute[routeIndex];
                AutoWowDungeonPath::ProbeResult const probe = ProbeLeg(
                    member, point.x, point.y, point.z);
                candidates.push_back({routeIndex, point.mapId == map->GetId(), probe.safe,
                    ProbeReachedStoredDestination(probe), probe.pathLength,
                    member->GetExactDist(point.x, point.y, point.z)});
                probes.push_back(probe);
            }

            // ConvoyV2: an unreachable slot must not bounce the follower between the points
            // around it. Settle on the best reachable point it already stands on, and never step
            // below that point while the leader frontier is unchanged.
            std::size_t settledFloor = 0;
            if (ConvoyV2Enabled())
            {
                uint32 const settledGuid = member->GetGUID().GetCounter();
                std::size_t const settledIndex = DungeonNavigatorConvoy::SettledRouteIndex(
                    candidates, maximumRouteIndex, TravelArrivalRadius, ConvoyMaximumPathLength);
                if (settledIndex != DungeonNavigatorConvoy::NoSelection)
                {
                    convoySettledRouteFloors[settledGuid] = {leaderFrontier, settledIndex};
                    convoyBackwardReanchorFloors.erase(settledGuid);
                    EraseConvoySharedRegroupAttempts(this, settledGuid);
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} convoy_member={} route_index={} "
                        "route_points={} movement=settled_best_reachable slot_index={} "
                        "leader_frontier={} distance={} slot_distance={}",
                        bot->GetName(), map->GetId(), member->GetName(), settledIndex,
                        travelRoute.size(), maximumRouteIndex, leaderFrontier,
                        candidates[settledIndex - scanBegin].physicalProgress, assignedDistance);
                    continue;
                }
                auto const settled = convoySettledRouteFloors.find(settledGuid);
                if (settled != convoySettledRouteFloors.end() &&
                    settled->second.first == leaderFrontier)
                {
                    settledFloor = settled->second.second;
                }
            }

            std::size_t const selection = DungeonNavigatorConvoy::SelectTarget(candidates,
                leaderFrontier, followerOrdinal, ConvoyMinimumProgress,
                ConvoyMaximumPathLength, settledFloor);
            if (selection == DungeonNavigatorConvoy::NoSelection)
            {
                // A follower displaced slightly off its trailing route slot by ordinary combat is
                // still a valid convoy member while physically inside the normal party cohesion
                // envelope. Do not force an unsafe backwards spline or stall the entire group;
                // hold its proven world position while cautious leader progress makes another
                // exact cached route slot reachable. Navigator-owned groups never depend on
                // ordinary formation movement to catch up here.
                if (DungeonNavigatorConvoy::CanHoldWithinCohesion(
                        member->GetMapId() == map->GetId(), leaderDistance,
                        ConvoyAdvanceDistance, PartyCohesionRadius))
                {
                    convoyBackwardReanchorFloors.erase(member->GetGUID().GetCounter());
                    EraseConvoySharedRegroupAttempts(this, member->GetGUID().GetCounter());
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} convoy_member={} route_index={} "
                        "route_points={} movement=cohesion_hold distance={}",
                        bot->GetName(), map->GetId(), member->GetName(), maximumRouteIndex,
                        travelRoute.size(), leaderDistance);
                    continue;
                }

                // A follower can be stranded on a collision layer that has no ordinary path to
                // any of its recent route slots. Before accepting a follower-only recovery, look
                // for one established route anchor that both sides can reach by normal walking.
                // The leader is moved to that same anchor so the convoy reconnects instead of
                // repeatedly retrying a path from the wrong side of the split.
                uint32 const memberGuid = member->GetGUID().GetCounter();
                DungeonRouteReconnect::SharedRegroupContext const sharedContext = {
                    map->GetId(), map->GetInstanceId(), travelRouteEncounterId,
                    travelRouteSpawnId, memberGuid};
                EnsureConvoySharedRegroupContext(this, memberGuid, sharedContext);
                if (ConvoyV2Enabled() &&
                    IsConvoySharedRegroupTerminal(this, memberGuid, sharedContext))
                {
                    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(
                        this, memberGuid);
                    if (DungeonNavigatorConvoy::ShouldReleaseSharedRegroupTerminal(
                            state.terminalLeaderFrontier, leaderFrontier, leaderDistance,
                            PartyCohesionRadius))
                    {
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_terminal_released "
                            "member={} terminal_frontier={} leader_frontier={} distance={}",
                            bot->GetName(), map->GetId(), member->GetName(),
                            state.terminalLeaderFrontier, leaderFrontier, leaderDistance);
                        state = {};
                        EnsureConvoySharedRegroupContext(this, memberGuid, sharedContext);
                    }
                }
                uint8 const sharedAttempts = GetConvoySharedRegroupAttempts(this, memberGuid);
                if (IsConvoySharedRegroupTerminal(this, memberGuid, sharedContext))
                {
                    // The bounded search already emitted its terminal for this exact route and
                    // member. Do not re-probe or emit an identical terminal on later ticks.
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    return false;
                }
                if (sharedAttempts >= ConvoySharedRegroupAttemptLimit)
                {
                    ConvoySharedRegroupState& state = GetConvoySharedRegroupState(
                        this, memberGuid);
                    DungeonRouteReconnect::SharedRegroupTerminalDecision const terminalDecision =
                        DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                            state.terminalState, sharedContext, true);
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    if (terminalDecision ==
                        DungeonRouteReconnect::SharedRegroupTerminalDecision::EnterTerminal)
                    {
                        state.terminalLeaderFrontier = leaderFrontier;
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} blocked=convoy_shared_regroup_terminal "
                            "member={} leader_frontier={} slot={} attempts={} attempt_limit={} "
                            "candidate_limit={} path_limit={} terminal=latched",
                            bot->GetName(), map->GetId(), member->GetName(), leaderFrontier,
                            followerOrdinal, sharedAttempts, ConvoySharedRegroupAttemptLimit,
                            ConvoyBackwardReanchorPointLimit, ConvoySharedRegroupMaximumPathLength);
                    }
                    return false;
                }

                uint8 const nextSharedAttempt = static_cast<uint8>(sharedAttempts + 1);
                std::size_t const sharedCandidateLimit =
                    DungeonRouteReconnect::SelectSharedAnchorSearchPrefix(
                        nextSharedAttempt, maximumRouteIndex + 1,
                        {ConvoySharedRegroupSmallPrefix, ConvoySharedRegroupMediumPrefix,
                         ConvoyBackwardReanchorPointLimit});
                if (!sharedCandidateLimit)
                {
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    return false;
                }

                for (Player* other : followers)
                {
                    if (other == member)
                        continue;
                    if (other->IsBeingTeleported())
                    {
                        nextScanTime = now + PartyCohesionBackoffMs;
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} blocked=party_cohesion "
                            "member={} distance={} combat={} teleporting={}",
                            bot->GetName(), map->GetId(), other->GetName(),
                            bot->GetExactDist(other), other->IsInCombat(),
                            other->IsBeingTeleported());
                        return false;
                    }
                    if (!combatGateAllowsConvoy(other, bot->GetExactDist(other)))
                    {
                        return false;
                    }
                }

                DungeonNavigatorConvoy::SharedRegroupBounds const sharedBounds = {
                    sharedCandidateLimit,
                    ConvoyMinimumProgress,
                    ConvoySharedRegroupMinimumLeaderRetreat,
                    ConvoySharedRegroupMaximumPathLength,
                };
                std::size_t const sharedScanBegin = maximumRouteIndex + 1 >
                    sharedCandidateLimit ? maximumRouteIndex + 1 - sharedCandidateLimit : 0;
                std::vector<DungeonNavigatorConvoy::SharedCandidate> sharedCandidates;
                std::vector<AutoWowDungeonPath::ProbeResult> sharedFollowerProbes;
                std::vector<AutoWowDungeonPath::ProbeResult> sharedLeaderProbes;
                sharedCandidates.reserve(maximumRouteIndex - sharedScanBegin + 1);
                sharedFollowerProbes.reserve(sharedCandidates.capacity());
                sharedLeaderProbes.reserve(sharedCandidates.capacity());
                auto const isOrdinaryWalkProbe = [](AutoWowDungeonPath::ProbeResult const& probe)
                {
                    return probe.mode == "navmesh" || probe.mode == "ground_line";
                };

                for (std::size_t routeIndex = maximumRouteIndex;;)
                {
                    DungeonNavigatorRoutePoint const& point = travelRoute[routeIndex];
                    AutoWowDungeonPath::ProbeResult followerProbe;
                    if (routeIndex >= scanBegin)
                    {
                        followerProbe = probes[routeIndex - scanBegin];
                    }
                    else
                    {
                        followerProbe = ProbeLeg(
                            member, point.x, point.y, point.z);
                    }
                    AutoWowDungeonPath::ProbeResult const leaderProbe =
                        ProbeLeg(bot, point.x, point.y, point.z);
                    sharedCandidates.push_back({
                        routeIndex,
                        point.mapId == map->GetId(),
                        isOrdinaryWalkProbe(followerProbe) && isOrdinaryWalkProbe(leaderProbe),
                        followerProbe.safe,
                        ProbeReachedStoredDestination(followerProbe),
                        followerProbe.pathLength,
                        member->GetExactDist(point.x, point.y, point.z),
                        leaderProbe.safe,
                        ProbeReachedStoredDestination(leaderProbe),
                        leaderProbe.pathLength,
                        bot->GetExactDist(point.x, point.y, point.z),
                    });
                    sharedFollowerProbes.push_back(std::move(followerProbe));
                    sharedLeaderProbes.push_back(leaderProbe);

                    if (routeIndex == sharedScanBegin ||
                        sharedCandidates.size() >= sharedCandidateLimit)
                    {
                        break;
                    }
                    --routeIndex;
                }

                auto const keepsPartyCohesion = [&](std::size_t selection)
                {
                    DungeonNavigatorRoutePoint const& point =
                        travelRoute[sharedCandidates[selection].routeIndex];
                    for (Player* other : followers)
                    {
                        if (other == member)
                            continue;
                        float const distance = other->GetExactDist(point.x, point.y, point.z);
                        if (!std::isfinite(distance) || distance > PartyCohesionRadius)
                            return false;
                    }
                    return true;
                };

                auto const sharedRemembered = convoyBackwardReanchorFloors.find(memberGuid);
                std::size_t const sharedRememberedRouteFloor =
                    sharedRemembered == convoyBackwardReanchorFloors.end() ?
                    DungeonNavigatorConvoy::NoSelection : sharedRemembered->second;
                std::size_t sharedSelection = DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
                    sharedCandidates, leaderFrontier, maximumRouteIndex, sharedBounds,
                    sharedRememberedRouteFloor);
                while (sharedSelection != DungeonNavigatorConvoy::NoSelection &&
                       !keepsPartyCohesion(sharedSelection))
                {
                    sharedCandidates[sharedSelection].ordinaryWalk = false;
                    sharedSelection = DungeonNavigatorConvoy::SelectSharedBackwardReanchor(
                        sharedCandidates, leaderFrontier, maximumRouteIndex, sharedBounds,
                        sharedRememberedRouteFloor);
                }

                SetConvoySharedRegroupAttempts(this, memberGuid, nextSharedAttempt);
                if (sharedSelection != DungeonNavigatorConvoy::NoSelection)
                {
                    DungeonNavigatorConvoy::SharedCandidate const& sharedCandidate =
                        sharedCandidates[sharedSelection];
                    std::size_t const anchorRouteIndex = sharedCandidate.routeIndex;
                    convoyPlans.clear();
                    convoyPlans.push_back({bot, botAI, anchorRouteIndex,
                        std::move(sharedLeaderProbes[sharedSelection]), false, leaderFrontier,
                        leaderFrontier, false, true, true, nextSharedAttempt});
                    convoyPlans.push_back({member, memberAI, anchorRouteIndex,
                        std::move(sharedFollowerProbes[sharedSelection]), false, leaderFrontier,
                        maximumRouteIndex, false, true, false, nextSharedAttempt});
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_selected "
                        "member={} anchor_route_index={} leader_frontier={} slot={} "
                        "candidate_count={} candidate_limit={} attempt={} attempt_limit={} "
                        "follower_path_length={} leader_path_length={} follower_progress={} "
                        "leader_retreat={} path_limit={}",
                        bot->GetName(), map->GetId(), member->GetName(), anchorRouteIndex,
                        leaderFrontier, followerOrdinal, sharedCandidates.size(),
                        sharedCandidateLimit, nextSharedAttempt,
                        ConvoySharedRegroupAttemptLimit, sharedCandidate.followerPathLength,
                        sharedCandidate.leaderPathLength, sharedCandidate.followerPhysicalProgress,
                        sharedCandidate.leaderPhysicalProgress, ConvoySharedRegroupMaximumPathLength);
                    break;
                }

                ConvoySharedRegroupState& state = GetConvoySharedRegroupState(
                    this, memberGuid);
                std::size_t const hardCandidateLimit = std::min<std::size_t>(
                    maximumRouteIndex + 1, ConvoyBackwardReanchorPointLimit);
                bool const fullyExhausted = DungeonNavigatorConvoy::IsSharedRegroupExhausted(
                    ConvoyV2Enabled(), nextSharedAttempt, ConvoySharedRegroupAttemptLimit,
                    sharedCandidateLimit, hardCandidateLimit);
                DungeonRouteReconnect::SharedRegroupTerminalDecision const terminalDecision =
                    DungeonRouteReconnect::ObserveSharedRegroupNoCandidate(
                        state.terminalState, sharedContext, fullyExhausted);
                if (terminalDecision ==
                    DungeonRouteReconnect::SharedRegroupTerminalDecision::EnterTerminal)
                {
                    state.terminalLeaderFrontier = leaderFrontier;
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} blocked=convoy_shared_regroup_terminal "
                        "member={} leader_frontier={} slot={} attempts={} attempt_limit={} "
                        "candidate_count={} candidate_limit={} path_limit={} terminal=latched",
                        bot->GetName(), map->GetId(), member->GetName(), leaderFrontier,
                        followerOrdinal, nextSharedAttempt, ConvoySharedRegroupAttemptLimit,
                        sharedCandidates.size(), sharedCandidateLimit,
                        ConvoySharedRegroupMaximumPathLength);
                    return false;
                }

                if (terminalDecision ==
                    DungeonRouteReconnect::SharedRegroupTerminalDecision::NoOp)
                {
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    return false;
                }

                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_no_candidate "
                    "member={} leader_frontier={} slot={} candidate_count={} candidate_limit={} "
                    "attempt={} attempt_limit={} path_limit={}",
                    bot->GetName(), map->GetId(), member->GetName(), leaderFrontier,
                    followerOrdinal, sharedCandidates.size(), sharedCandidateLimit,
                    nextSharedAttempt, ConvoySharedRegroupAttemptLimit,
                    ConvoySharedRegroupMaximumPathLength);
                if (fullyExhausted)
                {
                    nextScanTime = now + ConvoyBackwardReanchorBackoffMs;
                    return false;
                }

                // A vertical split can leave the follower on a lower collision layer while the
                // leader's recent trailing slots are all on the upper floor. Probe the established
                // route backward, never beyond this follower's normal slot, and take the first
                // complete ordinary path found. Descending iteration plus the pure policy makes
                // that point the deterministic furthest reachable reanchor. No route cursor or
                // player position is mutated here.
                uint32 const backwardMemberGuid = member->GetGUID().GetCounter();
                auto const remembered = convoyBackwardReanchorFloors.find(backwardMemberGuid);
                std::size_t const rememberedRouteFloor =
                    remembered == convoyBackwardReanchorFloors.end() ?
                        DungeonNavigatorConvoy::NoSelection : remembered->second;
                // Reuse the normal bounded probes before looking farther back; recovery remains
                // capped at 128 total established points rather than probing the recent window
                // twice.
                std::vector<DungeonNavigatorConvoy::Candidate> recoveryCandidates = candidates;
                std::vector<AutoWowDungeonPath::ProbeResult> recoveryProbes = probes;
                recoveryCandidates.reserve(std::min<std::size_t>(
                    maximumRouteIndex + 1, ConvoyBackwardReanchorPointLimit));
                recoveryProbes.reserve(recoveryCandidates.capacity());
                std::size_t recoverySelection =
                    DungeonNavigatorConvoy::SelectBackwardReanchor(recoveryCandidates,
                        leaderFrontier, maximumRouteIndex, ConvoyMinimumProgress,
                        rememberedRouteFloor);
                std::size_t examined = recoveryCandidates.size();
                std::size_t routeIndex = scanBegin;
                while (recoverySelection == DungeonNavigatorConvoy::NoSelection && routeIndex &&
                       examined < ConvoyBackwardReanchorPointLimit &&
                       (rememberedRouteFloor == DungeonNavigatorConvoy::NoSelection ||
                           rememberedRouteFloor < scanBegin))
                {
                    --routeIndex;
                    DungeonNavigatorRoutePoint const& point = travelRoute[routeIndex];
                    AutoWowDungeonPath::ProbeResult const probe = ProbeLeg(
                        member, point.x, point.y, point.z);
                    recoveryCandidates.push_back({routeIndex, point.mapId == map->GetId(),
                        probe.safe, ProbeReachedStoredDestination(probe), probe.pathLength,
                        member->GetExactDist(point.x, point.y, point.z)});
                    recoveryProbes.push_back(probe);
                    ++examined;
                    recoverySelection = DungeonNavigatorConvoy::SelectBackwardReanchor(
                        recoveryCandidates, leaderFrontier, maximumRouteIndex,
                        ConvoyMinimumProgress, rememberedRouteFloor);
                    if (rememberedRouteFloor != DungeonNavigatorConvoy::NoSelection &&
                        routeIndex == rememberedRouteFloor)
                    {
                        break;
                    }
                }

                if (recoverySelection != DungeonNavigatorConvoy::NoSelection)
                {
                    convoyPlans.push_back({member, memberAI,
                        recoveryCandidates[recoverySelection].routeIndex,
                        recoveryProbes[recoverySelection], true, leaderFrontier,
                        maximumRouteIndex});
                    continue;
                }

                nextScanTime = now + PartyCohesionBackoffMs;
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} blocked=convoy_no_reachable_route_point "
                    "member={} distance={} leader_frontier={} slot={} slot_probe={}",
                    bot->GetName(), map->GetId(), member->GetName(), leaderDistance,
                    leaderFrontier, followerOrdinal, probes.empty() ? std::string("{}") :
                        AutoWowDungeonPath::Json(probes.back(), false));
                return false;
            }
            convoyPlans.push_back(
                {member, memberAI, candidates[selection].routeIndex, probes[selection], false,
                    leaderFrontier, maximumRouteIndex});
        }
        else if (leaderDistance > PartyCohesionRadius)
        {
            nextScanTime = now + PartyCohesionBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} blocked=party_cohesion member={} distance={} combat={} teleporting={}",
                bot->GetName(), map->GetId(), member->GetName(), leaderDistance,
                member->IsInCombat(), member->IsBeingTeleported());
            return false;
        }
    }

    if (!convoyPlans.empty())
    {
        bool allStarted = true;
        bool usedBackwardReanchor = false;
        bool sharedLeaderStarted = false;
        bool sharedFollowerStarted = false;
        uint32 sharedMemberGuid = 0;
        std::size_t sharedAnchorRouteIndex = DungeonNavigatorConvoy::NoSelection;
        for (ConvoyPlan const& plan : convoyPlans)
        {
            AutoWowDungeonWalkAction walk(plan.memberAI);
            bool const started = walk.WalkPrepared(plan.probe);
            allStarted = allStarted && started;
            uint32 const memberGuid = plan.member->GetGUID().GetCounter();
            if (plan.sharedRegroup)
            {
                usedBackwardReanchor = true;
                if (plan.leaderMove)
                {
                    sharedLeaderStarted = started;
                    sharedAnchorRouteIndex = plan.routeIndex;
                }
                else
                {
                    sharedFollowerStarted = started;
                    sharedMemberGuid = memberGuid;
                    sharedAnchorRouteIndex = plan.routeIndex;
                }
                if (started && !plan.leaderMove)
                {
                    auto const remembered = convoyBackwardReanchorFloors.find(memberGuid);
                    if (remembered == convoyBackwardReanchorFloors.end())
                        convoyBackwardReanchorFloors.emplace(memberGuid, plan.routeIndex);
                    else
                        remembered->second = std::max(remembered->second, plan.routeIndex);
                }
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup "
                    "role={} member={} anchor_route_index={} frontier={} from={} movement={} "
                    "path_length={} attempt={} attempt_limit={}",
                    bot->GetName(), map->GetId(), plan.leaderMove ? "leader" : "follower",
                    plan.member->GetName(), plan.routeIndex, plan.leaderFrontier,
                    plan.fromRouteIndex, started ? "started" : "rejected", plan.probe.pathLength,
                    plan.sharedAttempt, ConvoySharedRegroupAttemptLimit);
            }
            else if (plan.backwardReanchor)
            {
                usedBackwardReanchor = true;
                if (started)
                {
                    auto const remembered = convoyBackwardReanchorFloors.find(memberGuid);
                    if (remembered == convoyBackwardReanchorFloors.end())
                        convoyBackwardReanchorFloors.emplace(memberGuid, plan.routeIndex);
                    else
                        remembered->second = std::max(remembered->second, plan.routeIndex);
                }
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} recovery=convoy_backward_reanchor "
                    "member={} frontier={} from={} to={} movement={} path_length={}",
                    bot->GetName(), map->GetId(), plan.member->GetName(),
                    plan.leaderFrontier, plan.fromRouteIndex, plan.routeIndex,
                    started ? "started" : "rejected", plan.probe.pathLength);
            }
            else
            {
                if (plan.postCombatRejoin)
                {
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} recovery=convoy_post_combat_rejoin "
                        "member={} distance={} path_length={} movement={}",
                        bot->GetName(), map->GetId(), plan.member->GetName(),
                        plan.member->GetExactDist(bot), plan.probe.pathLength,
                        started ? "started" : "rejected");
                }
                else
                {
                    if (started)
                        convoyBackwardReanchorFloors.erase(memberGuid);
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} convoy_member={} route_index={} "
                        "route_points={} movement={}",
                        bot->GetName(), map->GetId(), plan.member->GetName(), plan.routeIndex,
                        travelRoute.size(), started ? "started" : "rejected");
                }
            }
        }
        if (sharedLeaderStarted && sharedFollowerStarted &&
            sharedAnchorRouteIndex != DungeonNavigatorConvoy::NoSelection)
        {
            ActivateConvoySharedRegroup(this, sharedMemberGuid, sharedAnchorRouteIndex);
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} recovery=convoy_shared_regroup_armed "
                "member={} anchor_route_index={} attempt={} attempt_limit={}",
                bot->GetName(), map->GetId(), sharedMemberGuid, sharedAnchorRouteIndex,
                GetConvoySharedRegroupAttempts(this, sharedMemberGuid),
                ConvoySharedRegroupAttemptLimit);
        }
        nextScanTime = now + (usedBackwardReanchor ? ConvoyBackwardReanchorBackoffMs :
            (allStarted ? SuccessfulMoveRescanDelayMs() : PartyCohesionBackoffMs));
        return false;
    }

    DungeonEncounterList const* encounters = GetEncounters(map);
    if (!encounters)
    {
        ResetTravelRoute();
        nextScanTime = now + BlockedScanBackoffMs;
        LOG_DEBUG("playerbots", "[DungeonNavigator] bot={} map={} blocked=no_encounter_list",
            bot->GetName(), map->GetId());
        return false;
    }

    std::map<uint32, std::vector<DungeonEncounter const*>> encounterGroups;
    for (DungeonEncounter const* encounter : *encounters)
    {
        if (encounter && encounter->dbcEntry)
            encounterGroups[encounter->dbcEntry->encounterIndex].push_back(encounter);
    }

    // AutoWow.DungeonNav.Gates: encounter -> DungeonGate::Steps row whose position replaces the goal, and
    // how the leader walks there (the per-scan "approach_*" gate line).
    std::map<uint32, std::size_t> gateSteps;
    std::map<uint32, char const*> gateApproaches;
    if (GatesEnabled() && (gateMapId != map->GetId() || gateInstanceId != map->GetInstanceId()))
    {
        gateRuntime.clear();
        gateLogLast.clear();
        gateMapId = map->GetId();
        gateInstanceId = map->GetInstanceId();
    }
    // info = false: a per-scan decision, rate-limited per (row, result) by DungeonGate::ShouldLog.
    auto logGate = [&](std::size_t row, char const* result, bool info)
    {
        if (!info)
        {
            auto const inserted = gateLogLast.try_emplace({row, result}, now);
            if (!inserted.second)
            {
                if (!DungeonGate::ShouldLog(true, getMSTimeDiff(inserted.first->second, now)))
                    return;
                inserted.first->second = now;
            }
        }
        DungeonGate::Step const& step = DungeonGate::Steps[row];
        LOG_INFO("playerbots",
            "[DungeonNavigator] gate map={} enc={} step={} kind={} entry={} result={} bot={}",
            step.mapId, step.encounterIdx, step.stepOrder, DungeonGate::KindName(step.kind),
            step.entry, result, bot->GetName());
    };
    // First party member holding the item: the navigating bot, then followers in guid order.
    auto gateItemHolder = [&](uint32 itemId) -> Player*
    {
        if (bot->HasItemCount(itemId, 1))
            return bot;
        for (Player* member : followers)
            if (member->HasItemCount(itemId, 1))
                return member;
        return nullptr;
    };
    auto gateStepDone = [&](DungeonGate::Step const& step)
    {
        switch (step.doneWhen)
        {
            case DungeonGate::DoneWhen::GoUsed:
            {
                GameObject* gameObject =
                    GateObject(map, step.doneData ? step.doneData : step.spawnGuid);
                return gameObject && (gameObject->GetGoState() != GO_STATE_READY ||
                    gameObject->HasGameObjectFlag(GO_FLAG_IN_USE) ||
                    gameObject->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE));
            }
            case DungeonGate::DoneWhen::CreaturesDead:
            {
                bool searched = false;
                bool const live = NearestLiveGateCreature(bot, map, step, searched) != nullptr;
                return searched && !live;
            }
            case DungeonGate::DoneWhen::InstanceData:
                return script->GetData(step.doneData) >= step.doneValue;
            case DungeonGate::DoneWhen::EncounterDone:
                return IsEncounterComplete(script, step.encounterIdx);
            case DungeonGate::DoneWhen::Escorting:
            {
                Creature* creature = GateCreature(map, step.spawnGuid);
                return creature && creature->AI() && creature->AI()->IsEscorted();
            }
            case DungeonGate::DoneWhen::Unlocked:
            {
                if (step.doneValue && gateItemHolder(step.doneValue))
                    return true;
                // An instance reloaded with the Iron Clad Door stored open despawns the door.
                GameObject* door = GateObject(map, step.doneData);
                return door ? door->GetGoState() != GO_STATE_READY : map->IsGridLoaded(step.x, step.y);
            }
        }
        return false;
    };
    auto selectGateStep = [&](uint32 encounterIndex)
    {
        std::vector<std::size_t> const rowIndices = DungeonGate::StepsFor(map->GetId(), encounterIndex);
        std::vector<DungeonGate::Step> rows;
        std::vector<DungeonGate::StepRuntime> runtime;
        for (std::size_t row : rowIndices)
        {
            rows.push_back(DungeonGate::Steps[row]);
            runtime.push_back(gateRuntime[row]);
        }
        std::size_t const selected = DungeonGate::SelectStep(rows, runtime, GateBypassKeys(),
            [&](uint32 itemId) { return gateItemHolder(itemId) != nullptr; },
            [&](std::size_t index)
            {
                if (!gateStepDone(rows[index]))
                    return false;
                gateRuntime[rowIndices[index]].done = true;
                logGate(rowIndices[index], "done", true);
                return true;
            });
        return selected == DungeonGate::NoStep ? DungeonGate::NoStep : rowIndices[selected];
    };

    std::vector<DungeonEncounterSelection::CandidateFacts> candidates;
    std::map<uint32, EncounterGoal> goals;
    candidates.reserve(encounterGroups.size());
    uint8 const activeSpawnMask = uint8(1u << map->GetSpawnMode());
    SpellCreditBossFallbackGoal const spellCreditFallback = BuildSpellCreditBossFallback(
        map->GetId(), activeSpawnMask, script, encounterGroups);

    for (auto const& [encounterIndex, records] : encounterGroups)
    {
        bool const complete = IsEncounterComplete(script, encounterIndex);
        if (complete && travelRouteInitialized && travelRouteEncounterId == encounterIndex)
            ResetTravelRoute();

        EncounterGoal bestGoal;
        EncounterGoal fallbackGoal;
        uint32 diagnosticGoalId = 0;
        std::set<uint32> probedSpawns;

        if (!complete)
        {
            for (DungeonEncounter const* record : records)
            {
                if (record->creditType != ENCOUNTER_CREDIT_KILL_CREATURE)
                    continue;

                for (auto const& [spawnId, spawn] : sObjectMgr->GetAllCreatureData())
                {
                    uint32 const numericSpawnId = uint32(spawnId);
                    if (spawn.mapid != map->GetId() || !(spawn.spawnMask & activeSpawnMask) ||
                        !MatchesCredit(spawn, record->creditEntry))
                    {
                        continue;
                    }

                    if (!diagnosticGoalId || numericSpawnId < diagnosticGoalId)
                    {
                        diagnosticGoalId = numericSpawnId;
                        fallbackGoal = {numericSpawnId, spawn.posX, spawn.posY, spawn.posZ};
                    }
                    if (!probedSpawns.insert(numericSpawnId).second)
                        continue;

                    AutoWowDungeonPath::ProbeResult const probe =
                        AutoWowDungeonPath::Probe(bot, spawn.posX, spawn.posY, spawn.posZ);
                    // A safe NORMAL|INCOMPLETE probe is only bounded progress toward a distant
                    // goal. Treating it as direct reachability lets the leader consume the whole
                    // partial corridor in one MoveTo and outrun the party. Only a navmesh probe
                    // that reached the requested spawn may bypass stored-route progression.
                    if (probe.mode != "navmesh" || !ProbeReachedStoredDestination(probe))
                        continue;

                    EncounterGoal const candidate = {
                        numericSpawnId, spawn.posX, spawn.posY, spawn.posZ, probe.pathLength};
                    if (PreferGoal(candidate, bestGoal))
                        bestGoal = candidate;
                }
            }
        }

        if (!complete && spellCreditFallback.binding.selected &&
            spellCreditFallback.binding.encounterId == encounterIndex)
        {
            diagnosticGoalId = spellCreditFallback.binding.spawnId;
            fallbackGoal = spellCreditFallback.goal;
            AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
                bot, fallbackGoal.x, fallbackGoal.y, fallbackGoal.z);
            if (probe.mode == "navmesh" && ProbeReachedStoredDestination(probe))
            {
                fallbackGoal.pathLength = probe.pathLength;
                bestGoal = fallbackGoal;
            }
        }

        if (!complete && !bestGoal.spawnId && fallbackGoal.spawnId)
        {
            fallbackGoal.travelNodes = true;
            fallbackGoal.finalX = fallbackGoal.x;
            fallbackGoal.finalY = fallbackGoal.y;
            fallbackGoal.finalZ = fallbackGoal.z;
            bestGoal = fallbackGoal;
        }

        if (!complete && GatesEnabled())
        {
            std::size_t const gateRow = selectGateStep(encounterIndex);
            if (gateRow != DungeonGate::NoStep)
            {
                DungeonGate::Step const& step = DungeonGate::Steps[gateRow];
                bestGoal = {DungeonGate::GoalId(gateRow), step.x, step.y, step.z};
                AutoWowDungeonPath::ProbeResult probe =
                    AutoWowDungeonPath::Probe(bot, step.x, step.y, step.z);
                bool const reached = probe.mode == "navmesh" && ProbeReachedStoredDestination(probe);
                bool slopeFreeReached = false;
                if (!reached)
                {
                    probe = AutoWowDungeonPath::Probe(bot, step.x, step.y, step.z, false);
                    slopeFreeReached = probe.mode == "navmesh" && ProbeReachedStoredDestination(probe);
                }
                if (DungeonGate::SelectApproach(reached, slopeFreeReached) == DungeonGate::Approach::Direct)
                {
                    bestGoal.pathLength = probe.pathLength;
                    gateApproaches[encounterIndex] = reached ? "approach_direct" : "approach_slope_free";
                }
                else
                {
                    bestGoal.travelNodes = true;
                    bestGoal.finalX = step.x;
                    bestGoal.finalY = step.y;
                    bestGoal.finalZ = step.z;
                    gateApproaches[encounterIndex] = "approach_travel_nodes";
                }
                gateSteps[encounterIndex] = gateRow;
            }
        }

        if (bestGoal.spawnId)
            goals[encounterIndex] = bestGoal;

        DungeonEncounterSelection::CandidateFacts facts;
        facts.key = {map->GetId(), encounterIndex};
        // The policy requires a non-zero diagnostic goal even for blocked candidates. For an
        // unsafe encounter this is the lowest matching static spawn, never a movement target.
        // Script/spell-only encounters use the DBC row id and remain Unreachable unless the
        // conservative unique-boss policy supplied an ordinary static-spawn goal above.
        facts.goalId = bestGoal.spawnId ? bestGoal.spawnId : diagnosticGoalId;
        if (!facts.goalId && !records.empty())
            facts.goalId = records.front()->dbcEntry->id;
        facts.routeOrder = encounterIndex;
        facts.score = 0;
        facts.completion = complete ? DungeonEncounterSelection::Completion::Complete :
            DungeonEncounterSelection::Completion::Incomplete;
        facts.prerequisites = DungeonEncounterSelection::GateState::Satisfied;
        facts.reachability = bestGoal.spawnId ? DungeonEncounterSelection::Reachability::Reachable :
            DungeonEncounterSelection::Reachability::Unreachable;
        candidates.push_back(facts);
    }

    DungeonEncounterSelection::Selection const selection =
        DungeonEncounterSelection::Select(candidates);
    if (selection.status != DungeonEncounterSelection::Status::Selected)
    {
        ResetTravelRoute();
        nextScanTime = now + BlockedScanBackoffMs;
        LOG_DEBUG("playerbots", "[DungeonNavigator] bot={} map={} status={}",
            bot->GetName(), map->GetId(), StatusName(selection.status));
        for (DungeonEncounterSelection::CandidateDecision const& decision : selection.decisions)
        {
            if (decision.reason != DungeonEncounterSelection::BlockedReason::None)
            {
                LOG_DEBUG("playerbots", "[DungeonNavigator] map={} encounter={} goal={} blocked={}",
                    decision.key.mapId, decision.key.encounterId, decision.goalId,
                    BlockedReasonName(decision.reason));
            }
        }
        return false;
    }

    auto const goalItr = goals.find(selection.selected.encounterId);
    if (goalItr == goals.end() || goalItr->second.spawnId != selection.selectedGoalId)
    {
        ResetTravelRoute();
        nextScanTime = now + BlockedScanBackoffMs;
        LOG_DEBUG("playerbots", "[DungeonNavigator] bot={} map={} encounter={} blocked=goal_mismatch",
            bot->GetName(), map->GetId(), selection.selected.encounterId);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(navigatorTargetEncountersMutex);
        navigatorTargetEncounters[bot->GetGUID().GetCounter()] =
            {map->GetId(), map->GetInstanceId(), selection.selected.encounterId};
    }

    auto const gateStep = gateSteps.find(selection.selected.encounterId);
    if (gateStep != gateSteps.end())
    {
        std::size_t const row = gateStep->second;
        DungeonGate::Step const& step = DungeonGate::Steps[row];
        Creature* escorted = step.kind == DungeonGate::Kind::Escort ?
            GateCreature(map, step.spawnGuid) : nullptr;
        bool const escorting = escorted && escorted->IsAlive() && escorted->IsInWorld();
        bool const arrived = escorting || bot->GetExactDist(step.x, step.y, step.z) <= step.radius;
        if (!arrived)
            logGate(row, gateApproaches[selection.selected.encounterId], false);
        if (arrived)
        {
            DungeonGate::StepRuntime& runtime = gateRuntime[row];
            uint32 const elapsed = runtime.firstActMs ? getMSTimeDiff(runtime.firstActMs, now) : 0;
            DungeonGate::Wait const wait = DungeonGate::Evaluate(step, runtime, elapsed);
            nextScanTime = now + GateHoldMs;
            if (wait == DungeonGate::Wait::Skip)
            {
                runtime.skipped = true;
                logGate(row, "timeout_skip", true);
                return false;
            }
            if (wait == DungeonGate::Wait::Retry)
            {
                runtime = {};
                logGate(row, "timeout_retry", true);
                return false;
            }
            if (!runtime.firstActMs)
                runtime.firstActMs = now ? now : 1;
            if (wait == DungeonGate::Wait::Hold)
            {
                logGate(row, "wait", false);
                return false;
            }

            if (step.kind != DungeonGate::Kind::Escort)
            {
                // A direct gate walk moves the navigator only. Soak S56: the alliance probe's followers stayed
                // at the Gunpowder chest, 42 yd from the cannon (> DefaultSupportRadius), and the row logged
                // wait_party until the run was declared stuck. Walk each idle out-of-combat straggler to the
                // navigator (slope-checked path, else the slope-free one, like the navigator's approach).
                bool waiting = false;
                bool walked = false;
                for (Player* member : followers)
                {
                    if (bot->GetExactDist(member) <= DungeonPullReadiness::DefaultSupportRadius)
                        continue;
                    waiting = true;
                    PlayerbotAI* memberAI = member->isMoving() || member->IsInCombat() ? nullptr :
                        PlayerbotsMgr::instance().GetPlayerbotAI(member);
                    if (!memberAI)
                        continue;
                    AutoWowDungeonPath::ProbeResult probe = AutoWowDungeonPath::Probe(member,
                        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                    if (!probe.safe || !ProbeReachedStoredDestination(probe))
                        probe = AutoWowDungeonPath::Probe(member, bot->GetPositionX(), bot->GetPositionY(),
                            bot->GetPositionZ(), false);
                    if (AutoWowDungeonWalkAction(memberAI).WalkPrepared(probe))
                        walked = true;
                }
                if (waiting)
                {
                    nextScanTime = now + PartyCohesionBackoffMs;
                    logGate(row, walked ? "party_walk" : "wait_party", false);
                    return false;
                }
            }

            auto moveNear = [&](WorldObject* target, float distance)
            {
                bool const moved = MoveTo(target, distance, MovementPriority::MOVEMENT_NORMAL);
                nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() : BlockedScanBackoffMs);
                logGate(row, moved ? "move" : "move_rejected", !moved);
                return moved;
            };

            switch (step.kind)
            {
                case DungeonGate::Kind::UseGo:
                {
                    GameObject* gameObject = GateObject(map, step.spawnGuid);
                    if (!gameObject || !gameObject->isSpawned() ||
                        gameObject->GetGoState() != GO_STATE_READY ||
                        gameObject->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
                        gameObject->HasGameObjectFlag(GO_FLAG_IN_USE))
                    {
                        logGate(row, gameObject ? "go_not_ready" : "go_missing", false);
                        return false;
                    }
                    if (!bot->IsWithinDistInMap(gameObject, gameObject->GetInteractionDistance()))
                        return moveNear(gameObject,
                            std::max(0.0f, gameObject->GetInteractionDistance() - 1.0f));

                    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
                    use << gameObject->GetGUID();
                    bot->GetSession()->HandleGameObjectUseOpcode(use);
                    ++runtime.acts;
                    logGate(row, "use", true);
                    return true;
                }
                case DungeonGate::Kind::Gossip:
                {
                    Creature* npc = GateCreature(map, step.spawnGuid);
                    if (!npc || !npc->IsAlive() || !npc->IsInWorld())
                    {
                        logGate(row, "npc_missing", false);
                        return false;
                    }
                    if (!bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
                        return moveNear(npc, INTERACTION_DISTANCE - 1.0f);

                    // Same packet pair as the stock gossip actions (GossipHelloAction, ICC gunship).
                    bot->SetFacingToObject(npc);
                    WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
                    hello << npc->GetGUID();
                    bot->GetSession()->HandleGossipHelloOpcode(hello);
                    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION);
                    select << npc->GetGUID();
                    select << uint32(bot->PlayerTalkClass->GetGossipMenu().GetMenuId());
                    select << uint32(step.gossipOption);
                    bot->GetSession()->HandleGossipSelectOptionOpcode(select);
                    ++runtime.acts;
                    logGate(row, "gossip", true);
                    return true;
                }
                case DungeonGate::Kind::Escort:
                {
                    if (!escorting)
                    {
                        logGate(row, "npc_missing", false);
                        return false;
                    }
                    if (bot->GetExactDist(escorted) <= GateEscortDistance)
                    {
                        logGate(row, "wait", false);
                        return false;
                    }
                    return moveNear(escorted, GateEscortFollowDistance);
                }
                case DungeonGate::Kind::KillSet:
                case DungeonGate::Kind::ProxyKill:
                {
                    bool searched = false;
                    Creature* target = NearestLiveGateCreature(bot, map, step, searched);
                    if (!target)
                    {
                        logGate(row, "no_target", false);
                        return false;
                    }
                    if (!bot->IsWithinDistInMap(target, GateAttackDistance))
                        return moveNear(target, GateAttackApproachDistance);

                    bool const attacked = Attack(target);
                    nextScanTime = now + (attacked ? SuccessfulMoveRescanDelayMs() :
                        BlockedScanBackoffMs);
                    logGate(row, attacked ? "attack" : "attack_rejected", true);
                    return attacked;
                }
                case DungeonGate::Kind::EnterArea:
                    logGate(row, "wait", false);
                    return false;
                case DungeonGate::Kind::LootGo:
                {
                    GameObject* chest = GateObject(map, step.spawnGuid);
                    if (!chest || !chest->isSpawned() || chest->GetGoState() != GO_STATE_READY ||
                        chest->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
                        chest->HasGameObjectFlag(GO_FLAG_IN_USE))
                    {
                        logGate(row, chest ? "go_not_ready" : "go_missing", false);
                        return false;
                    }
                    uint32 const lockId = chest->GetGOInfo()->GetLockId();
                    if (!GameObjectLockPolicy::HasOrdinaryOpenAlternative(
                            lockId ? sLockStore.LookupEntry(lockId) : nullptr))
                    {
                        logGate(row, "go_locked", true);
                        return false;
                    }
                    if (!bot->IsWithinDistInMap(chest, INTERACTION_DISTANCE - 1.0f))
                        return moveNear(chest, INTERACTION_DISTANCE - 2.0f);

                    // Same exact-GO loot path as the NewRpgAction objective chest: the core
                    // SendLoot/LootItemInSlot/StoreLootItem pipeline generates and stores the item.
                    if (bot->isMoving())
                        bot->StopMoving();
                    bot->SetFacingToObject(chest);
                    bot->SendLoot(chest->GetGUID(), LOOT_SKINNING);
                    uint32 const maxSlot = chest->loot.GetMaxSlotInLootFor(bot);
                    for (uint32 slot = 0; slot < maxSlot; ++slot)
                    {
                        LootItem* item = chest->loot.LootItemInSlot(slot, bot);
                        if (!item || item->itemid != step.doneValue)
                            continue;
                        WorldPacket autostore(CMSG_AUTOSTORE_LOOT_ITEM, 1);
                        autostore << static_cast<uint8>(slot);
                        bot->GetSession()->HandleAutostoreLootItemOpcode(autostore);
                        break;
                    }
                    WorldPacket release(CMSG_LOOT_RELEASE, 8);
                    release << chest->GetGUID();
                    bot->GetSession()->HandleLootReleaseOpcode(release);
                    ++runtime.acts;
                    logGate(row, "loot", true);
                    return true;
                }
                case DungeonGate::Kind::UseItemOnGo:
                {
                    GameObject* gameObject = GateObject(map, step.spawnGuid);
                    if (!gameObject || !gameObject->isSpawned() ||
                        gameObject->GetGoState() != GO_STATE_READY)
                    {
                        logGate(row, gameObject ? "go_not_ready" : "go_missing", false);
                        return false;
                    }
                    Player* holder = gateItemHolder(step.keyItem);
                    Item* key = holder ? holder->GetItemByEntry(step.keyItem) : nullptr;
                    if (!key)
                    {
                        logGate(row, "no_key", false);
                        return false;
                    }
                    if (!holder->IsWithinDistInMap(gameObject, GateItemUseDistance))
                    {
                        if (holder == bot)
                            return moveNear(gameObject, GateItemUseApproachDistance);
                        PlayerbotAI* holderAI = PlayerbotsMgr::instance().GetPlayerbotAI(holder);
                        bool const moved = holderAI && AutoWowDungeonWalkAction(holderAI).Walk(
                            map->GetId(), gameObject->GetPositionX(), gameObject->GetPositionY(),
                            gameObject->GetPositionZ());
                        nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() : BlockedScanBackoffMs);
                        logGate(row, moved ? "holder_move" : "holder_move_rejected", !moved);
                        return moved;
                    }
                    if (holder->isMoving())
                    {
                        holder->StopMoving();
                        logGate(row, "holder_stop", false);
                        return false;
                    }

                    // Same CMSG_USE_ITEM layout as UseItemAction::UseItem with a GO target; the core
                    // item/spell/lock path checks range and the key and consumes it.
                    uint32 spellId = 0;
                    for (auto const& spell : key->GetTemplate()->Spells)
                    {
                        if (spell.SpellId > 0)
                        {
                            spellId = uint32(spell.SpellId);
                            break;
                        }
                    }
                    WorldPacket use(CMSG_USE_ITEM);
                    use << key->GetBagSlot() << key->GetSlot() << uint8(1) << spellId << key->GetGUID()
                        << uint32(0) << uint8(0);
                    use << uint32(TARGET_FLAG_GAMEOBJECT) << gameObject->GetGUID().WriteAsPacked();
                    holder->GetSession()->HandleUseItemOpcode(use);
                    ++runtime.acts;
                    // Soak S56: one use_item line could not tell a started cast from a refused one. 6250 has
                    // a 2 s cast broken by movement; the key is consumed only when it lands on the cannon,
                    // whose SAI then opens the door (the row's `done` line).
                    bool const casting = holder->FindCurrentSpellBySpellId(spellId) != nullptr;
                    logGate(row, casting ? "use_item" : "use_item_no_cast", true);
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] gate map={} enc={} step={} holder={} item={} spell={} casting={}",
                        step.mapId, step.encounterIdx, step.stepOrder, holder->GetName(), step.keyItem,
                        spellId, casting);
                    return true;
                }
            }
            return false;
        }
    }

    EncounterGoal goal = goalItr->second;
    if (spellCreditFallback.binding.selected &&
        spellCreditFallback.binding.encounterId == selection.selected.encounterId &&
        spellCreditFallback.binding.spawnId == goal.spawnId &&
        ShouldLogSpellCreditBossFallback(map->GetId(), map->GetInstanceId(),
            selection.selected.encounterId, goal.spawnId))
    {
        LOG_INFO("playerbots",
            "[DungeonNavigator] map={} encounter={} spawn={} entry={} script={} "
            "reason=unique_unmapped_spell_credit_boss",
            map->GetId(), selection.selected.encounterId, goal.spawnId,
            spellCreditFallback.binding.entry, spellCreditFallback.binding.scriptName);
    }
    if (!goal.travelNodes && goal.pathLength <= TravelArrivalRadius)
    {
        auto const creatureBounds = map->GetCreatureBySpawnIdStore().equal_range(goal.spawnId);
        if (creatureBounds.first == creatureBounds.second)
        {
            nextScanTime = now + BlockedScanBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                "activation_blocked={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                ActivationDecisionName(DungeonEncounterActivation::Decision::SpawnNotLoaded));
            return false;
        }

        for (auto itr = creatureBounds.first; itr != creatureBounds.second; ++itr)
        {
            Creature* creature = itr->second;
            DungeonEncounterActivation::TargetFacts const facts = {
                true,
                creature != nullptr,
                creature && creature->GetSpawnId() == goal.spawnId,
                creature && creature->IsAlive(),
                creature && creature->IsInWorld(),
                creature && creature->IsHostileTo(bot),
                creature && creature->isTargetableForAttack() && bot->IsValidAttackTarget(creature),
                creature && bot->IsWithinLOSInMap(creature),
            };
            DungeonEncounterActivation::Decision const decision =
                DungeonEncounterActivation::Evaluate(facts);
            if (decision != DungeonEncounterActivation::Decision::Attack)
            {
                // With gates on, maps that have table rows never use the nearest-button rule (Sunken
                // Temple statues must go in order).
                if (decision == DungeonEncounterActivation::Decision::NotTargetable &&
                    !(GatesEnabled() && DungeonGate::MapHasSteps(map->GetId())))
                {
                    DungeonProgressionInteraction::GateFacts const gate = {
                        true,
                        creature && creature->IsAlive(),
                        creature && creature->IsInWorld(),
                        true,
                        // Every exact party member passed the active/ambiguous combat gate above.
                        true,
                    };
                    std::vector<DungeonProgressionInteraction::CandidateFacts> interactionFacts;
                    std::vector<GameObject*> interactionObjects;
                    for (auto const& [spawnId, gameObject] : map->GetGameObjectBySpawnIdStore())
                    {
                        if (!gameObject || !gameObject->GetGOInfo())
                            continue;

                        uint32 const type = gameObject->GetGOInfo()->type;
                        interactionFacts.push_back({
                            static_cast<std::uint64_t>(spawnId),
                            gameObject->GetEntry(),
                            bot->GetExactDist2d(gameObject),
                            creature->GetExactDist2d(gameObject),
                            gameObject->IsInWorld(),
                            gameObject->isSpawned(),
                            gameObject->GetGoState() == GO_STATE_READY,
                            !gameObject->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE),
                            !gameObject->HasGameObjectFlag(GO_FLAG_IN_USE),
                            type == GAMEOBJECT_TYPE_GOOBER || type == GAMEOBJECT_TYPE_BUTTON,
                        });
                        interactionObjects.push_back(gameObject);
                    }

                    std::optional<std::size_t> const interaction =
                        DungeonProgressionInteraction::Select(gate, interactionFacts);
                    if (interaction)
                    {
                        GameObject* gameObject = interactionObjects[*interaction];
                        float const interactionDistance =
                            std::max(0.0f, gameObject->GetInteractionDistance() - 1.0f);
                        if (!bot->IsWithinDistInMap(gameObject, gameObject->GetInteractionDistance()))
                        {
                            bool const moved = MoveTo(gameObject, interactionDistance,
                                MovementPriority::MOVEMENT_NORMAL);
                            nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() :
                                BlockedScanBackoffMs);
                            LOG_INFO("playerbots",
                                "[DungeonNavigator] bot={} map={} encounter={} boss_spawn={} "
                                "progression_spawn={} progression_entry={} progression={}",
                                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                                gameObject->GetSpawnId(), gameObject->GetEntry(),
                                moved ? "move_to_interaction" : "movement_rejected");
                            return moved;
                        }

                        WorldPacket use(CMSG_GAMEOBJ_USE, 8);
                        use << gameObject->GetGUID();
                        bot->GetSession()->HandleGameObjectUseOpcode(use);
                        nextScanTime = now + SuccessfulMoveRescanDelayMs();
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} encounter={} boss_spawn={} "
                            "progression_spawn={} progression_entry={} progression=use",
                            bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                            gameObject->GetSpawnId(), gameObject->GetEntry());
                        return true;
                    }
                }

                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                    "activation_blocked={}",
                    bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                    ActivationDecisionName(decision));
                continue;
            }

            bool const attacked = Attack(creature);
            nextScanTime = now + (attacked ? SuccessfulMoveRescanDelayMs() :
                BlockedScanBackoffMs);
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} entry={} "
                "activation={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                creature->GetEntry(), attacked ? "attack" : "attack_action_rejected");
            return attacked;
        }

        nextScanTime = now + BlockedScanBackoffMs;
        return false;
    }

    bool const preserveCachedRoute = travelRouteInitialized && !travelRouteBlocked &&
        !travelRoute.empty() && travelRouteEncounterId == selection.selected.encounterId &&
        travelRouteSpawnId == goal.spawnId && travelRouteMapId == map->GetId() &&
        travelRouteInstanceId == map->GetInstanceId();
    if (preserveCachedRoute)
    {
        // A long route may briefly make the final spawn look directly reachable from an
        // intermediate landing. Keep consuming the proven route instead of oscillating between
        // direct and graph modes and losing the monotonic cursor.
        goal.travelNodes = true;
        goal.finalX = goal.x;
        goal.finalY = goal.y;
        goal.finalZ = goal.z;
    }
    if (goal.travelNodes)
    {
        bool const sameRoute = travelRouteInitialized &&
            travelRouteEncounterId == selection.selected.encounterId &&
            travelRouteSpawnId == goal.spawnId && travelRouteMapId == map->GetId() &&
            travelRouteInstanceId == map->GetInstanceId();
        if (!sameRoute)
        {
            ResetTravelRoute();
            travelRouteInitialized = true;
            travelRouteEncounterId = selection.selected.encounterId;
            travelRouteSpawnId = goal.spawnId;
            travelRouteMapId = map->GetId();
            travelRouteInstanceId = map->GetInstanceId();
        }

        bool const replanOnlyWalkRecovery = travelNoProgressReplans > 0;
        DungeonRouteReconnect::WalkRevalidationKey const walkRevalidationKey = {
            map->GetId(), map->GetInstanceId(), selection.selected.encounterId,
            goal.spawnId, travelNoProgressReplans,
        };
        bool const pendingWalkRevalidation = replanOnlyWalkRecovery &&
            DungeonRouteReconnect::IsWalkRevalidationPending(
                walkRevalidationState, walkRevalidationKey);

        if (!travelRouteBlocked && travelRoute.empty())
        {
            std::size_t const priorRoutePoints = replanOnlyWalkRecovery ?
                lastTravelWaypointIndex : 0;
            std::size_t const priorIndex = replanOnlyWalkRecovery ? travelRouteNextIndex : 0;
            if (replanOnlyWalkRecovery)
            {
                travelRouteNextIndex = 0;
                lastTravelWaypointIndex = 0;
            }

            WorldPosition start(bot);
            WorldPosition finish(map->GetId(), goal.finalX, goal.finalY, goal.finalZ);
            std::vector<PathNodePoint> route = FindDirectStoredWalkRoute(start, finish);
            bool completeStoredWalkRoute = !route.empty();
            char const* routeSource = completeStoredWalkRoute ? "direct_stored_walk" : nullptr;
            StoredWalkGraphRouteResult storedWalkGraphResult;
            if (route.empty())
            {
                storedWalkGraphResult = FindStoredWalkGraphRoute(start, finish, bot,
                    replanOnlyWalkRecovery);
                route = std::move(storedWalkGraphResult.route);
                if (!route.empty())
                {
                    completeStoredWalkRoute = true;
                    routeSource = storedWalkGraphResult.replanAttempted ?
                        "stored_walk_graph_replan" : "stored_walk_graph";
                }
            }
            if (replanOnlyWalkRecovery && storedWalkGraphResult.replanAttempted)
            {
                for (ReplanWalkCandidateEvidence const& candidate :
                    storedWalkGraphResult.replanCandidates)
                {
                    if (candidate.candidateNode)
                    {
                        LOG_INFO("playerbots",
                            "[DungeonNavigator] bot={} map={} encounter={} spawn={} replan=1 "
                            "prior_route_points={} prior_index={} candidate_source={} "
                            "candidate_node=({},{},{}) node_distance={} candidate_rank={} "
                            "candidate_limit={} attachment_safe={} attachment_reached={} "
                            "attachment_path_length={} all_edges_walk={} same_map={} "
                            "first_special_type={} first_special_index={} first_special_entry={} "
                            "first_special_map={} decision={}",
                            bot->GetName(), map->GetId(), selection.selected.encounterId,
                            goal.spawnId, priorRoutePoints, priorIndex,
                            "stored_walk_graph_replan", candidate.candidateNode->getX(),
                            candidate.candidateNode->getY(), candidate.candidateNode->getZ(),
                            candidate.nodeDistance, candidate.candidateRank,
                            candidate.candidateLimit, candidate.attachmentSafe,
                            candidate.attachmentReached, candidate.attachmentPathLength,
                            candidate.allEdgesWalk, candidate.sameMap,
                            candidate.firstSpecialType, candidate.firstSpecialIndex,
                            candidate.firstSpecialEntry, candidate.firstSpecialMap,
                            candidate.accepted ? "accepted" : "rejected");
                    }
                }
            }

            if (pendingWalkRevalidation && !route.empty())
            {
                bool const sameMapOrdinaryRoute = std::all_of(route.begin(), route.end(),
                    [&](PathNodePoint const& routePoint)
                    {
                        return routePoint.point.GetMapId() == map->GetId() &&
                            IsWalkPoint(routePoint.type);
                    });
                DungeonRouteReconnect::FreshOrdinaryWalkEvidence const evidence = {
                    true,
                    !route.empty(),
                    sameMapOrdinaryRoute,
                    completeStoredWalkRoute,
                };
                if (DungeonRouteReconnect::AcceptFreshOrdinaryWalkEvidence(
                        walkRevalidationState, walkRevalidationKey, evidence))
                {
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} instance={} encounter={} spawn={} "
                        "recovery=walk_revalidation_accepted revalidation=accepted "
                        "no_progress_epoch={} route_source={} route_points={} "
                        "evidence=fresh_ordinary_walk",
                        bot->GetName(), map->GetId(), map->GetInstanceId(),
                        selection.selected.encounterId, goal.spawnId,
                        travelNoProgressReplans, routeSource ? routeSource : "unknown",
                        route.size());
                }
                else
                {
                    route.clear();
                    completeStoredWalkRoute = false;
                    routeSource = nullptr;
                }
            }

            if (replanOnlyWalkRecovery && route.empty())
            {
                DungeonRouteReconnect::WalkRevalidationDecision const revalidationDecision =
                    DungeonRouteReconnect::ObserveWalkRevalidationNoProof(
                        walkRevalidationState, walkRevalidationKey);
                if (revalidationDecision ==
                    DungeonRouteReconnect::WalkRevalidationDecision::EnterPending)
                {
                    nextScanTime = now + BlockedScanBackoffMs;
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} instance={} encounter={} spawn={} "
                        "recovery=pending_revalidation revalidation=pending "
                        "no_progress_epoch={} prior_route_points={} prior_index={} "
                        "candidate_count={} candidate_limit={} delay_ms={} "
                        "decision=pending_revalidation",
                        bot->GetName(), map->GetId(), map->GetInstanceId(),
                        selection.selected.encounterId, goal.spawnId,
                        travelNoProgressReplans, priorRoutePoints, priorIndex,
                        storedWalkGraphResult.replanCandidates.size(),
                        TravelNodeReplanCandidateLimit, BlockedScanBackoffMs);
                }
                else if (revalidationDecision ==
                    DungeonRouteReconnect::WalkRevalidationDecision::EnterTerminal)
                {
                    BlockTravelRoute("no_proven_walk_recovery");
                    nextScanTime = now + BlockedScanBackoffMs;
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} instance={} encounter={} spawn={} "
                        "blocked=no_proven_walk_recovery revalidation=terminal "
                        "no_progress_epoch={} prior_route_points={} prior_index={} "
                        "candidate_count={} candidate_limit={} terminal_emit=once "
                        "decision=terminal_no_proven_walk_recovery",
                        bot->GetName(), map->GetId(), map->GetInstanceId(),
                        selection.selected.encounterId, goal.spawnId, travelNoProgressReplans,
                        priorRoutePoints, priorIndex,
                        storedWalkGraphResult.replanCandidates.size(),
                        TravelNodeReplanCandidateLimit);
                }
                return false;
            }
            if (route.empty())
            {
                // Prefer an actually calculated, grounded local corridor over opaque transition
                // metadata. A NORMAL|INCOMPLETE corridor is bounded progress only: its endpoint
                // becomes the temporary final cached point and the encounter is rescanned after
                // physical arrival. Closed doors therefore stop at the door; unproven elevators,
                // portals, and jumps remain rejected by DungeonPathSafety.
                AutoWowDungeonPath::ProbeResult const prepared = ProbeLeg(
                    bot, goal.finalX, goal.finalY, goal.finalZ);
                float endpointMovement = 0.0f;
                if (!prepared.path.empty())
                {
                    G3D::Vector3 const& endpoint = prepared.path.back();
                    endpointMovement = WorldPosition(bot).distance(
                        WorldPosition(map->GetId(), endpoint.x, endpoint.y, endpoint.z));
                }
                if (DungeonRouteReconnect::CanCachePreparedCorridor(prepared.safe,
                        prepared.path.size(), prepared.pathLength, endpointMovement,
                        TravelNoProgressDistance))
                {
                    route.reserve(prepared.path.size());
                    for (G3D::Vector3 const& point : prepared.path)
                    {
                        route.push_back({WorldPosition(map->GetId(), point.x, point.y, point.z),
                            NODE_PATH, 0});
                    }
                    routeSource = "prepared_corridor";
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                        "route_source=prepared_corridor mode={} route_points={} endpoint_movement={}",
                        bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                        prepared.mode, route.size(), endpointMovement);
                }
            }
            if (route.empty())
            {
                route = TravelNodeMap::getFullPath(start, finish, bot).getPath();
                if (!route.empty())
                    routeSource = "full_path";
            }
            if (route.empty())
            {
                BlockTravelRoute("no_route");
            }
            else
            {
                // A* can return a cheaper portal cycle before a final walking leg on the
                // current map.  V0 never executes that transition, but it may safely use
                // the final contiguous walking suffix when that suffix starts at the bot
                // (or at an independently validated reachable point).
                std::size_t walkingSuffix = 0;
                bool skippedTransition = false;
                std::size_t storedWalkAttachment = DungeonRouteReconnect::NoSelection;
                AutoWowDungeonPath::ProbeResult storedWalkAttachmentProbe;
                for (std::size_t index = 0; index < route.size(); ++index)
                {
                    PathNodePoint const& routePoint = route[index];
                    if (routePoint.point.GetMapId() != map->GetId() ||
                        !IsWalkPoint(routePoint.type))
                    {
                        walkingSuffix = index + 1;
                        skippedTransition = true;
                    }
                }

                char const* invalidReason = nullptr;
                if (walkingSuffix >= route.size())
                {
                    invalidReason = skippedTransition ? "unsupported_transition" : "no_walk_route";
                }
                else
                {
                    PathNodePoint const& first = route[walkingSuffix];
                    WorldPosition firstPosition(first.point);
                    if (first.point.GetMapId() != map->GetId() || !IsWalkPoint(first.type))
                    {
                        invalidReason = "unsupported_transition";
                    }
                    else if (WorldPosition(bot).distance(firstPosition) > TravelArrivalRadius)
                    {
                        AutoWowDungeonPath::ProbeResult const probe = ProbeLeg(
                            bot, first.point.GetPositionX(), first.point.GetPositionY(),
                            first.point.GetPositionZ());
                        if (!probe.safe || !ProbeReachedStoredDestination(probe))
                        {
                            std::size_t const scanEnd = std::min(route.size(),
                                walkingSuffix + static_cast<std::size_t>(
                                    TravelStoredWalkAttachmentPointLimit));
                            std::vector<DungeonRouteReconnect::StoredWalkAttachmentCandidate>
                                attachmentCandidates;
                            std::vector<AutoWowDungeonPath::ProbeResult> attachmentProbes;
                            attachmentCandidates.reserve(scanEnd - walkingSuffix);
                            attachmentProbes.reserve(scanEnd - walkingSuffix);
                            for (std::size_t index = walkingSuffix; index < scanEnd; ++index)
                            {
                                PathNodePoint const& candidatePoint = route[index];
                                bool const sameMapWalk =
                                    candidatePoint.point.GetMapId() == map->GetId() &&
                                    IsWalkPoint(candidatePoint.type);
                                AutoWowDungeonPath::ProbeResult candidateProbe;
                                if (sameMapWalk)
                                {
                                    candidateProbe = ProbeLeg(bot,
                                        candidatePoint.point.GetPositionX(),
                                        candidatePoint.point.GetPositionY(),
                                        candidatePoint.point.GetPositionZ());
                                }
                                attachmentCandidates.push_back({sameMapWalk,
                                    candidateProbe.safe,
                                    sameMapWalk && ProbeReachedStoredDestination(candidateProbe)});
                                attachmentProbes.push_back(std::move(candidateProbe));
                            }

                            std::size_t const relativeAttachment =
                                DungeonRouteReconnect::SelectEarliestStoredWalkAttachment(
                                    attachmentCandidates, completeStoredWalkRoute,
                                    skippedTransition, TravelStoredWalkAttachmentPointLimit);
                            if (relativeAttachment != DungeonRouteReconnect::NoSelection)
                            {
                                storedWalkAttachment = walkingSuffix + relativeAttachment;
                                storedWalkAttachmentProbe =
                                    std::move(attachmentProbes[relativeAttachment]);
                                LOG_INFO("playerbots",
                                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                                    "route_source={} recovery=stored_walk_prefix_attachment "
                                    "attachment_index={} skipped_prefix_points={} prepared_points={}",
                                    bot->GetName(), map->GetId(),
                                    selection.selected.encounterId, goal.spawnId,
                                    routeSource ? routeSource : "unknown", storedWalkAttachment,
                                    storedWalkAttachment - walkingSuffix,
                                    storedWalkAttachmentProbe.path.size());
                            }
                            else
                            {
                                invalidReason = skippedTransition ?
                                    "transition_suffix_unreachable" : "walk_prefix_unreachable";
                                LOG_INFO("playerbots",
                                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                                    "route_source={} diagnostic={} first_index={} probe={}",
                                    bot->GetName(), map->GetId(),
                                    selection.selected.encounterId, goal.spawnId,
                                    routeSource ? routeSource : "unknown", invalidReason,
                                    walkingSuffix, AutoWowDungeonPath::Json(probe, false));
                            }
                        }
                    }
                }

                if (invalidReason)
                {
                    // A transition-only graph can be a symptom of combat leaving the leader on a
                    // different collision layer from the rest of the party. Re-anchor the route to
                    // a lower-floor party member only when a strict party majority occupies that
                    // floor and an ordinary complete path reaches the member. No relocation occurs.
                    if (skippedTransition)
                    {
                        std::vector<DungeonRouteReconnect::PartyAnchorSample> memberSamples;
                        std::vector<float> nearbyHeights;
                        memberSamples.reserve(followers.size());
                        nearbyHeights.reserve(followers.size());
                        for (Player* member : followers)
                        {
                            bool const eligible = member && member->IsAlive() && member->IsInWorld() &&
                                member->GetMapId() == map->GetId() &&
                                member->GetInstanceId() == map->GetInstanceId();
                            float horizontalDistance = std::numeric_limits<float>::infinity();
                            float memberZ = std::numeric_limits<float>::infinity();
                            if (eligible)
                            {
                                float const dx = member->GetPositionX() - bot->GetPositionX();
                                float const dy = member->GetPositionY() - bot->GetPositionY();
                                horizontalDistance = std::sqrt(dx * dx + dy * dy);
                                memberZ = member->GetPositionZ();
                                if (std::isfinite(horizontalDistance) &&
                                    horizontalDistance <= PartyGroundAnchorRadius &&
                                    std::isfinite(memberZ))
                                {
                                    nearbyHeights.push_back(memberZ);
                                }
                            }
                            memberSamples.push_back({eligible, horizontalDistance, memberZ});
                        }

                        bool anchorReachable = false;
                        uint32 anchorGuid = 0;
                        float anchorZ = std::numeric_limits<float>::infinity();
                        float anchorPathLength = std::numeric_limits<float>::infinity();
                        AutoWowDungeonPath::ProbeResult anchorProbe;
                        if (nearbyHeights.size() >= PartyGroundMinimumSupport)
                        {
                            std::sort(nearbyHeights.begin(), nearbyHeights.end());
                            float const occupiedFloor = nearbyHeights[nearbyHeights.size() / 2];
                            for (Player* member : followers)
                            {
                                if (!member || std::fabs(member->GetPositionZ() - occupiedFloor) >
                                        PartyGroundAnchorVerticalTolerance)
                                {
                                    continue;
                                }
                                float const dx = member->GetPositionX() - bot->GetPositionX();
                                float const dy = member->GetPositionY() - bot->GetPositionY();
                                float const horizontalDistance = std::sqrt(dx * dx + dy * dy);
                                if (!std::isfinite(horizontalDistance) ||
                                    horizontalDistance > PartyGroundAnchorRadius)
                                {
                                    continue;
                                }

                                AutoWowDungeonPath::ProbeResult const probe =
                                    AutoWowDungeonPath::Probe(bot, member->GetPositionX(),
                                        member->GetPositionY(), member->GetPositionZ());
                                bool const reached = probe.safe &&
                                    ProbeReachedStoredDestination(probe);
                                float endpointMovement = 0.0f;
                                if (!probe.path.empty())
                                {
                                    G3D::Vector3 const& endpoint = probe.path.back();
                                    endpointMovement = WorldPosition(bot).distance(WorldPosition(
                                        map->GetId(), endpoint.x, endpoint.y, endpoint.z));
                                }
                                if (!reached || !DungeonRouteReconnect::CanCachePreparedCorridor(
                                        probe.safe, probe.path.size(), probe.pathLength,
                                        endpointMovement, TravelNoProgressDistance) ||
                                    probe.pathLength >= anchorPathLength)
                                {
                                    continue;
                                }

                                anchorReachable = true;
                                anchorGuid = member->GetGUID().GetCounter();
                                anchorZ = member->GetPositionZ();
                                anchorPathLength = probe.pathLength;
                                anchorProbe = probe;
                            }
                        }

                        DungeonRouteReconnect::PartyRouteReanchorFacts const facts = {
                            anchorReachable,
                            bot->GetPositionZ(),
                            anchorZ,
                            memberSamples,
                        };
                        DungeonRouteReconnect::PartyRouteReanchorPolicy const policy = {
                            PartyRouteReanchorMinimumDrop,
                            PartyRouteReanchorMaximumDrop,
                            PartyGroundAnchorRadius,
                            PartyGroundAnchorVerticalTolerance,
                            PartyGroundMinimumSupport,
                        };
                        DungeonRouteReconnect::PartyRouteReanchorDecision const decision =
                            DungeonRouteReconnect::EvaluatePartyRouteReanchor(facts, policy);
                        if (decision.allowed)
                        {
                            travelRoute.reserve(anchorProbe.path.size());
                            for (G3D::Vector3 const& point : anchorProbe.path)
                            {
                                travelRoute.push_back({map->GetId(), point.x, point.y, point.z});
                            }
                            LOG_INFO("playerbots",
                                "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                                "route_source=party_reanchor anchor_guid={} from_z={} anchor_z={} "
                                "route_points={} supporting_members={} eligible_members={}",
                                bot->GetName(), map->GetId(), selection.selected.encounterId,
                                goal.spawnId, anchorGuid, bot->GetPositionZ(), anchorZ,
                                travelRoute.size(), decision.supportingMembers,
                                decision.eligibleMembers);
                        }
                        else
                        {
                            BlockTravelRoute(invalidReason);
                        }
                    }
                    else
                    {
                        BlockTravelRoute(invalidReason);
                    }
                }
                else
                {
                    if (storedWalkAttachment != DungeonRouteReconnect::NoSelection)
                    {
                        travelRoute.reserve(storedWalkAttachmentProbe.path.size() +
                            route.size() - storedWalkAttachment - 1);
                        for (G3D::Vector3 const& point : storedWalkAttachmentProbe.path)
                            travelRoute.push_back({map->GetId(), point.x, point.y, point.z});
                    }
                    else
                    {
                        travelRoute.reserve(route.size() - walkingSuffix);
                    }

                    std::size_t const storedSuffixBegin =
                        storedWalkAttachment == DungeonRouteReconnect::NoSelection ?
                            walkingSuffix : storedWalkAttachment + 1;
                    for (std::size_t index = storedSuffixBegin; index < route.size(); ++index)
                    {
                        PathNodePoint const& routePoint = route[index];
                        travelRoute.push_back({routePoint.point.GetMapId(),
                            routePoint.point.GetPositionX(), routePoint.point.GetPositionY(),
                            routePoint.point.GetPositionZ()});
                    }
                }
            }
        }

        if (travelRouteBlocked)
        {
            nextScanTime = now + BlockedScanBackoffMs;
            if (!DungeonRouteReconnect::IsWalkRevalidationTerminal(
                    walkRevalidationState, walkRevalidationKey))
            {
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                    "route_source=travel_nodes blocked={}",
                    bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                    travelRouteBlockedReason);
            }
            return false;
        }

        WorldPosition botPosition(bot);
        bool const reachedPendingWaypoint =
            lastTravelEncounterId == selection.selected.encounterId &&
            lastTravelSpawnId == goal.spawnId &&
            lastTravelWaypointIndex >= travelRouteNextIndex &&
            lastTravelWaypointIndex < travelRoute.size() &&
            botPosition.distance(WorldPosition(map->GetId(), lastTravelWaypointX,
                lastTravelWaypointY, lastTravelWaypointZ)) <= TravelArrivalRadius;
        if (reachedPendingWaypoint)
            travelRouteNextIndex = lastTravelWaypointIndex;

        while (travelRouteNextIndex < travelRoute.size())
        {
            DungeonNavigatorRoutePoint const& point = travelRoute[travelRouteNextIndex];
            WorldPosition routePosition(point.mapId, point.x, point.y, point.z);
            if (botPosition.distance(routePosition) > TravelArrivalRadius)
                break;
            ++travelRouteNextIndex;
        }

        DungeonNavigatorRoutePoint const* finalRoutePoint =
            travelRoute.empty() ? nullptr : &travelRoute.back();
        DungeonRouteReconnect::ExhaustedRouteDecision const exhaustedDecision =
            DungeonRouteReconnect::EvaluateExhaustedRoute(
                travelRouteNextIndex,
                travelRoute.size(),
                finalRoutePoint && finalRoutePoint->mapId == map->GetId(),
                finalRoutePoint ? botPosition.distance(WorldPosition(finalRoutePoint->mapId,
                    finalRoutePoint->x, finalRoutePoint->y, finalRoutePoint->z)) :
                    std::numeric_limits<float>::infinity(),
                TravelArrivalRadius);
        if (exhaustedDecision ==
            DungeonRouteReconnect::ExhaustedRouteDecision::ReleaseForTerminalActivation)
        {
            std::size_t const completedPointCount = travelRoute.size();
            ResetTravelRoute();
            nextScanTime = now + SuccessfulMoveRescanDelayMs();
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_points={} "
                "route_source=travel_nodes completion=terminal_activation_rescan",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                completedPointCount);
            return false;
        }
        if (exhaustedDecision == DungeonRouteReconnect::ExhaustedRouteDecision::Block)
        {
            nextScanTime = now + BlockedScanBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                "route_points={} route_source=travel_nodes blocked=route_exhausted_unconfirmed",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                travelRouteNextIndex, travelRoute.size());
            return false;
        }

        WorldPosition finish(map->GetId(), goal.finalX, goal.finalY, goal.finalZ);
        bool safeFrontier = false;
        bool partialFrontier = false;
        char const* frontierBlockedReason = "no_forward_point";
        std::size_t selectedRouteIndex = travelRouteNextIndex;
        float partialFrontierX = 0.0f;
        float partialFrontierY = 0.0f;
        float partialFrontierZ = 0.0f;
        bool preparedCompletePrefix = false;
        std::vector<DungeonRouteReconnect::Candidate> reconnectCandidates;
        std::vector<AutoWowDungeonPath::ProbeResult> reconnectProbes;
        AutoWowDungeonPath::ProbeResult selectedPreparedProbe;
        bool hasSelectedPreparedProbe = false;

        auto selectForwardPoint = [&](std::size_t startIndex)
        {
            reconnectCandidates.clear();
            reconnectProbes.clear();
            partialFrontier = false;
            preparedCompletePrefix = false;
            hasSelectedPreparedProbe = false;
            uint32 examined = 0;
            for (std::size_t index = startIndex;
                 index < travelRoute.size() && examined < TravelLookaheadPointLimit;
                 ++index, ++examined)
            {
                DungeonNavigatorRoutePoint const& point = travelRoute[index];
                AutoWowDungeonPath::ProbeResult const probe =
                    ProbeLeg(bot, point.x, point.y, point.z);
                bool const destinationReached = ProbeReachedStoredDestination(probe);
                reconnectCandidates.push_back(
                    {probe.safe, destinationReached, probe.pathLength});
                reconnectProbes.push_back(probe);

                if (!partialFrontier && probe.safe && destinationReached &&
                    probe.path.size() >= 2 && probe.pathLength > TravelLookaheadDistance)
                {
                    std::vector<float> cumulativeDistances(probe.path.size(), 0.0f);
                    for (std::size_t pathIndex = 1; pathIndex < probe.path.size(); ++pathIndex)
                    {
                        G3D::Vector3 const& previous = probe.path[pathIndex - 1];
                        G3D::Vector3 const& current = probe.path[pathIndex];
                        float const deltaX = current.x - previous.x;
                        float const deltaY = current.y - previous.y;
                        float const deltaZ = current.z - previous.z;
                        cumulativeDistances[pathIndex] = cumulativeDistances[pathIndex - 1] +
                            std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                    }

                    std::size_t const prefixIndex =
                        DungeonRouteReconnect::SelectPreparedPathPrefix(cumulativeDistances,
                            TravelNoProgressDistance, TravelLookaheadDistance);
                    if (prefixIndex != DungeonRouteReconnect::NoSelection)
                    {
                        AutoWowDungeonPath::ProbeResult prefixProbe = probe;
                        prefixProbe.path.resize(prefixIndex + 1);
                        G3D::Vector3 const& endpoint = prefixProbe.path.back();
                        prefixProbe.destinationX = endpoint.x;
                        prefixProbe.destinationY = endpoint.y;
                        prefixProbe.destinationZ = endpoint.z;
                        prefixProbe.endpointDistance = 0.0f;
                        prefixProbe.pathLength = cumulativeDistances[prefixIndex];

                        partialFrontier = true;
                        preparedCompletePrefix = true;
                        selectedRouteIndex = index;
                        partialFrontierX = endpoint.x;
                        partialFrontierY = endpoint.y;
                        partialFrontierZ = endpoint.z;
                        selectedPreparedProbe = std::move(prefixProbe);
                        hasSelectedPreparedProbe = true;
                    }
                }

                if (!partialFrontier && !probe.path.empty())
                {
                    G3D::Vector3 const& endpoint = probe.path.back();
                    float const endpointMovement = WorldPosition(bot).distance(
                        WorldPosition(map->GetId(), endpoint.x, endpoint.y, endpoint.z));
                    if (DungeonRouteReconnect::CanUsePartialProgress(probe.safe,
                            destinationReached, probe.pathLength, endpointMovement,
                            TravelNoProgressDistance,
                            TravelLookaheadDistance + TravelPartialPathTolerance))
                    {
                        partialFrontier = true;
                        selectedRouteIndex = index;
                        partialFrontierX = endpoint.x;
                        partialFrontierY = endpoint.y;
                        partialFrontierZ = endpoint.z;
                        selectedPreparedProbe = probe;
                        hasSelectedPreparedProbe = true;
                    }
                }
            }

            std::size_t const reconnectIndex = DungeonRouteReconnect::SelectFarthestReachable(
                reconnectCandidates, TravelLookaheadDistance);
            if (reconnectIndex == DungeonRouteReconnect::NoSelection)
                return false;
            selectedRouteIndex = startIndex + reconnectIndex;
            selectedPreparedProbe = reconnectProbes[reconnectIndex];
            hasSelectedPreparedProbe = true;
            return true;
        };

        safeFrontier = selectForwardPoint(travelRouteNextIndex);
        if (!safeFrontier)
        {
            std::vector<DungeonRouteReconnect::StoredPoint> storedPoints(travelRoute.size());
            std::size_t const scanEnd = std::min(travelRouteNextIndex, travelRoute.size());
            std::size_t const scanBegin = scanEnd > TravelLookaheadPointLimit ?
                scanEnd - TravelLookaheadPointLimit : 0;
            for (std::size_t index = scanBegin; index < scanEnd; ++index)
            {
                DungeonNavigatorRoutePoint const& point = travelRoute[index];
                storedPoints[index] = {
                    point.mapId == map->GetId(),
                    botPosition.distance(WorldPosition(point.mapId, point.x, point.y, point.z)),
                };
            }

            std::size_t const anchor = DungeonRouteReconnect::SelectBackwardAnchor(
                storedPoints, travelRouteNextIndex, TravelLookaheadPointLimit, TravelArrivalRadius);
            if (anchor != DungeonRouteReconnect::NoSelection &&
                DungeonNavigatorConvoy::AcceptsLeaderBackwardAnchor(ConvoyV2Enabled(), anchor,
                    travelRouteNextIndex))
            {
                std::size_t const overshotIndex = travelRouteNextIndex;
                travelRouteNextIndex = anchor;
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} "
                    "route_source=travel_nodes recovery=backward_reanchor from_index={} to_index={}",
                    bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                    overshotIndex, anchor);
                safeFrontier = selectForwardPoint(travelRouteNextIndex);
            }
        }

        if (safeFrontier)
        {
            DungeonNavigatorRoutePoint const& point = travelRoute[selectedRouteIndex];
            WorldPosition routePosition(point.mapId, point.x, point.y, point.z);
            goal.x = point.x;
            goal.y = point.y;
            goal.z = point.z;
            goal.pathLength = routePosition.distance(finish);
            frontierBlockedReason = "none";
        }
        else if (partialFrontier)
        {
            goal.x = partialFrontierX;
            goal.y = partialFrontierY;
            goal.z = partialFrontierZ;
            goal.pathLength = WorldPosition(map->GetId(), goal.x, goal.y, goal.z).distance(finish);
            frontierBlockedReason = "none";
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                "route_points={} route_source=travel_nodes recovery={} "
                "endpoint_x={} endpoint_y={} endpoint_z={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                selectedRouteIndex, travelRoute.size(),
                preparedCompletePrefix ? "prepared_complete_prefix" : "partial_progress",
                goal.x, goal.y, goal.z);
        }
        else if (!replanOnlyWalkRecovery && travelRouteNextIndex < travelRoute.size())
        {
            // Combat movement can leave a clientless bot a fraction below otherwise valid WMO
            // ground. From there even an adjacent stored point reports NOPATH and the party cannot
            // recover physically. Correct only Z at the current XY, by at most two yards, and only
            // when the next ordinary route point is independently reachable from that corrected
            // height. The cursor is unchanged and no dungeon content is skipped.
            DungeonNavigatorRoutePoint const& nextPoint = travelRoute[travelRouteNextIndex];
            float const ground = map->GetHeight(bot->GetPhaseMask(), bot->GetPositionX(),
                bot->GetPositionY(), bot->GetPositionZ() + GroundReattachMaximum, true,
                GroundReattachMaximum + 1.0f);
            bool const groundValid = ground > INVALID_HEIGHT && std::isfinite(ground);
            float const correction = groundValid ? ground - bot->GetPositionZ() :
                std::numeric_limits<float>::infinity();
            bool continuationReached = false;
            if (groundValid && nextPoint.mapId == map->GetId())
            {
                AutoWowDungeonPath::ProbeResult const continuation = AutoWowDungeonPath::ProbeFrom(
                    bot, bot->GetPositionX(), bot->GetPositionY(), ground,
                    nextPoint.x, nextPoint.y, nextPoint.z);
                continuationReached = continuation.safe &&
                    ProbeReachedStoredDestination(continuation);
            }

            if (DungeonRouteReconnect::CanGroundReattach(groundValid, correction,
                    continuationReached, GroundReattachMinimum, GroundReattachMaximum))
            {
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "dungeon_ground_reattach");
                bot->NearTeleportTo(bot->GetPositionX(), bot->GetPositionY(),
                    ground + GroundReattachOffset, bot->GetOrientation());
                nextScanTime = now + SuccessfulMoveRescanDelayMs();
                LOG_INFO("playerbots",
                    "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                    "route_points={} route_source=travel_nodes recovery=ground_reattach "
                    "from_z={} to_z={} correction={}",
                    bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                    travelRouteNextIndex, travelRoute.size(), ground - correction,
                    ground + GroundReattachOffset, correction);
                return true;
            }

            // ConvoyV2: a short navmesh gap (NOPATH) to the immediate next point is walked along the
            // reversed safe probe from that point back to the leader. Nothing is skipped; each hop gets
            // DirectHopAttemptLimit tries before the ordinary block.
            if (ConvoyV2Enabled() && nextPoint.mapId == map->GetId() && !reconnectProbes.empty())
            {
                if (directHopRouteIndex != travelRouteNextIndex)
                {
                    directHopRouteIndex = travelRouteNextIndex;
                    directHopAttempts = 0;
                }
                AutoWowDungeonPath::ProbeResult const& forward = reconnectProbes.front();
                float const hopDx = nextPoint.x - bot->GetPositionX();
                float const hopDy = nextPoint.y - bot->GetPositionY();
                float const hopHorizontal = std::sqrt(hopDx * hopDx + hopDy * hopDy);
                float const hopVertical = nextPoint.z - bot->GetPositionZ();
                bool const forwardNoPath = !forward.safe && (forward.pathType & PATHFIND_NOPATH) != 0;
                bool reverseSafe = false;
                AutoWowDungeonPath::ProbeResult reverse;
                if (DungeonNavigatorConvoy::CanDirectHop(true, forwardNoPath, hopHorizontal, hopVertical,
                        true, DirectHopMaximumHorizontal, DirectHopMaximumVertical, directHopAttempts,
                        DirectHopAttemptLimit))
                {
                    reverse = AutoWowDungeonPath::ProbeFrom(bot, nextPoint.x, nextPoint.y, nextPoint.z,
                        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                    reverseSafe = reverse.safe && ProbeReachedStoredDestination(reverse) &&
                        reverse.path.size() >= 2;
                }
                if (DungeonNavigatorConvoy::CanDirectHop(true, forwardNoPath, hopHorizontal, hopVertical,
                        reverseSafe, DirectHopMaximumHorizontal, DirectHopMaximumVertical,
                        directHopAttempts, DirectHopAttemptLimit))
                {
                    ++directHopAttempts;
                    std::reverse(reverse.path.begin(), reverse.path.end());
                    reverse.destinationX = nextPoint.x;
                    reverse.destinationY = nextPoint.y;
                    reverse.destinationZ = nextPoint.z;
                    AutoWowDungeonWalkAction walk(botAI);
                    bool const moved = walk.WalkPrepared(reverse);
                    nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() : BlockedScanBackoffMs);
                    LOG_INFO("playerbots",
                        "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                        "route_points={} route_source=travel_nodes recovery=direct_hop attempt={} "
                        "horizontal={} vertical={} reverse_mode={} moved={} probe={}",
                        bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                        travelRouteNextIndex, travelRoute.size(), directHopAttempts, hopHorizontal,
                        hopVertical, reverse.mode, moved, AutoWowDungeonPath::Json(forward, false));
                    if (moved)
                        return true;
                }
            }

            if (!reconnectCandidates.empty())
                frontierBlockedReason = "no_reachable_waypoint";
        }
        else if (!reconnectCandidates.empty())
        {
            frontierBlockedReason = "no_reachable_waypoint";
        }

        if (!safeFrontier && !partialFrontier)
        {
            nextScanTime = now + BlockedScanBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                "route_points={} route_source=travel_nodes blocked={} probe={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                travelRouteNextIndex, travelRoute.size(), frontierBlockedReason,
                reconnectProbes.empty() ? std::string("{}") :
                    AutoWowDungeonPath::Json(reconnectProbes.front(), false));
            return false;
        }

        // The selected point is pending only. The cursor advances on a later scan after physical
        // arrival, never merely because a probe requested a farther stored route point.
        goal.waypointIndex = selectedRouteIndex;

        float const waypointDx = goal.x - lastTravelWaypointX;
        float const waypointDy = goal.y - lastTravelWaypointY;
        float const waypointDz = goal.z - lastTravelWaypointZ;
        bool const sameWaypoint = lastTravelEncounterId == selection.selected.encounterId &&
            lastTravelSpawnId == goal.spawnId && lastTravelWaypointIndex == goal.waypointIndex &&
            waypointDx * waypointDx + waypointDy * waypointDy + waypointDz * waypointDz < 1.0f;
        float const botDx = bot->GetPositionX() - lastTravelBotX;
        float const botDy = bot->GetPositionY() - lastTravelBotY;
        float const botDz = bot->GetPositionZ() - lastTravelBotZ;
        float const noProgressDistanceSq = TravelNoProgressDistance * TravelNoProgressDistance;

        if (sameWaypoint &&
            botDx * botDx + botDy * botDy + botDz * botDz < noProgressDistanceSq)
        {
            if (travelNoProgressRetries < TravelNoProgressRetryLimit)
                ++travelNoProgressRetries;
        }
        else
        {
            travelNoProgressRetries = 0;
        }

        lastTravelEncounterId = selection.selected.encounterId;
        lastTravelSpawnId = goal.spawnId;
        lastTravelWaypointIndex = goal.waypointIndex;
        lastTravelWaypointX = goal.x;
        lastTravelWaypointY = goal.y;
        lastTravelWaypointZ = goal.z;
        lastTravelBotX = bot->GetPositionX();
        lastTravelBotY = bot->GetPositionY();
        lastTravelBotZ = bot->GetPositionZ();

        DungeonRouteReconnect::NoProgressDecision const noProgressDecision =
            DungeonRouteReconnect::EvaluateNoProgress(travelNoProgressRetries,
                TravelNoProgressRetryLimit, travelNoProgressReplans,
                TravelNoProgressReplanLimit);
        if (noProgressDecision == DungeonRouteReconnect::NoProgressDecision::Replan)
        {
            std::size_t const routePointCount = travelRoute.size();
            ++travelNoProgressReplans;
            ReplanTravelRoute();
            nextScanTime = now + BlockedScanBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} route_index={} "
                "route_points={} route_source=travel_nodes recovery=no_progress_replan "
                "retries={} replans={} probe={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                goal.waypointIndex, routePointCount, TravelNoProgressRetryLimit,
                travelNoProgressReplans, hasSelectedPreparedProbe ?
                    AutoWowDungeonPath::Json(selectedPreparedProbe, false) : std::string("{}"));
            return false;
        }
        if (noProgressDecision == DungeonRouteReconnect::NoProgressDecision::Block)
        {
            std::size_t const routePointCount = travelRoute.size();
            nextScanTime = now + BlockedScanBackoffMs;
            LOG_INFO("playerbots",
                "[DungeonNavigator] bot={} map={} encounter={} spawn={} final_x={} final_y={} "
                "final_z={} route_index={} route_points={} waypoint_x={} waypoint_y={} waypoint_z={} "
                "remaining_distance={} route_source=travel_nodes blocked=no_progress retries={} "
                "probe={}",
                bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
                goal.finalX, goal.finalY, goal.finalZ, goal.waypointIndex, routePointCount,
                goal.x, goal.y, goal.z, goal.pathLength, travelNoProgressRetries,
                hasSelectedPreparedProbe ?
                    AutoWowDungeonPath::Json(selectedPreparedProbe, false) : std::string("{}"));
            BlockTravelRoute("no_progress");
            return false;
        }

        LOG_INFO("playerbots",
            "[DungeonNavigator] bot={} map={} encounter={} spawn={} final_x={} final_y={} "
            "final_z={} route_index={} route_points={} waypoint_x={} waypoint_y={} waypoint_z={} "
            "remaining_distance={} route_source=travel_nodes selection=next_encounter",
            bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId,
            goal.finalX, goal.finalY, goal.finalZ, goal.waypointIndex, travelRoute.size(),
            goal.x, goal.y, goal.z, goal.pathLength);
        AutoWowDungeonWalkAction walk(botAI);
        bool const moved = hasSelectedPreparedProbe ? walk.WalkPrepared(selectedPreparedProbe) :
            MoveTo(map->GetId(), goal.x, goal.y, goal.z, false, false, false, true,
                MovementPriority::MOVEMENT_NORMAL);
        nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() :
            BlockedScanBackoffMs);
        return moved;
    }

    ResetTravelRoute();
    LOG_INFO("playerbots",
        "[DungeonNavigator] bot={} map={} encounter={} spawn={} path_length={} selection=next_encounter",
        bot->GetName(), map->GetId(), selection.selected.encounterId, goal.spawnId, goal.pathLength);
    bool const moved = MoveTo(map->GetId(), goal.x, goal.y, goal.z, false, false, false, true,
        MovementPriority::MOVEMENT_NORMAL);
    nextScanTime = now + (moved ? SuccessfulMoveRescanDelayMs() :
        BlockedScanBackoffMs);
    return moved;
}
