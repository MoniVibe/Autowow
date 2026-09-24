#include "NewRpgAction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "AreaDefines.h"
#include "AttackAction.h"
#include "AutoWowBridge.h"
#include "AutoWowOracleFinisherIntent.h"
#include "AutoWowOracleOwnershipGate.h"
#include "AutoWowOracleRuntime.h"
#include "AutoWowOracleZoneTravelAssistPolicy.h"
#include "AutoWowQuestLedger.h"
#include "Bag.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathLoopBreaker.h"
#include "ZoneProgressionPolicy.h"
#include "DungeonPathWalkAction.h"
#include "DungeonPullReadinessGuard.h"
#include "ExactQuestAttackRecoveryPolicy.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GameObjectLockPolicy.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "OracleQuestDispatchPolicy.h"
#include "OracleQuestExecutor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestFinisherTransitionPolicy.h"
#include "QuestInventoryReliefPolicy.h"
#include "ErrandsPolicy.h"
#include "QuestObjectiveContext.h"
#include "QuestObjectiveTransitionPolicy.h"
#include "QuestSourceStallPolicy.h"
#include "QuestStallRecoveryPolicy.h"
#include "QuestTravelWalk.h"
#include "QuestSourceRotationPolicy.h"
#include "QuestValues.h"
#include "RaidTargetClaimValue.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SurvivalRecovery.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "UseItemAction.h"
#include "WorldPacket.h"

namespace
{
char const* ExactAttackRejectionName(ExactQuestAttackRecoveryPolicy::Rejection reason)
{
    using Rejection = ExactQuestAttackRecoveryPolicy::Rejection;
    switch (reason)
    {
        case Rejection::SourceMismatch: return "source_mismatch";
        case Rejection::TargetUnavailable: return "target_unavailable";
        case Rejection::Flight: return "flight";
        case Rejection::RaidClaim: return "raid_claim";
        case Rejection::Friendly: return "friendly";
        case Rejection::InvalidAttackTarget: return "invalid_attack_target";
        case Rejection::Vehicle: return "vehicle";
        case Rejection::DungeonPull: return "dungeon_pull";
        case Rejection::AlreadyEngaged: return "already_engaged";
        case Rejection::BlockedLineOfSight: return "blocked_line_of_sight";
        case Rejection::OtherAttackGuard: return "other_attack_guard";
    }
    return "unknown";
}

bool ShouldLogExactAttackRejection(uint64 botGuid, uint64 spawnGuid,
                                   ExactQuestAttackRecoveryPolicy::Rejection reason, uint32 nowMs)
{
    struct Slot
    {
        uint64 botGuid = 0;
        uint64 spawnGuid = 0;
        ExactQuestAttackRecoveryPolicy::Rejection reason =
            ExactQuestAttackRecoveryPolicy::Rejection::OtherAttackGuard;
        uint32 lastAtMs = 0;
    };
    static std::array<Slot, 32> slots{}; // World-thread only; bounded, no runtime ABI state.
    Slot& slot = slots[botGuid % slots.size()];
    if (slot.botGuid == botGuid && slot.spawnGuid == spawnGuid && slot.reason == reason &&
        static_cast<uint32>(nowMs - slot.lastAtMs) < 5000)
        return false;
    slot = {botGuid, spawnGuid, reason, nowMs};
    return true;
}

class ExactQuestDefenseAttackAction final : public AttackAction
{
public:
    explicit ExactQuestDefenseAttackAction(PlayerbotAI* botAI)
        : AttackAction(botAI, "autowow scripted-event defense")
    {
    }

    bool AttackExact(Unit* target) { return Attack(target); }
};

class TargetlessQuestItemUseAction final : public UseItemAction
{
public:
    explicit TargetlessQuestItemUseAction(PlayerbotAI* botAI)
        : UseItemAction(botAI, "autowow targetless quest item", true)
    {
    }

    bool UseItemAuto(Item* item) { return UseItemAction::UseItemAuto(item); }
};

// AutoWow.QuestItemTargetConditions.Enable: the same explicit-target spell conditions the core
// checks in Spell::CheckCast (caster, target). A target that fails them can never yield credit
// (q5441: an awake peon lacks aura 17743), so it is not a candidate.
bool QuestItemSpellTargetConditionsMet(Player* bot, uint32 itemId, WorldObject* target)
{
    ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
    if (!proto)
        return true;
    for (auto const& itemSpell : proto->Spells)
    {
        if (itemSpell.SpellId <= 0 || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        ConditionList const conditions = sConditionMgr->GetConditionsForNotGroupedEntry(
            CONDITION_SOURCE_TYPE_SPELL, static_cast<uint32>(itemSpell.SpellId));
        if (!conditions.empty() && !sConditionMgr->IsObjectMeetToConditions(bot, target, conditions))
            return false;
    }
    return true;
}

bool OracleRouteV2Enabled()
{
    return sConfigMgr->GetOption<bool>("AutoWow.OracleRouteV2.Enabled", false);
}

// World-thread-only recovery state. One slot per managed Oracle bot; a pending near teleport is
// completed by an exact live bind on a later tick, never counted as grounded route arrival.
struct ZoneAssistState
{
    uint64 botGuid = 0;
    AutoWowOracleRoute::RouteKey routeKey{};
    uint64 lastAt = 0;
    bool used = false;
    bool pending = false;
};

std::array<ZoneAssistState, AutoWowOracleRuntime::kMaxRuntimeBots> zoneAssistStates{};

ZoneAssistState* FindZoneAssist(uint64 botGuid)
{
    for (ZoneAssistState& state : zoneAssistStates)
        if (state.botGuid == botGuid)
            return &state;
    return nullptr;
}

ZoneAssistState* ReserveZoneAssist(uint64 botGuid)
{
    if (ZoneAssistState* state = FindZoneAssist(botGuid))
        return state;
    for (ZoneAssistState& state : zoneAssistStates)
        if (state.botGuid == 0)
        {
            state.botGuid = botGuid;
            return &state;
        }
    return nullptr;
}

uint64 StableAnchorId(uint32 mapId, uint32 instanceId,
                      AutoWowDungeonPath::ProbeResult const& probe)
{
    if (probe.path.empty())
        return 0;

    G3D::Vector3 const& endpoint = probe.path.back();
    float const endpointX = endpoint.x;
    float const endpointY = endpoint.y;
    float const endpointZ = endpoint.z;
    uint32 x = 0;
    uint32 y = 0;
    uint32 z = 0;
    static_assert(sizeof(x) == sizeof(endpointX));
    std::memcpy(&x, &endpointX, sizeof(x));
    std::memcpy(&y, &endpointY, sizeof(y));
    std::memcpy(&z, &endpointZ, sizeof(z));

    uint64 value = 1469598103934665603ULL;
    auto mix = [&value](uint64 part)
    {
        value ^= part;
        value *= 1099511628211ULL;
    };
    mix(mapId);
    mix(instanceId);
    mix(x);
    mix(y);
    mix(z);
    return value == 0 ? 1 : value;
}

std::vector<AutoWowDungeonPath::ProbeResult> SelectOracleRouteAnchorPaths(
    Player* bot, WorldPosition const& destination)
{
    std::vector<AutoWowDungeonPath::ProbeResult> selected;
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != destination.GetMapId())
        return selected;

    auto addDistinct = [&](AutoWowDungeonPath::ProbeResult const& probe)
    {
        if (!probe.safe || probe.path.size() < 2 ||
            selected.size() >= AutoWowOracleRoute::kMaxSuppliedAnchors)
            return;
        uint64 const id = StableAnchorId(bot->GetMapId(), bot->GetInstanceId(), probe);
        if (id == 0)
            return;
        for (AutoWowDungeonPath::ProbeResult const& existing : selected)
            if (StableAnchorId(bot->GetMapId(), bot->GetInstanceId(), existing) == id)
                return;
        selected.push_back(probe);
    };

    WorldPosition const start(bot);
    TravelPath segmented = TravelNodeMap::getFullPath(start, destination, bot, true);
    if (!segmented.empty())
    {
        std::vector<PathNodePoint> const route = segmented.getPath();
        float const initialDistance = bot->GetDistance2d(
            destination.GetPositionX(), destination.GetPositionY());
        for (std::size_t index = 0;
             index < route.size() &&
             index < AutoWowQuestGiverTravel::kMaxTravelMgrReanchorScanPoints &&
             selected.size() < AutoWowOracleRoute::kMaxSuppliedAnchors; ++index)
        {
            PathNodePoint const& node = route[index];
            if ((node.type != NODE_PREPATH && node.type != NODE_PATH && node.type != NODE_NODE) ||
                node.point.GetMapId() != bot->GetMapId())
                break;
            float const targetReduction = initialDistance - node.point.GetExactDist2d(
                destination.GetPositionX(), destination.GetPositionY());
            if (!std::isfinite(targetReduction) ||
                targetReduction < AutoWowOracleRoute::kProgressDistanceYards)
                continue;
            AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::ProbeFrom(
                bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                node.point.GetPositionX(), node.point.GetPositionY(), node.point.GetPositionZ());
            AutoWowQuestGiverTravel::PathFacts const facts =
                AutoWowQuestGiverTravel::BuildPathFacts(probe);
            if (AutoWowQuestGiverTravel::IsCompleteReprobe(facts) ||
                AutoWowQuestGiverTravel::IsSafeIncompleteTravelMgrProbe(facts))
                addDistinct(probe);
        }
    }

    if (selected.size() < AutoWowOracleRoute::kMaxSuppliedAnchors)
        if (auto direct = AutoWowQuestGiverTravel::SelectCompleteWalkProbe(bot, destination))
            addDistinct(*direct);
    return selected;
}

char const* RouteStatusName(AutoWowOracleRoute::RouteResult const& result)
{
    if (result.arrived)
        return "arrived";
    if (result.blocked)
        return "blocked";
    return "travelling";
}

void EmitOracleRouteReceipt(Player* bot, uint32 questId,
                            AutoWowOracleRoute::RouteResult const& result)
{
    if (!bot)
        return;

    LOG_DEBUG("playerbots",
        "[AutoWow Oracle T1 RouteReceipt] bot_guid={} quest_id={} "
        "route_key={}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{} "
        "segment={} quest_identity={}:{}:{} route_catalog_version={} failure_class={} "
        "status={} stage={} distance_available={} distance_initial={} "
        "distance_remaining={} distance_reduced={} anchor_attempts={} anchors={},{},{} "
        "failure={} next_retry_available={} next_retry={} lease_release={} "
        "lease_release_authoritative={} progress_observed={}",
        bot->GetGUID().GetCounter(), questId, result.routeKey.actor, result.routeKey.purpose,
        result.routeKey.mapId, result.routeKey.instanceId,
        static_cast<uint32>(result.routeKey.targetKind), result.routeKey.entry,
        result.routeKey.stableSpawn, result.routeKey.dbX, result.routeKey.dbY,
        result.routeKey.dbZ, result.routeKey.dbO, result.routeKey.radius,
        result.routeKey.controller, result.routeKey.job, result.routeKey.lease,
        static_cast<uint32>(result.routeKey.segment),
        result.routeKey.questId, static_cast<uint32>(result.routeKey.objectiveFamily),
        static_cast<uint32>(result.routeKey.objectiveSlot), result.routeKey.routeCatalogVersion,
        static_cast<uint32>(result.ledgerKey.failureClass), RouteStatusName(result),
        static_cast<uint32>(result.stage), result.distance.available,
        result.distance.initial, result.distance.remaining, result.distance.reduced,
        static_cast<uint32>(result.anchorAttempts), result.attemptedAnchors[0],
        result.attemptedAnchors[1], result.attemptedAnchors[2],
        static_cast<uint32>(result.failure), result.nextRetryAt != 0, result.nextRetryAt,
        result.releaseLease, result.leaseReleaseAuthoritative, result.progressObserved);
}
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;
    std::string out = botAI->rpgInfo.ToString();
    bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
    return true;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(
            questId, quest, AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter()));
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();

    // AutoWow.Survival.SafeRevive: after a corpse revive the bot first walks off the kill spot, then the RPG
    // holds in REST below the rest-gate hp/mana thresholds while the food strategy eats (SurvivalRecovery.h).
    if (AutoWowSafeRevive::Enabled() && !AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter()))
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        switch (AutoWowSafeRevive::RecoveryStep(botAI, x, y, z))
        {
            case AutoWowSafeRevive::Step::Walk:
                if (bot->isMoving() || IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL) ||
                    MoveTo(bot->GetMapId(), x, y, z, false, false, false, true))
                    return true;
                AutoWowSafeRevive::EndRetreat(bot->GetGUID().GetCounter());
                break;
            case AutoWowSafeRevive::Step::Rest:
                if (status != RPG_REST)
                {
                    info.ChangeToRest();
                    return true;
                }
                return false;  // stay in REST (no RPG movement); lower actions (food) run
            case AutoWowSafeRevive::Step::None:
                break;
        }
    }

    // AutoWow.DeathLoop: an escalated death in a zone whose level bracket starts well above the bot
    // leaves one relocation attempt, taken through the ordinary flight-travel status (walk to the
    // nearest flight master, fly to a zone bracketing the bot's level). No teleport.
    if (AutoWowDeathLoop::Enabled() && status != RPG_TRAVEL_FLIGHT && bot->IsAlive() &&
        AutoWowDeathLoop::TakeRelocation(bot->GetGUID().GetCounter()))
    {
        // AutoWow.DeathLoop.EscapeViaZoneProgression (default 0): a zone-progression trip to the nearest
        // level hub instead (soak-s13-full-r1: the flight relocation never got a L13 out of Duskwood).
        if (AutoWowDeathLoop::EscapeEnabled() && AutoWowZoneProgression::Enabled() && DeathLoopEscape())
            return true;
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        bool const found = SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        LOG_INFO("playerbots", "[DeathLoop] bot={} relocate lvl={} zone={} flight={} to_node={}", bot->GetName(),
                 bot->GetLevel(), bot->GetZoneId(), found, found ? path.back() : 0);
        if (found)
        {
            info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
            return true;
        }
    }

    // AutoWow.ZoneProgression: independent bots graduate to the next zone by normal travel (no teleport).
    if (AutoWowZoneProgression::Enabled() && ZoneProgressionStep())
        return true;

    // AutoWow.Errands: independent bots keep themselves supplied by town runs (no cheats, real gold).
    if (AutoWowErrands::Enabled() && ErrandsStep())
        return true;

    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // AutoWow.QuestScheduler: a directive that went a whole slice without progress rotates.
            if (sPlayerbotAIConfig.autoWowQuestScheduler && RotateStaleDoQuest())
                return true;
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        bool stuck = false;
        if (MoveFarTo(data->pos, /*questNoTeleport*/ false, &stuck))
            return true;
        if (stuck && IsAutoWowTravelBot())
        {
            MarkTravelDestinationFailed(data->pos);
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            AI_VALUE(LastMovement&, "last movement").clear();
            botAI->rpgInfo.SetMoveFarTo(WorldPosition());
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        if (IsAutoWowTravelBot())
            return false;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        bool stuck = false;
        if (MoveFarTo(data->pos, /*questNoTeleport*/ false, &stuck))
            return true;
        if (stuck && IsAutoWowTravelBot())
        {
            MarkTravelDestinationFailed(data->pos);
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            AI_VALUE(LastMovement&, "last movement").clear();
            botAI->rpgInfo.SetMoveFarTo(WorldPosition());
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        if (IsAutoWowTravelBot())
            return false;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

// AutoWow.Professions.TrainOnArrival (needs AutoWow.Professions.Enable; default off): a planned
// (cohort) bot that wandered to a friendly trainer learns there through the real-gold `trainer`
// action, whose learn filter keeps it to its assigned professions.
static void AutoWowTrainOnArrival(PlayerbotAI* botAI, Player* bot, WorldObject* object)
{
    if (!sPlayerbotAIConfig.autoWowProfessionsTrainOnArrival ||
        !sPlayerbotAIConfig.GetAutoWowProfessionPlan(bot->GetGUID().GetCounter()))
        return;
    Creature* trainer = object->ToCreature();
    if (!trainer || !trainer->IsTrainer() || trainer->IsHostileTo(bot))
        return;
    bot->SetSelection(trainer->GetGUID());
    botAI->DoSpecificAction("trainer", Event("autowow train", "learn"), true);
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            AutoWowTrainOnArrival(botAI, bot, object);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event event)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;

    // Oracle management starts at ChangeToDoQuest, before the first lease exists. Parse the event
    // without executing anything, then let the pure authority policy decide whether this is an
    // ordinary action, the runtime's exact tagged dispatch, or a fail-closed rejection.
    AutoWowOracle::Guid const botGuid = static_cast<AutoWowOracle::Guid>(bot->GetGUID().GetCounter());
    AutoWowOracle::Tick const oracleTick = AutoWowOracleRuntime::CurrentTick();
    std::string const source = event.GetSource();
    std::string const parameter = event.getParam();
    bool const tagged = std::string_view(source) == AutoWowOracleRuntime::TaggedEventSource();

    AutoWowOracleFinisher::Intent finisherIntent;
    bool const finisherParsed = tagged && AutoWowOracleFinisher::Parse(parameter, finisherIntent);
    AutoWowOracle::DecisionId decisionId = 0;
    bool const objectiveParsed = tagged && !finisherParsed &&
        AutoWowOracleRuntime::ParseDecisionId(parameter, decisionId);
    AutoWowOracle::DecisionId const eventDecisionId = finisherParsed ? finisherIntent.decisionId : decisionId;

    QuestObjectiveRuntime& runtime = dataPtr->objectiveRuntime;
    bool const oracleManaged = runtime.oracleManaged || AutoWowOracleRuntime::IsManagedBot(botGuid);
    bool const oracleLeaseRequired = AutoWowOracleRuntime::RequiresTaggedDispatch(botGuid);
    bool const oracleLeaseActive = AutoWowOracleRuntime::HasActiveLease(botGuid);
    bool const oracleGateOwned = AutoWowOracleRuntime::IsOwned(botGuid, oracleTick);
    AutoWowOracle::DecisionId const activeDecisionId = AutoWowOracleRuntime::ActiveDecisionId(botGuid);

    AutoWowOracleQuestDispatchPolicy::OwnershipState const ownership{
        oracleManaged, oracleLeaseRequired, oracleLeaseActive, oracleGateOwned, activeDecisionId};
    AutoWowOracleQuestDispatchPolicy::EventState const dispatchEvent{
        tagged,
        finisherParsed || objectiveParsed,
        eventDecisionId,
        finisherParsed ? AutoWowOracleQuestDispatchPolicy::WorkKind::Finisher
                       : AutoWowOracleQuestDispatchPolicy::WorkKind::Objective,
        finisherParsed ? finisherIntent.signedEntry : 0};
    AutoWowOracleQuestDispatchPolicy::Result const authority =
        AutoWowOracleQuestDispatchPolicy::Evaluate(ownership, dispatchEvent);
    if (authority == AutoWowOracleQuestDispatchPolicy::Result::Reject)
    {
        // The ordinary Playerbot strategy may call this action again between Oracle cadence
        // ticks with an untagged event. That is expected while the runtime still owns the
        // finisher; it must wait for the next tagged dispatch rather than turning a harmless
        // recurring action into an ``oracle_finisher_lease_expired`` blocker. Only report lease
        // loss when the live arbiter/gate is actually gone, or when a tagged finisher carries a
        // different decision id.
        bool const taggedDecisionLost = finisherParsed &&
            (!oracleLeaseActive || !oracleGateOwned || activeDecisionId == 0 ||
             finisherIntent.decisionId != activeDecisionId);
        bool const leaseLost = !oracleLeaseActive || !oracleGateOwned || activeDecisionId == 0;
        if (runtime.oracleFinisherAuthorized && oracleLeaseRequired &&
            (leaseLost || taggedDecisionLost))
            return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherLeaseExpired, /*unsupported*/ false);
        return false;
    }

    if (authority == AutoWowOracleQuestDispatchPolicy::Result::OracleTagged)
    {
        if (finisherParsed)
        {
            if (!AutoWowOracleRuntime::HasActiveLease(botGuid, finisherIntent.decisionId) ||
                !AutoWowOracleRuntime::Owns(botGuid, finisherIntent.decisionId, oracleTick))
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherLeaseExpired, /*unsupported*/ false);

            if (finisherIntent.questId != dataPtr->questId)
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);

            QuestStatus const status = bot->GetQuestStatus(dataPtr->questId);
            if ((status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE) ||
                bot->GetQuestRewardStatus(dataPtr->questId))
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);

            QuestFinisherRef const finisher = AI_VALUE(QuestFinisherRef, "active quest finisher");
            AutoWowQuestFinisher::Facts const facts{
                status,
                bot->GetQuestRewardStatus(dataPtr->questId),
                status == QUEST_STATUS_INCOMPLETE && bot->CanCompleteQuest(dataPtr->questId),
                finisher.signedEntry != 0};
            if (!AutoWowQuestFinisher::IsFinisherReady(facts) ||
                finisher.signedEntry != finisherIntent.signedEntry ||
                finisher.stableSpawn.GetRawValue() != finisherIntent.stableSpawnGuid)
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);

            if (runtime.oracleLeaseRequired && runtime.oracleLeaseDecisionId != finisherIntent.decisionId)
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);
            if (runtime.oracleFinisherAuthorized &&
                (runtime.oracleFinisherDecisionId != finisherIntent.decisionId ||
                 runtime.oracleFinisherQuestId != finisherIntent.questId ||
                 runtime.oracleFinisherSignedEntry != finisherIntent.signedEntry ||
                 runtime.oracleFinisherStableSpawnGuid != finisherIntent.stableSpawnGuid))
                return BlockQuest(*dataPtr, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);

            runtime.oracleFinisherAuthorized = true;
            runtime.oracleFinisherDecisionId = finisherIntent.decisionId;
            runtime.oracleFinisherQuestId = finisherIntent.questId;
            runtime.oracleFinisherSignedEntry = finisherIntent.signedEntry;
            runtime.oracleFinisherStableSpawnGuid = finisherIntent.stableSpawnGuid;
            runtime.oracleTaggedDispatch = true;
            bool const handled = DoCompletedQuest(*dataPtr);
            runtime.oracleTaggedDispatch = false;
            return handled;
        }

        if (!objectiveParsed || !AutoWowOracleRuntime::HasActiveLease(botGuid, decisionId) ||
            !AutoWowOracleRuntime::Owns(botGuid, decisionId, oracleTick))
            return false;

        if (bot->GetQuestStatus(dataPtr->questId) != QUEST_STATUS_INCOMPLETE)
            return false;

        switch (dataPtr->objectiveRuntime.phase)
        {
            case QuestActionPhase::ResolveFinisher:
            case QuestActionPhase::TravelToFinisher:
            case QuestActionPhase::InteractFinisher:
            case QuestActionPhase::VerifyReward:
            case QuestActionPhase::Complete:
                return false;
            default:
                break;
        }

        runtime.oracleTaggedDispatch = true;
        bool const handled = DoIncompleteQuest(*dataPtr);
        runtime.oracleTaggedDispatch = false;
        return handled;
    }

    // Restore baseline New-RPG questgiver scanning and quest-log organization only for the
    // unmanaged ordinary path. Oracle-managed directives have already returned Reject or entered
    // the exact tagged branch above, so they can never reach this broad legacy behavior.
    if (AutoWowOracleQuestDispatchPolicy::AllowsLegacyQuestMaintenance(authority) &&
        SearchQuestGiverAndAcceptOrReward())
        return true;

    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

