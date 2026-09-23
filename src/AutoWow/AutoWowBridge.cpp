/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowBridge.h"
#include "AutoWowCraftControl.h"
#include "AutoWowGuildTradeControl.h"
#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "OracleQuestDispatchPolicy.h"
#include "AdvanceFormation.h"
#include "BossApproachControl.h"
#include "CombatTelemetry.h"
#include "EncounterTelemetry.h"
#include "DungeonPathSafety.h"
#include "DungeonPathWalkAction.h"
#include "ExactPartyRepairPolicy.h"
#include "ExactBossTargetControl.h"
#include "FixtureAccelerationControl.h"
#include "FixtureFactoryControl.h"
#include "ProbePlaceControl.h"
#include "ProbeResetControl.h"
#include "QuestAcquisitionPolicy.h"
#include "QuestGiverTravelFeedback.h"
#include "QuestGiverTravelLifecycle.h"
#include "QuestPartyParticipantPolicy.h"
#include "RaidFixtureControl.h"
#include "WarsongFixtureControl.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "AttackAction.h"
#include "ChooseTravelTargetAction.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameObject.h"
#include "GameEventMgr.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Map.h"
#include "GatheringWorkerState.h"
#include "MoveToTravelTargetAction.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObserverControl.h"
#include "AutoWowProfessionEconomyTelemetry.h"
#include "QuestLogView.h"
#include "QuestObjectiveContext.h"
#include "QuestValues.h"
#include "MovementActions.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PortalAdmissionPolicy.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedValueContext.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "Value.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutoWowPolicy
{
namespace
{
std::mutex noTeleportMutex;
std::unordered_set<uint32> noTeleportBots;
}

void SetNoTeleport(uint32 botGuid, bool enabled)
{
    std::lock_guard<std::mutex> lock(noTeleportMutex);
    if (enabled)
        noTeleportBots.insert(botGuid);
    else
        noTeleportBots.erase(botGuid);
}

bool IsNoTeleport(uint32 botGuid)
{
    std::lock_guard<std::mutex> lock(noTeleportMutex);
    return noTeleportBots.contains(botGuid);
}
}

namespace
{
using boost::asio::ip::tcp;

constexpr uint32 AUTO_WOW_RESPONSE_TIMEOUT_MS = 2000;

bool IsNamedExteriorRoute(std::string_view name)
{
    constexpr std::string_view suffix = "-exterior";
    return name.size() > suffix.size() && name.ends_with(suffix);
}

bool AreaTriggerContainsPoint(AreaTrigger const& trigger, float x, float y, float z)
{
    float const dx = x - trigger.x;
    float const dy = y - trigger.y;
    float const dz = z - trigger.z;
    if (trigger.radius > 0.0f)
        return dx * dx + dy * dy + dz * dz <= trigger.radius * trigger.radius;

    float const sine = std::sin(trigger.orientation);
    float const cosine = std::cos(trigger.orientation);
    float const localX = dx * cosine + dy * sine;
    float const localY = dy * cosine - dx * sine;
    return std::fabs(localX) <= trigger.length * 0.5f &&
        std::fabs(localY) <= trigger.width * 0.5f &&
        std::fabs(dz) <= trigger.height * 0.5f;
}

std::pair<uint16, uint16> OracleProfessionPair(uint32 botGuid)
{
    switch (botGuid)
    {
        case 101: return {SKILL_HERBALISM, SKILL_ALCHEMY};
        case 112: return {SKILL_MINING, SKILL_BLACKSMITHING};
        case 121: return {SKILL_HERBALISM, SKILL_SKINNING};
        case 123: return {SKILL_MINING, SKILL_JEWELCRAFTING};
        case 139: return {SKILL_MINING, SKILL_ENGINEERING};
        case 144: return {SKILL_HERBALISM, SKILL_ALCHEMY};
        case 154: return {SKILL_MINING, SKILL_ENGINEERING};
        case 166: return {SKILL_HERBALISM, SKILL_INSCRIPTION};
        case 236: return {SKILL_HERBALISM, SKILL_TAILORING};
        case 244: return {SKILL_SKINNING, SKILL_LEATHERWORKING};
        default: return {0, 0};
    }
}

uint64 GetResolvedGiverSpawnId(WorldObject* giver)
{
    if (!giver)
        return 0;
    if (Creature* creature = giver->ToCreature())
        return creature->GetSpawnId();
    if (GameObject* gameObject = giver->ToGameObject())
        return gameObject->GetSpawnId();
    return 0;
}

AreaTrigger const* FindNonRaidDungeonExteriorTrigger(
    std::string_view routeName, uint32 map, float x, float y, float z)
{
    if (!IsNamedExteriorRoute(routeName))
        return nullptr;

    AreaTrigger const* best = nullptr;
    float bestDistanceSquared = std::numeric_limits<float>::infinity();
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        MapEntry const* targetMap = sMapStore.LookupEntry(teleport.target_mapId);
        if (!trigger || trigger->map != map || !targetMap || !targetMap->IsNonRaidDungeon() ||
            !AreaTriggerContainsPoint(*trigger, x, y, z))
            continue;

        float const dx = x - trigger->x;
        float const dy = y - trigger->y;
        float const dz = z - trigger->z;
        float const distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared < bestDistanceSquared ||
            (distanceSquared == bestDistanceSquared && (!best || triggerId < best->entry)))
        {
            best = trigger;
            bestDistanceSquared = distanceSquared;
        }
    }
    return best;
}

class AutoWowExactAttackAction final : public AttackAction
{
public:
    explicit AutoWowExactAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "autowow exact engage") {}

    bool AttackExact(Unit* target) { return Attack(target); }
};

enum class AutoWowRequestType
{
    Invalid,
    List,
    OracleLog,
    Destinations,
    Snapshot,
    CombatLog,
    EncounterLog,
    ProfessionEconomy,
    Craft,
    CraftStatus,
    GuildTrade,
    GuildTradeStatus,
    QuestLog,
    QuestObjective,
    GatherSourcePublish,
    Acceptance,
    Activate,
    Deactivate,
    Independent,
    Party,
    Rally,
    Deploy,
    Route,
    Advance,
    AdvancePoint,
    Engage,
    BossApproach,
    Scout,
    PathProbe,
    Quest,
    QuestAcquire,
    Recover,
    Observe,
    Pause,
    Resume,
    Travel,
    ProbeReset,
    ProbeFixture,
    FixtureInit,
    FixtureStatus,
    FixtureAccelerate,
    FixtureAccelerateOff,
    FixtureKill,
    RaidCreate,
    RaidStatus,
    RaidLeave,
    WsgQueue,
    WsgStatus,
    WsgLeave
};

struct AutoWowRequest
{
    AutoWowRequestType type = AutoWowRequestType::Invalid;
    uint32 botGuid = 0;
    std::string oracleSessionId;
    uint64 oracleCursor = 0;
    std::vector<uint32> memberGuids;
    std::string destination;
    uint32 fixtureLevel = 0;
    uint32 fixtureSpecIndex = 0;
    uint32 fixtureQuality = 0;
    uint32 fixturePacingPercent = 0;
    uint32 fixtureKillEntry = 0;
    uint64 fixtureKillSpawnId = 0;
    AutoWowProbePlace::WireRequest probe;
    uint32 raidDifficulty = 0;
    uint32 expectedMapId = 0;
    uint32 exactCreatureEntry = 0;
    uint32 exactPlayerGuid = 0;
    uint64 gatherSpawnId = 0;
    uint32 gatherEntry = 0;
    uint32 gatherMapId = 0;
    uint32 gatherInstanceId = 0;
    uint32 gatherMaterialItemId = 0;
    uint32 craftRecipeSpellId = 0;
    uint32 tradeBuyerGuid = 0;
    uint32 tradeItemGuid = 0;
    uint32 tradeItemEntry = 0;
    uint32 tradeQuantity = 0;
    uint32 tradePriceCopper = 0;
    AutoWowOracle::GatherGoal gatherGoal = AutoWowOracle::GatherGoal::Unknown;
    float coordinateX = 0.0f;
    float coordinateY = 0.0f;
    float coordinateZ = 0.0f;
    bool hasCoordinateOrientation = false;
    float coordinateOrientation = 0.0f;
    bool hasSourceCoordinates = false;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceZ = 0.0f;
    bool bossStatusOnly = false;
};

class AutoWowCompletion
{
public:
    void Resolve(std::string value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelled)
            return;

        m_value = std::move(value);
        m_done = true;
        m_condition.notify_one();
    }

    bool WaitFor(std::string& value, uint32 timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_done; }))
            return false;

        value = m_value;
        return true;
    }

    void Cancel()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled = true;
    }

    bool IsCancelled() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelled;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_done = false;
    bool m_cancelled = false;
    std::string m_value;
};

std::string Trim(std::string value)
{
    auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool ParseGatherGoal(std::string value, AutoWowOracle::GatherGoal& goal)
{
    value = Lowercase(std::move(value));
    if (value == "harvest-node" || value == "harvest_node" || value == "harvest")
    {
        goal = AutoWowOracle::GatherGoal::HarvestNode;
        return true;
    }
    if (value == "obtain-material" || value == "obtain_material" || value == "obtain")
    {
        goal = AutoWowOracle::GatherGoal::ObtainMaterial;
        return true;
    }
    return false;
}

bool ParseBotGuid(std::string const& value, uint32& botGuid)
{
    if (value.empty())
        return false;

    char* end = nullptr;
    unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > std::numeric_limits<uint32>::max())
        return false;

    botGuid = static_cast<uint32>(parsed);
    return true;
}

bool ParseUnsignedToken(std::string const& value, uint32& parsedValue)
{
    if (value.empty())
        return false;

    for (unsigned char character : value)
        if (!std::isdigit(character))
            return false;

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed > std::numeric_limits<uint32>::max())
        return false;

    parsedValue = static_cast<uint32>(parsed);
    return true;
}

bool ParseUnsigned64Token(std::string const& value, uint64& parsedValue)
{
    if (value.empty())
        return false;

    for (unsigned char character : value)
        if (!std::isdigit(character))
            return false;

    char* end = nullptr;
    unsigned long long const parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > std::numeric_limits<uint64>::max())
        return false;

    parsedValue = static_cast<uint64>(parsed);
    return true;
}

bool ParseFloatToken(std::string const& value, float& parsedValue)
{
    if (value.empty())
        return false;

    char* end = nullptr;
    float const parsed = std::strtof(value.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(parsed))
        return false;

    parsedValue = parsed;
    return true;
}

bool ParseOracleCursor(std::string const& value, uint64& parsedValue)
{
    if (value.empty())
        return false;

    for (unsigned char character : value)
        if (!std::isdigit(character))
            return false;

    errno = 0;
    char* end = nullptr;
    unsigned long long const parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || parsed > std::numeric_limits<uint64>::max())
        return false;

    parsedValue = static_cast<uint64>(parsed);
    return true;
}

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

std::string GatherLootAdmissionFailureName(std::uint32_t mask)
{
    if (mask == AutoWowGather::GatherLootAdmissionNone)
        return "none";

    std::string result;
    auto append = [&result](char const* name)
    {
        if (!result.empty())
            result += '|';
        result += name;
    };
    if (mask & AutoWowGather::GatherLootAdmissionNotWorker) append("not_worker");
    if (mask & AutoWowGather::GatherLootAdmissionNotPrepared) append("not_prepared");
    if (mask & AutoWowGather::GatherLootAdmissionGuidMismatch) append("guid");
    if (mask & AutoWowGather::GatherLootAdmissionRuntimeMismatch) append("runtime");
    if (mask & AutoWowGather::GatherLootAdmissionSpawnMismatch) append("spawn");
    if (mask & AutoWowGather::GatherLootAdmissionEntryMismatch) append("entry");
    if (mask & AutoWowGather::GatherLootAdmissionLiveItemMismatch) append("live_item");
    if (mask & AutoWowGather::GatherLootAdmissionRequestedMismatch) append("requested");
    return result;
}

struct QuestPartyMemberReceiptView
{
    uint32 guid = 0;
    std::string name;
    uint32 questStatus = static_cast<uint32>(QUEST_STATUS_NONE);
    uint32 phaseId = 255;
    int32 finisherEntry = 0;
    uint64 finisherStableSpawnGuid = 0;
    uint64 finisherRuntimeGuid = 0;
    bool online = false;
    bool playerbot = false;
    bool exactQuestInLog = false;
    bool rewarded = false;
    bool coreCanComplete = false;
    bool objectiveDone = false;
    bool questParticipant = false;
    bool directiveActive = false;
    bool primingActionStarted = false;
    bool hasExactFinisher = false;
    bool reachesExactFinisher = false;
    bool exactRelationVerified = false;
    bool coreCompleted = false;
    bool coreRewardConfirmed = false;
    std::string reason;
};

QuestPartyMemberReceiptView InspectQuestPartyMember(Player* member, PlayerbotAI* memberAI,
                                                    uint32 questId)
{
    QuestPartyMemberReceiptView view;
    if (!member)
    {
        view.reason = "member_missing";
        return view;
    }

    view.guid = member->GetGUID().GetCounter();
    view.name = member->GetName();
    view.online = member->IsInWorld();
    view.questStatus = static_cast<uint32>(member->GetQuestStatus(questId));
    view.rewarded = member->GetQuestRewardStatus(questId);
    auto const questStatus = member->getQuestStatusMap().find(questId);
    view.exactQuestInLog = questStatus != member->getQuestStatusMap().end() &&
        view.questStatus != static_cast<uint32>(QUEST_STATUS_NONE);
    view.coreCanComplete = view.exactQuestInLog &&
        member->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE && member->CanCompleteQuest(questId);
    view.objectiveDone = view.questStatus == static_cast<uint32>(QUEST_STATUS_COMPLETE) || view.coreCanComplete;
    view.coreCompleted = view.questStatus == static_cast<uint32>(QUEST_STATUS_COMPLETE);
    view.coreRewardConfirmed = view.rewarded;

    if (!memberAI)
    {
        view.reason = "member_ai_missing";
        return view;
    }

    view.playerbot = !memberAI->IsRealPlayer();
    if (!view.playerbot)
    {
        view.reason = "member_is_real_player";
        return view;
    }

    NewRpgInfo::DoQuest const* doQuest = std::get_if<NewRpgInfo::DoQuest>(&memberAI->rpgInfo.data);
    if (doQuest && doQuest->questId == questId)
    {
        view.directiveActive = true;
        view.phaseId = static_cast<uint32>(doQuest->objectiveRuntime.phase);
        QuestObjectiveRuntime const& runtime = doQuest->objectiveRuntime;
        QuestFinisherRef finisher = runtime.finisher;

        if (AiObjectContext* context = memberAI->GetAiObjectContext())
        {
            if (Value<QuestFinisherRef>* finisherValue =
                    context->GetValue<QuestFinisherRef>("active quest finisher"))
                finisher = finisherValue->Get();
        }

        view.finisherEntry = runtime.finisherReceipt.signedEntry != 0
            ? runtime.finisherReceipt.signedEntry
            : finisher.signedEntry;
        view.finisherStableSpawnGuid = runtime.finisherReceipt.stableSpawnGuid.GetRawValue() != 0
            ? runtime.finisherReceipt.stableSpawnGuid.GetRawValue()
            : finisher.stableSpawn.GetRawValue();
        view.finisherRuntimeGuid = runtime.finisherReceipt.runtimeGuid.GetRawValue() != 0
            ? runtime.finisherReceipt.runtimeGuid.GetRawValue()
            : finisher.runtimeGuid.GetRawValue();
        view.hasExactFinisher = view.finisherEntry != 0 && view.finisherStableSpawnGuid != 0;
        view.reachesExactFinisher = runtime.finisherReceipt.atInteractionRange;
        view.exactRelationVerified = runtime.finisherReceipt.exactRelationVerified;
        view.coreCompleted = view.coreCompleted || runtime.finisherReceipt.coreCompleted;
        view.coreRewardConfirmed = view.coreRewardConfirmed || runtime.finisherReceipt.rewardConfirmed;
        view.objectiveDone = view.objectiveDone || view.hasExactFinisher;
    }

    if (view.rewarded)
        view.reason = "quest_already_rewarded";
    else if (!view.exactQuestInLog)
        view.reason = "quest_not_in_member_log";
    else if (!member->IsAlive())
        view.reason = "member_dead";
    else if (!view.directiveActive)
        view.reason = "directive_not_active";
    return view;
}

AutoWowQuestParty::MemberFacts MakeQuestPartyMemberFacts(
    Player* member, QuestPartyMemberReceiptView const& view, bool leader)
{
    bool const statusAllowed = view.rewarded ||
        view.questStatus == static_cast<uint32>(QUEST_STATUS_INCOMPLETE) ||
        view.questStatus == static_cast<uint32>(QUEST_STATUS_COMPLETE);
    return {leader, member != nullptr, view.online, view.playerbot, member && member->IsAlive(),
            view.exactQuestInLog, view.rewarded, statusAllowed};
}

std::string QuestPartyNonParticipantReason(Player* member, QuestPartyMemberReceiptView const& view)
{
    if (!member || !view.playerbot)
        return "party_member_not_playerbot";
    if (!view.online)
        return "nonparticipant_offline";
    if (!member->IsAlive())
        return "nonparticipant_dead";
    if (view.rewarded)
        return "nonparticipant_rewarded";
    if (!view.exactQuestInLog)
        return "nonparticipant_quest_not_in_log";

    bool const statusAllowed = view.questStatus == static_cast<uint32>(QUEST_STATUS_INCOMPLETE) ||
        view.questStatus == static_cast<uint32>(QUEST_STATUS_COMPLETE);
    return statusAllowed ? "nonparticipant_directive_not_active" :
                           "nonparticipant_status_not_executable";
}

void AppendQuestPartyMemberReceipt(std::ostringstream& out, bool& first,
                                   QuestPartyMemberReceiptView const& view)
{
    if (!first)
        out << ',';
    first = false;
    out << "{\"guid\":" << view.guid
        << ",\"name\":" << JsonString(view.name)
        << ",\"quest_status\":" << view.questStatus
        << ",\"phase_id\":" << view.phaseId
        << ",\"online\":" << (view.online ? "true" : "false")
        << ",\"playerbot\":" << (view.playerbot ? "true" : "false")
        << ",\"exact_quest_in_log\":" << (view.exactQuestInLog ? "true" : "false")
        << ",\"participant\":" << (view.questParticipant ? "true" : "false")
        << ",\"rewarded\":" << (view.rewarded ? "true" : "false")
        << ",\"core_can_complete\":" << (view.coreCanComplete ? "true" : "false")
        << ",\"objective_done\":" << (view.objectiveDone ? "true" : "false")
        << ",\"directive_active\":" << (view.directiveActive ? "true" : "false")
        << ",\"priming_action_started\":" << (view.primingActionStarted ? "true" : "false")
        << ",\"finisher_entry\":" << view.finisherEntry
        << ",\"finisher_stable_spawn_guid\":" << view.finisherStableSpawnGuid
        << ",\"finisher_runtime_guid\":" << view.finisherRuntimeGuid
        << ",\"has_exact_finisher\":" << (view.hasExactFinisher ? "true" : "false")
        << ",\"reaches_exact_finisher\":" << (view.reachesExactFinisher ? "true" : "false")
        << ",\"exact_relation_verified\":" << (view.exactRelationVerified ? "true" : "false")
        << ",\"core_completed\":" << (view.coreCompleted ? "true" : "false")
        << ",\"core_reward_confirmed\":" << (view.coreRewardConfirmed ? "true" : "false")
        << ",\"reason\":" << JsonString(view.reason) << '}';
}

std::string ErrorResponse(std::string const& error)
{
    return "{\"ok\":false,\"error\":" + JsonString(error) + "}";
}

bool IsLeagueMember(uint32 botGuid)
{
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT 1 FROM autowow_league_member WHERE character_guid = {} AND retired_at IS NULL", botGuid);
    return result != nullptr;
}

std::string TravelStatusName(TravelStatus status)
{
    switch (status)
    {
        case TRAVEL_STATUS_NONE: return "none";
        case TRAVEL_STATUS_PREPARE: return "prepare";
        case TRAVEL_STATUS_TRAVEL: return "travel";
        case TRAVEL_STATUS_WORK: return "work";
        case TRAVEL_STATUS_COOLDOWN: return "cooldown";
        case TRAVEL_STATUS_EXPIRED: return "expired";
        default: return "unknown";
    }
}

void AppendStrategyArray(std::ostringstream& out, std::vector<std::string> const& strategies)
{
    out << '[';
    for (std::size_t index = 0; index < strategies.size(); ++index)
    {
        if (index)
            out << ',';
        out << JsonString(strategies[index]);
    }
    out << ']';
}

std::string QuestAcquisitionSnapshotJson(Player* bot, PlayerbotAI* botAI, TravelTarget* travelTarget);

std::string OraclePhaseName(std::uint8_t value)
{
    switch (static_cast<QuestActionPhase>(value))
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
    }
    return "unknown";
}

std::string OracleFailureName(std::uint16_t value)
{
    switch (static_cast<QuestFailureReason>(value))
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
    }
    return "unknown";
}

void AppendOracleJson(std::ostringstream& out, uint32 botGuid)
{
    AutoWowOracleRuntime::OracleBotSnapshot snapshot;
    AutoWowOracleRuntime::GetOracleSnapshot(botGuid, snapshot);
    AutoWowOracleReceiptStore::StreamStatus const stream =
        AutoWowOracleRuntime::GetOracleStreamStatus();
    bool const available = snapshot.managed && snapshot.published;

    out << ",\"oracle\":{\"source\":\"AutoWowOracleRuntime\""
        << ",\"available\":" << (available ? "true" : "false")
        << ",\"status\":";
    if (!snapshot.managed)
        out << "\"not_managed\"";
    else if (!snapshot.published)
        out << "\"unavailable\"";
    else
        out << "\"published\"";
    out << ",\"unavailable_reason\":";
    if (snapshot.managed && snapshot.published)
        out << "null";
    else if (!snapshot.managed)
        out << "\"not_managed_by_runtime\"";
    else
        out << "\"runtime_state_not_published\"";
    out << ",\"world_tick\":";
    if (snapshot.published)
        out << snapshot.worldTick;
    else
        out << "null";
    out << ",\"phase\":";
    if (snapshot.phaseAvailable)
        out << JsonString(OraclePhaseName(snapshot.phase));
    else
        out << "null";
    out << ",\"job_id\":null"
        << ",\"controller_instance_id\":null"
        << ",\"lease_owner\":null"
        << ",\"lease_epoch\":";
    if (snapshot.leaseEpochAvailable)
        out << snapshot.leaseEpoch;
    else
        out << "null";
    out << ",\"lease_expires_ms\":null"
        << ",\"lease_expires_tick\":";
    if (snapshot.leaseExpiresTickAvailable)
        out << snapshot.leaseExpiresTick;
    else
        out << "null";
    out << ",\"operation_id\":null"
        << ",\"decision_id\":";
    if (snapshot.leaseAvailable)
        out << snapshot.decisionId;
    else
        out << "null";
    out << ",\"intent_id\":";
    if (snapshot.leaseAvailable)
        out << snapshot.intentId;
    else
        out << "null";
    out << ",\"operation\":";
    if (snapshot.operationAvailable)
        out << JsonString(std::string(AutoWowOracle::OperationCodeName(snapshot.operation)));
    else
        out << "null";
    out << ",\"quest_id\":";
    if (snapshot.questIdAvailable)
        out << snapshot.questId;
    else
        out << "null";
    out << ",\"objective_index\":";
    if (snapshot.objectiveIndexAvailable)
        out << snapshot.objectiveIndex;
    else
        out << "null";
    out << ",\"target_identity\":";
    if (snapshot.targetIdentityAvailable)
        out << snapshot.targetIdentity;
    else
        out << "null";
    out << ",\"route_segment\":null"
        << ",\"route_status\":null"
        << ",\"failure_code\":";
    if (snapshot.failureAvailable)
        out << JsonString(OracleFailureName(snapshot.failure));
    else
        out << "null";
    out << ",\"last_progress_age_ms\":null"
        << ",\"backoff_until\":null"
        << ",\"last_receipt_sequence\":";
    if (snapshot.lastReceiptSequence != 0)
        out << snapshot.lastReceiptSequence;
    else
        out << "null";
    out << ",\"completion_receipt_sequence\":";
    if (snapshot.completionReceiptSequence != 0)
        out << snapshot.completionReceiptSequence;
    else
        out << "null";
    out << ",\"completion_claimable\":"
        << (snapshot.completionClaimable ? "true" : "false");
    out << ",\"session_id\":" << JsonString(stream.sessionId)
        << ",\"process_session_id\":" << JsonString(stream.sessionId)
        << ",\"process_id\":" << stream.processId
        << ",\"sequence\":" << stream.sequence
        << ",\"durable_sequence\":" << stream.durableSequence
        << ",\"drop_count\":" << stream.dropCount
        << ",\"receipt_drop_count\":" << stream.dropCount
        << ",\"queue_depth\":" << stream.queueDepth
        << ",\"queue_capacity\":" << stream.queueCapacity
        << ",\"query_depth\":" << stream.queryDepth
        << ",\"query_capacity\":" << stream.queryCapacity
        << ",\"export_enabled\":" << (stream.exportEnabled ? "true" : "false")
        << ",\"durability_inconclusive\":"
        << ((stream.evidenceInconclusive || snapshot.evidenceInconclusive) ? "true" : "false")
        << '}';
}

std::string SnapshotJson(Player* bot, PlayerbotAI* botAI)
{
    Unit* target = nullptr;
    TravelTarget* travelTarget = nullptr;
    Group* group = bot->GetGroup();

    if (AiObjectContext* context = botAI->GetAiObjectContext())
    {
        target = context->GetValue<Unit*>("current target")->Get();
        travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
    }

    std::string travelDestination;
    std::string travelStatus = "none";
    if (travelTarget)
    {
        travelStatus = TravelStatusName(travelTarget->getStatus());
        if (TravelDestination* destination = travelTarget->getDestination())
            travelDestination = destination->getTitle();
    }

    bool const activityAllowed = botAI->AllowActivity(ALL_ACTIVITY, true);
    bool const detailedMoveAllowed = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
    AutoWowGather::RouteSnapshot const gatherRoute =
        AutoWowGather::GetRouteSnapshot(bot->GetGUID().GetCounter());

    std::string suppressionReason;
    if (botAI->IsAutoWowPaused())
        suppressionReason = "paused";
    else if (!bot->IsAlive())
        suppressionReason = "dead";
    else if (bot->IsInCombat())
        suppressionReason = "combat";
    else if (!activityAllowed)
        suppressionReason = "activity_policy";
    else if (gatherRoute.explicitWorker && !gatherRoute.idleReason.empty())
        suppressionReason = gatherRoute.idleReason;

    std::ostringstream strategyJson;
    strategyJson << "{\"combat\":";
    AppendStrategyArray(strategyJson, botAI->GetStrategies(BOT_STATE_COMBAT));
    strategyJson << ",\"non_combat\":";
    AppendStrategyArray(strategyJson, botAI->GetStrategies(BOT_STATE_NON_COMBAT));
    strategyJson << ",\"dead\":";
    AppendStrategyArray(strategyJson, botAI->GetStrategies(BOT_STATE_DEAD));
    strategyJson << '}';

    std::ostringstream out;
    out << "{\"guid\":" << bot->GetGUID().GetCounter()
        << ",\"name\":" << JsonString(bot->GetName())
        << ",\"paused\":" << (botAI->IsAutoWowPaused() ? "true" : "false")
        << ",\"alive\":" << (bot->IsAlive() ? "true" : "false")
        << ",\"combat\":" << (bot->IsInCombat() ? "true" : "false")
        << ",\"state\":" << JsonString(botAI->HandleRemoteCommand("state"))
        << ",\"action\":" << JsonString(botAI->HandleRemoteCommand("action"))
        << ",\"strategies\":" << strategyJson.str()
        << ",\"activity\":{\"allowed\":" << (activityAllowed ? "true" : "false")
        << ",\"detailed_move_allowed\":" << (detailedMoveAllowed ? "true" : "false")
        << ",\"suppression_reason\":" << JsonString(suppressionReason) << "}"
        << ",\"position\":{\"map\":" << bot->GetMapId()
        << ",\"instance\":" << bot->GetInstanceId()
        << ",\"zone\":" << bot->GetZoneId()
        << ",\"area\":" << bot->GetAreaId()
        << ",\"x\":" << bot->GetPositionX()
        << ",\"y\":" << bot->GetPositionY()
        << ",\"z\":" << bot->GetPositionZ()
        << ",\"o\":" << bot->GetOrientation() << "}"
        << ",\"movement\":{\"is_moving\":" << (bot->isMoving() ? "true" : "false")
        << ",\"run_speed_yards_per_second\":" << bot->GetSpeed(MOVE_RUN)
        << ",\"walk_speed_yards_per_second\":" << bot->GetSpeed(MOVE_WALK)
        << ",\"swim_speed_yards_per_second\":" << bot->GetSpeed(MOVE_SWIM) << "}"
        << ",\"health\":{\"current\":" << bot->GetHealth()
        << ",\"max\":" << bot->GetMaxHealth()
        << ",\"pct\":" << bot->GetHealthPct() << "}"
        << ",\"power\":{\"type\":" << static_cast<uint32>(bot->getPowerType())
        << ",\"current\":" << bot->GetPower(bot->getPowerType())
        << ",\"max\":" << bot->GetMaxPower(bot->getPowerType()) << "}"
        << ",\"progress\":{\"level\":" << static_cast<uint32>(bot->GetLevel())
        << ",\"xp\":" << bot->GetUInt32Value(PLAYER_XP)
        << ",\"money_copper\":" << bot->GetMoney() << "}"
        << ",\"target\":{\"guid\":" << (target ? target->GetGUID().GetCounter() : 0)
        << ",\"name\":" << JsonString(target ? target->GetName() : "")
        << ",\"health_pct\":" << (target ? target->GetHealthPct() : 0.0f) << "}"
        << ",\"group\":{\"members\":" << (group ? group->GetMembersCount() : 0)
        << ",\"leader_guid\":" << (group ? group->GetLeaderGUID().GetCounter() : 0) << "}"
        << ",\"travel\":{\"status\":" << JsonString(travelStatus)
        << ",\"destination\":" << JsonString(travelDestination) << "}"
        << ",\"quest_acquisition\":" << QuestAcquisitionSnapshotJson(bot, botAI, travelTarget)
        << ",\"gather_route\":{\"explicit_worker\":" << (gatherRoute.explicitWorker ? "true" : "false")
        << ",\"status\":" << JsonString(gatherRoute.status)
        << ",\"event\":" << JsonString(gatherRoute.event)
         << ",\"event_sequence\":" << gatherRoute.eventSequence
         << ",\"idle_reason\":" << JsonString(gatherRoute.idleReason)
          << ",\"miss_count\":" << gatherRoute.missCount
          << ",\"awaiting_credit\":" << (gatherRoute.awaitingCredit ? "true" : "false")
          << ",\"oracle_exact_source_only\":" << (gatherRoute.oracleExactSourceOnly ? "true" : "false")
          << ",\"requested_material_item_id\":" << gatherRoute.requestedMaterialItemId
          << ",\"gather_goal\":" << JsonString(std::string(
                 AutoWowOracle::GatherGoalName(gatherRoute.gatherGoal)))
          << ",\"credit_evidence\":" << JsonString(gatherRoute.creditEvidence)
          << ",\"source_yields_requested_material\":"
          << (gatherRoute.sourceYieldsRequestedMaterial ? "true" : "false")
         << ",\"source_guid\":" << gatherRoute.sourceGuid
         << ",\"attempted_at_seconds\":" << gatherRoute.interactionAttemptedAtSeconds
         << ",\"loot_response_delta\":" << gatherRoute.lootResponseDelta
         << ",\"store_loot_execution_delta\":" << gatherRoute.storeLootExecutionDelta
         << ",\"autostore_loot_packet_delta\":" << gatherRoute.autostoreLootPacketDelta
         << ",\"loot_release_packet_delta\":" << gatherRoute.lootReleasePacketDelta
          << ",\"loot_packet_item_delta\":" << gatherRoute.lootPacketItemDelta
          << ",\"loot_allowed_owner_slot_delta\":" << gatherRoute.lootAllowedOwnerSlotDelta
          << ",\"loot_slot_type_rejected_delta\":" << gatherRoute.lootSlotTypeRejectedDelta
          << ",\"loot_policy_rejected_delta\":" << gatherRoute.lootPolicyRejectedDelta
          << ",\"loot_missing_template_delta\":" << gatherRoute.lootMissingTemplateDelta
          << ",\"loot_bag_reserve_rejected_delta\":" << gatherRoute.lootBagReserveRejectedDelta
          << ",\"loot_admission_failure_mask\":" << gatherRoute.lootAdmissionFailureMask
          << ",\"loot_admission_failure\":"
          << JsonString(GatherLootAdmissionFailureName(gatherRoute.lootAdmissionFailureMask))
          << ",\"skill_before\":" << gatherRoute.skillBefore
          << ",\"skill_after\":" << gatherRoute.skillAfter
          << ",\"death_recovery\":{\"active\":"
          << (gatherRoute.deathRecovery.active ? "true" : "false")
          << ",\"phase\":" << JsonString(std::string(
                 AutoWowGather::DeathRecoveryPhaseName(gatherRoute.deathRecovery.phase)))
          << ",\"event\":" << JsonString(std::string(
                 AutoWowGather::DeathRecoveryEventName(gatherRoute.deathRecovery.event)))
          << ",\"event_sequence\":" << gatherRoute.deathRecovery.eventSequence
          << ",\"idle_reason\":" << JsonString(std::string(
                 AutoWowGather::DeathRecoveryIdleReasonName(gatherRoute.deathRecovery.idleReason)))
          << ",\"release_attempts\":" << gatherRoute.deathRecovery.releaseAttempts
          << ",\"route_attempts\":" << gatherRoute.deathRecovery.routeAttempts
          << ",\"reclaim_attempts\":" << gatherRoute.deathRecovery.reclaimAttempts << "}"
          << ",\"oracle_source\":{\"valid\":" << (gatherRoute.oracleSource.valid ? "true" : "false")
          << ",\"spawn_id\":" << gatherRoute.oracleSource.spawnId
          << ",\"entry\":" << gatherRoute.oracleSource.entry
          << ",\"map\":" << gatherRoute.oracleSource.mapId
          << ",\"instance\":" << gatherRoute.oracleSource.instanceId
          << ",\"material_item_id\":" << gatherRoute.oracleSource.materialItemId << "}"
          << ",\"candidate\":";
    if (!gatherRoute.hasCandidate)
        out << "null";
    else
        out << "{\"spawn_id\":" << gatherRoute.candidate.spawnId
            << ",\"entry\":" << gatherRoute.candidate.entry
            << ",\"name\":" << JsonString(gatherRoute.candidate.name)
            << ",\"profession\":" << JsonString(AutoWowGather::ProfessionName(gatherRoute.candidate.profession))
            << ",\"required_skill\":" << gatherRoute.candidate.requiredSkill
            << ",\"map\":" << gatherRoute.candidate.mapId
            << ",\"x\":" << gatherRoute.candidate.x
            << ",\"y\":" << gatherRoute.candidate.y
            << ",\"z\":" << gatherRoute.candidate.z << '}';
    out << '}';
    AppendOracleJson(out, bot->GetGUID().GetCounter());
    out << '}';
    return out.str();
}

