/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// -------------------------------------------------------------------------------------------------
// Phase 1 objective-lock — RESOLUTION / POLICY acceptance matrix.
//
// These tests pin the *decision contract* of the objective-lock feature at the resolution/policy
// level. They exercise the real contract types from QuestObjectiveContext.h
// (QuestObjectiveSpec / QuestObjectiveRuntime / QuestFinisherRef and the phase/failure enums) and
// the real inline predicates on the spec (hasLock / acceptsCreatureEntry / acceptsGameObjectEntry).
//
// Where a decision is made by runtime code that requires live world data (target geometry, DB-backed
// relation resolution, actual RNG-driven grind selection), the Phase 1 *policy* is expressed here as
// small pure helper functions that operate ONLY on the contract types. Pinning them here fixes the
// intended behavior so the production resolver/targeter can be wired against a locked spec.
// Tests that genuinely cannot run without a booted world are declared with the DISABLED_ prefix and
// are documented as integration-only.
//
// IMPORTANT: production quest identifiers appear ONLY in the fixture block below. No quest id, item
// id, or creature/GO entry may be hardcoded anywhere except that block.
// -------------------------------------------------------------------------------------------------

#include "QuestObjectiveContext.h"
#include "GameObjectLockPolicy.h"
#include "AiObjectContext.h"
#include "LootValues.h"
#include "NewRpgInfo.h"
#include "QuestInventoryReliefPolicy.h"
#include "QuestKillSourcePolicy.h"
#include "QuestObjectiveTransitionPolicy.h"
#include "QuestSourceStallPolicy.h"
#include "QuestValues.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace
{
std::string ReadModuleSource(std::string const& relativePath)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string SourceBetween(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};

    std::size_t const finish = source.find(end, begin + start.size());
    return source.substr(begin, finish == std::string::npos ? std::string::npos : finish - begin);
}

LockEntry LockWithAlternatives(std::initializer_list<std::pair<LockKeyType, uint32>> alternatives)
{
    LockEntry lock{};
    std::size_t index = 0;
    for (auto const& alternative : alternatives)
    {
        if (index == MAX_LOCK_CASE)
            break;
        lock.Type[index] = static_cast<uint32>(alternative.first);
        lock.Index[index] = alternative.second;
        ++index;
    }
    return lock;
}

std::string GameObjectLootBody()
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::string const lootSource =
        SourceBetween(source, "case QuestActionPhase::LootSource:", "case QuestActionPhase::VerifyProgress:");
    return SourceBetween(lootSource, "if (rt.selectedSourceEntry < 0)\n            {",
                         "if (!rt.selectedTargetGuid.IsEmpty())");
}

// ===== Fixture quest facts (hardcoded ONLY here) =================================================
// q459: collect item 3297; allowed creature sources 1988, 1989; finisher creature 1992.
// q789: collect item 4862; allowed creature sources 3124, 3281; finisher creature 3143.
// q792: kill creature 3101 x8; finisher creature 3145.
// q916: collect item 5166; allowed creature source 1986; finisher creature 2082.
// q786: direct GameObject credits 3189/3190/3192; fixture spawn GUIDs 12388/12389/12390.
struct QuestFact
{
    uint32 questId;
    QuestObjectiveKind kind;
    uint32 requiredItemId;             // 0 for kill objectives
    int32 requiredNpcOrGoEntry;        // 0 for collect objectives; >0 creature, <0 gameobject
    uint32 requiredCount;
    std::vector<uint32> creatureSources;
    int32 finisherSignedEntry;         // >0 creature, <0 gameobject
};

constexpr uint32 Q459 = 459;
constexpr uint32 Q786 = 786;
constexpr uint32 Q789 = 789;
constexpr uint32 Q792 = 792;
constexpr uint32 Q916 = 916;
constexpr uint32 Q917 = 917;
constexpr uint32 Q5441 = 5441;
constexpr uint32 Q786_GO_0 = 3189;
constexpr uint32 Q786_GO_1 = 3190;
constexpr ObjectGuid::LowType Q786_GO_GUID_0 = 12388;
constexpr ObjectGuid::LowType Q786_GO_GUID_1 = 12389;
constexpr uint32 FINISHER_LOCAL_MAP = 1;
constexpr uint32 FINISHER_REMOTE_MAP = 0;
constexpr uint64 FINISHER_SPAWN_LOW = 1001;
constexpr uint64 FINISHER_SPAWN_MID = 1002;
constexpr uint64 FINISHER_SPAWN_HIGH = 1003;

const QuestFact kFactQ459{Q459, QuestObjectiveKind::CollectItem, 3297, 0, 1, {1988, 1989}, 1992};
const QuestFact kFactQ789{Q789, QuestObjectiveKind::CollectItem, 4862, 0, 1, {3124, 3281}, 3143};
const QuestFact kFactQ792{Q792, QuestObjectiveKind::CreatureCredit, 0, 3101, 8, {3101}, 3145};
const QuestFact kFactQ916{Q916, QuestObjectiveKind::CollectItem, 5166, 0, 1, {1986}, 2082};

// ===== Spec builders — mirror what the production resolver MUST emit for the fixture facts ========
QuestObjectiveSource CreatureSource(uint32 entry)
{
    QuestObjectiveSource s;
    s.type = QuestObjectiveSource::Type::Creature;
    s.entry = entry;
    return s;  // spawns left empty on purpose: policy tests never depend on live spawn geometry
}

QuestObjectiveSource GameObjectSource(uint32 entry)
{
    QuestObjectiveSource s;
    s.type = QuestObjectiveSource::Type::GameObject;
    s.entry = entry;
    return s;
}

QuestObjectiveSpec BuildSpec(QuestFact const& f, uint32 currentCount)
{
    QuestObjectiveSpec spec;
    spec.key.questId = f.questId;
    spec.key.slot = 0;
    spec.key.family =
        (f.kind == QuestObjectiveKind::CollectItem) ? QuestObjectiveFamily::Item : QuestObjectiveFamily::NpcOrGameObject;
    spec.kind = f.kind;
    spec.requiredItemId = f.requiredItemId;
    spec.requiredNpcOrGoEntry = f.requiredNpcOrGoEntry;
    spec.currentCount = currentCount;
    spec.requiredCount = f.requiredCount;
    for (uint32 e : f.creatureSources)
        spec.sources.push_back(CreatureSource(e));
    spec.supported = true;
    return spec;
}

QuestFinisherRef BuildFinisher(QuestFact const& f)
{
    QuestFinisherRef fin;
    fin.signedEntry = f.finisherSignedEntry;
    return fin;
}

QuestFinisherSpawnCandidate FinisherCandidate(int32 signedEntry, uint32 relationMask, uint32 mapId,
                                              uint64 spawnIdentity, float directDistance,
                                              float routeDistance = std::numeric_limits<float>::infinity())
{
    QuestFinisherSpawnCandidate candidate;
    candidate.signedEntry = signedEntry;
    candidate.relationMask = relationMask;
    candidate.mapId = mapId;
    candidate.spawnIdentity = spawnIdentity;
    candidate.directDistance = directDistance;
    candidate.routeDistance = routeDistance;
    return candidate;
}

// ===== Phase 1 policy helpers (pure; contract types only) ========================================
// Offensive legality: a locked objective may engage a candidate creature ONLY if the spec whitelists
// its entry. Distance / aggro state is deliberately NOT an input to legality.
bool MayEngageForObjective(QuestObjectiveSpec const& spec, uint32 candidateEntry, bool /*inAggroRange*/)
{
    return spec.hasLock() && spec.kind != QuestObjectiveKind::UseQuestItem &&
           spec.kind != QuestObjectiveKind::ScriptedEvent &&
           spec.acceptsCreatureEntry(candidateEntry);
}

bool MayUseQuestItemOn(QuestObjectiveSpec const& spec, uint32 itemId, uint32 candidateEntry)
{
    return spec.hasLock() && spec.kind == QuestObjectiveKind::UseQuestItem &&
           spec.questItemId == itemId && spec.acceptsCreatureEntry(candidateEntry);
}

// Loot legality for a collect objective: only the exact required item, only from a whitelisted source.
bool MayLootItemForObjective(QuestObjectiveSpec const& spec, uint32 sourceCreatureEntry, uint32 itemId)
{
    return spec.hasLock() && spec.kind == QuestObjectiveKind::CollectItem && itemId == spec.requiredItemId &&
           spec.acceptsCreatureEntry(sourceCreatureEntry);
}

