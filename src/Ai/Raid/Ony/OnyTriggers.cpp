#include "OnyTriggers.h"

#include "EncounterRoleTriggerPolicy.h"
#include "OnyBreathSignal.h"
#include "RaidFearResponsePolicy.h"
#include "RaidFearSignal.h"
#include "GenericTriggers.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "NearestNpcsValue.h"

OnyxiaDeepBreathTrigger::OnyxiaDeepBreathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony deep breath warning") {}

bool OnyxiaDeepBreathTrigger::IsActive()
{
    if (uint32 const encounterSpell = OnyxiaBreathSignal::GetEncounterActive(bot->GetInstanceId()))
    {
        OnyxiaBreathSignal::Record(bot->GetGUID(), encounterSpell);
        return true;
    }

    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || !boss->IsAlive())
    {
        OnyxiaBreathSignal::Clear(bot->GetGUID());
        return false;
    }

    // Onyxia's encounter script starts Deep Breath with an effectively instant AOE spell and
    // then waits before crossing the room. The AllSpellScript listener preserves that short
    // cast event at instance scope so ordinary bot AI ticks cannot miss the warning.
    if (boss->IsFlying() && boss->HasUnitState(UNIT_STATE_CASTING))
    {
        Spell* currentSpell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (currentSpell && currentSpell->m_spellInfo &&
            OnyxiaBreathSignal::IsDeepBreathSpell(currentSpell->m_spellInfo->Id))
        {
            OnyxiaBreathSignal::Record(bot->GetGUID(), currentSpell->m_spellInfo->Id);
        }
    }

    // Trigger evaluation and action execution happen on different AI ticks. Keep the warning
    // alive after CURRENT_GENERIC_SPELL disappears so the emergency movement cannot be lost.
    return OnyxiaBreathSignal::GetActive(bot->GetGUID()) != 0;
}

OnyxiaNearTailTrigger::OnyxiaNearTailTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony near tail") {}

bool OnyxiaNearTailTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || botAI->IsTank(bot))
        return false;

    // Skip if Onyxia is in air or transitioning
    if (!boss->IsInCombat() || boss->IsFlying() || !boss->GetVictim())
        return false;

    return true;
}
RaidOnyxiaFireballSplashTrigger::RaidOnyxiaFireballSplashTrigger(PlayerbotAI* botAI)
    : Trigger(botAI, "ony fireball splash incoming")
{
}

bool RaidOnyxiaFireballSplashTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || !boss->HasUnitState(UNIT_STATE_CASTING))
        return false;

    // Check if Onyxia is casting Fireball
    Spell* currentSpell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!currentSpell || !currentSpell->m_spellInfo || currentSpell->m_spellInfo->Id != 18392)  // 18392 is the classic Fireball ID  // 18392 is the classic Fireball ID
        return false;

    GuidVector nearbyUnits = AI_VALUE(GuidVector, "nearest friendly players");

    for (ObjectGuid guid : nearbyUnits)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == bot || !unit->IsAlive())
            continue;

        if (bot->GetDistance(unit) < 8.0f)
            return true;
    }

    return false;
}

RaidOnyxiaWhelpsSpawnTrigger::RaidOnyxiaWhelpsSpawnTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony whelps spawn") {}

bool RaidOnyxiaWhelpsSpawnTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss)
        return false;

    return EncounterRoleTriggerPolicy::ShouldTargetOnyxiaWhelps(
        botAI->IsHeal(bot), botAI->IsTank(bot), botAI->IsRanged(bot), boss->IsFlying());
}

RaidOnyxiaFlyingRangedPressureTrigger::RaidOnyxiaFlyingRangedPressureTrigger(PlayerbotAI* botAI)
    : Trigger(botAI, "ony flying ranged pressure")
{
}

bool RaidOnyxiaFlyingRangedPressureTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss)
        return false;

    return EncounterRoleTriggerPolicy::ShouldPressureFlyingOnyxia(
        botAI->IsHeal(bot), botAI->IsDps(bot), botAI->IsRanged(bot), boss->IsFlying());
}

bool RaidOnyxiaImminentFearTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || !RaidFearResponsePolicy::ShouldWarnForOnyxiaLanding(
            boss->IsAlive(), boss->IsFlying(), boss->GetHealthPct()))
    {
        return false;
    }

    RaidFearSignal::RecordImminent(bot->GetInstanceId(), boss->GetEntry());
    return true;
}

OnyxiaAvoidEggsTrigger::OnyxiaAvoidEggsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony avoid eggs") {}

bool OnyxiaAvoidEggsTrigger::IsActive()
{
    Position botPos = Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    if (botPos.GetExactDist2d(-35.0f, -165.0f) <= 5.0f)
        return true;

    if (botPos.GetExactDist2d(-35.0f, -260.0f) <= 5.0f)
        return true;

    return false;
}
