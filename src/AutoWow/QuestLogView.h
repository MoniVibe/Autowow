/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_QUEST_LOG_VIEW_H
#define AUTOWOW_QUEST_LOG_VIEW_H

#include <string>

#include "Define.h"

// Isolated, READ-ONLY questlog view for the AutoWow bridge.
//
// Build() reads the bot's live in-memory quest state (Player::getQuestStatusMap +
// sObjectMgr->GetQuestTemplate) on the world thread and returns a JSON snapshot of the active
// quests, their objectives (kind / required entry / required count / live current count / done),
// completion state, and a coarse Quest Director capability class. It performs NO SQL and mutates
// nothing — it never queries or writes character quest tables and never changes quest state.
//
// MUST be called on the world thread (the AutoWow bridge dispatches through the world-thread
// processor, so this holds by construction).
namespace AutoWowQuestLog
{
    struct Capability
    {
        char const* name;
        bool supported;
    };

    // Shared, behavior-neutral classifier used by both the live quest-log view and exact quest
    // acquisition. Callers provide the objective shape from the immutable quest template.
    Capability ClassifyCapability(uint32 questType, uint32 suggestedPlayers, uint32 sourceItemId,
                                  bool hasNpc, bool hasGameObject, bool hasItem, bool isComplete);

    // Build preserves objective.quest_id as the active-lock identity. When a completed quest is in
    // finisher mode, objective.active is false with an explicit inactive_reason and the top-level
    // finisher object carries the authoritative quest/route/bind/reward evidence.
    std::string Build(uint32 botGuid);
}

// Isolated, READ-ONLY snapshot of the single locked quest objective for the AutoWow bridge.
//
// Build() reads the bot's live objective-lock state — the authoritative QuestObjectiveSpec published
// through the "active quest objective" Value plus the mutable QuestObjectiveRuntime owned by
// NewRpgInfo::DoQuest — and returns a JSON snapshot (quest / family / slot / kind / phase / failure /
    // counts / selected source+target / finisher / lock), durable direct-GO receipts, active directive
    // identity, and loaded finisher position/distance. It performs NO SQL and mutates nothing: it never
    // touches the runtime, the Value, quest tables, or quest state.
//
// MUST be called on the world thread (the AutoWow bridge dispatches through the world-thread
// processor, so this holds by construction).
namespace AutoWowQuestObjective
{
    // T2a read-only expansion: route native-interaction evidence is serialized only from the
    // server-owned Oracle quest handoff; range and movement observations are never promoted.
    std::string Build(uint32 botGuid);

    // Read-only acceptance-evidence body: the objective snapshot + reward postcondition fields + the
    // engine-authoritative invariant counters (schema autowow.phase1.acceptance.evidence.v1). Mutates
    // nothing.
    // requestedQuestId is optional and read-only. It lets the evidence harness ask for a quest's
    // reward postcondition after the active objective has advanced to a different quest.
    std::string BuildAcceptance(uint32 botGuid, uint32 requestedQuestId = 0);
}

#endif  // AUTOWOW_QUEST_LOG_VIEW_H
