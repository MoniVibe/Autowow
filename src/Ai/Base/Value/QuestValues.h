/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTVALUES_H
#define PLAYERBOTS_QUESTVALUES_H

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "NamedObjectContext.h"
#include "QuestObjectiveContext.h"
#include "TravelMgr.h"
#include "Value.h"

class Player;
class PlayerbotAI;

struct CreatureData;
struct GameObjectData;

enum class QuestRelationFlag : uint32
{
    none = 0,
    objective1 = 1,
    objective2 = 2,
    objective3 = 4,
    objective4 = 8,
    questGiver = 16,
    questTaker = 32,
    maxQuestRelationFlag = 64
};

// Relation-map keys are bitmasks, not mutually exclusive enum values. A single entry commonly has
// multiple roles for one quest (for example Gornek both starts and finishes q789), so consumers must
// test the requested bit instead of looking up an exact numeric key.
constexpr bool HasQuestRelationFlag(uint32 relationMask, QuestRelationFlag flag)
{
    return (relationMask & static_cast<uint32>(flag)) != 0;
}

//                     questId, QuestRelationFlag
typedef std::unordered_map<uint32, uint32> questRelationMap;
//                     entry
typedef std::unordered_map<int32, questRelationMap> entryQuestRelationMap;

//                      entry
typedef std::unordered_map<int32, std::vector<GuidPosition>> questEntryGuidps;

//                      QuestRelationFlag
typedef std::unordered_map<uint32, questEntryGuidps> questRelationGuidps;

//                      questId
typedef std::unordered_map<uint32, questRelationGuidps> questGuidpMap;

//                      questId
typedef std::unordered_map<uint32, std::vector<GuidPosition>> questGiverMap;

// Pure ordering facts for a stable quest-finisher spawn. Production fills these from the exact
// quest relation and world/path data; focused tests exercise the same deterministic selector.
struct QuestFinisherSpawnCandidate
{
    int32 signedEntry = 0;
    uint32 relationMask = 0;
    uint32 mapId = MAPID_INVALID;
    uint64 spawnIdentity = 0;
    float directDistance = std::numeric_limits<float>::infinity();
    float routeDistance = std::numeric_limits<float>::infinity();
};

constexpr std::size_t NoQuestFinisherSpawnSelection = std::numeric_limits<std::size_t>::max();

inline bool IsUsableQuestFinisherDistance(float distance)
{
    return std::isfinite(distance) && distance >= 0.0f;
}

inline std::size_t SelectQuestFinisherSpawnCandidate(std::vector<QuestFinisherSpawnCandidate> const& candidates,
                                                     uint32 botMapId)
{
    std::size_t selected = NoQuestFinisherSpawnSelection;

    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        QuestFinisherSpawnCandidate const& candidate = candidates[index];
        if (candidate.signedEntry == 0 || candidate.mapId != botMapId ||
            !HasQuestRelationFlag(candidate.relationMask, QuestRelationFlag::questTaker))
            continue;

        bool const hasRoute = IsUsableQuestFinisherDistance(candidate.routeDistance);
        bool const hasDirectDistance = IsUsableQuestFinisherDistance(candidate.directDistance);
        if (!hasRoute && !hasDirectDistance)
            continue;

        if (selected == NoQuestFinisherSpawnSelection)
        {
            selected = index;
            continue;
        }

        QuestFinisherSpawnCandidate const& current = candidates[selected];
        bool const currentHasRoute = IsUsableQuestFinisherDistance(current.routeDistance);
        float const candidateDistance = hasRoute ? candidate.routeDistance : candidate.directDistance;
        float const currentDistance = currentHasRoute ? current.routeDistance : current.directDistance;

        // A confirmed route is safer than a straight-line fallback. Within the same distance class,
        // rank by distance, then stable spawn identity and signed entry so input order never matters.
        bool const better = hasRoute != currentHasRoute
                                ? hasRoute
                                : candidateDistance != currentDistance
                                      ? candidateDistance < currentDistance
                                      : candidate.spawnIdentity != current.spawnIdentity
                                            ? candidate.spawnIdentity < current.spawnIdentity
                                            : candidate.signedEntry < current.signedEntry;
        if (better)
            selected = index;
    }

    return selected;
}

