#ifndef PLAYERBOTS_QUEST_OBJECTIVE_TRANSITION_POLICY_H
#define PLAYERBOTS_QUEST_OBJECTIVE_TRANSITION_POLICY_H

#include "QuestObjectiveContext.h"

// A partially credited GO has no creature to acquire. Resolve again so the native
// source selector can choose an exact GO; creature kills can keep their current source.
[[nodiscard]] inline QuestActionPhase AfterPartialQuestCredit(QuestObjectiveKind kind,
                                                               int32 selectedSourceEntry)
{
    return kind == QuestObjectiveKind::UseQuestItem || selectedSourceEntry < 0
        ? QuestActionPhase::ResolveObjective
        : QuestActionPhase::AcquireTarget;
}

#endif
