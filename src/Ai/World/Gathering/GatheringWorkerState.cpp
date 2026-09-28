/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "GatheringWorkerState.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "G3D/Quat.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Bag.h"
#include "LastMovementValue.h"
#include "LootValues.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"

namespace AutoWowGather
{
namespace
{
struct WorkerState
{
    bool explicitWorker = false;
    DeathRecoveryState deathRecovery;
    // Once an Oracle source is published, this worker is fail-closed until that exact source is
    // republished or the worker is deactivated. This prevents ordinary selector fallback.
    bool oracleExactSourceOnly = false;
    AutoWowOracle::GatherSourceReference oracleSource;
    AutoWowGatherRuntimePolicy::ExactMotionProvenance oracleMotion;
    std::optional<Candidate> candidate;
    CandidateLease candidateLease;
    bool candidateSafetyAccepted = false;
    CooldownMap cooldowns;
    std::string status = "inactive";
    std::string event = "none";
    std::string idleReason = "not_worker_gather";
    std::uint64_t eventSequence = 0;
    std::uint32_t missCount = 0;
    std::uint32_t candidateAcquisitionCount = 0;
    std::uint32_t candidateProgressCount = 0;
    std::uint32_t leaseNoProgressExpirationCount = 0;
    std::uint32_t leaseMaxAgeExpirationCount = 0;
    std::uint32_t liquidRejectionCount = 0;
    ObjectGuid candidateRuntimeGuid;

    struct GatherReceipt
    {
        struct ItemBaseline
        {
            std::uint32_t entry = 0;
            std::uint32_t before = 0;
            std::uint32_t after = 0;
        };

        bool prepared = false;
        bool awaitingCredit = false;
        bool countersInitialized = false;
        bool lootOpened = false;
        std::string evidence = "none";
        std::uint64_t candidateSpawnId = 0;
        std::uint32_t candidateEntry = 0;
        Profession profession = Profession::None;
        SkillType skillType = SKILL_NONE;
        ObjectGuid sourceGuid;
        std::uint32_t sourceEntry = 0;
        std::uint64_t sourceSpawnId = 0;
        std::uint32_t sourceLootGenerationAtAttempt = 0;
        bool sourceMatched = false;
        bool sourceLootGenerationObserved = false;
        bool sourceYieldsRequestedMaterial = false;
        bool sourceLootProcessingObserved = false;
        bool sourcePresentAtLastObservation = false;
        bool sourceLootConsumedObserved = false;
        std::uint32_t sourceUnlootedCount = 0;
        std::uint32_t sourceLootItemCount = 0;
        std::uint8_t bagSpacePercent = 0;
        std::string failureDetail = "none";
        std::vector<std::uint32_t> sourceItemEntries;
        std::unordered_map<std::uint32_t, std::uint32_t> itemCountsBefore;
        std::vector<ItemBaseline> itemBaselines;
        std::uint32_t itemEntry = 0;
        std::uint32_t itemBefore = 0;
        std::uint32_t itemAfter = 0;
        std::uint32_t requestedMaterialItemId = 0;
        AutoWowOracle::GatherGoal goal = AutoWowOracle::GatherGoal::Unknown;
        bool matchingMaterialDelta = false;
        bool matchingSkillDelta = false;
        std::uint64_t preparedAtSeconds = 0;
        std::uint64_t attemptedAtSeconds = 0;
        std::uint64_t lootResponseAtAttempt = 0;
        std::uint64_t storeLootExecutionAtAttempt = 0;
        std::uint64_t autostoreLootPacketAtAttempt = 0;
        std::uint64_t lootReleasePacketAtAttempt = 0;
        std::uint64_t lootPacketItemAtAttempt = 0;
        std::uint64_t lootAllowedOwnerSlotAtAttempt = 0;
        std::uint64_t lootSlotTypeRejectedAtAttempt = 0;
        std::uint64_t lootPolicyRejectedAtAttempt = 0;
        std::uint64_t lootMissingTemplateAtAttempt = 0;
        std::uint64_t lootBagReserveRejectedAtAttempt = 0;
        std::uint64_t lootResponseDelta = 0;
        std::uint64_t storeLootExecutionDelta = 0;
        std::uint64_t autostoreLootPacketDelta = 0;
        std::uint64_t lootReleasePacketDelta = 0;
        std::uint64_t lootPacketItemDelta = 0;
        std::uint64_t lootAllowedOwnerSlotDelta = 0;
        std::uint64_t lootSlotTypeRejectedDelta = 0;
        std::uint64_t lootPolicyRejectedDelta = 0;
        std::uint64_t lootMissingTemplateDelta = 0;
        std::uint64_t lootBagReserveRejectedDelta = 0;
        std::uint32_t lootAdmissionFailureMask = GatherLootAdmissionNone;
        std::uint32_t skillBefore = 0;
        std::uint32_t skillAfter = 0;
    } gatherReceipt;
};

std::mutex stateMutex;
std::unordered_map<std::uint32_t, WorkerState> workerStates;
std::once_flag candidateCacheFlag;
std::vector<Candidate> candidateCache;

bool HasMiningTool(Player* bot)
{
    static std::uint32_t const tools[] = {
        756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893
    };
    return std::any_of(std::begin(tools), std::end(tools),
                       [bot](std::uint32_t itemId) { return bot->HasItemCount(itemId, 1); });
}

void BuildCandidateCache()
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST || !goInfo->GetLootId())
            continue;

        LockEntry const* lock = sLockStore.LookupEntry(goInfo->GetLockId());
        if (!lock)
            continue;

        for (std::uint8_t index = 0; index < 8; ++index)
        {
            if (lock->Type[index] != LOCK_KEY_SKILL)
                continue;

            std::uint32_t const skill = SkillByLockType(LockType(lock->Index[index]));
            Profession profession = Profession::None;
            if (skill == SKILL_HERBALISM)
                profession = Profession::Herbalism;
            else if (skill == SKILL_MINING)
                profession = Profession::Mining;
            else
                continue;

            Candidate candidate;
            candidate.spawnId = static_cast<std::uint64_t>(spawnId);
            candidate.entry = data.id;
            candidate.mapId = data.mapid;
            candidate.phaseMask = data.phaseMask;
            candidate.x = data.posX;
            candidate.y = data.posY;
            candidate.z = data.posZ;
            candidate.profession = profession;
            candidate.requiredSkill = std::max<std::uint32_t>(1, lock->Skill[index]);
            candidate.nodeLevel = goInfo->chest.level;
            candidate.name = goInfo->name;
            candidateCache.push_back(std::move(candidate));
            break;
        }
    }
}