/* ------------------------------------------------------------------------- *
 *  Phase-machine helpers
 * ------------------------------------------------------------------------- */

void NewRpgDoQuestAction::EnterQuestPhase(NewRpgInfo::DoQuest& data, QuestActionPhase phase)
{
    data.objectiveRuntime.phase = phase;
    // `lastReachPOI` doubles as the per-phase dwell/entry timestamp; 0 == "not
    // yet timed since entering this phase".
    data.lastReachPOI = 0;
}

bool NewRpgDoQuestAction::BlockQuest(NewRpgInfo::DoQuest& data, QuestFailureReason reason, bool unsupported)
{
    QuestObjectiveRuntime& rt = data.objectiveRuntime;
    QuestActionPhase const priorPhase = rt.phase;
    rt.failure = reason;
    rt.phase = QuestActionPhase::Blocked;
    uint32 const blockedAt = getMSTime();
    rt.blockedAtMs = blockedAt ? blockedAt : 1;  // 0 is the never-blocked sentinel
    LOG_DEBUG("playerbots", "[New RPG] {} quest {} blocked (reason {}, unsupported {})", bot->GetName(), data.questId,
              static_cast<uint32>(reason), unsupported);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitBlocked(bot, data.questId, AutoWowQuestLedger::ReasonName(reason),
                                        AutoWowQuestLedger::PhaseName(priorPhase));

    if (unsupported)
    {
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, data.questId, "executor_unsupported",
                                     AutoWowQuestLedger::PhaseName(priorPhase));
        // Genuinely un-runnable by this Phase-1 executor: deprioritize so this
        // bot won't reselect it, and return to Idle. The Director still owns the
        // real abandon decision; we do NOT touch the questAbandoned statistic.
        botAI->lowPriorityQuest.insert(data.questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }

    // Execution failure (stuck, no progress, finisher not yet reachable): hold in
    // the Blocked phase so the external Director can observe objectiveRuntime and
    // decide. The RPG_DO_QUEST -> IDLE status timeout is the ultimate safety net.
    return ForceToWait(3000);
}

bool NewRpgDoQuestAction::DeferBlockedQuest(NewRpgInfo::DoQuest& data)
{
    QuestObjectiveRuntime const& rt = data.objectiveRuntime;
    bool const oracleManaged = rt.oracleManaged || AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter());
    if (!QuestStallRecoveryPolicy::ShouldDeferBlocked(oracleManaged, rt.blockedAtMs, getMSTime()))
        return false;

    LOG_INFO("playerbots", "[New RPG] {} quest {} deferred after blocked hold (reason {}) for {} ms",
             bot->GetName(), data.questId, AutoWowQuestLedger::ReasonName(rt.failure),
             QuestStallRecoveryPolicy::kDeferMs);
    DeferQuestForStall(data.questId);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, data.questId, "blocked_deferred",
                                 AutoWowQuestLedger::PhaseName(rt.phase));
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::TravelProgressExpired(QuestObjectiveRuntime& rt, WorldPosition const& keyPos,
                                                float distance)
{
    namespace Stall = QuestStallRecoveryPolicy;
    return Stall::ObserveTravel(rt.travelWatch,
                                Stall::MakeTravelKey(keyPos.GetMapId(), keyPos.GetPositionX(), keyPos.GetPositionY()),
                                Stall::QuantizeYards(distance), getMSTime()) == Stall::TravelVerdict::Expired;
}

bool NewRpgDoQuestAction::ExpireUnreachableSource(NewRpgInfo::DoQuest& data, QuestObjectiveSpec const& spec)
{
    QuestObjectiveRuntime& rt = data.objectiveRuntime;
    QuestStallRecoveryPolicy::ResetTravel(rt.travelWatch);
    uint64 const failedSpawn = rt.selectedSourceSpawn.GetRawValue();
    LOG_INFO("playerbots", "[New RPG] {} quest {} travel_no_progress toward ({},{},{}) spawn={} rotations={}",
             bot->GetName(), data.questId, data.pos.GetPositionX(), data.pos.GetPositionY(),
             data.pos.GetPositionZ(), failedSpawn, rt.travelRotationCount);
    if (failedSpawn == 0 || rt.travelRotationCount >= QuestStallRecoveryPolicy::kMaxTravelRotations)
        return BlockQuest(data, QuestFailureReason::TravelNoProgress, /*unsupported*/ false);

    // Same shape as the exhausted-GO rotation in VerifyProgress: remember the unreachable spawn and
    // continue only when the rotation policy names another resolved same-map spawn.
    ++rt.travelRotationCount;
    rt.exhaustedSourceSpawns.insert(failedSpawn);
    rt.sourceRotationCooldownUntil[failedSpawn] = getMSTime() + QuestStallRecoveryPolicy::kDeferMs;
    ++rt.sourceRotationCount;
    std::vector<int32> objectiveEntries;
    std::vector<QuestSourceRotationPolicy::Candidate> candidates;
    for (QuestObjectiveSource const& source : spec.sources)
    {
        int32 const signedEntry = source.type == QuestObjectiveSource::Type::Creature
            ? static_cast<int32>(source.entry)
            : SignedGameObjectObjectiveEntry(source.entry);
        if (signedEntry == 0)
            continue;
        objectiveEntries.push_back(signedEntry);
        for (GuidPosition const& spawn : source.spawns)
            candidates.push_back({signedEntry, spawn.GetMapId(), spawn.GetRawValue(),
                bot->GetDistance2d(spawn.GetPositionX(), spawn.GetPositionY())});
    }
    std::vector<uint64> exhausted(rt.exhaustedSourceSpawns.begin(), rt.exhaustedSourceSpawns.end());
    QuestSourceRotationPolicy::Request const request{
        bot->GetMapId(), rt.selectedSourceEntry, failedSpawn, false,
        rt.sourceRotationCount, maxSourceRotations, objectiveEntries, exhausted, candidates};
    if (QuestSourceRotationPolicy::Decide(request).decision !=
        QuestSourceRotationPolicy::Decision::RotateToNextSource)
        return BlockQuest(data, QuestFailureReason::TravelNoProgress, /*unsupported*/ false);
    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
    return true;
}

namespace
{
// Per-bot full-bag escalation state. Kept outside NewRpgInfo so an Oracle re-arm or an RPG
// status change cannot reset the backoff. Bots update on map threads, hence the lock.
// ponytail: one global lock; entries are touched only while a bot's bags are full.
std::mutex fullBagReliefLock;
std::unordered_map<uint32, QuestInventoryReliefPolicy::FullBagRelief> fullBagReliefByBot;
}  // namespace

bool NewRpgDoQuestAction::RelieveFullBagsForQuest(NewRpgInfo::DoQuest& data)
{
    namespace Relief = QuestInventoryReliefPolicy;
    static_assert(Relief::PoorQuality == ITEM_QUALITY_POOR);

    QuestObjectiveRuntime& rt = data.objectiveRuntime;
    uint32 const botGuid = bot->GetGUID().GetCounter();
    uint32 const nowMs = getMSTime();
    Relief::FullBagStep step;
    {
        std::lock_guard<std::mutex> guard(fullBagReliefLock);
        step = Relief::ObserveFullBag(fullBagReliefByBot[botGuid], data.questId, nowMs);
    }

    auto setAside = [&]()
    {
        // Same shape as the Oracle "deferred unrunnable" path: keep the quest, lower its
        // selection priority, and let the bot move on to other work.
        botAI->lowPriorityQuest.insert(data.questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    };

    if (step == Relief::FullBagStep::Backoff)
        return setAside();

    if (step == Relief::FullBagStep::TryVendor && TryRelieveInventoryAtVendor(data, /*incompleteItem*/ true))
        return true;

    // Vendor window spent or no vendor progress possible: free the needed slots from safe junk.
    std::vector<Relief::BagItemFacts> items;
    auto collect = [&](uint8 bag, uint8 slot, Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return;
        Relief::BagItemFacts facts;
        facts.bag = bag;
        facts.slot = slot;
        facts.quality = static_cast<uint8>(proto->Quality);
        facts.questRelated = proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM ||
                             proto->StartQuest != 0 || bot->HasQuestForItem(proto->ItemId);
        facts.retainedClass = proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT ||
                              proto->Class == ITEM_CLASS_RECIPE;
        if (facts.quality == Relief::PoorQuality && !facts.questRelated && !facts.retainedClass)
        {  // item usage is the expensive fact; only junk candidates need it
            ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", proto->ItemId);
            facts.usageSafe = usage == ITEM_USAGE_NONE || usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH;
        }
        facts.value = proto->SellPrice * item->GetCount();
        items.push_back(facts);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        collect(INVENTORY_SLOT_BAG_0, slot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                collect(bagSlot, static_cast<uint8>(slot), bag->GetItemByPos(static_cast<uint8>(slot)));

    std::vector<Relief::BagItemFacts> const junk = Relief::SelectJunkToDestroy(items, Relief::NeededJunkSlots);
    if (junk.size() == Relief::NeededJunkSlots)
    {
        for (Relief::BagItemFacts const& victim : junk)
        {
            Item* item = bot->GetItemByPos(victim.bag, victim.slot);
            LOG_INFO("playerbots", "[New RPG] {} quest {} full-bag relief destroyed junk item {} x{} (value {})",
                     bot->GetName(), data.questId, item ? item->GetEntry() : 0, item ? item->GetCount() : 0,
                     victim.value);
            bot->DestroyItem(victim.bag, victim.slot, true);
        }
        {
            std::lock_guard<std::mutex> guard(fullBagReliefLock);
            Relief::ResolveFullBag(fullBagReliefByBot[botGuid]);
        }
        context->GetValue<uint8>("bag space")->Reset();
        rt.failure = QuestFailureReason::None;
        EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
        rt.attemptCount = 0;
        return true;
    }

    // Nothing safe to free: defer with a reason and back off before looking at this quest again.
    uint32 backoffMs = 0;
    {
        std::lock_guard<std::mutex> guard(fullBagReliefLock);
        backoffMs = Relief::DeferFullBag(fullBagReliefByBot[botGuid], nowMs);
    }
    LOG_DEBUG("playerbots", "[New RPG] {} quest {} deferred on full bags, backoff {} ms", bot->GetName(),
              data.questId, backoffMs);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, data.questId, "inventory_full",
                                 AutoWowQuestLedger::PhaseName(rt.phase));
    return setAside();
}

bool NewRpgDoQuestAction::MaintainQuestPartyCohesion(NewRpgInfo::DoQuest const& data,
                                                     bool objectiveWorkWindow)
{
    Group* group = bot->GetGroup();
    Player* leader = botAI->GetGroupLeader();
    if (!group || !leader || group->GetLeaderGUID() != leader->GetGUID() || leader == bot)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    auto const* leaderQuest = leaderAI
        ? std::get_if<NewRpgInfo::DoQuest>(&leaderAI->rpgInfo.data)
        : nullptr;
    if (!leaderQuest || leaderQuest->questId != data.questId)
        return false;

    AutoWowQuestPartyCohesion::Facts const facts{
        true,
        true,
        leader->IsInWorld(),
        leader->IsAlive(),
        bot->GetMapId() == leader->GetMapId(),
        bot->IsAlive(),
        bot->IsInCombat(),
        objectiveWorkWindow,
        leader->IsInWorld() && bot->GetMapId() == leader->GetMapId()
            ? bot->GetDistance2d(leader)
            : 0.0f};
    AutoWowQuestPartyCohesion::Result const decision =
        AutoWowQuestPartyCohesion::Evaluate(facts);

    switch (decision.action)
    {
        case AutoWowQuestPartyCohesion::Action::RejoinLeader:
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);

            if (botAI->DoSpecificAction("follow", Event("autowow quest cohesion", "", bot), true))
                return true;

            // Follow is a soft preference.  If the follow action cannot currently produce a
            // usable movement target, let the follower continue its own objective executor.
            return false;
        }
        case AutoWowQuestPartyCohesion::Action::AllowIndependent:
        case AutoWowQuestPartyCohesion::Action::NotApplicable:
        case AutoWowQuestPartyCohesion::Action::AllowObjective:
        case AutoWowQuestPartyCohesion::Action::AllowException:
            return false;
    }

    return false;
}

