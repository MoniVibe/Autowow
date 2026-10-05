/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CLASSFIXESP1STRATEGY_H
#define PLAYERBOTS_CLASSFIXESP1STRATEGY_H

#include <cstdint>

#include "ClassFixesP1Policy.h"
#include "GenericSpellActions.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// AutoWow.Combat.ClassFixesP1 (default 0): multi-mob kits and the Enhancement mana fix (ClassFixesP1Policy.h).
// AiFactory adds "fixes p1" to the combat and non-combat engines of covered bots (warrior Arms/Fury, rogue,
// Enhancement shaman, cat Feral druid); the triggers and gates hold only for a solo bot, so a grouped bot runs
// stock. Independent of the tactical layer ("tactical"); both multipliers stack.
using ClassFixCond = AutoWowClassFixesP1::Cond;

class ClassFixP1Trigger : public Trigger
{
public:
    ClassFixP1Trigger(PlayerbotAI* botAI, std::string const name, ClassFixCond condition)
        : Trigger(botAI, name), condition(condition)
    {
    }

    bool IsActive() override;

private:
    ClassFixCond condition;
};

class ClassFixP1Multiplier : public Multiplier
{
public:
    ClassFixP1Multiplier(PlayerbotAI* botAI) : Multiplier(botAI, "class fixes p1") {}

    float GetValue(Action* action) override;
};

class ClassFixesP1Strategy : public Strategy
{
public:
    ClassFixesP1Strategy(PlayerbotAI* botAI, std::uint32_t classId) : Strategy(botAI), classId(classId) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "fixes p1"; }

private:
    std::uint32_t classId;
};

// Single-target cc on an add (AutoWowClassFixesP1::PickAdd): rogue "blind on add" / "gouge on add".
class CastOnAddP1Action : public CastSpellAction
{
public:
    CastOnAddP1Action(PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

    Unit* GetTarget() override;
    ActionThreatType getThreatType() override { return ActionThreatType::None; }
};

#endif