std::uint64_t NowSeconds()
{
    using Seconds = std::chrono::seconds;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Record(WorkerState& state, std::string status, std::string event, std::string idleReason)
{
    if (state.status == status && state.event == event && state.idleReason == idleReason)
        return;

    state.status = std::move(status);
    state.event = std::move(event);
    state.idleReason = std::move(idleReason);
    ++state.eventSequence;
}

void Increment(std::uint32_t& counter)
{
    if (counter != std::numeric_limits<std::uint32_t>::max())
        ++counter;
}

std::uint64_t CooldownDeadline(std::uint64_t nowSeconds, std::uint32_t cooldownSeconds)
{
    std::uint64_t const boundedCooldown = std::max<std::uint64_t>(1, cooldownSeconds);
    return nowSeconds > std::numeric_limits<std::uint64_t>::max() - boundedCooldown
               ? std::numeric_limits<std::uint64_t>::max()
               : nowSeconds + boundedCooldown;
}

void ClearCandidateTracking(WorkerState& state)
{
    state.candidate.reset();
    state.candidateLease = CandidateLease{};
    state.candidateSafetyAccepted = false;
    state.candidateRuntimeGuid.Clear();
    state.oracleSource = {};
}

void PublishDeathRecoveryStatus(WorkerState& worker)
{
    DeathRecoveryState const& recovery = worker.deathRecovery;
    std::string status = "inactive";
    if (recovery.active)
        status = recovery.phase == DeathRecoveryPhase::Blocked ? "blocked" : "recovering";
    else if (recovery.phase == DeathRecoveryPhase::Recovered)
        status = "seeking";

    Record(worker, std::move(status), DeathRecoveryEventName(recovery.event),
           DeathRecoveryIdleReasonName(recovery.idleReason));
}

SkillType SkillForProfession(Profession profession)
{
    switch (profession)
    {
        case Profession::Herbalism: return SKILL_HERBALISM;
        case Profession::Mining: return SKILL_MINING;
        case Profession::Skinning: return SKILL_SKINNING;
        default: return SKILL_NONE;
    }
}

std::uint64_t CounterDelta(std::uint64_t current, std::uint64_t baseline)
{
    return current >= baseline ? current - baseline : 0;
}

void CaptureCounterBaseline(WorkerState::GatherReceipt& receipt, PlayerbotAI* botAI)
{
    if (botAI)
    {
        receipt.lootResponseAtAttempt = botAI->GetAutoWowLootResponseCount();
        receipt.storeLootExecutionAtAttempt = botAI->GetAutoWowStoreLootExecutionCount();
        receipt.autostoreLootPacketAtAttempt = botAI->GetAutoWowAutostoreLootPacketCount();
        receipt.lootReleasePacketAtAttempt = botAI->GetAutoWowLootReleasePacketCount();
        receipt.lootPacketItemAtAttempt = botAI->GetAutoWowLootPacketItemCount();
        receipt.lootAllowedOwnerSlotAtAttempt = botAI->GetAutoWowLootAllowedOwnerSlotCount();
        receipt.lootSlotTypeRejectedAtAttempt = botAI->GetAutoWowLootSlotTypeRejectedCount();
        receipt.lootPolicyRejectedAtAttempt = botAI->GetAutoWowLootPolicyRejectedCount();
        receipt.lootMissingTemplateAtAttempt = botAI->GetAutoWowLootMissingTemplateCount();
        receipt.lootBagReserveRejectedAtAttempt = botAI->GetAutoWowLootBagReserveRejectedCount();
        receipt.countersInitialized = true;
    }
}

void AddInventoryItemCount(std::unordered_map<std::uint32_t, std::uint32_t>& counts, Item* item)
{
    if (!item || !item->GetEntry())
        return;

    counts[item->GetEntry()] += item->GetCount();
}

void CaptureInventoryBaseline(WorkerState::GatherReceipt& receipt, Player* bot)
{
    if (!bot)
        return;

    // Capture carried state only. Bank/buyback/keyring/currency slots are outside the normal
    // gathering material storage path and would make an unrelated bank change look like loot.
    for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddInventoryItemCount(receipt.itemCountsBefore, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (std::uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (std::uint32_t slot = 0; slot < bag->GetBagSize(); ++slot)
            AddInventoryItemCount(receipt.itemCountsBefore, bag->GetItemByPos(static_cast<std::uint8_t>(slot)));
    }
}

std::uint8_t BagSpacePercent(Player* bot)
{
    if (!bot)
        return 0;

    std::uint32_t used = 0;
    std::uint32_t total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;

    for (std::uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        ItemTemplate const* bagTemplate = bag->GetTemplate();
        if (!bagTemplate || bagTemplate->Class != ITEM_CLASS_CONTAINER ||
            bagTemplate->SubClass != ITEM_SUBCLASS_CONTAINER)
            continue;

        total += bag->GetBagSize();
        used += bag->GetBagSize() - bag->GetFreeSlots();
    }

    return total ? static_cast<std::uint8_t>(std::min<std::uint32_t>(100, used * 100 / total)) : 0;
}

void AddSourceItem(WorkerState::GatherReceipt& receipt, std::uint32_t itemEntry)
{
    if (!itemEntry || std::find(receipt.sourceItemEntries.begin(), receipt.sourceItemEntries.end(), itemEntry) !=
                           receipt.sourceItemEntries.end())
        return;

    receipt.sourceItemEntries.push_back(itemEntry);
    auto const beforeIterator = receipt.itemCountsBefore.find(itemEntry);
    std::uint32_t const before = beforeIterator == receipt.itemCountsBefore.end() ? 0 : beforeIterator->second;
    receipt.itemBaselines.push_back({itemEntry, before, before});

    // itemEntry is the exact generated item once live loot is available. Before that, this is
    // the first source-table candidate and is replaced by the exact delta if one is observed.
    if (!receipt.itemEntry)
    {
        receipt.itemEntry = itemEntry;
        receipt.itemBefore = before;
    }
}

void CaptureLootTemplateItems(WorkerState::GatherReceipt& receipt, LootTemplateAccess const* lootTemplate,
                              std::uint8_t depth = 0)
{
    if (!lootTemplate || depth > 4)
        return;

    for (LootStoreItem* item : lootTemplate->Entries)
    {
        if (!item)
            continue;

        if (item->reference != 0)
        {
            std::uint32_t const referenceId = static_cast<std::uint32_t>(
                item->reference < 0 ? -item->reference : item->reference);
            LootTemplate const* reference = LootTemplates_Reference.GetLootFor(referenceId);
            CaptureLootTemplateItems(receipt, reinterpret_cast<LootTemplateAccess const*>(reference), depth + 1);
        }
        else if (item->itemid && sObjectMgr->GetItemTemplate(item->itemid))
        {
            AddSourceItem(receipt, item->itemid);
        }
    }
}

void CaptureSourceItems(WorkerState::GatherReceipt& receipt, GameObject* gameObject)
{
    if (!gameObject)
        return;

    for (LootItem const& item : gameObject->loot.items)
        AddSourceItem(receipt, item.itemid);
    for (LootItem const& item : gameObject->loot.quest_items)
        AddSourceItem(receipt, item.itemid);

    // A queued loot response may have already cleared the live Loot vectors by the time the
    // worker observes it. Keep the generic source-table candidate set captured at attempt time so
    // a later carried-item delta can still be matched without inventing a material.
    if (receipt.sourceItemEntries.empty())
    {
        LootTemplateAccess const* lootTemplate = DropMapValue::GetLootTemplate(
            ObjectGuid::Create<HighGuid::GameObject>(gameObject->GetEntry(), std::uint32_t(1)), LOOT_CORPSE);
        CaptureLootTemplateItems(receipt, lootTemplate);
    }
}

void UpdateSourceMaterialProof(WorkerState::GatherReceipt& receipt)
{
    if (receipt.requestedMaterialItemId != 0)
    {
        receipt.sourceYieldsRequestedMaterial =
            std::find(receipt.sourceItemEntries.begin(), receipt.sourceItemEntries.end(),
                      receipt.requestedMaterialItemId) != receipt.sourceItemEntries.end();
        return;
    }

    // Ordinary workers do not publish a material id. Preserve their generic behavior, but still
    // require an actual generated/template item before a receipt can be credited.
    receipt.sourceYieldsRequestedMaterial = !receipt.sourceItemEntries.empty();
}

void UpdateItemDeltas(WorkerState::GatherReceipt& receipt, Player* bot)
{
    // An inventory increase is only attributable after this exact source has generated loot.
    // The source-table candidates captured before the cast are deliberately not enough on their
    // own: another action could add the same material while this receipt is pending.
    if (!bot || !receipt.sourceMatched || !receipt.sourceLootGenerationObserved)
        return;

    for (WorkerState::GatherReceipt::ItemBaseline& item : receipt.itemBaselines)
    {
        item.after = bot->GetItemCount(item.entry, false);
        if (item.after <= item.before)
            continue;

        if (receipt.requestedMaterialItemId != 0 && item.entry != receipt.requestedMaterialItemId)
            continue;

        receipt.matchingMaterialDelta = true;
        std::uint32_t const itemDelta = item.after - item.before;
        std::uint32_t const selectedDelta = receipt.itemAfter >= receipt.itemBefore
                                                 ? receipt.itemAfter - receipt.itemBefore
                                                 : 0;
        if (!receipt.itemAfter || itemDelta > selectedDelta)
        {
            receipt.itemEntry = item.entry;
            receipt.itemBefore = item.before;
            receipt.itemAfter = item.after;
        }
    }
}

bool IsCandidateSourceMatch(WorkerState const& state, PlayerbotAI* botAI);

void CaptureReceiptBeforeState(WorkerState& state, Player* bot, PlayerbotAI* botAI)
{
    if (!state.candidate)
        return;

    WorkerState::GatherReceipt& receipt = state.gatherReceipt;
    receipt.candidateSpawnId = state.candidate->spawnId;
    receipt.candidateEntry = state.candidate->entry;
    receipt.profession = state.candidate->profession;
    receipt.skillType = SkillForProfession(receipt.profession);
    receipt.sourceEntry = state.candidate->entry;
    receipt.sourceSpawnId = state.candidate->spawnId;
    receipt.requestedMaterialItemId = state.oracleSource.materialItemId;
    receipt.goal = state.oracleSource.goal;

    CaptureInventoryBaseline(receipt, bot);
    receipt.bagSpacePercent = BagSpacePercent(bot);
    CaptureCounterBaseline(receipt, botAI);

    SkillType const skill = receipt.skillType;
    receipt.skillBefore = bot && skill != SKILL_NONE ? bot->GetSkillValue(skill) : 0;
    receipt.skillAfter = receipt.skillBefore;

    if (botAI && receipt.sourceGuid)
    {
        if (GameObject* gameObject = botAI->GetGameObject(receipt.sourceGuid))
        {
            receipt.sourceEntry = gameObject->GetEntry();
            receipt.sourceSpawnId = gameObject->GetSpawnId();
            receipt.sourceLootGenerationAtAttempt = gameObject->GetLootGenerationTime();
            CaptureSourceItems(receipt, gameObject);
            UpdateSourceMaterialProof(receipt);
        }
    }

    UpdateSourceMaterialProof(receipt);
    receipt.sourceMatched = IsCandidateSourceMatch(state, botAI);
}

void PrepareReceiptBeforeAttempt(WorkerState& state, Player* bot, PlayerbotAI* botAI)
{
    if (!state.candidate || state.gatherReceipt.awaitingCredit)
        return;

    state.gatherReceipt = WorkerState::GatherReceipt{};
    state.gatherReceipt.prepared = true;
    state.gatherReceipt.preparedAtSeconds = NowSeconds();
    state.gatherReceipt.sourceGuid = state.candidateRuntimeGuid;
    CaptureReceiptBeforeState(state, bot, botAI);
}

void UpdateReceiptDeltas(WorkerState::GatherReceipt& receipt, Player* bot, PlayerbotAI* botAI,
                         Profession profession)
{
    if (botAI && !receipt.countersInitialized)
        CaptureCounterBaseline(receipt, botAI);

    if (botAI && receipt.countersInitialized)
    {
        receipt.lootResponseDelta =
            CounterDelta(botAI->GetAutoWowLootResponseCount(), receipt.lootResponseAtAttempt);
        receipt.storeLootExecutionDelta =
            CounterDelta(botAI->GetAutoWowStoreLootExecutionCount(), receipt.storeLootExecutionAtAttempt);
        receipt.autostoreLootPacketDelta =
            CounterDelta(botAI->GetAutoWowAutostoreLootPacketCount(), receipt.autostoreLootPacketAtAttempt);
        receipt.lootReleasePacketDelta =
            CounterDelta(botAI->GetAutoWowLootReleasePacketCount(), receipt.lootReleasePacketAtAttempt);
        receipt.lootPacketItemDelta =
            CounterDelta(botAI->GetAutoWowLootPacketItemCount(), receipt.lootPacketItemAtAttempt);
        receipt.lootAllowedOwnerSlotDelta =
            CounterDelta(botAI->GetAutoWowLootAllowedOwnerSlotCount(), receipt.lootAllowedOwnerSlotAtAttempt);
        receipt.lootSlotTypeRejectedDelta =
            CounterDelta(botAI->GetAutoWowLootSlotTypeRejectedCount(), receipt.lootSlotTypeRejectedAtAttempt);
        receipt.lootPolicyRejectedDelta =
            CounterDelta(botAI->GetAutoWowLootPolicyRejectedCount(), receipt.lootPolicyRejectedAtAttempt);
        receipt.lootMissingTemplateDelta =
            CounterDelta(botAI->GetAutoWowLootMissingTemplateCount(), receipt.lootMissingTemplateAtAttempt);
        receipt.lootBagReserveRejectedDelta =
            CounterDelta(botAI->GetAutoWowLootBagReserveRejectedCount(), receipt.lootBagReserveRejectedAtAttempt);
    }

    SkillType const skill = receipt.skillType != SKILL_NONE ? receipt.skillType : SkillForProfession(profession);
    receipt.skillAfter = bot && skill != SKILL_NONE ? bot->GetSkillValue(skill) : receipt.skillBefore;
    receipt.matchingSkillDelta = receipt.sourceMatched && receipt.sourceLootGenerationObserved &&
                                 receipt.skillAfter > receipt.skillBefore;
    receipt.bagSpacePercent = BagSpacePercent(bot);
    UpdateItemDeltas(receipt, bot);
}

void ResolveReceiptSource(WorkerState& state, Player* bot)
{
    if (!state.candidate || !bot || !bot->GetMap())
        return;

    if (state.oracleExactSourceOnly &&
        (!AutoWowOracle::ValidGatherSourceReference(state.oracleSource) ||
         bot->GetMapId() != state.oracleSource.mapId ||
         bot->GetInstanceId() != state.oracleSource.instanceId))
        return;

    WorkerState::GatherReceipt& receipt = state.gatherReceipt;
    if (!receipt.sourceGuid && state.candidateRuntimeGuid)
    {
        receipt.sourceGuid = state.candidateRuntimeGuid;
    }

    if (!receipt.sourceGuid)
    {
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(state.candidate->spawnId);
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
        {
            GameObject* gameObject = iterator->second;
            if (gameObject && gameObject->GetEntry() == state.candidate->entry &&
                (!state.oracleExactSourceOnly || gameObject->GetInstanceId() == state.oracleSource.instanceId))
            {
                receipt.sourceGuid = gameObject->GetGUID();
                break;
            }
        }
    }

    if (!receipt.sourceGuid)
        return;

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        if (GameObject* gameObject = botAI->GetGameObject(receipt.sourceGuid))
        {
            receipt.sourceEntry = gameObject->GetEntry();
            receipt.sourceSpawnId = gameObject->GetSpawnId();
        }
    }
}

bool IsCandidateSourceMatch(WorkerState const& state, PlayerbotAI* botAI)
{
    WorkerState::GatherReceipt const& receipt = state.gatherReceipt;
    if (!botAI || !state.candidate || !receipt.sourceGuid || receipt.candidateSpawnId != state.candidate->spawnId ||
        receipt.candidateEntry != state.candidate->entry)
        return false;

    if (state.oracleExactSourceOnly &&
        (!AutoWowOracle::ValidGatherSourceReference(state.oracleSource) ||
         receipt.requestedMaterialItemId != state.oracleSource.materialItemId ||
         state.candidate->spawnId != state.oracleSource.spawnId ||
         state.candidate->entry != state.oracleSource.entry ||
         state.candidate->mapId != state.oracleSource.mapId))
        return false;

    if (receipt.sourceGuid.IsGameObject())
    {
        GameObject* gameObject = botAI->GetGameObject(receipt.sourceGuid);
        return gameObject && gameObject->GetGUID() == receipt.sourceGuid &&
               gameObject->GetEntry() == receipt.candidateEntry && gameObject->GetSpawnId() == receipt.candidateSpawnId &&
               gameObject->GetMapId() == state.candidate->mapId &&
               (!state.oracleExactSourceOnly || gameObject->GetInstanceId() == state.oracleSource.instanceId);
    }

    if (receipt.sourceGuid.IsCreature())
    {
        Creature* creature = botAI->GetCreature(receipt.sourceGuid);
        return creature && creature->GetGUID() == receipt.sourceGuid;
    }

    return false;
}

void UpdateSourceEvidence(WorkerState& state, Player* bot, PlayerbotAI* botAI)
{
    WorkerState::GatherReceipt& receipt = state.gatherReceipt;
    if (!receipt.sourceGuid || !receipt.sourceMatched || !botAI)
        return;

    receipt.sourcePresentAtLastObservation = false;

    // GetLootGUID is the exact source identity while the normal loot session is open. It is
    // deliberately latched because the canonical release path clears it before the worker's next
    // multi-second poll.
    if (bot && bot->GetLootGUID() == receipt.sourceGuid)
        receipt.sourceLootGenerationObserved = true;

    if (receipt.sourceGuid.IsGameObject())
    {
        GameObject* gameObject = botAI->GetGameObject(receipt.sourceGuid);
        if (!gameObject || gameObject->GetEntry() != receipt.candidateEntry ||
            gameObject->GetSpawnId() != receipt.candidateSpawnId ||
            gameObject->GetMapId() != state.candidate->mapId ||
            (state.oracleExactSourceOnly && gameObject->GetInstanceId() != state.oracleSource.instanceId))
            return;

        receipt.sourcePresentAtLastObservation = true;
        receipt.sourceUnlootedCount = gameObject->loot.unlootedCount;
        receipt.sourceLootItemCount = static_cast<std::uint32_t>(gameObject->loot.items.size() +
                                                                 gameObject->loot.quest_items.size());

        if (gameObject->GetLootGenerationTime() != receipt.sourceLootGenerationAtAttempt ||
            gameObject->getLootState() == GO_ACTIVATED)
        {
            receipt.sourceLootGenerationObserved = true;
            CaptureSourceItems(receipt, gameObject);
            UpdateSourceMaterialProof(receipt);
        }
        receipt.sourceLootConsumedObserved = LatchSourceLootConsumption(
            receipt.sourceLootConsumedObserved, receipt.sourceMatched, receipt.sourceLootGenerationObserved,
            receipt.sourcePresentAtLastObservation,
            receipt.countersInitialized &&
                CounterDelta(botAI->GetAutoWowLootResponseCount(), receipt.lootResponseAtAttempt) > 0,
            gameObject->loot.isLooted());
        return;
    }

    if (receipt.sourceGuid.IsCreature())
    {
        Creature* creature = botAI->GetCreature(receipt.sourceGuid);
        if (!creature)
            return;

        receipt.sourcePresentAtLastObservation = true;
        receipt.sourceUnlootedCount = creature->loot.unlootedCount;
        receipt.sourceLootItemCount = static_cast<std::uint32_t>(creature->loot.items.size() +
                                                                 creature->loot.quest_items.size());
        if (bot && bot->GetLootGUID() == receipt.sourceGuid)
            receipt.sourceLootGenerationObserved = true;
        bool const consumed = creature->loot.isLooted() &&
                              !creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) &&
                              !creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        receipt.sourceLootConsumedObserved = LatchSourceLootConsumption(
            receipt.sourceLootConsumedObserved, receipt.sourceMatched, receipt.sourceLootGenerationObserved,
            receipt.sourcePresentAtLastObservation,
            receipt.countersInitialized &&
                CounterDelta(botAI->GetAutoWowLootResponseCount(), receipt.lootResponseAtAttempt) > 0,
            consumed);
    }
}

