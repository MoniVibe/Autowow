/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_WORKERGATHERACTION_H
#define PLAYERBOTS_WORKERGATHERACTION_H

#include "GatheringCandidatePolicy.h"
#include "GatheringSafetyPolicy.h"
#include "NamedObjectContext.h"
#include "NewRpgBaseAction.h"
#include "../../../AutoWow/OracleGatherExecutor.h"

#include <cstdint>

class GameObject;
class Player;

class WorkerGatherAction : public NewRpgBaseAction, public Qualified
{
public:
    explicit WorkerGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "worker gather seek") {}

    using ReceiptPollFunction = bool (*)(void*, AutoWowGather::GatherCreditObservation*) noexcept;
    using ReceiptSnapshotFunction = bool (*)(void*, AutoWowGather::GatherReceiptSnapshot*) noexcept;
    using NativeStepFunction = bool (*)(void*,
                                        AutoWowOracleGatherExecutor::NativeStepRequest const&) noexcept;
    using EvidenceCaptureFunction = void (*)(void*, std::uint32_t,
                                              AutoWowOracle::EvidenceCounters&) noexcept;

    struct FixedTargetNativeContext
    {
        PlayerbotAI* botAI = nullptr;
        Player* bot = nullptr;
        GameObject* source = nullptr;
        AutoWowGather::Candidate candidate;
        bool candidateValid = false;
        AutoWowOracleGatherExecutor::FixedTargetObservation before;
        ReceiptSnapshotFunction controlledReceiptSnapshot = nullptr;
        void* controlledReceiptSnapshotContext = nullptr;
    };

    bool Execute(Event event) override;
    bool isUseful() override;

    // Concrete world-thread callback for AutoWow Oracle. The callback is exported as the exact
    // NativeStepFunction type so a runtime can pass it directly; it does not enter Execute(),
    // CurrentCandidate(), or SelectCandidateFor().
    static AutoWowOracleGatherExecutor::NativeStepObservation ExecuteFixedTargetNativeStep(
        void* context, AutoWowOracleGatherExecutor::NativeStepRequest const& request) noexcept;

    // Production receipt loop: poll canonical evidence before another native action, invoke at
    // most one exact native step, then poll canonical evidence again. The callback supplies the
    // live world operations; this seam is deliberately separate from candidate selection so the
    // focused test can exercise the same receipt handoff without inventing credit in Dispatch().
    static AutoWowOracleGatherExecutor::NativeStepObservation ExecuteCanonicalReceiptLoop(
        void* context, AutoWowOracleGatherExecutor::NativeStepRequest const& request,
        AutoWowOracleGatherExecutor::FixedTargetObservation const& before,
        ReceiptPollFunction observeReceipt, NativeStepFunction nativeStep,
        EvidenceCaptureFunction captureEvidence = nullptr) noexcept
    {
        AutoWowOracleGatherExecutor::NativeStepObservation result;
        result.after = before;
        result.after.nodeReservation = request.nodeReservation;
        result.after.world.ownershipProof = request.request.ownershipProof;
        result.after.candidate = request.candidate;

        if (!observeReceipt)
            return result;

        AutoWowGather::GatherCreditObservation beforeCredit;
        if (observeReceipt(context, &beforeCredit))
        {
            result.credit = beforeCredit;
            if (captureEvidence)
                captureEvidence(context, request.request.gather.materialItemId,
                                result.after.world.evidence);
            result.dispatchAccepted = true;
            result.stepsInvoked = 1;
            return result;
        }

        // A terminal source may have disappeared after the canonical loot pipeline consumed it.
        // A receipt-only poll therefore fails closed without inventing another native action.
        if (!nativeStep)
            return result;

        result.dispatchAccepted = nativeStep(context, request);
        result.stepsInvoked = 1;
        if (!result.dispatchAccepted)
            return result;

        AutoWowGather::GatherCreditObservation afterCredit;
        if (observeReceipt(context, &afterCredit))
        {
            result.credit = afterCredit;
            if (captureEvidence)
                captureEvidence(context, request.request.gather.materialItemId,
                                result.after.world.evidence);
        }
        return result;
    }

    // Production callback wrapper used by the focused receipt contract test. Live callers leave
    // the controlled snapshot hook null, so this invokes ObserveCandidateCredit against the real
    // worker state; tests provide raw source/inventory/skill snapshots, never a finished credit.
    static AutoWowOracleGatherExecutor::NativeStepObservation ExecuteProductionReceiptLoop(
        FixedTargetNativeContext* context,
        AutoWowOracleGatherExecutor::NativeStepRequest const& request,
        AutoWowOracleGatherExecutor::FixedTargetObservation const& before,
        NativeStepFunction nativeStep,
        EvidenceCaptureFunction captureEvidence = nullptr) noexcept
    {
        return ExecuteCanonicalReceiptLoop(context, request, before,
                                           &WorkerGatherAction::ObserveFixedTargetReceipt,
                                           nativeStep, captureEvidence);
    }

    static AutoWowOracleGatherExecutor::NativeStepFunction FixedTargetNativeCallback() noexcept
    {
        return &WorkerGatherAction::ExecuteFixedTargetNativeStep;
    }

    // Runtime-facing overload. It resolves the exact source itself and wires the concrete native
    // callback; the callback-taking overload below remains available for pure executor tests.
    static AutoWowOracleGatherExecutor::DispatchResult ExecuteFixedTargetGather(
        PlayerbotAI* botAI,
        AutoWowOracleExecutor::ExecutorRequest const& request,
        AutoWowOracle::IntentLease const& lease,
        AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot const& world,
        AutoWowOracleGatherExecutor::NodeReservationProof const& nodeReservation);

    // Runtime-facing overload for a frame that has already pre-probed and published the exact
    // source. It is required while a pending loot receipt has moved the node out of GO_READY; the
    // callback still revalidates the same identity and never resolves a replacement.
    static AutoWowOracleGatherExecutor::DispatchResult ExecuteFixedTargetGather(
        PlayerbotAI* botAI,
        AutoWowOracleExecutor::ExecutorRequest const& request,
        AutoWowOracle::IntentLease const& lease,
        AutoWowOracleGatherExecutor::FixedTargetWorldSnapshot const& world,
        AutoWowOracleGatherExecutor::NodeReservationProof const& nodeReservation,
        AutoWowOracleGatherExecutor::FixedTargetCandidate const& exactCandidate);

    // Pure fixed-target dispatch helper. It receives one exact candidate and forwards only the
    // typed fixed-target request; it never calls SelectCandidateFor() or substitutes current state.
    static AutoWowOracleGatherExecutor::DispatchResult ExecuteFixedTargetGather(
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

private:
    bool MoveDeterministicToExact(WorldPosition const& destination,
                                  AutoWowOracle::DecisionId decisionId,
                                  AutoWowOracle::GatherSourceReference const& source,
                                  bool* outStuck);
    bool ExecuteWatchdog();
    bool FinishLeaseObservation(AutoWowGather::Candidate const& candidate,
                                AutoWowGather::CandidateLeaseEvent leaseEvent);

    static bool ObserveFixedTargetReceipt(void* context,
                                          AutoWowGather::GatherCreditObservation* observation) noexcept;
    static bool DispatchFixedTargetNativeStep(
        void* context, AutoWowOracleGatherExecutor::NativeStepRequest const& request) noexcept;
    static void CaptureFixedTargetEvidence(void* context, std::uint32_t materialItemId,
                                           AutoWowOracle::EvidenceCounters& evidence) noexcept;
};

#endif