struct OracleLogResponse
{
    bool success = false;
    std::string json;
};

char const* OracleLogCursorErrorName(AutoWowOracleReceiptStore::CursorError error)
{
    switch (error)
    {
        case AutoWowOracleReceiptStore::CursorError::None:
            return "";
        case AutoWowOracleReceiptStore::CursorError::StaleSession:
            return "oraclelog_stale_session";
        case AutoWowOracleReceiptStore::CursorError::StaleCursor:
            return "oraclelog_stale_cursor";
        case AutoWowOracleReceiptStore::CursorError::CursorAhead:
            return "oraclelog_cursor_ahead";
    }
    return "oraclelog_cursor_invalid";
}

OracleLogResponse BuildOracleLogResponse(std::string const& requestedSession, uint64 cursor)
{
    AutoWowOracleReceiptStore::StreamStatus const stream =
        AutoWowOracleRuntime::GetOracleStreamStatus();
    AutoWowOracleReceiptStore::Store const& store = AutoWowOracleRuntime::Runtime::instance().ReceiptStore();
    std::uint64_t const oldestSequence = store.QueryOldestSequence();
    AutoWowOracleReceiptStore::CursorError const cursorError =
        AutoWowOracleReceiptStore::ValidateCursor(
            requestedSession, stream.sessionId, cursor, oldestSequence, stream.sequence);

    auto makeEnvelope = [&](bool success, char const* error, bool truncated,
                            std::uint64_t nextCursor, std::string const& records) -> std::string
    {
        std::ostringstream out;
        out << "{\"ok\":" << (success ? "true" : "false")
            << ",\"command\":\"oraclelog\""
            << ",\"session_id\":" << JsonString(stream.sessionId)
            << ",\"process_session_id\":" << JsonString(stream.sessionId)
            << ",\"process_id\":" << stream.processId
            << ",\"cursor\":" << cursor
            << ",\"next_cursor\":" << nextCursor
            << ",\"oldest_sequence\":" << oldestSequence
            << ",\"sequence\":" << stream.sequence
            << ",\"durable_sequence\":" << stream.durableSequence
            << ",\"drop_count\":" << stream.dropCount
            << ",\"receipt_drop_count\":" << stream.dropCount
            << ",\"queue_depth\":" << stream.queueDepth
            << ",\"queue_capacity\":" << stream.queueCapacity
            << ",\"query_depth\":" << stream.queryDepth
            << ",\"query_capacity\":" << stream.queryCapacity
            << ",\"export_enabled\":" << (stream.exportEnabled ? "true" : "false")
            << ",\"durability_inconclusive\":"
            << (stream.evidenceInconclusive ? "true" : "false");
        if (error)
            out << ",\"error\":" << JsonString(error);
        out << ",\"truncated\":" << (truncated ? "true" : "false")
            << ",\"records\":" << records << '}';
        return out.str();
    };

    if (cursorError != AutoWowOracleReceiptStore::CursorError::None)
    {
        return {false, makeEnvelope(false, OracleLogCursorErrorName(cursorError), false,
                                    cursor, "[]")};
    }

    std::ostringstream records;
    records << '[';
    std::uint64_t nextCursor = cursor;
    std::size_t emitted = 0;
    bool first = true;
    bool truncated = false;
    for (std::size_t index = 0; index < store.QueryDepth(); ++index)
    {
        AutoWowOracleReceiptStore::ReceiptRecord const* record = store.QueryAt(index);
        if (!record || record->sequence <= cursor)
            continue;
        if (emitted >= AutoWowOracleReceiptStore::kMaxOracleLogBatch)
        {
            truncated = true;
            break;
        }

        if (!first)
            records << ',';
        first = false;
        records << AutoWowOracleReceiptStore::SerializeReceiptJson(
            *record, stream.sessionId, store.IsSequenceDurable(*record),
            stream.evidenceInconclusive);
        nextCursor = record->sequence;
        ++emitted;
    }
    records << ']';
    return {true, makeEnvelope(true, nullptr, truncated, nextCursor, records.str())};
}

enum class QuestAcquisitionState
{
    Pending,
    Accepted,
    Blocked
};

struct QuestAcquisitionSession;

uint64 QuestAcquisitionMonotonicNowMs()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::vector<std::int16_t> QuestGiverSpawnEventBindings(uint32 spawnId, bool creature)
{
    std::vector<std::int16_t> bindings;
    auto const& eventGuids = creature ? sGameEventMgr->GameEventCreatureGuids
                                      : sGameEventMgr->GameEventGameobjectGuids;
    std::int64_t const eventMapSize =
        static_cast<std::int64_t>(sGameEventMgr->GetEventMap().size());
    for (std::size_t internalIndex = 0; internalIndex < eventGuids.size(); ++internalIndex)
    {
        auto const& guids = eventGuids[internalIndex];
        if (std::find(guids.begin(), guids.end(), spawnId) == guids.end())
            continue;
        std::int64_t const signedEventId = static_cast<std::int64_t>(internalIndex) -
            eventMapSize + 1;
        if (signedEventId >= std::numeric_limits<std::int16_t>::min() &&
            signedEventId <= std::numeric_limits<std::int16_t>::max() && signedEventId != 0)
            bindings.push_back(static_cast<std::int16_t>(signedEventId));
    }
    return bindings;
}

std::vector<std::uint16_t> ActiveQuestGiverEvents()
{
    auto const& active = sGameEventMgr->GetActiveEventList();
    return {active.begin(), active.end()};
}

class QuestAcquisitionDestination final : public TravelDestination
{
public:
    explicit QuestAcquisitionDestination(QuestAcquisitionSession* session);

    Quest const* GetQuestTemplate() override;
    bool isActive(Player* bot) override;
    void onTargetExpiry(Player* bot) override;
    bool reconcileBeforeNativeExpiry() const override { return true; }
    bool isIn(WorldPosition* pos, float radius = 0.0f) override;
    std::string const getName() override { return "AutoWowQuestAcquisitionDestination"; }
    int32 getEntry() override;
    std::string const getTitle() override;

private:
    QuestAcquisitionSession* m_session;
};

struct QuestAcquisitionSession
{
    QuestAcquisitionSession(uint32 leaderGuidValue, uint32 questIdValue, int32 giverEntryValue,
                            WorldPosition const& giverPointValue, GuidPosition const& giverSpawnValue,
                            uint64 giverGuidValue, double routeDistanceValue, float creationMovementSpeed,
                            std::string capabilityValue)
        : leaderGuid(leaderGuidValue), questId(questIdValue), giverEntry(giverEntryValue),
          giverPoint(giverPointValue), giverSpawn(giverSpawnValue),
          persistentGiverGuid(giverSpawnValue.GetCounter()), giverGuid(giverGuidValue),
          routeDistance(routeDistanceValue), capability(std::move(capabilityValue)),
          nativeCadenceObservedAtMs(QuestAcquisitionMonotonicNowMs()),
          journey(nativeCadenceObservedAtMs, routeDistanceValue, creationMovementSpeed),
          routeDeadlineMs(journey.Deadline().DurationMs()), destination(this)
    {
    }

    void AdvanceNativeMovementTick(uint32 elapsedMs)
    {
        // UpdateAI's elapsedMs is a sparse map-frame delta and is not an elapsed wall-clock
        // authority for this one-second production cadence.
        (void)elapsedMs;
        uint64 const nowMs = QuestAcquisitionMonotonicNowMs();
        nativeMovementCadence.Advance(
            AutoWowQuestAcquisitionBridge::SaturatingMonotonicDeltaMs(
                nativeCadenceObservedAtMs, nowMs));
        nativeCadenceObservedAtMs = nowMs;
    }

    bool NativeMovementTickDue() const
    {
        return nativeMovementCadence.IsDue();
    }

    void ArmNativeMovementTick()
    {
        nativeMovementCadence.Arm();
    }

    bool ReleaseTerminalTravelTarget(Player* leader, TravelTarget* target)
    {
        if (terminalTravelTargetReleased)
            return false;

        // Consume the release token even if the exact target is already gone. This prevents a
        // later, unrelated target from being cleared by a stale terminal session.
        terminalTravelTargetReleased = true;
        if (leader && target && target->getDestination() == &destination)
            TravelMgr::instance().setNullTravelTarget(leader);
        return true;
    }

    bool IsReadyForNativeWorkHandoff() const
    {
        return state == QuestAcquisitionState::Pending && partyCohesionReady &&
            reason == "ready_to_accept";
    }

    void ClearTerminalMovementFeedback()
    {
        if (state == QuestAcquisitionState::Pending)
            return;

        // A successful kick is not a successful terminal outcome. Clear the live claim when the
        // session is accepted, blocked, expired, or loses its target so observers cannot mistake
        // historical movement for an active order.
        movementKickAccepted = false;
        journey.Lifecycle().SetMovementAccepted(false);
        movementFeedbackMoving = false;
        movementStep = "terminal";
        journey.Lifecycle().MarkTerminal();
        movementState = std::string(AutoWowQuestGiverTravel::SessionMovementStateName(
            journey.Lifecycle().State()));
        movementDiagnostics.actionResult = AutoWowQuestAcquisition::MovementActionResult::NotAttempted;
        movementResult = "terminal";
    }

    void SetBlocked(std::string newReason, bool partialParty = false)
    {
        state = QuestAcquisitionState::Blocked;
        reason = std::move(newReason);
        journey.Block(reason);
        if (partialParty)
            partyOutcome = "partial_party";
        ClearTerminalMovementFeedback();
    }

    void SetAccepted(std::string newReason)
    {
        state = QuestAcquisitionState::Accepted;
        reason = std::move(newReason);
        journey.Accept(reason);
        ClearTerminalMovementFeedback();
    }

    uint32 StaleGiverWaitMs() const
    {
        return journey.Lifecycle().StaleGiverWaitMs(SessionAgeMs());
    }

    uint32 PartyArrivalWaitMs() const
    {
        return journey.Lifecycle().PartyArrivalWaitMs(SessionAgeMs());
    }

    uint32 SessionAgeMs() const
    {
        return journey.Deadline().ElapsedMs(QuestAcquisitionMonotonicNowMs());
    }

    void BeginStaleGiverWait(uint32 elapsedMs)
    {
        ++staleGiverChecks;
        giverResolution = "stale";
        AutoWowQuestGiverTravel::SessionDecision const decision =
            journey.Lifecycle().ObserveGiverAvailability(
                elapsedMs, false);
        movementState = std::string(AutoWowQuestGiverTravel::SessionMovementStateName(decision.state));
        partyCohesionReady = false;
        reason = "stale_giver";
    }

    void ClearStaleGiverWait(uint32 elapsedMs)
    {
        AutoWowQuestGiverTravel::SessionDecision const decision =
            journey.Lifecycle().ObserveGiverAvailability(
                elapsedMs, true);
        movementState = std::string(AutoWowQuestGiverTravel::SessionMovementStateName(decision.state));
        if (giverResolution == "stale")
            giverResolution = "resolved";
        if (reason == "stale_giver")
            reason = "traveling_to_selected_giver";
    }

    void ApplyLiveMovementFeedback(Player* leader)
    {
        if (!leader)
            return;

        if (state != QuestAcquisitionState::Pending)
        {
            ClearTerminalMovementFeedback();
            return;
        }

        AutoWowQuestGiverTravel::MovementFeedback const* feedback =
            AutoWowQuestGiverTravel::ReadMovementFeedback(leaderGuid);
        if (!feedback || feedback->sequence <= movementFeedbackSequence)
            return;

        ApplyMovementFeedbackRecord(*feedback);
    }

    void ApplyMovementFeedbackRecord(AutoWowQuestGiverTravel::MovementFeedback const& record)
    {
        AutoWowQuestGiverTravel::MovementFeedback const* feedback = &record;
        if (feedback->sequence <= movementFeedbackSequence || state != QuestAcquisitionState::Pending)
            return;
        movementFeedbackSequence = feedback->sequence;
        journey.ApplyStagedMoverOutcome(*feedback);
        movementObservations = std::max(movementObservations, feedback->observations);
        movementAttempts = std::max(movementAttempts, feedback->movementAttempts);
        movementRejects = std::max(movementRejects, feedback->rejectedAttempts);
        if (feedback->result != AutoWowQuestGiverTravel::MovementFeedbackResult::Deferred)
            noProgressChecks = feedback->noProgressChecks;
        movementFeedbackMoving = feedback->moving;
        movementRemainingDistance = feedback->remainingDistance;
        movementStep = std::string(AutoWowQuestGiverTravel::StepKindName(feedback->step));
        movementResult = std::string(
            AutoWowQuestGiverTravel::MovementFeedbackResultName(feedback->result));
        if (AutoWowQuestGiverTravel::IsBoundedLocalEscapeRecoveryExhausted(
                movementStep, movementResult))
            boundedLocalEscapeRecoveryExhausted = true;
        if (feedback->result == AutoWowQuestGiverTravel::MovementFeedbackResult::Parked)
        {
            movementKickAccepted = false;
            journey.Lifecycle().SetMovementAccepted(false);
            movementState = "leader_parked_waiting_followers";
            movementDiagnostics.actionResult = AutoWowQuestAcquisition::MovementActionResult::NotAttempted;
        }
        else if (feedback->result == AutoWowQuestGiverTravel::MovementFeedbackResult::Deferred)
        {
            // A deferred refresh leaves the previously accepted staged move authoritative. It is
            // neither a new attempt nor a failure and must not consume retry/no-progress budget.
            movementState = "traveling";
            movementDiagnostics.actionResult = AutoWowQuestAcquisition::MovementActionResult::NotAttempted;
        }
        else if (feedback->result == AutoWowQuestGiverTravel::MovementFeedbackResult::NoProgress)
        {
            // The probe found no safe immediate segment. Keep this as bounded no-progress
            // observation so a transient route can recover without spending hard-rejection budget.
            movementKickAccepted = false;
            journey.Lifecycle().SetMovementAccepted(false);
            movementState = "traveling";
            movementDiagnostics.actionResult = AutoWowQuestAcquisition::MovementActionResult::NotAttempted;
        }
        else
        {
            movementKickAccepted =
                feedback->result == AutoWowQuestGiverTravel::MovementFeedbackResult::Accepted;
            journey.Lifecycle().SetMovementAccepted(movementKickAccepted);
            movementState = "traveling";
            movementDiagnostics.actionResult = movementKickAccepted
                ? AutoWowQuestAcquisition::MovementActionResult::Accepted
                : AutoWowQuestAcquisition::MovementActionResult::Rejected;

            if (movementKickAccepted && std::isfinite(feedback->remainingDistance))
            {
                if (!feedback->moving && std::isfinite(lastObservedMovementDistance) &&
                    feedback->remainingDistance >= lastObservedMovementDistance - 1.0f)
                    ++noProgressChecks;
                else
                    noProgressChecks = 0;
                lastObservedMovementDistance = feedback->remainingDistance;
            }
        }
    }

    bool HasStrongOffNavmeshEvidence() const
    {
        return AutoWowQuestGiverTravel::HasStrongOffNavmeshEvidence({
            movementDiagnostics.pathProbe.attempted,
            movementDiagnostics.pathProbe.pathType,
            pathProbeGroundLineReason,
            strongOffNavmeshProbeCount});
    }

    bool ShouldDeferForHearthRecovery() const
    {
        return state == QuestAcquisitionState::Pending &&
            boundedLocalEscapeRecoveryExhausted && HasStrongOffNavmeshEvidence();
    }

    bool TryHearthstoneRecovery(Player* leader, PlayerbotAI* botAI,
                                bool campaignTravelOwned, bool inFlight, bool targetLost)
    {
        if (state != QuestAcquisitionState::Pending)
            return false;

        std::string const originalReason = targetLost
            ? "travel_target_lost"
            : noProgressChecks >= AutoWowQuestGiverTravel::kMaxNoProgressChecks
                ? "movement_no_progress_timeout"
                : movementRejects >= AutoWowQuestGiverTravel::kMaxMovementRejects
                    ? "movement_retry_limit"
                    : "travel_target_lost";

        AiObjectContext* context = botAI ? botAI->GetAiObjectContext() : nullptr;
        Value<uint32>* hearthstoneCount = context
            ? context->GetValue<uint32>("item count", "hearthstone")
            : nullptr;
        AutoWowQuestGiverTravel::HearthRecoveryFacts const facts{
            true,
            leader && leader->IsAlive(),
            leader && leader->IsInCombat(),
            inFlight,
            leader && leader->IsBeingTeleported(),
            campaignTravelOwned,
            boundedLocalEscapeRecoveryExhausted,
            HasStrongOffNavmeshEvidence(),
            leader && leader->IsInWorld() && !leader->InBattleground(),
            hearthstoneCount && hearthstoneCount->Get() > 0,
            hearthstoneAttempts};
        AutoWowQuestGiverTravel::HearthRecoveryDecision const decision =
            AutoWowQuestGiverTravel::EvaluateHearthRecovery(facts);
        if (decision == AutoWowQuestGiverTravel::HearthRecoveryDecision::NotEligible)
            return false;

        hearthstoneOriginalTerminalReason = originalReason;
        hearthstoneStatus = std::string(
            AutoWowQuestGiverTravel::HearthRecoveryDecisionName(decision));
        if (decision == AutoWowQuestGiverTravel::HearthRecoveryDecision::Cooldown)
        {
            hearthstoneFailureReason = "attempt_budget_exhausted";
            lifecycleDecision = originalReason;
            SetBlocked(originalReason);
            return true;
        }

        if (decision == AutoWowQuestGiverTravel::HearthRecoveryDecision::Unavailable)
        {
            hearthstoneFailureReason = !facts.hearthstoneUseful
                ? "action_not_useful"
                : "item_unavailable";
            lifecycleDecision = originalReason;
            SetBlocked(originalReason);
            return true;
        }

        // Count the request before entering the action engine. Even a rejected action therefore
        // consumes the one-session budget and cannot be retried by a later bridge/status tick.
        ++hearthstoneAttempts;
        hearthstoneStatus = "requested";
        bool const accepted = botAI && botAI->DoSpecificAction(
            "hearthstone", Event("autowow off-navmesh quest recovery"), true);
        if (accepted)
        {
            hearthstoneStatus = "accepted";
            hearthstoneReplanRequired = true;
            lifecycleDecision = originalReason;
            // This is an action acceptance, never quest acceptance. Retire the acquisition so the
            // next normal Director tick can choose a fresh quest after the hearth relocation.
            SetBlocked("hearthstone_recovery_accepted_replan");
        }
        else
        {
            hearthstoneStatus = "unavailable";
            hearthstoneFailureReason = "action_rejected";
            lifecycleDecision = originalReason;
            SetBlocked(originalReason);
        }
        return true;
    }

    void ExpireFromNativeTravelTarget()
    {
        if (state != QuestAcquisitionState::Pending)
            return;
        lifecycleDecision = "acquisition_timeout";
        SetBlocked(lifecycleDecision);
    }

    bool ApplyLifecycleLimits()
    {
        if (journey.ExpireIfDue(QuestAcquisitionMonotonicNowMs()))
        {
            state = QuestAcquisitionState::Blocked;
            lifecycleDecision = "acquisition_timeout";
            reason = lifecycleDecision;
            ClearTerminalMovementFeedback();
            return true;
        }
        AutoWowQuestGiverTravel::LifecycleFacts const facts{
            SessionAgeMs(), routeDeadlineMs, movementAttempts,
            movementRejects, noProgressChecks, StaleGiverWaitMs(), PartyArrivalWaitMs()};
        AutoWowQuestGiverTravel::LifecycleDecision const decision =
            AutoWowQuestGiverTravel::EvaluateLifecycle(facts);
        if (decision == AutoWowQuestGiverTravel::LifecycleDecision::Continue)
            return false;

        // A status/snapshot poll may observe the terminal escape result before the native AI tick
        // gets to perform the emergency-action gate. Keep that narrow, strongly evidenced case
        // pending for one world-thread recovery opportunity; all other lifecycle failures remain
        // terminal immediately.
        if (ShouldDeferForHearthRecovery())
            return false;

        lifecycleDecision = std::string(AutoWowQuestGiverTravel::LifecycleDecisionName(decision));
        if (decision == AutoWowQuestGiverTravel::LifecycleDecision::BlockedPartyArrival)
            SetBlocked(lifecycleDecision, true);
        else
            SetBlocked(lifecycleDecision);
        return true;
    }

    uint32 RemainingLifecycleMs() const
    {
        return journey.Deadline().RemainingMs(QuestAcquisitionMonotonicNowMs());
    }

    void AlignInstalledTravelDeadline(TravelTarget* target)
    {
        if (!target || target->getDestination() != &destination ||
            target->getStatus() != TRAVEL_STATUS_TRAVEL)
            return;

        // TravelTarget normally derives TRAVEL expiry from straight-line distance (roughly 18.6m
        // for q954), while staged routing needs the policy's route allowance (roughly 28.7m).
        // One installation binds the native relative timer to the session's immutable absolute
        // deadline. Polling never calls this method, so later mount/slow/dismount changes cannot
        // recompute or extend either clock.
        uint32 const remainingMs = RemainingLifecycleMs();
        if (!remainingMs || nativeDeadlineAligned ||
            !journey.InstallTarget(*target, QuestAcquisitionMonotonicNowMs()))
            return;
        nativeDeadlineAligned = true;
        nativeTravelTimeLeftMs = target->getTimeLeft();
        nativeTravelDeadlineMs = target->getStatusDeadline();
        nativeTravelDeadlineAtMs = journey.Deadline().DeadlineAtMs();
    }

    void ObserveInstalledTravelDeadline(TravelTarget* target)
    {
        if (!target || target->getDestination() != &destination)
            return;
        nativeTravelTimeLeftMs = target->getStatus() == TRAVEL_STATUS_TRAVEL
            ? target->getTimeLeft()
            : 0;
        nativeTravelDeadlineMs = target->getStatusDeadline();
    }

    void ReconcileTravelTarget(TravelTarget* target)
    {
        if (state != QuestAcquisitionState::Pending)
            return;
        // Native expiry and the immutable session deadline represent the same terminal boundary.
        // Reconcile timeout before interpreting an expired target as a lost target.
        if (journey.ExpireIfDue(QuestAcquisitionMonotonicNowMs()) ||
            (target && target->getDestination() == &destination &&
             target->getStatus() == TRAVEL_STATUS_EXPIRED))
        {
            ExpireFromNativeTravelTarget();
            return;
        }
        ObserveInstalledTravelDeadline(target);
        if (target && target->getDestination() == &destination &&
            (target->getStatus() == TRAVEL_STATUS_TRAVEL ||
             target->getStatus() == TRAVEL_STATUS_WORK))
            return;

        // Preserve the pending session until the native tick can evaluate the complete hearth
        // gate. This does not authorize hearth by itself; strong path evidence and the action
        // availability/cooldown checks still have to pass there.
        if (ShouldDeferForHearthRecovery())
            return;

        lifecycleDecision = "travel_target_lost";
        SetBlocked(lifecycleDecision, true);
    }

    AutoWowQuestGiverTravel::GiverLookupState FindGiver(Player* leader, WorldObject*& giver)
    {
        using AutoWowQuestGiverTravel::GiverLookupState;
        giver = nullptr;
        if (!leader)
            return GiverLookupState::ConfirmedMissing;

        uint32 const spawnId = persistentGiverGuid;
        if (!spawnId || giverSpawn.GetCounter() != persistentGiverGuid)
            return GiverLookupState::ConfirmedMissing;
        bool const backedBySpawnData = giverEntry > 0
            ? sObjectMgr->GetCreatureData(spawnId) != nullptr
            : giverEntry < 0 && sObjectMgr->GetGameObjectData(spawnId) != nullptr;
        if (backedBySpawnData && leader->GetMapId() != giverPoint.GetMapId())
            return GiverLookupState::GridUnloaded;
        Map* map = leader->GetMap();
        bool const gridLoaded = map && map->IsGridLoaded(
            giverPoint.GetPositionX(), giverPoint.GetPositionY());
        float const visibilityRange = leader->GetVisibilityRange();
        float const giverDistance = leader->GetExactDist2d(
            giverPoint.GetPositionX(), giverPoint.GetPositionY());
        bool const withinResolutionRange = std::isfinite(visibilityRange) &&
            visibilityRange > 0.0f && std::isfinite(giverDistance) &&
            giverDistance <= visibilityRange;
        GiverLookupState const preResolution = FindGiverResolution(
            backedBySpawnData, gridLoaded, withinResolutionRange, false);
        if (preResolution != GiverLookupState::ConfirmedMissing || !backedBySpawnData)
            return preResolution;

        giver = giverSpawn.GetWorldObject();
        if (!giver || !giver->IsInWorld() || giver->GetMapId() != leader->GetMapId())
            return GiverLookupState::ConfirmedMissing;

        AutoWowQuestAcquisition::PersistentGiverBindingFacts const binding{
            spawnId,
            giverGuid,
            giver->GetGUID().GetCounter(),
            GetResolvedGiverSpawnId(giver),
            giverPoint.GetMapId(),
            giver->GetMapId(),
            giverEntry,
            giver->GetEntry(),
            giverSpawn.IsCreature(),
            giverSpawn.IsGameObject()};
        if (!AutoWowQuestAcquisition::IsMatchingPersistentGiver(binding))
            return GiverLookupState::ConfirmedMissing;

        // The persistent DB spawn is the route identity. Bind the runtime GUID only after the
        // exact loaded spawn has passed entry/type/map validation; interaction remains gated on
        // this resolved object below.
        giverGuid = binding.runtimeGiverGuid;
        return GiverLookupState::Resolved;
    }

    AutoWowQuestGiverTravel::GiverLookupState FindGiverResolution(
        bool spawnDataExists, bool gridLoaded, bool withinResolutionRange,
        bool liveObjectResolved) const
    {
        return AutoWowQuestGiverTravel::ClassifyGiverLookup(
            spawnDataExists, gridLoaded, withinResolutionRange, liveObjectResolved);
    }

