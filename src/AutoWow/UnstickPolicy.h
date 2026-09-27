/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_UNSTICK_POLICY_H
#define AUTOWOW_UNSTICK_POLICY_H

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "QuestSchedulerPolicy.h"
#include "ZoneProgressionPolicy.h"

class Creature;
class Player;

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
//      reset ("[Unstick] combat_stall"); never in a battleground or during an instance encounter.
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
// Bot unit update: the combat watchdog (clears a stalled combat). Throttled per bot.
void CombatWatch(Player* bot, std::uint64_t nowMs);
// Taxi fare of a node path at full price (sum of the TaxiPath costs; 0 when a hop has no path).
std::uint32_t TaxiFare(std::vector<std::uint32_t> const& nodes);
// Why Player::ActivateTaxiPathTo refused this path (the core's own checks, in its order): busy, disable_move,
// mounted, shapeshift, casting, short_path, no_node, no_segment, no_mount, money, unknown.
char const* TaxiFailReason(Player* bot, std::vector<std::uint32_t> const& nodes, Creature* npc);
}  // namespace AutoWowUnstickV2

#endif  // AUTOWOW_UNSTICK_POLICY_H
