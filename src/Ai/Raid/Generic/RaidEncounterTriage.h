/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDENCOUNTERTRIAGE_H
#define PLAYERBOTS_RAIDENCOUNTERTRIAGE_H

#include "MovementActions.h"
#include "RaidTargetSelectionAction.h"
#include "Strategy.h"
#include "Trigger.h"

class RaidPriorityAddAvailableTrigger : public Trigger
{
public:
    RaidPriorityAddAvailableTrigger(PlayerbotAI* botAI) : Trigger(botAI, "raid priority add available") {}
    bool IsActive() override;
};

class RaidAddWaveGatherTrigger : public Trigger
{
public:
    RaidAddWaveGatherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "raid add wave gather") {}
    bool IsActive() override;
};

class RaidOwnedBossTargetAvailableTrigger : public Trigger
{
public:
    RaidOwnedBossTargetAvailableTrigger(PlayerbotAI* botAI) : Trigger(botAI, "raid owned boss target available") {}
    bool IsActive() override;
};

class RaidHoldOwnedBossTargetAction : public CooperativeRaidTargetAction
{
public:
    RaidHoldOwnedBossTargetAction(PlayerbotAI* botAI)
        : CooperativeRaidTargetAction(botAI, "raid hold owned boss target") {}
    bool Execute(Event event) override;
};

class RaidAttackPriorityAddAction : public CooperativeRaidTargetAction
{
public:
    RaidAttackPriorityAddAction(PlayerbotAI* botAI) : CooperativeRaidTargetAction(botAI, "raid attack priority add") {}
    bool Execute(Event event) override;
};

class RaidGatherAddWaveAction : public MovementAction
{
public:
    RaidGatherAddWaveAction(PlayerbotAI* botAI) : MovementAction(botAI, "raid gather add wave") {}
    bool Execute(Event event) override;
};

class RaidIncomingFearTrigger : public Trigger
{
public:
    RaidIncomingFearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "raid incoming fear", 1) {}
    bool IsActive() override;
};

class RaidPrepareForFearAction : public Action
{
public:
    RaidPrepareForFearAction(PlayerbotAI* botAI) : Action(botAI, "raid prepare for fear") {}
    bool Execute(Event event) override;
};

class RaidEncounterTriageStrategy : public Strategy
{
public:
    RaidEncounterTriageStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "raid triage"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
