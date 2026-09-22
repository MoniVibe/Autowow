/*
 * Guarded, exact one-craft runtime adapter for AutoWow.
 *
 * This surface is intentionally narrower than the planner. It accepts one learned recipe spell,
 * performs at most one native cast, and requires a later exact inventory-delta observation before
 * reporting completion. It does not select recipes, gather or buy reagents, retry, or train a
 * profession.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_CRAFT_CONTROL_H
#define MOD_PLAYERBOTS_AUTOWOW_CRAFT_CONTROL_H

#include <cstdint>
#include <string>

class Player;
class PlayerbotAI;

namespace AutoWowCraftControl
{
std::string Execute(Player* bot, PlayerbotAI* botAI, std::uint32_t recipeSpellId);
std::string Status(Player* bot);
}

#endif // MOD_PLAYERBOTS_AUTOWOW_CRAFT_CONTROL_H
