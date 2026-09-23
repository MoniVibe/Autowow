/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestLogView.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <variant>

#include "AiObjectContext.h"
#include "Creature.h"
#include "NewRpgInfo.h"
#include "LootObjectStack.h"
#include "LastMovementValue.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OracleQuestExecutor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "AutoWowAcceptance.h"
#include "QuestGiverTravelFeedback.h"
#include "QuestTravelWalk.h"
#include "QuestObjectiveContext.h"
#include "Object.h"
#include "Timer.h"

namespace
{
std::string JsonString(std::string const& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

void AppendJsonNumber(std::ostringstream& out, float value)
{
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}

// Stable string maps for the Phase 1 objective-lock enums (QuestObjectiveContext.h). These are the
// wire names the external Quest Director consumes; keep them in lockstep with the enum definitions.
char const* FamilyName(QuestObjectiveFamily family)
{
    switch (family)
    {
        case QuestObjectiveFamily::NpcOrGameObject: return "npc_or_gameobject";
        case QuestObjectiveFamily::Item: return "item";
        default: return "unknown";
    }
}

char const* KindName(QuestObjectiveKind kind)
{
    switch (kind)
    {
        case QuestObjectiveKind::CreatureCredit: return "creature_credit";
        case QuestObjectiveKind::GameObjectCredit: return "gameobject_credit";
        case QuestObjectiveKind::CollectItem: return "collect_item";
        case QuestObjectiveKind::UseQuestItem: return "use_quest_item";
        case QuestObjectiveKind::ScriptedEvent: return "scripted_event";
        case QuestObjectiveKind::Gathering: return "gathering";
        case QuestObjectiveKind::Unsupported: return "unsupported";
        default: return "unknown";
    }
}

char const* MovementPriorityName(MovementPriority priority)
{
    switch (priority)
    {
        case MovementPriority::MOVEMENT_IDLE: return "idle";
        case MovementPriority::MOVEMENT_WANDER: return "wander";
        case MovementPriority::MOVEMENT_NORMAL: return "normal";
        case MovementPriority::MOVEMENT_COMBAT: return "combat";
        case MovementPriority::MOVEMENT_FORCED: return "forced";
        default: return "unknown";
    }
}

char const* PhaseName(QuestActionPhase phase)
{
    switch (phase)
    {
        case QuestActionPhase::ResolveObjective: return "resolve_objective";
        case QuestActionPhase::TravelToSource: return "travel_to_source";
        case QuestActionPhase::AcquireTarget: return "acquire_target";
        case QuestActionPhase::SelfDefense: return "self_defense";
        case QuestActionPhase::EngageTarget: return "engage_target";
        case QuestActionPhase::InteractSource: return "interact_source";
        case QuestActionPhase::UseQuestItem: return "use_quest_item";
        case QuestActionPhase::EscortEvent: return "escort_event";
        case QuestActionPhase::LootSource: return "loot_source";
        case QuestActionPhase::VerifyProgress: return "verify_progress";
        case QuestActionPhase::WaitForRespawn: return "wait_for_respawn";
        case QuestActionPhase::ResolveFinisher: return "resolve_finisher";
        case QuestActionPhase::TravelToFinisher: return "travel_to_finisher";
        case QuestActionPhase::InteractFinisher: return "interact_finisher";
        case QuestActionPhase::VerifyReward: return "verify_reward";
        case QuestActionPhase::Complete: return "complete";
        case QuestActionPhase::Blocked: return "blocked";
        default: return "unknown";
    }
}

char const* FailureName(QuestFailureReason failure)
{
    switch (failure)
    {
        case QuestFailureReason::None: return "none";
        case QuestFailureReason::QuestMissing: return "quest_missing";
        case QuestFailureReason::QuestNotActive: return "quest_not_active";
        case QuestFailureReason::UnsupportedObjective: return "unsupported_objective";
        case QuestFailureReason::NoItemSource: return "no_item_source";
        case QuestFailureReason::NoSourceSpawn: return "no_source_spawn";
        case QuestFailureReason::NoLiveCandidate: return "no_live_candidate";
        case QuestFailureReason::ObjectiveSourcesExhausted: return "objective_sources_exhausted";
        case QuestFailureReason::EventActorDeadOrUnavailable: return "event_actor_dead_or_unavailable";
        case QuestFailureReason::SourcesRespawning: return "sources_respawning";
        case QuestFailureReason::SourceNotPathable: return "source_not_pathable";
        case QuestFailureReason::LootRightsDenied: return "loot_rights_denied";
        case QuestFailureReason::InventoryFull: return "inventory_full";
        case QuestFailureReason::ProgressDidNotChange: return "progress_did_not_change";
        case QuestFailureReason::NoFinisherRelation: return "no_finisher_relation";
        case QuestFailureReason::NoFinisherSpawn: return "no_finisher_spawn";
        case QuestFailureReason::FinisherNotLoaded: return "finisher_not_loaded";
        case QuestFailureReason::FinisherDeadOrUnavailable: return "finisher_dead_or_unavailable";
        case QuestFailureReason::CrossMapRouteUnavailable: return "cross_map_route_unavailable";
        case QuestFailureReason::MovementStuckNoTeleport: return "movement_stuck_no_teleport";
        case QuestFailureReason::InteractionRejected: return "interaction_rejected";
        case QuestFailureReason::CanRewardFalse: return "can_reward_false";
        case QuestFailureReason::RewardNotConfirmed: return "reward_not_confirmed";
        case QuestFailureReason::OracleFinisherLeaseExpired: return "oracle_finisher_lease_expired";
        case QuestFailureReason::OracleFinisherMismatch: return "oracle_finisher_mismatch";
        case QuestFailureReason::TravelNoProgress: return "travel_no_progress";
        case QuestFailureReason::IntentReplanExhausted: return "intent_replan_exhausted";
        case QuestFailureReason::IntentNoProgress: return "intent_no_progress";
        default: return "unknown";
    }
}

struct FinisherLiveView
{
    bool loaded = false;
    uint32 map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distance = -1.0f;
};

FinisherLiveView ReadFinisherLive(Player* bot, QuestObjectiveRuntime const& runtime)
{
    FinisherLiveView view;
    if (!bot || runtime.finisher.runtimeGuid.IsEmpty())
        return view;

    WorldObject* finisher = ObjectAccessor::GetWorldObject(*bot, runtime.finisher.runtimeGuid);
    if (!finisher || !finisher->IsInWorld())
        return view;

    view.loaded = true;
    view.map = finisher->GetMapId();
    view.x = finisher->GetPositionX();
    view.y = finisher->GetPositionY();
    view.z = finisher->GetPositionZ();
    view.distance = bot->GetDistance(finisher);
    return view;
}

bool IsFinisherPhase(QuestActionPhase phase)
{
    switch (phase)
    {
        case QuestActionPhase::ResolveFinisher:
        case QuestActionPhase::TravelToFinisher:
        case QuestActionPhase::InteractFinisher:
        case QuestActionPhase::VerifyReward:
        case QuestActionPhase::Complete:
            return true;
        default:
            return false;
    }
}

bool HasFinisherIdentity(QuestObjectiveRuntime const& runtime)
{
    return runtime.finisherReceipt.questId != 0 || runtime.finisher.signedEntry != 0 ||
           runtime.finisher.stableSpawn.GetRawValue() != 0 || !runtime.finisher.runtimeGuid.IsEmpty();
}

bool IsFinisherMode(QuestObjectiveRuntime const& runtime, bool directiveQuestComplete)
{
    return IsFinisherPhase(runtime.phase) || HasFinisherIdentity(runtime) || directiveQuestComplete;
}

QuestActionPhase EffectiveFinisherPhase(QuestObjectiveRuntime const& runtime, bool finisherMode)
{
    // A completed directive may be observed during the handoff tick before DoCompletedQuest has
    // entered ResolveFinisher. Expose the semantic phase without rewriting the objective phase.
    if (finisherMode && runtime.phase == QuestActionPhase::ResolveObjective)
        return QuestActionPhase::ResolveFinisher;
    return runtime.phase;
}

char const* FinisherStatusName(bool finisherMode, QuestActionPhase phase, bool rewardConfirmed,
                               bool rewardStatus)
{
    if (!finisherMode)
        return "inactive";
    if (rewardConfirmed || rewardStatus || phase == QuestActionPhase::Complete)
        return "rewarded";
    if (phase == QuestActionPhase::Blocked)
        return "blocked";
    return "active";
}

char const* FinisherQuestIdSource(bool finisherMode, uint32 receiptQuestId, uint32 directiveQuestId)
{
    if (!finisherMode)
        return "none";
    if (receiptQuestId != 0)
        return "finisher_receipt";
    if (directiveQuestId != 0)
        return "directive";
    return "unavailable";
}

char const* FinisherRouteStatus(bool finisherMode, QuestActionPhase phase, bool hasStableSpawn,
                                bool liveLoaded)
{
    if (!finisherMode)
        return "inactive";
    if (phase == QuestActionPhase::Blocked)
        return "blocked";
    if (!hasStableSpawn)
        return "unresolved";
    if (liveLoaded)
        return "live_target_loaded";
    return "stable_target_pending";
}

char const* FinisherBindStatus(bool finisherMode, QuestObjectiveRuntime const& runtime,
                               FinisherLiveView const& live, QuestFinisherReceipt const& receipt)
{
    if (!finisherMode)
        return "inactive";
    if (receipt.exactRelationVerified && receipt.atInteractionRange)
        return "verified";
    if (receipt.exactRelationVerified)
        return "relation_verified";
    if (!runtime.finisher.runtimeGuid.IsEmpty() && live.loaded)
        return "runtime_bound";
    if (!runtime.finisher.runtimeGuid.IsEmpty())
        return "runtime_guid_unresolved";
    return "unbound";
}

char const* ObjectiveInactiveReason(QuestObjectiveSpec const& spec, bool objectiveActive,
                                    bool finisherMode)
{
    if (objectiveActive)
        return "none";
    if (finisherMode)
        return "finisher_mode_no_active_objective";
    if (spec.key.questId == 0)
        return "no_active_objective";
    if (spec.requiredCount != 0 && spec.currentCount >= spec.requiredCount)
        return "objective_complete";
    if (!spec.supported)
        return "objective_unsupported";
    return "objective_not_locked";
}

std::string FinisherTelemetryJson(bool finisherMode, uint32 finisherQuestId,
                                  char const* questIdSource, QuestObjectiveRuntime const& runtime,
                                  FinisherLiveView const& live,
                                  WorldPosition const& routeTarget, QuestFinisherReceipt const& receipt,
                                  uint32 questStatus, bool canReward, bool rewardStatus,
                                  uint32 selectedRewardIndex, bool selectedRewardIndexKnown)
{
    bool const hasStableSpawn = runtime.finisher.stableSpawn.GetRawValue() != 0;
    bool const runtimeBound = !runtime.finisher.runtimeGuid.IsEmpty();
    bool const routeTargetAvailable = routeTarget != WorldPosition();
    QuestActionPhase const effectivePhase = EffectiveFinisherPhase(runtime, finisherMode);

    std::ostringstream out;
    out << "{\"active\":" << (finisherMode ? "true" : "false")
        << ",\"status\":" << JsonString(FinisherStatusName(
               finisherMode, effectivePhase, receipt.rewardConfirmed, rewardStatus))
        << ",\"phase\":" << JsonString(finisherMode ? PhaseName(effectivePhase) : "inactive")
        << ",\"quest_id\":" << finisherQuestId
        << ",\"quest_id_source\":" << JsonString(questIdSource)
        << ",\"signed_entry\":" << runtime.finisher.signedEntry
        << ",\"entry\":" << runtime.finisher.signedEntry
        << ",\"runtime_guid\":" << runtime.finisher.runtimeGuid.GetCounter()
        << ",\"guid\":" << runtime.finisher.runtimeGuid.GetCounter()
        << ",\"stable_spawn_guid\":" << runtime.finisher.stableSpawn.GetCounter()
        << ",\"route\":{\"status\":" << JsonString(FinisherRouteStatus(
               finisherMode, effectivePhase, hasStableSpawn, live.loaded))
        << ",\"target_available\":" << (routeTargetAvailable ? "true" : "false")
        << ",\"target\":{\"map\":" << routeTarget.GetMapId()
        << ",\"x\":" << routeTarget.GetPositionX()
        << ",\"y\":" << routeTarget.GetPositionY()
        << ",\"z\":" << routeTarget.GetPositionZ() << "}"
        << ",\"live_loaded\":" << (live.loaded ? "true" : "false")
        << ",\"live_distance\":" << live.distance
        << ",\"stable_identity\":" << ((finisherQuestId != 0 && hasStableSpawn) ? "true" : "false") << "}"
        << ",\"bind\":{\"status\":" << JsonString(FinisherBindStatus(
               finisherMode, runtime, live, receipt))
        << ",\"runtime_bound\":" << (runtimeBound ? "true" : "false")
        << ",\"live_loaded\":" << (live.loaded ? "true" : "false")
        << ",\"at_interaction_range\":" << (receipt.atInteractionRange ? "true" : "false")
        << ",\"exact_relation_verified\":" << (receipt.exactRelationVerified ? "true" : "false") << "}"
        << ",\"reward\":{\"quest_status\":" << questStatus
        << ",\"can_reward\":" << (canReward ? "true" : "false")
        << ",\"reward_status\":" << (rewardStatus ? "true" : "false")
        << ",\"selected_reward_index_known\":" << (selectedRewardIndexKnown ? "true" : "false")
        << ",\"selected_reward_index\":" << selectedRewardIndex
        << ",\"sequence\":" << receipt.sequence
        << ",\"completion_request_sent\":" << (receipt.completionRequestSent ? "true" : "false")
        << ",\"core_completed\":" << (receipt.coreCompleted ? "true" : "false")
        << ",\"reward_confirmed\":" << (receipt.rewardConfirmed ? "true" : "false") << "}}";
    return out.str();
}

std::string FinisherReceiptJson(QuestFinisherReceipt const& receipt)
{
    std::ostringstream out;
    out << "{\"sequence\":" << receipt.sequence
        << ",\"quest_id\":" << receipt.questId
        << ",\"signed_entry\":" << receipt.signedEntry
        << ",\"stable_spawn_guid\":" << receipt.stableSpawnGuid.GetRawValue()
        << ",\"runtime_guid\":" << receipt.runtimeGuid.GetRawValue()
        << ",\"exact_relation_verified\":" << (receipt.exactRelationVerified ? "true" : "false")
        << ",\"at_interaction_range\":" << (receipt.atInteractionRange ? "true" : "false")
        << ",\"completion_request_sent\":" << (receipt.completionRequestSent ? "true" : "false")
        << ",\"core_completed\":" << (receipt.coreCompleted ? "true" : "false")
        << ",\"reward_confirmed\":" << (receipt.rewardConfirmed ? "true" : "false") << '}';
    return out.str();
}

std::string DirectGameObjectReceiptsJson(NewRpgInfo const& info)
{
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (DirectGameObjectReceipt const& receipt : info.directGameObjectReceipts)
    {
        if (!first)
            out << ',';
        out << "{\"sequence\":" << receipt.sequence
            << ",\"quest_id\":" << receipt.questId
            << ",\"objective_slot\":" << static_cast<uint32>(receipt.objectiveSlot)
            << ",\"entry\":" << receipt.entry
            << ",\"guid\":" << receipt.guid.GetCounter()
            << ",\"before\":" << receipt.before
            << ",\"after\":" << receipt.after << '}';
        first = false;
    }
    out << ']';
    return out.str();
}

std::string OracleRouteNativeInteractionJson(uint32 botGuid, uint32 questId)
{
    AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff handoff;
    bool const available = AutoWowOracleQuestExecutor::GetQuestRouteHandoff(botGuid, handoff) &&
        handoff.actor == botGuid && handoff.questId == questId &&
        handoff.result.routeKey.actor == botGuid && handoff.result.routeKey.questId == questId &&
        handoff.result.routeKey.stableSpawn != 0 && handoff.lease != 0;
    bool const nativeInteraction = available && handoff.nativeInteractionObserved &&
        handoff.leaseReleaseAuthoritative && handoff.result.leaseReleaseAuthoritative;
    std::ostringstream out;
    out << "{\"available\":" << (available ? "true" : "false")
        << ",\"actor\":" << (available ? handoff.actor : 0)
        << ",\"quest_id\":" << (available ? handoff.questId : 0)
        << ",\"server_session_id\":\""
        << (available ? handoff.blockedReentryFacts.serverSessionId : 0) << '\"'
        << ",\"route_identity\":\"" << (available
            ? AutoWowOracleQuestExecutor::MakeQuestRouteIdentity(handoff.result.routeKey) : 0) << '\"'
        << ",\"lease\":\"" << (available ? handoff.lease : 0) << '\"'
        << ",\"stable_spawn\":" << (available ? handoff.result.routeKey.stableSpawn : 0)
        << ",\"native_interaction\":" << (nativeInteraction ? "true" : "false")
        << ",\"lease_release_authoritative\":"
        << (available && handoff.leaseReleaseAuthoritative ? "true" : "false") << '}';
    return out.str();
}
}  // namespace

namespace AutoWowQuestLog
{
Capability ClassifyCapability(uint32 questType, uint32 suggestedPlayers, uint32 sourceItemId,
                              bool hasNpc, bool hasGameObject, bool hasItem, bool isComplete)
{
    if (isComplete)
        return {"turnin", true};
    // Match NewRpgBaseAction::IsQuestCapableDoing: group/elite only when BOTH a non-normal type and
    // 2+ suggested players (normal WotLK quests are QuestType 2 / SuggestedPlayers 0).
    if (questType != 0 && suggestedPlayers >= 2)
        return {"dungeon_group", false};
    // A provided source item plus a creature objective is the live-proven UseQuestItem path.
    // Item-on-GameObject/corpse/ground shapes remain unsupported elsewhere.
    if (sourceItemId != 0 && hasNpc)
        return {"item_use", true};
    if (hasGameObject)
        return {"gameobject", false};
    if (hasNpc)
        return {"kill", true};
    if (hasItem)
        return {"loot", true};
    return {"talk", true};
}

std::string Build(uint32 botGuid)
{
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (!bot)
        return "{\"ok\":false,\"error\":\"bot_not_online\"}";

    std::ostringstream out;
    out << "{\"ok\":true,\"guid\":" << botGuid << ",\"quests\":[";

    bool firstQuest = true;
    for (auto const& entry : bot->getQuestStatusMap())
    {
        uint32 const questId = entry.first;
        QuestStatusData const& data = entry.second;

        if (data.Status == QUEST_STATUS_NONE || bot->IsQuestRewarded(questId))
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        bool hasNpc = false;
        bool hasGo = false;
        bool hasItem = false;
        bool allDone = true;

        std::ostringstream objectives;
        bool firstObjective = true;
        auto appendObjective = [&](char const* kind, int32 objEntry, uint32 required, uint32 current)
        {
            bool const done = current >= required;
            if (!done)
                allDone = false;
            if (!firstObjective)
                objectives << ',';
            objectives << "{\"kind\":" << JsonString(kind)
                       << ",\"entry\":" << objEntry
                       << ",\"required\":" << required
                       << ",\"current\":" << current
                       << ",\"done\":" << (done ? "true" : "false") << "}";
            firstObjective = false;
        };

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const reqNpcOrGo = quest->RequiredNpcOrGo[i];
            uint32 const reqCount = quest->RequiredNpcOrGoCount[i];
            if (reqNpcOrGo == 0 || reqCount == 0)
                continue;

            if (reqNpcOrGo > 0)
            {
                hasNpc = true;
                appendObjective("npc", reqNpcOrGo, reqCount, data.CreatureOrGOCount[i]);
            }
            else
            {
                hasGo = true;
                appendObjective("gameobject", -reqNpcOrGo, reqCount, data.CreatureOrGOCount[i]);
            }
        }

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const itemId = quest->RequiredItemId[i];
            uint32 const reqCount = quest->RequiredItemCount[i];
            if (itemId == 0 || reqCount == 0)
                continue;

            hasItem = true;
            appendObjective("item", static_cast<int32>(itemId), reqCount, data.ItemCount[i]);
        }

