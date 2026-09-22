#include "gtest/gtest.h"

#include "../src/AutoWow/AutoWowOracleRuntime.h"
#include "../src/AutoWow/AutoWowOracleExactHandoffPolicy.h"
#include "../src/AutoWow/AutoWowOracleQuestSelectionPolicy.h"
#include "../src/Ai/World/Rpg/QuestObjectiveContext.h"

#include <array>
#include <utility>

namespace
{
using namespace AutoWowOracleRuntime;

TEST(AutoWowOracleRuntimePolicyTest, DefersOnlyAuthoritativelyUnrunnableActiveQuest)
{
    QuestSelectionFacts facts;
    facts.incomplete = true;
    facts.resolverAvailable = true;
    // The resolver found no supported incomplete objective and the core cannot hand in.
    EXPECT_TRUE(ShouldDeferUnrunnableQuest(facts));

    // A native objective, including a source-item conversion resolved as UseQuestItem,
    // must retain its directive for normal objective work.
    facts.runnableObjective = true;
    EXPECT_FALSE(ShouldDeferUnrunnableQuest(facts));
    facts.runnableObjective = false;

    // Incomplete status can still be core-ready for the normal finisher transition.
    facts.coreReadyForFinisher = true;
    EXPECT_FALSE(ShouldDeferUnrunnableQuest(facts));
    facts.coreReadyForFinisher = false;

    // Missing resolver infrastructure is unknown, not evidence of unsupported work.
    facts.resolverAvailable = false;
    EXPECT_FALSE(ShouldDeferUnrunnableQuest(facts));
    facts.resolverAvailable = true;
    facts.incomplete = false;
    EXPECT_FALSE(ShouldDeferUnrunnableQuest(facts));
}

TEST(AutoWowOracleRuntimePolicyTest, BlockedRetrySurvivesPhaseChangesUntilCoreProgress)
{
    BotState state;
    state.blockedQuestId = 3521;
    state.blockedQuestRetries = 1;
    state.blockedObjective = {true, 3521,
        static_cast<std::uint8_t>(QuestObjectiveFamily::Item), 0, 0};
    BotState::ObjectiveProgress current = state.blockedObjective;

    // Native travel/loot phases and lease identities may change without objective credit.
    state.leaseIdentity.phase = static_cast<std::uint8_t>(QuestActionPhase::TravelToSource);
    EXPECT_TRUE(KeepBlockedQuestRetry(state.blockedQuestId, 3521, false,
                                      state.blockedObjective, current));
    state.leaseIdentity.phase = static_cast<std::uint8_t>(QuestActionPhase::LootSource);
    EXPECT_TRUE(KeepBlockedQuestRetry(state.blockedQuestId, 3521, false,
                                      state.blockedObjective, current));
    EXPECT_EQ(state.blockedQuestRetries, 1u);

    current.count = 1; // Real core item credit starts a new retry budget.
    EXPECT_FALSE(KeepBlockedQuestRetry(state.blockedQuestId, 3521, false,
                                       state.blockedObjective, current));
    current = state.blockedObjective;
    EXPECT_FALSE(KeepBlockedQuestRetry(state.blockedQuestId, 3522, false,
                                       state.blockedObjective, current));
    EXPECT_FALSE(KeepBlockedQuestRetry(state.blockedQuestId, 3521, true,
                                       state.blockedObjective, current));
    current.slot = 1;
    EXPECT_FALSE(KeepBlockedQuestRetry(state.blockedQuestId, 3521, false,
                                       state.blockedObjective, current));
}

GuidPosition MakeCreatureSpawn(std::uint32_t spawnId, std::uint16_t mapId, std::uint32_t entry)
{
    CreatureData data;
    data.id = entry;
    data.spawnId = spawnId;
    data.mapid = mapId;
    return GuidPosition(data);
}

GuidPosition MakeGameObjectSpawn(std::uint32_t spawnId, std::uint16_t mapId, std::uint32_t entry)
{
    GameObjectData data;
    data.id = entry;
    data.spawnId = spawnId;
    data.mapid = mapId;
    return GuidPosition(data);
}

QuestObjectiveSpec MakeBootstrapSpec()
{
    QuestObjectiveSpec spec;
    spec.key.questId = 827;
    spec.key.family = QuestObjectiveFamily::NpcOrGameObject;
    spec.key.slot = 0;
    spec.kind = QuestObjectiveKind::CreatureCredit;
    spec.requiredNpcOrGoEntry = 3208;
    spec.currentCount = 0;
    spec.requiredCount = 6;
    spec.supported = true;

    QuestObjectiveSource source;
    source.type = QuestObjectiveSource::Type::Creature;
    source.entry = 3208;
    source.spawns.emplace_back(MakeCreatureSpawn(9002, 1, 3208));
    source.spawns.emplace_back(MakeCreatureSpawn(7002, 530, 3208));
    source.spawns.emplace_back(MakeCreatureSpawn(7001, 530, 3208));
    spec.sources.emplace_back(std::move(source));
    return spec;
}

QuestObjectiveSpec MakeGameObjectBootstrapSpec()
{
    QuestObjectiveSpec spec;
    spec.key.questId = 8330;
    spec.key.family = QuestObjectiveFamily::NpcOrGameObject;
    spec.key.slot = 0;
    spec.kind = QuestObjectiveKind::GameObjectCredit;
    spec.requiredNpcOrGoEntry = -180510;
    spec.currentCount = 0;
    spec.requiredCount = 1;
    spec.supported = true;

    QuestObjectiveSource source;
    source.type = QuestObjectiveSource::Type::GameObject;
    source.entry = 180510;
    source.spawns.emplace_back(MakeGameObjectSpawn(21117, 530, 180510));
    spec.sources.emplace_back(std::move(source));
    return spec;
}

TEST(AutoWowOracleRuntimePolicyTest, ArrivedExactSourceOverridesDifferentGenericBootstrap)
{
    using namespace AutoWowOracleExactHandoff;
    QuestObjectiveSpec const spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const generic = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0);
    ASSERT_TRUE(generic.valid);
    std::uint64_t const selected = MakeCreatureSpawn(7002, 530, 3208).GetRawValue();
    ASSERT_NE(generic.targetGuid, selected); // The real q916 handoff mismatch shape.

