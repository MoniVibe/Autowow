/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_WORKERGATHERSTRATEGY_H
#define PLAYERBOTS_WORKERGATHERSTRATEGY_H

#include "Strategy.h"

class WorkerGatherStrategy : public Strategy
{
public:
    explicit WorkerGatherStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "worker gather"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
};

#endif
