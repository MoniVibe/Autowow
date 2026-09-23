/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TACTICALPRIESTSTRATEGY_H
#define PLAYERBOTS_TACTICALPRIESTSTRATEGY_H

#include <cstdint>

#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// Tactical combat layer, priest (docs/TACTICAL_COMBAT_PLAN.md 2.3 / 3.1; AutoWow.Tactics.Enable, treatment
// arm only - AiFactory adds these). A tactic is a VALUE (AutoWowTactics::Current), never a strategy swap:
// these static strategies stay in the engine for the whole session, their triggers fire only while the
// matching tactic runs, and TacticMultiplier scales stock actions from the per-tactic factor tables.
// With no tactic running (not eligible / out of an engagement) every trigger is off and every factor is 1.

// Wire-stable condition ids of the "tactic ..." triggers.
enum class TacticCondition : std::uint8_t
{
    Wand = 0,             // wand-cycle and the wand is not already shooting the current target
    Renew = 1,            // wand/burst/multi, hp < RenewHpPct (+ multi bonus), no Renew running
    Heal = 2,             // wand/burst/multi, hp < HealHpPct (+ multi bonus)
    Multi = 3,            // multi-mob control running (DoT spread)
    Scream = 4,           // multi: >= ScreamMinMelee fearable melee, (hp < ScreamHpPct or >= 3 attackers), no idle adds
    EmergencyScream = 5,  // emergency / escape: any fearable melee on the bot, Scream ready
    Emergency = 6,
    Escape = 7,
    RestMana = 8,         // out of combat below PullMinManaPct
    RestHealth = 9        // out of combat below PullMinHpPct
};

class TacticTrigger : public Trigger
{
public:
    TacticTrigger(PlayerbotAI* botAI, std::string const name, TacticCondition condition)
        : Trigger(botAI, name), condition(condition)
    {
    }

    bool IsActive() override;

private:
    TacticCondition condition;
};

class TacticMultiplier : public Multiplier
{
public:
    TacticMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic") {}

    float GetValue(Action* action) override;
};

class TacticalPriestStrategy : public Strategy
{
public:
    TacticalPriestStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "tactical"; }
};

class TacticalPriestNonCombatStrategy : public Strategy
{
public:
    TacticalPriestNonCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactical nc"; }
};

#endif