    bool PollGiverResolution(AutoWowQuestGiverTravel::GiverLookupState giverState,
                             uint32 elapsedMs)
    {
        using AutoWowQuestGiverTravel::GiverLookupState;
        if (giverState == GiverLookupState::GridUnloaded)
        {
            ClearStaleGiverWait(elapsedMs);
            giverResolution = "grid_unloaded";
            reason = "traveling_to_unloaded_giver_grid";
            return false;
        }
        if (giverState == GiverLookupState::ConfirmedMissing)
        {
            BeginStaleGiverWait(elapsedMs);
            if (journey.Lifecycle().StaleGiverWaitMs(elapsedMs) >=
                AutoWowQuestGiverTravel::kMaxStaleGiverWaitMs)
            {
                lifecycleDecision = "stale_giver";
                SetBlocked(lifecycleDecision);
            }
            return false;
        }
        ClearStaleGiverWait(elapsedMs);
        return true;
    }

    bool PollGiverResolutionForTest(bool spawnDataExists, bool gridLoaded,
                                    bool liveObjectResolved, uint32 elapsedMs)
    {
        return PollGiverResolution(FindGiverResolution(
            spawnDataExists, gridLoaded, gridLoaded, liveObjectResolved), elapsedMs);
    }

    bool Poll(Player* leader, bool performAcceptance = true)
    {
        if (state != QuestAcquisitionState::Pending)
        {
            ClearTerminalMovementFeedback();
            return false;
        }
        ApplyLiveMovementFeedback(leader);
        if (!leader || leader->GetGUID().GetCounter() != leaderGuid)
        {
            SetBlocked("leader_unavailable", true);
            return false;
        }
        if (ApplyLifecycleLimits())
            return false;

        QuestStatus const current = leader->GetQuestStatus(questId);
        if (current == QUEST_STATUS_INCOMPLETE)
        {
            observedLeaderStatus = current;
            if (expectedPartyMembers > 0 && acceptedMembers >= expectedPartyMembers)
            {
                partyOutcome = "cohesive";
                SetAccepted("accepted_exact_quest");
            }
            else
                SetBlocked("partial_party_acceptance", true);
            return false;
        }
        if (current != QUEST_STATUS_NONE)
        {
            observedLeaderStatus = current;
            SetBlocked("quest_status_not_none");
            return false;
        }

        // The selected spawn is validated on every pending poll, including while the leader is
        // still en route. A missing exact spawn starts a wall-clock stale-giver wait immediately;
        // it cannot hide until the general acquisition deadline expires.
        WorldObject* giver = nullptr;
        AutoWowQuestGiverTravel::GiverLookupState const giverState = FindGiver(leader, giver);
        uint32 const giverObservationAtMs = SessionAgeMs();
        if (!PollGiverResolution(giverState, giverObservationAtMs))
        {
            // DB-backed unloaded grids remain pending under the immutable overall deadline and do
            // not consume stale wait. Confirmed missing objects use the bounded stale lifecycle.
            if (state != QuestAcquisitionState::Pending || ApplyLifecycleLimits())
                return false;
            return true;
        }

        WorldPosition leaderPosition(leader);
        partyCohesionReady = false;
        if (!destination.TravelDestination::isIn(
                &leaderPosition, INTERACTION_DISTANCE * 1.5f))
        {
            AutoWowQuestGiverTravel::SessionDecision const decision = journey.Lifecycle().ObserveParty(
                SessionAgeMs(), false, expectedPartyMembers, 0, false);
            movementState = std::string(
                AutoWowQuestGiverTravel::SessionMovementStateName(decision.state));
            return true;
        }

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
        {
            SetBlocked("party_leader_unavailable", true);
            return false;
        }

        partyArrivedMembers = 0;
        uint32 currentPartyMembers = 0;
        for (GroupReference* reference = group->GetFirstMember(); reference;
             reference = reference->next())
        {
            Player* member = reference->GetSource();
            if (!member || !member->IsAlive())
                continue;

            ++currentPartyMembers;
            WorldPosition memberPosition(member);
            if (destination.TravelDestination::isIn(
                    &memberPosition, INTERACTION_DISTANCE * 1.5f))
                ++partyArrivedMembers;
        }

        if (currentPartyMembers != expectedPartyMembers ||
            partyArrivedMembers < expectedPartyMembers)
        {
            partyOutcome = "waiting_for_party";
            AutoWowQuestGiverTravel::SessionDecision const decision = journey.Lifecycle().ObserveParty(
                SessionAgeMs(), true,
                expectedPartyMembers, partyArrivedMembers, performAcceptance);
            movementState = std::string(
                AutoWowQuestGiverTravel::SessionMovementStateName(decision.state));
            ++partyArrivalChecks;
            if (ApplyLifecycleLimits())
                return false;
            return true;
        }
        AutoWowQuestGiverTravel::SessionDecision const partyDecision = journey.Lifecycle().ObserveParty(
            SessionAgeMs(), true,
            expectedPartyMembers, partyArrivedMembers, performAcceptance);
        partyOutcome = "cohesive";
        partyCohesionReady = true;
        movementState = std::string(
            AutoWowQuestGiverTravel::SessionMovementStateName(partyDecision.state));
        if (!giver->hasQuest(questId) || !leader->CanInteractWithQuestGiver(giver))
        {
            BeginStaleGiverWait(SessionAgeMs());
            if (!ApplyLifecycleLimits())
                return true;
            return false;
        }

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || !leader->CanTakeQuest(quest, false) || !leader->CanAddQuest(quest, false))
        {
            SetBlocked("selected_quest_no_longer_eligible");
            return false;
        }

        giverGuid = giver->GetGUID().GetCounter();
        giverDistance = leader->GetDistance(giver);

        if (!partyDecision.issueAcceptance)
        {
            // Snapshot/status polling is deliberately observational. It may refresh lifecycle
            // and readiness telemetry, but it never emits a quest opcode or edits a target.
            reason = "ready_to_accept";
            return true;
        }

        auto acceptExact = [&](Player* member)
        {
            if (!member || !member->IsAlive())
                return;

            if (member->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            {
                ++eligibleMembers;
                ++acceptedMembers;
                return;
            }
            if (member->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
                !member->CanInteractWithQuestGiver(giver) || !member->CanTakeQuest(quest, false) ||
                !member->CanAddQuest(quest, false))
                return;

            ++eligibleMembers;
            WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST);
            packet << giver->GetGUID() << questId << uint32(0);
            packet.rpos(0);
            member->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);
            if (member->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                ++acceptedMembers;
        };

        // The leader is the acceptance postcondition. Eligible followers receive the same exact
        // packet only while standing at that same giver; no broad accept action is invoked.
        acceptExact(leader);
        if (group)
        {
            for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            {
                Player* member = reference->GetSource();
                if (member != leader)
                    acceptExact(member);
            }
        }

        observedLeaderStatus = leader->GetQuestStatus(questId);
        if (previousLeaderStatus == QUEST_STATUS_NONE && observedLeaderStatus == QUEST_STATUS_INCOMPLETE)
        {
            if (acceptedMembers >= expectedPartyMembers)
            {
                partyOutcome = "cohesive";
                SetAccepted("accepted_exact_quest");
            }
            else
                SetBlocked("partial_party_acceptance", true);
        }
        else
            SetBlocked("acceptance_postcondition_failed");
        return false;
    }

    uint32 leaderGuid = 0;
    uint32 questId = 0;
    int32 giverEntry = 0;
    WorldPosition giverPoint;
    GuidPosition giverSpawn;
    uint64 persistentGiverGuid = 0;
    uint64 giverGuid = 0;
    double routeDistance = 0.0;
    std::string capability;
    QuestAcquisitionState state = QuestAcquisitionState::Pending;
    QuestStatus previousLeaderStatus = QUEST_STATUS_NONE;
    QuestStatus observedLeaderStatus = QUEST_STATUS_NONE;
    std::string reason = "traveling_to_selected_giver";
    std::string giverResolution = "resolved";
    std::string movementActivation = "not_attempted";
    // Kept under the existing wire name for compatibility; ApplyLiveMovementFeedback overwrites
    // this with the latest staged-mover result, so it is no longer an initial-only claim.
    bool movementKickAccepted = false;
    AutoWowQuestAcquisition::MovementDiagnosticsFacts movementDiagnostics;
    std::string pathProbeReason = "not_attempted";
    std::string pathProbeMode = "not_attempted";
    std::string pathProbeNavmeshReason;
    std::string pathProbeGroundLineReason;
    float giverDistance = -1.0f;
    uint32 arrivalChecks = 0;
    uint32 expectedPartyMembers = 0;
    uint32 partyArrivedMembers = 0;
    uint32 partyArrivalChecks = 0;
    uint32 staleGiverChecks = 0;
    uint32 movementObservations = 0;
    uint32 movementAttempts = 0;
    uint32 movementRejects = 0;
    uint32 noProgressChecks = 0;
    uint64 movementFeedbackSequence = 0;
    uint64 nativeCadenceObservedAtMs = 0;
    AutoWowQuestGiverTravel::QuestAcquisitionJourneyRuntime journey;
    uint32 routeDeadlineMs;
    uint32 nativeTravelTimeLeftMs = 0;
    uint32 nativeTravelDeadlineMs = 0;
    uint64 nativeTravelDeadlineAtMs = 0;
    bool nativeDeadlineAligned = false;
    uint32 eligibleMembers = 0;
    uint32 acceptedMembers = 0;
    bool movementFeedbackMoving = false;
    bool partyCohesionReady = false;
    float movementRemainingDistance = -1.0f;
    float lastObservedMovementDistance = -1.0f;
    std::string movementStep = "not_attempted";
    std::string movementResult = "not_attempted";
    std::uint32_t strongOffNavmeshProbeCount = 0;
    bool boundedLocalEscapeRecoveryExhausted = false;
    std::uint32_t hearthstoneAttempts = 0;
    std::string hearthstoneStatus = "not_attempted";
    std::string hearthstoneFailureReason;
    std::string hearthstoneOriginalTerminalReason;
    bool hearthstoneReplanRequired = false;
    std::string movementState = "traveling";
    std::string lifecycleDecision = "continue";
    std::string partyOutcome = "waiting_for_party";
    AutoWowQuestAcquisitionBridge::TickCadence nativeMovementCadence{
        AutoWowQuestAcquisitionBridge::kNativeTickCadenceMs};
    bool terminalTravelTargetReleased = false;
    QuestAcquisitionDestination destination;
};

QuestAcquisitionDestination::QuestAcquisitionDestination(QuestAcquisitionSession* session)
    : TravelDestination(INTERACTION_DISTANCE, INTERACTION_DISTANCE * 3.0f), m_session(session)
{
    addPoint(&m_session->giverPoint);
    // Do not let the native five-minute destination timer kill a healthy long staged route (q954
    // needs more than 24 short segments). The session still applies its route-aware deadline and
    // observes any native target expiry; no forced-target bypass is used.
    setExpireDelay(AutoWowQuestGiverTravel::kMaxAcquisitionLifetimeMs);
    setCooldownDelay(1000);
}

Quest const* QuestAcquisitionDestination::GetQuestTemplate()
{
    return m_session ? sObjectMgr->GetQuestTemplate(m_session->questId) : nullptr;
}

bool QuestAcquisitionDestination::isActive(Player* bot)
{
    return m_session && m_session->Poll(bot, true);
}

void QuestAcquisitionDestination::onTargetExpiry(Player* /*bot*/)
{
    if (m_session)
        m_session->ExpireFromNativeTravelTarget();
}

bool QuestAcquisitionDestination::isIn(WorldPosition* pos, float radius)
{
    return m_session && m_session->partyCohesionReady && TravelDestination::isIn(pos, radius);
}

int32 QuestAcquisitionDestination::getEntry()
{
    return m_session ? m_session->giverEntry : 0;
}

std::string const QuestAcquisitionDestination::getTitle()
{
    if (!m_session)
        return "quest acquisition unavailable";
    return "quest acquisition " + std::to_string(m_session->questId) + " giver " +
        std::to_string(m_session->giverEntry);
}

std::unordered_map<uint32, std::unique_ptr<QuestAcquisitionSession>> questAcquisitionSessions;

struct QuestAcquisitionPathText
{
    std::string reason = "not_attempted";
    std::string mode = "not_attempted";
    std::string navmeshReason;
    std::string groundLineReason;
};

void CaptureQuestAcquisitionMovement(
    QuestAcquisitionSession const& session, Player* bot, PlayerbotAI* botAI,
    TravelTarget* /*travelTarget*/, AutoWowQuestAcquisition::MovementActionResult actionResult,
    AutoWowQuestAcquisition::MovementOrderFacts const& orderFacts,
    AutoWowQuestAcquisition::MovementDiagnosticsFacts& facts, QuestAcquisitionPathText& pathText,
    bool probePath)
{
    using namespace AutoWowQuestAcquisition;

    facts = {};
    facts.order = orderFacts;
    facts.actionResult = actionResult;
    pathText = {};
    if (!bot || !botAI)
        return;

    facts.inCombat = bot->IsInCombat();
    facts.canMove = botAI->CanMove();
    facts.inFlight = bot->IsInFlight() ||
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE;
    facts.controlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    facts.beingTeleported = bot->IsBeingTeleported();
    facts.travelActivityAllowed = botAI->AllowActivity(TRAVEL_ACTIVITY, true);
    facts.detailedMoveAllowed = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
    facts.isMoving = bot->isMoving();

    if (AiObjectContext* context = botAI->GetAiObjectContext())
    {
        facts.canMoveAround = context->GetValue<bool>("can move around")->Get();
        facts.canLoot = context->GetValue<bool>("can loot")->Get();

        LastMovement& lastMovement = context->GetValue<LastMovement&>("last movement")->Get();
        uint32 const now = getMSTime();
        facts.waitingForNormalMove =
            !(MovementPriority::MOVEMENT_NORMAL > lastMovement.priority) &&
            lastMovement.lastdelayTime + lastMovement.msTime > now;
    }

    // Keep the probe strictly observational. It is useful even after a forced target has been
    // cleared, so use the session's immutable giver point rather than requiring a live target.
    if (probePath && bot->IsInWorld() && bot->GetMapId() == session.giverPoint.GetMapId())
    {
        AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
            bot, session.giverPoint.GetPositionX(), session.giverPoint.GetPositionY(),
            session.giverPoint.GetPositionZ());
        facts.pathProbe.attempted = true;
        facts.pathProbe.safe = probe.safe;
        facts.pathProbe.normalPathType = (probe.pathType & PATHFIND_NORMAL) != 0;
        facts.pathProbe.pathType = probe.pathType;
        facts.pathProbe.pointCount = probe.path.size();
        facts.pathProbe.endpointDistance = probe.endpointDistance;
        facts.pathProbe.pathLength = probe.pathLength;
        pathText.reason = probe.reason;
        pathText.mode = probe.mode;
        pathText.navmeshReason = probe.navmeshReason;
        pathText.groundLineReason = probe.groundLineReason;
    }
}

AutoWowQuestAcquisition::MovementOrderFacts PassiveQuestAcquisitionOrderFacts(
    QuestAcquisitionSession const& session, PlayerbotAI* botAI, TravelTarget* travelTarget);

void ObserveQuestAcquisitionHearthEvidence(
    QuestAcquisitionSession& session, Player* bot, PlayerbotAI* botAI,
    TravelTarget* travelTarget)
{
    if (!bot || !botAI || !bot->IsInWorld() ||
        bot->GetMapId() != session.giverPoint.GetMapId())
    {
        session.strongOffNavmeshProbeCount = 0;
        return;
    }

    AutoWowQuestAcquisition::MovementOrderFacts const orderFacts =
        PassiveQuestAcquisitionOrderFacts(session, botAI, travelTarget);
    AutoWowQuestAcquisition::MovementDiagnosticsFacts facts;
    QuestAcquisitionPathText pathText;
    CaptureQuestAcquisitionMovement(
        session, bot, botAI, travelTarget, session.movementDiagnostics.actionResult,
        orderFacts, facts, pathText, true);

    session.movementDiagnostics = facts;
    session.pathProbeReason = std::move(pathText.reason);
    session.pathProbeMode = std::move(pathText.mode);
    session.pathProbeNavmeshReason = std::move(pathText.navmeshReason);
    session.pathProbeGroundLineReason = std::move(pathText.groundLineReason);

    AutoWowQuestGiverTravel::OffNavmeshProbeFacts const probeFacts{
        facts.pathProbe.attempted,
        facts.pathProbe.pathType,
        session.pathProbeGroundLineReason,
        session.strongOffNavmeshProbeCount};
    if (AutoWowQuestGiverTravel::IsStrongOffNavmeshProbe(probeFacts))
        ++session.strongOffNavmeshProbeCount;
    else
        session.strongOffNavmeshProbeCount = 0;
}

AutoWowQuestAcquisition::MovementOrderFacts PassiveQuestAcquisitionOrderFacts(
    QuestAcquisitionSession const& session, PlayerbotAI* botAI, TravelTarget* travelTarget)
{
    AutoWowQuestAcquisition::MovementOrderFacts facts;
    if (!botAI || !travelTarget)
        return facts;

    facts.targetInstalled = travelTarget->getDestination() == &session.destination;
    facts.travelStatus = travelTarget->getStatus() == TRAVEL_STATUS_TRAVEL;
    facts.forced = travelTarget->isForced();
    facts.travelStrategy = botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT);
    // TravelTarget::isTraveling() can transition or clear a target. A snapshot must remain
    // observational, so status/strategy are the passive activation gates here. `forced` is
    // reported for diagnostics only; quest acquisition deliberately leaves it false so native
    // expiry and destination validity cannot be bypassed.
    facts.actionActivated = facts.targetInstalled && facts.travelStatus && facts.travelStrategy;
    return facts;
}

void AppendJsonNumber(std::ostringstream& out, double value)
{
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}

std::string QuestAcquisitionMovementJson(
    AutoWowQuestAcquisition::MovementDiagnosticsFacts const& facts,
    QuestAcquisitionPathText const& pathText)
{
    using namespace AutoWowQuestAcquisition;
    PathProbeDisposition const pathDisposition = ClassifyPathProbe(facts.pathProbe);
    MovementDiagnosticCategory const movementCategory = ClassifyMovementDiagnostics(facts);

    std::ostringstream out;
    out << "{\"category\":" << JsonString(std::string(MovementDiagnosticCategoryName(movementCategory)))
        << ",\"action_result\":"
        << JsonString(std::string(MovementActionResultName(facts.actionResult)))
        << ",\"in_combat\":" << (facts.inCombat ? "true" : "false")
        << ",\"can_move\":" << (facts.canMove ? "true" : "false")
        << ",\"can_move_around\":" << (facts.canMoveAround ? "true" : "false")
        << ",\"travel_activity_allowed\":" << (facts.travelActivityAllowed ? "true" : "false")
        << ",\"detailed_move_allowed\":" << (facts.detailedMoveAllowed ? "true" : "false")
        << ",\"in_flight\":" << (facts.inFlight ? "true" : "false")
        << ",\"controlled\":" << (facts.controlled ? "true" : "false")
        << ",\"being_teleported\":" << (facts.beingTeleported ? "true" : "false")
        << ",\"can_loot\":" << (facts.canLoot ? "true" : "false")
        << ",\"is_moving\":" << (facts.isMoving ? "true" : "false")
        << ",\"waiting_for_normal_move\":" << (facts.waitingForNormalMove ? "true" : "false")
        << ",\"target_installed\":" << (facts.order.targetInstalled ? "true" : "false")
        << ",\"travel_status_active\":" << (facts.order.travelStatus ? "true" : "false")
        << ",\"forced\":" << (facts.order.forced ? "true" : "false")
        << ",\"travel_strategy\":" << (facts.order.travelStrategy ? "true" : "false")
        << ",\"action_activated\":" << (facts.order.actionActivated ? "true" : "false")
        << ",\"path_probe\":{\"attempted\":" << (facts.pathProbe.attempted ? "true" : "false")
        << ",\"safe\":" << (facts.pathProbe.safe ? "true" : "false")
        << ",\"normal_path_type\":" << (facts.pathProbe.normalPathType ? "true" : "false")
        << ",\"path_type\":" << facts.pathProbe.pathType
        << ",\"point_count\":" << facts.pathProbe.pointCount
        << ",\"endpoint_distance\":";
    AppendJsonNumber(out, facts.pathProbe.endpointDistance);
    out << ",\"path_length\":";
    AppendJsonNumber(out, facts.pathProbe.pathLength);
    out << ",\"endpoint_complete\":" << (IsPathEndpointComplete(facts.pathProbe) ? "true" : "false")
        << ",\"disposition\":"
        << JsonString(std::string(PathProbeDispositionName(pathDisposition)))
        << ",\"reason\":" << JsonString(pathText.reason)
        << ",\"navmesh_reason\":" << JsonString(pathText.navmeshReason)
        << ",\"ground_line_reason\":" << JsonString(pathText.groundLineReason) << "}}";
    return out.str();
}

char const* QuestAcquisitionStateName(QuestAcquisitionState state)
{
    switch (state)
    {
        case QuestAcquisitionState::Pending: return "issued";
        case QuestAcquisitionState::Accepted: return "accepted";
        case QuestAcquisitionState::Blocked: return "blocked";
        default: return "blocked";
    }
}

void AppendTravelMgrDiagnostics(std::ostringstream& out, uint32 leaderGuid)
{
    AutoWowQuestGiverTravel::TravelMgrDiagnostics const* diagnostics =
        AutoWowQuestGiverTravel::ReadTravelMgrDiagnostics(leaderGuid);
    out << ",\"travel_mgr_diagnostics\":{\"present\":"
            << (diagnostics ? "true" : "false");
    if (diagnostics)
    {
        out << ",\"unattached_start_allowed\":"
            << (diagnostics->unattachedStartAllowed ? "true" : "false")
            << ",\"path_point_count\":" << diagnostics->pathPointCount
            << ",\"scan_points_considered\":" << diagnostics->scanPointsConsidered
            << ",\"same_map_walk_points\":" << diagnostics->sameMapWalkPoints
            << ",\"distance_eligible_points\":" << diagnostics->distanceEligiblePoints
            << ",\"fresh_probe_attempts\":" << diagnostics->freshProbeAttempts
            << ",\"fresh_probe_accepts\":" << diagnostics->freshProbeAccepts
            << ",\"selected\":" << (diagnostics->selected ? "true" : "false")
            << ",\"selected_route_index\":" << diagnostics->selectedRouteIndex
            << ",\"selected_distance\":";
        AppendJsonNumber(out, diagnostics->selectedDistance);
        out << ",\"nearest_route_distance\":";
        AppendJsonNumber(out, diagnostics->nearestRouteDistance);
        out << ",\"farthest_route_distance\":";
        AppendJsonNumber(out, diagnostics->farthestRouteDistance);
        out << ",\"reason\":" << JsonString(std::string(
            AutoWowQuestGiverTravel::TravelMgrDiagnosticReasonName(diagnostics->reason)));
    }
    out << '}';
}

void AppendStagedEntryDiagnostics(std::ostringstream& out, uint32 botGuid)
{
    AutoWowQuestGiverTravel::StagedEntryDiagnostics const* diagnostics =
        AutoWowQuestGiverTravel::ReadStagedEntryDiagnostics(botGuid);
    out << ",\"staged_entry_diagnostics\":{\"present\":"
        << (diagnostics ? "true" : "false");
    if (diagnostics)
    {
        AutoWowQuestGiverTravel::StagedEntryFacts const& facts = diagnostics->facts;
        out << ",\"decision\":" << JsonString(std::string(
            AutoWowQuestGiverTravel::StagedEntryDecisionName(diagnostics->decision)))
            << ",\"facts\":{\"activity_allowed\":"
            << (facts.activityAllowed ? "true" : "false")
            << ",\"exact_quest_acquisition_target\":"
            << (facts.exactQuestAcquisitionTarget ? "true" : "false")
            << ",\"travel_status_active\":"
            << (facts.travelStatusActive ? "true" : "false")
            << ",\"in_flight\":" << (facts.inFlight ? "true" : "false")
            << ",\"flying\":" << (facts.flying ? "true" : "false")
            << ",\"moving\":" << (facts.moving ? "true" : "false")
            << ",\"can_move_around\":" << (facts.canMoveAround ? "true" : "false")
            << ",\"loot_possible\":" << (facts.lootPossible ? "true" : "false") << '}';
    }
    out << '}';
}

void AppendHearthRecoveryTelemetry(std::ostringstream& out,
                                   QuestAcquisitionSession const& session)
{
    out << ",\"hearthstone_recovery\":{\"status\":"
        << JsonString(session.hearthstoneStatus)
        << ",\"attempts\":" << session.hearthstoneAttempts
        << ",\"max_attempts\":" << AutoWowQuestGiverTravel::kMaxHearthRecoveryAttempts
        << ",\"bounded_local_escape_exhausted\":"
        << (session.boundedLocalEscapeRecoveryExhausted ? "true" : "false")
        << ",\"strong_off_navmesh_probes\":" << session.strongOffNavmeshProbeCount
        << ",\"strong_off_navmesh_evidence\":"
        << (session.HasStrongOffNavmeshEvidence() ? "true" : "false")
        << ",\"original_terminal_reason\":"
        << JsonString(session.hearthstoneOriginalTerminalReason)
        << ",\"failure_reason\":" << JsonString(session.hearthstoneFailureReason)
        << ",\"replan_required\":"
        << (session.hearthstoneReplanRequired ? "true" : "false")
        // ObserverControl intentionally has no shared read-status API. Do not imply visual
        // corroboration when an observer relocation request failed or was never queried here.
        << ",\"observer_corroboration\":\"not_observed\"}";
}

std::string QuestAcquisitionResponse(QuestAcquisitionSession const& session)
{
    QuestAcquisitionPathText const pathText{
        session.pathProbeReason, session.pathProbeMode, session.pathProbeNavmeshReason,
        session.pathProbeGroundLineReason};
    bool const liveMovementAccepted =
        session.state == QuestAcquisitionState::Pending &&
        session.journey.Lifecycle().MovementAccepted();
    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"quest\",\"phase\":\"acquire\",\"state\":"
        << JsonString(QuestAcquisitionStateName(session.state))
        << ",\"guid\":" << session.leaderGuid
        << ",\"quest_id\":" << session.questId
        << ",\"capability_class\":" << JsonString(session.capability)
        << ",\"giver_entry\":" << session.giverEntry
        << ",\"giver_spawn_guid\":" << session.persistentGiverGuid
        << ",\"giver_guid\":" << session.giverGuid
        << ",\"giver_map\":" << session.giverPoint.GetMapId()
        << ",\"giver_x\":" << session.giverPoint.GetPositionX()
        << ",\"giver_y\":" << session.giverPoint.GetPositionY()
        << ",\"giver_z\":" << session.giverPoint.GetPositionZ()
        << ",\"route_distance\":" << session.routeDistance
        << ",\"loaded_giver_distance\":" << session.giverDistance
        << ",\"giver_resolution\":" << JsonString(session.giverResolution)
        << ",\"movement_activation\":" << JsonString(session.movementActivation)
        << ",\"movement_state\":" << JsonString(session.movementState)
        << ",\"movement_kick_accepted\":" << (liveMovementAccepted ? "true" : "false")
        << ",\"movement_feedback\":{\"sequence\":" << session.movementFeedbackSequence
        << ",\"observations\":" << session.movementObservations
        << ",\"attempts\":" << session.movementAttempts
        << ",\"rejected_attempts\":" << session.movementRejects
        << ",\"latest_step\":" << JsonString(session.movementStep)
        << ",\"latest_result\":" << JsonString(session.movementResult)
        << ",\"latest_accepted\":" << (liveMovementAccepted ? "true" : "false")
        << ",\"moving\":" << (session.movementFeedbackMoving ? "true" : "false")
        << ",\"remaining_distance\":";
    AppendJsonNumber(out, session.movementRemainingDistance);
    out << "}";
    AppendTravelMgrDiagnostics(out, session.leaderGuid);
    AppendStagedEntryDiagnostics(out, session.leaderGuid);
    out << ",\"movement_action_result\":"
        << JsonString(std::string(AutoWowQuestAcquisition::MovementActionResultName(
            session.movementDiagnostics.actionResult)))
        << ",\"movement_category\":"
        << JsonString(std::string(AutoWowQuestAcquisition::MovementDiagnosticCategoryName(
            AutoWowQuestAcquisition::ClassifyMovementDiagnostics(session.movementDiagnostics))))
        << ",\"movement\":"
        << QuestAcquisitionMovementJson(session.movementDiagnostics, pathText)
        << ",\"previous_status\":" << static_cast<uint32>(session.previousLeaderStatus)
        << ",\"observed_status\":" << static_cast<uint32>(session.observedLeaderStatus)
        << ",\"eligible_members\":" << session.eligibleMembers
        << ",\"accepted_members\":" << session.acceptedMembers
        << ",\"party\":{\"expected\":" << session.expectedPartyMembers
        << ",\"arrived\":" << session.partyArrivedMembers
        << ",\"arrival_checks\":" << session.partyArrivalChecks
        << ",\"wait_ms\":" << session.PartyArrivalWaitMs()
        << ",\"outcome\":" << JsonString(session.partyOutcome) << "}"
        << ",\"lifecycle\":{\"decision\":" << JsonString(session.lifecycleDecision)
        << ",\"status_observations\":" << session.journey.Lifecycle().Observations()
        << ",\"movement_observations\":" << session.movementObservations
        << ",\"movement_attempts\":" << session.movementAttempts
        << ",\"movement_rejects\":" << session.movementRejects
        << ",\"no_progress_checks\":" << session.noProgressChecks
        << ",\"stale_giver_checks\":" << session.staleGiverChecks
        << ",\"party_arrival_checks\":" << session.partyArrivalChecks
        << ",\"stale_giver_wait_ms\":" << session.StaleGiverWaitMs()
        << ",\"party_arrival_wait_ms\":" << session.PartyArrivalWaitMs()
        << ",\"route_deadline_ms\":" << session.routeDeadlineMs
        << ",\"native_travel_time_left_ms\":" << session.nativeTravelTimeLeftMs
        << ",\"native_status_deadline_elapsed_ms\":" << session.nativeTravelDeadlineMs
        << ",\"session_deadline_at_ms\":" << session.journey.Deadline().DeadlineAtMs()
        << ",\"native_aligned_deadline_at_ms\":" << session.nativeTravelDeadlineAtMs
        << ",\"age_ms\":" << session.SessionAgeMs() << "}"
        ;
    AppendHearthRecoveryTelemetry(out, session);
    out
        << ",\"journey_runtime\":" << session.journey.TelemetryJson("response")
        << ",\"reason\":" << JsonString(session.reason) << '}';
    return out.str();
}