    Facts f;
    f.pinned = f.sourceInObjective = f.runtimeTargetMatches = f.arrived = true;
    f.oldPhase = static_cast<std::uint8_t>(QuestActionPhase::TravelToSource);
    f.newPhase = static_cast<std::uint8_t>(QuestActionPhase::AcquireTarget);
    f.oldQuest = f.newQuest = 827;
    f.oldFamily = f.newFamily = static_cast<std::uint8_t>(QuestObjectiveFamily::NpcOrGameObject);
    f.oldSlot = f.newSlot = 0;
    f.routeEntry = f.selectedEntry = 3208;
    f.routeSpawn = f.selectedSpawn = selected;
    f.routeMap = f.currentMap = 530;
    f.routeInstance = f.currentInstance = 1;
    f.oldDecision = f.routeDecision = 19;

    auto const travel = f.oldPhase;
    auto const acquire = f.newPhase;
    auto const engage = static_cast<std::uint8_t>(QuestActionPhase::EngageTarget);
    ASSERT_TRUE(SameArrivedSource(f, travel, acquire));
    EXPECT_EQ(SelectLiveTarget(generic.targetGuid, f,
        UsePinnedTarget(f, travel, acquire, engage)), selected);
    EXPECT_TRUE(MayRestore(f, travel, acquire, true, true, 20, selected));
    EXPECT_FALSE(MayRestore(f, travel, acquire, false, true, 20, selected));
    EXPECT_FALSE(MayRestore(f, travel, acquire, true, false, 20, selected));
    EXPECT_FALSE(MayRestore(f, travel, acquire, true, true, 19, selected));
    EXPECT_FALSE(MayRestore(f, travel, acquire, true, true, 20, generic.targetGuid));

