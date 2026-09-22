/*
 * World-thread-only AutoWow Oracle runtime.
 */
#include "AutoWowOracleRuntime.h"

#include "AutoWowOracleOwnershipGate.h"
#include "AutoWowOracleExactHandoffPolicy.h"
#include "AutoWowOracleFinisherIntent.h"
#include "AutoWowOracleQuestSelectionPolicy.h"
#include "AutoWowQuestLedger.h"
#include "AiObjectContext.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "G3D/Vector3.h"
#include "GatheringWorkerState.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "OracleQuestDispatchPolicy.h"
#include "OracleQuestExecutor.h"
#include "OracleGatherRuntimePolicy.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestFinisherTransitionPolicy.h"
#include "QuestObjectiveContext.h"
#include "Value.h"
#include "WorkerGatherAction.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace AutoWowOracleRuntime
{
namespace
{
using AutoWowOracle::ActiveLeaseSnapshot;
using AutoWowOracle::Decision;
using AutoWowOracle::Domain;
using AutoWowOracle::FactVersion;
using AutoWowOracle::GatherSourceReference;
using AutoWowOracle::IntentCode;
using AutoWowOracle::LeaseResource;
using AutoWowOracle::OperationCode;
using AutoWowOracle::ScopeKind;

inline constexpr Tick kNoEligibleTelemetryInterval = 10;

std::uint64_t StableSessionIdentity(std::string_view sessionId) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char value : sessionId)
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

bool IsSeparator(char value) noexcept
{
    return value == ',' || std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool ParseGuidToken(std::string_view token, Guid& out) noexcept
{
    if (token.empty())
        return false;

    for (char value : token)
        if (value < '0' || value > '9')
            return false;

    Guid parsed = 0;
    char const* first = token.data();
    char const* last = token.data() + token.size();
    auto const result = std::from_chars(first, last, parsed, 10);
    if (result.ec != std::errc() || result.ptr != last || parsed == 0)
        return false;
    out = parsed;
    return true;
}

bool HasGuid(std::array<Guid, kMaxRuntimeBots> const& guids,
             std::size_t count, Guid guid) noexcept
{
    for (std::size_t index = 0; index < count; ++index)
        if (guids[index] == guid)
            return true;
    return false;
}

AutoWowOracleQuestExecutor::ObjectiveStepPhase ToExecutorPhase(QuestActionPhase phase) noexcept
{
    using Phase = AutoWowOracleQuestExecutor::ObjectiveStepPhase;
    switch (phase)
    {
        case QuestActionPhase::ResolveObjective: return Phase::ResolveObjective;
        case QuestActionPhase::TravelToSource: return Phase::TravelToSource;
        case QuestActionPhase::AcquireTarget: return Phase::AcquireTarget;
        case QuestActionPhase::SelfDefense: return Phase::SelfDefense;
        case QuestActionPhase::EngageTarget: return Phase::EngageTarget;
        case QuestActionPhase::InteractSource: return Phase::InteractSource;
        case QuestActionPhase::UseQuestItem: return Phase::UseQuestItem;
        case QuestActionPhase::EscortEvent: return Phase::EscortEvent;
        case QuestActionPhase::LootSource: return Phase::LootSource;
        case QuestActionPhase::VerifyProgress: return Phase::VerifyProgress;
        case QuestActionPhase::WaitForRespawn: return Phase::WaitForRespawn;
        case QuestActionPhase::ResolveFinisher:
        case QuestActionPhase::TravelToFinisher:
        case QuestActionPhase::InteractFinisher:
        case QuestActionPhase::VerifyReward:
        case QuestActionPhase::Complete:
        case QuestActionPhase::Blocked:
            return Phase::Finisher;
    }
    return Phase::Unknown;
}

bool SameLease(ActiveLeaseSnapshot const& proof, IntentLease const& lease, Tick now) noexcept
{
    return proof.active && lease.valid && proof.botGuid == lease.decision.scope.botGuid &&
           proof.decisionId == lease.decision.decisionId && proof.intentId == lease.decision.intentId &&
           proof.epoch == lease.decision.epoch && proof.expiresTick == lease.decision.expiresTick &&
           proof.expiresTick > now && AutoWowOracle::SameScope(proof.scope, lease.decision.scope);
}

AutoWowOracleExactHandoff::Facts ReadExactAcquireFacts(
    Player* bot, Guid botGuid, QuestObjectiveSpec const& spec,
    QuestObjectiveRuntime const& rt, IntentIdentity const& oldIdentity,
    DecisionId oldDecision, AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff& handoff)
{
    using namespace AutoWowOracleRoute;
    AutoWowOracleExactHandoff::Facts f;
    if (!bot || !spec.hasLock() || !rt.oracleRouteIdentityPinned ||
        !AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, handoff))
        return f;

    RouteKey const& key = handoff.result.routeKey;
    bool const creatureSource = rt.selectedSourceEntry > 0 &&
        key.targetKind == RouteTargetKind::Creature;
    bool const gameObjectSource = rt.selectedSourceEntry < 0 &&
        key.targetKind == RouteTargetKind::GameObject;
    if (!creatureSource && !gameObjectSource)
        return f;
    std::uint32_t const entry = static_cast<std::uint32_t>(
        rt.selectedSourceEntry < 0 ? -static_cast<std::int64_t>(rt.selectedSourceEntry)
                                   : rt.selectedSourceEntry);
    for (QuestObjectiveSource const& source : spec.sources)
    {
        if (source.entry != entry ||
            (source.type == QuestObjectiveSource::Type::Creature) != creatureSource)
            continue;
        for (GuidPosition const& spawn : source.spawns)
            if (spawn.GetRawValue() == rt.selectedSourceSpawn.GetRawValue() &&
                spawn.GetMapId() == bot->GetMapId())
                f.sourceInObjective = true;
    }

    f.runtimeTargetMatches = rt.selectedTargetGuid.IsEmpty();
    if (!f.runtimeTargetMatches && creatureSource)
    {
        Creature* target = ObjectAccessor::GetCreature(*bot, rt.selectedTargetGuid);
        f.runtimeTargetMatches = target && target->IsInWorld() &&
            target->GetSpawnId() == rt.selectedSourceSpawn.GetCounter() &&
            target->GetEntry() == entry && target->GetMapId() == bot->GetMapId() &&
            target->GetInstanceId() == bot->GetInstanceId();
    }
    else if (!f.runtimeTargetMatches && gameObjectSource)
    {
        GameObject* target = ObjectAccessor::GetGameObject(*bot, rt.selectedTargetGuid);
        f.runtimeTargetMatches = target && target->IsInWorld() &&
            target->GetSpawnId() == rt.selectedSourceSpawn.GetCounter() &&
            target->GetEntry() == entry && target->GetMapId() == bot->GetMapId() &&
            target->GetInstanceId() == bot->GetInstanceId();
    }
    f.pinned = rt.oracleRouteIdentityPinned && rt.oracleRouteReceiptAvailable &&
        rt.oracleRouteQuestId == spec.key.questId &&
        rt.oracleRouteStableSpawnGuid == key.stableSpawn &&
        rt.oracleRouteDecisionId == oldDecision &&
        key.actor == botGuid &&
        key.purpose == static_cast<PurposeId>(RoutePurpose::QuestSource);
    f.arrived = handoff.arrived && handoff.result.arrived;
    f.blocked = handoff.blocked || handoff.result.blocked;
    f.routeFailed = handoff.failure != RouteFailure::None ||
        handoff.result.failure != RouteFailure::None;
    f.oldPhase = oldIdentity.phase;
    f.newPhase = static_cast<std::uint8_t>(rt.phase);
    f.oldQuest = oldIdentity.questId;
    f.newQuest = spec.key.questId;
    f.oldFamily = oldIdentity.objectiveFamily;
    f.newFamily = static_cast<std::uint8_t>(spec.key.family);
    f.oldSlot = oldIdentity.objectiveSlot;
    f.newSlot = spec.key.slot;
    f.routeEntry = key.entry;
    f.selectedEntry = entry;
    f.routeSpawn = key.stableSpawn;
    f.selectedSpawn = rt.selectedSourceSpawn.GetRawValue();
    f.routeMap = key.mapId;
    f.currentMap = bot->GetMapId();
    f.routeInstance = key.instanceId;
    f.currentInstance = bot->GetInstanceId();
    f.oldDecision = oldDecision;
    f.routeDecision = handoff.lease;
    return f;
}

bool IsArrivedExactAcquire(AutoWowOracleExactHandoff::Facts const& facts) noexcept
{
    return AutoWowOracleExactHandoff::SameArrivedSource(facts,
        static_cast<std::uint8_t>(QuestActionPhase::TravelToSource),
        static_cast<std::uint8_t>(QuestActionPhase::AcquireTarget));
}

Tick GateExpiryFor(RuntimeConfig const& config, Tick now) noexcept
{
    std::uint64_t const cadence = std::max<std::uint64_t>(config.cadenceMs, 1000);
    std::uint64_t const ttlTicks = std::max<std::uint64_t>(config.leaseTtlTicks, 1);
    std::uint64_t const window = cadence * (ttlTicks + 1);
    Tick const maximum = std::numeric_limits<Tick>::max();
    if (window >= maximum - now)
        return maximum;
    return now + window;
}

void StopOwnedGatherMotion(Guid botGuid)
{
    std::uint32_t const guid = static_cast<std::uint32_t>(botGuid);
    AutoWowGatherRuntimePolicy::ExactMotionProvenance const provenance =
        AutoWowGather::GetOracleMotionProvenance(guid);
    if (provenance.valid)
    {
        if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid)))
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
    }
    AutoWowGather::ClearOracleMotionProvenance(guid);
}

GameObject* FindExactGatherSource(Player* bot, AutoWowGather::Candidate const& candidate,
                                  std::uint32_t instanceId)
{
    if (!bot || !bot->GetMap() || bot->GetMapId() != candidate.mapId ||
        bot->GetInstanceId() != instanceId)
        return nullptr;

    bot->GetMap()->LoadGrid(candidate.x, candidate.y);
    auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(
        static_cast<ObjectGuid::LowType>(candidate.spawnId));
    for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
    {
        GameObject* source = iterator->second;
        if (source && source->IsInWorld() && source->GetSpawnId() == candidate.spawnId &&
            source->GetEntry() == candidate.entry && source->GetMapId() == candidate.mapId &&
            source->GetInstanceId() == instanceId)
            return source;
    }
    return nullptr;
}

bool IsReadyGatherSource(GameObject* source)
{
    return source && source->IsInWorld() && source->isSpawned() &&
        source->GetGoState() == GO_STATE_READY &&
        !source->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND | GO_FLAG_NOT_SELECTABLE);
}

bool IsCompleteGatherPath(Player* bot, AutoWowGather::Candidate const& candidate)
{
    if (!bot || bot->GetMapId() != candidate.mapId)
        return false;

    PathGenerator path(bot);
    if (!path.CalculatePath(candidate.x, candidate.y, candidate.z))
        return false;

    std::uint32_t const disallowedPathTypes = PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE |
        PATHFIND_NOPATH | PATHFIND_FARFROMPOLY | PATHFIND_NOT_USING_PATH;
    if (static_cast<std::uint32_t>(path.GetPathType()) & disallowedPathTypes)
        return false;

    G3D::Vector3 const& endpoint = path.GetActualEndPosition();
    float const currentDistance = bot->GetExactDist(candidate.x, candidate.y, candidate.z);
    float const dx = endpoint.x - candidate.x;
    float const dy = endpoint.y - candidate.y;
    float const dz = endpoint.z - candidate.z;
    float const endpointDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return endpointDistance + 1.0f < currentDistance;
}

}  // namespace

