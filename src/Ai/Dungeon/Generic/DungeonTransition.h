/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONTRANSITION_H
#define PLAYERBOTS_DUNGEONTRANSITION_H

#include "Action.h"
#include "Strategy.h"

class DungeonTransitionStrategy : public Strategy
{
public:
    explicit DungeonTransitionStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dungeon transition"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
};

class DungeonPartyTransitionAction : public Action
{
public:
    explicit DungeonPartyTransitionAction(PlayerbotAI* botAI)
        : Action(botAI, "dungeon party transition") {}

    bool Execute(Event event) override;

private:
    uint32 nextCheckTime = 0;
};

#endif