    // The same pinned source stays stable in the native post-step observation even after
    // BindExact replaces a stable DB GUID with the matching runtime object GUID.
    f.oldPhase = acquire;
    f.newPhase = engage;
    EXPECT_EQ(SelectLiveTarget(generic.targetGuid, f,
        UsePinnedTarget(f, travel, acquire, engage)), selected);
    f.newPhase = acquire;
    EXPECT_EQ(SelectLiveTarget(generic.targetGuid, f,
        UsePinnedTarget(f, travel, acquire, engage)), selected);

    f.routeSpawn = generic.targetGuid;
    EXPECT_FALSE(UsePinnedTarget(f, travel, acquire, engage));
    EXPECT_EQ(SelectLiveTarget(generic.targetGuid, f,
        UsePinnedTarget(f, travel, acquire, engage)), generic.targetGuid);
    f.routeSpawn = selected;
    f.runtimeTargetMatches = false;
    EXPECT_FALSE(UsePinnedTarget(f, travel, acquire, engage));
    f.runtimeTargetMatches = true;
    f.routeInstance = 2;
    EXPECT_FALSE(UsePinnedTarget(f, travel, acquire, engage));
    f.routeInstance = 1;
    f.routeDecision = 18;
    EXPECT_FALSE(UsePinnedTarget(f, travel, acquire, engage));
}

AutoWowOracleQuestExecutor::NativeStepObservation StableResolverStep(
    void*, AutoWowOracleQuestExecutor::NativeObjectiveStepRequest const& request)
{
    AutoWowOracleQuestExecutor::NativeStepObservation result;
    result.dispatchAccepted = true;
    result.stepsInvoked = 1;
    result.after.world.onWorldThread = true;
    result.after.world.botAlive = true;
    result.after.world.botGuid = request.request.actorGuid;
    result.after.world.mapId = request.objectiveRuntime.targetMapId;
    result.after.world.instanceId = 1;
    result.after.world.tick = request.request.issuedTick + 1;
    result.after.world.frameVersion = request.request.frameVersion + 1;
    result.after.world.epoch = request.request.epoch;
    result.after.world.ownership = request.request.ownershipProof;
    result.after.objective = request.objectiveBefore;
    result.after.objective.phase = AutoWowOracleQuestExecutor::ObjectiveStepPhase::TravelToSource;
    // Native ResolveObjective clears DoQuest::selectedTargetGuid. The runtime's post-frame builder
    // supplies the same bootstrap identity, so executor revalidation still sees one exact target.
    result.after.objective.targetGuid = request.objectiveRuntime.targetGuid;
    return result;
}

TEST(AutoWowOracleRuntimePolicyTest, ParsesOnlyStrictDecimalUniqueGuids)
{
    std::array<Guid, kMaxRuntimeBots> guids{};
    std::size_t count = 0;
    ASSERT_TRUE(ParseGuidAllowlist(" 77, 42, 9007199254740991\t77 42 ", guids, count));
    ASSERT_EQ(count, 3U);
    EXPECT_EQ(guids[0], 42U);
    EXPECT_EQ(guids[1], 77U);
    EXPECT_EQ(guids[2], 9007199254740991ULL);
    EXPECT_TRUE(IsGuidAllowed(guids, count, 77));
    EXPECT_FALSE(IsGuidAllowed(guids, count, 78));

    EXPECT_FALSE(ParseGuidAllowlist(",42", guids, count));
    EXPECT_FALSE(ParseGuidAllowlist("42,", guids, count));
    EXPECT_FALSE(ParseGuidAllowlist("42,,77", guids, count));
    EXPECT_FALSE(ParseGuidAllowlist("0", guids, count));
    EXPECT_FALSE(ParseGuidAllowlist("0x2a", guids, count));
    EXPECT_FALSE(ParseGuidAllowlist("18446744073709551616", guids, count));
    ASSERT_TRUE(ParseGuidAllowlist("42,42", guids, count));
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(guids[0], 42U);

    ASSERT_TRUE(ParseGuidAllowlist(" \t\r\n", guids, count));
    EXPECT_EQ(count, 0U);
}

