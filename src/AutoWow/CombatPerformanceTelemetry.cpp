/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatPerformanceTelemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "AiObjectContext.h"
#include "Creature.h"
#include "DotLifetimeGate.h"
#include "GameTime.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TacticalRuntime.h"
#include "ThreatManager.h"
#include "Unit.h"
#include "UnitScript.h"

namespace AutoWowCombatPerformanceTelemetry
{
namespace
{
using CounterStore = std::unordered_map<std::uint32_t, RollingCounters>;

// v1 window store. The hooks run on map threads (MapUpdate.Threads), so every access - including the
// pointers returned by Find/FindOrCreate - is under this lock. Never held together with gLifetimeLock.
// ponytail: one global lock (uncontended cost ~ tens of ns per hook); shard by guid if it ever shows.
std::mutex gWindowLock;

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

void LifetimeCounters::Update(std::uint64_t diffMs, bool inCombat, bool dead, bool starved)
{
    SaturatingAdd(totals.wallMs, diffMs);
    if (dead)
        SaturatingAdd(totals.deadMs, diffMs);
    if (!inCombat)
        return;
    SaturatingAdd(totals.combatMs, diffMs);
    if (starved)
        SaturatingAdd(totals.starvedMs, diffMs);
    SaturatingAdd(recentCombatMs, diffMs);
    if (recentCombatMs > kRecentDpsWindowMs)
    {
        recentCombatMs /= 2;
        recentDamage /= 2;
    }
}

void LifetimeCounters::RecordFight() { SaturatingIncrement(totals.fights); }

void LifetimeCounters::RecordDamageDone(std::uint64_t nowMs, std::uint64_t amount, std::uint64_t creatureKey)
{
    SaturatingAdd(totals.damageDone, amount);
    SaturatingAdd(recentDamage, amount);
    if (!creatureKey)
        return;

    // Deterministic slot choice: the existing engagement, else the first free or expired slot, else the
    // oldest engagement (lowest index on ties).
    std::size_t slot = kTtkTracked;
    std::size_t oldest = 0;
    for (std::size_t k = 0; k < kTtkTracked; ++k)
    {
        Engagement const& e = engaged[k];
        if (e.key == creatureKey)
            return;
        bool const expired = !e.key || nowMs < e.firstMs || nowMs - e.firstMs >= kTtkEngageExpiryMs;
        if (expired && slot == kTtkTracked)
            slot = k;
        if (e.firstMs < engaged[oldest].firstMs)
            oldest = k;
    }
    if (slot == kTtkTracked)
        slot = oldest;
    engaged[slot] = Engagement{creatureKey, nowMs};
}

void LifetimeCounters::RecordDamageTaken(std::uint64_t amount) { SaturatingAdd(totals.damageTaken, amount); }

void LifetimeCounters::RecordHealing(std::uint64_t amount) { SaturatingAdd(totals.healing, amount); }

void LifetimeCounters::RecordDeath() { SaturatingIncrement(totals.deaths); }

void LifetimeCounters::RecordCast(bool onGcd)
{
    SaturatingIncrement(totals.casts);
    if (onGcd)
        SaturatingIncrement(totals.gcdCasts);
}

void LifetimeCounters::RecordKill(std::uint64_t nowMs, std::uint64_t creatureKey, std::int32_t levelDelta)
{
    SaturatingIncrement(totals.kills);
    for (Engagement& e : engaged)
    {
        if (!creatureKey || e.key != creatureKey)
            continue;
        std::uint64_t const ttk = nowMs >= e.firstMs ? nowMs - e.firstMs : 0;
        e = Engagement{};
        if (ttk >= kTtkEngageExpiryMs)
            return;  // stale engagement (evade / respawn under the same guid): a kill, not a TTK sample
        SaturatingIncrement(totals.ttkCount);
        if (pendingCount < kTtkPendingMax)
            pending[pendingCount++] = TtkSample{
                static_cast<std::uint32_t>(std::min<std::uint64_t>(ttk, std::numeric_limits<std::uint32_t>::max())),
                levelDelta};
        else
            SaturatingIncrement(totals.ttkDropped);
        return;
    }
}

void LifetimeCounters::RecordDotSkip(std::uint64_t skipKey)
{
    if (skipKey && skipKey == lastDotSkipKey)
        return;
    lastDotSkipKey = skipKey;
    SaturatingIncrement(totals.dotSkips);
}

bool LifetimeCounters::EmitDue(std::uint64_t nowMs, std::uint64_t intervalMs)
{
    if (!intervalMs)
        return false;
    if (!emitArmed)
    {
        emitArmed = true;
        lastEmitMs = nowMs;
        return false;
    }
    if (nowMs >= lastEmitMs && nowMs - lastEmitMs < intervalMs)
        return false;
    lastEmitMs = nowMs;
    return true;
}

std::string LifetimeCounters::DrainEmitFields(std::uint32_t classId)
{
    std::string out;
    out.reserve(320 + pendingCount * 16);
    auto field = [&out](char const* name, std::uint64_t value)
    {
        out += ",\"";
        out += name;
        out += "\":";
        out += std::to_string(value);
    };
    field("cv", tacticsTracked ? kLifetimeSchemaVersionTactics : kLifetimeSchemaVersion);
    field("cls", classId);
    field("wall_ms", totals.wallMs);
    field("combat_ms", totals.combatMs);
    field("dead_ms", totals.deadMs);
    field("starved_ms", totals.starvedMs);
    field("fights", totals.fights);
    field("kills", totals.kills);
    field("deaths", totals.deaths);
    field("dmg", totals.damageDone);
    field("taken", totals.damageTaken);
    field("heal", totals.healing);
    field("casts", totals.casts);
    field("gcd_casts", totals.gcdCasts);
    field("dot_skips", totals.dotSkips);
    field("ttk_n", totals.ttkCount);
    field("ttk_drop", totals.ttkDropped);
    out += ",\"ttk\":[";
    for (std::size_t k = 0; k < pendingCount; ++k)
    {
        if (k)
            out.push_back(',');
        out.push_back('[');
        out += std::to_string(pending[k].ms);
        out.push_back(',');
        out += std::to_string(pending[k].levelDelta);
        out.push_back(']');
    }
    out.push_back(']');
    pendingCount = 0;
    if (tacticsTracked)
    {
        out += ",\"tac_ms\":[";
        bool first = true;
        for (std::size_t id = 0; id < kTacticSlots; ++id)
        {
            if (!tacticMs[id])
                continue;
            if (!first)
                out.push_back(',');
            first = false;
            out.push_back('[');
            out += std::to_string(id);
            out.push_back(',');
            out += std::to_string(tacticMs[id]);
            out.push_back(']');
        }
        out += "],\"arm\":";
        out += std::to_string(tacticArm);
    }
    return out;
}

void LifetimeCounters::RecordTactic(std::uint8_t tacticId, std::uint32_t ms, std::uint8_t arm)
{
    tacticsTracked = true;
    tacticArm = arm;
    if (tacticId < kTacticSlots)
        SaturatingAdd(tacticMs[tacticId], ms);
}

std::uint64_t LifetimeCounters::EngagedAtMs(std::uint64_t creatureKey) const
{
    if (!creatureKey)
        return 0;
    for (Engagement const& e : engaged)
        if (e.key == creatureKey)
            return e.firstMs;
    return 0;
}

std::uint32_t LifetimeCounters::RecentDps() const
{
    if (recentCombatMs < kRecentDpsMinMs)
        return 0;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(recentDamage * 1000 / recentCombatMs, std::numeric_limits<std::uint32_t>::max()));
}

void RecordDamageDone(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    std::lock_guard<std::mutex> guard(gWindowLock);
    FindOrCreate(botGuid)->RecordDamageDone(nowMs, amount);
}

void RecordEffectiveHealing(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    std::lock_guard<std::mutex> guard(gWindowLock);
    FindOrCreate(botGuid)->RecordEffectiveHealing(nowMs, amount);
}

void RecordDamageTaken(std::uint32_t botGuid, std::uint64_t nowMs, std::uint64_t amount)
{
    if (!amount)
        return;

    std::lock_guard<std::mutex> guard(gWindowLock);
    FindOrCreate(botGuid)->RecordDamageTaken(nowMs, amount);
}

void RecordDeath(std::uint32_t botGuid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gWindowLock);
    FindOrCreate(botGuid)->RecordDeath(nowMs);
}

