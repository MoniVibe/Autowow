# AutoWoW Autopilot — Acceptance Scenarios V1

Schema: `autowow.autopilot.acceptance.v1`
Companion documents: `AUTOWOW_AUTOPILOT_V1_PLAN.md`, `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`

These scenarios are machine-checkable. A verifier consumes only:

1. the Autopilot's append-only JSONL receipts (`autopilot\state\receipts\*.jsonl`, schema `autowow.autopilot.receipt.v1`);
2. read-only bridge telemetry (`snapshot`, `questlog`, `questobjective`, `acceptance`, `combatlog`, `encounterlog`, `wsg-status`, `raid-status`, `fixture status`) on `127.0.0.1:18787`;
3. SELECT-only SQL snapshots (same guarded idiom as `scripts\QuestLogSource.ps1:72` `Invoke-QuestDbSelect`; password via `MYSQL_PWD` in a `finally`, never persisted).

Verdict rules follow `PHASE1_ACCEPTANCE_VERIFIER.md` exactly:

- **PASS** only when every required evidence item is present, versioned, and every invariant holds.
- **INCONCLUSIVE** when evidence is missing, unversioned, or ambiguous. Missing evidence is never success.
- **FAIL** when required evidence is present and wrong, or any forbidden action counter is nonzero.

## Global invariants (checked in every scenario)

| Invariant | Evidence source | Requirement |
|---|---|---|
| No teleport as travel/recovery | receipts + worldserver log scan | `teleport_count == 0` inside the measurement window (lab fixture provisioning before the window is exempt and must be receipted as `lab_fixture_setup`) |
| No direct DB character mutation | receipts + SQL audit | `direct_db_mutation_count == 0`; every Autopilot SQL statement logged with `statement_kind == "select"` |
| No hidden grinding fallback | receipts | `random_grind_fallback_count == 0`; a stalled objective escalates per retryPolicy, never degrades to generic grinding |
| Capability confinement | receipts | every `command` receipt's capability ∈ the job's `permittedCapabilities`; `capability_violation_count == 0` |
| No duplicate irreversible commands | receipts | for each idempotency key of an irreversible command (`party`, `quest`, `quest-acquire`, `raid-create`, `wsg-queue`, mail/economy ops), at most one receipt with `sent == true` |
| Success is objective progress | receipts + telemetry | job `Completed` requires every `successCriteria.counters` key satisfied by cited evidence receipts; movement, XP gain, or elapsed time alone never satisfy a counter |
| Secrets absent | receipt files | no password, connection string, or session credential appears in any receipt or report (env-var names only) |
| Allow-list enrollment | receipts | every controlled GUID ∈ the job's `characters.eligibleGuids` and the controller's global allow-list; `unenrolled_control_count == 0` |

## Scenario A — Quest

**Intent.** A character (party leader) accepts, performs, and turns in a supported quest with objective-counter evidence.

**Preconditions.** Worldserver + bridge up (`autowow-bridge-test.ps1` clean); leader GUID enrolled; quest in the supported capability classes (`scripts\QuestDirectorLib.ps1` `Test-CapabilitySupported`); no other gameplay mutator owns the roster (director/guardian paused or leader outside their leader set).

**Jobs.** One `QuestLevel` job, `permittedCapabilities` ⊇ `[bridge.snapshot, bridge.questlog, bridge.questobjective, bridge.acceptance, bridge.quest, bridge.quest.acquire, bridge.recover, sql.read]`.

**Machine-checkable PASS evidence.**

1. `job_transition` receipts traverse `Queued → Validating → Preparing → Executing → Verifying → Completed` (Traveling optional if already positioned).
2. An `observation` receipt records the baseline: quest id Q, objective index k, `current_baseline` from `questobjective <leader>`.
3. A later `observation` receipt shows the same (Q, k) with `current > current_baseline`, and eventually `current == required` (mirrors the q792 `8/8 complete` proof convention in `QUEST_TYPE_COVERAGE_REPORT.md`).
4. Turn-in proof, either: `acceptance <leader> <Q>` response with the reward postcondition confirmed (`PHASE1_ACCEPTANCE_VERIFIER.md` `reward_postcondition_confirmed == true`), or a SELECT snapshot pair showing Q absent from `character_queststatus` and present in `character_queststatus_rewarded` after the window.
5. `progress.counters.questsTurnedIn >= successCriteria.counters.questsTurnedIn`.

**FAIL.** Unrelated-target or grind fallback observed; teleport observed; turn-in claimed without item 4.
**INCONCLUSIVE.** Objective counter never observed (e.g. the known acquisition defect signature `QUEST_ACQUIRE_NO_LOG_NO_MOVEMENT_AFTER_ONE_RETRY`): job must end `Blocked('recovery-budget-exhausted' | 'phase-timeout')` with the signature recorded — that is a correct Autopilot outcome and an INCONCLUSIVE scenario, never a silent pass or a grinding fallback.

**Status today.** Runnable live. Known upstream risk: quest acquisition defect (top open gate in `OVERNIGHT_STATUS_20260717.md`); turn-in `complete → rewarded` not yet proven at scale.

