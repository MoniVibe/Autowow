/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassFixesP1Strategy.h"

#include <string_view>

#include "Creature.h"
#include "Map.h"
#include "Playerbots.h"
#include "TacticalRuntime.h"

using AutoWowClassFixesP1::Facts;
using AutoWowClassFixesP1::Gate;

namespace
{
std::uint32_t Pct(std::uint64_t cur, std::uint64_t max) { return max ? static_cast<std::uint32_t>(cur * 100 / max) : 0; }

bool CasterMob(Unit* unit)
{
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    CreatureTemplate const* t = creature ? creature->GetCreatureTemplate() : nullptr;
    return t && t->unit_class == CLASS_MAGE;  // same rule as the tactical snapshot (TacticalRuntime.cpp FactsOf)
}

// Live facts of the bot. Attackers come from the "attackers" value (the tactical snapshot's source), so a
// blinded / feared add still counts and still marks `cced`.
Facts FactsOf(PlayerbotAI* botAI, Player* bot)
{
    Facts f;
    Map* map = bot->GetMap();
    f.solo = !bot->GetGroup() && map && !map->Instanceable();
    f.inCombat = bot->IsInCombat();
    f.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    f.manaPct = Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA));
    f.rage = bot->GetPower(POWER_RAGE) / 10;
    f.comboPoints = bot->GetComboPoints();
    if (Aura* maelstrom = botAI->GetAura("maelstrom weapon", bot))
        f.maelstrom = maelstrom->GetStackAmount();
    f.inBear = botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr);
    f.battleStance = botAI->HasAura("battle stance", bot);
    if (bot->getClass() == CLASS_SHAMAN)
    {
        f.knowsWaterShield = botAI->HasSpell("water shield");
        f.waterShieldUp = botAI->HasAura("water shield", bot);
    }
    AiObjectContext* context = botAI->GetAiObjectContext();
    for (ObjectGuid const guid : context->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;
        ++f.attackers;
        if (unit->HasBreakableByDamageCrowdControlAura())
            f.cced = true;
    }
    f.targetCaster = CasterMob(context->GetValue<Unit*>("current target")->Get());
    return f;
}

// Action name -> gate. The solo Enhancement totem set itself (Searing + Healing Stream instead of Strength of
// Earth + Magma + Healing Stream + Windfury) is chosen in AiFactory; Magma and Call of the Elements (which
// drops every bar totem at once, each pull) are gated here. ponytail: linear scan of ~20 names per lookup; a
// map if profiles show it.
constexpr std::pair<std::string_view, Gate> kGates[] = {
    {"lightning shield", Gate::LightningShield},
    {"magma totem", Gate::ManaTotem},
    {"call of the elements", Gate::ManaTotem},
    {"fire nova", Gate::FireNova},
    {"earth shock", Gate::EarthShock},
    {"flame shock", Gate::FlameShock},
    {"healing wave", Gate::HealCast},
    {"lesser healing wave", Gate::HealCast},
    {"chain heal", Gate::HealCast},
    {"cat form", Gate::CatForm},
    {"cleave", Gate::AoeDamage},
    {"whirlwind", Gate::AoeDamage},
    {"sweeping strikes", Gate::AoeDamage},
    {"thunder clap", Gate::AoeDamage},
    {"bladestorm", Gate::AoeDamage},
    {"blade flurry", Gate::AoeDamage},
    {"fan of knives", Gate::AoeDamage},
    {"swipe (bear)", Gate::AoeDamage},
    {"swipe (cat)", Gate::AoeDamage},
};

Gate GateOf(std::string_view name)
{
    for (auto const& [n, g] : kGates)
        if (n == name)
            return g;
    return Gate::None;
}

bool ShamanGate(Gate g)
{
    return g == Gate::LightningShield || g == Gate::ManaTotem || g == Gate::FireNova || g == Gate::EarthShock ||
           g == Gate::FlameShock || g == Gate::HealCast;
}
}  // namespace

bool ClassFixP1Trigger::IsActive()
{
    if (!AutoWowTactics::ClassFixesP1())
        return false;
    return AutoWowClassFixesP1::Holds(condition, FactsOf(botAI, bot), AutoWowTactics::ClassFixesP1Params());
}

float ClassFixP1Multiplier::GetValue(Action* action)
{
    if (!action || !AutoWowTactics::ClassFixesP1())
        return 1.0f;
    Gate const gate = GateOf(action->getName());
    if (gate == Gate::None)
        return 1.0f;
    std::uint8_t const cls = bot->getClass();
    if (ShamanGate(gate) != (cls == CLASS_SHAMAN))
        return 1.0f;
    if (gate == Gate::CatForm && cls != CLASS_DRUID)
        return 1.0f;
    Facts f = FactsOf(botAI, bot);
    if (gate == Gate::CatForm)
    {
        AutoWowTactics::TacticId const t = AutoWowTactics::Current(bot);
        bool const running = t != AutoWowTactics::TacticId::None;
        f.escaping = running && AutoWowTactics::SlotOf(t) == AutoWowTactics::kSlotEscape;
        f.tacticMulti = running && AutoWowTactics::SlotOf(t) == AutoWowTactics::kSlotMulti;
    }
    return AutoWowClassFixesP1::Allowed(gate, f, AutoWowTactics::ClassFixesP1Params()) ? 1.0f : 0.0f;
}