bool IsSourceLootConsumed(PlayerbotAI* botAI, WorkerState::GatherReceipt const& receipt)
{
    if (!botAI || !receipt.sourceGuid || !receipt.sourceMatched || !receipt.sourceLootGenerationObserved)
        return false;

    if (receipt.sourceLootConsumedObserved)
        return true;

    if (receipt.sourceGuid.IsGameObject())
    {
        GameObject* gameObject = botAI->GetGameObject(receipt.sourceGuid);
        // A consumed gathering node is commonly GO_READY with cleared Loot vectors by the time
        // this observer runs. Generation identity plus Loot::isLooted is the durable source test;
        // The transient deactivation state is only an intermediate state and must not be required.
        return gameObject && gameObject->GetGUID() == receipt.sourceGuid &&
               gameObject->GetEntry() == receipt.candidateEntry && gameObject->GetSpawnId() == receipt.candidateSpawnId &&
               gameObject->loot.isLooted();
    }

    if (receipt.sourceGuid.IsCreature())
    {
        Creature* creature = botAI->GetCreature(receipt.sourceGuid);
        return creature && creature->loot.isLooted() &&
               !creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) &&
               !creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
    }

    return false;
}

GatherNoDurableCreditObservation NoDurableCreditObservation(WorkerState::GatherReceipt const& receipt)
{
    GatherNoDurableCreditObservation observation;
    observation.sourceMatched = receipt.sourceMatched;
    observation.sourceLootGenerationObserved = receipt.sourceLootGenerationObserved;
    observation.sourceYieldsRequestedMaterial = receipt.sourceYieldsRequestedMaterial;
    observation.sourcePresent = receipt.sourcePresentAtLastObservation;
    observation.sourceLootConsumedObserved = receipt.sourceLootConsumedObserved;
    observation.sourceUnlootedCount = receipt.sourceUnlootedCount;
    observation.bagSpacePercent = receipt.bagSpacePercent;
    observation.lootResponseDelta = receipt.lootResponseDelta;
    observation.storeLootExecutionDelta = receipt.storeLootExecutionDelta;
    observation.autostoreLootPacketDelta = receipt.autostoreLootPacketDelta;
    return observation;
}