// Progress is a strictly-positive delta over the baseline captured when the objective was locked.
bool IsObjectiveProgress(QuestObjectiveRuntime const& rt, QuestObjectiveSpec const& spec)
{
    return spec.currentCount > rt.baselineCount;
}

// Phase decision after target acquisition. No whitelist candidate -> WaitForRespawn, never a grind
// fallback onto a non-whitelisted mob.
QuestActionPhase DecideAfterAcquire(QuestObjectiveSpec const& spec, bool hasLiveWhitelistCandidate)
{
    if (!spec.hasLock())
        return QuestActionPhase::ResolveFinisher;  // objective satisfied / not constraining
    if (!hasLiveWhitelistCandidate)
        return QuestActionPhase::WaitForRespawn;
    return QuestActionPhase::EngageTarget;
}

// Finisher acceptance: exactly the quest's finisher relation entry, nothing else.
bool AcceptsFinisher(QuestFinisherRef const& finisher, int32 candidateSignedEntry)
{
    return finisher.signedEntry != 0 && candidateSignedEntry == finisher.signedEntry;
}

// Reward gate: CanRewardQuest == false is a hard blocker, not a retry.
QuestActionPhase DecideReward(bool canRewardQuest)
{
    return canRewardQuest ? QuestActionPhase::Complete : QuestActionPhase::Blocked;
}

// Self-defense never mutates objective selection; it engages the attacker and resumes the lock.
struct SelfDefenseResult
{
    int32 preservedSourceEntry;
    bool objectiveStillLocked;
    QuestActionPhase resumePhase;
};

SelfDefenseResult ResolveSelfDefense(QuestObjectiveSpec const& spec, QuestObjectiveRuntime const& rt)
{
    QuestActionPhase const resume = spec.kind == QuestObjectiveKind::ScriptedEvent
                                        ? QuestActionPhase::EscortEvent
                                        : QuestActionPhase::AcquireTarget;
    return SelfDefenseResult{rt.selectedSourceEntry, spec.hasLock(), resume};
}

// Deterministic offensive selection: only whitelist candidates are eligible; RNG never launders a
// non-whitelisted entry into a legal target. Returns 0 when no whitelist candidate exists.
int32 SelectOffensiveTarget(QuestObjectiveSpec const& spec, std::vector<uint32> const& candidateEntries)
{
    for (uint32 e : candidateEntries)
        if (MayEngageForObjective(spec, e, /*inAggroRange*/ true))
            return static_cast<int32>(e);
    return 0;
}

QuestActionPhase DecideAtGameObjectSource(QuestObjectiveSpec const& spec)
{
    if (spec.kind == QuestObjectiveKind::CollectItem)
        return QuestActionPhase::LootSource;
    if (spec.kind == QuestObjectiveKind::GameObjectCredit)
        return QuestActionPhase::InteractSource;
    return QuestActionPhase::Blocked;
}
}  // namespace

TEST(QuestObjectiveResolution, OrdinaryOpenAlternativeAllowsMixedOpenAndProfessionLock)
{
    LockEntry lock = LockWithAlternatives({{LOCK_KEY_SKILL, LOCKTYPE_OPEN_KNEELING},
                                           {LOCK_KEY_SKILL, LOCKTYPE_HERBALISM}});
    EXPECT_TRUE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&lock));
    EXPECT_TRUE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(nullptr));
}

TEST(QuestObjectiveResolution, LockPolicyRejectsProfessionKeySpellAndNonOrdinaryRoutes)
{
    LockEntry herbalismOnly = LockWithAlternatives({{LOCK_KEY_SKILL, LOCKTYPE_HERBALISM}});
    LockEntry keyOnly = LockWithAlternatives({{LOCK_KEY_ITEM, 5}});
    LockEntry spellOnly = LockWithAlternatives({{LOCK_KEY_SPELL, 12345}});
    LockEntry blasting = LockWithAlternatives({{LOCK_KEY_SKILL, LOCKTYPE_BLASTING}});
    LockEntry vehicle = LockWithAlternatives({{LOCK_KEY_SKILL, LOCKTYPE_OPEN_FROM_VEHICLE}});
    LockEntry tinkering = LockWithAlternatives({{LOCK_KEY_SKILL, LOCKTYPE_OPEN_TINKERING}});

    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&herbalismOnly));
    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&keyOnly));
    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&spellOnly));
    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&blasting));
    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&vehicle));
    EXPECT_FALSE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&tinkering));
}

TEST(QuestObjectiveResolution, EveryCoreOrdinaryOpenLockTypeIsExplicitlyAdmitted)
{
    for (LockType const lockType : {LOCKTYPE_OPEN, LOCKTYPE_QUICK_OPEN, LOCKTYPE_OPEN_KNEELING,
                                    LOCKTYPE_OPEN_ATTACKING, LOCKTYPE_SLOW_OPEN})
    {
        LockEntry lock = LockWithAlternatives({{LOCK_KEY_SKILL, static_cast<uint32>(lockType)}});
        EXPECT_TRUE(GameObjectLockPolicy::HasOrdinaryOpenAlternative(&lock))
            << static_cast<uint32>(lockType);
    }
}

TEST(QuestObjectiveResolution, GameObjectQuestLootGateRequiresCoreInteractionChecks)
{
    std::string const body = GameObjectLootBody();
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("exactLoot.reqItem == 0"), std::string::npos);
    EXPECT_NE(body.find("GameObjectLockPolicy::HasOrdinaryOpenAlternative"), std::string::npos);
    EXPECT_NE(body.find("go->GetGoState() == GO_STATE_READY"), std::string::npos);
    EXPECT_NE(body.find("GO_FLAG_NOT_SELECTABLE"), std::string::npos);
    EXPECT_NE(body.find("go->ActivateToQuest(bot)"), std::string::npos);
}

TEST(QuestObjectiveResolution, AdmittedGameObjectQuestLootUsesCorePipelineWithoutAmbientOrDirectCredit)
{
    std::string const body = GameObjectLootBody();
    ASSERT_FALSE(body.empty());

    for (std::string_view const required : {"LootObject exactLoot", "bot->SendLoot(go->GetGUID(), LOOT_SKINNING)",
                                             "HandleAutostoreLootItemOpcode", "HandleLootReleaseOpcode"})
        EXPECT_NE(body.find(required), std::string::npos) << required;

    for (std::string_view const forbidden : {"available loot", "DoSpecificAction(\"loot\"", "AddItem(",
                                              "AddQuest(", "CompleteQuest(", "RewardQuest(",
                                              "KilledMonsterCredit(", "CastedCreatureOrGO(", "q3521", "152095"})
        EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
}

// =================================================================================================
// Source resolution — each quest resolves EXACTLY its listed sources, no extras.
// (Exercises the real acceptsCreatureEntry contract on a resolver-shaped spec.)
// =================================================================================================
TEST(QuestObjectiveResolution, Q459_ResolvesExactlyItsItemSources)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ459, 0);
    EXPECT_TRUE(spec.acceptsCreatureEntry(1988));
    EXPECT_TRUE(spec.acceptsCreatureEntry(1989));
    // extras / neighbours / other-quest sources must be rejected
    EXPECT_FALSE(spec.acceptsCreatureEntry(1990));
    EXPECT_FALSE(spec.acceptsCreatureEntry(3124));  // q789 source
    EXPECT_FALSE(spec.acceptsCreatureEntry(3101));  // q792 source
    EXPECT_FALSE(spec.acceptsCreatureEntry(1992));  // its own finisher, not a source
    EXPECT_EQ(spec.sources.size(), 2u);
}

TEST(QuestObjectiveResolution, Q789_ResolvesExactlyItsItemSources)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ789, 0);
    EXPECT_TRUE(spec.acceptsCreatureEntry(3124));
    EXPECT_TRUE(spec.acceptsCreatureEntry(3281));
    EXPECT_FALSE(spec.acceptsCreatureEntry(1988));  // q459 source
    EXPECT_FALSE(spec.acceptsCreatureEntry(1989));
    EXPECT_FALSE(spec.acceptsCreatureEntry(3143));  // its own finisher
    EXPECT_EQ(spec.sources.size(), 2u);
}

