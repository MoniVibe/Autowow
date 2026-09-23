#pragma once

#include <cstdint>
#include <limits>

namespace AutoWowOracleRuntime
{
// The objective resolver is re-read from the core-backed Value before these facts are supplied.
// An incomplete quest with no runnable objective and no core-ready hand-in must yield the
// independent bot's directive so another quest or ordinary work can run. Quest state is untouched.
struct QuestSelectionFacts
{
    bool incomplete = false;
    bool coreReadyForFinisher = false;
    bool resolverAvailable = false;
    bool runnableObjective = false;
};

[[nodiscard]] inline bool ShouldDeferUnrunnableQuest(QuestSelectionFacts const& facts)
{
    return facts.incomplete && !facts.coreReadyForFinisher &&
        facts.resolverAvailable && !facts.runnableObjective;
}

// A selected directive can be runnable in principle yet yield nothing the arbiter can lease: the
// quest frame cannot be built (finisher ready but no same-map finisher spawn resolves) or it is
// built with zero candidates (objective sources only on another map). Nothing then dispatches,
// blocks, or defers, and the bot idles on that directive forever. This streak counts consecutive
// lease-less cadence passes for one quest; AutoWow.OracleRuntime.DeferNoCandidatePasses (0 = off)
// turns a long enough streak into the same deferral as an unrunnable quest.
enum class NoCandidateKind : std::uint8_t
{
    None = 0,
    NoFrame = 1,      // quest frame not buildable (typically a cross-map finisher)
    NoCandidate = 2   // frame built, zero candidates (typically cross-map objective sources)
};

struct NoCandidateStreak
{
    std::uint32_t questId = 0;
    std::uint32_t passes = 0;
    NoCandidateKind kind = NoCandidateKind::None;
};

inline void ObserveNoCandidate(NoCandidateStreak& streak, std::uint32_t questId, NoCandidateKind kind) noexcept
{
    if (questId == 0)
    {
        streak = {};
        return;
    }
    if (streak.questId != questId)
        streak = {questId, 0, kind};
    streak.kind = kind;
    if (streak.passes < std::numeric_limits<std::uint32_t>::max())
        ++streak.passes;
}

[[nodiscard]] inline bool ShouldDeferNoCandidateQuest(NoCandidateStreak const& streak,
                                                      std::uint32_t questId,
                                                      std::uint32_t thresholdPasses) noexcept
{
    return thresholdPasses != 0 && questId != 0 && streak.questId == questId &&
        streak.passes >= thresholdPasses;
}

[[nodiscard]] inline char const* NoCandidateReasonName(NoCandidateKind kind) noexcept
{
    return kind == NoCandidateKind::NoFrame ? "oracle_no_frame" : "oracle_no_candidate";
}

} // namespace AutoWowOracleRuntime
