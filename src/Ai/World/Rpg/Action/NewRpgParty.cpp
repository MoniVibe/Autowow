/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Party leader tick (policy: AutoWow/PartyPolicy.h, runtime: AutoWow/PartyRuntime.cpp).

#include <mutex>
#include <unordered_map>

#include "GameTime.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "PartyPolicy.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"

namespace
{
// Leader guid -> (group quest last preferred, game ms). A blocked quest is not forced again for a while.
std::mutex gPreferLock;
std::unordered_map<uint32, std::pair<uint32, uint64>> gPreferred;
constexpr uint64 kRepreferMs = 600000;
}

bool NewRpgBaseAction::PartyStep()
{
    AutoWowParty::LeaderOrder order;
    if (!AutoWowParty::GetLeaderOrder(bot->GetGUID().GetCounter(), order))
        return false;
    NewRpgInfo& info = botAI->rpgInfo;
    switch (order.phase)
    {
        case AutoWowParty::Phase::Approach:
        {
            if (!bot->IsAlive() || bot->IsInFlight() || bot->IsInCombat() || bot->GetMapId() != order.map)
                return false;
            // No-teleport long walk to the entrance trigger; followers follow (world thread sets it up).
            bool const stuck = WalkLeg(WorldPosition(order.map, order.x, order.y, order.z));
            AutoWowParty::NoteApproachTick(bot->GetGUID().GetCounter(), stuck);
            return true;
        }
        case AutoWowParty::Phase::Stage:
        case AutoWowParty::Phase::Inside:
        case AutoWowParty::Phase::Exit:
            // The world thread stages the party in the trigger and the dungeon navigator leads inside: hold.
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            return true;
        case AutoWowParty::Phase::None:
            break;
    }
    // Questing together: an incomplete group quest in the log goes first (the party exists for it).
    if (std::get_if<NewRpgInfo::DoQuest>(&info.data) || !bot->IsAlive() || bot->IsInCombat())
        return false;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint64 const now = uint64(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 const id = bot->GetQuestSlotQuestId(i);
        Quest const* quest = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
        if (quest && AutoWowParty::IsGroupQuest(quest->GetType(), quest->GetSuggestedPlayers()) &&
            bot->GetQuestStatus(id) == QUEST_STATUS_INCOMPLETE && IsQuestCapableDoing(quest))
        {
            {
                std::lock_guard<std::mutex> guard(gPreferLock);
                auto const it = gPreferred.find(guid);
                if (it != gPreferred.end() && it->second.first == id && now < it->second.second + kRepreferMs)
                    continue;
                gPreferred[guid] = {id, now};
            }
            info.ChangeToDoQuest(id, quest);
            return true;
        }
    }
    return false;
}
