/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "GatheringCandidatePolicy.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace
{
using AutoWowGather::Candidate;
using AutoWowGather::CooldownMap;
using AutoWowGather::Profession;
using AutoWowGather::Profile;

std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string_view FunctionBody(std::string const& source, std::string_view start, std::string_view end)
{
    std::size_t const begin = source.find(start);
    if (begin == std::string::npos)
        return {};
    std::size_t const finish = source.find(end, begin + start.size());
    return finish == std::string::npos ? std::string_view(source).substr(begin)
                                      : std::string_view(source).substr(begin, finish - begin);
}

Candidate MakeCandidate(std::uint64_t spawnId, Profession profession, float x, std::uint32_t requiredSkill = 1,
                        std::uint32_t mapId = 0, std::uint32_t nodeLevel = 1)
{
    Candidate candidate;
    candidate.spawnId = spawnId;
    candidate.entry = static_cast<std::uint32_t>(1000 + spawnId);
    candidate.mapId = mapId;
    candidate.phaseMask = 1;
    candidate.x = x;
    candidate.profession = profession;
    candidate.requiredSkill = requiredSkill;
    candidate.nodeLevel = nodeLevel;
    return candidate;
}

Profile SkilledProfile()
{
    Profile profile;
    profile.mapId = 0;
    profile.phaseMask = 1;
    profile.level = 20;
    profile.herbalismSkill = 100;
    profile.miningSkill = 100;
    profile.hasMiningTool = true;
    profile.maxDistance = 1000.0f;
    profile.maxNodeLevelAboveBot = 5;
    return profile;
}
}

TEST(WorkerGatherEligibility, FiltersProfessionRankToolMapPhaseLevelAndRange)
{
    Profile profile = SkilledProfile();
    CooldownMap cooldowns;

    EXPECT_TRUE(AutoWowGather::IsEligible(MakeCandidate(1, Profession::Herbalism, 20.0f, 100), profile,
                                           cooldowns, 1000));
    EXPECT_TRUE(AutoWowGather::IsEligible(MakeCandidate(2, Profession::Mining, 20.0f, 100), profile,
                                           cooldowns, 1000));

    profile.herbalismSkill = 99;
    EXPECT_FALSE(AutoWowGather::IsEligible(MakeCandidate(3, Profession::Herbalism, 20.0f, 100), profile,
                                            cooldowns, 1000));
    profile.herbalismSkill = 100;

    profile.hasMiningTool = false;
    EXPECT_FALSE(AutoWowGather::IsEligible(MakeCandidate(4, Profession::Mining, 20.0f, 100), profile,
                                            cooldowns, 1000));
    profile.hasMiningTool = true;

    EXPECT_FALSE(AutoWowGather::IsEligible(MakeCandidate(5, Profession::Herbalism, 20.0f, 1, 1), profile,
                                            cooldowns, 1000));

    Candidate wrongPhase = MakeCandidate(6, Profession::Herbalism, 20.0f);
    wrongPhase.phaseMask = 2;
    EXPECT_FALSE(AutoWowGather::IsEligible(wrongPhase, profile, cooldowns, 1000));
    EXPECT_FALSE(AutoWowGather::IsEligible(MakeCandidate(7, Profession::Herbalism, 20.0f, 1, 0, 26), profile,
                                            cooldowns, 1000));
    EXPECT_FALSE(AutoWowGather::IsEligible(MakeCandidate(8, Profession::Herbalism, 1001.0f), profile,
                                            cooldowns, 1000));
}

TEST(WorkerGatherSelection, NearestChoiceIsDeterministicAndMissCooldownAdvancesRoute)
{
    Profile profile = SkilledProfile();
    std::vector<Candidate> candidates = {
        MakeCandidate(30, Profession::Herbalism, 30.0f),
        MakeCandidate(20, Profession::Mining, 10.0f),
        MakeCandidate(10, Profession::Herbalism, 10.0f),
    };
    // Equal-distance tie is entry then spawn id, independent of source-container order.
    candidates[1].entry = 2000;
    candidates[2].entry = 2000;

    CooldownMap cooldowns;
    auto selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1000);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);

    cooldowns[10] = 1100;
    selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1000);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 20u);

    selected = AutoWowGather::SelectCandidate(candidates, profile, cooldowns, 1100);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(candidates[*selected].spawnId, 10u);
}

