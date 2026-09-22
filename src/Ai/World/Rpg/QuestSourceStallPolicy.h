#ifndef PLAYERBOTS_QUEST_SOURCE_STALL_POLICY_H
#define PLAYERBOTS_QUEST_SOURCE_STALL_POLICY_H

#include <cstdint>

namespace QuestSourceStallPolicy
{
inline constexpr std::uint32_t kUnavailableSourceBudgetMs = 90 * 1000;

// The native objective counter and canonical objective identity are the only reasons to
// discard an earlier unavailable-source observation. Phase and target changes are not progress.
inline void ResetAfterObjectiveChangeOrCredit(std::uint32_t& firstFailureTimeMs,
                                               bool objectiveChanged, bool creditAdvanced)
{
    if (objectiveChanged || creditAdvanced)
        firstFailureTimeMs = 0;
}

// Call only when a source is unavailable after acquisition, resolution, or a respawn wait.
// A long walk or normal combat does not start or evaluate this budget.
[[nodiscard]] inline bool RecordUnavailableSource(std::uint32_t& firstFailureTimeMs,
                                                   std::uint32_t nowMs, std::uint32_t limitMs)
{
    if (firstFailureTimeMs == 0)
    {
        // 0 is the unstarted sentinel; if the 32-bit millisecond clock is exactly at 0,
        // defer its first observation by one tick.
        firstFailureTimeMs = nowMs;
        return false;
    }
    return static_cast<std::uint32_t>(nowMs - firstFailureTimeMs) >= limitMs;
}
}

#endif
