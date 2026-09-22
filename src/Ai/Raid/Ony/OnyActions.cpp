#include "OnyActions.h"

#include "GenericSpellActions.h"
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "OnyBreathSignal.h"
#include "Playerbots.h"
#include "PositionAction.h"
#include "RaidSafeZoneSelectionPolicy.h"

bool RaidOnyxiaMoveToSideAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss)
        return false;

    float angleToBot = boss->GetAngle(bot);
    float bossFacing = boss->GetOrientation();
    float diff = fabs(angleToBot - bossFacing);
    if (diff > M_PI)
        diff = 2 * M_PI - diff;

    float distance = bot->GetDistance(boss);

    // Too close (30 yards) and either in front or behind
    if (distance <= 30.0f && (diff < M_PI / 4 || diff > 3 * M_PI / 4))
    {
        float offsetAngle = bossFacing + M_PI_2;  // 90° to the right
        float offsetDist = 15.0f;

        float sideX = boss->GetPositionX() + offsetDist * cos(offsetAngle);
        float sideY = boss->GetPositionY() + offsetDist * sin(offsetAngle);

        // bot->Yell("Too close to front or tail — moving to side of Onyxia!", LANG_UNIVERSAL);
        return MoveTo(boss->GetMapId(), sideX, sideY, boss->GetPositionZ(), false, false, false, false,
                      MovementPriority::MOVEMENT_COMBAT);
    }

    return false;
}

bool RaidOnyxiaSpreadOutAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");

    if (!boss)
        return false;

    // Trigger may fire on one tick, but the action can execute on a later tick.
    // By that time the cast may have finished, so current spell can be null.
    Spell* currentSpell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!currentSpell || !currentSpell->m_spellInfo)
        return false;

    // Fireball
    if (currentSpell->m_spellInfo->Id != 18392)
        return false;

    Unit* unitTarget = currentSpell->m_targets.GetUnitTarget();
    Player* target = unitTarget ? unitTarget->ToPlayer() : nullptr;
    if (!target || target != bot)
        return false;

    // bot->Yell("Spreading out — I'm the Fireball target!", LANG_UNIVERSAL);
    return MoveFromGroup(9.0f);  // move 9 yards
}

bool RaidOnyxiaMoveToSafeZoneAction::Execute(Event /*event*/)
{
    uint32 spellId = OnyxiaBreathSignal::GetActive(bot->GetGUID());
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    Spell* currentSpell = boss ? boss->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (!spellId && currentSpell && currentSpell->m_spellInfo &&
        OnyxiaBreathSignal::IsDeepBreathSpell(currentSpell->m_spellInfo->Id))
    {
        spellId = currentSpell->m_spellInfo->Id;
        OnyxiaBreathSignal::Record(bot->GetGUID(), spellId);
    }
    if (!spellId)
        return false;

    std::vector<SafeZone> safeZones = GetSafeZonesForBreath(spellId);
    if (safeZones.empty())
        return false;

    std::vector<RaidSafeZoneSelectionPolicy::Point> zonePoints;
    zonePoints.reserve(safeZones.size());
    for (SafeZone const& zone : safeZones)
        zonePoints.push_back({zone.pos.GetPositionX(), zone.pos.GetPositionY()});

    // Deep Breath is directional: crossing the room to follow a shared raid anchor can move a
    // bot through the breath lane. Each responder therefore uses its nearest valid shelter.
    std::size_t const bestIndex = RaidSafeZoneSelectionPolicy::SelectClosest(
        {bot->GetPositionX(), bot->GetPositionY()}, zonePoints);
    SafeZone* bestZone = &safeZones[bestIndex];
    float const bestDist = bot->GetExactDist2d(bestZone->pos.GetPositionX(), bestZone->pos.GetPositionY());

    if (bot->IsWithinDist2d(bestZone->pos.GetPositionX(), bestZone->pos.GetPositionY(), bestZone->radius))
    {
        if (OnyxiaBreathSignal::MarkArrival(bot->GetGUID(), spellId))
        {
            LOG_INFO("playerbots", "[Onyxia] breath_safe_arrival bot={} guid={} spell={} pos=({:.2f},{:.2f},{:.2f})",
                bot->GetName(), bot->GetGUID().GetCounter(), spellId, bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ());
        }
        return false;  // Already safe
    }

    // Stop current spell first
    bot->AttackStop();
    bot->InterruptNonMeleeSpells(false);

    if (OnyxiaBreathSignal::MarkMove(bot->GetGUID(), spellId))
    {
        LOG_INFO("playerbots",
            "[Onyxia] breath_safe_move bot={} guid={} spell={} from=({:.2f},{:.2f},{:.2f}) to=({:.2f},{:.2f},{:.2f}) distance={:.2f}",
            bot->GetName(), bot->GetGUID().GetCounter(), spellId, bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), bestZone->pos.GetPositionX(), bestZone->pos.GetPositionY(),
            bestZone->pos.GetPositionZ(), bestDist);
    }

    // bot->Yell("Moving to Safe Zone!", LANG_UNIVERSAL);
    return MoveTo(bot->GetMapId(), bestZone->pos.GetPositionX(), bestZone->pos.GetPositionY(), bestZone->pos.GetPositionZ(),
                  false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
}

bool RaidOnyxiaKillWhelpsAction::Execute(Event /*event*/)
{
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    // If already attacking a whelp, don't swap targets
    if (currentTarget && currentTarget->GetEntry() == 11262)
    {
        return false;
    }
    GuidVector targets = AI_VALUE(GuidVector, "possible targets");
    for (ObjectGuid guid : targets)
    {
        Creature* unit = botAI->GetCreature(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld())
            continue;

        if (unit->GetEntry() == 11262)  // Onyxia Whelp
        {
            // bot->Yell("Attacking Whelps!", LANG_UNIVERSAL);
            return AttackWithRaidTargetClaim(
                unit, "ony whelp baseline", RaidTargetAuthority::EncounterBaseline);
        }
    }
    return false;
}

bool RaidOnyxiaAttackFlyingBossAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || !boss->IsAlive() || !boss->IsFlying())
        return false;

    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget == boss)
        return false;

    return AttackWithRaidTargetClaim(
        boss, "ony flying baseline", RaidTargetAuthority::EncounterBaseline);
}

bool OnyxiaAvoidEggsAction::Execute(Event /*event*/)
{
    Position botPos = Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    float x, y;

    // get safe zone slightly away from eggs (Can this be dynamic?)
    if (botPos.GetExactDist2d(-36.0f, -164.0f) <= 5.0f)
    {
        x = -10.0f;
        y = -180.0f;
    }
    else if (botPos.GetExactDist2d(-34.0f, -262.0f) <= 5.0f)
    {
        x = -16.0f;
        y = -250.0f;
    }
    else
    {
        return false;  // Not in danger zone
    }

    // bot->Yell("Too close to eggs — backing off!", LANG_UNIVERSAL);

    return MoveTo(bot->GetMapId(), x, y, bot->GetPositionZ(), false, false, false, false,
                  MovementPriority::MOVEMENT_COMBAT);
}
