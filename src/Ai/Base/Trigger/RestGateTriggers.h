/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RESTGATETRIGGERS_H
#define PLAYERBOTS_RESTGATETRIGGERS_H

#include "RestGate.h"
#include "Trigger.h"

// AutoWow.Survival.RestGate: out of combat below the pull threshold ("rest gate health" / "rest gate
// mana"); pushed by the "food" strategy only with the flag on (RestGate.h).
class RestGateTrigger : public Trigger
{
public:
    RestGateTrigger(PlayerbotAI* botAI, std::string const name, bool mana) : Trigger(botAI, name), mana(mana) {}

    bool IsActive() override
    {
        return mana ? AutoWowRestGate::NeedsRestMana(botAI) : AutoWowRestGate::NeedsRestHealth(botAI);
    }

private:
    bool mana;
};

#endif