QuestBootstrapIdentity ResolveQuestBootstrapIdentity(
    ::QuestObjectiveSpec const& spec, ::QuestActionPhase phase, std::uint32_t botMapId,
    Guid selectedTargetGuid) noexcept
{
    QuestBootstrapIdentity result;
    if (selectedTargetGuid != 0)
    {
        result.valid = true;
        result.targetGuid = selectedTargetGuid;
        return result;
    }

    bool const initialBootstrapPhase = phase == QuestActionPhase::ResolveObjective ||
        phase == QuestActionPhase::TravelToSource ||
        phase == QuestActionPhase::WaitForRespawn ||
        phase == QuestActionPhase::AcquireTarget;
    bool const sourceInteractionPhase = phase == QuestActionPhase::InteractSource ||
        phase == QuestActionPhase::LootSource;
    if (!spec.hasLock() || (!initialBootstrapPhase && !sourceInteractionPhase))
        return result;

    for (QuestObjectiveSource const& source : spec.sources)
    {
        if (source.entry == 0)
            continue;

        for (GuidPosition const& spawn : source.spawns)
        {
            std::uint64_t const rawGuid = spawn.GetRawValue();
            if (rawGuid == 0 || spawn.GetMapId() != botMapId)
                continue;

            bool const sourceIsGameObject = source.type == QuestObjectiveSource::Type::GameObject;
            // A creature target is only safe to bootstrap before native target acquisition. Once
            // the state machine is in an interaction/loot phase, only a stable GameObject source
            // may re-seed the tagged candidate; this is the narrow recovery for a live GO whose
            // runtime GUID has not yet been bound. Native range, selection, ownership, and loot
            // gates remain authoritative after dispatch.
            if (sourceInteractionPhase && !sourceIsGameObject)
                continue;
            if ((sourceIsGameObject && !spawn.IsGameObject()) ||
                (!sourceIsGameObject && !spawn.IsCreature()))
                continue;
            if (spec.key.family == ::QuestObjectiveFamily::NpcOrGameObject)
            {
                std::int64_t const signedRequiredEntry = spec.requiredNpcOrGoEntry;
                bool const entryMatches = sourceIsGameObject
                    ? signedRequiredEntry < 0 &&
                        static_cast<std::int64_t>(source.entry) == -signedRequiredEntry
                    : signedRequiredEntry > 0 &&
                        static_cast<std::int64_t>(source.entry) == signedRequiredEntry;
                if (!entryMatches)
                    continue;
            }
            bool const better = !result.valid || rawGuid < result.targetGuid ||
                (rawGuid == result.targetGuid && source.entry < result.sourceEntry) ||
                (rawGuid == result.targetGuid && source.entry == result.sourceEntry &&
                 sourceIsGameObject < result.sourceIsGameObject);
            if (!better)
                continue;

            result.valid = true;
            result.bootstrap = true;
            result.targetGuid = rawGuid;
            result.mapId = spawn.GetMapId();
            result.sourceEntry = source.entry;
            result.sourceIsGameObject = sourceIsGameObject;
        }
    }
    return result;
}

bool ParseGuidAllowlist(std::string_view text,
                        std::array<Guid, kMaxRuntimeBots>& out,
                        std::size_t& outCount) noexcept
{
    out = {};
    outCount = 0;
    bool tokenSinceComma = false;
    bool commaPending = false;
    std::size_t position = 0;

    while (position < text.size())
    {
        if (std::isspace(static_cast<unsigned char>(text[position])) != 0)
        {
            ++position;
            continue;
        }

        if (text[position] == ',')
        {
            if (!tokenSinceComma || commaPending)
            {
                out = {};
                outCount = 0;
                return false;
            }
            tokenSinceComma = false;
            commaPending = true;
            ++position;
            continue;
        }

        std::size_t const start = position;
        while (position < text.size() && !IsSeparator(text[position]))
            ++position;

        Guid guid = 0;
        if (!ParseGuidToken(text.substr(start, position - start), guid))
        {
            out = {};
            outCount = 0;
            return false;
        }

        if (!HasGuid(out, outCount, guid))
        {
            if (outCount >= out.size())
            {
                out = {};
                outCount = 0;
                return false;
            }
            out[outCount++] = guid;
        }
        tokenSinceComma = true;
        commaPending = false;
    }

    if (commaPending)
    {
        out = {};
        outCount = 0;
        return false;
    }

    std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(outCount));
    return true;
}

bool IsGuidAllowed(std::array<Guid, kMaxRuntimeBots> const& guids,
                   std::size_t count, Guid guid) noexcept
{
    return guid != 0 && count <= guids.size() && HasGuid(guids, count, guid);
}

bool AdvanceCadence(CadenceClock& clock, std::uint32_t diffMs,
                    std::uint32_t cadenceMs) noexcept
{
    if (cadenceMs == 0)
        return false;

    std::uint64_t const elapsed = static_cast<std::uint64_t>(clock.elapsedMs) + diffMs;
    if (elapsed < cadenceMs)
    {
        clock.elapsedMs = static_cast<std::uint32_t>(elapsed);
        return false;
    }

    clock.elapsedMs = 0;
    return true;
}

bool ValidateConfig(RuntimeConfig const& config) noexcept
{
    if (config.cadenceMs < kMinCadenceMs || config.cadenceMs > kMaxCadenceMs ||
        config.maxBots == 0 || config.maxBots > AutoWowOracle::kMaxBotLeases ||
        config.leaseTtlTicks < kMinLeaseTtlTicks || config.leaseTtlTicks > kMaxLeaseTtlTicks ||
        config.botGuidCount > config.botGuids.size())
        return false;

    for (std::size_t index = 0; index < config.botGuidCount; ++index)
    {
        if (config.botGuids[index] == 0 || HasGuid(config.botGuids, index, config.botGuids[index]))
            return false;
    }

    return !config.enabled ||
           (config.botGuidCount != 0 && config.maxBots <= config.botGuidCount);
}

BotState* BoundedBotState::Find(Guid guid) noexcept
{
    for (std::size_t index = 0; index < size_; ++index)
        if (slots_[index].occupied && slots_[index].guid == guid)
            return &slots_[index];
    return nullptr;
}

BotState const* BoundedBotState::Find(Guid guid) const noexcept
{
    for (std::size_t index = 0; index < size_; ++index)
        if (slots_[index].occupied && slots_[index].guid == guid)
            return &slots_[index];
    return nullptr;
}

BotState* BoundedBotState::FindOrCreate(Guid guid) noexcept
{
    if (guid == 0)
        return nullptr;
    if (BotState* existing = Find(guid))
        return existing;
    if (size_ >= slots_.size())
        return nullptr;

    slots_[size_] = {};
    slots_[size_].occupied = true;
    slots_[size_].guid = guid;
    return &slots_[size_++];
}

bool BoundedBotState::Erase(Guid guid) noexcept
{
    for (std::size_t index = 0; index < size_; ++index)
    {
        if (!slots_[index].occupied || slots_[index].guid != guid)
            continue;
        slots_[index] = slots_[size_ - 1];
        slots_[size_ - 1] = {};
        --size_;
        return true;
    }
    return false;
}

void BoundedBotState::Clear() noexcept
{
    slots_ = {};
    size_ = 0;
}

std::size_t BoundedBotState::activeLeaseCount() const noexcept
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < size_; ++index)
        if (slots_[index].occupied && slots_[index].lease.valid)
            ++count;
    return count;
}

void ReceiptRing::Push(Receipt const& receipt) noexcept
{
    std::size_t const slot = (oldest_ + size_) % entries_.size();
    entries_[slot] = receipt;
    if (size_ < entries_.size())
    {
        ++size_;
        return;
    }
    oldest_ = (oldest_ + 1) % entries_.size();
}

Receipt const* ReceiptRing::At(std::size_t oldestIndex) const noexcept
{
    if (oldestIndex >= size_)
        return nullptr;
    return &entries_[(oldest_ + oldestIndex) % entries_.size()];
}

void ReceiptRing::Clear() noexcept
{
    entries_ = {};
    oldest_ = 0;
    size_ = 0;
}

struct Runtime::LiveQuestFrame
{
    bool valid = false;
    bool finisherMode = false;
    std::int32_t sourceEntry = 0;
    std::uint8_t phase = 0;
    std::uint16_t failure = 0;
    IntentIdentity identity;
    AutoWowOracle::WorldReadFrame frame;
    AutoWowOracleQuestExecutor::QuestObjectiveObservation observation;
};

struct Runtime::LiveGatherFrame
{
    bool valid = false;
    AutoWowOracle::GatherGoal goal = AutoWowOracle::GatherGoal::Unknown;
    AutoWowOracle::WorldReadFrame frame;
    AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot world;
    AutoWowOracleGatherExecutor::FixedTargetCandidate candidate;
};

struct Runtime::NativeDispatchContext
{
    Runtime* runtime = nullptr;
    Guid botGuid = 0;
};

Runtime& Runtime::instance()
{
    static Runtime runtime;
    return runtime;
}

OracleBotSnapshot* Runtime::FindOracleSnapshot(Guid botGuid) noexcept
{
    if (botGuid == 0)
        return nullptr;
    for (OracleBotSnapshot& snapshot : oracleSnapshots_)
        if (snapshot.occupied && snapshot.guid == botGuid)
            return &snapshot;
    for (OracleBotSnapshot& snapshot : oracleSnapshots_)
    {
        if (snapshot.occupied)
            continue;
        snapshot = {};
        snapshot.occupied = true;
        snapshot.guid = botGuid;
        return &snapshot;
    }
    return nullptr;
}

void Runtime::PublishQuestOracleSnapshot(Guid botGuid, LiveQuestFrame const& live) noexcept
{
    OracleBotSnapshot* snapshot = FindOracleSnapshot(botGuid);
    if (!snapshot)
        return;

    std::uint64_t const lastReceiptSequence = snapshot->lastReceiptSequence;
    std::uint64_t const completionReceiptSequence = snapshot->completionReceiptSequence;
    bool const completionDurabilityEligible = snapshot->completionDurabilityEligible;
    bool const evidenceInconclusive = snapshot->evidenceInconclusive;
    *snapshot = {};
    snapshot->occupied = true;
    snapshot->managed = true;
    snapshot->published = live.valid;
    snapshot->guid = botGuid;
    snapshot->worldTick = live.frame.tick;
    snapshot->phaseAvailable = live.valid;
    snapshot->phase = live.phase;
    snapshot->failureAvailable = live.valid;
    snapshot->failure = live.failure;
    snapshot->questIdAvailable = live.valid && live.observation.objective.questId != 0;
    snapshot->questId = live.observation.objective.questId;
    snapshot->objectiveIndexAvailable = live.valid && live.observation.objective.valid;
    snapshot->objectiveIndex = live.observation.objective.slot;
    snapshot->targetIdentityAvailable = live.valid && live.observation.objective.targetGuid != 0;
    snapshot->targetIdentity = live.observation.objective.targetGuid;
    snapshot->lastReceiptSequence = lastReceiptSequence;
    snapshot->completionReceiptSequence = completionReceiptSequence;
    snapshot->completionDurabilityEligible = completionDurabilityEligible;
    snapshot->evidenceInconclusive = evidenceInconclusive;

    BotState const* state = state_.Find(botGuid);
    if (state && state->lease.valid)
    {
        snapshot->leaseAvailable = true;
        snapshot->decisionId = state->lease.decision.decisionId;
        snapshot->intentId = state->lease.decision.intentId;
        snapshot->operationAvailable = true;
        snapshot->operation = state->lease.decision.operation;
        snapshot->leaseEpochAvailable = state->lease.decision.epoch != 0;
        snapshot->leaseEpoch = state->lease.decision.epoch;
        snapshot->leaseExpiresTickAvailable = true;
        snapshot->leaseExpiresTick = state->lease.decision.expiresTick;
    }
}

