#ifndef PLAYERBOTS_NEWRPGACTION_H
#define PLAYERBOTS_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "QuestObjectiveContext.h"
#include "QuestPartyCohesionPolicy.h"
#include "QuestSourceRotationPolicy.h"
#include "TravelMgr.h"

namespace AutoWowQuestFinisher
{
struct StableSpawnIdentity
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 entry = 0;
    uint32 spawnId = 0;
    bool gameObject = false;
};

// AzerothCore assigns a new map-local runtime GUID when it loads a DB spawn. Its low counter
// need not equal the DB spawn ID, so identity uses the spawn fields and only requires a live GUID.
inline bool MatchesStableSpawn(StableSpawnIdentity const& expected,
                               StableSpawnIdentity const& live, uint64 runtimeGuid)
{
    return runtimeGuid != 0 && expected.mapId == live.mapId &&
        expected.instanceId == live.instanceId && expected.entry == live.entry &&
        expected.spawnId != 0 && expected.spawnId == live.spawnId &&
        expected.gameObject == live.gameObject;
}

// Pure admission facts for the exact stable finisher walk target. Runtime lookup supplies these
// facts from the loaded map spawn store; the predicate keeps identity and world-instance gates
// separate from movement and reward handling.
struct LoadedStableTargetFacts
{
    bool present = false;
    bool inWorld = false;
    bool usable = false;
    bool sameMap = false;
    bool sameInstance = false;
    bool exactEntry = false;
    bool exactSpawn = false;
    bool runtimeGuidPresent = false;
    bool exactFamily = false;
};

inline bool IsExactLoadedStableTarget(LoadedStableTargetFacts const& facts)
{
    return facts.present && facts.inWorld && facts.usable && facts.sameMap && facts.sameInstance &&
           facts.exactEntry && facts.exactSpawn && facts.runtimeGuidPresent && facts.exactFamily;
}
}

class TellRpgStatusAction : public Action
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : Action(botAI, "rpg status") {}

    bool Execute(Event event) override;
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_GO_CAMP] = 15;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    // static NewRpgStatusTransitionProb transitionMat;
    const int32 statusWanderNpcDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusWanderRandomDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusRestDuration = 30 * IN_MILLISECONDS ;
    const int32 statusDoQuestDuration = 30 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusOutDoorPvPDuration = HOUR * IN_MILLISECONDS ;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};

class NewRpgGoCampAction : public NewRpgBaseAction
{
public:
    NewRpgGoCampAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go camp") {}
    bool Execute(Event event) override;
};

class NewRpgWanderRandomAction : public NewRpgBaseAction
{
public:
    NewRpgWanderRandomAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wander random") {}
    bool Execute(Event event) override;
};

class NewRpgWanderNpcAction : public NewRpgBaseAction
{
public:
    NewRpgWanderNpcAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg move npcs") {}
    bool Execute(Event event) override;

    const uint32 npcStayTime = 8 * 1000;
};

