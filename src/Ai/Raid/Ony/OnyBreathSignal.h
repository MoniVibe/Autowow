#ifndef PLAYERBOTS_ONYBREATHSIGNAL_H
#define PLAYERBOTS_ONYBREATHSIGNAL_H

#include "Define.h"
#include "ObjectGuid.h"

namespace OnyxiaBreathSignal
{
bool IsDeepBreathSpell(uint32 spellId);
bool RecordEncounter(uint32 instanceId, uint32 spellId);
uint32 GetEncounterActive(uint32 instanceId);
void ClearEncounter(uint32 instanceId);
void Record(ObjectGuid botGuid, uint32 spellId);
uint32 GetActive(ObjectGuid botGuid);
bool MarkMove(ObjectGuid botGuid, uint32 spellId);
bool MarkArrival(ObjectGuid botGuid, uint32 spellId);
void Clear(ObjectGuid botGuid);
}

#endif
