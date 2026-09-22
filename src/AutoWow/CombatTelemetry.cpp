/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatTelemetry.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include "AiObjectContext.h"
#include "Creature.h"
#include "Group.h"
#include "LastSpellCastValue.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ThreatManager.h"
#include "Unit.h"

namespace AutoWowCombatTelemetry
{
namespace
{
constexpr std::size_t kMaxThreatLinks = 32;

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
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

char const* JsonBool(bool value)
{
    return value ? "true" : "false";
}

float Percent(std::uint32_t current, std::uint32_t maximum)
{
    if (!maximum)
        return 0.0f;

    return static_cast<float>(current) * 100.0f / static_cast<float>(maximum);
}

char const* DeathStateName(DeathState state)
{
    switch (state)
    {
        case DeathState::Alive: return "alive";
        case DeathState::JustDied: return "just_died";
        case DeathState::Corpse: return "corpse";
        case DeathState::Dead: return "dead";
        case DeathState::JustRespawned: return "just_respawned";
        default: return "unknown";
    }
}

UnitView MakeUnitView(Unit* unit)
{
    UnitView view;
    if (!unit || !unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
        return view;

    view.present = true;
    view.guid = static_cast<std::uint32_t>(unit->GetGUID().GetCounter());
    view.entry = unit->ToPlayer() ? 0 : unit->GetEntry();
    view.name = unit->GetName();
    view.isPlayer = unit->ToPlayer() != nullptr;
    view.alive = unit->IsAlive();
    view.health = unit->GetHealth();
    view.maxHealth = unit->GetMaxHealth();
    view.healthPct = Percent(view.health, view.maxHealth);
    return view;
}

void AppendUnitView(std::ostringstream& out, UnitView const& view)
{
    if (!view.present)
    {
        out << "null";
        return;
    }

    out << "{\"guid\":" << view.guid
        << ",\"entry\":" << view.entry
        << ",\"name\":" << JsonString(view.name)
        << ",\"is_player\":" << JsonBool(view.isPlayer)
        << ",\"alive\":" << JsonBool(view.alive)
        << ",\"health\":{\"current\":" << view.health
        << ",\"max\":" << view.maxHealth
        << ",\"pct\":" << view.healthPct << "}}";
}

void AppendThreatLink(std::ostringstream& out, ThreatLink const& link)
{
    out << "{\"source\":";
    AppendUnitView(out, link.source);
    out << ",\"victim\":";
    AppendUnitView(out, link.victim);
    out << ",\"threat\":" << link.threat
        << ",\"source_targets_bot\":" << JsonBool(link.sourceTargetsBot) << "}";
}
}

std::string Serialize(Snapshot const& snapshot)
{
    std::ostringstream out;
    out << "{\"ok\":true,\"schema\":" << JsonString(kSchema)
        << ",\"version\":" << kVersion
        << ",\"telemetry\":{\"guid\":" << snapshot.guid
        << ",\"name\":" << JsonString(snapshot.name)
        << ",\"alive\":" << JsonBool(snapshot.alive)
        << ",\"death_state\":" << JsonString(snapshot.deathState)
        << ",\"in_combat\":" << JsonBool(snapshot.inCombat)
        << ",\"location\":{\"map_id\":" << snapshot.mapId
        << ",\"instance_id\":" << snapshot.instanceId << "}"
        << ",\"health\":{\"current\":" << snapshot.health
        << ",\"max\":" << snapshot.maxHealth
        << ",\"pct\":" << snapshot.healthPct << "}"
        << ",\"mana\":{\"current\":" << snapshot.mana
        << ",\"max\":" << snapshot.maxMana
        << ",\"pct\":" << snapshot.manaPct << "}"
        << ",\"power\":{\"type\":" << snapshot.powerType
        << ",\"current\":" << snapshot.power
        << ",\"max\":" << snapshot.maxPower << "}"
        << ",\"role\":{\"mask\":" << snapshot.roleMask
        << ",\"tank\":" << JsonBool(snapshot.roleTank)
        << ",\"healer\":" << JsonBool(snapshot.roleHealer)
        << ",\"dps\":" << JsonBool(snapshot.roleDps)
        << ",\"main_tank\":" << JsonBool(snapshot.mainTank)
        << ",\"explicit_main_tank\":" << JsonBool(snapshot.explicitMainTank)
        << ",\"assist_tank\":" << JsonBool(snapshot.assistTank)
        << ",\"group_tanks\":" << snapshot.groupTanks << "}"
        << ",\"group\":{\"members\":" << snapshot.groupMembers
        << ",\"leader_guid\":" << snapshot.groupLeaderGuid << "}"
        << ",\"victim\":";
    AppendUnitView(out, snapshot.victim);
    out << ",\"ai_target\":";
    AppendUnitView(out, snapshot.aiTarget);
    out << ",\"healer\":{\"target\":";
    AppendUnitView(out, snapshot.healerTarget);
    out << ",\"intent\":" << JsonString(snapshot.healerIntent) << "}"
        << ",\"threat\":{\"threatened_by_me\":" << snapshot.threatenedByMeCount
        << ",\"owners_targeting_bot\":" << snapshot.ownersTargetingBot
        << ",\"links_truncated\":" << JsonBool(snapshot.threatLinksTruncated)
        << ",\"links\":[";

    for (std::size_t index = 0; index < snapshot.threatLinks.size(); ++index)
    {
        if (index)
            out << ',';
        AppendThreatLink(out, snapshot.threatLinks[index]);
    }

    out << "]}"
        << ",\"recent\":{\"active_spell_id\":" << snapshot.activeSpellId
        << ",\"last_spell\":{\"id\":" << snapshot.lastSpellId
        << ",\"target_guid\":" << snapshot.lastSpellTargetGuid
        << ",\"time\":" << snapshot.lastSpellTime << "}"
        << ",\"counters\":{\"available\":" << JsonBool(snapshot.counters.available)
        << ",\"schema\":" << JsonString(AutoWowCombatPerformanceTelemetry::kSchema)
        << ",\"version\":" << AutoWowCombatPerformanceTelemetry::kVersion
        << ",\"tracked\":" << JsonBool(snapshot.counters.tracked)
        << ",\"active\":" << JsonBool(snapshot.counters.active)
        << ",\"window\":{\"start_unix_ms\":" << snapshot.counters.windowStartUnixMs
        << ",\"duration_ms\":" << snapshot.counters.windowDurationMs
        << ",\"max_ms\":" << snapshot.counters.maxWindowMs
        << ",\"idle_timeout_ms\":" << snapshot.counters.idleTimeoutMs << "}"
        << ",\"window_ms\":" << snapshot.counters.windowDurationMs
        << ",\"damage_done\":" << snapshot.counters.damageDone
        << ",\"effective_healing\":" << snapshot.counters.effectiveHealing
        << ",\"sources\":{\"damage\":\"UnitScript::OnDamage amount\","
           "\"healing\":\"UnitScript::OnHeal effective gain\","
           "\"attribution\":\"controlled units resolve to their playerbot owner\"}"
        << ",\"overhealing\":{\"available\":"
        << JsonBool(snapshot.counters.overhealingAvailable)
        << ",\"amount\":" << snapshot.counters.overhealing
        << ",\"reason\":\"UnitScript::OnHeal exposes effective gain only\"}"
        << ",\"damage_taken\":" << snapshot.counters.damageTaken
        << ",\"deaths\":" << snapshot.counters.deaths
        << ",\"combat\":{\"entries\":" << snapshot.counters.combatEntries
        << ",\"exits\":" << snapshot.counters.combatExits << "}"
        << ",\"aggro\":{\"threat_samples\":" << snapshot.counters.threatSamples
        << ",\"max_threatened_by_me\":" << snapshot.counters.maxThreatenedByMe
        << ",\"max_owners_targeting_bot\":" << snapshot.counters.maxOwnersTargetingBot
        << ",\"max_threat\":" << snapshot.counters.maxThreat << "}"
        << ",\"next_hook\":\"server combat event hooks active\"}}}}";
    return out.str();
}

std::string Build(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI)
        return "{\"ok\":false,\"error\":\"bot_unavailable\"}";