std::string QuestAcquisitionSnapshotJsonForSession(
    QuestAcquisitionSession& session, Player* bot, PlayerbotAI* botAI, TravelTarget* travelTarget,
    AutoWowQuestAcquisition::MovementOrderFacts const* explicitOrderFacts = nullptr)
{
    // Pull the latest world-thread mover result before exposing telemetry. The initial kick is
    // not authoritative once staged movement has started retrying.
    if (session.state == QuestAcquisitionState::Pending && bot && botAI)
        ObserveQuestAcquisitionHearthEvidence(session, bot, botAI, travelTarget);
    session.ReconcileTravelTarget(travelTarget);
    if (session.state == QuestAcquisitionState::Pending && bot)
        session.Poll(bot, false);
    session.ApplyLiveMovementFeedback(bot);
    bool const liveMovementAccepted =
        session.state == QuestAcquisitionState::Pending &&
        session.journey.Lifecycle().MovementAccepted();
    // Production snapshots always derive these facts from the live bot AI. The bridge lifecycle
    // scenario has no Engine, so its test seam supplies already-observed passive facts explicitly
    // instead of invoking PlayerbotAI::HasStrategy on an intentionally lightweight AI.
    AutoWowQuestAcquisition::MovementOrderFacts const orderFacts = explicitOrderFacts
        ? *explicitOrderFacts
        : PassiveQuestAcquisitionOrderFacts(session, botAI, travelTarget);

    AutoWowQuestAcquisition::MovementDiagnosticsFacts facts;
    QuestAcquisitionPathText pathText;
    CaptureQuestAcquisitionMovement(
        session, bot, botAI, travelTarget, session.movementDiagnostics.actionResult, orderFacts,
        facts, pathText, true);

    std::ostringstream out;
    out << "{\"active\":true"
        << ",\"state\":" << JsonString(QuestAcquisitionStateName(session.state))
        << ",\"quest_id\":" << session.questId
        << ",\"giver_entry\":" << session.giverEntry
        << ",\"giver_spawn_guid\":" << session.persistentGiverGuid
        << ",\"giver_guid\":" << session.giverGuid
        << ",\"movement_state\":" << JsonString(session.movementState)
        << ",\"movement_feedback\":{\"sequence\":" << session.movementFeedbackSequence
        << ",\"observations\":" << session.movementObservations
        << ",\"attempts\":" << session.movementAttempts
        << ",\"rejected_attempts\":" << session.movementRejects
        << ",\"latest_step\":" << JsonString(session.movementStep)
        << ",\"latest_result\":" << JsonString(session.movementResult)
        << ",\"latest_accepted\":" << (liveMovementAccepted ? "true" : "false")
        << ",\"moving\":" << (session.movementFeedbackMoving ? "true" : "false")
        << ",\"remaining_distance\":";
    AppendJsonNumber(out, session.movementRemainingDistance);
    out << "}";
    AppendTravelMgrDiagnostics(out, session.leaderGuid);
    AppendStagedEntryDiagnostics(out, session.leaderGuid);
    out << ",\"party\":{\"expected\":" << session.expectedPartyMembers
        << ",\"arrived\":" << session.partyArrivedMembers
        << ",\"arrival_checks\":" << session.partyArrivalChecks
        << ",\"wait_ms\":" << session.PartyArrivalWaitMs()
        << ",\"outcome\":" << JsonString(session.partyOutcome) << "}"
        << ",\"lifecycle\":{\"decision\":" << JsonString(session.lifecycleDecision)
        << ",\"status_observations\":" << session.journey.Lifecycle().Observations()
        << ",\"movement_observations\":" << session.movementObservations
        << ",\"movement_attempts\":" << session.movementAttempts
        << ",\"movement_rejects\":" << session.movementRejects
        << ",\"no_progress_checks\":" << session.noProgressChecks
        << ",\"stale_giver_checks\":" << session.staleGiverChecks
        << ",\"party_arrival_checks\":" << session.partyArrivalChecks
        << ",\"stale_giver_wait_ms\":" << session.StaleGiverWaitMs()
        << ",\"party_arrival_wait_ms\":" << session.PartyArrivalWaitMs()
        << ",\"route_deadline_ms\":" << session.routeDeadlineMs
        << ",\"native_travel_time_left_ms\":" << session.nativeTravelTimeLeftMs
        << ",\"native_status_deadline_elapsed_ms\":" << session.nativeTravelDeadlineMs
        << ",\"session_deadline_at_ms\":" << session.journey.Deadline().DeadlineAtMs()
        << ",\"native_aligned_deadline_at_ms\":" << session.nativeTravelDeadlineAtMs
        << ",\"age_ms\":" << session.SessionAgeMs() << "}"
        ;
    AppendHearthRecoveryTelemetry(out, session);
    out
        << ",\"journey_runtime\":" << session.journey.TelemetryJson("snapshot")
        << ",\"movement\":" << QuestAcquisitionMovementJson(facts, pathText) << '}';
    return out.str();
}

std::string QuestAcquisitionSnapshotJson(Player* bot, PlayerbotAI* botAI, TravelTarget* travelTarget)
{
    if (!bot || !botAI)
        return "null";
    auto const sessionIt = questAcquisitionSessions.find(bot->GetGUID().GetCounter());
    if (sessionIt == questAcquisitionSessions.end() || !sessionIt->second)
        return "null";
    return QuestAcquisitionSnapshotJsonForSession(*sessionIt->second, bot, botAI, travelTarget);
}

class AutoWowBridgeOperation final : public PlayerbotOperation
{
public:
    AutoWowBridgeOperation(AutoWowRequest request, std::shared_ptr<AutoWowCompletion> completion)
        : m_request(std::move(request)), m_completion(std::move(completion))
    {
    }

    bool Execute() override
    {
        if (m_completion->IsCancelled())
            return false;

        if (m_request.type == AutoWowRequestType::List)
            return Finish(true, ListBots());

        if (m_request.type == AutoWowRequestType::OracleLog)
        {
            OracleLogResponse const response =
                BuildOracleLogResponse(m_request.oracleSessionId, m_request.oracleCursor);
            return Finish(response.success, response.json);
        }

        if (m_request.type == AutoWowRequestType::FixtureInit)
            return Finish(true, AutoWowFixture::Init(m_request.botGuid, m_request.fixtureLevel,
                                                     m_request.fixtureSpecIndex, m_request.fixtureQuality));

        if (m_request.type == AutoWowRequestType::ProbeReset)
            return Finish(true, AutoWowProbeReset::Reset(m_request.destination, m_request.expectedMapId,
                                                         m_request.raidDifficulty, m_request.memberGuids));

        if (m_request.type == AutoWowRequestType::ProbeFixture)
            return Finish(true, AutoWowProbePlace::Execute(m_request.probe));

        if (m_request.type == AutoWowRequestType::FixtureStatus)
            return Finish(true, AutoWowFixture::Status(m_request.botGuid));

        if (m_request.type == AutoWowRequestType::FixtureAccelerate)
            return Finish(true, AutoWowFixtureAcceleration::Accelerate(
                m_request.botGuid, m_request.fixturePacingPercent));

        if (m_request.type == AutoWowRequestType::FixtureAccelerateOff)
            return Finish(true, AutoWowFixtureAcceleration::Disable(m_request.botGuid));

        if (m_request.type == AutoWowRequestType::FixtureKill)
            return Finish(true, AutoWowFixtureAcceleration::KillExact(
                m_request.botGuid, m_request.fixtureKillEntry, m_request.fixtureKillSpawnId));

        if (m_request.type == AutoWowRequestType::RaidCreate)
            return Finish(true, AutoWowRaid::Create(m_request.botGuid, m_request.memberGuids,
                                                    m_request.raidDifficulty));

        if (m_request.type == AutoWowRequestType::RaidStatus)
            return Finish(true, AutoWowRaid::Status(m_request.botGuid));

        if (m_request.type == AutoWowRequestType::RaidLeave)
            return Finish(true, AutoWowRaid::Leave(m_request.botGuid));

        if (m_request.type == AutoWowRequestType::WsgQueue)
            return Finish(true, AutoWowWarsong::Queue(m_request.memberGuids));

        if (m_request.type == AutoWowRequestType::WsgStatus)
            return Finish(true, AutoWowWarsong::Status(m_request.memberGuids));

        if (m_request.type == AutoWowRequestType::WsgLeave)
            return Finish(true, AutoWowWarsong::Leave(m_request.memberGuids));

        if (m_request.type == AutoWowRequestType::Activate)
            return ActivateBot();

        if (m_request.type == AutoWowRequestType::Deactivate)
            return DeactivateBot();

        if (m_request.type == AutoWowRequestType::Independent)
            return ArmIndependentBot();

        if (m_request.type == AutoWowRequestType::Party)
            return CreateParty();

        if (m_request.type == AutoWowRequestType::Rally)
            return RallyParty();

        if (m_request.type == AutoWowRequestType::Deploy)
            return DeployLeagueMember();

        if (m_request.type == AutoWowRequestType::Route)
            return RouteParty();

        if (m_request.type == AutoWowRequestType::Advance ||
            m_request.type == AutoWowRequestType::AdvancePoint)
            return AdvanceDungeonParty();

        if (m_request.type == AutoWowRequestType::Engage)
            return EngageNearby();

        if (m_request.type == AutoWowRequestType::BossApproach)
            return ApproachBoss();

        if (m_request.type == AutoWowRequestType::Scout)
            return ScoutNearby();

        if (m_request.type == AutoWowRequestType::Quest)
            return QuestParty();

        if (m_request.type == AutoWowRequestType::QuestAcquire)
            return AcquireQuestParty();

        if (m_request.type == AutoWowRequestType::Recover)
            return RecoverQuestParty();

        if (m_request.type == AutoWowRequestType::Observe)
            return Finish(true, AutoWowObserver::Dispatch(m_request.destination, m_request.botGuid,
                                                          m_request.memberGuids.empty() ? 0u : m_request.memberGuids.front()));

        if (m_request.type == AutoWowRequestType::GatherSourcePublish)
            return PublishGatherSource();

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));

        switch (m_request.type)
        {
            case AutoWowRequestType::Destinations:
                return Finish(true, ListDestinations(bot));
            case AutoWowRequestType::Snapshot:
                return Finish(true, "{\"ok\":true,\"bot\":" + SnapshotJson(bot, botAI) + "}");
            case AutoWowRequestType::CombatLog:
                return Finish(true, AutoWowCombatTelemetry::Build(bot, botAI));
            case AutoWowRequestType::EncounterLog:
                return Finish(true, AutoWowEncounterTelemetry::Build(bot, botAI));
            case AutoWowRequestType::ProfessionEconomy:
                return Finish(true, AutoWowProfessionEconomyTelemetry::Build(bot, IsLeagueMember(m_request.botGuid)));
            case AutoWowRequestType::Craft:
                if (!IsLeagueMember(m_request.botGuid))
                    return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));
                if (!AutoWowOracleRuntime::IsManagedBot(m_request.botGuid))
                    return Finish(false, ErrorResponse("bot_not_managed_by_oracle"));
                return Finish(true, AutoWowCraftControl::Execute(bot, botAI, m_request.craftRecipeSpellId));
            case AutoWowRequestType::CraftStatus:
                if (!IsLeagueMember(m_request.botGuid))
                    return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));
                if (!AutoWowOracleRuntime::IsManagedBot(m_request.botGuid))
                    return Finish(false, ErrorResponse("bot_not_managed_by_oracle"));
                return Finish(true, AutoWowCraftControl::Status(bot));
            case AutoWowRequestType::GuildTrade:
                if (!IsLeagueMember(m_request.botGuid) || !IsLeagueMember(m_request.tradeBuyerGuid))
                    return Finish(false, ErrorResponse("both_bots_not_enrolled_in_league"));
                if (!AutoWowOracleRuntime::IsManagedBot(m_request.botGuid) ||
                    !AutoWowOracleRuntime::IsManagedBot(m_request.tradeBuyerGuid))
                    return Finish(false, ErrorResponse("both_bots_not_managed_by_oracle"));
                return Finish(true, AutoWowGuildTradeControl::Start(
                    bot, botAI, m_request.tradeBuyerGuid, m_request.tradeItemGuid,
                    m_request.tradeItemEntry, m_request.tradeQuantity,
                    m_request.tradePriceCopper));
            case AutoWowRequestType::GuildTradeStatus:
                return Finish(true, AutoWowGuildTradeControl::Status(m_request.botGuid));
            case AutoWowRequestType::PathProbe:
            {
                AutoWowDungeonPath::ProbeResult const probe = m_request.hasSourceCoordinates ?
                    AutoWowDungeonPath::ProbeFrom(bot, m_request.sourceX, m_request.sourceY, m_request.sourceZ,
                                                  m_request.coordinateX, m_request.coordinateY, m_request.coordinateZ) :
                    AutoWowDungeonPath::Probe(bot, m_request.coordinateX, m_request.coordinateY, m_request.coordinateZ);
                return Finish(true, "{\"ok\":true,\"order\":\"pathprobe\",\"guid\":" +
                    std::to_string(m_request.botGuid) + ",\"probe\":" + AutoWowDungeonPath::Json(probe) + "}");
            }
            case AutoWowRequestType::QuestLog:
                return Finish(true, AutoWowQuestLog::Build(m_request.botGuid));
            case AutoWowRequestType::QuestObjective:
                return Finish(true, AutoWowQuestObjective::Build(m_request.botGuid));
            case AutoWowRequestType::Acceptance:
                return Finish(true, AutoWowQuestObjective::BuildAcceptance(
                    m_request.botGuid, m_request.destination.empty() ? 0u : static_cast<uint32>(std::stoul(m_request.destination))));
            case AutoWowRequestType::Pause:
                botAI->SetAutoWowPaused(true);
                return Finish(true, OrderResponse("pause", bot));
            case AutoWowRequestType::Resume:
                botAI->SetAutoWowPaused(false);
                return Finish(true, OrderResponse("resume", bot));
            case AutoWowRequestType::Travel:
                return ExecuteTravel(bot, botAI);
            default:
                return Finish(false, ErrorResponse("invalid_request"));
        }
    }

    ObjectGuid GetBotGuid() const override
    {
        if (!m_request.botGuid)
            return ObjectGuid::Empty;
        return ObjectGuid::Create<HighGuid::Player>(m_request.botGuid);
    }

    uint32 GetPriority() const override { return 50; }

    std::string GetName() const override { return "AutoWowBridge"; }

    bool IsValid() const override { return !m_completion->IsCancelled(); }

