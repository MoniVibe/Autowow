/*
 * Committed travel intent for New RPG MoveFarTo (AutoWow.TravelIntent.Enable, default off).
 *
 * Root cause this replaces (research/quests/STARTER_STALLS.md): every accepted prepared-walk segment
 * re-armed MoveFarTo's stuck counter, and each replan from a segment end picked the segment back, so a
 * bot bounced A -> B -> A forever while "making progress". Here the bot commits to one segment toward
 * one goal and replans only on events:
 *   - the committed segment was reached, interrupted, or is unsafe (combat / danger area);
 *   - consecutive failures reach the replan budget        -> give up `intent_replan_exhausted`;
 *   - no net progress TOWARD THE GOAL over the window     -> give up `intent_no_progress`;
 *   - the goal moved (> kGoalMoveYards) or changed        -> a fresh intent.
 * A candidate that returns to a recently committed segment start/end is an oscillation and is refused;
 * a candidate replacing a still-live segment must beat it by the hysteresis margin.
 *
 * Pure: callers supply quantized facts. Integer yards / wrap-safe milliseconds, no RNG, no world access.
 */
#ifndef PLAYERBOTS_TRAVEL_INTENT_POLICY_H
#define PLAYERBOTS_TRAVEL_INTENT_POLICY_H

#include <array>
#include <cmath>
#include <cstdint>

namespace TravelIntentPolicy
{
inline constexpr std::uint8_t kIntentVersion = 1;
inline constexpr std::uint32_t kGoalMoveYards = 20;         // goal drift that counts as a new goal
inline constexpr std::uint32_t kReachYards = 6;             // committed segment endpoint reached
inline constexpr std::uint32_t kRevisitYards = 15;          // candidate this close to a recent point = bounce
inline constexpr std::uint32_t kMinStepYards = 5;           // a segment must actually move the bot
inline constexpr std::uint32_t kMinImprovementYards = 10;   // goal progress that re-arms the window
inline constexpr std::uint32_t kMaxObservationGapMs = 10 * 1000;  // longer gaps (combat, other status) not charged
inline constexpr std::uint32_t kReplanCooldownMs = 2000;    // after a failed replan; same vantage = same answer
inline constexpr std::size_t kRecentPoints = 8;             // start+end of the last 4 committed segments
inline constexpr std::uint32_t kStaleIntentMs = 60 * 1000;  // unobserved this long: the old intent is history

struct Params
{
    std::uint32_t replanFailCount = 5;
    std::uint32_t progressWindowMs = 90 * 1000;
    std::uint32_t hysteresisPct = 20;
};

struct Point
{
    std::uint32_t mapId = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;  // carried for re-issuing a committed endpoint; distances are 2D
};

[[nodiscard]] inline Point MakePoint(std::uint32_t mapId, float x, float y, float z = 0.0f)
{
    return {mapId, static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y)),
            static_cast<std::int32_t>(std::floor(z))};
}

// Integer 2D yards; a different map is "infinitely" far.
[[nodiscard]] inline std::uint32_t DistanceYards(Point const& a, Point const& b)
{
    if (a.mapId != b.mapId)
        return 0xFFFFFFFFu;
    std::int64_t const dx = static_cast<std::int64_t>(a.x) - b.x;
    std::int64_t const dy = static_cast<std::int64_t>(a.y) - b.y;
    return static_cast<std::uint32_t>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
}

enum class GiveUp : std::uint8_t
{
    None,
    ReplanExhausted,  // ledger `blocked` reason intent_replan_exhausted
    NoProgress        // ledger `blocked` reason intent_no_progress
};

struct Intent
{
    std::uint8_t version = kIntentVersion;
    bool active = false;
    Point goal;
    std::uint32_t createdMs = 0;
    // progress toward the GOAL (never re-armed by a segment)
    std::uint32_t bestGoalYards = 0;
    std::uint32_t chargedMs = 0;
    std::uint32_t lastObservedMs = 0;
    // committed segment
    bool hasSegment = false;
    Point segment;
    std::uint32_t segmentGoalYards = 0;  // goal distance from the segment endpoint (the plan's value)
    std::uint32_t failures = 0;          // consecutive interruptions / refused or failed replans
    std::uint32_t lastFailureMs = 0;
    std::uint32_t segmentsCommitted = 0;
    std::array<Point, kRecentPoints> recent{};
    std::uint8_t recentCount = 0;
    std::uint8_t recentNext = 0;
    GiveUp giveUp = GiveUp::None;
};

enum class Verdict : std::uint8_t
{
    Progress,    // armed, re-armed or goal distance improved
    Travelling,  // window not spent
    NoProgress   // no goal improvement for the whole window
};

