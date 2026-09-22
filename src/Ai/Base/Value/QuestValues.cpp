/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestValues.h"

#include <algorithm>

#include "GameObject.h"
#include "ItemTemplate.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "QuestFinisherTransitionPolicy.h"
#include "QuestKillSourcePolicy.h"
#include "SharedValueContext.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

// What kind of a relation does this entry have with this quest.
entryQuestRelationMap EntryQuestRelationMapValue::Calculate()
{
    entryQuestRelationMap rMap;

    for (auto relation : *sObjectMgr->GetCreatureQuestRelationMap())
        rMap[relation.first][relation.second] |= (int)QuestRelationFlag::questGiver;

    for (auto relation : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        rMap[relation.first][relation.second] |= (int)QuestRelationFlag::questTaker;

    for (auto relation : *sObjectMgr->GetGOQuestRelationMap())
        rMap[-(int32)relation.first][relation.second] |= (int)QuestRelationFlag::questGiver;

    // Gameobject INVOLVED relations are quest finishers (takers), not givers. The creature path
    // above tags involved relations as questTaker; mirroring that here lets gameobject turn-ins be
    // resolved through the quest-taker map.
    for (auto relation : *sObjectMgr->GetGOQuestInvolvedRelationMap())
        rMap[-(int32)relation.first][relation.second] |= (int)QuestRelationFlag::questTaker;

    // Quest objectives
    ObjectMgr::QuestMap const& questMap = sObjectMgr->GetQuestTemplates();

    for (auto& questItr : questMap)
    {
        uint32 questId = questItr.first;
        Quest* quest = questItr.second;

        for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
        {
            uint32 relationFlag = 1 << objective;

            // Kill objective
            if (quest->RequiredNpcOrGo[objective])
                rMap[quest->RequiredNpcOrGo[objective]][questId] |= relationFlag;

            // Loot objective
            if (quest->RequiredItemId[objective])
            {
                for (auto& entry : GAI_VALUE2(std::vector<int32>, "item drop list", quest->RequiredItemId[objective]))
                    rMap[entry][questId] |= relationFlag;
            }
        }
    }

    return rMap;
}

// Get all the objective entries for a specific quest.
void FindQuestObjectData::GetObjectiveEntries()
{
    relationMap = GAI_VALUE(entryQuestRelationMap, "entry quest relation");
}

// Data worker. Checks for a specific creature what quest they are needed for and puts them in the proper place in the
// quest map.
void FindQuestObjectData::operator()(CreatureData const& creData)
{
    uint32 entry = creData.id;

    for (auto& relation : relationMap[entry])
    {
        uint32 questId = relation.first;
        uint32 flag = relation.second;
        data[questId][flag][entry].push_back(GuidPosition(creData));
    }
}

// GameObject data worker. Checks for a specific gameObject what quest they are needed for and puts them in the proper
// place in the quest map.
void FindQuestObjectData::operator()(GameObjectData const& goData)
{
    int32 entry = goData.id * -1;

    for (auto& relation : relationMap[entry])
    {
        uint32 questId = relation.first;
        uint32 flag = relation.second;
        data[questId][flag][entry].push_back(GuidPosition(goData));
    }
}

// Goes past all creatures and gameobjects and creatures the full quest guid map.
questGuidpMap QuestGuidpMapValue::Calculate()
{
    FindQuestObjectData worker;
    for (auto const& itr : sObjectMgr->GetAllCreatureData())
        worker(itr.second);
    for (auto const& itr : sObjectMgr->GetAllGOData())
        worker(itr.second);

    return worker.GetResult();
}

// Selects all questgivers for a specific level (range).
questGiverMap QuestGiversValue::Calculate()
{
    uint32 level = 0;
    std::string const q = getQualifier();
    bool hasQualifier = !q.empty();

    if (hasQualifier)
        level = stoi(q);

    questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();

    questGiverMap guidps;

    for (auto& qPair : questMap)
    {
        for (auto& relationBucket : qPair.second)
        {
            if (!HasQuestRelationFlag(relationBucket.first, QuestRelationFlag::questGiver))
                continue;

            for (auto& entry : relationBucket.second)
            {
                for (auto const& guidp : entry.second)
                {
                    uint32 questId = qPair.first;

                    if (hasQualifier)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

                        if (quest && (level < quest->GetMinLevel() || (int)level > quest->GetQuestLevel() + 10))
                            continue;
                    }

                    guidps[questId].push_back(guidp);
                }
            }
        }
    }

    return guidps;
}

