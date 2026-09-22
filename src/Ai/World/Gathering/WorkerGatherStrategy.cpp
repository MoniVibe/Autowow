/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "WorkerGatherStrategy.h"

void WorkerGatherStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Queue::Push deduplicates baskets by the constructed Action name. Both the qualified watchdog
    // and the normal seek construct WorkerGatherAction("worker gather seek"), so scheduling the
    // watchdog first leaves its ActionNode in the queue and drops the selector before it can call
    // SelectCandidateFor. The normal Execute path observes lease progress and expiry as well, so
    // schedule that path directly; 5.5 still outranks canonical add-gathering discovery (5.0).
    triggers.push_back(new TriggerNode("timer", { NextAction("worker gather seek", 5.5f) }));
}