TEST(AutoWowOracleRuntimePolicyTest, CadenceConsumesOneDueTickWithoutCatchup)
{
    CadenceClock clock;
    EXPECT_FALSE(AdvanceCadence(clock, 25, 100));
    EXPECT_EQ(clock.elapsedMs, 25U);
    EXPECT_TRUE(AdvanceCadence(clock, 1000, 100));
    EXPECT_EQ(clock.elapsedMs, 0U);
    EXPECT_FALSE(AdvanceCadence(clock, 99, 100));
    EXPECT_TRUE(AdvanceCadence(clock, 1, 100));
}

TEST(AutoWowOracleRuntimePolicyTest, ConfigBoundsAreStrictWhenEnabled)
{
    RuntimeConfig config;
    config.enabled = true;
    config.botGuidCount = 1;
    config.botGuids[0] = 42;
    EXPECT_TRUE(ValidateConfig(config));

    config.maxBots = static_cast<std::uint32_t>(AutoWowOracle::kMaxBotLeases + 1);
    EXPECT_FALSE(ValidateConfig(config));
    config.maxBots = 1;
    config.cadenceMs = kMinCadenceMs - 1;
    EXPECT_FALSE(ValidateConfig(config));
    config.cadenceMs = 1000;
    config.cadenceMs = kMaxCadenceMs;
    EXPECT_TRUE(ValidateConfig(config));
    config.cadenceMs = kMaxCadenceMs + 1;
    EXPECT_FALSE(ValidateConfig(config));
    config.cadenceMs = 1000;
    config.botGuidCount = 0;
    EXPECT_FALSE(ValidateConfig(config));
}

TEST(AutoWowOracleRuntimePolicyTest, StateAndReceiptStorageStayBounded)
{
    BoundedBotState state;
    for (std::size_t index = 0; index < AutoWowOracle::kMaxBotLeases; ++index)
        ASSERT_NE(state.FindOrCreate(index + 1), nullptr);
    EXPECT_EQ(state.size(), AutoWowOracle::kMaxBotLeases);
    EXPECT_EQ(state.FindOrCreate(AutoWowOracle::kMaxBotLeases + 1), nullptr);
    ASSERT_NE(state.Find(7), nullptr);
    state.Find(7)->lease.valid = true;
    EXPECT_EQ(state.activeLeaseCount(), 1U);
    EXPECT_TRUE(state.Erase(7));
    EXPECT_EQ(state.activeLeaseCount(), 0U);
    EXPECT_EQ(state.size(), AutoWowOracle::kMaxBotLeases - 1);
    EXPECT_NE(state.FindOrCreate(AutoWowOracle::kMaxBotLeases + 1), nullptr);
    EXPECT_EQ(state.size(), AutoWowOracle::kMaxBotLeases);

    ReceiptRing ring;
    for (std::size_t index = 0; index < ReceiptRing::capacity() + 3; ++index)
    {
        Receipt receipt;
        receipt.decisionId = index + 1;
        ring.Push(receipt);
    }
    ASSERT_EQ(ring.size(), ReceiptRing::capacity());
    ASSERT_NE(ring.At(0), nullptr);
    EXPECT_EQ(ring.At(0)->decisionId, 4U);
    EXPECT_EQ(ring.At(ring.size() - 1)->decisionId, ReceiptRing::capacity() + 3);
    EXPECT_EQ(ring.At(ring.size()), nullptr);

    ring.Clear();
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(ring.At(0), nullptr);

    state.Clear();
    EXPECT_EQ(state.size(), 0U);
    EXPECT_EQ(state.activeLeaseCount(), 0U);
}