std::vector<GuidPosition> ActiveQuestGiversValue::Calculate()
{
    questGiverMap qGivers = GAI_VALUE2(questGiverMap, "quest givers", bot->GetLevel());

    std::vector<GuidPosition> retQuestGivers;

    for (auto& qGiver : qGivers)
    {
        uint32 questId = qGiver.first;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            continue;
        }

        if (!bot->CanTakeQuest(quest, false))
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);

        if (status != QUEST_STATUS_NONE)
            continue;

        for (auto& guidp : qGiver.second)
        {
            CreatureTemplate const* creatureTemplate = guidp.GetCreatureTemplate();

            if (creatureTemplate)
            {
                if (bot->GetFactionReactionTo(bot->GetFactionTemplateEntry(),
                                              sFactionTemplateStore.LookupEntry(creatureTemplate->faction)) <
                    REP_FRIENDLY)
                    continue;
            }

            if (!guidp.IsCreatureOrGOAccessible())
                continue;

            retQuestGivers.push_back(guidp);
        }
    }

    return retQuestGivers;
}

std::vector<GuidPosition> ActiveQuestTakersValue::Calculate()
{
    questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();

    std::vector<GuidPosition> retQuestTakers;

    QuestStatusMap& questStatusMap = bot->getQuestStatusMap();

    for (auto& questStatus : questStatusMap)
    {
        uint32 questId = questStatus.first;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

        if (!quest)
        {
            continue;
        }

        QuestStatus status = questStatus.second.Status;
        if ((status != QUEST_STATUS_COMPLETE || bot->GetQuestRewardStatus(questId)) &&
            (!quest->IsAutoComplete() || !bot->CanTakeQuest(quest, false)))
            continue;

        auto q = questMap.find(questId);

        if (q == questMap.end())
            continue;

        for (auto& relationBucket : q->second)
        {
            if (!HasQuestRelationFlag(relationBucket.first, QuestRelationFlag::questTaker))
                continue;

            for (auto& entry : relationBucket.second)
            {
                if (entry.first > 0)
                {
                    if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry.first))
                    {
                        if (bot->GetFactionReactionTo(bot->GetFactionTemplateEntry(),
                                                      sFactionTemplateStore.LookupEntry(info->faction)) < REP_FRIENDLY)
                            continue;
                    }
                }

                for (GuidPosition guidp : entry.second) // copy: IsCreatureOrGOAccessible() is non-const
                {
                    if (!guidp.IsCreatureOrGOAccessible())
                        continue;

                    retQuestTakers.push_back(guidp);
                }
            }
        }
    }

    return retQuestTakers;
}