class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    // Objective-execution driver (incomplete quest) and finisher driver
    // (completed quest). Both walk the QuestActionPhase state machine that
    // lives in DoQuest::objectiveRuntime.
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);

    /* PHASE-MACHINE HELPERS (Phase 1: creature-kill + creature-item + exact turn-in) */
    // Advance to a new phase and reset the per-phase dwell timer.
    void EnterQuestPhase(NewRpgInfo::DoQuest& data, QuestActionPhase phase);
    // Record a typed blocker for the external Director. `unsupported` marks the
    // quest genuinely un-runnable by this Phase-1 executor (deprioritized via
    // lowPriorityQuest + returned to Idle); otherwise the bot holds in the
    // Blocked phase so the Director can observe objectiveRuntime.failure and own
    // the abandon decision. Never increments the questAbandoned statistic.
    bool BlockQuest(NewRpgInfo::DoQuest& data, QuestFailureReason reason, bool unsupported);
    // AutoWow.QuestFullBagRelief.Enable: bounded escalation for an incomplete collect-item quest
    // stalled on 100% bag occupancy (vendor relief -> destroy safe junk -> defer with backoff).
    // See QuestInventoryReliefPolicy. Never reached when the flag is off.
    bool RelieveFullBagsForQuest(NewRpgInfo::DoQuest& data);
    // Starter-stall recovery (QuestStallRecoveryPolicy); never reached when the flags are off.
    // AutoWow.QuestBlockedDefer.Enable: defer a non-Oracle quest held in Blocked and return to Idle.
    bool DeferBlockedQuest(NewRpgInfo::DoQuest& data);
    // AutoWow.QuestTravelProgressWatch.Enable: observe net approach to keyPos; true when the budget
    // expired without a best-distance improvement.
    bool TravelProgressExpired(QuestObjectiveRuntime& rt, WorldPosition const& keyPos, float distance);
    // Objective-side expiry: rotate away from the unreachable source spawn (bounded), else block with
    // `blockReason` (AutoWow.Travel.VerticalSnap passes the travel intent's give-up reason).
    bool ExpireUnreachableSource(NewRpgInfo::DoQuest& data, QuestObjectiveSpec const& spec,
                                 QuestFailureReason blockReason = QuestFailureReason::TravelNoProgress);
    // Keep a quest participant with its leader during ordinary non-combat travel. Combat, loot,
    // scripted interactions, and corpse recovery remain independent so the cohesion rule cannot
    // suppress legitimate work or rescue behavior.
    bool MaintainQuestPartyCohesion(NewRpgInfo::DoQuest const& data, bool objectiveWorkWindow);
    // Live, delta-capable objective counters (combined index scheme:
    // [0, QUEST_OBJECTIVES_COUNT) = creature/GO kills, remainder = required items).
    int32 ObjectiveCurrentCount(uint32 questId, int32 objectiveIdx) const;
    int32 ObjectiveRequiredCount(Quest const* quest, int32 objectiveIdx) const;
    // Walk/path-only travel target for the active objective. Prefer the nearest
    // exact resolved source spawn; ordinary objectives may fall back to quest-POI data when no
    // stable spawn is available. Spell-focus item objectives require an exact focus spawn and fail
    // closed instead of using a POI hint. Never teleports.
    bool ResolveSourceTravelPos(QuestObjectiveSpec const& spec, uint32 questId, int32 objectiveIdx,
                                QuestObjectiveRuntime& runtime, WorldPosition& out);
    // Bind the exact loaded gameobject selected by ResolveSourceTravelPos. The
    // stable spawn wins; the nearest same-entry object is only a loaded-grid
    // fallback. This never broadens the objective whitelist.
    bool BindSourceGameObject(QuestObjectiveRuntime& rt, float range);
    // Bind a live exact creature for a non-offensive quest-item CAST objective.
    // Cooldown entries prevent repeatedly using an already-processed target.
    bool BindQuestItemTarget(QuestObjectiveSpec const& spec, QuestObjectiveRuntime& rt, float range);
    // Bind the exact scripted quest starter while its native escort/event AI is
    // moving it. The stable spawn GUID remains authoritative after it leaves
    // its database coordinates.
    bool BindScriptedEventTarget(QuestObjectiveSpec const& spec, QuestObjectiveRuntime& rt, float range);
    // Bind the runtime finisher GUID only to the stable spawn selected by the finisher Value.
    // A same-entry nearby object is never an acceptable substitute for an exact finisher.
    bool BindFinisher(QuestObjectiveRuntime& rt, float range, WorldObject** outObject = nullptr);

    // Drive one OracleTagged T2 route tick.  The exact stable source/finisher descriptor is enough
    // to start; live identity is required only by ExactLiveBind/Verify.  This helper never selects
    // an ordinary RPG fallback and never services self-defense.
    bool DriveOracleQuestRoute(NewRpgInfo::DoQuest& data, QuestObjectiveSpec const* objective,
                               int32 objectiveIdx, bool finisher);
    WorldObject* BindExactOracleRouteTarget(QuestObjectiveRuntime& runtime, bool finisher);

    const uint32 poiStayTime = 5 * 60 * 1000;
    // Distance under which travel phases count the destination as reached.
    const float questTravelArriveDist = 10.0f;
    // Time budget spent working a single source (acquire/engage/loot) before
    // re-resolving the objective or blocking on no progress.
    const uint32 questSourceBudgetMs = 5 * 60 * 1000;
    // Time budget to acquire a target at a reached source before re-resolving.
    const uint32 questAcquireBudgetMs = 60 * 1000;
    // Allow the core to process a normal gameobject-use packet and update the
    // authoritative quest counter before retrying.
    const uint32 questInteractionVerifyMs = 3 * 1000;
    // Scripted escorts are deliberately allowed longer than a normal source
    // interaction; the native route owns the actual completion/failure.
    const uint32 questEscortBudgetMs = 15 * MINUTE * IN_MILLISECONDS;
    // Re-resolve attempts before a no-progress / no-target blocker fires.
    const uint32 maxObjectiveAttempts = 3;
    // Hard bound for deterministic source rotation. The resolved source list is finite; this
    // ceiling prevents malformed or duplicate spawn data from producing an infinite walk loop.
    const uint32 maxSourceRotations = 16;
    // Turn-in retries before a reward-not-confirmed blocker fires.
    const uint32 maxRewardAttempts = 5;
};

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;
};

#endif
