/*
 * AutoWoW strategic server module. Original custom implementation.
 * Released under GNU GPL v2, or (at your option) any later version.
 */

#include "StrategyPolicy.h"

#include "Config.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
constexpr std::uint64_t kDaySeconds = 86400;
constexpr std::uint32_t kIncomePeriodSeconds = 1800;
constexpr std::uint32_t kMaxCatchUpPeriods = 4;
constexpr std::size_t kMaxTrackedPlayers = 10000;
constexpr std::size_t kMaxTrackedPairs = 20000;

struct PairState
{
    std::uint32_t count = 0;
    std::uint64_t lastSeconds = 0;
};

struct ObserverState
{
    std::mutex mutex;
    bool initialized = false;
    std::uint64_t day = 0;
    std::unordered_map<std::string, PairState> pairs;
    std::unordered_map<std::string, std::uint32_t> points;
};

ObserverState gObserver;
std::atomic<bool> gObserveGlory{false};
std::atomic<bool> gObserveIncome{false};

std::uint64_t UnixSeconds()
{
    auto const seconds = GameTime::GetGameTime().count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
}

class StrategyWorldScript : public WorldScript
{
public:
    StrategyWorldScript() : WorldScript("AutoWowStrategyWorld", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE}) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        gObserveGlory.store(sConfigMgr->GetOption<bool>("AutoWow.Strategy.Glory.Observe", false));
        gObserveIncome.store(sConfigMgr->GetOption<bool>("AutoWow.Strategy.Territory.Observe", false));
    }

    void OnStartup() override
    {
        OnAfterConfigLoad(false);
        std::uint64_t const now = UnixSeconds();
        _lastObservedPeriod = now / kIncomePeriodSeconds;
        std::lock_guard<std::mutex> lock(gObserver.mutex);
        gObserver.day = now / kDaySeconds;
        gObserver.initialized = true;
        // This is session-local observation. There is deliberately no actual glory/reward balance.
        if (gObserveGlory.load() || gObserveIncome.load())
            LOG_INFO("autowow.strategy", "observer_started mode=observe_only day={} period={}",
                     gObserver.day, _lastObservedPeriod);
    }

    void OnUpdate(std::uint32_t /*diff*/) override
    {
        if (!gObserveGlory.load() && !gObserveIncome.load())
            return;

        std::uint64_t const now = UnixSeconds();
        std::uint64_t const day = now / kDaySeconds;
        if (gObserveGlory.load())
        {
            std::lock_guard<std::mutex> lock(gObserver.mutex);
            if (day > gObserver.day)
            {
                // Bounded housekeeping belongs on the world thread, not in a map-thread kill hook.
                gObserver.pairs.clear();
                gObserver.points.clear();
                gObserver.day = day;
            }
        }

        if (!gObserveIncome.load())
            return;

        auto const plan = AutoWowStrategy::PlanIncome(_lastObservedPeriod, now,
                                                      kIncomePeriodSeconds, kMaxCatchUpPeriods);
        if (!plan.valid || !plan.firstPeriod)
            return;

        // Timing receipt only. No territory ownership, stock, item creation, offline payout or XP is implied.
        LOG_INFO("autowow.strategy", "territory_periods_due mode=observe_only first={} last={} skipped={}",
                 plan.firstPeriod, plan.lastPeriod, plan.skippedPeriods);
        _lastObservedPeriod = plan.lastPeriod;
    }

private:
    std::uint64_t _lastObservedPeriod = 0;
};

class StrategyPlayerScript : public PlayerScript
{
public:
    StrategyPlayerScript() : PlayerScript("AutoWowStrategyPlayer", {PLAYERHOOK_ON_PVP_KILL}) { }

    void OnPlayerPVPKill(Player* killer, Player* victim) override
    {
        if (!gObserveGlory.load() || !killer || !victim || !killer->GetSession() || !victim->GetSession())
            return;

        std::string const killerGuid = killer->GetGUID().ToString();
        std::string const victimGuid = victim->GetGUID().ToString();
        std::string const pair = killerGuid < victimGuid ? killerGuid + ":" + victimGuid :
                                                         victimGuid + ":" + killerGuid;
        std::uint64_t const now = UnixSeconds();
        bool const bothInBattleground = killer->InBattleground() && victim->InBattleground();
        auto const killerTeam = bothInBattleground ? killer->GetBgTeamId() : killer->GetTeamId();
        auto const victimTeam = bothInBattleground ? victim->GetBgTeamId() : victim->GetTeamId();
        AutoWowStrategy::GloryFacts const facts{
            true,
            killer == victim,
            killer->GetSession()->GetAccountId() == victim->GetSession()->GetAccountId(),
            killer->GetSession()->IsHeadless() && victim->GetSession()->IsHeadless(),
            killerTeam != TEAM_NEUTRAL && victimTeam != TEAM_NEUTRAL && killerTeam != victimTeam,
            killer->IsInWorld() && victim->IsInWorld() && killer->GetMapId() == victim->GetMapId() &&
                killer->GetInstanceId() == victim->GetInstanceId() &&
                killer->InBattleground() == victim->InBattleground(),
            bool(killer->duel) || bool(victim->duel),
            killer->InArena() || victim->InArena(),
            victim->HasNoPVPCreditAura(),
            victim->GetLevel(),
            static_cast<std::uint8_t>(Acore::XP::GetGrayLevel(killer->GetLevel()))
        };

        // No Player pointers escape this call. No DB queries, movement, honor/XP or inventory operations.
        AutoWowStrategy::GloryDecision decision;
        std::uint32_t total = 0;
        {
            std::lock_guard<std::mutex> lock(gObserver.mutex);
            if (!gObserver.initialized || now / kDaySeconds != gObserver.day)
                return;

            auto const foundPair = gObserver.pairs.find(pair);
            auto const foundPlayer = gObserver.points.find(killerGuid);
            PairState const prior = foundPair == gObserver.pairs.end() ? PairState{} : foundPair->second;
            total = foundPlayer == gObserver.points.end() ? 0 : foundPlayer->second;
            decision = AutoWowStrategy::EvaluateGlory(facts, {}, prior.count, total, now, prior.lastSeconds);
            if (decision.candidatePoints)
            {
                if ((foundPair == gObserver.pairs.end() && gObserver.pairs.size() >= kMaxTrackedPairs) ||
                    (foundPlayer == gObserver.points.end() && gObserver.points.size() >= kMaxTrackedPlayers))
                    return;

                gObserver.pairs[pair] = {prior.count + 1, now};
                total += decision.candidatePoints;
                gObserver.points[killerGuid] = total;
            }
        }

        LOG_INFO("autowow.strategy",
                 "glory_candidate mode=observe_only killer={} victim={} reason={} points_preview={} day_preview={}",
                 killerGuid, victimGuid, AutoWowStrategy::GloryReasonName(decision.reason),
                 decision.candidatePoints, total);
    }
};
} // namespace

void Addmod_autowow_strategyScripts()
{
    new StrategyWorldScript();
    new StrategyPlayerScript();
}
