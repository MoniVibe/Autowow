# Phase 1 Finisher Guardrail Report

Status: **R6 narrow edit-only closeout complete; compilation, tests, build, and server execution intentionally prohibited**

Lane: `luna/max-autowow-quest-finisher-transition-v2`

Source lane: `D:\Games\wowstuff\AutoWoW\work\luna-max-autowow-quest-finisher-transition-v2\mod-playerbots`

The deployment checkout and the authoritative shared baseline were not edited. The partial WSL
build tree remains preserved and no finisher-lane compiler, Ninja, or CMake process is running.

## Root causes addressed

1. The exact `QuestParty` path still had a leader-oriented legacy action block. Its broad questgiver
   actions could inspect nearby menus and reach unrelated acceptance/reward behavior. The exact path
   now has no `talk to quest giver`, `accept all quests`, `SearchQuestGiverAndAcceptOrReward`, direct
   `CompleteQuest`, or direct `RewardQuest` call.
2. The Oracle runtime rejected an empty objective lock as soon as all counted objectives were done,
   so its lease ended before a completed/incomplete-but-core-completable quest could execute its
   finisher. The runtime now publishes a bounded `finisherMode` frame under the existing Oracle
   arbiter lease and dispatches a matching tagged finisher intent into the native state machine.
3. The finisher travel branch used `MoveRandomNear` when the exact spawn was missing or movement
   failed. It now waits for a bounded interval and then emits a typed blocker.
4. The finisher binder could fall back to a nearby same-entry object. That could select the wrong
   NPC/GO. It now accepts only the deterministic stable spawn selected by `active quest finisher`.
5. The bridge previously primed only the leader for an explicit quest. It now preflights every party
   member's exact quest ownership and drives each playerbot's own New-RPG directive. The response
   includes per-member exact-quest, exact-finisher, interaction-range, core-completion, and reward
   postcondition receipts.

## R3 Terra NO-GO corrections

1. Oracle ownership is now durable for the full native state/arbiter lease. `NewRpgDoQuestAction`
   rejects untagged New-RPG execution while the central runtime still owns the bot, even if a
   mutable `DoQuest` value was reset or the short-lived native gate expired between cadences.
   Tagged events also require the exact active arbiter decision id.
2. The per-cadence `GateRelease` RAII path was removed. The native gate is renewed at each runtime
   cadence and released only through terminal `ReleaseLease` cleanup: completion, dispatch failure,
   lease expiry, or explicit identity invalidation/replan. Cleanup clears both the central gate and
   the phase-local Oracle lease/finisher authorization markers.
3. Lease reuse now compares an exact `IntentIdentity`: quest id, native phase, objective family/slot
   and payload, target GUID, finisher creature-vs-GameObject kind, signed entry, and stable spawn.
   Any mismatch releases the old lease before planning the current frame, so a current finisher is
   never dispatched under an old decision id.
4. `MoveFarTo` now has a strict deterministic-path option. Exact finisher travel enables it; if
   mmap/direct movement cannot verify a route, the randomized forward-cone sampler is bypassed and
   the finisher state reports `MovementStuckNoTeleport` instead of wandering toward an indirect point.
5. Contract coverage now includes durable gate bypass rejection, exact identity drift/replan, strict
   movement ordering, and a two-member creature/negative-entry-GameObject core-handler flow.

## R4 Terra NO-GO corrections

1. Oracle ownership now begins at `ChangeToDoQuest`, not at first lease acquisition. The
   `QuestObjectiveRuntime::oracleManaged` marker is populated from the configured Oracle allowlist
   at every production `ChangeToDoQuest` call site. An Oracle-managed bot with a directive but no
   lease therefore rejects every untagged New-RPG event, including a completed quest that would
   otherwise enter `DoCompletedQuest`.
2. `QuestParty` no longer directly primes `new rpg do quest` for an Oracle-managed party member.
   It installs the exact directive and strategy, reports `oracle_runtime_pending`, and leaves lease
   acquisition plus the sole tagged dispatch to `AutoWowOracleRuntime`. Unmanaged bots retain the
   ordinary direct-prime behavior.
3. Dispatch authority is centralized in the pure `OracleQuestDispatchPolicy`: initially unleased,
   partially leased, untagged, malformed-tag, stale-decision, and missing-finisher-entry states all
   fail closed. Only a complete active lease/gate tuple plus its exact decision id is accepted.
   Terminal release preserves the managed marker, so unrelated strategy ticks cannot turn the bot
   into an ordinary quest executor after the lease is released.
4. Strict finisher movement now owns explicit provenance: decision id, quest id, stable finisher
   spawn, and the exact `LastMovement` issue stamp and endpoint. Before any wait/continue shortcut,
   `MoveFarTo` rejects inherited or stale movement, stops the current spline, clears MotionMaster and
   `LastMovement`, and replans. Only movement issued by the current strict route can continue.
   `MoveRandomNear` always invalidates strict-finisher provenance.
