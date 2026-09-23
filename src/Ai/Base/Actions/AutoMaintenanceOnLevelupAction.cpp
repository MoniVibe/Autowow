#include "AutoMaintenanceOnLevelupAction.h"

#include "AutoWowOracleRuntime.h"
#include "SpellMgr.h"

#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "BroadcastHelper.h"
#include "AutoWowCohortPolicy.h"

namespace
{
// AutoWow.Independent.AutoMaintenance: only consulted after the legacy random/Oracle gates failed.
// Mode 0 (default) returns before any other lookup, so the legacy path is unchanged.
bool IndependentMaintenance(PlayerbotAI* botAI, Player* bot, bool oracleManaged,
                            bool (*allows)(uint32 mode, bool eligible))
{
    uint32 const mode = sPlayerbotAIConfig.autoWowIndependentAutoMaintenance;
    if (!mode)
        return false;

    return allows(mode, AutoWowCohortPolicy::IsIndependentMaintenanceEligible(
                            sRandomPlayerbotMgr.IsRandomBot(bot), oracleManaged, botAI->IsAutoWowIndependentParty()));
}
}  // namespace

bool AutoMaintenanceOnLevelupAction::Execute(Event /*event*/)
{
    AutoPickTalents();
    AutoLearnSpell();
    AutoTeleportForLevel();
    AutoUpgradeEquip();

    return true;
}

void AutoMaintenanceOnLevelupAction::AutoTeleportForLevel()
{
    if (!sPlayerbotAIConfig.autoTeleportForLevel || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (botAI->HasRealPlayerMaster())
        return;

    sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
    return;
}

void AutoMaintenanceOnLevelupAction::AutoPickTalents()
{
    bool const oracleManaged = AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter());
    if (!sPlayerbotAIConfig.autoPickTalents ||
        (!sRandomPlayerbotMgr.IsRandomBot(bot) && !oracleManaged &&
         !IndependentMaintenance(botAI, bot, oracleManaged, AutoWowCohortPolicy::IndependentAutoTalents)))
        return;

    if (bot->GetFreeTalentPoints() <= 0)
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitTalentsTree(true, true, true);
    factory.InitPetTalents();
}

void AutoMaintenanceOnLevelupAction::AutoLearnSpell()
{
    std::ostringstream out;
    LearnSpells(&out);

    if (!out.str().empty())
    {
        std::string const temp = out.str();
        out.seekp(0);
        out << "Learned spells: ";
        out << temp;
        out.seekp(-2, out.cur);
        out << ".";
        botAI->TellMaster(out);
    }
    return;
}

void AutoMaintenanceOnLevelupAction::LearnSpells(std::ostringstream* out)
{
    BroadcastHelper::BroadcastLevelup(botAI, bot);
    bool const oracleManaged = AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter());
    if (sPlayerbotAIConfig.autoLearnTrainerSpells &&
        (sRandomPlayerbotMgr.IsRandomBot(bot) || oracleManaged))
        LearnTrainerSpells(out);
    else if (sPlayerbotAIConfig.autoLearnTrainerSpells &&
             IndependentMaintenance(botAI, bot, oracleManaged, AutoWowCohortPolicy::IndependentAutoSpells))
    {
        // Class spells only: no InitSkills (free riding / maxed weapon skills), no tradeskill recipes.
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitClassSpells();
        factory.InitAvailableSpells(true);
        factory.InitPet();
    }

    if (sPlayerbotAIConfig.autoLearnQuestSpells &&
        (sRandomPlayerbotMgr.IsRandomBot(bot) || oracleManaged ||
         IndependentMaintenance(botAI, bot, oracleManaged, AutoWowCohortPolicy::IndependentAutoSpells)))
        LearnQuestSpells(out);
}

void AutoMaintenanceOnLevelupAction::LearnTrainerSpells(std::ostringstream* /*out*/)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitSkills();
    factory.InitClassSpells();
    factory.InitAvailableSpells();
    factory.InitPet();
}

void AutoMaintenanceOnLevelupAction::LearnQuestSpells(std::ostringstream* out)
{
    ObjectMgr::QuestMap const& questTemplates = sObjectMgr->GetQuestTemplates();
    for (ObjectMgr::QuestMap::const_iterator i = questTemplates.begin(); i != questTemplates.end(); ++i)
    {
        Quest const* quest = i->second;

        if (!quest->GetRequiredClasses() || quest->IsRepeatable() || quest->GetMinLevel() < 10 ||
            quest->GetMinLevel() > bot->GetLevel())
        {
            continue;
        }

        if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false) ||
            !bot->SatisfyQuestSkill(quest, false))
        {
            continue;
        }

        int32 spellId = quest->GetRewSpellCast();
        if (!spellId)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        bool found = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && spellInfo->Effects[i].TriggerSpell &&
                !bot->HasSpell(spellInfo->Effects[i].TriggerSpell))
            {
                if (SpellInfo const* triggeredInfo = sSpellMgr->GetSpellInfo(spellInfo->Effects[i].TriggerSpell))
                    if (triggeredInfo->Effects[0].Effect == SPELL_EFFECT_TRADE_SKILL)
                        break;

                found = true;
                break;
            }
        }

        if (!found)
            continue;

        bot->CastSpell(bot, spellId, true);

        uint32 rewSpellId = quest->GetRewSpell();
        if (rewSpellId)
        {
            if (SpellInfo const* rewSpellInfo = sSpellMgr->GetSpellInfo(rewSpellId))
            {
                *out << FormatSpell(rewSpellInfo) << ", ";
                continue;
            }
        }

        *out << FormatSpell(spellInfo) << ", ";
    }
}

std::string const AutoMaintenanceOnLevelupAction::FormatSpell(SpellInfo const* sInfo)
{
    std::ostringstream out;
    std::string const rank = sInfo->Rank[0];

    if (rank.empty())
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << "]|h|r";
    else
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << " " << rank << "]|h|r";

    return out.str();
}

void AutoMaintenanceOnLevelupAction::AutoUpgradeEquip()
{
    // Deliberately not widened by AutoWow.Independent.AutoMaintenance: independent bots never get
    // free gear, consumables, ammo or reagents.
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) &&
        !AutoWowOracleRuntime::IsManagedBot(bot->GetGUID().GetCounter()))
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());

    factory.CleanupConsumables();

    factory.InitAmmo();
    factory.InitReagents();
    factory.InitFood();
    factory.InitConsumables();
    factory.InitPotions();

    if (sPlayerbotAIConfig.autoUpgradeEquip)
        factory.InitEquipment(true);
}
