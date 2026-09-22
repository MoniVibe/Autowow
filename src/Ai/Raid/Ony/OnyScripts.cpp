#include "OnyBreathSignal.h"

#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Unit.h"

namespace
{
constexpr uint32 OnyxiaEntry = 10184;
constexpr uint32 OnyxiaMap = 249;

void RecordDeepBreath(Unit* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo || caster->GetEntry() != OnyxiaEntry || caster->GetMapId() != OnyxiaMap ||
        !OnyxiaBreathSignal::IsDeepBreathSpell(spellInfo->Id))
    {
        return;
    }

    uint32 const instanceId = caster->GetInstanceId();
    bool const newSignal = OnyxiaBreathSignal::RecordEncounter(instanceId, spellInfo->Id);
    uint32 awakenedBots = 0;
    Map::PlayerList const& players = caster->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator it = players.begin(); it != players.end(); ++it)
    {
        Player* player = it->GetSource();
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!player || !player->IsAlive() || !botAI)
            continue;

        OnyxiaBreathSignal::Record(player->GetGUID(), spellInfo->Id);
        // A prior movement or cast can leave an otherwise healthy bot sleeping beyond the
        // warning window. Wake the normal AI engine; the listener never chooses a destination
        // and never relocates a player itself.
        botAI->SetNextCheckDelay(0);
        ++awakenedBots;
    }

    if (newSignal)
    {
        LOG_INFO("playerbots", "[Onyxia] breath_signal instance={} spell={} caster_guid={} awakened_bots={}",
            instanceId, spellInfo->Id, caster->GetGUID().GetCounter(), awakenedBots);
    }
}
}

class OnyxiaBreathListenerScript : public AllSpellScript
{
public:
    OnyxiaBreathListenerScript() : AllSpellScript("OnyxiaBreathListenerScript") { }

    void OnSpellPrepare(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo) override
    {
        RecordDeepBreath(caster, spellInfo);
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        RecordDeepBreath(caster, spellInfo);
    }
};

void AddSC_OnyxiaBotScripts()
{
    new OnyxiaBreathListenerScript();
}