void FinalizeCandidateCredit(WorkerState& state, GatherCreditDecision decision)
{
    if (!state.candidate)
        return;

    std::uint64_t const spawnId = state.candidate->spawnId;
    std::uint32_t const cooldownSeconds =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.GatherSeek.MissCooldownSeconds", 300);
    state.cooldowns[spawnId] = CooldownDeadline(NowSeconds(), cooldownSeconds);
    state.gatherReceipt.awaitingCredit = false;

    bool const materialRequired = state.gatherReceipt.goal == AutoWowOracle::GatherGoal::ObtainMaterial;
    bool const success = materialRequired
        ? IsConfirmedMaterialCredit(decision)
        : IsConfirmedHarvestNodeCredit(decision);
    state.gatherReceipt.evidence = GatherCreditDecisionName(decision);

    if (success)
    {
        ClearCandidateTracking(state);
        Record(state, "cooldown", "candidate_completed", "gather_succeeded");
        return;
    }

    Increment(state.missCount);
    std::string failureReason = GatherCreditDecisionName(decision);
    if (decision == GatherCreditDecision::FailedNoDurableCredit)
    {
        GatherNoDurableCreditDetail const detail = DiagnoseNoDurableCredit(
            NoDurableCreditObservation(state.gatherReceipt));
        state.gatherReceipt.failureDetail = GatherNoDurableCreditDetailName(detail);
        failureReason += ":" + state.gatherReceipt.failureDetail;
    }
    ClearCandidateTracking(state);
    Record(state, "cooldown", "candidate_credit_failed", std::move(failureReason));
}

