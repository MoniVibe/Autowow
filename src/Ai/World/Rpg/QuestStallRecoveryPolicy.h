/*
 * Pure stall-recovery policies for the New RPG quest executor (starter-zone stall lane).
 *
 * No world access: callers supply quantized facts. Integer yards / milliseconds only, so the
 * decisions are reproducible from the same observation sequence.
 *
 *   TravelWatch      - AutoWow.QuestTravelProgressWatch.Enable. A walk toward one destination must
 *                      reduce its best-so-far distance by kTravelMinImprovementYards within
 *                      kTravelNoProgressBudgetMs of observed (out-of-combat, action-driven) time.
 *                      MoveFarTo's own stuck detector is re-armed by every prepared walk segment, so
 *                      two alternating segments (A -> B -> A) never trip it; this watch does.
 *   BlockedDeferral  - AutoWow.QuestBlockedDefer.Enable. A non-Oracle quest held in Blocked for
 *                      kBlockedDwellMs is deferred for kDeferMs and the bot returns to Idle instead of
 *                      idling in Blocked until the 30-minute DO_QUEST status timeout.
 */
#ifndef PLAYERBOTS_QUEST_STALL_RECOVERY_POLICY_H
#define PLAYERBOTS_QUEST_STALL_RECOVERY_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace QuestStallRecoveryPolicy
{
inline constexpr std::uint32_t kTravelNoProgressBudgetMs = 150 * 1000;
inline constexpr std::uint32_t kTravelMinImprovementYards = 10;
// An observation gap longer than this (combat, death, another RPG status owning the bot) is not
// charged to the travel budget: the watch only measures time the travel phase actually drove.
inline constexpr std::uint32_t kTravelMaxObservationGapMs = 10 * 1000;

inline constexpr std::uint32_t kBlockedDwellMs = 30 * 1000;
inline constexpr std::uint32_t kDeferMs = 10 * 60 * 1000;
inline constexpr std::size_t kMaxDeferredQuests = 25;  // MAX_QUEST_LOG_SIZE
// Travel expiries allowed to rotate to another resolved spawn of the same objective before the
// objective blocks: spawns of one objective are usually clustered behind the same obstacle.
inline constexpr std::uint32_t kMaxTravelRotations = 2;

// Destination identity in integer yards; a new key re-arms the watch.
struct TravelKey
{
    std::uint32_t mapId = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;

    [[nodiscard]] bool operator==(TravelKey const& other) const
    {
        return mapId == other.mapId && x == other.x && y == other.y;
    }
};

[[nodiscard]] inline TravelKey MakeTravelKey(std::uint32_t mapId, float x, float y)
{
    return {mapId, static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y))};
}

[[nodiscard]] inline std::uint32_t QuantizeYards(float distance)
{
    if (!std::isfinite(distance) || distance <= 0.0f)
        return 0;
    if (distance >= 4.0e9f)
        return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(distance);
}

struct TravelWatch
{
    bool armed = false;
    TravelKey key;
    std::uint32_t bestYards = 0;
    std::uint32_t chargedMs = 0;       // observed time since the last best-distance improvement
    std::uint32_t lastObservedMs = 0;
};

enum class TravelVerdict : std::uint8_t
{
    Progress,    // armed, re-armed, or best distance improved
    Travelling,  // no improvement yet, budget not spent
    Expired      // no improvement for the whole budget: the destination is not being approached
};

[[nodiscard]] inline TravelVerdict ObserveTravel(TravelWatch& watch, TravelKey const& key,
                                                 std::uint32_t distanceYards, std::uint32_t nowMs,
                                                 std::uint32_t budgetMs = kTravelNoProgressBudgetMs)
{
    if (!watch.armed || !(watch.key == key))
    {
        watch = {true, key, distanceYards, 0, nowMs};
        return TravelVerdict::Progress;
    }

    std::uint32_t const gap = nowMs - watch.lastObservedMs;  // wrap-safe unsigned difference
    watch.lastObservedMs = nowMs;
    if (gap <= kTravelMaxObservationGapMs)
        watch.chargedMs += gap;

    if (distanceYards + kTravelMinImprovementYards <= watch.bestYards)
    {
        watch.bestYards = distanceYards;
        watch.chargedMs = 0;
        return TravelVerdict::Progress;
    }
    return watch.chargedMs >= budgetMs ? TravelVerdict::Expired : TravelVerdict::Travelling;
}

