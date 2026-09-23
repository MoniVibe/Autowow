/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUEST_OBJECTIVE_CONTEXT_H
#define PLAYERBOTS_QUEST_OBJECTIVE_CONTEXT_H

// Phase 1 objective-lock vocabulary (types only; no behavior).
//
// Ownership model (see PHASE1_OBJECTIVE_LOCK_DESIGN.md):
//   - QuestObjectiveSpec    : authoritative facts derived from Quest / QuestStatusData / relation
//                             and item-drop maps. Refreshed whenever quest progress changes.
//   - QuestObjectiveRuntime : mutable execution state, owned by NewRpgInfo::DoQuest.
//   - "active quest objective" Value publishes a read-only snapshot to targeting/loot/travel/bridge.
//
// The canonical objective identity is (questId, family, slot) — never the flattened POI index,
// because NPC/GO slot 0 and item slot 0 are distinct objectives.

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjectGuid.h"
#include "OracleRouteExecutor.h"
#include "QuestStallRecoveryPolicy.h"
#include "TravelMgr.h"  // GuidPosition

enum class QuestObjectiveFamily : uint8
{
    NpcOrGameObject,
    Item
};

enum class QuestObjectiveKind : uint8
{
    CreatureCredit,
    GameObjectCredit,
    CollectItem,
    UseQuestItem,
    ScriptedEvent,
    Gathering,
    Unsupported
};

// AzerothCore sets QUEST_SPECIAL_FLAGS_CAST together with KILL and SPEAKTO for every
// RequiredNpcOrGo objective. A cast flag alone therefore does not identify an item-use quest.
// Requiring a real quest source item keeps ordinary kill-credit quests offensive while preserving
// the dedicated quest-item interaction path for quests such as Lazy Peons (q5441).
[[nodiscard]] inline bool UsesQuestSourceItemForCredit(bool hasAggregateCastFlag, uint32 questSourceItemId)
{
    return hasAggregateCastFlag && questSourceItemId != 0;
}

struct QuestObjectiveKey
{
    uint32 questId = 0;
    uint8 slot = 0;
    QuestObjectiveFamily family = QuestObjectiveFamily::NpcOrGameObject;
};

struct QuestObjectiveSource
{
    enum class Type : uint8 { Creature, GameObject };

    Type type = Type::Creature;
    uint32 entry = 0;
    std::vector<GuidPosition> spawns;
};

struct PartyObjectiveMemberState
{
    ObjectGuid memberGuid;
    bool ownsQuest = false;
    bool needsObjective = false;
    bool inCreditRange = false;
    bool lootEligible = false;
    uint32 currentCount = 0;
    uint32 requiredCount = 0;
};

struct QuestObjectiveSpec
{
    QuestObjectiveKey key;
    QuestObjectiveKind kind = QuestObjectiveKind::Unsupported;

    int32 requiredNpcOrGoEntry = 0;  // signed: >0 creature, <0 gameobject
    uint32 requiredItemId = 0;
    uint32 questItemId = 0;          // source item used on a CAST objective (for example q5441)

    uint32 currentCount = 0;
    uint32 requiredCount = 0;

    std::vector<QuestObjectiveSource> sources;

    ObjectGuid ownerGuid;
    ObjectGuid partyLeaderGuid;
    uint64 partyCohortId = 0;
    std::vector<PartyObjectiveMemberState> partyMembers;

    bool supported = false;

    // --- Consumer-facing API (fixed contract for targeting / loot) --------------------------------
    // A spec is "locked" (drives strict targeting/loot) only when it identifies a real, supported,
    // still-incomplete objective.
    [[nodiscard]] bool hasLock() const
    {
        return supported && key.questId != 0 && currentCount < requiredCount;
    }

    // Required quest loot must not be discarded by generic inventory-reserve policies while the
    // exact collect objective is still active. This does not bypass real inventory capacity: the
    // normal CMSG_AUTOSTORE_LOOT_ITEM path remains authoritative for whether the item can be stored.
    [[nodiscard]] bool isPendingRequiredItem(uint32 itemId) const
    {
        return hasLock() && kind == QuestObjectiveKind::CollectItem && requiredItemId != 0 &&
               requiredItemId == itemId;
    }

    // Exact offensive/loot whitelist: an entry is valid only if it is one of this objective's
    // resolved sources of the matching type.
    [[nodiscard]] bool acceptsCreatureEntry(uint32 entry) const
    {
        for (auto const& s : sources)
            if (s.type == QuestObjectiveSource::Type::Creature && s.entry == entry)
                return true;
        return false;
    }

    [[nodiscard]] bool acceptsGameObjectEntry(uint32 entry) const
    {
        for (auto const& s : sources)
            if (s.type == QuestObjectiveSource::Type::GameObject && s.entry == entry)
                return true;
        return false;
    }
};

