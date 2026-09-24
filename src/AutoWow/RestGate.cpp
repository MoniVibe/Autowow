/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RestGate.h"

#include "Config.h"
#include "DeathLoopBreaker.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"

namespace AutoWowRestGate
{
namespace
{
std::uint32_t Pct(std::uint64_t cur, std::uint64_t max) { return max ? static_cast<std::uint32_t>(cur * 100 / max) : 100; }

// AutoWow.DeathLoop.V2 (c): forced rest pending for this bot (false with V2 off).
bool Forced(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    return bot && AutoWowDeathLoop::V2Enabled() &&
           AutoWowDeathLoop::RestPending(static_cast<std::uint32_t>(bot->GetGUID().GetCounter()));
}

// forced: any bot (the death-loop breaker only escalates autonomous ones), else independent party only.
Player* EligibleBot(PlayerbotAI* botAI, bool forced)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || (!forced && !botAI->IsAutoWowIndependentParty()) || !bot->IsAlive() || bot->InBattleground())
        return nullptr;
    Group const* group = bot->GetGroup();
    if (group && group->GetMembersCount() > 1)
        return nullptr;
    Map const* map = bot->GetMap();
    return map && !map->Instanceable() ? bot : nullptr;
}

bool UsesMana(Player* bot) { return bot->getPowerType() == POWER_MANA; }
std::uint32_t HpPct(Player* bot) { return Pct(bot->GetHealth(), bot->GetMaxHealth()); }
std::uint32_t ManaPct(Player* bot) { return Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)); }
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.RestGate", false);
    Params& p = detail::gParams;
    p.minHpPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.RestGate.MinHpPct", 70);
    p.minManaPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Survival.RestGate.MinManaPct", 60);
}

bool HoldProactivePull(PlayerbotAI* botAI)
{
    bool const forced = Forced(botAI);
    Player* bot = Enabled() || forced ? EligibleBot(botAI, forced) : nullptr;
    if (!bot)
        return false;
    bool const hold =
        Hold(detail::gParams, HpPct(bot), ManaPct(bot), UsesMana(bot), bot->HasAura(kResurrectionSicknessAura));
    if (forced && !hold)
        AutoWowDeathLoop::ClearRestPending(static_cast<std::uint32_t>(bot->GetGUID().GetCounter()));
    return hold;
}

bool NeedsRestHealth(PlayerbotAI* botAI)
{
    bool const forced = Forced(botAI);
    Player* bot = Enabled() || forced ? EligibleBot(botAI, forced) : nullptr;
    return bot && !bot->IsInCombat() && NeedHealth(detail::gParams, HpPct(bot));
}

bool NeedsRestMana(PlayerbotAI* botAI)
{
    bool const forced = Forced(botAI);
    Player* bot = Enabled() || forced ? EligibleBot(botAI, forced) : nullptr;
    return bot && !bot->IsInCombat() && NeedMana(detail::gParams, ManaPct(bot), UsesMana(bot));
}
}  // namespace AutoWowRestGate