private:
    bool Finish(bool success, std::string response)
    {
        m_completion->Resolve(std::move(response));
        return success;
    }

    std::string ListBots() const
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"bots\":[";
        bool first = true;
        for (auto const& entry : sRandomPlayerbotMgr.GetAllBots())
        {
            Player* bot = entry.second;
            PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
            if (!bot || !botAI)
                continue;

            if (!first)
                out << ',';
            out << SnapshotJson(bot, botAI);
            first = false;
        }
        out << "]}";
        return out.str();
    }

    bool PublishGatherSource()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* bot = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (botAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));
        if (!AutoWowGather::IsExplicitWorker(m_request.botGuid))
            return Finish(false, ErrorResponse("gather_source_requires_worker_mode"));

        AutoWowOracle::GatherSourceReference const reference{
            true, m_request.gatherSpawnId, m_request.gatherEntry, m_request.gatherMapId,
            m_request.gatherMaterialItemId, m_request.gatherInstanceId, m_request.gatherGoal};
        if (!AutoWowOracle::ValidGatherSourceReference(reference) ||
            !AutoWowGather::PublishOracleSource(bot, botAI, reference))
            return Finish(false, ErrorResponse("exact_gather_source_unresolved"));

        AutoWowGather::RouteSnapshot const route = AutoWowGather::GetRouteSnapshot(m_request.botGuid);
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"gather-source\",\"guid\":" << m_request.botGuid
            << ",\"state\":\"published\",\"spawn_id\":" << reference.spawnId
            << ",\"entry\":" << reference.entry << ",\"map\":" << reference.mapId
            << ",\"instance\":" << reference.instanceId
            << ",\"material_item_id\":" << reference.materialItemId
            << ",\"goal\":" << JsonString(std::string(
                   AutoWowOracle::GatherGoalName(reference.goal)))
            << ",\"exact_only\":" << (route.oracleExactSourceOnly ? "true" : "false") << '}';
        return Finish(true, out.str());
    }

    bool ActivateBot()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(m_request.botGuid);
        if (ObjectAccessor::FindConnectedPlayer(guid))
            return Finish(false, ErrorResponse("bot_already_online"));

        // A fresh login is mode-neutral until a later deploy order. Do not inherit stale worker
        // state from an abnormal prior logout.
        AutoWowGather::DeactivateWorker(m_request.botGuid);
        AutoWowPolicy::SetNoTeleport(m_request.botGuid, false);

        // This uses Playerbots' normal asynchronous character-login path. League orchestration
        // purposefully does not insert or mutate character rows directly.
        sRandomPlayerbotMgr.AddPlayerBot(guid, 0);

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"activate\",\"guid\":" << m_request.botGuid
            << ",\"state\":\"login_queued\"}";
        return Finish(true, out.str());
    }

    bool DeactivateBot()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(m_request.botGuid);
        AutoWowGather::DeactivateWorker(m_request.botGuid);
        AutoWowPolicy::SetNoTeleport(m_request.botGuid, false);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot)
            return Finish(false, ErrorResponse("bot_not_online"));

        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
        // Logout can destroy the Player object immediately, so never dereference the stale pointer
        // while forming the acknowledgement.
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"deactivate\",\"guid\":" << m_request.botGuid << "}";
        return Finish(true, out.str());
    }

    bool ArmIndependentBot()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(m_request.botGuid);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (bot->GetGroup())
            return Finish(false, ErrorResponse("independent_requires_solo"));
        if (bot->IsInCombat())
            return Finish(false, ErrorResponse("independent_deferred_combat"));

        // A race seed is an autonomous player, not a worker and not a follower. Full reset clears
        // stale travel/RPG state from login or a previous mode, then the native quest/combat
        // strategies are reapplied. The independent flag is also honored by group-master
        // maintenance so a solo seed does not immediately fall back to +follow.
        AutoWowGather::DeactivateWorker(m_request.botGuid);
        AutoWowPolicy::SetNoTeleport(m_request.botGuid, true);
        botAI->SetMaster(nullptr);
        botAI->SetAutoWowIndependentParty(true);
        botAI->Reset(true);

        auto const [firstSkill, secondSkill] = OracleProfessionPair(m_request.botGuid);
        // A planned bot (AutoWow.Professions.Enable) keeps its trainer-learned professions: the
        // reconcile would drop them and InitTradeSkills no longer re-grants for it.
        if (firstSkill && secondSkill && AutoWowOracleRuntime::IsManagedBot(m_request.botGuid) &&
            !sPlayerbotAIConfig.GetAutoWowProfessionPlan(m_request.botGuid))
        {
            sRandomPlayerbotMgr.SetValue(bot, "professionRollType",
                                         1u); // PlayerbotFactory::ProfessionRollType::Random
            sRandomPlayerbotMgr.SetValue(bot, "firstSkill", firstSkill);
            sRandomPlayerbotMgr.SetValue(bot, "secondSkill", secondSkill);
            PlayerbotFactory factory(bot, bot->GetLevel());
            factory.ReconcilePrimaryTradeSkills(firstSkill, secondSkill);
            factory.InitSkills();
            bot->SaveToDB(false, false);
        }

        // New RPG owns autonomous destinations and quest movement. The legacy RPG strategy also
        // schedules move-to-target and random movement, which can repeatedly cancel that route.
        botAI->ChangeStrategy(
            "+grind,+new rpg,-rpg,-follow,-move random,-travel,-worker gather", BOT_STATE_NON_COMBAT);
        botAI->InvalidateActivityPolicy();
        botAI->AllowActivity(ALL_ACTIVITY, true);
        botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
        botAI->DoNextAction();

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"independent\",\"guid\":" << m_request.botGuid
            << ",\"state\":\"armed\",\"master\":null,\"group\":false"
            << ",\"native\":true,\"quest\":true,\"gather\":true,\"loot\":true}";
        return Finish(true, out.str());
    }

    bool CreateParty()
    {
        ObjectGuid leaderGuid = ObjectGuid::Create<HighGuid::Player>(m_request.botGuid);
        Player* leader = ObjectAccessor::FindPlayer(leaderGuid);
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));

        if (m_request.memberGuids.empty() || m_request.memberGuids.size() > 4)
            return Finish(false, ErrorResponse("party_requires_one_to_four_members"));

        std::vector<Player*> members;
        std::set<uint32> requestedGuids{m_request.botGuid};
        Group* group = leader->GetGroup();
        for (uint32 memberGuid : m_request.memberGuids)
        {
            if (!requestedGuids.insert(memberGuid).second)
                return Finish(false, ErrorResponse("party_member_duplicate"));

            Player* member = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(memberGuid));
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI)
                return Finish(false, ErrorResponse("party_member_not_online"));
            if (member->GetTeamId() != leader->GetTeamId())
                return Finish(false, ErrorResponse("cross_faction_party_not_allowed"));

            members.push_back(member);
        }

        std::vector<Player*> requestedPlayers{leader};
        requestedPlayers.insert(requestedPlayers.end(), members.begin(), members.end());

        std::set<Group*> subsetGroups;
        std::size_t groupedRequestedCount = 0;
        bool allGroupedRequestedMembersPresent = true;
        for (Player* requestedPlayer : requestedPlayers)
        {
            Group* requestedGroup = requestedPlayer->GetGroup();
            if (!requestedGroup)
                continue;

            ++groupedRequestedCount;
            subsetGroups.insert(requestedGroup);
            if (!requestedGroup->IsMember(requestedPlayer->GetGUID()))
                allGroupedRequestedMembersPresent = false;
        }

        // Preflight every distinct group reached through the requested roster before mutating any
        // of them. A single special, foreign, human, or offline member closes the repair path.
        bool allSubsetGroupsOrdinary = true;
        bool allSubsetGroupMembersRequested = true;
        bool allSubsetGroupMembersOnline = true;
        bool allSubsetGroupMembersPlayerbots = true;
        for (Group* subsetGroup : subsetGroups)
        {
            if (subsetGroup->isRaidGroup() || subsetGroup->isBGGroup() || subsetGroup->isBFGroup() ||
                subsetGroup->isLFGGroup())
            {
                allSubsetGroupsOrdinary = false;
            }

            for (Group::MemberSlot const& slot : subsetGroup->GetMemberSlots())
            {
                if (requestedGuids.find(slot.guid.GetCounter()) == requestedGuids.end())
                    allSubsetGroupMembersRequested = false;

                Player* subsetMember = ObjectAccessor::FindPlayer(slot.guid);
                if (!subsetMember)
                {
                    allSubsetGroupMembersOnline = false;
                    allSubsetGroupMembersPlayerbots = false;
                    continue;
                }
                if (!PlayerbotsMgr::instance().GetPlayerbotAI(subsetMember))
                    allSubsetGroupMembersPlayerbots = false;
            }
        }

        AutoWowExactParty::ExactPartySignals signals;
        signals.requestedCount = members.size() + 1;
        signals.currentCount = group ? group->GetMembersCount() : 0;
        signals.subsetGroupCount = subsetGroups.size();
        signals.groupedRequestedCount = groupedRequestedCount;
        signals.requestedLeaderPresent = requestedGuids.find(m_request.botGuid) != requestedGuids.end();
        signals.allRequestedOnline = true;
        signals.allRequestedPlayerbots = true;
        signals.sameFaction = true;
        signals.leaderHasGroup = group != nullptr;
        signals.leaderPresentInCurrentGroup = group && group->IsMember(leader->GetGUID());
        signals.allSubsetGroupsOrdinary = allSubsetGroupsOrdinary;
        signals.allSubsetGroupMembersRequested = allSubsetGroupMembersRequested;
        signals.allSubsetGroupMembersOnline = allSubsetGroupMembersOnline;
        signals.allSubsetGroupMembersPlayerbots = allSubsetGroupMembersPlayerbots;
        signals.allGroupedRequestedMembersPresent = allGroupedRequestedMembersPresent;

        AutoWowExactParty::ExactPartyAction const partyAction =
            AutoWowExactParty::ClassifyExactParty(signals);
        if (partyAction == AutoWowExactParty::ExactPartyAction::Refuse)
            return Finish(false, ErrorResponse(subsetGroups.empty() ?
                "party_existing_group_not_exact_roster" : "party_roster_group_conflict"));

        bool const repairedSubset = partyAction == AutoWowExactParty::ExactPartyAction::RepairSubset;
        bool const reconfigured = partyAction == AutoWowExactParty::ExactPartyAction::Reconfigure;
        if (reconfigured)
        {
            // Keep exact rosters idempotent after restart while defensively preserving the old
            // adapter contract: every requested member must already be in this ordinary group.
            if (group->isRaidGroup() || group->isBGGroup() || group->isBFGroup() || group->isLFGGroup() ||
                group->GetMembersCount() != members.size() + 1 || leader->GetGroup() != group)
            {
                return Finish(false, ErrorResponse("party_existing_group_not_exact_roster"));
            }
            for (Player* member : members)
                if (member->GetGroup() != group || !group->IsMember(member->GetGUID()))
                    return Finish(false, ErrorResponse("party_existing_group_not_exact_roster"));

            if (group->GetLeaderGUID() != leader->GetGUID())
                group->ChangeLeader(leader->GetGUID());
        }

        if (repairedSubset)
        {
            // Policy preflight proved every distinct requested-subset group safe. Disband all of
            // them before rebuilding; this also covers an ungrouped intended leader whose members
            // are fragmented across one or more ordinary bot-only groups.
            std::vector<Group*> groupsToDisband(subsetGroups.begin(), subsetGroups.end());
            for (Group* subsetGroup : groupsToDisband)
                subsetGroup->Disband();
            group = nullptr;
        }

        if (partyAction == AutoWowExactParty::ExactPartyAction::Create || repairedSubset)
        {
            group = new Group;
            if (!group->Create(leader))
            {
                delete group;
                return Finish(false, ErrorResponse("party_create_failed"));
            }
            sGroupMgr->AddGroup(group);

            for (Player* member : members)
            {
                if (!group->AddMember(member))
                {
                    group->Disband();
                    return Finish(false, ErrorResponse("party_add_member_failed"));
                }
            }
        }

        leaderAI->SetMaster(nullptr);
        leaderAI->SetAutoWowIndependentParty(true);
        // The party leader is the independent anchor. Followers may use the leader as their
        // master, but the leader must never inherit a stale follow strategy from an earlier party.
        leaderAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
        for (Player* member : members)
        {
            if (PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member))
            {
                memberAI->SetMaster(leader);
                memberAI->SetAutoWowIndependentParty(true);
                // Group membership is social state, not an instruction to become an idle escort.
                // Members start their own native quest/combat loop immediately; an explicit quest,
                // rally, or instance order may opt them into coordinated movement for that task.
                memberAI->ChangeStrategy(
                    "+grind,+new rpg,-follow,-move random,-travel", BOT_STATE_NON_COMBAT);
            }
        }

        // Forming a party ends solo worker mode even before the next deploy order arrives.
        AutoWowGather::DeactivateWorker(m_request.botGuid);
        AutoWowPolicy::SetNoTeleport(m_request.botGuid, false);
        leaderAI->ChangeStrategy("-worker gather", BOT_STATE_NON_COMBAT);
        leaderAI->InvalidateActivityPolicy();
        for (Player* member : members)
        {
            PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (!memberAI)
                continue;
            AutoWowGather::DeactivateWorker(member->GetGUID().GetCounter());
            AutoWowPolicy::SetNoTeleport(member->GetGUID().GetCounter(), false);
            memberAI->ChangeStrategy("-worker gather", BOT_STATE_NON_COMBAT);
            memberAI->InvalidateActivityPolicy();
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"party\",\"leader_guid\":" << m_request.botGuid
            << ",\"members\":" << group->GetMembersCount()
            << ",\"state\":\"" << (repairedSubset ? "repaired_subset" :
                (reconfigured ? "reconfigured" : "created")) << "\"}";
        return Finish(true, out.str());
    }

    bool RallyParty()
    {
        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("leader_has_no_party"));
        if (!leader->IsAlive())
            return Finish(false, ErrorResponse("rally_requires_alive_party"));
        if (leader->IsInCombat())
            return Finish(false, ErrorResponse("rally_deferred_combat"));

        // TeleportTo cannot select another player's live instance. A cross-instance rally would
        // create one private copy per member even though every character appears at the same XYZ.
        // Require the exterior -> grouped instance-entry route for that case.
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            if (!member || !member->IsAlive())
                return Finish(false, ErrorResponse("rally_requires_alive_party"));
            if (member->IsInCombat())
                return Finish(false, ErrorResponse("rally_deferred_combat"));
            if (leader->GetInstanceId() && member->GetInstanceId() != leader->GetInstanceId())
                return Finish(false, ErrorResponse("rally_refuses_cross_instance"));
        }

        uint32 moved = 0;
        leaderAI->SetAutoWowIndependentParty(true);
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            if (!member || member == leader)
                continue;

            PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (!memberAI)
                continue;

            memberAI->SetAutoWowIndependentParty(false);
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(member, AutoWowQuestLedger::Event::Contaminated, 0, "bridge_rally_teleport");
            member->TeleportTo(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(),
                               leader->GetPositionZ(), leader->GetOrientation());
            memberAI->Reset();
            ++moved;
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"rally\",\"leader_guid\":" << m_request.botGuid
            << ",\"moved\":" << moved << "}";
        return Finish(true, out.str());
    }

    bool DeployLeagueMember()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (botAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));

        Group* group = bot->GetGroup();
        uint32 configured = 0;
        bool deathRecoveryStarted = false;
        std::string mode;
        if (group)
        {
            if (group->GetLeaderGUID() != bot->GetGUID())
                return Finish(false, ErrorResponse("deploy_requires_party_leader"));

            for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            {
                Player* member = reference->GetSource();
                PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                if (!member || !memberAI)
                    continue;

                // A grouped deploy is a mode change. Drop any previous solo-worker force-active and
                // no-teleport state before applying party behavior.
                AutoWowGather::DeactivateWorker(member->GetGUID().GetCounter());
                AutoWowPolicy::SetNoTeleport(member->GetGUID().GetCounter(), false);
                memberAI->SetAutoWowIndependentParty(member == bot);
                memberAI->InvalidateActivityPolicy();

                if (member == bot)
                    memberAI->ChangeStrategy("+grind,+move random,-follow,-worker gather", BOT_STATE_NON_COMBAT);
                else
                    memberAI->ChangeStrategy("+follow,-move random,-grind,-worker gather", BOT_STATE_NON_COMBAT);
                memberAI->Reset();
                memberAI->DoNextAction();
                ++configured;
            }
            mode = "party_grind";
        }
        else
        {
            if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
                return Finish(false, ErrorResponse("worker_deploy_deferred_flight"));

            // Full reset clears retained TravelTarget/NewRpgInfo state. Apply the dedicated worker
            // strategy afterwards so AiFactory cannot re-add competing free-roam strategies.
            botAI->Reset(true);
            botAI->ChangeStrategy(
                "+worker gather,+gather,+loot,-new rpg,-rpg,-travel,-stay,-grind,-move random,-follow",
                BOT_STATE_NON_COMBAT);
            AutoWowGather::ActivateWorker(m_request.botGuid);
            AutoWowPolicy::SetNoTeleport(m_request.botGuid, true);
            botAI->InvalidateActivityPolicy();
            botAI->AllowActivity(ALL_ACTIVITY, true);
            botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);

            // A worker may be activated from a persisted released-ghost state.  Re-establish the
            // no-teleport policy before asking Playerbots to run its ordinary corpse action; this
            // never grants life or relocates the bot, and the protected action fails closed when
            // no walkable corpse/spirit-healer route exists.
            if (!bot->IsAlive())
            {
                deathRecoveryStarted = botAI->DoSpecificAction(
                    "find corpse", Event("persistent worker deploy recovery"), true);

                // FindCorpseAction may legitimately return false while the worker policy is
                // waiting for a corpse or a retry window. Report the policy lifecycle rather
                // than turning that ordinary wait into a false "not started" response.
                AutoWowGather::RouteSnapshot const recoverySnapshot =
                    AutoWowGather::GetRouteSnapshot(m_request.botGuid);
                deathRecoveryStarted = deathRecoveryStarted ||
                    (recoverySnapshot.deathRecovery.active &&
                     recoverySnapshot.deathRecovery.phase !=
                         AutoWowGather::DeathRecoveryPhase::Blocked);
            }
            botAI->DoNextAction();
            configured = 1;
            mode = "worker_gather";
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"deploy\",\"guid\":" << m_request.botGuid
            << ",\"mode\":" << JsonString(mode) << ",\"configured\":" << configured
            << ",\"death_recovery_started\":" << (deathRecoveryStarted ? "true" : "false") << "}";
        return Finish(true, out.str());
    }

    bool RouteParty()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("route_requires_party_leader"));

        struct Route
        {
            char const* name;
            uint32 map;
            float x;
            float y;
            float z;
            float o;
        };
        static Route const routes[] = {
            // Bootstrap staging only: place each first-run party beside a legitimate starter questgiver.
            // Subsequent quest objectives and turn-ins use Playerbots travel targets, never teleports.
            { "northshire", 0, -8943.50f, -129.50f, 83.72f, 0.0f },
            { "valley-of-trials", 1, -615.50f, -4326.00f, 40.09f, 0.0f },
            // Recovery staging for the Northstar leader's existing Night Elf quest chain.
            { "teldrassil", 1, 10463.50f, 829.50f, 1381.02f, 0.0f },
            // Disposable acceptance fixture for q435, Escorting Erland. Staging is beside the
            // legitimate quest giver; all escort movement and credit remain native script work.
            { "q435-erland", 0, 1409.68f, 1090.26f, 53.7694f, 0.0f },
            // Disposable direct-GameObject interaction fixture for q786. Staging is beside the
            // legitimate Lar Prowltusk spawn; acceptance, GO use, completion, and reward remain
            // native quest/playerbot work after this one pre-objective relocation.
            { "q786-lar-prowltusk", 1, -776.0f, -4830.36f, 19.8326f, 0.0f },
            // Disposable five-player combat fixture. The coordinates are the authoritative map
            // 574 entrance target; grouping happens first so every member enters one normal instance.
            { "uk-entry", 574, 153.789f, -86.548f, 12.551f, 0.304f },
            // Authoritative map-571 target of the Utgarde Keep exit trigger. Routing the formed
            // group here first guarantees the next map-574 transfer is a shared far teleport.
            // Persisted NODE_PORTAL 1557 is the authoritative exterior center for trigger 4745.
            // The old Z=218 fixture dropped characters 180 yards through the entrance terrain.
            { "uk-exterior", 571, 1257.11f, -4854.48f, 37.248f, 0.274f },
            // Additional portability fixtures stop at authoritative exterior area-trigger centers.
            // DungeonTransition performs the real instance admission; there are deliberately no
            // interior waypoints or encounter-specific instructions for these probes.
            { "nexus-exterior", 571, 3902.79f, 6985.69f, 75.0f, 0.0f },
            { "dtk-exterior", 571, 4775.32f, -2017.16f, 235.0f, 0.0f },
            // Onyxia staging stops outside the lair. Portal entry and all movement after this
            // relocation use Advance's normal mmap-backed pathing; there is no interior teleport.
            { "ony-exterior", 1, -4747.17f, -3753.27f, 49.8122f, 0.0f },
            // Vault's exterior mmap has a gap immediately before trigger 5258: PathGenerator returns
            // SHORTCUT|NOPATH for the otherwise grounded 14-yard approach. This fixture-only route
            // remains outside the instance but stages at the authoritative trigger center, avoiding
            // an unsafe direct spline. Advance still emits the ordinary area-trigger protocol and
            // all map-624 movement remains strictly path-probed.
            { "voa-exterior", 571, 5494.88f, 2839.81f, 420.811f, 0.0f },
            // First-pack staging witness: 16 yards short of the two Dragonflayer Ironhelms at
            // x=211.4/213.4. This is a lab-only pull setup, not campaign travel.
            { "uk-first-pack", 574, 195.0f, -65.0f, 24.764f, 0.0f }
        };

        Route const* route = nullptr;
        std::string requested = Lowercase(m_request.destination);
        for (Route const& candidate : routes)
        {
            if (requested == candidate.name)
            {
                route = &candidate;
                break;
            }
        }
        if (!route)
            return Finish(false, ErrorResponse("route_not_found"));

        // A named exterior fixture that already lies in a normal-dungeon ingress volume is staged
        // from ObjectMgr's authoritative geometry. Stack at the exact center: it is deterministic,
        // guaranteed inside even where the terrain surface sits just beyond a narrow trigger's
        // vertical extent, and cannot let a follower formation offset escape the volume. Raid
        // exteriors and ordinary routes keep their authored staging points.
        AreaTrigger const* exteriorTrigger = FindNonRaidDungeonExteriorTrigger(
            route->name, route->map, route->x, route->y, route->z);
        uint32 const stagingMap = exteriorTrigger ? exteriorTrigger->map : route->map;
        float const stagingX = exteriorTrigger ? exteriorTrigger->x : route->x;
        float const stagingY = exteriorTrigger ? exteriorTrigger->y : route->y;
        float const stagingZ = exteriorTrigger ? exteriorTrigger->z : route->z;

        uint32 moved = 0;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI)
                continue;

            memberAI->SetAutoWowIndependentParty(member == leader);

            // Hold only exterior-dungeon followers at the exact trigger center until the native
            // DungeonTransition strategy admits them one at a time. Without this hold, formation
            // AI can leave a narrow vertical trigger between its two-second polls. The leader
            // remains active and establishes the shared instance; each follower is resumed by
            // ActivatePortal after its ordinary area-trigger packet is handled.
            bool const holdForSerialAdmission = exteriorTrigger && member != leader;
            if (holdForSerialAdmission)
                memberAI->SetAutoWowPaused(true);
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(member, AutoWowQuestLedger::Event::Contaminated, 0, "bridge_route_teleport");
            member->TeleportTo(stagingMap, stagingX, stagingY, stagingZ, route->o);
            memberAI->Reset();
            if (holdForSerialAdmission)
                memberAI->SetAutoWowPaused(true);
            ++moved;
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"route\",\"leader_guid\":" << m_request.botGuid
            << ",\"route\":" << JsonString(route->name) << ",\"moved\":" << moved << "}";
        return Finish(true, out.str());
    }

    bool AdvanceDungeonParty()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));

        // Some scripted dungeon gauntlets leave players in combat with no victim and no attackers
        // after their temporary creatures despawn. Recover only that inert state; real combat remains
        // a hard movement guard.
        auto clearStaleCombat = [](Player* player)
        {
            if (!player->IsInCombat() || player->GetVictim() || !player->getAttackers().empty())
                return false;
            player->CombatStop(true);
            return !player->IsInCombat();
        };
        uint32 staleCombatCleared = clearStaleCombat(leader) ? 1u : 0u;
        if (!leader->IsAlive())
            return Finish(false, ErrorResponse("advance_requires_alive_leader"));
        if (leader->IsInCombat())
            return Finish(false, ErrorResponse("advance_deferred_combat"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("advance_requires_party_leader"));

        struct Waypoint
        {
            char const* name;
            uint32 map;
            float x;
            float y;
            float z;
            bool requiresInstance;
        };
        static Waypoint const waypoints[] = {
            // Utgarde Keep route landmarks derived from authoritative creature/boss spawns. These
            // are ordinary pathfinding goals: unlike RouteParty, Advance never teleports a player.
            { "uk-forge-end", 574, 390.0f, -3.0f, 22.8f, true },
            { "uk-drake-room", 574, 385.0f, 145.0f, 30.8f, true },
            { "uk-keleseth", 574, 220.0f, 200.0f, 40.9f, true },
            { "uk-tunnel", 574, 105.0f, 240.0f, 43.0f, true },
            { "uk-lower-hall", 574, 96.0f, 186.0f, 49.4f, true },
            { "uk-stairs-1", 574, 105.0f, 92.0f, 65.8f, true },
            { "uk-stairs-2", 574, 89.0f, 98.0f, 87.2f, true },
            { "uk-stairs-3", 574, 108.0f, 58.0f, 109.1f, true },
            { "uk-skarvald", 574, 112.0f, -30.0f, 118.9f, true },
            { "uk-upper-ramp", 574, 238.0f, 13.0f, 135.3f, true },
            { "uk-upper-hall", 574, 230.0f, -6.0f, 178.6f, true },
            { "uk-worg-hall", 574, 266.0f, -102.0f, 190.5f, true },
            { "uk-worg-turn", 574, 281.0f, -148.0f, 190.5f, true },
            { "uk-gauntlet-east", 574, 210.0f, -176.0f, 190.0f, true },
            { "uk-gauntlet-portcullis", 574, 167.133f, -202.933f, 180.566f, true },
            { "uk-west-corridor", 574, 181.556f, -223.644f, 180.677f, true },
            { "uk-corridor-bend", 574, 195.533f, -242.400f, 180.633f, true },
            { "uk-southwest-corridor", 574, 207.680f, -268.480f, 180.500f, true },
            { "uk-south-corridor", 574, 226.187f, -274.507f, 180.500f, true },
            { "uk-ingvar-north", 574, 239.533f, -275.667f, 180.500f, true },
            { "uk-inner-approach", 574, 245.556f, -287.378f, 180.500f, true },
            { "uk-final-approach", 574, 250.222f, -308.356f, 180.500f, true },
            { "uk-ingvar-platform", 574, 245.822f, -331.733f, 180.500f, true },
            // The only Onyxia relocation is the exterior route above. Walk to the exterior portal, then
            // continue through the ordinary shared raid instance. The staged tunnel coordinates are
            // sampled from the authoritative Onyxian Warder waypoint paths (125500/125510/125520),
            // so the raid clears real trash instead of requesting one brittle path across the whole map.
            // Area-trigger centers can be above the walkable surface. Movement uses the canonical
            // grounded travel-node height; activation still uses the authoritative trigger volume.
            // Grounded approach inside trigger 2848. The trigger center itself is not attached to
            // this exterior navmesh; activation below still checks the authoritative DBC volume.
            { "ony-portal", 1, -4760.0f, -3752.8f, 49.43f, false },
            { "ony-upper-warders", 249, -45.0f, -96.0f, -36.0f, true },
            { "ony-descending-tunnel", 249, -105.0f, -116.0f, -45.0f, true },
            { "ony-lower-turn", 249, -160.0f, -160.0f, -57.0f, true },
            { "ony-lower-warders", 249, -165.0f, -200.0f, -66.0f, true },
            { "ony-chamber-mouth", 249, -111.0f, -214.0f, -75.0f, true },
            { "ony-boss-approach", 249, -10.0f, -180.0f, -87.0f, true },
            // Vault entry is the authoritative center of exterior trigger 5258. Every map-624
            // landmark below is an ordinary mmap-backed goal after normal shared-instance entry.
            { "voa-portal", 571, 5494.88f, 2839.81f, 416.811f, false },
            // Safe assembly points sit between the authoritative warder spawns. The director waits
            // for the full raid here before an exact trash pull instead of letting the front tank
            // enter aggro range while healers are still descending the entrance ramp.
            { "voa-entrance-staging", 624, -285.0f, -103.4f, 106.1f, true },
            { "voa-central-hub", 624, -219.29f, -126.94f, 102.95f, true },
            { "voa-archavon-west-staging", 624, -150.0f, -103.5f, 103.3f, true },
            { "voa-archavon-approach", 624, 93.885f, -101.531f, 91.401f, true },
            { "voa-emalon-north-staging", 624, -219.0f, -145.0f, 101.0f, true },
            { "voa-emalon-south-corridor", 624, -218.85f, -196.20f, 97.59f, true },
            { "voa-emalon-approach", 624, -221.8f, -243.8f, 96.8f, true },
            // The remaining Vault wings use the same ordinary mmap-backed walker. Each staging
            // point stops short of the authoritative warder/boss spawn so exact pulls, not
            // movement aggro or interior relocation, start the encounters.
            { "voa-koralon-south-staging", 624, -218.9f, -82.0f, 100.0f, true },
            { "voa-koralon-first-warder", 624, -218.8f, -65.0f, 97.6f, true },
            { "voa-koralon-second-warder", 624, -218.9f, -3.0f, 97.7f, true },
            { "voa-koralon-approach", 624, -218.5f, 62.0f, 96.8f, true },
            // Toravon's wing branches east from the upper Emalon spine, not from Archavon's
            // hall. Preserve the two right-angle turns explicitly: the apparent diagonal from
            // Archavon's east hall crosses open void. The safe staging point remains outside
            // Frost Warder aggro range so an exact pull starts combat only after all ten arrive.
            { "voa-toravon-east-staging", 624, -219.0f, -175.0f, 98.5f, true },
            { "voa-toravon-arch-corner", 624, -130.0f, -175.0f, 98.0f, true },
            { "voa-toravon-safe-staging", 624, -80.0f, -175.0f, 98.0f, true },
            { "voa-toravon-first-warder", 624, -43.2f, -185.0f, 97.6f, true },
            { "voa-toravon-second-warder", 624, -43.2f, -201.0f, 97.6f, true },
            { "voa-toravon-approach", 624, -43.3f, -247.0f, 96.8f, true }
        };

        Waypoint coordinateWaypoint = {
            "coordinate", leader->GetMapId(), m_request.coordinateX, m_request.coordinateY,
            m_request.coordinateZ, leader->GetInstanceId() != 0
        };
        Waypoint const* waypoint = m_request.type == AutoWowRequestType::AdvancePoint ?
            &coordinateWaypoint : nullptr;
        std::string requested = Lowercase(m_request.destination);
        for (Waypoint const& candidate : waypoints)
        {
            if (waypoint)
                break;
            if (requested == candidate.name)
            {
                waypoint = &candidate;
                break;
            }
        }
        if (!waypoint)
            return Finish(false, ErrorResponse("advance_waypoint_not_found"));

        struct PortalSpec
        {
            char const* waypoint;
            uint32 exteriorMap;
            uint32 targetMap;
            uint32 triggerId;
            float readinessRadius;
        };
        static PortalSpec const portalSpecs[] = {
            { "ony-portal", 1, 249, 2848, 16.0f },
            // Tighter than the 14-yard exterior staging offset, requiring an mmap-backed approach.
            { "voa-portal", 571, 624, 5258, 8.0f }
        };
        PortalSpec const* portal = nullptr;
        for (PortalSpec const& candidate : portalSpecs)
        {
            if (requested == candidate.waypoint)
            {
                portal = &candidate;
                break;
            }
        }

        // Headless playerbots do not have a game client to emit CMSG_AREATRIGGER when their
        // movement spline enters a portal volume. Once the complete raid has walked into the
        // configured entrance radius, emulate the same client packet that Playerbots' generic
        // AreaTriggerAction uses. Admission is deliberately staged across world ticks: the leader
        // enters first and establishes the temporary group bind, then one follower enters per
        // subsequent call. Sending the whole raid in one tick races InstanceMap::Add and can create
        // a different temporary instance for every member.
        if (portal)
        {
            AreaTrigger const* portalTrigger = sObjectMgr->GetAreaTrigger(portal->triggerId);
            if (!portalTrigger)
                return Finish(false, ErrorResponse("advance_portal_trigger_not_found"));

            auto activatePortal = [portal](Player* member)
            {
                member->StopMoving();
                WorldPacket packet(CMSG_AREATRIGGER);
                packet << portal->triggerId;
                packet.rpos(0);
                member->GetSession()->HandleAreaTriggerOpcode(packet);
            };

            if (leader->GetMapId() == portal->exteriorMap && !leader->GetInstanceId())
            {
                bool completeRaidReady = true;
                for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
                {
                    Player* member = reference->GetSource();
                    PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                    if (!member || !memberAI || !member->IsAlive() || member->IsInCombat() ||
                        member->GetMapId() != leader->GetMapId() ||
                        member->GetInstanceId() != leader->GetInstanceId())
                        return Finish(false, ErrorResponse("advance_requires_portal_ready_raid"));

                    if (!member->IsInAreaTriggerRadius(portalTrigger, 0.0f))
                        completeRaidReady = false;
                }

                if (completeRaidReady)
                {
                    activatePortal(leader);
                    std::ostringstream out;
                    out << "{\"ok\":true,\"order\":\"advance\",\"leader_guid\":" << m_request.botGuid
                        << ",\"waypoint\":" << JsonString(portal->waypoint)
                        << ",\"mode\":\"area_trigger_protocol\""
                        << ",\"stage\":\"leader\",\"trigger_id\":" << portal->triggerId
                        << ",\"target_map\":" << portal->targetMap
                        << ",\"activated_guid\":" << leader->GetGUID().GetCounter()
                        << ",\"activated_members\":1}";
                    return Finish(true, out.str());
                }
            }
            else if (leader->GetMapId() == portal->targetMap && leader->GetInstanceId())
            {
                Player* nextFollower = nullptr;
                Player* pendingFollower = nullptr;
                uint32 insideMembers = 0;
                for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
                {
                    Player* member = reference->GetSource();
                    PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                    if (!member || !memberAI || !member->IsAlive() || member->IsInCombat())
                        return Finish(false, ErrorResponse("advance_requires_portal_ready_raid"));

                    bool const withinReadinessRadius =
                        member->IsInAreaTriggerRadius(portalTrigger, 0.0f);
                    AutoWowPortalAdmission::MemberLocation const memberLocation =
                        AutoWowPortalAdmission::ClassifyMember(member->GetMapId(),
                            member->GetInstanceId(), leader->GetMapId(), leader->GetInstanceId(),
                            portal->exteriorMap, withinReadinessRadius);
                    if (memberLocation ==
                        AutoWowPortalAdmission::MemberLocation::InsideExpectedInstance)
                    {
                        ++insideMembers;
                        continue;
                    }
                    if (member->IsBeingTeleported())
                    {
                        if (!pendingFollower)
                            pendingFollower = member;
                        continue;
                    }
                    if (memberLocation == AutoWowPortalAdmission::MemberLocation::SplitInstance)
                        return Finish(false, ErrorResponse("advance_portal_split_instance"));
                    if (memberLocation == AutoWowPortalAdmission::MemberLocation::Displaced)
                        return Finish(false, ErrorResponse("advance_portal_member_displaced"));
                    if (!nextFollower)
                        nextFollower = member;
                }

                std::ostringstream out;
                out << "{\"ok\":true,\"order\":\"advance\",\"leader_guid\":" << m_request.botGuid
                    << ",\"waypoint\":" << JsonString(portal->waypoint)
                    << ",\"mode\":\"area_trigger_protocol\""
                    << ",\"trigger_id\":" << portal->triggerId
                    << ",\"target_map\":" << portal->targetMap;
                if (pendingFollower)
                {
                    out << ",\"stage\":\"follower_transfer_pending\",\"pending_guid\":"
                        << pendingFollower->GetGUID().GetCounter()
                        << ",\"activated_members\":0,\"inside_before\":" << insideMembers;
                }
                else if (nextFollower && !nextFollower->IsInAreaTriggerRadius(portalTrigger, 0.0f))
                {
                    PlayerbotAI* nextAI = PlayerbotsMgr::instance().GetPlayerbotAI(nextFollower);
                    AutoWowDungeonWalkAction walk(nextAI);
                    bool const started = walk.Walk(
                        waypoint->map, waypoint->x, waypoint->y, waypoint->z);
                    out << ",\"stage\":\"follower_repositioning\",\"reposition_guid\":"
                        << nextFollower->GetGUID().GetCounter()
                        << ",\"started\":" << (started ? "true" : "false")
                        << ",\"activated_members\":0,\"inside_before\":" << insideMembers;
                }
                else if (nextFollower)
                {
                    activatePortal(nextFollower);
                    out << ",\"stage\":\"follower\",\"activated_guid\":"
                        << nextFollower->GetGUID().GetCounter() << ",\"activated_members\":1"
                        << ",\"inside_before\":" << insideMembers;
                }
                else
                {
                    out << ",\"stage\":\"complete\",\"activated_members\":0"
                        << ",\"inside_before\":" << insideMembers;
                }
                out << "}";
                return Finish(true, out.str());
            }
            else
                return Finish(false, ErrorResponse("advance_requires_portal_ready_raid"));
        }

        if (leader->GetMapId() != waypoint->map ||
            (waypoint->requiresInstance && !leader->GetInstanceId()))
            return Finish(false, ErrorResponse("advance_requires_matching_instance"));

        struct AdvanceMember
        {
            Player* player;
            PlayerbotAI* ai;
        };
        std::vector<AdvanceMember> advanceMembers;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI || !member->IsAlive())
                return Finish(false, ErrorResponse("advance_requires_alive_party"));
            if (member->GetMapId() != leader->GetMapId() || member->GetInstanceId() != leader->GetInstanceId())
                return Finish(false, ErrorResponse("advance_requires_shared_instance"));
            if (member != leader && clearStaleCombat(member))
                ++staleCombatCleared;
            if (member->IsInCombat())
                return Finish(false, ErrorResponse("advance_deferred_combat"));

            advanceMembers.push_back({member, memberAI});
        }

        std::sort(advanceMembers.begin(), advanceMembers.end(), [leader](AdvanceMember const& left, AdvanceMember const& right)
        {
            if (left.player == leader)
                return true;
            if (right.player == leader)
                return false;
            return left.player->GetGUID().GetCounter() < right.player->GetGUID().GetCounter();
        });

        float originX = leader->GetPositionX();
        float originY = leader->GetPositionY();
        float formationOrientation = leader->GetOrientation();
        if (m_request.type == AutoWowRequestType::AdvancePoint && m_request.hasCoordinateOrientation)
        {
            formationOrientation = m_request.coordinateOrientation;
            originX = waypoint->x - std::cos(formationOrientation);
            originY = waypoint->y - std::sin(formationOrientation);
        }
        struct PlannedAdvance
        {
            AdvanceMember member;
            AutoWowDungeonPath::ProbeResult probe;
            bool stationary = false;
        };
        std::vector<PlannedAdvance> plans;
        plans.reserve(advanceMembers.size());

        // Preflight the complete formation before changing any strategy or MotionMaster. If one
        // member cannot obtain a complete grounded path, the entire order fails atomically.
        for (std::size_t slot = 0; slot < advanceMembers.size(); ++slot)
        {
            float const lateralScale = m_request.type == AutoWowRequestType::AdvancePoint ? 0.0f : 1.0f;
            AutoWowAdvanceFormation::Destination const destination = AutoWowAdvanceFormation::ForSlot(
                static_cast<uint32>(slot), originX, originY, waypoint->x, waypoint->y,
                formationOrientation, lateralScale);
            float destinationZ = waypoint->z;
            if (slot)
            {
                Player* member = advanceMembers[slot].player;
                Map* map = member->GetMap();
                map->GetGridTerrainData(destination.x, destination.y);
                // Search immediately above the continuous member/waypoint plane. A taller cast can
                // select overhead arches as "ground" in multi-level instance collision.
                float const searchTop = std::max(member->GetPositionZ(), waypoint->z) + 2.0f;
                float const formationGround = map->GetHeight(
                    member->GetPhaseMask(), destination.x, destination.y, searchTop, true, 20.0f);
                if (formationGround <= INVALID_HEIGHT || !std::isfinite(formationGround) ||
                    std::fabs(formationGround - waypoint->z) > 8.0f)
                {
                    std::ostringstream rejected;
                    rejected << "{\"ok\":false,\"error\":\"advance_formation_floor_rejected\""
                             << ",\"leader_guid\":" << m_request.botGuid
                             << ",\"member_guid\":" << member->GetGUID().GetCounter()
                             << ",\"waypoint\":" << JsonString(waypoint->name) << "}";
                    return Finish(false, rejected.str());
                }
                destinationZ = formationGround;
            }
            if (advanceMembers[slot].player->GetExactDist(destination.x, destination.y, destinationZ) <= 0.75f)
            {
                plans.push_back({advanceMembers[slot], {}, true});
                continue;
            }
            AutoWowDungeonPath::ProbeResult probe = AutoWowDungeonPath::Probe(
                advanceMembers[slot].player, destination.x, destination.y, destinationZ);
            if (!probe.safe)
            {
                std::ostringstream rejected;
                rejected << "{\"ok\":false,\"error\":\"advance_path_rejected\""
                         << ",\"leader_guid\":" << m_request.botGuid
                         << ",\"member_guid\":" << advanceMembers[slot].player->GetGUID().GetCounter()
                         << ",\"waypoint\":" << JsonString(waypoint->name)
                         << ",\"probe\":" << AutoWowDungeonPath::Json(probe, false) << "}";
                return Finish(false, rejected.str());
            }
            plans.push_back({advanceMembers[slot], std::move(probe), false});
        }

        uint32 configured = 0;
        uint32 startedMembers = 0;
        for (PlannedAdvance const& plan : plans)
        {
            Player* member = plan.member.player;
            PlayerbotAI* memberAI = plan.member.ai;

            if (plan.stationary)
            {
                ++startedMembers;
                ++configured;
                continue;
            }

            memberAI->SetAutoWowIndependentParty(member == leader);
            memberAI->Reset();
            if (member == leader)
                memberAI->ChangeStrategy("-move random,-grind,-travel,-follow", BOT_STATE_NON_COMBAT);
            else
                memberAI->ChangeStrategy("+follow,-move random,-grind,-travel", BOT_STATE_NON_COMBAT);

            // Give every member an explicit mmap-backed route. A follower-only order can strand
            // ranged members behind a boss-room threshold after the leader crosses it; repeatedly
            // re-enabling +follow does not provide those members with a path through the doorway.
            AutoWowDungeonWalkAction walk(memberAI);
            if (walk.WalkPrepared(plan.probe))
                ++startedMembers;
            ++configured;
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"advance\",\"leader_guid\":" << m_request.botGuid
            << ",\"waypoint\":" << JsonString(waypoint->name)
            << ",\"mode\":\"normal_pathfinding\",\"started\":" << (startedMembers ? "true" : "false")
            << ",\"formation\":\""
            << (m_request.type == AutoWowRequestType::AdvancePoint ? "single_file" : "corridor_grid") << "\""
            << ",\"started_members\":" << startedMembers
            << ",\"configured\":" << configured
            << ",\"stale_combat_cleared\":" << staleCombatCleared << "}";
        return Finish(true, out.str());
    }

    bool EngageExactUnit(Player* leader, Group* group, Unit* target, char const* mode, uint32 requestedEntry = 0)
    {
        if (!leader->IsAlive())
            return Finish(false, ErrorResponse("exact_engage_requires_alive_leader"));
        if (!target)
            return Finish(false, ErrorResponse("exact_target_not_found_in_range"));

        AutoWowExactBossTarget::TargetFacts const leaderFacts = {
            target->IsInWorld(),
            target->GetMapId() == leader->GetMapId(),
            target->GetInstanceId() == leader->GetInstanceId(),
            target->IsAlive(),
            target->IsHostileTo(leader),
            target->isTargetableForAttack() && leader->IsValidAttackTarget(target),
            leader->GetDistance(target),
        };
        std::string targetError;
        if (!AutoWowExactBossTarget::ValidateTargetFacts(leaderFacts, targetError))
            return Finish(false, ErrorResponse(targetError));

        struct PartyAttackIntent
        {
            Player* member;
            PlayerbotAI* ai;
            AiObjectContext* context;
        };

        std::vector<PartyAttackIntent> party;
        bool leaderSeen = false;
        // Preflight the complete roster before setting any target. An offline, paused, dead,
        // split-instance, out-of-range, non-hostile, or blocked member makes the order fail closed
        // instead of partially pulling the encounter with a subset of the group.
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            AiObjectContext* memberContext = memberAI ? memberAI->GetAiObjectContext() : nullptr;
            if (!member || !memberAI || !memberContext)
                return Finish(false, ErrorResponse("exact_engage_requires_online_playerbot_party"));
            if (memberAI->IsAutoWowPaused())
                return Finish(false, ErrorResponse("exact_engage_party_member_paused"));
            if (!member->IsAlive())
                return Finish(false, ErrorResponse("exact_engage_requires_alive_party"));

            AutoWowExactBossTarget::TargetFacts const memberFacts = {
                target->IsInWorld(),
                target->GetMapId() == member->GetMapId(),
                target->GetInstanceId() == member->GetInstanceId(),
                target->IsAlive(),
                target->IsHostileTo(member),
                target->isTargetableForAttack() && member->IsValidAttackTarget(target),
                member->GetDistance(target),
            };
            std::string memberError;
            if (!AutoWowExactBossTarget::ValidateTargetFacts(memberFacts, memberError))
                return Finish(false, ErrorResponse("exact_target_not_attackable_for_entire_party"));
            if (!member->IsWithinLOSInMap(target))
                return Finish(false, ErrorResponse("exact_target_not_in_entire_party_los"));

            leaderSeen = leaderSeen || member == leader;
            party.push_back({member, memberAI, memberContext});
        }

        if (!leaderSeen || party.empty())
            return Finish(false, ErrorResponse("exact_engage_invalid_party_roster"));

        uint32 intentMembers = 0;
        uint32 startedMembers = 0;
        for (PartyAttackIntent const& intent : party)
        {
            // Pin the normal Playerbots target values, then use its regular attack action. This is
            // attack intent only: no relocation, spawn, kill, or persistence shortcut is involved.
            intent.context->GetValue<GuidVector>("prioritized targets")->Set({target->GetGUID()});
            ++intentMembers;
            AutoWowExactAttackAction attack(intent.ai);
            if (attack.AttackExact(target))
                ++startedMembers;
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"engage\",\"mode\":" << JsonString(mode)
            << ",\"leader_guid\":" << m_request.botGuid;
        if (requestedEntry)
            out << ",\"target_entry\":" << requestedEntry;
        out << ",\"target_guid\":" << target->GetGUID().GetCounter()
            << ",\"party_members\":" << party.size()
            << ",\"intent_members\":" << intentMembers
            << ",\"started_members\":" << startedMembers << "}";
        return Finish(true, out.str());
    }

    bool EngageExactCreature(Player* leader, Group* group)
    {
        uint32 const requestedEntry = m_request.exactCreatureEntry;
        Creature* target = leader->FindNearestCreature(
            requestedEntry, AutoWowExactBossTarget::kMaxTargetDistance, true);
        if (!target)
            return Finish(false, ErrorResponse("exact_target_not_found_in_range"));
        if (target->GetEntry() != requestedEntry)
            return Finish(false, ErrorResponse("exact_engage_target_entry_mismatch"));
        return EngageExactUnit(leader, group, target, "exact_entry", requestedEntry);
    }

    bool EngageExactPlayer(Player* leader, Group* group)
    {
        Player* target = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(m_request.exactPlayerGuid));
        if (!target)
            return Finish(false, ErrorResponse("exact_player_target_not_online"));
        return EngageExactUnit(leader, group, target, "exact_player");
    }

    bool EngageNearby()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (botAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));
        Group* group = bot->GetGroup();
        if (m_request.exactCreatureEntry || m_request.exactPlayerGuid)
        {
            if (!group || group->GetLeaderGUID() != bot->GetGUID())
                return Finish(false, ErrorResponse("exact_engage_requires_grouped_leader"));
            if (m_request.exactCreatureEntry)
                return EngageExactCreature(bot, group);
            return EngageExactPlayer(bot, group);
        }

        if (group && group->GetLeaderGUID() != bot->GetGUID())
            return Finish(false, ErrorResponse("engage_requires_party_leader"));

        bool const started = botAI->DoSpecificAction("attack anything", Event(), true);
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"engage\",\"guid\":" << m_request.botGuid
            << ",\"started\":" << (started ? "true" : "false") << "}";
        return Finish(true, out.str());
    }

    bool ApproachBoss()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (leaderAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("approach_requires_grouped_leader"));

        AutoWowBossApproach::Result const approach = AutoWowBossApproach::Run(
            leader, leaderAI, m_request.bossStatusOnly);
        if (!approach.shouldEngage || !approach.target)
            return Finish(approach.ok, approach.Json());

        // The approach controller has gated the full cohort. Drop temporary travel intents before
        // handing the already-resolved target to the existing exact attack action.
        AutoWowBossApproach::ReleaseMovement(leader);
        bool const engaged = EngageExactUnit(leader, group, approach.target, "boss_approach",
                                             approach.targetEntry);
        AutoWowBossApproach::MarkEngaged(leader, engaged);
        return engaged;
    }

    bool ScoutNearby()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* botAI = bot ? PlayerbotsMgr::instance().GetPlayerbotAI(bot) : nullptr;
        if (!bot || !botAI)
            return Finish(false, ErrorResponse("bot_not_online"));
        if (botAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));
        if (Group* group = bot->GetGroup(); group && group->GetLeaderGUID() != bot->GetGUID())
            return Finish(false, ErrorResponse("scout_requires_party_leader"));

        bool const moved = botAI->DoSpecificAction("move random", Event(), true);
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"scout\",\"guid\":" << m_request.botGuid
            << ",\"moved\":" << (moved ? "true" : "false") << "}";
        return Finish(true, out.str());
    }

    void EnableLeagueNoTeleport(Group* group)
    {
        if (!group)
            return;

        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            if (member)
                AutoWowPolicy::SetNoTeleport(member->GetGUID().GetCounter(), true);
        }
    }

    void EnsureQuestAcquisitionTravelStrategy(PlayerbotAI* botAI)
    {
        if (botAI && !botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+travel,-grind,-move random,-new rpg,-follow",
                                  BOT_STATE_NON_COMBAT);
    }

    bool AcquireQuestParty()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));
        if (leaderAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));
        if (!leader->IsAlive() || leader->IsInCombat())
            return Finish(false, ErrorResponse("quest_acquisition_deferred"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("quest_requires_party_leader"));

        AiObjectContext* context = leaderAI->GetAiObjectContext();
        TravelTarget* travelTarget = context ? context->GetValue<TravelTarget*>("travel target")->Get() : nullptr;
        if (!context || !travelTarget)
            return Finish(false, ErrorResponse("travel_target_unavailable"));

        // Repeating the exact acquire command while its one persistent journey is pending is a
        // status check, not a reselection. Terminal sessions are retired before a new generation.
        auto existing = questAcquisitionSessions.find(m_request.botGuid);
        if (existing != questAcquisitionSessions.end() &&
            existing->second->state == QuestAcquisitionState::Pending)
        {
            existing->second->ReconcileTravelTarget(travelTarget);
            if (existing->second->state == QuestAcquisitionState::Pending)
                existing->second->Poll(leader, false);
            if (existing->second->state == QuestAcquisitionState::Pending)
                existing->second->ApplyLiveMovementFeedback(leader);
            else
                existing->second->ReleaseTerminalTravelTarget(leader, travelTarget);
            AutoWowQuestAcquisition::MovementActionResult const actionResult =
                existing->second->movementDiagnostics.actionResult;
            AutoWowQuestAcquisition::MovementOrderFacts const orderFacts =
                PassiveQuestAcquisitionOrderFacts(*existing->second, leaderAI, travelTarget);
            QuestAcquisitionPathText pathText;
            CaptureQuestAcquisitionMovement(
                *existing->second, leader, leaderAI, travelTarget, actionResult, orderFacts,
                existing->second->movementDiagnostics, pathText, true);
            existing->second->pathProbeReason = std::move(pathText.reason);
            existing->second->pathProbeMode = std::move(pathText.mode);
            existing->second->pathProbeNavmeshReason = std::move(pathText.navmeshReason);
            existing->second->pathProbeGroundLineReason = std::move(pathText.groundLineReason);
            return Finish(true, QuestAcquisitionResponse(*existing->second));
        }
        if (existing != questAcquisitionSessions.end())
        {
            existing->second->ReleaseTerminalTravelTarget(leader, travelTarget);
            questAcquisitionSessions.erase(existing);
        }

        EnableLeagueNoTeleport(group);

        uint32 aliveMembers = 0;
        std::vector<Player*> alivePartyMembers;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            if (Player* member = reference->GetSource(); member && member->IsAlive())
            {
                ++aliveMembers;
                alivePartyMembers.push_back(member);
            }

        WorldPosition leaderPosition(leader);
        std::vector<AutoWowQuestAcquisition::Candidate> facts;
        std::vector<WorldPosition> live;
        std::vector<GuidPosition> giverSpawns;
        uint32 remoteCandidates = 0;
        uint32 sameMapEligibleCandidates = 0;
        uint32 unresolvedGivers = 0;
        bool partyNoCommonEligible = false;
        std::string lastGiverResolutionFailure = "none";

        // The shared quest-position value is built from ObjectMgr spawn data, so it covers
        // unloaded grids without borrowing the legacy TravelMgr quest destination pointers.
        // Those destinations are non-owning; this session instead copies the selected point.
        // The map is a compute-once process-lifetime value, so a const reference avoids a full copy.
        questGuidpMap const& questMap =
            sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();
        for (auto const& [questId, relationMap] : questMap)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            bool hasNpc = false;
            bool hasGameObject = false;
            bool hasItem = false;
            for (uint8 slot = 0; slot < QUEST_OBJECTIVES_COUNT; ++slot)
            {
                if (!quest->RequiredNpcOrGo[slot] || !quest->RequiredNpcOrGoCount[slot])
                    continue;
                hasNpc = hasNpc || quest->RequiredNpcOrGo[slot] > 0;
                hasGameObject = hasGameObject || quest->RequiredNpcOrGo[slot] < 0;
            }
            for (uint8 slot = 0; slot < QUEST_ITEM_OBJECTIVES_COUNT; ++slot)
                hasItem = hasItem || (quest->RequiredItemId[slot] && quest->RequiredItemCount[slot]);

            AutoWowQuestLog::Capability const capability = AutoWowQuestLog::ClassifyCapability(
                quest->GetType(), quest->GetSuggestedPlayers(), quest->GetSrcItemId(),
                hasNpc, hasGameObject, hasItem, false);
            bool const activeQuest = leader->GetQuestStatus(questId) != QUEST_STATUS_NONE ||
                leader->IsQuestRewarded(questId);
            bool const coreEligible = !activeQuest && leader->GetMap()->GetEntry()->IsWorldMap() &&
                quest->GetQuestLevel() < static_cast<int32>(leader->GetLevel()) + 5 &&
                leader->CanTakeQuest(quest, false) && leader->CanAddQuest(quest, false);
            bool const partySuitable = quest->GetSuggestedPlayers() <= aliveMembers;

            std::vector<AutoWowQuestGiverTravel::PartyQuestEligibilityFacts> const
                partyPrerequisiteFacts = [&]()
            {
                std::vector<AutoWowQuestGiverTravel::PartyQuestEligibilityFacts> result;
                result.reserve(alivePartyMembers.size());
                for (Player* member : alivePartyMembers)
                {
                    bool const questStatusNone = member->GetQuestStatus(questId) == QUEST_STATUS_NONE &&
                        !member->IsQuestRewarded(questId);
                    result.push_back({
                        true,
                        questStatusNone,
                        member->CanTakeQuest(quest, false),
                        member->CanAddQuest(quest, false),
                        false,
                        false});
                }
                return result;
            }();
            bool const candidatePoolEligible = !activeQuest && coreEligible && partySuitable &&
                capability.supported;
            if (candidatePoolEligible &&
                !AutoWowQuestGiverTravel::IsCommonPartyQuestEligible(partyPrerequisiteFacts))
            {
                // Do not route a cohesive party to a quest that a follower cannot accept. This is
                // deliberately counted as a pool-level failure so the response can distinguish it
                // from an empty route or a missing giver.
                partyNoCommonEligible = true;
                continue;
            }

            for (auto const& [relationMask, entrySpawns] : relationMap)
            {
                if (!HasQuestRelationFlag(relationMask, QuestRelationFlag::questGiver))
                    continue;

                for (auto const& [giverEntry, spawns] : entrySpawns)
                {
                    std::vector<AutoWowQuestGiverTravel::PartyQuestEligibilityFacts>
                        partyEntryFacts = partyPrerequisiteFacts;
                    if (giverEntry > 0)
                    {
                        CreatureTemplate const* creatureTemplate =
                            sObjectMgr->GetCreatureTemplate(static_cast<uint32>(giverEntry));
                        FactionTemplateEntry const* giverFaction = creatureTemplate ?
                            sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
                        if (!giverFaction || leader->GetFactionReactionTo(
                                leader->GetFactionTemplateEntry(), giverFaction) < REP_FRIENDLY)
                            continue;

                        if (candidatePoolEligible)
                        {
                            for (std::size_t memberIndex = 0;
                                 memberIndex < alivePartyMembers.size(); ++memberIndex)
                            {
                                partyEntryFacts[memberIndex].exactGiverEligibilityKnown = true;
                                partyEntryFacts[memberIndex].exactGiverEligible =
                                    alivePartyMembers[memberIndex]->GetFactionReactionTo(
                                        alivePartyMembers[memberIndex]->GetFactionTemplateEntry(),
                                        giverFaction) >= REP_FRIENDLY;
                            }
                            if (!AutoWowQuestGiverTravel::IsCommonPartyQuestEligible(partyEntryFacts))
                            {
                                partyNoCommonEligible = true;
                                continue;
                            }
                        }
                    }

                    for (GuidPosition const& spawn : spawns)
                    {
                        WorldPosition point(spawn);
                        MapEntry const* candidateMap = sMapStore.LookupEntry(point.GetMapId());
                        if (!candidateMap || !candidateMap->IsWorldMap())
                            continue;

                        // Outbound distance matters for directional portal edges.
                        double const distance = leaderPosition.distance(&point);
                        if (distance >= 200000.0)
                            continue;

                        uint32 const persistentSpawnGuid = spawn.GetCounter();
                        bool const persistentSpawn = persistentSpawnGuid &&
                            (giverEntry > 0
                                 ? sObjectMgr->GetCreatureData(persistentSpawnGuid) != nullptr
                                 : giverEntry < 0 &&
                                       sObjectMgr->GetGameObjectData(persistentSpawnGuid) != nullptr);
                        if (persistentSpawn && !AutoWowQuestAcquisition::IsEventBoundSpawnEligible(
                                QuestGiverSpawnEventBindings(persistentSpawnGuid, giverEntry > 0),
                                ActiveQuestGiverEvents()))
                            continue;

                        if (point.GetMapId() != leader->GetMapId())
                        {
                            if (!activeQuest && coreEligible && partySuitable && capability.supported)
                                ++remoteCandidates;
                            continue;
                        }

                        if (!activeQuest && coreEligible && partySuitable && capability.supported)
                            ++sameMapEligibleCandidates;

                        Map* leaderMap = leader->GetMap();
                        bool const gridLoaded = leaderMap && leaderMap->IsGridLoaded(
                            point.GetPositionX(), point.GetPositionY());
                        GuidPosition liveSpawn = spawn;
                        // Do not resolve an object before its authoritative spawn grid is loaded.
                        // The persistent position/spawn ID is sufficient for route selection.
                        WorldObject* liveGiver = gridLoaded ? liveSpawn.GetWorldObject() : nullptr;
                        Creature* liveCreature = gridLoaded && liveSpawn.IsCreature()
                            ? liveSpawn.GetCreature()
                            : nullptr;
                        AutoWowQuestAcquisition::LiveGiverFacts const giverFacts{
                            liveGiver ? liveGiver->GetGUID().GetCounter() : 0,
                            giverEntry,
                            liveGiver ? liveGiver->GetEntry() : 0,
                            liveSpawn.IsCreature(),
                            liveSpawn.IsGameObject(),
                            liveGiver && liveGiver->IsInWorld() &&
                                liveGiver->GetMapId() == point.GetMapId(),
                            !liveSpawn.IsCreature() || (liveCreature && liveCreature->IsAlive()),
                            liveGiver && liveGiver->hasQuest(questId)};
                        AutoWowQuestAcquisition::GiverResolutionFailure const giverResolution =
                            AutoWowQuestAcquisition::ClassifyGiverResolution(giverFacts);
                        if (giverResolution != AutoWowQuestAcquisition::GiverResolutionFailure::None)
                        {
                            if (persistentSpawn && !gridLoaded)
                            {
                                std::size_t const sourceIndex = live.size();
                                live.emplace_back(point);
                                giverSpawns.emplace_back(std::move(liveSpawn));
                                facts.push_back({sourceIndex, questId, giverEntry, point.GetMapId(),
                                                 point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(),
                                                 distance, activeQuest, coreEligible, partySuitable,
                                                 capability.supported, 0, persistentSpawnGuid});
                                continue;
                            }
                            ++unresolvedGivers;
                            lastGiverResolutionFailure = std::string(
                                AutoWowQuestAcquisition::GiverResolutionFailureName(giverResolution));
                            continue;
                        }

                        if (candidatePoolEligible && liveGiver)
                        {
                            std::vector<AutoWowQuestGiverTravel::PartyQuestEligibilityFacts>
                                partyGiverFacts = partyEntryFacts;
                            for (std::size_t memberIndex = 0;
                                 memberIndex < alivePartyMembers.size(); ++memberIndex)
                            {
                                // Player::CanInteractWithQuestGiver includes distance. A follower
                                // who is still en route must not fail candidate selection merely
                                // because the route has not brought them to this giver yet. For a
                                // member already in range, enforce the complete live-object check;
                                // otherwise retain only the distance-independent checks above and
                                // defer the interaction postcondition to arrival.
                                if (alivePartyMembers[memberIndex]->IsWithinDistInMap(
                                        liveGiver, INTERACTION_DISTANCE))
                                {
                                    partyGiverFacts[memberIndex].exactGiverEligibilityKnown = true;
                                    partyGiverFacts[memberIndex].exactGiverEligible =
                                        alivePartyMembers[memberIndex]->CanInteractWithQuestGiver(liveGiver);
                                }
                            }
                            if (!AutoWowQuestGiverTravel::IsCommonPartyQuestEligible(partyGiverFacts))
                            {
                                partyNoCommonEligible = true;
                                continue;
                            }
                        }

                        std::size_t const sourceIndex = live.size();
                        live.emplace_back(point);
                        giverSpawns.emplace_back(std::move(liveSpawn));
                        facts.push_back({sourceIndex, questId, giverEntry, point.GetMapId(),
                                         point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(), distance,
                                         activeQuest, coreEligible, partySuitable, capability.supported,
                                         giverFacts.liveGiverGuid, persistentSpawnGuid});
                    }
                }
            }
        }

        std::optional<std::size_t> const selected = AutoWowQuestAcquisition::SelectCandidate(facts);
        if (!selected)
        {
            bool const giverResolutionFailed = sameMapEligibleCandidates > 0 && unresolvedGivers > 0;
            char const* const reason = partyNoCommonEligible
                ? "party_no_common_eligible_quest"
                : giverResolutionFailed ? "giver_resolution_failed"
                : remoteCandidates > 0 ? "cross_map_route_unavailable" : "no_supported_eligible_questgiver";
            std::ostringstream out;
            out << "{\"ok\":true,\"order\":\"quest\",\"phase\":\"acquire\",\"state\":\"blocked\""
                << ",\"guid\":" << m_request.botGuid
                << ",\"quest_id\":0,\"giver_entry\":0,\"candidates\":" << facts.size()
                << ",\"remote_candidates\":" << remoteCandidates
                << ",\"unresolved_givers\":" << unresolvedGivers
                << ",\"party_no_common_eligible\":"
                << (partyNoCommonEligible ? "true" : "false")
                << ",\"giver_resolution\":" << JsonString(giverResolutionFailed ? "failed" : "not_attempted")
                << ",\"giver_resolution_failure\":" << JsonString(lastGiverResolutionFailure)
                << ",\"giver_guid\":0,\"reason\":" << JsonString(reason) << "}";
            return Finish(true, out.str());
        }

        AutoWowQuestAcquisition::Candidate const& chosen = facts[*selected];
        WorldPosition const& chosenPoint = live[chosen.sourceIndex];
        GuidPosition chosenSpawn = giverSpawns[chosen.sourceIndex];
        bool const persistentIdentity = chosen.persistentSpawnGuid &&
            chosenSpawn.GetCounter() == chosen.persistentSpawnGuid &&
            chosenSpawn.GetMapId() == chosen.mapId &&
            chosenSpawn.GetMapId() == chosenPoint.GetMapId();
        WorldObject* chosenGiver = chosen.liveGiverGuid ? chosenSpawn.GetWorldObject() : nullptr;
        bool const runtimeIdentity = !chosen.liveGiverGuid ||
            (chosenGiver && AutoWowQuestAcquisition::IsMatchingPersistentGiver({
                chosen.persistentSpawnGuid,
                chosen.liveGiverGuid,
                chosenGiver->GetGUID().GetCounter(),
                GetResolvedGiverSpawnId(chosenGiver),
                chosenPoint.GetMapId(),
                chosenGiver->GetMapId(),
                chosen.giverEntry,
                chosenGiver->GetEntry(),
                chosenSpawn.IsCreature(),
                chosenSpawn.IsGameObject()}));
        if (!persistentIdentity || !runtimeIdentity)
        {
            std::ostringstream out;
            out << "{\"ok\":true,\"order\":\"quest\",\"phase\":\"acquire\",\"state\":\"blocked\""
                << ",\"guid\":" << m_request.botGuid
                << ",\"quest_id\":" << chosen.questId
                << ",\"giver_entry\":" << chosen.giverEntry
                << ",\"giver_guid\":0,\"giver_resolution\":\"failed\""
                << ",\"reason\":\"giver_resolution_failed\"}";
            return Finish(true, out.str());
        }
        Quest const* chosenQuest = sObjectMgr->GetQuestTemplate(chosen.questId);
        bool chosenHasNpc = false;
        bool chosenHasGameObject = false;
        bool chosenHasItem = false;
        for (uint8 slot = 0; chosenQuest && slot < QUEST_OBJECTIVES_COUNT; ++slot)
        {
            if (!chosenQuest->RequiredNpcOrGo[slot] || !chosenQuest->RequiredNpcOrGoCount[slot])
                continue;
            chosenHasNpc = chosenHasNpc || chosenQuest->RequiredNpcOrGo[slot] > 0;
            chosenHasGameObject = chosenHasGameObject || chosenQuest->RequiredNpcOrGo[slot] < 0;
        }
        for (uint8 slot = 0; chosenQuest && slot < QUEST_ITEM_OBJECTIVES_COUNT; ++slot)
            chosenHasItem = chosenHasItem ||
                (chosenQuest->RequiredItemId[slot] && chosenQuest->RequiredItemCount[slot]);
        AutoWowQuestLog::Capability const chosenCapability = AutoWowQuestLog::ClassifyCapability(
            chosenQuest ? chosenQuest->GetType() : 0, chosenQuest ? chosenQuest->GetSuggestedPlayers() : 0,
            chosenQuest ? chosenQuest->GetSrcItemId() : 0, chosenHasNpc, chosenHasGameObject, chosenHasItem, false);

        auto session = std::make_unique<QuestAcquisitionSession>(
            m_request.botGuid, chosen.questId, chosen.giverEntry, chosenPoint, chosenSpawn,
            chosen.liveGiverGuid, chosen.distance, leader->GetSpeed(MOVE_RUN), chosenCapability.name);
        QuestAcquisitionSession* active = session.get();
        active->expectedPartyMembers = aliveMembers;
        AutoWowQuestGiverTravel::ClearMovementFeedback(m_request.botGuid);
        questAcquisitionSessions[m_request.botGuid] = std::move(session);

        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI || !member->IsAlive())
                continue;
            memberAI->SetAutoWowIndependentParty(member == leader);
            if (member == leader)
                memberAI->ChangeStrategy("+travel,-grind,-move random,-new rpg,-follow", BOT_STATE_NON_COMBAT);
            else
                memberAI->ChangeStrategy("+follow,-grind,-move random,-new rpg,-travel", BOT_STATE_NON_COMBAT);
        }

        if (!active->Poll(leader))
            return Finish(true, QuestAcquisitionResponse(*active));

        travelTarget->setTarget(&active->destination, &active->giverPoint);
        active->AlignInstalledTravelDeadline(travelTarget);
        EnsureQuestAcquisitionTravelStrategy(leaderAI);
        bool const targetInstalled = travelTarget->getDestination() == &active->destination;
        bool const travelStatus = travelTarget->getStatus() == TRAVEL_STATUS_TRAVEL;
        bool const forced = travelTarget->isForced();
        bool const travelStrategy = leaderAI->HasStrategy("travel", BOT_STATE_NON_COMBAT);
        bool const actionActivated = targetInstalled && travelStatus && travelStrategy &&
            travelTarget->isTraveling();
        active->movementKickAccepted = false;
        MoveToTravelTargetAction stagedMover(leaderAI);
        stagedMover.ExecuteQuestGiverStagedEntry(travelTarget);
        active->ApplyLiveMovementFeedback(leader);
        if (active->state == QuestAcquisitionState::Pending)
            active->ArmNativeMovementTick();
        AutoWowQuestAcquisition::MovementOrderFacts const movementFacts{
            targetInstalled, travelStatus, forced, travelStrategy, actionActivated};
        AutoWowQuestAcquisition::MovementActivationFailure const movementFailure =
            AutoWowQuestAcquisition::ClassifyMovementActivation(movementFacts);
        active->movementActivation = std::string(
            AutoWowQuestAcquisition::MovementActivationFailureName(movementFailure));
        bool const leaderParked = active->movementState == "leader_parked_waiting_followers";
        AutoWowQuestAcquisition::MovementActionResult const actionResult = !actionActivated || leaderParked
            ? AutoWowQuestAcquisition::MovementActionResult::NotAttempted
            : active->movementKickAccepted
                ? AutoWowQuestAcquisition::MovementActionResult::Accepted
                : AutoWowQuestAcquisition::MovementActionResult::Rejected;
        QuestAcquisitionPathText pathText;
        CaptureQuestAcquisitionMovement(
            *active, leader, leaderAI, travelTarget, actionResult, movementFacts,
            active->movementDiagnostics, pathText, true);
        active->pathProbeReason = std::move(pathText.reason);
        active->pathProbeMode = std::move(pathText.mode);
        active->pathProbeNavmeshReason = std::move(pathText.navmeshReason);
        active->pathProbeGroundLineReason = std::move(pathText.groundLineReason);
        bool const movementOrderActive = AutoWowQuestAcquisition::IsMovementOrderActive(movementFacts);
        if (!movementOrderActive)
        {
            active->SetBlocked("movement_order_not_active");
            active->ReleaseTerminalTravelTarget(leader, travelTarget);
        }
        return Finish(true, QuestAcquisitionResponse(*active));
    }

    bool QuestParty()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));
        if (leaderAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("quest_requires_party_leader"));

        // Optional explicit quest selection. The bridge NEVER grants, completes, abandons, or edits
        // quests: it only validates that the requested quest is genuinely in the leader's log and is
        // compatible with the party, then constrains the normal route/RPG selection to that quest.
        uint32 requestedQuest = 0;
        if (!m_request.destination.empty())
            requestedQuest = static_cast<uint32>(std::strtoul(m_request.destination.c_str(), nullptr, 10));

        Quest const* requestedTemplate = nullptr;
        AutoWowQuestParty::ParticipantPlan participantPlan;
        if (requestedQuest)
        {
            QuestStatusMap const& statusMap = leader->getQuestStatusMap();
            if (statusMap.find(requestedQuest) == statusMap.end() && !leader->IsQuestRewarded(requestedQuest))
            {
                std::ostringstream out;
                out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                    << ",\"phase\":\"blocked\",\"requested\":true,\"quest_id\":" << requestedQuest
                    << ",\"reason\":\"quest_not_in_leader_log\"}";
                return Finish(true, out.str());
            }

            requestedTemplate = sObjectMgr->GetQuestTemplate(requestedQuest);
            uint32 aliveMembers = 0;
            for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
                if (Player* member = reference->GetSource(); member && member->IsAlive())
                    ++aliveMembers;
            // Party compatibility: a quest is incompatible only when it suggests more players than
            // the party currently fields (matches the engine's group gate; normal quests suggest 0).
            if (!requestedTemplate || requestedTemplate->GetSuggestedPlayers() > aliveMembers)
            {
                std::ostringstream out;
                out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                    << ",\"phase\":\"blocked\",\"requested\":true,\"quest_id\":" << requestedQuest
                    << ",\"reason\":\"quest_not_party_compatible\"}";
                return Finish(true, out.str());
            }

            // An explicit quest directive uses a participant-set contract. The leader must be an
            // active participant, while other valid Playerbots may escort without owning the exact
            // quest. Only participants are ever primed or included in quest postcondition proof.
            std::ostringstream memberReceipts;
            bool firstMemberReceipt = true;
            std::vector<AutoWowQuestParty::MemberFacts> memberFacts;
            for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            {
                Player* member = reference->GetSource();
                PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                QuestPartyMemberReceiptView view = InspectQuestPartyMember(member, memberAI, requestedQuest);
                AutoWowQuestParty::MemberFacts const facts =
                    MakeQuestPartyMemberFacts(member, view, member == leader);
                memberFacts.push_back(facts);
                AutoWowQuestParty::MemberDisposition const disposition =
                    AutoWowQuestParty::Classify(facts);
                view.questParticipant = disposition == AutoWowQuestParty::MemberDisposition::Participant;
                if (disposition == AutoWowQuestParty::MemberDisposition::InvalidRoster)
                {
                    if (!member)
                        view.reason = "member_missing";
                    else if (!memberAI || !view.playerbot)
                        view.reason = "member_not_playerbot";
                }
                else if (disposition == AutoWowQuestParty::MemberDisposition::NonParticipant)
                    view.reason = QuestPartyNonParticipantReason(member, view);
                AppendQuestPartyMemberReceipt(memberReceipts, firstMemberReceipt, view);
            }

            participantPlan = AutoWowQuestParty::BuildPlan(memberFacts);
            if (participantPlan.decision == AutoWowQuestParty::Decision::RejectRoster ||
                participantPlan.decision == AutoWowQuestParty::Decision::RejectLeader)
            {
                std::ostringstream out;
                out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                    << ",\"requested\":true,\"party_exact\":true,\"party_contract\":\"participant_set\""
                    << ",\"phase\":\"blocked\",\"quest_id\":" << requestedQuest
                    << ",\"result_quest\":0,\"reason\":\"party_member_exact_quest_preflight_failed\""
                    << ",\"participant_decision\":"
                    << JsonString(std::string(AutoWowQuestParty::DecisionName(participantPlan.decision)))
                    << ",\"participant_count\":" << participantPlan.participantCount
                    << ",\"nonparticipant_count\":" << participantPlan.nonParticipantCount
                    << ",\"leader_participant\":"
                    << (participantPlan.leaderParticipates ? "true" : "false")
                    << ",\"member_receipts\":[" << memberReceipts.str() << "]}";
                return Finish(true, out.str());
            }
        }

        // League parties must report movement failure to the external director rather than
        // silently using New-RPG's ordinary teleport fallback.
        EnableLeagueNoTeleport(group);

        // Re-arm this in-memory mode on every quest dispatch.  Groups survive a server restart,
        // but PlayerbotAI state does not; relying on CreateParty() alone would let the native
        // group-maintenance tick rebuild a bot-to-bot master/follow chain on the first resumed
        // quest.  The quest order is the idempotent boundary for the persistent campaign, so each
        // live member starts from its own native quest/combat loop before an explicit operation
        // elects any temporary coordination.
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI || memberAI->IsAutoWowPaused() || !member->IsAlive())
                continue;

            memberAI->SetAutoWowIndependentParty(true);
            memberAI->ChangeStrategy(
                "+grind,+new rpg,-follow,-move random,-travel", BOT_STATE_NON_COMBAT);
        }

        uint32 questId = 0;
        QuestStatus questStatus = QUEST_STATUS_NONE;
        std::string phase = "acquire";
        std::vector<TravelDestination*> destinations;
        uint32 starterAccepts = 0;
        uint32 starterEligible = 0;
        bool starterFound = false;
        bool starterInRange = false;
        std::set<std::string> starterBlockers;

        auto selectQuestRoute = [&]()
        {
            questId = 0;
            questStatus = QUEST_STATUS_NONE;
            phase = "acquire";
            destinations.clear();

            // Completed quests take precedence so the party first collects legitimate rewards and unlocks
            // follow-up quests. Otherwise select the first active objective. New quest givers are only
            // considered when the leader has no actionable quest.
            for (QuestStatus const wantedStatus : { QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE })
            {
                for (auto const& entry : leader->getQuestStatusMap())
                {
                    if (requestedQuest && entry.first != requestedQuest)
                        continue;
                    if (leader->IsQuestRewarded(entry.first) || entry.second.Status != wantedStatus)
                        continue;

                    std::vector<TravelDestination*> candidate =
                        TravelMgr::instance().getQuestTravelDestinations(leader, entry.first, true, false, 5000.0f);
                    if (candidate.empty())
                        continue;

                    questId = entry.first;
                    questStatus = wantedStatus;
                    phase = wantedStatus == QUEST_STATUS_COMPLETE ? "turnin" : "objective";
                    destinations = std::move(candidate);
                    break;
                }
                if (!destinations.empty())
                    break;
            }

            // Generic new-questgiver discovery is only for the auto path; an explicit quest must
            // route to its own objective/turn-in, never wander to a different questgiver.
            if (destinations.empty() && !requestedQuest)
                destinations = TravelMgr::instance().getQuestTravelDestinations(leader, -1, true, false, 5000.0f);

            return !destinations.empty();
        };

        auto findActiveQuest = [&](uint32& activeQuestId, QuestStatus& activeQuestStatus)
        {
            activeQuestId = 0;
            activeQuestStatus = QUEST_STATUS_NONE;
            for (QuestStatus const wantedStatus : { QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE })
            {
                for (auto const& entry : leader->getQuestStatusMap())
                {
                    if (requestedQuest && entry.first != requestedQuest)
                        continue;
                    if (leader->IsQuestRewarded(entry.first) || entry.second.Status != wantedStatus)
                        continue;

                    if (!sObjectMgr->GetQuestTemplate(entry.first))
                        continue;

                    activeQuestId = entry.first;
                    activeQuestStatus = wantedStatus;
                    return true;
                }
            }
            return false;
        };

        // An explicit Director selection always uses the Phase 1 New-RPG state machine. The legacy
        // TravelTarget route can reach a POI, but it cannot advance ResolveObjective/Travel/Acquire/
        // Loot/Verify phases or preserve the strict objective lock between ticks.
        if (!requestedQuest && !selectQuestRoute())
        {
            // A newly staged party has no active quest yet, and Playerbots' nearest-NPC cache can be stale
            // immediately after a teleport. Use only the real questgiver standing beside the party and send
            // the normal accept-quest packet through Playerbots. This bootstrap never grants a quest directly.
            uint32 starterEntry = 0;
            uint32 starterQuest = 0;
            uint32 starterSpawnId = 0;
            Creature* starter = nullptr;

            // Bootstrap routes can share a continent, so map id alone is not enough to identify
            // the staged questgiver. Select only a loaded, nearby known starter; this keeps the
            // normal accept packet path while avoiding a distant starter from another route.
            auto selectNearbyStarter = [&](uint32 entry, uint32 questId, uint32 spawnId)
            {
                Creature* candidate = ObjectAccessor::GetSpawnedCreatureByDBGUID(leader->GetMapId(), spawnId);
                if (!candidate || !leader->IsWithinDistInMap(candidate, INTERACTION_DISTANCE * 6))
                    candidate = leader->FindNearestCreature(entry, INTERACTION_DISTANCE * 6);
                if (!candidate)
                    return false;

                starterEntry = entry;
                starterQuest = questId;
                starterSpawnId = spawnId;
                starter = candidate;
                return true;
            };

            if (leader->GetMapId() == 0)
            {
                // Erland: Escorting Erland; Deputy Willem: A Threat Within.
                if (!selectNearbyStarter(1978, 435, 19107))
                    selectNearbyStarter(823, 783, 79942);
            }
            else if (leader->GetMapId() == 1)
            {
                // Lar Prowltusk: Thwarting Kolkar Aggression; Foreman Thazz'ril: Lazy Peons.
                if (!selectNearbyStarter(3140, 786, 4699))
                    selectNearbyStarter(11378, 5441, 3427);
            }

            // The group has been staged at this questgiver. Resolve the actual loaded spawn first rather
            // than relying solely on Playerbots' nearby-unit cache after a teleport.
            Quest const* starterTemplate = starterQuest ? sObjectMgr->GetQuestTemplate(starterQuest) : nullptr;
            starterFound = starter != nullptr;
            starterInRange = starter && leader->IsWithinDistInMap(starter, INTERACTION_DISTANCE * 3);
            if (starter && starterTemplate && starterInRange)
            {
                for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
                {
                    Player* member = reference->GetSource();
                    PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                    if (!member || !memberAI || !member->IsAlive())
                        continue;

                    if (!member->CanTakeQuest(starterTemplate, false))
                    {
                        if (!member->SatisfyQuestStatus(starterTemplate, false))
                            starterBlockers.insert("status");
                        else if (!member->SatisfyQuestExclusiveGroup(starterTemplate, false))
                            starterBlockers.insert("exclusive_group");
                        else if (!member->SatisfyQuestClass(starterTemplate, false))
                            starterBlockers.insert("class");
                        else if (!member->SatisfyQuestRace(starterTemplate, false))
                            starterBlockers.insert("race");
                        else if (!member->SatisfyQuestLevel(starterTemplate, false))
                            starterBlockers.insert("level");
                        else if (!member->SatisfyQuestSkill(starterTemplate, false))
                            starterBlockers.insert("skill");
                        else if (!member->SatisfyQuestReputation(starterTemplate, false))
                            starterBlockers.insert("reputation");
                        else if (!member->SatisfyQuestPreviousQuest(starterTemplate, false))
                            starterBlockers.insert("previous_quest");
                        else if (!member->SatisfyQuestTimed(starterTemplate, false))
                            starterBlockers.insert("timed");
                        else if (!member->SatisfyQuestNextChain(starterTemplate, false))
                            starterBlockers.insert("next_chain");
                        else if (!member->SatisfyQuestPrevChain(starterTemplate, false))
                            starterBlockers.insert("previous_chain");
                        else if (!member->SatisfyQuestBreadcrumb(starterTemplate, false))
                            starterBlockers.insert("breadcrumb");
                        else if (!member->SatisfyQuestDay(starterTemplate, false))
                            starterBlockers.insert("daily_limit");
                        else if (!member->SatisfyQuestWeek(starterTemplate, false))
                            starterBlockers.insert("weekly_limit");
                        else if (!member->SatisfyQuestMonth(starterTemplate, false))
                            starterBlockers.insert("monthly_limit");
                        else if (!member->SatisfyQuestSeasonal(starterTemplate, false))
                            starterBlockers.insert("seasonal");
                        else if (!member->SatisfyQuestConditions(starterTemplate, false))
                            starterBlockers.insert("conditions");
                        else
                            starterBlockers.insert("disabled_or_unknown");
                        continue;
                    }

                    ++starterEligible;

                    WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST);
                    packet << starter->GetGUID() << starterQuest << uint32(0);
                    packet.rpos(0);
                    if (memberAI->DoSpecificAction("accept quest", Event("autowow starter quest", packet, member), true))
                        ++starterAccepts;
                }
            }

            selectQuestRoute();
        }

        if (destinations.empty())
        {
            if (requestedQuest)
            {
                // Explicit quest orders drive every playerbot through the same durable New-RPG
                // directive. No broad quest-giver scan is permitted here; finisher interaction is
                // selected and executed by NewRpgDoQuestAction for each actual participant;
                // nonparticipants remain escorts and are never used as quest proof.
                std::ostringstream memberReceipts;
                bool firstMemberReceipt = true;
                bool allDirectivesActive = true;
                bool allHasExactFinisher = true;
                bool allReachedExactFinisher = true;
                bool allExactRelations = true;
                bool allCoreCompleted = true;
                bool allCoreRewards = true;
                bool allObjectivesDone = true;
                uint32 primedMemberCount = 0;
                uint32 oracleRuntimePendingCount = 0;
                uint32 memberCount = 0;
                uint32 participantCount = 0;
                uint32 nonparticipantCount = 0;

                for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
                {
                    ++memberCount;
                    Player* member = reference->GetSource();
                    PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
                    QuestPartyMemberReceiptView view = InspectQuestPartyMember(member, memberAI, requestedQuest);
                    AutoWowQuestParty::MemberFacts const facts =
                        MakeQuestPartyMemberFacts(member, view, member == leader);
                    bool const memberParticipant = AutoWowQuestParty::IsParticipant(facts);
                    view.questParticipant = memberParticipant;

                    if (memberParticipant)
                    {
                        ++participantCount;
                        bool const oracleManaged =
                            AutoWowOracleRuntime::IsManagedBot(member->GetGUID().GetCounter());
                        auto* currentRpgQuest = std::get_if<NewRpgInfo::DoQuest>(&memberAI->rpgInfo.data);
                        if (!currentRpgQuest || currentRpgQuest->questId != requestedQuest)
                            memberAI->rpgInfo.ChangeToDoQuest(
                                requestedQuest, requestedTemplate, oracleManaged);
                        else
                            currentRpgQuest->objectiveRuntime.oracleManaged = oracleManaged;

                        if (AiObjectContext* activeContext = memberAI->GetAiObjectContext())
                        {
                            if (Value<QuestObjectiveSpec>* objectiveValue =
                                    activeContext->GetValue<QuestObjectiveSpec>("active quest objective"))
                                objectiveValue->Reset();
                            if (Value<QuestFinisherRef>* finisherValue =
                                    activeContext->GetValue<QuestFinisherRef>("active quest finisher"))
                                finisherValue->Reset();
                        }

                        // Every participant retains its own objective, travel, combat, loot, and
                        // reward executor. Group cohesion may still create a temporary follow
                        // preference when a member is genuinely separated, but no participant is
                        // permanently controlled by the party leader and native maintenance must
                        // not recreate that relationship after a restart.
                        memberAI->SetAutoWowIndependentParty(true);
                        memberAI->ChangeStrategy(
                            "+grind,-travel,-move random,-follow,+new rpg", BOT_STATE_NON_COMBAT);
                        AutoWowOracleQuestDispatchPolicy::OwnershipState const preLease{
                            oracleManaged, false, false, false, 0};
                        AutoWowOracleQuestDispatchPolicy::EventState const untaggedBridgeEvent{};
                        bool const bridgeMayDispatch = AutoWowOracleQuestDispatchPolicy::Evaluate(
                            preLease, untaggedBridgeEvent) ==
                            AutoWowOracleQuestDispatchPolicy::Result::Ordinary;
                        if (bridgeMayDispatch)
                            view.primingActionStarted = memberAI->DoSpecificAction(
                                "new rpg do quest", Event("autowow quest", "", member), true);
                        else
                            ++oracleRuntimePendingCount;
                        if (view.primingActionStarted)
                            ++primedMemberCount;

                        QuestPartyMemberReceiptView after = InspectQuestPartyMember(member, memberAI, requestedQuest);
                        after.primingActionStarted = view.primingActionStarted;
                        after.questParticipant = true;
                        view = std::move(after);
                        if (oracleManaged && !view.coreRewardConfirmed)
                            view.reason = "oracle_runtime_pending";
                        if (!view.directiveActive)
                            view.reason = "directive_not_active_after_prime";
                    }
                    else
                    {
                        ++nonparticipantCount;
                        view.reason = QuestPartyNonParticipantReason(member, view);
                        // Not owning the leader's requested quest is not a command to become an
                        // idle escort. Keep the nonparticipant on its own native New-RPG loop so
                        // it can select/execute its own quest, travel, fight, loot, and recover.
                        // It remains excluded from this request's participant proof; a later party
                        // request can enroll it when it owns the exact shared quest.
                        if (member && memberAI && view.playerbot && view.online && member->IsAlive())
                        {
                            memberAI->SetAutoWowIndependentParty(true);
                            memberAI->ChangeStrategy(
                                "+grind,+new rpg,-follow,-move random,-travel", BOT_STATE_NON_COMBAT);
                        }
                    }

                    // These aggregates intentionally cover only actual quest participants. A
                    // follower without the quest must never turn a leader-only objective into a
                    // false failure or contribute a completion/reward proof.
                    if (memberParticipant)
                    {
                        if (!view.rewarded)
                        {
                            allDirectivesActive = allDirectivesActive && view.directiveActive;
                            allHasExactFinisher = allHasExactFinisher && view.hasExactFinisher;
                            allReachedExactFinisher = allReachedExactFinisher && view.reachesExactFinisher;
                            allExactRelations = allExactRelations && view.exactRelationVerified;
                        }
                        allCoreCompleted = allCoreCompleted && view.coreCompleted;
                        allCoreRewards = allCoreRewards && view.coreRewardConfirmed;
                        allObjectivesDone = allObjectivesDone && view.objectiveDone;
                    }
                    AppendQuestPartyMemberReceipt(memberReceipts, firstMemberReceipt, view);
                }

                char const* partyPhase = "blocked";
                if (allCoreRewards)
                    partyPhase = "complete";
                else if (allDirectivesActive && allHasExactFinisher)
                    partyPhase = "turnin";
                else if (allDirectivesActive && !allObjectivesDone)
                    partyPhase = "objective";

                std::ostringstream out;
                out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                    << ",\"requested\":true,\"party_exact\":true,\"party_contract\":\"participant_set\""
                    << ",\"phase\":" << JsonString(partyPhase)
                    << ",\"quest_id\":" << requestedQuest
                    << ",\"result_quest\":" << requestedQuest
                    << ",\"quest_status\":" << static_cast<uint32>(leader->GetQuestStatus(requestedQuest))
                    << ",\"rpg_driven\":true"
                    << ",\"member_count\":" << memberCount
                    << ",\"participant_count\":" << participantCount
                    << ",\"nonparticipant_count\":" << nonparticipantCount
                    << ",\"participant_decision\":"
                    << JsonString(std::string(AutoWowQuestParty::DecisionName(participantPlan.decision)))
                    << ",\"leader_participant\":"
                    << (participantPlan.leaderParticipates ? "true" : "false")
                    << ",\"primed_member_count\":" << primedMemberCount
                    << ",\"oracle_runtime_pending_count\":" << oracleRuntimePendingCount
                    << ",\"all_participant_directives_active\":"
                    << (allDirectivesActive ? "true" : "false")
                    << ",\"all_participant_has_exact_finisher\":"
                    << (allHasExactFinisher ? "true" : "false")
                    << ",\"all_participant_reached_exact_finisher\":"
                    << (allReachedExactFinisher ? "true" : "false")
                    << ",\"all_participant_exact_relations_verified\":"
                    << (allExactRelations ? "true" : "false")
                    << ",\"all_participant_core_completed\":"
                    << (allCoreCompleted ? "true" : "false")
                    << ",\"all_participant_core_reward_confirmed\":"
                    << (allCoreRewards ? "true" : "false")
                    // Backward-compatible aliases; their scope is now the participant set.
                    << ",\"all_member_directives_active\":" << (allDirectivesActive ? "true" : "false")
                    << ",\"all_has_exact_finisher\":" << (allHasExactFinisher ? "true" : "false")
                    << ",\"all_reached_exact_finisher\":" << (allReachedExactFinisher ? "true" : "false")
                    << ",\"all_exact_relations_verified\":" << (allExactRelations ? "true" : "false")
                    << ",\"all_core_completed\":" << (allCoreCompleted ? "true" : "false")
                    << ",\"all_core_reward_confirmed\":" << (allCoreRewards ? "true" : "false")
                    << ",\"member_receipts\":[" << memberReceipts.str() << "]"
                    << ",\"accept_attempts\":0,\"turnin_attempts\":0}";
                return Finish(true, out.str());
            }

            // Some legitimate low-level quests do not have a TravelMgr route. Playerbots' RPG quest
            // action still knows their client POIs and objective state, so use that normal action instead
            // of falling back to unbounded random movement or invented quest progress.
            uint32 activeQuestId = 0;
            QuestStatus activeQuestStatus = QUEST_STATUS_NONE;
            if (findActiveQuest(activeQuestId, activeQuestStatus))
            {
                Quest const* activeQuest = sObjectMgr->GetQuestTemplate(activeQuestId);
                if (activeQuest)
                {
                    bool const oracleManaged =
                        AutoWowOracleRuntime::IsManagedBot(leader->GetGUID().GetCounter());
                    auto* currentRpgQuest = std::get_if<NewRpgInfo::DoQuest>(&leaderAI->rpgInfo.data);
                    if (!currentRpgQuest || currentRpgQuest->questId != activeQuestId)
                        leaderAI->rpgInfo.ChangeToDoQuest(
                            activeQuestId, activeQuest, oracleManaged);
                    else
                        currentRpgQuest->objectiveRuntime.oracleManaged = oracleManaged;

                    // ChangeToDoQuest replaces the authoritative quest immediately, while these
                    // calculated Values normally cache for one second. Explicit Director orders
                    // prime the phase machine in this same world tick, so invalidate both views to
                    // prevent a stale prior quest from producing a false objective/finisher block.
                    if (AiObjectContext* activeContext = leaderAI->GetAiObjectContext())
                    {
                        activeContext->GetValue<QuestObjectiveSpec>("active quest objective")->Reset();
                        activeContext->GetValue<QuestFinisherRef>("active quest finisher")->Reset();
                    }

                    // Enable the recurring driver before priming it. A movement action may
                    // legitimately defer on its first tick (and return false) while leaving the
                    // authoritative DoQuest directive active. Gating the strategy on that one
                    // return value strands the bot between phases, notably on vertically separated
                    // finishers. The durable matching directive is the success postcondition.
                    leaderAI->ChangeStrategy("+grind,-travel,-move random,-follow,+new rpg", BOT_STATE_NON_COMBAT);
                    AutoWowOracleQuestDispatchPolicy::OwnershipState const preLease{
                        oracleManaged, false, false, false, 0};
                    AutoWowOracleQuestDispatchPolicy::EventState const untaggedBridgeEvent{};
                    bool const bridgeMayDispatch = AutoWowOracleQuestDispatchPolicy::Evaluate(
                        preLease, untaggedBridgeEvent) ==
                        AutoWowOracleQuestDispatchPolicy::Result::Ordinary;
                    bool const primingActionStarted = bridgeMayDispatch && leaderAI->DoSpecificAction(
                        "new rpg do quest", Event("autowow quest", "", leader), true);
                    auto* activeRpgQuest = std::get_if<NewRpgInfo::DoQuest>(&leaderAI->rpgInfo.data);
                    bool const rpgDirectiveActive = activeRpgQuest && activeRpgQuest->questId == activeQuestId;
                    if (rpgDirectiveActive)
                    {
                        bool const finisherDirective = activeRpgQuest->objectiveRuntime.phase ==
                                                           QuestActionPhase::ResolveFinisher ||
                                                       activeRpgQuest->objectiveRuntime.phase ==
                                                           QuestActionPhase::TravelToFinisher ||
                                                       activeRpgQuest->objectiveRuntime.phase ==
                                                           QuestActionPhase::InteractFinisher ||
                                                       activeRpgQuest->objectiveRuntime.phase ==
                                                           QuestActionPhase::VerifyReward ||
                                                       activeRpgQuest->objectiveRuntime.phase == QuestActionPhase::Complete;
                        std::ostringstream out;
                        out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                            << ",\"phase\":" << JsonString(finisherDirective || activeQuestStatus == QUEST_STATUS_COMPLETE
                                                                     ? "turnin" : "objective")
                            << ",\"quest_id\":" << activeQuestId
                            << ",\"result_quest\":" << activeQuestId
                            << ",\"quest_status\":" << static_cast<uint32>(activeQuestStatus)
                            << ",\"destination\":\"playerbots_rpg_poi\""
                            << ",\"rpg_driven\":true"
                            << ",\"oracle_runtime_pending\":" << (oracleManaged ? "true" : "false")
                            << ",\"priming_action_started\":" << (primingActionStarted ? "true" : "false")
                            << ",\"accept_attempts\":0"
                            << ",\"starter_found\":" << (starterFound ? "true" : "false")
                            << ",\"starter_in_range\":" << (starterInRange ? "true" : "false")
                            << ",\"starter_eligible\":" << starterEligible
                            << ",\"starter_accepts\":" << starterAccepts
                            << ",\"turnin_attempts\":0}";
                        return Finish(true, out.str());
                    }

                    // The priming tick explicitly rejected or completed the directive. Restore the
                    // fail-closed idle profile before returning the ordinary blocked response below.
                    leaderAI->ChangeStrategy("-grind,-travel,-move random,-follow,-new rpg", BOT_STATE_NON_COMBAT);
                }
            }

            std::ostringstream starterBlockerText;
            bool firstStarterBlocker = true;
            for (auto const& blocker : starterBlockers)
            {
                if (!firstStarterBlocker)
                    starterBlockerText << ',';
                starterBlockerText << blocker;
                firstStarterBlocker = false;
            }
            std::ostringstream out;
            out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
                << ",\"phase\":\"blocked\",\"quest_id\":0,\"accept_attempts\":0"
                << ",\"starter_found\":" << (starterFound ? "true" : "false")
                << ",\"starter_in_range\":" << (starterInRange ? "true" : "false")
                << ",\"starter_eligible\":" << starterEligible
                << ",\"starter_blockers\":" << JsonString(starterBlockerText.str())
                << ",\"starter_accepts\":" << starterAccepts
                << ",\"turnin_attempts\":0}";
            return Finish(true, out.str());
        }

        TravelDestination* destination = nullptr;
        WorldPosition* point = nullptr;
        WorldPosition leaderPosition(leader);
        for (TravelDestination* candidate : destinations)
        {
            if (!candidate)
                continue;

            std::vector<WorldPosition*> points = candidate->nextPoint(&leaderPosition, true);
            if (points.empty() || !points.front())
                continue;

            destination = candidate;
            point = points.front();
            break;
        }
        if (!destination || !point)
            return Finish(false, ErrorResponse("quest_destination_has_no_reachable_point"));

        AiObjectContext* context = leaderAI->GetAiObjectContext();
        if (!context)
            return Finish(false, ErrorResponse("bot_context_unavailable"));
        TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
        if (!target)
            return Finish(false, ErrorResponse("travel_target_unavailable"));

        target->setTarget(destination, point);
        target->setForced(true);

        leaderAI->ChangeStrategy(phase == "objective" ? "+travel,+grind,-move random,-follow,-new rpg"
                                                        : "+travel,-grind,-move random,-follow,-new rpg",
                                 BOT_STATE_NON_COMBAT);
        leaderAI->DoNextAction();

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"quest\",\"guid\":" << m_request.botGuid
            << ",\"phase\":" << JsonString(phase)
            << ",\"quest_id\":" << questId
            << ",\"quest_status\":" << static_cast<uint32>(questStatus)
            << ",\"destination\":" << JsonString(destination->getTitle())
            << ",\"accept_attempts\":0"
            << ",\"starter_found\":" << (starterFound ? "true" : "false")
            << ",\"starter_in_range\":" << (starterInRange ? "true" : "false")
            << ",\"starter_eligible\":" << starterEligible
            << ",\"starter_accepts\":" << starterAccepts
            << ",\"turnin_attempts\":0}";
        return Finish(true, out.str());
    }

    bool RecoverQuestParty()
    {
        if (!IsLeagueMember(m_request.botGuid))
            return Finish(false, ErrorResponse("bot_not_enrolled_in_league"));

        Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(m_request.botGuid));
        PlayerbotAI* leaderAI = leader ? PlayerbotsMgr::instance().GetPlayerbotAI(leader) : nullptr;
        if (!leader || !leaderAI)
            return Finish(false, ErrorResponse("leader_not_online"));
        if (leaderAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));
        if (leader->IsInCombat())
            return Finish(false, ErrorResponse("recovery_deferred_combat"));

        Group* group = leader->GetGroup();
        if (!group || group->GetLeaderGUID() != leader->GetGUID())
            return Finish(false, ErrorResponse("recovery_requires_party_leader"));

        EnableLeagueNoTeleport(group);

        // Reset only transient Playerbots movement/RPG state. This does not teleport, alter a quest,
        // grant progress, or select a random destination. The director issues a fresh quest plan on
        // the following cycle.
        uint32 resetMembers = 0;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->GetSource();
            PlayerbotAI* memberAI = member ? PlayerbotsMgr::instance().GetPlayerbotAI(member) : nullptr;
            if (!member || !memberAI || memberAI->IsAutoWowPaused())
                continue;

            memberAI->SetAutoWowIndependentParty(true);
            memberAI->Reset(member == leader);
            if (member == leader)
                memberAI->ChangeStrategy("-travel,-grind,-move random,-follow,-new rpg", BOT_STATE_NON_COMBAT);
            else
                // Recovery clears the failed plan; it must not turn the member into an idle
                // escort. The next explicit shared-quest dispatch may temporarily prime +follow,
                // but between plans every bot returns to its own native quest/combat loop.
                memberAI->ChangeStrategy("+grind,+new rpg,-follow,-move random,-travel", BOT_STATE_NON_COMBAT);
            ++resetMembers;
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"recover\",\"guid\":" << m_request.botGuid
            << ",\"recovery\":\"replan_pending\",\"reset_members\":" << resetMembers << "}";
        return Finish(true, out.str());
    }

    std::string OrderResponse(std::string const& order, Player* bot) const
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":" << JsonString(order)
            << ",\"guid\":" << bot->GetGUID().GetCounter() << "}";
        return out.str();
    }

    std::string ListDestinations(Player* bot) const
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"guid\":" << bot->GetGUID().GetCounter() << ",\"destinations\":[";

        bool first = true;
        uint32 count = 0;
        std::set<std::string> titles;
        auto append = [&out, &first, &count, &titles](std::vector<TravelDestination*> const& destinations,
                                                       std::string const& category)
        {
            for (TravelDestination* destination : destinations)
            {
                if (!destination || count >= 25)
                    return;

                std::string title = destination->getTitle();
                if (title.empty() || !titles.insert(title).second)
                    continue;

                if (!first)
                    out << ',';
                out << "{\"title\":" << JsonString(title) << ",\"category\":" << JsonString(category) << "}";
                first = false;
                ++count;
            }
        };

        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (!bot->IsQuestRewarded(entry.first))
                append(TravelMgr::instance().getQuestTravelDestinations(bot, entry.first, true, false, 5000.0f), "quest");
        }
        append(TravelMgr::instance().getQuestTravelDestinations(bot, -1, true, false, 5000.0f), "questgiver");
        append(TravelMgr::instance().getExploreTravelDestinations(bot, true, true), "explore");
        append(TravelMgr::instance().getRpgTravelDestinations(bot, true, true), "rpg");
        append(TravelMgr::instance().getGrindTravelDestinations(bot, true, true, 5000.0f), "grind");
        append(TravelMgr::instance().getBossTravelDestinations(bot, true, true), "boss");

        out << "]}";
        return out.str();
    }

    bool ExecuteTravel(Player* bot, PlayerbotAI* botAI)
    {
        if (botAI->IsAutoWowPaused())
            return Finish(false, ErrorResponse("bot_paused"));

        if (Lowercase(m_request.destination) == "random")
        {
            sRandomPlayerbotMgr.RandomTeleportForLevel(bot);

            std::ostringstream out;
            out << "{\"ok\":true,\"order\":\"travel\",\"guid\":" << bot->GetGUID().GetCounter()
                << ",\"destination\":\"random\",\"mode\":\"level_appropriate\"}";
            return Finish(true, out.str());
        }

        TravelDestination* destination = ChooseTravelTargetAction::FindDestination(bot, m_request.destination);
        if (!destination)
            return Finish(false, ErrorResponse("destination_not_found"));

        WorldPosition botPosition(bot);
        std::vector<WorldPosition*> points = destination->nextPoint(&botPosition, true);
        if (points.empty())
            return Finish(false, ErrorResponse("destination_has_no_reachable_point"));

        AiObjectContext* context = botAI->GetAiObjectContext();
        if (!context)
            return Finish(false, ErrorResponse("bot_context_unavailable"));

        TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
        if (!target)
            return Finish(false, ErrorResponse("travel_target_unavailable"));

        target->setTarget(destination, points.front());
        target->setForced(true);

        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"travel\",\"guid\":" << bot->GetGUID().GetCounter()
            << ",\"destination\":" << JsonString(destination->getTitle()) << "}";
        return Finish(true, out.str());
    }

    AutoWowRequest m_request;
    std::shared_ptr<AutoWowCompletion> m_completion;
};

