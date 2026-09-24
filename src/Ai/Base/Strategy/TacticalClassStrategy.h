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

// Wire-stable condition ids of the "tac ..." triggers (slot names: TacticalPolicy.h).
enum class ClassTacticCondition : std::uint8_t
{
    Single = 0,
    Multi = 1,
    Emergency = 2,
    Escape = 3,
    Heal = 4,              // single/multi, hp < HealHpPct (+10 in multi)
    Control = 5,           // multi, control ready, >= ControlMinMelee controllable melee on the bot,
                           // (hp < ControlHpPct or >= 3 attackers), no idle adds near (area fear/stun/nova)
    ControlAdd = 6,        // multi, control ready, >= 2 attackers, no idle adds near (single-target cc on the add)
    EmergencyControl = 7,  // emergency/escape, control ready, >= 1 controllable melee on the bot
    MeleeOnMe = 8,         // single/multi, a melee attacker on the bot
    Runner = 9,            // single/multi, the current target flees (not feared) and is not snared
    LowMana = 10,          // single/multi, mana user below LowManaPct
    LifeTap = 11,          // single/multi, mana below LowManaPct and hp >= EmergencyExitHpPct + 15
    PetLow = 12,           // single/multi, combat pet alive below 40 % hp
    Kite = 13              // single/multi, the current target is rooted/frozen in melee range (step out)
};

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
