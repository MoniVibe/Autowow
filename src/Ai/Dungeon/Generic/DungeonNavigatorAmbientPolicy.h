/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATORAMBIENTPOLICY_H
#define PLAYERBOTS_DUNGEONNAVIGATORAMBIENTPOLICY_H

namespace DungeonNavigatorAmbientPolicy
{
// Navigator-owned instances must not acquire ambient movement or grind work. Map::IsDungeon()
// deliberately treats five-player dungeons and raids alike, while the explicit non-combat state
// supplied by callers leaves world maps and instance bots without the navigator unchanged.
template <typename BotT, typename BotAIT, typename BotStateT>
bool ShouldSuppress(BotT const* bot, BotAIT* botAI, BotStateT nonCombatState)
{
    if (!bot || !botAI || !bot->IsInWorld())
        return false;

    auto const* map = bot->GetMap();
    return map && map->IsDungeon() &&
        botAI->HasStrategy("dungeon navigator", nonCombatState);
}

// A bot-led navigator convoy owns follower movement while both bots are out of combat. Keep the
// ordinary follow strategy installed so resets and human-led groups retain their normal
// behaviour, but prevent its formation point from replacing a prepared navigator spline only
// while convoy movement is eligible to run. Combat restores ordinary follow/assist closure.
// The caller supplies the two relationship facts because PlayerbotAI already resolves the
// authoritative group leader; the policy itself remains a small, testable, mutation-free seam.
template <typename BotT, typename BotAIT, typename LeaderAIT, typename BotStateT>
bool ShouldSuppressOrdinaryFollow(BotT const* bot, BotAIT* botAI, LeaderAIT* leaderAI,
                                  bool isGroupFollower, bool sameContext,
                                  BotStateT currentState, BotStateT nonCombatState)
{
    if (currentState != nonCombatState || !isGroupFollower || !sameContext ||
        !bot || !botAI || !leaderAI || !bot->IsInWorld())
    {
        return false;
    }

    if (leaderAI->GetState() != nonCombatState)
        return false;

    auto const* map = bot->GetMap();
    return map && map->IsDungeon() &&
        botAI->HasStrategy("dungeon navigator", nonCombatState) &&
        leaderAI->HasStrategy("dungeon navigator", nonCombatState);
}
}

#endif