std::vector<GuidPosition> ActiveQuestObjectivesValue::Calculate()
{
    questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();

    std::vector<GuidPosition> retQuestObjectives;

    QuestStatusMap& questStatusMap = bot->getQuestStatusMap();

    for (auto& questStatus : questStatusMap)
    {
        uint32 questId = questStatus.first;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            continue;
        }

        QuestStatusData statusData = questStatus.second;
        if (statusData.Status != QUEST_STATUS_INCOMPLETE)
            continue;

        for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
        {
            if (quest->RequiredItemCount[objective])
            {
                uint32 reqCount = quest->RequiredItemCount[objective];
                uint32 hasCount = statusData.ItemCount[objective];

                if (!reqCount || hasCount >= reqCount)
                    continue;
            }

            if (quest->RequiredNpcOrGoCount[objective])
            {
                uint32 reqCount = quest->RequiredNpcOrGoCount[objective];
                uint32 hasCount = statusData.CreatureOrGOCount[objective];

                if (!reqCount || hasCount >= reqCount)
                    continue;
            }

            auto q = questMap.find(questId);

            if (q == questMap.end())
                continue;

            auto qt = q->second.find((int)QuestRelationFlag(1 << objective));

            if (qt == q->second.end())
                continue;

            for (auto& entry : qt->second)
            {
                for (GuidPosition guidp : entry.second) // copy: IsCreatureOrGOAccessible() is non-const
                {
                    if (!guidp.IsCreatureOrGOAccessible())
                        continue;

                    retQuestObjectives.push_back(guidp);
                }
            }
        }
    }

    return retQuestObjectives;
}