// Pure spell-contract facts used to recognize a required-item objective that is completed by using
// the quest's start item near a spell focus. Production translates ItemTemplate / SpellInfo data
// into these facts; focused tests exercise the ambiguity and fail-closed rules without a world DB.
struct QuestItemUseSpellFact
{
    uint32 spellId = 0;
    bool onUse = false;
    uint32 requiredSpellFocusId = 0;
    std::vector<uint32> createdItemIds;
};

struct QuestItemSpellFocusMatch
{
    uint32 spellId = 0;
    uint32 spellFocusId = 0;

    [[nodiscard]] bool isResolved() const { return spellId != 0 && spellFocusId != 0; }
};

// Exactly one ON_USE spell must both require a focus and create the objective's required item.
// Multiple matches are intentionally rejected even when they name the same focus: choosing which
// item spell to execute would otherwise depend on item-template slot ordering.
inline QuestItemSpellFocusMatch ResolveQuestItemSpellFocus(
    uint32 requiredItemId, std::vector<QuestItemUseSpellFact> const& spells)
{
    QuestItemSpellFocusMatch match;
    if (requiredItemId == 0)
        return match;

    for (QuestItemUseSpellFact const& spell : spells)
    {
        if (!spell.onUse || spell.spellId == 0 || spell.requiredSpellFocusId == 0)
            continue;

        bool createsRequiredItem = false;
        for (uint32 createdItemId : spell.createdItemIds)
        {
            if (createdItemId == requiredItemId)
            {
                createsRequiredItem = true;
                break;
            }
        }
        if (!createsRequiredItem)
            continue;

        if (match.isResolved())
            return QuestItemSpellFocusMatch{};

        match.spellId = spell.spellId;
        match.spellFocusId = spell.requiredSpellFocusId;
    }

    return match;
}

// Runtime source entries use the quest-map signed convention: creatures are positive and
// gameobjects are negative. Reject values that cannot be represented instead of wrapping them.
constexpr int32 SignedGameObjectObjectiveEntry(uint32 gameObjectEntry)
{
    return gameObjectEntry != 0 && gameObjectEntry <= static_cast<uint32>(std::numeric_limits<int32>::max())
               ? -static_cast<int32>(gameObjectEntry)
               : 0;
}

// Returns the quest relation Flags for all entries and quests
class EntryQuestRelationMapValue : public SingleCalculatedValue<entryQuestRelationMap>
{
public:
    EntryQuestRelationMapValue(PlayerbotAI* botAI) : SingleCalculatedValue(botAI, "entry quest relation map") {}

    entryQuestRelationMap Calculate() override;
};

// Generic quest object finder
class FindQuestObjectData
{
public:
    FindQuestObjectData() { GetObjectiveEntries(); }

    void GetObjectiveEntries();
    void operator()(CreatureData const& creatureData);
    void operator()(GameObjectData const& gameobjectData);
    questGuidpMap GetResult() const { return data; };

private:
    std::unordered_map<int32, std::vector<std::pair<uint32, QuestRelationFlag>>> entryMap;
    std::unordered_map<uint32, std::vector<std::pair<uint32, QuestRelationFlag>>> itemMap;

    entryQuestRelationMap relationMap;

    questGuidpMap data;
};

// All objects to start, do or finish a quest.
class QuestGuidpMapValue : public SingleCalculatedValue<questGuidpMap>
{
public:
    QuestGuidpMapValue(PlayerbotAI* botAI) : SingleCalculatedValue(botAI, "quest guidp map") {}

    questGuidpMap Calculate() override;
};

// All questgivers and their quests that are Useful for a specific level
class QuestGiversValue : public SingleCalculatedValue<questGiverMap>, public Qualified
{
public:
    QuestGiversValue(PlayerbotAI* botAI) : SingleCalculatedValue(botAI, "quest givers") {}

