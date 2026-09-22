/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoWowAcceptance.h"

#include <atomic>

namespace
{
std::atomic<uint64> g_unrelatedOffensivePull{0};
std::atomic<uint64> g_randomGrindFallback{0};
std::atomic<uint64> g_teleport{0};
// directQuestDbMutation is never incremented: the AutoWow surface performs no direct quest-table write.
}  // namespace

namespace AutoWowAcceptance
{
void NoteUnrelatedOffensivePull() { g_unrelatedOffensivePull.fetch_add(1, std::memory_order_relaxed); }
void NoteRandomGrindFallback() { g_randomGrindFallback.fetch_add(1, std::memory_order_relaxed); }
void NoteTeleport() { g_teleport.fetch_add(1, std::memory_order_relaxed); }

Counters Get()
{
    Counters c;
    c.unrelatedOffensivePull = g_unrelatedOffensivePull.load(std::memory_order_relaxed);
    c.randomGrindFallback = g_randomGrindFallback.load(std::memory_order_relaxed);
    c.teleport = g_teleport.load(std::memory_order_relaxed);
    c.directQuestDbMutation = 0;
    return c;
}

}  // namespace AutoWowAcceptance