int32 NewRpgDoQuestAction::ObjectiveCurrentCount(uint32 questId, int32 objectiveIdx) const
{
    auto const& map = bot->getQuestStatusMap();
    auto it = map.find(questId);
    if (it == map.end())
        return 0;
    const QuestStatusData& q_status = it->second;
    if (objectiveIdx >= 0 && objectiveIdx < QUEST_OBJECTIVES_COUNT)
        return q_status.CreatureOrGOCount[objectiveIdx];
    if (objectiveIdx >= QUEST_OBJECTIVES_COUNT && objectiveIdx < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        return q_status.ItemCount[objectiveIdx - QUEST_OBJECTIVES_COUNT];
    return 0;
}

int32 NewRpgDoQuestAction::ObjectiveRequiredCount(Quest const* quest, int32 objectiveIdx) const
{
    if (!quest)
        return 0;
    if (objectiveIdx >= 0 && objectiveIdx < QUEST_OBJECTIVES_COUNT)
        return quest->RequiredNpcOrGoCount[objectiveIdx];
    if (objectiveIdx >= QUEST_OBJECTIVES_COUNT && objectiveIdx < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        return quest->RequiredItemCount[objectiveIdx - QUEST_OBJECTIVES_COUNT];
    return 0;
}

bool NewRpgDoQuestAction::ResolveSourceTravelPos(QuestObjectiveSpec const& spec, uint32 questId, int32 objectiveIdx,
                                                 QuestObjectiveRuntime& runtime, WorldPosition& out)
{
    // The quest POI is only a coarse objective-area hint. The resolver publishes
    // exact stable source spawns, so walk to a deterministic same-map spawn and
    // let strict targeting bind the live unit. When a source has been exhausted,
    // rotate it out for a bounded cooldown; never fall back to an unrelated grind.
    struct ResolvedSource
    {
        QuestObjectiveSource const* source = nullptr;
        GuidPosition const* spawn = nullptr;
        float distance = 0.0f;
    };

    if (!runtime.sourceRotationObjectiveKnown || runtime.sourceRotationQuestId != spec.key.questId ||
        runtime.sourceRotationSlot != spec.key.slot || runtime.sourceRotationFamily != spec.key.family)
    {
        runtime.sourceRotationObjectiveKnown = true;
        runtime.sourceRotationQuestId = spec.key.questId;
        runtime.sourceRotationSlot = spec.key.slot;
        runtime.sourceRotationFamily = spec.key.family;
        runtime.sourceRotationCount = 0;
        runtime.exhaustedSourceSpawns.clear();
        runtime.sourceRotationCooldownUntil.clear();
    }

    uint32 const now = getMSTime();
    for (auto iterator = runtime.sourceRotationCooldownUntil.begin();
         iterator != runtime.sourceRotationCooldownUntil.end();)
    {
        if (iterator->second <= now)
        {
            runtime.exhaustedSourceSpawns.erase(iterator->first);
            iterator = runtime.sourceRotationCooldownUntil.erase(iterator);
        }
        else
            ++iterator;
    }

    std::vector<ResolvedSource> candidates;
    std::vector<QuestSourceRotationPolicy::Candidate> policyCandidates;
    std::vector<int32> objectiveEntries;

    for (QuestObjectiveSource const& source : spec.sources)
    {
        int32 const signedEntry = source.type == QuestObjectiveSource::Type::Creature
                                      ? static_cast<int32>(source.entry)
                                      : SignedGameObjectObjectiveEntry(source.entry);
        if (signedEntry == 0)
            continue;
        objectiveEntries.push_back(signedEntry);

        for (GuidPosition const& spawn : source.spawns)
        {
            uint64 const spawnId = spawn.GetRawValue();
            if (spawnId == 0 || spawn.GetMapId() != bot->GetMapId() ||
                runtime.exhaustedSourceSpawns.count(spawnId) != 0)
                continue;

            // A CAST objective commonly leaves the interacted creature alive
            // while its script resets. Do not route straight back to that same
            // loaded creature during its per-target cooldown; choose the next
            // exact spawn instead. Unloaded spawns have never been selected in
            // this process and remain valid travel candidates.
            if (spec.kind == QuestObjectiveKind::UseQuestItem &&
                source.type == QuestObjectiveSource::Type::Creature)
            {
                GuidPosition probe = spawn;
                if (Creature* creature = probe.GetCreature())
                {
                    auto cooldown = runtime.targetCooldownUntil.find(creature->GetGUID());
                    if (cooldown != runtime.targetCooldownUntil.end() && cooldown->second > getMSTime())
                        continue;
                }
            }

            float const distance = bot->GetDistance2d(spawn.GetPositionX(), spawn.GetPositionY());
            candidates.push_back({&source, &spawn, distance});
            policyCandidates.push_back({signedEntry, spawn.GetMapId(), spawnId, distance});
        }
    }

    std::vector<uint64> exhausted;
    exhausted.reserve(runtime.exhaustedSourceSpawns.size());
    for (uint64 const spawnId : runtime.exhaustedSourceSpawns)
        exhausted.push_back(spawnId);

    QuestSourceRotationPolicy::Request const rotationRequest{
        bot->GetMapId(),
        runtime.selectedSourceEntry,
        runtime.selectedSourceSpawn.GetRawValue(),
        false,
        runtime.sourceRotationCount,
        maxSourceRotations,
        objectiveEntries,
        exhausted,
        policyCandidates};
    QuestSourceRotationPolicy::Result const rotation = QuestSourceRotationPolicy::Decide(rotationRequest);

    if (rotation.decision == QuestSourceRotationPolicy::Decision::RotateToNextSource)
    {
        auto resolved = std::find_if(candidates.begin(), candidates.end(), [&](ResolvedSource const& candidate)
        {
            int32 const signedEntry = candidate.source->type == QuestObjectiveSource::Type::Creature
                                          ? static_cast<int32>(candidate.source->entry)
                                          : SignedGameObjectObjectiveEntry(candidate.source->entry);
            return signedEntry == rotation.candidate.signedEntry &&
                   candidate.spawn->GetRawValue() == rotation.candidate.spawnKey;
        });
        if (resolved == candidates.end())
            return false;

        QuestObjectiveSource const* bestSource = resolved->source;
        GuidPosition const* bestSpawn = resolved->spawn;
        uint64 const previousSpawnId = runtime.selectedSourceSpawn.GetRawValue();
        runtime.selectedSourceEntry = bestSource->type == QuestObjectiveSource::Type::Creature
                                          ? static_cast<int32>(bestSource->entry)
                                          : SignedGameObjectObjectiveEntry(bestSource->entry);
        if (runtime.selectedSourceEntry == 0)
            return false;
        runtime.selectedSourceSpawn = *bestSpawn;
        if (previousSpawnId != 0 && previousSpawnId != bestSpawn->GetRawValue())
            runtime.attemptCount = 0;
        out = WorldPosition(*bestSpawn);
        return true;
    }

    // Every resolved same-map source is temporarily exhausted. Let the native
    // phase wait for the earliest cooldown instead of converting a valid quest
    // into a generic POI/grind fallback. The caller handles the bounded wait.
    if (!runtime.exhaustedSourceSpawns.empty())
        return false;

    // A spell-focus objective must route to an exact resolved focus spawn. A quest POI is only an
    // area hint and cannot establish that the required focus exists there.
    if (spec.kind == QuestObjectiveKind::UseQuestItem)
        return false;

    // Stable spawn data is occasionally absent. Retain the proven quest-POI
    // fallback (valid height + same-map movement) for those quests.
    std::vector<POIInfo> poiInfo;
    if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        return false;

    const POIInfo* best = nullptr;
    float bestDist = 0.0f;
    for (const POIInfo& poi : poiInfo)
    {
        if (poi.objectiveIdx != objectiveIdx)
            continue;
        float d = bot->GetDistance2d(poi.pos.x, poi.pos.y);
        if (!best || d < bestDist)
        {
            best = &poi;
            bestDist = d;
        }
    }
    // Fall back to any POI for this quest if none matched the exact objective
    // index (POI data occasionally lacks per-objective granularity).
    if (!best && !poiInfo.empty())
    {
        for (const POIInfo& poi : poiInfo)
        {
            float d = bot->GetDistance2d(poi.pos.x, poi.pos.y);
            if (!best || d < bestDist)
            {
                best = &poi;
                bestDist = d;
            }
        }
    }
    if (!best)
        return false;

    float dx = best->pos.x, dy = best->pos.y;
    float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));
    if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
        return false;

    out = WorldPosition(bot->GetMapId(), dx, dy, dz);
    return true;
}

bool NewRpgDoQuestAction::BindFinisher(QuestObjectiveRuntime& rt, float /*range*/, WorldObject** outObject)
{
    if (outObject)
        *outObject = nullptr;

    QuestFinisherRef& fin = rt.finisher;
    if (!fin.runtimeGuid.IsEmpty())
    {
        // Already bound; make sure it's still loaded.
        if (WorldObject* obj = ObjectAccessor::GetWorldObject(*bot, fin.runtimeGuid))
        {
            // Oracle binding rechecks the DB spawn store below. A live runtime GUID is generated
            // independently of its stable DB spawn ID and cannot prove that relation by itself.
            if (rt.oracleFinisherAuthorized)
            {
                fin.runtimeGuid = ObjectGuid();
            }
            else
            {
                rt.finisherReceipt.runtimeGuid = obj->GetGUID();
                if (outObject)
                    *outObject = obj;
                return obj->IsInWorld();
            }
        }
        fin.runtimeGuid = ObjectGuid();
    }

    bool isGameObject = fin.signedEntry < 0;
    uint32 entry = static_cast<uint32>(std::abs(fin.signedEntry));

    auto acceptsExactStableTarget = [&](WorldObject* candidate) -> bool
    {
        if (!candidate)
            return false;

        GameObject* const gameObject = candidate->ToGameObject();
        Creature* const creature = candidate->ToCreature();
        bool const exactFamily = isGameObject ? gameObject != nullptr : creature != nullptr;
        bool const usable = isGameObject ? gameObject && gameObject->isSpawned()
                                         : creature && creature->IsAlive();
        AutoWowQuestFinisher::StableSpawnIdentity const expected{
            fin.stableSpawn.GetMapId(), bot->GetInstanceId(), entry,
            fin.stableSpawn.GetCounter(), isGameObject};
        AutoWowQuestFinisher::StableSpawnIdentity const live{
            candidate->GetMapId(), candidate->GetInstanceId(), candidate->GetEntry(),
            gameObject ? gameObject->GetSpawnId()
                       : creature ? creature->GetSpawnId() : 0,
            gameObject != nullptr};
        bool const exactSpawn = AutoWowQuestFinisher::MatchesStableSpawn(
            expected, live, candidate->GetGUID().GetRawValue());
        AutoWowQuestFinisher::LoadedStableTargetFacts const facts{
            true,
            candidate->IsInWorld(),
            usable,
            candidate->GetMapId() == bot->GetMapId() && fin.stableSpawn.GetMapId() == bot->GetMapId(),
            candidate->GetInstanceId() == bot->GetInstanceId(),
            isGameObject ? gameObject && gameObject->GetEntry() == entry : creature && creature->GetEntry() == entry,
            exactSpawn,
            candidate->GetGUID().GetRawValue() != 0,
            exactFamily};
        return AutoWowQuestFinisher::IsExactLoadedStableTarget(facts);
    };

    // Prefer an exact stable spawn already present in the loaded map store. GuidPosition's
    // ObjectAccessor lookup can miss an object that is live in a loaded grid, while the spawn
    // store retains the authoritative DB spawn identity. This is a lookup only: it does not
    // summon, relocate, or broaden the accepted finisher relation.
    WorldObject* exact = nullptr;
    if (bot->GetMap() && fin.stableSpawn.GetMapId() == bot->GetMapId())
    {
        ObjectGuid::LowType const spawnId = static_cast<ObjectGuid::LowType>(fin.stableSpawn.GetCounter());
        if (isGameObject)
        {
            auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(spawnId);
            for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
            {
                if (acceptsExactStableTarget(iterator->second))
                {
                    exact = iterator->second;
                    break;
                }
            }
        }
        else
        {
            auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
            for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
            {
                if (acceptsExactStableTarget(iterator->second))
                {
                    exact = iterator->second;
                    break;
                }
            }
        }
    }

    // Retain the existing loaded-object lookup as a compatible fallback, but subject it to the
    // same exact identity gates before it can become a walk target.
    if (!exact)
    {
        WorldObject* hinted = isGameObject ? static_cast<WorldObject*>(fin.stableSpawn.GetGameObject())
                                           : static_cast<WorldObject*>(fin.stableSpawn.GetCreature());
        if (acceptsExactStableTarget(hinted))
            exact = hinted;
    }

    if (exact)
    {
        fin.runtimeGuid = exact->GetGUID();
        rt.finisherReceipt.runtimeGuid = fin.runtimeGuid;
        if (outObject)
            *outObject = exact;
        return true;
    }

    // The stable spawn is the only accepted identity. If it is not loaded in this map/instance,
    // the caller waits or blocks; it must not silently bind a different same-entry NPC/GO.
    return false;
}

bool NewRpgDoQuestAction::BindSourceGameObject(QuestObjectiveRuntime& rt, float range)
{
    if (rt.selectedSourceEntry >= 0)
        return false;
    if (rt.oracleRouteIdentityPinned)
    {
        WorldObject* exact = BindExactOracleRouteTarget(rt, false);
        return exact && exact->ToGameObject() && bot->GetDistance(exact) <= range;
    }

    uint32 const entry = static_cast<uint32>(-rt.selectedSourceEntry);
    if (!rt.selectedTargetGuid.IsEmpty())
    {
        if (GameObject* go = ObjectAccessor::GetGameObject(*bot, rt.selectedTargetGuid);
            go && go->IsInWorld() && go->isSpawned() && go->GetEntry() == entry)
            return true;
        rt.selectedTargetGuid.Clear();
    }

    // The quest source may be a valid DB spawn in an unloaded grid. Gathering already uses this
    // exact-spawn pattern: load the published source cell, then resolve by spawn id instead of
    // relying on the bot's current object visibility set. This is a normal visibility operation,
    // not a summon or quest-state mutation; all identity and instance gates remain authoritative.
    if (bot->GetMap() && rt.selectedSourceSpawn.GetMapId() == bot->GetMapId())
    {
        bot->GetMap()->LoadGrid(rt.selectedSourceSpawn.GetPositionX(), rt.selectedSourceSpawn.GetPositionY());
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(
            static_cast<ObjectGuid::LowType>(rt.selectedSourceSpawn.GetCounter()));
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
        {
            GameObject* exact = iterator->second;
            if (exact && exact->IsInWorld() && exact->isSpawned() && exact->GetSpawnId() ==
                    rt.selectedSourceSpawn.GetCounter() &&
                exact->GetEntry() == entry && exact->GetMapId() == bot->GetMapId() &&
                exact->GetInstanceId() == bot->GetInstanceId() && bot->GetDistance(exact) <= range)
            {
                rt.selectedTargetGuid = exact->GetGUID();
                return true;
            }
        }
    }

    if (GameObject* exact = rt.selectedSourceSpawn.GetGameObject();
        exact && exact->IsInWorld() && exact->isSpawned() && exact->GetEntry() == entry &&
        bot->GetDistance(exact) <= range)
    {
        rt.selectedTargetGuid = exact->GetGUID();
        return true;
    }

    if (GameObject* nearest = bot->FindNearestGameObject(entry, range))
    {
        rt.selectedTargetGuid = nearest->GetGUID();
        return true;
    }
    return false;
}

bool NewRpgDoQuestAction::BindQuestItemTarget(QuestObjectiveSpec const& spec, QuestObjectiveRuntime& rt, float range)
{
    if (rt.selectedSourceEntry <= 0 || spec.kind != QuestObjectiveKind::UseQuestItem)
        return false;
    if (rt.oracleRouteIdentityPinned)
    {
        WorldObject* exact = BindExactOracleRouteTarget(rt, false);
        return exact && exact->ToCreature() && bot->GetDistance(exact) <= range;
    }

    uint32 const entry = static_cast<uint32>(rt.selectedSourceEntry);
    uint32 const now = getMSTime();
    bool const targetConditionGate = sPlayerbotAIConfig.autoWowQuestItemTargetConditions;
    auto eligible = [&](Creature* creature)
    {
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->GetEntry() != entry ||
            bot->GetDistance(creature) > range)
            return false;
        if (targetConditionGate && !QuestItemSpellTargetConditionsMet(bot, spec.questItemId, creature))
            return false;

        auto cooldown = rt.targetCooldownUntil.find(creature->GetGUID());
        if (cooldown == rt.targetCooldownUntil.end())
            return true;
        if (cooldown->second > now)
            return false;
        rt.targetCooldownUntil.erase(cooldown);
        return true;
    };

    if (!rt.selectedTargetGuid.IsEmpty())
    {
        if (Creature* creature = ObjectAccessor::GetCreature(*bot, rt.selectedTargetGuid); eligible(creature))
            return true;
        rt.selectedTargetGuid.Clear();
    }

    Creature* best = nullptr;
    float bestDistance = 0.0f;
    for (QuestObjectiveSource const& source : spec.sources)
    {
        if (source.type != QuestObjectiveSource::Type::Creature || source.entry != entry)
            continue;
        for (GuidPosition const& spawn : source.spawns)
        {
            GuidPosition probe = spawn;
            Creature* creature = probe.GetCreature();
            if (!eligible(creature))
                continue;
            float const distance = bot->GetDistance(creature);
            if (!best || distance < bestDistance ||
                (distance == bestDistance && creature->GetGUID().GetCounter() < best->GetGUID().GetCounter()))
            {
                best = creature;
                bestDistance = distance;
            }
        }
    }

    // Stable spawn maps can lag a loaded grid. The nearest-entry fallback is
    // still exact and retains every eligibility/cooldown gate above.
    if (!best)
    {
        Creature* nearest = bot->FindNearestCreature(entry, range, true);
        if (eligible(nearest))
            best = nearest;
    }

    if (!best)
        return false;

    rt.selectedTargetGuid = best->GetGUID();
    return true;
}