    Snapshot snapshot;
    snapshot.guid = static_cast<std::uint32_t>(bot->GetGUID().GetCounter());
    snapshot.name = bot->GetName();
    snapshot.alive = bot->IsAlive();
    snapshot.deathState = DeathStateName(bot->getDeathState());
    snapshot.inCombat = bot->IsInCombat();
    snapshot.mapId = bot->GetMapId();
    snapshot.instanceId = bot->GetInstanceId();
    snapshot.health = bot->GetHealth();
    snapshot.maxHealth = bot->GetMaxHealth();
    snapshot.healthPct = Percent(snapshot.health, snapshot.maxHealth);
    snapshot.mana = bot->GetPower(POWER_MANA);
    snapshot.maxMana = bot->GetMaxPower(POWER_MANA);
    snapshot.manaPct = Percent(snapshot.mana, snapshot.maxMana);
    snapshot.powerType = static_cast<std::uint32_t>(bot->getPowerType());
    snapshot.power = bot->GetPower(bot->getPowerType());
    snapshot.maxPower = bot->GetMaxPower(bot->getPowerType());

    snapshot.roleTank = PlayerbotAI::IsTank(bot);
    snapshot.roleHealer = PlayerbotAI::IsHeal(bot);
    snapshot.roleDps = PlayerbotAI::IsDps(bot);
    if (snapshot.roleTank)
        snapshot.roleMask |= static_cast<std::uint32_t>(BOT_ROLE_TANK);
    if (snapshot.roleHealer)
        snapshot.roleMask |= static_cast<std::uint32_t>(BOT_ROLE_HEALER);
    if (snapshot.roleDps)
        snapshot.roleMask |= static_cast<std::uint32_t>(BOT_ROLE_DPS);