TEST(AutoWowOracleRuntimePolicyTest, BootstrapSourceProducesAQuestCandidate)
{
    QuestObjectiveSpec const spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);

    ASSERT_TRUE(identity.valid);
    EXPECT_TRUE(identity.bootstrap);
    EXPECT_EQ(identity.targetGuid, MakeCreatureSpawn(7001, 530, 3208).GetRawValue());
    EXPECT_EQ(identity.mapId, 530U);
    EXPECT_EQ(identity.sourceEntry, 3208U);

    AutoWowOracle::WorldReadFrame frame;
    frame.version = 1;
    frame.tick = 1;
    frame.epoch = 1;
    frame.scope = {AutoWowOracle::ScopeKind::PersistentCampaign, 7, 7};
    frame.bot = {7, true, false, 530, 1};

    AutoWowOracle::OracleCandidate candidate;
    candidate.domain = AutoWowOracle::Domain::Quest;
    candidate.intent = AutoWowOracle::IntentCode::QuestObjective;
    candidate.resource = AutoWowOracle::LeaseResource::QuestGather;
    candidate.priority = 100;
    candidate.ttlTicks = 1;
    candidate.targetGuid = identity.targetGuid;
    candidate.action = "quest.objective";
    candidate.qualifier = "autowow.oracle.runtime";
    candidate.executorAvailable = true;
    candidate.actorGuid = 7;
    candidate.operation = AutoWowOracle::OperationCode::QuestObjective;
    candidate.quest = {true, spec.key.questId, AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject,
                       spec.key.slot, static_cast<std::uint32_t>(spec.requiredNpcOrGoEntry), 0};

    ASSERT_TRUE(AutoWowOracle::AddCandidate(frame, candidate));
    AutoWowOracle::PlanResult const planned = AutoWowOracle::Plan(frame);
    ASSERT_TRUE(planned.hasDecision);
    EXPECT_EQ(planned.decision.targetGuid, identity.targetGuid);
    EXPECT_EQ(planned.decision.operation, AutoWowOracle::OperationCode::QuestObjective);
}

TEST(AutoWowOracleRuntimePolicyTest, BootstrapIdentityRemainsStableAcrossResolverDispatchPhases)
{
    QuestObjectiveSpec const spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const before = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    QuestBootstrapIdentity const after = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::TravelToSource, 530, 0);
    QuestBootstrapIdentity const waiting = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::WaitForRespawn, 530, 0);

    ASSERT_TRUE(before.valid);
    ASSERT_TRUE(after.valid);
    ASSERT_TRUE(waiting.valid);
    EXPECT_TRUE(before.bootstrap);
    EXPECT_TRUE(after.bootstrap);
    EXPECT_TRUE(waiting.bootstrap);
    EXPECT_EQ(before.targetGuid, after.targetGuid);
    EXPECT_EQ(before.mapId, after.mapId);
    EXPECT_EQ(before.sourceEntry, after.sourceEntry);
    EXPECT_EQ(before.targetGuid, waiting.targetGuid);
    EXPECT_EQ(before.mapId, waiting.mapId);
    EXPECT_EQ(before.sourceEntry, waiting.sourceEntry);
    EXPECT_FALSE(waiting.sourceIsGameObject);
}

TEST(AutoWowOracleRuntimePolicyTest, GameObjectWaitPhaseKeepsStableSource)
{
    QuestObjectiveSpec const spec = MakeGameObjectBootstrapSpec();
    QuestBootstrapIdentity const resolving = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    QuestBootstrapIdentity const waiting = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::WaitForRespawn, 530, 0);

    ASSERT_TRUE(resolving.valid);
    ASSERT_TRUE(waiting.valid);
    EXPECT_TRUE(waiting.bootstrap);
    EXPECT_TRUE(waiting.sourceIsGameObject);
    EXPECT_EQ(waiting.targetGuid, resolving.targetGuid);
    EXPECT_EQ(waiting.mapId, resolving.mapId);
    EXPECT_EQ(waiting.sourceEntry, resolving.sourceEntry);
}

TEST(AutoWowOracleRuntimePolicyTest, PartialCreatureCreditCanReacquireStableSource)
{
    QuestObjectiveSpec spec = MakeBootstrapSpec();
    spec.currentCount = 4;
    QuestBootstrapIdentity const resolving = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    QuestBootstrapIdentity const acquiring = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0);

    ASSERT_TRUE(resolving.valid);
    ASSERT_TRUE(acquiring.valid);
    EXPECT_TRUE(acquiring.bootstrap);
    EXPECT_FALSE(acquiring.sourceIsGameObject);
    EXPECT_EQ(acquiring.targetGuid, resolving.targetGuid);
    EXPECT_EQ(acquiring.mapId, resolving.mapId);
    EXPECT_EQ(acquiring.sourceEntry, resolving.sourceEntry);
}