bool NewRpgDoQuestAction::BindScriptedEventTarget(QuestObjectiveSpec const& spec, QuestObjectiveRuntime& rt,
                                                   float range)
{
    if (rt.selectedSourceEntry <= 0 || spec.kind != QuestObjectiveKind::ScriptedEvent)
        return false;
    if (rt.oracleRouteIdentityPinned)
        return BindExactOracleRouteTarget(rt, false) != nullptr;

    uint32 const entry = static_cast<uint32>(rt.selectedSourceEntry);
    if (!rt.selectedTargetGuid.IsEmpty())
    {
        if (Creature* creature = ObjectAccessor::GetCreature(*bot, rt.selectedTargetGuid);
            creature && creature->IsInWorld() && creature->GetEntry() == entry)
            return true;
        rt.selectedTargetGuid.Clear();
    }

    // The stable DB GUID continues to identify an escort after it has walked
    // away from its spawn coordinates. Do not apply a range gate to this exact
    // lookup: the caller will path back to the moving creature if it fell behind.
    GuidPosition exactSpawn = rt.selectedSourceSpawn;
    if (Creature* exact = exactSpawn.GetCreature(); exact && exact->IsInWorld() && exact->GetEntry() == entry)
    {
        rt.selectedTargetGuid = exact->GetGUID();
        return true;
    }

    if (Creature* nearest = bot->FindNearestCreature(entry, range, true))
    {
        rt.selectedTargetGuid = nearest->GetGUID();
        return true;
    }
    return false;
}

WorldObject* NewRpgDoQuestAction::BindExactOracleRouteTarget(QuestObjectiveRuntime& rt, bool finisher)
{
    GuidPosition stable = finisher ? rt.finisher.stableSpawn : rt.selectedSourceSpawn;
    int32 const signedEntry = finisher ? rt.finisher.signedEntry : rt.selectedSourceEntry;
    if (signedEntry == 0 || stable.GetRawValue() == 0 || !bot->GetMap() ||
        stable.GetMapId() != bot->GetMapId())
        return nullptr;

    bool const gameObject = signedEntry < 0;
    uint32 const entry = static_cast<uint32>(std::abs(signedEntry));
    auto accepts = [&](WorldObject* candidate)
    {
        if (!candidate || !candidate->IsInWorld())
            return false;
        if (gameObject)
        {
            GameObject* go = candidate->ToGameObject();
            if (!go || !go->isSpawned())
                return false;
            return AutoWowQuestFinisher::MatchesStableSpawn(
                {stable.GetMapId(), bot->GetInstanceId(), entry, stable.GetCounter(), true},
                {go->GetMapId(), go->GetInstanceId(), go->GetEntry(), go->GetSpawnId(), true},
                go->GetGUID().GetRawValue());
        }
        Creature* creature = candidate->ToCreature();
        if (!creature || !creature->IsAlive())
            return false;
        return AutoWowQuestFinisher::MatchesStableSpawn(
            {stable.GetMapId(), bot->GetInstanceId(), entry, stable.GetCounter(), false},
            {creature->GetMapId(), creature->GetInstanceId(), creature->GetEntry(),
             creature->GetSpawnId(), false}, creature->GetGUID().GetRawValue());
    };

    WorldObject* exact = nullptr;
    ObjectGuid::LowType const spawnId = static_cast<ObjectGuid::LowType>(stable.GetCounter());
    if (gameObject)
    {
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(spawnId);
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
            if (accepts(iterator->second))
            {
                exact = iterator->second;
                break;
            }
    }
    else
    {
        auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
            if (accepts(iterator->second))
            {
                exact = iterator->second;
                break;
            }
    }

    if (!exact)
    {
        WorldObject* hinted = gameObject ? static_cast<WorldObject*>(stable.GetGameObject())
                                         : static_cast<WorldObject*>(stable.GetCreature());
        if (accepts(hinted))
            exact = hinted;
    }
    if (exact)
    {
        if (finisher)
        {
            rt.finisher.runtimeGuid = exact->GetGUID();
            rt.finisherReceipt.runtimeGuid = exact->GetGUID();
        }
        else
            rt.selectedTargetGuid = exact->GetGUID();
    }
    return exact;
}

