/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include "AutoWowQuestLedger.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "AutoWowAcceptance.h"
#include "AutoWowBridge.h"
#include "AutoWowOracleRuntime.h"
#include "AutonomousRpgTravelPolicy.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathLoopBreaker.h"
#include "DungeonPathSafety.h"
#include "DungeonPathWalkAction.h"
#include "Formulas.h"
#include "GameTime.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GearUpgradePolicy.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "MotionMaster.h"
#include "NavmeshSnap.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "OutdoorPvPMgr.h"
#include "PackAvoidPolicy.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PartyPolicy.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestInventoryReliefPolicy.h"
#include "QuestPackets.h"
#include "QuestSchedulerPolicy.h"
#include "QuestStallRecoveryPolicy.h"
#include "QuestTravelWalk.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SellAction.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "SurvivalRecovery.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "UnstickPolicy.h"
#include "WalkingV2Policy.h"

namespace
{
// Keep the expensive TravelMgr graph probe out of the per-bot NewRpgInfo header. The map is
// intentionally keyed by the persistent bot GUID and contains only the latest timestamp per bot.
std::unordered_map<uint32, uint32> questWalkProbeTsByBot;

// Only the AutoWow autonomous RPG travel path uses this short rejection memory. Keeping it
// outside NewRpgInfo avoids resetting a failed destination on an ordinary RPG status change.
struct FailedTravelDestination
{
    WorldPosition pos;
    uint32 failedAt;
};
std::unordered_map<uint32, std::vector<FailedTravelDestination>> failedTravelByBot;
std::unordered_map<uint32, uint32> lastAutonomousTravelTickByBot;

// Timed stall deferrals (AutoWow.QuestBlockedDefer.Enable). Outside NewRpgInfo so a status change or
// a fresh DoQuest cannot clear them; bots update on map threads, hence the lock.
// ponytail: one global lock; touched only when the flag is on (select + defer paths).
std::mutex stallDeferralLock;
std::unordered_map<uint32, QuestStallRecoveryPolicy::DeferralBook> stallDeferralsByBot;
constexpr size_t maxFailedTravelDestinations = 8;

// AutoWow.QuestScheduler.Enable: per-bot momentum / slice / rotation cooldown (QuestSchedulerPolicy).
// ponytail: one global lock, held only for in-memory bookkeeping (never across world queries).
std::mutex questSchedLock;
std::unordered_map<uint32, QuestSchedulerPolicy::BotState> questSchedByBot;
// AutoWow.Unstick.V2: next quest-log trim look per bot (game-time ms; under questSchedLock).
std::unordered_map<uint32, uint64> logTrimNextMsByBot;
constexpr uint64 logTrimCheckMs = 60 * 1000;

// Quest-log counters in slot order (same fields as the ledger `progress` sampler).
std::vector<QuestSchedulerPolicy::Observation> ObserveQuestLog(Player* bot)
{
    std::vector<QuestSchedulerPolicy::Observation> out;
    QuestStatusMap& statusMap = bot->getQuestStatusMap();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        auto const it = statusMap.find(questId);
        if (it == statusMap.end())
            continue;
        QuestSchedulerPolicy::Observation obs;
        obs.counters.quest = questId;
        for (size_t k = 0; k < AutoWowQuestLedger::kCreatureCounters; ++k)
            obs.counters.c[k] = it->second.CreatureOrGOCount[k];
        for (size_t k = 0; k < AutoWowQuestLedger::kItemCounters; ++k)
            obs.counters.i[k] = it->second.ItemCount[k];
        obs.complete = it->second.Status == QUEST_STATUS_COMPLETE;
        out.push_back(obs);
    }
    return out;
}

struct VendorReliefCandidate
{
    uint32 mapId = 0;
    uint32 zoneId = 0;
    uint32 phaseMask = 0;
    uint32 checkedAtMs = 0;
    WorldPosition pos;
};
std::unordered_map<uint32, VendorReliefCandidate> vendorReliefCandidatesByBot;
constexpr uint32 vendorReliefCacheMs = 60 * 1000;
constexpr float maxVendorReliefDistance = 800.0f;
std::unordered_map<uint32, uint32> lastQuestBagPurchaseAttemptByBot;

struct QuestBagOffer
{
    uint32 itemId = 0;
    uint32 vendorSlot = 0;
    uint32 priceCopper = 0;
};

bool TravelDestinationCoolingDown(uint32 botGuid, WorldPosition const& pos)
{
    auto it = failedTravelByBot.find(botGuid);
    if (it == failedTravelByBot.end())
        return false;
    auto& failed = it->second;
    failed.erase(std::remove_if(failed.begin(), failed.end(), [](FailedTravelDestination const& entry)
    {
        return !AutonomousRpgTravelPolicy::IsCoolingDown(entry.failedAt, getMSTime());
    }), failed.end());
    for (FailedTravelDestination const& entry : failed)
        if (entry.pos.GetMapId() == pos.GetMapId() && entry.pos.GetExactDist2d(pos) < 30.0f)
            return true;
    return false;
}

WorldPosition SelectReachableAutoWowTravelPos(Player* bot, std::vector<WorldLocation> const& locs,
                                               float minRange, float maxRange = 250.0f)
{
    // A cache location is only a coarse point of interest. Re-ground it, then require a complete
    // local mmap route before committing a long-lived GO_GRIND/GO_CAMP status to it.
    std::vector<WorldPosition> candidates;
    uint32 coolingCandidates = 0;
    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        inCity = (zone->flags & AREA_FLAG_CAPITAL) != 0;
    for (WorldLocation const& loc : locs)
    {
        if (loc.GetMapId() != bot->GetMapId() || bot->GetExactDist2d(loc) < minRange ||
            bot->GetExactDist2d(loc) > maxRange)
            continue;
        float const ground = std::max(bot->GetMap()->GetHeight(loc.GetPositionX(), loc.GetPositionY(), MAX_HEIGHT),
                                      bot->GetMap()->GetWaterLevel(loc.GetPositionX(), loc.GetPositionY()));
        if (ground == INVALID_HEIGHT || ground == VMAP_INVALID_HEIGHT_VALUE)
            continue;
        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(),
                                                loc.GetPositionY(), ground) != bot->GetZoneId())
            continue;
        if (AutoWowDeathLoop::Enabled() && AutoWowDeathLoop::IsDangerous(bot->GetGUID().GetCounter(),
                loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY()))
            continue;
        WorldPosition candidate(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), ground,
                                loc.GetOrientation());
        if (!TravelDestinationCoolingDown(bot->GetGUID().GetCounter(), candidate))
            candidates.push_back(candidate);
        else
            ++coolingCandidates;
    }

    size_t const eligibleCandidates = candidates.size();
    size_t probes = 0;
    WorldPosition bestPartial;
    float bestPartialProgress = 0.0f;
    for (size_t attempt = 0; attempt < 8 && !candidates.empty(); ++attempt)
    {
        ++probes;
        size_t const index = urand(0, candidates.size() - 1);
        WorldPosition const candidate = candidates[index];
        candidates[index] = candidates.back();
        candidates.pop_back();
        PathGenerator path(bot);
        path.SetSlopeCheck(true);
        path.CalculatePath(candidate.GetPositionX(), candidate.GetPositionY(), candidate.GetPositionZ());
        PathType const type = path.GetPathType();
        G3D::Vector3 const& endpoint = path.GetActualEndPosition();
        if (AutonomousRpgTravelPolicy::IsCompleteLocalRoute(
                (type & PATHFIND_NORMAL) != 0, (type & PATHFIND_INCOMPLETE) != 0,
                (type & PATHFIND_NOPATH) != 0, (type & PATHFIND_FARFROMPOLY) != 0,
                (type & PATHFIND_SHORTCUT) != 0, (type & PATHFIND_NOT_USING_PATH) != 0,
                candidate.GetExactDist(endpoint.x, endpoint.y, endpoint.z)))
            return candidate;

        // A long coarse point may have only a safe initial navmesh leg. Probe the corridor
        // with terrain checks and retain that reachable endpoint as a short local destination.
        AutoWowDungeonPath::ProbeResult const segment = AutoWowDungeonPath::Probe(
            bot, candidate.GetPositionX(), candidate.GetPositionY(), candidate.GetPositionZ());
        if (segment.safe && segment.mode == "navmesh" && segment.pathType == PATHFIND_NORMAL &&
            segment.endpointDistance <= 10.0f)
            return candidate;
        if (!segment.safe || segment.mode != "navmesh" || segment.path.empty())
            continue;
        G3D::Vector3 const& legEnd = segment.path.back();
        WorldPosition const leg(bot->GetMapId(), legEnd.x, legEnd.y, legEnd.z,
                                candidate.GetOrientation());
        bool const sameZone = bot->GetMap()->GetZoneId(bot->GetPhaseMask(),
            legEnd.x, legEnd.y, legEnd.z) == bot->GetZoneId();
        AutonomousRpgTravelPolicy::PartialSegmentFacts const facts{
            segment.safe && segment.mode == "navmesh",
            segment.pathType == (PATHFIND_NORMAL | PATHFIND_INCOMPLETE),
            sameZone,
            TravelDestinationCoolingDown(bot->GetGUID().GetCounter(), leg),
            segment.pathLength,
            bot->GetExactDist(candidate.GetPositionX(), candidate.GetPositionY(),
                              candidate.GetPositionZ()),
            segment.endpointDistance,
            bot->GetExactDist(legEnd.x, legEnd.y, legEnd.z)};
        auto const selected = AutonomousRpgTravelPolicy::SelectPartialSegmentEndpoint(
            facts, {legEnd.x, legEnd.y, legEnd.z});
        if (selected.accepted && selected.progress > bestPartialProgress)
        {
            bestPartialProgress = selected.progress;
            bestPartial = leg;
        }
    }
    if (bestPartial != WorldPosition())
    {
        LOG_DEBUG("playerbots", "[New RPG] AutoWow {} selected grounded partial travel segment ({},{},{},{}) progress={} probed={}",
                  bot->GetName(), bestPartial.GetMapId(), bestPartial.GetPositionX(),
                  bestPartial.GetPositionY(), bestPartial.GetPositionZ(), bestPartialProgress, probes);
        return bestPartial;
    }
    LOG_DEBUG("playerbots", "[New RPG] AutoWow {} no reachable local travel destination coarse={} eligible={} cooldown={} probed={}",
              bot->GetName(), locs.size(), eligibleCandidates, coolingCandidates, probes);
    return {};
}

