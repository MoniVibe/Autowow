/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ACCEPTANCE_H
#define AUTOWOW_ACCEPTANCE_H

#include "Define.h"

// Engine-authoritative Phase 1 acceptance invariant counters (schema
// autowow.phase1.acceptance.evidence.v1). These are VIOLATION GUARDS: each is incremented only if the
// objective-lock enforcement is ever bypassed, so the release gate requires all of them to remain 0.
// They are read out through the read-only `acceptance` bridge order; nothing here mutates game state.
namespace AutoWowAcceptance
{
    struct Counters
    {
        uint64 unrelatedOffensivePull = 0;   // strict locked GrindTarget return that was not whitelisted
        uint64 randomGrindFallback = 0;       // generic grind / SetGrindTarget taken while a lock was active
        uint64 teleport = 0;                  // TeleportTo on the quest source/finisher path under a lock
        uint64 directQuestDbMutation = 0;     // direct quest/xp/item table write by the surface (never incremented)
    };

    // Increment sites (called from the enforcement points; all no-ops in the happy path).
    void NoteUnrelatedOffensivePull();
    void NoteRandomGrindFallback();
    void NoteTeleport();

    // Read-only process-lifetime snapshot. The external evidence harness records a baseline at the
    // beginning of its run and evaluates deltas, so it never needs a state-mutating reset command.
    Counters Get();
}

#endif  // AUTOWOW_ACCEPTANCE_H