Unit* CastOnAddP1Action::GetTarget()
{
    AiObjectContext* ctx = botAI->GetAiObjectContext();
    Unit* current = ctx->GetValue<Unit*>("current target")->Get();
    std::vector<Unit*> units;
    std::vector<AutoWowClassFixesP1::AddCandidate> candidates;
    for (ObjectGuid const guid : ctx->GetValue<GuidVector>("attackers")->Get())
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->GetMapId() != bot->GetMapId())
            continue;
        AutoWowClassFixesP1::AddCandidate c;
        c.guid = guid.GetCounter();
        c.hpPct = static_cast<std::uint32_t>(unit->GetHealthPct());
        c.currentTarget = unit == current;
        c.cced = unit->HasBreakableByDamageCrowdControlAura();
        units.push_back(unit);
        candidates.push_back(c);
    }
    std::int32_t const pick = AutoWowClassFixesP1::PickAdd(candidates);
    return pick < 0 ? nullptr : units[pick];
}

// ---- per-class trigger tables (relevance: Strategy.h) -------------------------------------------------
namespace
{
// Warrior Arms / Fury: Bloodrage when rage-starved (the pull: no rage, no Charge in range -> white swings only);
// 2+ attackers: Sweeping Strikes, Thunder Clap, Demoralizing Shout, Cleave (AoeOk: off while an add is feared);
// Intimidating Shout on 3+ attackers or 2+ below HurtHpPct; Retaliation in battle stance (no stance dance:
// the stock prerequisites flapped stances and dumped rage, see TacticalClassStrategy.cpp WarriorNodes).
void WarriorNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("fix p1 rage starved", {NextAction("bloodrage", ACTION_HIGH + 9)}));
    t.push_back(new TriggerNode("fix p1 aoe ok", {NextAction("sweeping strikes", ACTION_HIGH + 9),
                                                  NextAction("thunder clap", ACTION_HIGH + 6),
                                                  NextAction("demoralizing shout", ACTION_HIGH + 5),
                                                  NextAction("cleave", ACTION_HIGH + 2)}));
    t.push_back(new TriggerNode("fix p1 warrior control", {NextAction("intimidating shout", ACTION_INTERRUPT + 5)}));
    t.push_back(new TriggerNode("fix p1 retaliation", {NextAction("retaliation", ACTION_EMERGENCY + 1)}));
}
// Rogue: Evasion at 2+ attackers or below HurtHpPct; Adrenaline Rush at 2+; Blade Flurry at 2+ unless an add is in
// cc; 3+ attackers: Blind (else Gouge) the healthiest add that is not the target; Kidney Shot a caster target.
void RogueNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("fix p1 adds or hurt", {NextAction("evasion", ACTION_HIGH + 9)}));
    t.push_back(new TriggerNode("fix p1 adds", {NextAction("adrenaline rush", ACTION_HIGH + 8)}));
    t.push_back(new TriggerNode("fix p1 aoe ok", {NextAction("blade flurry", ACTION_HIGH + 7)}));
    t.push_back(new TriggerNode("fix p1 many adds", {NextAction("blind on add", ACTION_INTERRUPT + 4),
                                                     NextAction("gouge on add", ACTION_INTERRUPT + 3)}));
    t.push_back(new TriggerNode("fix p1 caster kidney", {NextAction("kidney shot", ACTION_HIGH + 6)}));
}
// Enhancement shaman, mana: Water Shield (Lightning Shield gated off once it is known), Searing Totem as the fire
// totem (AiFactory; Magma / Fire Nova / Call of the Elements gated off), Earth / Flame Shock only above their
// mana lines, cast-time heals only instant: Healing Wave on Maelstrom x5 below MaelstromHealHpPct (tactical
// "tac heal" Lesser Healing Wave gated to Maelstrom x5 / EmergencyHpPct). 2+ attackers: Shamanistic Rage (30%
// less damage taken + mana), Feral Spirit.
void ShamanNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("fix p1 water shield", {NextAction("water shield", 20.1f)}));  // LS: nc 20, combat 18.5
    t.push_back(new TriggerNode("fix p1 maelstrom heal", {NextAction("healing wave", ACTION_CRITICAL_HEAL + 5)}));
    t.push_back(new TriggerNode("fix p1 adds", {NextAction("shamanistic rage", ACTION_HIGH + 9),
                                                NextAction("feral spirit", ACTION_HIGH + 8)}));
}
// Feral cat druid: Bear Form at 2+ attackers or below BearEnterHpPct, held until 1 attacker and BearExitHpPct
// (Cat Form gated off meanwhile: the stock "cat form" trigger at 28 and the tactical "bear form" at 29 flapped
// forms every tick); in bear: Demoralizing Roar, Swipe (AoeOk), Mangle, Maul, Frenzied Regeneration when hurt.
void DruidNodes(std::vector<TriggerNode*>& t)
{
    t.push_back(new TriggerNode("fix p1 bear hold", {NextAction("dire bear form", ACTION_HIGH + 9),
                                                     NextAction("demoralizing roar", ACTION_HIGH + 5),
                                                     NextAction("mangle (bear)", ACTION_HIGH + 3),
                                                     NextAction("maul", ACTION_HIGH + 1)}));
    t.push_back(new TriggerNode("fix p1 aoe ok", {NextAction("swipe (bear)", ACTION_HIGH + 4)}));
    t.push_back(new TriggerNode("fix p1 bear hurt", {NextAction("frenzied regeneration", ACTION_HIGH + 8)}));
}
}  // namespace

void ClassFixesP1Strategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            WarriorNodes(triggers);
            break;
        case CLASS_ROGUE:
            RogueNodes(triggers);
            break;
        case CLASS_SHAMAN:
            ShamanNodes(triggers);
            break;
        case CLASS_DRUID:
            DruidNodes(triggers);
            break;
        default:
            break;
    }
}

void ClassFixesP1Strategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ClassFixP1Multiplier(botAI));
}
