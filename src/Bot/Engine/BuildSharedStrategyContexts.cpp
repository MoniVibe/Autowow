/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "DungeonStrategyContext.h"
#include "RaidStrategyContext.h"
#include "StrategyContext.h"
#include "Ai/Dungeon/Generic/DungeonNavigatorContext.h"
#include "Ai/Raid/Generic/RaidEncounterTriageContext.h"
#include "Ai/World/Gathering/GatheringStrategyContext.h"

void AiObjectContext::BuildSharedStrategyContexts(SharedNamedObjectContextList<Strategy>& strategyContexts)
{
    strategyContexts.Add(new StrategyContext());
    strategyContexts.Add(new MovementStrategyContext());
    strategyContexts.Add(new AssistStrategyContext());
    strategyContexts.Add(new QuestStrategyContext());
    strategyContexts.Add(new GatheringStrategyContext());
    strategyContexts.Add(new DungeonStrategyContext());
    strategyContexts.Add(new DungeonNavigatorStrategyContext());
    strategyContexts.Add(new RaidStrategyContext());
    strategyContexts.Add(new RaidEncounterTriageStrategyContext());
}