void Runtime::PublishGatherOracleSnapshot(Guid botGuid, LiveGatherFrame const& live) noexcept
{
    OracleBotSnapshot* snapshot = FindOracleSnapshot(botGuid);
    if (!snapshot)
        return;

    std::uint64_t const lastReceiptSequence = snapshot->lastReceiptSequence;
    std::uint64_t const completionReceiptSequence = snapshot->completionReceiptSequence;
    bool const completionDurabilityEligible = snapshot->completionDurabilityEligible;
    bool const evidenceInconclusive = snapshot->evidenceInconclusive;
    *snapshot = {};
    snapshot->occupied = true;
    snapshot->managed = true;
    snapshot->published = live.valid;
    snapshot->guid = botGuid;
    snapshot->worldTick = live.frame.tick;
    snapshot->targetIdentityAvailable = live.valid && live.candidate.reference.spawnId != 0;
    snapshot->targetIdentity = live.candidate.reference.spawnId;
    snapshot->lastReceiptSequence = lastReceiptSequence;
    snapshot->completionReceiptSequence = completionReceiptSequence;
    snapshot->completionDurabilityEligible = completionDurabilityEligible;
    snapshot->evidenceInconclusive = evidenceInconclusive;

    BotState const* state = state_.Find(botGuid);
    if (state && state->lease.valid)
    {
        snapshot->leaseAvailable = true;
        snapshot->decisionId = state->lease.decision.decisionId;
        snapshot->intentId = state->lease.decision.intentId;
        snapshot->operationAvailable = true;
        snapshot->operation = state->lease.decision.operation;
        snapshot->leaseEpochAvailable = state->lease.decision.epoch != 0;
        snapshot->leaseEpoch = state->lease.decision.epoch;
        snapshot->leaseExpiresTickAvailable = true;
        snapshot->leaseExpiresTick = state->lease.decision.expiresTick;
    }
}

void Runtime::InvalidateOracleSnapshot(Guid botGuid) noexcept
{
    OracleBotSnapshot* snapshot = FindOracleSnapshot(botGuid);
    if (!snapshot)
        return;

    std::uint64_t const lastReceiptSequence = snapshot->lastReceiptSequence;
    bool const evidenceInconclusive = snapshot->evidenceInconclusive;
    *snapshot = {};
    snapshot->occupied = true;
    snapshot->managed = true;
    snapshot->guid = botGuid;
    snapshot->lastReceiptSequence = lastReceiptSequence;
    snapshot->evidenceInconclusive = evidenceInconclusive;
}

bool Runtime::GetOracleSnapshot(Guid botGuid, OracleBotSnapshot& out)
{
    if (!configLoaded_)
        LoadConfig();

    out = {};
    out.guid = botGuid;
    out.managed = config_.enabled &&
        IsGuidAllowed(config_.botGuids, config_.botGuidCount, botGuid);
    if (!out.managed)
        return true;

    for (OracleBotSnapshot const& snapshot : oracleSnapshots_)
    {
        if (snapshot.occupied && snapshot.guid == botGuid)
        {
            out = snapshot;
            break;
        }
    }
    out.managed = true;

    // The cached publication can predate a release later in the same world update. Clear all
    // lease-derived fields before overlaying the current bounded runtime state so list() cannot
    // expose a stale operation or owner after the arbiter has released it.
    out.leaseAvailable = false;
    out.decisionId = 0;
    out.intentId = 0;
    out.operationAvailable = false;
    out.operation = AutoWowOracle::OperationCode::Unknown;
    out.leaseEpochAvailable = false;
    out.leaseEpoch = 0;
    out.leaseExpiresTickAvailable = false;
    out.leaseExpiresTick = 0;

    BotState const* state = state_.Find(botGuid);
    if (state && state->lease.valid)
    {
        out.leaseAvailable = true;
        out.decisionId = state->lease.decision.decisionId;
        out.intentId = state->lease.decision.intentId;
        out.operationAvailable = true;
        out.operation = state->lease.decision.operation;
        out.leaseEpochAvailable = state->lease.decision.epoch != 0;
        out.leaseEpoch = state->lease.decision.epoch;
        out.leaseExpiresTickAvailable = true;
        out.leaseExpiresTick = state->lease.decision.expiresTick;
    }

    AutoWowOracleReceiptStore::StreamStatus const stream = receiptStore_.Status();
    if (stream.evidenceInconclusive)
        out.evidenceInconclusive = true;
    out.completionClaimable = IsCompletionClaimable(
        out, stream.durableSequence,
        stream.evidenceInconclusive || out.evidenceInconclusive);
    return true;
}

AutoWowOracleReceiptStore::StreamStatus Runtime::GetOracleStreamStatus()
{
    if (!configLoaded_)
        LoadConfig();
    return receiptStore_.Status();
}

void Runtime::LoadConfig()
{
    config_.enabled = sConfigMgr->GetOption<bool>("AutoWow.OracleRuntime.Enabled", false);
    config_.zoneTravelAssist = sConfigMgr->GetOption<bool>(
        "AutoWow.OracleRuntime.ZoneTravelAssist", false);
    config_.cadenceMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.OracleRuntime.CadenceMs", 1000);
    config_.maxBots = sConfigMgr->GetOption<std::uint32_t>("AutoWow.OracleRuntime.MaxBots", 1);
    config_.leaseTtlTicks = sConfigMgr->GetOption<std::uint32_t>(
        "AutoWow.OracleRuntime.LeaseTtlTicks", 3);

    std::string const allowlist = sConfigMgr->GetOption<std::string>(
        "AutoWow.OracleRuntime.BotGuids", "");
    bool const parsed = ParseGuidAllowlist(allowlist, config_.botGuids, config_.botGuidCount);
    if (!parsed || !ValidateConfig(config_))
    {
        LOG_ERROR("playerbots", "AutoWow Oracle runtime disabled: invalid allowlist/cadence/max-bots configuration");
        config_ = {};
    }

    AutoWowOracleReceiptStore::Config receiptConfig;
    receiptConfig.enabled = sConfigMgr->GetOption<bool>(
        "AutoWow.OracleRuntime.ReceiptExport.Enabled", config_.enabled);
    receiptConfig.directory = sConfigMgr->GetOption<std::string>(
        "AutoWow.OracleRuntime.ReceiptExport.Directory", "logs/autowow-oracle");
    receiptConfig.rotationBytes = sConfigMgr->GetOption<std::uint32_t>(
        "AutoWow.OracleRuntime.ReceiptExport.RotationBytes",
        static_cast<std::uint32_t>(AutoWowOracleReceiptStore::kDefaultRotationBytes));
    receiptConfig.retentionFiles = sConfigMgr->GetOption<std::uint32_t>(
        "AutoWow.OracleRuntime.ReceiptExport.RetentionFiles",
        static_cast<std::uint32_t>(AutoWowOracleReceiptStore::kMaxRetentionFiles));
    receiptStore_.Start(receiptConfig);

    configLoaded_ = true;
}

