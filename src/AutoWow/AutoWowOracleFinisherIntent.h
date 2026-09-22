/*
 * Exact, bounded finisher handoff for an Oracle-owned New-RPG quest.
 *
 * The tagged form is deliberately narrow:
 *   finisher:<decision-id>:<quest-id>:<signed-entry>:<stable-spawn-guid>
 *
 * The decision id is checked against the existing Oracle ownership lease by the world-thread
 * action. The quest and stable spawn identity are checked against the bot's live, in-log finisher
 * Value before the normal AzerothCore questgiver handlers are called. This is an intent/frame
 * handoff, never a quest mutation or reward authority.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_FINISHER_INTENT_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_FINISHER_INTENT_H

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace AutoWowOracleFinisher
{
struct Intent
{
    std::uint64_t decisionId = 0;
    std::uint32_t questId = 0;
    std::int32_t signedEntry = 0;
    std::uint64_t stableSpawnGuid = 0;
};

inline std::string Encode(Intent const& intent)
{
    if (intent.decisionId == 0 || intent.questId == 0 || intent.signedEntry == 0 ||
        intent.stableSpawnGuid == 0)
        return {};
    return "finisher:" + std::to_string(intent.decisionId) + ":" +
        std::to_string(intent.questId) + ":" + std::to_string(intent.signedEntry) + ":" +
        std::to_string(intent.stableSpawnGuid);
}

namespace Detail
{
inline bool ParseUnsigned(std::string_view token, std::uint64_t& out) noexcept
{
    if (token.empty())
        return false;
    std::uint64_t parsed = 0;
    auto const result = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size())
        return false;
    out = parsed;
    return true;
}

inline bool ParseSigned(std::string_view token, std::int64_t& out) noexcept
{
    if (token.empty())
        return false;
    std::int64_t parsed = 0;
    auto const result = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size())
        return false;
    out = parsed;
    return true;
}
}

inline bool Parse(std::string_view encoded, Intent& out) noexcept
{
    out = {};
    constexpr std::string_view prefix = "finisher:";
    if (!encoded.starts_with(prefix))
        return false;

    std::array<std::string_view, 4> fields{};
    std::size_t begin = prefix.size();
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        std::size_t const separator = encoded.find(':', begin);
        if (index + 1 < fields.size())
        {
            if (separator == std::string_view::npos)
                return false;
            fields[index] = encoded.substr(begin, separator - begin);
            begin = separator + 1;
        }
        else
        {
            if (separator != std::string_view::npos)
                return false;
            fields[index] = encoded.substr(begin);
        }
    }

    std::uint64_t decisionId = 0;
    std::uint64_t questId = 0;
    std::int64_t signedEntry = 0;
    std::uint64_t stableSpawnGuid = 0;
    if (!Detail::ParseUnsigned(fields[0], decisionId) || decisionId == 0 ||
        !Detail::ParseUnsigned(fields[1], questId) || questId == 0 ||
        questId > std::numeric_limits<std::uint32_t>::max() ||
        !Detail::ParseSigned(fields[2], signedEntry) || signedEntry == 0 ||
        signedEntry < std::numeric_limits<std::int32_t>::min() ||
        signedEntry > std::numeric_limits<std::int32_t>::max() ||
        !Detail::ParseUnsigned(fields[3], stableSpawnGuid) || stableSpawnGuid == 0)
        return false;

    out.decisionId = decisionId;
    out.questId = static_cast<std::uint32_t>(questId);
    out.signedEntry = static_cast<std::int32_t>(signedEntry);
    out.stableSpawnGuid = stableSpawnGuid;
    return true;
}
}

#endif  // MOD_PLAYERBOTS_AUTOWOW_ORACLE_FINISHER_INTENT_H
