/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGSTRATEGYCONTEXT_H
#define PLAYERBOTS_GATHERINGSTRATEGYCONTEXT_H

#include "NamedObjectContext.h"
#include "WorkerGatherStrategy.h"

class GatheringStrategyContext : public NamedObjectContext<Strategy>
{
public:
    GatheringStrategyContext() { creators["worker gather"] = &GatheringStrategyContext::worker_gather; }

private:
    static Strategy* worker_gather(PlayerbotAI* botAI) { return new WorkerGatherStrategy(botAI); }
};

#endif
