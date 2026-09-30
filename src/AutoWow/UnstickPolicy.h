/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_UNSTICK_POLICY_H
#define AUTOWOW_UNSTICK_POLICY_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "QuestSchedulerPolicy.h"
#include "ZoneProgressionPolicy.h"

class Creature;
class Player;
class PlayerbotAI;

// AutoWow.Unstick.V2 (default 0). soak-s49..s51: ~20 cohort bots, about half of the cohort bot-hours, earned
// ~0 XP, frozen in one of five traps. With the flag:
//   1. Town trap: the AutoWow grind-spot search widens past 250 yd in 250-yd bands up to GrindMaxYards. A bot
//      with no grind spot in that reach and no XP for NoXpMs graduates (zone progression trigger `unstick`) to
//      the nearest route hub whose band fits its level (PickUnstickRoute), even when no route leaves its zone;
//      walk, then the AutoWow.Transports portal fallback. A clogged quest log (more than LogTrimAbove entries)
//      sheds out-of-zone quests without progress for LogTrimStaleMs (PickTrims; stock abandon, ledger
//      `abandoned` reason log_trim).
//   2. Flight: a zone-graduation flight the bot cannot pay for is not chosen (soak-s51: 1-424 copper against
//      a 530-copper Stonetalon Peak -> Nijel's Point fare); a flight leg that returns to idle without taking off
//      switches the trip to walk (portal fallback applies); taxi activation failures log at INFO with a reason.
//   3. Combat stall: in combat with no damage dealt / taken / healing for CombatStallMs -> CombatStop and threat
//      reset; the original path stays excluded from battlegrounds and active instance encounters. The productive
//      watchdog and the pre-combat latch cleanup below are stricter open-world-only paths. A solo cohort bot latched
//      in the AI combat engine before core combat began drops the unchanged target after the same window. All log
//      "[Unstick] combat_stall".
//   4. Pinned party: a group-quest party disbands (reason `stalled`) after AutoWow.Party.StallMs without quest-log
//      progress, or once its leader's zone graduation gave up after the party formed.
//   5. Instance strand: an unpartied cohort bot alive on an instance map (none of our runs, not a dungeon probe)
//      is teleported to its hearthstone bind (ledger zone_move reason instance_strand, mode portal).
// Value-only below the runtime section: integer yards / game-time ms, no RNG, stable orders.
namespace AutoWowUnstickV2
{
inline constexpr std::uint8_t kStateVersion = 1;
inline constexpr std::uint32_t kGrindBaseYards = 250;  // the ordinary AutoWow grind-spot reach
inline constexpr std::uint32_t kGrindBandYards = 250;

struct Params
{
    std::uint32_t grindMaxYards = 1000;        // AutoWow.Unstick.GrindMaxYards
    std::uint64_t noXpMs = 1800000;            // AutoWow.Unstick.NoXpMs (0 = unstick graduation off)
    std::uint32_t logTrimAbove = 15;           // AutoWow.Unstick.LogTrimAbove (0 = trim off)
    std::uint64_t logTrimStaleMs = 3600000;    // AutoWow.Unstick.LogTrimStaleMs
    std::uint64_t combatStallMs = 180000;      // AutoWow.Unstick.CombatStallMs (0 = watchdog off)
    std::uint64_t partyStallMs = 1200000;      // AutoWow.Party.StallMs (0 = off)
    std::uint32_t strandSkipMin = 72385;       // AutoWow.Unstick.StrandSkipGuidMin..Max: dungeon probe guids
    std::uint32_t strandSkipMax = 72394;
};

// Grind search bands past the base reach, nearest first: (250,500], (500,750], ... up to maxYards.
[[nodiscard]] inline std::vector<std::pair<std::uint32_t, std::uint32_t>> GrindBands(std::uint32_t maxYards)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (std::uint32_t lo = kGrindBaseYards; lo < maxYards; lo += kGrindBandYards)
        out.emplace_back(lo, std::min(lo + kGrindBandYards, maxYards));
    return out;
}

