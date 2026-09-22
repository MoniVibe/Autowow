/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATORCONTEXT_H
#define PLAYERBOTS_DUNGEONNAVIGATORCONTEXT_H

#include "DungeonNavigator.h"
#include "DungeonTransition.h"
#include "NamedObjectContext.h"

class DungeonNavigatorStrategyContext : public NamedObjectContext<Strategy>
{
public:
    DungeonNavigatorStrategyContext()
    {
        creators["dungeon navigator"] = &DungeonNavigatorStrategyContext::dungeonNavigator;
        creators["dungeon transition"] = &DungeonNavigatorStrategyContext::dungeonTransition;
    }

private:
    static Strategy* dungeonNavigator(PlayerbotAI* botAI) { return new DungeonNavigatorStrategy(botAI); }
    static Strategy* dungeonTransition(PlayerbotAI* botAI) { return new DungeonTransitionStrategy(botAI); }
};

class DungeonNavigatorActionContext : public NamedObjectContext<Action>
{
public:
    DungeonNavigatorActionContext()
    {
        creators["dungeon navigate next encounter"] =
            &DungeonNavigatorActionContext::navigateNextEncounter;
        creators["dungeon party transition"] =
            &DungeonNavigatorActionContext::partyTransition;
    }

private:
    static Action* navigateNextEncounter(PlayerbotAI* botAI)
    {
        return new DungeonNavigateNextEncounterAction(botAI);
    }
    static Action* partyTransition(PlayerbotAI* botAI)
    {
        return new DungeonPartyTransitionAction(botAI);
    }
};

#endif
