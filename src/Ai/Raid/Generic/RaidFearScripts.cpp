/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidFearResponsePolicy.h"
#include "RaidFearSignal.h"

#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"

namespace
{
bool AppliesFearAura(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (effect.ApplyAuraName == SPELL_AURA_MOD_FEAR)
            return true;
    }
    return false;
}

void RecordRaidFear(Unit* caster, SpellInfo const* spellInfo)
{
    Creature* creature = caster ? caster->ToCreature() : nullptr;
    Map* map = caster ? caster->GetMap() : nullptr;
    if (!creature || !map || !map->IsRaid() || !spellInfo ||
        !RaidFearResponsePolicy::SpellUsesFear(spellInfo->GetAllEffectsMechanicMask(), AppliesFearAura(spellInfo)))
    {
        return;
    }

    uint32 awakenedBots = 0;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator it = players.begin(); it != players.end(); ++it)
    {
        Player* player = it->GetSource();
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!player || !player->IsAlive() || !botAI || !caster->IsHostileTo(player))
            continue;

        botAI->SetNextCheckDelay(0);
        ++awakenedBots;
    }
    if (!awakenedBots)
        return;

    if (RaidFearSignal::Record(caster->GetInstanceId(), creature->GetEntry(), spellInfo->Id))
    {
        LOG_INFO("playerbots", "[RaidFear] learned instance={} caster={} entry={} spell={} awakened_bots={}",
            caster->GetInstanceId(), creature->GetName(), creature->GetEntry(), spellInfo->Id, awakenedBots);
    }
}
}

class RaidFearListenerScript : public AllSpellScript
{
public:
    RaidFearListenerScript() : AllSpellScript("RaidFearListenerScript") { }

    void OnSpellPrepare(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo) override
    {
        RecordRaidFear(caster, spellInfo);
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        RecordRaidFear(caster, spellInfo);
    }
};

void AddSC_RaidFearBotScripts()
{
    new RaidFearListenerScript();
}