bool Runtime::EnsureQuestDirective(Guid botGuid, BotState& state)
{
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || !ai || ai->IsRealPlayer())
        return false;

    auto resetObjectiveValues = [ai]()
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return;
        if (Value<QuestObjectiveSpec>* objective =
                context->GetValue<QuestObjectiveSpec>("active quest objective"))
            objective->Reset();
        if (Value<QuestFinisherRef>* finisher =
                context->GetValue<QuestFinisherRef>("active quest finisher"))
            finisher->Reset();
    };

    auto readObjectiveProgress = [ai]() -> BotState::ObjectiveProgress
    {
        BotState::ObjectiveProgress progress;
        AiObjectContext* context = ai->GetAiObjectContext();
        Value<QuestObjectiveSpec>* value = context
            ? context->GetValue<QuestObjectiveSpec>("active quest objective") : nullptr;
        if (!value)
            return progress;
        QuestObjectiveSpec const spec = value->Get();
        if (!spec.hasLock())
            return progress;
        progress.known = true;
        progress.questId = spec.key.questId;
        progress.family = static_cast<std::uint8_t>(spec.key.family);
        progress.slot = spec.key.slot;
        progress.count = spec.currentCount;
        return progress;
    };

    auto resetBlockedRetry = [&state]()
    {
        state.blockedQuestId = 0;
        state.blockedQuestRetryTick = 0;
        state.blockedQuestRetries = 0;
        state.blockedObjective = {};
    };

    auto evaluateRouteReentry = [&](AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff& handoff)
    {
        std::uint64_t const serverSessionId =
            StableSessionIdentity(receiptStore_.SessionId());
        (void)AutoWowOracleQuestExecutor::SetQuestRouteServerSession(
            botGuid, serverSessionId);
        (void)AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, handoff);
        AutoWowOracleQuestExecutor::QuestRouteReentryFacts facts;
        facts.targetLoadKnown = bot->GetMap() &&
            bot->GetMapId() == handoff.result.routeKey.mapId;
        facts.targetLoaded = facts.targetLoadKnown &&
            bot->GetMap()->IsGridLoaded(handoff.result.routeKey.dbX,
                                        handoff.result.routeKey.dbY);
        facts.positionKnown = bot->IsInWorld();
        facts.mapId = bot->GetMapId();
        facts.x = bot->GetPositionX();
        facts.y = bot->GetPositionY();
        facts.serverSessionId = serverSessionId;
        return AutoWowOracleQuestExecutor::EvaluateQuestRouteReentry(
            handoff.blockedReentryFacts, facts);
    };

    AutoWowOracleQuestExecutor::QuestRouteBackoffSet activeRouteBackoffs;
    auto* current = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
    if (current && current->questId != 0)
    {
        QuestStatus const status = bot->GetQuestStatus(current->questId);
        bool const stillActive =
            (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE) &&
            !bot->GetQuestRewardStatus(current->questId);

        if (stillActive && current->objectiveRuntime.phase != QuestActionPhase::Blocked)
        {
            current->objectiveRuntime.oracleManaged = true;
            // A cached empty objective may only mean that the Value was reset while the
            // directive changed. Resolve again from the current quest/core counters before
            // deciding that this quest cannot supply native work. In particular, a quest
            // ready for a normal finisher is never deferred for having no objective lock.
            AiObjectContext* context = ai->GetAiObjectContext();
            Value<QuestObjectiveSpec>* objective = context
                ? context->GetValue<QuestObjectiveSpec>("active quest objective") : nullptr;
            QuestObjectiveSpec resolved = objective ? objective->Get() : QuestObjectiveSpec{};
            if (objective && (!resolved.hasLock() || resolved.key.questId != current->questId))
            {
                objective->Reset();
                resolved = objective->Get();
            }
            bool const coreReadyForFinisher =
                status == QUEST_STATUS_COMPLETE || bot->CanCompleteQuest(current->questId);
            QuestSelectionFacts const selectionFacts{
                status == QUEST_STATUS_INCOMPLETE,
                coreReadyForFinisher,
                objective != nullptr,
                resolved.hasLock() && resolved.key.questId == current->questId};
            if (ShouldDeferUnrunnableQuest(selectionFacts))
            {
                uint32 const deferredQuestId = current->questId;
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::Emit(ai->GetBot(), AutoWowQuestLedger::Event::Deferred, deferredQuestId,
                        "oracle_unrunnable", AutoWowQuestLedger::PhaseName(current->objectiveRuntime.phase));
                if (state.lease.valid)
                    ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
                ai->lowPriorityQuest.insert(deferredQuestId);
                ai->rpgInfo.ChangeToIdle();
                resetObjectiveValues();
                resetBlockedRetry();
                current = nullptr;
                LOG_DEBUG("playerbots",
                    "[AutoWow Oracle] deferred unrunnable quest bot={} quest={} "
                    "core_ready={} resolver_available={}",
                    botGuid, deferredQuestId, coreReadyForFinisher, objective != nullptr);
            }
            if (current)
            {
                // A re-armed quest spends several nonblocked cadences traveling and interacting.
                // Keep its budget through phase/lease changes, but clear it when core objective
                // progress or a different quest makes a later failure genuinely new work.
                if (!KeepBlockedQuestRetry(state.blockedQuestId, current->questId,
                        status == QUEST_STATUS_COMPLETE, state.blockedObjective,
                        readObjectiveProgress()))
                    resetBlockedRetry();
                return true;
            }
        }

        if (current && stillActive && current->objectiveRuntime.phase == QuestActionPhase::Blocked)
        {
            // T2a manifest expansion: the Oracle-owned runtime is the terminal owner of the
            // strategic/tactical lease, so it must consume the quest route's durable handoff.
            AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff routeHandoff;
            bool const hasMatchingRouteBlock =
                AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, routeHandoff) &&
                routeHandoff.blocked && routeHandoff.questId == current->questId;
            if (hasMatchingRouteBlock)
            {
                AutoWowOracleRoute::Tick const nowSeconds = CurrentTick() / 1000U;
                AutoWowOracleQuestExecutor::QuestRouteReentrySignal const reentrySignal =
                    evaluateRouteReentry(routeHandoff);
                bool const retryReady = nowSeconds >= routeHandoff.nextRetryAt &&
                    (!routeHandoff.requiresWorldChange || reentrySignal !=
                        AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None);
                if (retryReady)
                {
                    if (reentrySignal !=
                        AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None)
                        (void)AutoWowOracleQuestExecutor::RecordQuestRouteReentrySignal(
                            botGuid, reentrySignal);
                    if (state.lease.valid)
                    {
                        ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
                        AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff released;
                        if (AutoWowOracleQuestExecutor::GetQuestRouteHandoff(
                                botGuid, released) && released.failure ==
                                    AutoWowOracleRoute::RouteFailure::LeaseReleaseFailed)
                        {
                            ai->rpgInfo.ChangeToIdle();
                            resetObjectiveValues();
                            return false;
                        }
                    }
                    Quest const* quest = sObjectMgr->GetQuestTemplate(current->questId);
                    if (!quest)
                        return false;
                    ai->rpgInfo.ChangeToDoQuest(current->questId, quest, true);
                    resetObjectiveValues();
                    ai->SetAutoWowIndependentParty(true);
                    ai->ChangeStrategy(
                        "+grind,-travel,-move random,-follow,+new rpg", BOT_STATE_NON_COMBAT);
                    LOG_DEBUG("playerbots",
                        "[AutoWow Oracle] re-armed persisted route block bot={} quest={} "
                        "failure_class={} retry_at={} reentry_signal={}",
                        botGuid, current->questId,
                        static_cast<std::uint32_t>(routeHandoff.blockKey.failureClass),
                        routeHandoff.nextRetryAt, static_cast<std::uint32_t>(reentrySignal));
                    return true;
                }

                // Active route backoff is a scheduler exclusion, not an owned wait. Keep the
                // quest and its completion state intact, release ownership, and let selection
                // consider another unfinished objective or fall through to gather/other work.
                if (state.lease.valid)
                    ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
                ai->rpgInfo.ChangeToIdle();
                resetObjectiveValues();
                current = nullptr;
            }

            // A failed tagged step must not strand an independent seed. Give the exact same
            // quest one bounded re-arm after a short quiet window; if it blocks again, park it
            // in the native low-priority set and let the next active quest win. This never
            // abandons or edits the quest in the database.
            if (!hasMatchingRouteBlock && state.blockedQuestId != current->questId)
            {
                state.blockedQuestId = current->questId;
                state.blockedQuestRetryTick = 0;
                state.blockedQuestRetries = 0;
                state.blockedObjective = readObjectiveProgress();
            }

            if (!hasMatchingRouteBlock && tick_ < state.blockedQuestRetryTick)
                return true;

            if (!hasMatchingRouteBlock && state.blockedQuestRetries < 1)
            {
                ++state.blockedQuestRetries;
                state.blockedQuestRetryTick = tick_ + 10;
                if (state.lease.valid)
                    ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
                Quest const* quest = sObjectMgr->GetQuestTemplate(current->questId);
                if (quest)
                {
                    ai->rpgInfo.ChangeToDoQuest(current->questId, quest, true);
                    resetObjectiveValues();
                    ai->SetAutoWowIndependentParty(true);
                    ai->ChangeStrategy(
                        "+grind,-travel,-move random,-follow,+new rpg", BOT_STATE_NON_COMBAT);
                    LOG_DEBUG("playerbots",
                        "[AutoWow Oracle] re-armed blocked quest bot={} quest={} retry={}",
                        botGuid, current->questId, state.blockedQuestRetries);
                    return true;
                }
            }

            if (!hasMatchingRouteBlock)
            {
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::Emit(ai->GetBot(), AutoWowQuestLedger::Event::Deferred, current->questId,
                        "oracle_blocked_parked", AutoWowQuestLedger::PhaseName(current->objectiveRuntime.phase));
                ai->lowPriorityQuest.insert(current->questId);
                LOG_DEBUG("playerbots",
                    "[AutoWow Oracle] parked blocked quest bot={} quest={} after bounded re-arm",
                    botGuid, current->questId);
                ai->rpgInfo.ChangeToIdle();
                resetBlockedRetry();
                current = nullptr;
            }
        }

        if (current)
        {
            if (state.lease.valid)
                ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
            ai->rpgInfo.ChangeToIdle();
            resetObjectiveValues();
            current = nullptr;
        }
    }

    AutoWowOracleRoute::QuestId reentryEligibleQuestId = 0;
    AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff handoff;
    if (AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, handoff) &&
        handoff.blocked && handoff.questId != 0)
    {
        AutoWowOracleQuestExecutor::QuestRouteReentrySignal const signal =
            evaluateRouteReentry(handoff);
        AutoWowOracleRoute::Tick const nowSeconds = CurrentTick() / 1000U;
        bool const retryReady = nowSeconds >= handoff.nextRetryAt &&
            (!handoff.requiresWorldChange || signal !=
                AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None);
        if (retryReady)
        {
            reentryEligibleQuestId = handoff.questId;
            if (signal != AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None)
                (void)AutoWowOracleQuestExecutor::RecordQuestRouteReentrySignal(
                    botGuid, signal);
        }
    }
    activeRouteBackoffs = AutoWowOracleQuestExecutor::CollectActiveQuestRouteBackoffs(
        botGuid, CurrentTick() / 1000U, reentryEligibleQuestId);
    auto isRouteBackoffActive = [&](AutoWowOracleRoute::QuestId questId)
    {
        for (std::uint8_t index = 0; index < activeRouteBackoffs.count; ++index)
            if (activeRouteBackoffs.questIds[index] == questId)
                return true;
        return false;
    };

    // Deterministic bootstrap for configured independent seeds. Prefer a completed quest so a
    // legitimate reward/unlock is collected before starting more objective work; otherwise use
    // the lowest active quest id that the native runtime has not already deprioritized.
    uint32 selectedQuestId = 0;
    QuestStatus selectedStatus = QUEST_STATUS_NONE;
    for (QuestStatus const wantedStatus : {QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE})
    {
        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (entry.second.Status != wantedStatus || bot->IsQuestRewarded(entry.first) ||
                isRouteBackoffActive(entry.first) ||
                ai->lowPriorityQuest.find(entry.first) != ai->lowPriorityQuest.end() ||
                !sObjectMgr->GetQuestTemplate(entry.first))
                continue;

            if (selectedQuestId == 0 || entry.first < selectedQuestId)
            {
                selectedQuestId = entry.first;
                selectedStatus = wantedStatus;
            }
        }
        if (selectedQuestId != 0)
            break;
    }

    if (selectedQuestId == 0)
    {
        if (state.lease.valid)
            ReleaseLease(state, MakeCleanupFrame(botGuid, state.lease));
        return false;
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(selectedQuestId);
    ai->rpgInfo.ChangeToDoQuest(selectedQuestId, quest, true);
    resetObjectiveValues();
    ai->SetAutoWowIndependentParty(true);
    ai->ChangeStrategy("+grind,-travel,-move random,-follow,+new rpg", BOT_STATE_NON_COMBAT);
    state.blockedQuestId = 0;
    state.blockedQuestRetryTick = 0;
    state.blockedQuestRetries = 0;
    LOG_DEBUG("playerbots", "[AutoWow Oracle] bootstrapped quest bot={} quest={} status={}",
              botGuid, selectedQuestId, static_cast<uint32>(selectedStatus));
    return true;
}

bool Runtime::BuildLiveQuestFrame(Guid botGuid, Tick tick, FactVersion version,
                                  AutoWowOracle::Epoch epoch,
                                  ActiveLeaseSnapshot const& ownership,
                                  LiveQuestFrame& out) const
{
    out = {};
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
        return false;

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai || ai->IsRealPlayer() || !ai->GetAiObjectContext())
        return false;

    auto* specValue = ai->GetAiObjectContext()->GetValue<QuestObjectiveSpec>("active quest objective");
    if (!specValue)
        return false;
    QuestObjectiveSpec const spec = specValue->Get();
    auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
    if (!doQuest || doQuest->questId == 0)
        return false;

    bool finisherMode = false;
    QuestFinisherRef finisher;
    if (!spec.hasLock())
    {
        auto* finisherValue = ai->GetAiObjectContext()->GetValue<QuestFinisherRef>("active quest finisher");
        if (!finisherValue)
            return false;
        finisher = finisherValue->Get();
        AutoWowQuestFinisher::Facts const facts{
            bot->GetQuestStatus(doQuest->questId),
            bot->GetQuestRewardStatus(doQuest->questId),
            bot->GetQuestStatus(doQuest->questId) == QUEST_STATUS_INCOMPLETE &&
                bot->CanCompleteQuest(doQuest->questId),
            finisher.signedEntry != 0 && !finisher.stableSpawn.IsEmpty()};
        if (!AutoWowQuestFinisher::IsFinisherReady(facts))
            return false;
        finisherMode = true;
    }
    else if (spec.key.questId == 0 || spec.key.questId != doQuest->questId)
        return false;

    using namespace AutoWowOracleQuestExecutor;
    QuestObjectiveSnapshot objective;
    objective.valid = true;
    objective.botGuid = botGuid;
    objective.questId = doQuest->questId;
    objective.family = finisherMode || spec.key.family != ::QuestObjectiveFamily::Item
        ? AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject
        : AutoWowOracle::QuestObjectiveFamily::Item;
    objective.slot = finisherMode ? 0 : spec.key.slot;
    objective.requiredEntry = finisherMode
        ? static_cast<std::uint32_t>(finisher.signedEntry < 0
                ? -static_cast<std::int64_t>(finisher.signedEntry)
                : finisher.signedEntry)
        : objective.family == AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject
              ? static_cast<std::uint32_t>(spec.requiredNpcOrGoEntry < 0
                    ? -static_cast<std::int64_t>(spec.requiredNpcOrGoEntry)
                    : spec.requiredNpcOrGoEntry)
              : 0;
    objective.requiredItemId = finisherMode || objective.family == AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject
        ? 0
        : spec.requiredItemId;
    QuestBootstrapIdentity const bootstrap = finisherMode
        ? QuestBootstrapIdentity{}
        : ResolveQuestBootstrapIdentity(
            spec, doQuest->objectiveRuntime.phase, bot->GetMapId(),
            doQuest->objectiveRuntime.selectedTargetGuid.GetRawValue());
    // Once the native route has actually arrived, its selected DB spawn is the only source that
    // can carry the pin through the Travel -> Acquire lease change. The generic bootstrap is still
    // used without this proof (and may legitimately choose a different first spawn).
    Guid arrivedSourceGuid = 0;
    if (!finisherMode)
    {
        BotState const* current = state_.Find(botGuid);
        AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff handoff;
        if (current && current->lease.valid)
        {
            AutoWowOracleExactHandoff::Facts const facts = ReadExactAcquireFacts(
                bot, botGuid, spec, doQuest->objectiveRuntime, current->leaseIdentity,
                current->lease.decision.decisionId, handoff);
            arrivedSourceGuid = AutoWowOracleExactHandoff::SelectLiveTarget(
                0, facts, AutoWowOracleExactHandoff::UsePinnedTarget(facts,
                    static_cast<std::uint8_t>(QuestActionPhase::TravelToSource),
                    static_cast<std::uint8_t>(QuestActionPhase::AcquireTarget),
                    static_cast<std::uint8_t>(QuestActionPhase::EngageTarget)));
        }
    }
    objective.targetGuid = finisherMode ? finisher.stableSpawn.GetRawValue()
        : arrivedSourceGuid != 0 ? arrivedSourceGuid : bootstrap.targetGuid;
    objective.targetMapId = finisherMode ? finisher.stableSpawn.GetMapId() : bot->GetMapId();
    objective.targetMapApplicable = true;
    objective.currentCount = finisherMode ? 0 : spec.currentCount;
    objective.requiredCount = finisherMode ? 1 : spec.requiredCount;
    objective.phase = finisherMode ? AutoWowOracleQuestExecutor::ObjectiveStepPhase::Finisher
                                   : ToExecutorPhase(doQuest->objectiveRuntime.phase);

    AutoWowOracle::WorldReadFrame frame;
    frame.version = version;
    frame.tick = tick;
    frame.epoch = epoch;
    frame.scope = {ScopeKind::PersistentCampaign, botGuid, botGuid};
    frame.bot = {botGuid, bot->IsAlive(), bot->IsInCombat(), bot->GetMapId(), bot->GetInstanceId()};
    frame.evidence.objective = objective.currentCount;
    frame.evidence.progress = objective.currentCount;
    frame.evidence.failures = doQuest->objectiveRuntime.failure == QuestFailureReason::None ? 0 : 1;

    if (objective.targetGuid != 0 && bot->IsAlive() &&
        (finisherMode || IsBoundedObjectivePhase(objective.phase)))
    {
        AutoWowOracle::OracleCandidate candidate;
        candidate.domain = Domain::Quest;
        candidate.intent = IntentCode::QuestObjective;
        candidate.resource = LeaseResource::QuestGather;
        candidate.classification = AutoWowOracle::IntentClassification::Persistent;
        candidate.priority = 100;
        candidate.ttlTicks = config_.leaseTtlTicks;
        candidate.targetGuid = objective.targetGuid;
        candidate.action = finisherMode ? "quest.finisher" : "quest.objective";
        candidate.qualifier = finisherMode ? "autowow.oracle.finisher" : "autowow.oracle.runtime";
        candidate.executorAvailable = true;
        candidate.requiresBotAlive = true;
        candidate.requiresSameMap = true;
        candidate.actorGuid = botGuid;
        candidate.operation = OperationCode::QuestObjective;
        candidate.quest = {true, objective.questId, objective.family, objective.slot,
                           objective.requiredEntry, objective.requiredItemId};
        candidate.itemId = objective.requiredItemId;
        AutoWowOracle::AddCandidate(frame, candidate);
    }

    AutoWowOracleQuestExecutor::ObjectiveWorldSnapshot world;
    world.onWorldThread = true;
    world.botAlive = bot->IsAlive();
    world.botGuid = botGuid;
    world.mapId = bot->GetMapId();
    world.instanceId = bot->GetInstanceId();
    world.tick = tick;
    world.frameVersion = version;
    world.epoch = epoch;
    world.ownership = ownership;

    out.frame = frame;
    out.observation = {world, objective};
    out.finisherMode = finisherMode;
    out.sourceEntry = doQuest->objectiveRuntime.selectedSourceEntry;
    out.phase = static_cast<std::uint8_t>(doQuest->objectiveRuntime.phase);
    out.failure = static_cast<std::uint16_t>(doQuest->objectiveRuntime.failure);
    out.identity.valid = true;
    out.identity.finisherMode = finisherMode;
    out.identity.finisherIsGameObject = finisherMode && finisher.signedEntry < 0;
    out.identity.questId = objective.questId;
    // Keep the native phase, not the coarser executor phase. ResolveFinisher, TravelToFinisher,
    // and InteractFinisher are separate identities and must not renew the same decision.
    out.identity.phase = static_cast<std::uint8_t>(doQuest->objectiveRuntime.phase);
    out.identity.objectiveFamily = static_cast<std::uint8_t>(objective.family);
    out.identity.objectiveSlot = objective.slot;
    out.identity.requiredEntry = objective.requiredEntry;
    out.identity.requiredItemId = objective.requiredItemId;
    out.identity.targetGuid = objective.targetGuid;
    out.identity.finisherSignedEntry = finisherMode ? finisher.signedEntry : 0;
    out.identity.finisherStableSpawnGuid = finisherMode ? finisher.stableSpawn.GetRawValue() : 0;
    out.valid = true;
    return true;
}