bool ParseRequest(std::string requestText, AutoWowRequest& request, std::string& error)
{
    requestText = Trim(std::move(requestText));
    std::istringstream input(requestText);
    std::string command;
    input >> command;
    command = Lowercase(command);

    if (command == "list")
    {
        request.type = AutoWowRequestType::List;
        return true;
    }

    if (command == "oraclelog")
    {
        request.type = AutoWowRequestType::OracleLog;
        std::string first;
        std::string second;
        std::string extra;
        if (!(input >> first))
            return true;

        if (input >> second)
        {
            if (input >> extra || !ParseOracleCursor(second, request.oracleCursor))
            {
                error = "oraclelog requires [session_id] [cursor]";
                return false;
            }
            request.oracleSessionId = std::move(first);
            return true;
        }

        if (!ParseOracleCursor(first, request.oracleCursor))
        {
            error = "oraclelog requires [cursor] or <session_id> <cursor>";
            return false;
        }
        return true;
    }

    if (command == "wsg" || command == "wsg-queue" || command == "wsg-status" || command == "wsg-leave")
    {
        AutoWowWarsong::WireOperation operation = AutoWowWarsong::WireOperation::Status;
        if (!AutoWowWarsong::ParseWireRequest(requestText, operation, request.memberGuids, error))
            return false;

        request.type = operation == AutoWowWarsong::WireOperation::Queue ? AutoWowRequestType::WsgQueue :
                       operation == AutoWowWarsong::WireOperation::Status ? AutoWowRequestType::WsgStatus :
                                                                           AutoWowRequestType::WsgLeave;
        return true;
    }

    if (command == "raid" || command == "raid-create" || command == "raid-status" || command == "raid-leave")
    {
        AutoWowRaid::WireRequest raidRequest;
        if (!AutoWowRaid::ParseWireRequest(requestText, raidRequest, error))
            return false;

        request.botGuid = raidRequest.subjectGuid;
        request.memberGuids = std::move(raidRequest.memberGuids);
        request.raidDifficulty = raidRequest.difficulty;
        request.type = raidRequest.operation == AutoWowRaid::WireOperation::Create ? AutoWowRequestType::RaidCreate :
                       raidRequest.operation == AutoWowRaid::WireOperation::Status ? AutoWowRequestType::RaidStatus :
                                                                                     AutoWowRequestType::RaidLeave;
        return true;
    }

    if (command == "probe-reset")
    {
        std::string route;
        std::string mapToken;
        std::string difficultyToken;
        if (!(input >> route >> mapToken >> difficultyToken) ||
            !ParseUnsignedToken(mapToken, request.expectedMapId) || !request.expectedMapId ||
            !ParseUnsignedToken(difficultyToken, request.raidDifficulty))
        {
            error = "probe-reset requires <exterior-route> <target-map> <difficulty> <guid...>";
            return false;
        }

        request.destination = Lowercase(route);
        std::string guidToken;
        while (input >> guidToken)
        {
            uint32 guid = 0;
            if (!ParseBotGuid(guidToken, guid))
            {
                error = "probe-reset roster GUIDs must be positive numeric";
                return false;
            }
            request.memberGuids.push_back(guid);
        }
        if (request.memberGuids.empty())
        {
            error = "probe-reset requires at least one roster GUID";
            return false;
        }

        request.type = AutoWowRequestType::ProbeReset;
        return true;
    }

    // Fixture-only quest-probe SETUP verbs (AutoWow.Probe.Enable, GUID gate in ProbePlaceControl).
    if (command == "probe-login" || command == "probe-setlevel" || command == "probe-place" ||
        command == "probe-status")
    {
        if (!AutoWowProbePlace::ParseWireRequest(requestText, request.probe, error))
            return false;
        request.botGuid = request.probe.guid;
        request.type = AutoWowRequestType::ProbeFixture;
        return true;
    }

    if (command == "fixture")
    {
        std::string subcommand;
        input >> subcommand;
        subcommand = Lowercase(subcommand);

        if (subcommand == "status")
        {
            std::string guidToken;
            std::string extra;
            if (!(input >> guidToken) || !ParseBotGuid(guidToken, request.botGuid))
            {
                error = "fixture status requires a positive numeric GUID";
                return false;
            }
            if (input >> extra)
            {
                error = "fixture status accepts exactly one GUID";
                return false;
            }

            request.type = AutoWowRequestType::FixtureStatus;
            return true;
        }

        if (subcommand == "init")
        {
            std::string guidToken;
            std::string levelToken;
            std::string specToken;
            std::string qualityToken;
            std::string extra;
            if (!(input >> guidToken >> levelToken >> specToken >> qualityToken))
            {
                error = "fixture init requires <guid> <level> <spec-index> <quality>";
                return false;
            }

            if (!ParseBotGuid(guidToken, request.botGuid))
            {
                error = "fixture init GUID must be positive numeric";
                return false;
            }
            if (!ParseUnsignedToken(levelToken, request.fixtureLevel) || !request.fixtureLevel)
            {
                error = "fixture init level must be a positive number";
                return false;
            }
            if (!ParseUnsignedToken(specToken, request.fixtureSpecIndex) ||
                !AutoWowFixture::IsValidSpecIndex(request.fixtureSpecIndex))
            {
                error = "fixture init spec-index is outside the supported range 0..19";
                return false;
            }
            if (!ParseUnsignedToken(qualityToken, request.fixtureQuality) ||
                !AutoWowFixture::IsValidQuality(request.fixtureQuality))
            {
                error = "fixture init quality is outside the supported enum range 0..5";
                return false;
            }
            if (input >> extra)
            {
                error = "fixture init accepts exactly four arguments after init";
                return false;
            }

            request.type = AutoWowRequestType::FixtureInit;
            return true;
        }

        if (subcommand == "accelerate" || subcommand == "accelerate-off")
        {
            std::string guidToken;
            std::string pacingToken;
            std::string extra;
            if (!(input >> guidToken) || !ParseBotGuid(guidToken, request.botGuid))
            {
                error = "fixture accelerate requires a positive numeric GUID";
                return false;
            }

            if (subcommand == "accelerate")
            {
                if (!(input >> pacingToken) ||
                    !ParseUnsignedToken(pacingToken, request.fixturePacingPercent) ||
                    !AutoWowFixtureAcceleration::IsValidPacingPercent(request.fixturePacingPercent))
                {
                    error = "fixture accelerate requires pacing percent 1000";
                    return false;
                }
            }
            if (input >> extra)
            {
                error = subcommand == "accelerate"
                    ? "fixture accelerate accepts exactly GUID and pacing percent"
                    : "fixture accelerate-off accepts exactly one GUID";
                return false;
            }

            request.type = subcommand == "accelerate"
                ? AutoWowRequestType::FixtureAccelerate
                : AutoWowRequestType::FixtureAccelerateOff;
            return true;
        }

        if (subcommand == "kill")
        {
            std::string guidToken;
            std::string selector;
            std::string entryToken;
            std::string spawnSelector;
            std::string spawnToken;
            std::string extra;
            if (!(input >> guidToken >> selector >> entryToken >> spawnSelector >> spawnToken) ||
                !ParseBotGuid(guidToken, request.botGuid) || Lowercase(selector) != "entry" ||
                !ParseUnsignedToken(entryToken, request.fixtureKillEntry) ||
                Lowercase(spawnSelector) != "spawn" ||
                !ParseUnsigned64Token(spawnToken, request.fixtureKillSpawnId) ||
                !AutoWowFixtureAcceleration::IsValidExactTarget(
                    request.fixtureKillEntry, request.fixtureKillSpawnId) ||
                (input >> extra))
            {
                error = "fixture kill requires <guid> entry <creature-entry> spawn <stable-spawn-id>";
                return false;
            }

            request.type = AutoWowRequestType::FixtureKill;
            return true;
        }

        error = "fixture requires a subcommand (init|status|accelerate|accelerate-off|kill)";
        return false;
    }

    if (command == "observe")
    {
        request.type = AutoWowRequestType::Observe;
        std::string sub;
        input >> sub;
        request.destination = Lowercase(sub);
        if (request.destination.empty())
        {
            error = "observe requires a subcommand (watch|relocate|status|release|protect)";
            return false;
        }

        std::string observerGuid;
        input >> observerGuid;
        if (!ParseBotGuid(observerGuid, request.botGuid))
        {
            error = "observe requires a positive numeric observer GUID";
            return false;
        }

        std::string leaderGuid;
        if (input >> leaderGuid)
        {
            uint32 parsedLeader = 0;
            if (!ParseBotGuid(leaderGuid, parsedLeader))
            {
                error = "observe leader GUID must be positive numeric";
                return false;
            }
            request.memberGuids.push_back(parsedLeader);
        }

        return true;
    }

    if (command == "gather-source" || command == "oracle-gather")
    {
        std::string botGuid;
        std::string mapToken;
        std::string instanceToken;
        std::string spawnToken;
        std::string entryToken;
        std::string materialToken;
        std::string goalToken;
        std::string extra;
        if (!(input >> botGuid >> mapToken >> instanceToken >> spawnToken >> entryToken >> materialToken >> goalToken) ||
            (input >> extra) || !ParseBotGuid(botGuid, request.botGuid) ||
            !ParseUnsignedToken(mapToken, request.gatherMapId) ||
            !ParseUnsignedToken(instanceToken, request.gatherInstanceId) ||
            !ParseUnsigned64Token(spawnToken, request.gatherSpawnId) ||
            !ParseUnsignedToken(entryToken, request.gatherEntry) ||
            !ParseUnsignedToken(materialToken, request.gatherMaterialItemId) ||
            !ParseGatherGoal(goalToken, request.gatherGoal) ||
            !request.gatherMapId || !request.gatherEntry || !request.gatherMaterialItemId)
        {
            error = "gather-source requires <bot-guid> <map-id> <instance-id> <spawn-id> <entry> <material-item-id> <harvest-node|obtain-material>";
            return false;
        }
        request.type = AutoWowRequestType::GatherSourcePublish;
        return true;
    }

    if (command == "quest")
    {
        AutoWowQuestAcquisition::WireRequest questRequest;
        if (!AutoWowQuestAcquisition::ParseWireRequest(requestText, questRequest, error))
            return false;

        request.botGuid = questRequest.leaderGuid;
        if (questRequest.mode == AutoWowQuestAcquisition::WireMode::Acquire)
            request.type = AutoWowRequestType::QuestAcquire;
        else
        {
            request.type = AutoWowRequestType::Quest;
            if (questRequest.mode == AutoWowQuestAcquisition::WireMode::ExplicitQuest)
                request.destination = std::to_string(questRequest.questId);
        }
        return true;
    }

    if (command == "boss" || command == "boss-approach")
    {
        std::string leaderGuid;
        if (!(input >> leaderGuid))
        {
            error = "boss requires a leader GUID or status <leader GUID>";
            return false;
        }

        if (Lowercase(leaderGuid) == "status")
        {
            request.bossStatusOnly = true;
            if (!(input >> leaderGuid))
            {
                error = "boss status requires a leader GUID";
                return false;
            }
        }

        if (!ParseBotGuid(leaderGuid, request.botGuid))
        {
            error = "boss requires a positive numeric leader GUID";
            return false;
        }

        std::string extra;
        if (input >> extra)
        {
            error = "boss accepts a leader GUID or status <leader GUID>";
            return false;
        }
        request.type = AutoWowRequestType::BossApproach;
        return true;
    }

    std::string botGuid;
    input >> botGuid;
    if (!ParseBotGuid(botGuid, request.botGuid))
    {
        error = "expected a positive numeric bot GUID";
        return false;
    }

    if (command == "activate")
        request.type = AutoWowRequestType::Activate;
    else if (command == "deactivate")
        request.type = AutoWowRequestType::Deactivate;
    else if (command == "independent")
        request.type = AutoWowRequestType::Independent;
    else if (command == "party")
    {
        request.type = AutoWowRequestType::Party;
        std::string memberGuid;
        while (input >> memberGuid)
        {
            uint32 parsedGuid = 0;
            if (!ParseBotGuid(memberGuid, parsedGuid))
            {
                error = "party members must be positive numeric bot GUIDs";
                return false;
            }
            request.memberGuids.push_back(parsedGuid);
        }
        if (request.memberGuids.empty())
        {
            error = "party requires at least one member GUID";
            return false;
        }
    }
    else if (command == "rally")
        request.type = AutoWowRequestType::Rally;
    else if (command == "deploy")
        request.type = AutoWowRequestType::Deploy;
    else if (command == "route")
    {
        request.type = AutoWowRequestType::Route;
        std::getline(input, request.destination);
        request.destination = Trim(std::move(request.destination));
        if (request.destination.empty())
        {
            error = "route requires a route name";
            return false;
        }
    }
    else if (command == "advance")
    {
        request.type = AutoWowRequestType::Advance;
        std::getline(input, request.destination);
        request.destination = Trim(std::move(request.destination));
        if (request.destination.empty())
        {
            error = "advance requires a named dungeon waypoint";
            return false;
        }
    }
    else if (command == "advancepoint")
    {
        std::string xToken;
        std::string yToken;
        std::string zToken;
        std::string orientationToken;
        std::string extra;
        if (!(input >> xToken >> yToken >> zToken) ||
            !ParseFloatToken(xToken, request.coordinateX) ||
            !ParseFloatToken(yToken, request.coordinateY) ||
            !ParseFloatToken(zToken, request.coordinateZ))
        {
            error = "advancepoint requires finite numeric x y z coordinates and optional orientation";
            return false;
        }
        if (input >> orientationToken)
        {
            if (!ParseFloatToken(orientationToken, request.coordinateOrientation) || (input >> extra))
            {
                error = "advancepoint orientation must be one finite numeric value";
                return false;
            }
            request.hasCoordinateOrientation = true;
        }
        request.type = AutoWowRequestType::AdvancePoint;
    }
    else if (command == "engage")
    {
        AutoWowExactBossTarget::WireRequest engageRequest;
        if (!AutoWowExactBossTarget::ParseWireRequest(requestText, engageRequest, error))
            return false;

        request.type = AutoWowRequestType::Engage;
        request.botGuid = engageRequest.leaderGuid;
        if (engageRequest.mode == AutoWowExactBossTarget::EngageMode::CreatureEntry)
            request.exactCreatureEntry = engageRequest.creatureEntry;
        else if (engageRequest.mode == AutoWowExactBossTarget::EngageMode::PlayerGuid)
            request.exactPlayerGuid = engageRequest.playerGuid;
    }
    else if (command == "scout")
        request.type = AutoWowRequestType::Scout;
    else if (command == "pathprobe")
    {
        std::string xToken;
        std::string yToken;
        std::string zToken;
        std::string sourceXToken;
        std::string sourceYToken;
        std::string sourceZToken;
        if (!(input >> xToken >> yToken >> zToken) ||
            !ParseFloatToken(xToken, request.coordinateX) ||
            !ParseFloatToken(yToken, request.coordinateY) ||
            !ParseFloatToken(zToken, request.coordinateZ))
        {
            error = "pathprobe requires finite numeric x y z coordinates";
            return false;
        }
        if (input >> sourceXToken)
        {
            if (!(input >> sourceYToken >> sourceZToken) ||
                !ParseFloatToken(sourceXToken, request.sourceX) ||
                !ParseFloatToken(sourceYToken, request.sourceY) ||
                !ParseFloatToken(sourceZToken, request.sourceZ))
            {
                error = "pathprobe virtual source requires finite numeric source-x source-y source-z";
                return false;
            }
            std::string extra;
            if (input >> extra)
            {
                error = "pathprobe accepts guid dest-x dest-y dest-z and optional source-x source-y source-z";
                return false;
            }
            request.hasSourceCoordinates = true;
        }
        request.type = AutoWowRequestType::PathProbe;
    }
    else if (command == "recover")
        request.type = AutoWowRequestType::Recover;
    else if (command == "destinations")
        request.type = AutoWowRequestType::Destinations;
    else if (command == "snapshot")
        request.type = AutoWowRequestType::Snapshot;
    else if (command == "combatlog")
        request.type = AutoWowRequestType::CombatLog;
    else if (command == "encounterlog")
        request.type = AutoWowRequestType::EncounterLog;
    else if (command == "professioneconomy")
        request.type = AutoWowRequestType::ProfessionEconomy;
    else if (command == "craft")
    {
        std::string spellToken;
        std::string extra;
        if (!(input >> spellToken) || !ParseUnsignedToken(spellToken, request.craftRecipeSpellId) ||
            !request.craftRecipeSpellId || (input >> extra))
        {
            error = "craft requires exactly bot-guid and a positive recipe spell id";
            return false;
        }
        request.type = AutoWowRequestType::Craft;
    }
    else if (command == "craft-status")
    {
        std::string extra;
        if (input >> extra)
        {
            error = "craft-status accepts only bot-guid";
            return false;
        }
        request.type = AutoWowRequestType::CraftStatus;
    }
    else if (command == "guild-trade")
    {
        std::string buyer, itemGuid, itemEntry, quantity, price, extra;
        if (!(input >> buyer >> itemGuid >> itemEntry >> quantity >> price) ||
            !ParseUnsignedToken(buyer, request.tradeBuyerGuid) || !request.tradeBuyerGuid ||
            !ParseUnsignedToken(itemGuid, request.tradeItemGuid) || !request.tradeItemGuid ||
            !ParseUnsignedToken(itemEntry, request.tradeItemEntry) || !request.tradeItemEntry ||
            !ParseUnsignedToken(quantity, request.tradeQuantity) || !request.tradeQuantity ||
            !ParseUnsignedToken(price, request.tradePriceCopper) || !request.tradePriceCopper ||
            (input >> extra))
        {
            error = "guild-trade requires seller-guid buyer-guid item-guid item-entry quantity price-copper";
            return false;
        }
        request.type = AutoWowRequestType::GuildTrade;
    }
    else if (command == "guild-trade-status")
    {
        std::string extra;
        if (input >> extra)
        {
            error = "guild-trade-status accepts only seller-guid";
            return false;
        }
        request.type = AutoWowRequestType::GuildTradeStatus;
    }
    else if (command == "questlog")
        request.type = AutoWowRequestType::QuestLog;
    else if (command == "questobjective")
        request.type = AutoWowRequestType::QuestObjective;
    else if (command == "acceptance")
    {
        request.type = AutoWowRequestType::Acceptance;
        // Optional explicit quest id: `acceptance <bot-guid> [quest-id]`. This is read-only and is
        // used only to query a reward postcondition after the active objective has moved on.
        std::string questIdToken;
        if (input >> questIdToken)
        {
            uint32 parsedQuest = 0;
            if (!ParseBotGuid(questIdToken, parsedQuest))
            {
                error = "acceptance quest id must be a positive number";
                return false;
            }
            request.destination = std::to_string(parsedQuest);
        }
    }
    else if (command == "pause")
        request.type = AutoWowRequestType::Pause;
    else if (command == "resume")
        request.type = AutoWowRequestType::Resume;
    else if (command == "travel")
    {
        request.type = AutoWowRequestType::Travel;
        std::getline(input, request.destination);
        request.destination = Trim(std::move(request.destination));
        if (request.destination.empty())
        {
            error = "travel requires a destination name";
            return false;
        }
    }
    else
    {
        error = "unknown command";
        return false;
    }

    return true;
}