QuestObjectiveSpec ActiveQuestObjectiveValue::Calculate()
{
    // Default spec: hasLock() == false. Returned whenever the bot is not driving a quest through the
    // New RPG state, or has no supported incomplete objective left (e.g. quest is complete).
    QuestObjectiveSpec none;

    auto* dq = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!dq)
        return none;

    uint32 questId = dq->questId;
    if (!questId)
        return none;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return none;

    QuestStatusMap& questStatusMap = bot->getQuestStatusMap();
    auto itStatus = questStatusMap.find(questId);
    if (itStatus == questStatusMap.end())
        return none;

    QuestStatusData const& statusData = itStatus->second;

    questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();

    // Collect every stable spawn known for a (signed) source entry of this quest, scanning across all
    // relation-flag buckets so both creature-kill and item-drop sources resolve. Signed entry keys
    // follow the guidp map convention: >0 creature, <0 gameobject.
    auto resolveSpawns = [&](int32 signedEntry) -> std::vector<GuidPosition>
    {
        std::vector<GuidPosition> out;
        auto qit = questMap.find(questId);
        if (qit == questMap.end())
            return out;
        for (auto& flagBucket : qit->second)
        {
            auto eit = flagBucket.second.find(signedEntry);
            if (eit != flagBucket.second.end())
                out.insert(out.end(), eit->second.begin(), eit->second.end());
        }
        return out;
    };

    // Group/dungeon quests are Unsupported in Phase 1 (SuggestedPlayers >= 2 on a typed quest).
    bool const groupQuest = quest->GetType() != 0 && quest->GetSuggestedPlayers() >= 2;

    auto makeSpec = [&](QuestObjectiveFamily family, uint8 slot) -> QuestObjectiveSpec
    {
        QuestObjectiveSpec s;
        s.key.questId = questId;
        s.key.slot = slot;
        s.key.family = family;
        s.ownerGuid = bot->GetGUID();

        if (family == QuestObjectiveFamily::NpcOrGameObject)
        {
            int32 reqEntry = quest->RequiredNpcOrGo[slot];
            s.requiredNpcOrGoEntry = reqEntry;
            s.requiredCount = quest->RequiredNpcOrGoCount[slot];
            s.currentCount = statusData.CreatureOrGOCount[slot];

            if (reqEntry > 0)
            {
                if (groupQuest)
                {
                    s.kind = QuestObjectiveKind::Unsupported;
                    s.supported = false;
                }
                else if (UsesQuestSourceItemForCredit(
                             quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_CAST), quest->GetSrcItemId()))
                {
                    // CAST objectives advance by using the quest's source item on
                    // the exact creature entry. They are non-offensive: the
                    // dedicated phase sends the normal item-use opcode and waits
                    // for the authoritative CreatureOrGOCount delta.
                    s.kind = QuestObjectiveKind::UseQuestItem;
                    s.questItemId = quest->GetSrcItemId();
                    s.supported = !groupQuest && s.questItemId != 0;
                    if (s.supported)
                    {
                        QuestObjectiveSource src;
                        src.type = QuestObjectiveSource::Type::Creature;
                        src.entry = static_cast<uint32>(reqEntry);
                        src.spawns = resolveSpawns(reqEntry);
                        s.sources.push_back(std::move(src));
                    }
                }
                else
                {
                    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(reqEntry);
                    if (creature && QuestKillSourcePolicy::CanUseKillExecutor(creature->unit_flags))
                    {
                        s.kind = QuestObjectiveKind::CreatureCredit;
                        s.supported = true;
                        QuestObjectiveSource src;
                        src.type = QuestObjectiveSource::Type::Creature;
                        src.entry = static_cast<uint32>(reqEntry);
                        src.spawns = resolveSpawns(reqEntry);
                        s.sources.push_back(std::move(src));
                    }
                    else
                    {
                        // The native kill path cannot create spell/script credit for an
                        // unattackable target. Leave it to the normal unsupported-quest deferral.
                        s.kind = QuestObjectiveKind::Unsupported;
                        s.supported = false;
                    }
                }
            }
            else if (reqEntry < 0)
            {
                s.kind = QuestObjectiveKind::GameObjectCredit;
                s.supported = !groupQuest;
                if (s.supported)
                {
                    QuestObjectiveSource src;
                    src.type = QuestObjectiveSource::Type::GameObject;
                    src.entry = static_cast<uint32>(-reqEntry);
                    src.spawns = resolveSpawns(reqEntry);
                    s.sources.push_back(std::move(src));
                }
            }
            else
            {
                s.kind = QuestObjectiveKind::Unsupported;
                s.supported = false;
            }
        }
        else  // QuestObjectiveFamily::Item
        {
            uint32 itemId = quest->RequiredItemId[slot];
            s.requiredItemId = itemId;
            s.requiredCount = quest->RequiredItemCount[slot];
            s.currentCount = statusData.ItemCount[slot];

            if (itemId && !groupQuest)
            {
                s.kind = QuestObjectiveKind::CollectItem;
                s.supported = true;

                // item drop list: positive entry -> creature drop, negative -> gameobject drop.
                std::unordered_map<int32, QuestObjectiveSource> byEntry;
                for (int32 srcEntry : GAI_VALUE2(std::vector<int32>, "item drop list", itemId))
                {
                    QuestObjectiveSource& src = byEntry[srcEntry];
                    if (src.entry == 0 && src.spawns.empty())
                    {
                        src.type = srcEntry >= 0 ? QuestObjectiveSource::Type::Creature
                                                 : QuestObjectiveSource::Type::GameObject;
                        src.entry = (uint32)(srcEntry >= 0 ? srcEntry : -srcEntry);
                        src.spawns = resolveSpawns(srcEntry);
                    }
                }
                for (auto& kv : byEntry)
                    s.sources.push_back(std::move(kv.second));

                // A required item with no loot sources can instead be created by an ON_USE spell
                // on the quest's start item. Resolve this only from the item/spell/focus metadata:
                // no quest/item/focus/GO identity is special-cased, and every uncertain shape stays
                // unsupported. The core remains authoritative for actual spell-focus range and item
                // creation when the executor later submits the normal item-use packet.
                if (s.requiredCount > 0 && s.currentCount < s.requiredCount && s.sources.empty())
                {
                    // Empty CollectItem specs are not executable. A complete spell-focus chain
                    // below may opt this objective back in as UseQuestItem; otherwise fail closed.
                    s.kind = QuestObjectiveKind::Unsupported;
                    s.supported = false;

                    uint32 const questItemId = quest->GetSrcItemId();
                    ItemTemplate const* questItem = questItemId ? sObjectMgr->GetItemTemplate(questItemId) : nullptr;
                    std::vector<QuestItemUseSpellFact> spellFacts;

                    if (questItem)
                    {
                        for (auto const& itemSpell : questItem->Spells)
                        {
                            if (itemSpell.SpellId <= 0)
                                continue;

                            QuestItemUseSpellFact fact;
                            fact.spellId = static_cast<uint32>(itemSpell.SpellId);
                            fact.onUse = itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE;

                            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(fact.spellId))
                            {
                                fact.requiredSpellFocusId = spellInfo->RequiresSpellFocus;
                                for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                                {
                                    if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM ||
                                         effect.Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                                        effect.ItemType != 0)
                                        fact.createdItemIds.push_back(effect.ItemType);
                                }
                            }

                            spellFacts.push_back(std::move(fact));
                        }
                    }

                    QuestItemSpellFocusMatch const focusMatch =
                        ResolveQuestItemSpellFocus(s.requiredItemId, spellFacts);
                    if (focusMatch.isResolved())
                    {
                        std::unordered_map<uint32, QuestObjectiveSource> focusSources;
                        if (GameObjectTemplateContainer const* templates = sObjectMgr->GetGameObjectTemplates())
                        {
                            for (auto const& [entry, goTemplate] : *templates)
                            {
                                if (goTemplate.type != GAMEOBJECT_TYPE_SPELL_FOCUS ||
                                    goTemplate.spellFocus.focusId != focusMatch.spellFocusId)
                                    continue;

                                QuestObjectiveSource& source = focusSources[entry];
                                source.type = QuestObjectiveSource::Type::GameObject;
                                source.entry = entry;
                            }
                        }

                        for (auto const& goDataEntry : sObjectMgr->GetAllGOData())
                        {
                            GameObjectData const& goData = goDataEntry.second;
                            auto source = focusSources.find(goData.id);
                            if (source != focusSources.end())
                                source->second.spawns.emplace_back(goData);
                        }

                        for (auto& focusSource : focusSources)
                        {
                            QuestObjectiveSource& source = focusSource.second;
                            if (!source.spawns.empty())
                                s.sources.push_back(std::move(source));
                        }
                        std::sort(s.sources.begin(), s.sources.end(),
                                  [](QuestObjectiveSource const& left, QuestObjectiveSource const& right)
                                  { return left.entry < right.entry; });

                        if (!s.sources.empty())
                        {
                            s.kind = QuestObjectiveKind::UseQuestItem;
                            s.questItemId = questItemId;
                            s.supported = true;
                        }
                    }
                }
            }
            else
            {
                s.kind = QuestObjectiveKind::Unsupported;
                s.supported = false;
            }
        }

        return s;
    };

    auto isIncomplete = [](QuestObjectiveSpec const& s) -> bool
    { return s.requiredCount > 0 && s.currentCount < s.requiredCount; };

    // Build all specs in canonical template order: NPC/GO family slots first, then item family slots.
    std::vector<QuestObjectiveSpec> specs;
    for (uint8 slot = 0; slot < QUEST_OBJECTIVES_COUNT; ++slot)
        if (quest->RequiredNpcOrGo[slot] != 0)
            specs.push_back(makeSpec(QuestObjectiveFamily::NpcOrGameObject, slot));
    for (uint8 slot = 0; slot < QUEST_ITEM_OBJECTIVES_COUNT; ++slot)
        if (quest->RequiredItemId[slot] != 0)
            specs.push_back(makeSpec(QuestObjectiveFamily::Item, slot));

    // Script-owned exploration/event quests can have no ordinary counted objective at all. For the
    // conservative v0 support path, recognize only a scripted creature that starts the quest. The
    // native script remains authoritative for movement, combat, failure, and GroupEventHappens;
    // Playerbots merely follows and protects that exact creature. Area triggers and spell-complete
    // quests therefore remain unsupported instead of being misclassified as escorts.
    if (specs.empty() && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
    {
        QuestObjectiveSpec eventSpec;
        eventSpec.key.questId = questId;
        eventSpec.key.slot = 0;
        eventSpec.key.family = QuestObjectiveFamily::NpcOrGameObject;
        eventSpec.kind = QuestObjectiveKind::ScriptedEvent;
        eventSpec.ownerGuid = bot->GetGUID();
        eventSpec.currentCount = statusData.Explored ? 1 : 0;
        eventSpec.requiredCount = 1;

        auto qit = questMap.find(questId);
        if (qit != questMap.end())
        {
            std::unordered_map<uint32, QuestObjectiveSource> byEntry;
            for (auto const& relationBucket : qit->second)
            {
                if (!HasQuestRelationFlag(relationBucket.first, QuestRelationFlag::questGiver))
                    continue;

                for (auto const& entrySpawns : relationBucket.second)
                {
                    int32 const signedEntry = entrySpawns.first;
                    if (signedEntry <= 0)
                        continue;

                    CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(signedEntry);
                    if (!creatureTemplate || creatureTemplate->ScriptID == 0)
                        continue;

                    QuestObjectiveSource& source = byEntry[static_cast<uint32>(signedEntry)];
                    source.type = QuestObjectiveSource::Type::Creature;
                    source.entry = static_cast<uint32>(signedEntry);
                    source.spawns.insert(source.spawns.end(), entrySpawns.second.begin(), entrySpawns.second.end());
                }
            }

            for (auto& [entry, source] : byEntry)
                eventSpec.sources.push_back(std::move(source));
            std::sort(eventSpec.sources.begin(), eventSpec.sources.end(),
                      [](QuestObjectiveSource const& left, QuestObjectiveSource const& right)
                      { return left.entry < right.entry; });
        }

        eventSpec.supported = !groupQuest && !eventSpec.sources.empty();
        if (eventSpec.supported)
        {
            eventSpec.requiredNpcOrGoEntry = static_cast<int32>(eventSpec.sources.front().entry);
            specs.push_back(std::move(eventSpec));
        }
    }

    // Keep the objective the runtime already locked onto (by selected source entry) as long as it is
    // still a supported, still-incomplete objective — avoids thrashing between objectives mid-quest.
    int32 lockedEntry = dq->objectiveRuntime.selectedSourceEntry;
    if (lockedEntry != 0)
    {
        for (auto& s : specs)
        {
            if (!s.supported || !isIncomplete(s))
                continue;
            bool matches = lockedEntry > 0 ? s.acceptsCreatureEntry((uint32)lockedEntry)
                                           : s.acceptsGameObjectEntry((uint32)(-lockedEntry));
            if (matches)
                return s;
        }
    }

    // Otherwise pick the first supported, still-incomplete objective in template order.
    for (auto& s : specs)
        if (s.supported && isIncomplete(s))
            return s;

    return none;
}

