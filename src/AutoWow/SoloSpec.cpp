/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Cohort.SoloSpec runtime (policy: AutoWow/SoloSpecPolicy.h).

#include "SoloSpecPolicy.h"

#include <string>
#include <vector>

#include "AiFactory.h"
#include "AutoWowGuildsPolicy.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "TacticalRuntime.h"

static_assert(WARRIOR_TAB_ARMS == 0 && WARRIOR_TAB_FURY == 1 && PALADIN_TAB_RETRIBUTION == 2 &&
              HUNTER_TAB_BEAST_MASTERY == 0 && ROGUE_TAB_COMBAT == 1 && PRIEST_TAB_SHADOW == 2 &&
              DEATH_KNIGHT_TAB_BLOOD == 0 && DEATH_KNIGHT_TAB_UNHOLY == 2 && SHAMAN_TAB_ENHANCEMENT == 1 &&
              MAGE_TAB_FIRE == 1 && MAGE_TAB_FROST == 2 && WARLOCK_TAB_AFFLICTION == 0 &&
              WARLOCK_TAB_DEMONOLOGY == 1 && DRUID_TAB_BALANCE == 0 && DRUID_TAB_FERAL == 1);

namespace AutoWowSoloSpec
{
namespace
{
bool gEnabled = false;
std::vector<AutoWowGuilds::GuidRange> gExempt;

bool HasTemplate(std::uint32_t cls, std::int32_t specNo)
{
    for (std::uint32_t level = 0; level < MAX_LEVEL; ++level)
        if (!sPlayerbotAIConfig.parsedSpecLinkOrder[cls][specNo][level].empty())
            return true;
    return false;
}
}  // namespace

void LoadConfig()
{
    gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Cohort.SoloSpec", false);
    gExempt.clear();
    std::string const exempt = sConfigMgr->GetOption<std::string>("AutoWow.Cohort.SoloSpecExempt", "");
    if (gEnabled && !exempt.empty() && !AutoWowGuilds::ParseGuidRanges(exempt, gExempt))
        LOG_ERROR("server.loading", "[SoloSpec] bad AutoWow.Cohort.SoloSpecExempt '{}': no exemptions", exempt);
    if (gEnabled)
        LOG_INFO("server.loading", "[SoloSpec] enabled: {} exempt ranges", gExempt.size());
}

bool Enabled() { return gEnabled; }

void OnLogin(Player* bot)
{
    if (!gEnabled || !bot)
        return;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    if (!ai || ai->IsRealPlayer() || !AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid) ||
        AutoWowGuilds::InRanges(gExempt, guid))
        return;

    std::map<uint8, uint32> tabs = AiFactory::GetPlayerSpecTabs(bot);
    std::uint32_t const spent = tabs[0] + tabs[1] + tabs[2];
    std::uint32_t const tab = AiFactory::GetPlayerSpecTab(bot);
    std::uint32_t const cls = bot->getClass();
    std::int32_t specNo = SpecToApply(cls, tab, spent);
    if (specNo == kKeep)
        return;
    // A grouped bot may be a recruited dungeon healer / tank; an instanced one is mid-run: keep its tree.
    Map* map = bot->GetMap();
    if (bot->GetGroup() || !map || map->Instanceable())
    {
        LOG_INFO("playerbots", "[SoloSpec] bot={} guid={} class={} tab={} keep why={}", bot->GetName(), guid, cls,
                 tab, bot->GetGroup() ? "group" : "instance");
        return;
    }
    // Priest: the staged shadow leveling template when AutoWow.Tactics.PriestShadowLevelingSpec has one.
    if (cls == CLASS_PRIEST && AutoWowTactics::ShadowLevelingSpec() &&
        HasTemplate(cls, static_cast<std::int32_t>(AutoWowTactics::kShadowLevelingSpecNo)))
        specNo = static_cast<std::int32_t>(AutoWowTactics::kShadowLevelingSpecNo);
    if (!HasTemplate(cls, specNo))
    {
        LOG_ERROR("playerbots", "[SoloSpec] bot={} guid={} class={} spec={} has no premade template", bot->GetName(),
                  guid, cls, specNo);
        return;
    }

    // resetTalents(true): no gold cost (bots pay nothing for the respec).
    PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitGlyphs(false);
    LOG_INFO("playerbots", "[SoloSpec] bot={} guid={} class={} lvl={} reset tab={} spent={} -> spec={} ({}) cost=0 "
             "now_tab={}", bot->GetName(), guid, cls, bot->GetLevel(), tab, spent, specNo,
             sPlayerbotAIConfig.premadeSpecName[cls][specNo], AiFactory::GetPlayerSpecTab(bot));
}
}  // namespace AutoWowSoloSpec
