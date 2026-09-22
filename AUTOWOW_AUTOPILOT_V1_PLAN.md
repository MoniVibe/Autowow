# AutoWoW Autopilot V1 — Audit and Design Plan

Date: 2026-07-18
Author: Fable (user-facing Autopilot agent)
Companion deliverables: `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`, `AUTOWOW_AUTOPILOT_ACCEPTANCE_V1.md`

The Autopilot is the persistent strategic layer: it turns player/campaign intent into long-running typed jobs, selects characters, plans prerequisites, schedules and retries, measures objective progress, and explains itself. It delegates every tactical action to the server-side Oracle and the AutoWow bridge. It never presses buttons, scrapes screens, injects memory, fabricates packets, or runs a competing combat engine.

Everything below was verified in source on 2026-07-18. Paths are relative to `D:\Games\wowstuff\AutoWoW` unless absolute. The shared module worktree `_phase1_worktree\mod-playerbots` (branch `phase1-objective-lock`, ~99 dirty files) was treated strictly read-only.

---

## 1. Existing control-surface inventory (verified in source)

### 1.1 The AutoWow bridge (the only live command surface)

- Implementation: `_phase1_worktree\mod-playerbots\src\AutoWow\AutoWowBridge.cpp` (3608 lines). Started from `src\Script\Playerbots.cpp:424`.
- Transport: TCP `127.0.0.1` only (`Run()` binds `address_v4::loopback()`, AutoWowBridge.cpp:3586), port from `AutoWow.BridgePort` (default 18787; deployed value 18787 in `server\configs\modules\playerbots.conf`). One UTF-8 request line per connection (`read_until '\n'`, ServeClient at :3529), max 1024 bytes (`request_too_large`), one JSON response line, then close.
- Concurrency model: every command is parsed on the bridge thread (`ParseRequest`, :3079-3505), queued through `PlayerbotWorldThreadProcessor` (queue 10000, batch 100), and answered after world-thread execution or a 2000 ms timeout (`AUTO_WOW_RESPONSE_TIMEOUT_MS`, :102). Errors are always `{"ok":false,"error":"<code>"}` (`ErrorResponse`, :365).
- Command grammar (exact, from `ParseRequest`):
  - Read-only: `list`, `destinations <guid>`, `snapshot <guid>`, `combatlog <guid>`, `encounterlog <guid>`, `questlog <guid>`, `questobjective <guid>`, `acceptance <guid> [quest-id]`, `boss status <leader>`, `wsg-status`, `raid-status`, `fixture status <guid>`, `scout <guid>`, `pathprobe <guid> x y z [sx sy sz]`.
  - Mutating: `activate|deactivate <guid>`, `party <leader> <m1..m4>`, `rally <leader>`, `deploy <guid>` (party_grind or worker_gather mode), `route <leader> <name>`, `advance <leader> <waypoint>`, `advancepoint <leader> x y z [o]`, `engage <leader> entry <id> | player <guid>`, `boss <leader>`, `quest <leader> [acquire | <quest-id>]`, `recover <leader>`, `pause|resume <guid>`, `travel <guid> <destination|random>`, `wsg-queue/wsg-leave`, `raid-create/raid-leave`, lab-only `probe-reset` and `fixture init`.
  - `observe <watch|relocate|status|protect|release>` is a separate default-closed camera surface gated by `AutoWow.ObserverGuids` (deployed: `"21,40454"`).
- Server-side gates the Autopilot can rely on (fail-closed): league enrollment required for state-changing orders (`IsLeagueMember` SELECT against `acore_playerbots.autowow_league_member`, AutoWowBridge.cpp:372 → `bot_not_enrolled_in_league`); paused bots reject orders (`bot_paused`); `party` rejects duplicates, offline members, cross-faction (:1197); real players are refused throughout; requests time out rather than execute late.
- No-teleport policy: `AutoWowPolicy::SetNoTeleport/IsNoTeleport` (AutoWowBridge.cpp:82-95), enforced in `NewRpgBaseAction::MoveFarTo` (`src\Ai\World\Rpg\Action\NewRpgBaseAction.cpp:37`, hold-and-report at :112-120); violations would trip `AutoWowAcceptance::NoteTeleport()` counters (schema `autowow.phase1.acceptance.evidence.v1`, all counters must stay 0).

### 1.2 Oracle runtime and typed contracts (built, mostly not yet live)