QuestFinisherRef ActiveQuestFinisherValue::Calculate()
{
    QuestFinisherRef ref;  // signedEntry == 0 means "no finisher resolved".

    auto* dq = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!dq)
        return ref;

    uint32 questId = dq->questId;
    if (!questId)
        return ref;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return ref;

    // Only resolve a finisher once the quest is actually ready to hand in. AzerothCore can leave a
    // speak-to/turn-in quest at QUEST_STATUS_INCOMPLETE after all counted objectives are satisfied;
    // CanCompleteQuest() is the authoritative, read-only predicate for the normal client
    // CMSG_QUESTGIVER_REQUEST_REWARD transition. Requiring QUEST_STATUS_COMPLETE here strands those
    // quests before a concrete finisher can ever be published.
    QuestStatus status = bot->GetQuestStatus(questId);
    bool const ready = AutoWowQuestFinisher::IsFinisherReady({
                           status,
                           bot->GetQuestRewardStatus(questId),
                           status == QUEST_STATUS_INCOMPLETE && bot->CanCompleteQuest(questId),
                           true}) ||
                       (quest->IsAutoComplete() && bot->CanTakeQuest(quest, false));
    if (!ready)
        return ref;

    questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();
    auto q = questMap.find(questId);
    if (q == questMap.end())
        return ref;

    // GameObject INVOLVED relations are now correctly tagged questTaker (see EntryQuestRelationMapValue),
    // so both creature and gameobject turn-ins resolve through the questTaker bucket. Keep all
    // relation-bearing spawns long enough for the common selector to enforce exact relation and map.
    std::vector<QuestFinisherSpawnCandidate> candidates;
    std::vector<GuidPosition> stableSpawns;
    WorldPosition botPosition(bot);

    for (auto& relationBucket : q->second)
    {
        for (auto& entry : relationBucket.second)
        {
            int32 signedEntry = entry.first;

            // Faction gate for creature finishers, mirroring ActiveQuestTakersValue.
            if (signedEntry > 0)
            {
                if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(signedEntry))
                {
                    if (bot->GetFactionReactionTo(bot->GetFactionTemplateEntry(),
                                                  sFactionTemplateStore.LookupEntry(info->faction)) < REP_FRIENDLY)
                        continue;
                }
            }

            for (auto const& guidp : entry.second)
            {
                // Preserve signed identity all the way through selection: positive entries may only
                // bind creature spawns and negative entries may only bind gameobject spawns.
                if ((signedEntry > 0 && !guidp.IsCreature()) || (signedEntry < 0 && !guidp.IsGameObject()))
                    continue;

                QuestFinisherSpawnCandidate candidate;
                candidate.signedEntry = signedEntry;
                candidate.relationMask = relationBucket.first;
                candidate.mapId = guidp.GetMapId();
                candidate.spawnIdentity = guidp.GetCounter();

                if (candidate.mapId == bot->GetMapId())
                {
                    WorldPosition spawnPosition(guidp);
                    candidate.directDistance = botPosition.distance(spawnPosition);

                    // Prefer actual navigable path length when the bot-aware pathfinder can safely
                    // produce a complete same-map route. A failed probe retains deterministic direct
                    // distance as a conservative fallback; it never admits a remote-map spawn.
                    PathGenerator path(bot);
                    path.CalculatePath(spawnPosition.GetPositionX(), spawnPosition.GetPositionY(),
                                       spawnPosition.GetPositionZ());
                    uint32 const pathType = static_cast<uint32>(path.GetPathType());
                    uint32 const rejectedPathTypes = PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH |
                                                     PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY;
                    if ((pathType & PATHFIND_NORMAL) && !(pathType & rejectedPathTypes))
                        candidate.routeDistance = path.getPathLength();
                }

                candidates.push_back(candidate);
                stableSpawns.push_back(guidp);
            }
        }
    }

    std::size_t const selected = SelectQuestFinisherSpawnCandidate(candidates, bot->GetMapId());
    if (selected == NoQuestFinisherSpawnSelection)
        return ref;  // Fail closed: campaign travel must explicitly support remote-map finishers first.

    ref.signedEntry = candidates[selected].signedEntry;
    ref.stableSpawn = stableSpawns[selected];
    return ref;
}

