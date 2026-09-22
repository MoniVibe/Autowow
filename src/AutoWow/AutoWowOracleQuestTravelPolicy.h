/*
 * Pure quest/travel policy adapter for the shared AutoWow Oracle.
 *
 * The normalized facts and the typed transition checks preserve the isolated quest lane's
 * semantics. The only public execution value is an AutoWowOracle::OracleCandidate; callers must
 * publish it through AutoWowOracle::Plan and AutoWowOracle::OracleArbiter.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_TRAVEL_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_TRAVEL_POLICY_H

#include "AutoWowOracleContract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <tuple>

namespace AutoWowOracleQuestTravel
{
using Guid = AutoWowOracle::Guid;
using QuestId = std::uint32_t;
using Tick = AutoWowOracle::Tick;
using FrameVersion = AutoWowOracle::FactVersion;
using Epoch = AutoWowOracle::Epoch;

inline constexpr std::size_t kMaxObjectiveSources = 16;
inline constexpr std::size_t kMaxTravelTransitions = 32;
inline constexpr std::uint32_t kQuestTravelTtlTicks = 4;
inline constexpr Tick kInitialBackoffTicks = 2;
inline constexpr Tick kMaximumBackoffTicks = 64;
inline constexpr std::uint16_t kQuestPriority = 300;
inline constexpr std::uint16_t kTravelPriority = 200;
inline constexpr std::uint16_t kRecoveryPriority = 100;

enum class IntentKind : std::uint8_t
{
    None,
    AcquireQuest,
    AcceptQuest,
    Objective,
    TurnInQuest,
    Travel,
    Recover
};

enum class PlanBlocker : std::uint8_t
{
    None,
    InvalidFrame,
    BotUnavailable,
    QuestUnavailable,
    QuestAlreadyRewarded,
    GiverUnavailable,
    AcceptanceUnavailable,
    CapabilityUnsupported,
    ObjectiveUnavailable,
    FinisherUnavailable,
    TurnInUnavailable,
    NoTravelRoute,
    CrossMapRouteUnavailable,
    BackoffActive
};

enum class CapabilityClass : std::uint8_t
{
    TurnIn,
    DungeonGroup,
    ItemUse,
    GameObject,
    Kill,
    Loot,
    Talk
};

struct CapabilityFacts
{
    std::uint32_t questType = 0;
    std::uint32_t suggestedPlayers = 0;
    std::uint32_t sourceItemId = 0;
    bool hasNpcObjective = false;
    bool hasGameObjectObjective = false;
    bool hasItemObjective = false;
    bool questComplete = false;
};

struct Capability
{
    CapabilityClass kind = CapabilityClass::Talk;
    bool supported = true;
};

// Completion wins first, followed by unsupported group/game-object shapes, then the supported
// item-on-creature, creature, item, and talk paths from the isolated quest lane.
inline constexpr Capability ClassifyCapability(CapabilityFacts const& facts)
{
    if (facts.questComplete)
        return {CapabilityClass::TurnIn, true};
    if (facts.questType != 0 && facts.suggestedPlayers >= 2)
        return {CapabilityClass::DungeonGroup, false};
    if (facts.sourceItemId != 0 && facts.hasNpcObjective)
        return {CapabilityClass::ItemUse, true};
    if (facts.hasGameObjectObjective)
        return {CapabilityClass::GameObject, false};
    if (facts.hasNpcObjective)
        return {CapabilityClass::Kill, true};
    if (facts.hasItemObjective)
        return {CapabilityClass::Loot, true};
    return {CapabilityClass::Talk, true};
}

struct Endpoint
{
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool mapValidated = false;
};

inline bool ValidEndpoint(Endpoint const& endpoint)
{
    return endpoint.mapValidated && std::isfinite(endpoint.x) && std::isfinite(endpoint.y) &&
        std::isfinite(endpoint.z);
}

inline bool SameEndpoint(Endpoint const& left, Endpoint const& right)
{
    return ValidEndpoint(left) && ValidEndpoint(right) && left.mapId == right.mapId &&
        left.instanceId == right.instanceId && left.x == right.x && left.y == right.y &&
        left.z == right.z;
}

inline bool RequiresTransition(Endpoint const& source, Endpoint const& destination)
{
    return source.mapId != destination.mapId || source.instanceId != destination.instanceId;
}

enum class ObjectiveFamily : std::uint8_t
{
    NpcOrGameObject,
    Item
};

enum class ObjectiveKind : std::uint8_t
{
    CreatureCredit,
    GameObjectCredit,
    CollectItem,
    UseQuestItem,
    ScriptedEvent,
    Unsupported
};

struct ObjectiveKey
{
    QuestId questId = 0;
    ObjectiveFamily family = ObjectiveFamily::NpcOrGameObject;
    std::uint8_t slot = 0;
};

inline constexpr bool SameObjective(ObjectiveKey const& left, ObjectiveKey const& right)
{
    return left.questId == right.questId && left.family == right.family && left.slot == right.slot;
}

struct ObjectiveSourceFact
{
    std::uint32_t entry = 0;
    Guid targetGuid = 0;
    Endpoint endpoint;
    bool resolved = false;
    bool loaded = false;
    bool alive = false;
    bool lootable = false;
    bool interactable = false;
    bool atInteractionRange = false;
    bool available = false;
};

struct ObjectiveFacts
{
    bool present = false;
    bool supported = false;
    ObjectiveKey key;
    ObjectiveKind kind = ObjectiveKind::Unsupported;
    std::uint32_t requiredEntry = 0;
    std::uint32_t requiredItemId = 0;
    std::uint32_t currentCount = 0;
    std::uint32_t requiredCount = 0;
    std::uint32_t baselineCount = 0;
    std::uint32_t lastObservedCount = 0;
    std::array<ObjectiveSourceFact, kMaxObjectiveSources> sources{};
    std::size_t sourceCount = 0;
};

struct ObjectiveProgress
{
    bool present = false;
    bool complete = false;
    bool progressed = false;
    bool regressed = false;
    std::uint32_t referenceCount = 0;
    std::uint32_t currentCount = 0;
    std::uint32_t requiredCount = 0;
    std::uint32_t positiveDelta = 0;
};

inline ObjectiveProgress EvaluateObjectiveProgress(ObjectiveFacts const& objective)
{
    ObjectiveProgress progress;
    progress.present = objective.present && objective.supported && objective.key.questId != 0;
    progress.referenceCount = std::max(objective.baselineCount, objective.lastObservedCount);
    progress.currentCount = objective.currentCount;
    progress.requiredCount = objective.requiredCount;
    progress.regressed = progress.currentCount < progress.referenceCount;
    progress.progressed = progress.currentCount > progress.referenceCount;
    progress.positiveDelta = progress.progressed ? progress.currentCount - progress.referenceCount : 0;
    progress.complete = progress.present && progress.requiredCount != 0 &&
        progress.currentCount >= progress.requiredCount;
    return progress;
}

inline bool HasObjectiveLock(ObjectiveFacts const& objective)
{
    return objective.present && objective.supported && objective.key.questId != 0 &&
        objective.kind != ObjectiveKind::Unsupported && objective.requiredCount != 0 &&
        objective.currentCount < objective.requiredCount;
}

struct ActorFacts
{
    Guid guid = 0;
    std::uint32_t entry = 0;
    Endpoint endpoint;
    bool resolved = false;
    bool loaded = false;
    bool alive = false;
    bool relationVerified = false;
    bool offersQuest = false;
    bool interactionReady = false;
};

struct QuestFacts
{
    QuestId questId = 0;
    bool known = false;
    bool available = false;
    bool active = false;
    bool complete = false;
    bool rewarded = false;
    bool offerSelected = false;
    bool canAccept = false;
    bool canTurnIn = false;
    CapabilityFacts capability;
    ActorFacts giver;
    ActorFacts finisher;
};

struct BotFacts
{
    Guid botGuid = 0;
    bool alive = true;
    bool inCombat = false;
    std::uint64_t money = 0;
    Endpoint position;
};

enum class TravelKind : std::uint8_t
{
    WalkingApproach,
    AreaTrigger,
    GameObject,
    Transport,
    Taxi,
    Unsupported
};

struct TravelTransitionFact
{
    std::uint64_t stableId = 0;
    TravelKind kind = TravelKind::Unsupported;
    Endpoint source;
    Endpoint destination;
    std::uint64_t cost = 0;
    std::uint64_t fare = 0;
    bool available = false;
    bool normalTransitionValidated = false;
    bool directedProof = false;
    bool dynamicTransport = false;
    bool taxiNodesKnown = false;
    bool seedGold = false;
    bool travelCheat = false;
};

enum class TravelPurpose : std::uint8_t
{
    Acquire,
    Objective,
    TurnIn
};

enum class FailureKind : std::uint8_t
{
    None,
    GiverUnavailable,
    AcceptanceRejected,
    ObjectiveUnavailable,
    ObjectiveNoProgress,
    FinisherUnavailable,
    TurnInRejected,
    TravelUnavailable,
    RecoveryUnavailable
};

struct RetryFacts
{
    bool active = false;
    FailureKind failure = FailureKind::None;
    std::uint32_t attemptCount = 0;
    Tick lastAttemptTick = 0;
    Tick nextEligibleTick = 0;
};

inline constexpr Tick BackoffTicks(std::uint32_t attemptCount)
{
    std::uint32_t const shift = attemptCount > 5 ? 5 : attemptCount;
    Tick const delay = kInitialBackoffTicks << shift;
    return delay > kMaximumBackoffTicks ? kMaximumBackoffTicks : delay;
}

inline constexpr Tick SaturatingAdd(Tick left, Tick right)
{
    Tick const maximum = std::numeric_limits<Tick>::max();
    return right > maximum - left ? maximum : left + right;
}

inline constexpr Tick NextRetryTick(RetryFacts const& retry)
{
    return retry.nextEligibleTick != 0 ? retry.nextEligibleTick :
        SaturatingAdd(retry.lastAttemptTick, BackoffTicks(retry.attemptCount));
}

inline constexpr bool RetryReady(Tick now, RetryFacts const& retry)
{
    return !retry.active || now >= NextRetryTick(retry);
}

enum class RecoveryReason : std::uint8_t
{
    None,
    BotBusy,
    BotUnavailable,
    MissingFacts,
    GiverUnavailable,
    AcceptanceUnavailable,
    CapabilityUnsupported,
    ObjectiveSourceUnavailable,
    ObjectiveNoProgress,
    FinisherUnavailable,
    TurnInUnavailable,
    TravelUnavailable,
    CrossMapRouteUnavailable,
    Backoff
};

struct QuestTravelFrame
{
    FrameVersion version = 0;
    Tick tick = 0;
    Epoch epoch = 0;
    BotFacts bot;
    QuestFacts quest;
    ObjectiveFacts objective;
    RetryFacts retry;
    std::array<TravelTransitionFact, kMaxTravelTransitions> transitions{};
    std::size_t transitionCount = 0;
};

inline bool FrameWithinBounds(QuestTravelFrame const& frame)
{
    return frame.objective.sourceCount <= frame.objective.sources.size() &&
        frame.transitionCount <= frame.transitions.size();
}

inline bool AddObjectiveSource(QuestTravelFrame& frame, ObjectiveSourceFact const& source)
{
    if (frame.objective.sourceCount >= frame.objective.sources.size())
        return false;
    frame.objective.sources[frame.objective.sourceCount++] = source;
    return true;
}

inline bool AddTravelTransition(QuestTravelFrame& frame, TravelTransitionFact const& transition)
{
    if (frame.transitionCount >= frame.transitions.size())
        return false;
    frame.transitions[frame.transitionCount++] = transition;
    return true;
}

// This result is policy evidence plus one shared candidate. It is not a lease and has no executor
// callback; the shared Oracle contract remains the sole decision and lease authority.
struct QuestTravelPlanResult
{
    bool hasCandidate = false;
    PlanBlocker blocker = PlanBlocker::None;
    Capability capability;
    ObjectiveProgress progress;
    IntentKind kind = IntentKind::None;
    RecoveryReason recovery = RecoveryReason::None;
    Tick retryAfterTick = 0;
    Guid targetGuid = 0;
    std::uint32_t targetEntry = 0;
    std::uint32_t itemId = 0;
    AutoWowOracle::OracleCandidate candidate;
};

namespace Detail
{
inline constexpr std::uint8_t TravelKindRank(TravelKind kind)
{
    switch (kind)
    {
        case TravelKind::WalkingApproach: return 0;
        case TravelKind::AreaTrigger: return 1;
        case TravelKind::GameObject: return 2;
        case TravelKind::Transport: return 3;
        case TravelKind::Taxi: return 4;
        case TravelKind::Unsupported: return 5;
    }
    return 5;
}

inline bool SourceUsable(ObjectiveSourceFact const& source)
{
    return source.available && source.resolved && source.loaded && source.interactable &&
        source.atInteractionRange && (source.alive || source.lootable);
}

inline bool SourceRouteCandidate(ObjectiveSourceFact const& source)
{
    return source.available && source.resolved && ValidEndpoint(source.endpoint);
}

inline bool SourceMatchesObjective(ObjectiveFacts const& objective,
                                   ObjectiveSourceFact const& source)
{
    switch (objective.kind)
    {
        case ObjectiveKind::CreatureCredit:
        case ObjectiveKind::GameObjectCredit:
        case ObjectiveKind::UseQuestItem:
        case ObjectiveKind::ScriptedEvent:
            return objective.requiredEntry != 0 && source.entry == objective.requiredEntry;
        case ObjectiveKind::CollectItem:
            return objective.requiredItemId != 0 && source.entry == objective.requiredItemId;
        case ObjectiveKind::Unsupported:
            return false;
    }
    return false;
}

inline bool ActorUsable(ActorFacts const& actor, bool requireQuestRelation)
{
    return actor.resolved && actor.loaded && actor.alive && ValidEndpoint(actor.endpoint) &&
        (!requireQuestRelation || actor.relationVerified);
}

inline bool TransitionMatches(TravelTransitionFact const& transition, Endpoint const& source,
                              Endpoint const& destination)
{
    return transition.available && transition.stableId != 0 &&
        SameEndpoint(transition.source, source) && SameEndpoint(transition.destination, destination);
}

inline bool ValidTransition(TravelTransitionFact const& transition, QuestTravelFrame const& frame,
                            Endpoint const& destination)
{
    if (!TransitionMatches(transition, frame.bot.position, destination) ||
        !ValidEndpoint(transition.source) || !ValidEndpoint(transition.destination) ||
        transition.seedGold || transition.travelCheat)
        return false;

    bool const crossMap = RequiresTransition(transition.source, transition.destination);
    switch (transition.kind)
    {
        case TravelKind::WalkingApproach:
            return !crossMap;
        case TravelKind::AreaTrigger:
        case TravelKind::GameObject:
            return transition.normalTransitionValidated && transition.directedProof;
        case TravelKind::Transport:
            return transition.normalTransitionValidated && transition.directedProof &&
                transition.dynamicTransport;
        case TravelKind::Taxi:
            return transition.directedProof && transition.taxiNodesKnown &&
                frame.bot.money >= transition.fare;
        case TravelKind::Unsupported:
            return false;
    }
    return false;
}

inline bool SourceLess(ObjectiveSourceFact const& left, ObjectiveSourceFact const& right)
{
    auto const leftKey = std::tuple(left.endpoint.mapId, left.endpoint.instanceId, left.entry,
                                    left.targetGuid, left.endpoint.x, left.endpoint.y, left.endpoint.z);
    auto const rightKey = std::tuple(right.endpoint.mapId, right.endpoint.instanceId, right.entry,
                                     right.targetGuid, right.endpoint.x, right.endpoint.y,
                                     right.endpoint.z);
    return leftKey < rightKey;
}

inline bool TransitionLess(TravelTransitionFact const& left, TravelTransitionFact const& right)
{
    auto const leftKey = std::tuple(left.cost, TravelKindRank(left.kind), left.stableId,
                                    left.destination.mapId, left.destination.instanceId,
                                    left.destination.x, left.destination.y, left.destination.z);
    auto const rightKey = std::tuple(right.cost, TravelKindRank(right.kind), right.stableId,
                                     right.destination.mapId, right.destination.instanceId,
                                     right.destination.x, right.destination.y, right.destination.z);
    return leftKey < rightKey;
}

struct Selection
{
    bool found = false;
    std::size_t index = 0;
};

inline Selection SelectSource(ObjectiveFacts const& objective)
{
    Selection selection;
    for (std::size_t index = 0; index < objective.sourceCount; ++index)
    {
        ObjectiveSourceFact const& candidate = objective.sources[index];
        if (!SourceRouteCandidate(candidate) || !SourceMatchesObjective(objective, candidate))
            continue;
        if (!selection.found || SourceLess(candidate, objective.sources[selection.index]))
        {
            selection.found = true;
            selection.index = index;
        }
    }
    return selection;
}

inline Selection SelectTransition(QuestTravelFrame const& frame, Endpoint const& target)
{
    Selection selection;
    for (std::size_t index = 0; index < frame.transitionCount; ++index)
    {
        TravelTransitionFact const& candidate = frame.transitions[index];
        if (!ValidTransition(candidate, frame, target))
            continue;
        if (!selection.found || TransitionLess(candidate, frame.transitions[selection.index]))
        {
            selection.found = true;
            selection.index = index;
        }
    }
    return selection;
}

inline std::string_view ActionFor(IntentKind kind, TravelPurpose purpose,
                                  RecoveryReason recovery)
{
    switch (kind)
    {
        case IntentKind::AcquireQuest: return "quest.acquire";
        case IntentKind::AcceptQuest: return "quest.accept";
        case IntentKind::Objective: return "quest.objective";
        case IntentKind::TurnInQuest: return "quest.turn_in";
        case IntentKind::Travel:
            switch (purpose)
            {
                case TravelPurpose::Acquire: return "quest.travel.acquire";
                case TravelPurpose::Objective: return "quest.travel.objective";
                case TravelPurpose::TurnIn: return "quest.travel.turn_in";
            }
            return "quest.travel.objective";
        case IntentKind::Recover:
            switch (recovery)
            {
                case RecoveryReason::BotBusy: return "recovery.quest.bot_busy";
                case RecoveryReason::BotUnavailable: return "recovery.quest.bot_unavailable";
                case RecoveryReason::CapabilityUnsupported: return "recovery.quest.unsupported";
                case RecoveryReason::CrossMapRouteUnavailable: return "recovery.quest.cross_map_route";
                case RecoveryReason::Backoff: return "recovery.quest.backoff";
                default: return "recovery.quest.travel";
            }
        case IntentKind::None:
            return {};
    }
    return {};
}

inline constexpr AutoWowOracle::OperationCode OperationFor(IntentKind kind)
{
    switch (kind)
    {
        case IntentKind::AcquireQuest:
            return AutoWowOracle::OperationCode::QuestAcquire;
        case IntentKind::AcceptQuest:
            return AutoWowOracle::OperationCode::QuestAccept;
        case IntentKind::Objective:
            return AutoWowOracle::OperationCode::QuestObjective;
        case IntentKind::TurnInQuest:
            return AutoWowOracle::OperationCode::QuestTurnIn;
        case IntentKind::Travel:
            return AutoWowOracle::OperationCode::Navigate;
        case IntentKind::Recover:
            return AutoWowOracle::OperationCode::Recover;
        case IntentKind::None:
            return AutoWowOracle::OperationCode::Unknown;
    }
    return AutoWowOracle::OperationCode::Unknown;
}

inline AutoWowOracle::QuestReference QuestReferenceFor(QuestTravelFrame const& frame,
    IntentKind kind)
{
    AutoWowOracle::QuestReference reference;
    reference.questId = frame.quest.questId;
    reference.valid = reference.questId != 0;

    if (kind == IntentKind::Objective)
    {
        reference.questId = frame.objective.key.questId;
        reference.valid = reference.questId != 0;
        reference.objectiveSlot = frame.objective.key.slot;
        reference.objectiveFamily = frame.objective.key.family == ObjectiveFamily::Item ?
            AutoWowOracle::QuestObjectiveFamily::Item :
            AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject;
        reference.requiredEntry = frame.objective.requiredEntry;
        reference.requiredItemId = frame.objective.requiredItemId;
    }
    return reference;
}

inline AutoWowOracle::OracleCandidate MakeCandidate(
    QuestTravelFrame const& frame, IntentKind kind, Guid targetGuid, std::uint32_t itemId,
    TravelPurpose purpose, RecoveryReason recovery, Endpoint const* destination,
    TravelTransitionFact const* transition, AutoWowOracle::IntentClassification classification)
{
    AutoWowOracle::OracleCandidate candidate;
    candidate.domain = kind == IntentKind::Travel ? AutoWowOracle::Domain::Navigation :
        kind == IntentKind::Recover ? AutoWowOracle::Domain::Recovery : AutoWowOracle::Domain::Quest;
    candidate.intent = kind == IntentKind::Travel ? AutoWowOracle::IntentCode::Navigate :
        kind == IntentKind::Recover ? AutoWowOracle::IntentCode::Recover :
                                      AutoWowOracle::IntentCode::QuestObjective;
    candidate.resource = kind == IntentKind::Travel ? AutoWowOracle::LeaseResource::Transition :
        kind == IntentKind::Recover ? AutoWowOracle::LeaseResource::Recovery :
                                      AutoWowOracle::LeaseResource::QuestGather;
    candidate.classification = classification;
    candidate.priority = kind == IntentKind::Travel ? kTravelPriority :
        kind == IntentKind::Recover ? kRecoveryPriority : kQuestPriority;
    candidate.ttlTicks = kQuestTravelTtlTicks;
    candidate.targetGuid = targetGuid;
    candidate.itemId = itemId;
    candidate.action = ActionFor(kind, purpose, recovery);
    candidate.operation = OperationFor(kind);
    candidate.quest = QuestReferenceFor(frame, kind);
    candidate.available = true;
    // Only the exact active-objective path has a provisional native adapter. Quest acquisition,
    // acceptance, turn-in, travel, and recovery remain visible as blocked planning receipts until
    // their executor contracts are separately verified.
    candidate.executorAvailable = kind == IntentKind::Objective && HasObjectiveLock(frame.objective);
    candidate.requiresBotAlive = recovery != RecoveryReason::BotUnavailable;
    candidate.requiresSameMap = true;

    if (kind == IntentKind::Travel && destination != nullptr && transition != nullptr &&
        RequiresTransition(frame.bot.position, *destination))
    {
        candidate.requiresSameMap = false;
        candidate.transition = {true, transition->stableId, transition->source.mapId,
            transition->source.instanceId, transition->destination.mapId,
            transition->destination.instanceId, transition->normalTransitionValidated,
            transition->directedProof, transition->travelCheat, transition->seedGold};
    }
    return candidate;
}

inline QuestTravelPlanResult MakeResult(
    QuestTravelFrame const& frame, IntentKind kind, Guid targetGuid = 0, std::uint32_t itemId = 0,
    TravelPurpose purpose = TravelPurpose::Objective, RecoveryReason recovery = RecoveryReason::None,
    Endpoint const* destination = nullptr, TravelTransitionFact const* transition = nullptr,
    AutoWowOracle::IntentClassification classification = AutoWowOracle::IntentClassification::Persistent)
{
    QuestTravelPlanResult result;
    result.hasCandidate = true;
    result.kind = kind;
    result.recovery = recovery;
    result.targetGuid = targetGuid;
    result.itemId = itemId;
    result.candidate = MakeCandidate(frame, kind, targetGuid, itemId, purpose, recovery, destination,
                                     transition, classification);
    return result;
}

inline QuestTravelPlanResult MakeRecovery(QuestTravelFrame const& frame, RecoveryReason reason,
                                          Tick retryAfterTick = 0)
{
    QuestTravelPlanResult result = MakeResult(frame, IntentKind::Recover, 0, 0,
                                               TravelPurpose::Objective, reason);
    result.retryAfterTick = retryAfterTick;
    return result;
}

inline QuestTravelPlanResult RecoverForTravel(QuestTravelFrame const& frame,
                                              QuestTravelPlanResult result, TravelPurpose purpose,
                                              Guid targetGuid, Endpoint const& destination)
{
    Capability const capability = result.capability;
    ObjectiveProgress const progress = result.progress;

    if (!ValidEndpoint(destination))
    {
        result = MakeRecovery(frame, RecoveryReason::TravelUnavailable);
        result.capability = capability;
        result.progress = progress;
        result.blocker = PlanBlocker::NoTravelRoute;
        return result;
    }

    Selection const selection = SelectTransition(frame, destination);
    if (!selection.found)
    {
        bool const crossMap = RequiresTransition(frame.bot.position, destination);
        result = MakeRecovery(frame, crossMap ? RecoveryReason::CrossMapRouteUnavailable
                                              : RecoveryReason::TravelUnavailable);
        result.capability = capability;
        result.progress = progress;
        result.blocker = crossMap ? PlanBlocker::CrossMapRouteUnavailable : PlanBlocker::NoTravelRoute;
        return result;
    }

    result = MakeResult(frame, IntentKind::Travel, targetGuid, 0, purpose, RecoveryReason::None,
                        &destination, &frame.transitions[selection.index]);
    result.capability = capability;
    result.progress = progress;
    return result;
}

inline QuestTravelPlanResult Block(QuestTravelPlanResult result, PlanBlocker blocker)
{
    result.hasCandidate = false;
    result.blocker = blocker;
    result.candidate = {};
    return result;
}
}  // namespace Detail

inline QuestTravelPlanResult PlanQuestTravel(
    QuestTravelFrame const& frame,
    AutoWowOracle::IntentClassification classification = AutoWowOracle::IntentClassification::Persistent)
{
    QuestTravelPlanResult result;
    result.capability = ClassifyCapability(frame.quest.capability);
    result.progress = EvaluateObjectiveProgress(frame.objective);

    if (!FrameWithinBounds(frame))
        return Detail::Block(result, PlanBlocker::InvalidFrame);
    if (frame.bot.botGuid == 0 || !ValidEndpoint(frame.bot.position))
        return Detail::Block(result, PlanBlocker::BotUnavailable);
    if (!frame.bot.alive)
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::BotUnavailable);
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }
    if (frame.bot.inCombat)
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::BotBusy);
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }
    if (frame.retry.active && !RetryReady(frame.tick, frame.retry) &&
        !(frame.retry.failure == FailureKind::ObjectiveNoProgress && result.progress.progressed))
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::Backoff, NextRetryTick(frame.retry));
        result.blocker = PlanBlocker::BackoffActive;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }
    if (!frame.quest.known || frame.quest.questId == 0)
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::MissingFacts);
        result.blocker = PlanBlocker::QuestUnavailable;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }
    if (frame.quest.rewarded)
        return Detail::Block(result, PlanBlocker::QuestAlreadyRewarded);

    if (!frame.quest.active)
    {
        if (!frame.quest.available || !frame.quest.giver.offersQuest ||
            !Detail::ActorUsable(frame.quest.giver, true))
        {
            result = Detail::MakeRecovery(frame, RecoveryReason::GiverUnavailable);
            result.blocker = PlanBlocker::GiverUnavailable;
            result.capability = ClassifyCapability(frame.quest.capability);
            result.progress = EvaluateObjectiveProgress(frame.objective);
            result.candidate.classification = classification;
            return result;
        }
        if (!frame.quest.giver.interactionReady)
        {
            result = Detail::RecoverForTravel(frame, result, TravelPurpose::Acquire,
                                              frame.quest.giver.guid, frame.quest.giver.endpoint);
            result.candidate.classification = classification;
            return result;
        }
        if (!frame.quest.offerSelected)
        {
            result = Detail::MakeResult(frame, IntentKind::AcquireQuest, frame.quest.giver.guid);
            result.targetEntry = frame.quest.giver.entry;
            result.candidate.classification = classification;
            return result;
        }
        if (!frame.quest.canAccept)
        {
            result = Detail::MakeRecovery(frame, RecoveryReason::AcceptanceUnavailable);
            result.blocker = PlanBlocker::AcceptanceUnavailable;
            result.candidate.classification = classification;
            return result;
        }
        result = Detail::MakeResult(frame, IntentKind::AcceptQuest, frame.quest.giver.guid);
        result.targetEntry = frame.quest.giver.entry;
        result.candidate.classification = classification;
        return result;
    }

    if (!result.capability.supported)
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::CapabilityUnsupported);
        result.blocker = PlanBlocker::CapabilityUnsupported;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }

    bool const objectiveComplete = frame.quest.complete || result.progress.complete;
    if (!objectiveComplete)
    {
        if (!HasObjectiveLock(frame.objective) || frame.objective.key.questId != frame.quest.questId)
        {
            result = Detail::MakeRecovery(frame, RecoveryReason::ObjectiveSourceUnavailable);
            result.blocker = PlanBlocker::ObjectiveUnavailable;
            result.capability = ClassifyCapability(frame.quest.capability);
            result.progress = EvaluateObjectiveProgress(frame.objective);
            result.candidate.classification = classification;
            return result;
        }

        Detail::Selection const source = Detail::SelectSource(frame.objective);
        if (!source.found)
        {
            result = Detail::MakeRecovery(frame, RecoveryReason::ObjectiveSourceUnavailable);
            result.blocker = PlanBlocker::ObjectiveUnavailable;
            result.capability = ClassifyCapability(frame.quest.capability);
            result.progress = EvaluateObjectiveProgress(frame.objective);
            result.candidate.classification = classification;
            return result;
        }

        ObjectiveSourceFact const& selected = frame.objective.sources[source.index];
        if (!Detail::SourceUsable(selected))
        {
            if (!selected.atInteractionRange)
            {
                result = Detail::RecoverForTravel(frame, result, TravelPurpose::Objective,
                                                  selected.targetGuid, selected.endpoint);
                result.candidate.classification = classification;
                return result;
            }
            result = Detail::MakeRecovery(frame, RecoveryReason::ObjectiveSourceUnavailable);
            result.blocker = PlanBlocker::ObjectiveUnavailable;
            result.capability = ClassifyCapability(frame.quest.capability);
            result.progress = EvaluateObjectiveProgress(frame.objective);
            result.candidate.classification = classification;
            return result;
        }

        std::uint32_t const itemId = frame.objective.kind == ObjectiveKind::CollectItem ||
                frame.objective.kind == ObjectiveKind::UseQuestItem
            ? frame.objective.requiredItemId
            : 0;
        result = Detail::MakeResult(frame, IntentKind::Objective, selected.targetGuid, itemId);
        result.targetEntry = selected.entry;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }

    if (!Detail::ActorUsable(frame.quest.finisher, true))
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::FinisherUnavailable);
        result.blocker = PlanBlocker::FinisherUnavailable;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }
    if (!frame.quest.finisher.interactionReady)
    {
        result = Detail::RecoverForTravel(frame, result, TravelPurpose::TurnIn,
                                          frame.quest.finisher.guid, frame.quest.finisher.endpoint);
        result.candidate.classification = classification;
        return result;
    }
    if (!frame.quest.canTurnIn)
    {
        result = Detail::MakeRecovery(frame, RecoveryReason::TurnInUnavailable);
        result.blocker = PlanBlocker::TurnInUnavailable;
        result.capability = ClassifyCapability(frame.quest.capability);
        result.progress = EvaluateObjectiveProgress(frame.objective);
        result.candidate.classification = classification;
        return result;
    }

    result = Detail::MakeResult(frame, IntentKind::TurnInQuest, frame.quest.finisher.guid);
    result.targetEntry = frame.quest.finisher.entry;
    result.capability = ClassifyCapability(frame.quest.capability);
    result.progress = EvaluateObjectiveProgress(frame.objective);
    result.candidate.classification = classification;
    return result;
}

inline bool AddQuestTravelCandidate(
    AutoWowOracle::WorldReadFrame& sharedFrame, QuestTravelFrame const& questFrame,
    AutoWowOracle::IntentClassification classification = AutoWowOracle::IntentClassification::Persistent)
{
    QuestTravelPlanResult const plan = PlanQuestTravel(questFrame, classification);
    return plan.hasCandidate && AutoWowOracle::AddCandidate(sharedFrame, plan.candidate);
}
}  // namespace AutoWowOracleQuestTravel

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_QUEST_TRAVEL_POLICY_H