void CooldownCandidate(WorkerState& state, std::uint32_t cooldownSeconds, std::string reason,
                       CandidateLeaseEvent leaseEvent, CandidateFailureKind failureKind)
{
    if (state.candidate)
        state.cooldowns[state.candidate->spawnId] = CooldownDeadline(NowSeconds(), cooldownSeconds);

    state.gatherReceipt.awaitingCredit = false;
    ClearCandidateTracking(state);
    Increment(state.missCount);

    std::string event = "candidate_miss";
    if (leaseEvent == CandidateLeaseEvent::ExpiredNoProgress)
    {
        Increment(state.leaseNoProgressExpirationCount);
        event = "candidate_lease_expired";
    }
    else if (leaseEvent == CandidateLeaseEvent::ExpiredMaxLease)
    {
        Increment(state.leaseMaxAgeExpirationCount);
        event = "candidate_lease_expired";
    }
    else if (failureKind == CandidateFailureKind::UnsafeLiquid)
    {
        Increment(state.liquidRejectionCount);
        event = "candidate_liquid_rejected";
    }

    Record(state, "cooldown", std::move(event), std::move(reason));
}
}

bool HasRequiredTool(Player* bot, Candidate const& candidate)
{
    if (!bot)
        return false;
    return candidate.profession != Profession::Mining || HasMiningTool(bot);
}

void ActivateWorker(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    WorkerState& state = workerStates[botGuid];
    state = WorkerState{};
    state.explicitWorker = true;
    Record(state, "seeking", "worker_activated", "awaiting_candidate");
}

void DeactivateWorker(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    workerStates.erase(botGuid);
}

bool IsExplicitWorker(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state != workerStates.end() && state->second.explicitWorker;
}

DeathRecoveryTransition PlanDeathRecovery(std::uint32_t botGuid, bool alive, bool ghost,
                                          bool corpseAvailable, bool corpseNear,
                                          bool routeInProgress)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker)
        return {};

    WorkerState& worker = state->second;
    DeathRecoveryObservation const observation = {
        true, alive, ghost, corpseAvailable, corpseNear, routeInProgress, NowSeconds()};
    bool const wasActive = worker.deathRecovery.active;
    DeathRecoveryTransition const transition =
        EvaluateDeathRecovery(worker.deathRecovery, observation);

    // A death invalidates any live node/loot receipt. Keep exact Oracle mode fail-closed, but
    // require its owner to publish a fresh source after the worker returns alive.
    if (!wasActive && transition.next.active)
    {
        ClearCandidateTracking(worker);
        worker.gatherReceipt = WorkerState::GatherReceipt{};
    }

    worker.deathRecovery = transition.next;
    PublishDeathRecoveryStatus(worker);
    return transition;
}

void ObserveWorkerAlive(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker ||
        !state->second.deathRecovery.active)
        return;

    WorkerState& worker = state->second;
    DeathRecoveryObservation const observation = {
        true, true, false, false, false, false, NowSeconds()};
    DeathRecoveryTransition const transition =
        EvaluateDeathRecovery(worker.deathRecovery, observation);
    ClearCandidateTracking(worker);
    worker.gatherReceipt = WorkerState::GatherReceipt{};
    worker.deathRecovery = transition.next;
    PublishDeathRecoveryStatus(worker);
}

bool IsOracleExactSourceOnly(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state != workerStates.end() && state->second.explicitWorker && state->second.oracleExactSourceOnly;
}

bool PublishOracleSource(Player* bot, PlayerbotAI* botAI,
                         AutoWowOracle::GatherSourceReference const& reference)
{
    if (!bot || !botAI)
        return false;

    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    std::optional<Candidate> const candidate = ResolveExactCandidate(reference);
    if (!candidate || !AdoptExactCandidate(botGuid, *candidate, reference))
        return false;

    // Adoption establishes exact-only suppression first. Publication does not report success
    // until all inherited movement surfaces are invalidated, regardless of runtime cadence.
    AutoWowGatherRuntimePolicy::ExactSourcePublicationDecision const publication =
        AutoWowGatherRuntimePolicy::EvaluateExactSourcePublication(
            {bot->isMoving(), true, GetOracleMotionProvenance(botGuid).valid});
    if (publication.cancelActiveMovement)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    if (publication.invalidateLastMovement)
        botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
    if (publication.invalidateExactMotionProvenance)
        ClearOracleMotionProvenance(botGuid);
    return true;
}

bool SourceYieldsMaterial(GameObject* source, std::uint32_t materialItemId)
{
    if (!source || materialItemId == 0)
        return false;

    WorkerState::GatherReceipt receipt;
    CaptureSourceItems(receipt, source);
    return std::find(receipt.sourceItemEntries.begin(), receipt.sourceItemEntries.end(), materialItemId) !=
        receipt.sourceItemEntries.end();
}

std::optional<Candidate> CurrentCandidate(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state == workerStates.end() ? std::optional<Candidate>{} : state->second.candidate;
}

std::optional<Candidate> SelectCandidateFor(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI)
        return std::nullopt;

    std::call_once(candidateCacheFlag, BuildCandidateCache);

    Profile profile;
    profile.mapId = bot->GetMapId();
    profile.phaseMask = bot->GetPhaseMask();
    profile.x = bot->GetPositionX();
    profile.y = bot->GetPositionY();
    profile.z = bot->GetPositionZ();
    profile.level = bot->GetLevel();
    profile.herbalismSkill = botAI->HasSkill(SKILL_HERBALISM) ? bot->GetSkillValue(SKILL_HERBALISM) : 0;
    profile.miningSkill = botAI->HasSkill(SKILL_MINING) ? bot->GetSkillValue(SKILL_MINING) : 0;
    profile.skinningSkill = botAI->HasSkill(SKILL_SKINNING) ? bot->GetSkillValue(SKILL_SKINNING) : 0;
    profile.hasMiningTool = HasMiningTool(bot);
    profile.anySkill = sPlayerbotAIConfig.autoWowGatherAnySkill;
    profile.maxDistance = sConfigMgr->GetOption<float>("AutoWow.GatherSeek.SearchDistance", 1200.0f);
    profile.maxNodeLevelAboveBot =
        sConfigMgr->GetOption<std::uint32_t>("AutoWow.GatherSeek.MaxNodeLevelAboveBot", 5);

    std::lock_guard<std::mutex> lock(stateMutex);
    WorkerState& state = workerStates[bot->GetGUID().GetCounter()];
    if (!state.explicitWorker)
        return std::nullopt;
    if (state.oracleExactSourceOnly)
    {
        Record(state, "blocked", "ordinary_selector_suppressed", "external_exact_source_required");
        return std::nullopt;
    }

    std::uint64_t const now = NowSeconds();
    for (auto cooldown = state.cooldowns.begin(); cooldown != state.cooldowns.end();)
    {
        if (cooldown->second <= now)
            cooldown = state.cooldowns.erase(cooldown);
        else
            ++cooldown;
    }

    std::optional<std::size_t> const selected =
        AutoWowGather::SelectCandidate(candidateCache, profile, state.cooldowns, now);
    if (!selected)
    {
        std::string reason = "no_eligible_candidate";
        if (!profile.herbalismSkill && !profile.miningSkill)
            reason = "no_gathering_profession";
        else if (!profile.herbalismSkill && profile.miningSkill && !profile.hasMiningTool)
            reason = "missing_mining_tool";
        Record(state, "idle", "candidate_selection_empty", reason);
        ClearCandidateTracking(state);
        return std::nullopt;
    }

    Candidate const& selectedCandidate = candidateCache[*selected];
    if (state.candidate && state.candidate->spawnId == selectedCandidate.spawnId)
        return state.candidate;

    state.gatherReceipt = WorkerState::GatherReceipt{};
    state.candidateRuntimeGuid.Clear();
    state.candidate = selectedCandidate;
    CandidateLeaseSample leaseSample;
    leaseSample.candidateId = selectedCandidate.spawnId;
    leaseSample.nowSeconds = now;
    leaseSample.position = {profile.x, profile.y, profile.z};
    leaseSample.distanceToCandidate = std::sqrt(DistanceSquared(profile, selectedCandidate));
    state.candidateLease =
        ObserveCandidateLease(CandidateLease{}, leaseSample, DefaultCandidateLeasePolicy).lease;
    state.candidateSafetyAccepted = false;
    Increment(state.candidateAcquisitionCount);
    Record(state, "selected", "candidate_selected", {});
    return state.candidate;
}