bool NewRpgDoQuestAction::DriveOracleQuestRoute(NewRpgInfo::DoQuest& data,
                                                 QuestObjectiveSpec const* objective,
                                                 int32 objectiveIdx, bool finisher)
{
    using namespace AutoWowOracleRoute;

    QuestObjectiveRuntime& rt = data.objectiveRuntime;
    bool const enabled = OracleRouteV2Enabled();
    GuidPosition const stable = finisher ? rt.finisher.stableSpawn : rt.selectedSourceSpawn;
    int32 const signedEntry = finisher ? rt.finisher.signedEntry : rt.selectedSourceEntry;
    AutoWowQuestFinisher::RouteAdmissionFacts const admission{
        rt.oracleManaged, rt.oracleTaggedDispatch, enabled,
        signedEntry != 0 && stable.GetRawValue() != 0 && WorldPosition(stable) != WorldPosition()};
    if (!admission.oracleManaged || !admission.oracleTagged || !admission.routeV2Enabled)
        return false;
    if (!AutoWowQuestFinisher::UseOracleRoute(admission))
        return BlockQuest(data, QuestFailureReason::OracleRouteInvalidDescriptor, false);

    uint64 const botGuid = bot->GetGUID().GetCounter();
    AutoWowOracleRuntime::OracleBotSnapshot snapshot;
    if (!AutoWowOracleRuntime::GetOracleSnapshot(botGuid, snapshot) || !snapshot.leaseAvailable ||
        snapshot.decisionId == 0 || snapshot.intentId == 0 ||
        snapshot.decisionId != rt.oracleLeaseDecisionId)
        return BlockQuest(data, QuestFailureReason::OracleRouteInvalidDescriptor, false);

    RouteIntent intent;
    intent.actor = botGuid;
    intent.purpose = static_cast<PurposeId>(finisher ? RoutePurpose::QuestTarget
                                                     : RoutePurpose::QuestSource);
    intent.mapId = stable.GetMapId();
    intent.instanceId = bot->GetInstanceId();
    intent.targetKind = finisher ? RouteTargetKind::QuestFinisher
                                 : signedEntry < 0 ? RouteTargetKind::GameObject
                                                   : RouteTargetKind::Creature;
    intent.entry = static_cast<EntryId>(std::abs(signedEntry));
    intent.stableSpawn = stable.GetRawValue();
    intent.dbX = stable.GetPositionX();
    intent.dbY = stable.GetPositionY();
    intent.dbZ = stable.GetPositionZ();
    intent.dbO = stable.GetOrientation();
    intent.radius = finisher ? INTERACTION_DISTANCE : questTravelArriveDist;
    intent.controller = botGuid;
    intent.job = snapshot.intentId;
    intent.lease = snapshot.decisionId;
    intent.segment = stable.GetMapId() == bot->GetMapId()
        ? RouteSegment::GridLoadApproach
        : RouteSegment::InstanceAdmission;
    intent.questId = data.questId;
    intent.objectiveFamily = finisher ? kQuestFinisherObjectiveFamily
        : static_cast<ObjectiveFamilyId>(objective ? objective->key.family
                                                   : QuestObjectiveFamily::NpcOrGameObject);
    intent.objectiveSlot = finisher ? kQuestFinisherObjectiveSlot
        : static_cast<ObjectiveSlotId>(objective ? objective->key.slot : 0);
    intent.routeCatalogVersion = kQuestRouteCatalogVersion;

    auto currentReentryFacts = [&]()
    {
        bool const targetLoaded = bot->GetMap() && stable.GetMapId() == bot->GetMapId() &&
            bot->GetMap()->IsGridLoaded(stable.GetPositionX(), stable.GetPositionY());
        AutoWowOracleQuestExecutor::QuestRouteReentryFacts facts;
        facts.targetLoadKnown = bot->GetMap() && stable.GetMapId() == bot->GetMapId();
        facts.targetLoaded = targetLoaded;
        facts.positionKnown = bot->IsInWorld();
        facts.mapId = bot->GetMapId();
        facts.x = bot->GetPositionX();
        facts.y = bot->GetPositionY();
        return facts;
    };

    WorldPosition const destination(stable);
    auto selectPreparedPath = [&]() -> std::optional<AutoWowDungeonPath::ProbeResult>
    {
        if (auto direct = AutoWowQuestGiverTravel::SelectCompleteWalkProbe(bot, destination))
            return direct;
        return AutoWowQuestGiverTravel::SelectTravelMgrWalkProbe(bot, destination);
    };

    // Oracle ownership ticks are milliseconds; the route policy's recovery/backoff constants are
    // explicitly seconds.  Convert at this boundary so 15/30/45 and 30/120/600 retain their units.
    Tick const now = static_cast<Tick>(AutoWowOracleRuntime::CurrentTick() / 1000U);
    RouteKey const routeKey = MakeRouteKey(intent);
    RouteLedgerKey const ledgerKey = MakeRouteLedgerKey(routeKey);
    SessionLedger durableLedger = AutoWowOracleQuestExecutor::LoadQuestRouteLedger(botGuid);
    WorldChangeId reentrySignalId = 0;

    auto enterExactTargetPhase = [&]()
    {
        rt.oracleRouteActive = false;
        rt.oracleRouteIdentityPinned = true;
        if (finisher)
            EnterQuestPhase(data, QuestActionPhase::InteractFinisher);
        else if (objective && objective->kind == QuestObjectiveKind::UseQuestItem)
            EnterQuestPhase(data, QuestActionPhase::UseQuestItem);
        else if (objective && objective->kind == QuestObjectiveKind::ScriptedEvent)
            EnterQuestPhase(data, QuestActionPhase::EscortEvent);
        else if (rt.selectedSourceEntry < 0)
            EnterQuestPhase(data, objective && objective->key.family == QuestObjectiveFamily::Item
                                      ? QuestActionPhase::LootSource
                                      : QuestActionPhase::InteractSource);
        else
            EnterQuestPhase(data, QuestActionPhase::AcquireTarget);
    };

    ZoneAssistState* assistState = FindZoneAssist(botGuid);
    bool const sameAssistedTarget = assistState &&
        assistState->routeKey.questId == routeKey.questId &&
        assistState->routeKey.mapId == routeKey.mapId &&
        assistState->routeKey.instanceId == routeKey.instanceId &&
        assistState->routeKey.targetKind == routeKey.targetKind &&
        assistState->routeKey.entry == routeKey.entry &&
        assistState->routeKey.stableSpawn == routeKey.stableSpawn;
    if (assistState && assistState->pending && !sameAssistedTarget)
        assistState->pending = false;
    if (assistState && assistState->pending)
    {
        bool const teleporting = bot->IsBeingTeleported();
        WorldObject* exact = nullptr;
        bool inRange = false;
        if (!teleporting && bot->GetMap() && bot->GetMapId() == stable.GetMapId() &&
            bot->GetInstanceId() == intent.instanceId)
        {
            bot->GetMap()->LoadGrid(stable.GetPositionX(), stable.GetPositionY());
            exact = BindExactOracleRouteTarget(rt, finisher);
            if (exact)
                inRange = finisher ? IsWithinInteractionDist(exact)
                    : bot->GetDistance(exact) <= static_cast<float>(intent.radius);
        }
        uint64 const elapsed = now >= assistState->lastAt ? now - assistState->lastAt : 0;
        AutoWowOracleZoneTravelAssist::BindDecision const bindDecision =
            AutoWowOracleZoneTravelAssist::DecidePostTeleportBind(
                teleporting, exact != nullptr, inRange, elapsed);
        if (bindDecision == AutoWowOracleZoneTravelAssist::BindDecision::EnterNativePhase)
        {
            assistState->pending = false;
            rt.oracleRouteSession = {};
            rt.oracleRouteReceiptAvailable = false;
            rt.oracleRouteDecisionId = snapshot.decisionId;
            LOG_INFO("playerbots",
                "[AutoWow Oracle ZoneTravelAssist] bot_guid={} quest_id={} "
                "spawn={} status=exact_bound teleport_not_walk_proof=true",
                botGuid, data.questId, stable.GetRawValue());
            enterExactTargetPhase();
            return true;
        }
        if (bindDecision == AutoWowOracleZoneTravelAssist::BindDecision::Wait)
        {
            if (exact && !inRange && !bot->IsInCombat())
                (void)MoveWorldObjectTo(exact->GetGUID());
            return teleporting ? true : ForceToWait(250);
        }
        assistState->pending = false;
        return BlockQuest(data, finisher ? QuestFailureReason::NoFinisherSpawn
                                         : QuestFailureReason::NoLiveCandidate, false);
    }

    auto tryZoneAssist = [&](RouteFailure failure) -> bool
    {
        bool const managed = AutoWowOracleRuntime::IsManagedBot(botGuid);
        bool const assistEnabled = AutoWowOracleRuntime::Runtime::instance().Config().zoneTravelAssist;
        if (!managed || !assistEnabled)
            return false;
        ZoneAssistState* state = ReserveZoneAssist(botGuid);
        uint32 sourceZone = 0;
        uint32 targetZone = 0;
        bool const sameMapAndInstance = bot->GetMap() &&
            bot->GetMapId() == stable.GetMapId() &&
            bot->GetMap()->GetInstanceId() == bot->GetInstanceId() &&
            intent.instanceId == bot->GetInstanceId();
        if (sameMapAndInstance)
        {
            sourceZone = bot->GetMap()->GetZoneId(bot->GetPhaseMask(),
                bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            targetZone = bot->GetMap()->GetZoneId(bot->GetPhaseMask(),
                stable.GetPositionX(), stable.GetPositionY(), stable.GetPositionZ());
        }
        AutoWowOracleZoneTravelAssist::Facts const facts{
            assistEnabled,
            managed && botAI->IsAutoWowIndependentParty() &&
                !botAI->IsRealPlayer() && !botAI->IsAutoWowPaused(),
            rt.oracleManaged && rt.oracleTaggedDispatch &&
                AutoWowOracleRuntime::HasActiveLease(botGuid, snapshot.decisionId),
            signedEntry != 0 && stable.GetRawValue() != 0,
            bot->IsInWorld() && bot->IsAlive(), bot->IsInCombat(),
            bot->IsInFlight() || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
                FLIGHT_MOTION_TYPE,
            bot->GetTransport() != nullptr || bot->GetVehicle() != nullptr,
            bot->IsBeingTeleported(),
            bot->isMoving() || (bot->movespline && !bot->movespline->Finalized()),
            sameMapAndInstance,
            sourceZone != 0 && sourceZone == targetZone,
            MapMgr::IsValidMapCoord(stable.GetMapId(), stable.GetPositionX(),
                stable.GetPositionY(), stable.GetPositionZ(), stable.GetOrientation()),
            bot->GetExactDist(stable.GetPositionX(), stable.GetPositionY(),
                stable.GetPositionZ()) > static_cast<float>(intent.radius) + 1.0f,
            state && (!state->used || (now >= state->lastAt &&
                now - state->lastAt >= AutoWowOracleZoneTravelAssist::kCooldownSeconds)),
            failure};
        if (!AutoWowOracleZoneTravelAssist::ShouldAssist(facts))
            return false;

        WorldPosition const exactDestination(stable);
        if (!bot->TeleportTo(exactDestination))
            return false;
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, data.questId,
                                     "zone_travel_assist", AutoWowQuestLedger::PhaseName(rt.phase));
        state->routeKey = routeKey;
        state->lastAt = now;
        state->used = true;
        state->pending = true;
        rt.oracleRouteActive = false;
        rt.oracleRouteReceiptAvailable = false;
        LOG_INFO("playerbots",
            "[AutoWow Oracle ZoneTravelAssist] bot_guid={} quest_id={} spawn={} "
            "map={} instance={} zone={} failure={} status=teleport_issued "
            "teleport_not_walk_proof=true",
            botGuid, data.questId, stable.GetRawValue(), bot->GetMapId(),
            bot->GetInstanceId(), sourceZone, static_cast<uint32>(failure));
        return true;
    };

    std::optional<AutoWowDungeonPath::ProbeResult> targetProbe = selectPreparedPath();
    AutoWowOracleQuestExecutor::QuestRouteReentryFacts reentryFacts = currentReentryFacts();
    double const estimatedSeconds = targetProbe
        ? std::max(0.0, static_cast<double>(MoveDelay(targetProbe->pathLength)))
        : 0.0;

    bool const sameRoute = rt.oracleRouteSession.state.startedAt != 0 &&
        rt.oracleRouteSession.state.routeKey == routeKey;
    if (!rt.oracleRouteActive)
    {
        AutoWowOracleQuestExecutor::QuestRouteRuntimeHandoff priorHandoff;
        bool const hasPriorHandoff = AutoWowOracleQuestExecutor::GetQuestRouteHandoff(
            botGuid, priorHandoff);
        AutoWowOracleQuestExecutor::QuestRouteReentrySignal const reentrySignal =
            hasPriorHandoff && priorHandoff.qualifyingReentrySignal !=
                    AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None
            ? priorHandoff.qualifyingReentrySignal
            : hasPriorHandoff
            ? AutoWowOracleQuestExecutor::EvaluateQuestRouteReentry(
                priorHandoff.blockedReentryFacts, reentryFacts)
            : AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None;
        bool const explicitReentry = hasPriorHandoff && priorHandoff.requiresWorldChange &&
            priorHandoff.questId == data.questId && reentrySignal !=
                AutoWowOracleQuestExecutor::QuestRouteReentrySignal::None;
        reentrySignalId = static_cast<WorldChangeId>(reentrySignal);
        AutoWowOracleQuestExecutor::QuestRoutePolicyStep started;
        if (sameRoute)
        {
            rt.oracleRouteSession.ledger = durableLedger;
            started = AutoWowOracleQuestExecutor::OracleQuestExecutor::ResetQuestRoute(
                true, true, enabled, rt.oracleRouteSession, now, explicitReentry,
                reentrySignalId);
        }
        else
        {
            AutoWowOracleQuestExecutor::QuestRouteRequest const request{
                true, true, enabled, intent, estimatedSeconds, now, durableLedger,
                explicitReentry, reentrySignalId};
            started = AutoWowOracleQuestExecutor::OracleQuestExecutor::StartQuestRoute(request);
        }
        if (!started.applicable)
            return false;

        rt.oracleRouteSession = started.route.session;
        rt.oracleRouteReceipt = started.route.result;
        rt.oracleRouteReceiptAvailable = true;
        rt.oracleRouteActive = !started.route.result.blocked && !started.route.result.arrived;
        rt.oracleRouteFinisher = finisher;
        rt.oracleRouteQuestId = data.questId;
        rt.oracleRouteSignedEntry = signedEntry;
        rt.oracleRouteStableSpawnGuid = stable.GetRawValue();
        rt.oracleRouteDecisionId = snapshot.decisionId;
        rt.oracleRouteStartedAt = now;
        rt.oracleRouteObservationInitialized = false;
        rt.oracleRouteIdentityPinned = false;
        rt.oracleRouteNativeInteractionObserved = false;
        durableLedger = started.route.session.ledger;
        if (!AutoWowOracleQuestExecutor::StoreQuestRouteLedger(botGuid, durableLedger))
        {
            started.route.result.failure = RouteFailure::HandoffCapacityExceeded;
            started.route.result.blocked = true;
            started.route.result.releaseLease = true;
            started.route.result.leaseReleaseAuthoritative = false;
            started.route.result.stage = RouteStage::Blocked;
            EmitOracleRouteReceipt(bot, data.questId, started.route.result);
            return BlockQuest(data, QuestFailureReason::OracleRouteBlocked, false);
        }
        if (started.route.result.blocked || started.route.result.arrived)
        {
            if (!AutoWowOracleQuestExecutor::PublishQuestRouteHandoff(
                    botGuid, started.route, reentryFacts))
            {
                started.route.result.failure = RouteFailure::HandoffCapacityExceeded;
                started.route.result.blocked = true;
                started.route.result.releaseLease = true;
                started.route.result.leaseReleaseAuthoritative = false;
                started.route.result.stage = RouteStage::Blocked;
            }
        }
        EmitOracleRouteReceipt(bot, data.questId, started.route.result);
        if (started.route.result.blocked)
        {
            QuestFailureReason const reason = started.route.result.failure == RouteFailure::UnsupportedTransition
                ? QuestFailureReason::OracleRouteUnsupportedTransition
                : QuestFailureReason::OracleRouteBlocked;
            return BlockQuest(data, reason, false);
        }
        return true;
    }

    if (rt.oracleRouteQuestId != data.questId || rt.oracleRouteFinisher != finisher ||
        rt.oracleRouteSignedEntry != signedEntry ||
        rt.oracleRouteStableSpawnGuid != stable.GetRawValue() ||
        rt.oracleRouteSession.state.ledgerKey != ledgerKey)
        return BlockQuest(data, QuestFailureReason::OracleRouteInvalidDescriptor, false);

    RouteObservation observation;
    std::optional<AutoWowDungeonPath::ProbeResult> preparedMove;
    RouteStage const stage = rt.oracleRouteSession.state.stage;
    observation.descriptorConfirmed = true;
    auto observeLoad = [&]()
    {
        observation.targetLoadKnown = bot->GetMap() && stable.GetMapId() == bot->GetMapId();
        observation.targetLoaded = observation.targetLoadKnown &&
            bot->GetMap()->IsGridLoaded(stable.GetPositionX(), stable.GetPositionY());
    };

    if (stage == RouteStage::SelectSafeAnchor)
    {
        std::vector<AutoWowDungeonPath::ProbeResult> const anchorPaths =
            SelectOracleRouteAnchorPaths(bot, destination);
        observation.anchorSearchComplete = true;
        observation.availableAnchorCount = static_cast<uint8>(anchorPaths.size());
        observeLoad();
        observation.hasGridState = observation.targetLoadKnown;
        observation.gridLoaded = observation.targetLoaded;
        auto attempted = [&](uint64 anchorId)
        {
            for (uint8 index = 0;
                 index < rt.oracleRouteSession.state.anchorAttemptCount &&
                 index < kMaxSuppliedAnchors; ++index)
                if (rt.oracleRouteSession.state.anchorAttempts[index] == anchorId)
                    return true;
            return false;
        };
        for (AutoWowDungeonPath::ProbeResult const& candidate : anchorPaths)
        {
            uint64 const anchorId = StableAnchorId(
                bot->GetMapId(), bot->GetInstanceId(), candidate);
            if (attempted(anchorId))
                continue;
            G3D::Vector3 const& endpoint = candidate.path.back();
            WorldPosition anchorPosition(bot->GetMapId(), endpoint.x, endpoint.y, endpoint.z);
            observation.safeAnchorSupplied = true;
            observation.safeAnchor.anchorId = anchorId;
            observation.safeAnchor.mapId = bot->GetMapId();
            observation.safeAnchor.instanceId = bot->GetInstanceId();
            observation.safeAnchor.areaId = anchorPosition.getAreaId();
            observation.safeAnchor.coordinate = {
                endpoint.x, endpoint.y, endpoint.z, bot->GetOrientation()};
            observation.safeAnchor.callerSupplied = true;
            observation.safeAnchor.safe = true;
            break;
        }
    }
    else if (stage == RouteStage::GroundPlan)
    {
        SafeAnchor const& anchor = rt.oracleRouteSession.state.selectedAnchor;
        AutoWowDungeonPath::ProbeResult const probe = AutoWowDungeonPath::Probe(
            bot, static_cast<float>(anchor.coordinate.x),
            static_cast<float>(anchor.coordinate.y), static_cast<float>(anchor.coordinate.z));
        if (probe.safe && probe.path.size() >= 2)
        {
            preparedMove = probe;
            targetProbe = selectPreparedPath();
            observation.groundPlanSupplied = true;
            observation.groundPlan.routeKey = rt.oracleRouteSession.state.routeKey;
            observation.groundPlan.anchorId = anchor.anchorId;
            G3D::Vector3 const& endpoint = probe.path.back();
            observation.groundPlan.nextPoint = {
                endpoint.x, endpoint.y, endpoint.z, bot->GetOrientation()};
            observation.groundPlan.estimatedRemainingDistance = targetProbe
                ? targetProbe->pathLength : probe.pathLength;
            observation.groundPlan.callerSupplied = true;
            observation.groundPlan.valid = true;
            observation.groundPlan.nextPointSupplied = true;
        }
    }
    else if (stage == RouteStage::Reprobe)
    {
        observation.reprobeComplete = !bot->isMoving();
        if (observation.reprobeComplete)
        {
            targetProbe = selectPreparedPath();
            if (targetProbe && std::isfinite(targetProbe->pathLength))
            {
                observation.hasRemainingDistance = true;
                observation.remainingDistance = targetProbe->pathLength;
            }
            observation.hasSegmentIndex = true;
            observation.segmentIndex = rt.oracleRouteSession.state.anchorAttemptCount;
            observation.hasMapArea = true;
            observation.currentMapId = bot->GetMapId();
            observation.currentAreaId = bot->GetAreaId();
            observeLoad();
            observation.hasGridState = observation.targetLoadKnown;
            observation.gridLoaded = observation.targetLoaded;
            uint32 const objectiveCount = objective && objectiveIdx >= 0
                ? static_cast<uint32>(std::max(0, ObjectiveCurrentCount(data.questId, objectiveIdx)))
                : rt.oracleRouteLastObjectiveCount;
            uint8 const questStatus = static_cast<uint8>(bot->GetQuestStatus(data.questId));
            uint32 const inventoryCount = objective && objective->requiredItemId != 0
                ? bot->GetItemCount(objective->requiredItemId)
                : rt.oracleRouteLastInventoryCount;
            uint64 const interactionSequence = rt.finisherReceipt.sequence;
            if (rt.oracleRouteObservationInitialized)
            {
                observation.objectiveDelta = objectiveCount != rt.oracleRouteLastObjectiveCount;
                observation.questDelta = questStatus != rt.oracleRouteLastQuestStatus;
                observation.inventoryDelta = inventoryCount != rt.oracleRouteLastInventoryCount;
                observation.interactionDelta =
                    interactionSequence != rt.oracleRouteLastInteractionSequence;
            }
            rt.oracleRouteObservationInitialized = true;
            rt.oracleRouteLastObjectiveCount = objectiveCount;
            rt.oracleRouteLastQuestStatus = questStatus;
            rt.oracleRouteLastInventoryCount = inventoryCount;
            rt.oracleRouteLastInteractionSequence = interactionSequence;
        }
    }
    else if (stage == RouteStage::ExactLiveBind || stage == RouteStage::Approach ||
             stage == RouteStage::Verify)
    {
        observeLoad();
        WorldObject* exact = observation.targetLoaded
            ? BindExactOracleRouteTarget(rt, finisher) : nullptr;
        if (stage == RouteStage::ExactLiveBind)
            observation.exactBindComplete = true;
        if (exact)
        {
            observation.hasLiveIdentity = true;
            observation.liveIdentity = rt.oracleRouteSession.state.routeKey;
            bool const within = finisher ? IsWithinInteractionDist(exact)
                : bot->GetDistance(exact) <= static_cast<float>(intent.radius);
            if (stage == RouteStage::Approach)
                observation.approachConfirmed = within;
            if (stage == RouteStage::Verify)
            {
                observation.withinRadius = within;
                observation.verificationPassed = !finisher || exact->hasInvolvedQuest(data.questId);
            }
        }
    }

    AutoWowOracleQuestExecutor::QuestRoutePolicyStep advanced =
        AutoWowOracleQuestExecutor::OracleQuestExecutor::AdvanceQuestRoute(
            true, true, enabled, rt.oracleRouteSession, observation, now);
    if (!advanced.applicable)
        return false;

    // A recoverable route failure is the first assist point. Do this before persisting its
    // blocked ledger or publishing a grounded-route receipt: the teleport is separate evidence.
    if (advanced.route.result.blocked && tryZoneAssist(advanced.route.result.failure))
        return true;

    rt.oracleRouteSession = advanced.route.session;
    rt.oracleRouteReceipt = advanced.route.result;
    rt.oracleRouteReceiptAvailable = true;
    durableLedger = advanced.route.session.ledger;
    if (!AutoWowOracleQuestExecutor::StoreQuestRouteLedger(botGuid, durableLedger))
    {
        advanced.route.result.failure = RouteFailure::HandoffCapacityExceeded;
        advanced.route.result.ledgerKey = MakeRouteLedgerKey(
            advanced.route.result.routeKey, advanced.route.result.failure);
        advanced.route.result.blocked = true;
        advanced.route.result.arrived = false;
        advanced.route.result.releaseLease = true;
        advanced.route.result.leaseReleaseAuthoritative = false;
        advanced.route.result.stage = RouteStage::Blocked;
        rt.oracleRouteActive = false;
        EmitOracleRouteReceipt(bot, data.questId, advanced.route.result);
        return BlockQuest(data, QuestFailureReason::OracleRouteBlocked, false);
    }

    AutoWowQuestFinisher::RouteNativeAction const nativeAction =
        AutoWowQuestFinisher::SelectRouteNativeAction(advanced.route.result);
    if (nativeAction == AutoWowQuestFinisher::RouteNativeAction::WalkPreparedPath && preparedMove)
    {
        AutoWowDungeonWalkAction walk(botAI);
        AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason rejectReason;
        (void)walk.WalkPrepared(*preparedMove, &rejectReason);
    }
    else if (nativeAction == AutoWowQuestFinisher::RouteNativeAction::ApproachExactTarget)
    {
        if (WorldObject* exact = BindExactOracleRouteTarget(rt, finisher))
            (void)MoveWorldObjectTo(exact->GetGUID());
    }
    else if (nativeAction == AutoWowQuestFinisher::RouteNativeAction::ReresolveExactTarget)
        (void)BindExactOracleRouteTarget(rt, finisher);

    // SelectSafeAnchor/GroundPlan cannot invoke Recover without an observation.  Bound that caller
    // wait with the same exact ledger key and policy delay schedule, without inventing an anchor.
    RouteStage const boundedStage = advanced.route.session.state.stage;
    bool const callerBoundedStage = boundedStage == RouteStage::Descriptor ||
        boundedStage == RouteStage::SelectSafeAnchor || boundedStage == RouteStage::GroundPlan;
    if (callerBoundedStage && !advanced.route.result.arrived && !advanced.route.result.blocked &&
        now >= rt.oracleRouteStartedAt && now - rt.oracleRouteStartedAt >= kBlockAfterSeconds)
    {
        if (tryZoneAssist(RouteFailure::Stalled))
            return true;
        LedgerUpdate const update = RecordBlocked(
            durableLedger, ledgerKey, now, RouteFailure::Stalled);
        durableLedger = update.ledger;
        RouteResult bounded = advanced.route.result;
        bounded.routeKey = rt.oracleRouteSession.state.routeKey;
        bounded.ledgerKey = update.ledgerKey;
        bounded.stage = RouteStage::Blocked;
        bounded.failure = RouteFailure::Stalled;
        bounded.command.kind = RouteCommandKind::Yield;
        bounded.command.routeKey = bounded.routeKey;
        bounded.nextRetryAt = update.nextRetryAt;
        bounded.releaseLease = true;
        bounded.leaseReleaseAuthoritative = false;
        bounded.blocked = true;
        bounded.arrived = false;
        rt.oracleRouteSession.state.stage = RouteStage::Blocked;
        rt.oracleRouteSession.state.failure = RouteFailure::Stalled;
        rt.oracleRouteSession.state.releaseLease = true;
        rt.oracleRouteSession.ledger = durableLedger;
        rt.oracleRouteReceipt = bounded;
        advanced.route.result = bounded;
        advanced.route.session = rt.oracleRouteSession;
        if (!AutoWowOracleQuestExecutor::StoreQuestRouteLedger(botGuid, durableLedger))
        {
            bounded.failure = RouteFailure::HandoffCapacityExceeded;
            bounded.ledgerKey = MakeRouteLedgerKey(bounded.routeKey, bounded.failure);
            advanced.route.result = bounded;
        }
    }

    if (advanced.route.result.blocked || advanced.route.result.arrived)
    {
        if (!AutoWowOracleQuestExecutor::PublishQuestRouteHandoff(
                botGuid, advanced.route, reentryFacts))
        {
            advanced.route.result.failure = RouteFailure::HandoffCapacityExceeded;
            advanced.route.result.ledgerKey = MakeRouteLedgerKey(
                advanced.route.result.routeKey, advanced.route.result.failure);
            advanced.route.result.blocked = true;
            advanced.route.result.arrived = false;
            advanced.route.result.releaseLease = true;
            advanced.route.result.leaseReleaseAuthoritative = false;
            advanced.route.result.stage = RouteStage::Blocked;
        }
    }
    EmitOracleRouteReceipt(bot, data.questId, advanced.route.result);
    if (advanced.route.result.blocked ||
        nativeAction == AutoWowQuestFinisher::RouteNativeAction::UnsupportedTransition)
    {
        rt.oracleRouteActive = false;
        QuestFailureReason const reason = advanced.route.result.failure == RouteFailure::UnsupportedTransition
            ? QuestFailureReason::OracleRouteUnsupportedTransition
            : advanced.route.result.failure == RouteFailure::SafeAnchorRequired
                ? QuestFailureReason::OracleRouteNoSafeAnchor
                : QuestFailureReason::OracleRouteBlocked;
        return BlockQuest(data, reason, false);
    }

    if (advanced.route.result.arrived)
    {
        enterExactTargetPhase();
    }
    return true;
}

/* ------------------------------------------------------------------------- *
 *  Objective execution (INCOMPLETE quest)
 *
 *  Phase pipeline:
 *    ResolveObjective -> TravelToSource -> AcquireTarget -> EngageTarget
 *                     -> [InteractSource/LootSource] -> VerifyProgress
 *                     -> (loop / next objective)
 *  Targeting and combat are delegated to the grind/target selection and combat
 *  strategies; this driver only positions the bot, records the objective
 *  identity/baseline into objectiveRuntime, and gates progress by DELTA.
 * ------------------------------------------------------------------------- */
bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest ? data.quest : sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    QuestObjectiveRuntime& rt = data.objectiveRuntime;

    // Normalize a stale phase left over from the finisher side or a prior quest.
    switch (rt.phase)
    {
        case QuestActionPhase::ResolveFinisher:
        case QuestActionPhase::TravelToFinisher:
        case QuestActionPhase::InteractFinisher:
        case QuestActionPhase::VerifyReward:
        case QuestActionPhase::Complete:
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            break;
        default:
            break;
    }

    // A prior tick raised a typed blocker: hold for the Director.
    if (rt.phase == QuestActionPhase::Blocked)
    {
        if (sPlayerbotAIConfig.autoWowQuestBlockedDefer && DeferBlockedQuest(data))
            return true;
        return ForceToWait(3000);
    }

    bool const objectiveWorkWindow =
        rt.phase == QuestActionPhase::InteractSource ||
        rt.phase == QuestActionPhase::UseQuestItem ||
        rt.phase == QuestActionPhase::EscortEvent ||
        rt.phase == QuestActionPhase::LootSource ||
        rt.phase == QuestActionPhase::VerifyProgress;
    if (MaintainQuestPartyCohesion(data, objectiveWorkWindow))
        return true;

    // The "active quest objective" value publishes the authoritative, still-
    // incomplete objective for this bot (per-bot AI value). Its key is the
    // canonical identity (questId, family, slot); the flattened POI index is
    // derived locally only to feed the count helpers / POI machinery.
    QuestObjectiveSpec spec = AI_VALUE(QuestObjectiveSpec, "active quest objective");
    if (!spec.hasLock())
    {
        // A quest can still be persisted as INCOMPLETE after every counted objective is satisfied.
        // In that state ActiveQuestObjective is intentionally empty, so do not classify it as an
        // unsupported objective: ask the exact finisher path to submit the normal core completion
        // request. The finisher Value is relation-resolved and remains tied to this DoQuest quest id.
        QuestFinisherRef const finisher = AI_VALUE(QuestFinisherRef, "active quest finisher");
        AutoWowQuestFinisher::Facts const finisherFacts{
            bot->GetQuestStatus(questId),
            bot->GetQuestRewardStatus(questId),
            bot->CanCompleteQuest(questId),
            finisher.signedEntry != 0};
        if (AutoWowQuestFinisher::IsFinisherReady(finisherFacts))
            return DoCompletedQuest(data);

        // No supported, runnable objective and no core-authorized finisher transition (unsupported
        // objective, incomplete objective, or missing finisher relation).
        return BlockQuest(data, QuestFailureReason::UnsupportedObjective, /*unsupported*/ true);
    }
    // A full inventory cannot accept a newly dropped quest item. Pause source acquisition while
    // alive and out of combat, then sell only the existing policy's safe gray items at a real
    // vendor. Keep the core loot/storage path authoritative when the objective resumes.
    if (QuestInventoryReliefPolicy::ShouldRelieveIncompleteItem(
            questId, spec, AI_VALUE(uint8, "bag space"), bot->IsAlive(), bot->IsInCombat()))
    {
        rt.failure = QuestFailureReason::InventoryFull;
        if (sPlayerbotAIConfig.autoWowQuestFullBagRelief)
            return RelieveFullBagsForQuest(data);
        if (TryRelieveInventoryAtVendor(data, /*incompleteItem*/ true))
            return true;
        return BlockQuest(data, QuestFailureReason::InventoryFull, /*unsupported*/ false);
    }
    const int32 objectiveIdx = (spec.key.family == QuestObjectiveFamily::Item)
                                   ? static_cast<int32>(QUEST_OBJECTIVES_COUNT) + spec.key.slot
                                   : spec.key.slot;
    const bool itemObjective = spec.key.family == QuestObjectiveFamily::Item;
    bool const objectiveChanged = !rt.sourceRotationObjectiveKnown ||
        rt.sourceRotationQuestId != spec.key.questId ||
        rt.sourceRotationSlot != spec.key.slot ||
        rt.sourceRotationFamily != spec.key.family;
    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(rt.lastProgressTimeMs,
        objectiveChanged, !objectiveChanged &&
            static_cast<uint32>(std::max<int32>(0, ObjectiveCurrentCount(questId, objectiveIdx))) >
                rt.lastObservedCount);
    auto sourceFailureBudgetExpired = [&]()
    {
        return QuestSourceStallPolicy::RecordUnavailableSource(rt.lastProgressTimeMs,
            getMSTime(), QuestSourceStallPolicy::kUnavailableSourceBudgetMs);
    };

    // A resolver upgrade can turn an already-published CollectItem shape into UseQuestItem while
    // its runtime still carries an old target/phase. Re-resolve first so a newly discovered focus GO
    // is selected and traveled to; never use a stale quest POI or creature selection as the focus.
    if (spec.kind == QuestObjectiveKind::UseQuestItem)
    {
        bool const selectedSourceIsValid =
            rt.selectedSourceEntry > 0
                ? spec.acceptsCreatureEntry(static_cast<uint32>(rt.selectedSourceEntry))
                : rt.selectedSourceEntry < 0
                      ? spec.acceptsGameObjectEntry(static_cast<uint32>(-rt.selectedSourceEntry))
                      : false;
        bool const stalePhase =
            rt.phase == QuestActionPhase::AcquireTarget || rt.phase == QuestActionPhase::SelfDefense ||
            rt.phase == QuestActionPhase::EngageTarget || rt.phase == QuestActionPhase::LootSource ||
            rt.phase == QuestActionPhase::InteractSource;
        if (stalePhase || (rt.phase == QuestActionPhase::UseQuestItem && !selectedSourceIsValid))
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
    }

    switch (rt.phase)
    {
        case QuestActionPhase::ResolveObjective:
        {
            // Clear the previous live-source identity before resolving. The
            // resolver/policy chooses the nearest legal same-map spawn; seeding
            // it with the first vector element would make the initial selection
            // look like a rotation away from the actual objective.
            rt.selectedSourceEntry = 0;
            rt.selectedSourceSpawn = GuidPosition();
            if (spec.sources.empty())
            {
                // Fall back to the objective's signed creature/GO entry.
                rt.selectedSourceEntry = spec.requiredNpcOrGoEntry;
            }
            rt.selectedTargetGuid = ObjectGuid();

            // Delta baseline: whatever is already on the counter does NOT count as
            // progress this attempt.
            rt.baselineCount = ObjectiveCurrentCount(questId, objectiveIdx);
            if (objectiveChanged || rt.baselineCount > rt.lastObservedCount)
                rt.lastObservedCount = rt.baselineCount;
            rt.failure = QuestFailureReason::None;
            rt.selectedRewardIndex = 0;
            rt.selectedRewardIndexKnown = false;

            WorldPosition pos;
            if (!ResolveSourceTravelPos(spec, questId, objectiveIdx, rt, pos))
            {
                if (!rt.exhaustedSourceSpawns.empty())
                {
                    if (sourceFailureBudgetExpired())
                        return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
                    EnterQuestPhase(data, QuestActionPhase::WaitForRespawn);
                    return ForceToWait(5000);
                }
                return BlockQuest(data, QuestFailureReason::NoSourceSpawn, /*unsupported*/ false);
            }

            data.pos = pos;
            EnterQuestPhase(data, QuestActionPhase::TravelToSource);
            return true;
        }

        case QuestActionPhase::TravelToSource:
        {
            if (rt.oracleManaged && rt.oracleTaggedDispatch && OracleRouteV2Enabled())
                return DriveOracleQuestRoute(data, &spec, objectiveIdx, /*finisher*/ false);

            if (data.pos == WorldPosition())
            {
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }
            if (bot->GetDistance(data.pos) <= questTravelArriveDist)
            {
                if (spec.kind == QuestObjectiveKind::UseQuestItem)
                    EnterQuestPhase(data, QuestActionPhase::UseQuestItem);
                else if (spec.kind == QuestObjectiveKind::ScriptedEvent)
                    EnterQuestPhase(data, QuestActionPhase::EscortEvent);
                else if (rt.selectedSourceEntry < 0)
                    EnterQuestPhase(data, itemObjective ? QuestActionPhase::LootSource
                                                       : QuestActionPhase::InteractSource);
                else
                    EnterQuestPhase(data, QuestActionPhase::AcquireTarget);
                return true;
            }
            if (sPlayerbotAIConfig.autoWowQuestTravelProgressWatch && !rt.oracleManaged &&
                TravelProgressExpired(rt, data.pos, bot->GetDistance(data.pos)))
                return ExpireUnreachableSource(data, spec);
            bool stuck = false;
            if (MoveFarTo(data.pos, /*questNoTeleport*/ true, &stuck))
            {
                if (stuck)
                    return BlockQuest(data, TravelStuckReason(), /*unsupported*/ false);
                return true;
            }
            // Sampler couldn't land a candidate this tick — small nudge so the
            // next tick retries from a different vantage (still walk-only).
            return MoveRandomNear(10.0f);
        }

        case QuestActionPhase::AcquireTarget:
        {
            // Delegate target selection + self-defense to the grind/combat
            // strategies. We only watch for progress and for engagement.
            if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }
            auto rotateUnavailableSource = [&]() -> bool
            {
                if (sourceFailureBudgetExpired())
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
                uint64 const sourceSpawnId = rt.selectedSourceSpawn.GetRawValue();
                if (sourceSpawnId != 0)
                {
                    rt.exhaustedSourceSpawns.insert(sourceSpawnId);
                    rt.sourceRotationCooldownUntil[sourceSpawnId] =
                        getMSTime() + questAcquireBudgetMs / 2;
                    ++rt.sourceRotationCount;
                }
                if (rt.sourceRotationCount >= maxSourceRotations ||
                    (++rt.attemptCount >= maxObjectiveAttempts && sourceSpawnId == 0))
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            };
            if (rt.oracleRouteIdentityPinned)
            {
                // This timer and the durable unavailable-source clock run through failed binds,
                // rejected attacks and movement attempts. Neither a fresh route nor a moving
                // spline is objective credit.
                if (!data.lastReachPOI)
                    data.lastReachPOI = getMSTime();
                else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questAcquireBudgetMs)
                    return rotateUnavailableSource();

                WorldObject* exact = BindExactOracleRouteTarget(rt, false);
                Creature* creature = exact ? exact->ToCreature() : nullptr;
                if (!creature)
                {
                    if (ShouldLogExactAttackRejection(bot->GetGUID().GetCounter(),
                            rt.selectedSourceSpawn.GetRawValue(),
                            ExactQuestAttackRecoveryPolicy::Rejection::TargetUnavailable,
                            getMSTime()))
                    {
                        LOG_DEBUG("playerbots",
                            "[New RPG] exact quest attack rejected bot={} quest={} spawn={} "
                            "reason=exact_bind_unavailable map={} instance={}",
                            bot->GetGUID().GetCounter(), questId,
                            rt.selectedSourceSpawn.GetCounter(), bot->GetMapId(),
                            bot->GetInstanceId());
                    }
                    if (sourceFailureBudgetExpired())
                        return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
                    EnterQuestPhase(data, QuestActionPhase::TravelToSource);
                    return true;
                }
                context->GetValue<GuidVector>("prioritized targets")->Set({creature->GetGUID()});
                ExactQuestDefenseAttackAction attack(botAI);
                if (attack.AttackExact(creature))
                {
                    rt.oracleRouteNativeInteractionObserved = true;
                    AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                        bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                    EnterQuestPhase(data, QuestActionPhase::EngageTarget);
                    return true;
                }

                RaidTargetClaim const& claim = AI_VALUE(RaidTargetClaim&, "raid target claim");
                bool raidClaimAllows = true;
                if (claim.target && !RaidTargetClaimPolicy::IsExpired(getMSTime(), claim.expiresAtMs))
                {
                    Unit* claimed = botAI->GetUnit(claim.target);
                    raidClaimAllows = !claimed || !claimed->IsAlive() || !claimed->IsInWorld() ||
                        claimed == creature;
                }
                ExactQuestAttackRecoveryPolicy::Facts const attackFacts{
                    spec.acceptsCreatureEntry(creature->GetEntry()) &&
                        creature->GetSpawnId() == rt.selectedSourceSpawn.GetCounter() &&
                        creature->GetMapId() == bot->GetMapId() &&
                        creature->GetInstanceId() == bot->GetInstanceId(),
                    creature->IsInWorld() && creature->IsAlive(),
                    bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
                        bot->HasUnitState(UNIT_STATE_IN_FLIGHT),
                    raidClaimAllows,
                    bot->IsFriendlyTo(creature),
                    bot->IsValidAttackTarget(creature),
                    !botAI->IsInVehicle() || botAI->IsInVehicle(false, false, true),
                    DungeonPullReadiness::IsReady(
                        botAI, bot, creature, "autowow scripted-event defense"),
                    bot->IsInCombat() && bot->GetVictim() == creature,
                    bot->IsWithinLOSInMap(creature)};
                auto const rejection = ExactQuestAttackRecoveryPolicy::Classify(attackFacts);
                if (rejection == ExactQuestAttackRecoveryPolicy::Rejection::AlreadyEngaged)
                {
                    EnterQuestPhase(data, QuestActionPhase::EngageTarget);
                    return true;
                }
                bool const failureBudgetExpired = sourceFailureBudgetExpired();
                bool approachIssued = false;
                if (!failureBudgetExpired && ExactQuestAttackRecoveryPolicy::ShouldApproach(rejection) &&
                    !bot->isMoving() && !bot->IsBeingTeleported())
                    approachIssued = MoveWorldObjectTo(creature->GetGUID());
                if (ShouldLogExactAttackRejection(bot->GetGUID().GetCounter(),
                        rt.selectedSourceSpawn.GetRawValue(), rejection, getMSTime()))
                {
                    LOG_DEBUG("playerbots",
                        "[New RPG] exact quest attack rejected bot={} quest={} spawn={} "
                        "runtime_guid={} reason={} distance={} los={} attackable={} friendly={} "
                        "raid_claim_allows={} dungeon_ready={} flight={} vehicle_allows={} "
                        "moving={} approach_issued={} failure_budget_expired={}",
                        bot->GetGUID().GetCounter(), questId,
                        rt.selectedSourceSpawn.GetCounter(), creature->GetGUID().GetCounter(),
                        ExactAttackRejectionName(rejection), bot->GetDistance(creature),
                        attackFacts.lineOfSight, attackFacts.validAttackTarget,
                        attackFacts.friendly, attackFacts.raidClaimAllows,
                        attackFacts.dungeonPullReady, attackFacts.flight,
                        attackFacts.vehicleAllows, bot->isMoving(), approachIssued,
                        failureBudgetExpired);
                }
                if (failureBudgetExpired)
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
                if (approachIssued)
                    return true;
                return ForceToWait(250);
            }
            if (!rt.selectedTargetGuid.IsEmpty())
            {
                // A low-level party can kill its selected source between two
                // New-RPG ticks. Preserve the runtime GUID in the targeter and
                // recognize the corpse here so that fast kills still enter the
                // objective-locked loot pipeline instead of silently returning
                // to acquisition with 0 objective credit.
                if (Unit* selected = ObjectAccessor::GetUnit(*bot, rt.selectedTargetGuid);
                    selected && !selected->IsAlive())
                {
                    EnterQuestPhase(data, QuestActionPhase::LootSource);
                    return true;
                }
            }
            if (bot->IsInCombat() || AI_VALUE(Unit*, "current target"))
            {
                EnterQuestPhase(data, QuestActionPhase::EngageTarget);
                return true;
            }
            if (!data.lastReachPOI)
                data.lastReachPOI = getMSTime();
            else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questAcquireBudgetMs)
                return rotateUnavailableSource();
            // Look around while the grind strategy engages nearby mobs.
            return MoveRandomNear(8.0f);
        }

        case QuestActionPhase::SelfDefense:
        case QuestActionPhase::EngageTarget:
        {
            if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }
            bool engaged = bot->IsInCombat() || AI_VALUE(Unit*, "current target");
            if (!engaged)
            {
                // Combat resolved without a counter change. For creature-item
                // quests the drop is looted off the corpse, so give looting a
                // moment; otherwise go re-acquire.
                EnterQuestPhase(data, itemObjective ? QuestActionPhase::LootSource : QuestActionPhase::AcquireTarget);
                return true;
            }
            if (!data.lastReachPOI)
                data.lastReachPOI = getMSTime();
            else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questSourceBudgetMs)
            {
                // Fighting for a long time with no counter movement.
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }
            return true;  // busy fighting; combat strategy owns the action
        }

        case QuestActionPhase::InteractSource:
        {
            int32 const current = ObjectiveCurrentCount(questId, objectiveIdx);
            if (current > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }

            if (!BindSourceGameObject(rt, 2.0f * sPlayerbotAIConfig.sightDistance))
            {
                if (bot->GetDistance(data.pos) > questTravelArriveDist)
                {
                    EnterQuestPhase(data, QuestActionPhase::TravelToSource);
                    return true;
                }
                if (++rt.attemptCount >= maxObjectiveAttempts)
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, /*unsupported*/ false);
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }

            GameObject* go = ObjectAccessor::GetGameObject(*bot, rt.selectedTargetGuid);
            if (!go || !go->IsInWorld() || !go->isSpawned())
            {
                rt.selectedTargetGuid.Clear();
                EnterQuestPhase(data, QuestActionPhase::WaitForRespawn);
                return true;
            }

            if (bot->GetDistance(go) > INTERACTION_DISTANCE - 1.0f)
            {
                if (MoveWorldObjectTo(go->GetGUID()))
                    return true;
                if (rt.oracleRouteIdentityPinned)
                    return ForceToWait(250);
                return MoveRandomNear(4.0f);
            }

            if (!data.lastReachPOI)
            {
                if (bot->isMoving())
                    bot->StopMoving();
                bot->SetFacingToObject(go);

                // Use the same client opcode path as normal Playerbots GO
                // interactions. The core remains authoritative for range,
                // flags, scripts, and whether quest credit is awarded.
                uint32 const before = static_cast<uint32>(
                    std::max<int32>(0, ObjectiveCurrentCount(questId, objectiveIdx)));
                WorldPacket use(CMSG_GAMEOBJ_USE, 8);
                use << go->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(use);
                if (rt.oracleRouteIdentityPinned)
                {
                    rt.oracleRouteNativeInteractionObserved = true;
                    AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                        bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                }
                uint32 const after = static_cast<uint32>(
                    std::max<int32>(0, ObjectiveCurrentCount(questId, objectiveIdx)));
                DirectGameObjectReceipt const& receipt = botAI->rpgInfo.RecordDirectGameObjectReceipt(
                    questId, spec.key.slot, go->GetEntry(), go->GetGUID(), before, after);

                uint32 const receiptTime = getMSTime();
                if (RebaseAfterDirectGameObjectCredit(rt, receipt, receiptTime))
                {
                    QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(
                        rt.lastProgressTimeMs, false, true);
                    // The opcode handler applies GO credit synchronously. Drop cached views now so
                    // the next tick/telemetry read observes the next objective or completed-directive
                    // finisher instead of briefly reselecting the credited GO.
                    context->GetValue<QuestObjectiveSpec>("active quest objective")->Reset();
                    context->GetValue<QuestFinisherRef>("active quest finisher")->Reset();
                    data.lastReachPOI = 0;
                    return true;
                }

                data.lastReachPOI = receiptTime;
                return true;
            }

            if (GetMSTimeDiffToNow(data.lastReachPOI) < questInteractionVerifyMs)
                return ForceToWait(250);

            if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }

            rt.selectedTargetGuid.Clear();
            if (++rt.attemptCount >= maxObjectiveAttempts)
                return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            return true;
        }

        case QuestActionPhase::UseQuestItem:
        {
            int32 const current = ObjectiveCurrentCount(questId, objectiveIdx);
            if (current > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }

            if (bot->IsInCombat())
                return ForceToWait(500);  // self-defense owns combat; never cast through it

            if (spec.sources.empty() || rt.selectedSourceEntry == 0)
                return BlockQuest(data, QuestFailureReason::NoSourceSpawn, /*unsupported*/ false);

            if (rt.selectedSourceEntry < 0)
            {
                // A spell focus is an environmental cast prerequisite, not the spell target. Bind
                // the exact resolved GO only for movement, then submit the canonical targetless
                // item-use packet nearby. Core CheckSpellFocus remains authoritative for focus ID,
                // spawn state, and range; the spell effect remains authoritative for item creation.
                if (!BindSourceGameObject(rt, 2.0f * sPlayerbotAIConfig.sightDistance))
                {
                    if (++rt.attemptCount >= maxObjectiveAttempts)
                        return BlockQuest(data, QuestFailureReason::NoLiveCandidate, /*unsupported*/ false);
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                    return true;
                }

                GameObject* focus = ObjectAccessor::GetGameObject(*bot, rt.selectedTargetGuid);
                if (!focus || !focus->IsInWorld() || !focus->isSpawned() ||
                    !spec.acceptsGameObjectEntry(focus->GetEntry()))
                {
                    rt.selectedTargetGuid.Clear();
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                    return true;
                }

                if (bot->GetDistance(focus) > CONTACT_DISTANCE)
                {
                    if (MoveWorldObjectTo(focus->GetGUID(), CONTACT_DISTANCE))
                        return true;
                    if (rt.oracleRouteIdentityPinned)
                        return ForceToWait(250);
                    return MoveRandomNear(4.0f);
                }

                Item* questItem = spec.questItemId ? bot->GetItemByEntry(spec.questItemId) : nullptr;
                if (!questItem)
                    return BlockQuest(data, QuestFailureReason::NoItemSource, /*unsupported*/ false);

                if (data.lastReachPOI && GetMSTimeDiffToNow(data.lastReachPOI) >= questSourceBudgetMs)
                    return BlockQuest(data, QuestFailureReason::ProgressDidNotChange, /*unsupported*/ false);

                if (!data.lastReachPOI)
                {
                    if (bot->isMoving())
                        bot->StopMoving();
                    bot->SetFacingToObject(focus);

                    TargetlessQuestItemUseAction use(botAI);
                    if (!use.UseItemAuto(questItem))
                    {
                        if (++rt.attemptCount >= maxObjectiveAttempts)
                            return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
                        EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                        return true;
                    }

                    botAI->RecordAutoWowQuestItemUsePacket();
                    if (rt.oracleRouteIdentityPinned)
                    {
                        rt.oracleRouteNativeInteractionObserved = true;
                        AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                            bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                    }
                    data.lastReachPOI = getMSTime();
                    return true;
                }

                if (GetMSTimeDiffToNow(data.lastReachPOI) < questInteractionVerifyMs)
                    return ForceToWait(250);

                if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
                {
                    EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                    return true;
                }

                if (++rt.attemptCount >= maxObjectiveAttempts)
                    return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }

            if (!BindQuestItemTarget(spec, rt, 2.0f * sPlayerbotAIConfig.sightDistance))
            {
                if (++rt.attemptCount >= maxObjectiveAttempts)
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, /*unsupported*/ false);
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }

            Creature* target = ObjectAccessor::GetCreature(*bot, rt.selectedTargetGuid);
            if (!target || !target->IsInWorld() || !target->IsAlive() ||
                !spec.acceptsCreatureEntry(target->GetEntry()))
            {
                rt.selectedTargetGuid.Clear();
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }

            if (bot->GetDistance(target) > INTERACTION_DISTANCE - 1.0f)
            {
                if (MoveWorldObjectTo(target->GetGUID()))
                    return true;
                if (rt.oracleRouteIdentityPinned)
                    return ForceToWait(250);
                return MoveRandomNear(4.0f);
            }

            Item* questItem = spec.questItemId ? bot->GetItemByEntry(spec.questItemId) : nullptr;
            if (!questItem)
                return BlockQuest(data, QuestFailureReason::NoItemSource, /*unsupported*/ false);

            if (!data.lastReachPOI)
            {
                if (bot->isMoving())
                    bot->StopMoving();
                bot->SetFacingToObject(target);

                UseItemAction use(botAI, "autowow exact quest item");
                if (!use.UseItemOnUnit(questItem, target))
                {
                    rt.targetCooldownUntil[target->GetGUID()] = getMSTime() + 15000;
                    rt.selectedTargetGuid.Clear();
                    if (++rt.attemptCount >= maxObjectiveAttempts)
                        return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                    return true;
                }

                botAI->RecordAutoWowQuestItemUsePacket();
                if (rt.oracleRouteIdentityPinned)
                {
                    rt.oracleRouteNativeInteractionObserved = true;
                    AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                        bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                }
                // A successfully submitted interaction should not select the
                // same peon again while the script resets/moves it.
                rt.targetCooldownUntil[target->GetGUID()] = getMSTime() + 180000;
                data.lastReachPOI = getMSTime();
                return true;
            }

            if (GetMSTimeDiffToNow(data.lastReachPOI) < questInteractionVerifyMs)
                return ForceToWait(250);

            if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }

            rt.selectedTargetGuid.Clear();
            if (++rt.attemptCount >= maxObjectiveAttempts)
                return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            return true;
        }

        case QuestActionPhase::EscortEvent:
        {
            // Completion/failure is owned entirely by the native quest script.
            // This phase only follows the exact scripted starter and defends it
            // from real attackers; it never writes an objective counter.
            if (!BindScriptedEventTarget(spec, rt, 4.0f * sPlayerbotAIConfig.sightDistance))
            {
                if (!data.lastReachPOI)
                    data.lastReachPOI = getMSTime();
                else if (GetMSTimeDiffToNow(data.lastReachPOI) >= 30000)
                    return BlockQuest(data, QuestFailureReason::NoLiveCandidate, /*unsupported*/ false);
                return ForceToWait(500);
            }

            Creature* escort = ObjectAccessor::GetCreature(*bot, rt.selectedTargetGuid);
            if (!escort || !escort->IsInWorld())
            {
                rt.selectedTargetGuid.Clear();
                return ForceToWait(500);
            }
            if (!escort->IsAlive())
                return BlockQuest(data, QuestFailureReason::EventActorDeadOrUnavailable, /*unsupported*/ false);

            Unit* defenseTarget = nullptr;
            for (Unit* attacker : escort->getAttackers())
            {
                if (!attacker || !attacker->IsAlive() || !bot->IsValidAttackTarget(attacker))
                    continue;
                if (!defenseTarget || attacker->GetGUID().GetCounter() < defenseTarget->GetGUID().GetCounter())
                    defenseTarget = attacker;
            }

            if (defenseTarget)
            {
                context->GetValue<GuidVector>("prioritized targets")->Set({defenseTarget->GetGUID()});
                ExactQuestDefenseAttackAction attack(botAI);
                if (attack.AttackExact(defenseTarget))
                    return true;
            }

            // Normal self-defense/combat strategy keeps ownership until combat
            // ends. The next non-combat tick resumes following the escort.
            if (bot->IsInCombat())
                return true;

            float const distance = bot->GetDistance(escort);
            if (distance > 10.0f)
            {
                if (MoveWorldObjectTo(escort->GetGUID()))
                    return true;
                if (rt.oracleRouteIdentityPinned)
                    return ForceToWait(250);
                return MoveRandomNear(4.0f);
            }

            if (bot->isMoving() && distance < 4.0f)
                bot->StopMoving();

            if (!data.lastReachPOI)
                data.lastReachPOI = getMSTime();
            else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questEscortBudgetMs)
                return BlockQuest(data, QuestFailureReason::ProgressDidNotChange, /*unsupported*/ false);

            return ForceToWait(500);
        }

        case QuestActionPhase::LootSource:
        {
            if (ObjectiveCurrentCount(questId, objectiveIdx) > static_cast<int32>(rt.baselineCount))
            {
                EnterQuestPhase(data, QuestActionPhase::VerifyProgress);
                return true;
            }

            // Drive the normal Playerbots loot pipeline explicitly. The generic
            // available-loot stack is transient and can be cleared between the
            // combat tick and this delayed quest tick, so re-seed only the exact
            // retained objective target before normal selection. LootAction and
            // LootObject still enforce whitelist, item need, ownership, distance,
            // and lootable-state checks; this does not bypass game loot rules.
            // LootAction's objective selector enforces the source-entry whitelist
            // and exact required item before any interaction.
            if (rt.selectedSourceEntry < 0 &&
                !BindSourceGameObject(rt, 2.0f * sPlayerbotAIConfig.sightDistance))
            {
                // A prepared movement segment can finish after the phase transition, or another
                // higher-priority movement owner can leave the bot outside the source radius. Do
                // not sit in LootSource waiting for an object that is no longer nearby; resume the
                // same exact source travel path and let the normal bind/loot gates run again.
                if (bot->GetDistance(data.pos) > questTravelArriveDist)
                {
                    EnterQuestPhase(data, QuestActionPhase::TravelToSource);
                    return true;
                }
                if (!data.lastReachPOI)
                    data.lastReachPOI = getMSTime();
                else if (GetMSTimeDiffToNow(data.lastReachPOI) >= 30000)
                {
                    if (++rt.attemptCount >= maxObjectiveAttempts)
                        return BlockQuest(data, QuestFailureReason::NoLiveCandidate, /*unsupported*/ false);
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                }
                return true;
            }

            if (rt.selectedSourceEntry < 0)
            {
                GameObject* go = ObjectAccessor::GetGameObject(*bot, rt.selectedTargetGuid);
                if (!go || !go->IsInWorld() || !go->isSpawned())
                {
                    rt.selectedTargetGuid.Clear();
                    EnterQuestPhase(data, QuestActionPhase::WaitForRespawn);
                    return true;
                }

                if (bot->GetDistance(go) > INTERACTION_DISTANCE - 1.0f)
                {
                    if (MoveWorldObjectTo(go->GetGUID()))
                        return true;
                    if (rt.oracleRouteIdentityPinned)
                        return ForceToWait(250);
                    return MoveRandomNear(4.0f);
                }

                // The generic loot selector can legitimately ignore a chest
                // that was not discovered through its ambient scan. We already
                // hold the exact objective-locked GO, so construct the normal
                // LootObject explicitly and retain every IsLootPossible gate
                // (quest need, lock item/skill, flags, height, and spawn state).
                //
                // OpenLootAction's fallback opening-spell path can deactivate a
                // simple quest chest without producing SMSG_LOOT_RESPONSE. Once
                // the same gates have passed, open the normal core loot window
                // directly. Player::SendLoot still generates the real GO loot,
                // drives the ordinary packet/autostore pipeline, and awards the
                // item/quest credit in core; nothing is granted or mutated here.
                LootObject exactLoot(bot, go->GetGUID());
                bool advertisesRequiredQuestItem = false;
                if (GameObjectQuestItemList const* questItems = sObjectMgr->GetGameObjectQuestItemList(go->GetEntry()))
                {
                    advertisesRequiredQuestItem =
                        std::find(questItems->begin(), questItems->end(), spec.requiredItemId) != questItems->end();
                }

                uint32 const lockId = go->GetGOInfo()->GetLockId();
                LockEntry const* lockInfo = lockId ? sLockStore.LookupEntry(lockId) : nullptr;
                bool const ordinaryOpenAlternative =
                    !lockId || (lockInfo && GameObjectLockPolicy::HasOrdinaryOpenAlternative(lockInfo));
                bool const ordinaryQuestContainer = advertisesRequiredQuestItem && exactLoot.reqItem == 0 &&
                                                    ordinaryOpenAlternative && go->ActivateToQuest(bot) &&
                                                    go->GetGoState() == GO_STATE_READY &&
                                                    !go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE);
                if (ordinaryQuestContainer)
                {
                    if (!data.lastReachPOI)
                    {
                        if (bot->isMoving())
                            bot->StopMoving();
                        bot->SetFacingToObject(go);
                        context->GetValue<LootObject>("loot target")->Set(exactLoot);
                        bot->SendLoot(go->GetGUID(), LOOT_SKINNING);
                        if (rt.oracleRouteIdentityPinned)
                        {
                            rt.oracleRouteNativeInteractionObserved = true;
                            AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                                bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                        }

                        // A headless Playerbot has no client UI to click the
                        // slot exposed by SendLoot. Consume only the visible
                        // slot matching this objective through the same world
                        // session opcode handler a real client uses. The core's
                        // LootItemInSlot/StoreLootItem path remains authoritative
                        // for ownership, quest conditions, inventory capacity,
                        // item creation, and ItemAddedQuestCheck.
                        uint32 const maxSlot = go->loot.GetMaxSlotInLootFor(bot);
                        for (uint32 slot = 0; slot < maxSlot; ++slot)
                        {
                            LootItem* item = go->loot.LootItemInSlot(slot, bot);
                            if (!item || item->itemid != spec.requiredItemId)
                                continue;

                            WorldPacket autostore(CMSG_AUTOSTORE_LOOT_ITEM, 1);
                            autostore << static_cast<uint8>(slot);
                            bot->GetSession()->HandleAutostoreLootItemOpcode(autostore);
                            botAI->RecordAutoWowStoreLootExecution();
                            botAI->RecordAutoWowAutostoreLootPacket();
                            break;
                        }

                        WorldPacket release(CMSG_LOOT_RELEASE, 8);
                        release << go->GetGUID();
                        bot->GetSession()->HandleLootReleaseOpcode(release);
                        botAI->RecordAutoWowLootReleasePacket();
                        data.lastReachPOI = getMSTime();
                        return true;
                    }

                    if (GetMSTimeDiffToNow(data.lastReachPOI) < questInteractionVerifyMs)
                        return ForceToWait(250);

                    // The normal packet handler has had time to autostore. If
                    // the counter did not move, release/re-resolve instead of
                    // spamming the same chest every AI tick.
                    WorldPacket* release = new WorldPacket(CMSG_LOOT_RELEASE, 8);
                    *release << go->GetGUID();
                    bot->GetSession()->QueuePacket(release);
                    context->GetValue<LootObject>("loot target")->Set(LootObject());
                    rt.selectedTargetGuid.Clear();
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                    return true;
                }

                // Do not fall through into ambient loot for an objective GO:
                // that path can consume/deactivate it without the exact quest
                // item. Locked or profession-gated sources remain unsupported
                // here until their specialist interaction is selected.
                rt.selectedTargetGuid.Clear();
                if (++rt.attemptCount >= maxObjectiveAttempts)
                    return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);
                EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                return true;
            }

            if (!rt.selectedTargetGuid.IsEmpty())
                AI_VALUE(LootObjectStack*, "available loot")->Add(rt.selectedTargetGuid);

            botAI->DoSpecificAction("loot", Event("quest objective loot", "", bot), true);
            if (botAI->DoSpecificAction("open loot", Event("quest objective loot", "", bot), true))
                return true;
            if (botAI->DoSpecificAction("move to loot", Event("quest objective loot", "", bot), true))
                return true;

            if (!data.lastReachPOI)
                data.lastReachPOI = getMSTime();
            else if (GetMSTimeDiffToNow(data.lastReachPOI) >= 30000)
            {
                // Loot window elapsed with no item gain (no drop this time).
                rt.selectedTargetGuid = ObjectGuid();
                EnterQuestPhase(data, rt.selectedSourceEntry < 0 ? QuestActionPhase::ResolveObjective
                                                                 : QuestActionPhase::AcquireTarget);
                return true;
            }
            return true;  // let the loot strategy work
        }

        case QuestActionPhase::VerifyProgress:
        {
            int32 cur = ObjectiveCurrentCount(questId, objectiveIdx);
            int32 required = ObjectiveRequiredCount(quest, objectiveIdx);
            if (cur > static_cast<int32>(rt.baselineCount))
            {
                // Real, delta-based progress.
                rt.lastObservedCount = cur;
                QuestSourceStallPolicy::ResetAfterObjectiveChangeOrCredit(
                    rt.lastProgressTimeMs, false, true);
                rt.attemptCount = 0;
                if (required > 0 && cur >= required)
                {
                    // Objective satisfied — re-resolve to pick the next incomplete
                    // objective (or the quest flips COMPLETE and Execute routes to
                    // the finisher machine next tick).
                    rt.baselineCount = cur;
                    EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
                }
                else
                {
                    // Partial progress; rebase and keep working this source.
                    rt.baselineCount = cur;
                    rt.selectedTargetGuid = ObjectGuid();
                    EnterQuestPhase(data, AfterPartialQuestCredit(spec.kind, rt.selectedSourceEntry));
                }
                return true;
            }

            // No delta. This is the xp-without-progress case: the bot may have
            // spent time / gained xp on non-qualifying kills. Record it and, after
            // a few attempts, surface a typed blocker for the Director.
            LOG_DEBUG("playerbots",
                      "[New RPG] {} quest {} objective {} xp-without-progress (count still {} of {}, attempt {})",
                      bot->GetName(), questId, objectiveIdx, cur, required, rt.attemptCount);
            // A GO item source that produced no core item delta is not useful for an immediate
            // replay. Remember its DB spawn and try another resolved source, if one exists.
            // This affects source selection only; no item, loot, or quest credit is synthesized.
            if (spec.key.family == QuestObjectiveFamily::Item && rt.selectedSourceEntry < 0 &&
                rt.selectedSourceSpawn.GetRawValue() != 0)
            {
                uint64 const failedSpawn = rt.selectedSourceSpawn.GetRawValue();
                rt.exhaustedSourceSpawns.insert(failedSpawn);
                rt.sourceRotationCooldownUntil[failedSpawn] = getMSTime() + 5U * 60U * 1000U;
                ++rt.sourceRotationCount;
                std::vector<int32> objectiveEntries;
                std::vector<QuestSourceRotationPolicy::Candidate> candidates;
                for (QuestObjectiveSource const& source : spec.sources)
                {
                    int32 const signedEntry = source.type == QuestObjectiveSource::Type::Creature
                        ? static_cast<int32>(source.entry)
                        : SignedGameObjectObjectiveEntry(source.entry);
                    if (signedEntry == 0)
                        continue;
                    objectiveEntries.push_back(signedEntry);
                    for (GuidPosition const& spawn : source.spawns)
                        candidates.push_back({signedEntry, spawn.GetMapId(), spawn.GetRawValue(),
                            bot->GetDistance2d(spawn.GetPositionX(), spawn.GetPositionY())});
                }
                std::vector<uint64> exhausted(rt.exhaustedSourceSpawns.begin(),
                                              rt.exhaustedSourceSpawns.end());
                QuestSourceRotationPolicy::Request const request{
                    bot->GetMapId(), rt.selectedSourceEntry, failedSpawn, false,
                    rt.sourceRotationCount, maxSourceRotations, objectiveEntries, exhausted,
                    candidates};
                bool const alternateSource = QuestSourceRotationPolicy::Decide(request).decision ==
                    QuestSourceRotationPolicy::Decision::RotateToNextSource;
                LOG_DEBUG("playerbots",
                    "[New RPG] {} quest {} objective {} exhausted GO source={} "
                    "alternate={} rotations={}", bot->GetName(), questId, objectiveIdx,
                    failedSpawn, alternateSource, rt.sourceRotationCount);
                if (!alternateSource)
                    return BlockQuest(data, QuestFailureReason::ProgressDidNotChange, false);
            }
            if (++rt.attemptCount >= maxObjectiveAttempts)
                return BlockQuest(data, QuestFailureReason::ProgressDidNotChange, /*unsupported*/ false);
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            return true;
        }

        case QuestActionPhase::WaitForRespawn:
            // Sources briefly depleted; re-resolve after a bounded wait so the
            // rotation can select the next/respawned exact source. Never resume
            // at the exhausted spawn without passing through the whitelist.
            if (sourceFailureBudgetExpired())
                return BlockQuest(data, QuestFailureReason::NoLiveCandidate, false);
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            return rt.exhaustedSourceSpawns.empty() ? true : ForceToWait(5000);

        default:
            // Any finisher-side phase reaching here means the quest is no longer
            // COMPLETE (e.g. progress was reverted) — restart objective work.
            EnterQuestPhase(data, QuestActionPhase::ResolveObjective);
            return true;
    }
}

