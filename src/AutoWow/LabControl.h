/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_LAB_CONTROL_H
#define AUTOWOW_LAB_CONTROL_H

class ChatHandler;

// Combat lab runtime (see LabPolicy.h). GM chat commands, all refused unless AutoWow.Lab.Enable = 1:
//   .autowow lab kit [ilvl]                 level/spec/gear the invoking Lab<Class> character (lab account only)
//   .autowow lab spawn <entry> <count> [lv] summon hostile copies on the marker ring; they attack the invoker
//   .autowow lab reset                      despawn lab mobs, teleport to the marker, heal, refill, clear cooldowns
// AutoWow.Lab.Trace = 1 writes lab-trace.jsonl (casts, swings, per-second resource/damage) for every player or
// self-bot inside AutoWow.Lab.Radius of the marker.
namespace AutoWowLab
{
void LoadConfig();
void AddScripts();

bool HandleKit(ChatHandler* handler, char const* args);
bool HandleSpawn(ChatHandler* handler, char const* args);
bool HandleReset(ChatHandler* handler, char const* args);
}  // namespace AutoWowLab

#endif