// Unstick graduation hub: routes for the bot's team to another zone whose band starts at most levelMargin
// above the bot (never a zone that out-levels it), whatever zone they leave. Smallest level gap to the band
// first (0 = the band holds the level), then hubs on the bot's map by squared distance, then table order.
// soak-s51: a L30 in Desolace (no route leaves 405 for L30; Dustwallow starts at 35) stayed in Shadowprey.
[[nodiscard]] inline AutoWowZoneProgression::Route const* PickUnstickRoute(
    std::vector<AutoWowZoneProgression::Route> const& routes, std::uint32_t team, std::uint32_t level,
    std::uint32_t levelMargin, std::uint32_t zone, std::uint32_t map, std::int32_t x, std::int32_t y)
{
    AutoWowZoneProgression::Route const* best = nullptr;
    std::uint32_t bestGap = 0;
    bool bestSameMap = false;
    std::int64_t bestDist2 = 0;
    for (AutoWowZoneProgression::Route const& r : routes)
    {
        if (r.to == zone || (r.team != 0 && r.team != team) || r.minLevel > level + levelMargin)
            continue;
        std::uint32_t const gap = level > r.maxLevel ? level - r.maxLevel : level < r.minLevel ? r.minLevel - level : 0;
        bool const sameMap = r.map == map;
        std::int64_t const dx = std::int64_t(r.x) - x;
        std::int64_t const dy = std::int64_t(r.y) - y;
        std::int64_t const dist2 = sameMap ? dx * dx + dy * dy : 0;
        bool take = !best;
        if (!take && gap != bestGap)
            take = gap < bestGap;
        else if (!take && sameMap != bestSameMap)
            take = sameMap;
        else if (!take)
            take = dist2 < bestDist2;
        if (take)
        {
            best = &r;
            bestGap = gap;
            bestSameMap = sameMap;
            bestDist2 = dist2;
        }
    }
    return best;
}

// XP watch: true once level and XP have not moved for noXpMs. A change (or the first sample) restarts it.
struct XpWatch
{
    std::uint32_t level = 0;
    std::uint32_t xp = 0;
    std::uint64_t sinceMs = 0;  // 0 = no sample yet
};

[[nodiscard]] inline bool NoteXp(XpWatch& w, std::uint32_t level, std::uint32_t xp, std::uint64_t nowMs,
                                 std::uint64_t noXpMs)
{
    if (!w.sinceMs || w.level != level || w.xp != xp)
    {
        w = XpWatch{level, xp, nowMs ? nowMs : 1};
        return false;
    }
    return noXpMs && nowMs >= w.sinceMs && nowMs - w.sinceMs >= noXpMs;
}

// Quest-log ages: when each quest in the log last moved (accepted, a counter, completion). The first sight of a
// quest stamps nowMs (after a restart a quest is fresh again: trimming waits LogTrimStaleMs). Log order.
struct QuestAge
{
    AutoWowQuestLedger::QuestCounters counters;
    bool complete = false;
    std::uint64_t sinceMs = 0;
};

struct AgeBook
{
    std::vector<QuestAge> entries;  // quests that left the log are forgotten

    void Observe(std::vector<QuestSchedulerPolicy::Observation> const& log, std::uint64_t nowMs)
    {
        std::vector<QuestAge> next;
        next.reserve(log.size());
        for (QuestSchedulerPolicy::Observation const& obs : log)
        {
            QuestAge age{obs.counters, obs.complete, nowMs};
            for (QuestAge const& before : entries)
                if (before.counters.quest == obs.counters.quest)
                {
                    if (before.complete == obs.complete && AutoWowQuestLedger::SameCounters(before.counters, obs.counters))
                        age.sinceMs = before.sinceMs;
                    break;
                }
            next.push_back(age);
        }
        entries.swap(next);
    }
};

