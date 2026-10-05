/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ClassFixesP1Policy.h"

#include "gtest/gtest.h"

using namespace AutoWowClassFixesP1;

namespace
{
Facts Fighting(std::uint32_t attackers, std::uint32_t hp = 100)
{
    Facts f;
    f.solo = true;
    f.inCombat = true;
    f.attackers = attackers;
    f.hpPct = hp;
    return f;
}
Params const kP;
}  // namespace

TEST(ClassFixesP1Policy, GroupedOrOutOfCombatRunsStock)
{
    Facts f = Fighting(3, 20);
    f.solo = false;
    for (std::uint8_t c = 0; c <= static_cast<std::uint8_t>(Cond::WaterShield); ++c)
        EXPECT_FALSE(Holds(static_cast<Cond>(c), f, kP)) << int(c);
    for (std::uint8_t g = 0; g <= static_cast<std::uint8_t>(Gate::AoeDamage); ++g)
        EXPECT_TRUE(Allowed(static_cast<Gate>(g), f, kP)) << int(g);

    Facts idle = Fighting(0, 20);
    idle.inCombat = false;
    EXPECT_FALSE(Holds(Cond::BearHold, idle, kP));  // a hurt druid out of combat does not go bear
    EXPECT_FALSE(Holds(Cond::AddsOrHurt, idle, kP));
    EXPECT_TRUE(Allowed(Gate::CatForm, idle, kP));
    EXPECT_TRUE(Allowed(Gate::HealCast, idle, kP));  // out-of-combat heals unchanged
}

TEST(ClassFixesP1Policy, AddThresholds)
{
    EXPECT_FALSE(Holds(Cond::Adds, Fighting(1), kP));
    EXPECT_TRUE(Holds(Cond::Adds, Fighting(2), kP));
    EXPECT_FALSE(Holds(Cond::ManyAdds, Fighting(2), kP));
    EXPECT_TRUE(Holds(Cond::ManyAdds, Fighting(3), kP));
    // Rogue Evasion: adds or hurt.
    EXPECT_FALSE(Holds(Cond::AddsOrHurt, Fighting(1, 50), kP));
    EXPECT_TRUE(Holds(Cond::AddsOrHurt, Fighting(1, 49), kP));
    EXPECT_TRUE(Holds(Cond::AddsOrHurt, Fighting(2, 100), kP));
    // Intimidating Shout: 3+, or 2 when hurt.
    EXPECT_FALSE(Holds(Cond::WarriorControl, Fighting(2, 80), kP));
    EXPECT_TRUE(Holds(Cond::WarriorControl, Fighting(2, 40), kP));
    EXPECT_TRUE(Holds(Cond::WarriorControl, Fighting(3, 100), kP));
}

TEST(ClassFixesP1Policy, RetaliationNeedsBattleStance)
{
    Facts f = Fighting(3);
    EXPECT_FALSE(Holds(Cond::Retaliation, f, kP));  // berserker stance: no stance dance
    f.battleStance = true;
    EXPECT_TRUE(Holds(Cond::Retaliation, f, kP));
    f.attackers = 2;
    EXPECT_FALSE(Holds(Cond::Retaliation, f, kP));
    f.hpPct = 59;
    EXPECT_TRUE(Holds(Cond::Retaliation, f, kP));
}

TEST(ClassFixesP1Policy, RageAndKidney)
{
    Facts f = Fighting(1);
    f.rage = 9;
    EXPECT_TRUE(Holds(Cond::RageStarved, f, kP));
    f.rage = 10;
    EXPECT_FALSE(Holds(Cond::RageStarved, f, kP));

    f.targetCaster = true;
    f.comboPoints = 2;
    EXPECT_FALSE(Holds(Cond::CasterKidney, f, kP));
    f.comboPoints = 3;
    EXPECT_TRUE(Holds(Cond::CasterKidney, f, kP));
    f.targetCaster = false;
    EXPECT_FALSE(Holds(Cond::CasterKidney, f, kP));
}

// Enhancement: the S116 mana sinks are gated solo, the shocks only above their mana lines, cast-time heals only
// when instant (Maelstrom x5) or in an emergency.
TEST(ClassFixesP1Policy, ShamanManaGates)
{
    Facts f = Fighting(1, 80);
    EXPECT_FALSE(Allowed(Gate::ManaTotem, f, kP));
    EXPECT_FALSE(Allowed(Gate::FireNova, f, kP));
    EXPECT_TRUE(Allowed(Gate::LightningShield, f, kP));  // Water Shield not learned yet (< 20)
    f.knowsWaterShield = true;
    EXPECT_FALSE(Allowed(Gate::LightningShield, f, kP));

    f.manaPct = 50;
    EXPECT_TRUE(Allowed(Gate::EarthShock, f, kP));
    f.manaPct = 49;
    EXPECT_FALSE(Allowed(Gate::EarthShock, f, kP));
    EXPECT_TRUE(Allowed(Gate::FlameShock, f, kP));
    f.manaPct = 29;
    EXPECT_FALSE(Allowed(Gate::FlameShock, f, kP));

    f.hpPct = 45;
    f.maelstrom = 4;
    EXPECT_FALSE(Allowed(Gate::HealCast, f, kP));  // the tactical "tac heal" LHW cast is held
    f.maelstrom = 5;
    EXPECT_TRUE(Allowed(Gate::HealCast, f, kP));
    EXPECT_TRUE(Holds(Cond::MaelstromHeal, f, kP));
    f.hpPct = 55;
    EXPECT_FALSE(Holds(Cond::MaelstromHeal, f, kP));
    f.maelstrom = 0;
    f.hpPct = 29;
    EXPECT_TRUE(Allowed(Gate::HealCast, f, kP));  // emergency
}

