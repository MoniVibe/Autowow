/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

// Pure policy for the per-quest PROBE bridge verbs (probe-login, probe-setlevel, probe-place,
// probe-status). No world access: wire parsing, the fixture-only GUID gate, level validation and
// response formatting are unit-tested here. Live mutation stays in ProbePlaceControl.cpp.
//
// Every verb is SETUP for a quest probe on an explicitly allowlisted fixture character. The whole
// surface is behind AutoWow.Probe.Enable (default 0); when it is off every verb is refused before
// any world object is resolved.

#ifndef MOD_PLAYERBOTS_PROBE_PLACE_POLICY_H
#define MOD_PLAYERBOTS_PROBE_PLACE_POLICY_H

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "AutoWowRandomBotPolicy.h"

namespace AutoWowProbePlace
{
inline constexpr char kEnableConfigKey[] = "AutoWow.Probe.Enable";
inline constexpr char kOracleGuidsConfigKey[] = "AutoWow.OracleRuntime.BotGuids";
// Persistent cohort (docs/COHORT_PLAN.md). Never a probe target even if listed as a fixture.
inline constexpr std::uint32_t kCohortFirstGuid = 62955;
inline constexpr std::uint32_t kCohortLastGuid = 63004;
inline constexpr std::uint32_t kDeathKnightClassId = 6;
inline constexpr std::uint32_t kDeathKnightMinLevel = 55;
inline constexpr std::uint32_t kDefaultQuality = 2;  // uncommon, same default as probe-quest.ps1

enum class Verb : std::uint8_t
{
    Login,
    SetLevel,
    Place,
    Status
};

inline constexpr char const* VerbOperation(Verb verb)
{
    switch (verb)
    {
        case Verb::Login: return "probe_login";
        case Verb::SetLevel: return "probe_setlevel";
        case Verb::Place: return "probe_place";
        case Verb::Status: return "probe_status";
    }
    return "probe_unknown";
}

struct WireRequest
{
    Verb verb = Verb::Status;
    std::uint32_t guid = 0;
    std::uint32_t level = 0;       // setlevel
    bool hasSpec = false;          // setlevel: absent = the bot's stored spec (or 0)
    std::uint32_t specIndex = 0;
    std::uint32_t quality = kDefaultQuality;
    std::uint32_t questId = 0;     // place (required), status (optional, 0 = none)
    bool dropOthers = false;       // place: also abandon every other quest in the log
};

namespace detail
{
inline bool ParseU32(std::string const& token, std::uint32_t& out)
{
    if (token.empty() || token.size() > 10)
        return false;
    std::uint64_t value = 0;
    for (char const c : token)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFull)
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}
}  // namespace detail

// Wire grammar (whitespace separated, verb case-insensitive via the bridge's lowercase command):
//   probe-login    <guid>
//   probe-setlevel <guid> <level> [<spec-index> <quality>]
//   probe-place    <guid> <questId> [drop-others]
//   probe-status   <guid> [<questId>]
// Returns false with a human-readable error for anything else; never partially fills a request.
inline bool ParseWireRequest(std::string const& text, WireRequest& out, std::string& error)
{
    std::istringstream input(text);
    std::string command;
    input >> command;
    for (char& c : command)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');

    std::vector<std::string> args;
    for (std::string token; input >> token;)
        args.push_back(token);

    WireRequest request;
    if (command == "probe-login")
        request.verb = Verb::Login;
    else if (command == "probe-setlevel")
        request.verb = Verb::SetLevel;
    else if (command == "probe-place")
        request.verb = Verb::Place;
    else if (command == "probe-status")
        request.verb = Verb::Status;
    else
    {
        error = "unknown probe verb";
        return false;
    }

    if (args.empty() || !detail::ParseU32(args[0], request.guid) || !request.guid)
    {
        error = command + " requires a positive numeric GUID";
        return false;
    }

    switch (request.verb)
    {
        case Verb::Login:
            if (args.size() != 1)
            {
                error = "probe-login accepts exactly one GUID";
                return false;
            }
            break;
        case Verb::SetLevel:
            if ((args.size() != 2 && args.size() != 4) || !detail::ParseU32(args[1], request.level) ||
                !request.level)
            {
                error = "probe-setlevel requires <guid> <level> [<spec-index> <quality>]";
                return false;
            }
            if (args.size() == 4)
            {
                if (!detail::ParseU32(args[2], request.specIndex) || request.specIndex >= 20 ||
                    !detail::ParseU32(args[3], request.quality) || request.quality > 5)
                {
                    error = "probe-setlevel spec-index must be 0..19 and quality 0..5";
                    return false;
                }
                request.hasSpec = true;
            }
            break;
        case Verb::Place:
            if (args.size() < 2 || args.size() > 3 || !detail::ParseU32(args[1], request.questId) ||
                !request.questId || (args.size() == 3 && args[2] != "drop-others"))
            {
                error = "probe-place requires <guid> <questId> [drop-others]";
                return false;
            }
            request.dropOthers = args.size() == 3;
            break;
        case Verb::Status:
            if (args.size() > 2 || (args.size() == 2 && !detail::ParseU32(args[1], request.questId)))
            {
                error = "probe-status requires <guid> [<questId>]";
                return false;
            }
            break;
    }

    out = request;
    return true;
}

