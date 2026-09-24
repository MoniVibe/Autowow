/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowQuestLedger.h"

#include "AutoWowTrainPolicy.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestObjectiveContext.h"

static_assert(AutoWowQuestLedger::kCreatureCounters == QUEST_OBJECTIVES_COUNT);
static_assert(AutoWowQuestLedger::kItemCounters == QUEST_ITEM_OBJECTIVES_COUNT);

namespace AutoWowQuestLedger
{
void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Ledger.Enable", false);
    detail::gRunId = sConfigMgr->GetOption<std::string>("AutoWow.Ledger.RunId", "");
    detail::gBlockedDedupeMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Ledger.BlockedDedupeMs", 0);
    detail::gProgressSampleMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Ledger.ProgressSampleMs", 0);
    detail::gSkillUpEnabled = sConfigMgr->GetOption<bool>("AutoWow.Ledger.SkillUp", false);
}

char const* ReasonName(QuestFailureReason reason)
{
    switch (reason)
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
        case QuestFailureReason::OracleRouteInvalidDescriptor: return "oracle_route_invalid_descriptor";
        case QuestFailureReason::OracleRouteUnsupportedTransition: return "oracle_route_unsupported_transition";
        case QuestFailureReason::OracleRouteNoSafeAnchor: return "oracle_route_no_safe_anchor";
        case QuestFailureReason::OracleRouteBlocked: return "oracle_route_blocked";
        case QuestFailureReason::TravelNoProgress: return "travel_no_progress";
        case QuestFailureReason::IntentReplanExhausted: return "intent_replan_exhausted";
        case QuestFailureReason::IntentNoProgress: return "intent_no_progress";
    }
    return "unknown";
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
    }
    return "unknown";
}

namespace
{
bool IsRecordedBot(Player* player)
{
    if (!detail::gEnabled || !player)
        return false;
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    return ai && !ai->IsRealPlayer();
}

// Fills the bot context (time, identity, position, quest counters) of a row. Caller filtered.
void FillRow(Player* player, Event ev, std::uint32_t questId, char const* reason, char const* phase, Row& row)
{
    row.ev = ev;
    row.ms = static_cast<std::uint64_t>(GameTime::GetGameTimeMS().count());
    row.bot = static_cast<std::uint32_t>(player->GetGUID().GetCounter());
    row.team = static_cast<std::uint32_t>(player->GetTeamId());
    row.level = player->GetLevel();
    row.quest = questId;
    row.map = player->GetMapId();
    row.zone = player->GetZoneId();
    row.x = static_cast<std::int32_t>(std::floor(player->GetPositionX()));
    row.y = static_cast<std::int32_t>(std::floor(player->GetPositionY()));
    if (questId)
    {
        QuestStatusMap& statusMap = player->getQuestStatusMap();
        auto const it = statusMap.find(questId);
        if (it != statusMap.end())
        {
            for (std::size_t k = 0; k < kCreatureCounters; ++k)
                row.c[k] = it->second.CreatureOrGOCount[k];
            for (std::size_t k = 0; k < kItemCounters; ++k)
                row.i[k] = it->second.ItemCount[k];
        }
    }
    row.reason = reason;
    row.phase = phase;
}

// Bots update on map threads; the dedupe table is shared. Touched only on blocked events.
std::mutex gBlockedLock;
std::unordered_map<std::uint32_t, BlockedDedupeState> gBlockedByBot;

struct PendingKiller
{
    KillerKind kind = KillerKind::Unknown;
    std::uint32_t id = 0;
    std::uint32_t level = 0;
};
// Killer noted by the kill hook until the victim's OnPlayerJustDied. Touched only on deaths.
std::mutex gKillerLock;
std::unordered_map<std::uint32_t, PendingKiller> gKillerByBot;

struct ProgressSampler
{
    std::uint64_t nextAtMs = 0;
    std::vector<QuestCounters> last;
};
// ponytail: one global lock; held for one short per-bot check each update while sampling is on.
std::mutex gProgressLock;
std::unordered_map<std::uint32_t, ProgressSampler> gProgressByBot;
}  // namespace