void RecordCombatEntry(std::uint32_t botGuid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gWindowLock);
    FindOrCreate(botGuid)->RecordCombatEntry(nowMs);
}

void RecordCombatExit(std::uint32_t botGuid, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(gWindowLock);
    if (RollingCounters* counters = Find(botGuid))
        counters->RecordCombatExit(nowMs);
}

void RecordThreatSample(std::uint32_t botGuid, std::uint64_t nowMs, std::uint32_t threatenedByMe,
                        std::uint32_t ownersTargetingBot, float highestThreat)
{
    bool const hasThreat = threatenedByMe != 0 || ownersTargetingBot != 0 || highestThreat > 0.0f;
    std::lock_guard<std::mutex> guard(gWindowLock);
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
    std::lock_guard<std::mutex> guard(gWindowLock);
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
    std::lock_guard<std::mutex> guard(gWindowLock);
    Store().erase(botGuid);
}

static_assert(kPowerMana == POWER_MANA && kPowerRage == POWER_RAGE && kPowerEnergy == POWER_ENERGY);

namespace
{
// Lifetime store. Unit/Player updates run on map threads (MapUpdate.Threads), so like the v1 window
// store this one is mutex-guarded (its own lock). Touched only when AutoWow.CombatTelemetry.Enable is on.
std::mutex gLifetimeLock;
std::unordered_map<std::uint32_t, LifetimeCounters> gLifetime;

// Runs fn on the bot's counters under the lock, creating them on first use (bounded; beyond
// kMaxLifetimeBots new bots are simply not tracked - existing totals are never evicted).
template <typename Fn>
void WithLifetime(std::uint32_t botGuid, Fn&& fn)
{
    std::lock_guard<std::mutex> guard(gLifetimeLock);
    auto it = gLifetime.find(botGuid);
    if (it == gLifetime.end())
    {
        if (gLifetime.size() >= kMaxLifetimeBots)
            return;
        it = gLifetime.emplace(botGuid, LifetimeCounters{}).first;
    }
    fn(it->second);
}

std::uint64_t CreatureKey(Unit* unit)
{
    return unit && unit->IsCreature() ? unit->GetGUID().GetRawValue() : 0;
}

void UpdateLifetime(Player* bot, std::uint32_t botGuid, std::uint32_t diff)
{
    bool const inCombat = bot->IsInCombat();
    bool const dead = !bot->IsAlive();
    Powers const power = bot->getPowerType();
    bool const starved = inCombat && IsResourceStarved(static_cast<std::uint8_t>(power), bot->GetPower(power),
                                                       bot->GetMaxPower(power));
    std::uint64_t const nowMs = NowMs();
    std::string fields;
    WithLifetime(botGuid, [&](LifetimeCounters& c)
    {
        c.Update(diff, inCombat, dead, starved);
        if (c.EmitDue(nowMs, detail::gLogIntervalMs))
            fields = c.DrainEmitFields(bot->getClass());
    });
    // C6: logged outside the store lock. EmitCombat is a no-op unless AutoWow.Ledger.Enable is on.
    if (!fields.empty())
        AutoWowQuestLedger::EmitCombat(bot, fields);
}
}  // namespace

