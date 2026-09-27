/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUTOWOWQUESTLEDGER_H
#define _PLAYERBOT_AUTOWOWQUESTLEDGER_H

// Per-quest outcome ledger: one push-based JSON line per quest lifecycle event for playerbots,
// written to logger `autowow.ledger` and folded offline (AutoWoW scripts/ledger-reduce.py).
//
// Contract (schema v1):
//   - Field order is fixed: v, run, ms, ev, bot, team, lvl, quest, map, zone, x, y, c, i, reason, phase.
//   - Event names are append-only and never reused; a meaning change bumps kSchemaVersion.
//   - Positions are integer yards (floor); counters are the core QuestStatusData counters.
//   - Gate: AutoWow.Ledger.Enable (default 0). Disabled cost is one cached bool read per call site.
//   - Optional trailing fields (appended after `phase`, only on the events that carry them):
//       n  (blocked, AutoWow.Ledger.BlockedDedupeMs > 0): occurrences this line stands for (>= 1).
//          Lines without `n` stand for exactly one occurrence.
//       killer, kid, klvl (died): killer kind (player|creature|environment|unknown), killer id
//          (creature entry, or player guid-low; a pet/guardian/totem kill names its owning player),
//          killer level. quest is 0; zone/x/y are the victim's.
//       victim, vlvl, honorable (pvp_kill): victim guid-low, victim level, core honor eligibility.
//       cv, cls, ... (combat): bot-level cumulative combat totals, schema `cv`
//          (AutoWowCombatPerformanceTelemetry::LifetimeCounters::DrainEmitFields). quest is 0.
//   - `progress` (AutoWow.Ledger.ProgressSampleMs > 0): a per-bot periodic diff of every quest-log
//     entry's c/i counters; one line per quest whose counters changed since the previous sample.
//     A quest's first sample is a silent baseline. reason/phase are empty.
//   - `skill_up` (AutoWow.Ledger.SkillUp = 1): a profession skill value/max change of a bot.
//       skill, old, new, max, cause: skill line id, value before and after, max after, and
//       cause update (core UpdateSkill/UpdateSkillPro) | learn (first rank, old 0) | rank (rank-up,
//       old == new, max grew). quest is 0. Sum(new - old) per team is the profession KPI delta.
//   - `death_loop` (AutoWow.DeathLoop.Enable): one line per escalated death (AutoWowDeathLoop).
//     reason = trigger (cluster|level_gap|cluster_level_gap); quest = the quest deferred for it (or 0);
//     zone/x/y are the victim's. Trailing: deaths (same-spot deaths in the window, incl. this one),
//     klvl (killer level, 0 = unknown), zlow (zone bracket low, 0 = unknown), relocate (0|1),
//     dx, dy, dr (danger-area center and radius, integer yards), cool_ms (danger cooldown).
//   - `engage` (AutoWow.Tactics.Observe/Enable): one line per solo-priest engagement (tactical layer).
//     quest is 0; lvl/zone/x/y at the engagement end. Trailing fields, schema `ecv`
//     (AutoWowTactics::FormatEngageFields): ecv, cls, tab, arm, eng, tac0, tacs=[[tactic, ms], ...],
//     tacs_drop, mobs_max, adds, elite_n, lvl_dmax, load_max (centi mob-equivalents), pvp, dur_ms, kills,
//     outcome (0 win, 1 died, 2 escaped, 3 died after escape, 4 no kill), hp0, hp1, mp0, mp1 (pct),
//     mana_spent, hp_lost (absolute), casts, wand_ms, cc_n, shield_n, gap_ms, gap_rest_ms, pull_risk.
//   - `zone_move` (AutoWow.ZoneProgression.Enable): one line per finished or abandoned zone graduation.
//     reason = trigger (level|no_quests|death_loop: AutoWow.DeathLoop.EscapeViaZoneProgression escape,
//     from = the zone escaped); quest is 0; lvl/zone/x/y at the end of the move. Trailing:
//     from, to (zone ids), travel_ms (graduation to hub arrival or give-up), arrived (bool),
//     mode (walk|flight|chain|portal|none: the last travel mode issued). With AutoWow.Transports.Enable also
//     legs=[[walk|flight|travel_object|transport|portal, ms], ...] in travel order (same event id).
//   - `errand` (AutoWow.Errands.Enable): one line per finished town run. reason = outcome
//     (done|travel_gave_up|errands_timeout|return_gave_up); quest is 0; lvl/zone/x/y at the end. Trailing:
//     town (innkeeper spawn guid), town_zone, needs (AutoWowErrands::Need bits), done (Done bits: sold 1,
//     repaired 2, restocked 4, trained 8, bound 16, learned_fp 32, skipped 64, geared 128 with
//     AutoWow.Gear.Upgrades, tooled 256 with AutoWow.Supply.Outfit), spent, sold (copper),
//     dur0, dur1 (average equipped durability % at arrival / after the errands), bag0, bag1 (free bag
//     slots, same points), travel_ms (decision to arrival), return_ms, leg (walk|flight|hearth|none: the
//     leg that took the bot to town), hearth (bool: the hearthstone was cast).
//   - `trade` (event 17; AutoWow.Trade.Enable, AutoWow.Ledger.Treasury): one line per auction-house or
//     mail result, or per fee paid (AutoWowTrade, TradePolicy.h). reason = action; quest is 0. Trailing:
//     action (post|buy|sold|expired|mail|fee|tax), item (entry, 0 = none), count, price (copper: listing
//     buyout, purchase price, sale's winning bid, fee), gold (signed copper change of the bot's money),
//     ah (auction id, 0 = unknown); `fee` also kind (flight|repair|train). `tax` (AutoWow.Guilds.TaxPct): the
//     vendor-income tax paid into the bot's house guild bank (also a `guild` row, reason tax).
//   - `party` (AutoWow.Party.Enable): one line when a cohort party forms (reason `formed`) and one when it
//     disbands (reason = level_drift|separated|member_offline|dungeon_done|no_purpose|max_age|too_small|
//     disabled). bot/lvl/zone/x/y = the leader (else the first online member); quest 0. Trailing: pid
//     (run-scoped party id, never reused), members (guids ascending), roles (tank|healer|dps, same order),
//     leader, why (dungeon|group_quest), dmap (dungeon map id, 0 = none), age_ms (0 at formation).
//   - `dungeon` (AutoWow.Dungeon.Enable): one line per run event. reason = entered|boss_killed|completed|
//     wiped|abandoned|approach_gave_up|stage_failed|portal_fallback. Trailing: pid, dmap, inst (instance id,
//     0 before entry), enc (encounter index for boss_killed, else -1), mask / all (completed / all
//     DungeonEncounter bits), dur_ms (since the approach started), members.
//   - `contract` (event 18; AutoWow.Contracts.Enable): one line per hunt contract issued and one when it
//     ends (ContractsPolicy.h). reason = issued|done|expired|abandoned; quest is 0; lvl/zone/x/y the bot's.
//     Trailing: issuer (faction_board), cid (run-scoped contract id, never reused), amap, ax, ay (anchor
//     map and integer yards), anchor (anchor spawn id), entries (target entry count), kills, target,
//     dur_ms (since issue; 0 on issued).
//   - `guild` (event 19; AutoWow.Guilds.Enable): one line per house-guild membership or treasury movement
//     (AutoWowGuildsPolicy.h). reason = created|joined|skip_other_guild|tax|pay|deposit|refused|postage|
//     levy|grant (AutoWow.Supply.Outfit: bank -> member starter-equipment grant);
//     quest is 0; team is the row's. Trailing: house (house name), gid (guild id), copper, balance_after
//     (guild bank copper after the movement), op (refused only: the movement refused).
//   - `supply` (event 20; AutoWow.Supply.Enable): one line per bag-chain step (SupplyPolicy.h). reason =
//     order|donate|feed|craft|deliver|pay|surplus|xp|refused|travel|list|buy|sold (the last three:
//     AutoWow.Supply.Market)|outfit (AutoWow.Supply.Outfit: a tool bought, "line":"outfit"); quest is 0; team is the row's. Trailing: house, oid (team order id, run-scoped, never reused), item, count, copper (xp: the XP amount), from,
//     to (guid-lows, 0 = treasury / none), op (refused: the movement; travel: other_map|stuck; surplus:
//     auction|vendor; line outfit: outfit|grant_bot_cap|grant_team_budget).
//   - `classquest` (event 23; AutoWow.ClassQuests.Enable): one line per class-quest reward granted to a cohort /
//     supply role bot (ClassQuestPolicy.h). reason = grant; quest = the reward's quest (not rewarded: quest status
//     is untouched). Trailing: spell (learned, 0 = none), item (stored, 0 = none), level (the quest's MinLevel).
// The formatter below is pure (no world access) so it is unit-testable; Emit() lives in the .cpp.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class Player;
enum class QuestActionPhase : std::uint8_t;
enum class QuestFailureReason : std::uint16_t;

