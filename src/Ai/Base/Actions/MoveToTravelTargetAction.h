/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOVETOTRAVELTARGETACTION_H
#define PLAYERBOTS_MOVETOTRAVELTARGETACTION_H

#include "MovementActions.h"

class PlayerbotAI;
class TravelTarget;

class MoveToTravelTargetAction : public MovementAction
{
public:
    MoveToTravelTargetAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to travel target") {}

    bool Execute(Event event) override;
    // Controlled quest-acquisition entry point. It keeps the normal safety gates but bypasses
    // generic Engine::ExecuteAction usefulness/strategy scheduling before staged movement.
    bool ExecuteQuestGiverStagedEntry(TravelTarget* target);
    bool isUseful() override;

private:
    bool MoveQuestGiverStaged(TravelTarget* target);
};

#endif