/* ------------------------------------------------------------------------- *
 *  Exact finisher turn-in (COMPLETE quest)
 *
 *  Phase pipeline:
 *    ResolveFinisher -> TravelToFinisher -> InteractFinisher -> VerifyReward
 *                    -> Complete
 *  Uses the exact creature/GO finisher resolved by the "active quest finisher"
 *  value. Interacts ONLY with that finisher (never SearchQuestGiverAndAccept-
 *  OrReward, which would OrganizeQuestLog and touch unrelated quests). Reward is
 *  a verified postcondition, not "packet sent".
 * ------------------------------------------------------------------------- */
bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest ? data.quest : sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
    {
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    QuestObjectiveRuntime& rt = data.objectiveRuntime;

    // A reward-capacity blocker is recoverable when the bot can reach a vendor. Keep this
    // inside the exact finisher state machine so the Oracle roster does not need the legacy
    // free-roam RPG strategy, and retry only after the core sell handler has run.
    if (TryRelieveInventoryAtVendor(data))
        return true;

    // Arriving from the objective side (or a stale phase): start the finisher run.
    // A Blocked phase is preserved so a finisher-side blocker keeps holding for
    // the Director instead of silently restarting the turn-in loop.
    switch (rt.phase)
    {
        case QuestActionPhase::ResolveFinisher:
        case QuestActionPhase::TravelToFinisher:
        case QuestActionPhase::InteractFinisher:
        case QuestActionPhase::VerifyReward:
        case QuestActionPhase::Complete:
        case QuestActionPhase::Blocked:
            break;
        default:
            EnterQuestPhase(data, QuestActionPhase::ResolveFinisher);
            break;
    }

    bool const finisherWorkWindow =
        rt.phase == QuestActionPhase::InteractFinisher ||
        rt.phase == QuestActionPhase::VerifyReward ||
        rt.phase == QuestActionPhase::Complete ||
        rt.phase == QuestActionPhase::Blocked;
    if (MaintainQuestPartyCohesion(data, finisherWorkWindow))
        return true;

    switch (rt.phase)
    {
        case QuestActionPhase::ResolveFinisher:
        {
            QuestFinisherRef fin = AI_VALUE(QuestFinisherRef, "active quest finisher");
            if (fin.signedEntry == 0 || WorldPosition(fin.stableSpawn) == WorldPosition())
                return BlockQuest(data, QuestFailureReason::NoFinisherRelation, /*unsupported*/ false);

            if (rt.oracleFinisherAuthorized &&
                (fin.stableSpawn.GetRawValue() != rt.oracleFinisherStableSpawnGuid ||
                 fin.signedEntry != rt.oracleFinisherSignedEntry || rt.oracleFinisherQuestId != questId))
                return BlockQuest(data, QuestFailureReason::OracleFinisherMismatch, /*unsupported*/ false);

            fin.runtimeGuid = ObjectGuid();  // resolve the live GUID on approach
            rt.finisher = fin;
            rt.finisherReceipt = {};
            rt.finisherReceipt.questId = questId;
            rt.finisherReceipt.signedEntry = fin.signedEntry;
            rt.finisherReceipt.stableSpawnGuid = fin.stableSpawn;
            rt.finisherReceipt.coreCompleted = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
            data.pos = WorldPosition(rt.finisher.stableSpawn);
            rt.attemptCount = 0;

            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !rt.completionAnnounced)
            {
                BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
                botAI->rpgStatistic.questCompleted++;
                rt.completionAnnounced = true;
            }

            EnterQuestPhase(data, QuestActionPhase::TravelToFinisher);
            return true;
        }

        case QuestActionPhase::TravelToFinisher:
        {
            if (rt.oracleManaged && rt.oracleTaggedDispatch && OracleRouteV2Enabled())
                return DriveOracleQuestRoute(data, nullptr, -1, /*finisher*/ true);

            // Bind the live GUID as soon as the stable spawn is loaded. Keep the resolved object
            // directly so a loaded finisher at 74-108 yards cannot fall back to the stale
            // GuidPosition in data.pos between the bind and this travel decision.
            WorldObject* fobj = nullptr;
            BindFinisher(rt, INTERACTION_DISTANCE * 5.0f, &fobj);
            if (!fobj && !rt.finisher.runtimeGuid.IsEmpty())
                fobj = ObjectAccessor::GetWorldObject(*bot, rt.finisher.runtimeGuid);

            if (fobj && IsWithinInteractionDist(fobj))
            {
                rt.finisherReceipt.runtimeGuid = fobj->GetGUID();
                rt.finisherReceipt.atInteractionRange = true;
                EnterQuestPhase(data, QuestActionPhase::InteractFinisher);
                return true;
            }

            WorldPosition dest = fobj ? WorldPosition(fobj->GetMapId(), fobj->GetPositionX(), fobj->GetPositionY(),
                                                      fobj->GetPositionZ())
                                      : data.pos;

            if (!fobj && bot->GetDistance(data.pos) <= questTravelArriveDist)
            {
                // Reached the finisher POI but its spawn isn't loaded / not the
                // pinned spawn. Give it a moment, then block for the Director.
                if (!data.lastReachPOI)
                    data.lastReachPOI = getMSTime();
                else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questAcquireBudgetMs)
                    return BlockQuest(data, QuestFailureReason::NoFinisherSpawn, /*unsupported*/ false);
                return ForceToWait(250);
            }

            if (sPlayerbotAIConfig.autoWowQuestTravelProgressWatch && !rt.oracleManaged &&
                TravelProgressExpired(rt, data.pos, bot->GetDistance(dest)))
            {
                QuestStallRecoveryPolicy::ResetTravel(rt.travelWatch);
                return BlockQuest(data, QuestFailureReason::TravelNoProgress, /*unsupported*/ false);
            }
            bool stuck = false;
            bool const strictOracleRoute = StrictFinisherMovementPolicy::IsEnabled(
                rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId);
            bool const oracleQuestNoTeleport = StrictFinisherMovementPolicy::UseQuestNoTeleport(
                rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId);
            // The persistent AutoWow roster must finish quests like players: use the same
            // grounded walk proof as objective travel and never silently teleport a finisher.
            // Non-roster Playerbots retain the historical recovery behavior, and strict Oracle
            // finishers still retain their exact route identity and fail-closed semantics.
            bool const autoWowQuestNoTeleport =
                AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter());
            bool const questNoTeleport = oracleQuestNoTeleport || autoWowQuestNoTeleport;
            bool const allowLegacyTeleportRecovery =
                StrictFinisherMovementPolicy::AllowLegacyTeleportRecovery(
                    rt.oracleFinisherAuthorized, rt.oracleFinisherDecisionId) && !autoWowQuestNoTeleport;
            StrictFinisherMovementPolicy::RouteIdentity const strictRoute{
                strictOracleRoute ? rt.oracleFinisherDecisionId : 0, questId,
                rt.finisher.stableSpawn.GetRawValue()};
            bool const moved = MoveFarTo(dest, questNoTeleport, &stuck,
                                         /*deterministicPath*/ strictOracleRoute, strictRoute,
                                         allowLegacyTeleportRecovery);
            if (moved)
            {
                if (stuck)
                    return BlockQuest(data, TravelStuckReason(), /*unsupported*/ false);
                data.lastReachPOI = 0;
                return true;
            }
            if (stuck)
                return BlockQuest(data, TravelStuckReason(), /*unsupported*/ false);
            if (!data.lastReachPOI)
                data.lastReachPOI = getMSTime();
            else if (GetMSTimeDiffToNow(data.lastReachPOI) >= questAcquireBudgetMs)
                return BlockQuest(data, QuestFailureReason::MovementStuckNoTeleport, /*unsupported*/ false);
            return ForceToWait(250);
        }

        case QuestActionPhase::InteractFinisher:
        {
            WorldObject* fobj = rt.finisher.runtimeGuid.IsEmpty()
                                    ? nullptr
                                    : ObjectAccessor::GetWorldObject(*bot, rt.finisher.runtimeGuid);
            if (!fobj)
            {
                // Lost the object (unloaded / moved) — go re-approach and rebind.
                EnterQuestPhase(data, QuestActionPhase::TravelToFinisher);
                return true;
            }
            if (!IsWithinInteractionDist(fobj))
            {
                EnterQuestPhase(data, QuestActionPhase::TravelToFinisher);
                return true;
            }

            // The resolved finisher must own this quest. This exact relation check applies to
            // both status paths so a stale or merely nearby object cannot complete or reward it.
            if (!fobj->hasInvolvedQuest(questId))
                return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);

            rt.finisherReceipt.runtimeGuid = fobj->GetGUID();
            rt.finisherReceipt.atInteractionRange = true;
            rt.finisherReceipt.exactRelationVerified = true;

            // Postcondition already met (e.g. a prior tick's turn-in applied).
            if (bot->GetQuestRewardStatus(questId))
            {
                rt.finisherReceipt.coreCompleted = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
                rt.finisherReceipt.rewardConfirmed = true;
                EnterQuestPhase(data, QuestActionPhase::VerifyReward);
                return true;
            }

            // Some speak-to/turn-in quests remain QUEST_STATUS_INCOMPLETE until the client asks the
            // exact involved NPC/GO to complete them. Submit that ordinary packet through the core
            // handler; never call CompleteQuest, RewardQuest, or mutate quest state directly here.
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            {
                if (!bot->CanCompleteQuest(questId))
                    return BlockQuest(data, QuestFailureReason::CanRewardFalse, /*unsupported*/ false);

                if (!data.lastReachPOI)
                {
                    WorldPacket request(CMSG_QUESTGIVER_REQUEST_REWARD, 16);
                    request << fobj->GetGUID() << questId;
                    request.rpos(0);
                    rt.finisherReceipt.sequence = ++rt.finisherRequestSequence;
                    rt.finisherReceipt.completionRequestSent = true;
                    bot->GetSession()->HandleQuestgiverRequestRewardOpcode(request);
                    if (rt.oracleRouteIdentityPinned)
                    {
                        rt.oracleRouteNativeInteractionObserved = true;
                        AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                            bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
                    }
                    data.lastReachPOI = getMSTime();
                    return true;
                }

                if (GetMSTimeDiffToNow(data.lastReachPOI) < questInteractionVerifyMs)
                    return ForceToWait(250);

                if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                {
                    data.lastReachPOI = 0;
                    if (++rt.attemptCount >= maxRewardAttempts)
                        return BlockQuest(data, QuestFailureReason::CanRewardFalse, /*unsupported*/ false);
                    return true;
                }
                rt.finisherReceipt.coreCompleted = true;
            }

            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !rt.completionAnnounced)
            {
                // The completion request above is the only transition out of the incomplete state.
                // Announce it only after the core has actually changed the status.
                BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
                botAI->rpgStatistic.questCompleted++;
                rt.completionAnnounced = true;
            }

            rt.finisherReceipt.coreCompleted = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;

            if (!bot->CanInteractWithQuestGiver(fobj))
                return BlockQuest(data, QuestFailureReason::InteractionRejected, /*unsupported*/ false);

            bool const canRewardQuest = bot->CanRewardQuest(quest, false);
            if (!canRewardQuest)
                return BlockQuest(data, ClassifyRewardFailure(false, false), /*unsupported*/ false);

            // Select first, then ask the indexed overload about that exact choice. With the general
            // quest gate already green, indexed failure is the core's reward-storage check.
            rt.selectedRewardIndex = BestRewardIndex(quest);
            rt.selectedRewardIndexKnown = true;
            bool const canRewardSelected = bot->CanRewardQuest(quest, rt.selectedRewardIndex, false);
            QuestFailureReason const rewardFailure = ClassifyRewardFailure(canRewardQuest, canRewardSelected);
            if (rewardFailure != QuestFailureReason::None)
            {
                // Do not publish InventoryFull and let the Oracle arbiter re-arm the quest before
                // the same finisher state can recover it. Give the capacity owner one immediate
                // chance to route/sell, then fall back to the typed blocker if no route exists.
                if (rewardFailure == QuestFailureReason::InventoryFull)
                {
                    rt.failure = rewardFailure;
                    if (TryRelieveInventoryAtVendor(data))
                        return true;
                }
                return BlockQuest(data, rewardFailure, /*unsupported*/ false);
            }

            // Turn in ONLY this quest at ONLY this finisher. TurnInQuest reuses
            // CanRewardQuest + BestRewardIndex and sends the choose-reward packet.
            TurnInQuest(quest, fobj->GetGUID());
            if (rt.oracleRouteIdentityPinned)
            {
                rt.oracleRouteNativeInteractionObserved = true;
                AutoWowOracleQuestExecutor::RecordQuestRouteNativeInteraction(
                    bot->GetGUID().GetCounter(), rt.oracleRouteDecisionId);
            }
            if (!rt.finisherReceipt.sequence)
                rt.finisherReceipt.sequence = ++rt.finisherRequestSequence;
            rt.rewardAttemptTimeMs = getMSTime();
            ForceToWait(1500);
            EnterQuestPhase(data, QuestActionPhase::VerifyReward);
            return true;
        }

        case QuestActionPhase::VerifyReward:
        {
            // Success is the applied reward, NOT the sent packet.
            if (bot->GetQuestRewardStatus(questId))
            {
                rt.finisherReceipt.coreCompleted = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
                rt.finisherReceipt.rewardConfirmed = true;
                BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
                botAI->rpgStatistic.questRewarded++;
                LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {} at finisher {}", bot->GetName(), questId,
                          std::abs(rt.finisher.signedEntry));
                EnterQuestPhase(data, QuestActionPhase::Complete);
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
            if (++rt.attemptCount >= maxRewardAttempts)
                return BlockQuest(data, QuestFailureReason::RewardNotConfirmed, /*unsupported*/ false);
            // Retry the interaction (re-check range / rebind as needed).
            EnterQuestPhase(data, QuestActionPhase::InteractFinisher);
            return true;
        }

        case QuestActionPhase::Complete:
            botAI->rpgInfo.ChangeToIdle();
            return true;

        case QuestActionPhase::Blocked:
            if (sPlayerbotAIConfig.autoWowQuestBlockedDefer && DeferBlockedQuest(data))
                return true;
            return ForceToWait(3000);

        default:
            EnterQuestPhase(data, QuestActionPhase::ResolveFinisher);
            return true;
    }
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes[0], nodes[nodes.size() - 1]);
        info.ChangeToIdle();
        return true;
    }
    return true;
}