std::optional<Candidate> ResolveExactCandidate(
    AutoWowOracle::GatherSourceReference const& reference)
{
    // materialItemId is part of the fixed source identity even though the legacy worker cache
    // stores source facts rather than a planner material. Do not accept an under-specified Oracle
    // reference and do not broaden this into a cache selector.
    if (!AutoWowOracle::ValidGatherSourceReference(reference) || reference.materialItemId == 0)
        return std::nullopt;

    std::call_once(candidateCacheFlag, BuildCandidateCache);

    std::optional<Candidate> exact;
    for (Candidate const& candidate : candidateCache)
    {
        if (candidate.spawnId != reference.spawnId || candidate.entry != reference.entry ||
            candidate.mapId != reference.mapId || candidate.profession == Profession::None ||
            candidate.requiredSkill == 0)
            continue;

        // A duplicate full cache identity is ambiguous. Fail closed rather than silently handing
        // the Oracle a different cache record.
        if (exact)
            return std::nullopt;
        exact = candidate;
    }
    return exact;
}

bool AdoptExactCandidate(std::uint32_t botGuid, Candidate const& candidate,
                         AutoWowOracle::GatherSourceReference const& reference)
{
    if (!botGuid || !candidate.spawnId || !candidate.entry ||
        !AutoWowOracle::ValidGatherSourceReference(reference) || reference.materialItemId == 0 ||
        reference.spawnId != candidate.spawnId || reference.entry != candidate.entry ||
        reference.mapId != candidate.mapId ||
        candidate.profession == Profession::None || !candidate.requiredSkill)
        return false;

    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker)
        return false;

    WorkerState& worker = state->second;
    bool const sameCandidate = worker.candidate &&
        worker.candidate->spawnId == candidate.spawnId && worker.candidate->entry == candidate.entry &&
        worker.candidate->mapId == candidate.mapId;
    bool const sameSource = sameCandidate && worker.oracleExactSourceOnly &&
        AutoWowOracle::SameGatherSourceReference(worker.oracleSource, reference);
    if (worker.gatherReceipt.awaitingCredit && !sameSource)
        return false;

    worker.oracleExactSourceOnly = true;
    worker.oracleSource = reference;
    if (!sameSource)
    {
        worker.oracleMotion = {};
        worker.gatherReceipt = WorkerState::GatherReceipt{};
        worker.candidateLease = CandidateLease{};
        worker.candidateSafetyAccepted = false;
        worker.candidateRuntimeGuid.Clear();
        worker.candidate = candidate;
        worker.candidateSafetyAccepted = true;
        Increment(worker.candidateAcquisitionCount);
        Record(worker, "selected", "oracle_fixed_candidate_adopted", {});
    }
    else
    {
        worker.candidate = candidate;
        worker.candidateSafetyAccepted = true;
    }
    return true;
}

AutoWowGatherRuntimePolicy::ExactMotionProvenance GetOracleMotionProvenance(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state == workerStates.end() ? AutoWowGatherRuntimePolicy::ExactMotionProvenance{}
                                      : state->second.oracleMotion;
}

bool SetOracleMotionProvenance(std::uint32_t botGuid, AutoWowOracle::DecisionId decisionId,
                               AutoWowOracle::GatherSourceReference const& reference)
{
    if (!botGuid || !decisionId || !AutoWowOracle::ValidGatherSourceReference(reference))
        return false;

    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker ||
        !state->second.oracleExactSourceOnly ||
        !AutoWowOracle::SameGatherSourceReference(state->second.oracleSource, reference))
        return false;

    state->second.oracleMotion = {true, decisionId, reference};
    return true;
}

void ClearOracleMotionProvenance(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state != workerStates.end())
        state->second.oracleMotion = {};
}

bool CandidateSafetyCheckRequired(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state != workerStates.end() && state->second.explicitWorker && state->second.candidate &&
           !state->second.candidateSafetyAccepted;
}

void MarkCandidateSafetyAccepted(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state != workerStates.end() && state->second.explicitWorker && state->second.candidate)
        state->second.candidateSafetyAccepted = true;
}

void SetCandidateRuntimeGuid(std::uint32_t botGuid, std::uint64_t spawnId, ObjectGuid const& runtimeGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker || !state->second.candidate || !runtimeGuid ||
        state->second.candidate->spawnId != spawnId)
        return;

    state->second.candidateRuntimeGuid = runtimeGuid;

    // This handoff happens after the worker has resolved the exact live spawn but before the
    // canonical OpenLootAction casts the gathering spell. Capture profession, source generation,
    // possible source items, carried material counts, skill, and packet counters here so a
    // synchronous core skill update cannot be mistaken for the pre-attempt baseline.
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    PrepareReceiptBeforeAttempt(state->second, bot, botAI);
}

void SetCandidateRuntimeIdentity(std::uint32_t botGuid,
                                 AutoWowOracle::GatherSourceReference const& reference,
                                 ObjectGuid const& runtimeGuid)
{
    if (!AutoWowOracle::ValidGatherSourceReference(reference) || reference.materialItemId == 0 || !runtimeGuid)
        return;

    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker || !state->second.oracleExactSourceOnly ||
        !state->second.candidate ||
        !AutoWowOracle::SameGatherSourceReference(state->second.oracleSource, reference) ||
        state->second.candidate->spawnId != reference.spawnId ||
        state->second.candidate->entry != reference.entry ||
        state->second.candidate->mapId != reference.mapId)
        return;

    state->second.candidateRuntimeGuid = runtimeGuid;
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    PrepareReceiptBeforeAttempt(state->second, bot, botAI);
}

