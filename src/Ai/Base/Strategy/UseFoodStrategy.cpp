/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseFoodStrategy.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "DeathLoopBreaker.h"
#include "RestGate.h"

void UseFoodStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    if (botAI->HasCheat(BotCheatMask::food))
    {
        triggers.push_back(new TriggerNode("medium health", { NextAction("food", 3.0f) }));
        triggers.push_back(new TriggerNode("high mana", { NextAction("drink", 3.0f) }));
    }
    else
    {
        triggers.push_back(new TriggerNode("low health", { NextAction("food", 3.0f) }));
        triggers.push_back(new TriggerNode("low mana", { NextAction("drink", 3.0f) }));
    }

    // AutoWow.Survival.RestGate (default 0): a solo independent bot eats / drinks up to the pull thresholds
    // (above the New RPG moves at 3.0). Without food it just regenerates while the pull hold lasts.
    // AutoWow.DeathLoop.V2 uses the same triggers for its forced rest after a spirit-healer res.
    if (AutoWowRestGate::Enabled() || AutoWowDeathLoop::V2Enabled())
    {
        triggers.push_back(new TriggerNode("rest gate health", { NextAction("food", 4.1f) }));
        triggers.push_back(new TriggerNode("rest gate mana", { NextAction("drink", 4.1f) }));
    }
}