        Capability const capability = ClassifyCapability(
            quest->GetType(), quest->GetSuggestedPlayers(), quest->GetSrcItemId(),
            hasNpc, hasGo, hasItem, data.Status == QUEST_STATUS_COMPLETE);
        bool const isComplete = data.Status == QUEST_STATUS_COMPLETE;

        if (!firstQuest)
            out << ',';
        out << "{\"id\":" << questId
            << ",\"title\":" << JsonString(quest->GetTitle())
            << ",\"level\":" << quest->GetQuestLevel()
            << ",\"quest_type\":" << quest->GetType()
            << ",\"suggested_players\":" << quest->GetSuggestedPlayers()
            << ",\"status\":" << static_cast<uint32>(data.Status)
            << ",\"is_complete\":" << (isComplete ? "true" : "false")
            << ",\"objectives_all_done\":" << (allDone ? "true" : "false")
            << ",\"capability_class\":" << JsonString(capability.name)
            << ",\"supported\":" << (capability.supported ? "true" : "false")
            << ",\"objectives\":[" << objectives.str() << "]}";
        firstQuest = false;
    }

    out << "]}";
    return out.str();
}
}  // namespace AutoWowQuestLog

namespace AutoWowQuestObjective
{
std::string Build(uint32 botGuid)
{
    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (!bot)
        return "{\"ok\":false,\"error\":\"bot_not_online\"}";

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI)
        return "{\"ok\":false,\"error\":\"bot_not_online\"}";

