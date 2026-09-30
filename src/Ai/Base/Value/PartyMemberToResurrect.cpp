/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToResurrect.h"
#include <array>

#include "Corpse.h"
#include "Group.h"
#include "Map.h"
#include "Playerbots.h"
#include "ResurrectionTargetPolicy.h"
#include "Spell.h"

namespace ResurrectionTargetPolicy
{
namespace
{
Failure FailureFor(CorpseValidation validation)
{
    switch (validation)
    {
        case CorpseValidation::Missing:
            return Failure::MissingCorpse;
        case CorpseValidation::Bones:
            return Failure::Bones;
        case CorpseValidation::WrongOwner:
            return Failure::WrongOwner;
        case CorpseValidation::WrongMap:
            return Failure::WrongMap;
        case CorpseValidation::NotInWorld:
            return Failure::CorpseNotInWorld;
        default:
            return Failure::None;
    }
}

CorpseValidation ValidateCorpse(Player* healer, Player* dead, Corpse* corpse)
{
    if (!corpse)
        return CorpseValidation::Missing;
    if (corpse->GetType() == CORPSE_BONES)
        return CorpseValidation::Bones;
    if (corpse->GetOwnerGUID() != dead->GetGUID())
        return CorpseValidation::WrongOwner;
    if (!corpse->IsInWorld())
        return CorpseValidation::NotInWorld;
    if (!healer->GetMap() || corpse->GetMap() != healer->GetMap())
        return CorpseValidation::WrongMap;

    return CorpseValidation::Valid;
}
}

bool HasResurrectionEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (spellInfo->Effects[i].Effect == SPELL_EFFECT_RESURRECT ||
            spellInfo->Effects[i].Effect == SPELL_EFFECT_RESURRECT_NEW ||
            spellInfo->Effects[i].Effect == SPELL_EFFECT_SELF_RESURRECT)
        {
            return true;
        }
    }

    return false;
}

ResolvedTarget Resolve(Player* healer, Player* dead)
{
    ResolvedTarget resolved;
    resolved.player = dead;

    if (!healer || !dead)
        return resolved;
    if (!dead->IsInWorld())
    {
        resolved.failure = Failure::Offline;
        return resolved;
    }
    if (dead->IsAlive())
    {
        resolved.failure = Failure::Alive;
        return resolved;
    }

    bool const resurrectableDeathState = dead->getDeathState() == DeathState::Corpse ||
                                         dead->getDeathState() == DeathState::Dead;
    if (!resurrectableDeathState)
    {
        resolved.failure = Failure::TransitionalDeathState;
        return resolved;
    }
    if (dead->isResurrectRequested())
    {
        resolved.failure = Failure::PendingRequest;
        return resolved;
    }

    bool const released = dead->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    bool const unitOnHealerMap = healer->GetMap() && dead->GetMap() == healer->GetMap();
    CorpseValidation corpseValidation = CorpseValidation::NotRequired;

    if (released)
    {
        Corpse* corpse = healer->GetMap() ? healer->GetMap()->GetCorpseByPlayer(dead->GetGUID()) : nullptr;
        resolved.corpse = corpse;
        corpseValidation = ValidateCorpse(healer, dead, corpse);
        if (corpseValidation != CorpseValidation::Valid)
        {
            resolved.failure = FailureFor(corpseValidation);
            return resolved;
        }
    }

    resolved.kind = ClassifyTarget(true, resurrectableDeathState, false, released,
                                   unitOnHealerMap, corpseValidation);
    if (resolved.kind == TargetKind::Invalid)
    {
        resolved.failure = Failure::WrongMap;
        return resolved;
    }

    resolved.failure = Failure::None;
    return resolved;
}

WorldObject* GetAnchor(ResolvedTarget const& resolved)
{
    if (resolved.kind == TargetKind::ReleasedCorpse)
        return resolved.corpse;
    if (resolved.kind == TargetKind::UnreleasedUnit)
        return resolved.player;
    return nullptr;
}

bool HasResurrectionReservation(Player* healer, Player* dead, Corpse* corpse)
{
    if (!healer || !dead)
        return false;

    Group* group = healer->GetGroup();
    if (!group)
        return false;

    ObjectGuid const playerGuid = dead->GetGUID();
    ObjectGuid const corpseGuid = corpse ? corpse->GetGUID() : ObjectGuid::Empty;
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* caster = gref->GetSource();
        if (!caster || caster == healer || !caster->IsNonMeleeSpellCast(true))
            continue;

        for (uint8 type = CURRENT_GENERIC_SPELL; type < CURRENT_MAX_SPELL; ++type)
        {
            Spell* current = caster->GetCurrentSpell(static_cast<CurrentSpellTypes>(type));
            if (!current || !HasResurrectionEffect(current->m_spellInfo))
                continue;

            if (current->m_targets.GetUnitTargetGUID() == playerGuid)
                return true;
            if (corpseGuid && current->m_targets.GetCorpseTargetGUID() == corpseGuid)
                return true;
        }
    }

    return false;
}
}

Unit* PartyMemberToResurrect::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::array<std::vector<Player*>, 4> candidates;
    Player* master = GetMaster();
    float const searchRadius = ResurrectionTargetPolicy::SearchRadius(
        sPlayerbotAIConfig.spellDistance, bot->GetMap() && bot->GetMap()->IsDungeon());
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        // GetSource is the authoritative online group member and remains usable when a released
        // ghost has moved to another map while its corpse remains beside this healer.
        Player* player = gref->GetSource();
        if (!player || player == bot || !player->IsInWorld() || player->IsGameMaster())
            continue;

        auto const priority = ResurrectionTargetPolicy::ClassifyRole(
            player == master, botAI->IsHeal(player), botAI->IsTank(player));
        candidates[static_cast<std::size_t>(priority)].push_back(player);
    }

    for (std::vector<Player*> const& priorityBand : candidates)
    {
        for (Player* player : priorityBand)
        {
            ResurrectionTargetPolicy::ResolvedTarget const resolved =
                ResurrectionTargetPolicy::Resolve(bot, player);
            if (resolved.kind == ResurrectionTargetPolicy::TargetKind::Invalid)
                continue;
            if (ResurrectionTargetPolicy::HasResurrectionReservation(bot, player, resolved.corpse))
                continue;

            WorldObject* anchor = ResurrectionTargetPolicy::GetAnchor(resolved);
            if (!anchor || bot->GetDistance(anchor) >= searchRadius)
                continue;

            return player;
        }
    }

    return nullptr;
}