TEST(ClassFixesP1Policy, WaterShieldInAndOutOfCombat)
{
    Facts f;
    f.solo = true;
    f.knowsWaterShield = true;
    EXPECT_TRUE(Holds(Cond::WaterShield, f, kP));
    f.inCombat = true;
    EXPECT_TRUE(Holds(Cond::WaterShield, f, kP));
    f.waterShieldUp = true;
    EXPECT_FALSE(Holds(Cond::WaterShield, f, kP));
    f.waterShieldUp = false;
    f.knowsWaterShield = false;
    EXPECT_FALSE(Holds(Cond::WaterShield, f, kP));
}

// Bear hold has hysteresis on hp (enter 45, leave 60) and blocks Cat Form while it holds, so the stock "cat
// form" trigger (28) can no longer undo the bear swap (29) every tick.
TEST(ClassFixesP1Policy, BearHoldHysteresisAndCatGate)
{
    Facts f = Fighting(1, 50);
    EXPECT_FALSE(Holds(Cond::BearHold, f, kP));
    EXPECT_TRUE(Allowed(Gate::CatForm, f, kP));
    f.hpPct = 44;
    EXPECT_TRUE(Holds(Cond::BearHold, f, kP));
    EXPECT_FALSE(Allowed(Gate::CatForm, f, kP));
    f.inBear = true;
    f.hpPct = 55;
    EXPECT_TRUE(Holds(Cond::BearHold, f, kP));  // still below the exit line
    EXPECT_FALSE(Allowed(Gate::CatForm, f, kP));
    f.hpPct = 60;
    EXPECT_FALSE(Holds(Cond::BearHold, f, kP));
    EXPECT_TRUE(Allowed(Gate::CatForm, f, kP));
    f.attackers = 2;
    EXPECT_TRUE(Holds(Cond::BearHold, f, kP));
    EXPECT_FALSE(Allowed(Gate::CatForm, f, kP));
    f.escaping = true;  // tactical escape shifts to cat for Dash
    EXPECT_TRUE(Allowed(Gate::CatForm, f, kP));

    Facts m = Fighting(1, 90);
    m.tacticMulti = true;  // tactical multi (one elite) casts Bear Form too
    EXPECT_FALSE(Allowed(Gate::CatForm, m, kP));

    Facts h = Fighting(1, 40);
    EXPECT_FALSE(Holds(Cond::BearHurt, h, kP));  // Frenzied Regeneration needs bear
    h.inBear = true;
    EXPECT_TRUE(Holds(Cond::BearHurt, h, kP));
}

TEST(ClassFixesP1Policy, AoeOffWhileAnAddIsInCc)
{
    Facts f = Fighting(2);
    EXPECT_TRUE(Holds(Cond::AoeOk, f, kP));
    EXPECT_TRUE(Allowed(Gate::AoeDamage, f, kP));
    f.cced = true;
    EXPECT_FALSE(Holds(Cond::AoeOk, f, kP));
    EXPECT_FALSE(Allowed(Gate::AoeDamage, f, kP));
    EXPECT_FALSE(Holds(Cond::AoeOk, Fighting(1), kP));
}

TEST(ClassFixesP1Policy, PickAddHealthiestNonTargetNotCced)
{
    EXPECT_EQ(PickAdd({}), -1);
    std::vector<AddCandidate> c = {{10, 100, true, false}, {12, 80, false, false}, {11, 80, false, false},
                                   {9, 100, false, true}};
    EXPECT_EQ(PickAdd(c), 2);  // 80% tie -> lowest guid 11; target and cc'd add skipped
    c.push_back({20, 95, false, false});
    EXPECT_EQ(PickAdd(c), 4);
    EXPECT_EQ(PickAdd({{1, 100, true, false}, {2, 100, false, true}}), -1);
}

TEST(ClassFixesP1Policy, CoveredClassesAndTabs)
{
    EXPECT_TRUE(Covers(1, 0));    // Arms
    EXPECT_TRUE(Covers(1, 1));    // Fury
    EXPECT_FALSE(Covers(1, 2));   // Protection
    for (std::uint32_t tab = 0; tab < 3; ++tab)
        EXPECT_TRUE(Covers(4, tab));  // rogue
    EXPECT_TRUE(Covers(7, 1));    // Enhancement
    EXPECT_FALSE(Covers(7, 0));
    EXPECT_FALSE(Covers(7, 2));
    EXPECT_TRUE(Covers(11, 1));   // Feral
    EXPECT_FALSE(Covers(11, 0));  // Balance
    for (std::uint32_t cls : {2u, 3u, 5u, 6u, 8u, 9u, 0u, 10u})
        EXPECT_FALSE(Covers(cls, 1)) << cls;
}
