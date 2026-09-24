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

    // Live facts for the pure condition (AutoWowTactics::ConditionHolds); unit reads only where needed.
    AutoWowTactics::TriggerFacts f;
    f.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    f.usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    f.manaPct = Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA));
    if (condition == ClassTacticCondition::PetLow)
    {
        Pet* pet = bot->GetPet();
        f.petLow = pet && pet->IsAlive() && Pct(pet->GetHealth(), pet->GetMaxHealth()) < 40;
    }
    if (condition == ClassTacticCondition::Runner || condition == ClassTacticCondition::Kite)
    {
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (target && target->IsAlive())
        {
            f.targetRootedInMelee = (target->isFrozen() || target->HasRootAura()) && bot->IsWithinMeleeRange(target);
            f.targetFleeing = target->HasUnitState(UNIT_STATE_FLEEING) && !target->HasAuraType(SPELL_AURA_MOD_FEAR) &&
                              !target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
        }
    }
    return AutoWowTactics::ConditionHolds(condition, id, snap, AutoWowTactics::Params(family), f);
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
// Rush, Last Stand. No Shield Wall / Shield Block / Retaliation: their stock stance prerequisites (defensive vs
// battle) flapped stances every GCD and dumped rage, and Shield Wall / Block need a shield. Escape: shout,
// Hamstring, flee (all damage x0; unreachable while the warrior has no escape tool).
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
                                                  NextAction("last stand", ACTION_EMERGENCY + 5)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("hamstring", ACTION_EMERGENCY + 3),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Rogue (50-54): stock stealth opener and combo-point logic; long-fight finishers off (Rupture, Expose Armor,
// Feint); Evasion below HealHpPct; multi: Blade Flurry (Evasion waits for HealHpPct instead of burning its 5 min
// cooldown at the pull; no Kidney Shot, which spent 1-point combo stacks meant for Eviscerate); emergency:
// Evasion, Kidney Shot; escape: Vanish, Gouge, Sprint, flee (all damage x0, so the Gouge is not broken).
void RogueNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac heal", {NextAction("evasion", ACTION_HIGH + 9)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("blade flurry", ACTION_HIGH + 8)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("evasion", ACTION_EMERGENCY + 6),
                                                  NextAction("kidney shot", ACTION_EMERGENCY + 5)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("vanish", ACTION_EMERGENCY + 6),
                                               NextAction("gouge", ACTION_EMERGENCY + 5),
                                               NextAction("sprint", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Mage (30-34): frost kite - Frost Nova on a melee attacker, then step out of a lone rooted/frozen target (flee,
// below the stock Blink-back; ConditionHolds Kite); multi: Frost Nova on >= ControlMinMelee melee, Polymorph the add, Ice Barrier,
// Cone of Cold; emergency: Frost Nova on any melee, Ice Barrier, Mana Shield, Ice Block (Evocation and long
// casts off); escape: Frost Nova, Blink back, flee (all damage x0; unreachable while the mage has no escape
// tool in kClassTables).
void MageNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac melee on me", {NextAction("frost nova", ACTION_INTERRUPT + 3)}));
    t.push_back(new TriggerNode("tac kite", {NextAction("flee", ACTION_MOVE + 4)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("ice barrier", ACTION_HIGH + 9),
                                              NextAction("cone of cold", ACTION_HIGH + 5)}));
    t.push_back(new TriggerNode("tac control", {NextAction("frost nova", ACTION_INTERRUPT + 5)}));
    t.push_back(new TriggerNode("tac control add", {NextAction("polymorph", ACTION_INTERRUPT + 4)}));
    t.push_back(new TriggerNode("tac emergency control", {NextAction("frost nova", ACTION_EMERGENCY + 7)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("ice barrier", ACTION_EMERGENCY + 5),
                                                  NextAction("mana shield", ACTION_EMERGENCY + 4),
                                                  NextAction("ice block", ACTION_EMERGENCY + 3)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("blink back", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Shaman (90-94): Searing Totem and Flame Shock on a single target (Magma Totem / Fire Nova off), Lesser
// Healing Wave / Healing Wave below HealHpPct for every spec (enhancement / elemental have no stock self-heal);
// multi: Stoneclaw Totem soaks, Flame Shock; emergency: Shamanistic Rage, LHW, HW, Stoneclaw (nukes off);
// escape: Earthbind Totem, Frost Shock, Ghost Wolf, flee.
void ShamanNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac single", {NextAction("searing totem", ACTION_NORMAL + 5),
                                               NextAction("flame shock", ACTION_HIGH + 1)}));
    t.push_back(new TriggerNode("tac heal", {NextAction("lesser healing wave", ACTION_CRITICAL_HEAL + 2),
                                             NextAction("healing wave", ACTION_CRITICAL_HEAL + 1)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("stoneclaw totem", ACTION_HIGH + 8),
                                              NextAction("flame shock", ACTION_HIGH + 2)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("shamanistic rage", ACTION_EMERGENCY + 6),
                                                  NextAction("lesser healing wave", ACTION_EMERGENCY + 5),
                                                  NextAction("healing wave", ACTION_EMERGENCY + 4),
                                                  NextAction("stoneclaw totem", ACTION_EMERGENCY + 3)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("earthbind totem", ACTION_EMERGENCY + 5),
                                               NextAction("frost shock", ACTION_EMERGENCY + 4),
                                               NextAction("ghost wolf", ACTION_EMERGENCY + 3),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Hunter (40-44): the pet tanks - Mend Pet below 40 %, Wing Clip + Disengage when a mob reaches the hunter;
// multi: Freezing Trap for the add, Multi-Shot; emergency: Deterrence, Feign Death (the pet keeps the mobs),
// Mend Pet; escape: Feign Death, Disengage, flee.
void HunterNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac pet low", {NextAction("mend pet", ACTION_HIGH + 7)}));
    t.push_back(new TriggerNode("tac melee on me", {NextAction("wing clip", ACTION_HIGH + 6),
                                                    NextAction("disengage", ACTION_HIGH + 5)}));
    t.push_back(new TriggerNode("tac control add", {NextAction("freezing trap", ACTION_INTERRUPT + 4)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("multi-shot", ACTION_HIGH + 2)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("deterrence", ACTION_EMERGENCY + 6),
                                                  NextAction("feign death", ACTION_EMERGENCY + 5),
                                                  NextAction("mend pet", ACTION_EMERGENCY + 2)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("feign death", ACTION_EMERGENCY + 5),
                                               NextAction("disengage", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Warlock (20-24): drain-tank behind the pet - Drain Life below HealHpPct (70), Life Tap only while hp is high
// (x0 in emergency / escape), Health Funnel when the pet is low; multi: Fear the add, Corruption spread;
// emergency: Howl of Terror on melee, Death Coil, healthstone, Drain Life; escape: Howl of Terror, Death Coil,
// flee.
void WarlockNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac heal", {NextAction("drain life", ACTION_HIGH + 6)}));
    t.push_back(new TriggerNode("tac life tap", {NextAction("life tap", ACTION_HIGH + 1)}));
    t.push_back(new TriggerNode("tac pet low", {NextAction("health funnel", ACTION_HIGH + 5)}));
    t.push_back(new TriggerNode("tac control add", {NextAction("fear on cc", ACTION_INTERRUPT + 3)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("corruption on attacker", ACTION_HIGH + 4)}));
    t.push_back(new TriggerNode("tac emergency control", {NextAction("howl of terror", ACTION_EMERGENCY + 7)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("death coil", ACTION_EMERGENCY + 6),
                                                  NextAction("healthstone", ACTION_EMERGENCY + 5),
                                                  NextAction("drain life", ACTION_EMERGENCY + 4)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("howl of terror", ACTION_EMERGENCY + 5),
                                               NextAction("death coil", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Paladin (70-74): Flash of Light / Holy Light below HealHpPct; multi: Consecration, Hammer of Justice on the
// target; emergency: Hammer of Justice on melee, Divine Shield, Lay on Hands, Divine Protection, Flash of
// Light, Holy Light (offence stays on; Consecration / Holy Wrath off); escape: Divine Shield, flee (unreachable
// while the paladin has no escape tool in kClassTables).
void PaladinNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac heal", {NextAction("flash of light", ACTION_CRITICAL_HEAL + 2),
                                             NextAction("holy light", ACTION_CRITICAL_HEAL + 1)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("consecration", ACTION_HIGH + 5)}));
    t.push_back(new TriggerNode("tac control add", {NextAction("hammer of justice", ACTION_INTERRUPT + 3)}));
    t.push_back(new TriggerNode("tac emergency control", {NextAction("hammer of justice", ACTION_EMERGENCY + 8)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("divine shield", ACTION_EMERGENCY + 7),
                                                  NextAction("lay on hands", ACTION_EMERGENCY + 6),
                                                  NextAction("divine protection", ACTION_EMERGENCY + 5),
                                                  NextAction("flash of light", ACTION_EMERGENCY + 4),
                                                  NextAction("holy light", ACTION_EMERGENCY + 3)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("divine shield", ACTION_EMERGENCY + 5),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Druid (80-84): form switching - Regrowth / Rejuvenation below HealHpPct (the stock caster-form prerequisite
// leaves the form); multi: Bear Form + Demoralizing Roar + Swipe, Entangling Roots on the add (caster nukes
// x0.3); emergency: Barkskin, Frenzied Regeneration, Regrowth, Rejuvenation, Healing Touch (offence off);
// escape: Entangling Roots on the target, Dash, flee.
void DruidNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac heal", {NextAction("regrowth", ACTION_CRITICAL_HEAL + 2),
                                             NextAction("rejuvenation", ACTION_CRITICAL_HEAL + 1)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("bear form", ACTION_HIGH + 9),
                                              NextAction("demoralizing roar", ACTION_HIGH + 5),
                                              NextAction("swipe (bear)", ACTION_HIGH + 4)}));
    t.push_back(new TriggerNode("tac control add", {NextAction("entangling roots on cc", ACTION_INTERRUPT + 3)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("barkskin", ACTION_EMERGENCY + 6),
                                                  NextAction("frenzied regeneration", ACTION_EMERGENCY + 5),
                                                  NextAction("regrowth", ACTION_EMERGENCY + 4),
                                                  NextAction("rejuvenation", ACTION_EMERGENCY + 3),
                                                  NextAction("healing touch", ACTION_EMERGENCY + 2)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("entangling roots", ACTION_EMERGENCY + 5),
                                               NextAction("dash", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
// Death Knight (100-104, level 55+): Death Strike below HealHpPct; multi: Pestilence, Blood Boil, Death and
// Decay; emergency: Icebound Fortitude, Death Strike, Anti-Magic Shell, Death Pact; escape: Chains of Ice, flee.
void DeathKnightNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("tac heal", {NextAction("death strike", ACTION_HIGH + 6)}));
    t.push_back(new TriggerNode("tac multi", {NextAction("pestilence", ACTION_HIGH + 5),
                                              NextAction("blood boil", ACTION_HIGH + 4),
                                              NextAction("death and decay", ACTION_HIGH + 3)}));
    t.push_back(new TriggerNode("tac emergency", {NextAction("icebound fortitude", ACTION_EMERGENCY + 6),
                                                  NextAction("death strike", ACTION_EMERGENCY + 5),
                                                  NextAction("anti magic shell", ACTION_EMERGENCY + 4),
                                                  NextAction("death pact", ACTION_EMERGENCY + 3)}));
    t.push_back(new TriggerNode("tac escape", {NextAction("chains of ice", ACTION_EMERGENCY + 4),
                                               NextAction("flee", ACTION_EMERGENCY + 2)}));
}
}  // namespace

void TacticalClassStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    switch (family)
    {
        case Family::DeathKnight:
            DeathKnightNodes(triggers);
            break;
        case Family::Druid:
            DruidNodes(triggers);
            break;
        case Family::Paladin:
            PaladinNodes(triggers);
            break;
        case Family::Warlock:
            WarlockNodes(triggers);
            break;
        case Family::Hunter:
            HunterNodes(triggers);
            break;
        case Family::Shaman:
            ShamanNodes(triggers);
            break;
        case Family::Mage:
            MageNodes(triggers);
            break;
        case Family::Rogue:
            RogueNodes(triggers);
            break;
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