// The fixture-only gate. Order is fixed so the refusal code is deterministic:
//   disabled -> cohort range -> not in AutoWow.FixtureGuids -> in the Oracle allowlist.
// The fixture allowlist is fail-CLOSED (any malformed token or a literal 0 refuses every GUID);
// the Oracle list is fail-SAFE through GuidListContains (a malformed token matches every GUID).
// Returns nullptr when the GUID may be used.
inline char const* GateGuid(bool enabled, std::string_view fixtureGuids, std::string_view oracleGuids,
                            std::uint32_t guid)
{
    if (!enabled)
        return "probe_disabled";
    if (!guid)
        return "probe_guid_invalid";
    if (guid >= kCohortFirstGuid && guid <= kCohortLastGuid)
        return "probe_guid_is_cohort";
    bool const fixtureListValid = !AutoWowRandomBotPolicy::GuidListContains(fixtureGuids, 0);
    if (!fixtureListValid || !AutoWowRandomBotPolicy::GuidListContains(fixtureGuids, guid))
        return "probe_guid_not_fixture";
    if (AutoWowRandomBotPolicy::GuidListContains(oracleGuids, guid))
        return "probe_guid_in_oracle_allowlist";
    return nullptr;
}

// probe-setlevel accepts up OR down; a death knight never goes below its starting level.
inline char const* ValidateLevel(std::uint32_t level, std::uint32_t maxLevel, std::uint32_t classId)
{
    if (!level || !maxLevel || level > maxLevel)
        return "probe_level_out_of_range";
    if (classId == kDeathKnightClassId && level < kDeathKnightMinLevel)
        return "probe_level_below_death_knight_floor";
    return nullptr;
}

// One quest-starter spawn (creature_queststarter or gameobject_queststarter joined to its spawn).
struct StarterCandidate
{
    bool gameObject = false;
    std::uint32_t entry = 0;
    std::uint64_t spawnId = 0;
    bool worldMap = false;     // continent (never an instance map)
    bool friendly = false;     // creature faction is at least friendly to the bot (gameobjects: true)
    bool onBotMap = false;
};

// Deterministic starter choice, explicit key (no container-order dependence):
//   worldMap desc, friendly desc, onBotMap desc, creature before gameobject, spawnId asc, entry asc.
// Returns candidates.size() when no candidate is on a world map (placement is then refused).
// ponytail: game_event-bound spawns are not filtered; holiday quests sit on the avoid list anyway.
inline std::size_t SelectStarter(std::vector<StarterCandidate> const& candidates)
{
    std::size_t best = candidates.size();
    auto key = [](StarterCandidate const& c)
    {
        return std::make_tuple(!c.worldMap, !c.friendly, !c.onBotMap, c.gameObject, c.spawnId, c.entry);
    };
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        if (!candidates[i].worldMap)
            continue;
        if (best == candidates.size() || key(candidates[i]) < key(candidates[best]))
            best = i;
    }
    return best;
}

// Quest status names follow the core QuestStatus enum values (QuestDef.h).
inline constexpr char const* QuestStatusName(std::uint32_t status)
{
    switch (status)
    {
        case 0: return "none";
        case 1: return "complete";
        case 3: return "incomplete";
        case 5: return "failed";
        case 6: return "rewarded";
        default: return "unknown";
    }
}

// ---- response formatting (single-line JSON, fixed field order) ----

inline std::string JsonString(std::string_view value)
{
    std::string out = "\"";
    for (unsigned char const c : value)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x20)
            out.push_back('?');
        else
            out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

// Positions are telemetry only (never a digest input); two decimals keep the wire stable.
inline std::string Fixed2(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
    return buffer;
}

inline std::string FormatError(Verb verb, std::uint32_t guid, std::string_view code)
{
    return std::string("{\"ok\":false,\"operation\":\"") + VerbOperation(verb) + "\",\"guid\":" +
           std::to_string(guid) + ",\"error\":" + JsonString(code) + "}";
}

inline std::string FormatLogin(std::uint32_t guid, std::string_view state)
{
    return std::string("{\"ok\":true,\"operation\":\"probe_login\",\"guid\":") + std::to_string(guid) +
           ",\"state\":" + JsonString(state) + "}";
}

struct Position
{
    std::uint32_t map = 0;
    std::uint32_t zone = 0;
    std::uint32_t area = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;
};