- Contract: `src\AutoWow\AutoWowOracleContract.h` (2063 lines) — `OperationCode` (:151, snake_case names at :178), `Decision` (:749), `IntentLease` (:794), `Receipt` (:800) with `EvidenceCounters {objective,progress,resource,combat,healing,deaths,pvp,failures}` (:468), `ActiveLeaseSnapshot` (:842), `Scope{PersistentCampaign|LabFixture}` (:276), and the `OracleArbiter<256>` lease/reservation engine (:1392) with resource precedence `Idle < QuestGather < Transition < CombatPositioning < Recovery` (:307).
- Runtime: `src\AutoWow\AutoWowOracleRuntime.{h,cpp}` — default-off world-thread driver called from `Playerbots.cpp:437`. Config `AutoWow.OracleRuntime.{Enabled=0, BotGuids="", CadenceMs=1000, MaxBots=1, LeaseTtlTicks=3}` (playerbots.conf.dist:2176-2180; **absent from the deployed conf → off in production**). One native step per cadence tick, no catch-up loop, GUID allowlist, refuses real players.
- **The single live authority**: `IsVerifiedOperationTuple` (AutoWowOracleContract.h:703) admits only `Domain::Quest + IntentCode::QuestObjective + LeaseResource::QuestGather + OperationCode::QuestObjective`, dispatched as exactly one `"new rpg do quest"` action via `OracleQuestExecutor` (`Runtime::NativeDispatch`, AutoWowOracleRuntime.cpp:516). `OracleGatherExecutor` and `OraclePvpCombatExecutor` are complete, unit-tested, and **not wired** (present only in cmake + tests + own files). Oracle receipts land in an in-memory `ReceiptRing` (cap 64, AutoWowOracleRuntime.h:28) — **not exported anywhere**.
- Ownership: `AutoWowOracleOwnershipGate.{h,cpp}` — bounded native claim registry (`Claim/Release/Owns`), event source tag `"autowow.oracle"`.

### 1.3 Quest and objective telemetry

- Read-only views: `src\AutoWow\QuestLogView.{h,cpp}` behind bridge `questlog` / `questobjective` / `acceptance`. Objective identity is `(questId, family, slot)` per `QuestObjectiveSpec` (`src\Ai\World\Rpg\QuestObjectiveContext.h:105`), with the durable in-RAM `DirectGameObjectReceipt` ledger (cap 64).
- `quest` command response carries `phase: acquire|objective|turnin`, `quest_id`, `quest_status`, `destination`, `accept_attempts`, `turnin_attempts` (AutoWowBridge.cpp:2924); the acquire path returns rich movement/giver diagnostics (`QuestAcquisitionResponse`, :887).
- PowerShell normalization: `scripts\QuestLogSource.ps1` — `Send-Bridge` (:15, the canonical socket client), `New-QuestDbContext` (:39), guarded SELECT-only `Invoke-QuestDbSelect` (:72), `Get-NormalizedQuestLog` (:168, bridge-first with save-lagged DB fallback).
- Decision library: `scripts\QuestDirectorLib.ps1` — `Test-CapabilitySupported`, `Select-DirectorQuest`, `Get-DirectorQuestProgressDecision`, `Get-AcquireDecision`, backoff ledger trio.

### 1.4 League/campaign scripts (the operational layer to compose, not duplicate)

