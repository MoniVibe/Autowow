/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatPerformanceTelemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "ThreatManager.h"
#include "Unit.h"
#include "UnitScript.h"

namespace AutoWowCombatPerformanceTelemetry
{
namespace
{
using CounterStore = std::unordered_map<std::uint32_t, RollingCounters>;

CounterStore& Store()
{
    static CounterStore store;
    return store;
}

std::uint64_t NowMs()
{
    auto const now = GameTime::GetGameTimeMS().count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

void SaturatingAdd(std::uint64_t& target, std::uint64_t amount)
{
    if (std::numeric_limits<std::uint64_t>::max() - target < amount)
        target = std::numeric_limits<std::uint64_t>::max();
    else
        target += amount;
}

void SaturatingIncrement(std::uint32_t& target)
{
    if (target != std::numeric_limits<std::uint32_t>::max())
        ++target;
}

RollingCounters* Find(std::uint32_t botGuid)
{
    CounterStore& store = Store();
    auto existing = store.find(botGuid);
    return existing == store.end() ? nullptr : &existing->second;
}

RollingCounters* FindOrCreate(std::uint32_t botGuid)
{
    CounterStore& store = Store();
    if (RollingCounters* existing = Find(botGuid))
        return existing;

    if (store.size() >= kMaxTrackedBots)
    {
        auto oldest = store.begin();
        for (auto candidate = store.begin(); candidate != store.end(); ++candidate)
        {
            if (candidate->second.LastActivityMs() < oldest->second.LastActivityMs())
                oldest = candidate;
        }

        // The map is intentionally capped. In the unlikely event that more than the configured
        // capacity of bots emits events, the oldest window is evicted rather than allowing combat
        // telemetry to grow without bound.
        if (oldest != store.end())
            store.erase(oldest);
    }

    return &store.emplace(botGuid, RollingCounters{}).first->second;
}

bool IsPlayerbotPlayer(Unit* unit, std::uint32_t& botGuid)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    if (!botAI || botAI->IsRealPlayer())
        return false;

    botGuid = static_cast<std::uint32_t>(player->GetGUID().GetCounter());
    return true;
}

bool IsAttributedPlayerbot(Unit* unit, std::uint32_t& botGuid)
{
    Player* player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!player)
        return false;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    if (!botAI || botAI->IsRealPlayer())
        return false;

    botGuid = static_cast<std::uint32_t>(player->GetGUID().GetCounter());
    return true;
}

struct ThreatSample
{
    std::uint32_t threatenedByMe = 0;
    std::uint32_t ownersTargetingBot = 0;
    float highestThreat = 0.0f;
};

ThreatSample ReadThreatSample(Player* bot)
{
    ThreatSample sample;
    if (!bot)
        return sample;

    auto const& threatenedByMe = bot->GetThreatMgr().GetThreatenedByMeList();
    sample.threatenedByMe = static_cast<std::uint32_t>(threatenedByMe.size());

    for (auto const& [guid, reference] : threatenedByMe)
    {
        (void)guid;
        if (!reference)
            continue;

        sample.highestThreat = std::max(sample.highestThreat, reference->GetThreat());
        Creature* source = reference->GetOwner();
        if (source && source->GetVictim() == bot)
            ++sample.ownersTargetingBot;
    }

    return sample;
}
}

bool RollingCounters::ShouldReset(std::uint64_t nowMs) const
{
    if (!active)
        return false;

    if (nowMs < windowStartMs || nowMs < lastActivityMs)
        return true;

    return nowMs - windowStartMs >= kMaxWindowMs || nowMs - lastActivityMs >= kIdleTimeoutMs;
}

void RollingCounters::ResetWindow()
{
    active = false;
    windowStartMs = 0;
    lastActivityMs = 0;
    lastThreatSampleMs = 0;
    hasThreatSample = false;
    damageDone = 0;
    effectiveHealing = 0;
    damageTaken = 0;
    deaths = 0;
    combatEntries = 0;
    combatExits = 0;
    threatSamples = 0;
    maxThreatenedByMe = 0;
    maxOwnersTargetingBot = 0;
    maxThreat = 0.0f;
}

void RollingCounters::StartWindow(std::uint64_t nowMs)
{
    ResetWindow();
    active = true;
    windowStartMs = nowMs;
    lastActivityMs = nowMs;
}

void RollingCounters::Touch(std::uint64_t nowMs)
{
    if (!active || ShouldReset(nowMs))
        StartWindow(nowMs);
    else
        lastActivityMs = nowMs;
}

