/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_RESURRECTIONTARGETPOLICY_H
#define PLAYERBOTS_RESURRECTIONTARGETPOLICY_H

#include <cstdint>

class Corpse;
class Player;
class SpellInfo;
class WorldObject;

namespace ResurrectionTargetPolicy
{
enum class RolePriority : std::uint8_t
{
    Master,
    Healer,
    Tank,
    Other
};

enum class TargetKind : std::uint8_t
{
    Invalid,
    UnreleasedUnit,
    ReleasedCorpse
};

enum class CorpseValidation : std::uint8_t
{
    NotRequired,
    Valid,
    Missing,
    Bones,
    WrongOwner,
    WrongMap,
    NotInWorld
};

enum class Failure : std::uint8_t
{
    None,
    MissingTarget,
    Offline,
    Alive,
    TransitionalDeathState,
    PendingRequest,
    MissingCorpse,
    Bones,
    WrongOwner,
    WrongMap,
    CorpseNotInWorld
};

struct ResolvedTarget
{
    TargetKind kind = TargetKind::Invalid;
    Failure failure = Failure::MissingTarget;
    Player* player = nullptr;
    Corpse* corpse = nullptr;
};

inline RolePriority ClassifyRole(bool isMaster, bool isHealer, bool isTank)
{
    if (isMaster)
        return RolePriority::Master;
    if (isHealer)
        return RolePriority::Healer;
    if (isTank)
        return RolePriority::Tank;
    return RolePriority::Other;
}

inline bool HasHigherPriority(RolePriority candidate, RolePriority current)
{
    return candidate < current;
}

inline TargetKind ClassifyTarget(bool online, bool resurrectableDeathState, bool pendingRequest,
                                 bool released, bool unitOnHealerMap,
                                 CorpseValidation corpseValidation)
{
    if (!online || !resurrectableDeathState || pendingRequest)
        return TargetKind::Invalid;

    if (released)
    {
        return corpseValidation == CorpseValidation::Valid ? TargetKind::ReleasedCorpse
                                                            : TargetKind::Invalid;
    }

    return unitOnHealerMap ? TargetKind::UnreleasedUnit : TargetKind::Invalid;
}

inline bool CanCastInCombat(bool healerInCombat, bool combatResurrection)
{
    // WoW's restriction follows the caster's own combat state.  An out-of-combat healer may use
    // an ordinary resurrection even while another group member is fighting; an in-combat healer
    // requires a combat-resurrection spell.
    return !healerInCombat || combatResurrection;
}

inline bool IsGeometryReady(bool withinCastRange, bool hasLineOfSight)
{
    return withinCastRange && hasLineOfSight;
}

inline bool NeedsMovement(bool withinCastRange, bool hasLineOfSight)
{
    return !IsGeometryReady(withinCastRange, hasLineOfSight);
}

inline float SearchRadius(float castDistance, bool instancedDungeon)
{
    if (castDistance <= 0.0f)
        return 0.0f;

    // A dead tank can release while the back line is more than two spell ranges away.  Instances
    // need enough bounded look-ahead for the healer to select that real corpse and navigate to it;
    // outdoor parties retain the conservative historical radius.
    return castDistance * (instancedDungeon ? 5.0f : 2.0f);
}

inline char const* ToString(TargetKind kind)
{
    switch (kind)
    {
        case TargetKind::UnreleasedUnit:
            return "unit";
        case TargetKind::ReleasedCorpse:
            return "corpse";
        default:
            return "invalid";
    }
}

inline char const* ToString(Failure failure)
{
    switch (failure)
    {
        case Failure::None:
            return "none";
        case Failure::MissingTarget:
            return "missing_target";
        case Failure::Offline:
            return "offline";
        case Failure::Alive:
            return "alive";
        case Failure::TransitionalDeathState:
            return "transitional_death_state";
        case Failure::PendingRequest:
            return "pending_request";
        case Failure::MissingCorpse:
            return "missing_corpse";
        case Failure::Bones:
            return "bones";
        case Failure::WrongOwner:
            return "wrong_owner";
        case Failure::WrongMap:
            return "wrong_map";
        case Failure::CorpseNotInWorld:
            return "corpse_not_in_world";
        default:
            return "unknown";
    }
}

ResolvedTarget Resolve(Player* healer, Player* dead);
WorldObject* GetAnchor(ResolvedTarget const& resolved);
bool HasResurrectionEffect(SpellInfo const* spellInfo);
bool HasResurrectionReservation(Player* healer, Player* dead, Corpse* corpse);
}

#endif