TEST(QuestObjectiveResolution, PendingRequiredItemBypassesOnlyGenericBagReserve)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ789, 0);

    EXPECT_TRUE(spec.isPendingRequiredItem(kFactQ789.requiredItemId));
    EXPECT_FALSE(spec.isPendingRequiredItem(kFactQ459.requiredItemId));

    spec.currentCount = spec.requiredCount;
    EXPECT_FALSE(spec.isPendingRequiredItem(kFactQ789.requiredItemId));

    QuestObjectiveSpec killSpec = BuildSpec(kFactQ792, 0);
    EXPECT_FALSE(killSpec.isPendingRequiredItem(kFactQ789.requiredItemId));
}

TEST(QuestObjectiveResolution, DropMapPreservesEverySourceForOneQuestItem)
{
    DropMap drops;
    drops.emplace(kFactQ789.requiredItemId, static_cast<int32>(kFactQ789.creatureSources[0]));
    drops.emplace(kFactQ789.requiredItemId, static_cast<int32>(kFactQ789.creatureSources[1]));

    auto const range = drops.equal_range(kFactQ789.requiredItemId);
    std::vector<int32> sources;
    for (auto itr = range.first; itr != range.second; ++itr)
        sources.push_back(itr->second);

    std::sort(sources.begin(), sources.end());
    EXPECT_EQ(sources, (std::vector<int32>{3124, 3281}));
}

TEST(QuestObjectiveResolution, Q792_ResolvesExactlyItsKillSource)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    EXPECT_TRUE(spec.acceptsCreatureEntry(3101));
    EXPECT_FALSE(spec.acceptsCreatureEntry(3145));  // finisher, not a kill source
    EXPECT_FALSE(spec.acceptsCreatureEntry(1988));
    EXPECT_EQ(spec.sources.size(), 1u);
}

TEST(QuestObjectiveResolution, Q916_ResolvesExactlyItsSingleItemSource)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ916, 0);
    EXPECT_TRUE(spec.acceptsCreatureEntry(1986));
    EXPECT_FALSE(spec.acceptsCreatureEntry(1988));
    EXPECT_FALSE(spec.acceptsCreatureEntry(1989));
    EXPECT_FALSE(spec.acceptsCreatureEntry(2082));  // finisher
    EXPECT_EQ(spec.sources.size(), 1u);
}

TEST(QuestObjectiveResolution, Q917_CollectsWebwoodEggFromExactGameObjectSource)
{
    QuestObjectiveSpec spec;
    spec.key = QuestObjectiveKey{Q917, 0, QuestObjectiveFamily::Item};
    spec.kind = QuestObjectiveKind::CollectItem;
    spec.requiredItemId = 5167;
    spec.currentCount = 0;
    spec.requiredCount = 1;
    spec.sources.push_back(GameObjectSource(4406));
    spec.supported = true;

    EXPECT_TRUE(spec.hasLock());
    EXPECT_TRUE(spec.acceptsGameObjectEntry(4406));
    EXPECT_FALSE(spec.acceptsGameObjectEntry(4407));
    EXPECT_FALSE(spec.acceptsCreatureEntry(4406));
    EXPECT_EQ(DecideAtGameObjectSource(spec), QuestActionPhase::LootSource);
}

TEST(QuestObjectiveResolution, PureGameObjectCreditUsesInteractionPhase)
{
    QuestObjectiveSpec spec;
    spec.key = QuestObjectiveKey{12345, 0, QuestObjectiveFamily::NpcOrGameObject};
    spec.kind = QuestObjectiveKind::GameObjectCredit;
    spec.requiredNpcOrGoEntry = -777;
    spec.currentCount = 0;
    spec.requiredCount = 1;
    spec.sources.push_back(GameObjectSource(777));
    spec.supported = true;

    EXPECT_TRUE(spec.hasLock());
    EXPECT_TRUE(spec.acceptsGameObjectEntry(777));
    EXPECT_EQ(DecideAtGameObjectSource(spec), QuestActionPhase::InteractSource);
}

TEST(QuestObjectiveResolution, DirectGameObjectReceiptsAreDurableAndMonotonicAcrossHandoff)
{
    NewRpgInfo info;
    info.ChangeToDoQuest(Q786, nullptr);

    ObjectGuid const firstGuid =
        ObjectGuid::Create<HighGuid::GameObject>(Q786_GO_0, Q786_GO_GUID_0);
    ObjectGuid const secondGuid =
        ObjectGuid::Create<HighGuid::GameObject>(Q786_GO_1, Q786_GO_GUID_1);
    DirectGameObjectReceipt const first =
        info.RecordDirectGameObjectReceipt(Q786, 0, Q786_GO_0, firstGuid, 0, 1);
    DirectGameObjectReceipt const second =
        info.RecordDirectGameObjectReceipt(Q786, 1, Q786_GO_1, secondGuid, 0, 1);

    EXPECT_EQ(first.sequence, 1u);
    EXPECT_EQ(second.sequence, 2u);
    EXPECT_LT(first.sequence, second.sequence);
    EXPECT_EQ(first.questId, Q786);
    EXPECT_EQ(first.objectiveSlot, 0u);
    EXPECT_EQ(first.entry, Q786_GO_0);
    EXPECT_EQ(first.guid.GetCounter(), Q786_GO_GUID_0);
    EXPECT_EQ(first.before, 0u);
    EXPECT_EQ(first.after, 1u);

    info.ChangeToIdle();
    ASSERT_EQ(info.directGameObjectReceipts.size(), 2u);
    EXPECT_EQ(info.directGameObjectReceipts.front().sequence, 1u);
    EXPECT_EQ(info.directGameObjectReceipts.back().sequence, 2u);

    info.Reset();
    ASSERT_EQ(info.directGameObjectReceipts.size(), 2u);
    EXPECT_EQ(info.nextDirectGameObjectReceiptSequence, 3u);
}

TEST(QuestObjectiveResolution, SuccessfulDirectGameObjectReceiptImmediatelyRebasesObjective)
{
    QuestObjectiveRuntime runtime;
    runtime.phase = QuestActionPhase::InteractSource;
    runtime.baselineCount = 0;
    runtime.lastObservedCount = 0;
    runtime.attemptCount = 2;
    runtime.failure = QuestFailureReason::InteractionRejected;
    runtime.selectedTargetGuid =
        ObjectGuid::Create<HighGuid::GameObject>(Q786_GO_0, Q786_GO_GUID_0);

    DirectGameObjectReceipt receipt;
    receipt.questId = Q786;
    receipt.objectiveSlot = 0;
    receipt.entry = Q786_GO_0;
    receipt.guid = runtime.selectedTargetGuid;
    receipt.before = 0;
    receipt.after = 1;

    EXPECT_TRUE(RebaseAfterDirectGameObjectCredit(runtime, receipt, 4321));
    EXPECT_EQ(runtime.phase, QuestActionPhase::ResolveObjective);
    EXPECT_EQ(runtime.baselineCount, 1u);
    EXPECT_EQ(runtime.lastObservedCount, 1u);
    EXPECT_EQ(runtime.lastProgressTimeMs, 4321u);
    EXPECT_EQ(runtime.attemptCount, 0u);
    EXPECT_EQ(runtime.failure, QuestFailureReason::None);
    EXPECT_TRUE(runtime.selectedTargetGuid.IsEmpty());
}

TEST(QuestObjectiveResolution, RepeatedSourceFailureBudgetSurvivesResolveAndWait)
{
    std::uint32_t firstFailureMs = 0;
    constexpr std::uint32_t budgetMs = 90000;

    // Travel and normal combat are absent from the failure observer. The first failed source
    // after a long walk therefore starts the clock at the failure, not at quest selection.
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 500000, budgetMs));
    EXPECT_EQ(firstFailureMs, 500000u);
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 545000, budgetMs));
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 589999, budgetMs));
    EXPECT_TRUE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 590000, budgetMs));
}

TEST(QuestObjectiveResolution, SourceFailureBudgetResetsOnlyOnCreditOrNewObjective)
{
    std::uint32_t firstFailureMs = 0;
    constexpr std::uint32_t budgetMs = 90000;

    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 1000, budgetMs));
    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(firstFailureMs, false, false);
    EXPECT_EQ(firstFailureMs, 1000u); // Phase, target, and lease churn do not reset it.
    EXPECT_TRUE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 91000, budgetMs));

    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(firstFailureMs, false, true);
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 91001, budgetMs));
    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(firstFailureMs, true, false);
    EXPECT_FALSE(QuestSourceStallPolicy::RecordUnavailableSource(firstFailureMs, 190001, budgetMs));
}

