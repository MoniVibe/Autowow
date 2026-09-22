/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef AUTOWOW_CAMPAIGN_TRAVEL_ACTION_H
#define AUTOWOW_CAMPAIGN_TRAVEL_ACTION_H

#include "MovementActions.h"
#include "NamedObjectContext.h"

class CampaignTravelAction final : public MovementAction
{
public:
    explicit CampaignTravelAction(PlayerbotAI* botAI) : MovementAction(botAI, "campaign travel") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool ExecuteSelectedLeg();
};

class CampaignTravelActionContext : public NamedObjectContext<Action>
{
public:
    CampaignTravelActionContext()
    {
        creators["campaign travel"] = &CampaignTravelActionContext::campaignTravel;
    }

private:
    static Action* campaignTravel(PlayerbotAI* botAI) { return new CampaignTravelAction(botAI); }
};

#endif  // AUTOWOW_CAMPAIGN_TRAVEL_ACTION_H
