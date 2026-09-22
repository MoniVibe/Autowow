#include "AiObjectContext.h"
#include "StrategyContext.h"
#include "Ai/Dungeon/DungeonStrategyContext.h"
#include "Ai/Dungeon/Generic/DungeonNavigatorContext.h"
#include "Ai/Raid/Generic/RaidEncounterTriageContext.h"
#include "Ai/Raid/RaidStrategyContext.h"
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