std::uint32_t RecentDpsFor(std::uint32_t botGuid)
{
    if (!TelemetryEnabled())
        return 0;
    std::lock_guard<std::mutex> guard(gLifetimeLock);
    auto const it = gLifetime.find(botGuid);
    return it == gLifetime.end() ? 0 : it->second.RecentDps();
}

void RecordDotSkip(std::uint32_t botGuid, std::uint64_t skipKey)
{
    if (TelemetryEnabled())
        WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordDotSkip(skipKey); });
}

void RecordTactic(std::uint32_t botGuid, std::uint8_t tacticId, std::uint32_t ms, std::uint8_t arm)
{
    if (TelemetryEnabled())
        WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordTactic(tacticId, ms, arm); });
}

}  // namespace AutoWowCombatPerformanceTelemetry

namespace AutoWowDotLifetimeGate
{
bool Allows(PlayerbotAI* botAI, Unit* target, std::string_view spell)
{
    if (!Enabled() || !botAI || !target || !botAI->GetBot())
        return true;
    Kind const kind = Classify(spell);
    if (kind == Kind::None)
        return true;

    Player* const bot = botAI->GetBot();
    Creature* const creature = target->ToCreature();
    std::uint32_t const rank = creature ? creature->GetCreatureTemplate()->rank : 0;
    Input in;
    in.kind = kind;
    // Players (PvP) are never gated; elites, rare elites, bosses and dungeon/world bosses keep their DoTs.
    in.bossOrElite = !creature || rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE ||
                     rank == CREATURE_ELITE_WORLDBOSS || creature->IsDungeonBoss() || creature->isWorldBoss();
    if (in.bossOrElite)
        return true;

    std::uint64_t const maxHp = target->GetMaxHealth();
    in.hp = target->GetHealth();
    in.hpPct = maxHp ? static_cast<std::uint32_t>(in.hp * 100 / maxHp) : 100;
    in.botLevel = bot->GetLevel();

    std::string const name(spell);
    if (Aura* aura = botAI->GetAura(name, target, true))
    {
        in.remainingMs = aura->GetDuration() > 0 ? static_cast<std::uint32_t>(aura->GetDuration()) : 0;
        in.durationMs = aura->GetMaxDuration() > 0 ? static_cast<std::uint32_t>(aura->GetMaxDuration()) : 0;
    }
    else if (std::uint32_t const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get())
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            in.durationMs = info->GetMaxDuration() > 0 ? static_cast<std::uint32_t>(info->GetMaxDuration()) : 0;
    }