CandidateLeaseEvent ObserveCandidateProgress(std::uint32_t botGuid, float x, float y, float z,
                                             float distanceToCandidate, std::uint32_t cooldownSeconds)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker || !state->second.candidate)
        return CandidateLeaseEvent::None;

    CandidateLeaseSample sample;
    sample.candidateId = state->second.candidate->spawnId;
    sample.nowSeconds = NowSeconds();
    sample.position = {x, y, z};
    sample.distanceToCandidate = distanceToCandidate;

    CandidateLeaseTransition const transition =
        ObserveCandidateLease(state->second.candidateLease, sample, DefaultCandidateLeasePolicy);
    state->second.candidateLease = transition.lease;

    if (transition.event == CandidateLeaseEvent::Acquired)
        Increment(state->second.candidateAcquisitionCount);
    else if (transition.event == CandidateLeaseEvent::Progress)
        Increment(state->second.candidateProgressCount);
    else if (transition.event == CandidateLeaseEvent::ExpiredNoProgress ||
             transition.event == CandidateLeaseEvent::ExpiredMaxLease)
    {
        CooldownCandidate(state->second, cooldownSeconds, CandidateLeaseReason(transition.event), transition.event,
                          CandidateFailureKind::Generic);
    }

    return transition.event;
}

void SetRouteStatus(std::uint32_t botGuid, std::string status, std::string event, std::string idleReason)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker)
        return;
    Record(state->second, std::move(status), std::move(event), std::move(idleReason));
}

bool HasPendingCandidateCredit(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botGuid);
    return state != workerStates.end() && state->second.explicitWorker && state->second.candidate &&
           state->second.gatherReceipt.awaitingCredit;
}

bool IsPendingGatherSourceLoot(PlayerbotAI* botAI, ObjectGuid const& sourceGuid, std::uint32_t itemId)
{
    if (!botAI || !botAI->GetBot())
        return false;

    bool const sourceGuidUsable = sourceGuid && sourceGuid.IsGameObject();
    GameObject* liveSource = sourceGuidUsable ? botAI->GetGameObject(sourceGuid) : nullptr;

    bool const liveGeneratedItemMatches =
        liveSource && itemId &&
        (std::any_of(liveSource->loot.items.begin(), liveSource->loot.items.end(),
                     [itemId](LootItem const& item) { return item.itemid == itemId; }) ||
         std::any_of(liveSource->loot.quest_items.begin(), liveSource->loot.quest_items.end(),
                     [itemId](LootItem const& item) { return item.itemid == itemId; }));

    std::lock_guard<std::mutex> lock(stateMutex);
    auto const state = workerStates.find(botAI->GetBot()->GetGUID().GetCounter());
    if (state == workerStates.end())
        return false;

    WorkerState& worker = state->second;
    WorkerState::GatherReceipt const& receipt = worker.gatherReceipt;
    Candidate const* candidate = worker.candidate ? &*worker.candidate : nullptr;
    bool const requestedMaterialMatches = receipt.requestedMaterialItemId == 0 ||
        receipt.requestedMaterialItemId == itemId;

    std::uint32_t failures = GatherLootAdmissionNone;
    if (!worker.explicitWorker)
        failures |= GatherLootAdmissionNotWorker;
    if (!receipt.prepared)
        failures |= GatherLootAdmissionNotPrepared;
    if (!sourceGuidUsable || !itemId || botAI->GetBot()->GetLootGUID() != sourceGuid || !liveSource ||
        liveSource->GetGUID() != sourceGuid || receipt.sourceGuid != sourceGuid)
        failures |= GatherLootAdmissionGuidMismatch;
    if (!candidate || worker.candidateRuntimeGuid != sourceGuid)
        failures |= GatherLootAdmissionRuntimeMismatch;
    if (!candidate || receipt.candidateSpawnId != candidate->spawnId ||
        receipt.sourceSpawnId != candidate->spawnId || !liveSource || liveSource->GetSpawnId() != candidate->spawnId)
        failures |= GatherLootAdmissionSpawnMismatch;
    if (!candidate || receipt.candidateEntry != candidate->entry || receipt.sourceEntry != candidate->entry ||
        !liveSource || liveSource->GetEntry() != candidate->entry)
        failures |= GatherLootAdmissionEntryMismatch;
    if (!liveGeneratedItemMatches)
        failures |= GatherLootAdmissionLiveItemMismatch;
    if (!requestedMaterialMatches)
        failures |= GatherLootAdmissionRequestedMismatch;

    worker.gatherReceipt.lootAdmissionFailureMask = failures;
    return failures == GatherLootAdmissionNone;
}

std::optional<GatherCreditDecision> ObserveCandidateCredit(Player* bot, PlayerbotAI* botAI)
{
    return ObserveCandidateCredit(bot, botAI, nullptr);
}

std::optional<GatherCreditDecision> ObserveCandidateCredit(
    Player* bot, PlayerbotAI* botAI, GatherCreditObservation* observationOut)
{
    if (!bot)
        return std::nullopt;

    std::uint32_t const botGuid = bot->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker || !state->second.candidate ||
        !state->second.gatherReceipt.awaitingCredit)
        return std::nullopt;

    WorkerState& worker = state->second;
    ResolveReceiptSource(worker, bot);
    worker.gatherReceipt.sourceMatched = worker.gatherReceipt.sourceMatched || IsCandidateSourceMatch(worker, botAI);
    UpdateSourceEvidence(worker, bot, botAI);
    UpdateReceiptDeltas(worker.gatherReceipt, bot, botAI, worker.candidate->profession);

    bool const processingDelta = worker.gatherReceipt.storeLootExecutionDelta > 0 ||
                                 worker.gatherReceipt.autostoreLootPacketDelta > 0;
    if (worker.gatherReceipt.sourceMatched && worker.gatherReceipt.sourceLootGenerationObserved && processingDelta)
        worker.gatherReceipt.sourceLootProcessingObserved = true;

    GatherCreditObservation observation;
    observation.interactionAttempted = true;
    observation.lootResponseObserved = worker.gatherReceipt.lootResponseDelta > 0;
    observation.lootProcessingObserved = worker.gatherReceipt.sourceLootProcessingObserved;
    observation.sourceMatched = worker.gatherReceipt.sourceMatched;
    observation.sourceLootGenerationObserved = worker.gatherReceipt.sourceLootGenerationObserved;
    observation.sourceYieldsRequestedMaterial = worker.gatherReceipt.sourceYieldsRequestedMaterial;
    observation.materialFulfillmentRequired =
        worker.gatherReceipt.goal == AutoWowOracle::GatherGoal::ObtainMaterial;
    observation.sourceLootConsumed = IsSourceLootConsumed(botAI, worker.gatherReceipt);
    observation.matchingMaterialDelta = worker.gatherReceipt.matchingMaterialDelta;
    observation.matchingSkillDelta = worker.gatherReceipt.matchingSkillDelta;
    observation.skillIncreased = worker.gatherReceipt.skillAfter > worker.gatherReceipt.skillBefore;
    std::uint64_t const now = NowSeconds();
    observation.elapsedSeconds = now >= worker.gatherReceipt.attemptedAtSeconds
                                     ? now - worker.gatherReceipt.attemptedAtSeconds
                                     : 0;

    if (observationOut)
        *observationOut = observation;

    GatherCreditDecision const decision = EvaluateGatherCredit(observation);
    if (decision == GatherCreditDecision::AwaitingCredit)
    {
        if (observation.lootResponseObserved && !worker.gatherReceipt.lootOpened)
        {
            worker.gatherReceipt.lootOpened = true;
            worker.gatherReceipt.evidence = "loot_opened";
            Record(worker, "awaiting_credit", "gather_loot_opened", "awaiting_durable_credit");
        }
        return decision;
    }

    FinalizeCandidateCredit(worker, decision);
    return decision;
}