    AiObjectContext* context = botAI->GetAiObjectContext();
    if (!context)
        return "{\"ok\":false,\"error\":\"bot_context_unavailable\"}";

    // Authoritative published snapshot. A null Value pointer means the objective-lock Value is not
    // registered on this build; report it instead of fabricating an objective.
    Value<QuestObjectiveSpec>* specValue = context->GetValue<QuestObjectiveSpec>("active quest objective");
    if (!specValue)
        return "{\"ok\":false,\"error\":\"objective_value_unavailable\"}";

    QuestObjectiveSpec const spec = specValue->Get();

    // Mutable execution state is owned by NewRpgInfo::DoQuest. When the bot is not currently on the
    // DoQuest RPG status there is no runtime; report defaults (phase=resolve_objective, failure=none,
    // zeroed) so the schema stays fixed. Read-only: never mutate the runtime.
    QuestObjectiveRuntime const defaultRuntime;
    QuestObjectiveRuntime const* runtime = &defaultRuntime;
    NewRpgInfo::DoQuest const* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (doQuest)
        runtime = &doQuest->objectiveRuntime;
    bool const directiveActive = doQuest != nullptr;
    uint32 const directiveQuestId = doQuest ? doQuest->questId : 0;
    FinisherLiveView const finisherLive = ReadFinisherLive(bot, *runtime);

