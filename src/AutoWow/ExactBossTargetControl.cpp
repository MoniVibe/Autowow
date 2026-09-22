/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ExactBossTargetControl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace
{
std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool ParsePositiveUint32(std::string const& value, uint32& parsedValue)
{
    if (value.empty())
        return false;

    for (unsigned char character : value)
        if (!std::isdigit(character))
            return false;

    char* end = nullptr;
    unsigned long long const parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || !parsed || parsed > std::numeric_limits<uint32>::max())
        return false;

    parsedValue = static_cast<uint32>(parsed);
    return true;
}
}

namespace AutoWowExactBossTarget
{
bool ParseWireRequest(std::string const& requestText, WireRequest& request, std::string& error)
{
    request = {};
    error.clear();

    std::istringstream input(requestText);
    std::string command;
    std::string leaderToken;
    uint32 leaderGuid = 0;
    if (!(input >> command >> leaderToken) || Lowercase(command) != "engage" ||
        !ParsePositiveUint32(leaderToken, leaderGuid))
    {
        error = "engage requires a positive numeric leader GUID";
        return false;
    }

    std::string selector;
    if (!(input >> selector))
    {
        request.leaderGuid = leaderGuid;
        request.mode = EngageMode::Nearby;
        return true;
    }

    std::string const normalizedSelector = Lowercase(selector);
    if (normalizedSelector != "entry" && normalizedSelector != "player")
    {
        error = "engage selector must be entry or player";
        return false;
    }

    std::string targetToken;
    uint32 targetValue = 0;
    if (!(input >> targetToken) || !ParsePositiveUint32(targetToken, targetValue))
    {
        error = normalizedSelector == "entry"
            ? "engage entry requires a positive numeric creature entry"
            : "engage player requires a positive numeric player GUID";
        return false;
    }

    std::string extra;
    if (input >> extra)
    {
        error = normalizedSelector == "entry"
            ? "engage entry accepts exactly one creature entry"
            : "engage player accepts exactly one player GUID";
        return false;
    }

    request.leaderGuid = leaderGuid;
    if (normalizedSelector == "entry")
    {
        request.creatureEntry = targetValue;
        request.mode = EngageMode::CreatureEntry;
    }
    else
    {
        request.playerGuid = targetValue;
        request.mode = EngageMode::PlayerGuid;
    }
    return true;
}

bool ValidateTargetFacts(TargetFacts const& facts, std::string& error)
{
    error.clear();
    if (!facts.inWorld)
        error = "exact_engage_target_not_in_world";
    else if (!facts.sameMap)
        error = "exact_engage_target_wrong_map";
    else if (!facts.sameInstance)
        error = "exact_engage_target_wrong_instance";
    else if (!facts.alive)
        error = "exact_engage_target_dead";
    else if (!facts.hostile)
        error = "exact_engage_target_not_hostile";
    else if (!facts.attackable)
        error = "exact_engage_target_not_attackable";
    else if (!std::isfinite(facts.distance) || facts.distance < 0.0f || facts.distance > kMaxTargetDistance)
        error = "exact_engage_target_out_of_range";

    return error.empty();
}
}