std::string Dispatch(std::string const& requestText)
{
    AutoWowRequest request;
    std::string error;
    if (!ParseRequest(requestText, request, error))
        return ErrorResponse(error);

    std::shared_ptr<AutoWowCompletion> completion = std::make_shared<AutoWowCompletion>();
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<AutoWowBridgeOperation>(std::move(request), completion)))
        return ErrorResponse("world_thread_queue_full");

    std::string response;
    if (!completion->WaitFor(response, AUTO_WOW_RESPONSE_TIMEOUT_MS))
    {
        completion->Cancel();
        return ErrorResponse("world_thread_timeout");
    }

    return response;
}

void ServeClient(tcp::socket socket)
{
    try
    {
        boost::asio::streambuf buffer;
        boost::asio::read_until(socket, buffer, '\n');

        std::istream input(&buffer);
        std::string request;
        std::getline(input, request);
        if (!request.empty() && request.back() == '\r')
            request.pop_back();

        if (request.size() > 1024)
        {
            std::string response = ErrorResponse("request_too_large") + "\n";
            boost::asio::write(socket, boost::asio::buffer(response));
            return;
        }

        std::string response = Dispatch(request) + "\n";
        boost::asio::write(socket, boost::asio::buffer(response));
    }
    catch (std::exception const& exception)
    {
        LOG_ERROR("playerbots", "AutoWow bridge client error: {}", exception.what());
    }
}