TEST(QuestObjectiveResolution, PartialCreditReturnsGameObjectSourcesToResolution)
{
    EXPECT_EQ(AfterPartialQuestCredit(QuestObjectiveKind::CollectItem,
                                     -static_cast<int32>(Q786_GO_0)),
              QuestActionPhase::ResolveObjective);
    EXPECT_EQ(AfterPartialQuestCredit(QuestObjectiveKind::GameObjectCredit,
                                     -static_cast<int32>(Q786_GO_0)),
              QuestActionPhase::ResolveObjective);
    EXPECT_EQ(AfterPartialQuestCredit(QuestObjectiveKind::CreatureCredit,
                                     kFactQ792.requiredNpcOrGoEntry),
              QuestActionPhase::AcquireTarget);
    EXPECT_EQ(AfterPartialQuestCredit(QuestObjectiveKind::CollectItem,
                                     static_cast<int32>(kFactQ459.creatureSources.front())),
              QuestActionPhase::AcquireTarget);
    EXPECT_EQ(AfterPartialQuestCredit(QuestObjectiveKind::UseQuestItem,
                                     kFactQ792.requiredNpcOrGoEntry),
              QuestActionPhase::ResolveObjective);
}

TEST(QuestObjectiveResolution, NoDeltaDirectGameObjectReceiptDoesNotRebase)
{
    QuestObjectiveRuntime runtime;
    runtime.phase = QuestActionPhase::InteractSource;
    runtime.baselineCount = 0;

    DirectGameObjectReceipt receipt;
    receipt.before = 0;
    receipt.after = 0;

    EXPECT_FALSE(RebaseAfterDirectGameObjectCredit(runtime, receipt, 4321));
    EXPECT_EQ(runtime.phase, QuestActionPhase::InteractSource);
    EXPECT_EQ(runtime.baselineCount, 0u);
}

TEST(QuestObjectiveResolution, Q5441UsesExactQuestItemOnExactCreatureWithoutOffensiveTargeting)
{
    QuestObjectiveSpec spec;
    spec.key = QuestObjectiveKey{Q5441, 0, QuestObjectiveFamily::NpcOrGameObject};
    spec.kind = QuestObjectiveKind::UseQuestItem;
    spec.requiredNpcOrGoEntry = 10556;
    spec.questItemId = 16114;
    spec.currentCount = 0;
    spec.requiredCount = 5;
    spec.sources.push_back(CreatureSource(10556));
    spec.supported = true;

    EXPECT_TRUE(spec.hasLock());
    EXPECT_TRUE(MayUseQuestItemOn(spec, 16114, 10556));
    EXPECT_FALSE(MayUseQuestItemOn(spec, 16115, 10556));
    EXPECT_FALSE(MayUseQuestItemOn(spec, 16114, 10557));
    EXPECT_FALSE(MayEngageForObjective(spec, 10556, /*inAggroRange*/ true));
}

TEST(QuestObjectiveResolution, AggregateCastFlagDoesNotMisclassifyOrdinaryKillQuest)
{
    // AzerothCore applies the aggregate KILL|CAST|SPEAKTO flags to ordinary RequiredNpcOrGo
    // objectives such as q784. Without a source item this must remain normal creature credit.
    EXPECT_FALSE(UsesQuestSourceItemForCredit(/*hasAggregateCastFlag*/ true, /*questSourceItemId*/ 0));
    EXPECT_TRUE(UsesQuestSourceItemForCredit(/*hasAggregateCastFlag*/ true, /*questSourceItemId*/ 16114));
    EXPECT_FALSE(UsesQuestSourceItemForCredit(/*hasAggregateCastFlag*/ false, /*questSourceItemId*/ 16114));
}

TEST(QuestObjectiveResolution, KillExecutorRejectsAuthoredUnattackableCreatureCredit)
{
    using QuestKillSourcePolicy::CanUseKillExecutor;
    EXPECT_TRUE(CanUseKillExecutor(0));
    EXPECT_TRUE(CanUseKillExecutor(UNIT_FLAG_IMMUNE_TO_NPC));
    EXPECT_FALSE(CanUseKillExecutor(UNIT_FLAG_NON_ATTACKABLE));
    EXPECT_FALSE(CanUseKillExecutor(UNIT_FLAG_NOT_ATTACKABLE_1));
    EXPECT_FALSE(CanUseKillExecutor(UNIT_FLAG_NON_ATTACKABLE_2));
    EXPECT_FALSE(CanUseKillExecutor(UNIT_FLAG_IMMUNE_TO_PC));
    EXPECT_FALSE(CanUseKillExecutor(UNIT_FLAG_NOT_SELECTABLE));
    // Authored spell-credit dummies may combine both immunity flags with not-selectable.
    EXPECT_FALSE(CanUseKillExecutor(0x02000300));
    // The source-item spell path remains distinct from kill execution.
    EXPECT_TRUE(UsesQuestSourceItemForCredit(true, 16114));
}

TEST(QuestObjectiveResolution, NoCrossQuestSourceBleed)
{
    QuestObjectiveSpec q459 = BuildSpec(kFactQ459, 0);
    QuestObjectiveSpec q789 = BuildSpec(kFactQ789, 0);
    QuestObjectiveSpec q916 = BuildSpec(kFactQ916, 0);
    // No spec accepts any other spec's sources.
    for (uint32 e : {3124u, 3281u, 1986u})
        EXPECT_FALSE(q459.acceptsCreatureEntry(e));
    for (uint32 e : {1988u, 1989u, 1986u})
        EXPECT_FALSE(q789.acceptsCreatureEntry(e));
    for (uint32 e : {1988u, 1989u, 3124u, 3281u})
        EXPECT_FALSE(q916.acceptsCreatureEntry(e));
}

// =================================================================================================
// GameObject involved-relation resolves as a quest taker (finisher).
// A GO finisher is a signed-negative entry; it must resolve via the GameObject whitelist, and a
// creature of the same numeric value must NOT be mistaken for it.
// =================================================================================================
TEST(QuestObjectiveResolution, GameObjectFinisherResolvesAsQuestTaker)
{
    // Synthetic GO finisher (not a production quest id): entry 55 as a gameobject taker.
    constexpr uint32 kGoEntry = 55;
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    spec.sources.push_back(GameObjectSource(kGoEntry));

    EXPECT_TRUE(spec.acceptsGameObjectEntry(kGoEntry));
    // A creature with the same numeric entry is a different family and must be rejected.
    EXPECT_FALSE(spec.acceptsCreatureEntry(kGoEntry));

    QuestFinisherRef fin;
    fin.signedEntry = -static_cast<int32>(kGoEntry);  // <0 => gameobject taker
    EXPECT_TRUE(AcceptsFinisher(fin, -static_cast<int32>(kGoEntry)));
    EXPECT_FALSE(AcceptsFinisher(fin, static_cast<int32>(kGoEntry)));  // positive == creature, wrong family
}

TEST(QuestObjectiveResolution, CompositeGiverTakerRelationStillResolvesFinisher)
{
    uint32 const composite = static_cast<uint32>(QuestRelationFlag::questGiver) |
                             static_cast<uint32>(QuestRelationFlag::questTaker);

    EXPECT_TRUE(HasQuestRelationFlag(composite, QuestRelationFlag::questGiver));
    EXPECT_TRUE(HasQuestRelationFlag(composite, QuestRelationFlag::questTaker));
    EXPECT_FALSE(HasQuestRelationFlag(composite, QuestRelationFlag::objective1));
}

TEST(QuestObjectiveResolution, DirectorQuestDirectiveSurvivesStrategyResetDecision)
{
    NewRpgInfo info;
    EXPECT_FALSE(info.HasActiveQuestDirective());

    info.data = NewRpgInfo::DoQuest{};
    EXPECT_TRUE(info.HasActiveQuestDirective());

    info.data = NewRpgInfo::Idle{};
    EXPECT_FALSE(info.HasActiveQuestDirective());
}

TEST(QuestObjectiveResolution, FinisherDirectiveSuppressesProactiveLegacyGrind)
{
    QuestObjectiveSpec completed;
    completed.supported = true;
    completed.key.questId = 459;
    completed.currentCount = 8;
    completed.requiredCount = 8;

    EXPECT_TRUE(ShouldSuppressLegacyQuestGrind(true, completed));
    EXPECT_FALSE(ShouldSuppressLegacyQuestGrind(false, completed));

    QuestObjectiveSpec locked = completed;
    locked.currentCount = 7;
    EXPECT_TRUE(locked.hasLock());
    EXPECT_FALSE(ShouldSuppressLegacyQuestGrind(true, locked));
}

