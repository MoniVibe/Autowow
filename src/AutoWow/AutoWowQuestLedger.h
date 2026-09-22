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
    Contaminated = 5
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
    }
    return "unknown";
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
};

namespace detail
{
inline bool gEnabled = false;
inline std::string gRunId;

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
    out += "\"}";
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
}  // namespace AutoWowQuestLedger

#endif