    std::ostringstream sources;
    bool firstSource = true;
    for (QuestObjectiveSource const& source : spec.sources)
    {
        if (!firstSource)
            sources << ',';
        sources << "{\"type\":\""
                << (source.type == QuestObjectiveSource::Type::Creature ? "creature" : "gameobject")
                << "\",\"entry\":" << source.entry << ",\"spawn_count\":" << source.spawns.size() << '}';
        firstSource = false;
    }

    WorldPosition const travelPos = doQuest ? doQuest->pos : WorldPosition();
    Quest const* directiveQuest = directiveQuestId ? sObjectMgr->GetQuestTemplate(directiveQuestId) : nullptr;
    bool const directiveQuestComplete = directiveQuest &&
                                        bot->GetQuestStatus(directiveQuestId) == QUEST_STATUS_COMPLETE;
    bool const objectiveActive = spec.hasLock();
    bool const finisherMode = IsFinisherMode(*runtime, directiveQuestComplete);
    uint32 const finisherQuestId = finisherMode
                                       ? (runtime->finisherReceipt.questId
                                              ? runtime->finisherReceipt.questId
                                              : directiveQuestId)
                                       : 0;
    Quest const* finisherQuest = finisherQuestId ? sObjectMgr->GetQuestTemplate(finisherQuestId) : nullptr;
    uint32 const finisherQuestStatus = finisherQuestId
                                           ? static_cast<uint32>(bot->GetQuestStatus(finisherQuestId))
                                           : 0;
    bool const finisherCanReward = finisherQuest && bot->CanRewardQuest(finisherQuest, false);
    bool const finisherRewardStatus = finisherQuestId && bot->GetQuestRewardStatus(finisherQuestId);
    bool const selectedRewardIndexKnown = finisherMode && runtime->selectedRewardIndexKnown;
    uint32 const selectedRewardIndex = selectedRewardIndexKnown ? runtime->selectedRewardIndex : 0;
    WorldPosition const finisherRouteTarget =
        (finisherMode && runtime->finisher.stableSpawn.GetRawValue() != 0)
            ? WorldPosition(runtime->finisher.stableSpawn)
            : travelPos;