uint8 FreeQuestLogSlotValue::Calculate()
{
    uint8 numQuest = 0;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);

        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        numQuest++;
    }

    return MAX_QUEST_LOG_SIZE - numQuest;
}

uint32 DialogStatusValue::getDialogStatus(Player* bot, int32 questgiver, uint32 questId)
{
    uint32 dialogStatus = DIALOG_STATUS_NONE;

    QuestRelationBounds rbounds;   // QuestRelations (quest-giver)
    QuestRelationBounds irbounds;  // InvolvedRelations (quest-finisher)

    if (questgiver > 0)
    {
        rbounds = sObjectMgr->GetCreatureQuestRelationBounds(questgiver);
        irbounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(questgiver);
    }
    else
    {
        rbounds = sObjectMgr->GetGOQuestRelationBounds(questgiver * -1);
        irbounds = sObjectMgr->GetGOQuestInvolvedRelationBounds(questgiver * -1);
    }

    // Check markings for quest-finisher
    for (QuestRelations::const_iterator itr = irbounds.first; itr != irbounds.second; ++itr)
    {
        if (questId && itr->second != questId)
            continue;

        Quest const* pQuest = sObjectMgr->GetQuestTemplate(itr->second);
        if (!pQuest)
        {
            continue;
        }

        uint32 dialogStatusNew = DIALOG_STATUS_NONE;

        QuestStatus status = bot->GetQuestStatus(itr->second);

        if ((status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(itr->second)) ||
            (pQuest->IsAutoComplete() && bot->CanTakeQuest(pQuest, false)))
        {
            if (pQuest->IsAutoComplete() && pQuest->IsRepeatable())
            {
                dialogStatusNew = DIALOG_STATUS_REWARD_REP;
            }
            else
            {
                dialogStatusNew = DIALOG_STATUS_REWARD2;
            }
        }
        else if (status == QUEST_STATUS_INCOMPLETE)
        {
            dialogStatusNew = DIALOG_STATUS_INCOMPLETE;
        }

        if (dialogStatusNew > dialogStatus)
        {
            dialogStatus = dialogStatusNew;
        }
    }

    // check markings for quest-giver
    for (QuestRelations::const_iterator itr = rbounds.first; itr != rbounds.second; ++itr)
    {
        if (questId && itr->second != questId)
            continue;

        Quest const* pQuest = sObjectMgr->GetQuestTemplate(itr->second);
        if (!pQuest)
        {
            continue;
        }

        uint32 dialogStatusNew = DIALOG_STATUS_NONE;

        QuestStatus status = bot->GetQuestStatus(itr->second);

        if (status == QUEST_STATUS_NONE)  // For all other cases the mark is handled either at some place else, or with
                                          // involved-relations already
        {
            if (bot->CanSeeStartQuest(pQuest))
            {
                if (bot->SatisfyQuestLevel(pQuest, false))
                {
                    int32 lowLevelDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
                    if (pQuest->IsAutoComplete() ||
                        (pQuest->IsRepeatable() &&
                         bot->getQuestStatusMap()[itr->second].Status == QUEST_STATUS_REWARDED))
                    {
                        dialogStatusNew = DIALOG_STATUS_REWARD_REP;
                    }
                    else if (lowLevelDiff < 0 || bot->GetLevel() <= bot->GetQuestLevel(pQuest) + uint32(lowLevelDiff))
                    {
                        dialogStatusNew = DIALOG_STATUS_AVAILABLE;
                    }
                    else
                    {
                        dialogStatusNew = DIALOG_STATUS_LOW_LEVEL_AVAILABLE;
                    }
                }
                else
                {
                    dialogStatusNew = DIALOG_STATUS_UNAVAILABLE;
                }
            }
        }

        if (dialogStatusNew > dialogStatus)
        {
            dialogStatus = dialogStatusNew;
        }
    }

    return dialogStatus;
}

uint32 DialogStatusValue::Calculate() { return getDialogStatus(bot, stoi(getQualifier())); }

uint32 DialogStatusQuestValue::Calculate()
{
    return getDialogStatus(bot, getMultiQualifier(getQualifier(), 0), getMultiQualifier(getQualifier(), 1));
}

bool CanAcceptQuestValue::Calculate()
{
    return AI_VALUE2(uint32, "dialog status", getQualifier()) == DIALOG_STATUS_AVAILABLE;
};

bool CanAcceptQuestLowLevelValue::Calculate()
{
    uint32 dialogStatus = AI_VALUE2(uint32, "dialog status", getQualifier());
    return dialogStatus == DIALOG_STATUS_LOW_LEVEL_AVAILABLE;
};

bool CanTurnInQuestValue::Calculate()
{
    uint32 dialogStatus = AI_VALUE2(uint32, "dialog status", getQualifier());
    return dialogStatus == DIALOG_STATUS_REWARD2 || dialogStatus == DIALOG_STATUS_REWARD ||
           dialogStatus == DIALOG_STATUS_REWARD_REP;
};