void EnsureQuestAcquisitionOwnedStrategies(
    QuestAcquisitionSession const& session, Player* leader, PlayerbotAI* leaderAI,
    bool campaignTravelOwned, bool inFlight, bool canMove,
    bool travelActivityAllowed, bool detailedMoveAllowed)
{
    if (!leader || !leaderAI)
        return;

    using AutoWowQuestGiverTravel::EvaluateQuestAcquisitionStrategy;
    using AutoWowQuestGiverTravel::QuestAcquisitionStrategyDecision;
    using AutoWowQuestGiverTravel::QuestAcquisitionStrategyFacts;
    using AutoWowQuestGiverTravel::QuestAcquisitionStrategyRole;

    bool const leaderNonCombatGatesClear = canMove && travelActivityAllowed && detailedMoveAllowed;
    QuestAcquisitionStrategyFacts const leaderFacts{
        session.state == QuestAcquisitionState::Pending,
        leader->IsAlive(),
        leader->IsInCombat(),
        inFlight,
        leader->IsBeingTeleported(),
        campaignTravelOwned,
        leaderAI->IsAutoWowPaused(),
        leaderNonCombatGatesClear,
        QuestAcquisitionStrategyRole::Leader,
        leaderAI->HasStrategy("travel", BOT_STATE_NON_COMBAT),
        leaderAI->HasStrategy("follow", BOT_STATE_NON_COMBAT),
        leaderAI->HasStrategy("grind", BOT_STATE_NON_COMBAT),
        leaderAI->HasStrategy("move random", BOT_STATE_NON_COMBAT),
        leaderAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)};
    if (EvaluateQuestAcquisitionStrategy(leaderFacts) == QuestAcquisitionStrategyDecision::Restore)
        leaderAI->ChangeStrategy(
            "+travel,-follow,-grind,-move random,-new rpg", BOT_STATE_NON_COMBAT);

    Group* group = leader->GetGroup();
    if (!group)
        return;

    for (GroupReference* reference = group->GetFirstMember(); reference;
         reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || member == leader || !member->IsAlive())
            continue;

        PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
        if (!memberAI)
            continue;

        bool const memberInFlight = member->IsInFlight() ||
            member->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE;
        bool const memberGatesClear = memberAI->CanMove() &&
            memberAI->AllowActivity(TRAVEL_ACTIVITY, true) &&
            memberAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
        QuestAcquisitionStrategyFacts const followerFacts{
            session.state == QuestAcquisitionState::Pending,
            member->IsAlive(),
            member->IsInCombat(),
            memberInFlight,
            member->IsBeingTeleported(),
            campaignTravelOwned,
            memberAI->IsAutoWowPaused(),
            memberGatesClear,
            QuestAcquisitionStrategyRole::Follower,
            memberAI->HasStrategy("travel", BOT_STATE_NON_COMBAT),
            memberAI->HasStrategy("follow", BOT_STATE_NON_COMBAT),
            memberAI->HasStrategy("grind", BOT_STATE_NON_COMBAT),
            memberAI->HasStrategy("move random", BOT_STATE_NON_COMBAT),
            memberAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)};
        if (EvaluateQuestAcquisitionStrategy(followerFacts) == QuestAcquisitionStrategyDecision::Restore)
            memberAI->ChangeStrategy(
                "+follow,-grind,-move random,-new rpg,-travel", BOT_STATE_NON_COMBAT);
    }
}
}  // namespace

AutoWowBridge::QuestAcquisitionTickResult AutoWowBridge::TickQuestAcquisition(
    PlayerbotAI* botAI, uint32 elapsedMs)
{
    using namespace AutoWowQuestAcquisitionBridge;

    if (!botAI)
        return TickResult::NoWork;

    Player* leader = botAI->GetBot();
    if (!leader)
        return TickResult::NoWork;

    uint32 const leaderGuid = leader->GetGUID().GetCounter();
    auto const sessionIt = questAcquisitionSessions.find(leaderGuid);
    if (sessionIt == questAcquisitionSessions.end() || !sessionIt->second ||
        sessionIt->second->leaderGuid != leaderGuid)
        return TickResult::NoWork;

    QuestAcquisitionSession& session = *sessionIt->second;
    AiObjectContext* context = botAI->GetAiObjectContext();
    TravelTarget* travelTarget = context
        ? context->GetValue<TravelTarget*>("travel target")->Get()
        : nullptr;

    auto releaseTerminal = [&session, leader, travelTarget]()
    {
        return session.ReleaseTerminalTravelTarget(leader, travelTarget)
            ? TickResult::Terminal
            : TickResult::NoWork;
    };

    if (session.state != QuestAcquisitionState::Pending)
        return releaseTerminal();

    // Campaign Travel owns the map-thread movement lane whenever it has a live non-combat
    // journey. Do not reconcile or replace its target from this lower-priority owner.
    bool const campaignTravelOwned = botAI->GetState() == BOT_STATE_NON_COMBAT && leader->IsAlive() &&
        !leader->IsInCombat() && botAI->HasCampaignTravelWork();
    if (campaignTravelOwned || botAI->IsAutoWowPaused())
        return TickResult::NoWork;

    // Combat, death, flight, and teleport recovery remain owned by their native engines. The
    // acquisition session stays pending and will resume once these gates clear.
    bool const inFlight = leader->IsInFlight() ||
        leader->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE;
    if (!leader->IsAlive() || leader->IsInCombat() || inFlight || leader->IsBeingTeleported())
        return TickResult::NoWork;

    bool const canMove = botAI->CanMove();
    bool const travelActivityAllowed = botAI->AllowActivity(TRAVEL_ACTIVITY, true);
    bool const detailedMoveAllowed = botAI->AllowActivity(DETAILED_MOVE_ACTIVITY, true);
    if (!canMove || !travelActivityAllowed || !detailedMoveAllowed)
        return TickResult::NoWork;

    session.AdvanceNativeMovementTick(elapsedMs);
    session.ApplyLiveMovementFeedback(leader);
    if (session.state != QuestAcquisitionState::Pending)
        return releaseTerminal();

    // The terminal escape result is recorded by the staged mover before the native tick sees it.
    // Refresh the observational path evidence first, then give the pure classifier exactly one
    // world-thread opportunity to invoke the existing hearthstone action.
    ObserveQuestAcquisitionHearthEvidence(session, leader, botAI, travelTarget);
    bool const targetInstalledBeforeRecovery = travelTarget &&
        travelTarget->getDestination() == &session.destination;
    bool const targetActiveBeforeRecovery = targetInstalledBeforeRecovery &&
        (travelTarget->getStatus() == TRAVEL_STATUS_TRAVEL ||
         travelTarget->getStatus() == TRAVEL_STATUS_WORK);
    if (session.TryHearthstoneRecovery(
            leader, botAI, campaignTravelOwned, inFlight, !targetActiveBeforeRecovery))
        return releaseTerminal();

    session.ReconcileTravelTarget(travelTarget);
    if (session.state != QuestAcquisitionState::Pending)
        return releaseTerminal();

    // This is an observational lifecycle poll. It intentionally passes false so the native tick
    // never emits quest opcodes; once the party is cohesive at the giver, ordinary travel/work
    // processing gets one tick to invoke the destination with acceptance enabled.
    session.Poll(leader, false);
    if (session.state != QuestAcquisitionState::Pending)
        return releaseTerminal();

    EnsureQuestAcquisitionOwnedStrategies(
        session, leader, botAI, campaignTravelOwned, inFlight, canMove,
        travelActivityAllowed, detailedMoveAllowed);

    bool const targetInstalled = travelTarget &&
        travelTarget->getDestination() == &session.destination;
    bool const targetActive = targetInstalled &&
        (travelTarget->getStatus() == TRAVEL_STATUS_TRAVEL ||
         travelTarget->getStatus() == TRAVEL_STATUS_WORK);
    // Accepted feedback is historical telemetry and can remain `moving=true` after its segment
    // stops. Only current live motion suppresses a cadence-due dispatch; the staged mover's own
    // reservation/deferred guard still prevents overlapping movement requests.
    bool const movementInFlight = leader->isMoving();

    TickFacts const facts{
        true,
        false,
        false,
        false,
        leader->IsAlive(),
        leader->IsInCombat(),
        inFlight,
        leader->IsBeingTeleported(),
        canMove,
        travelActivityAllowed,
        detailedMoveAllowed,
        campaignTravelOwned,
        targetInstalled,
        targetActive,
        session.IsReadyForNativeWorkHandoff(),
        session.NativeMovementTickDue(),
        movementInFlight};
    TickResult const decision = DecideTick(facts);
    if (decision != TickResult::Owned)
        return decision;

    if (!ShouldDispatchStagedMover(facts))
        return TickResult::Owned;

    MoveToTravelTargetAction stagedMover(botAI);
    stagedMover.ExecuteQuestGiverStagedEntry(travelTarget);
    // Arm even for a rejected/deferred attempt. The next ordinary AI tick may observe feedback,
    // but it cannot issue another staged move until the fixed cadence has elapsed.
    session.ArmNativeMovementTick();
    session.ApplyLiveMovementFeedback(leader);
    if (session.state != QuestAcquisitionState::Pending)
        return releaseTerminal();

    return TickResult::Owned;
}

AutoWowQuestAcquisitionBridgeTest::LifecycleScenarioResult
AutoWowQuestAcquisitionBridgeTest::RunNativeExpiryScenario()
{
    constexpr uint32 testLeaderGuid = 0xFFF00001U;
    WorldPosition giverPoint(1, 100.0f, 200.0f, 30.0f, 0.0f);
    GuidPosition giverSpawn;
    QuestAcquisitionSession session(testLeaderGuid, 954, 3649, giverPoint, giverSpawn, 56,
                                      3878.58, 7.0f, "talk");
    session.expectedPartyMembers = 2;

    PlayerbotAI ai;
    TravelTarget target(&ai);
    target.setTarget(&session.destination, &session.giverPoint);
    session.AlignInstalledTravelDeadline(&target);

    AutoWowQuestGiverTravel::ClearMovementFeedback(testLeaderGuid);
    for (uint32 segment = 0; segment < 27; ++segment)
    {
        AutoWowQuestGiverTravel::RecordMovementFeedback(
            testLeaderGuid, AutoWowQuestGiverTravel::StepKind::TravelMgrSegment,
            AutoWowQuestGiverTravel::MovementFeedbackResult::Accepted, true,
            3878.58f - static_cast<float>(segment + 1) * 140.0f);
        if (AutoWowQuestGiverTravel::MovementFeedback const* feedback =
                AutoWowQuestGiverTravel::ReadMovementFeedback(testLeaderGuid))
            session.ApplyMovementFeedbackRecord(*feedback);
    }

    bool const active = target.isActiveAtElapsed(target.getStatusDeadline() + 1U);
    session.ReconcileTravelTarget(&target);
    LifecycleScenarioResult result;
    result.targetInactive = !active && target.getStatus() == TRAVEL_STATUS_EXPIRED;
    result.response = QuestAcquisitionResponse(session);
    AutoWowQuestAcquisition::MovementOrderFacts const passiveOrderFacts{
        true, true, false, true, true};
    result.snapshot = QuestAcquisitionSnapshotJsonForSession(
        session, nullptr, nullptr, &target, &passiveOrderFacts);

    QuestAcquisitionSession giverSession(testLeaderGuid + 1U, 827, 3208, giverPoint,
                                         giverSpawn, 56, 386.181, 7.0f, "talk");
    giverSession.PollGiverResolutionForTest(true, false, false, 1000U);
    result.unloadedGiverDidNotStartStaleWait =
        giverSession.state == QuestAcquisitionState::Pending &&
        giverSession.giverResolution == "grid_unloaded" &&
        giverSession.journey.Lifecycle().StaleGiverWaitMs(1000U) == 0U;
    giverSession.PollGiverResolutionForTest(true, true, false, 2000U);
    giverSession.PollGiverResolutionForTest(
        true, true, false, 2000U + AutoWowQuestGiverTravel::kMaxStaleGiverWaitMs);
    result.confirmedMissingGiverReachedBoundedTerminal =
        giverSession.state == QuestAcquisitionState::Blocked &&
        giverSession.reason == "stale_giver";
    AutoWowQuestGiverTravel::ClearMovementFeedback(testLeaderGuid);
    return result;
}

void AutoWowBridge::Start()
{
    int32 configuredPort = sConfigMgr->GetOption<int32>("AutoWow.BridgePort", 18787);
    if (configuredPort == 0)
    {
        LOG_INFO("playerbots", "AutoWow bridge is disabled");
        return;
    }

    if (configuredPort < 1 || configuredPort > std::numeric_limits<uint16>::max())
    {
        LOG_ERROR("playerbots", "AutoWow bridge has invalid port {}", configuredPort);
        return;
    }

    bool expected = false;
    if (!started.compare_exchange_strong(expected, true))
        return;

    std::thread([this, configuredPort] { Run(static_cast<uint16>(configuredPort)); }).detach();
}

void AutoWowBridge::Run(uint16 port)
{
    try
    {
        boost::asio::io_context ioContext;
        tcp::acceptor acceptor(ioContext, tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
        LOG_INFO("playerbots", "AutoWow bridge listening on 127.0.0.1:{}", port);

        for (;;)
        {
            tcp::socket socket(ioContext);
            boost::system::error_code error;
            acceptor.accept(socket, error);
            if (error)
            {
                LOG_ERROR("playerbots", "AutoWow bridge accept failed: {}", error.message());
                continue;
            }

            std::thread(ServeClient, std::move(socket)).detach();
        }
    }
    catch (std::exception const& exception)
    {
        LOG_ERROR("playerbots", "AutoWow bridge could not start: {}", exception.what());
        started = false;
    }
}
