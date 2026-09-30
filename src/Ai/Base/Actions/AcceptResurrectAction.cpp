/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptResurrectAction.h"
#include "Event.h"
#include "LastMovementValue.h"
#include "Playerbots.h"

bool AcceptResurrectAction::Execute(Event event)
{
    if (bot->IsAlive())
        return false;

    WorldPacket p(event.getPacket());
    p.rpos(0);
    ObjectGuid guid;
    p >> guid;

    WorldPacket packet(CMSG_RESURRECT_RESPONSE, 8 + 1);
    packet << guid;
    packet << uint8(1);                                        // accept
    bool const wasReleased = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    bot->GetSession()->HandleResurrectResponseOpcode(packet);  // queue the packet to get around race condition

    // A real resurrection supersedes a pending released-corpse entrance walk. Prevent the dead
    // engine's generic area-trigger action from consuming a stale portal after the bot revives.
    context->GetValue<LastMovement&>("last area trigger")->Get().lastAreaTrigger = 0;

    // A player resurrection can teleport to the stored resurrection point first.  In that case
    // AzerothCore schedules DELAYED_RESURRECT_PLAYER and IsAlive() remains false in this stack.
    // PlayerbotsPlayerScript::OnPlayerResurrect emits the authoritative completion event later.
    LOG_DEBUG("playerbots", "[CorpseRescue] accept target={} source={} released={} alive_after={} delayed={}",
              bot->GetName(), guid.GetCounter(), wasReleased, bot->IsAlive(), bot->IsBeingTeleported());

    return true;
}