enum class QuestActionPhase : uint8
{
    ResolveObjective,
    TravelToSource,
    AcquireTarget,
    SelfDefense,
    EngageTarget,
    InteractSource,
    UseQuestItem,
    EscortEvent,
    LootSource,
    VerifyProgress,
    WaitForRespawn,
    ResolveFinisher,
    TravelToFinisher,
    InteractFinisher,
    VerifyReward,
    Complete,
    Blocked
};

enum class QuestFailureReason : uint16
{
    None,
    QuestMissing,
    QuestNotActive,
    UnsupportedObjective,
    NoItemSource,
    NoSourceSpawn,
    NoLiveCandidate,
    ObjectiveSourcesExhausted,
    EventActorDeadOrUnavailable,
    SourcesRespawning,
    SourceNotPathable,
    LootRightsDenied,
    InventoryFull,
    ProgressDidNotChange,
    NoFinisherRelation,
    NoFinisherSpawn,
    FinisherNotLoaded,
    FinisherDeadOrUnavailable,
    CrossMapRouteUnavailable,
    MovementStuckNoTeleport,
    InteractionRejected,
    CanRewardFalse,
    RewardNotConfirmed,
    OracleFinisherLeaseExpired,
    OracleFinisherMismatch,
    OracleRouteInvalidDescriptor,
    OracleRouteUnsupportedTransition,
    OracleRouteNoSafeAnchor,
    OracleRouteBlocked,
    TravelNoProgress,  // appended: AutoWow.QuestTravelProgressWatch.Enable budget spent
    IntentReplanExhausted,  // appended: AutoWow.TravelIntent.Enable replan budget spent
    IntentNoProgress        // appended: AutoWow.TravelIntent.Enable no goal progress in the window
};

struct QuestFinisherRef
{
    int32 signedEntry = 0;  // >0 creature, <0 gameobject
    GuidPosition stableSpawn;
    ObjectGuid runtimeGuid;
};

// Per-bot evidence for the exact finisher sequence. The receipt records identity and postconditions
// around the ordinary questgiver handlers; it is never a grant/completion authority.
struct QuestFinisherReceipt
{
    uint64 sequence = 0;
    uint32 questId = 0;
    int32 signedEntry = 0;
    ObjectGuid stableSpawnGuid;
    ObjectGuid runtimeGuid;
    bool exactRelationVerified = false;
    bool atInteractionRange = false;
    bool completionRequestSent = false;
    bool coreCompleted = false;
    bool rewardConfirmed = false;
};

// A world-thread receipt captured around the exact CMSG_GAMEOBJ_USE handler call used by the
// direct GameObject-credit path. The sequence is assigned by NewRpgInfo's durable per-bot ledger;
// before/after are the authoritative quest counters observed synchronously around the opcode.
struct DirectGameObjectReceipt
{
    uint64 sequence = 0;
    uint32 questId = 0;
    uint8 objectiveSlot = 0;
    uint32 entry = 0;
    ObjectGuid guid;
    uint32 before = 0;
    uint32 after = 0;

    [[nodiscard]] bool credited() const { return after > before; }
};

struct QuestObjectiveRuntime
{
    QuestActionPhase phase = QuestActionPhase::ResolveObjective;
    QuestFailureReason failure = QuestFailureReason::None;

    int32 selectedSourceEntry = 0;
    GuidPosition selectedSourceSpawn;
    ObjectGuid selectedTargetGuid;

    uint32 baselineCount = 0;
    uint32 lastObservedCount = 0;
    uint32 lastProgressTimeMs = 0;
    uint32 attemptCount = 0;

    // Source rotation is scoped to one canonical objective. A source is added only after the
    // executor has waited at the resolved spawn with no active target; it is never marked merely
    // because a different source was selected or because a target is temporarily out of range.
    uint32 sourceRotationQuestId = 0;
    uint8 sourceRotationSlot = 0;
    QuestObjectiveFamily sourceRotationFamily = QuestObjectiveFamily::NpcOrGameObject;
    bool sourceRotationObjectiveKnown = false;
    uint32 sourceRotationCount = 0;
    std::unordered_set<uint64> exhaustedSourceSpawns;
    std::unordered_map<uint64, uint32> sourceRotationCooldownUntil;

    std::unordered_map<ObjectGuid, uint32> targetCooldownUntil;

    // Starter-stall recovery (flag-gated consumers only). blockedAtMs is the Blocked entry time
    // (0 = never blocked); travelWatch measures net approach to the current travel destination.
    uint32 blockedAtMs = 0;
    QuestStallRecoveryPolicy::TravelWatch travelWatch;
    uint32 travelRotationCount = 0;  // travel-expiry rotations spent on this DoQuest