void RollingCounters::RecordDamageDone(std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    Touch(nowMs);
    SaturatingAdd(damageDone, amount);
}

void RollingCounters::RecordEffectiveHealing(std::uint64_t nowMs, std::uint64_t amount)
{
    // A zero effective gain is still a server heal event, so it starts/keeps the session window.
    Touch(nowMs);
    SaturatingAdd(effectiveHealing, amount);
}

void RollingCounters::RecordDamageTaken(std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    Touch(nowMs);
    SaturatingAdd(damageTaken, amount);
}

void RollingCounters::RecordDeath(std::uint64_t nowMs)
{
    Touch(nowMs);
    SaturatingIncrement(deaths);
}

void RollingCounters::RecordCombatEntry(std::uint64_t nowMs)
{
    Touch(nowMs);
    SaturatingIncrement(combatEntries);
}

void RollingCounters::RecordCombatExit(std::uint64_t nowMs)
{
    if (!active)
        return;

    if (ShouldReset(nowMs))
    {
        ResetWindow();
        return;
    }

    Touch(nowMs);
    SaturatingIncrement(combatExits);
}

void RollingCounters::RecordThreatSample(std::uint64_t nowMs, std::uint32_t threatenedByMe,
                                          std::uint32_t ownersTargetingBot, float highestThreat)
{
    bool const hasThreat = threatenedByMe != 0 || ownersTargetingBot != 0 || highestThreat > 0.0f;
    if (!hasThreat)
    {
        if (active && ShouldReset(nowMs))
            ResetWindow();
        return;
    }

    if (active && ShouldReset(nowMs))
    {
        ResetWindow();
    }
    else if (!active)
        StartWindow(nowMs);

    if (hasThreatSample && nowMs >= lastThreatSampleMs &&
        nowMs - lastThreatSampleMs < kThreatSampleIntervalMs)
        return;

    Touch(nowMs);
    hasThreatSample = true;
    lastThreatSampleMs = nowMs;
    SaturatingIncrement(threatSamples);
    maxThreatenedByMe = std::max(maxThreatenedByMe, threatenedByMe);
    maxOwnersTargetingBot = std::max(maxOwnersTargetingBot, ownersTargetingBot);
    if (std::isfinite(highestThreat) && highestThreat > maxThreat)
        maxThreat = highestThreat;
}

void RollingCounters::Tick(std::uint64_t nowMs)
{
    if (ShouldReset(nowMs))
        ResetWindow();
}

CounterSnapshot RollingCounters::Snapshot(std::uint64_t nowMs) const
{
    CounterSnapshot snapshot;
    if (!active || ShouldReset(nowMs))
        return snapshot;

    snapshot.tracked = true;
    snapshot.active = true;
    snapshot.windowStartUnixMs = windowStartMs;
    snapshot.windowDurationMs = nowMs >= windowStartMs ? nowMs - windowStartMs : 0;
    snapshot.damageDone = damageDone;
    snapshot.effectiveHealing = effectiveHealing;
    snapshot.damageTaken = damageTaken;
    snapshot.deaths = deaths;
    snapshot.combatEntries = combatEntries;
    snapshot.combatExits = combatExits;
    snapshot.threatSamples = threatSamples;
    snapshot.maxThreatenedByMe = maxThreatenedByMe;
    snapshot.maxOwnersTargetingBot = maxOwnersTargetingBot;
    snapshot.maxThreat = maxThreat;
    return snapshot;
}

void RecordDamageDone(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    FindOrCreate(botGuid)->RecordDamageDone(nowMs, amount);
}

void RecordEffectiveHealing(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    FindOrCreate(botGuid)->RecordEffectiveHealing(nowMs, amount);
}

void RecordDamageTaken(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    FindOrCreate(botGuid)->RecordDamageTaken(nowMs, amount);
}

void RecordDeath(std::uint32_t botGuid, std::uint64_t nowMs)
{
    FindOrCreate(botGuid)->RecordDeath(nowMs);
}

void RecordCombatEntry(std::uint32_t botGuid, std::uint64_t nowMs)
{
    FindOrCreate(botGuid)->RecordCombatEntry(nowMs);
}

void RecordCombatExit(std::uint32_t botGuid, std::uint64_t nowMs)
{
    if (RollingCounters* counters = Find(botGuid))
        counters->RecordCombatExit(nowMs);
}