5. A new executable behavior-policy test file covers initial unleased rejection, exact active-lease
   tag enforcement, stale phase/target/finisher identity, inherited random movement, terminal
   release, and independent creature plus negative-entry GameObject member dispatch. These tests
   were authored and registered but **not executed** under the R4 edit-only directive.

## R5 narrow closeout

1. Strict deterministic finisher movement is now explicitly opt-in. Production enables it only
   when `oracleFinisherAuthorized` is true and `oracleFinisherDecisionId` is nonzero. Legacy
   non-Oracle finisher movement continues through the historical `MoveFarTo` behavior, including
   its ordinary fallback path; it does not inherit Oracle provenance enforcement.
2. `StrictFinisherMovementPolicy::IsEnabled` is the shared production/test seam. Its executable
   policy case proves all unauthorized or zero-decision combinations remain off and only an
   authorized nonzero Oracle decision enables strict mode.
3. Registered `QuestFinisherPartyContractTest` no longer contains obsolete R3 source-string tests.
   The meaningful strict parser and exact intent-identity tests remain. Runtime authority and
   movement behavior live in `QuestFinisherBehaviorPolicyTest`; live core-handler behavior remains
   a future integration proof rather than being misrepresented by source grep.

## R6 legacy/default-neutral restoration

1. Baseline `SearchQuestGiverAndAcceptOrReward` behavior, including its ordinary quest-log
   organization, is restored after dispatch classification for the unmanaged `Ordinary` branch.
   Initially-unleased managed events still reject before this call, and active Oracle work still
   enters only the exact tagged branch.
2. Oracle exact finishers retain strict deterministic routing and no-teleport recovery only when
   `oracleFinisherAuthorized` is true and the decision id is nonzero. Unmanaged finishers pass
   `questNoTeleport=false`, `deterministicPath=false`, and explicitly allow baseline teleport
   recovery even if a stale/external AutoWow party no-teleport marker remains. Other `MoveFarTo`
   callers keep the existing no-teleport-policy default.
3. Executable pure-policy cases cover managed rejection versus unmanaged legacy maintenance, plus
   all strict/no-teleport/legacy-recovery opt-in combinations. Narrow source-wiring contracts verify
   the production action classifies authority before broad legacy maintenance and passes the policy
   outputs into the finisher movement call. These tests are authored but not run.

## Implementation delta

- `src/AutoWow/AutoWowBridge.cpp`
  - Removed broad exact-party questgiver actions.
  - Added all-member exact quest preflight; unmanaged members may be directly primed, while
    Oracle-managed members wait for runtime lease acquisition and tagged dispatch.
  - Added `member_receipts` and aggregate completion/finisher fields.
- `src/Ai/World/Rpg/Action/NewRpgAction.cpp`
  - Added durable arbiter-plus-native-gate enforcement for every Oracle-owned quest action.
  - Requires the exact current decision id for numeric and finisher tags.
  - Uses the exact involved quest relation and ordinary AzerothCore request/reward handlers.
  - Records finisher receipt milestones and removes finisher random movement/fallback binding.
- `src/Ai/World/Rpg/Action/NewRpgBaseAction.{h,cpp}`
  - Adds `deterministicPath` and exact route identity to `MoveFarTo`; exact finisher travel rejects
    inherited movement, cannot use the random forward-cone fallback, and fails through a typed
    no-teleport blocker.
- `src/Ai/World/Rpg/StrictFinisherMovementPolicy.h`
  - Defines the pure exact-route and exact-`LastMovement` provenance decision seam.
- `src/Ai/World/Rpg/NewRpgInfo.{h,cpp}`
  - Carries and resets strict-finisher movement provenance and marks Oracle management when an exact
    quest directive is installed.
- `src/Ai/World/Rpg/Action/NewRpgAction.h`
  - Documents exact stable-spawn-only finisher binding.
- `src/Ai/World/Rpg/QuestObjectiveContext.h`
  - Adds durable Oracle lease markers, finisher authorization state, and `QuestFinisherReceipt` evidence.
- `src/Ai/Base/Value/QuestValues.cpp` and `src/AutoWow/QuestFinisherTransitionPolicy.h`
  - Keep `INCOMPLETE + CanCompleteQuest + exact finisher` distinct from ordinary outstanding work;
    `COMPLETE` remains a turn-in directive.
- `src/AutoWow/AutoWowOracleFinisherIntent.h`
  - Adds strict encode/parse for
    `finisher:<decision-id>:<quest-id>:<signed-entry>:<stable-spawn-guid>`.
- `src/AutoWow/AutoWowOracleRuntime.cpp`
  - Adds bounded finisher frames and leases, exact tagged dispatch, durable gate renewal/cleanup,
    exact intent identity comparison, stale-lease replan, and Oracle finisher receipts.
