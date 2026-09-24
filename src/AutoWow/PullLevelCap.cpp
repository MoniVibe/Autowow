/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PullLevelCap.h"

#include <atomic>

#include "Config.h"
#include "GameTime.h"
#include "Log.h"

namespace AutoWowPullCap
{
namespace
{
std::atomic<std::uint64_t> gSkipped{0};
std::atomic<std::uint64_t> gQuestAllowed{0};
std::atomic<std::int64_t> gNextLogMs{0};
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.PullLevelCap", false);
    Params& p = detail::gParams;
    p.maxLevelAbove = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.PullLevelCap.MaxLevelAbove", 2);
    p.elitePenalty = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.PullLevelCap.ElitePenalty", 3);
    p.healthyPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.PullLevelCap.HealthyPct", 80);
    p.aloneYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.PullLevelCap.AloneYards", 15);
}

void Note(bool skipped)
{
    std::uint64_t const s = skipped ? ++gSkipped : gSkipped.load();
    std::uint64_t const q = skipped ? gQuestAllowed.load() : ++gQuestAllowed;
    std::int64_t const now = GameTime::GetGameTimeMS().count();
    std::int64_t next = gNextLogMs.load();
    if (now < next || !gNextLogMs.compare_exchange_strong(next, now + 60000))
        return;
    LOG_INFO("playerbots", "[PullCap] over-cap candidates skipped={} quest_allowed={} (process totals)", s, q);
}
}  // namespace AutoWowPullCap
