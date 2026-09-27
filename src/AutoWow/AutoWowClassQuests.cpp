/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.ClassQuests runtime (policy and verified table: AutoWow/ClassQuestPolicy.h). Grants a cohort / supply role
// bot the class-quest rewards of its level at login (PlayerbotHolder::OnBotLogin) and on every level change.

#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "ClassQuestPolicy.h"
#include "Config.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellMgr.h"
#include "SupplyPolicy.h"

namespace AutoWowClassQuests
{
namespace
{
// A PlayerbotAI-driven cohort member or supply role (reps / artisans); never a real player.
bool Covered(Player* bot)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai || ai->IsRealPlayer())
        return false;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    return AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) ||
           (AutoWowSupply::Enabled() && AutoWowSupply::RoleOf(guid).role != AutoWowSupply::Role::None);
}

void Emit(Player* bot, Pick const& p, char const* cause)
{
    LOG_INFO("playerbots", "[ClassQuest] bot={} grant cause={} class={} lvl={} quest={} spell={} item={} level={}",
             bot->GetName(), cause, bot->getClass(), bot->GetLevel(), p.quest, p.spell, p.item, p.level);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitClassQuest(bot, "grant", p.quest, LedgerFields(p));
}

class ClassQuestPlayerScript : public PlayerScript
{
public:
    ClassQuestPlayerScript() : PlayerScript("AutoWowClassQuestPlayer", {PLAYERHOOK_ON_LEVEL_CHANGED}) {}

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (Enabled())
            Grant(player, "level");
    }
};
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.ClassQuests.Enable", false);
    detail::gMounts = sConfigMgr->GetOption<bool>("AutoWow.ClassQuests.Mounts", false);
    if (detail::gEnabled)
        LOG_INFO("server.loading", "[ClassQuest] enabled: {} reward rows, mounts {}", kRewards.size(),
                 detail::gMounts ? 1 : 0);
}

void Grant(Player* bot, char const* cause)
{
    if (!detail::gEnabled || !bot || !Covered(bot))
        return;
    std::vector<Pick> const picks =
        Select(bot->getClass(), bot->getRace(), bot->GetLevel(), detail::gMounts,
               [bot](std::uint32_t spell) { return bot->HasSpell(spell); },
               [bot](std::uint32_t item) { return bot->HasItemCount(item, 1, true); });
    for (Pick const& p : picks)
    {
        if (p.spell)
        {
            if (!sSpellMgr->GetSpellInfo(p.spell))
                continue;
            bot->learnSpell(p.spell);
            if (!bot->HasSpell(p.spell))
                continue;  // refused by the core (e.g. a missing prerequisite): retried at the next login / level
        }
        else
        {
            if (!sObjectMgr->GetItemTemplate(p.item))
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, p.item, 1) != EQUIP_ERR_OK)
            {
                LOG_INFO("playerbots", "[ClassQuest] bot={} skip cause={} item={}: no bag room", bot->GetName(), cause,
                         p.item);
                continue;  // retried at the next login / level
            }
            Item* item = bot->StoreNewItem(dest, p.item, true);
            if (!item)
                continue;
            bot->SendNewItem(item, 1, true, false);
        }
        Emit(bot, p, cause);
    }
}

void AddScripts() { new ClassQuestPlayerScript(); }
}  // namespace AutoWowClassQuests
