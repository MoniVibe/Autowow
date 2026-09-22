/*
 * Pure policy for safe, idempotent AutoWoW exact-party creation and repair.
 * Live Group and Playerbot mutations remain in AutoWowBridge.cpp.
 */
#ifndef MOD_PLAYERBOTS_EXACT_PARTY_REPAIR_POLICY_H
#define MOD_PLAYERBOTS_EXACT_PARTY_REPAIR_POLICY_H

#include <cstddef>

namespace AutoWowExactParty
{
enum class ExactPartyAction
{
    Refuse,
    Create,
    Reconfigure,
    RepairSubset
};

struct ExactPartySignals
{
    std::size_t requestedCount = 0;
    std::size_t currentCount = 0;
    std::size_t subsetGroupCount = 0;
    std::size_t groupedRequestedCount = 0;
    bool requestedLeaderPresent = false;
    bool allRequestedOnline = false;
    bool allRequestedPlayerbots = false;
    bool sameFaction = false;
    bool leaderHasGroup = false;
    bool leaderPresentInCurrentGroup = false;
    bool allSubsetGroupsOrdinary = false;
    bool allSubsetGroupMembersRequested = false;
    bool allSubsetGroupMembersOnline = false;
    bool allSubsetGroupMembersPlayerbots = false;
    bool allGroupedRequestedMembersPresent = false;
};

inline constexpr ExactPartyAction ClassifyExactParty(ExactPartySignals const& state)
{
    if (state.requestedCount < 2 || state.requestedCount > 5 || !state.requestedLeaderPresent ||
        !state.allRequestedOnline || !state.allRequestedPlayerbots || !state.sameFaction)
    {
        return ExactPartyAction::Refuse;
    }

    if (state.leaderHasGroup != state.leaderPresentInCurrentGroup)
        return ExactPartyAction::Refuse;

    if (state.subsetGroupCount == 0)
    {
        return state.groupedRequestedCount == 0 && !state.leaderHasGroup ?
            ExactPartyAction::Create : ExactPartyAction::Refuse;
    }

    if (state.groupedRequestedCount == 0 || state.groupedRequestedCount > state.requestedCount ||
        state.subsetGroupCount > state.groupedRequestedCount ||
        (state.leaderHasGroup && (state.currentCount == 0 || state.currentCount > state.requestedCount)) ||
        !state.allSubsetGroupsOrdinary || !state.allSubsetGroupMembersRequested ||
        !state.allSubsetGroupMembersOnline || !state.allSubsetGroupMembersPlayerbots ||
        !state.allGroupedRequestedMembersPresent)
    {
        return ExactPartyAction::Refuse;
    }

    if (state.subsetGroupCount == 1 && state.leaderHasGroup &&
        state.currentCount == state.requestedCount &&
        state.groupedRequestedCount == state.requestedCount)
    {
        return ExactPartyAction::Reconfigure;
    }

    // Every requested player's extant group has already passed the closed safety checks above.
    // Any remaining shape is a safe subset/fragmentation repair: disband all of those groups and
    // rebuild the exact roster under the configured leader.
    return ExactPartyAction::RepairSubset;
}
} // namespace AutoWowExactParty

#endif