    LastMovement const& lastMovement = context->GetValue<LastMovement&>("last movement")->Get();
    uint32 const lastMoveAgeMs = lastMovement.msTime == 0 ? 0 : GetMSTimeDiffToNow(lastMovement.msTime);
    uint32 const stuckAgeMs = botAI->rpgInfo.stuckTs == 0 ? 0 : GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs);
    WorldPosition const moveFarPos = botAI->rpgInfo.moveFarPos;
    AutoWowQuestGiverTravel::QuestWalkProbeDiagnostics const* walkProbe =
        AutoWowQuestGiverTravel::ReadQuestWalkDiagnostics(botGuid);
    AutoWowQuestGiverTravel::QuestWalkProbeDiagnostics onDemandWalkProbe;
    bool walkProbeOnDemand = false;
    if (!walkProbe && directiveActive && travelPos != WorldPosition() &&
        travelPos.GetMapId() == bot->GetMapId())
    {
        // Observer reads may arrive between autonomous ticks, or after the bridge has cleared a
        // completed journey's transient record. Re-run the same bounded, no-teleport selector as a
        // read-only snapshot so the response still explains the current route failure. The result
        // is not executed; it only prepares collision data and records scalar diagnostics.
        AutoWowQuestGiverTravel::QuestWalkProbeSelection const selection =
            AutoWowQuestGiverTravel::SelectQuestWalkProbeDetailed(bot, travelPos, false);
        onDemandWalkProbe = selection.diagnostics;
        walkProbe = &onDemandWalkProbe;
        walkProbeOnDemand = true;
    }

    // Read-only corpse diagnostics for the selected objective target. These
    // distinguish a no-drop corpse from group-loot ownership denial without
    // opening loot, changing flags, or touching quest state.
    bool selectedTargetLoaded = false;
    bool selectedTargetAlive = false;
    bool selectedTargetCorpse = false;
    bool selectedTargetLootable = false;
    bool selectedTargetAllowedToLoot = false;
    float selectedTargetDistance = -1.0f;
    if (!runtime->selectedTargetGuid.IsEmpty())
    {
        if (Unit* selected = ObjectAccessor::GetUnit(*bot, runtime->selectedTargetGuid))
        {
            selectedTargetLoaded = true;
            selectedTargetAlive = selected->IsAlive();
            selectedTargetDistance = bot->GetDistance(selected);
            if (Creature* creature = selected->ToCreature())
            {
                selectedTargetCorpse = creature->getDeathState() == DeathState::Corpse;
                selectedTargetLootable = creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
                selectedTargetAllowedToLoot = bot->isAllowedToLoot(creature);
            }
        }
    }

    // Read-only visibility facts for the exact persistent objective source. These distinguish an
    // unloaded grid from a loaded-but-absent/respawning object without forcing a grid load from
    // the observer endpoint or changing quest state.
    bool selectedSourceGridLoaded = false;
    bool selectedSourcePresent = false;
    bool selectedSourceInWorld = false;
    bool selectedSourceSpawned = false;
    if (runtime->selectedSourceEntry < 0 && bot->GetMap() &&
        runtime->selectedSourceSpawn.GetMapId() == bot->GetMapId())
    {
        selectedSourceGridLoaded = bot->GetMap()->IsGridLoaded(
            runtime->selectedSourceSpawn.GetPositionX(), runtime->selectedSourceSpawn.GetPositionY());
        if (selectedSourceGridLoaded)
        {
            auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(
                static_cast<ObjectGuid::LowType>(runtime->selectedSourceSpawn.GetCounter()));
            for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
            {
                GameObject* source = iterator->second;
                if (!source || source->GetSpawnId() != runtime->selectedSourceSpawn.GetCounter() ||
                    source->GetEntry() != static_cast<uint32>(-runtime->selectedSourceEntry) ||
                    source->GetMapId() != bot->GetMapId() || source->GetInstanceId() != bot->GetInstanceId())
                    continue;
                selectedSourcePresent = true;
                selectedSourceInWorld = source->IsInWorld();
                selectedSourceSpawned = source->isSpawned();
                break;
            }
        }
    }

    LootObject const lootTarget = context->GetValue<LootObject>("loot target")->Get();
    bool const canLoot = context->GetValue<bool>("can loot")->Get();
    bool const hasAvailableLoot = context->GetValue<bool>("has available loot")->Get();
    ObjectGuid const activeLootGuid = bot->GetLootGUID();
    uint8 const bagSpacePercent = context->GetValue<uint8>("bag space")->Get();

    std::ostringstream out;
    out << "{\"ok\":true,\"guid\":" << botGuid
        << ",\"directive_active\":" << (directiveActive ? "true" : "false")
        << ",\"directive_quest_id\":" << directiveQuestId
        << ",\"oracle_route\":" << OracleRouteNativeInteractionJson(
               botGuid, directiveQuestId ? directiveQuestId : spec.key.questId)
        << ",\"finisher\":" << FinisherTelemetryJson(
               finisherMode, finisherQuestId,
               FinisherQuestIdSource(finisherMode, runtime->finisherReceipt.questId, directiveQuestId),
               *runtime, finisherLive, finisherRouteTarget, runtime->finisherReceipt,
               finisherQuestStatus, finisherCanReward, finisherRewardStatus, selectedRewardIndex,
               selectedRewardIndexKnown)
        << ",\"direct_go_receipts\":" << DirectGameObjectReceiptsJson(botAI->rpgInfo)
        << ",\"movement\":{\"is_moving\":" << (bot->isMoving() ? "true" : "false")
        << ",\"last_move_to\":{\"map\":" << lastMovement.lastMoveToMapId
        << ",\"x\":" << lastMovement.lastMoveToX
        << ",\"y\":" << lastMovement.lastMoveToY
        << ",\"z\":" << lastMovement.lastMoveToZ << "}"
        << ",\"last_move_delay_ms\":" << lastMovement.lastdelayTime
        << ",\"last_move_age_ms\":" << lastMoveAgeMs
        << ",\"last_move_priority\":" << JsonString(MovementPriorityName(lastMovement.priority))
        << ",\"rpg_move_far\":{\"map\":" << moveFarPos.GetMapId()
        << ",\"x\":" << moveFarPos.GetPositionX()
        << ",\"y\":" << moveFarPos.GetPositionY()
        << ",\"z\":" << moveFarPos.GetPositionZ() << "}"
        << ",\"nearest_move_far_distance\":" << botAI->rpgInfo.nearestMoveFarDis
        << ",\"stuck_attempts\":" << botAI->rpgInfo.stuckAttempts
        << ",\"stuck_age_ms\":" << stuckAgeMs
        << ",\"walk_probe\":{\"available\":" << (walkProbe ? "true" : "false");
    if (walkProbe)
    {
        out << ",\"source\":" << JsonString(walkProbeOnDemand ? "on_demand_snapshot" : "runtime_attempt")
            << ",\"selection_source\":" << JsonString(walkProbe->selectionSource)
            << ",\"attempted\":" << (walkProbe->attempted ? "true" : "false")
            << ",\"direct\":{\"attempted\":"
            << (walkProbe->directProbeAttempted ? "true" : "false")
            << ",\"safe\":" << (walkProbe->directProbeSafe ? "true" : "false")
            << ",\"path_type\":" << walkProbe->directPathType
            << ",\"point_count\":" << walkProbe->directPointCount
            << ",\"endpoint_distance\":";
        AppendJsonNumber(out, walkProbe->directEndpointDistance);
        out << ",\"path_length\":";
        AppendJsonNumber(out, walkProbe->directPathLength);
        out << ",\"reason\":" << JsonString(walkProbe->directReason)
            << ",\"navmesh_reason\":" << JsonString(walkProbe->directNavmeshReason)
            << ",\"ground_line_reason\":" << JsonString(walkProbe->directGroundLineReason) << "}"
            << ",\"travel_mgr\":{\"attempted\":"
            << (walkProbe->travelMgrAttempted ? "true" : "false")
            << ",\"path_point_count\":" << walkProbe->travelMgr.pathPointCount
            << ",\"scan_points_considered\":" << walkProbe->travelMgr.scanPointsConsidered
            << ",\"same_map_walk_points\":" << walkProbe->travelMgr.sameMapWalkPoints
            << ",\"distance_eligible_points\":" << walkProbe->travelMgr.distanceEligiblePoints
            << ",\"fresh_probe_attempts\":" << walkProbe->travelMgr.freshProbeAttempts
            << ",\"fresh_probe_accepts\":" << walkProbe->travelMgr.freshProbeAccepts
            << ",\"selected\":" << (walkProbe->travelMgr.selected ? "true" : "false")
            << ",\"selected_route_index\":" << walkProbe->travelMgr.selectedRouteIndex
            << ",\"selected_distance\":";
        AppendJsonNumber(out, walkProbe->travelMgr.selectedDistance);
        out << ",\"nearest_route_distance\":";
        AppendJsonNumber(out, walkProbe->travelMgr.nearestRouteDistance);
        out << ",\"farthest_route_distance\":";
        AppendJsonNumber(out, walkProbe->travelMgr.farthestRouteDistance);
        out << ",\"reason\":" << JsonString(std::string(
                   AutoWowQuestGiverTravel::TravelMgrDiagnosticReasonName(walkProbe->travelMgr.reason)))
            << "}"
            << ",\"walk_prepared_called\":" << (walkProbe->walkPreparedCalled ? "true" : "false")
            << ",\"walk_prepared_accepted\":" << (walkProbe->walkPreparedAccepted ? "true" : "false")
            << ",\"walk_reject_reason\":" << JsonString(std::string(
                   AutoWowQuestGiverTravel::QuestWalkPreparedRejectReasonName(walkProbe->walkRejectReason)));
    }
    out << "}}"
        << ",\"objective\":{"
        << "\"quest_id\":" << spec.key.questId
        << ",\"active\":" << (objectiveActive ? "true" : "false")
        << ",\"inactive_reason\":" << JsonString(
               ObjectiveInactiveReason(spec, objectiveActive, finisherMode))
        << ",\"objective_family\":" << JsonString(FamilyName(spec.key.family))
        << ",\"objective_slot\":" << static_cast<uint32>(spec.key.slot)
        << ",\"objective_kind\":" << JsonString(KindName(spec.kind))
        << ",\"phase\":" << JsonString(PhaseName(runtime->phase))
        << ",\"failure_reason\":" << JsonString(FailureName(runtime->failure))
        << ",\"supported\":" << (spec.supported ? "true" : "false")
        << ",\"required_npc_or_go_entry\":" << spec.requiredNpcOrGoEntry
        << ",\"required_item_id\":" << spec.requiredItemId
        << ",\"quest_item_id\":" << spec.questItemId
        << ",\"current_count\":" << spec.currentCount
        << ",\"required_count\":" << spec.requiredCount
        << ",\"baseline_count\":" << runtime->baselineCount
        << ",\"selected_source_entry\":" << runtime->selectedSourceEntry
        << ",\"source_entries\":[" << sources.str() << ']'
        << ",\"selected_source_spawn\":{\"guid\":" << runtime->selectedSourceSpawn.GetCounter()
        << ",\"map\":" << runtime->selectedSourceSpawn.GetMapId()
        << ",\"x\":" << runtime->selectedSourceSpawn.GetPositionX()
        << ",\"y\":" << runtime->selectedSourceSpawn.GetPositionY()
        << ",\"z\":" << runtime->selectedSourceSpawn.GetPositionZ()
        << ",\"distance\":" << bot->GetDistance(runtime->selectedSourceSpawn)
        << ",\"grid_loaded\":" << (selectedSourceGridLoaded ? "true" : "false")
        << ",\"spawn_present\":" << (selectedSourcePresent ? "true" : "false")
        << ",\"in_world\":" << (selectedSourceInWorld ? "true" : "false")
        << ",\"spawned\":" << (selectedSourceSpawned ? "true" : "false") << '}'
        << ",\"travel_position\":{\"map\":" << travelPos.GetMapId()
        << ",\"x\":" << travelPos.GetPositionX()
        << ",\"y\":" << travelPos.GetPositionY()
        << ",\"z\":" << travelPos.GetPositionZ() << '}'
        << ",\"selected_target_guid\":" << runtime->selectedTargetGuid.GetCounter()
        << ",\"selected_target\":{\"loaded\":" << (selectedTargetLoaded ? "true" : "false")
        << ",\"alive\":" << (selectedTargetAlive ? "true" : "false")
        << ",\"corpse\":" << (selectedTargetCorpse ? "true" : "false")
        << ",\"lootable\":" << (selectedTargetLootable ? "true" : "false")
        << ",\"allowed_to_loot\":" << (selectedTargetAllowedToLoot ? "true" : "false")
        << ",\"distance\":" << selectedTargetDistance << '}'
        << ",\"loot_state\":{\"target_guid\":" << lootTarget.guid.GetCounter()
        << ",\"can_loot\":" << (canLoot ? "true" : "false")
        << ",\"has_available_loot\":" << (hasAvailableLoot ? "true" : "false")
        << ",\"active_loot_guid\":" << activeLootGuid.GetCounter()
        << ",\"bag_space_percent\":" << static_cast<uint32>(bagSpacePercent)
        << ",\"outgoing_queue_depth\":" << botAI->GetPendingBotOutgoingPacketCount()
        << ",\"loot_response_count\":" << botAI->GetAutoWowLootResponseCount()
        << ",\"store_loot_execution_count\":" << botAI->GetAutoWowStoreLootExecutionCount()
        << ",\"autostore_packet_count\":" << botAI->GetAutoWowAutostoreLootPacketCount()
        << ",\"loot_release_packet_count\":" << botAI->GetAutoWowLootReleasePacketCount() << '}'
        << ",\"quest_item_use_packet_count\":" << botAI->GetAutoWowQuestItemUsePacketCount()
        << ",\"finisher_entry\":" << runtime->finisher.signedEntry
        << ",\"finisher_guid\":" << runtime->finisher.runtimeGuid.GetCounter()
        << ",\"finisher_receipt\":" << FinisherReceiptJson(runtime->finisherReceipt)
        << ",\"finisher_live\":{\"loaded\":" << (finisherLive.loaded ? "true" : "false")
        << ",\"position\":{\"map\":" << finisherLive.map
        << ",\"x\":" << finisherLive.x
        << ",\"y\":" << finisherLive.y
        << ",\"z\":" << finisherLive.z << "}"
        << ",\"distance\":" << finisherLive.distance << "}"
        << ",\"selected_reward_index_known\":" << (runtime->selectedRewardIndexKnown ? "true" : "false")
        << ",\"selected_reward_index\":" << runtime->selectedRewardIndex
        << ",\"has_lock\":" << (spec.hasLock() ? "true" : "false")
        << "}}";
    return out.str();
}

