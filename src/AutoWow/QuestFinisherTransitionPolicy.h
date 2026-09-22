/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify it under the terms of the
 * GNU General Public License version 2 or later.
 */

#ifndef PLAYERBOTS_AUTOWOW_QUEST_FINISHER_TRANSITION_POLICY_H
#define PLAYERBOTS_AUTOWOW_QUEST_FINISHER_TRANSITION_POLICY_H

#include "QuestDef.h"
#include "OracleRouteExecutor.h"

namespace AutoWowQuestFinisher
{
// A quest can be ready for a finisher before AzerothCore has changed its persisted Status from
// INCOMPLETE to COMPLETE. This is the normal state immediately before the client sends
// CMSG_QUESTGIVER_REQUEST_REWARD; the core's CanCompleteQuest() predicate is the authority that
// decides whether that transition is legal.
enum class Directive : uint8
{
    None,
    RequestCompletion,
    TurnIn
};

struct Facts
{
    QuestStatus status = QUEST_STATUS_NONE;
    bool rewarded = false;
    bool coreCanComplete = false;
    bool finisherResolved = false;
};

[[nodiscard]] inline Directive SelectDirective(Facts const& facts)
{
    if (facts.rewarded || !facts.finisherResolved)
        return Directive::None;

    if (facts.status == QUEST_STATUS_COMPLETE)
        return Directive::TurnIn;

    if (facts.status == QUEST_STATUS_INCOMPLETE && facts.coreCanComplete)
        return Directive::RequestCompletion;

    return Directive::None;
}

[[nodiscard]] inline bool IsFinisherReady(Facts const& facts)
{
    return SelectDirective(facts) != Directive::None;
}

struct RouteAdmissionFacts
{
    bool oracleManaged = false;
    bool oracleTagged = false;
    bool routeV2Enabled = false;
    bool exactStableDescriptor = false;
};

[[nodiscard]] inline bool UseOracleRoute(RouteAdmissionFacts const& facts)
{
    return facts.oracleManaged && facts.oracleTagged && facts.routeV2Enabled &&
           facts.exactStableDescriptor;
}

// Closed conversion table from pure-policy commands to the only native operations this lane may
// perform.  Unknown values do not acquire a movement fallback.
enum class RouteNativeAction : uint8
{
    None,
    SupplySafeAnchor,
    SupplyGroundPlan,
    WalkPreparedPath,
    Reprobe,
    ExactLiveBind,
    ApproachExactTarget,
    VerifyExactTarget,
    ReresolveExactTarget,
    YieldLease,
    UnsupportedTransition
};

[[nodiscard]] inline RouteNativeAction SelectRouteNativeAction(
    AutoWowOracleRoute::RouteResult const& result)
{
    if (result.failure == AutoWowOracleRoute::RouteFailure::UnsupportedTransition)
        return RouteNativeAction::UnsupportedTransition;

    switch (result.command.kind)
    {
        case AutoWowOracleRoute::RouteCommandKind::SelectSafeAnchor:
            return RouteNativeAction::SupplySafeAnchor;
        case AutoWowOracleRoute::RouteCommandKind::GroundPlan:
        case AutoWowOracleRoute::RouteCommandKind::Repath:
            return RouteNativeAction::SupplyGroundPlan;
        case AutoWowOracleRoute::RouteCommandKind::Move:
            return RouteNativeAction::WalkPreparedPath;
        case AutoWowOracleRoute::RouteCommandKind::Reprobe:
            return RouteNativeAction::Reprobe;
        case AutoWowOracleRoute::RouteCommandKind::ExactLiveBind:
            return RouteNativeAction::ExactLiveBind;
        case AutoWowOracleRoute::RouteCommandKind::Approach:
            return RouteNativeAction::ApproachExactTarget;
        case AutoWowOracleRoute::RouteCommandKind::Verify:
            return RouteNativeAction::VerifyExactTarget;
        case AutoWowOracleRoute::RouteCommandKind::Reresolve:
            return RouteNativeAction::ReresolveExactTarget;
        case AutoWowOracleRoute::RouteCommandKind::Yield:
            return RouteNativeAction::YieldLease;
        case AutoWowOracleRoute::RouteCommandKind::None:
            return RouteNativeAction::None;
    }
    return RouteNativeAction::UnsupportedTransition;
}
}  // namespace AutoWowQuestFinisher

#endif  // PLAYERBOTS_AUTOWOW_QUEST_FINISHER_TRANSITION_POLICY_H
