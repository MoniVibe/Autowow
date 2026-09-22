/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGWORKERSTATE_H
#define PLAYERBOTS_GATHERINGWORKERSTATE_H

#include "GatheringCandidatePolicy.h"
#include "WorkerGatherDeathRecoveryPolicy.h"
#include "GatheringReceiptPolicy.h"
#include "GatheringSafetyPolicy.h"
#include "ObjectGuid.h"
#include "../../../AutoWow/OracleGatherExecutor.h"
#include "../../../AutoWow/OracleGatherRuntimePolicy.h"

#include <cstdint>
#include <optional>
#include <string>

class Player;
class PlayerbotAI;
class GameObject;

namespace AutoWowGather
{
enum GatherLootAdmissionFailure : std::uint32_t
{
    GatherLootAdmissionNone = 0,
    GatherLootAdmissionNotWorker = 1u << 0,
    GatherLootAdmissionNotPrepared = 1u << 1,
    GatherLootAdmissionGuidMismatch = 1u << 2,
    GatherLootAdmissionRuntimeMismatch = 1u << 3,
    GatherLootAdmissionSpawnMismatch = 1u << 4,
    GatherLootAdmissionEntryMismatch = 1u << 5,
    GatherLootAdmissionLiveItemMismatch = 1u << 6,
    GatherLootAdmissionRequestedMismatch = 1u << 7
};

enum class CandidateFailureKind : std::uint8_t
{
    Generic = 0,
    UnsafeLiquid
};

struct RouteSnapshot
{
    bool explicitWorker = false;
    bool hasCandidate = false;
    Candidate candidate;
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
    bool awaitingCredit = false;
    bool oracleExactSourceOnly = false;
    AutoWowOracle::GatherSourceReference oracleSource;
    AutoWowOracle::GatherGoal gatherGoal = AutoWowOracle::GatherGoal::Unknown;
    AutoWowGatherRuntimePolicy::ExactMotionProvenance oracleMotion;
    std::string creditEvidence = "none";
    std::uint64_t candidateSpawnId = 0;
    std::uint32_t candidateEntry = 0;
    std::uint32_t profession = 0;
    std::uint32_t skillType = 0;
    std::uint64_t sourceGuid = 0;
    std::uint32_t sourceEntry = 0;
    bool sourceMatched = false;
    bool sourceLootGenerationObserved = false;
    bool sourceYieldsRequestedMaterial = false;
    bool sourcePresentAtLastObservation = false;
    bool sourceLootConsumedObserved = false;
    std::uint32_t sourceUnlootedCount = 0;
    std::uint32_t sourceLootItemCount = 0;
    std::uint32_t bagSpacePercent = 0;
    std::string failureDetail = "none";
    bool matchingMaterialDelta = false;
    bool matchingSkillDelta = false;
    std::uint32_t itemEntry = 0;
    std::uint32_t requestedMaterialItemId = 0;
    std::uint32_t itemBefore = 0;
    std::uint32_t itemAfter = 0;
    std::uint64_t interactionAttemptedAtSeconds = 0;
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
    DeathRecoveryState deathRecovery;
};

void ActivateWorker(std::uint32_t botGuid);
void DeactivateWorker(std::uint32_t botGuid);
bool IsExplicitWorker(std::uint32_t botGuid);

// Plan one bounded handoff to the ordinary playerbot death actions. The returned transition is
// already reserved in worker state, so repeated dead-engine ticks cannot emit duplicate release,
// corpse-route, or reclaim packets inside the policy backoff window.
DeathRecoveryTransition PlanDeathRecovery(std::uint32_t botGuid, bool alive, bool ghost,
                                          bool corpseAvailable, bool corpseNear,
                                          bool routeInProgress);
// Called by the first non-combat worker tick after PlayerbotAI has switched back from the dead
// engine. Gathering remains paused until this observation sees the bot alive.
void ObserveWorkerAlive(std::uint32_t botGuid);
std::optional<Candidate> CurrentCandidate(std::uint32_t botGuid);
std::optional<Candidate> SelectCandidateFor(Player* bot, PlayerbotAI* botAI);
// Exact Oracle resolution is deliberately separate from the ordinary selector. It matches the
// published source identity against the worker's authoritative candidate cache and never changes
// worker state or chooses a nearest/eligible substitute.
std::optional<Candidate> ResolveExactCandidate(
    AutoWowOracle::GatherSourceReference const& reference);
bool IsOracleExactSourceOnly(std::uint32_t botGuid);
// Publish one complete source identity for the Oracle. Resolution is exact and fail-closed; this
// never invokes the ordinary selector or chooses a nearby replacement.
bool PublishOracleSource(Player* bot, PlayerbotAI* botAI,
                         AutoWowOracle::GatherSourceReference const& reference);
bool AdoptExactCandidate(std::uint32_t botGuid, Candidate const& candidate,
                         AutoWowOracle::GatherSourceReference const& reference);
AutoWowGatherRuntimePolicy::ExactMotionProvenance GetOracleMotionProvenance(std::uint32_t botGuid);
bool SetOracleMotionProvenance(std::uint32_t botGuid, AutoWowOracle::DecisionId decisionId,
                               AutoWowOracle::GatherSourceReference const& reference);
void ClearOracleMotionProvenance(std::uint32_t botGuid);
bool HasRequiredTool(Player* bot, Candidate const& candidate);
bool CandidateSafetyCheckRequired(std::uint32_t botGuid);
void MarkCandidateSafetyAccepted(std::uint32_t botGuid);
void SetCandidateRuntimeGuid(std::uint32_t botGuid, std::uint64_t spawnId, ObjectGuid const& runtimeGuid);
void SetCandidateRuntimeIdentity(std::uint32_t botGuid,
                                 AutoWowOracle::GatherSourceReference const& reference,
                                 ObjectGuid const& runtimeGuid);
CandidateLeaseEvent ObserveCandidateProgress(std::uint32_t botGuid, float x, float y, float z,
                                             float distanceToCandidate, std::uint32_t cooldownSeconds);
void SetRouteStatus(std::uint32_t botGuid, std::string status, std::string event, std::string idleReason = {});
bool HasPendingCandidateCredit(std::uint32_t botGuid);
// Read-only canonical-loot admission for the prepared exact source, including synchronous store
// before post-interaction credit flags are set. This does not select a source, mutate inventory, or
// infer credit from a worker merely being in gather mode.
bool IsPendingGatherSourceLoot(PlayerbotAI* botAI, ObjectGuid const& sourceGuid, std::uint32_t itemId);
std::optional<GatherCreditDecision> ObserveCandidateCredit(Player* bot, PlayerbotAI* botAI);
std::optional<GatherCreditDecision> ObserveCandidateCredit(
    Player* bot, PlayerbotAI* botAI, GatherCreditObservation* observationOut);
bool MarkCandidateGathered(std::uint32_t botGuid, std::uint64_t spawnId);
void MarkCandidateMiss(std::uint32_t botGuid, std::uint32_t cooldownSeconds, std::string reason,
                       CandidateFailureKind failureKind = CandidateFailureKind::Generic);
RouteSnapshot GetRouteSnapshot(std::uint32_t botGuid);

// Read-only source-table/generated-loot proof used by the exact Oracle handoff. It never opens
// loot, changes inventory, or selects another source.
bool SourceYieldsMaterial(GameObject* source, std::uint32_t materialItemId);

// Fixed-target Oracle entry point. The caller supplies the already-fresh world, ownership, and
// node-reservation observations. This wrapper intentionally does not resolve or select a
// candidate; the concrete worker overload in WorkerGatherAction owns that live handoff.
inline AutoWowOracleGatherExecutor::DispatchResult ExecuteFixedTargetGather(
    AutoWowOracleExecutor::ExecutorRequest const& request,
    AutoWowOracle::IntentLease const& lease,
    AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot const& world,
    AutoWowOracleGatherExecutor::NodeReservationProof const& nodeReservation,
    AutoWowOracleGatherExecutor::FixedTargetCandidate const& exactCandidate,
    AutoWowOracleGatherExecutor::NativeStepFunction nativeStep,
    void* context = nullptr)
{
    AutoWowOracleGatherExecutor::FixedTargetObservation before;
    before.world = world;
    before.nodeReservation = nodeReservation;
    before.candidate = exactCandidate;
    return AutoWowOracleGatherExecutor::OracleGatherExecutor::Dispatch(
        request, lease, exactCandidate, before, nativeStep, context);
}
}

#endif
