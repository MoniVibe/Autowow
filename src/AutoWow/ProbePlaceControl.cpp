/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "ProbePlaceControl.h"

#include <vector>

#include "AutoWowBridge.h"
#include "AutoWowQuestLedger.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DBCStores.h"
#include "FixtureFactoryControl.h"
#include "GatheringWorkerState.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

namespace AutoWowProbePlace
{
namespace
{
Position PositionOf(Player* bot)
{
    return {bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(),
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation()};
}

bool ResolveOnlineBot(WireRequest const& request, Player*& bot, PlayerbotAI*& ai, std::string& error)
{
    bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(request.guid));
    if (!bot)
    {
        error = "fixture_target_not_online";
        return false;
    }
    ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai || ai->IsRealPlayer())
    {
        error = "fixture_target_not_playerbot";
        return false;
    }
    return true;
}

// Same removal as the core `.quest remove` command for a connected player: clear every log slot
// holding the quest, take its source item, forget the active status (and, when asked, the reward).
void RemoveQuest(Player* bot, Quest const* quest, bool unreward)
{
    uint32 const questId = quest->GetQuestId();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        if (bot->GetQuestSlotQuestId(slot) != questId)
            continue;
        bot->SetQuestSlot(slot, 0);
        bot->TakeQuestSourceItem(questId, false);
        if (quest->HasFlag(QUEST_FLAGS_FLAGS_PVP))
        {
            bot->pvpInfo.IsHostile = bot->pvpInfo.IsInHostileArea || bot->HasPvPForcingQuest();
            bot->UpdatePvPState();
        }
    }
    if (unreward)
        bot->RemoveRewardedQuest(questId);
    bot->RemoveActiveQuest(questId, false);
}

std::string Login(WireRequest const& request)
{
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(request.guid);
    if (!sCharacterCache->GetCharacterAccountIdByGuid(guid))
        return FormatError(request.verb, request.guid, "probe_character_not_found");

    if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai || ai->IsRealPlayer())
            return FormatError(request.verb, request.guid, "fixture_target_not_playerbot");
        return FormatLogin(request.guid, "already_online");
    }

    // The bridge `activate` login path without its league-enrollment check (the probe gate replaced
    // it): mode-neutral login through Playerbots' normal asynchronous character login.
    AutoWowGather::DeactivateWorker(request.guid);
    AutoWowPolicy::SetNoTeleport(request.guid, false);
    sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
    return FormatLogin(request.guid, "login_queued");
}

std::string Place(WireRequest const& request)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(request.questId);
    if (!quest)
        return FormatError(request.verb, request.guid, "probe_quest_unknown");

    Player* bot = nullptr;
    PlayerbotAI* ai = nullptr;
    std::string error;
    if (!ResolveOnlineBot(request, bot, ai, error))
        return FormatError(request.verb, request.guid, error);
    if (!bot->IsAlive())
        return FormatError(request.verb, request.guid, "probe_target_dead");
    if (bot->IsInCombat())
        return FormatError(request.verb, request.guid, "fixture_target_in_combat");
    if (bot->IsInFlight())
        return FormatError(request.verb, request.guid, "probe_target_in_flight");
    if (bot->IsBeingTeleported())
        return FormatError(request.verb, request.guid, "probe_teleport_in_progress");

    // Starter spawns: creature_queststarter / gameobject_queststarter joined to their spawn rows.
    std::vector<StarterCandidate> candidates;
    std::vector<SpawnData const*> spawns;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
    {
        if (questId != request.questId)
            continue;
        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* faction =
            creatureTemplate ? sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
        bool const friendly = faction &&
            bot->GetFactionReactionTo(bot->GetFactionTemplateEntry(), faction) >= REP_FRIENDLY;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.id != entry && data.id2 != entry && data.id3 != entry)
                continue;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            candidates.push_back({false, entry, spawnId, map && map->IsWorldMap(), friendly,
                                  data.mapid == bot->GetMapId()});
            spawns.push_back(&data);
        }
    }
    for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
    {
        if (questId != request.questId)
            continue;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            if (data.id != entry)
                continue;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            candidates.push_back({true, entry, spawnId, map && map->IsWorldMap(), true,
                                  data.mapid == bot->GetMapId()});
            spawns.push_back(&data);
        }
    }

    std::size_t const chosen = SelectStarter(candidates);
    if (chosen == candidates.size())
        return FormatError(request.verb, request.guid,
                           candidates.empty() ? "probe_no_starter_spawn" : "probe_no_world_map_starter");
    SpawnData const& spawn = *spawns[chosen];

    PlaceFacts facts;
    facts.guid = request.guid;
    facts.questId = request.questId;
    facts.candidates = static_cast<uint32>(candidates.size());
    facts.starter.kind = candidates[chosen].gameObject ? "gameobject" : "creature";
    facts.starter.entry = candidates[chosen].entry;
    facts.starter.spawnId = candidates[chosen].spawnId;
    facts.starter.position = {spawn.mapid, 0, 0, spawn.posX, spawn.posY, spawn.posZ, spawn.orientation};
    facts.previousStatus = static_cast<uint32>(bot->GetQuestStatus(request.questId));
    facts.previousRewarded = bot->IsQuestRewarded(request.questId);

    // All refusals precede mutation. SETUP from here: every change below is attributed to the probe
    // by one `contaminated` ledger line per touched quest (reason probe_setup).
    bool const ledger = AutoWowQuestLedger::Enabled();
    if (request.dropOthers)
    {
        std::vector<uint32> others;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const other = bot->GetQuestSlotQuestId(slot);
            if (other && other != request.questId)
                others.push_back(other);
        }
        for (uint32 other : others)
        {
            Quest const* otherQuest = sObjectMgr->GetQuestTemplate(other);
            if (!otherQuest)
                continue;
            if (ledger)
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, other, "probe_setup");
            RemoveQuest(bot, otherQuest, false);
            facts.droppedOthers.push_back(other);
        }
    }

    if (ledger)
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, request.questId, "probe_setup");
    RemoveQuest(bot, quest, true);
    // Quest-class objective items left in the bags would pre-complete a fresh accept.
    for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
    {
        uint32 const itemId = quest->RequiredItemId[index];
        ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_QUEST)
            continue;
        uint32 const count = bot->GetItemCount(itemId, true);
        if (!count)
            continue;
        bot->DestroyItemCount(itemId, count, true, false);
        facts.questItemsDestroyed += count;
    }
    ai->lowPriorityQuest.erase(request.questId);

    // Drop stale travel/RPG state tied to the old position or the removed quests, then place.
    ai->Reset(true);
    if (!bot->TeleportTo(spawn.mapid, spawn.posX, spawn.posY, spawn.posZ, spawn.orientation))
        return FormatError(request.verb, request.guid, "probe_teleport_failed");
    facts.teleport = spawn.mapid == bot->GetMapId() ? "done" : "queued";
    return FormatPlace(facts);
}