TEST(WorkerGatherSourceContract, UsesNoTeleportMmapAndCanonicalLootWithoutMutationOrPoolOracle)
{
    std::string const action = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    ASSERT_FALSE(action.empty());
    EXPECT_NE(action.find("MoveFarTo(destination, /*questNoTeleport*/ true, &stuck)"), std::string::npos);
    std::size_t const lootDistanceGuard =
        action.find("bot->GetDistance(go) <= sPlayerbotAIConfig.lootDistance");
    std::size_t const gatherHandoff = action.find("DoSpecificAction(\"add gathering loot\"");
    ASSERT_NE(lootDistanceGuard, std::string::npos);
    ASSERT_NE(gatherHandoff, std::string::npos);
    EXPECT_LT(lootDistanceGuard, gatherHandoff);
    // The fixed-target Oracle callback performs its own exact source resolution before the
    // ordinary worker path. Scope this ordering contract to the ordinary Execute body so the
    // callback's independent, exact lookup cannot make the ordinary safety ordering fail.
    std::size_t const ordinaryBegin = action.find("bool WorkerGatherAction::Execute(Event /*event*/)");
    ASSERT_NE(ordinaryBegin, std::string::npos);
    std::string_view const ordinary = std::string_view(action).substr(ordinaryBegin);
    std::size_t const gridLoad = ordinary.find("LoadGrid(candidate->x, candidate->y)");
    std::size_t const spawnLookup = ordinary.find("GetGameObjectBySpawnIdStore");
    ASSERT_NE(gridLoad, std::string::npos);
    ASSERT_NE(spawnLookup, std::string::npos);
    EXPECT_LT(gridLoad, spawnLookup);
    EXPECT_NE(action.find("Event(\"worker gather\", go->GetGUID())"), std::string::npos);
    EXPECT_EQ(action.find("DoSpecificAction(\"add gathering loot\", Event(), true)"), std::string::npos);
    EXPECT_EQ(action.find("ObjectGuid::Create<HighGuid::GameObject>"), std::string::npos);
    EXPECT_NE(action.find("not_spawned_or_ready_in_sight"), std::string::npos);

    std::string const addLoot = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/AddLootAction.cpp");
    ASSERT_FALSE(addLoot.empty());
    std::string_view const gatheringExecute =
        FunctionBody(addLoot, "bool AddGatheringLootAction::Execute", "bool AddGatheringLootAction::AddLoot");
    ASSERT_FALSE(gatheringExecute.empty());
    EXPECT_NE(gatheringExecute.find("event.getObject()"), std::string_view::npos);
    EXPECT_NE(gatheringExecute.find("return AddLoot(guid)"), std::string_view::npos);
    EXPECT_NE(gatheringExecute.find("return AddAllLootAction::Execute(event)"), std::string_view::npos);

    for (std::string_view forbidden : {"PoolMgr", "TeleportTo(", "SetSkill(", "UpdateSkill(",
                                       "StoreNewItem", "AddItem(", "ModifyMoney("})
        EXPECT_EQ(action.find(forbidden), std::string::npos) << forbidden;

    std::string const state = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/GatheringWorkerState.cpp");
    ASSERT_FALSE(state.empty());
    EXPECT_NE(state.find("sObjectMgr->GetAllGOData()"), std::string::npos);
    EXPECT_NE(state.find("SKILL_HERBALISM"), std::string::npos);
    EXPECT_NE(state.find("SKILL_MINING"), std::string::npos);
    EXPECT_EQ(state.find("PoolMgr"), std::string::npos);

    std::string const loot = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/LootAction.cpp");
    ASSERT_FALSE(loot.empty());
    EXPECT_NE(loot.find("IsPendingGatherSourceLoot"), std::string::npos);
    EXPECT_NE(loot.find("GetLootGUID()"), std::string::npos);
    std::string_view const gatherAdmission = FunctionBody(
        state, "bool IsPendingGatherSourceLoot", "std::optional<GatherCreditDecision> ObserveCandidateCredit");
    ASSERT_FALSE(gatherAdmission.empty());
    EXPECT_NE(gatherAdmission.find("receipt.prepared"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("GetLootGUID() != sourceGuid"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("liveSource->GetGUID() != sourceGuid"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("worker.candidateRuntimeGuid != sourceGuid"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("receipt.sourceGuid != sourceGuid"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("liveSource->loot.items"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("liveSource->loot.quest_items"), std::string_view::npos);
    EXPECT_NE(gatherAdmission.find("liveGeneratedItemMatches"), std::string_view::npos);
    EXPECT_EQ(gatherAdmission.find("receipt.sourceItemEntries"), std::string_view::npos);
    EXPECT_EQ(gatherAdmission.find("receipt.sourceYieldsRequestedMaterial"), std::string_view::npos);
    EXPECT_EQ(gatherAdmission.find("receipt.awaitingCredit"), std::string_view::npos);
    EXPECT_EQ(gatherAdmission.find("receipt.sourceMatched"), std::string_view::npos);
    std::string_view const storeLoot = FunctionBody(
        loot, "bool StoreLootAction::Execute", "bool StoreLootAction::IsLootAllowed");
    ASSERT_FALSE(storeLoot.empty());
    std::size_t const autostorePacket = storeLoot.find("CMSG_AUTOSTORE_LOOT_ITEM");
    ASSERT_NE(autostorePacket, std::string_view::npos);
    EXPECT_NE(storeLoot.find("QueuePacket(packet)", autostorePacket), std::string_view::npos);
    EXPECT_EQ(loot.find("ITEM_CLASS_TRADE_GOODS"), std::string::npos);
    EXPECT_NE(state.find("sourceGuid.IsGameObject()"), std::string::npos);
    EXPECT_NE(state.find("sourceItemEntries.begin()"), std::string::npos);
    EXPECT_NE(state.find("requestedMaterialItemId == 0"), std::string::npos);
}

TEST(WorkerGatherSourceContract, ExactGatherLootBypassesOnlyOrdinaryStrategyAfterCapGate)
{
    std::string const loot = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/LootAction.cpp");
    ASSERT_FALSE(loot.empty());
    std::string_view const allowed = FunctionBody(
        loot, "bool StoreLootAction::IsLootAllowed", "bool ReleaseLootAction::Execute");
    ASSERT_FALSE(allowed.empty());

    std::size_t const templateLookup = allowed.find("sObjectMgr->GetItemTemplate(itemid)");
    std::size_t const missingTemplateGate = allowed.find("if (!proto)", templateLookup);
    std::size_t const maxCount = allowed.find("uint32 max = proto->MaxCount", missingTemplateGate);
    std::size_t const cappedItemGate = allowed.find(
        "if (max > 0 && botAI->GetBot()->HasItemCount(itemid, max, true))", maxCount);
    std::size_t const cappedItemReject = allowed.find("return false;", cappedItemGate);
    std::size_t const exactGatherBypass = allowed.find("IsPendingGatherSourceLoot", cappedItemReject);
    std::size_t const ordinaryStrategy = allowed.find("lootStrategy->CanLoot(proto, context)", exactGatherBypass);

    ASSERT_NE(templateLookup, std::string_view::npos);
    ASSERT_NE(missingTemplateGate, std::string_view::npos);
    ASSERT_NE(maxCount, std::string_view::npos);
    ASSERT_NE(cappedItemGate, std::string_view::npos);
    ASSERT_NE(cappedItemReject, std::string_view::npos);
    ASSERT_NE(exactGatherBypass, std::string_view::npos);
    ASSERT_NE(ordinaryStrategy, std::string_view::npos);
    EXPECT_LT(templateLookup, missingTemplateGate);
    EXPECT_LT(missingTemplateGate, maxCount);
    EXPECT_LT(maxCount, cappedItemGate);
    EXPECT_LT(cappedItemGate, cappedItemReject);
    EXPECT_LT(cappedItemReject, exactGatherBypass);
    EXPECT_LT(exactGatherBypass, ordinaryStrategy);
}

TEST(WorkerGatherTelemetryContract, StoreLootReportsEveryReadOnlyRejectionStage)
{
    std::string const loot = ReadSource(ModuleRoot() / "src/Ai/Base/Actions/LootAction.cpp");
    ASSERT_FALSE(loot.empty());
    std::string_view const execute = FunctionBody(
        loot, "bool StoreLootAction::Execute", "bool StoreLootAction::IsLootAllowed");
    ASSERT_FALSE(execute.empty());

    for (std::string_view marker : {"RecordAutoWowLootPacketItems(items)",
                                    "RecordAutoWowLootAllowedOwnerSlot()",
                                    "RecordAutoWowLootSlotTypeRejected()",
                                    "RecordAutoWowLootPolicyRejected()",
                                    "RecordAutoWowLootMissingTemplate()",
                                    "RecordAutoWowLootBagReserveRejected()",
                                    "RecordAutoWowAutostoreLootPacket()"})
        EXPECT_NE(execute.find(marker), std::string_view::npos) << marker;

    std::string const state = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/GatheringWorkerState.cpp");
    ASSERT_FALSE(state.empty());
    std::string_view const admission = FunctionBody(
        state, "bool IsPendingGatherSourceLoot", "std::optional<GatherCreditDecision> ObserveCandidateCredit");
    ASSERT_FALSE(admission.empty());
    for (std::string_view reason : {"GatherLootAdmissionNotWorker", "GatherLootAdmissionNotPrepared",
                                    "GatherLootAdmissionGuidMismatch", "GatherLootAdmissionRuntimeMismatch",
                                    "GatherLootAdmissionSpawnMismatch", "GatherLootAdmissionEntryMismatch",
                                    "GatherLootAdmissionLiveItemMismatch", "GatherLootAdmissionRequestedMismatch"})
        EXPECT_NE(admission.find(reason), std::string_view::npos) << reason;
    EXPECT_NE(admission.find("lootAdmissionFailureMask = failures"), std::string_view::npos);
}

TEST(WorkerGatherSourceContract, OracleExactPathOwnsPausesAndNeverFallsBackOrRandomizes)
{
    std::string const action = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    ASSERT_FALSE(action.empty());

    std::string_view const dispatch = FunctionBody(
        action, "bool WorkerGatherAction::DispatchFixedTargetNativeStep",
        "AutoWowOracleGatherExecutor::NativeStepObservation WorkerGatherAction::ExecuteFixedTargetNativeStep");
    ASSERT_FALSE(dispatch.empty());
    EXPECT_NE(dispatch.find("MoveDeterministicToExact"), std::string_view::npos);
    EXPECT_EQ(dispatch.find("SelectCandidateFor"), std::string_view::npos);
    EXPECT_EQ(dispatch.find("MoveFarTo"), std::string_view::npos);
    EXPECT_EQ(dispatch.find("TeleportTo("), std::string_view::npos);
    EXPECT_EQ(dispatch.find("rand_norm"), std::string_view::npos);

    std::string_view const fixed = FunctionBody(
        action, "AutoWowOracleGatherExecutor::NativeStepObservation WorkerGatherAction::ExecuteFixedTargetNativeStep",
        "AutoWowOracleGatherExecutor::DispatchResult WorkerGatherAction::ExecuteFixedTargetGather");
    ASSERT_FALSE(fixed.empty());
    EXPECT_NE(fixed.find("IsAutoWowPaused"), std::string_view::npos);
    EXPECT_NE(fixed.find("AutoWowOracleRuntime::Owns"), std::string_view::npos);
    EXPECT_NE(fixed.find("AdoptExactCandidate"), std::string_view::npos);
    EXPECT_EQ(fixed.find("SelectCandidateFor"), std::string_view::npos);
    EXPECT_EQ(fixed.find("MoveFarTo"), std::string_view::npos);
    EXPECT_EQ(fixed.find("TeleportTo("), std::string_view::npos);
    EXPECT_EQ(fixed.find("rand_norm"), std::string_view::npos);
    EXPECT_EQ(fixed.find("lootConfirmed"), std::string_view::npos);

    std::string const state = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/GatheringWorkerState.cpp");
    ASSERT_FALSE(state.empty());
    EXPECT_NE(state.find("oracleExactSourceOnly"), std::string::npos);
    EXPECT_NE(state.find("ordinary_selector_suppressed"), std::string::npos);
    EXPECT_NE(state.find("SourceYieldsMaterial"), std::string::npos);

    std::string const ownership = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowOracleOwnershipGate.h");
    ASSERT_FALSE(ownership.empty());
    EXPECT_NE(ownership.find("bool IsOwned"), std::string::npos);
}

TEST(WorkerGatherSourceContract, ProductionCallbackFeedsCanonicalReceiptIntoExecutorResult)
{
    std::string const action = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    ASSERT_FALSE(action.empty());
    std::string const actionHeader = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.h");
    ASSERT_FALSE(actionHeader.empty());

    std::size_t const callbackBegin = action.find(
        "AutoWowOracleGatherExecutor::NativeStepObservation WorkerGatherAction::ExecuteFixedTargetNativeStep");
    std::size_t const productionOverload = action.find(
        "AutoWowOracleGatherExecutor::DispatchResult WorkerGatherAction::ExecuteFixedTargetGather(",
        callbackBegin);
    ASSERT_NE(callbackBegin, std::string::npos);
    ASSERT_NE(productionOverload, std::string::npos);
    std::string_view const callback = std::string_view(action).substr(callbackBegin,
        productionOverload - callbackBegin);

    // The production callback must enter the canonical receipt loop; the loop itself polls the
    // real worker receipt observer before and after one exact native step, and hands the typed
    // observation to NativeStepObservation::credit.
    EXPECT_NE(callback.find("ExecuteCanonicalReceiptLoop"), std::string_view::npos);
    EXPECT_NE(action.find("ObserveCandidateCredit(context->bot, context->botAI, observation)"),
              std::string::npos);
    EXPECT_NE(actionHeader.find("result.credit = beforeCredit"), std::string::npos);
    EXPECT_NE(actionHeader.find("result.credit = afterCredit"), std::string::npos);
    EXPECT_NE(actionHeader.find("if (!nativeStep)"), std::string::npos);
    EXPECT_NE(action.find("CaptureFixedTargetEvidence"), std::string::npos);
    EXPECT_NE(action.find("controlledReceiptSnapshot"), std::string::npos);
    EXPECT_NE(action.find("MakeGatherCreditObservation"), std::string::npos);
    EXPECT_NE(actionHeader.find("ExecuteProductionReceiptLoop"), std::string::npos);
    EXPECT_EQ(callback.find("SelectCandidateFor"), std::string_view::npos);
    EXPECT_EQ(callback.find("MarkCandidateGathered"), std::string_view::npos);
}

TEST(WorkerGatherSourceContract, ExactPathRejectsPartialAndDirectFallbackTypes)
{
    std::string const action = ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherAction.cpp");
    ASSERT_FALSE(action.empty());
    EXPECT_NE(action.find("PATHFIND_INCOMPLETE"), std::string::npos);
    EXPECT_NE(action.find("PATHFIND_NOPATH"), std::string::npos);
    EXPECT_NE(action.find("PATHFIND_FARFROMPOLY"), std::string::npos);
    EXPECT_NE(action.find("PATHFIND_NOT_USING_PATH"), std::string::npos);
    std::string_view const fixed = FunctionBody(
        action, "bool WorkerGatherAction::MoveDeterministicToExact",
        "AutoWowOracleGatherExecutor::NativeStepObservation WorkerGatherAction::ExecuteFixedTargetNativeStep");
    ASSERT_FALSE(fixed.empty());
    EXPECT_EQ(fixed.find("MoveFarTo"), std::string_view::npos);
    EXPECT_EQ(fixed.find("TeleportTo("), std::string_view::npos);
}

TEST(WorkerGatherSourceContract, OrdinaryGatherRemainsLocalAndWorkerStrategyIsSeparate)
{
    std::string const ordinary = ReadSource(ModuleRoot() / "src/Ai/Base/Strategy/LootNonCombatStrategy.cpp");
    ASSERT_FALSE(ordinary.empty());
    std::string_view const gather =
        FunctionBody(ordinary, "void GatherStrategy::InitTriggers", "void RevealStrategy::InitTriggers");
    ASSERT_FALSE(gather.empty());
    EXPECT_NE(gather.find("add gathering loot"), std::string_view::npos);
    EXPECT_EQ(gather.find("worker gather"), std::string_view::npos);
    EXPECT_EQ(gather.find("MoveFarTo"), std::string_view::npos);

    std::string const strategy =
        ReadSource(ModuleRoot() / "src/Ai/World/Gathering/WorkerGatherStrategy.cpp");
    ASSERT_FALSE(strategy.empty());
    EXPECT_NE(strategy.find("worker gather seek"), std::string::npos);
}

TEST(WorkerGatherLifecycle, DeployIsExplicitIsolatedAndClearedOnModeChangeOrDeactivate)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    std::string_view const deploy = FunctionBody(bridge, "bool DeployLeagueMember()", "bool RouteParty()");
    ASSERT_FALSE(deploy.empty());
    EXPECT_NE(deploy.find("AutoWowGather::ActivateWorker"), std::string_view::npos);
    EXPECT_NE(deploy.find("AutoWowGather::DeactivateWorker"), std::string_view::npos);
    EXPECT_NE(deploy.find("AutoWowPolicy::SetNoTeleport(m_request.botGuid, true)"), std::string_view::npos);
    std::size_t const noTeleport = deploy.find("AutoWowPolicy::SetNoTeleport(m_request.botGuid, true)");
    std::size_t const deathRecovery = deploy.find("DoSpecificAction(\n                    \"find corpse\"");
    ASSERT_NE(deathRecovery, std::string_view::npos);
    EXPECT_LT(noTeleport, deathRecovery);
    EXPECT_NE(deploy.find("persistent worker deploy recovery"), std::string_view::npos);
    EXPECT_EQ(deploy.find("TeleportTo("), std::string_view::npos);
    EXPECT_EQ(deploy.find("ResurrectPlayer("), std::string_view::npos);
    EXPECT_NE(deploy.find("worker_deploy_deferred_flight"), std::string_view::npos);
    EXPECT_NE(deploy.find("+worker gather,+gather,+loot"), std::string_view::npos);
    for (std::string_view removed : {"-new rpg", "-rpg", "-travel", "-stay", "-grind", "-move random",
                                     "-follow"})
        EXPECT_NE(deploy.find(removed), std::string_view::npos) << removed;

    std::string_view const deactivate = FunctionBody(bridge, "bool DeactivateBot()", "bool CreateParty()");
    ASSERT_FALSE(deactivate.empty());
    EXPECT_NE(deactivate.find("AutoWowGather::DeactivateWorker"), std::string_view::npos);
    EXPECT_NE(deactivate.find("AutoWowPolicy::SetNoTeleport(m_request.botGuid, false)"), std::string_view::npos);

    std::string_view const createParty = FunctionBody(bridge, "bool CreateParty()", "bool RallyParty()");
    ASSERT_FALSE(createParty.empty());
    EXPECT_NE(createParty.find("AutoWowGather::DeactivateWorker"), std::string_view::npos);
    EXPECT_NE(createParty.find("-worker gather"), std::string_view::npos);

    std::string const playerbot = ReadSource(ModuleRoot() / "src/Bot/PlayerbotAI.cpp");
    ASSERT_FALSE(playerbot.empty());
    std::string_view const active = FunctionBody(playerbot, "bool PlayerbotAI::AllowActive", "bool PlayerbotAI::AllowActivity");
    ASSERT_FALSE(active.empty());
    EXPECT_NE(active.find("AutoWowGather::IsExplicitWorker"), std::string_view::npos);
}

TEST(WorkerGatherTelemetryContract, SnapshotAndConfigExposeRouteActivityAndReasons)
{
    std::string const bridge = ReadSource(ModuleRoot() / "src/AutoWow/AutoWowBridge.cpp");
    ASSERT_FALSE(bridge.empty());
    for (std::string_view field : {"\\\"strategies\\\"", "\\\"activity\\\"", "\\\"suppression_reason\\\"",
                                   "\\\"gather_route\\\"", "\\\"explicit_worker\\\"", "\\\"event_sequence\\\"",
                                   "\\\"idle_reason\\\"", "\\\"candidate\\\"",
                                   "\\\"loot_packet_item_delta\\\"", "\\\"loot_allowed_owner_slot_delta\\\"",
                                   "\\\"loot_slot_type_rejected_delta\\\"", "\\\"loot_policy_rejected_delta\\\"",
                                   "\\\"loot_missing_template_delta\\\"", "\\\"loot_bag_reserve_rejected_delta\\\"",
                                   "\\\"loot_admission_failure_mask\\\"", "\\\"loot_admission_failure\\\""})
        EXPECT_NE(bridge.find(field), std::string::npos) << field;

    std::string const config = ReadSource(ModuleRoot() / "conf/playerbots.conf.dist");
    ASSERT_FALSE(config.empty());
    for (std::string_view key : {"AutoWow.GatherSeek.Enabled", "AutoWow.GatherSeek.SearchDistance",
                                 "AutoWow.GatherSeek.SightDistance", "AutoWow.GatherSeek.MissCooldownSeconds",
                                 "AutoWow.GatherSeek.MaxNodeLevelAboveBot"})
        EXPECT_NE(config.find(key), std::string::npos) << key;
}
