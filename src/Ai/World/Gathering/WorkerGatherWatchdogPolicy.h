/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_WORKERGATHERWATCHDOGPOLICY_H
#define PLAYERBOTS_WORKERGATHERWATCHDOGPOLICY_H

#include "GatheringSafetyPolicy.h"

#include <cstdint>

namespace AutoWowGather
{
struct WorkerGatherWatchdogObservation
{
    bool explicitWorker = false;
    bool hasCandidate = false;
    std::uint64_t candidateId = 0;
    std::uint64_t leaseCandidateId = 0;
    CandidateLeaseEvent leaseEvent = CandidateLeaseEvent::None;
    std::uint64_t lootTargetCandidateId = 0;
};

struct WorkerGatherWatchdogDecision
{
    bool actionSucceeded = false;
    bool expireCandidate = false;
    bool clearMatchingLootTarget = false;
    std::uint8_t eventSequenceAdvance = 0;
};

inline WorkerGatherWatchdogDecision EvaluateWorkerGatherWatchdog(
    WorkerGatherWatchdogObservation const& observation)
{
    bool const expired = observation.leaseEvent == CandidateLeaseEvent::ExpiredNoProgress ||
                         observation.leaseEvent == CandidateLeaseEvent::ExpiredMaxLease;
    if (!observation.explicitWorker || !observation.hasCandidate || !observation.candidateId ||
        observation.leaseCandidateId != observation.candidateId || !expired)
        return {};

    WorkerGatherWatchdogDecision decision;
    decision.actionSucceeded = true;
    decision.expireCandidate = true;
    decision.clearMatchingLootTarget =
        observation.lootTargetCandidateId && observation.lootTargetCandidateId == observation.candidateId;
    decision.eventSequenceAdvance = 1;
    return decision;
}
}

#endif
