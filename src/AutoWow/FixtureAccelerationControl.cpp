/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "FixtureAccelerationControl.h"

#include "Config.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"

#include "FixtureFactoryControl.h"

#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
constexpr std::size_t kPacingMoveTypes = 7; // walk, run, backwards, swim, swim-back, flight, flight-back
constexpr float kMaxExactKillDistance = 120.0F;

struct SavedRates
{
    std::array<float, kPacingMoveTypes> rates{};
};

std::unordered_map<std::uint32_t, SavedRates> activeFixtures;

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
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << static_cast<char>(character); break;
        }
    }
    out << '"';
    return out.str();
}

std::string Error(std::uint32_t guid, std::string const& code)
{
    return "{\"ok\":false,\"order\":\"fixture-acceleration\",\"guid\":" +
        std::to_string(guid) + ",\"error\":" + JsonString(code) + "}";
}

Player* ResolveFixture(std::uint32_t guid, PlayerbotAI*& ai, std::string& error, bool requireReadyState)
{
    ai = nullptr;
    if (!guid)
    {
        error = "fixture_guid_required";
        return nullptr;
    }

    std::string const configured = sConfigMgr->GetOption<std::string>(
        AutoWowFixtureAcceleration::kMicroScenarioGuidsConfigKey, "");
    if (!AutoWowFixture::IsFixtureGuidAllowlisted(configured, guid))
    {
        error = "micro_scenario_guid_not_allowlisted";
        return nullptr;
    }

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
    if (!bot || !bot->IsInWorld())
    {
        error = "fixture_target_not_online";
        return nullptr;
    }

    ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai || ai->IsRealPlayer())
    {
        error = "fixture_target_not_playerbot";
        return nullptr;
    }
    if (requireReadyState && ai->IsAutoWowPaused())
    {
        error = "fixture_target_paused";
        return nullptr;
    }
    if (requireReadyState && !bot->IsAlive())
    {
        error = "fixture_target_dead";
        return nullptr;
    }
    if (requireReadyState && bot->IsInCombat())
    {
        error = "fixture_target_in_combat";
        return nullptr;
    }
    return bot;
}

void SetPacing(Player* bot, SavedRates const& baseline, float multiplier)
{
    for (std::size_t index = 0; index < kPacingMoveTypes; ++index)
    {
        float const baselineRate = baseline.rates[index];
        bot->SetSpeed(static_cast<UnitMoveType>(index),
            baselineRate > 0.0F ? baselineRate * multiplier : multiplier, true);
    }
}

std::string RateJson(Player* bot)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t index = 0; index < kPacingMoveTypes; ++index)
    {
        if (index)
            out << ',';
        out << bot->GetSpeedRate(static_cast<UnitMoveType>(index));
    }
    out << ']';
    return out.str();
}
}

namespace AutoWowFixtureAcceleration
{
bool IsValidPacingPercent(std::uint32_t pacingPercent)
{
    return pacingPercent == kFixturePacingPercent;
}

bool IsValidExactTarget(std::uint32_t entry, std::uint64_t stableSpawnId)
{
    return entry != 0 && stableSpawnId != 0 && stableSpawnId <= std::numeric_limits<std::uint64_t>::max();
}

std::string Accelerate(std::uint32_t guid, std::uint32_t pacingPercent)
{
    if (!IsValidPacingPercent(pacingPercent))
        return Error(guid, "fixture_pacing_must_be_1000_percent");

    PlayerbotAI* ai = nullptr;
    std::string error;
    Player* bot = ResolveFixture(guid, ai, error, true);
    if (!bot)
        return Error(guid, error);

    auto existing = activeFixtures.find(guid);
    if (existing != activeFixtures.end())
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"fixture-acceleration\",\"guid\":" << guid
            << ",\"state\":\"already_active\",\"pacing_percent\":1000"
            << ",\"pacing_multiplier\":10,\"rates\":" << RateJson(bot) << '}';
        return out.str();
    }

    SavedRates saved;
    for (std::size_t index = 0; index < kPacingMoveTypes; ++index)
        saved.rates[index] = bot->GetSpeedRate(static_cast<UnitMoveType>(index));
    activeFixtures.emplace(guid, saved);
    SetPacing(bot, saved, kFixturePacingMultiplier);

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"fixture-acceleration\",\"guid\":" << guid
        << ",\"state\":\"active\",\"pacing_percent\":1000,\"pacing_multiplier\":10"
        << ",\"original_rates\":[";
    for (std::size_t index = 0; index < kPacingMoveTypes; ++index)
    {
        if (index)
            out << ',';
        out << saved.rates[index];
    }
    out << "],\"rates\":" << RateJson(bot) << '}';
    return out.str();
}

std::string Disable(std::uint32_t guid)
{
    PlayerbotAI* ai = nullptr;
    std::string error;
    // Cleanup is allowed while paused, dead, or in combat so a failed scenario cannot strand
    // accelerated movement behind a readiness gate.
    Player* bot = ResolveFixture(guid, ai, error, false);
    if (!bot)
        return Error(guid, error);

    auto iterator = activeFixtures.find(guid);
    if (iterator == activeFixtures.end())
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"order\":\"fixture-acceleration\",\"guid\":" << guid
            << ",\"state\":\"already_inactive\"}";
        return out.str();
    }

    for (std::size_t index = 0; index < kPacingMoveTypes; ++index)
        bot->SetSpeed(static_cast<UnitMoveType>(index), iterator->second.rates[index], true);
    activeFixtures.erase(iterator);

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"fixture-acceleration\",\"guid\":" << guid
        << ",\"state\":\"inactive\",\"rates\":" << RateJson(bot) << '}';
    return out.str();
}

std::string KillExact(std::uint32_t guid, std::uint32_t entry, std::uint64_t stableSpawnId)
{
    if (!IsValidExactTarget(entry, stableSpawnId))
        return Error(guid, "fixture_exact_target_requires_entry_and_spawn");
    if (activeFixtures.find(guid) == activeFixtures.end())
        return Error(guid, "fixture_pacing_not_active");

    PlayerbotAI* ai = nullptr;
    std::string error;
    Player* bot = ResolveFixture(guid, ai, error, true);
    if (!bot)
        return Error(guid, error);

    Creature* target = ObjectAccessor::GetSpawnedCreatureByDBGUID(
        bot->GetMapId(), static_cast<ObjectGuid::LowType>(stableSpawnId));
    if (!target || !target->IsInWorld())
        return Error(guid, "fixture_exact_target_not_in_world");
    if (target->GetMapId() != bot->GetMapId() || target->GetInstanceId() != bot->GetInstanceId())
        return Error(guid, "fixture_exact_target_wrong_instance");
    if (target->GetEntry() != entry)
        return Error(guid, "fixture_exact_target_entry_mismatch");
    if (!target->IsAlive())
        return Error(guid, "fixture_exact_target_already_dead");
    if (target->isWorldBoss())
        return Error(guid, "fixture_exact_target_world_boss_forbidden");
    if (!bot->IsWithinDistInMap(target, kMaxExactKillDistance))
        return Error(guid, "fixture_exact_target_out_of_range");

    target->Kill(bot, target);

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"fixture-kill\",\"guid\":" << guid
        << ",\"entry\":" << entry << ",\"stable_spawn_id\":" << stableSpawnId
        << ",\"native_kill_requested\":true,\"synthetic_quest_credit\":false}";
    return out.str();
}
}
