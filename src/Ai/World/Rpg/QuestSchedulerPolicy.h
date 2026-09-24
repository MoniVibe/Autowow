/*
 * Pure quest-log scheduler for the New RPG DO_QUEST status (AutoWow.QuestScheduler.Enable).
 *
 * Stock selection picks one quest-log entry uniformly at random and works it until reward, an
 * unsupported blocker, or the 30-minute DO_QUEST timeout, while the bot keeps accepting more quests
 * at passing givers: one stuck quest starves the rest (probe signature `select:fixation_starved`).
 * This policy replaces the random pick with a deterministic order and a per-quest time slice.
 *
 * No world access: callers supply quantized facts (integer yards, uint32 ms from getMSTime, wrap-safe).
 *
 *   Order (lexicographic, lower wins):
 *     tier   0 turn-in (complete, finisher POI resolved), 1 level-appropriate, 2 grey but cheap
 *     cost   distance to the nearest objective/finisher POI in yards, minus kMomentumBonusYards when
 *            the quest's counters moved within kMomentumWindowMs (saturating at 0), minus the gear
 *            bonus (AutoWow.QuestScheduler.PreferGearRewards: kGearWeaponBonusYards when a reward is a
 *            weapon upgrade for the bot, else kGearArmorBonusYards for any other equippable upgrade)
 *     id     quest id ascending (stable tie-break)
 *   Grey incomplete quests farther than kCheapGreyYards are not candidates; the runtime drops them
 *   (owner: progression over completion).
 *   Slice: the directive rotates when neither its start nor its last counter change is younger than
 *   kSliceMs (kTurnInSliceMs for a turn-in); the rotated quest cools down for kRotateCooldownMs.
 *
 * Avoid list (AutoWow.QuestAvoidIds / AutoWow.QuestAvoidFile / AutoWow.QuestLowPriorityIds):
 * ParseQuestIdList accepts ids separated by commas or whitespace, with `#` comments to end of line.
 */
#ifndef PLAYERBOTS_QUEST_SCHEDULER_POLICY_H
#define PLAYERBOTS_QUEST_SCHEDULER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "AutoWowQuestLedger.h"
#include "QuestStallRecoveryPolicy.h"

namespace QuestSchedulerPolicy
{
inline constexpr std::uint32_t kSliceMs = 5 * 60 * 1000;
inline constexpr std::uint32_t kTurnInSliceMs = 10 * 60 * 1000;  // finisher walks move no counters
inline constexpr std::uint32_t kRotateCooldownMs = 10 * 60 * 1000;
inline constexpr std::uint32_t kMomentumWindowMs = 5 * 60 * 1000;
inline constexpr std::uint32_t kMomentumBonusYards = 150;
inline constexpr std::uint32_t kCheapGreyYards = 100;
inline constexpr std::uint32_t kGearWeaponBonusYards = 300;  // PreferGearRewards: weapon upgrade reward
inline constexpr std::uint32_t kGearArmorBonusYards = 150;   // PreferGearRewards: other equippable upgrade
inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

// Per-quest counter memory (the ledger `progress` idea, in process). A quest's first observation is a
// baseline (lastProgressMs 0 = no momentum); a later counter or completion change stamps nowMs.
struct MomentumEntry
{
    AutoWowQuestLedger::QuestCounters counters;
    bool complete = false;
    std::uint32_t lastProgressMs = 0;
};

struct Observation
{
    AutoWowQuestLedger::QuestCounters counters;
    bool complete = false;
};

struct MomentumBook
{
    std::vector<MomentumEntry> entries;  // quest-log order; quests that left the log are forgotten

    void Observe(std::vector<Observation> const& current, std::uint32_t nowMs)
    {
        std::vector<MomentumEntry> next;
        next.reserve(current.size());
        for (Observation const& obs : current)
        {
            MomentumEntry entry{obs.counters, obs.complete, 0};
            for (MomentumEntry const& before : entries)
                if (before.counters.quest == obs.counters.quest)
                {
                    bool const moved = before.complete != obs.complete ||
                        !AutoWowQuestLedger::SameCounters(before.counters, obs.counters);
                    entry.lastProgressMs = moved ? (nowMs ? nowMs : 1) : before.lastProgressMs;
                    break;
                }
            next.push_back(entry);
        }
        entries.swap(next);
    }

