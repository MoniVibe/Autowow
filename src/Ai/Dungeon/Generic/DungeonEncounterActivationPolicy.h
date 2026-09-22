/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef _PLAYERBOT_DUNGEON_ENCOUNTER_ACTIVATION_POLICY_H
#define _PLAYERBOT_DUNGEON_ENCOUNTER_ACTIVATION_POLICY_H

namespace DungeonEncounterActivation
{
enum class Decision
{
    Attack,
    NotArrived,
    SpawnNotLoaded,
    SpawnMismatch,
    Dead,
    NotInWorld,
    NotHostile,
    NotTargetable,
    NoLineOfSight,
};

struct TargetFacts
{
    bool arrived = false;
    bool loaded = false;
    bool exactSpawn = false;
    bool alive = false;
    bool inWorld = false;
    bool hostile = false;
    bool targetable = false;
    bool lineOfSight = false;
};

inline Decision Evaluate(TargetFacts const& facts)
{
    if (!facts.arrived)
        return Decision::NotArrived;
    if (!facts.loaded)
        return Decision::SpawnNotLoaded;
    if (!facts.exactSpawn)
        return Decision::SpawnMismatch;
    if (!facts.alive)
        return Decision::Dead;
    if (!facts.inWorld)
        return Decision::NotInWorld;
    if (!facts.hostile)
        return Decision::NotHostile;
    if (!facts.targetable)
        return Decision::NotTargetable;
    if (!facts.lineOfSight)
        return Decision::NoLineOfSight;
    return Decision::Attack;
}
}

#endif