// AutoWow.DeathLoop: a quest with an objective POI (bot's map) inside one of the bot's danger areas is
// deferred (lowPriorityQuest + ledger `deferred` reason `death_loop_area`) instead of being picked.
bool DeathLoopDefersQuest(Player* bot, uint32 questId, std::vector<POIInfo> const& pois)
{
    for (POIInfo const& poi : pois)
    {
        if (AutoWowDeathLoop::IsDangerous(bot->GetGUID().GetCounter(), bot->GetMapId(), poi.pos.x, poi.pos.y))
        {
            AutoWowDeathLoop::DeferQuest(bot, questId);
            return true;
        }
    }
    return false;
}

// AutoWow.Travel.VerticalSnap: `dest` on its floor's navmesh (NavmeshSnap.h); unchanged when the bot's map
// has no navmesh query or the snap box holds no walkable poly.
WorldPosition SnapTravelGoal(Player* bot, WorldPosition const& dest)
{
    dtNavMeshQuery const* query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (dest.GetMapId() != bot->GetMapId() || !query ||
        !AutoWowVerticalSnap::Snap(*query, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), x, y, z))
        return dest;
    return WorldPosition(dest.GetMapId(), x, y, z, dest.GetOrientation());
}
}

// AutoWow.QuestScheduler.PreferGearRewards: a reward (choice or fixed) the stock "item upgrade" value
// rates EQUIP (empty slot) or REPLACE (beats the equipped item for this bot's class/spec weights).
static uint32 GearRewardBonusYards(PlayerbotAI* botAI, Quest const* quest)
{
    bool weapon = false;
    bool other = false;
    auto consider = [&](uint32 itemId)
    {
        ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!proto)
            return;
        ItemUsage const usage =
            botAI->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", std::to_string(itemId))->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            return;
        (proto->Class == ITEM_CLASS_WEAPON ? weapon : other) = true;
    };
    for (uint8 k = 0; k < QUEST_REWARD_CHOICES_COUNT; ++k)
        consider(quest->RewardChoiceItemId[k]);
    for (uint8 k = 0; k < QUEST_REWARDS_COUNT; ++k)
        consider(quest->RewardItemId[k]);
    return QuestSchedulerPolicy::GearBonusYards(weapon, other);
}

bool NewRpgBaseAction::IsAutoWowTravelBot() const
{
    return botAI->IsAutoWowIndependentParty() ||
        AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter());
}

void NewRpgBaseAction::MarkTravelDestinationFailed(WorldPosition const& pos)
{
    uint32 const botGuid = bot->GetGUID().GetCounter();
    auto& failed = failedTravelByBot[botGuid];
    failed.erase(std::remove_if(failed.begin(), failed.end(), [&](FailedTravelDestination const& entry)
    {
        return entry.pos.GetMapId() == pos.GetMapId() && entry.pos.GetExactDist2d(pos) < 30.0f;
    }), failed.end());
    if (failed.size() >= maxFailedTravelDestinations)
        failed.erase(failed.begin());
    failed.push_back({pos, getMSTime()});
    LOG_INFO("playerbots", "[New RPG] AutoWow {} replans failed destination ({},{},{},{}) cooldown_ms={}",
             bot->GetName(), pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
             AutonomousRpgTravelPolicy::kFailedDestinationCooldownMs);
}

void NewRpgBaseAction::DeferQuestForStall(uint32 questId)
{
    std::lock_guard<std::mutex> guard(stallDeferralLock);
    stallDeferralsByBot[bot->GetGUID().GetCounter()].Defer(questId, getMSTime());
}

bool NewRpgBaseAction::IsQuestStallDeferred(uint32 questId)
{
    std::lock_guard<std::mutex> guard(stallDeferralLock);
    auto it = stallDeferralsByBot.find(bot->GetGUID().GetCounter());
    return it != stallDeferralsByBot.end() && it->second.IsDeferred(questId, getMSTime());
}

bool NewRpgBaseAction::ScheduleDoQuest(bool commit)
{
    uint32 const nowMs = getMSTime();
    uint32 const botGuid = bot->GetGUID().GetCounter();
    std::vector<QuestSchedulerPolicy::Observation> const log = ObserveQuestLog(bot);

    // In-memory facts first (momentum, rotation cooldown), then world queries without the lock.
    std::vector<std::pair<uint32, uint32>> lastProgress;  // questId -> lastProgressMs, eligible only
    {
        std::lock_guard<std::mutex> guard(questSchedLock);
        QuestSchedulerPolicy::BotState& state = questSchedByBot[botGuid];
        state.momentum.Observe(log, nowMs);
        for (QuestSchedulerPolicy::Observation const& obs : log)
            if (!state.cooldown.IsDeferred(obs.counters.quest, nowMs))
                lastProgress.emplace_back(obs.counters.quest, state.momentum.LastProgressMs(obs.counters.quest));
    }

    uint32 const greyLevel = Acore::XP::GetGrayLevel(bot->GetLevel());
    std::vector<QuestSchedulerPolicy::Candidate> candidates;
    for (auto const& [questId, lastProgressMs] : lastProgress)
    {
        if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
            continue;
        if (sPlayerbotAIConfig.autoWowQuestBlockedDefer && IsQuestStallDeferred(questId))
            continue;
        if (QuestSchedulerPolicy::ContainsQuestId(sPlayerbotAIConfig.autoWowQuestAvoidIds, questId))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::vector<POIInfo> poiInfo;
        if (!quest || !GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true) ||
            (AutoWowDeathLoop::Enabled() && DeathLoopDefersQuest(bot, questId, poiInfo)))
            continue;

        uint32 distance = std::numeric_limits<uint32>::max();
        for (POIInfo const& poi : poiInfo)
            distance = std::min(distance, QuestStallRecoveryPolicy::QuantizeYards(bot->GetDistance2d(poi.pos.x, poi.pos.y)));
        QuestSchedulerPolicy::Candidate candidate;
        candidate.questId = questId;
        candidate.complete = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
        candidate.grey = bot->GetQuestLevel(quest) <= static_cast<int32>(greyLevel);
        candidate.distanceYards = distance;
        candidate.lastProgressMs = lastProgressMs;
        if (QuestSchedulerPolicy::PreferGearRewards())
            candidate.gearBonusYards = GearRewardBonusYards(botAI, quest);
        candidates.push_back(candidate);
    }

    size_t const best = QuestSchedulerPolicy::PickBest(candidates, nowMs);
    if (!commit)
        return best != QuestSchedulerPolicy::npos;

    // Outleveled grey work far from the bot is dropped (progression over completion).
    for (QuestSchedulerPolicy::Candidate const& candidate : candidates)
    {
        if (!QuestSchedulerPolicy::IsDroppableGrey(candidate))
            continue;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            if (bot->GetQuestSlotQuestId(slot) != candidate.questId)
                continue;
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, candidate.questId, "sched_drop_grey");
            LOG_DEBUG("playerbots", "[New RPG] {} scheduler drops grey quest {} ({} yd)", bot->GetName(),
                      candidate.questId, candidate.distanceYards);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << slot;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            botAI->rpgStatistic.questDropped++;
            break;
        }
    }

    if (best == QuestSchedulerPolicy::npos)
        return false;
    QuestSchedulerPolicy::Candidate const& chosen = candidates[best];
    Quest const* quest = sObjectMgr->GetQuestTemplate(chosen.questId);
    {
        std::lock_guard<std::mutex> guard(questSchedLock);
        QuestSchedulerPolicy::BeginSlice(questSchedByBot[botGuid].slice, chosen.questId, chosen.complete, nowMs);
    }
    LOG_DEBUG("playerbots", "[New RPG] {} scheduler picks quest {} (complete {}, grey {}, {} yd, momentum {}) of {}",
              bot->GetName(), chosen.questId, chosen.complete, chosen.grey, chosen.distanceYards,
              QuestSchedulerPolicy::HasMomentum(chosen, nowMs), candidates.size());
    botAI->rpgInfo.ChangeToDoQuest(chosen.questId, quest, AutoWowOracleRuntime::IsManagedBot(botGuid));
    return true;
}