bool Runtime::BuildLiveGatherFrame(Guid botGuid, Tick tick, FactVersion version,
                                   AutoWowOracle::Epoch epoch,
                                   ActiveLeaseSnapshot const& ownership,
                                   LiveGatherFrame& out) const
{
    out = {};
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
        return false;

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai || ai->IsRealPlayer() || ai->IsAutoWowPaused() || !bot->IsAlive() || bot->IsInCombat())
        return false;

    AutoWowGather::RouteSnapshot const route = AutoWowGather::GetRouteSnapshot(
        static_cast<std::uint32_t>(botGuid));
    GatherSourceReference const& reference = route.oracleSource;
    if (!route.explicitWorker || !route.oracleExactSourceOnly || !route.hasCandidate ||
        !AutoWowOracle::ValidGatherSourceReference(reference) || reference.materialItemId == 0 ||
        route.candidate.spawnId != reference.spawnId || route.candidate.entry != reference.entry ||
        route.candidate.mapId != reference.mapId)
        return false;

    if (bot->GetMapId() != reference.mapId || bot->GetInstanceId() != reference.instanceId)
        return false;

    bool const latchedSourceProof = route.oracleExactSourceOnly && route.sourceMatched &&
        route.sourceGuid != 0 && route.sourceEntry == reference.entry &&
        route.requestedMaterialItemId == reference.materialItemId &&
        AutoWowOracle::SameGatherSourceReference(route.oracleSource, reference) &&
        route.sourceYieldsRequestedMaterial;
    bool const awaitingCredit = route.awaitingCredit && latchedSourceProof;
    GameObject* source = FindExactGatherSource(bot, route.candidate, reference.instanceId);
    LootObject pendingLoot(bot, source ? source->GetGUID() : ObjectGuid::Empty);
    bool const lootReady = !pendingLoot.IsEmpty();
    bool const sourceReady = IsReadyGatherSource(source);
    bool const sourceProof = latchedSourceProof ||
        (source && AutoWowGather::SourceYieldsMaterial(source, reference.materialItemId));
    if (!sourceProof || (!sourceReady && !lootReady && !awaitingCredit))
        return false;

    float const interactDistance = sPlayerbotAIConfig.lootDistance;
    bool const withinInteractRange = bot->GetExactDist(
        route.candidate.x, route.candidate.y, route.candidate.z) <= interactDistance;
    bool const reachable = awaitingCredit || withinInteractRange ||
        IsCompleteGatherPath(bot, route.candidate);
    if (!reachable)
        return false;

    bool const toolRequired = route.candidate.profession == AutoWowGather::Profession::Mining;
    bool const toolAvailable = AutoWowGather::HasRequiredTool(bot, route.candidate);
    if (!toolAvailable)
        return false;

    AutoWowOracle::WorldReadFrame frame;
    frame.version = version;
    frame.tick = tick;
    frame.epoch = epoch;
    frame.scope = {ScopeKind::PersistentCampaign, botGuid, botGuid};
    frame.bot = {botGuid, bot->IsAlive(), bot->IsInCombat(), bot->GetMapId(), bot->GetInstanceId()};
    if (reference.goal == AutoWowOracle::GatherGoal::ObtainMaterial)
    {
        frame.evidence.resource = bot->GetItemCount(reference.materialItemId, false);
        frame.evidence.progress = frame.evidence.resource;
    }
    else
    {
        // HarvestNode tracks canonical profession progress rather than pretending a material
        // request exists. The source material remains part of the exact reservation identity.
        frame.evidence.progress = route.skillAfter;
    }

    AutoWowOracle::OracleCandidate candidate;
    candidate.domain = Domain::Gathering;
    candidate.intent = IntentCode::GatherSource;
    candidate.resource = LeaseResource::QuestGather;
    candidate.classification = AutoWowOracle::IntentClassification::Persistent;
    candidate.priority = 100;
    candidate.ttlTicks = config_.leaseTtlTicks;
    candidate.targetGuid = reference.spawnId;
    candidate.itemId = reference.materialItemId;
    candidate.action = AutoWowOracleGatherExecutor::kWorkerGatherAction;
    candidate.qualifier = AutoWowOracleGatherExecutor::kGatherQualifier;
    candidate.available = sourceReady || lootReady || awaitingCredit;
    candidate.executorAvailable = true;
    candidate.requiresBotAlive = true;
    candidate.requiresSameMap = true;
    candidate.actorGuid = botGuid;
    candidate.operation = OperationCode::GatherSource;
    candidate.gather = reference;
    if (!AutoWowOracle::AddCandidate(frame, candidate))
        return false;

    AutoWowOracleGatherExecutor::FixedTargetCandidate exact;
    exact.reference = reference;
    exact.facts.alive = bot->IsAlive();
    exact.facts.available = sourceReady || lootReady || awaitingCredit;
    exact.facts.reachable = reachable;
    exact.facts.profession = route.candidate.profession;
    exact.facts.toolRequired = toolRequired;
    exact.facts.toolAvailable = toolAvailable;
    exact.facts.sourceYieldsRequestedMaterial = sourceProof;

    AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot world;
    world.onWorldThread = true;
    world.botAlive = bot->IsAlive();
    world.botGuid = botGuid;
    world.mapId = bot->GetMapId();
    world.instanceId = bot->GetInstanceId();
    world.tick = tick;
    world.frameVersion = version;
    world.epoch = epoch;
    world.withinInteractRange = withinInteractRange || awaitingCredit;
    world.lootReady = lootReady;
    world.ownershipProof = ownership;
    world.evidence = frame.evidence;

    out.frame = frame;
    out.goal = reference.goal;
    out.world = world;
    out.candidate = exact;
    out.valid = true;
    return true;
}

AutoWowOracle::WorldReadFrame Runtime::MakeCleanupFrame(Guid botGuid,
                                                         IntentLease const& lease) const
{
    AutoWowOracle::WorldReadFrame frame;
    frame.version = frameVersion_;
    frame.tick = tick_;
    frame.epoch = lease.valid ? lease.decision.epoch : epoch_;
    frame.scope = lease.valid ? lease.decision.scope
                              : AutoWowOracle::Scope{ScopeKind::PersistentCampaign, botGuid, botGuid};
    frame.bot.guid = botGuid;
    if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid)))
    {
        frame.bot.alive = bot->IsAlive();
        frame.bot.inCombat = bot->IsInCombat();
        frame.bot.mapId = bot->GetMapId();
        frame.bot.instanceId = bot->GetInstanceId();
    }
    return frame;
}

void Runtime::Record(Receipt const& receipt) noexcept
{
    receipts_.Push(receipt);
    AutoWowOracleReceiptStore::PublishResult const published = receiptStore_.Publish(
        receipt, tick_, receipt.scope.botGuid != 0 ? receipt.scope.botGuid : receipt.actorGuid);
    Guid const botGuid = receipt.scope.botGuid != 0 ? receipt.scope.botGuid : receipt.actorGuid;
    if (OracleBotSnapshot* snapshot = FindOracleSnapshot(botGuid))
    {
        snapshot->lastReceiptSequence = published.sequence;
        ObserveCompletionReceipt(
            *snapshot, receipt.status, published.sequence, published.queueAccepted);
        snapshot->evidenceInconclusive = snapshot->evidenceInconclusive ||
            !published.queueAccepted;
    }
}

bool Runtime::RequiresTaggedDispatch(Guid botGuid) const noexcept
{
    BotState const* state = state_.Find(botGuid);
    return state && state->lease.valid;
}

bool Runtime::HasActiveLease(Guid botGuid, DecisionId decisionId) const noexcept
{
    BotState const* state = state_.Find(botGuid);
    if (!state || !state->lease.valid)
        return false;

    ActiveLeaseSnapshot const active = arbiter_.QueryActiveLease(botGuid, tick_);
    if (!active.active || active.decisionId != state->lease.decision.decisionId ||
        active.intentId != state->lease.decision.intentId || active.epoch != state->lease.decision.epoch)
        return false;

    return decisionId == 0 || active.decisionId == decisionId;
}

bool Runtime::IsManagedBot(Guid botGuid)
{
    if (!configLoaded_)
        LoadConfig();
    return config_.enabled && IsGuidAllowed(config_.botGuids, config_.botGuidCount, botGuid);
}

DecisionId Runtime::ActiveDecisionId(Guid botGuid) const noexcept
{
    if (!HasActiveLease(botGuid))
        return 0;
    BotState const* state = state_.Find(botGuid);
    return state ? state->lease.decision.decisionId : 0;
}