struct TrimFact
{
    std::uint32_t quest = 0;
    std::int32_t zoneOrSort = 0;  // quest template ZoneOrSort (> 0 zone id; <= 0 class / profession / none)
    bool complete = false;
    std::uint64_t sinceMs = 0;    // AgeBook
};

// Quests to abandon from a clogged log: only with more than `above` entries, only incomplete quests of another
// zone (ZoneOrSort > 0, not the bot's zone) without progress for staleMs; oldest first (quest id on ties), and
// no more than bring the log back to `above`. above 0 = off.
[[nodiscard]] inline std::vector<std::uint32_t> PickTrims(std::vector<TrimFact> const& log, std::uint32_t zone,
                                                          std::uint64_t nowMs, std::uint32_t above,
                                                          std::uint64_t staleMs)
{
    std::vector<std::uint32_t> out;
    if (!above || log.size() <= above)
        return out;
    std::vector<TrimFact> stale;
    for (TrimFact const& f : log)
        if (!f.complete && f.zoneOrSort > 0 && std::uint32_t(f.zoneOrSort) != zone && nowMs >= f.sinceMs &&
            nowMs - f.sinceMs >= staleMs)
            stale.push_back(f);
    std::sort(stale.begin(), stale.end(), [](TrimFact const& a, TrimFact const& b)
              { return a.sinceMs != b.sinceMs ? a.sinceMs < b.sinceMs : a.quest < b.quest; });
    std::size_t const room = log.size() - above;
    for (std::size_t k = 0; k < stale.size() && k < room; ++k)
        out.push_back(stale[k].quest);
    return out;
}

// Combat watchdog: in combat since combatSinceMs, last damage dealt / taken / healing at activityMs (0 = none).
// Stalled when neither moved for stallMs (activity before the combat began does not count). stallMs 0 = off.
[[nodiscard]] inline bool CombatStalled(std::uint64_t combatSinceMs, std::uint64_t activityMs, std::uint64_t nowMs,
                                        std::uint64_t stallMs)
{
    if (!stallMs || !combatSinceMs)
        return false;
    std::uint64_t const from = std::max(combatSinceMs, activityMs);
    return nowMs >= from && nowMs - from >= stallMs;
}

// Separate productive-combat watchdog. Outgoing damage and healing are intentionally absent from the
// sample: they can repeat forever while a geometry-separated creature heals back. Progress is a different
// creature target, a new target-health low-water mark, incoming damage, or movement by at least 8 yards.
// The first sample starts the clock. A missing target clears it. Uses the ordinary CombatStallMs window.
inline constexpr std::int32_t kProductiveMovementYards = 8;
inline constexpr std::uint64_t kProactiveRetryBackoffMs = 30000;

struct ProductiveCombatWatch
{
    std::uint64_t target = 0;
    std::uint32_t lowTargetHp = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint64_t incomingMs = 0;
    std::uint64_t sinceMs = 0;
    std::uint32_t targetMaxHp = 0;
};

struct ProductiveCombatSample
{
    std::uint64_t target = 0;
    std::uint32_t targetHp = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint64_t incomingMs = 0;
    std::uint32_t targetMaxHp = 0;
};