inline void ResetTravel(TravelWatch& watch) { watch = {}; }

// Oracle-managed directives keep the historical Blocked hold for their external arbiter.
[[nodiscard]] inline bool ShouldDeferBlocked(bool oracleManaged, std::uint32_t blockedAtMs, std::uint32_t nowMs,
                                             std::uint32_t dwellMs = kBlockedDwellMs)
{
    return !oracleManaged && blockedAtMs != 0 && static_cast<std::uint32_t>(nowMs - blockedAtMs) >= dwellMs;
}

// Per-bot timed deferral book. Bounded to the quest-log size; expired entries are dropped on use.
struct DeferredQuest
{
    std::uint32_t questId = 0;
    std::uint32_t untilMs = 0;
};

struct DeferralBook
{
    std::vector<DeferredQuest> entries;

    void Purge(std::uint32_t nowMs)
    {
        // Signed wrap-safe comparison: an entry is live while untilMs is still ahead of nowMs.
        entries.erase(std::remove_if(entries.begin(), entries.end(), [nowMs](DeferredQuest const& entry)
        {
            return static_cast<std::int32_t>(entry.untilMs - nowMs) <= 0;
        }), entries.end());
    }

    void Defer(std::uint32_t questId, std::uint32_t nowMs, std::uint32_t deferMs = kDeferMs)
    {
        Purge(nowMs);
        entries.erase(std::remove_if(entries.begin(), entries.end(), [questId](DeferredQuest const& entry)
        {
            return entry.questId == questId;
        }), entries.end());
        if (entries.size() >= kMaxDeferredQuests)
            entries.erase(entries.begin());  // oldest deferral first
        entries.push_back({questId, nowMs + deferMs});
    }

    [[nodiscard]] bool IsDeferred(std::uint32_t questId, std::uint32_t nowMs)
    {
        Purge(nowMs);
        return std::any_of(entries.begin(), entries.end(), [questId](DeferredQuest const& entry)
        {
            return entry.questId == questId;
        });
    }
};

// AutoWow.QuestItemTargetConditions.Enable: a CAST objective's credit creature can be a script
// result that is never spawned (q9303 credits 16534 "Inoculated Owlkin"; the item spell targets
// 16518). The item spell's explicit-target conditions (conditions source type 17) name the real
// target. Only positive CONDITION_OBJECT_ENTRY_GUID(31) rows on the explicit target
// (ConditionTarget 1) with TypeID unit(3) and a non-zero entry qualify; the result is sorted and
// unique so resolution order is stable.
struct SpellTargetConditionFact
{
    std::uint32_t conditionType = 0;
    std::uint32_t conditionTarget = 0;  // 0 caster, 1 explicit target
    std::uint32_t value1 = 0;
    std::uint32_t value2 = 0;
    bool negative = false;
};

inline constexpr std::uint32_t kConditionObjectEntryGuid = 31;
inline constexpr std::uint32_t kTypeIdUnit = 3;

[[nodiscard]] inline std::vector<std::uint32_t> SpellTargetCreatureEntries(
    std::vector<SpellTargetConditionFact> const& facts)
{
    std::vector<std::uint32_t> entries;
    for (SpellTargetConditionFact const& fact : facts)
        if (fact.conditionType == kConditionObjectEntryGuid && fact.conditionTarget == 1 &&
            fact.value1 == kTypeIdUnit && fact.value2 != 0 && !fact.negative)
            entries.push_back(fact.value2);
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    return entries;
}
}  // namespace QuestStallRecoveryPolicy

#endif  // PLAYERBOTS_QUEST_STALL_RECOVERY_POLICY_H