void Runtime::ReleaseLease(BotState& state, AutoWowOracle::WorldReadFrame const& frame)
{
    Guid const botGuid = state.guid;
    DecisionId const releasedDecisionId = state.lease.valid
        ? state.lease.decision.decisionId : 0;
    bool nativeInteractionObserved = false;
    bool arbiterReleaseSucceeded = false;
    if (state.lease.valid)
    {
        AutoWowOracle::Receipt const releaseReceipt = arbiter_.Release(frame, state.lease);
        Record(releaseReceipt);
        arbiterReleaseSucceeded =
            releaseReceipt.status == AutoWowOracle::ReceiptStatus::Completed &&
            releaseReceipt.reason == AutoWowOracle::ReceiptReason::LeaseReleased &&
            releaseReceipt.decisionId == state.lease.decision.decisionId &&
            releaseReceipt.intentId == state.lease.decision.intentId &&
            releaseReceipt.epoch == state.lease.decision.epoch &&
            releaseReceipt.actorGuid == state.lease.decision.actorGuid &&
            releaseReceipt.ownerGuid == state.lease.decision.ownerGuid &&
            AutoWowOracle::SameScope(releaseReceipt.scope, state.lease.decision.scope);
    }

    // The tagged-event gate is released only from this terminal cleanup path. In particular,
    // do not release it at the end of an ordinary cadence: the native finisher state machine may
    // need several asynchronous ticks before interaction and reward confirmation complete.
    AutoWowOracleRuntime::Release(botGuid);
    if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid)))
    {
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
            {
                nativeInteractionObserved =
                    doQuest->objectiveRuntime.oracleRouteNativeInteractionObserved;
                AutoWowOracleQuestDispatchPolicy::OwnershipState const terminal =
                    AutoWowOracleQuestDispatchPolicy::TerminalRelease({
                        doQuest->objectiveRuntime.oracleManaged,
                        doQuest->objectiveRuntime.oracleLeaseRequired,
                        state.lease.valid,
                        true,
                        doQuest->objectiveRuntime.oracleLeaseDecisionId});
                doQuest->objectiveRuntime.oracleManaged = terminal.managed;
                doQuest->objectiveRuntime.oracleLeaseRequired = terminal.leaseRequired;
                doQuest->objectiveRuntime.oracleLeaseDecisionId = terminal.activeDecisionId;
                doQuest->objectiveRuntime.oracleFinisherAuthorized = false;
                doQuest->objectiveRuntime.oracleFinisherDecisionId = 0;
                doQuest->objectiveRuntime.oracleFinisherQuestId = 0;
                doQuest->objectiveRuntime.oracleFinisherSignedEntry = 0;
                doQuest->objectiveRuntime.oracleFinisherStableSpawnGuid = 0;
                doQuest->objectiveRuntime.oracleRouteIdentityPinned = false;
                doQuest->objectiveRuntime.oracleRouteNativeInteractionObserved = false;
            }
        }
    }

    AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff routeHandoff;
    (void)AutoWowOracleQuestExecutor::SetQuestRouteServerSession(
        botGuid, StableSessionIdentity(receiptStore_.SessionId()));
    if (AutoWowOracleQuestExecutor::CompleteQuestRouteLeaseRelease(
            botGuid, releasedDecisionId, CurrentTick() / 1000U,
            arbiterReleaseSucceeded, routeHandoff))
    {
        nativeInteractionObserved = nativeInteractionObserved ||
            routeHandoff.nativeInteractionObserved;
        AutoWowOracleRoute::RouteResult const& result = routeHandoff.result;
        char const* status = !arbiterReleaseSucceeded ? "release_failed"
            : routeHandoff.blocked ? "blocked" : "native_interaction";
        LOG_DEBUG("playerbots",
            "[AutoWow Oracle T1 RouteReceipt] bot_guid={} quest_id={} "
            "route_key={}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{} "
            "route_identity={} server_session_id={} segment={} "
            "quest_identity={}:{}:{} route_catalog_version={} failure_class={} "
            "status={} stage={} distance_available={} distance_initial={} "
            "distance_remaining={} distance_reduced={} anchor_attempts={} anchors={},{},{} "
            "failure={} next_retry_available={} next_retry={} lease_release=true "
            "lease_release_authoritative={} native_interaction={}",
            botGuid, routeHandoff.questId, result.routeKey.actor, result.routeKey.purpose,
            result.routeKey.mapId, result.routeKey.instanceId,
            static_cast<std::uint32_t>(result.routeKey.targetKind), result.routeKey.entry,
            result.routeKey.stableSpawn, result.routeKey.dbX, result.routeKey.dbY,
            result.routeKey.dbZ, result.routeKey.dbO, result.routeKey.radius,
            result.routeKey.controller, result.routeKey.job, result.routeKey.lease,
            AutoWowOracleQuestExecutor::MakeQuestRouteIdentity(result.routeKey),
            routeHandoff.blockedReentryFacts.serverSessionId,
            static_cast<std::uint32_t>(result.routeKey.segment), result.routeKey.questId,
            static_cast<std::uint32_t>(result.routeKey.objectiveFamily),
            static_cast<std::uint32_t>(result.routeKey.objectiveSlot),
            result.routeKey.routeCatalogVersion,
            static_cast<std::uint32_t>(routeHandoff.blockKey.failureClass), status,
            static_cast<std::uint32_t>(result.stage), result.distance.available,
            result.distance.initial, result.distance.remaining, result.distance.reduced,
            static_cast<std::uint32_t>(result.anchorAttempts), result.attemptedAnchors[0],
            result.attemptedAnchors[1], result.attemptedAnchors[2],
            static_cast<std::uint32_t>(result.failure), result.nextRetryAt != 0,
            result.nextRetryAt, arbiterReleaseSucceeded, nativeInteractionObserved);
    }
    if (botGuid != 0)
        StopOwnedGatherMotion(botGuid);
    state.lease = {};
    state.leaseIdentity = {};
}

bool Runtime::RefreshOracleOwnership(Guid botGuid, IntentLease const& lease) const noexcept
{
    if (botGuid == 0 || !lease.valid || lease.decision.operation != OperationCode::GatherSource ||
        lease.decision.decisionId == 0)
        return false;

    Tick const now = CurrentTick();
    Tick const extension = static_cast<Tick>(std::max<std::uint32_t>(config_.cadenceMs, 1000));
    if (now > std::numeric_limits<Tick>::max() - extension)
        return false;

    // Claim is an upsert for this bot. Reasserting it only after the arbiter lease is renewed
    // keeps the native ownership gate alive for the whole GatherSource lease without granting it
    // an independent lifetime.
    return ClaimGather(botGuid, lease.decision.decisionId, lease.decision.gather,
                       now + extension) &&
        OwnsGatherSource(botGuid, lease.decision.decisionId, lease.decision.gather, now);
}

Receipt Runtime::MakeExecutorReceipt(Decision const& decision,
                                      AutoWowOracleQuestExecutor::DispatchResult const& result) const noexcept
{
    bool const nativeUnavailable =
        result.reason == AutoWowOracleQuestExecutor::QuestObjectiveReason::NativeUnavailable ||
        result.reason == AutoWowOracleQuestExecutor::QuestObjectiveReason::ExecutorUnavailable;
    Receipt receipt;
    receipt.status = result.accepted
        ? (result.status == AutoWowOracleExecutor::ExecutorStatus::Completed
            ? AutoWowOracle::ReceiptStatus::Completed
            : AutoWowOracle::ReceiptStatus::Progressing)
        : (result.status == AutoWowOracleExecutor::ExecutorStatus::Blocked
            ? AutoWowOracle::ReceiptStatus::Blocked
            : AutoWowOracle::ReceiptStatus::Failed);
    receipt.reason = result.accepted
        ? AutoWowOracle::ReceiptReason::None
        : (result.status == AutoWowOracleExecutor::ExecutorStatus::Blocked || nativeUnavailable
            ? AutoWowOracle::ReceiptReason::ExecutorBlocked
            : AutoWowOracle::ReceiptReason::ExecutorFailed);
    receipt.decisionId = decision.decisionId;
    receipt.intentId = decision.intentId;
    receipt.domain = decision.domain;
    receipt.resource = decision.resource;
    receipt.scope = decision.scope;
    receipt.frameVersion = result.receipt.valid ? result.receipt.frameVersion
                                                : decision.preconditions.frameVersion;
    receipt.epoch = decision.epoch;
    receipt.before = result.receipt.valid ? result.receipt.proof.before : decision.evidence;
    receipt.after = result.receipt.valid ? result.receipt.proof.after : decision.evidence;
    receipt.objectiveId = decision.objectiveId;
    receipt.rallyPointId = decision.rallyPointId;
    receipt.actorGuid = decision.actorGuid;
    receipt.ownerGuid = decision.ownerGuid;
    receipt.targetGuid = decision.targetGuid;
    receipt.itemId = decision.itemId;
    receipt.role = decision.role;
    receipt.plannerMode = decision.plannerMode;
    receipt.plannerReason = decision.plannerReason;
    receipt.executorAvailable = decision.executorAvailable;
    receipt.operation = decision.operation;
    receipt.quest = decision.quest;
    receipt.gather = decision.gather;
    return receipt;
}

Receipt Runtime::MakeGatherExecutorReceipt(
    Decision const& decision,
    AutoWowOracleGatherExecutor::DispatchResult const& result) const noexcept
{
    bool const blocked = result.status == AutoWowOracleExecutor::ExecutorStatus::Blocked;
    bool const completed = result.accepted &&
        result.status == AutoWowOracleExecutor::ExecutorStatus::Completed;
    bool const progressing = result.accepted &&
        result.status == AutoWowOracleExecutor::ExecutorStatus::Progressing;

    Receipt receipt;
    receipt.status = completed ? AutoWowOracle::ReceiptStatus::Completed :
        progressing ? AutoWowOracle::ReceiptStatus::Progressing :
        blocked ? AutoWowOracle::ReceiptStatus::Blocked : AutoWowOracle::ReceiptStatus::Failed;
    receipt.reason = completed || progressing ? AutoWowOracle::ReceiptReason::None :
        blocked ? AutoWowOracle::ReceiptReason::ExecutorBlocked :
                  AutoWowOracle::ReceiptReason::ExecutorFailed;
    receipt.decisionId = decision.decisionId;
    receipt.intentId = decision.intentId;
    receipt.domain = decision.domain;
    receipt.resource = decision.resource;
    receipt.scope = decision.scope;
    receipt.frameVersion = result.after.world.frameVersion != 0 ? result.after.world.frameVersion :
        decision.preconditions.frameVersion;
    receipt.epoch = decision.epoch;
    receipt.before = result.before.world.evidence;
    receipt.after = result.after.world.evidence;
    receipt.actorGuid = decision.actorGuid;
    receipt.ownerGuid = decision.ownerGuid;
    receipt.targetGuid = decision.targetGuid;
    receipt.itemId = decision.itemId;
    receipt.role = decision.role;
    receipt.plannerMode = decision.plannerMode;
    receipt.plannerReason = decision.plannerReason;
    receipt.executorAvailable = decision.executorAvailable;
    receipt.operation = decision.operation;
    receipt.quest = decision.quest;
    receipt.gather = decision.gather;
    return receipt;
}