void RecordThreatSample(std::uint32_t botGuid, std::uint64_t nowMs, std::uint32_t threatenedByMe,
                        std::uint32_t ownersTargetingBot, float highestThreat)
{
    bool const hasThreat = threatenedByMe != 0 || ownersTargetingBot != 0 || highestThreat > 0.0f;
    if (RollingCounters* counters = Find(botGuid))
    {
        counters->RecordThreatSample(nowMs, threatenedByMe, ownersTargetingBot, highestThreat);
        return;
    }

    if (hasThreat)
        FindOrCreate(botGuid)->RecordThreatSample(nowMs, threatenedByMe, ownersTargetingBot, highestThreat);
}

CounterSnapshot SnapshotFor(std::uint32_t botGuid)
{
    auto const& store = Store();
    auto const found = store.find(botGuid);
    if (found == store.end())
        return CounterSnapshot{};

    CounterSnapshot snapshot = found->second.Snapshot(NowMs());
    snapshot.tracked = true;
    return snapshot;
}

void Forget(std::uint32_t botGuid)
{
    Store().erase(botGuid);
}

class CombatPerformanceTelemetryScript : public UnitScript
{
public:
    CombatPerformanceTelemetryScript()
        : UnitScript("AutoWowCombatPerformanceTelemetry", true, std::vector<uint16>{
            UNITHOOK_ON_HEAL,
            UNITHOOK_ON_DAMAGE,
            UNITHOOK_ON_UNIT_UPDATE,
            UNITHOOK_ON_UNIT_ENTER_COMBAT,
            UNITHOOK_ON_UNIT_EXIT_COMBAT,
            UNITHOOK_ON_UNIT_DEATH
        })
    {
    }

    void OnHeal(Unit* healer, Unit* /*receiver*/, uint32& gain) override
    {
        std::uint32_t botGuid = 0;
        if (IsAttributedPlayerbot(healer, botGuid))
            RecordEffectiveHealing(botGuid, NowMs(), gain);
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        std::uint32_t botGuid = 0;
        std::uint64_t const nowMs = NowMs();
        if (IsAttributedPlayerbot(attacker, botGuid))
            RecordDamageDone(botGuid, nowMs, damage);

        if (IsPlayerbotPlayer(victim, botGuid))
            RecordDamageTaken(botGuid, nowMs, damage);
    }

    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        std::uint32_t botGuid = 0;
        if (!IsPlayerbotPlayer(unit, botGuid))
            return;

        std::uint64_t const nowMs = NowMs();
        ThreatSample const sample = ReadThreatSample(unit->ToPlayer());
        if (RollingCounters* counters = Find(botGuid))
        {
            counters->Tick(nowMs);
            counters->RecordThreatSample(nowMs, sample.threatenedByMe, sample.ownersTargetingBot,
                                         sample.highestThreat);
        }
        else if (sample.threatenedByMe || sample.ownersTargetingBot || sample.highestThreat > 0.0f)
        {
            RecordThreatSample(botGuid, nowMs, sample.threatenedByMe, sample.ownersTargetingBot,
                               sample.highestThreat);
        }
    }

    void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
    {
        std::uint32_t botGuid = 0;
        if (IsPlayerbotPlayer(unit, botGuid))
            RecordCombatEntry(botGuid, NowMs());
    }

    void OnUnitExitCombat(Unit* unit) override
    {
        std::uint32_t botGuid = 0;
        if (IsPlayerbotPlayer(unit, botGuid))
            RecordCombatExit(botGuid, NowMs());
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        std::uint32_t botGuid = 0;
        if (IsPlayerbotPlayer(unit, botGuid))
            RecordDeath(botGuid, NowMs());
    }
};

// AutoWow.CombatTelemetry.Enable-gated player-side hooks. Registered unconditionally (the flag is read
// after script registration); every handler early-returns on the cached flag.
class CombatTelemetryPlayerScript : public PlayerScript
{
public:
    CombatTelemetryPlayerScript()
        : PlayerScript("AutoWowCombatTelemetryPlayer", {
            PLAYERHOOK_ON_PLAYER_ENTER_COMBAT
        })
    {
    }

    // C1: CombatManager::UpdateOwnerCombatState fires this on every false->true transition.
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (!TelemetryEnabled())
            return;
        std::uint32_t botGuid = 0;
        if (IsPlayerbotPlayer(player, botGuid))
            RecordCombatEntry(botGuid, NowMs());
    }
};

void LoadConfig()
{
    detail::gTelemetryEnabled = sConfigMgr->GetOption<bool>("AutoWow.CombatTelemetry.Enable", false);
}

void AddAutoWowCombatPerformanceTelemetryScript()
{
    new CombatPerformanceTelemetryScript();
    new CombatTelemetryPlayerScript();
}
}