// Arms the intent for `goal` (a new or drifted goal replaces the old intent) and charges the
// progress-toward-goal window. Segment commits never touch this window.
[[nodiscard]] inline Verdict Observe(Intent& intent, Point const& goal, std::uint32_t goalYards,
                                     std::uint32_t nowMs, Params const& params)
{
    if (!intent.active || DistanceYards(intent.goal, goal) > kGoalMoveYards ||
        static_cast<std::uint32_t>(nowMs - intent.lastObservedMs) > kStaleIntentMs)
    {
        intent = {};
        intent.active = true;
        intent.goal = goal;
        intent.createdMs = nowMs;
        intent.bestGoalYards = goalYards;
        intent.lastObservedMs = nowMs;
        return Verdict::Progress;
    }

    std::uint32_t const gap = nowMs - intent.lastObservedMs;  // wrap-safe
    intent.lastObservedMs = nowMs;
    if (gap <= kMaxObservationGapMs)
        intent.chargedMs += gap;
    if (goalYards + kMinImprovementYards <= intent.bestGoalYards)
    {
        intent.bestGoalYards = goalYards;
        intent.chargedMs = 0;
        return Verdict::Progress;
    }
    return intent.chargedMs >= params.progressWindowMs ? Verdict::NoProgress : Verdict::Travelling;
}

enum class SegmentState : std::uint8_t
{
    None,         // nothing committed: plan
    Follow,       // moving toward the committed endpoint: hold the commitment
    Reached,      // endpoint reached: plan the next segment
    Interrupted,  // stopped short: re-issue the committed segment unless a candidate beats it
    Unsafe        // combat / danger area: drop the segment and plan
};

[[nodiscard]] inline SegmentState ClassifySegment(Intent const& intent, Point const& here, bool moving, bool danger)
{
    if (!intent.hasSegment)
        return SegmentState::None;
    if (danger)
        return SegmentState::Unsafe;
    if (DistanceYards(here, intent.segment) <= kReachYards)
        return SegmentState::Reached;
    return moving ? SegmentState::Follow : SegmentState::Interrupted;
}

[[nodiscard]] inline bool IsRecent(Intent const& intent, Point const& p)
{
    for (std::uint8_t i = 0; i < intent.recentCount; ++i)
        if (DistanceYards(intent.recent[i], p) <= kRevisitYards)
            return true;
    return false;
}

// Admission of a candidate segment endpoint. Refuses a bounce back to a recent segment point, a
// non-step, a dangerous endpoint, and - while a committed segment is still live - anything that does
// not beat the committed plan's goal distance by hysteresisPct.
[[nodiscard]] inline bool Admit(Intent const& intent, Point const& here, Point const& candidate,
                                std::uint32_t candidateGoalYards, bool candidateDanger, Params const& params)
{
    if (candidateDanger || DistanceYards(here, candidate) < kMinStepYards || IsRecent(intent, candidate))
        return false;
    if (!intent.hasSegment)
        return true;
    std::uint32_t const pct = params.hysteresisPct > 100 ? 100 : params.hysteresisPct;
    return static_cast<std::uint64_t>(candidateGoalYards) * 100 <=
        static_cast<std::uint64_t>(intent.segmentGoalYards) * (100 - pct);
}

inline void Remember(Intent& intent, Point const& p)
{
    intent.recent[intent.recentNext] = p;
    intent.recentNext = static_cast<std::uint8_t>((intent.recentNext + 1) % kRecentPoints);
    if (intent.recentCount < kRecentPoints)
        ++intent.recentCount;
}

inline void Commit(Intent& intent, Point const& here, Point const& endpoint, std::uint32_t endpointGoalYards)
{
    Remember(intent, here);
    Remember(intent, endpoint);
    intent.hasSegment = true;
    intent.segment = endpoint;
    intent.segmentGoalYards = endpointGoalYards;
    intent.failures = 0;
    ++intent.segmentsCommitted;
}

inline void DropSegment(Intent& intent) { intent.hasSegment = false; }

// A reached endpoint closes the segment and clears the failure streak (the plan worked).
inline void CompleteSegment(Intent& intent)
{
    intent.hasSegment = false;
    intent.failures = 0;
}

// Counts one failure; true when the replan budget is spent.
[[nodiscard]] inline bool NoteFailure(Intent& intent, std::uint32_t nowMs, Params const& params)
{
    ++intent.failures;
    intent.lastFailureMs = nowMs;
    return intent.failures >= (params.replanFailCount == 0 ? 1 : params.replanFailCount);
}

[[nodiscard]] inline bool ReplanCoolingDown(Intent const& intent, std::uint32_t nowMs)
{
    return intent.failures != 0 && static_cast<std::uint32_t>(nowMs - intent.lastFailureMs) < kReplanCooldownMs;
}

// Terminal: records the reason for the caller's typed block and disarms the intent.
inline void Abandon(Intent& intent, GiveUp reason)
{
    intent = {};
    intent.giveUp = reason;
}

// Read-and-clear for the caller that raises the quest block.
[[nodiscard]] inline GiveUp TakeGiveUp(Intent& intent)
{
    GiveUp const reason = intent.giveUp;
    intent.giveUp = GiveUp::None;
    return reason;
}
}  // namespace TravelIntentPolicy

#endif