void Runtime::ProcessConfiguredGatherBot(Guid botGuid)
{
    // GatherSource has its own live frame and executor lifecycle. It is reached for an active
    // GatherSource lease or after the quest frame fails, and only when the worker has explicitly
    // published one exact source.
    BotState* state = state_.FindOrCreate(botGuid);
    if (!state)
        return;

    ActiveLeaseSnapshot proof = arbiter_.QueryActiveLease(botGuid, tick_);
    FactVersion const version = ++frameVersion_;
    LiveGatherFrame live;
    if (!BuildLiveGatherFrame(botGuid, tick_, version, epoch_, proof, live))
    {
        InvalidateOracleSnapshot(botGuid);
        if (state->lease.valid)
            ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));
        else
        {
            Release(botGuid);
            StopOwnedGatherMotion(botGuid);
        }
        state_.Erase(botGuid);
        return;
    }
    PublishGatherOracleSnapshot(botGuid, live);

    // This runtime owns one operation lease per bot. Do not try to renew a QuestObjective lease
    // against a GatherSource frame (or silently leave it behind); release the old domain before
    // the deliberate GatherSource plan is acquired.
    if (state->lease.valid && state->lease.decision.operation != OperationCode::GatherSource)
        ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));

    IntentLease lease;
    if (state->lease.valid)
    {
        AutoWowOracle::LeaseResult const renewed = arbiter_.Renew(live.frame, state->lease);
        Record(renewed.receipt);
        if (!renewed.hasLease)
        {
            Release(botGuid);
            StopOwnedGatherMotion(botGuid);
            state->lease = {};
            state_.Erase(botGuid);
            return;
        }
        state->lease = renewed.lease;
        lease = renewed.lease;
    }
    else
    {
        AutoWowOracle::PlanResult const plan = AutoWowOracle::Plan(live.frame);
        Record(plan.receipt);
        if (!plan.hasDecision)
        {
            // A published source that cannot produce a valid GatherSource decision is blocked.
            // Do not recurse into this method: recursion could repeatedly re-enter planning and
            // would make an insufficient frame look like a live production lease.
            Release(botGuid);
            StopOwnedGatherMotion(botGuid);
            state_.Erase(botGuid);
            return;
        }

        AutoWowOracle::LeaseResult const acquired = arbiter_.Acquire(live.frame, plan.decision);
        Record(acquired.receipt);
        if (!acquired.hasLease)
        {
            Release(botGuid);
            StopOwnedGatherMotion(botGuid);
            state_.Erase(botGuid);
            return;
        }
        state->lease = acquired.lease;
        lease = acquired.lease;
    }

    proof = arbiter_.QueryActiveLease(botGuid, tick_);
    if (!SameLease(proof, lease, tick_))
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    live.world.ownershipProof = proof;
    AutoWowOracle::Receipt const nodeReceipt = arbiter_.ReserveNode(live.frame, lease);
    Record(nodeReceipt);
    if (nodeReceipt.status != AutoWowOracle::ReceiptStatus::Accepted)
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    AutoWowOracleExecutor::ExecutorResult const prepared =
        AutoWowOracleExecutor::PrepareRequest(lease, proof, tick_);
    if (!prepared.accepted || prepared.request.gatherGoal != live.goal)
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    if (!RefreshOracleOwnership(botGuid, lease))
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (bot)
    {
        AutoWowGatherRuntimePolicy::ExactMotionProvenance const provenance =
            AutoWowGather::GetOracleMotionProvenance(static_cast<std::uint32_t>(botGuid));
        bool const currentExactMotion = AutoWowGatherRuntimePolicy::MatchesExactMotionProvenance(
            provenance, lease.decision.decisionId, lease.decision.gather);
        AutoWowGatherRuntimePolicy::ExactMotionDecision const motion =
            AutoWowGatherRuntimePolicy::EvaluateExactMotion(
                {true, bot->isMoving(), currentExactMotion});
        if (motion.clearInheritedMotion)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            AutoWowGather::ClearOracleMotionProvenance(static_cast<std::uint32_t>(botGuid));
        }
    }
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    AutoWowOracleGatherExecutor::NodeReservationProof const nodeReservation{nodeReceipt,
        lease.decision.expiresTick};
    AutoWowOracleGatherExecutor::DispatchResult const dispatched =
        WorkerGatherAction::ExecuteFixedTargetGather(
            botAI, prepared.request, lease, live.world, nodeReservation, live.candidate);
    Record(MakeGatherExecutorReceipt(lease.decision, dispatched));

    if (!dispatched.accepted ||
        dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Completed ||
        dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Failed ||
        dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Blocked ||
        dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Rejected)
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    AutoWowOracle::WorldReadFrame renewalFrame = live.frame;
    renewalFrame.version = dispatched.after.world.frameVersion;
    renewalFrame.tick = tick_;
    renewalFrame.epoch = epoch_;
    renewalFrame.evidence = dispatched.after.world.evidence;
    AutoWowOracle::LeaseResult const renewed = arbiter_.Renew(renewalFrame, state->lease);
    Record(renewed.receipt);
    struct GatherRenewalContext
    {
        Runtime* runtime = nullptr;
        AutoWowOracle::WorldReadFrame const* frame = nullptr;
        Guid botGuid = 0;
    } renewalContext{this, &renewalFrame, botGuid};
    auto const refreshGate = [](void* raw, IntentLease const& renewedLease) noexcept -> bool
    {
        GatherRenewalContext* context = static_cast<GatherRenewalContext*>(raw);
        return context && context->runtime &&
            context->runtime->RefreshOracleOwnership(context->botGuid, renewedLease);
    };
    auto const releaseRenewedLease = [](void* raw, IntentLease const& renewedLease) noexcept
    {
        GatherRenewalContext* context = static_cast<GatherRenewalContext*>(raw);
        if (context && context->runtime && context->frame)
            context->runtime->Record(
                context->runtime->arbiter_.Release(*context->frame, renewedLease));
    };
    AutoWowGatherRuntimePolicy::RenewalCommitResult const committed =
        AutoWowGatherRuntimePolicy::CommitRenewedGatherLease(
            renewed, &renewalContext, refreshGate, releaseRenewedLease);
    if (committed.keepRenewedLease)
        state->lease = renewed.lease;
    else
    {
        Release(botGuid);
        StopOwnedGatherMotion(botGuid);
        state->lease = {};
        state_.Erase(botGuid);
    }
}

AutoWowOracleQuestExecutor::NativeStepObservation Runtime::NativeDispatch(
    void* context, AutoWowOracleQuestExecutor::NativeObjectiveStepRequest const& request) noexcept
{
    NativeDispatchContext* dispatch = static_cast<NativeDispatchContext*>(context);
    AutoWowOracleQuestExecutor::NativeStepObservation result;
    if (!dispatch || !dispatch->runtime || dispatch->botGuid == 0)
        return result;

    Player* bot = ObjectAccessor::FindPlayer(
        ObjectGuid::Create<HighGuid::Player>(dispatch->botGuid));
    PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!bot || !ai)
        return result;

    result.stepsInvoked = 1;
    result.dispatchAccepted = ai->DoSpecificAction(
        "new rpg do quest",
        Event(std::string(TaggedEventSource()),
              std::to_string(request.request.decisionId), bot), true);

    Runtime::LiveQuestFrame after;
    ActiveLeaseSnapshot const proof = dispatch->runtime->arbiter_.QueryActiveLease(
        dispatch->botGuid, dispatch->runtime->tick_);
    FactVersion const version = ++dispatch->runtime->frameVersion_;
    if (dispatch->runtime->BuildLiveQuestFrame(dispatch->botGuid, dispatch->runtime->tick_,
                                               version, dispatch->runtime->epoch_, proof, after))
    {
        result.after = after.observation;
        dispatch->runtime->PublishQuestOracleSnapshot(dispatch->botGuid, after);
    }
    else
        dispatch->runtime->InvalidateOracleSnapshot(dispatch->botGuid);
    return result;
}