- DB authority: `scripts\league-simulation.ps1` (install/seed/launch/status/record-quest-state/challenges; writes the `autowow_league_*` tables; `-PreserveOtherFleets` keeps concurrent rosters online).
- Quest Director V1: `scripts\league-simulation-director.ps1` + `start-league-simulation-director.ps1` — the current per-party quest loop (120 s no-progress guard, `MaxRecoveriesPerQuest 2`, backoff ledger `leagues\results\quest-director-backoff.json`, receipts `leagues\results\quest-director-v1-<stamp>.jsonl`, `-ObserveOnly` mode).
- Supervisor: `scripts\persistent-campaign-supervisor.ps1` (adopt-don't-kill, exclusive `LockPath`, dry-run default) + guardian/progression/sanity observers. Campaign state: `work\persistent-campaign-supervisor\state.json`. As of 2026-07-18 the campaign processes are stopped; WSL worldserver + Windows authserver/mysqld are live (`work\phase1-wsl-runtime\runtime-processes.json`).
- Operator façade to emulate: `scripts\autowow-ops.ps1` (Plan default, `-Apply` for mutations, state doc `work\autowow-ops\quest-director.json`).

### 1.5 Party, dungeon, raid probes

- `scripts\dungeon-director.ps1` (UK waypoint route, `-Apply` gated, instance-id cohesion check at :207), `dungeon-fixture-*`, `raid-readiness-proof*`, `raid-loot-snapshot.ps1` (SELECT-only inventory join), `probe-lab*`, `probe-rotation*` (RETIRED/QUARANTINED/RETEST policy per `PROBE_ROTATION_POLICY.md`), `wsg-10v10-proof*`, `pvp-role-fixture*`.
- C++ handlers: `WarsongFixtureControl.cpp` (wsg family), `RaidFixtureControl.cpp` (raid family), `BossApproachControl.cpp`, `ExactBossTargetControl.cpp`, `AdvanceFormation.cpp`, `ProbeResetControl.cpp`, `FixtureFactoryControl.cpp` (fixtures gated by deployed `AutoWow.FixtureGuids`, 54 GUIDs).

### 1.6 Gathering and economy scripts

- Implemented worker path (live): bridge `deploy` (solo) → `AutoWowGather::ActivateWorker` + `SetNoTeleport(true)` + strategy `+worker gather` (AutoWowBridge.cpp:1462-1466); state/telemetry `src\Ai\World\Gathering\GatheringWorkerState.cpp`, surfaced in `snapshot` as the ~30-field `gather_route` block (AutoWowBridge.cpp:486-515). Config `AutoWow.GatherSeek.*` deployed on.
- Scripts: `gathering-planner.ps1` (pure), `gathering-orchestrator.ps1` (`-Apply -ConfirmLiveBridge` double gate), `gathering-telemetry.ps1`, `gathering-proof.ps1`, `gather-lane.ps1`, `persistent-gatherer-status.ps1` (receipt schema `autowow.persistent-gatherer-status.v1`).
- Known gap: `GATHER_SUCCESS_NO_PERSISTED_DELTA` (`PERSISTENT_GATHERER_AUDIT_20260716.md`, 07-17 lab evidence) — events without proven inventory delta; the fixed-target `OracleGatherExecutor` exists to close this but is not wired.

### 1.7 Server lifecycle and health

- `scripts\start-server.ps1`/`stop-server.ps1` (bare `worldserver.pid`/`authserver.pid`), `start-mysql.ps1`/`stop-mysql.ps1` (`MYSQL_ROOT_PASSWORD` process-only), `smoke-test.ps1 [-RequireBots]`, `autowow-bridge-test.ps1`, WSL variants (`start-phase1-wsl-worldserver.ps1`, relay `phase1-wsl-mysql-relay.ps1`, manifest `work\phase1-wsl-runtime\runtime-processes.json`).
- The Autopilot must not deploy/restart/reconfigure the server (assignment constraint); it only checks health read-only.

### 1.8 Existing JSONL logs and receipts (wire-format precedents)

- Record idiom: `{ "timestamp_utc": "<ISO-8601 o>", "event": "<name>", ...fields }`, `ConvertTo-Json -Compress`, `[System.IO.File]::AppendAllText(path, line + newline, UTF8Encoding($false))` (e.g. `league-simulation-director.ps1:59 Write-Receipt`).
- Versioned schemas in live receipts: `autowow.probe-lab.receipt.v1`, `autowow.overnight-guardian.v1`, `autowow.persistent-gatherer-status.v1`, `autowow.persistent-campaign-supervisor`, `autowow.phase1.acceptance.evidence.v1`, `autowow.campaign.evidence.v1`.
- Locations: `leagues\results\*.jsonl` (campaign), `logs\**\*.jsonl` (agents, labs, probes, guardian).
- PID convention: `*.pid.json` `{pid|process_id, started_utc, ...context}` with stale detection via `Get-Process`; stop scripts verify `Win32_Process.CommandLine` before killing (`stop-autowow-agent.ps1:20-24`).
- Database: `acore_playerbots.autowow_league_*` (8 tables, DDL `azerothcore-wotlk\modules\mod-playerbots\data\sql\playerbots\custom\2026_07_12_00_autowow_league_v0.sql`). C++ reads only `autowow_league_member`; `autowow_league_quest_state` is written by `league-simulation.ps1` only; `_ledger`/`_contract` are schema-only.

### 1.9 What is genuinely missing today (never fake these)

| Capability | Status | Evidence |
|---|---|---|
| Oracle quest_objective execution | Built, default-off, allowlisted, not enabled in deployed conf | AutoWowOracleRuntime.cpp:326; deployed playerbots.conf lacks the keys |
| Oracle gather_source / gather_route | Library + tests only; runtime allowlist blocks it; no native callback | OracleGatherExecutor.h:230-233; AutoWowOracleContract.h:710 |
| Oracle craft / vendor / bank / mail / auction | Contract enums + pure policy headers only | AutoWowOracleGatherCraftEconomyPolicy.h; no executor, no bridge command |
| Oracle pvp_objective / pvp_challenge | `OraclePvpCombatExecutor` unwired; WSG works via bridge fixture path instead | audit §7; WarsongFixtureControl.cpp |
| Oracle receipt export | In-memory ring only (64) | AutoWowOracleRuntime.h:28 |
| Bridge follow/attack/loot | Not implemented | AUTOWOW_BRIDGE.md; ParseRequest has no such verbs |
| Profession training | No surface at all | audit §1.6/§4 |
| Quest turn-in at scale | `complete→rewarded` not yet proven; acquisition defect open | OVERNIGHT_STATUS_20260717.md gate 1 |

---

## 2. Stable Autopilot job model

Defined normatively in `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json` (JSON Schema 2020-12, `schemaVersion: 1`). Summary:

- 15 kinds: QuestLevel, Gather, Craft, TrainProfession, VendorRepair, BankDeposit, MailTransfer, FormParty, Travel, Dungeon, Raid, Battleground, OpenWorldPvp, Restock, Idle.
- Envelope (all required): `jobId` (`job-<utcstamp>-<8hex>`), `idempotencyKey` (SHA-256 over campaign|kind|canonical spec|sorted eligibleGuids — the store refuses a second non-terminal job with the same key), `kind`, `owner{campaign,requestedBy,team}`, `characters{eligibleGuids (explicit allow-list), assignedGuids, maxConcurrent}`, `priority` 0-100 (ties broken createdAt then jobId — deterministic), `prerequisites[]` (typed), `successCriteria{measure, counters}`, `timeoutPolicy{jobTimeoutMinutes, phaseTimeoutMinutes, noProgressSeconds=120}`, `retryPolicy{maxAttempts=3, maxRecoveriesPerObjective=2, backoffSeconds, onExhaust=block}` (mirrors `QUEST_RECOVERY_V0.md`), `cancellation`, `permittedCapabilities[]` (closed allow-list), `progress{counters, lastProgressAt, attempt, recoveriesUsed, evidence[]}`, `phase`, `blockedReason{code,detail,since,nextAction}`, `leaseRef`, `receiptLog{file,lastSeq}`, `executionMode: dry-run|live` (default dry-run), `spec` (kind-discriminated).
- Capability vocabulary: `bridge.*` maps 1:1 to `ParseRequest` verbs; `oracle.*` mirrors `OperationCodeName()` exactly (`oracle.quest_objective`, `oracle.gather_source`, …); `bridge.fixture.*`/`bridge.probe.reset` are lab-only; `sql.read` covers guarded SELECT snapshots. A capability whose backing surface is not deployed blocks the job (`oracle-capability-not-deployed`); the adapter refuses anything not listed (fail closed).
- Honest-success rule (normative, in the schema description): a job is Completed only when every `successCriteria.counters` target is met by cited receipt evidence. Movement, XP, or elapsed time never satisfy a counter.

## 3. Character capability profile (read-only, derived)

Assembled per GUID from three sources, with observed facts strictly separated from inferred suitability:

- `observed.bridge` — `snapshot <guid>` (identity, position, vitals, target, AI action, travel state, combat state, pause state, `gather_route` when a worker); `questlog`/`questobjective <guid>` (active quests + objective counters); `list` (online membership).
- `observed.sql` (SELECT-only, `Invoke-QuestDbSelect` idiom) — `characters` (class, race, level, money, online), `character_skills` (professions + skill values), `character_spell` (known spells/recipes), `character_inventory ⋈ item_instance ⋈ item_template` (equipped ilvl, broken gear via durability, bag usage, consumables), `character_queststatus(_rewarded)` (save-lagged; bridge preferred), `groups`/`group_member`, `guild_member`, `autowow_league_member` (team, role, professions plan, active).
- `observed.autopilot` — current job binding, lease ref, last receipts.
- `inferred` — role capability (tank/healer/dps from class+spec table), talent completeness, gear adequacy vs a job's floor, travel readiness, "available for assignment" (no other mutator owns the GUID). Every inferred field names the observed fields it derives from.

Freshness: every profile carries `collectedAt` and per-source staleness; planners must refuse to act on snapshots older than the job's staleness bound (default 60 s live) — fail closed, matching the Oracle's own stale-frame rejections.

## 4. Planner and state machine

Deterministic per-job progression (single-threaded planner tick, jobs ordered by priority/createdAt/jobId):

```
Queued → Validating → Preparing → Traveling → Executing → Verifying → Completed
                │           │          │            │           │
                └───────────┴──────────┴─────┬──────┴───────────┘
                                             ▼
                          Blocked | Recovering | Suspended | Cancelled | Failed
```

- Validating: schema + prerequisites + capability deployment + character eligibility/enrollment + roster-conflict check (no other mutator owns the GUIDs). Failures → Blocked with typed code, never silent.
- Preparing: bind `assignedGuids`, form party if the spec requires it, verify consumables/repair prerequisites (as observations, since economy capabilities are missing → Blocked when unmet and unfixable).
- Traveling: only via `travel`/`route`/`advance` (Playerbots resolvers) — teleport is never a travel or recovery mechanism. Arrival verified by snapshot position.
- Executing: issue the kind's native cycle (e.g. `quest <leader>`), then poll observations on a bounded cadence.
- Verifying: re-read objective counters/SQL snapshots; only verified deltas update `progress.counters`.
- Recovering (bounded): stall = no qualifying movement AND no objective progress for `noProgressSeconds` (120 s default, combat defers). Ladder: bridge `recover` + native replan, at most `maxRecoveriesPerObjective` (2); then Blocked(`recovery-budget-exhausted`) for operator review — mirrors the director's `quest_manual_review_required`. Death → wait for native corpse recovery, receipt it; unrecoverable → Blocked. Recovery never rewrites the objective into generic grinding.
- Suspended (operator `pause`), Cancelled (operator `cancel`, acknowledged between commands, never mid-wire), Failed (only via `onExhaust: fail` or unrecoverable contradiction).
- Every transition emits exactly one receipt (schema `autowow.autopilot.receipt.v1`, §6). Phase timeouts and job timeouts emit `phase-timeout`/`job-timeout` Blocked/Failed receipts.

## 5. Human-facing control surface

V1 is a PowerShell 7 CLI (`autopilot\autowow-autopilot.ps1`), local-only, composing the guarded surfaces. Any future HTTP API binds 127.0.0.1 only. Commands:

```
autowow-autopilot status                             # controller + queue + per-job one-liners
autowow-autopilot roster [-Team alliance|horde]      # capability profiles (observed vs inferred)
autowow-autopilot assign quest --leader-guid 10 [--team northstar] [--target-level N]
autowow-autopilot assign gather --item-id 2770 --count 100 --guid 6     # accepted, then Blocked until oracle.gather_source deploys
autowow-autopilot form-party --roles tank,healer,dps,dps,dps --pool <guids>
autowow-autopilot run dungeon --name "Utgarde Keep" --leader-guid 2
autowow-autopilot queue battleground --name wsg --team-size 10
autowow-autopilot pause --character <guid> | --job <id>
autowow-autopilot cancel --job <id>
autowow-autopilot explain --character <guid> | --job <id>
autowow-autopilot run-once [-Mode dry-run|live]      # one deterministic planner tick
autowow-autopilot reconcile                          # startup reconciliation, also implicit on start
```

`explain` answers, from the job document + receipts alone: what the character is trying to do (kind, spec, phase); why this action was selected (planner receipt with inputs); what exact progress occurred (verified counters + citing receipts); why it is waiting/blocked (`blockedReason.code/detail/since`); what it will try next (`blockedReason.nextAction` / next transition preconditions).

Safety posture inherited from the ecosystem: dry-run is the default everywhere; `-Mode live` additionally requires a non-empty character allow-list file and `-ConfirmLiveBridge` (double gate, like `gathering-orchestrator.ps1:193-194`); zero managed characters ship enrolled.

## 6. Persistence and idempotence

- State root: `autopilot\state\` — `jobs\<jobId>.json` (one document per job, atomic write: temp file + `Move-Item -Force` on the same volume), `receipts\<jobId>.jsonl` + `receipts\controller.jsonl` (append-only, UTF-8 no BOM, `AppendAllText`), `allowlist.json` (managed-character enrollment, empty by default), `autopilot.pid.json` (single-owner lock).
- Receipt schema `autowow.autopilot.receipt.v1`: `{schema, schema_version:1, seq, receipt_id, timestamp_utc, job_id, event, from?, to?, reason?, command?{surface, capability, wire, mode, sent, idempotency_key}, response?, observation?, counters?, blocked?{code,detail}}` — vocabulary aligned with `ReceiptStatusName`/`ReceiptReasonName` so future Oracle receipts correlate 1:1.
- Idempotency: every irreversible command (`party`, `quest`, `quest-acquire`, `raid-create`, `wsg-queue`, future economy ops) carries `idempotency_key = SHA256(jobId|phase|attempt|wire)`. Before sending, the adapter scans the job's receipt log; a key already receipted `sent:true` (live) or `would_send` (dry-run) is not re-sent unless a later receipt explicitly expired it (`order_expired`/`retry_authorized`). Bounded replay: only the receipt tail since the last terminal/phase receipt is scanned.
- Restart reconciliation (also Scenario G): acquire the pid lock (stale PID detected via `Get-Process`, receipted `stale_lock_recovered`); for each non-terminal job, re-derive phase from (job doc, receipt tail, fresh live/fixture observation); resume, never restart from Queued, never re-issue receipted irreversible commands. Live-state contradiction (e.g. party dissolved, character offline) → Blocked(`stale-state`) with detail, not improvisation.
- Secrets: never persisted. SQL access uses the `MYSQL_PWD`-in-`finally` idiom; receipts carry env-var names only (matches the ecosystem-wide rule).

## 7. Acceptance scenarios

Machine-checkable scenarios A-G (quest, gather, profession, dungeon, raid, PvP, restart-recovery) with exact evidence sources, PASS/FAIL/INCONCLUSIVE semantics, and per-scenario current-status labels are specified in `AUTOWOW_AUTOPILOT_ACCEPTANCE_V1.md`. Fail-closed rules follow `PHASE1_ACCEPTANCE_VERIFIER.md`: missing evidence is INCONCLUSIVE, never success; forbidden-action counters must be zero; success is objective counters only.

## 8. Minimal implementation slice (buildable now, no Playerbots C++ changes)

Local-only PowerShell 7 controller in a new isolated directory `autopilot\` (the project root is not itself a git repository; this directory touches neither repo, neither worktree, nor the live server):

- `autopilot\AutopilotLib.ps1` — job store (atomic writes), schema + semantic validation, deterministic state machine with receipt emission, capability gate (static deployed-capability table: all `oracle.*` undeployed today), character-profile reader (fixture-first; live reads via `Send-Bridge` from `scripts\QuestLogSource.ps1`), bridge adapter with the dry-run seam (wire strings built by shelling to `scripts\autowow-control.ps1 -EmitRequestOnly` so the wire format stays single-sourced), idempotency ledger, reconciliation, explain.
- `autopilot\autowow-autopilot.ps1` — CLI entry (§5).
- `autopilot\fixtures\*.json` — recorded bridge responses (snapshot/questobjective/list) for dry-run and tests.
- `autopilot\tests\autopilot.tests.ps1` — Pester v5, fully offline (TestDrive), covering: schema validation, idempotency-key stability, legal/illegal transitions + receipt emission, Scenario A dry-run walk to Completed with wire-string equality against `autowow-control.ps1 -EmitRequestOnly`, Scenario B Gather → `Blocked('oracle-capability-not-deployed')` with zero commands, Scenario G restart/reconcile/no-duplicate proof, single-instance lock behavior.
- Defaults: `executionMode=dry-run`, `allowlist.json=[]` (zero managed characters), no live socket opened by any test.
- Explicitly represented but disabled: Gather jobs validate and immediately block on `oracle.gather_source` until Sol deploys the native gathering Oracle runtime.

### Proposed implementation file list

```
autopilot\README.md
autopilot\AutopilotLib.ps1
autopilot\autowow-autopilot.ps1
autopilot\fixtures\list.json
autopilot\fixtures\snapshot-10.json
autopilot\fixtures\questobjective-10-baseline.json
autopilot\fixtures\questobjective-10-progressed.json
autopilot\fixtures\questobjective-10-complete.json
autopilot\tests\autopilot.tests.ps1
autopilot\state\            (runtime-created; jobs\, receipts\, allowlist.json, autopilot.pid.json)
```

Root deliverables: `AUTOWOW_AUTOPILOT_V1_PLAN.md` (this file), `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`, `AUTOWOW_AUTOPILOT_ACCEPTANCE_V1.md`.

## 9. Open integration questions for the Sol orchestrator

1. **Roster ownership handshake.** The conceptual-lock rule is one gameplay mutator per roster GUID. When the Autopilot goes live, does it acquire GUIDs by (a) registering as a supervisor child role, (b) a lock file convention under `work\`, or (c) Sol assigning disjoint GUID sets per lane? Proposal: a `work\autopilot\ownership.json` claim file mirroring the supervisor's adopt-don't-kill semantics, honored by director/guardian start scripts.
2. **Oracle receipt export.** External determinism proofs need the `ReceiptRing` surfaced. Preferred shape: a read-only bridge verb (`oraclelog <guid>`?) or a JSONL appender inside the runtime? The Autopilot only needs read access; today it labels Oracle-side receipts as unavailable.
3. **Oracle runtime enablement path.** When `AutoWow.OracleRuntime.Enabled=1` ships with a GUID allowlist, who owns that allowlist — Sol's maintenance window process, or may the Autopilot propose (never write) allowlist entries via a receipted request file?
4. **Gather executor deployment gate.** Widening `IsVerifiedOperationTuple` to `GatherSource` plus a native seek/interact/loot callback is a Luna lane + maintenance window. What receipt/schema will announce "gather oracle deployed" so the Autopilot can flip its capability table from a fact, not an assumption?
5. **`autowow_league_quest_state` ownership.** PowerShell writes it, C++ ignores it. Should the Autopilot treat it as authoritative campaign state (and keep it updated for its own jobs), or as legacy director-only state to be superseded by Autopilot job documents?
6. **Battleground scope.** Only WSG has a control path (`WarsongFixtureControl`). Are AB/AV/EotS wire families planned on the bridge, or should the Battleground job kind stay WSG-only until an Oracle pvp executor lands?
7. **Turn-in proof surface.** `acceptance <guid> <quest-id>` covers reward postconditions; is that the agreed long-term proof for Scenario A step 4, or will a dedicated turn-in receipt land with Phase 1D (exact finisher)?
8. **Placement of Autopilot receipts.** V1 keeps them under `autopilot\state\receipts\`. If Sol's evidence reviewers should consume them, should they move under `logs\autopilot\` (lab convention) once live?

## 10. V1.1 addendum (2026-07-18, Sol integration decisions accepted)

Sol's answers to §9 are implemented as follows; the strategic/tactical boundary of §0 is unchanged.

1. **Capability truth source (decision #4, plus current-state correction).** Capability availability is never inferred from source files, wire-verb existence, or passing unit tests. `autopilot\AutopilotLib.ps1` `New-AutopilotCapabilityProvider` supports: `manifest` (schema `autowow.oracle.capabilities.v1`: server build/session, contract schema version, runtime_enabled, per-operation deployed / native_adapter_available / runtime_authorized / receipt_export_available / constraints, generated_at), `unavailable` (default; everything undeployed), and a `bridge` stub reserved for the future endpoint (reports unavailable until it exists). Unknown schema versions and malformed manifests fail closed. Conventional discovery path: `work\autopilot\capabilities.json` (Sol-published); explicit `-CapabilityManifest` wins. An operation is available only when runtime_enabled AND deployed AND native adapter AND runtime authorized. Fixture manifests live under `autopilot\fixtures\capabilities\` (including `current.json`, which encodes the 2026-07-18 correction: quest runtime compiling, gather integrated-but-unauthorized, craft 14/14 tests undeployed, pvp 9/9 undeployed, navigate in isolation, worldserver stopped).
2. **Ownership leasing (decision #1).** `work\autopilot\ownership.json`, schema `autowow.autopilot.ownership.v1`: per-lease controller instance id, PID, job id, character GUID, acquiredAt/renewedAt/expiresAt, state proposed|active|released|expired. Atomic writes; acquire/renew/release with receipts; a foreign active lease is never superseded (even with a dead-looking PID — only time expires it) and yields `Blocked('roster-owned')` with owner instance/pid/job/expiry; expired/released leases are reclaimable; the planner re-acquires automatically after release/expiry. The Autopilot writes only its own file; live operation additionally requires Sol-assigned disjoint GUIDs until directors honor the registry.
3. **Oracle receipts (decision #2).** Pluggable read-only provider (`New-AutopilotOracleReceiptProvider`): `bridge` (future, unavailable today), `jsonl` (parses `autowow.oracle.receipt.v1` records with decision_id, optional job_id, bot_guid, operation, resource, epoch, target_hash, fact_version_before/after, native_steps, status, reason, timestamp_utc), `unavailable` (default). `Join-AutopilotOracleEvidence` correlates command receipts by botGuid + timestamp window (+ jobId when the Oracle supplies it). The in-memory ring is never scraped; absent export = evidence unavailable, stated plainly in `receipts` output.
4. **Enrollment proposals (decision #3).** `autowow-autopilot propose-enrollment -JobId <id>` writes `work\autopilot\oracle-enrollment-request.json` (schema `autowow.autopilot.oracle-enrollment-request.v1`: job ids, GUIDs, required capabilities, expiry, reason, controller instance), upserted by jobId, receipted `enrollment_proposed`. It never edits playerbots.conf, never restarts worldserver, never enrolls a bot.
5. **Campaign state (decision #5).** Autopilot job documents + receipts are authoritative for Autopilot jobs. `autowow_league_quest_state` is legacy read-only compatibility data; the Autopilot never writes it.
6. **Battleground scope (decision #6).** WSG-only job acceptance stands, and WSG execution authority is the exact PvP Oracle: Battleground requires `oracle.pvp_objective`, Travel requires `oracle.navigate`, OpenWorldPvp requires `oracle.pvp_challenge`+`oracle.pvp_objective` — all Blocked until deployed. Legacy bridge wsg/travel tactics are not Autopilot execution authority.
7. **Quest completion proof (decision #7).** Unchanged from V1 and now structural: `Completed` requires the rewarded/postcondition evidence receipt; the evidence provider seam (`Get-AutopilotQuestObjectiveFacts` + the Oracle receipt provider) lets a future exact turn-in receipt replace or augment `acceptance` without touching the state machine.
8. **Receipt placement (decision #8).** Live jobs write `logs\autopilot\<run-id>\<jobId>.jsonl` (absolute path + runId stored in `receiptLog`); dry-run/tests stay under `autopilot\state\receipts\`. Logs are append-only, secret-free.
9. **Live quintuple gate.** A live socket opens only with: job `executionMode=live` AND `-ConfirmLiveBridge` AND allow-list enrollment AND an active ownership lease AND deployed capability evidence. Each missing layer refuses with a distinct error; all five refusals are unit-tested.
10. **Operator surface.** CLI commands: `status`, `roster`, `jobs`, `explain` (wait states: waiting-for-server-capability / waiting-for-ownership / preparing-prerequisites / traveling / executing / verifying / recovering / suspended / completed / terminal-failure), `capabilities`, `receipts` (with optional Oracle correlation), `propose-enrollment`, `assign`, `form-party`, `run`, `queue`, `pause`, `resume`, `cancel`, `run-once`, `reconcile`. No web dashboard in this phase.

Verification: 52 Pester tests green, covering the rehearsal matrix (QuestLevel dry-run to Completed; Gather/Craft/Travel/WSG blocked with explicit capability reasons; restart reconciliation; ownership conflict/expiry/stale; capability manifest transition unavailable→deployed; malformed/unknown schema fail-closed for jobs, manifests, and ownership; unit-tested-but-undeployed stays blocked; no duplicate mutating command across restart; the five live-gate refusals).

### Remaining server integration requirements (for Sol)

- Publish a real `autowow.oracle.capabilities.v1` manifest at `work\autopilot\capabilities.json` (or name the bridge endpoint) once the maintenance build deploys; include server build/session id so the Autopilot can detect restarts.
- Define the Oracle receipt export surface (bridge verb or JSONL path) matching `autowow.oracle.receipt.v1`; include `job_id` passthrough if the enrollment request supplies it, else correlation stays botGuid+window.
- Assign disjoint GUID sets (or teach directors the ownership registry) before any live Autopilot operation.
- Confirm the enrollment-request consumption flow (who reads `oracle-enrollment-request.json`, and what receipt announces approval).
- A recorded live capture of `snapshot`/`questobjective`/`quest` responses to replace the recorded-shape fixtures.

## 11. V1.2 addendum (2026-07-18): Persistent Player Experience

V1.2 turns the controller into a hands-free persistent autopilot rehearsed entirely through fixtures (the worldserver stayed stopped throughout; capability truth remained manifest-only; passing unit tests and compiled-but-undeployed runtimes still count as undeployed).

1. **Persistent run loop** (`modules\AutopilotRunLoop.ps1`): `autowow-autopilot run [-Once] [-DurationMinutes n] [-TickSeconds n] [-FixtureClock] [-DryRun:$true]` — deterministic cadence, pid single-instance lock, startup reconciliation, per-tick capability-manifest refresh, server-session-change detection (identity = manifest `server_build`+`session_id`; change ⇒ `server_session_changed` receipt, leaseRef invalidation, automatic re-validation of in-flight jobs; ownership FILE leases stay valid), atomic checkpointing (`autowow.autopilot.checkpoint.v1`), graceful Ctrl+C/stop.request shutdown with full ownership release, crash recovery via lease TTL expiry + ledger reconciliation. Live loops refuse outright until the supervised quester prerequisites exist. Job/phase timeout enforcement with bounded recovery (`Recovering` issues one bridge `recover` per budget unit, then `recovery-budget-exhausted` — never grinding) lives in `Invoke-AutopilotJobStep`.
2. **Profiles** (`modules\AutopilotProfiles.ps1`, `schemas\profile.schema.json`, eight samples in `profiles\`): versioned strategic preference (`autowow.autopilot.profile.v1`) — allowed kinds, priority weights, zones/materials/professions, role, risk, reserves, repair threshold, death cap, UTC play windows (midnight wrap), PvP/grouping/economy permissions with spend caps. Profiles never override capability manifests, ownership, allowlists, or game rules.
3. **Campaigns** (`modules\AutopilotCampaign.ps1`, `schemas\campaign.schema.json`, `campaigns\northstar-sample.json`): player-editable `autowow.autopilot.campaign.v1` documents (bare goals; the envelope stamps the goal schema). `campaign validate|plan|apply|status|explain|stop`. `plan` shows every generated node with capability evidence, prerequisites, roster-fact findings (unknown stays unknown), and blockers before anything can execute; `apply` creates jobs idempotently with dependency prerequisites wired in topological order, stamps profile metadata, and auto-writes enrollment proposals — it never sends a command. Goal-level outcomes without an observation stream (reached-level) are reported unknown, never inferred from job counters.
4. **Goal dependency planner** (`modules\AutopilotGoals.ps1`): deterministic DAG per work-order rules (ReachLevel→QuestLevel(+optional Dungeon per profile); Stockpile→Gather; CraftItem→Gather/Train(conditional)→Craft; RunDungeon→VendorRepair→Restock→FormParty→Travel→Dungeon; RunRaid/RunBattleground analogous), content-hashed idempotent node ids, Kahn ordering with sorted tie-breaks, typed cycle detection. No grinding, teleport, or reward nodes exist in the vocabulary.
5. **Capability-aware scheduler** (`modules\AutopilotScheduler.ps1`): one active mutating job per character; ownership acquired before activation; priority + bounded aging (max +20 over 10 h); dependencies before dependants; deterministic tie-breaks; blocked jobs self-heal without occupying the slot (a blocked Gather never becomes a QuestLevel — both jobs persist with receipted reasons); `job_held`/`character_idle` receipts on reason change only.
6. **Roster discovery** (`modules\AutopilotRoster.ps1`, fixtures in `fixtures\roster\`): read-only provider (fixture now, bridge later — refuses rather than guesses), `autowow.autopilot.roster-snapshot.v1`, per-activity suitability that explains missing requirements and routes unobserved facts to `unknown` (tri-state verdicts). No character-table queries.
7. **Progress summaries** (`modules\AutopilotSummary.ps1`): `summary`/`history` from receipts only — quest turn-ins, command counts (sent vs would-send vs deduplicated), blocked durations by code, ownership conflicts, controller lifecycle; domains without evidence streams render `unknown (no evidence stream)`, never zero-as-fact.
8. **Gather executor (dry-run)**: once a manifest evidences `oracle.gather_source`, Gather jobs rehearse via oracle-surface `would_send` intents and complete only on verified inventory observations (`progress_observed_gather`); live oracle dispatch has no surface and always refuses.
9. **Eight-hour simulated soak** (`tests\soak.tests.ps1`): 96 fixture-clock ticks covering a mid-run crash+restart (seq continuity proven), an ownership conflict resolved by expiry, a capability deploy that unblocks gathering mid-run, one server-session change, one quest and one gather completing on evidence, craft/PvP blocked throughout, zero duplicate irreversible commands, zero one-mutator violations, all leases released/expired, every terminal result evidenced.
10. **Sol contracts**: `AUTOWOW_ORACLE_INTEGRATION_CONTRACTS_V1.md` (root) specifies all six integration contracts (capabilities, oracle receipts, enrollment request/result, roster snapshot, session semantics) with required/optional fields, correlation keys, and worked examples.

Verification: **187 Pester tests green across 8 suites** (52 V1.1 originals preserved unchanged in intent; scenario updates only where V1.2 upgraded semantics, e.g. Gather now proceeds under a deployed manifest instead of blocking as unimplemented).

### Remaining requirements for the first supervised live quester

1. Sol publishes a real `autowow.oracle.capabilities.v1` manifest at `work\autopilot\capabilities.json` (post-maintenance build, with `server_build`/`session_id`), reporting `bridge.deployed=true` and the quest surfaces authorized.
2. Sol answers an enrollment request with an `autowow.autopilot.enrollment-result.v1` document (see contracts doc) and a disjoint GUID assignment (directors do not honor the ownership registry yet).
3. Operator enrolls exactly the approved GUIDs in `autopilot\state\allowlist.json` and flips one QuestLevel job to `executionMode=live`.
4. A supervised `run` with `-ConfirmLiveBridge`, small `DurationMinutes`, and an observer watching `status`/`explain`; live command receipts (`sent=true`) then carry the same idempotency guarantees already proven in dry-run.
5. Recorded live captures of `snapshot`/`questobjective`/`quest` responses to replace the recorded-shape fixtures, and (per contracts doc) the Oracle receipt export so `receipts -OracleReceiptsPath` correlation runs on real evidence.

## 12. Constraint compliance statement

- No keyboard/mouse automation, screen scraping, memory injection, packet fabrication, or credential entry anywhere in the design.
- Server state untouched: no deploy/restart/reconfigure; shared module worktree read-only; no DB writes (SELECT-only, guarded).
- No teleport as travel/recovery; no quest/XP/item/money/recipe/honor/achievement/boss-credit grants; no random-movement or grinding fallback; allow-list enrollment before any control; fail closed on stale state, missing capability, ownership conflict, or schema mismatch; ordinary Playerbots behavior preserved for unowned bots.