// =================================================================================================
// NPC objective reads RequiredNpcOrGoCount.
// =================================================================================================
TEST(QuestObjectiveResolution, Q792_ReadsRequiredNpcOrGoCount)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    EXPECT_EQ(spec.requiredCount, 8u);
    EXPECT_EQ(spec.requiredNpcOrGoEntry, 3101);  // signed >0 => creature
    EXPECT_EQ(spec.kind, QuestObjectiveKind::CreatureCredit);
    EXPECT_EQ(spec.key.family, QuestObjectiveFamily::NpcOrGameObject);
    EXPECT_EQ(spec.requiredItemId, 0u);
}

// =================================================================================================
// Target whitelist — a closer unrelated legal mob (including one inside aggro range) is rejected.
// =================================================================================================
TEST(QuestObjectiveResolution, Q792_CloserUnrelatedLegalMobRejected)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    ASSERT_TRUE(spec.hasLock());
    // 1988 is a perfectly attackable, legal mob and (hypothetically) closer — still illegal here.
    EXPECT_FALSE(MayEngageForObjective(spec, 1988, /*inAggroRange*/ false));
    // The correct source is engageable.
    EXPECT_TRUE(MayEngageForObjective(spec, 3101, /*inAggroRange*/ false));
}

TEST(QuestObjectiveResolution, AggroRangeDoesNotOverrideWhitelist)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    // Same non-source mob, now explicitly inside aggro range: legality is unchanged (still rejected).
    EXPECT_FALSE(MayEngageForObjective(spec, 1988, /*inAggroRange*/ true));
    EXPECT_FALSE(MayEngageForObjective(spec, 1989, /*inAggroRange*/ true));
    // Whitelisted source remains legal whether or not it is inside aggro range.
    EXPECT_TRUE(MayEngageForObjective(spec, 3101, /*inAggroRange*/ true));
    EXPECT_TRUE(MayEngageForObjective(spec, 3101, /*inAggroRange*/ false));
}

// =================================================================================================
// Exact-item-target — a creature carrying a DIFFERENT quest's item (but not the selected required
// item) is rejected for this objective.
// =================================================================================================
TEST(QuestObjectiveResolution, Q459_CreatureCarryingDifferentQuestsItemRejected)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ459, 0);  // needs item 3297 from 1988/1989
    // 3124 drops q789's item 4862, not q459's 3297 -> not a valid source or loot for q459.
    EXPECT_FALSE(spec.acceptsCreatureEntry(3124));
    EXPECT_FALSE(MayLootItemForObjective(spec, 3124, 4862));  // wrong source AND wrong item
    // Even a *whitelisted* source only yields the exact required item.
    EXPECT_TRUE(MayLootItemForObjective(spec, 1988, 3297));
    EXPECT_FALSE(MayLootItemForObjective(spec, 1988, 4862));  // whitelisted source, wrong item
}

TEST(QuestObjectiveResolution, ExactItemDrivesLootAcrossQuests)
{
    QuestObjectiveSpec q916 = BuildSpec(kFactQ916, 0);  // item 5166 from 1986
    EXPECT_TRUE(MayLootItemForObjective(q916, 1986, 5166));
    EXPECT_FALSE(MayLootItemForObjective(q916, 1986, 3297));  // q459's item off q916's source
    EXPECT_FALSE(MayLootItemForObjective(q916, 1988, 5166));  // right item, wrong (q459) source
}

// =================================================================================================
// Self-defense preserves objective context.
// =================================================================================================
TEST(QuestObjectiveResolution, SelfDefensePreservesLockedObjective)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 2);  // mid-progress, still locked
    QuestObjectiveRuntime rt;
    rt.selectedSourceEntry = 3101;
    rt.phase = QuestActionPhase::EngageTarget;

    SelfDefenseResult r = ResolveSelfDefense(spec, rt);
    EXPECT_EQ(r.preservedSourceEntry, 3101);       // selection untouched by self-defense
    EXPECT_TRUE(r.objectiveStillLocked);           // objective remains locked
    EXPECT_EQ(r.resumePhase, QuestActionPhase::AcquireTarget);
}

TEST(QuestObjectiveResolution, SelfDefenseAttackerIsNotAdoptedAsSource)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 2);
    // The attacker is some random non-whitelist mob; engaging it in self-defense must not make it a
    // legal objective target.
    EXPECT_FALSE(MayEngageForObjective(spec, 1989, /*inAggroRange*/ true));
    EXPECT_FALSE(spec.acceptsCreatureEntry(1989));
}

// =================================================================================================
// Scripted escort/event — protect and follow the named source; never treat it as an offensive target
// and never fall through to ambient grinding while the native script owns completion.
// =================================================================================================
TEST(QuestObjectiveResolution, ScriptedEventSourceIsNonOffensive)
{
    QuestObjectiveSpec spec;
    spec.key.questId = 435;
    spec.key.family = QuestObjectiveFamily::NpcOrGameObject;
    spec.kind = QuestObjectiveKind::ScriptedEvent;
    spec.currentCount = 0;
    spec.requiredCount = 1;
    spec.supported = true;
    QuestObjectiveSource source;
    source.type = QuestObjectiveSource::Type::Creature;
    source.entry = 1978;  // Deathstalker Erland
    spec.sources.push_back(source);

    ASSERT_TRUE(spec.hasLock());
    ASSERT_TRUE(spec.acceptsCreatureEntry(1978));
    EXPECT_FALSE(MayEngageForObjective(spec, 1978, /*inAggroRange*/ true));
    EXPECT_TRUE(ShouldSuppressLegacyQuestGrind(/*questDirectiveActive*/ true, spec));
}

TEST(QuestObjectiveResolution, ScriptedEventStillAllowsSeparateSelfDefensePhase)
{
    QuestObjectiveSpec spec;
    spec.key.questId = 435;
    spec.kind = QuestObjectiveKind::ScriptedEvent;
    spec.currentCount = 0;
    spec.requiredCount = 1;
    spec.supported = true;

    QuestObjectiveRuntime rt;
    rt.selectedSourceEntry = 1978;
    rt.phase = QuestActionPhase::EscortEvent;
    SelfDefenseResult result = ResolveSelfDefense(spec, rt);

    EXPECT_EQ(result.preservedSourceEntry, 1978);
    EXPECT_EQ(result.resumePhase, QuestActionPhase::EscortEvent);
}

// =================================================================================================
// No-valid-target -> WaitForRespawn (no grind fallback).
// =================================================================================================
TEST(QuestObjectiveResolution, NoWhitelistCandidateWaitsForRespawn)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    EXPECT_EQ(DecideAfterAcquire(spec, /*hasLiveWhitelistCandidate*/ false), QuestActionPhase::WaitForRespawn);
}

TEST(QuestObjectiveResolution, NoWhitelistCandidateNeverEngagesRandomMob)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    QuestActionPhase phase = DecideAfterAcquire(spec, /*hasLiveWhitelistCandidate*/ false);
    EXPECT_NE(phase, QuestActionPhase::EngageTarget);
    // And the selector returns "none" rather than laundering a nearby illegal mob.
    EXPECT_EQ(SelectOffensiveTarget(spec, {1988, 1989, 3124}), 0);
}

// =================================================================================================
// Progress baseline / delta — a pre-existing 3/8 is not "progress".
// =================================================================================================
TEST(QuestObjectiveResolution, PreExistingCountIsBaselineNotProgress)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 3);  // already at 3/8 when locked
    QuestObjectiveRuntime rt;
    rt.baselineCount = 3;
    EXPECT_FALSE(IsObjectiveProgress(rt, spec));  // 3 -> 3 is not progress
}

TEST(QuestObjectiveResolution, PositiveDeltaCountsAsProgress)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 4);  // one kill after baseline
    QuestObjectiveRuntime rt;
    rt.baselineCount = 3;
    EXPECT_TRUE(IsObjectiveProgress(rt, spec));  // 3 -> 4 is progress
}

// =================================================================================================
// Completion count — 7/8 + one kill -> 8/8.
// =================================================================================================
TEST(QuestObjectiveResolution, SevenOfEightStillLocked)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 7);
    EXPECT_TRUE(spec.hasLock());  // 7 < 8
    EXPECT_EQ(DecideAfterAcquire(spec, /*hasLiveWhitelistCandidate*/ true), QuestActionPhase::EngageTarget);
}