void NewRpgBaseAction::QuestLogTrimStep()
{
    uint32 const botGuid = bot->GetGUID().GetCounter();
    if (!botAI->IsAutoWowIndependentParty() || AutoWowOracleRuntime::IsManagedBot(botGuid) || !bot->GetMap() ||
        bot->GetMap()->Instanceable() || botAI->rpgInfo.GetStatus() == RPG_DO_QUEST)
        return;
    uint64 const nowMs = static_cast<uint64>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    {
        std::lock_guard<std::mutex> guard(questSchedLock);
        uint64& next = logTrimNextMsByBot[botGuid];
        if (nowMs < next)
            return;
        next = nowMs + logTrimCheckMs;
    }
    AutoWowUnstickV2::Params const& p = AutoWowUnstickV2::detail::gParams;
    std::vector<AutoWowUnstickV2::TrimFact> facts;
    for (AutoWowUnstickV2::QuestAge const& age : AutoWowUnstickV2::ObserveAges(botGuid, ObserveQuestLog(bot), nowMs))
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(age.counters.quest))
            facts.push_back({age.counters.quest, quest->GetZoneOrSort(), age.complete, age.sinceMs});
    for (uint32 const questId : AutoWowUnstickV2::PickTrims(facts, bot->GetZoneId(), nowMs, p.logTrimAbove,
                                                            p.logTrimStaleMs))
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            if (bot->GetQuestSlotQuestId(slot) != questId)
                continue;
            LOG_INFO("playerbots", "[Unstick] bot={} log_trim quest={} zone={} log={}", bot->GetName(), questId,
                     bot->GetZoneId(), facts.size());
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << slot;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            AutoWowQuestLedger::detail::tAbandonReason = "log_trim";
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            AutoWowQuestLedger::detail::tAbandonReason = "";
            botAI->rpgStatistic.questDropped++;
            break;
        }
}

bool NewRpgBaseAction::RotateStaleDoQuest()
{
    auto* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    uint32 const botGuid = bot->GetGUID().GetCounter();
    // Oracle-managed directives keep their external arbiter (as AutoWow.QuestBlockedDefer does).
    if (!data || !data->questId || data->objectiveRuntime.oracleManaged ||
        AutoWowOracleRuntime::IsManagedBot(botGuid))
        return false;

    uint32 const nowMs = getMSTime();
    uint32 const questId = data->questId;
    std::vector<QuestSchedulerPolicy::Observation> const log = ObserveQuestLog(bot);
    bool turnIn = false;
    for (QuestSchedulerPolicy::Observation const& obs : log)
        if (obs.counters.quest == questId)
            turnIn = obs.complete;
    {
        std::lock_guard<std::mutex> guard(questSchedLock);
        QuestSchedulerPolicy::BotState& state = questSchedByBot[botGuid];
        state.momentum.Observe(log, nowMs);
        if (state.slice.questId != questId)
        {
            // Directive started outside the scheduler (chat command, Oracle hand-off): slice from now.
            QuestSchedulerPolicy::BeginSlice(state.slice, questId, turnIn, nowMs);
            return false;
        }
        state.slice.turnIn = turnIn;
        if (!QuestSchedulerPolicy::SliceExpired(state.slice, state.momentum.LastProgressMs(questId), nowMs))
            return false;
        state.cooldown.Defer(questId, nowMs, QuestSchedulerPolicy::kRotateCooldownMs);
        state.slice = {};
    }

    LOG_INFO("playerbots", "[New RPG] {} quest {} rotated after a slice without progress (turn-in {}), cooldown {} ms",
             bot->GetName(), questId, turnIn, QuestSchedulerPolicy::kRotateCooldownMs);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, questId, "sched_rotate",
                                 AutoWowQuestLedger::PhaseName(data->objectiveRuntime.phase));
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

QuestFailureReason NewRpgBaseAction::TravelStuckReason()
{
    if (sPlayerbotAIConfig.autoWowTravelIntent)
    {
        switch (TravelIntentPolicy::TakeGiveUp(botAI->rpgInfo.travelIntent))
        {
            case TravelIntentPolicy::GiveUp::ReplanExhausted: return QuestFailureReason::IntentReplanExhausted;
            case TravelIntentPolicy::GiveUp::NoProgress: return QuestFailureReason::IntentNoProgress;
            case TravelIntentPolicy::GiveUp::None: break;
        }
    }
    return QuestFailureReason::MovementStuckNoTeleport;
}

bool NewRpgBaseAction::MoveFarToIntent(WorldPosition const& dest, bool questNoTeleport, bool* outStuck,
                                       bool deterministicPath,
                                       StrictFinisherMovementPolicy::RouteIdentity strictRoute)
{
    if (sPlayerbotAIConfig.autoWowWalkingV2)
        return MoveFarToIntentV2(dest, questNoTeleport, outStuck, deterministicPath, strictRoute);

    using namespace TravelIntentPolicy;
    Intent& intent = botAI->rpgInfo.travelIntent;
    Params const params{sPlayerbotAIConfig.autoWowTravelIntentReplanFailCount,
                        sPlayerbotAIConfig.autoWowTravelIntentProgressWindowMs,
                        sPlayerbotAIConfig.autoWowTravelIntentHysteresisPct};
    uint32 const now = getMSTime();
    uint32 const botGuid = bot->GetGUID().GetCounter();
    Point const goal = MakePoint(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    Point const here = MakePoint(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    auto dangerous = [&](Point const& p)
    {
        return AutoWowDeathLoop::Enabled() &&
            AutoWowDeathLoop::IsDangerous(botGuid, p.mapId, static_cast<float>(p.x), static_cast<float>(p.y));
    };
    auto recordStrict = [&]()
    {
        if (!deterministicPath)
            return;
        LastMovement& issued = AI_VALUE(LastMovement&, "last movement");
        botAI->rpgInfo.strictFinisherMovement = {
            true,
            strictRoute,
            {true, issued.msTime, issued.lastMoveToMapId, issued.lastMoveToX,
             issued.lastMoveToY, issued.lastMoveToZ}};
    };
    auto giveUp = [&](GiveUp reason) -> bool
    {
        LOG_INFO("playerbots", "[New RPG] AutoWow {} travel intent gives up ({}) toward ({},{},{},{}) at ({},{}) "
                 "best={} segments={} failures={}",
                 bot->GetName(), reason == GiveUp::NoProgress ? "intent_no_progress" : "intent_replan_exhausted",
                 dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                 bot->GetPositionX(), bot->GetPositionY(), intent.bestGoalYards, intent.segmentsCommitted,
                 intent.failures);
        if (AutoWowUnstick::Enabled() && reason == GiveUp::ReplanExhausted)
            AutoWowUnstick::NoteReplanExhausted(botGuid);
        Abandon(intent, reason);
        if (outStuck)
            *outStuck = true;
        // Quest callers block on (true, stuck); GO_GRIND/GO_CAMP mark the destination failed on (false, stuck).
        return questNoTeleport;
    };

    if (Observe(intent, goal, DistanceYards(here, goal), now, params) == Verdict::NoProgress)
        return giveUp(GiveUp::NoProgress);

    bool const moving = IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL) || bot->isMoving();
    bool const inCombat = bot->IsInCombat();

    // Final approach: inside the direct-path radius the goal itself is the segment.
    if (bot->GetExactDist(dest) < pathFinderDis)
    {
        if (moving)
            return true;
        bool const moved = MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                  false, false, false, true);
        if (moved)
            recordStrict();
        return moved;
    }

    switch (ClassifySegment(intent, here, moving, inCombat || (intent.hasSegment && dangerous(intent.segment))))
    {
        case SegmentState::Follow:
            return true;
        case SegmentState::Reached:
            CompleteSegment(intent);
            break;
        case SegmentState::Unsafe:
            DropSegment(intent);
            if (inCombat)
                return false;
            break;
        case SegmentState::Interrupted:
            if (ReplanCoolingDown(intent, now))
                return true;
            if (NoteFailure(intent, now, params))
                return giveUp(GiveUp::ReplanExhausted);
            break;
        case SegmentState::None:
            if (ReplanCoolingDown(intent, now))
                return true;
            break;
    }

    auto admit = [&](G3D::Vector3 const& end) -> bool
    {
        Point const candidate = MakePoint(bot->GetMapId(), end.x, end.y, end.z);
        uint32 const candidateGoalYards = DistanceYards(candidate, goal);
        if (!Admit(intent, here, candidate, candidateGoalYards, dangerous(candidate), params))
            return false;
        Commit(intent, here, candidate, candidateGoalYards);
        return true;
    };

    // 1. Prepared quest walk: the same ordered proof as the legacy path (complete direct, TravelMgr
    //    segment, deterministic local detour), now committed instead of re-selected per tick.
    if (questNoTeleport)
    {
        AutoWowQuestGiverTravel::QuestWalkProbeSelection selection =
            AutoWowQuestGiverTravel::SelectQuestWalkProbeDetailed(bot, dest);
        if (selection.probe && !selection.probe->path.empty() && admit(selection.probe->path.back()))
        {
            AutoWowDungeonWalkAction walk(botAI);
            selection.diagnostics.walkPreparedCalled = true;
            AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason rejectReason =
                AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::None;
            bool const accepted = walk.WalkPrepared(*selection.probe, &rejectReason);
            selection.diagnostics.walkPreparedAccepted = accepted;
            selection.diagnostics.walkRejectReason = rejectReason;
            AutoWowQuestGiverTravel::RecordQuestWalkDiagnostics(botGuid, selection.diagnostics);
            if (accepted)
            {
                recordStrict();
                return true;
            }
            DropSegment(intent);
        }
    }

    // 2. mmap route to the true destination (slope-checked, as the legacy executor).
    {
        PathGenerator path(bot);
        path.SetSlopeCheck(true);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        if (!(path.GetPathType() & ~typeOk))
        {
            G3D::Vector3 const endPos = path.GetActualEndPosition();
            if (dest.GetExactDist(endPos.x, endPos.y, endPos.z) + 5.0f < bot->GetDistance(dest) && admit(endPos))
            {
                if (MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true))
                {
                    recordStrict();
                    return true;
                }
                DropSegment(intent);
            }
        }
    }

    // 3. Nothing admissible. An interrupted segment is re-issued (the commitment holds); otherwise
    //    count the failed replan and let the caller's existing no-candidate handling run.
    if (intent.hasSegment)
    {
        bool const moved = MoveTo(intent.segment.mapId, static_cast<float>(intent.segment.x),
                                  static_cast<float>(intent.segment.y), static_cast<float>(intent.segment.z),
                                  false, false, false, true);
        if (moved)
            recordStrict();
        return moved;
    }
    if (NoteFailure(intent, now, params))
        return giveUp(GiveUp::ReplanExhausted);
    return false;
}