inline std::string FormatPosition(Position const& p)
{
    return "{\"map\":" + std::to_string(p.map) + ",\"zone\":" + std::to_string(p.zone) + ",\"area\":" +
           std::to_string(p.area) + ",\"x\":" + Fixed2(p.x) + ",\"y\":" + Fixed2(p.y) + ",\"z\":" + Fixed2(p.z) +
           ",\"o\":" + Fixed2(p.orientation) + "}";
}

struct Objective
{
    char const* kind = "creature";  // creature | gameobject | item
    std::uint32_t index = 0;
    std::uint32_t entry = 0;
    std::uint32_t current = 0;
    std::uint32_t required = 0;
};

struct QuestFacts
{
    std::uint32_t questId = 0;
    std::uint32_t status = 0;       // core QuestStatus
    bool rewarded = false;
    std::int32_t slot = -1;         // -1 = not in the quest log
    std::vector<Objective> objectives;
};

inline std::string FormatQuest(QuestFacts const& q)
{
    std::string out = "{\"id\":" + std::to_string(q.questId) + ",\"status\":" +
                      JsonString(QuestStatusName(q.status)) + ",\"rewarded\":" + (q.rewarded ? "true" : "false") +
                      ",\"slot\":" + std::to_string(q.slot) + ",\"objectives\":[";
    for (std::size_t i = 0; i < q.objectives.size(); ++i)
    {
        Objective const& o = q.objectives[i];
        if (i)
            out += ',';
        out += "{\"kind\":" + JsonString(o.kind) + ",\"index\":" + std::to_string(o.index) + ",\"entry\":" +
               std::to_string(o.entry) + ",\"current\":" + std::to_string(o.current) + ",\"required\":" +
               std::to_string(o.required) + "}";
    }
    return out + "]}";
}

struct StatusFacts
{
    std::uint32_t guid = 0;
    std::uint32_t level = 0;
    std::uint32_t xp = 0;
    bool alive = false;
    bool inCombat = false;
    Position position;
    std::uint32_t questLogCount = 0;
    bool hasQuest = false;
    QuestFacts quest;
};

inline std::string FormatStatus(StatusFacts const& s)
{
    std::string out = "{\"ok\":true,\"operation\":\"probe_status\",\"guid\":" + std::to_string(s.guid) +
                      ",\"level\":" + std::to_string(s.level) + ",\"xp\":" + std::to_string(s.xp) +
                      ",\"alive\":" + (s.alive ? "true" : "false") + ",\"in_combat\":" +
                      (s.inCombat ? "true" : "false") + ",\"position\":" + FormatPosition(s.position) +
                      ",\"quest_log_count\":" + std::to_string(s.questLogCount) + ",\"quest\":";
    out += s.hasQuest ? FormatQuest(s.quest) : "null";
    return out + "}";
}

struct Starter
{
    char const* kind = "creature";  // creature | gameobject
    std::uint32_t entry = 0;
    std::uint64_t spawnId = 0;
    Position position;
};

struct PlaceFacts
{
    std::uint32_t guid = 0;
    std::uint32_t questId = 0;
    std::uint32_t candidates = 0;
    Starter starter;
    std::uint32_t previousStatus = 0;
    bool previousRewarded = false;
    std::uint32_t questItemsDestroyed = 0;
    std::vector<std::uint32_t> droppedOthers;
    char const* teleport = "queued";
};

inline std::string FormatPlace(PlaceFacts const& p)
{
    std::string out = "{\"ok\":true,\"operation\":\"probe_place\",\"guid\":" + std::to_string(p.guid) +
                      ",\"quest_id\":" + std::to_string(p.questId) + ",\"candidates\":" +
                      std::to_string(p.candidates) + ",\"starter\":{\"kind\":" + JsonString(p.starter.kind) +
                      ",\"entry\":" + std::to_string(p.starter.entry) + ",\"spawn_id\":" +
                      std::to_string(p.starter.spawnId) + ",\"position\":" + FormatPosition(p.starter.position) +
                      "},\"cleared\":{\"previous_status\":" + JsonString(QuestStatusName(p.previousStatus)) +
                      ",\"previous_rewarded\":" + (p.previousRewarded ? "true" : "false") +
                      ",\"quest_items_destroyed\":" + std::to_string(p.questItemsDestroyed) + "},\"dropped_others\":[";
    for (std::size_t i = 0; i < p.droppedOthers.size(); ++i)
    {
        if (i)
            out += ',';
        out += std::to_string(p.droppedOthers[i]);
    }
    return out + "],\"teleport\":" + JsonString(p.teleport) + ",\"ledger\":\"contaminated:probe_setup\"}";
}
}  // namespace AutoWowProbePlace

#endif  // MOD_PLAYERBOTS_PROBE_PLACE_POLICY_H
