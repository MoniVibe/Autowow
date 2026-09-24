/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalClassStrategy.h"

#include "Pet.h"
#include "Playerbots.h"
#include "Spell.h"
#include "TacticalRuntime.h"

using AutoWowTactics::Family;
using AutoWowTactics::TacticId;

namespace
{
std::uint32_t Pct(std::uint64_t cur, std::uint64_t max) { return max ? static_cast<std::uint32_t>(cur * 100 / max) : 0; }

// The wand is already auto-repeating on the current target: re-casting "shoot" would restart it.
bool WandShootingTarget(PlayerbotAI* botAI, Player* bot)
{
    Spell* autoRepeat = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    if (!autoRepeat)
        return false;
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    return target && autoRepeat->m_targets.GetUnitTarget() == target;
}
}  // namespace

bool ClassTacticTrigger::IsActive()
{
    AutoWowTactics::EngagementSnapshot snap;
    TacticId const id = AutoWowTactics::Current(bot, &snap);
    Family const family = AutoWowTactics::FamilyOf(id);
    if (id == TacticId::None || family == Family::Priest)  // priests run TacticalPriestStrategy
        return false;
    AutoWowTactics::ClassParams const& p = AutoWowTactics::Params(family);
    std::uint32_t const slot = AutoWowTactics::SlotOf(id);
    bool const single = slot == AutoWowTactics::kSlotSingle;
    bool const multi = slot == AutoWowTactics::kSlotMulti;
    bool const emergency = slot == AutoWowTactics::kSlotEmergency;
    bool const escape = slot == AutoWowTactics::kSlotEscape;
    bool const fighting = single || multi;
    bool const controlReady = (snap.cds & AutoWowTactics::kCdControl) != 0;
    std::uint32_t const hp = Pct(bot->GetHealth(), bot->GetMaxHealth());
    std::uint32_t const maxMana = bot->GetMaxPower(POWER_MANA);
    std::uint32_t const mana = Pct(bot->GetPower(POWER_MANA), maxMana);

    switch (condition)
    {
        case ClassTacticCondition::Single:
            return single;
        case ClassTacticCondition::Multi:
            return multi;
        case ClassTacticCondition::Emergency:
            return emergency;
        case ClassTacticCondition::Escape:
            return escape;
        case ClassTacticCondition::Heal:
            return fighting && hp < p.healHpPct + (multi ? 10 : 0);
        case ClassTacticCondition::Control:
            return multi && controlReady && snap.fearableMelee >= p.controlMinMelee &&
                   (hp < p.controlHpPct || snap.attackers >= 3) && !snap.addsNear;
        case ClassTacticCondition::ControlAdd:
            return multi && controlReady && snap.attackers >= 2 && !snap.addsNear;
        case ClassTacticCondition::EmergencyControl:
            return (emergency || escape) && controlReady && snap.fearableMelee >= 1;
        case ClassTacticCondition::MeleeOnMe:
            return fighting && snap.melee >= 1;
        case ClassTacticCondition::LowMana:
            return fighting && maxMana && mana < p.lowManaPct;
        case ClassTacticCondition::LifeTap:
            return fighting && maxMana && mana < p.lowManaPct && hp >= p.emergencyExitHpPct + 15;
        case ClassTacticCondition::PetLow:
        {
            Pet* pet = bot->GetPet();
            return fighting && pet && pet->IsAlive() && Pct(pet->GetHealth(), pet->GetMaxHealth()) < 40;
        }
        case ClassTacticCondition::Runner:
        case ClassTacticCondition::Kite:
        {
            if (!fighting)
                return false;
            Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (!target || !target->IsAlive())
                return false;
            if (condition == ClassTacticCondition::Kite)
                return (target->isFrozen() || target->HasRootAura()) && bot->IsWithinMeleeRange(target);
            return target->HasUnitState(UNIT_STATE_FLEEING) && !target->HasAuraType(SPELL_AURA_MOD_FEAR) &&
                   !target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
        }
        default:
            return false;
    }
}

float ClassTacticMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;
    TacticId const id = AutoWowTactics::Current(bot);
    if (id == TacticId::None || AutoWowTactics::FamilyOf(id) == Family::Priest)
        return 1.0f;
    std::string const name = action->getName();
    if (name == "shoot" && WandShootingTarget(botAI, bot))
        return 0.0f;
    return AutoWowTactics::FactorPermille(id, name) / 1000.0f;
}

// ---- per-family trigger tables (relevance: Strategy.h; emergency 90+) ------------------------------
namespace
{
// Warrior (60-64): rage pacing (Sunder off, Heroic Strike x0.5 so rage goes to Rend / Overpower / Victory
// Rush), Hamstring a fleeing target, Victory Rush below HealHpPct; multi: Thunder Clap + Demoralizing Shout +
// Cleave, Intimidating Shout on >= ControlMinMelee melee; emergency: Intimidating Shout on any melee, Victory
// Rush, Last Stand, Shield Wall, Retaliation, Shield Block (stance prerequisites are stock); escape: shout,
// Hamstring, flee (all damage x0).
void WarriorNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac runner", {NextAction("hamstring", ACTION_HIGH + 9)}));
    t.push_back(new TriggerNode("tac heal", {NextAction("victory rush", ACTION_INTERRUPT + 2)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("thunder clap", ACTION_HIGH + 6),
                                              NextAction("demoralizing shout", ACTION_HIGH + 5),
                                              NextAction("cleave", ACTION_HIGH + 2)}));
    t.push_back(new TriggerNode("tac control", {NextAction("intimidating shout", ACTION_INTERRUPT + 5)}));
    t.push_back(new TriggerNode("tac emergency control", {NextAction("intimidating shout", ACTION_EMERGENCY + 7)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("victory rush", ACTION_EMERGENCY + 6),
                                                  NextAction("last stand", ACTION_EMERGENCY + 5),
                                                  NextAction("shield wall", ACTION_EMERGENCY + 4),
                                                  NextAction("retaliation", ACTION_EMERGENCY + 3),
                                                  NextAction("shield block", ACTION_EMERGENCY + 2)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("hamstring", ACTION_EMERGENCY + 3),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
}  // namespace

void TacticalClassStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    switch (family)
    {
        case Family::Warrior:
            WarriorNodes(triggers);
            break;
        default:
            break;
    }
}

void TacticalClassStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ClassTacticMultiplier(botAI));
}