namespace AutoWowQuestLedger
{
inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kCreatureCounters = 4;  // QUEST_OBJECTIVES_COUNT
inline constexpr std::size_t kItemCounters = 6;      // QUEST_ITEM_OBJECTIVES_COUNT

// Numeric values are wire-stable; append only.
enum class Event : std::uint8_t
{
    Accepted = 0,
    Rewarded = 1,
    Abandoned = 2,
    Blocked = 3,
    Deferred = 4,
    Contaminated = 5,
    Died = 6,
    PvpKill = 7,
    Combat = 8,
    Progress = 9,
    DeathLoop = 10,
    SkillUp = 11,
    Engage = 12,
    ZoneMove = 13,
    Errand = 14,
    Party = 15,
    Dungeon = 16,
    Trade = 17,
    Contract = 18,
    Guild = 19,
    Supply = 20,
    SelfCraft = 21,
    Squad = 22,
    ClassQuest = 23
};

inline constexpr char const* EventName(Event ev)
{
    switch (ev)
    {
        case Event::Accepted: return "accepted";
        case Event::Rewarded: return "rewarded";
        case Event::Abandoned: return "abandoned";
        case Event::Blocked: return "blocked";
        case Event::Deferred: return "deferred";
        case Event::Contaminated: return "contaminated";
        case Event::Died: return "died";
        case Event::PvpKill: return "pvp_kill";
        case Event::Combat: return "combat";
        case Event::Progress: return "progress";
        case Event::SkillUp: return "skill_up";
        case Event::DeathLoop: return "death_loop";
        case Event::Engage: return "engage";
        case Event::ZoneMove: return "zone_move";
        case Event::Errand: return "errand";
        case Event::Party: return "party";
        case Event::Dungeon: return "dungeon";
        case Event::Trade: return "trade";
        case Event::Contract: return "contract";
        case Event::Guild: return "guild";
        case Event::Supply: return "supply";
        case Event::SelfCraft: return "selfcraft";
        case Event::Squad: return "squad";
        case Event::ClassQuest: return "classquest";
    }
    return "unknown";
}

// Wire-stable; append only.
enum class KillerKind : std::uint8_t
{
    Player = 0,
    Creature = 1,
    Environment = 2,
    Unknown = 3
};

inline constexpr char const* KillerKindName(KillerKind kind)
{
    switch (kind)
    {
        case KillerKind::Player: return "player";
        case KillerKind::Creature: return "creature";
        case KillerKind::Environment: return "environment";
        case KillerKind::Unknown: return "unknown";
    }
    return "unknown";
}

// Mirrors the core Player::RewardHonor gate for a player victim (arena excluded by the caller).
inline constexpr bool IsHonorableKill(bool sameTeam, bool ffaRealm, std::uint32_t killerGrayLevel,
                                      std::uint32_t victimLevel, bool victimNoPvpCredit)
{
    return !victimNoPvpCredit && (!sameTeam || ffaRealm) && victimLevel > killerGrayLevel;
}

struct Row
{
    Event ev = Event::Accepted;
    std::uint64_t ms = 0;
    std::uint32_t bot = 0;
    std::uint32_t team = 0;
    std::uint32_t level = 0;
    std::uint32_t quest = 0;
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t c[kCreatureCounters] = {};
    std::uint16_t i[kItemCounters] = {};
    char const* reason = "";  // static literal from a closed name table
    char const* phase = "";   // static literal from a closed name table
    std::uint32_t n = 0;      // blocked repeat count; 0 = field omitted (one occurrence)
    // died
    KillerKind killer = KillerKind::Unknown;
    std::uint32_t killerId = 0;
    std::uint32_t killerLevel = 0;
    // pvp_kill
    std::uint32_t victim = 0;
    std::uint32_t victimLevel = 0;
    bool honorable = false;
    // combat, death_loop, engage: pre-formatted trailing fields (",\"cv\":1,..."), appended verbatim
    std::string_view extra;
    // skill_up
    std::uint32_t skill = 0;
    std::uint32_t skillOld = 0;
    std::uint32_t skillNew = 0;
    std::uint32_t skillMax = 0;
    char const* cause = "";  // static literal: update | learn | rank
};

// Blocked-row dedupe (pure; unit-tested). A (quest, reason, phase) key emits on change; repeats of
// the same key are counted and emitted as one heartbeat line with `n` once heartbeatMs has passed.
// On a key change the previous key's uncounted repeats are flushed first, so sum(n) is exact except
// for a trailing (< heartbeatMs) tail of a key the bot never reports again.
struct BlockedKey
{
    std::uint32_t quest = 0;
    char const* reason = "";  // static literals: ReasonName / PhaseName tables
    char const* phase = "";
};

struct BlockedDedupeState
{
    BlockedKey key;
    bool active = false;
    std::uint64_t lastEmitMs = 0;
    std::uint32_t suppressed = 0;
};

struct BlockedDecision
{
    std::uint32_t flushN = 0;  // > 0: first emit a line for flushKey with n = flushN
    BlockedKey flushKey;
    std::uint32_t n = 0;       // > 0: emit this occurrence with n; 0 = suppressed
};

inline bool SameBlockedKey(BlockedKey const& a, BlockedKey const& b)
{
    return a.quest == b.quest && std::string_view(a.reason ? a.reason : "") == (b.reason ? b.reason : "") &&
           std::string_view(a.phase ? a.phase : "") == (b.phase ? b.phase : "");
}

inline BlockedDecision DedupeBlocked(BlockedDedupeState& s, BlockedKey const& key, std::uint64_t nowMs,
                                     std::uint64_t heartbeatMs)
{
    BlockedDecision out;
    if (s.active && SameBlockedKey(s.key, key))
    {
        if (nowMs - s.lastEmitMs < heartbeatMs)
        {
            ++s.suppressed;
            return out;
        }
        out.n = s.suppressed + 1;
    }
    else
    {
        if (s.active && s.suppressed)
        {
            out.flushN = s.suppressed;
            out.flushKey = s.key;
        }
        s.key = key;
        s.active = true;
        out.n = 1;
    }
    s.lastEmitMs = nowMs;
    s.suppressed = 0;
    return out;
}

// Progress sampling (pure; unit-tested). prev holds the last sample per quest-log entry. Returns the
// quests of cur whose counters differ from their previous sample, in cur order; quests new to the
// log are a baseline only (their `accepted` line carries the counters). prev becomes cur, so quests
// that left the log are forgotten.
struct QuestCounters
{
    std::uint32_t quest = 0;
    std::uint16_t c[kCreatureCounters] = {};
    std::uint16_t i[kItemCounters] = {};
};

inline bool SameCounters(QuestCounters const& a, QuestCounters const& b)
{
    for (std::size_t k = 0; k < kCreatureCounters; ++k)
        if (a.c[k] != b.c[k])
            return false;
    for (std::size_t k = 0; k < kItemCounters; ++k)
        if (a.i[k] != b.i[k])
            return false;
    return true;
}

inline std::vector<std::uint32_t> DiffProgress(std::vector<QuestCounters>& prev, std::vector<QuestCounters> const& cur)
{
    std::vector<std::uint32_t> changed;
    for (QuestCounters const& now : cur)
        for (QuestCounters const& before : prev)
            if (before.quest == now.quest)
            {
                if (!SameCounters(before, now))
                    changed.push_back(now.quest);
                break;
            }
    prev = cur;
    return changed;
}

namespace detail
{
inline bool gEnabled = false;
inline std::string gRunId;
inline std::uint64_t gBlockedDedupeMs = 0;
inline std::uint64_t gProgressSampleMs = 0;
inline bool gSkillUpEnabled = false;

inline void AppendEscaped(std::string& out, std::string_view text)
{
    static constexpr char kHex[] = "0123456789abcdef";
    for (char ch : text)
    {
        unsigned char const u = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\')
        {
            out.push_back('\\');
            out.push_back(ch);
        }
        else if (u < 0x20)
        {
            out += "\\u00";
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0xF]);
        }
        else
            out.push_back(ch);
    }
}
}  // namespace detail