TEST(QuestObjectiveResolution, EighthKillCompletesObjective)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 8);
    EXPECT_FALSE(spec.hasLock());  // 8 == 8, no longer constrains targeting
    // With the objective satisfied, the flow advances to the finisher rather than more killing.
    EXPECT_EQ(DecideAfterAcquire(spec, /*hasLiveWhitelistCandidate*/ true), QuestActionPhase::ResolveFinisher);
}

// =================================================================================================
// Exact finisher — a closer wrong reward NPC does not divert the hand-in.
// =================================================================================================
TEST(QuestObjectiveResolution, FinisherAcceptsOnlyExactEntry)
{
    QuestFinisherRef fin = BuildFinisher(kFactQ459);  // finisher creature 1992
    EXPECT_TRUE(AcceptsFinisher(fin, 1992));
    EXPECT_FALSE(AcceptsFinisher(fin, 3143));  // q789 finisher, closer or not
    EXPECT_FALSE(AcceptsFinisher(fin, 1988));  // a source, not the finisher
    EXPECT_FALSE(AcceptsFinisher(fin, 2082));  // q916 finisher
}

TEST(QuestObjectiveResolution, FinisherIsDistinctFromSources)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ459, 0);
    QuestFinisherRef fin = BuildFinisher(kFactQ459);
    // The finisher entry is never one of the objective sources.
    EXPECT_FALSE(spec.acceptsCreatureEntry(static_cast<uint32>(fin.signedEntry)));
}

TEST(QuestObjectiveResolution, FinisherSelectionIsStableAcrossPermutations)
{
    uint32 const taker = static_cast<uint32>(QuestRelationFlag::questTaker);
    std::vector<uint64> spawnOrder{FINISHER_SPAWN_HIGH, FINISHER_SPAWN_LOW, FINISHER_SPAWN_MID};
    std::sort(spawnOrder.begin(), spawnOrder.end());

    do
    {
        std::vector<QuestFinisherSpawnCandidate> candidates;
        for (uint64 spawnIdentity : spawnOrder)
            candidates.push_back(FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_LOCAL_MAP,
                                                   spawnIdentity, 25.0f, 30.0f));

        std::size_t const selected = SelectQuestFinisherSpawnCandidate(candidates, FINISHER_LOCAL_MAP);
        ASSERT_NE(selected, NoQuestFinisherSpawnSelection);
        EXPECT_EQ(candidates[selected].spawnIdentity, FINISHER_SPAWN_LOW);
        EXPECT_EQ(candidates[selected].signedEntry, kFactQ459.finisherSignedEntry);
    } while (std::next_permutation(spawnOrder.begin(), spawnOrder.end()));
}

TEST(QuestObjectiveResolution, NearestSameMapRouteWins)
{
    uint32 const taker = static_cast<uint32>(QuestRelationFlag::questTaker);
    std::vector<QuestFinisherSpawnCandidate> candidates{
        FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_LOCAL_MAP, FINISHER_SPAWN_LOW,
                          5.0f, 50.0f),
        FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_LOCAL_MAP, FINISHER_SPAWN_MID,
                          20.0f, 30.0f),
        FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_REMOTE_MAP, FINISHER_SPAWN_HIGH,
                          1.0f, 1.0f)};

    std::size_t const selected = SelectQuestFinisherSpawnCandidate(candidates, FINISHER_LOCAL_MAP);
    ASSERT_NE(selected, NoQuestFinisherSpawnSelection);
    EXPECT_EQ(candidates[selected].spawnIdentity, FINISHER_SPAWN_MID);
}

TEST(QuestObjectiveResolution, RemoteOnlyFinisherFailsClosed)
{
    uint32 const taker = static_cast<uint32>(QuestRelationFlag::questTaker);
    std::vector<QuestFinisherSpawnCandidate> candidates{
        FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_REMOTE_MAP, FINISHER_SPAWN_LOW,
                          1.0f, 1.0f)};

    EXPECT_EQ(SelectQuestFinisherSpawnCandidate(candidates, FINISHER_LOCAL_MAP),
              NoQuestFinisherSpawnSelection);
}

TEST(QuestObjectiveResolution, FinisherSelectionRequiresExactTakerRelation)
{
    uint32 const giver = static_cast<uint32>(QuestRelationFlag::questGiver);
    uint32 const taker = static_cast<uint32>(QuestRelationFlag::questTaker);
    std::vector<QuestFinisherSpawnCandidate> candidates{
        FinisherCandidate(kFactQ459.finisherSignedEntry, giver, FINISHER_LOCAL_MAP, FINISHER_SPAWN_LOW,
                          1.0f, 1.0f),
        FinisherCandidate(kFactQ459.finisherSignedEntry, taker, FINISHER_LOCAL_MAP, FINISHER_SPAWN_MID,
                          10.0f, 10.0f)};

    std::size_t const selected = SelectQuestFinisherSpawnCandidate(candidates, FINISHER_LOCAL_MAP);
    ASSERT_NE(selected, NoQuestFinisherSpawnSelection);
    EXPECT_EQ(candidates[selected].spawnIdentity, FINISHER_SPAWN_MID);

    candidates.pop_back();
    EXPECT_EQ(SelectQuestFinisherSpawnCandidate(candidates, FINISHER_LOCAL_MAP),
              NoQuestFinisherSpawnSelection);
}

// =================================================================================================
// Reward precondition — CanRewardQuest false -> blocker.
// =================================================================================================
TEST(QuestObjectiveResolution, CanRewardFalseIsHardBlocker)
{
    EXPECT_EQ(DecideReward(/*canRewardQuest*/ false), QuestActionPhase::Blocked);
}

TEST(QuestObjectiveResolution, CanRewardTrueProceedsToComplete)
{
    EXPECT_EQ(DecideReward(/*canRewardQuest*/ true), QuestActionPhase::Complete);
}

TEST(QuestObjectiveResolution, SelectedRewardStorageFailureClassifiesAsInventoryFull)
{
    EXPECT_EQ(ClassifyRewardFailure(/*canRewardQuest*/ true, /*canRewardSelectedReward*/ false),
              QuestFailureReason::InventoryFull);
}

TEST(QuestObjectiveResolution, GeneralRewardFailureRemainsCanRewardFalse)
{
    EXPECT_EQ(ClassifyRewardFailure(/*canRewardQuest*/ false, /*canRewardSelectedReward*/ false),
              QuestFailureReason::CanRewardFalse);
    EXPECT_EQ(ClassifyRewardFailure(/*canRewardQuest*/ true, /*canRewardSelectedReward*/ true),
              QuestFailureReason::None);
}

TEST(QuestObjectiveResolution, FullBagRecoveryOnlyInterruptsSafeIncompleteItemWork)
{
    QuestObjectiveSpec item = BuildSpec(kFactQ916, 0);
    using QuestInventoryReliefPolicy::ShouldRelieveIncompleteItem;

    EXPECT_TRUE(ShouldRelieveIncompleteItem(item.key.questId, item, 100, true, false));
    EXPECT_FALSE(ShouldRelieveIncompleteItem(item.key.questId, item, 99, true, false));
    EXPECT_FALSE(ShouldRelieveIncompleteItem(item.key.questId, item, 100, true, true));
    EXPECT_FALSE(ShouldRelieveIncompleteItem(item.key.questId, item, 100, false, false));
    EXPECT_FALSE(ShouldRelieveIncompleteItem(kFactQ792.questId, item, 100, true, false));

    item.currentCount = item.requiredCount;
    EXPECT_FALSE(ShouldRelieveIncompleteItem(item.key.questId, item, 100, true, false));
    QuestObjectiveSpec kill = BuildSpec(kFactQ792, 0);
    EXPECT_FALSE(ShouldRelieveIncompleteItem(kill.key.questId, kill, 100, true, false));
}

TEST(QuestObjectiveResolution, VendorReliefSpawnMustBeLocalPhasedStockedAndFriendly)
{
    using QuestInventoryReliefPolicy::AcceptVendorSpawn;
    constexpr uint32 map = 1;
    constexpr uint32 zone = 141;
    constexpr uint32 phase = 1;
    EXPECT_TRUE(AcceptVendorSpawn(map, zone, phase, map, zone, phase, true, true, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, 0, zone, phase, true, true, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, map, 12, phase, true, true, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, map, zone, 2, true, true, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, map, zone, phase, false, true, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, map, zone, phase, true, false, true));
    EXPECT_FALSE(AcceptVendorSpawn(map, zone, phase, map, zone, phase, true, true, false));
}