void Runtime::ProcessConfiguredBot(Guid botGuid)
{
    // This is the only per-bot live boundary. All eligibility reads and the bounded
    // Plan -> lease -> Prepare/finisher-frame -> tagged native dispatch sequence stay on the world thread.
    BotState* state = state_.FindOrCreate(botGuid);
    if (!state)
        return;

    // Once GatherSource owns the bot, keep that lease as the driving domain until the gather
    // executor reports a terminal result, pause/invalid-frame, or lease failure. Do not let a
    // newly readable quest frame preempt the live exact-source lease between runtime ticks.
    if (state->lease.valid && state->lease.decision.operation == OperationCode::GatherSource)
    {
        ProcessConfiguredGatherBot(botGuid);
        return;
    }

    if (!EnsureQuestDirective(botGuid, *state))
    {
        ProcessConfiguredGatherBot(botGuid);
        return;
    }

    ActiveLeaseSnapshot proof = arbiter_.QueryActiveLease(botGuid, tick_);
    FactVersion const version = ++frameVersion_;
    LiveQuestFrame live;
    if (!BuildLiveQuestFrame(botGuid, tick_, version, epoch_, proof, live))
    {
        InvalidateOracleSnapshot(botGuid);
        if (state->lease.valid)
        {
            ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));
            state_.Erase(botGuid);
        }
        ProcessConfiguredGatherBot(botGuid);
        return;
    }
    PublishQuestOracleSnapshot(botGuid, live);

    // Any stale non-quest/non-gather lease is not eligible for quest renewal. An active
    // GatherSource lease was handled above and must not be released by this path.
    if (state->lease.valid && state->lease.decision.operation != OperationCode::QuestObjective &&
        state->lease.decision.operation != OperationCode::GatherSource)
        ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));

    AutoWowOracleExactHandoff::Facts pendingExactAcquireHandoff;
    AutoWowOracleRoute::RouteSession pendingExactRouteSession;
    AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff pendingExactRouteHandoff;
    bool hasPendingExactAcquireHandoff = false;

    // A lease is reusable only for the exact same intent frame. A quest can remain selected while
    // its native phase, target, or finisher identity changes; releasing here forces a fresh plan
    // and prevents the current finisher from being dispatched under the old decision id.
    if (state->lease.valid && !SameIntentIdentity(state->leaseIdentity, live.identity))
    {
        // Capture only an arrived, exact DB source under the old Travel decision. ReleaseLease
        // must still clear the mutable pin; a fresh authorized decision restores it below.
        AutoWowOracleExactHandoff::Facts pendingExactAcquire;
        AutoWowOracleRoute::RouteSession pendingRouteSession;
        AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff pendingRouteHandoff;
        if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid)))
        {
            if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            {
                if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
                {
                    auto* value = ai->GetAiObjectContext()
                        ? ai->GetAiObjectContext()->GetValue<QuestObjectiveSpec>("active quest objective")
                        : nullptr;
                    if (value)
                    {
                        QuestObjectiveSpec const spec = value->Get();
                        pendingExactAcquire = ReadExactAcquireFacts(
                            bot, botGuid, spec, doQuest->objectiveRuntime,
                            state->leaseIdentity, state->lease.decision.decisionId,
                            pendingRouteHandoff);
                        pendingRouteSession = doQuest->objectiveRuntime.oracleRouteSession;
                    }
                }
            }
        }
        bool const exactAcquireCandidate = IsArrivedExactAcquire(pendingExactAcquire) &&
            live.identity.targetGuid == pendingExactAcquire.selectedSpawn;
        ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));
        if (exactAcquireCandidate)
        {
            // This is local to this cadence; any failed plan, grant, or gate leaves the pin
            // cleared. Preserve the value until after all ordinary lease checks succeed.
            pendingExactAcquireHandoff = pendingExactAcquire;
            pendingExactRouteSession = pendingRouteSession;
            pendingExactRouteHandoff = pendingRouteHandoff;
            hasPendingExactAcquireHandoff = true;
        }
        // Keep bot-level retry and no-progress history while replacing only the stale lease.
        // Erasing this slot on each native phase transition reset the bounded re-arm budget.
    }

    AutoWowOracle::IntentLease lease;
    if (state->lease.valid)
    {
        AutoWowOracle::LeaseResult const renewed = arbiter_.Renew(live.frame, state->lease);
        Record(renewed.receipt);
        if (!renewed.hasLease)
        {
            ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));
            state_.Erase(botGuid);
            return;
        }
        state->lease = renewed.lease;
        state->leaseIdentity = live.identity;
        lease = renewed.lease;
    }
    else
    {
        AutoWowOracle::PlanResult const plan = AutoWowOracle::Plan(live.frame);
        Record(plan.receipt);
        if (!plan.hasDecision)
        {
            // No eligible quest objective is a terminal blocked tick. The separate gather path is
            // entered only from an invalid quest frame, never recursively from this branch. Keep
            // the bounded state slot so this diagnostic is rate-limited across blocked ticks.
            if (state->lastNoEligibleTelemetryTick == 0 ||
                tick_ < state->lastNoEligibleTelemetryTick ||
                tick_ - state->lastNoEligibleTelemetryTick >= kNoEligibleTelemetryInterval)
            {
                LOG_DEBUG("playerbots",
                    "[AutoWow Oracle] no eligible quest bot={} quest={} phase={} source_entry={} "
                    "target_guid={} candidate_count={} reason={}",
                    botGuid, live.observation.objective.questId, live.identity.phase,
                    live.sourceEntry, live.observation.objective.targetGuid,
                    live.frame.candidateCount, AutoWowOracle::ReceiptReasonName(plan.receipt.reason));
                state->lastNoEligibleTelemetryTick = tick_;
            }
            ReleaseLease(*state, live.frame);
            return;
        }
        state->lastNoEligibleTelemetryTick = 0;

        AutoWowOracle::LeaseResult const acquired = arbiter_.Acquire(live.frame, plan.decision);
        Record(acquired.receipt);
        if (!acquired.hasLease)
        {
            state_.Erase(botGuid);
            return;
        }
        state->lease = acquired.lease;
        state->leaseIdentity = live.identity;
        lease = acquired.lease;
    }

    proof = arbiter_.QueryActiveLease(botGuid, tick_);
    if (!SameLease(proof, lease, tick_))
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    live.observation.world.ownership = proof;
    AutoWowOracleQuestExecutor::PreparedObjective prepared;
    if (!live.finisherMode)
    {
        prepared = AutoWowOracleQuestExecutor::OracleQuestExecutor::Prepare(lease, live.observation);
        if (!prepared.accepted)
        {
            ReleaseLease(*state, live.frame);
            state_.Erase(botGuid);
            return;
        }
    }

    AutoWowOracle::Tick const gateNow = CurrentTick();
    // Keep the native gate alive across the whole expected arbiter lease window. It is renewed
    // here on every cadence and released only by ReleaseLease; HasActiveLease remains the
    // authoritative arbiter check if the process stalls or the gate expires first.
    AutoWowOracle::Tick const gateExpiry = GateExpiryFor(config_, gateNow);
    bool const claimed = Claim(botGuid, lease.decision.decisionId, gateExpiry);
    if (!claimed || !Owns(botGuid, lease.decision.decisionId, CurrentTick()))
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    // Mirror the central lease authority into DoQuest for phase-local diagnostics and for the
    // native action's finisher checks. The central Runtime::RequiresTaggedDispatch query remains
    // the durable guard even if another path resets DoQuest's mutable data.
    bool leaseMarkerSet = false;
    if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid)))
    {
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            if (auto* doQuest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
            {
                AutoWowOracleQuestDispatchPolicy::OwnershipState const acquired =
                    AutoWowOracleQuestDispatchPolicy::Acquired(true, lease.decision.decisionId);
                doQuest->objectiveRuntime.oracleManaged = acquired.managed;
                doQuest->objectiveRuntime.oracleLeaseRequired = acquired.leaseRequired;
                doQuest->objectiveRuntime.oracleLeaseDecisionId = acquired.activeDecisionId;
                leaseMarkerSet = true;
            }
        }
    }
    if (!leaseMarkerSet)
    {
        ReleaseLease(*state, live.frame);
        state_.Erase(botGuid);
        return;
    }

    if (hasPendingExactAcquireHandoff)
    {
        AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff released;
        bool const releasedExactRoute =
            AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, released) &&
            released.lease == pendingExactAcquireHandoff.oldDecision &&
            released.result.routeKey.stableSpawn == pendingExactAcquireHandoff.selectedSpawn &&
            released.arrived && !released.blocked && released.leaseReleaseAuthoritative;
        bool const grantAllowsRestore = AutoWowOracleExactHandoff::MayRestore(
            pendingExactAcquireHandoff,
            static_cast<std::uint8_t>(QuestActionPhase::TravelToSource),
            static_cast<std::uint8_t>(QuestActionPhase::AcquireTarget),
            releasedExactRoute, state->lease.valid && SameLease(proof, lease, tick_),
            lease.decision.decisionId, lease.decision.targetGuid);
        if (grantAllowsRestore)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
            PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
            auto* doQuest = ai ? std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data) : nullptr;
            if (bot && doQuest && doQuest->questId == pendingExactAcquireHandoff.newQuest &&
                bot->GetMapId() == pendingExactAcquireHandoff.currentMap &&
                bot->GetInstanceId() == pendingExactAcquireHandoff.currentInstance &&
                doQuest->objectiveRuntime.phase == QuestActionPhase::AcquireTarget &&
                doQuest->objectiveRuntime.selectedSourceEntry > 0 &&
                doQuest->objectiveRuntime.selectedSourceSpawn.GetRawValue() ==
                    pendingExactAcquireHandoff.selectedSpawn)
            {
                AutoWowOracleRoute::RouteStep rebound;
                rebound.session = pendingExactRouteSession;
                rebound.result = pendingExactRouteHandoff.result;
                rebound.session.state.routeKey.job = lease.decision.intentId;
                rebound.session.state.routeKey.lease = lease.decision.decisionId;
                rebound.session.state.releaseLease = false;
                rebound.session.state.leaseReleaseAuthoritative = false;
                rebound.result.routeKey.job = lease.decision.intentId;
                rebound.result.routeKey.lease = lease.decision.decisionId;
                rebound.result.command.routeKey.job = lease.decision.intentId;
                rebound.result.command.routeKey.lease = lease.decision.decisionId;
                rebound.result.releaseLease = false;
                rebound.result.leaseReleaseAuthoritative = false;
                if (AutoWowOracleQuestExecutor::PublishQuestRouteHandoff(
                        botGuid, rebound, released.blockedReentryFacts))
                {
                    QuestObjectiveRuntime& rt = doQuest->objectiveRuntime;
                    rt.oracleRouteSession = rebound.session;
                    rt.oracleRouteReceipt = rebound.result;
                    rt.oracleRouteReceiptAvailable = true;
                    rt.oracleRouteDecisionId = lease.decision.decisionId;
                    rt.oracleRouteIdentityPinned = true;
                    LOG_DEBUG("playerbots", "[AutoWow Oracle] exact arrived source rebound bot={} quest={} spawn={} old_decision={} new_decision={}",
                        botGuid, doQuest->questId, pendingExactAcquireHandoff.selectedSpawn,
                        pendingExactAcquireHandoff.oldDecision, lease.decision.decisionId);
                }
            }
        }
    }

    if (live.finisherMode)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
        PlayerbotAI* ai = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        auto* finisherValue = ai && ai->GetAiObjectContext()
            ? ai->GetAiObjectContext()->GetValue<QuestFinisherRef>("active quest finisher")
            : nullptr;
        QuestFinisherRef const finisher = finisherValue ? finisherValue->Get() : QuestFinisherRef();
        AutoWowOracleFinisher::Intent const intent{
            lease.decision.decisionId,
            live.observation.objective.questId,
            finisher.signedEntry,
            finisher.stableSpawn.GetRawValue()};
        std::string const encodedIntent = AutoWowOracleFinisher::Encode(intent);
        bool const dispatchAccepted = bot && ai && !encodedIntent.empty() && ai->DoSpecificAction(
            "new rpg do quest",
            Event(std::string(TaggedEventSource()), encodedIntent, bot), true);
        LiveQuestFrame afterDispatch;
        ActiveLeaseSnapshot const afterProof = arbiter_.QueryActiveLease(botGuid, tick_);
        FactVersion const afterVersion = ++frameVersion_;
        if (BuildLiveQuestFrame(botGuid, tick_, afterVersion, epoch_, afterProof, afterDispatch))
            PublishQuestOracleSnapshot(botGuid, afterDispatch);
        else
            InvalidateOracleSnapshot(botGuid);
        bool const rewardConfirmed = bot && bot->GetQuestRewardStatus(live.observation.objective.questId);

        AutoWowOracle::EvidenceCounters after = live.frame.evidence;
        if (rewardConfirmed)
        {
            after.objective = after.objective + 1;
            after.progress = after.progress + 1;
        }
        Record(AutoWowOracle::Detail::MakeReceipt(
            lease.decision,
            rewardConfirmed ? AutoWowOracle::ReceiptStatus::Completed
                            : dispatchAccepted ? AutoWowOracle::ReceiptStatus::Progressing
                                                : AutoWowOracle::ReceiptStatus::Failed,
            dispatchAccepted ? AutoWowOracle::ReceiptReason::None
                              : AutoWowOracle::ReceiptReason::ExecutorFailed,
            live.frame.version, live.frame.evidence, after));

        LOG_DEBUG("playerbots", "[AutoWow Oracle] {} quest {} finisher dispatch accepted={} exact_entry={} stable_guid={} core_reward_confirmed={}",
                  bot ? bot->GetName() : "offline", live.observation.objective.questId,
                  dispatchAccepted, finisher.signedEntry, finisher.stableSpawn.GetRawValue(), rewardConfirmed);
        if (!dispatchAccepted || rewardConfirmed)
        {
            ReleaseLease(*state, live.frame);
            state_.Erase(botGuid);
        }
        return;
    }

    NativeDispatchContext context{this, botGuid};
    AutoWowOracleQuestExecutor::DispatchResult const dispatched =
        AutoWowOracleQuestExecutor::OracleQuestExecutor::Dispatch(
            prepared, live.observation, &Runtime::NativeDispatch, &context);
    Record(MakeExecutorReceipt(lease.decision, dispatched));

    if (!dispatched.accepted || dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Completed)
    {
        bool const preserveBlockedState =
            !dispatched.accepted &&
            dispatched.status == AutoWowOracleExecutor::ExecutorStatus::Blocked;
        AutoWowOracle::WorldReadFrame const cleanup = dispatched.afterValidated
            ? MakeCleanupFrame(botGuid, lease)
            : live.frame;
        ReleaseLease(*state, cleanup);
        // Keep the bounded re-arm counters for a blocked quest. Erasing the slot here would make
        // every subsequent cadence look like the first failure and would turn recovery into an
        // infinite same-quest loop. Terminal success/failure still drops the slot normally.
        if (!preserveBlockedState)
            state_.Erase(botGuid);
        return;
    }

    AutoWowOracle::WorldReadFrame renewalFrame = live.frame;
    if (dispatched.afterValidated)
    {
        renewalFrame.version = dispatched.receipt.frameVersion;
        renewalFrame.tick = tick_;
        renewalFrame.epoch = epoch_;
        renewalFrame.bot.guid = botGuid;
        renewalFrame.bot.mapId = dispatched.receipt.ownershipProof.scope.botGuid == botGuid
            ? live.observation.world.mapId
            : renewalFrame.bot.mapId;
        renewalFrame.bot.instanceId = live.observation.world.instanceId;
        renewalFrame.bot.alive = live.observation.world.botAlive;
    }

    AutoWowOracle::LeaseResult const renewed = arbiter_.Renew(renewalFrame, state->lease);
    Record(renewed.receipt);
    if (renewed.hasLease)
        state->lease = renewed.lease;
    else
    {
        ReleaseLease(*state, MakeCleanupFrame(botGuid, state->lease));
        state_.Erase(botGuid);
    }
}

void Runtime::Update(std::uint32_t diffMs)
{
    if (!configLoaded_)
        LoadConfig();
    if (!config_.enabled)
        return;

    if (!AdvanceCadence(cadence_, diffMs, config_.cadenceMs))
        return;

    ++tick_;
    std::size_t driven = 0;
    for (std::size_t index = 0; index < config_.botGuidCount && driven < config_.maxBots; ++index)
    {
        Guid const guid = config_.botGuids[index];
        if (!IsGuidAllowed(config_.botGuids, config_.botGuidCount, guid))
            continue;
        ++driven;
        ProcessConfiguredBot(guid);
    }
}

void Update(std::uint32_t diffMs)
{
    Runtime::instance().Update(diffMs);
}

bool RequiresTaggedDispatch(Guid botGuid) noexcept
{
    return Runtime::instance().RequiresTaggedDispatch(botGuid);
}

bool HasActiveLease(Guid botGuid, DecisionId decisionId) noexcept
{
    return Runtime::instance().HasActiveLease(botGuid, decisionId);
}

bool IsManagedBot(Guid botGuid)
{
    return Runtime::instance().IsManagedBot(botGuid);
}

DecisionId ActiveDecisionId(Guid botGuid) noexcept
{
    return Runtime::instance().ActiveDecisionId(botGuid);
}

bool GetOracleSnapshot(Guid botGuid, OracleBotSnapshot& out)
{
    return Runtime::instance().GetOracleSnapshot(botGuid, out);
}

AutoWowOracleReceiptStore::StreamStatus GetOracleStreamStatus()
{
    return Runtime::instance().GetOracleStreamStatus();
}

}  // namespace AutoWowOracleRuntime