inline bool Enabled() { return detail::gEnabled; }

inline std::string FormatLine(std::string_view runId, Row const& row)
{
    std::string out;
    out.reserve(256);
    out += "{\"v\":";
    out += std::to_string(kSchemaVersion);
    out += ",\"run\":\"";
    detail::AppendEscaped(out, runId);
    out += "\",\"ms\":";
    out += std::to_string(row.ms);
    out += ",\"ev\":\"";
    out += EventName(row.ev);
    out += "\",\"bot\":";
    out += std::to_string(row.bot);
    out += ",\"team\":";
    out += std::to_string(row.team);
    out += ",\"lvl\":";
    out += std::to_string(row.level);
    out += ",\"quest\":";
    out += std::to_string(row.quest);
    out += ",\"map\":";
    out += std::to_string(row.map);
    out += ",\"zone\":";
    out += std::to_string(row.zone);
    out += ",\"x\":";
    out += std::to_string(row.x);
    out += ",\"y\":";
    out += std::to_string(row.y);
    out += ",\"c\":[";
    for (std::size_t k = 0; k < kCreatureCounters; ++k)
    {
        if (k)
            out.push_back(',');
        out += std::to_string(row.c[k]);
    }
    out += "],\"i\":[";
    for (std::size_t k = 0; k < kItemCounters; ++k)
    {
        if (k)
            out.push_back(',');
        out += std::to_string(row.i[k]);
    }
    out += "],\"reason\":\"";
    detail::AppendEscaped(out, row.reason ? row.reason : "");
    out += "\",\"phase\":\"";
    detail::AppendEscaped(out, row.phase ? row.phase : "");
    out += "\"";
    if (row.n)
    {
        out += ",\"n\":";
        out += std::to_string(row.n);
    }
    if (row.ev == Event::Died)
    {
        out += ",\"killer\":\"";
        out += KillerKindName(row.killer);
        out += "\",\"kid\":";
        out += std::to_string(row.killerId);
        out += ",\"klvl\":";
        out += std::to_string(row.killerLevel);
    }
    else if (row.ev == Event::PvpKill)
    {
        out += ",\"victim\":";
        out += std::to_string(row.victim);
        out += ",\"vlvl\":";
        out += std::to_string(row.victimLevel);
        out += ",\"honorable\":";
        out += row.honorable ? "true" : "false";
    }
    else if (row.ev == Event::Combat || row.ev == Event::DeathLoop || row.ev == Event::Engage ||
             row.ev == Event::ZoneMove || row.ev == Event::Errand || row.ev == Event::Trade ||
             row.ev == Event::Party || row.ev == Event::Dungeon || row.ev == Event::Contract ||
             row.ev == Event::Guild || row.ev == Event::Supply || row.ev == Event::SelfCraft ||
             row.ev == Event::Squad || row.ev == Event::ClassQuest)
        out += row.extra;
    else if (row.ev == Event::SkillUp)
    {
        out += ",\"skill\":";
        out += std::to_string(row.skill);
        out += ",\"old\":";
        out += std::to_string(row.skillOld);
        out += ",\"new\":";
        out += std::to_string(row.skillNew);
        out += ",\"max\":";
        out += std::to_string(row.skillMax);
        out += ",\"cause\":\"";
        out += row.cause ? row.cause : "";
        out += "\"";
    }
    out += "}";
    return out;
}