// AutoWow.Travel.Safe: idle hostile creatures near the bot, for chunk threat scoring (WalkingV2Policy
// PathThreat). The scan covers the 120 yd chunk ring plus aggro reach; the count is order-independent.
static std::vector<WalkingV2Policy::Mob> ScanTravelMobs(Player* bot)
{
    constexpr float kScanYards = 150.0f;
    std::list<Creature*> found;
    Acore::AllWorldObjectsInRange check(bot, kScanYards);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, kScanYards);
    std::vector<WalkingV2Policy::Mob> mobs;
    for (Creature* c : found)
    {
        if (!c || !c->IsInWorld() || !c->IsAlive() || c->IsInCombat() || c->IsCivilian() ||
            c->HasReactState(REACT_PASSIVE) || !c->IsHostileTo(bot))
            continue;
        WalkingV2Policy::Mob m;
        m.x = static_cast<std::int32_t>(std::floor(c->GetPositionX()));
        m.y = static_cast<std::int32_t>(std::floor(c->GetPositionY()));
        m.level = c->GetLevel();
        m.elite = c->isElite();
        m.aggroYards = static_cast<std::uint32_t>(std::max(0.0f, c->GetAggroRange(bot)));
        mobs.push_back(m);
    }
    return mobs;
}

// AutoWow.Walking.V2 (WalkingV2Policy.h has the root cause). The committed intent of MoveFarToIntent with
// two changes: a goal beyond the chunk ring is first walked by per-chunk mmap paths (goal-monotone
// admission), and an interrupted segment is dropped and replanned from where the bot stands - only a
// replan that finds nothing is charged to the replan budget, so combat/rest stops no longer spend it.
bool NewRpgBaseAction::MoveFarToIntentV2(WorldPosition const& requestedDest, bool questNoTeleport, bool* outStuck,
                                         bool deterministicPath,
                                         StrictFinisherMovementPolicy::RouteIdentity strictRoute)
{
    // AutoWow.Travel.VerticalSnap (NavmeshSnap.h): the goal snapped onto its floor's navmesh, and every path
    // below computed without the slope check. OFF: dest == requestedDest and the slope check stays on.
    bool const verticalSnap = AutoWowVerticalSnap::Enabled();
    WorldPosition const dest = verticalSnap ? SnapTravelGoal(bot, requestedDest) : requestedDest;
    using namespace TravelIntentPolicy;
    Intent& intent = botAI->rpgInfo.travelIntent;
    Params const params{sPlayerbotAIConfig.autoWowTravelIntentReplanFailCount,
                        sPlayerbotAIConfig.autoWowTravelIntentProgressWindowMs,
                        sPlayerbotAIConfig.autoWowTravelIntentHysteresisPct};
    uint32 const now = getMSTime();
    uint32 const botGuid = bot->GetGUID().GetCounter();
    Point const goal = MakePoint(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    Point const here = MakePoint(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // AutoWow.Survival.HardEscape (2): a point in a zone bracketed WalkZoneMargin above the bot (not the bot's
    // own zone) is dangerous too. OFF: hardEscape is false and `dangerous` is the death-loop query alone.
    bool const hardEscape = AutoWowDeathLoop::HardEscapeEnabled();
    uint32 const botZone = bot->GetZoneId();
    auto dangerous = [&](Point const& p)
    {
        if (AutoWowDeathLoop::Enabled() &&
            AutoWowDeathLoop::IsDangerous(botGuid, p.mapId, static_cast<float>(p.x), static_cast<float>(p.y)))
            return true;
        if (!hardEscape || p.mapId != bot->GetMapId())
            return false;
        uint32 const zone = AutoWowDeathLoop::ZoneAt(bot->GetMap(), static_cast<float>(p.x), static_cast<float>(p.y));
        return WalkingV2Policy::ZoneDanger(zone, botZone, AutoWowDeathLoop::ZoneMinLevel(zone), bot->GetLevel(),
                                           AutoWowDeathLoop::detail::gHardParams.walkZoneMargin);
    };
    auto recordStrict = [&]()
    {
        if (!deterministicPath)
            return;
        LastMovement& issued = AI_VALUE(LastMovement&, "last movement");
        botAI->rpgInfo.strictFinisherMovement = {
            true,
            strictRoute,
            {true, issued.msTime, issued.lastMoveToMapId, issued.lastMoveToX,
             issued.lastMoveToY, issued.lastMoveToZ}};
    };
    auto giveUp = [&](GiveUp reason) -> bool
    {
        LOG_INFO("playerbots", "[New RPG] AutoWow {} travel intent gives up ({}) toward ({},{},{},{}) at ({},{}) "
                 "best={} segments={} failures={} walking=v2",
                 bot->GetName(), reason == GiveUp::NoProgress ? "intent_no_progress" : "intent_replan_exhausted",
                 dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                 bot->GetPositionX(), bot->GetPositionY(), intent.bestGoalYards, intent.segmentsCommitted,
                 intent.failures);
        if (AutoWowUnstick::Enabled() && reason == GiveUp::ReplanExhausted)
            AutoWowUnstick::NoteReplanExhausted(botGuid);
        Abandon(intent, reason);
        if (outStuck)
            *outStuck = true;
        return questNoTeleport;
    };
    // Commits `end` as the segment and walks it; a refused MoveTo drops the commitment again.
    auto walkSegment = [&](G3D::Vector3 const& end, Point const& candidate) -> bool
    {
        Commit(intent, here, candidate, DistanceYards(candidate, goal));
        if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, true))
        {
            recordStrict();
            return true;
        }
        DropSegment(intent);
        return false;
    };

    if (Observe(intent, goal, DistanceYards(here, goal), now, params) == Verdict::NoProgress)
        return giveUp(GiveUp::NoProgress);

    bool const moving = IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL) || bot->isMoving();
    bool const inCombat = bot->IsInCombat();

    if (bot->GetExactDist(dest) < pathFinderDis)
    {
        if (moving)
            return true;
        bool const moved = MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                  false, false, false, true);
        if (moved)
            recordStrict();
        return moved;
    }

    switch (ClassifySegment(intent, here, moving, inCombat || (intent.hasSegment && dangerous(intent.segment))))
    {
        case SegmentState::Follow:
            return true;
        case SegmentState::Reached:
            CompleteSegment(intent);
            break;
        case SegmentState::Unsafe:
            DropSegment(intent);
            if (inCombat)
                return false;
            break;
        case SegmentState::Interrupted:
            DropSegment(intent);  // replan from here; not a failure until the replan finds nothing
            break;
        case SegmentState::None:
            if (ReplanCoolingDown(intent, now))
                return true;
            break;
    }

    // 0s. AutoWow.Travel.Safe: the same fan, but each admissible chunk is scored by the idle hostile mobs
    //     its mmap path passes (WalkingV2Policy PickChunk: first clear chunk, else the least threatened),
    //     and a chunk path through a death-loop danger area is not admissible. OFF = step 0 unchanged.
    if (sPlayerbotAIConfig.autoWowTravelSafe && WalkingV2Policy::UseChunks(here, goal))
    {
        Map* map = bot->GetMap();
        std::vector<WalkingV2Policy::ChunkChoice> choices;
        std::vector<G3D::Vector3> ends;
        std::vector<WalkingV2Policy::Mob> mobs;
        bool mobsScanned = false;
        for (std::size_t k = 0; k < WalkingV2Policy::kChunkProbes; ++k)
        {
            Point const probe = WalkingV2Policy::ChunkProbe(here, goal, k);
            float const px = static_cast<float>(probe.x);
            float const py = static_cast<float>(probe.y);
            float pz = map->GetHeight(bot->GetPhaseMask(), px, py, bot->GetPositionZ() + 50.0f, true, 100.0f);
            if (!std::isfinite(pz) || pz <= INVALID_HEIGHT)
                pz = bot->GetPositionZ();
            PathGenerator path(bot);
            path.SetSlopeCheck(!verticalSnap);
            path.CalculatePath(px, py, pz);
            choices.emplace_back();
            ends.push_back(path.GetActualEndPosition());
            if (path.GetPathType() & ~typeOk)
                continue;
            Point const candidate = MakePoint(bot->GetMapId(), ends.back().x, ends.back().y, ends.back().z);
            std::vector<Point> points;
            for (G3D::Vector3 const& v : path.GetPath())
                points.push_back(MakePoint(bot->GetMapId(), v.x, v.y, v.z));
            bool danger = dangerous(candidate);
            // ponytail: every 4th path point (~16 yd) against the 60 yd danger circles, one breaker lock each.
            for (std::size_t i = 0; !danger && i < points.size(); i += 4)
                danger = dangerous(points[i]);
            if (!WalkingV2Policy::AdmitChunk(here, candidate, goal, danger))
                continue;
            if (!mobsScanned)
            {
                mobs = ScanTravelMobs(bot);
                mobsScanned = true;
            }
            // AutoWow.Survival.PackAvoid: packs by the path count at any level, weighted by their size.
            choices.back() = {true, AutoWowPackAvoid::Enabled()
                                        ? AutoWowPackAvoid::PackPathThreat(points, mobs, bot->GetLevel(),
                                                                           AutoWowPackAvoid::Get().linkYards)
                                        : WalkingV2Policy::PathThreat(points, mobs, bot->GetLevel())};
            if (choices.back().threat == 0)
                break;  // PickChunk takes the first clear chunk
        }
        std::size_t const pick = WalkingV2Policy::PickChunk(choices);
        if (pick != WalkingV2Policy::kNoChunk &&
            walkSegment(ends[pick], MakePoint(bot->GetMapId(), ends[pick].x, ends[pick].y, ends[pick].z)))
            return true;
    }
    // 0. Chunked walk: path each chunk probe on its own, commit the first goal-monotone endpoint.
    else if (WalkingV2Policy::UseChunks(here, goal))
    {
        Map* map = bot->GetMap();
        for (std::size_t k = 0; k < WalkingV2Policy::kChunkProbes; ++k)
        {
            Point const probe = WalkingV2Policy::ChunkProbe(here, goal, k);
            float const px = static_cast<float>(probe.x);
            float const py = static_cast<float>(probe.y);
            // ponytail: probe height from vmap/grid below bot z + 50; the navmesh end-poly search spans
            // 200 yd vertically anyway, so a miss falls back to the bot's own z.
            float pz = map->GetHeight(bot->GetPhaseMask(), px, py, bot->GetPositionZ() + 50.0f, true, 100.0f);
            if (!std::isfinite(pz) || pz <= INVALID_HEIGHT)
                pz = bot->GetPositionZ();
            PathGenerator path(bot);
            path.SetSlopeCheck(!verticalSnap);
            path.CalculatePath(px, py, pz);
            if (path.GetPathType() & ~typeOk)
                continue;
            G3D::Vector3 const endPos = path.GetActualEndPosition();
            Point const candidate = MakePoint(bot->GetMapId(), endPos.x, endPos.y, endPos.z);
            if (WalkingV2Policy::AdmitChunk(here, candidate, goal, dangerous(candidate)) &&
                walkSegment(endPos, candidate))
                return true;
        }
    }

    auto admit = [&](G3D::Vector3 const& end) -> bool
    {
        Point const candidate = MakePoint(bot->GetMapId(), end.x, end.y, end.z);
        uint32 const candidateGoalYards = DistanceYards(candidate, goal);
        if (!Admit(intent, here, candidate, candidateGoalYards, dangerous(candidate), params))
            return false;
        Commit(intent, here, candidate, candidateGoalYards);
        return true;
    };

    // 1. Prepared quest walk (as MoveFarToIntent).
    if (questNoTeleport)
    {
        AutoWowQuestGiverTravel::QuestWalkProbeSelection selection =
            AutoWowQuestGiverTravel::SelectQuestWalkProbeDetailed(bot, dest);
        if (selection.probe && !selection.probe->path.empty() && admit(selection.probe->path.back()))
        {
            AutoWowDungeonWalkAction walk(botAI);
            selection.diagnostics.walkPreparedCalled = true;
            AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason rejectReason =
                AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::None;
            bool const accepted = walk.WalkPrepared(*selection.probe, &rejectReason);
            selection.diagnostics.walkPreparedAccepted = accepted;
            selection.diagnostics.walkRejectReason = rejectReason;
            AutoWowQuestGiverTravel::RecordQuestWalkDiagnostics(botGuid, selection.diagnostics);
            if (accepted)
            {
                recordStrict();
                return true;
            }
            DropSegment(intent);
        }
    }

    // 2. mmap route to the true destination (as MoveFarToIntent).
    {
        PathGenerator path(bot);
        path.SetSlopeCheck(!verticalSnap);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (!(path.GetPathType() & ~typeOk))
        {
            G3D::Vector3 const endPos = path.GetActualEndPosition();
            if (dest.GetExactDist(endPos.x, endPos.y, endPos.z) + 5.0f < bot->GetDistance(dest) && admit(endPos))
            {
                if (MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true))
                {
                    recordStrict();
                    return true;
                }
                DropSegment(intent);
            }
        }
    }

    // 3. Nothing admissible from here: one failed replan.
    if (NoteFailure(intent, now, params))
        return giveUp(GiveUp::ReplanExhausted);
    return false;
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest, bool questNoTeleport, bool* outStuck,
                                 bool deterministicPath,
                                 StrictFinisherMovementPolicy::RouteIdentity strictRoute,
                                 bool allowLegacyTeleportRecovery)
{
    if (outStuck)
        *outStuck = false;

    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    if (dest != botAI->rpgInfo.moveFarPos)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // AutoWow.Survival.Unstick (default 0): a bot frozen on its travel goal hearths home, or portals out of a
    // navmesh hole (SurvivalRecovery.h). OFF = nothing observed.
    if (AutoWowUnstick::Enabled() &&
        AutoWowUnstick::Step(botAI, dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()))
        return true;

    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    if (deterministicPath)
    {
        if (!strictRoute.IsValid())
        {
            if (outStuck)
                *outStuck = true;
            return false;
        }

        StrictFinisherMovementPolicy::LastMovementFacts const observed{
            bot->isMoving() || lastMovement.msTime != 0,
            lastMovement.msTime,
            lastMovement.lastMoveToMapId,
            lastMovement.lastMoveToX,
            lastMovement.lastMoveToY,
            lastMovement.lastMoveToZ};
        StrictFinisherMovementPolicy::Decision const inherited =
            StrictFinisherMovementPolicy::Evaluate(
                strictRoute, observed, botAI->rpgInfo.strictFinisherMovement);
        if (inherited == StrictFinisherMovementPolicy::Decision::RejectAndClearInherited)
        {
            if (bot->isMoving())
                bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            lastMovement.clear();
            botAI->rpgInfo.strictFinisherMovement = {};
        }
        else if (inherited == StrictFinisherMovementPolicy::Decision::NoInheritedMovement)
        {
            botAI->rpgInfo.strictFinisherMovement = {};
        }
    }

    // AutoWow.TravelIntent.Enable: no-teleport travellers commit to a route segment instead of
    // re-selecting (and re-arming the stuck counter) every tick. OFF = the legacy path below.
    if (sPlayerbotAIConfig.autoWowTravelIntent && bot->GetMapId() == dest.GetMapId() &&
        (questNoTeleport || deterministicPath || IsAutoWowTravelBot()))
        return MoveFarToIntent(dest, questNoTeleport, outStuck, deterministicPath, strictRoute);

    auto issueMove = [&](uint32 mapId, float x, float y, float z)
    {
        bool const moved = MoveTo(mapId, x, y, z, false, false, false, true);
        if (moved && deterministicPath)
        {
            LastMovement& issued = AI_VALUE(LastMovement&, "last movement");
            botAI->rpgInfo.strictFinisherMovement = {
                true,
                strictRoute,
                {true, issued.msTime, issued.lastMoveToMapId, issued.lastMoveToX,
                 issued.lastMoveToY, issued.lastMoveToZ}};
        }
        return moved;
    };

    // A direct quest point can be a valid ground coordinate while still being outside the local
    // mmap corridor. Reuse the staged mover's exact-path proof: complete direct walk first, then a
    // bounded TravelMgr re-anchor whose candidate is freshly probed from the live bot position.
    // Strict Oracle finishers may use this only as an intermediate segment: the requested
    // destination and strict route identity remain unchanged, while every executed segment still
    // comes from the same fresh no-teleport probe.
    auto tryPreparedQuestWalk = [&](bool force = false) -> bool
    {
        if (!bot->IsInWorld() || bot->GetMapId() != dest.GetMapId())
            return false;

        constexpr uint32 questWalkProbeCooldownMs = 5000;
        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const lastProbeTs = questWalkProbeTsByBot[botGuid];
        if (!force && lastProbeTs != 0 && GetMSTimeDiffToNow(lastProbeTs) < questWalkProbeCooldownMs)
            return false;

        questWalkProbeTsByBot[botGuid] = getMSTime();
        AutoWowQuestGiverTravel::QuestWalkProbeSelection selection =
            AutoWowQuestGiverTravel::SelectQuestWalkProbeDetailed(bot, dest);
        if (!selection.probe)
            return false;

        AutoWowDungeonWalkAction walk(botAI);
        selection.diagnostics.walkPreparedCalled = true;
        AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason rejectReason =
            AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::None;
        bool const accepted = walk.WalkPrepared(*selection.probe, &rejectReason);
        selection.diagnostics.walkPreparedAccepted = accepted;
        selection.diagnostics.walkRejectReason = rejectReason;
        AutoWowQuestGiverTravel::RecordQuestWalkDiagnostics(botGuid, selection.diagnostics);
        if (!accepted)
            return false;

        if (deterministicPath)
        {
            // Preserve the strict handoff across the next AI tick. Without this provenance record,
            // the next deterministic MoveFarTo call would treat its own prepared spline as an
            // unrelated inherited move and clear it before it could finish.
            LastMovement& issued = AI_VALUE(LastMovement&, "last movement");
            botAI->rpgInfo.strictFinisherMovement = {
                true,
                strictRoute,
                {true, issued.msTime, issued.lastMoveToMapId, issued.lastMoveToX,
                 issued.lastMoveToY, issued.lastMoveToZ}};
        }

        botAI->rpgInfo.nearestMoveFarDis = bot->GetExactDist(dest);
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        G3D::Vector3 const& endpoint = selection.probe->path.back();
        LOG_DEBUG("playerbots", "[New RPG] {} walk-only quest route toward {} via ({},{},{}) mode {}",
                  bot->GetName(), dest.GetMapId(), endpoint.x, endpoint.y, endpoint.z,
                  selection.probe->mode);
        return true;
    };

    bool const autoWowTravel = IsAutoWowTravelBot();
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    bool const watchDestination = autoWowTravel && !questNoTeleport && !deterministicPath &&
        (status == RPG_GO_GRIND || status == RPG_GO_CAMP);

    // Ordinary Playerbots retain their historical movement timing. For autonomous AutoWow bots,
    // evaluate destination progress even while a spline is active: repeatedly reissued partial
    // paths can otherwise keep the wait gate true forever and hide a genuine oscillation.
    if (!watchDestination && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    {
        if (!watchDestination && bot->isMoving() && lastMovement.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMovement.lastMoveToX, lastMovement.lastMoveToY,
                                                lastMovement.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    bool stalled = false;
    if (watchDestination)
        stalled = AutonomousRpgTravelPolicy::ObserveProgress(
            disToDest, botAI->rpgInfo.nearestMoveFarDis, botAI->rpgInfo.stuckTs,
            botAI->rpgInfo.stuckAttempts,
            lastAutonomousTravelTickByBot[bot->GetGUID().GetCounter()], getMSTime());
    else if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
        stalled = true;
    if (stalled)
    {
        // No meaningful progress toward dest for `stuckTime`. Ordinary
        // Playerbots may use the historical teleport fallback, but the
        // AutoWow league must expose the failure to its external recovery
        // loop rather than silently changing world position.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        // No-teleport hold when either:
        //   - this is a quest-path move (objective source / finisher travel), so
        //     the quest never changes world position by teleport, or
        //   - the AutoWow league policy forbids teleport for this bot.
        // The quest path additionally surfaces the stall via `outStuck` so the
        // caller can raise a typed MovementStuckNoTeleport blocker for the
        // external recovery loop instead of silently spinning.
        bool const policyNoTeleport = AutoWowPolicy::IsNoTeleport(bot->GetGUID().GetCounter());
        if (questNoTeleport || autoWowTravel || (policyNoTeleport && !allowLegacyTeleportRecovery))
        {
            if (questNoTeleport && tryPreparedQuestWalk(true))
                return true;

            LOG_DEBUG("playerbots", "[New RPG] {} no-teleport hold for {} at ({},{},{},{}) toward ({},{},{},{})",
                      questNoTeleport ? "quest-path" : "AutoWow", bot->GetName(), bot->GetPositionX(),
                      bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), dest.GetPositionX(),
                      dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId());
            if (outStuck)
                *outStuck = true;
            return !watchDestination;
        }

        // Keep the historical fallback for non-AutoWow, non-quest bots.
        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        // Acceptance guard: a no-teleport (quest-path / AutoWow) bot is held above and must never reach a
        // real teleport. Counting here catches any regression that bypasses that hold.
        if (questNoTeleport || autoWowTravel || (policyNoTeleport && !allowLegacyTeleportRecovery))
            AutoWowAcceptance::NoteTeleport();
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "rpg_stuck_teleport");
        return bot->TeleportTo(dest);
    }

    if (watchDestination)
    {
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
            return true;
        if (bot->isMoving() && lastMovement.lastMoveToMapId == bot->GetMapId())
        {
            float const remaining = bot->GetExactDist(lastMovement.lastMoveToX,
                                                       lastMovement.lastMoveToY,
                                                       lastMovement.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    float dis = bot->GetExactDist(dest);
    if (questNoTeleport && tryPreparedQuestWalk())
        return true;

    if (dis < pathFinderDis)
    {
        return issueMove(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        // Keep the runtime route calculation aligned with the read-only path probe. Without
        // slope checking the two can disagree on steep terrain: the probe authorizes a grounded
        // route while this executor returns a partial/no-progress result and never commits a
        // meaningful waypoint.
        path.SetSlopeCheck(true);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return issueMove(bot->GetMapId(), endPos.x, endPos.y, endPos.z);
            }
        }
    }

    if (questNoTeleport && tryPreparedQuestWalk())
        return true;

    // A strict exact-finisher route must never substitute a randomized stepping stone for the
    // verified destination. The caller owns the typed failure/blocked transition.
    if (deterministicPath)
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    // An autonomous bot must reject an unreachable destination so its RPG status can choose
    // another activity. Random forward-cone steps can repeatedly return it to the same obstacle.
    if (watchDestination)
    {
        if (outStuck)
            *outStuck = true;
        return false;
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.SetSlopeCheck(true);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return issueMove(bot->GetMapId(), rx, ry, rz);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*, Position const* anchor)
{
    // Random movement can never be inherited as exact-finisher movement, even if it happened to
    // use the same map and endpoint in the same update.
    botAI->rpgInfo.strictFinisherMovement = {};
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = anchor ? anchor->GetPositionX() : bot->GetPositionX();
    const float y = anchor ? anchor->GetPositionY() : bot->GetPositionY();
    const float z = anchor ? anchor->GetPositionZ() : bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

bool NewRpgBaseAction::TryRelieveInventoryAtVendor(NewRpgInfo::DoQuest& data, bool incompleteItem)
{
    QuestObjectiveRuntime& runtime = data.objectiveRuntime;
    if (runtime.failure != QuestFailureReason::InventoryFull || !bot->IsAlive() || bot->IsInCombat())
        return false;

    // The incomplete-item caller has just observed 100% occupied bag slots. Its explicit
    // capacity fact is stronger than the cached general maintenance "should sell" value.
    if (!incompleteItem && !AI_VALUE(bool, "should sell"))
        return false;

    bool const canSell = AI_VALUE(bool, "can sell");
    bool const canSellGray = AI_VALUE(bool, "can sell gray");
    if (!incompleteItem && !canSell && !canSellGray)
        return false;

    // Only an incomplete collect-item quest with no safe gray sale can buy a bag.
    // The bag goes straight to an empty equipment slot, so a full backpack is valid.
    uint8 emptyBagSlot = NULL_SLOT;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            emptyBagSlot = slot;
            break;
        }
    uint32 const botGuid = bot->GetGUID().GetCounter();
    auto const purchaseAttempt = lastQuestBagPurchaseAttemptByBot.find(botGuid);
    bool const purchaseCoolingDown = purchaseAttempt != lastQuestBagPurchaseAttemptByBot.end() &&
        GetMSTimeDiffToNow(purchaseAttempt->second) < QuestInventoryReliefPolicy::BagPurchaseCooldownMs;
    context->GetValue<uint8>("bag space")->Reset();
    bool const buyBag = QuestInventoryReliefPolicy::ShouldTryBagPurchase(
        incompleteItem, bot->IsAlive(), bot->IsInCombat(), AI_VALUE(uint8, "bag space"),
        canSellGray, emptyBagSlot != NULL_SLOT, purchaseCoolingDown);
    if (incompleteItem && !canSellGray && !buyBag)
        return false;

    auto findBagOffer = [&](Creature* vendor) -> std::optional<QuestBagOffer>
    {
        VendorItemData const* offers = vendor ? vendor->GetVendorItems() : nullptr;
        if (!offers)
            return std::nullopt;
        for (uint32 vendorSlot = 0; vendorSlot < offers->GetItemCount(); ++vendorSlot)
        {
            VendorItem const* offer = offers->GetItem(vendorSlot);
            ItemTemplate const* proto = offer ? sObjectMgr->GetItemTemplate(offer->item) : nullptr;
            if (!proto)
                continue;
            bool const ordinaryBag = proto->Class == ITEM_CLASS_CONTAINER &&
                proto->SubClass == ITEM_SUBCLASS_CONTAINER && proto->InventoryType == INVTYPE_BAG;
            if (!ordinaryBag || proto->ContainerSlots < QuestInventoryReliefPolicy::MinBagSlots ||
                proto->BuyCount != 1 || offer->ExtendedCost != 0 || proto->BuyPrice == 0 ||
                proto->BuyPrice > QuestInventoryReliefPolicy::MaxBagPriceCopper)
                continue;
            bool const available = offer->maxcount == 0 ||
                vendor->GetVendorItemCurrentCount(offer) >= proto->BuyCount;
            uint32 const price = QuestInventoryReliefPolicy::DiscountedBagPrice(
                proto->BuyPrice, bot->GetReputationPriceDiscount(vendor));
            if (!QuestInventoryReliefPolicy::AcceptBagOffer(
                    ordinaryBag, proto->ContainerSlots, proto->BuyCount, offer->ExtendedCost,
                    available, proto->BuyPrice, price, bot->GetMoney()))
                continue;

            uint16 destination = 0;
            if (bot->CanEquipNewItem(emptyBagSlot, destination, proto->ItemId, false) != EQUIP_ERR_OK)
                continue;
            return QuestBagOffer{proto->ItemId, vendorSlot, price};
        }
        return std::nullopt;
    };

    GuidVector const vendors = AI_VALUE(GuidVector, "possible new rpg targets");
    ObjectGuid vendorGuid;
    WorldObject* vendorObject = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();

    for (ObjectGuid const& candidateGuid : vendors)
    {
        WorldObject* candidate = ObjectAccessor::GetWorldObject(*bot, candidateGuid);
        Creature* creature = candidate ? candidate->ToCreature() : nullptr;
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() ||
            !creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
            continue;
        if (incompleteItem &&
            (creature->GetZoneId() != bot->GetZoneId() || !bot->IsFriendlyTo(creature) ||
             !sObjectMgr->GetNpcVendorItemList(creature->GetEntry())))
            continue;
        if (buyBag && !findBagOffer(creature))
            continue;

        float const distance = bot->GetExactDist(creature);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            vendorGuid = candidateGuid;
            vendorObject = creature;
        }
    }

    if (!vendorObject)
    {
        // Bag purchases require a live nearby vendor and a checked offer; a distant generic
        // vendor route is not sufficient evidence of stock, price, or an interactable NPC.
        if (buyBag)
            return false;
        // The vendor need not be loaded yet. Prefer the existing RPG travel catalogue; the
        // spawn-catalog fallback below also uses real core creature data and normal walking.
        WorldPosition botPosition(bot);
        WorldPosition vendorPosition;
        float nearestDestinationDistance = std::numeric_limits<float>::max();

        for (TravelDestination* destination : TravelMgr::instance().getRpgTravelDestinations(
                 bot, /*ignoreFull*/ true, /*ignoreInactive*/ true))
        {
            RpgTravelDestination* rpgDestination = dynamic_cast<RpgTravelDestination*>(destination);
            CreatureTemplate const* proto = rpgDestination ? rpgDestination->GetCreatureTemplate() : nullptr;
            if (!rpgDestination || !proto || !(proto->npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;
            if (incompleteItem)
            {
                FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(proto->faction);
                if (!sObjectMgr->GetNpcVendorItemList(proto->Entry) || !faction ||
                    Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), faction) < REP_NEUTRAL)
                    continue;
            }

            std::vector<WorldPosition*> points = destination->nextPoint(&botPosition, true);
            if (points.empty() || !points.front() || points.front()->GetMapId() != bot->GetMapId())
                continue;
            if (incompleteItem &&
                bot->GetMap()->GetZoneId(bot->GetPhaseMask(), points.front()->GetPositionX(),
                                         points.front()->GetPositionY(), points.front()->GetPositionZ()) !=
                    bot->GetZoneId())
                continue;

            float const distance = botPosition.distance(*points.front());
            if (incompleteItem && distance > maxVendorReliefDistance)
                continue;
            if (distance < nearestDestinationDistance)
            {
                nearestDestinationDistance = distance;
                vendorPosition = *points.front();
            }
        }

        if (vendorPosition == WorldPosition())
        {
            // The RPG travel cache can be empty in an Oracle-only world. Use the core's
            // already-loaded spawn catalog to find a real nearby, same-zone vendor.
            // Cache the result per bot for one minute; scanning every quest tick is unnecessary.
            uint32 const botGuid = bot->GetGUID().GetCounter();
            VendorReliefCandidate& cached = vendorReliefCandidatesByBot[botGuid];
            if (cached.checkedAtMs && cached.mapId == bot->GetMapId() &&
                cached.zoneId == bot->GetZoneId() &&
                cached.phaseMask == bot->GetPhaseMask() &&
                GetMSTimeDiffToNow(cached.checkedAtMs) < vendorReliefCacheMs)
            {
                vendorPosition = cached.pos;
            }
            else
            {
                cached = {bot->GetMapId(), bot->GetZoneId(), bot->GetPhaseMask(),
                          getMSTime(), WorldPosition()};
                for (auto const& [spawnId, spawn] : sObjectMgr->GetAllCreatureData())
                {
                    (void)spawnId;
                    if (spawn.mapid != bot->GetMapId() || !(spawn.phaseMask & bot->GetPhaseMask()) ||
                        !(spawn.spawnMask & (1u << bot->GetMap()->GetSpawnMode())))
                        continue;

                    CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(spawn.id);
                    if (!proto)
                        continue;

                    bool const vendorFlag =
                        ((spawn.npcflag ? spawn.npcflag : proto->npcflag) & UNIT_NPC_FLAG_VENDOR) != 0;
                    if (!vendorFlag)
                        continue;
                    bool const stocked = sObjectMgr->GetNpcVendorItemList(spawn.id) != nullptr;
                    if (!stocked)
                        continue;

                    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(proto->faction);
                    bool const friendly = faction &&
                        Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), faction) >= REP_NEUTRAL;
                    if (!friendly)
                        continue;
                    uint32 const zone = bot->GetMap()->GetZoneId(bot->GetPhaseMask(),
                                                                  spawn.posX, spawn.posY, spawn.posZ);
                    if (!QuestInventoryReliefPolicy::AcceptVendorSpawn(
                            bot->GetMapId(), bot->GetZoneId(), bot->GetPhaseMask(),
                            spawn.mapid, zone, spawn.phaseMask,
                            vendorFlag, stocked, friendly))
                        continue;

                    WorldPosition const candidate(spawn.mapid, spawn.posX, spawn.posY, spawn.posZ);
                    float const distance = botPosition.distance(candidate);
                    if (distance < nearestDestinationDistance && distance <= maxVendorReliefDistance)
                    {
                        nearestDestinationDistance = distance;
                        vendorPosition = candidate;
                    }
                }
                cached.pos = vendorPosition;
            }
        }

        if (vendorPosition == WorldPosition())
            return false;

        LOG_DEBUG("playerbots", "[New RPG] {} quest {} inventory relief routing to vendor at ({},{},{})",
                  bot->GetName(), data.questId, vendorPosition.GetPositionX(), vendorPosition.GetPositionY(),
                  vendorPosition.GetPositionZ());
        bool stuck = false;
        if (!MoveFarTo(vendorPosition, /*questNoTeleport*/ incompleteItem, &stuck) && stuck)
            ForceToWait(500);
        return true;
    }

    if (!IsWithinInteractionDist(vendorObject))
    {
        if (!MoveWorldObjectTo(vendorGuid))
            ForceToWait(250);
        return true;
    }

    if (incompleteItem && !bot->GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR))
        return false;

    if (buyBag)
    {
        // Recheck the actual state at interaction time. The first full-bag sample can be old
        // after travel, and a normal gray sale always takes priority if one became possible.
        context->GetValue<uint8>("bag space")->Reset();
        if (AI_VALUE(uint8, "bag space") < 100)
        {
            runtime.failure = QuestFailureReason::None;
            runtime.phase = QuestActionPhase::ResolveObjective;
            data.lastReachPOI = 0;
            return true;
        }
        context->GetValue<bool>("can sell gray")->Reset();
        if (!AI_VALUE(bool, "can sell gray"))
        {
            if (emptyBagSlot == NULL_SLOT ||
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, emptyBagSlot))
                return false;

            std::optional<QuestBagOffer> offer = findBagOffer(vendorObject->ToCreature());
            if (!offer)
                return false;
            uint16 destination = 0;
            if (bot->CanEquipNewItem(emptyBagSlot, destination, offer->itemId, false) != EQUIP_ERR_OK)
                return false;

            uint32 const moneyBefore = bot->GetMoney();
            lastQuestBagPurchaseAttemptByBot[botGuid] = getMSTime();
            bot->GetSession()->SetCurrentVendor(0);
            // Core chooses CanEquipNewItem/EquipNewItem for this equipment destination,
            // and handles price, stock, vendor access, and item creation normally.
            bot->BuyItemFromVendorSlot(vendorGuid, offer->vendorSlot, offer->itemId, 1,
                                       INVENTORY_SLOT_BAG_0, emptyBagSlot);
            Item* const equippedBag = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, emptyBagSlot);
            uint32 const moneyAfter = bot->GetMoney();
            context->GetValue<uint8>("bag space")->Reset();
            if (!equippedBag || equippedBag->GetEntry() != offer->itemId ||
                moneyAfter > moneyBefore || moneyAfter < QuestInventoryReliefPolicy::MoneyReserveCopper ||
                moneyBefore - moneyAfter != offer->priceCopper ||
                AI_VALUE(uint8, "bag space") >= 100)
            {
                LOG_ERROR("playerbots", "[New RPG] {} quest {} bag relief purchase did not verify",
                          bot->GetName(), data.questId);
                return false;
            }

            LOG_INFO("playerbots", "[New RPG] {} quest {} bought bag {} into slot {} for {} copper",
                     bot->GetName(), data.questId, offer->itemId, emptyBagSlot, moneyBefore - moneyAfter);
            runtime.failure = QuestFailureReason::None;
            runtime.phase = QuestActionPhase::ResolveObjective;
            data.lastReachPOI = 0;
            runtime.attemptCount = 0;
            return true;
        }
    }

    std::string const mode = incompleteItem ? "autowow-gray" : (canSell ? "vendor" : "autowow-gray");
    LOG_DEBUG("playerbots", "[New RPG] {} quest {} inventory relief selling mode={}", bot->GetName(), data.questId,
              mode);
    if (!botAI->DoSpecificAction("sell", Event("autowow inventory relief", mode), true))
    {
        ForceToWait(500);
        return true;
    }

    // The item source must be re-resolved after a vendor detour. Reward recovery retains its
    // existing exact-finisher retry; neither path credits the quest or creates an item.
    runtime.failure = QuestFailureReason::None;
    runtime.phase = incompleteItem ? QuestActionPhase::ResolveObjective : QuestActionPhase::InteractFinisher;
    data.lastReachPOI = 0;
    runtime.attemptCount = 0;
    runtime.selectedRewardIndexKnown = false;
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->rpgStatistic.questRewarded++;
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::CanInteractWithQuestGiver(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT: // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT: // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        // AutoWow.Gear.Upgrades: no equip upgrade among the choices - take the one that sells for most (gold
        // for the vendor weapon errand) instead of the stat score of items the bot will never wear.
        if (AutoWowGear::Enabled() && bestUsage != ITEM_USAGE_EQUIP)
        {
            std::vector<std::uint32_t> prices;
            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                prices.push_back(proto ? proto->SellPrice : 0);
            }
            return static_cast<uint32>(AutoWowGear::MostValuableChoice(prices));
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                float score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + 3 < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // AutoWow.Party.Enable: in a cohort party, group quests up to the party size (and dungeon quests for a
    // dungeon party) are doable; the avoid list still applies. Flag off / no party: the solo rule below.
    bool dungeonParty = false;
    if (std::uint32_t const partySize = AutoWowParty::Enabled() ?
            AutoWowParty::PartySize(bot->GetGUID().GetCounter(), &dungeonParty) : 0)
        return AutoWowParty::QuestCapableInParty(quest->GetType(), quest->GetSuggestedPlayers(), partySize,
                                                 dungeonParty) &&
               !QuestSchedulerPolicy::ContainsQuestId(sPlayerbotAIConfig.autoWowQuestAvoidIds, quest->GetQuestId());

    // Elite quest and dungeon quest etc
    if (quest->GetType() != 0)
        return false;

    // now we only capable of doing solo quests
    if (quest->GetSuggestedPlayers() >= 2)
        return false;

    // AutoWow.QuestAvoidIds / QuestAvoidFile (default empty): never accept, choose or keep these.
    if (QuestSchedulerPolicy::ContainsQuestId(sPlayerbotAIConfig.autoWowQuestAvoidIds, quest->GetQuestId()))
        return false;

    return true;
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) ||
            bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, questId, "drop_unworthy_or_failed");
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        if (quest->GetZoneOrSort() < 0 || (quest->GetZoneOrSort() > 0 && quest->GetZoneOrSort() != botZoneId))
        {
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, questId, "drop_off_zone");
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Deferred, questId, "drop_log_clear");
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->rpgStatistic.questDropped++;
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    if (ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract(true, 80.0f))
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
        if (bot->CanInteractWithQuestGiver(object))
        {
            InteractWithNpcOrGameObjectForQuest(npcOrGo);
            ForceToWait(5000);
            return true;
        }
        return MoveWorldObjectTo(npcOrGo);
    }
    return false;
}