- `src/AutoWow/AutoWowOracleRuntime.h`
  - Defines the testable exact intent identity and exposes fail-closed lease queries used by the
    native action gate.
- `src/AutoWow/OracleQuestDispatchPolicy.h`
  - Defines the pure initially-unleased, active tagged lease, and terminal-release authority rules
    used by both the bridge and New-RPG action.
- `src/AutoWow/QuestLogView.cpp`
  - Publishes finisher receipt identity and postconditions, including new Oracle failure names.
- `tests/QuestFinisherPartyContractTest.cpp`
  - Retains pure wire-parser and exact lease-identity contracts. Obsolete source-string assertions
    were removed in R5.
- `tests/QuestFinisherBehaviorPolicyTest.cpp`
  - Adds executable behavior-level policy cases for the six R4 acceptance boundaries. This is real
    behavior of the production policy seams, plus explicit strict-mode opt-in/default-neutral
    coverage. It is not a live-world/core-session integration test and has not been executed.
- `mod-playerbots.cmake`
  - Registers the new test only behind `BUILD_TESTING`.

## Static proof completed in R4

- `git diff --check`: passed after the R4 edits.
- Exact `QuestParty` source slice: no broad questgiver calls and no direct quest grant/completion or
  reward calls; member receipts and all-member reward proof are present.
- `DoCompletedQuest` source slice: no `MoveRandomNear`; exact relation and
  `HandleQuestgiverRequestRewardOpcode` are present; strict deterministic travel is selected.
- Oracle runtime/action source: durable `RequiresTaggedDispatch`/`HasActiveLease` enforcement,
  tagged-source checks, no per-tick gate release, exact identity replan, and terminal gate cleanup
  are present.
- Production wiring checks confirm the bridge and action both call `OracleQuestDispatchPolicy`, all
  production `ChangeToDoQuest` sites carry the managed marker, and only the runtime emits the tagged
  event for managed bots.
- Movement ordering checks confirm strict provenance evaluation and inherited-motion clearing occur
  before `IsWaitingForLastMove`, and deterministic finisher routing returns before the random cone.
- The behavior-policy test source contains all six requested executable cases and is registered
  behind `BUILD_TESTING`. This confirms test presence and wiring only, not test success.
- No finisher-lane processes remain.

## R5 static closeout

- `git diff --check`: passed after the R5 edits.
- Registered finisher tests contain no filesystem/source-reader or source-string assertions.
- Production strict-mode selection calls `IsEnabled(oracleFinisherAuthorized,
  oracleFinisherDecisionId)` and passes that result as `deterministicPath`.
- The executable policy source covers legacy unauthorized, authorized-with-zero-decision, and
  authorized-with-nonzero-decision behavior.
- These are diff/static results only. No executable test result is claimed.

## R6 static closeout

- `git diff --check`: passed after the R6 edits.
- Static branch assertions confirm managed rejection cannot reach legacy quest maintenance, while
  unmanaged `Ordinary` dispatch can call the baseline helper.
- Static movement assertions confirm only authorized nonzero Oracle finishers request strict and
  no-teleport behavior; unmanaged finishers request legacy teleport recovery.
- Registered policy/source contract cases cover both branch families. They remain unexecuted under
  the edit-only instruction.

## Behavioral proof status

- **Authored, not run:** `QuestFinisherBehaviorPolicyTest` exercises production pure-policy behavior
  for initially-unleased rejection, active-lease tagging, stale identity, inherited random movement,
  terminal release, and two independently leased creature/negative-entry-GO members.
- **Not yet proven:** compilation, GoogleTest execution, a real AzerothCore session invoking the
  creature and GO core handlers, party-member reward persistence, and live receipts.
- `QuestFinisherPartyContractTest` now contains pure parser/identity contracts only. It is registered
  but unexecuted, so no pass result is claimed.

## Not run by explicit instruction

- No CMake configure, Ninja, compiler, link, worldserver restart, or live quest proof was started.
- The new tests are registered but not executed because R4 explicitly forbids tests as well as
  configure/build/server actions.

## Residual risks / next proof

- A fresh maintenance-window build may expose API or ABI issues that static review cannot catch.
- The R6 changes have not been compiled or test-executed under Terra's NO-GO instruction; the
  preserved WSL build remains paused and must not be restarted implicitly.
- The live proof must exercise both a creature and a negative-entry GameObject finisher, with at least
  two party members, and capture `member_receipts` showing exact quest ownership, stable finisher,
  interaction range, core completion, and reward confirmation for each member.
- The Oracle handoff is one bounded finisher step per exact phase lease; movement and core handler
  effects remain asynchronous and must be observed over multiple ticks. Phase/target changes should
  produce a new decision id in live receipts.
- Existing generic non-QuestParty RPG paths retain their legacy questgiver behavior; this change
  scopes the removal to the exact directed path required by the acceptance gate.

## Resume gate

Wait for explicit authorization before running tests, touching the preserved build tree, starting
compilation, or exercising the server.