    questGiverMap Calculate() override;
};

// All questgivers that have a quest for the bot.
class ActiveQuestGiversValue : public CalculatedValue<std::vector<GuidPosition>>
{
public:
    ActiveQuestGiversValue(PlayerbotAI* botAI) : CalculatedValue(botAI, "active quest givers", 5) {}

    std::vector<GuidPosition> Calculate() override;
};

// All quest takers that the bot has a quest for.
class ActiveQuestTakersValue : public CalculatedValue<std::vector<GuidPosition>>
{
public:
    ActiveQuestTakersValue(PlayerbotAI* botAI) : CalculatedValue(botAI, "active quest takers", 5) {}

    std::vector<GuidPosition> Calculate() override;
};

// All objectives that the bot still has to complete.
class ActiveQuestObjectivesValue : public CalculatedValue<std::vector<GuidPosition>>
{
public:
    ActiveQuestObjectivesValue(PlayerbotAI* botAI) : CalculatedValue(botAI, "active quest objectives", 5) {}

    std::vector<GuidPosition> Calculate() override;
};

// Phase 1 objective-lock authority: the single objective the bot is currently working on for the
// quest held by NewRpgInfo::DoQuest. This is a PER-BOT value: retrieve via AI_VALUE(QuestObjectiveSpec,
// "active quest objective"). A returned spec drives strict targeting/loot only when spec.hasLock() is true; an
// empty (default) spec means "no supported incomplete objective / not doing a quest".
class ActiveQuestObjectiveValue : public CalculatedValue<QuestObjectiveSpec>
{
public:
    ActiveQuestObjectiveValue(PlayerbotAI* botAI) : CalculatedValue(botAI, "active quest objective", 1) {}

    QuestObjectiveSpec Calculate() override;
};

// Phase 1 turn-in authority: the exact quest-taker (finisher) creature/GO for the quest held by
// NewRpgInfo::DoQuest, resolved only once the quest is complete and unrewarded. This is a PER-BOT
// value: retrieve via AI_VALUE(QuestFinisherRef, "active quest finisher"). signedEntry == 0 means "no finisher".
class ActiveQuestFinisherValue : public CalculatedValue<QuestFinisherRef>
{
public:
    ActiveQuestFinisherValue(PlayerbotAI* botAI) : CalculatedValue(botAI, "active quest finisher", 1) {}

    QuestFinisherRef Calculate() override;
};

// Free quest log slots
class FreeQuestLogSlotValue : public Uint8CalculatedValue
{
public:
    FreeQuestLogSlotValue(PlayerbotAI* botAI) : Uint8CalculatedValue(botAI, "free quest log slots", 2) {}

    uint8 Calculate() override;
};

// Dialog status npc
class DialogStatusValue : public Uint32CalculatedValue, public Qualified
{
public:
    DialogStatusValue(PlayerbotAI* botAI, std::string const name = "dialog status")
        : Uint32CalculatedValue(botAI, name, 2)
    {
    }

    static uint32 getDialogStatus(Player* bot, int32 questgiver, uint32 questId = 0);

    uint32 Calculate() override;
};

// Dialog status npc quest
class DialogStatusQuestValue : public DialogStatusValue
{
public:
    DialogStatusQuestValue(PlayerbotAI* botAI) : DialogStatusValue(botAI, "dialog status quest") {}

    uint32 Calculate() override;
};

// Can accept quest from npc
class CanAcceptQuestValue : public BoolCalculatedValue, public Qualified
{
public:
    CanAcceptQuestValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "can accept quest npc") {}

    bool Calculate() override;
};

// Can accept low level quest from npc
class CanAcceptQuestLowLevelValue : public BoolCalculatedValue, public Qualified
{
public:
    CanAcceptQuestLowLevelValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "can accept quest low level npc") {}

    bool Calculate() override;
};

// Can hand in quest to npc
class CanTurnInQuestValue : public BoolCalculatedValue, public Qualified
{
public:
    CanTurnInQuestValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "can turn in quest npc") {}

    bool Calculate() override;
};

#endif