[[nodiscard]] inline bool ProductiveCombatStalled(ProductiveCombatWatch& w, ProductiveCombatSample const& sample,
                                                  std::uint64_t nowMs, std::uint64_t stallMs)
{
    if (!stallMs || !sample.target)
    {
        w = ProductiveCombatWatch{};
        return false;
    }

    std::int64_t const dx = std::int64_t(sample.x) - w.x;
    std::int64_t const dy = std::int64_t(sample.y) - w.y;
    std::int64_t const dz = std::int64_t(sample.z) - w.z;
    std::int64_t constexpr move2 = std::int64_t(kProductiveMovementYards) * kProductiveMovementYards;
    bool const first = !w.sinceMs;
    bool const targetChanged = !first && sample.target != w.target;
    bool const maxHpChanged = !first && !targetChanged && sample.targetMaxHp != w.targetMaxHp;
    bool const newLow = !first && !targetChanged && !maxHpChanged && sample.targetHp < w.lowTargetHp;
    bool const tookDamage = !first && sample.incomingMs > w.incomingMs;
    bool const moved = !first && dx * dx + dy * dy + dz * dz >= move2;
    if (first || targetChanged || maxHpChanged || newLow || tookDamage || moved)
    {
        w.target = sample.target;
        w.lowTargetHp =
            targetChanged || maxHpChanged || first ? sample.targetHp : std::min(w.lowTargetHp, sample.targetHp);
        w.x = sample.x;
        w.y = sample.y;
        w.z = sample.z;
        w.incomingMs = sample.incomingMs;
        w.targetMaxHp = sample.targetMaxHp;
        w.sinceMs = nowMs ? nowMs : 1;
        return false;
    }

    return nowMs >= w.sinceMs && nowMs - w.sinceMs >= stallMs;
}

// Runtime eligibility facts are kept pure so every exclusion is regression-tested. Cohort means an
// ordinary configured AutoWoW character rather than a native random bot. Independent arming is deliberately
// not a fact: a newly activated, still-unarmed cohort character must be recoverable.
struct ProductiveCombatScope
{
    bool alive = true;
    bool solo = true;
    bool openWorld = true;
    bool cohort = true;
    bool masterless = true;
    bool oracleManaged = false;
    bool paused = false;
    bool userControlled = false;
};

[[nodiscard]] inline bool ProductiveCombatEligible(ProductiveCombatScope const& s)
{
    return s.alive && s.solo && s.openWorld && s.cohort && s.masterless && !s.oracleManaged && !s.paused &&
           !s.userControlled;
}

// AttackAction installs the AI target / Unit victim and switches engines before core combat necessarily begins.
// If the pull never enters core combat, the native "invalid target" trigger cannot drop an otherwise valid live
// creature. Keep this narrower than the productive-combat watchdog: the exact same target must occupy both slots.
// Movement, target changes and target-health progress are timed by ProductiveCombatStalled. An active cast only
// defers the final clear; it does not restart the timer, so periodic self-maintenance cannot mask a stale target.
struct StaleTargetScope
{
    ProductiveCombatScope owner;
    bool battleground = false;
    bool coreCombat = false;
    bool aiCombat = true;
    bool sameLiveCreatureTarget = true;
    bool casting = false;
};

[[nodiscard]] inline bool StaleTargetEligible(StaleTargetScope const& s)
{
    return ProductiveCombatEligible(s.owner) && !s.battleground && !s.coreCombat && s.aiCombat &&
           s.sameLiveCreatureTarget && !s.casting;
}

[[nodiscard]] inline bool StaleTargetObserved(StaleTargetScope const& s)
{
    StaleTargetScope observation = s;
    observation.casting = false;
    return StaleTargetEligible(observation);
}

// A pre-combat timeout owns one exact runtime target GUID for a short retry backoff. This is not a persistent
// ignore list: ordinary proactive grinding alone consults it, self-defence and objective-locked selectors bypass
// it, and the exact expiry restores eligibility. The full raw GUID distinguishes loaded objects that share a low
// counter; logout lifecycle bounds the ephemeral identity across sessions.
struct ProactiveRetryBackoff
{
    std::uint64_t target = 0;
    std::uint64_t untilMs = 0;
};

[[nodiscard]] inline ProactiveRetryBackoff StartProactiveRetryBackoff(std::uint64_t target,
                                                                      std::uint64_t nowMs)
{
    if (!target)
        return {};
    std::uint64_t const room = std::numeric_limits<std::uint64_t>::max() - nowMs;
    return {target, nowMs + std::min(room, kProactiveRetryBackoffMs)};
}