TEST(AutoWowOracleRuntimePolicyTest, PartialGameObjectCreditCanReacquireStableSource)
{
    QuestObjectiveSpec spec = MakeGameObjectBootstrapSpec();
    spec.currentCount = 1;
    spec.requiredCount = 2;
    QuestBootstrapIdentity const resolving = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    QuestBootstrapIdentity const acquiring = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0);

    ASSERT_TRUE(resolving.valid);
    ASSERT_TRUE(acquiring.valid);
    EXPECT_TRUE(acquiring.bootstrap);
    EXPECT_TRUE(acquiring.sourceIsGameObject);
    EXPECT_EQ(acquiring.targetGuid, resolving.targetGuid);
    EXPECT_EQ(acquiring.mapId, resolving.mapId);
    EXPECT_EQ(acquiring.sourceEntry, resolving.sourceEntry);
}

TEST(AutoWowOracleRuntimePolicyTest, AcquireTargetRejectsInvalidCreatureSource)
{
    QuestObjectiveSpec spec = MakeBootstrapSpec();
    spec.sources.front().spawns.clear();
    spec.sources.front().spawns.emplace_back(MakeCreatureSpawn(7001, 1, 3208));
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0).valid);

    spec.sources.front().spawns.front() = MakeGameObjectSpawn(7001, 530, 3208);
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0).valid);

    spec = MakeBootstrapSpec();
    spec.currentCount = spec.requiredCount;
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0).valid);
}

TEST(AutoWowOracleRuntimePolicyTest, GameObjectLootPhaseBootstrapsExactStableSource)
{
    QuestObjectiveSpec const spec = MakeGameObjectBootstrapSpec();
    GuidPosition const source = MakeGameObjectSpawn(21117, 530, 180510);
    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::LootSource, 530, 0);

    ASSERT_TRUE(identity.valid);
    EXPECT_TRUE(identity.bootstrap);
    EXPECT_TRUE(identity.sourceIsGameObject);
    EXPECT_EQ(identity.targetGuid, source.GetRawValue());
    EXPECT_EQ(identity.mapId, 530U);
    EXPECT_EQ(identity.sourceEntry, 180510U);

    AutoWowOracle::WorldReadFrame frame;
    frame.version = 1;
    frame.tick = 1;
    frame.epoch = 1;
    frame.scope = {AutoWowOracle::ScopeKind::PersistentCampaign, 7, 7};
    frame.bot = {7, true, false, 530, 1};

    AutoWowOracle::OracleCandidate candidate;
    candidate.domain = AutoWowOracle::Domain::Quest;
    candidate.intent = AutoWowOracle::IntentCode::QuestObjective;
    candidate.resource = AutoWowOracle::LeaseResource::QuestGather;
    candidate.priority = 100;
    candidate.ttlTicks = 1;
    candidate.targetGuid = identity.targetGuid;
    candidate.action = "quest.objective";
    candidate.qualifier = "autowow.oracle.runtime";
    candidate.executorAvailable = true;
    candidate.actorGuid = 7;
    candidate.operation = AutoWowOracle::OperationCode::QuestObjective;
    candidate.quest = {true, spec.key.questId,
                       AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject, spec.key.slot,
                       static_cast<std::uint32_t>(-spec.requiredNpcOrGoEntry), 0};

    ASSERT_TRUE(AutoWowOracle::AddCandidate(frame, candidate));
    AutoWowOracle::PlanResult const planned = AutoWowOracle::Plan(frame);
    ASSERT_TRUE(planned.hasDecision);
    EXPECT_EQ(planned.decision.targetGuid, source.GetRawValue());
    EXPECT_EQ(planned.decision.quest.questId, spec.key.questId);
    EXPECT_EQ(planned.decision.quest.requiredEntry, 180510U);
    EXPECT_EQ(planned.decision.preconditions.mapId, 530U);
    EXPECT_EQ(planned.decision.preconditions.instanceId, 1U);
    EXPECT_TRUE(planned.decision.preconditions.requireSameMap);
    EXPECT_EQ(planned.decision.actorGuid, 7U);
    EXPECT_EQ(planned.decision.operation, AutoWowOracle::OperationCode::QuestObjective);
}

