#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "Duration.h"
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    // Long-range walk/path toward `dest`.
    //
    // `questNoTeleport`: when set, this is a quest-path move (objective source
    // or finisher travel). On genuine stuck the historical teleport recovery is
    // suppressed for ALL bots (not just the AutoWow league) and the failure is
    // surfaced via `outStuck` so the caller can raise a typed
    // MovementStuckNoTeleport blocker. When false (default), legacy free-roam /
    // grind behavior is unchanged: AutoWow bots hold, other bots teleport.
    //
    // `outStuck` (optional): set to true only on the tick the stuck threshold
    // trips while teleport is suppressed; left false otherwise.
    //
    // `deterministicPath`: accept only a verified mmap/direct path. It disables the historical
    // randomized forward-cone stepping-stone fallback and returns false when no route endpoint
    // is verified. Exact quest finishers use this mode so a missing path becomes a typed blocker,
    // never an indirect random walk toward an unrelated object.
    //
    // `allowLegacyTeleportRecovery`: a narrow compatibility override used only by unmanaged
    // finishers. It restores baseline stuck teleport recovery even when an external AutoWow party
    // marker remains set. All other callers retain the existing policy default.
    bool MoveFarTo(WorldPosition dest, bool questNoTeleport = false, bool* outStuck = nullptr,
                   bool deterministicPath = false,
                   StrictFinisherMovementPolicy::RouteIdentity strictRoute = {},
                   bool allowLegacyTeleportRecovery = false);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();
    // Relieve an inventory blocker at a vendor through the ordinary SellAction/core handler.
    // Incomplete item objectives use the narrower Oracle gray-item sale policy and resume
    // objective resolution; completed quests preserve their existing reward retry behavior.
    bool TryRelieveInventoryAtVendor(NewRpgInfo::DoQuest& data, bool incompleteItem = false);

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    WorldPosition SelectRandomGrindPos(Player* bot);
    WorldPosition SelectRandomCampPos(Player* bot);
    bool IsAutoWowTravelBot() const;
    void MarkTravelDestinationFailed(WorldPosition const& pos);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * 1000;
};

#endif