bool MarkCandidateGathered(std::uint32_t botGuid, std::uint64_t spawnId)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker || !state->second.candidate ||
        state->second.candidate->spawnId != spawnId)
        return false;

    // This legacy call site is reached when CastSpell accepted the gathering interaction. It is
    // deliberately an attempt receipt only; completion is emitted later by ObserveCandidateCredit
    // after canonical skill or post-loot source evidence is observed.
    if (state->second.gatherReceipt.awaitingCredit)
        return true;

    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    WorkerState& worker = state->second;

    bool const preparedForCandidate = worker.gatherReceipt.prepared &&
                                      worker.gatherReceipt.candidateSpawnId == worker.candidate->spawnId &&
                                      worker.gatherReceipt.candidateEntry == worker.candidate->entry &&
                                      worker.gatherReceipt.sourceGuid == worker.candidateRuntimeGuid;
    if (!preparedForCandidate)
    {
        worker.gatherReceipt = WorkerState::GatherReceipt{};
        worker.gatherReceipt.sourceGuid = worker.candidateRuntimeGuid;
        ResolveReceiptSource(worker, bot);
        CaptureReceiptBeforeState(worker, bot, botAI);
    }

    worker.gatherReceipt.awaitingCredit = true;
    worker.gatherReceipt.evidence = "interaction_attempted";
    worker.gatherReceipt.attemptedAtSeconds = NowSeconds();
    worker.gatherReceipt.sourceMatched = worker.gatherReceipt.sourceMatched || IsCandidateSourceMatch(worker, botAI);

    // CastSpell may have synchronously generated the source loot before this legacy attempt hook
    // returns. Capture those exact generated item ids without changing inventory or skill state.
    if (botAI && worker.gatherReceipt.sourceGuid.IsGameObject())
        if (GameObject* gameObject = botAI->GetGameObject(worker.gatherReceipt.sourceGuid))
        {
            CaptureSourceItems(worker.gatherReceipt, gameObject);
            UpdateSourceMaterialProof(worker.gatherReceipt);
        }

    UpdateSourceEvidence(worker, bot, botAI);
    Record(state->second, "awaiting_credit", "gather_interaction_attempted", "awaiting_gather_credit");
    return true;
}

void MarkCandidateMiss(std::uint32_t botGuid, std::uint32_t cooldownSeconds, std::string reason,
                       CandidateFailureKind failureKind)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto state = workerStates.find(botGuid);
    if (state == workerStates.end() || !state->second.explicitWorker)
        return;

    CooldownCandidate(state->second, cooldownSeconds, std::move(reason), CandidateLeaseEvent::None, failureKind);
}

RouteSnapshot GetRouteSnapshot(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    RouteSnapshot snapshot;
    auto const state = workerStates.find(botGuid);
    if (state == workerStates.end())
        return snapshot;

    snapshot.explicitWorker = state->second.explicitWorker;
    snapshot.hasCandidate = state->second.candidate.has_value();
    if (state->second.candidate)
        snapshot.candidate = *state->second.candidate;
    snapshot.status = state->second.status;
    snapshot.event = state->second.event;
    snapshot.idleReason = state->second.idleReason;
    snapshot.eventSequence = state->second.eventSequence;
    snapshot.missCount = state->second.missCount;
    snapshot.candidateAcquisitionCount = state->second.candidateAcquisitionCount;
    snapshot.candidateProgressCount = state->second.candidateProgressCount;
    snapshot.leaseNoProgressExpirationCount = state->second.leaseNoProgressExpirationCount;
    snapshot.leaseMaxAgeExpirationCount = state->second.leaseMaxAgeExpirationCount;
    snapshot.liquidRejectionCount = state->second.liquidRejectionCount;
    snapshot.awaitingCredit = state->second.gatherReceipt.awaitingCredit;
    snapshot.oracleExactSourceOnly = state->second.oracleExactSourceOnly;
    snapshot.oracleSource = state->second.oracleSource;
    snapshot.gatherGoal = state->second.oracleSource.goal;
    snapshot.oracleMotion = state->second.oracleMotion;
    snapshot.creditEvidence = state->second.gatherReceipt.evidence;
    snapshot.candidateSpawnId = state->second.gatherReceipt.candidateSpawnId;
    snapshot.candidateEntry = state->second.gatherReceipt.candidateEntry;
    snapshot.profession = static_cast<std::uint32_t>(state->second.gatherReceipt.profession);
    snapshot.skillType = static_cast<std::uint32_t>(state->second.gatherReceipt.skillType);
    snapshot.sourceGuid = state->second.gatherReceipt.sourceGuid.GetCounter();
    snapshot.sourceEntry = state->second.gatherReceipt.sourceEntry;
    snapshot.sourceMatched = state->second.gatherReceipt.sourceMatched;
    snapshot.sourceLootGenerationObserved = state->second.gatherReceipt.sourceLootGenerationObserved;
    snapshot.sourceYieldsRequestedMaterial = state->second.gatherReceipt.sourceYieldsRequestedMaterial;
    snapshot.sourcePresentAtLastObservation = state->second.gatherReceipt.sourcePresentAtLastObservation;
    snapshot.sourceLootConsumedObserved = state->second.gatherReceipt.sourceLootConsumedObserved;
    snapshot.sourceUnlootedCount = state->second.gatherReceipt.sourceUnlootedCount;
    snapshot.sourceLootItemCount = state->second.gatherReceipt.sourceLootItemCount;
    snapshot.bagSpacePercent = state->second.gatherReceipt.bagSpacePercent;
    snapshot.failureDetail = state->second.gatherReceipt.failureDetail;
    snapshot.matchingMaterialDelta = state->second.gatherReceipt.matchingMaterialDelta;
    snapshot.matchingSkillDelta = state->second.gatherReceipt.matchingSkillDelta;
    snapshot.itemEntry = state->second.gatherReceipt.itemEntry;
    snapshot.requestedMaterialItemId = state->second.gatherReceipt.requestedMaterialItemId;
    snapshot.itemBefore = state->second.gatherReceipt.itemBefore;
    snapshot.itemAfter = state->second.gatherReceipt.itemAfter;
    snapshot.interactionAttemptedAtSeconds = state->second.gatherReceipt.attemptedAtSeconds;
    snapshot.lootResponseDelta = state->second.gatherReceipt.lootResponseDelta;
    snapshot.storeLootExecutionDelta = state->second.gatherReceipt.storeLootExecutionDelta;
    snapshot.autostoreLootPacketDelta = state->second.gatherReceipt.autostoreLootPacketDelta;
    snapshot.lootReleasePacketDelta = state->second.gatherReceipt.lootReleasePacketDelta;
    snapshot.lootPacketItemDelta = state->second.gatherReceipt.lootPacketItemDelta;
    snapshot.lootAllowedOwnerSlotDelta = state->second.gatherReceipt.lootAllowedOwnerSlotDelta;
    snapshot.lootSlotTypeRejectedDelta = state->second.gatherReceipt.lootSlotTypeRejectedDelta;
    snapshot.lootPolicyRejectedDelta = state->second.gatherReceipt.lootPolicyRejectedDelta;
    snapshot.lootMissingTemplateDelta = state->second.gatherReceipt.lootMissingTemplateDelta;
    snapshot.lootBagReserveRejectedDelta = state->second.gatherReceipt.lootBagReserveRejectedDelta;
    snapshot.lootAdmissionFailureMask = state->second.gatherReceipt.lootAdmissionFailureMask;
    snapshot.skillBefore = state->second.gatherReceipt.skillBefore;
    snapshot.skillAfter = state->second.gatherReceipt.skillAfter;
    snapshot.deathRecovery = state->second.deathRecovery;
    return snapshot;
}
}
