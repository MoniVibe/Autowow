/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONENCOUNTERSELECTIONPOLICY_H
#define PLAYERBOTS_DUNGEONENCOUNTERSELECTIONPOLICY_H

#include "Define.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace DungeonEncounterSelection
{
struct EncounterKey
{
    uint32 mapId = 0;
    uint32 encounterId = 0;
};

enum class Completion
{
    Unknown,
    Incomplete,
    Complete
};

enum class GateState
{
    Unknown,
    Blocked,
    Satisfied
};

enum class Reachability
{
    Unknown,
    Unreachable,
    Reachable
};

struct CandidateFacts
{
    EncounterKey key;
    uint32 goalId = 0;
    uint32 routeOrder = 0;
    int32 score = 0;
    Completion completion = Completion::Unknown;
    GateState prerequisites = GateState::Unknown;
    Reachability reachability = Reachability::Unknown;
};

enum class BlockedReason
{
    None,
    InvalidKey,
    DuplicateKey,
    CompletionUnknown,
    AlreadyComplete,
    PrerequisitesUnknown,
    PrerequisitesBlocked,
    ReachabilityUnknown,
    Unreachable
};

enum class Status
{
    Selected,
    NoCandidates,
    AllComplete,
    Blocked,
    InvalidInput
};

struct CandidateDecision
{
    EncounterKey key;
    uint32 goalId = 0;
    BlockedReason reason = BlockedReason::None;
};

struct Selection
{
    Status status = Status::NoCandidates;
    EncounterKey selected;
    uint32 selectedGoalId = 0;
    std::vector<CandidateDecision> decisions;
};

namespace Detail
{
constexpr bool SameKey(EncounterKey const& left, EncounterKey const& right)
{
    return left.mapId == right.mapId && left.encounterId == right.encounterId;
}

inline bool CanonicalFactsOrder(CandidateFacts const& left, CandidateFacts const& right)
{
    return std::tie(left.key.mapId, left.key.encounterId, left.goalId, left.routeOrder, left.score,
               left.completion, left.prerequisites, left.reachability) <
        std::tie(right.key.mapId, right.key.encounterId, right.goalId, right.routeOrder, right.score,
            right.completion, right.prerequisites, right.reachability);
}

constexpr BlockedReason Classify(CandidateFacts const& candidate)
{
    if (candidate.completion == Completion::Unknown)
        return BlockedReason::CompletionUnknown;
    if (candidate.completion == Completion::Complete)
        return BlockedReason::AlreadyComplete;
    if (candidate.prerequisites == GateState::Unknown)
        return BlockedReason::PrerequisitesUnknown;
    if (candidate.prerequisites == GateState::Blocked)
        return BlockedReason::PrerequisitesBlocked;
    if (candidate.reachability == Reachability::Unknown)
        return BlockedReason::ReachabilityUnknown;
    if (candidate.reachability == Reachability::Unreachable)
        return BlockedReason::Unreachable;
    return BlockedReason::None;
}

inline bool Prefer(CandidateFacts const& candidate, CandidateFacts const& current)
{
    if (candidate.score != current.score)
        return candidate.score > current.score;
    if (candidate.routeOrder != current.routeOrder)
        return candidate.routeOrder < current.routeOrder;
    if (candidate.key.mapId != current.key.mapId)
        return candidate.key.mapId < current.key.mapId;
    if (candidate.key.encounterId != current.key.encounterId)
        return candidate.key.encounterId < current.key.encounterId;
    return candidate.goalId < current.goalId;
}
}

inline Selection Select(std::vector<CandidateFacts> const& input)
{
    Selection selection;
    if (input.empty())
        return selection;

    std::vector<CandidateFacts> candidates = input;
    std::sort(candidates.begin(), candidates.end(), Detail::CanonicalFactsOrder);

    bool invalidInput = false;
    bool allComplete = true;
    CandidateFacts const* selected = nullptr;
    selection.decisions.reserve(candidates.size());

    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        CandidateFacts const& candidate = candidates[index];
        bool const duplicate = (index && Detail::SameKey(candidates[index - 1].key, candidate.key)) ||
            (index + 1 < candidates.size() && Detail::SameKey(candidate.key, candidates[index + 1].key));

        BlockedReason reason;
        if (!candidate.key.mapId || !candidate.goalId)
        {
            reason = BlockedReason::InvalidKey;
            invalidInput = true;
        }
        else if (duplicate)
        {
            reason = BlockedReason::DuplicateKey;
            invalidInput = true;
        }
        else
        {
            reason = Detail::Classify(candidate);
        }

        selection.decisions.push_back({candidate.key, candidate.goalId, reason});
        allComplete = allComplete && candidate.completion == Completion::Complete;
        if (reason == BlockedReason::None && (!selected || Detail::Prefer(candidate, *selected)))
            selected = &candidate;
    }

    if (invalidInput)
    {
        selection.status = Status::InvalidInput;
        return selection;
    }
    if (selected)
    {
        selection.status = Status::Selected;
        selection.selected = selected->key;
        selection.selectedGoalId = selected->goalId;
        return selection;
    }

    selection.status = allComplete ? Status::AllComplete : Status::Blocked;
    return selection;
}
}

#endif