    QuestFinisherRef finisher;
    QuestFinisherReceipt finisherReceipt;
    uint32 rewardAttemptTimeMs = 0;
    uint32 selectedRewardIndex = 0;
    bool selectedRewardIndexKnown = false;
    bool completionAnnounced = false;

    // Set at ChangeToDoQuest for configured Oracle bots, before the first lease exists. This makes
    // the initially-unleased directive fail closed until the runtime performs the sole tagged dispatch.
    bool oracleManaged = false;

    // Set only while the AutoWow Oracle arbiter owns this bot's quest state.  This is a durable
    // fail-closed marker for the native action: a short-lived tagged-event gate may expire between
    // runtime cadences, but an untagged NewRpgStrategy event must still be rejected until the
    // arbiter lease is terminally released.
    bool oracleLeaseRequired = false;
    uint64 oracleLeaseDecisionId = 0;

    // Oracle-owned finisher authorization is a bounded handoff from the tagged decision/lease.
    // The native finisher state machine still re-resolves and validates this identity every tick.
    bool oracleFinisherAuthorized = false;
    uint64 oracleFinisherDecisionId = 0;
    uint32 oracleFinisherQuestId = 0;
    int32 oracleFinisherSignedEntry = 0;
    uint64 oracleFinisherStableSpawnGuid = 0;
    uint64 finisherRequestSequence = 0;

    // T2 route state is populated only during the runtime's exact tagged dispatch.  The session
    // carries per-attempt state; its ledger is mirrored to the server-session ledger owned by the
    // action implementation before and after every policy call.
    bool oracleTaggedDispatch = false;
    bool oracleRouteActive = false;
    bool oracleRouteFinisher = false;
    uint32 oracleRouteQuestId = 0;
    int32 oracleRouteSignedEntry = 0;
    uint64 oracleRouteStableSpawnGuid = 0;
    uint64 oracleRouteDecisionId = 0;
    uint64 oracleRouteStartedAt = 0;
    AutoWowOracleRoute::RouteSession oracleRouteSession;
    AutoWowOracleRoute::RouteResult oracleRouteReceipt;
    bool oracleRouteReceiptAvailable = false;
    bool oracleRouteIdentityPinned = false;
    bool oracleRouteNativeInteractionObserved = false;

    // Last real observations used to emit one-shot deltas after a completed reprobe.  Command
    // issuance never changes these values and therefore cannot manufacture progress.
    bool oracleRouteObservationInitialized = false;
    uint32 oracleRouteLastObjectiveCount = 0;
    uint8 oracleRouteLastQuestStatus = 0;
    uint32 oracleRouteLastInventoryCount = 0;
    uint64 oracleRouteLastInteractionSequence = 0;
};

// Apply a successful direct-GO receipt immediately instead of waiting for a later VerifyProgress
// sample. The caller also invalidates the published objective/finisher Values and resets its phase
// dwell timer, allowing the next objective (or completed-quest finisher) to take ownership at once.
[[nodiscard]] inline bool RebaseAfterDirectGameObjectCredit(QuestObjectiveRuntime& runtime,
                                                             DirectGameObjectReceipt const& receipt,
                                                             uint32 progressTimeMs)
{
    if (!receipt.credited())
        return false;

    runtime.baselineCount = receipt.after;
    runtime.lastObservedCount = receipt.after;
    runtime.lastProgressTimeMs = progressTimeMs;
    runtime.attemptCount = 0;
    runtime.selectedTargetGuid.Clear();
    runtime.failure = QuestFailureReason::None;
    runtime.phase = QuestActionPhase::ResolveObjective;
    return true;
}

// Once the ordinary quest preconditions pass, the indexed overload checks storage for the exact
// selected choice reward plus fixed rewards. A failure at that second gate is therefore an
// inventory-capacity blocker, not an opaque CanRewardFalse result.
[[nodiscard]] inline QuestFailureReason ClassifyRewardFailure(bool canRewardQuest,
                                                               bool canRewardSelectedReward)
{
    if (!canRewardQuest)
        return QuestFailureReason::CanRewardFalse;
    if (!canRewardSelectedReward)
        return QuestFailureReason::InventoryFull;
    return QuestFailureReason::None;
}

// While an external quest directive is active, generic grind has exactly two legal modes:
// strict objective-source targeting (when hasLock() is true), or no proactive target at all.
// The latter covers finisher travel, transition ticks, unsupported objectives, and typed blockers;
// self-defense is evaluated before this policy by GrindTargetValue and remains allowed.
[[nodiscard]] inline bool ShouldSuppressLegacyQuestGrind(bool questDirectiveActive,
                                                         QuestObjectiveSpec const& objective)
{
    return questDirectiveActive && (!objective.hasLock() || objective.kind == QuestObjectiveKind::ScriptedEvent);
}

#endif  // PLAYERBOTS_QUEST_OBJECTIVE_CONTEXT_H