[[nodiscard]] inline bool ProactiveRetryBlocked(ProactiveRetryBackoff const& backoff, std::uint64_t target,
                                                std::uint64_t nowMs)
{
    return target && target == backoff.target && nowMs < backoff.untilMs;
}

[[nodiscard]] inline bool ShouldClearMatchingPull(std::uint64_t pullTarget, std::uint64_t failedTarget)
{
    return pullTarget && pullTarget == failedTarget;
}

// Party quest progress: sig folds the members' quest logs (FNV-1a over ids / counters / status). A new sig (or
// the first look, sinceMs 0) restarts the window; true once it has held for stallMs. stallMs 0 = off.
inline constexpr std::uint64_t kFoldSeed = 1469598103934665603ull;

[[nodiscard]] inline std::uint64_t Fold(std::uint64_t h, std::uint64_t v)
{
    for (int k = 0; k < 8; ++k)
    {
        h ^= (v >> (k * 8)) & 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

[[nodiscard]] inline bool PartyStalled(std::uint64_t& sig, std::uint64_t& sinceMs, std::uint64_t newSig,
                                       std::uint64_t nowMs, std::uint64_t stallMs)
{
    if (!sinceMs || sig != newSig)
    {
        sig = newSig;
        sinceMs = nowMs ? nowMs : 1;
        return false;
    }
    return stallMs && nowMs >= sinceMs && nowMs - sinceMs >= stallMs;
}

// ---- runtime (AutoWow/UnstickRuntime.cpp) -----------------------------------------------------------------
namespace detail
{
inline bool gEnabled = false;  // AutoWow.Unstick.V2
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }

// World init. Reads AutoWow.Unstick.* and AutoWow.Party.StallMs.
void LoadConfig();
// Bot tick (NewRpg / zone progression): the bot's level and XP have not moved for NoXpMs.
bool XpStalled(Player* bot, std::uint64_t nowMs);
// The bot's zone graduation gave up at nowMs (zone progression); a party formed at or before that disbands.
void NoteGaveUp(std::uint32_t guid, std::uint64_t nowMs);
bool GaveUpSince(std::uint32_t guid, std::uint64_t sinceMs);
// Updates the bot's AgeBook with its current quest log and returns the ages (log order).
std::vector<QuestAge> ObserveAges(std::uint32_t guid, std::vector<QuestSchedulerPolicy::Observation> const& log,
                                  std::uint64_t nowMs);
// Damage dealt / taken or healing done by the bot (its pets count as the bot). Unit hook, map threads.
void NoteCombatActivity(std::uint32_t guid, std::uint64_t nowMs);
// Incoming damage is ordinary activity and productive progress. Unit hook, map threads.
void NoteCombatIncomingDamage(std::uint32_t guid, std::uint64_t nowMs);
// Bot unit update: the combat watchdog (clears a stalled combat). Throttled per bot.
void CombatWatch(Player* bot, std::uint64_t nowMs);
// Ordinary proactive-grind admission for a runtime target. Rechecks current strict S80 ownership before locking.
bool ProactiveRetryBlocked(Player* bot, PlayerbotAI* botAI, std::uint64_t target);
// Bot logout lifecycle: forget only this bot's unstick trackers, including a live retry backoff.
void Forget(std::uint32_t guid);
// Taxi fare of a node path at full price (sum of the TaxiPath costs; 0 when a hop has no path).
std::uint32_t TaxiFare(std::vector<std::uint32_t> const& nodes);
// Why Player::ActivateTaxiPathTo refused this path (the core's own checks, in its order): busy, disable_move,
// mounted, shapeshift, casting, short_path, no_node, no_segment, no_mount, money, unknown.
char const* TaxiFailReason(Player* bot, std::vector<std::uint32_t> const& nodes, Creature* npc);
}  // namespace AutoWowUnstickV2

#endif  // AUTOWOW_UNSTICK_POLICY_H