## Scenario B — Gathering

**Intent.** A character travels normally, gathers exactly the requested resource, and stops at the requested count.

**Jobs.** One `Gather` job `{item: {itemId, count}, stopAtCount}`, requiring capability `oracle.gather_source` (+ `oracle.gather_route` for routed travel).

**Machine-checkable PASS evidence.**

1. Baseline and final SELECT snapshots of `character_inventory ⋈ item_instance ⋈ item_template` for the target itemId (idiom of `docs\RAID_LOOT_SNAPSHOT_RUNBOOK.md` / `GATHERING_PROOF_PLAN.md`): `final_count - baseline_count == stopAtCount` requested delta, with **no negative delta** and no confounding source (no vendor/mail/AH receipts in the window).
4. Per-node receipts from the Oracle gathering executor (`GatheringReceiptPolicy.h`) correlating each increment to a node interaction — closes the `GATHER_SUCCESS_NO_PERSISTED_DELTA` gap in `PERSISTENT_GATHERER_AUDIT_20260716.md`.
2. A `command` receipt shows the stop order issued at exactly `stopAtCount`, and no gather command after it.
3. Gathering-skill rank non-regressing (`character_skills` snapshot pair, `GATHERING_PROGRESSION_V0.md` gate).

**Status today. BLOCKED — represented but disabled.** The fixed-target gathering executor (`OracleGatherExecutor`) is source-only; the Oracle runtime's verified-operation allowlist (`AutoWowOracleContract.h:703` `IsVerifiedOperationTuple`) authorizes only `quest_objective`. The Autopilot must keep Gather jobs `Blocked('oracle-capability-not-deployed')` and must not emulate gathering through travel + pray. This scenario activates only after Sol deploys the native gathering Oracle.

## Scenario C — Profession

**Intent.** A character obtains prerequisites, trains or crafts normally, and inventory/skill changes prove success.

**Jobs.** `TrainProfession {professionSkillId, targetSkill}` then `Craft {recipeSpellId, count}`; prerequisites expressed as typed `prerequisites` (money, reagents via `item-present`).

**Machine-checkable PASS evidence.**

1. `character_skills` snapshot pair: skill value reaches `targetSkill` (train) / increments consistent with crafting.
2. `character_spell` snapshot shows the recipe spell known (train) — `recipe-known` measure.
3. Inventory snapshot pair proves reagent consumption and crafted-item creation (`count` new items, correct itemId), no negative unexplained deltas.
4. Money delta consistent with trainer/vendor costs only (`gold-delta-at-vendor` measure), receipted.

**Status today. BLOCKED.** No bridge command or deployed Oracle executor performs training, crafting, vendor purchase, or bank/mail economy (`oracle.craft`, `oracle.vendor_buy`, `oracle.bank_*`, `oracle.mail_*` are contract enums only; bridge has no equivalent). Jobs stay `Blocked('oracle-capability-not-deployed')`. `LEAGUE_V0_REPORT.md` confirms the economy-worker loop is unimplemented.

## Scenario D — Five-player dungeon

**Intent.** The planner forms a valid party, travels to the entrance, completes the run, loots normally, records deaths and boss kills.

**Jobs.** `FormParty {roles: [tank,healer,dps,dps,dps]}` → `Dungeon {name: "Utgarde Keep", roles}` (UK is the proven reference; `DUNGEON_RAID_PROGRESS_REPORT.md`).

**Machine-checkable PASS evidence.**

1. Party proof: `party <leader> <members...>` ok-response receipt + `snapshot` observations showing all five share the leader's group.
2. Role assignment receipt: chosen tank/healer/dps GUIDs with the capability-profile facts (class/spec) that justified each seat.
3. Travel: `route`/`advance`/`advancepoint` receipts, each arrival verified by snapshot position within `arrivalRadius`; all members one `instance_id` before the first pull (`scripts\dungeon-director.ps1:207` convention).
4. Boss kills: SQL `instance.completedEncounters` bitmask delta == expected bosses (UK full clear = 7 per the proven run), plus `encounterlog` receipts per encounter.
5. Deaths: telemetry `bot_died`/`bot_revived` events recorded into `progress.counters.deaths` (zero not required — recorded, honestly).
6. Loot: inventory snapshot pair (raid-loot-snapshot idiom) listing `new_item_guids`; no item grants outside loot windows.

**Status today.** Runnable live via bridge (`party`, `route`, `advance`, `advancepoint`, `engage`, `boss`, `combatlog`, `encounterlog`) — proven by the UK clear and `dungeon-director.ps1`. The Autopilot adds job persistence + role planning on top.

## Scenario E — Raid

**Intent.** An adequately geared, role-balanced raid stages, attempts bosses, distributes loot, records encounter receipts.

**Jobs.** `FormParty`(×2 groups via `raid-create`) → `Raid {name: "Vault of Archavon", rosterSize: 10, difficulty}`.

**Machine-checkable PASS evidence.**

