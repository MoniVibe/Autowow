/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDFEARSIGNAL_H
#define PLAYERBOTS_RAIDFEARSIGNAL_H

#include "Define.h"

namespace RaidFearSignal
{
bool Record(uint32 instanceId, uint32 casterEntry, uint32 spellId);
bool RecordImminent(uint32 instanceId, uint32 casterEntry);
uint32 GetActiveSpell(uint32 instanceId);
bool IsActive(uint32 instanceId);
bool IsKnownFearCaster(uint32 casterEntry);
void ClearInstance(uint32 instanceId);
}

#endif
