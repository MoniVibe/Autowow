/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TACTICALCLASSSTRATEGY_H
#define PLAYERBOTS_TACTICALCLASSSTRATEGY_H

#include <cstdint>

#include "Strategy.h"
#include "TacticalPolicy.h"
#include "Trigger.h"

class PlayerbotAI;

// Tactical combat layer for every non-priest class (docs/TACTICAL_COMBAT_PLAN.md 2.3 / 5; AutoWow.Tactics.Enable
// + AutoWow.Tactics.Classes, treatment arm only - AiFactory adds "tactical"). Same model as the priest
// (TacticalPriestStrategy.h): the tactic is a VALUE (AutoWowTactics::Current), these static strategies stay in
// the engine all session, the "tac ..." triggers fire only while the matching tactic runs, and
// ClassTacticMultiplier scales stock actions from AutoWow.Tactics.<Class>.Factors.*. With no tactic running
// every trigger is off and every factor is 1. The "tac ..." triggers are registered once, in the shared
// TriggerContext; each class context registers "tactical" with its family.

// Condition ids and their pure evaluation: AutoWowTactics::ClassTacticCondition / ConditionHolds
// (TacticalPolicy.h, unit-tested).
using ClassTacticCondition = AutoWowTactics::ClassTacticCondition;

class ClassTacticTrigger : public Trigger
{
public:
    ClassTacticTrigger(PlayerbotAI* botAI, std::string const name, ClassTacticCondition condition)
        : Trigger(botAI, name), condition(condition)
    {
    }

    bool IsActive() override;

private:
    ClassTacticCondition condition;
};

class ClassTacticMultiplier : public Multiplier
{
public:
    ClassTacticMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "class tactic") {}

    float GetValue(Action* action) override;
};

class TacticalClassStrategy : public Strategy
{
public:
    TacticalClassStrategy(PlayerbotAI* botAI, AutoWowTactics::Family family) : Strategy(botAI), family(family) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "tactical"; }

private:
    AutoWowTactics::Family family;
};

#endif