std::string Status(WireRequest const& request)
{
    Player* bot = nullptr;
    PlayerbotAI* ai = nullptr;
    std::string error;
    if (!ResolveOnlineBot(request, bot, ai, error))
        return FormatError(request.verb, request.guid, error);

    StatusFacts facts;
    facts.guid = request.guid;
    facts.level = bot->GetLevel();
    facts.xp = bot->GetUInt32Value(PLAYER_XP);
    facts.alive = bot->IsAlive();
    facts.inCombat = bot->IsInCombat();
    facts.position = PositionOf(bot);
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot))
            ++facts.questLogCount;

    Quest const* quest = request.questId ? sObjectMgr->GetQuestTemplate(request.questId) : nullptr;
    if (request.questId && !quest)
        return FormatError(request.verb, request.guid, "probe_quest_unknown");
    if (quest)
    {
        facts.hasQuest = true;
        facts.quest.questId = request.questId;
        facts.quest.status = static_cast<uint32>(bot->GetQuestStatus(request.questId));
        facts.quest.rewarded = bot->IsQuestRewarded(request.questId);
        uint16 const slot = bot->FindQuestSlot(request.questId);
        facts.quest.slot = slot < MAX_QUEST_LOG_SIZE ? static_cast<int32>(slot) : -1;

        auto const statusIt = bot->getQuestStatusMap().find(request.questId);
        QuestStatusData const* data = statusIt != bot->getQuestStatusMap().end() ? &statusIt->second : nullptr;
        for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
        {
            int32 const npcOrGo = quest->RequiredNpcOrGo[index];
            if (!npcOrGo || !quest->RequiredNpcOrGoCount[index])
                continue;
            facts.quest.objectives.push_back({npcOrGo > 0 ? "creature" : "gameobject", index,
                                              static_cast<uint32>(npcOrGo > 0 ? npcOrGo : -npcOrGo),
                                              data ? data->CreatureOrGOCount[index] : 0u,
                                              quest->RequiredNpcOrGoCount[index]});
        }
        for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
        {
            if (!quest->RequiredItemId[index] || !quest->RequiredItemCount[index])
                continue;
            facts.quest.objectives.push_back({"item", index, quest->RequiredItemId[index],
                                              data ? data->ItemCount[index] : 0u,
                                              quest->RequiredItemCount[index]});
        }
    }
    return FormatStatus(facts);
}
}  // namespace

std::string Execute(WireRequest const& request)
{
    // The gate is evaluated before any world object is resolved; with AutoWow.Probe.Enable = 0
    // every verb is refused here.
    if (char const* refusal = GateGuid(
            sConfigMgr->GetOption<bool>(kEnableConfigKey, false),
            sConfigMgr->GetOption<std::string>(AutoWowFixture::kFixtureGuidsConfigKey, ""),
            sConfigMgr->GetOption<std::string>(kOracleGuidsConfigKey, ""), request.guid))
        return FormatError(request.verb, request.guid, refusal);

    switch (request.verb)
    {
        case Verb::Login:
            return Login(request);
        case Verb::SetLevel:
            return AutoWowFixture::SetLevel(request.guid, request.level,
                                            request.hasSpec ? static_cast<int32>(request.specIndex) : -1,
                                            request.quality);
        case Verb::Place:
            return Place(request);
        case Verb::Status:
            return Status(request);
    }
    return FormatError(request.verb, request.guid, "probe_unknown_verb");
}
}  // namespace AutoWowProbePlace
