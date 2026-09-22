/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_GATHERINGACTIONCONTEXT_H
#define PLAYERBOTS_GATHERINGACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "WorkerGatherAction.h"

class GatheringActionContext : public NamedObjectContext<Action>
{
public:
    GatheringActionContext() { creators["worker gather seek"] = &GatheringActionContext::worker_gather_seek; }

private:
    static Action* worker_gather_seek(PlayerbotAI* botAI) { return new WorkerGatherAction(botAI); }
};

#endif