TEST(AutoWowOracleRuntimePolicyTest, GameObjectBootstrapRejectsCrossMapSource)
{
    QuestObjectiveSpec spec = MakeGameObjectBootstrapSpec();
    spec.sources.front().spawns.front() = MakeGameObjectSpawn(21117, 1, 180510);

    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::LootSource, 530, 0);
    QuestBootstrapIdentity const waiting = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::WaitForRespawn, 530, 0);
    QuestBootstrapIdentity const acquiring = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0);
    EXPECT_FALSE(identity.valid);
    EXPECT_EQ(identity.targetGuid, 0U);
    EXPECT_FALSE(waiting.valid);
    EXPECT_EQ(waiting.targetGuid, 0U);
    EXPECT_FALSE(acquiring.valid);
}

TEST(AutoWowOracleRuntimePolicyTest, CreatureDoesNotReappearAsAnInteractionPhaseBootstrap)
{
    QuestObjectiveSpec const spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::LootSource, 530, 0);
    EXPECT_FALSE(identity.valid);
}

TEST(AutoWowOracleRuntimePolicyTest, GameObjectBootstrapRejectsMismatchedSpawnKind)
{
    QuestObjectiveSpec spec = MakeGameObjectBootstrapSpec();
    spec.sources.front().spawns.front() = MakeCreatureSpawn(21117, 530, 180510);

    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::LootSource, 530, 0);
    QuestBootstrapIdentity const waiting = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::WaitForRespawn, 530, 0);
    QuestBootstrapIdentity const acquiring = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::AcquireTarget, 530, 0);
    EXPECT_FALSE(identity.valid);
    EXPECT_FALSE(waiting.valid);
    EXPECT_FALSE(acquiring.valid);
}

TEST(AutoWowOracleRuntimePolicyTest, WaitPhaseRejectsMismatchedRequiredEntry)
{
    QuestObjectiveSpec creature = MakeBootstrapSpec();
    creature.requiredNpcOrGoEntry = 3209;
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        creature, QuestActionPhase::WaitForRespawn, 530, 0).valid);
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        creature, QuestActionPhase::AcquireTarget, 530, 0).valid);

    QuestObjectiveSpec gameObject = MakeGameObjectBootstrapSpec();
    gameObject.requiredNpcOrGoEntry = -180511;
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        gameObject, QuestActionPhase::WaitForRespawn, 530, 0).valid);
    EXPECT_FALSE(ResolveQuestBootstrapIdentity(
        gameObject, QuestActionPhase::AcquireTarget, 530, 0).valid);
}

