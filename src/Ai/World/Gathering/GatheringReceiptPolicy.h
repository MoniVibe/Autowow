/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGRECEIPTPOLICY_H
#define PLAYERBOTS_GATHERINGRECEIPTPOLICY_H

#include <cstdint>

namespace AutoWowGather
{
// A gathering spell being accepted is only an interaction attempt. The source must subsequently
// produce canonical loot-processing or skill evidence before the worker can cool it as completed.
enum class GatherCreditDecision : std::uint8_t
{
    AwaitingInteraction = 0,
    AwaitingCredit,
    ConfirmedLootConsumed,
    ConfirmedSkillCredit,
    FailedNoLootResponse,
    FailedNoDurableCredit,
    ConfirmedMaterialCredit
};

// Raw receipt inputs used by the live observer and by controlled contract fixtures. Inventory and
// skill deltas are derived here instead of allowing a test or caller to inject a finished credit
// decision directly.
struct GatherReceiptSnapshot
{
    bool interactionAttempted = false;
    bool lootResponseObserved = false;
    bool lootProcessingObserved = false;
    bool sourceMatched = false;
    bool sourceLootGenerationObserved = false;
    bool sourceLootConsumed = false;
    bool sourceYieldsRequestedMaterial = false;
    std::uint32_t materialBefore = 0;
    std::uint32_t materialAfter = 0;
    std::uint32_t skillBefore = 0;
    std::uint32_t skillAfter = 0;
    std::uint64_t elapsedSeconds = 0;
    std::uint64_t timeoutSeconds = 30;
};

// Gathering materials must pass the canonical loot packet/store path, but ordinary loot policy
// may reject trade goods that have no player item-usage value (notably mining ore). This predicate
// is intentionally stricter than "the worker is gathering": every fact must identify the prepared
// exact source and an item present in that source's currently generated live loot. StoreLootAction
// can run synchronously before OpenLootAction marks the receipt as awaiting credit, and before the
// generated items are copied into receipt telemetry. It is used only to admit the normal autostore
// packet; it never grants an item or completes a receipt.
inline bool IsPreparedExactGatherLootAllowed(bool explicitWorker, bool receiptPrepared,
                                             bool sourceGuidMatches, bool liveGeneratedItemMatches,
                                             bool requestedMaterialMatches)
{
    return explicitWorker && receiptPrepared && sourceGuidMatches && liveGeneratedItemMatches &&
           requestedMaterialMatches;
}

struct GatherCreditObservation
{
    bool interactionAttempted = false;
    bool lootResponseObserved = false;
    bool lootProcessingObserved = false;
    bool sourceMatched = false;
    bool sourceLootGenerationObserved = false;
    bool sourceLootConsumed = false;
    // This is an independent proof from the selected source's generated loot/template. The
    // requested material id in a planner request is not evidence that this source can produce it.
    bool sourceYieldsRequestedMaterial = false;
    // When true, the request is ObtainMaterial. Source consumption/template yield and skill-only
    // evidence remain HarvestNode evidence; only a positive requested-material inventory delta
    // can complete this receipt.
    bool materialFulfillmentRequired = false;
    bool matchingMaterialDelta = false;
    // Kept separate from the old generic skillIncreased bit for receipt diagnostics. The generic
    // bit is intentionally not sufficient for credit.
    bool matchingSkillDelta = false;
    bool skillIncreased = false;
    std::uint64_t elapsedSeconds = 0;
    // The worker is polled in multi-second intervals and loot packets may be queued behind the
    // interaction. Keep the wait finite, but long enough to observe the normal loot pipeline.
    std::uint64_t timeoutSeconds = 30;
};

inline GatherCreditObservation MakeGatherCreditObservation(
    GatherReceiptSnapshot const& snapshot, bool materialFulfillmentRequired)
{
    GatherCreditObservation observation;
    observation.interactionAttempted = snapshot.interactionAttempted;
    observation.lootResponseObserved = snapshot.lootResponseObserved;
    observation.lootProcessingObserved = snapshot.lootProcessingObserved;
    observation.sourceMatched = snapshot.sourceMatched;
    observation.sourceLootGenerationObserved = snapshot.sourceLootGenerationObserved;
    observation.sourceLootConsumed = snapshot.sourceLootConsumed;
    observation.sourceYieldsRequestedMaterial = snapshot.sourceYieldsRequestedMaterial;
    observation.materialFulfillmentRequired = materialFulfillmentRequired;
    observation.matchingMaterialDelta = snapshot.materialAfter > snapshot.materialBefore;
    observation.matchingSkillDelta = snapshot.skillAfter > snapshot.skillBefore;
    observation.skillIncreased = observation.matchingSkillDelta;
    observation.elapsedSeconds = snapshot.elapsedSeconds;
    observation.timeoutSeconds = snapshot.timeoutSeconds;
    return observation;
}

// Source consumption and packet processing are observed on different asynchronous paths. Preserve
// an exact-source Loot::isLooted observation until the normal processing counters catch up; source
// disappearance by itself is never sufficient to set this latch.
inline bool LatchSourceLootConsumption(bool previouslyObserved, bool sourceMatched,
                                       bool sourceLootGenerationObserved, bool sourcePresent,
                                       bool lootResponseObserved, bool sourceLooted)
{
    return previouslyObserved ||
           (sourceMatched && sourceLootGenerationObserved && sourcePresent && lootResponseObserved && sourceLooted);
}

enum class GatherNoDurableCreditDetail : std::uint8_t
{
    SourceMismatch = 0,
    SourceGenerationUnobserved,
    SourceMaterialUnproven,
    SourceTerminalStateUnobserved,
    LootResponseNotProcessed,
    LootNotQueuedHighBagUsage,
    LootFilteredOrIneligible,
    AutostoreRejected,
    SourceConsumedWithoutProcessing,
    SourceLiveUnconsumed
};

struct GatherNoDurableCreditObservation
{
    bool sourceMatched = false;
    bool sourceLootGenerationObserved = false;
    bool sourceYieldsRequestedMaterial = false;
    bool sourcePresent = false;
    bool sourceLootConsumedObserved = false;
    std::uint32_t sourceUnlootedCount = 0;
    std::uint8_t bagSpacePercent = 0;
    std::uint64_t lootResponseDelta = 0;
    std::uint64_t storeLootExecutionDelta = 0;
    std::uint64_t autostoreLootPacketDelta = 0;
};

inline GatherNoDurableCreditDetail DiagnoseNoDurableCredit(
    GatherNoDurableCreditObservation const& observation)
{
    if (!observation.sourceMatched)
        return GatherNoDurableCreditDetail::SourceMismatch;
    if (!observation.sourceLootGenerationObserved)
        return GatherNoDurableCreditDetail::SourceGenerationUnobserved;
    if (!observation.sourceYieldsRequestedMaterial)
        return GatherNoDurableCreditDetail::SourceMaterialUnproven;
    if (observation.sourceLootConsumedObserved)
        return GatherNoDurableCreditDetail::SourceConsumedWithoutProcessing;
    if (!observation.sourcePresent)
        return GatherNoDurableCreditDetail::SourceTerminalStateUnobserved;
    if (observation.lootResponseDelta > 0 && observation.storeLootExecutionDelta == 0)
        return GatherNoDurableCreditDetail::LootResponseNotProcessed;
    if (observation.sourceUnlootedCount > 0 && observation.autostoreLootPacketDelta > 0)
        return GatherNoDurableCreditDetail::AutostoreRejected;
    if (observation.sourceUnlootedCount > 0 && observation.storeLootExecutionDelta > 0 &&
        observation.bagSpacePercent > 80)
        return GatherNoDurableCreditDetail::LootNotQueuedHighBagUsage;
    if (observation.sourceUnlootedCount > 0 && observation.storeLootExecutionDelta > 0)
        return GatherNoDurableCreditDetail::LootFilteredOrIneligible;
    return GatherNoDurableCreditDetail::SourceLiveUnconsumed;
}

inline char const* GatherNoDurableCreditDetailName(GatherNoDurableCreditDetail detail)
{
    switch (detail)
    {
        case GatherNoDurableCreditDetail::SourceMismatch: return "source_mismatch";
        case GatherNoDurableCreditDetail::SourceGenerationUnobserved: return "source_generation_unobserved";
        case GatherNoDurableCreditDetail::SourceMaterialUnproven: return "source_material_unproven";
        case GatherNoDurableCreditDetail::SourceTerminalStateUnobserved: return "source_terminal_state_unobserved";
        case GatherNoDurableCreditDetail::LootResponseNotProcessed: return "loot_response_not_processed";
        case GatherNoDurableCreditDetail::LootNotQueuedHighBagUsage: return "loot_not_queued_high_bag_usage";
        case GatherNoDurableCreditDetail::LootFilteredOrIneligible: return "loot_filtered_or_ineligible";
        case GatherNoDurableCreditDetail::AutostoreRejected: return "autostore_rejected";
        case GatherNoDurableCreditDetail::SourceConsumedWithoutProcessing: return "source_consumed_without_processing";
        case GatherNoDurableCreditDetail::SourceLiveUnconsumed: return "source_live_unconsumed";
        default: return "unknown";
    }
}

inline GatherCreditDecision EvaluateGatherCredit(GatherCreditObservation const& observation)
{
    if (!observation.interactionAttempted)
        return GatherCreditDecision::AwaitingInteraction;

    bool const sourceProof = observation.sourceMatched && observation.sourceLootGenerationObserved &&
        observation.sourceYieldsRequestedMaterial;

    if (observation.materialFulfillmentRequired)
    {
        // ObtainMaterial is intentionally stricter than HarvestNode. A consumed node or a
        // can-yield template proves only that harvesting happened; it does not prove the requested
        // material entered this bot's inventory.
        if (sourceProof && observation.matchingMaterialDelta)
            return GatherCreditDecision::ConfirmedMaterialCredit;
    }
    else
    {
        // Loot::isLooted is only durable evidence when the exact pending source was matched and
        // the normal loot pipeline has actually processed it. This is HarvestNode evidence.
        if (sourceProof && observation.sourceLootConsumed && observation.lootProcessingObserved)
            return GatherCreditDecision::ConfirmedLootConsumed;

        // Core UpdateGatherSkill is canonical HarvestNode credit only when the increase is tied to
        // this pending source and profession. The legacy aggregate skillIncreased bit is ignored.
        if (sourceProof && observation.matchingSkillDelta)
            return GatherCreditDecision::ConfirmedSkillCredit;

        if (sourceProof && observation.matchingMaterialDelta)
            return GatherCreditDecision::ConfirmedMaterialCredit;
    }

    // A source-linked inventory/skill delta without an independent source-yield proof is not
    // attributable to this interaction. Keep waiting/fail closed instead of turning an unrelated
    // item or skill change into gather completion.

    if (observation.elapsedSeconds < observation.timeoutSeconds)
        return GatherCreditDecision::AwaitingCredit;

    if (!observation.lootResponseObserved && !observation.lootProcessingObserved)
        return GatherCreditDecision::FailedNoLootResponse;

    return GatherCreditDecision::FailedNoDurableCredit;
}

inline char const* GatherCreditDecisionName(GatherCreditDecision decision)
{
    switch (decision)
    {
        case GatherCreditDecision::AwaitingInteraction: return "interaction_pending";
        case GatherCreditDecision::AwaitingCredit: return "awaiting_durable_credit";
        case GatherCreditDecision::ConfirmedLootConsumed: return "loot_consumed";
        case GatherCreditDecision::ConfirmedSkillCredit: return "skill_credit";
        case GatherCreditDecision::ConfirmedMaterialCredit: return "material_credit";
        case GatherCreditDecision::FailedNoLootResponse: return "no_loot_response";
        case GatherCreditDecision::FailedNoDurableCredit: return "no_durable_credit";
        default: return "unknown";
    }
}

inline bool IsConfirmedHarvestNodeCredit(GatherCreditDecision decision)
{
    return decision == GatherCreditDecision::ConfirmedLootConsumed ||
           decision == GatherCreditDecision::ConfirmedSkillCredit;
}

inline bool IsConfirmedMaterialCredit(GatherCreditDecision decision)
{
    return decision == GatherCreditDecision::ConfirmedMaterialCredit;
}

inline bool IsConfirmedGatherCredit(GatherCreditDecision decision)
{
    return IsConfirmedHarvestNodeCredit(decision) || IsConfirmedMaterialCredit(decision);
}
}

#endif