// Reads AutoWow.Ledger.Enable / AutoWow.Ledger.RunId. Called once at world init.
void LoadConfig();

// Stable wire names for the typed executor enums (QuestObjectiveContext.h).
char const* ReasonName(QuestFailureReason reason);
char const* PhaseName(QuestActionPhase phase);

// World-thread only. No-op when disabled, when player is null, or when player is not a playerbot
// (real players and human-controlled bots are never recorded).
void Emit(Player* player, Event ev, std::uint32_t questId, char const* reason = "", char const* phase = "");

// Blocked event through the per-bot dedupe (AutoWow.Ledger.BlockedDedupeMs; 0 = every occurrence,
// identical to Emit(Blocked)). reason/phase must be static literals (ReasonName / PhaseName).
void EmitBlocked(Player* player, std::uint32_t questId, char const* reason, char const* phase);

// Death attribution. Core order: Unit::Kill fires the kill hook (killer known) and the victim's
// next update fires OnPlayerJustDied. NoteKiller records the killer for a recorded bot victim;
// EmitDied emits `died` with it (or kind unknown when no kill hook fired) and forgets it.
void NoteKiller(Player* victim, KillerKind kind, std::uint32_t killerId, std::uint32_t killerLevel);
void EmitDied(Player* victim);
// `pvp_kill` on the killer's row (no-op unless the killer is a recorded bot).
void EmitPvpKill(Player* killer, Player* victim, bool honorable);
// `combat` (no-op unless the player is a recorded bot); fields from DrainEmitFields.
void EmitCombat(Player* player, std::string_view fields);
// `death_loop` (no-op unless the player is a recorded bot); reason is a static literal, fields are the
// pre-formatted trailing fields.
void EmitDeathLoop(Player* player, std::uint32_t questId, char const* reason, std::string_view fields);
// `engage` (no-op unless the player is a recorded bot); fields from AutoWowTactics::FormatEngageFields.
void EmitEngage(Player* player, std::string_view fields);
// `zone_move` (no-op unless the player is a recorded bot); reason is a static literal (trigger name),
// fields from AutoWowZoneProgression::LedgerFields.
void EmitZoneMove(Player* player, char const* reason, std::string_view fields);
// `errand` (no-op unless the player is a recorded bot); reason is a static literal (outcome name),
// fields from AutoWowErrands::LedgerFields.
void EmitErrand(Player* player, char const* reason, std::string_view fields);
// `trade` (no-op unless the player is a recorded bot); reason is a static literal (action name), fields
// from AutoWowTrade::LedgerFields.
void EmitTrade(Player* player, char const* reason, std::string_view fields);
// `party` / `dungeon` (no-op unless the player is a recorded bot); reason is a static literal, fields from
// AutoWowParty::PartyFields / DungeonFields.
void EmitParty(Player* player, char const* reason, std::string_view fields);
void EmitDungeon(Player* player, char const* reason, std::string_view fields);
// `contract` (no-op unless the player is a recorded bot); reason is a static literal (ReasonName), fields
// from AutoWowContracts::LedgerFields.
void EmitContract(Player* player, char const* reason, std::string_view fields);
// `guild` (no-op unless the player is a recorded bot); reason is a static literal (AutoWowGuilds::ReasonName),
// fields from AutoWowGuilds::LedgerFields.
void EmitGuild(Player* player, char const* reason, std::string_view fields);
// `supply` (no-op unless the player is a recorded bot); reason is a static literal (AutoWowSupply::ReasonName),
// fields from AutoWowSupply::LedgerFields.
void EmitSupply(Player* player, char const* reason, std::string_view fields);
// `selfcraft` (no-op unless the player is a recorded bot); reason is a static literal
// (AutoWowSelfCraft::ReasonName), fields from AutoWowSelfCraft::LedgerFields.
void EmitSelfCraft(Player* player, char const* reason, std::string_view fields);
// `squad` (no-op unless the player is a recorded bot); reason is a static literal (AutoWowSquad::ReasonName),
// fields from AutoWowSquad::LedgerFields.
void EmitSquad(Player* player, char const* reason, std::string_view fields);
// `classquest` (no-op unless the player is a recorded bot); reason is a static literal (grant), quest the granted
// reward's quest, fields from AutoWowClassQuests::LedgerFields.
void EmitClassQuest(Player* player, char const* reason, std::uint32_t questId, std::string_view fields);
// Per-bot `progress` sampler; call from the bot update. No-op unless AutoWow.Ledger.ProgressSampleMs
// > 0 and the player is a recorded bot; rate-limited per bot to one diff per sample period.
void SampleProgress(Player* player);

// ---- skill_up (AutoWow.Ledger.SkillUp, default 0; also needs AutoWow.Ledger.Enable) ----

inline bool SkillUpEnabled() { return detail::gSkillUpEnabled; }

// `skill_up` for a recorded bot; no-op for a non-profession skill line or when disabled.
void EmitSkillUp(Player* player, std::uint32_t skill, std::uint32_t oldValue, std::uint32_t newValue,
                 std::uint32_t maxValue, char const* cause);
}  // namespace AutoWowQuestLedger

#endif