    [[nodiscard]] std::uint32_t LastProgressMs(std::uint32_t questId) const
    {
        for (MomentumEntry const& entry : entries)
            if (entry.counters.quest == questId)
                return entry.lastProgressMs;
        return 0;
    }
};

struct Candidate
{
    std::uint32_t questId = 0;
    bool complete = false;            // QUEST_STATUS_COMPLETE with a resolved finisher POI
    bool grey = false;                // quest level <= the bot's grey level
    std::uint32_t distanceYards = 0;  // nearest POI, 2D
    std::uint32_t lastProgressMs = 0; // MomentumBook; 0 = none observed
    std::uint32_t gearBonusYards = 0; // GearBonusYards (PreferGearRewards only; 0 = no reward upgrade)
};

// AutoWow.QuestScheduler.PreferGearRewards (soak-s14-full-r1: 0 greens across the cohort, starter
// weapons at L9-15): a quest whose reward (choice or fixed) upgrades the bot is worth a detour; a
// weapon most (it is the damage race).
[[nodiscard]] inline std::uint32_t GearBonusYards(bool weaponUpgrade, bool otherUpgrade)
{
    return weaponUpgrade ? kGearWeaponBonusYards : (otherUpgrade ? kGearArmorBonusYards : 0);
}

[[nodiscard]] inline bool HasMomentum(Candidate const& c, std::uint32_t nowMs)
{
    return c.lastProgressMs != 0 && static_cast<std::uint32_t>(nowMs - c.lastProgressMs) < kMomentumWindowMs;
}

// Grey incomplete quests past the cheap radius are outleveled work: not scheduled, droppable.
[[nodiscard]] inline bool IsDroppableGrey(Candidate const& c)
{
    return c.grey && !c.complete && c.distanceYards > kCheapGreyYards;
}

[[nodiscard]] inline std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> SortKey(Candidate const& c,
                                                                                  std::uint32_t nowMs)
{
    std::uint32_t const tier = c.complete ? 0 : (c.grey ? 2 : 1);
    std::uint32_t cost = c.distanceYards;
    if (HasMomentum(c, nowMs))
        cost = cost > kMomentumBonusYards ? cost - kMomentumBonusYards : 0;
    cost = cost > c.gearBonusYards ? cost - c.gearBonusYards : 0;
    return {tier, cost, c.questId};
}

// Index of the best schedulable candidate, or npos.
[[nodiscard]] inline std::size_t PickBest(std::vector<Candidate> const& candidates, std::uint32_t nowMs)
{
    std::size_t best = npos;
    for (std::size_t k = 0; k < candidates.size(); ++k)
    {
        if (IsDroppableGrey(candidates[k]))
            continue;
        if (best == npos || SortKey(candidates[k], nowMs) < SortKey(candidates[best], nowMs))
            best = k;
    }
    return best;
}

struct Slice
{
    std::uint32_t questId = 0;
    std::uint32_t startedMs = 0;
    bool turnIn = false;
};

inline void BeginSlice(Slice& slice, std::uint32_t questId, bool turnIn, std::uint32_t nowMs)
{
    slice = {questId, nowMs, turnIn};
}

// True once the directive has gone a whole slice without a counter change (wrap-safe ages).
[[nodiscard]] inline bool SliceExpired(Slice const& slice, std::uint32_t lastProgressMs, std::uint32_t nowMs)
{
    if (!slice.questId)
        return false;
    std::uint32_t idle = static_cast<std::uint32_t>(nowMs - slice.startedMs);
    if (lastProgressMs)
        idle = std::min(idle, static_cast<std::uint32_t>(nowMs - lastProgressMs));
    return idle >= (slice.turnIn ? kTurnInSliceMs : kSliceMs);
}

// Per-bot scheduler state; the rotation cooldown reuses the stall-deferral book.
struct BotState
{
    MomentumBook momentum;
    Slice slice;
    QuestStallRecoveryPolicy::DeferralBook cooldown;
};

// Parses a quest-id list into a sorted, unique vector. Separators: ',', whitespace; '#' starts a
// comment to end of line. Returns false (out untouched) on any non-numeric token or a zero/overflowing id.
[[nodiscard]] inline bool ParseQuestIdList(std::string_view text, std::vector<std::uint32_t>& out)
{
    std::vector<std::uint32_t> ids;
    std::uint64_t value = 0;
    bool inNumber = false;
    bool inComment = false;
    auto flush = [&]() -> bool
    {
        if (!inNumber)
            return true;
        inNumber = false;
        if (!value || value > 0xFFFFFFFFull)
            return false;
        ids.push_back(static_cast<std::uint32_t>(value));
        value = 0;
        return true;
    };
    for (char const ch : text)
    {
        if (inComment)
        {
            inComment = ch != '\n';
            continue;
        }
        if (ch >= '0' && ch <= '9')
        {
            inNumber = true;
            value = value * 10 + static_cast<std::uint64_t>(ch - '0');
            if (value > 0xFFFFFFFFull)
                return false;
            continue;
        }
        if (!flush())
            return false;
        if (ch == '#')
            inComment = true;
        else if (ch != ',' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            return false;
    }
    if (!flush())
        return false;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    out.swap(ids);
    return true;
}

// Runtime flag (PlayerbotAIConfig.cpp reads AutoWow.QuestScheduler.PreferGearRewards; default 0).
namespace detail
{
inline bool gPreferGearRewards = false;
}
inline bool PreferGearRewards() { return detail::gPreferGearRewards; }

[[nodiscard]] inline bool ContainsQuestId(std::vector<std::uint32_t> const& sortedIds, std::uint32_t questId)
{
    return std::binary_search(sortedIds.begin(), sortedIds.end(), questId);
}
}  // namespace QuestSchedulerPolicy

#endif  // PLAYERBOTS_QUEST_SCHEDULER_POLICY_H
