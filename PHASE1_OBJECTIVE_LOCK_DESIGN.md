# Phase 1 objective-lock — design note & progress

Date: 2026-07-13 (Asia/Jerusalem)
Source: advisor brief "AutoWoW quest-engine architecture review" (narrow objective-lock layer).
Baseline: mod-playerbots at `93aaea3d` (matches advisor pin) with local uncommitted work preserved.

## 0. Guardrails honored

The active overnight run was **not** touched: no build, restart, process change, DB write, or
live command experiment. All edits are source-only in the local checkout (the running binary is the
previously-built one and is unaffected until a deliberate maintenance-window rebuild). No client, no
public ports, no observer-camera changes.

## 1. Local verification vs the advisor's public-commit findings

The advisor verified against public `93aaea3d` and asked that each item be re-checked locally
(uncertainty #7). Result:

| Advisor finding | Local status | Action |
|---|---|---|
| Bug: `ActiveQuestObjectivesValue` reads `RequiredItemCount` for NPC/GO objectives (`QuestValues.cpp`) | **Already fixed locally** (`RequiredNpcOrGoCount` at line 279 — the one-line fix from `QUEST_CAPABILITY_MATRIX.md`) | none (verified correct) |
| Bug: `MoveFarTo()` teleports after stuck timeout (no-teleport violation) | **Already handled for league bots** — `MoveFarTo` checks `AutoWowPolicy::IsNoTeleport` and holds; `QuestParty` calls `EnableLeagueNoTeleport(group)` | keep; extend to a formal `NoTeleport` policy for all objective travel in a later increment |
| Bug: GO **involved** relation stored as `questGiver` (`QuestValues.cpp`) | **Confirmed, unfixed** | **FIXED** (see §2) |
| Bug: TravelMgr objective destination is the `else` of quest-taker | **Confirmed, unfixed** | **FIXED** (see §2) |
| Nearby mobs bypass quest relevance; item relevance overly broad; POI→generic grind; non-delta progress; generic turn-in; `SetGrindTarget` fallback | Confirmed in local source | Addressed by the objective-lock layer (remaining Phase 1 work, §3) |

## 2. Fixes landed this increment (small, surgical, reversible)

1. **`src/Ai/Base/Value/QuestValues.cpp`** — `EntryQuestRelationMapValue::Calculate`: the
   `GetGOQuestInvolvedRelationMap()` loop now tags entries `questTaker` (was `questGiver`), mirroring
   the creature involved-relation path. Gameobject turn-ins are now resolvable through the taker map.
2. **`src/Mgr/Travel/TravelMgr.cpp`** — quest travel-table construction: objective destinations are
   now an **independent** `if (flag & objective1..4)` branch instead of the `else` of the quest-taker
   check. This stops giver-only relations from producing false objective-0 destinations and lets a
   relation that is both taker and objective produce both destinations. Objective slot mapping is
   unchanged (objective1→0 … objective4→3).
3. **New `src/Ai/World/Rpg/QuestObjectiveContext.h`** — types-only Phase 1 vocabulary
   (`QuestObjectiveKey/Spec/Source`, `PartyObjectiveMemberState`, `QuestActionPhase`,
   `QuestFailureReason`, `QuestFinisherRef`, `QuestObjectiveRuntime`). No behavior; header compiles on
   its own.

**Verification status:** these are unbuilt — Playerbots must be compiled in a maintenance window.
Fixes 1–2 are exactly the advisor's recommended corrections; the header is inert. The Phase 1A unit
tests (below) are the intended gate.

## 3. Remaining Phase 1 (maintenance-window-gated) — not yet authored

Per the advisor's bounded set, in dependency order:
- **1A resolver Value** — `ActiveQuestObjectiveValue`/`QuestObjectiveResolver` in `QuestValues.*`
  producing a `QuestObjectiveSpec` (direct creature; item via `"item drop list"`; exact finisher via
  the now-correct taker map). Register `"active quest objective"` in `ValueContext.h`.
- **1A runtime** — embed `QuestObjectiveRuntime` in `NewRpgInfo::DoQuest`; reset on
  `ChangeToDoQuest`/`ChangeToIdle`/quest switch; extend `ToString()`.
- **1B strict targeting** — objective-whitelist branch in `GrindTargetValue` before legacy selection;
  self-defense exception tagged separately; deterministic scoring; suppress `SetGrindTarget` in
  `ChooseTravelTargetAction` while an objective lock is active. No in-aggro bypass, no grind fallback.
- **1C exact loot** — objective-specific branch in `LootObjectStack`/`LootAction` (`GetBestForObjective`),
  exact `(questId, family, slot, itemId)` need check, re-read count after loot. **Inspect the local
  corpse-loot delta first** (advisor's pinned-vs-local discrepancy) before changing `LootObjectStack`.
- **1D exact finisher** — resolve from the corrected taker relations, stable `GuidPosition`, exact
  interaction; reuse `TurnInQuest`/`BestRewardIndex`/`CanRewardQuest`; verify reward postcondition.
- **1E no-teleport policy** — formalize `MovementRecoveryPolicy::NoTeleport` for all objective/finisher
  travel (builds on the existing league `IsNoTeleport` hold).
- **1F telemetry** — extend `QuestLogView`/`AutoWowBridge` with the objective/target/loot/finisher/
  progress snapshot (schema in the advisor doc §8).
- **1G maintenance-window live proof** — fixtures q459/q789/q792/q916; release-blocking invariants
  (`unrelated_offensive_target_count==0`, `teleport_count==0`, `direct_quest_db_mutation_count==0`, …).

GoogleTest source-resolution/targeting tests (advisor §8 matrix) are authored alongside 1A–1D and run
via the existing server test runner **in the maintenance window** — they require a build.

## 4. Rollback

Fixes 1–2 are single-hunk edits; `git checkout -- src/Ai/Base/Value/QuestValues.cpp
src/Mgr/Travel/TravelMgr.cpp` reverts them. The new header is removable with no dependents until 1A
references it. Nothing here alters the running server.

## 5. Known unsupported objective categories (unchanged from the matrix)

item-use, escort/event, dungeon/group, generic GameObject-use, directed gathering, and cross-map are
explicit unsupported/blocked states in Phase 1 — Phases 2–4 per the advisor.