    snapshot.mainTank = PlayerbotAI::IsMainTank(bot);
    snapshot.explicitMainTank = PlayerbotAI::IsExplicitMainTank(bot);
    snapshot.assistTank = PlayerbotAI::IsAssistTank(bot);
    snapshot.groupTanks = PlayerbotAI::GetGroupTankNum(bot);

    if (Group* group = bot->GetGroup())
    {
        snapshot.groupMembers = group->GetMembersCount();
        snapshot.groupLeaderGuid = static_cast<std::uint32_t>(group->GetLeaderGUID().GetCounter());
    }

    snapshot.victim = MakeUnitView(bot->GetVictim());

    if (AiObjectContext* context = botAI->GetAiObjectContext())
    {
        if (auto* currentTarget = context->GetValue<Unit*>("current target"))
            snapshot.aiTarget = MakeUnitView(currentTarget->Get());

        // PartyMemberToHeal is the same read-only AI value consumed by healer actions. Reading it
        // here exposes the healer's current intent without issuing an action or spell.
        if (snapshot.roleHealer)
        {
            if (auto* healerTarget = context->GetValue<Unit*>("party member to heal"))
                snapshot.healerTarget = MakeUnitView(healerTarget->Get());
        }

        if (auto* activeSpell = context->GetValue<std::uint32_t>("active spell"))
            snapshot.activeSpellId = activeSpell->Get();

        if (auto* lastSpell = context->GetValue<LastSpellCast&>("last spell cast"))
        {
            LastSpellCast& record = lastSpell->Get();
            snapshot.lastSpellId = record.id;
            snapshot.lastSpellTargetGuid = static_cast<std::uint32_t>(record.target.GetCounter());
            snapshot.lastSpellTime = static_cast<std::int64_t>(record.timer);
        }
    }

    snapshot.counters = AutoWowCombatPerformanceTelemetry::SnapshotFor(snapshot.guid);
    snapshot.recentCountersAvailable = snapshot.counters.available;
    snapshot.recentCountersWindowMs = snapshot.counters.windowDurationMs >
        std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max() :
        static_cast<std::uint32_t>(snapshot.counters.windowDurationMs);

    if (!snapshot.roleHealer)
        snapshot.healerIntent = "not_healer";
    else if (!snapshot.healerTarget.present)
        snapshot.healerIntent = "no_target";
    else if (snapshot.healerTarget.guid == snapshot.guid)
        snapshot.healerIntent = "self";
    else
        snapshot.healerIntent = "target_selected";

    auto const& threatenedByMe = bot->GetThreatMgr().GetThreatenedByMeList();
    snapshot.threatenedByMeCount = static_cast<std::uint32_t>(threatenedByMe.size());
    snapshot.threatLinks.reserve(std::min(threatenedByMe.size(), kMaxThreatLinks));

    for (auto const& [guid, reference] : threatenedByMe)
    {
        (void)guid;
        if (!reference)
            continue;

        Creature* source = reference->GetOwner();
        if (!source)
            continue;

        Unit* sourceVictim = source->GetVictim();
        bool const sourceTargetsBot = sourceVictim && sourceVictim->GetGUID() == bot->GetGUID();
        if (sourceTargetsBot)
            ++snapshot.ownersTargetingBot;

        UnitView sourceView = MakeUnitView(source);
        if (!sourceView.present)
            continue;

        ThreatLink link;
        link.source = std::move(sourceView);
        link.victim = MakeUnitView(sourceVictim);
        link.threat = reference->GetThreat();
        link.sourceTargetsBot = sourceTargetsBot;
        snapshot.threatLinks.push_back(std::move(link));
    }

    std::sort(snapshot.threatLinks.begin(), snapshot.threatLinks.end(), [](ThreatLink const& left, ThreatLink const& right)
    {
        if (left.source.guid != right.source.guid)
            return left.source.guid < right.source.guid;
        return left.threat > right.threat;
    });

    if (snapshot.threatLinks.size() > kMaxThreatLinks)
    {
        snapshot.threatLinksTruncated = true;
        snapshot.threatLinks.resize(kMaxThreatLinks);
    }

    return Serialize(snapshot);
}
}