void SampleProgress(Player* player)
{
    if (!detail::gProgressSampleMs || !IsRecordedBot(player))
        return;

    std::uint64_t const nowMs = static_cast<std::uint64_t>(GameTime::GetGameTimeMS().count());
    std::uint32_t const bot = static_cast<std::uint32_t>(player->GetGUID().GetCounter());
    {
        std::lock_guard<std::mutex> guard(gProgressLock);
        ProgressSampler& sampler = gProgressByBot[bot];
        if (nowMs < sampler.nextAtMs)
            return;
        sampler.nextAtMs = nowMs + detail::gProgressSampleMs;
    }

    std::vector<QuestCounters> current;
    QuestStatusMap& statusMap = player->getQuestStatusMap();
    for (std::uint8_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        std::uint32_t const questId = player->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        auto const it = statusMap.find(questId);
        if (it == statusMap.end())
            continue;
        QuestCounters counters;
        counters.quest = questId;
        for (std::size_t k = 0; k < kCreatureCounters; ++k)
            counters.c[k] = it->second.CreatureOrGOCount[k];
        for (std::size_t k = 0; k < kItemCounters; ++k)
            counters.i[k] = it->second.ItemCount[k];
        current.push_back(counters);
    }

    std::vector<std::uint32_t> changed;
    {
        std::lock_guard<std::mutex> guard(gProgressLock);
        changed = DiffProgress(gProgressByBot[bot].last, current);
    }
    for (std::uint32_t const questId : changed)
    {
        Row row;
        FillRow(player, Event::Progress, questId, "", "", row);
        LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
    }
}

void NoteKiller(Player* victim, KillerKind kind, std::uint32_t killerId, std::uint32_t killerLevel)
{
    if (!IsRecordedBot(victim))
        return;
    std::lock_guard<std::mutex> guard(gKillerLock);
    gKillerByBot[static_cast<std::uint32_t>(victim->GetGUID().GetCounter())] = {kind, killerId, killerLevel};
}

void EmitDied(Player* victim)
{
    if (!IsRecordedBot(victim))
        return;
    PendingKiller killer;
    {
        std::lock_guard<std::mutex> guard(gKillerLock);
        auto const it = gKillerByBot.find(static_cast<std::uint32_t>(victim->GetGUID().GetCounter()));
        if (it != gKillerByBot.end())
        {
            killer = it->second;
            gKillerByBot.erase(it);
        }
    }
    Row row;
    FillRow(victim, Event::Died, 0, "", "", row);
    row.killer = killer.kind;
    row.killerId = killer.id;
    row.killerLevel = killer.level;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitPvpKill(Player* killer, Player* victim, bool honorable)
{
    if (!victim || !IsRecordedBot(killer))
        return;
    Row row;
    FillRow(killer, Event::PvpKill, 0, "", "", row);
    row.victim = static_cast<std::uint32_t>(victim->GetGUID().GetCounter());
    row.victimLevel = victim->GetLevel();
    row.honorable = honorable;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitCombat(Player* player, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Combat, 0, "", "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitEngage(Player* player, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Engage, 0, "", "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitSkillUp(Player* player, std::uint32_t skill, std::uint32_t oldValue, std::uint32_t newValue,
                 std::uint32_t maxValue, char const* cause)
{
    if (!detail::gSkillUpEnabled || !AutoWowTrainPolicy::IsProfessionSkillLine(skill) || !IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::SkillUp, 0, "", "", row);
    row.skill = skill;
    row.skillOld = oldValue;
    row.skillNew = newValue;
    row.skillMax = maxValue;
    row.cause = cause;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitDeathLoop(Player* player, std::uint32_t questId, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::DeathLoop, questId, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitZoneMove(Player* player, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::ZoneMove, 0, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitErrand(Player* player, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Errand, 0, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitTrade(Player* player, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Trade, 0, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitParty(Player* player, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Party, 0, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitDungeon(Player* player, char const* reason, std::string_view fields)
{
    if (!IsRecordedBot(player))
        return;
    Row row;
    FillRow(player, Event::Dungeon, 0, reason, "", row);
    row.extra = fields;
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void Emit(Player* player, Event ev, std::uint32_t questId, char const* reason, char const* phase)
{
    if (!IsRecordedBot(player))
        return;

    Row row;
    FillRow(player, ev, questId, reason, phase, row);
    LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
}

void EmitBlocked(Player* player, std::uint32_t questId, char const* reason, char const* phase)
{
    if (!IsRecordedBot(player))
        return;
    if (!detail::gBlockedDedupeMs)
    {
        Emit(player, Event::Blocked, questId, reason, phase);
        return;
    }

    std::uint64_t const nowMs = static_cast<std::uint64_t>(GameTime::GetGameTimeMS().count());
    BlockedDecision decision;
    {
        std::lock_guard<std::mutex> guard(gBlockedLock);
        decision = DedupeBlocked(gBlockedByBot[static_cast<std::uint32_t>(player->GetGUID().GetCounter())],
                                 BlockedKey{questId, reason, phase}, nowMs, detail::gBlockedDedupeMs);
    }
    if (decision.flushN)
    {
        Row row;
        FillRow(player, Event::Blocked, decision.flushKey.quest, decision.flushKey.reason, decision.flushKey.phase,
                row);
        row.n = decision.flushN;
        LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
    }
    if (decision.n)
    {
        Row row;
        FillRow(player, Event::Blocked, questId, reason, phase, row);
        row.n = decision.n;
        LOG_INFO("autowow.ledger", "{}", FormatLine(detail::gRunId, row));
    }
}
}  // namespace AutoWowQuestLedger
