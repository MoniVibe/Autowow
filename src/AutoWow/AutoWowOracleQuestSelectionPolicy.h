#pragma once

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

} // namespace AutoWowOracleRuntime
