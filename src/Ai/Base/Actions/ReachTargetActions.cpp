/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReachTargetActions.h"

#include <cmath>

#include "Action.h"
#include "CriticalMemberRecoveryPolicy.h"
#include "Event.h"
#include "Map.h"
#include "PathGenerator.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RaidTargetClaimValue.h"
#include "../ReleasedCorpseApproachPolicy.h"
#include "ResurrectionTargetPolicy.h"
#include "ServerFacade.h"
#include "Timer.h"

namespace
{
constexpr float CorpseApproachHeightSearch =
    ReleasedCorpseApproachPolicy::MaxVerticalDelta * 2.0f + 2.0f;
constexpr float CorpseApproachEndpointTolerance = 2.5f;
constexpr uint32 CorpseApproachRejectedPathTypes =
    PATHFIND_SHORTCUT | PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH |
    PATHFIND_SHORT | PATHFIND_FARFROMPOLY;

bool IsReachableCorpseApproachPath(PathGenerator const& path, float x, float y, float z)
{
    uint32 const pathType = static_cast<uint32>(path.GetPathType());
    if (!(pathType & PATHFIND_NORMAL) || (pathType & CorpseApproachRejectedPathTypes))
        return false;

    G3D::Vector3 const& endpoint = path.GetActualEndPosition();
    float const dx = endpoint.x - x;
    float const dy = endpoint.y - y;
    float const dz = endpoint.z - z;
    return dx * dx + dy * dy + dz * dz <=
           CorpseApproachEndpointTolerance * CorpseApproachEndpointTolerance;
}

bool HasActiveBlockingRaidClaim(PlayerbotAI* memberAI)
{
    if (!memberAI)
        return false;

    RaidTargetClaim const& claim =
        memberAI->GetAiObjectContext()->GetValue<RaidTargetClaim&>("raid target claim")->Get();
    return claim.target && !RaidTargetClaimPolicy::IsExpired(getMSTime(), claim.expiresAtMs) &&
           static_cast<uint8>(claim.authority) >=
               static_cast<uint8>(RaidTargetAuthority::BossOwnership);
}

bool HasActiveAvoidance(PlayerbotAI* memberAI)
{
    if (!memberAI)
        return false;

    Action* avoidance = memberAI->GetAiObjectContext()->GetAction("avoid aoe");
    return avoidance && avoidance->isUseful();
}

bool IsRecoveryMovementSuperseded(PlayerbotAI* memberAI, Player* member)
{
    if (!member || !member->IsAlive() || PlayerbotAI::IsTank(member))
        return true;

    return HasActiveBlockingRaidClaim(memberAI) || HasActiveAvoidance(memberAI);
}

bool IsCriticalRecoveryTarget(Unit* target, Player* healer)
{
    return target && healer && target->IsPlayer() && target->IsAlive() && target->IsInWorld() &&
           target->GetHealthPct() < sPlayerbotAIConfig.criticalHealth &&
           CriticalMemberRecoveryPolicy::IsWithinBoundedRecoveryRange(
               healer->GetDistance2d(target), sPlayerbotAIConfig.healDistance);
}
}

bool ReachTargetAction::Execute(Event /*event*/) { return ReachCombatTo(AI_VALUE(Unit*, GetTargetName()), distance); }

bool ReachTargetAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    // do not move while casting
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        return false;
    }
    Unit* target = GetTarget();
    // float dis = distance + CONTACT_DISTANCE;
    return target &&
           !bot->IsWithinCombatRange(target, distance);  // ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float,
                                                         // "distance", GetTargetName()), distance);
}

std::string const ReachTargetAction::GetTargetName() { return "current target"; }

bool CastReachTargetSpellAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"),
                                                (distance + sPlayerbotAIConfig.contactDistance));
}

ReachSpellAction::ReachSpellAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach spell", botAI->GetRange("spell"))
{
}

ReachPartyMemberToHealAction::ReachPartyMemberToHealAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to heal", botAI->GetRange("heal"))
{
}