std::string BuildAcceptance(uint32 botGuid, uint32 requestedQuestId)
{
    // Reuse the objective snapshot as the "objective" block; append reward postcondition + counters.
    std::string objective = Build(botGuid);
    if (objective.rfind("{\"ok\":false", 0) == 0)
        return objective;  // offline / value-unavailable: surface the error unchanged

    Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
    if (!bot || !botAI)
        return "{\"ok\":false,\"error\":\"bot_not_online\"}";

    AiObjectContext* context = botAI->GetAiObjectContext();
    Value<QuestObjectiveSpec>* specValue =
        context ? context->GetValue<QuestObjectiveSpec>("active quest objective") : nullptr;
    QuestObjectiveSpec const spec = specValue ? specValue->Get() : QuestObjectiveSpec();

    NewRpgInfo::DoQuest const* doQuest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    bool const directiveActive = doQuest != nullptr;
    uint32 const directiveQuestId = doQuest ? doQuest->questId : 0;

    // A completed directive no longer has an incomplete objective spec. Prefer the durable directive
    // identity so acceptance without an explicit quest id still reports its finisher/reward state.
    uint32 const activeQuestId = directiveActive ? directiveQuestId : spec.key.questId;
    uint32 const questId = requestedQuestId ? requestedQuestId : activeQuestId;
    bool const objectiveMatchesTrackedQuest = questId != 0 && spec.key.questId == questId;
    bool const directiveMatchesTrackedQuest = directiveActive && directiveQuestId == questId;
    Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
    uint32 const status = questId ? static_cast<uint32>(bot->GetQuestStatus(questId)) : 0;
    bool const canReward = quest && bot->CanRewardQuest(quest, false);
    bool const rewardStatus = questId ? bot->GetQuestRewardStatus(questId) : false;

    QuestObjectiveRuntime const defaultRuntime;
    QuestObjectiveRuntime const* runtime = &defaultRuntime;
    if (doQuest)
        runtime = &doQuest->objectiveRuntime;

    bool const runtimeMatchesTrackedQuest = directiveMatchesTrackedQuest;
    bool const selectedRewardIndexKnown = runtimeMatchesTrackedQuest && runtime->selectedRewardIndexKnown;
    uint32 const selectedRewardIndex = selectedRewardIndexKnown ? runtime->selectedRewardIndex : 0;
    bool const selectedRewardInventoryFull = runtimeMatchesTrackedQuest &&
                                             runtime->failure == QuestFailureReason::InventoryFull;
    FinisherLiveView const finisherLive = runtimeMatchesTrackedQuest
                                              ? ReadFinisherLive(bot, *runtime)
                                              : FinisherLiveView();

    AutoWowAcceptance::Counters const c = AutoWowAcceptance::Get();

    // objective == {"ok":true,"guid":G,"objective":{...}} — drop its final brace and append fields.
    std::string body = objective;
    if (!body.empty() && body.back() == '}')
        body.pop_back();

    std::ostringstream out;
    int32 const finisherEntry = runtimeMatchesTrackedQuest ? runtime->finisher.signedEntry : 0;
    uint32 const finisherGuid = runtimeMatchesTrackedQuest ? runtime->finisher.runtimeGuid.GetCounter() : 0;
    QuestFinisherReceipt const finisherReceipt = runtimeMatchesTrackedQuest
        ? runtime->finisherReceipt
        : QuestFinisherReceipt();

    out << body
        << ",\"tracked_quest_id\":" << questId
        << ",\"tracked_oracle_route\":" << OracleRouteNativeInteractionJson(botGuid, questId)
        << ",\"objective_matches_tracked_quest\":" << (objectiveMatchesTrackedQuest ? "true" : "false")
        << ",\"directive_matches_tracked_quest\":" << (directiveMatchesTrackedQuest ? "true" : "false")
        << ",\"reward\":{\"quest_status\":" << status
        << ",\"can_reward\":" << (canReward ? "true" : "false")
        << ",\"selected_reward_index_known\":" << (selectedRewardIndexKnown ? "true" : "false")
        << ",\"selected_reward_index\":" << selectedRewardIndex
        << ",\"selected_reward_inventory_full\":" << (selectedRewardInventoryFull ? "true" : "false")
        << ",\"reward_status\":" << (rewardStatus ? "true" : "false")
        << ",\"finisher_entry\":" << finisherEntry
        << ",\"finisher_guid\":" << finisherGuid
        << ",\"finisher_receipt\":" << FinisherReceiptJson(finisherReceipt)
        << ",\"finisher_live\":{\"loaded\":" << (finisherLive.loaded ? "true" : "false")
        << ",\"position\":{\"map\":" << finisherLive.map
        << ",\"x\":" << finisherLive.x
        << ",\"y\":" << finisherLive.y
        << ",\"z\":" << finisherLive.z << "}"
        << ",\"distance\":" << finisherLive.distance << "}"
        << ",\"relation_verified\":" << (finisherReceipt.exactRelationVerified ? "true" : "false") << "}"
        << ",\"objective_context_present\":" << (objectiveMatchesTrackedQuest && spec.hasLock() ? "true" : "false")
        << ",\"invariants\":{\"unrelated_offensive_target_count\":" << c.unrelatedOffensivePull
        << ",\"random_grind_fallback_count\":" << c.randomGrindFallback
        << ",\"teleport_count\":" << c.teleport
        << ",\"direct_quest_db_mutation_count\":" << c.directQuestDbMutation << "}"
        << ",\"read_only\":true"
        << ",\"schema\":\"autowow.phase1.acceptance.evidence.v1\",\"schema_version\":1}";
    return out.str();
}
}  // namespace AutoWowQuestObjective