    // TTK inputs come from the combat telemetry store (AutoWow.CombatTelemetry.Enable). Without it the
    // estimate is unknown and only the HP floor applies.
    namespace T = AutoWowCombatPerformanceTelemetry;
    std::uint32_t const botGuid = static_cast<std::uint32_t>(bot->GetGUID().GetCounter());
    std::uint64_t const targetKey = target->GetGUID().GetRawValue();
    std::uint32_t dps = 0;
    std::uint64_t engagedMs = 0;
    if (T::TelemetryEnabled())
    {
        std::uint64_t const nowMs = T::NowMs();
        std::lock_guard<std::mutex> guard(T::gLifetimeLock);
        auto const it = T::gLifetime.find(botGuid);
        if (it != T::gLifetime.end())
        {
            dps = it->second.RecentDps();
            std::uint64_t const first = it->second.EngagedAtMs(targetKey);
            if (first && nowMs >= first)
                engagedMs = nowMs - first;
        }
    }
    in.estTtkMs = EstimateTtkMs(in.hp, maxHp, engagedMs, dps);

    if (Evaluate(in, detail::gParams) == Verdict::Allow)
        return true;
    T::RecordDotSkip(botGuid, targetKey ^ std::hash<std::string_view>{}(spell));
    return false;
}
}  // namespace AutoWowDotLifetimeGate

namespace AutoWowCombatPerformanceTelemetry
{
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
        {
            RecordEffectiveHealing(botGuid, NowMs(), gain);
            if (TelemetryEnabled())
                WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordHealing(gain); });
        }
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        std::uint32_t botGuid = 0;
        std::uint64_t const nowMs = NowMs();
        if (IsAttributedPlayerbot(attacker, botGuid))
        {
            RecordDamageDone(botGuid, nowMs, damage);
            if (TelemetryEnabled() && damage)
                WithLifetime(botGuid,
                             [&](LifetimeCounters& c) { c.RecordDamageDone(nowMs, damage, CreatureKey(victim)); });
        }

        if (IsPlayerbotPlayer(victim, botGuid))
        {
            RecordDamageTaken(botGuid, nowMs, damage);
            if (TelemetryEnabled() && damage)
                WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordDamageTaken(damage); });
        }
    }

    void OnUnitUpdate(Unit* unit, uint32 diff) override
    {
        std::uint32_t botGuid = 0;
        if (!IsPlayerbotPlayer(unit, botGuid))
            return;

        if (TelemetryEnabled())
            UpdateLifetime(unit->ToPlayer(), botGuid, diff);

        std::uint64_t const nowMs = NowMs();
        ThreatSample const sample = ReadThreatSample(unit->ToPlayer());
        std::lock_guard<std::mutex> guard(gWindowLock);
        if (RollingCounters* counters = Find(botGuid))
        {
            counters->Tick(nowMs);
            counters->RecordThreatSample(nowMs, sample.threatenedByMe, sample.ownersTargetingBot,
                                         sample.highestThreat);
        }
        else if (sample.threatenedByMe || sample.ownersTargetingBot || sample.highestThreat > 0.0f)
        {
            // Same effect as RecordThreatSample(botGuid, ...) for an untracked bot, without re-locking.
            FindOrCreate(botGuid)->RecordThreatSample(nowMs, sample.threatenedByMe, sample.ownersTargetingBot,
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

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        std::uint32_t botGuid = 0;
        if (IsPlayerbotPlayer(unit, botGuid))
        {
            RecordDeath(botGuid, NowMs());
            if (TelemetryEnabled())
                WithLifetime(botGuid, [](LifetimeCounters& c) { c.RecordDeath(); });
            return;
        }

        // AutoWow.Tactics.Observe/Enable (default 0): engagement kill count. One cached bool when off.
        if (AutoWowTactics::Tracking() && unit && unit->IsCreature() && IsAttributedPlayerbot(killer, botGuid))
            AutoWowTactics::NoteKill(botGuid);

        // C4: a creature killed by a bot's (or its pet's) killing blow closes that bot's engagement.
        if (TelemetryEnabled() && unit && unit->IsCreature() && IsAttributedPlayerbot(killer, botGuid))
        {
            Player* const owner = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
            std::int32_t const levelDelta =
                static_cast<std::int32_t>(unit->GetLevel()) - static_cast<std::int32_t>(owner->GetLevel());
            std::uint64_t const nowMs = NowMs();
            WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordKill(nowMs, CreatureKey(unit), levelDelta); });
        }
    }
};