TEST(AutoWowOracleRuntimePolicyTest, ExecutorAcceptsStableBootstrapIdentityAfterFirstDispatch)
{
    QuestObjectiveSpec const spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    ASSERT_TRUE(identity.valid);

    AutoWowOracleExecutor::ExecutorRequest request;
    request.valid = true;
    request.requestId = 1;
    request.operation = AutoWowOracle::OperationCode::QuestObjective;
    request.domain = AutoWowOracle::Domain::Quest;
    request.intent = AutoWowOracle::IntentCode::QuestObjective;
    request.resource = AutoWowOracle::LeaseResource::QuestGather;
    request.scope = {AutoWowOracle::ScopeKind::LabFixture, 7, 7};
    request.decisionId = 1;
    request.intentId = 2;
    request.actorGuid = 7;
    request.targetGuid = identity.targetGuid;
    request.executorAvailable = true;
    request.quest = {true, spec.key.questId, AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject,
                     spec.key.slot, spec.requiredNpcOrGoEntry > 0
                         ? static_cast<std::uint32_t>(spec.requiredNpcOrGoEntry) : 0U, 0};
    request.epoch = 1;
    request.frameVersion = 1;
    request.issuedTick = 1;
    request.expiresTick = 10;
    request.ttlTicks = 1;
    request.ownershipProof.active = true;
    request.ownershipProof.botGuid = 7;
    request.ownershipProof.decisionId = request.decisionId;
    request.ownershipProof.intentId = request.intentId;
    request.ownershipProof.epoch = request.epoch;
    request.ownershipProof.expiresTick = request.expiresTick;
    request.ownershipProof.scope = request.scope;
    request.ownershipProof.operation = request.operation;

    AutoWowOracleQuestExecutor::QuestObjectiveObservation before;
    before.world.onWorldThread = true;
    before.world.botAlive = true;
    before.world.botGuid = 7;
    before.world.mapId = 530;
    before.world.instanceId = 1;
    before.world.tick = 2;
    before.world.frameVersion = request.frameVersion;
    before.world.epoch = request.epoch;
    before.world.ownership = request.ownershipProof;
    before.objective.valid = true;
    before.objective.botGuid = 7;
    before.objective.questId = spec.key.questId;
    before.objective.family = AutoWowOracle::QuestObjectiveFamily::NpcOrGameObject;
    before.objective.slot = spec.key.slot;
    before.objective.requiredEntry = static_cast<std::uint32_t>(spec.requiredNpcOrGoEntry);
    before.objective.targetGuid = identity.targetGuid;
    before.objective.targetMapId = 530;
    before.objective.currentCount = 0;
    before.objective.requiredCount = spec.requiredCount;
    before.objective.phase = AutoWowOracleQuestExecutor::ObjectiveStepPhase::ResolveObjective;

    AutoWowOracleQuestExecutor::PreparedObjective const prepared =
        AutoWowOracleQuestExecutor::OracleQuestExecutor::Prepare(request, before);
    ASSERT_TRUE(prepared.accepted);
    AutoWowOracleQuestExecutor::DispatchResult const dispatched =
        AutoWowOracleQuestExecutor::OracleQuestExecutor::Dispatch(
            prepared, before, &StableResolverStep);
    ASSERT_TRUE(dispatched.valid);
    EXPECT_TRUE(dispatched.accepted);
    EXPECT_TRUE(dispatched.afterValidated);
    EXPECT_EQ(dispatched.reason, AutoWowOracleQuestExecutor::QuestObjectiveReason::None);
}

TEST(AutoWowOracleRuntimePolicyTest, CrossMapOnlySourceFailsClosedWithNoEligibleCandidate)
{
    QuestObjectiveSpec spec = MakeBootstrapSpec();
    for (QuestObjectiveSource& source : spec.sources)
        for (GuidPosition& spawn : source.spawns)
            spawn = MakeCreatureSpawn(9002, 1, source.entry);

    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 530, 0);
    EXPECT_FALSE(identity.valid);
    EXPECT_EQ(identity.targetGuid, 0U);

    AutoWowOracle::WorldReadFrame frame;
    frame.version = 1;
    frame.tick = 1;
    frame.epoch = 1;
    frame.scope = {AutoWowOracle::ScopeKind::PersistentCampaign, 7, 7};
    frame.bot = {7, true, false, 530, 1};
    AutoWowOracle::PlanResult const planned = AutoWowOracle::Plan(frame);
    EXPECT_FALSE(planned.hasDecision);
    EXPECT_EQ(planned.receipt.reason, AutoWowOracle::ReceiptReason::NoEligibleIntent);
}

TEST(AutoWowOracleRuntimePolicyTest, ExistingLiveTargetAlwaysWinsOverBootstrap)
{
    QuestObjectiveSpec spec = MakeBootstrapSpec();
    QuestBootstrapIdentity const identity = ResolveQuestBootstrapIdentity(
        spec, QuestActionPhase::ResolveObjective, 1, 0xabc);

    ASSERT_TRUE(identity.valid);
    EXPECT_FALSE(identity.bootstrap);
    EXPECT_EQ(identity.targetGuid, 0xabcU);
    EXPECT_EQ(identity.sourceEntry, 0U);
}
}
