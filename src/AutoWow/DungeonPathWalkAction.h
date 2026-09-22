#ifndef _PLAYERBOT_AUTOWOW_DUNGEON_PATH_WALK_ACTION_H
#define _PLAYERBOT_AUTOWOW_DUNGEON_PATH_WALK_ACTION_H

#include "DungeonPathSafety.h"
#include "QuestGiverTravelFeedback.h"

#include "AiObjectContext.h"
#include "MovementActions.h"
#include "MotionMaster.h"
#include "Player.h"

// Executes the exact path that passed AutoWowDungeonPath preflight. This is shared by bridge
// orders and autonomous dungeon navigation so a validated ground-line fallback is not discarded
// and silently recalculated as an unusable navmesh path by a second movement layer.
class AutoWowDungeonWalkAction final : public MovementAction
{
public:
    explicit AutoWowDungeonWalkAction(PlayerbotAI* botAI) :
        MovementAction(botAI, "autowow dungeon walk") {}

    bool Execute(Event /*event*/) override { return false; }

    bool Walk(uint32 mapId, float x, float y, float z)
    {
        if (mapId != bot->GetMapId())
            return false;
        return WalkPrepared(AutoWowDungeonPath::Probe(bot, x, y, z));
    }

    bool WalkPrepared(AutoWowDungeonPath::ProbeResult const& probe,
                      AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason* rejectReason = nullptr)
    {
        auto reject = [rejectReason](AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason reason)
        {
            if (rejectReason)
                *rejectReason = reason;
            return false;
        };
        if (rejectReason)
            *rejectReason = AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::None;

        if (!probe.safe || probe.path.size() < 2)
            return reject(probe.safe ? AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::PathTooShort
                                     : AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::UnsafeProbe);

        UpdateMovementState();
        if (!IsMovingAllowed())
            return reject(AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::MovementNotAllowed);

        G3D::Vector3 const& destination = probe.path.back();
        if (bot->GetExactDist(destination.x, destination.y, destination.z) <= 0.25f)
        {
            if (rejectReason)
                *rejectReason = AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::AlreadyAtEndpoint;
            return true;
        }
        if (IsDuplicateMove(destination.x, destination.y, destination.z) ||
            IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        {
            if (IsDuplicateMove(destination.x, destination.y, destination.z))
                return reject(AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::DuplicateMove);
            return reject(AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::WaitingForLastMove);
        }

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        MotionMaster* motion = bot->GetMotionMaster();
        if (!motion)
            return reject(AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::MotionMasterMissing);

        Movement::PointsArray path = probe.path;
        motion->Clear();
        motion->MoveSplinePath(&path, FORCED_MOVEMENT_NONE);

        // Reserve the prepared normal-priority spline for its uncapped estimated travel time.
        // Ordinary movement therefore cannot replace it on the next AI tick, while combat
        // movement remains free to pre-empt it through the existing priority ordering.
        float reservationMs = 1000.0f * MoveDelay(probe.pathLength);
        if (reservationMs < 0.0f)
            reservationMs = 0.0f;
        context->GetValue<LastMovement&>("last movement")->Get().Set(
            bot->GetMapId(), destination.x, destination.y, destination.z,
            bot->GetOrientation(), reservationMs, MovementPriority::MOVEMENT_NORMAL);
        WaitForReach(probe.pathLength);
        if (rejectReason)
            *rejectReason = AutoWowQuestGiverTravel::QuestWalkPreparedRejectReason::Accepted;
        return true;
    }
};

#endif
