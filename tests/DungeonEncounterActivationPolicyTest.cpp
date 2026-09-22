/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "DungeonEncounterActivationPolicy.h"

#include "gtest/gtest.h"

using DungeonEncounterActivation::Decision;
using DungeonEncounterActivation::TargetFacts;

namespace
{
TargetFacts AttackableTarget()
{
    return {true, true, true, true, true, true, true, true};
}
}

TEST(DungeonEncounterActivationPolicy, RequiresArrivalBeforeExplicitAttack)
{
    TargetFacts facts = AttackableTarget();
    facts.arrived = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::NotArrived);
}

TEST(DungeonEncounterActivationPolicy, RequiresExactLoadedLiveSpawn)
{
    TargetFacts facts = AttackableTarget();
    facts.loaded = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::SpawnNotLoaded);

    facts = AttackableTarget();
    facts.exactSpawn = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::SpawnMismatch);

    facts = AttackableTarget();
    facts.alive = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::Dead);

    facts = AttackableTarget();
    facts.inWorld = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::NotInWorld);
}

TEST(DungeonEncounterActivationPolicy, RequiresHostileTargetableLineOfSightTarget)
{
    TargetFacts facts = AttackableTarget();
    facts.hostile = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::NotHostile);

    facts = AttackableTarget();
    facts.targetable = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::NotTargetable);

    facts = AttackableTarget();
    facts.lineOfSight = false;
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(facts), Decision::NoLineOfSight);
}

TEST(DungeonEncounterActivationPolicy, AcceptsOnlyFullyValidatedExactTarget)
{
    EXPECT_EQ(DungeonEncounterActivation::Evaluate(AttackableTarget()), Decision::Attack);
}