TEST(QuestObjectiveResolution, BagPurchaseRequiresFreshFullStateAndNoSafeGraySale)
{
    using QuestInventoryReliefPolicy::ShouldTryBagPurchase;
    EXPECT_TRUE(ShouldTryBagPurchase(true, true, false, 100, false, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(false, true, false, 100, false, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, false, false, 100, false, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, true, true, 100, false, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, true, false, 99, false, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, true, false, 100, true, true, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, true, false, 100, false, false, false));
    EXPECT_FALSE(ShouldTryBagPurchase(true, true, false, 100, false, true, true));
}

TEST(QuestObjectiveResolution, BagOfferIsOrdinaryCappedAndLeavesCopperReserve)
{
    using QuestInventoryReliefPolicy::AcceptBagOffer;
    EXPECT_TRUE(AcceptBagOffer(true, 6, 1, 0, true, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(false, 6, 1, 0, true, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 4, 1, 0, true, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 2, 0, true, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 1, true, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 0, false, 500, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 0, true, 501, 450, 950));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 0, true, 500, 501, 1100));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 0, true, 500, 450, 949));
}

TEST(QuestObjectiveResolution, BagPurchasePriceMatchesCoreFloatDiscountAndReserveBoundary)
{
    using QuestInventoryReliefPolicy::AcceptBagOffer;
    using QuestInventoryReliefPolicy::DiscountedBagPrice;
    EXPECT_EQ(DiscountedBagPrice(500, 1.0f), 500u);
    EXPECT_EQ(DiscountedBagPrice(500, 0.95f), 475u);
    EXPECT_EQ(DiscountedBagPrice(500, 0.9f), 450u);
    EXPECT_TRUE(AcceptBagOffer(true, 6, 1, 0, true, 500,
                               DiscountedBagPrice(500, 0.95f), 975));
    EXPECT_FALSE(AcceptBagOffer(true, 6, 1, 0, true, 500,
                                DiscountedBagPrice(500, 0.95f), 974));
}

TEST(QuestObjectiveResolution, IncompleteItemInventoryRecoveryUsesGrayVendorAndResumesSource)
{
    std::string const quest = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgAction.cpp");
    std::string const incomplete = SourceBetween(quest, "bool NewRpgDoQuestAction::DoIncompleteQuest(",
                                                "bool NewRpgDoQuestAction::DoCompletedQuest(");
    std::string const completed = SourceBetween(quest, "bool NewRpgDoQuestAction::DoCompletedQuest(",
                                               "case QuestActionPhase::ResolveFinisher:");
    std::string const vendor = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgBaseAction.cpp");
    std::string const relief = SourceBetween(vendor, "bool NewRpgBaseAction::TryRelieveInventoryAtVendor(",
                                             "bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(");

    EXPECT_NE(incomplete.find("TryRelieveInventoryAtVendor(data, /*incompleteItem*/ true)"), std::string::npos);
    EXPECT_NE(completed.find("TryRelieveInventoryAtVendor(data)"), std::string::npos);
    EXPECT_NE(relief.find("incompleteItem ? \"autowow-gray\""), std::string::npos);
    EXPECT_NE(relief.find("GetAllCreatureData()"), std::string::npos);
    EXPECT_NE(relief.find("GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR)"), std::string::npos);
    EXPECT_NE(relief.find("BuyItemFromVendorSlot(vendorGuid, offer->vendorSlot, offer->itemId, 1,"),
              std::string::npos);
    EXPECT_NE(relief.find("INVENTORY_SLOT_BAG_0, emptyBagSlot"), std::string::npos);
    EXPECT_NE(relief.find("lastQuestBagPurchaseAttemptByBot[botGuid] = getMSTime()"), std::string::npos);
    EXPECT_NE(relief.find("context->GetValue<uint8>(\"bag space\")->Reset()"), std::string::npos);
    EXPECT_NE(relief.find("incompleteItem ? QuestActionPhase::ResolveObjective : QuestActionPhase::InteractFinisher"),
              std::string::npos);
}

// =================================================================================================
// No random grind — repeated seeds produce zero non-whitelist offensive selections.
// =================================================================================================
TEST(QuestObjectiveResolution, RepeatedSeedsNeverSelectNonWhitelist)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);  // whitelist == {3101}
    // A candidate pool dominated by legal, attackable, but non-objective mobs plus the lone source.
    std::vector<uint32> pool = {1988, 1989, 3124, 3281, 1986, 3101, 2082, 1992};

    size_t nonWhitelistSelections = 0;
    for (unsigned seed = 0; seed < 256; ++seed)
    {
        std::vector<uint32> shuffled = pool;
        std::mt19937 rng(seed);
        std::shuffle(shuffled.begin(), shuffled.end(), rng);

        int32 pick = SelectOffensiveTarget(spec, shuffled);
        if (pick != 0 && !spec.acceptsCreatureEntry(static_cast<uint32>(pick)))
            ++nonWhitelistSelections;
    }
    EXPECT_EQ(nonWhitelistSelections, 0u);
}

TEST(QuestObjectiveResolution, GrindSelectionAlwaysReturnsWhitelistOrNone)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    // Pool with the source present -> must pick the source.
    EXPECT_EQ(SelectOffensiveTarget(spec, {1988, 3101, 1989}), 3101);
    // Pool without the source -> must pick nothing (caller then waits for respawn).
    EXPECT_EQ(SelectOffensiveTarget(spec, {1988, 1989, 3124}), 0);
}

// =================================================================================================
// Compatibility — no objective lock -> legacy behavior unchanged.
// =================================================================================================
TEST(QuestObjectiveResolution, UnsupportedSpecDoesNotLock)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    spec.supported = false;  // resolver could not model this objective -> legacy path
    EXPECT_FALSE(spec.hasLock());
    // With no lock, whitelist gating is inert (legacy targeting decides), so MayEngage is false for
    // all entries here — i.e. objective-lock imposes no constraint and the caller falls back.
    EXPECT_FALSE(MayEngageForObjective(spec, 3101, /*inAggroRange*/ true));
}

TEST(QuestObjectiveResolution, ZeroQuestIdDoesNotLock)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 0);
    spec.key.questId = 0;  // no active objective
    EXPECT_FALSE(spec.hasLock());
}

TEST(QuestObjectiveResolution, CompletedObjectiveDoesNotLock)
{
    QuestObjectiveSpec spec = BuildSpec(kFactQ792, 8);  // already complete
    EXPECT_FALSE(spec.hasLock());  // completed objective imposes no targeting constraint
}

TEST(QuestObjectiveResolutionSourceContract, ExplicitBridgePersistsDeferredNewRpgDirective)
{
    std::string const source = ReadModuleSource("src/AutoWow/AutoWowBridge.cpp");
    std::string const questParty = SourceBetween(source, "bool QuestParty()", "bool RecoverQuestParty()");
    ASSERT_FALSE(questParty.empty());

    std::size_t const enableDriver =
        questParty.find("ChangeStrategy(\"+grind,-travel,-move random,-follow,+new rpg\"");
    std::size_t const primeTick = questParty.find("DoSpecificAction(\"new rpg do quest\"");
    std::size_t const directivePostcondition = questParty.find("bool const rpgDirectiveActive");
    ASSERT_NE(enableDriver, std::string::npos);
    ASSERT_NE(primeTick, std::string::npos);
    ASSERT_NE(directivePostcondition, std::string::npos);
    EXPECT_LT(enableDriver, primeTick);
    EXPECT_LT(primeTick, directivePostcondition);
    EXPECT_NE(questParty.find("priming_action_started"), std::string::npos);
}