// AutoWow.CombatTelemetry.Enable-gated player-side hooks. Registered unconditionally (the flag is read
// after script registration); every handler early-returns on the cached flag.
class CombatTelemetryPlayerScript : public PlayerScript
{
public:
    CombatTelemetryPlayerScript()
        : PlayerScript("AutoWowCombatTelemetryPlayer", {
            PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
            PLAYERHOOK_ON_SPELL_CAST,
            PLAYERHOOK_ON_LOGOUT
        })
    {
    }

    // C1: CombatManager::UpdateOwnerCombatState fires this on every false->true transition.
    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (!TelemetryEnabled())
            return;
        std::uint32_t botGuid = 0;
        if (!IsPlayerbotPlayer(player, botGuid))
            return;
        RecordCombatEntry(botGuid, NowMs());
        WithLifetime(botGuid, [](LifetimeCounters& c) { c.RecordFight(); });
    }

    // C2 casts / GCD casts (reducer: GCD utilisation estimate). Triggered casts are not bot decisions.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        // AutoWow.Tactics.Observe/Enable (default 0): engagement shield / scream / cast counts.
        if (AutoWowTactics::Tracking() && spell && !spell->IsTriggered())
            AutoWowTactics::NoteCast(player, spell->GetSpellInfo());
        if (!TelemetryEnabled() || !spell || spell->IsTriggered())
            return;
        std::uint32_t botGuid = 0;
        if (!IsPlayerbotPlayer(player, botGuid))
            return;
        bool const onGcd = spell->GetSpellInfo() && spell->GetSpellInfo()->StartRecoveryTime > 0;
        WithLifetime(botGuid, [&](LifetimeCounters& c) { c.RecordCast(onGcd); });
    }

    void OnPlayerLogout(Player* player) override
    {
        if (AutoWowTactics::Tracking() && player)
            AutoWowTactics::Forget(static_cast<std::uint32_t>(player->GetGUID().GetCounter()));
        if (!TelemetryEnabled() || !player)
            return;
        std::uint32_t const botGuid = static_cast<std::uint32_t>(player->GetGUID().GetCounter());
        std::string fields;
        {
            std::lock_guard<std::mutex> guard(gLifetimeLock);
            auto const it = gLifetime.find(botGuid);
            if (it == gLifetime.end())
                return;
            // Final line so the tail since the last periodic emit is not lost.
            if (detail::gLogIntervalMs)
                fields = it->second.DrainEmitFields(player->getClass());
            gLifetime.erase(it);
        }
        if (!fields.empty())
            AutoWowQuestLedger::EmitCombat(player, fields);
    }
};

void LoadConfig()
{
    detail::gTelemetryEnabled = sConfigMgr->GetOption<bool>("AutoWow.CombatTelemetry.Enable", false);
    detail::gLogIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.CombatTelemetry.LogIntervalMs", 60000);
    AutoWowDotLifetimeGate::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Combat.DotLifetimeGate", false);
    AutoWowDotLifetimeGate::Params& gate = AutoWowDotLifetimeGate::detail::gParams;
    gate.minTargetHpPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Combat.DotMinTargetHpPct", 10);
    gate.minDurationPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Combat.DotMinDurationPct", 50);
    gate.highHpPerLevel = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Combat.DotHighHpPerLevel", 150);
}

void AddAutoWowCombatPerformanceTelemetryScript()
{
    new CombatPerformanceTelemetryScript();
    new CombatTelemetryPlayerScript();
}
}
