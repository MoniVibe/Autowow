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
// The formatter below is pure (no world access) so it is unit-testable; Emit() lives in the .cpp.

#include <cstdint>
#include <string>
#include <string_view>

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
    PvpKill = 7
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

namespace detail
{
inline bool gEnabled = false;
inline std::string gRunId;
inline std::uint64_t gBlockedDedupeMs = 0;

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
}  // namespace AutoWowQuestLedger

#endif