1. Roster proof: `raid-create` ok receipt with exactly `rosterSize` unique GUIDs; `raid-status` observation shows the raid composition and difficulty.
2. Gear adequacy receipt: capability profile per member (equipped ilvl from SELECT snapshot) meets the job's stated floor; broken-gear members excluded with a `roster_substitution` receipt.
3. Encounter receipts: `encounterlog` + `combatlog` per attempt; boss kill proven by `instance.completedEncounters` delta (VoA Archavon+Emalon is the proven reference, zero-death PASS in `DUNGEON_RAID_PROGRESS_REPORT.md`).
4. Loot distribution: loot snapshot pair with `new_item_guids` and `equipped_slot_replacements` (`docs\RAID_LOOT_SNAPSHOT_RUNBOOK.md`), all via normal loot.
5. Wipe honesty: a failed attempt (e.g. Onyxia air phase, known FAIL) produces `attempt_failed` receipts with deaths recorded and the job `Blocked`/retrying per policy — never a fabricated kill.

**Status today.** Runnable live for VoA-class encounters; Onyxia remains a known-fail probe. Raid formation beyond the bridge `raid` family (e.g. summoning, consumable distribution) is not automated.

## Scenario F — PvP

**Intent.** A legal team queues normally, participates, and records real objectives/deaths/healing/damage/outcome without fabricating honor.

**Jobs.** `Battleground {name: "wsg", teamSize: 10}` (reference: `WSG_10V10` proof, 57/57 kills, 2 captures).

**Machine-checkable PASS evidence.**

1. Queue proof: `wsg-queue` ok receipt with exactly 20 unique enrolled GUIDs (`scripts\autowow-control.ps1` roster validation), both factions receipted.
2. Match proof: `wsg-status` observations transition queued → in-progress → ended; final scoreboard receipt records kills, deaths, healing, damage, flag captures per character, and the winner.
3. Outcome recorded into `progress.counters` (`battleground-scoreboard` measure); honor/XP deltas read from SELECT snapshots only — the Autopilot writes no honor, ever (`no fake honor` invariant).
4. Leave/cleanup: `wsg-leave` receipts return the roster to its prior state; `snapshot` confirms no character stuck in the BG map.

**Status today.** Runnable live (`wsg-queue|wsg-status|wsg-leave` + role fixtures). `OpenWorldPvp` via `autowow_league_challenge` rows + world-PvP fixture remains semi-manual; the Autopilot may record challenges but tactical execution depends on `oracle.pvp_objective` (not deployed).

## Scenario G — Recovery (controller restart)

**Intent.** Restart the Autopilot mid-job and prove reconciliation without duplicate irreversible operations.

**Procedure.**

1. Start a `QuestLevel` job (dry-run mode is sufficient to prove the property; live run proves it stronger). Let it reach `Executing` with at least one irreversible command receipted (`quest <leader>` — in dry-run, `sent=false` recorded with its idempotency key).
2. Kill the controller process (`Stop-Process`, no graceful shutdown).
3. Restart the controller.

**Machine-checkable PASS evidence.**

1. Single-owner proof: the restarted controller detects the stale `autopilot.pid.json` (dead PID), removes it, and acquires ownership; receipts `controller_started` → `stale_lock_recovered`.
2. Reconciliation receipts: `reconciliation_started`, then for each non-terminal job a `job_reconciled` receipt citing (a) the persisted job document, (b) the receipt log tail, (c) a fresh live observation (`snapshot`/`questobjective` in live mode; recorded fixture in dry-run).
3. The job resumes in a phase consistent with observed state — never restarts from `Queued`, never re-runs a completed phase.
4. Idempotency proof: across the whole receipt log, every irreversible-command idempotency key has ≤1 `sent == true` receipt (dry-run: ≤1 `would_send` per key unless a receipted `order_expired`/`retry_authorized` intervenes).
5. No duplicate party invites, crafting batches, auctions, mail, or Oracle commands: `duplicate_irreversible_command_count == 0`.

**FAIL.** Any duplicate irreversible command; a resumed job whose phase contradicts live state; two controller instances holding the lock simultaneously.
**Status today.** Fully runnable in dry-run with the v1 slice; live variant once live mode is enabled.

## v1 slice acceptance (what must pass before anything goes live)

The minimal implementation ships with these offline checks green:

1. Schema validation: every example job in `autopilot\tests\fixtures\` validates against `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`; malformed jobs are rejected with typed errors.
2. State machine: every legal transition emits exactly one receipt; illegal transitions throw; `Blocked` always carries a `blockedReason.code`.
3. Scenario A dry-run: a `QuestLevel` job walks the full phase path against recorded bridge fixtures, emitting `would_send` commands with stable idempotency keys, and the exact wire strings match `autowow-control.ps1 -EmitRequestOnly` output for the same arguments.
4. Scenario B representation: a `Gather` job validates, then immediately transitions `Queued → Validating → Blocked('oracle-capability-not-deployed')` with a receipt naming the missing capability `oracle.gather_source`. It never emits a bridge command.
5. Scenario G dry-run: kill/restart harness proves reconciliation and idempotency as above.