bool ReachPartyMemberToHealAction::Execute(Event event)
{
    Unit* target = GetTarget();
    if (!IsCriticalRecoveryTarget(target, bot))
        return ReachTargetAction::Execute(event);

    if (IsRecoveryMovementSuperseded(botAI, bot))
        return false;

    bool moved = ReachCombatTo(target, distance);
    if (!moved && !bot->IsWithinLOSInMap(target))
        moved = MoveToLOS(target, true);

    CriticalMemberRecoveryPolicy::Decision attempt;
    attempt.action = CriticalMemberRecoveryPolicy::Action::HealerApproach;
    attempt.reason = CriticalMemberRecoveryPolicy::Reason::HealerApproachAccepted;
    attempt.claim.targetGuid = target->GetGUID().GetRawValue();
    attempt.claim.claimantGuid = bot->GetGUID().GetRawValue();
    attempt.claim.expiresAtMs = getMSTime() + CriticalMemberRecoveryPolicy::kClaimLifetimeMs;
    CriticalMemberRecoveryPolicy::Decision const outcome =
        CriticalMemberRecoveryPolicy::AfterHealerApproach(attempt, moved);

    if (outcome.action == CriticalMemberRecoveryPolicy::Action::NoPathOrMovementRejected)
    {
        // ReachCombatTo owns the real nav/path check.  Its false result also covers a duplicate or
        // movement-priority rejection, so report that combined outcome honestly and fail closed;
        // do not make the critical member, tank pickup, or avoidance action acquire a second move.
        LOG_DEBUG(
            "playerbots",
            "[CriticalHealRecovery] target_guid={} claimant_guid={} action=healer_approach "
            "outcome=no_path_or_move_rejected claim_expires_ms={} distance={} heal_distance={} "
            "max_multiplier={} rejoin=fail_closed",
            target->GetGUID().GetRawValue(), bot->GetGUID().GetRawValue(),
            outcome.claim.expiresAtMs, bot->GetDistance2d(target), sPlayerbotAIConfig.healDistance,
            CriticalMemberRecoveryPolicy::kMaxRecoveryRangeMultiplier);
    }

    return moved;
}

bool ReachPartyMemberToHealAction::isUseful()
{
    Unit* target = GetTarget();
    if (!IsCriticalRecoveryTarget(target, bot))
        return ReachTargetAction::isUseful();

    if (IsRecoveryMovementSuperseded(botAI, bot) ||
        botAI->HasStrategy("stay", botAI->GetState()) ||
        bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        return false;
    }

    return !(bot->GetDistance2d(target) <= sPlayerbotAIConfig.healDistance &&
             bot->IsWithinLOSInMap(target));
}

std::string const ReachPartyMemberToHealAction::GetTargetName() { return "party member to heal"; }

ReachPartyMemberToResurrectAction::ReachPartyMemberToResurrectAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to resurrect", botAI->GetRange("spell"))
{
}

bool ReachPartyMemberToResurrectAction::MoveToReleasedCorpseFallback(
    WorldObject* anchor, Player* dead, char const* reason)
{
    auto const candidates = ReleasedCorpseApproachPolicy::BuildCandidates(
        anchor->GetPositionX(), anchor->GetPositionY());
    ReleasedCorpseApproachPolicy::Selection const selection =
        ReleasedCorpseApproachPolicy::Select(
            candidates, anchor->GetPositionZ(),
            ReleasedCorpseApproachPolicy::FirstFallbackProbe,
            [this, anchor](ReleasedCorpseApproachPolicy::Candidate const& candidate)
            {
                ReleasedCorpseApproachPolicy::Evaluation evaluation;
                Map* map = bot->GetMap();
                if (!map)
                    return evaluation;

                float const ground = map->GetHeight(
                    bot->GetPhaseMask(), candidate.x, candidate.y,
                    anchor->GetPositionZ() + ReleasedCorpseApproachPolicy::MaxVerticalDelta,
                    true, CorpseApproachHeightSearch);
                evaluation.floorValid = ground > INVALID_HEIGHT && std::isfinite(ground);
                evaluation.endpointZ = ground;
                if (!evaluation.floorValid ||
                    !ReleasedCorpseApproachPolicy::IsSameVerticalLevel(anchor->GetPositionZ(), ground))
                {
                    return evaluation;
                }

                evaluation.corpseLineOfSight = anchor->IsWithinLOS(
                    candidate.x, candidate.y, ground + bot->GetCollisionHeight());
                if (!evaluation.corpseLineOfSight)
                    return evaluation;

                PathGenerator path(bot);
                bool const calculated = path.CalculatePath(candidate.x, candidate.y, ground, false);
                evaluation.navmeshReachable = calculated &&
                    IsReachableCorpseApproachPath(path, candidate.x, candidate.y, ground);
                return evaluation;
            });

    bool started = false;
    if (selection.found)
    {
        started = MoveTo(
            anchor->GetMapId(), selection.candidate.x, selection.candidate.y,
            selection.endpointZ, false, false, true, false,
            MovementPriority::MOVEMENT_NORMAL);
    }

    LOG_DEBUG("playerbots",
              "[CorpseRescue] endpoint healer={} target={} reason={} selected={} probe={} radius={} "
              "angle_index={} probes={} started={}",
              bot->GetName(), dead->GetName(), reason, selection.found,
              selection.found ? selection.candidate.probeIndex : 0,
              selection.found ? selection.candidate.radius : 0.0f,
              selection.found ? static_cast<uint32>(selection.candidate.angleIndex) : 0,
              selection.probesEvaluated, started);
    return started;
}

bool ReachPartyMemberToResurrectAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    Player* dead = target ? target->ToPlayer() : nullptr;
    ResurrectionTargetPolicy::ResolvedTarget const resolved =
        ResurrectionTargetPolicy::Resolve(bot, dead);
    if (resolved.kind == ResurrectionTargetPolicy::TargetKind::Invalid ||
        ResurrectionTargetPolicy::HasResurrectionReservation(bot, dead, resolved.corpse))
    {
        return false;
    }

    WorldObject* anchor = ResurrectionTargetPolicy::GetAnchor(resolved);
    if (!anchor)
        return false;

    bool const withinRange = bot->GetDistance(anchor) <= distance;
    bool const hasLineOfSight = bot->IsWithinLOSInMap(anchor);
    if (!hasLineOfSight)
    {
        bool started = MoveToLOS(anchor, true);
        char const* method = "los";
        if (!started && resolved.kind == ResurrectionTargetPolicy::TargetKind::ReleasedCorpse)
        {
            // The exact corpse point is probe zero above.  If it has no usable path/LOS waypoint,
            // search four deterministic rings around that same corpse.  This is bounded to 32
            // additional read-only probes (33 total candidates), rejects other floors, and never
            // teleports or introduces random movement.
            started = MoveToReleasedCorpseFallback(anchor, dead, "los");
            method = "corpse_ring";
        }
        else if (!started)
        {
            // Some valid corpse anchors have no path point that already satisfies target-side LOS
            // (doorways, ramps, and uneven raid terrain are common examples).  Do not stall at the
            // current position: take a normal bounded step toward the real corpse and let the next
            // AI tick re-evaluate range and LOS.  This remains ordinary navmesh movement.
            started = MoveTo(anchor, distance, MovementPriority::MOVEMENT_NORMAL);
            method = "approach";
        }
        LOG_DEBUG("playerbots", "[CorpseRescue] move healer={} target={} mode={} reason=los method={} started={}",
                  bot->GetName(), dead->GetName(), ResurrectionTargetPolicy::ToString(resolved.kind),
                  method, started);
        return started;
    }

    if (!withinRange)
    {
        bool started = MoveTo(anchor, distance, MovementPriority::MOVEMENT_NORMAL);
        if (!started && resolved.kind == ResurrectionTargetPolicy::TargetKind::ReleasedCorpse)
            started = MoveToReleasedCorpseFallback(anchor, dead, "range");
        LOG_DEBUG("playerbots", "[CorpseRescue] move healer={} target={} mode={} reason=range started={}",
                  bot->GetName(), dead->GetName(), ResurrectionTargetPolicy::ToString(resolved.kind),
                  started);
        return started;
    }

    return false;
}

bool ReachPartyMemberToResurrectAction::isUseful()
{
    if (botAI->HasStrategy("stay", botAI->GetState()) ||
        bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        return false;
    }

    Unit* target = GetTarget();
    Player* dead = target ? target->ToPlayer() : nullptr;
    ResurrectionTargetPolicy::ResolvedTarget const resolved =
        ResurrectionTargetPolicy::Resolve(bot, dead);
    if (resolved.kind == ResurrectionTargetPolicy::TargetKind::Invalid ||
        ResurrectionTargetPolicy::HasResurrectionReservation(bot, dead, resolved.corpse))
    {
        return false;
    }

    WorldObject* anchor = ResurrectionTargetPolicy::GetAnchor(resolved);
    if (!anchor)
        return false;

    return ResurrectionTargetPolicy::NeedsMovement(bot->GetDistance(anchor) <= distance,
                                                    bot->IsWithinLOSInMap(anchor));
}

std::string const ReachPartyMemberToResurrectAction::GetTargetName() { return "party member to resurrect"; }