ObjectGuid NewRpgBaseAction::ChooseNpcOrGameObjectToInteract(bool questgiverOnly, float distanceLimit)
{
    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");

    if (possibleTargets.empty() && possibleGameObjects.empty())
        return ObjectGuid();

    WorldObject* nearestObject = nullptr;
    for (ObjectGuid& guid : possibleTargets)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    for (ObjectGuid& guid : possibleGameObjects)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    if (nearestObject)
        return nearestObject->GetGUID();

    // No questgiver to accept or reward
    if (questgiverOnly)
        return ObjectGuid();

    if (possibleTargets.empty())
        return ObjectGuid();

    int idx = urand(0, possibleTargets.size() - 1);
    ObjectGuid guid = possibleTargets[idx];
    WorldObject* object = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
    if (!object)
        object = ObjectAccessor::GetGameObject(*bot, guid);

    if (object && object->IsInWorld())
    {
        return object->GetGUID();
    }
    return ObjectGuid();
}

bool NewRpgBaseAction::HasQuestToAcceptOrReward(WorldObject* object)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            return true;
        }
    }
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            return true;
        }
    }
    return false;
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    if (IsAutoWowTravelBot())
    {
        WorldPosition dest = SelectReachableAutoWowTravelPos(bot, locs, 60.0f);
        // AutoWow.Unstick.V2: nothing within 250 yd (soak-s51 town trap: 0-1 spots near Shadowprey Village)
        // -> the next 250-yd band out, nearest first, up to GrindMaxYards. A long target is walked in the
        // selector's grounded partial segments.
        // ponytail: up to 8 path probes per band per call; a per-bot cache if town bots show up in perf.
        if (dest == WorldPosition() && AutoWowUnstickV2::Enabled())
            for (auto const& [lo, hi] : AutoWowUnstickV2::GrindBands(AutoWowUnstickV2::detail::gParams.grindMaxYards))
            {
                dest = SelectReachableAutoWowTravelPos(bot, locs, float(lo), float(hi));
                if (dest != WorldPosition())
                    break;
            }
        LOG_DEBUG("playerbots", "[New RPG] AutoWow {} selected grounded grind destination ({},{},{},{})",
                  bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
                  dest.GetPositionZ());
        return dest;
    }
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (AutoWowDeathLoop::Enabled() && AutoWowDeathLoop::IsDangerous(bot->GetGUID().GetCounter(),
                loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY()))
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);
    if (IsAutoWowTravelBot())
    {
        WorldPosition const dest = SelectReachableAutoWowTravelPos(bot, locs, 50.0f);
        LOG_DEBUG("playerbots", "[New RPG] AutoWow {} selected grounded camp destination ({},{},{},{})",
                  bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
                  dest.GetPositionZ());
        return dest;
    }

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            break;
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            if (sPlayerbotAIConfig.autoWowQuestScheduler)
                return ScheduleDoQuest(true);
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;
                if (sPlayerbotAIConfig.autoWowQuestBlockedDefer && IsQuestStallDeferred(questId))
                    continue;
                if (QuestSchedulerPolicy::ContainsQuestId(sPlayerbotAIConfig.autoWowQuestAvoidIds, questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true) &&
                    (!AutoWowDeathLoop::Enabled() || !DeathLoopDefersQuest(bot, questId, poiInfo)))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(
                        questId, quest, AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter()));
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            if (sPlayerbotAIConfig.autoWowQuestScheduler)
                return ScheduleDoQuest(false);
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;
                if (sPlayerbotAIConfig.autoWowQuestBlockedDefer && IsQuestStallDeferred(questId))
                    continue;
                if (QuestSchedulerPolicy::ContainsQuestId(sPlayerbotAIConfig.autoWowQuestAvoidIds, questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true) &&
                    (!AutoWowDeathLoop::Enabled() || !DeathLoopDefersQuest(bot, questId, poiInfo)))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}
