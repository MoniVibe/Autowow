/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalPriestStrategy.h"

#include "Playerbots.h"
#include "Spell.h"
#include "TacticalRuntime.h"

using AutoWowTactics::TacticId;

namespace
{
std::uint32_t HpPct(Player* bot)
{
    return bot->GetMaxHealth() ? static_cast<std::uint32_t>(std::uint64_t(bot->GetHealth()) * 100 / bot->GetMaxHealth())
                               : 0;
}

bool SingleOrMulti(TacticId id)
{
    return id == TacticId::PriestWand || id == TacticId::PriestBurst || id == TacticId::PriestMulti;
}

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

bool TacticTrigger::IsActive()
{
    if (condition == TacticCondition::RestMana)
        return AutoWowTactics::NeedsRestMana(botAI);
    if (condition == TacticCondition::RestHealth)
        return AutoWowTactics::NeedsRestHealth(botAI);

    AutoWowTactics::EngagementSnapshot snap;
    TacticId const id = AutoWowTactics::Current(bot, &snap);
    if (id == TacticId::None)
        return false;
    AutoWowTactics::PriestParams const& p = AutoWowTactics::Priest();
    std::uint32_t const bonus = id == TacticId::PriestMulti ? p.multiHealBonusPct : 0;
    bool const screamReady = (snap.cds & AutoWowTactics::kCdScream) != 0;

    switch (condition)
    {
        case TacticCondition::Wand:
            return id == TacticId::PriestWand && (snap.cds & AutoWowTactics::kCdWand) && !WandShootingTarget(botAI, bot);
        case TacticCondition::Renew:
            return SingleOrMulti(id) && HpPct(bot) < p.renewHpPct + bonus && !botAI->HasAura("renew", bot);
        case TacticCondition::Heal:
            return SingleOrMulti(id) && HpPct(bot) < p.healHpPct + bonus;
        case TacticCondition::Multi:
            return id == TacticId::PriestMulti;
        case TacticCondition::Scream:
            return id == TacticId::PriestMulti && screamReady && snap.fearableMelee >= p.screamMinMelee &&
                   (HpPct(bot) < p.screamHpPct || snap.attackers >= 3) && !snap.addsNear;
        case TacticCondition::EmergencyScream:
            return (id == TacticId::PriestEmergency || id == TacticId::PriestEscape) && screamReady &&
                   snap.fearableMelee >= 1;
        case TacticCondition::Emergency:
            return id == TacticId::PriestEmergency;
        case TacticCondition::Escape:
            return id == TacticId::PriestEscape;
        default:
            return false;
    }
}

float TacticMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;
    AutoWowTactics::EngagementSnapshot snap;
    TacticId const id = AutoWowTactics::Current(bot, &snap);
    if (id == TacticId::None)
        return 1.0f;
    std::string const name = action->getName();
    if (name == "shoot" && WandShootingTarget(botAI, bot))
        return 0.0f;
    if (id == TacticId::PriestWand && name == "power word: shield" &&
        snap.manaPct < AutoWowTactics::Priest().shieldMinManaPct)
        return 0.0f;
    return AutoWowTactics::FactorPermille(id, name) / 1000.0f;
}

void TacticalPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Wand-cycle: shield (stock "being attacked", mana-gated by the multiplier) -> SW:P once -> wand to the end.
    triggers.push_back(new TriggerNode("tactic wand", {NextAction("shadow word: pain", ACTION_NORMAL + 3),
                                                       NextAction("shoot", ACTION_NORMAL + 2)}));
    triggers.push_back(new TriggerNode("tactic renew", {NextAction("renew", ACTION_MEDIUM_HEAL + 2)}));
    triggers.push_back(new TriggerNode("tactic heal", {NextAction("heal", ACTION_CRITICAL_HEAL + 1)}));
    triggers.push_back(
        new TriggerNode("tactic multi", {NextAction("shadow word: pain on attacker", ACTION_NORMAL + 6)}));
    triggers.push_back(new TriggerNode("tactic scream", {NextAction("psychic scream", ACTION_INTERRUPT + 5)}));
    triggers.push_back(new TriggerNode("tactic emergency scream", {NextAction("psychic scream", ACTION_EMERGENCY + 4)}));
    triggers.push_back(new TriggerNode("tactic emergency", {NextAction("desperate prayer", ACTION_EMERGENCY + 5),
                                                            NextAction("power word: shield", ACTION_EMERGENCY + 3),
                                                            NextAction("flash heal", ACTION_EMERGENCY + 2),
                                                            NextAction("renew", ACTION_EMERGENCY + 1)}));
    // ponytail: escape = scream (above) + stock flee step; the pull-origin retreat is plan lane T4.
    triggers.push_back(new TriggerNode("tactic escape", {NextAction("flee", ACTION_EMERGENCY + 2)}));
}

void TacticalPriestStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticMultiplier(botAI));
}

void TacticalPriestNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Pre-pull readiness (plan 3.2): drink / eat up to the pull thresholds instead of re-pulling at ~16 %.
    triggers.push_back(new TriggerNode("tactic rest mana", {NextAction("drink", 4.1f)}));
    triggers.push_back(new TriggerNode("tactic rest health", {NextAction("food", 4.1f)}));
}
