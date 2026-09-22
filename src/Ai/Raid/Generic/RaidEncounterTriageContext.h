/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDENCOUNTERTRIAGECONTEXT_H
#define PLAYERBOTS_RAIDENCOUNTERTRIAGECONTEXT_H

#include "NamedObjectContext.h"
#include "RaidEncounterTriage.h"

class RaidEncounterTriageStrategyContext : public NamedObjectContext<Strategy>
{
public:
    RaidEncounterTriageStrategyContext()
    {
        creators["raid triage"] = &RaidEncounterTriageStrategyContext::raidTriage;
    }

private:
    static Strategy* raidTriage(PlayerbotAI* ai) { return new RaidEncounterTriageStrategy(ai); }
};

class RaidEncounterTriageActionContext : public NamedObjectContext<Action>
{
public:
    RaidEncounterTriageActionContext()
    {
        creators["raid attack priority add"] = &RaidEncounterTriageActionContext::attackPriorityAdd;
        creators["raid gather add wave"] = &RaidEncounterTriageActionContext::gatherAddWave;
        creators["raid hold owned boss target"] = &RaidEncounterTriageActionContext::holdOwnedBossTarget;
        creators["raid prepare for fear"] = &RaidEncounterTriageActionContext::prepareForFear;
    }

private:
    static Action* attackPriorityAdd(PlayerbotAI* ai) { return new RaidAttackPriorityAddAction(ai); }
    static Action* gatherAddWave(PlayerbotAI* ai) { return new RaidGatherAddWaveAction(ai); }
    static Action* holdOwnedBossTarget(PlayerbotAI* ai) { return new RaidHoldOwnedBossTargetAction(ai); }
    static Action* prepareForFear(PlayerbotAI* ai) { return new RaidPrepareForFearAction(ai); }
};

class RaidEncounterTriageTriggerContext : public NamedObjectContext<Trigger>
{
public:
    RaidEncounterTriageTriggerContext()
    {
        creators["raid priority add available"] = &RaidEncounterTriageTriggerContext::priorityAddAvailable;
        creators["raid add wave gather"] = &RaidEncounterTriageTriggerContext::addWaveGather;
        creators["raid owned boss target available"] = &RaidEncounterTriageTriggerContext::ownedBossTargetAvailable;
        creators["raid incoming fear"] = &RaidEncounterTriageTriggerContext::incomingFear;
    }

private:
    static Trigger* priorityAddAvailable(PlayerbotAI* ai) { return new RaidPriorityAddAvailableTrigger(ai); }
    static Trigger* addWaveGather(PlayerbotAI* ai) { return new RaidAddWaveGatherTrigger(ai); }
    static Trigger* ownedBossTargetAvailable(PlayerbotAI* ai) { return new RaidOwnedBossTargetAvailableTrigger(ai); }
    static Trigger* incomingFear(PlayerbotAI* ai) { return new RaidIncomingFearTrigger(ai); }
};

#endif