TEST(QuestObjectiveResolutionSourceContract, QuestObjectiveTelemetrySeparatesFinisherFromObjective)
{
    std::string const source = ReadModuleSource("src/AutoWow/QuestLogView.cpp");
    std::string const build = SourceBetween(source, "std::string Build(uint32 botGuid)",
                                            "std::string BuildAcceptance(uint32 botGuid");
    ASSERT_FALSE(build.empty());

    // The existing objective identity remains sourced from the active-lock spec. Completed-quest
    // finisher identity is additive and must not overwrite objective.quest_id with a completed quest.
    EXPECT_NE(build.find("spec.key.questId"), std::string::npos);
    EXPECT_NE(build.find("objectiveActive = spec.hasLock()"), std::string::npos);
    EXPECT_NE(build.find("ObjectiveInactiveReason(spec, objectiveActive, finisherMode)"),
              std::string::npos);

    // The finisher view is a separate stable object with receipt-first quest identity, explicit
    // phase/status, and route/bind/reward evidence.
    EXPECT_NE(build.find("FinisherTelemetryJson("), std::string::npos);
    EXPECT_NE(build.find("runtime->finisherReceipt.questId"), std::string::npos);
    EXPECT_NE(build.find("FinisherQuestIdSource"), std::string::npos);
    EXPECT_NE(source.find("finisher_mode_no_active_objective"), std::string::npos);
    EXPECT_NE(source.find("\\\"status\\\":"), std::string::npos);
    EXPECT_NE(source.find("\\\"phase\\\":"), std::string::npos);
    EXPECT_NE(source.find("\\\"route\\\":"), std::string::npos);
    EXPECT_NE(source.find("\\\"bind\\\":"), std::string::npos);
    EXPECT_NE(source.find("\\\"reward\\\":"), std::string::npos);
}

TEST(QuestObjectiveResolutionSourceContract, QuestObjectiveTelemetryIsReadOnly)
{
    std::string const source = ReadModuleSource("src/AutoWow/QuestLogView.cpp");
    std::string const build = SourceBetween(source, "std::string Build(uint32 botGuid)",
                                            "std::string BuildAcceptance(uint32 botGuid");
    ASSERT_FALSE(build.empty());

    EXPECT_EQ(build.find("SetQuest"), std::string::npos);
    EXPECT_EQ(build.find("->RewardQuest("), std::string::npos);
    EXPECT_EQ(build.find("->CompleteQuest("), std::string::npos);
    EXPECT_EQ(build.find("LoadGrid"), std::string::npos);
    EXPECT_NE(build.find("GetQuestRewardStatus"), std::string::npos);
}

// =================================================================================================
// Integration-only tests (DISABLED_): require a booted world / DB and are NOT part of the pure
// resolution suite. They document the wiring points for the maintenance-window integration pass.
//   * Enable by removing the DISABLED_ prefix once an IntegrationTestFixture-backed world is stood
//     up (see src/test/mocks/IntegrationTestFixture.h) and the resolver entry point is linkable.
// =================================================================================================
TEST(QuestObjectiveResolutionIntegration, DISABLED_ResolverBuildsSpecFromWorldRelations)
{
    // INTEGRATION-ONLY: drive the production resolver against DB-backed Quest / QuestStatusData /
    // creature_involvedrelation + item-drop maps for q792 and assert the emitted QuestObjectiveSpec
    // equals BuildSpec(kFactQ792, current). Cannot run without a loaded world.
    GTEST_SKIP() << "Requires a booted world (DB-backed quest relations).";
}

TEST(QuestObjectiveResolutionIntegration, DISABLED_AggroGeometryRespectsWhitelist)
{
    // INTEGRATION-ONLY: place a whitelisted source far and a non-source mob inside real aggro range,
    // then assert the live targeter still selects the source. Needs world geometry / grid.
    GTEST_SKIP() << "Requires world geometry (real aggro-range grid).";
}

// =================================================================================================
// Full-bag stall relief (AutoWow.QuestFullBagRelief.Enable): pure escalation + junk selection.
// =================================================================================================
TEST(QuestFullBagRelief, VendorWindowThenFallbackWithinOneEpisode)
{
    using namespace QuestInventoryReliefPolicy;
    FullBagRelief s;
    EXPECT_EQ(ObserveFullBag(s, 916, 1000), FullBagStep::TryVendor);
    EXPECT_EQ(ObserveFullBag(s, 916, 1000 + VendorReliefBudgetMs - 1), FullBagStep::TryVendor);
    // Observations keep the episode alive only while they arrive inside the gap.
    FullBagRelief live;
    uint32 t = 1000;
    for (; t < 1000 + VendorReliefBudgetMs; t += 1000)
        EXPECT_EQ(ObserveFullBag(live, 916, t), FullBagStep::TryVendor);
    EXPECT_EQ(ObserveFullBag(live, 916, t), FullBagStep::Fallback);
}

TEST(QuestFullBagRelief, QuestChangeAndEpisodeGapReopenVendorWindow)
{
    using namespace QuestInventoryReliefPolicy;
    FullBagRelief s;
    uint32 t = 0;
    for (; t <= VendorReliefBudgetMs; t += 1000)
        (void)ObserveFullBag(s, 916, t);
    EXPECT_EQ(ObserveFullBag(s, 916, t), FullBagStep::Fallback);
    EXPECT_EQ(ObserveFullBag(s, 917, t), FullBagStep::TryVendor);  // different quest
    FullBagRelief g;
    for (t = 0; t <= VendorReliefBudgetMs; t += 1000)
        (void)ObserveFullBag(g, 916, t);
    EXPECT_EQ(ObserveFullBag(g, 916, t + FullBagEpisodeGapMs + 1), FullBagStep::TryVendor);
}

TEST(QuestFullBagRelief, DeferBackoffIsExponentialCappedAndSuppressesReevaluation)
{
    using namespace QuestInventoryReliefPolicy;
    EXPECT_EQ(DeferBackoffMs(0), 5000u);
    EXPECT_EQ(DeferBackoffMs(1), 10000u);
    EXPECT_EQ(DeferBackoffMs(2), 20000u);
    EXPECT_EQ(DeferBackoffMs(3), 40000u);
    EXPECT_EQ(DeferBackoffMs(4), 60000u);
    EXPECT_EQ(DeferBackoffMs(255), 60000u);

    FullBagRelief s;
    uint32 const t0 = 50000;
    (void)ObserveFullBag(s, 916, t0);
    EXPECT_EQ(DeferFullBag(s, t0), 5000u);
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 4999), FullBagStep::Backoff);
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 5000), FullBagStep::TryVendor);  // fresh vendor window
    EXPECT_EQ(DeferFullBag(s, t0 + 5000), 10000u);
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 14999), FullBagStep::Backoff);

    // A long idle gap (bot set the quest aside) keeps the exponent: no reset to 5 s.
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 200000), FullBagStep::TryVendor);
    EXPECT_EQ(DeferFullBag(s, t0 + 200000), 20000u);

    ResolveFullBag(s);
    EXPECT_EQ(s.defers, 0u);
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 300000), FullBagStep::TryVendor);
}

TEST(QuestFullBagRelief, BackoffSurvivesMsTimerWrap)
{
    using namespace QuestInventoryReliefPolicy;
    FullBagRelief s;
    uint32 const t0 = 0xFFFFF000u;
    (void)ObserveFullBag(s, 916, t0);
    (void)DeferFullBag(s, t0);  // retryAt wraps past zero
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 1000), FullBagStep::Backoff);
    EXPECT_EQ(ObserveFullBag(s, 916, t0 + 5000), FullBagStep::TryVendor);
}

TEST(QuestFullBagRelief, JunkSelectionIsPoorOnlyLowestValueDeterministic)
{
    using namespace QuestInventoryReliefPolicy;
    auto poor = [](uint8 bag, uint8 slot, uint32 value)
    {
        BagItemFacts f;
        f.bag = bag;
        f.slot = slot;
        f.quality = PoorQuality;
        f.usageSafe = true;
        f.value = value;
        return f;
    };
    BagItemFacts quest = poor(255, 23, 0);
    quest.questRelated = true;
    BagItemFacts reagent = poor(255, 24, 0);
    reagent.retainedClass = true;
    BagItemFacts green = poor(255, 25, 0);
    green.quality = 2;  // soulbound gear above gray is never junk
    BagItemFacts useful = poor(255, 26, 0);
    useful.usageSafe = false;  // equip/keep usage
    BagItemFacts cheapLate = poor(20, 3, 7);
    BagItemFacts cheapEarly = poor(19, 5, 7);
    BagItemFacts dear = poor(255, 27, 90);

    std::vector<BagItemFacts> const items{quest, reagent, green, useful, dear, cheapLate, cheapEarly};
    std::vector<BagItemFacts> const one = SelectJunkToDestroy(items, NeededJunkSlots);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].bag, 19);
    EXPECT_EQ(one[0].slot, 5);

    std::vector<BagItemFacts> const all = SelectJunkToDestroy(items, 10);
    ASSERT_EQ(all.size(), 3u);  // exactly the three safe gray items, never more
    EXPECT_EQ(all[2].value, 90u);

    EXPECT_TRUE(SelectJunkToDestroy({quest, reagent, green, useful}, NeededJunkSlots).empty());
}
