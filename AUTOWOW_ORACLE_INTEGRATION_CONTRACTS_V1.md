# AutoWoW Oracle Integration Contracts V1

> **V1.4 addendum (2026-07-18): manifest binding, generator, and verifier.**
> The Autopilot now ships the authoritative publication tooling
> (`autopilot\modules\AutopilotManifest.ps1`; CLI `manifest generate|verify`).
> Additive optional fields on `autowow.oracle.capabilities.v1`:
> `worldserver_sha256` (exact hash of the running binary, computed read-only via
> `wsl sha256sum`), `module_fingerprint` (`{source, module_dir, head,
> dirty_files, diff_sha256, fingerprint}` from read-only git facts),
> `session_evidence` (`{wrapper_pid, wrapper_process_start, runtime_started_utc,
> distro, binary_path, sha_source}` — `server_build`/`session_id` are DERIVED
> deterministically from these), `bridge.evidence_level`
> (`port-listen|not-listening`), per-operation `proof`
> (`{status: proven|in-progress|unproven, evidence_kind, evidence_path,
> recorded_by, recorded_at, note}`), and `notes`.
> Two Sol-authored inputs feed generation: `autowow.autopilot.capability-declarations.v1`
> (`{runtime_enabled, sql_read_deployed, operations[flags]}`) and
> `autowow.autopilot.capability-proofs.v1` (`{proofs:[{operation, status,
> evidence_kind, evidence_path, recorded_by, recorded_at}]}`).
> Generator invariants: claims only ever DOWNGRADE (observed deployed config
> disables runtime_authorized; missing proof evidence -> unproven; the quest
> family `quest_acquire/quest_accept/quest_objective/quest_turn_in` can never be
> 'proven' without an existing `evidence_kind: "live-walk-accept-proof"` file);
> publication grade (`source: "deployed-runtime"`) requires `-AsSolPublication`
> plus established sha + session bindings, otherwise the output is
> `interim-probe` and can never authorize live sends.
> Verifier (`manifest verify`) recomputes every binding from current reality and
> FAILS on: non-publication source, binary hash mismatch, session identity
> mismatch (restart), module fingerprint drift, staleness/future-dating,
> deployed-config contradiction, or any proof-audit violation.
> Sol's publication command once the lane is green:
> `.\autowow-autopilot.ps1 manifest generate -OutPath ..\work\autopilot\capabilities.json -DeclarationsPath <decl.json> -ProofsPath <proofs.json> -AsSolPublication`.

Date: 2026-07-18
Status: normative for the Sol orchestrator / Autopilot boundary (AutoWoW Autopilot V1.2, item 10)
Audience: Sol (server orchestrator, producer of server-side documents) and the Autopilot controller (`autopilot\AutopilotLib.ps1`, consumer)
Companions: `AUTOWOW_AUTOPILOT_V1_PLAN.md` (§9 questions, §10 accepted decisions), `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`

This document freezes the exact JSON contracts exchanged between Sol and the Autopilot. Where an implementation already exists, the contract below matches it bit-for-bit and cites file and line; where a contract is new, it is marked **proposed** and is ready to implement server-side without further design. Nothing in this document claims that any capability is deployed; deployment truth flows only through contract 1.

Global rules (every contract below inherits these):

- Every document carries `schema` (string id) and `schema_version` (integer). A consumer that sees an unknown `schema`, an unsupported `schema_version`, or malformed JSON MUST fail closed: reject the document with a typed reason and treat the data as unavailable/invalid — never "best effort" parse, never default to a permissive reading.
- Unknown facts are `null` and are declared unknown (with a reason where the schema provides one). Facts are never guessed, never defaulted to a plausible value.
- All timestamps are ISO-8601 UTC strings (round-trip format, e.g. `2026-07-18T12:00:00.0000000Z`). Consumers parse them with `ConvertTo-AutopilotUtcDateTime` (`autopilot\AutopilotLib.ps1:339-354`), never naive `[datetime]::Parse`.
- Files are written atomically (temp file + rename, `Write-AutopilotFileAtomic`, `autopilot\AutopilotLib.ps1:356-366`), UTF-8 without BOM. JSONL files are append-only, one JSON object per line.
- Passing unit tests, source presence, or wire-verb existence NEVER means a capability is deployed (Sol decision #4, `AUTOWOW_AUTOPILOT_V1_PLAN.md` §10.1; enforced at `autopilot\AutopilotLib.ps1:52-56, 212`).

---

## 0.1 Contract summary

| # | Contract id | Producer | Consumer | Location (transport) | Status |
|---|---|---|---|---|---|
| 1 | `autowow.oracle.capabilities.v1` | Sol | Autopilot (`New-AutopilotCapabilityProvider`) | `work\autopilot\capabilities.json` (file; future bridge verb reserved) | Consumer implemented (`AutopilotLib.ps1:78-152`); producer fixture-only today (`autopilot\fixtures\capabilities\*.json`) — **no real manifest is published yet** |
| 2 | `autowow.oracle.receipt.v1` | Sol / worldserver Oracle runtime (receipt export) | Autopilot (`New-AutopilotOracleReceiptProvider`, `Join-AutopilotOracleEvidence`) | JSONL file (path named by Sol; future bridge verb, e.g. `oraclelog`) | Consumer implemented (`AutopilotLib.ps1:1005-1101`); producer NOT deployed — receipts live only in an in-memory ring of 64 (`AutoWowOracleRuntime.h:28`, per plan §1.2) |
| 3 | `autowow.autopilot.enrollment-request.v1` (implemented id: `autowow.autopilot.oracle-enrollment-request.v1`) | Autopilot (`New-AutopilotEnrollmentProposal`) | Sol | `work\autopilot\oracle-enrollment-request.json` (file) | Producer implemented (`AutopilotLib.ps1:943-995`); Sol-side consumption flow unconfirmed (plan §10 "Remaining server integration requirements") |
| 4 | `autowow.autopilot.enrollment-result.v1` | Sol | Autopilot | `work\autopilot\oracle-enrollment-result.json` (file) | **Proposed** (new in this document; nothing implemented on either side) |
| 5 | `autowow.autopilot.roster-snapshot.v1` | Fixtures today; future: Sol (periodic file `work\autopilot\roster-snapshot.json`) or a read-only bridge verb `rostersnapshot` | Autopilot roster provider | File or bridge verb | **Proposed** (schema authoritative here; no provider implemented yet) |
| 6 | Server-session identity `(server_build, session_id)` + change semantics | Sol (via contract 1 fields) | Autopilot | Carried inside contract 1 | Fields implemented in the manifest parser (`AutopilotLib.ps1:134-135`); change-detection semantics **proposed** |

## 0.2 Required for the first supervised live quester

The live quintuple gate (`autopilot\AutopilotLib.ps1:1268-1293`; plan §10.9) refuses a live socket unless every layer holds. In contract terms, the first supervised live QuestLevel run requires:

1. **Contract 1 with real values** published at `work\autopilot\capabilities.json`: `runtime_enabled: true`, `bridge.deployed: true`, an `operations[]` entry for `quest_objective` with `deployed`, `native_adapter_available`, and `runtime_authorized` all `true`, plus a real `server_build` and a non-null `session_id` (so contract 6 restart detection works). Until this exists, every job blocks with `oracle-capability-not-deployed` — correctly.
2. **Contract 3 → Contract 4 round trip**: the Autopilot's enrollment request answered by a Sol enrollment result with `status: "approved"` covering the job's GUIDs and its `oracle.*` capabilities, with `allowlistApplied: true` (Sol has actually written the deployed GUID allowlist — `AutoWow.OracleRuntime.BotGuids` and/or league membership). Absence of a result is *pending*, never approval (§4).
3. **Disjoint GUID assignment** from Sol (plan §10.2 and "Remaining server integration requirements"): no other mutator (director, supervisor, guardian) may own the job's GUIDs; the ownership registry `work\autopilot\ownership.json` must show no foreign active lease.
4. Local gates unchanged and out of Sol's scope but listed for completeness: job `executionMode=live`, operator `-ConfirmLiveBridge`, GUIDs present in `autopilot\state\allowlist.json`, an active ownership lease held by this controller+job.

Contract 2 (receipt export) is NOT required for the first supervised run — `receipt_export_available: false` simply means Oracle-side evidence is reported unavailable and completion proof rests on the rewarded/postcondition evidence via `questobjective`/`acceptance` (plan §10.7). It is strongly recommended for determinism review.

---

## 1. `autowow.oracle.capabilities.v1` — deployed-runtime capability manifest

**Purpose.** The single source of truth for what is actually deployed and authorized on the running worldserver. The Autopilot's capability provider consumes it; every `oracle.*`/`bridge.*`/`sql.read` availability decision derives from it. Without a manifest, everything is undeployed (fail closed, `AutopilotLib.ps1:52-56, 92-105`).

**Producer:** Sol. **Consumer:** Autopilot `New-AutopilotCapabilityProvider -Kind manifest` (`autopilot\AutopilotLib.ps1:78-152`), discovery via `Resolve-AutopilotCapabilityProvider` (`:154-162`).

**Transport / location.** A JSON file at the conventional path `work\autopilot\capabilities.json` (relative to the project root; discovery at `AutopilotLib.ps1:159`). An explicitly passed manifest path wins over the conventional path (`:158`). A future bridge verb is reserved: provider kind `bridge` exists and reports unavailable until the endpoint is real (`:108-110`) — it never scrapes.

Sol SHOULD regenerate the file at worldserver start and whenever deployment state changes; writes MUST be atomic (temp + rename) so the Autopilot never reads a torn document.

### 1.1 JSON Schema (draft 2020-12)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://autowow.local/schemas/autowow.oracle.capabilities.v1.json",
  "title": "autowow.oracle.capabilities.v1",
  "type": "object",
  "required": ["schema", "schema_version", "server_build", "session_id",
               "contract_schema_version", "runtime_enabled", "generated_at",
               "bridge", "sql_read_deployed", "operations"],
  "additionalProperties": true,
  "properties": {
    "schema": { "const": "autowow.oracle.capabilities.v1" },
    "schema_version": { "const": 1 },
    "server_build": { "type": ["string", "null"],
      "description": "Deploy identity component; bump per deploy. Null only when genuinely unknown." },
    "session_id": { "type": ["string", "null"],
      "description": "Worldserver session identity component; regenerate on every worldserver start. Null means no live session (e.g. maintenance)." },
    "contract_schema_version": { "type": "integer", "minimum": 1,
      "description": "Version of the C++ Oracle contract (AutoWowOracleContract.h) the server was built against. Informational to the Autopilot today (not read by the parser)." },
    "runtime_enabled": { "type": "boolean",
      "description": "AutoWow.OracleRuntime.Enabled in the DEPLOYED conf. False means no oracle.* operation is available regardless of per-operation flags." },
    "generated_at": { "type": "string", "format": "date-time" },
    "bridge": {
      "type": "object",
      "required": ["deployed"],
      "properties": { "deployed": { "type": "boolean" } },
      "additionalProperties": true
    },
    "sql_read_deployed": { "type": "boolean" },
    "operations": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["operation", "deployed", "native_adapter_available",
                     "runtime_authorized", "receipt_export_available", "constraints"],
        "additionalProperties": true,
        "properties": {
          "operation": {
            "enum": ["combat_action", "navigate", "quest_acquire", "quest_accept",
                     "quest_objective", "quest_turn_in", "gather_route", "gather_source",
                     "craft", "bank_withdraw", "bank_deposit", "mail_receive", "mail_send",
                     "vendor_buy", "vendor_sell", "auction_buy", "auction_sell",
                     "roster_assignment", "recover", "pvp_objective", "pvp_challenge",
                     "disengage"],
            "description": "snake_case OperationCode name (OperationCodeName, AutoWowOracleContract.h:178-230). 'unknown' is the C++ zero value and is NOT a legal manifest entry."
          },
          "deployed": { "type": "boolean" },
          "native_adapter_available": { "type": "boolean" },
          "runtime_authorized": { "type": "boolean" },
          "receipt_export_available": { "type": "boolean" },
          "constraints": { "type": ["object", "null"] }
        }
      }
    }
  }
}
```

### 1.2 Field table

"Producer-required" is what a conforming Sol manifest MUST contain. "Consumer enforcement" is what the implemented parser actually does — it is deliberately more tolerant than the producer schema, but every tolerance defaults *conservatively* (toward "not deployed"), never permissively.

| Field | Producer | Consumer enforcement (`AutopilotLib.ps1`) |
|---|---|---|
| `schema` | REQUIRED, exactly `autowow.oracle.capabilities.v1` | Mismatch/missing → provider unavailable, reason `manifest schema not supported (fail closed)` (`:125-130`) |
| `schema_version` | REQUIRED, exactly `1` | `[int64] != 1` → fail closed, same reason (`:127`) |
| `server_build` | REQUIRED (`string` or `null`) | Optional; absent → `$null` (`:134`) |
| `session_id` | REQUIRED (`string` or `null`) | Optional; absent → `$null` (`:135`) |
| `contract_schema_version` | REQUIRED | Not read by the parser today; carried for humans and future compatibility checks |
| `runtime_enabled` | REQUIRED | Absent → `false` (`:136`) — i.e. everything unavailable |
| `generated_at` | REQUIRED | Absent → `$null` (`:137`) |
| `bridge.deployed` | REQUIRED | Absent/malformed → `false` (`:138-140`) |
| `sql_read_deployed` | REQUIRED | Absent → `false` (`:141`) |
| `operations[]` | REQUIRED (may be empty) | Absent → empty table; entries without an `operation` key are ignored (`:142-148`) |
| `operations[].operation` | REQUIRED, snake_case OperationCode name | Keys the per-operation lookup; the Autopilot capability id is `oracle.` + this name (`:201-203`) |
| `operations[].deployed` / `native_adapter_available` / `runtime_authorized` / `receipt_export_available` | REQUIRED booleans | Each absent → `false` (`:205-208`) |
| `operations[].constraints` | REQUIRED (`object` or `null`) | Absent → `$null` (`:209`); free-form, surfaced verbatim in capability status |

Malformed JSON at any point → provider unavailable with reason `manifest unreadable (fail closed): <error>` (`:118-124`). A missing file → `manifest not found: <path>` (`:114-117`).

### 1.3 The availability equation

For an `oracle.<operation>` capability (`Get-AutopilotCapabilityStatus`, `AutopilotLib.ps1:201-220`, equation at `:212`):

```
available = runtime_enabled AND deployed AND native_adapter_available AND runtime_authorized
```

- An operation absent from `operations[]` is unavailable with reason `operation '<name>' absent from deployed manifest` (`:217-219`).
- `bridge.*` capabilities are available iff `bridge.deployed` (`:196-199`). `sql.read` iff `sql_read_deployed` (`:191-194`).
- **`receipt_export_available` is NOT part of the equation.** It gates *evidence expectations only*: when `true`, the Autopilot may expect contract-2 receipts for that operation and reviewers may demand Oracle-side correlation; when `false`, Oracle evidence is reported "unavailable" plainly and is never fabricated (`:208`; plan §10.3). It never blocks or unblocks execution.
- Passing unit tests or source presence never flips any flag (comment at `:210-211`; the shipped fixture `current.json` demonstrates this: craft "14/14 authoritative tests" is still `deployed: false`).

### 1.4 Versioning / fail-closed

`schema_version` bumps on any breaking field change. The consumer accepts exactly version 1; anything else fails closed to the `unavailable` provider with a typed reason string (`:125-130`), which cascades to `Blocked('oracle-capability-not-deployed')` for jobs requiring those capabilities (`:1453-1462`). There is no partial acceptance of a future version.

### 1.5 Worked example

The shipped hypothetical post-maintenance fixture `autopilot\fixtures\capabilities\deployed-quest.json` (bridge + quest_objective fully deployed):

```json
{
  "schema": "autowow.oracle.capabilities.v1",
  "schema_version": 1,
  "server_build": "fixture-deployed-quest",
  "session_id": "fixture-session-1",
  "contract_schema_version": 1,
  "runtime_enabled": true,
  "generated_at": "2026-07-18T12:00:00.0000000Z",
  "bridge": { "deployed": true },
  "sql_read_deployed": true,
  "operations": [
    {
      "operation": "quest_objective",
      "deployed": true,
      "native_adapter_available": true,
      "runtime_authorized": true,
      "receipt_export_available": false,
      "constraints": null
    }
  ]
}
```

Under this manifest, `oracle.quest_objective` is available; every other `oracle.*` operation is unavailable ("absent from deployed manifest"). Contrast `autopilot\fixtures\capabilities\current.json`, which encodes the real 2026-07-18 state: `runtime_enabled: false`, `session_id: null`, every operation `deployed: false` — nothing is available, whatever the test suites say.

---

## 2. `autowow.oracle.receipt.v1` — exported Oracle receipts

**Purpose.** Externalize the Oracle runtime's decision/lease/execution receipts so the Autopilot (and evidence reviewers) can correlate its command receipts with what the server actually did. Today those receipts exist only in an in-memory ring of 64 (`AutoWowOracleRuntime.h:28`, per plan §1.2) and are exported nowhere; the Autopilot therefore reports Oracle evidence unavailable rather than scraping (`AutopilotLib.ps1:998-1001, 1013`).

**Producer:** Sol / the worldserver Oracle runtime (a JSONL appender inside the runtime, or a read-only bridge verb — plan §9 Q2 proposed `oraclelog <guid>`). **Consumer:** Autopilot `New-AutopilotOracleReceiptProvider -Kind jsonl` (`AutopilotLib.ps1:1005-1038`) and `Join-AutopilotOracleEvidence` (`:1040-1101`).

**Transport / location.** JSONL file, one receipt per line, append-only, UTF-8 no BOM. The path is Sol's to name (the Autopilot takes it as `-Path`). Fixture exemplar: `autopilot\fixtures\oracle-receipts\quest-792-run.jsonl`. The future `bridge` provider kind exists and fails closed until the verb is deployed (`:1019`).

### 2.1 JSON Schema (draft 2020-12) — one JSONL record

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://autowow.local/schemas/autowow.oracle.receipt.v1.json",
  "title": "autowow.oracle.receipt.v1",
  "type": "object",
  "required": ["schema", "schema_version", "decision_id", "job_id", "bot_guid",
               "operation", "resource", "epoch", "target_hash",
               "fact_version_before", "fact_version_after", "native_steps",
               "status", "reason", "timestamp_utc"],
  "additionalProperties": true,
  "properties": {
    "schema": { "const": "autowow.oracle.receipt.v1" },
    "schema_version": { "const": 1 },
    "decision_id": { "type": "integer", "minimum": 0,
      "description": "Oracle DecisionId (uint64, AutoWowOracleContract.h:31; Receipt.decisionId :804)." },
    "job_id": { "type": ["string", "null"],
      "description": "Autopilot jobId passthrough when the enrollment supplied it; null otherwise. See 2.3." },
    "bot_guid": { "type": "integer", "minimum": 1,
      "description": "Character GUID (uint64, AutoWowOracleContract.h:23)." },
    "operation": {
      "enum": ["unknown", "combat_action", "navigate", "quest_acquire", "quest_accept",
               "quest_objective", "quest_turn_in", "gather_route", "gather_source",
               "craft", "bank_withdraw", "bank_deposit", "mail_receive", "mail_send",
               "vendor_buy", "vendor_sell", "auction_buy", "auction_sell",
               "roster_assignment", "recover", "pvp_objective", "pvp_challenge", "disengage"],
      "description": "OperationCodeName (AutoWowOracleContract.h:178-230). 'unknown' may appear on rejected/invalid decisions."
    },
    "resource": {
      "enum": ["idle", "quest_gather", "transition", "combat_positioning", "recovery"],
      "description": "LeaseResourceName (AutoWowOracleContract.h:337-353; enum :307-314, precedence Idle<QuestGather<Transition<CombatPositioning<Recovery)."
    },
    "epoch": { "type": "integer", "minimum": 0,
      "description": "Lease epoch (uint64, AutoWowOracleContract.h:30; Receipt.epoch :810)." },
    "target_hash": { "type": "string",
      "description": "Deterministic target identity string; composition in 2.2." },
    "fact_version_before": { "type": "integer", "minimum": 0,
      "description": "FactVersion of the world-read frame before execution (uint64, AutoWowOracleContract.h:29)." },
    "fact_version_after": { "type": "integer", "minimum": 0,
      "description": "FactVersion after execution. after == before with native_steps 0 means nothing executed." },
    "native_steps": { "type": "integer", "minimum": 0,
      "description": "Count of native actions dispatched for this receipt (the runtime performs one native step per cadence tick)." },
    "status": {
      "enum": ["accepted", "progressing", "completed", "blocked", "failed", "rejected"],
      "description": "ReceiptStatusName (AutoWowOracleContract.h:365-383; enum :355-363)."
    },
    "reason": {
      "enum": ["none", "planned", "lease_acquired", "lease_already_owned", "lease_renewed",
               "lease_released", "no_eligible_intent", "invalid_decision", "invalid_scope",
               "invalid_frame", "stale_frame", "stale_epoch", "expired", "precondition_failed",
               "lab_only_persistent_scope", "bot_lease_conflict", "squad_owner_conflict",
               "item_reservation_conflict", "capacity_exceeded", "not_owner",
               "executor_blocked", "executor_failed", "node_reservation_conflict"],
      "description": "ReceiptReasonName (AutoWowOracleContract.h:412-464; enum :385-410)."
    },
    "timestamp_utc": { "type": "string", "format": "date-time" }
  }
}
```

### 2.2 `target_hash` composition

A compact, deterministic identity for the receipt's target, derived from the contract's reference structs. Colon-joined, lowercase, no whitespace:

| Operation family | Format | Components (source struct) |
|---|---|---|
| Quest (`quest_acquire`, `quest_accept`, `quest_objective`, `quest_turn_in`) | `q<questId>:f<family>:s<slot>:e<entry>` | `QuestReference` (`AutoWowOracleContract.h:531-539`): `questId`; `family` = numeric `QuestObjectiveFamily` (`:521-526`: 0 = none, 1 = npc_or_gameobject, 2 = item); `slot` = `objectiveSlot`; `entry` = `requiredEntry` (0 when none) |
| Gather (`gather_source`, `gather_route`) | `g<spawnId>:e<entry>:m<mapId>` | `GatherSourceReference` (`AutoWowOracleContract.h:541-548`): `spawnId` (NodeId), `entry`, `mapId` |

Fixture examples: `q792:f1:s0:e3101` (quest 792, NPC/GO family, slot 0, required entry 3101) and `q827:f1:s0:e0` (no required entry). Other operation families define their formats when their executors are wired; until then producers MUST NOT invent formats — an operation without a defined target reference emits the empty-target hash of its family or is deferred with the executor.

### 2.3 Correlation keys

Correlation links Autopilot `command` receipts (`autowow.autopilot.receipt.v1`) with Oracle receipts. **Correlation is evidence linking only — it never upgrades a command into success by itself** (`AutopilotLib.ps1:1040-1047`).

| Priority | Key | Property |
|---|---|---|
| Primary | `job_id` | Exact, unambiguous. Present only when Sol's enrollment plumbing passes the Autopilot `jobId` through to the runtime (contract 3 supplies it; plan §10 "Remaining server integration requirements"). |
| Fallback | `bot_guid` + `operation` + `timestamp_utc` window | Heuristic. Default window ±300 s (`AutopilotLib.ps1:1052`). |

Why `job_id` passthrough upgrades correlation: the fallback keys are ambiguous whenever one GUID serves two jobs across a window boundary, two commands of the same operation land inside one window, or clock skew forces a wide window. A passed-through `job_id` makes each Oracle receipt attributable to exactly one Autopilot job — across controller restarts, across overlapping jobs on a shared GUID pool — and lets the consumer *reject* receipts claiming a different job rather than merely failing to match them.

As implemented, `Join-AutopilotOracleEvidence` enforces: `job_id` equality when the receipt carries a non-null `job_id` (`:1065-1067`), `bot_guid` membership in the job's assigned+eligible GUIDs (`:1068`), and the timestamp window (`:1069-1073`); `operation` is carried into the correlation output (`:1086`) for the reviewer but is not yet an automatic filter. Each match records `matched_by` (`botGuid`, `timestamp`, and `jobId` when present, `:1095`). An unparseable `timestamp_utc` excludes the receipt from matching (fail closed, `:1073`).

### 2.4 Versioning / fail-closed

Per-line: a line whose `schema` is not `autowow.oracle.receipt.v1` or that is not valid JSON is skipped and counted (`:1025-1031`), reported as "(N unrecognized lines skipped)" (`:1034`). Per-file: a missing file → provider unavailable with reason (`:1022`). No provider (or the `unavailable`/`bridge` kinds) → evidence reported unavailable with a reason (`:1013, 1019, 1054-1056`); the in-memory ring is never scraped.

### 2.5 Worked example

From `autopilot\fixtures\oracle-receipts\quest-792-run.jsonl` (line 2 — a completed quest-objective step):

```json
{"schema":"autowow.oracle.receipt.v1","schema_version":1,"decision_id":502,"job_id":null,"bot_guid":10,"operation":"quest_objective","resource":"QuestGather","epoch":3,"target_hash":"q792:f1:s0:e3101","fact_version_before":42,"fact_version_after":43,"native_steps":1,"status":"completed","reason":"none","timestamp_utc":"2026-07-18T12:00:20.0000000Z"}
```

Note on `resource` casing: the fixture predates this document and uses the C++ identifier `QuestGather`; the normative wire value is the `LeaseResourceName` string `quest_gather` (`AutoWowOracleContract.h:343-344`). Sol's exporter MUST emit the snake_case names; the fixture will be regenerated when a real export exists. (Line 3 of the same fixture shows a blocked receipt: `status:"blocked"`, `reason:"executor_blocked"`, `native_steps:0`, `fact_version_before == fact_version_after` — the honest shape for "nothing executed".)

---

## 3. `autowow.autopilot.enrollment-request.v1` — Autopilot → Sol enrollment proposal

**Purpose.** The Autopilot proposes — never applies — Oracle enrollment for a job's GUIDs and required `oracle.*` capabilities. It never edits `playerbots.conf`, never restarts worldserver, never enrolls a bot (Sol decision #3, plan §10.4; the document itself carries this note, `AutopilotLib.ps1:984`).

**Producer:** Autopilot `New-AutopilotEnrollmentProposal` (`autopilot\AutopilotLib.ps1:945-995`), receipted `enrollment_proposed` (`:988-993`). **Consumer:** Sol.

**Transport / location.** JSON file at `work\autopilot\oracle-enrollment-request.json` (default path, `Get-AutopilotPaths`, `AutopilotLib.ps1:286`). Atomic write (`:987`). Requests are upserted by `jobId` — a re-proposal replaces that job's entry and keeps the others (`:970-978`).

### 3.1 Schema id: canonical name and implemented alias

- **Implemented id (on the wire today):** `autowow.autopilot.oracle-enrollment-request.v1` (`$script:AutopilotEnrollmentSchema`, `AutopilotLib.ps1:943`).
- **Canonical id (this document's naming scheme):** `autowow.autopilot.enrollment-request.v1` — aligning with `autowow.autopilot.enrollment-result.v1` (contract 4).

Migration rule: Sol MUST accept the implemented id for `schema_version` 1. The Autopilot keeps writing the implemented id for v1; the canonical id takes effect only with `schema_version` 2 (or a coordinated same-version rename that updates `AutopilotLib.ps1:943` and Sol's reader in one change). Neither side may treat the two ids as simultaneously valid aliases within one version — a v1 document with the canonical id is an unknown schema and fails closed on both sides.

### 3.2 JSON Schema (draft 2020-12)

Field names are camelCase in this Autopilot-produced document (matching the implementation), unlike the snake_case server-produced envelopes.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://autowow.local/schemas/autowow.autopilot.enrollment-request.v1.json",
  "title": "autowow.autopilot.enrollment-request.v1 (implemented id: autowow.autopilot.oracle-enrollment-request.v1)",
  "type": "object",
  "required": ["schema", "schema_version", "generatedAt", "controllerInstanceId", "note", "requests"],
  "additionalProperties": true,
  "properties": {
    "schema": { "const": "autowow.autopilot.oracle-enrollment-request.v1" },
    "schema_version": { "const": 1 },
    "generatedAt": { "type": "string", "format": "date-time" },
    "controllerInstanceId": { "type": "string",
      "description": "Stable controller identity 'ap-<12hex>' (Get-AutopilotControllerInstanceId, AutopilotLib.ps1:312-326)." },
    "note": { "type": "string",
      "description": "Fixed proposal-only disclaimer (AutopilotLib.ps1:984)." },
    "requests": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["jobId", "guids", "requiredCapabilities", "expiresAt", "reason", "requestedAt"],
        "additionalProperties": true,
        "properties": {
          "jobId": { "type": "string", "pattern": "^job-\\d{8}T\\d{6}Z-[0-9a-f]{8}$" },
          "guids": { "type": "array", "items": { "type": "integer", "minimum": 1 }, "minItems": 1 },
          "requiredCapabilities": {
            "type": "array",
            "items": { "type": "string", "pattern": "^oracle\\.[a-z_]+$" },
            "description": "Sorted unique oracle.* capability ids: the job's permittedCapabilities plus its kind's required capabilities, filtered to oracle.* (AutopilotLib.ps1:956-957; kind table :229-245)."
          },
          "expiresAt": { "type": "string", "format": "date-time",
            "description": "Request TTL; default now + 24 h (AutopilotLib.ps1:949, 965)." },
          "reason": { "type": "string" },
          "requestedAt": { "type": "string", "format": "date-time" }
        }
      }
    }
  }
}
```

### 3.3 Field table

| Field | Required | Notes |
|---|---|---|
| `schema`, `schema_version` | REQUIRED | v1 wire id is the implemented (long) id; see 3.1 |
| `generatedAt` | REQUIRED | Document (re)write time |
| `controllerInstanceId` | REQUIRED | Lets Sol attribute the request and cross-check against ownership leases |
| `note` | REQUIRED | Fixed disclaimer text; Sol may ignore content but its presence marks a conforming producer |
| `requests[]` | REQUIRED | Upserted by `jobId`; an empty array is legal (all proposals withdrawn/expired) |
| `requests[].jobId` / `guids` / `requiredCapabilities` / `expiresAt` / `reason` / `requestedAt` | REQUIRED | Sol SHOULD ignore entries whose `expiresAt` has passed |

**Fail-closed.** Sol rejects unknown schema/version or malformed JSON and processes nothing from the file. The Autopilot side already fails soft-closed when re-reading its own file: an unreadable or foreign-schema file contributes zero existing requests to the upsert (`AutopilotLib.ps1:970-977`).

### 3.4 Correlation keys

`jobId` is the correlation key throughout the enrollment lifecycle: request entry → result entry (contract 4) → Oracle receipt `job_id` passthrough (contract 2). `controllerInstanceId` secondarily ties requests to ownership leases (`work\autopilot\ownership.json`, schema `autowow.autopilot.ownership.v1`, `AutopilotLib.ps1:757`).

### 3.5 Worked example

```json
{
  "schema": "autowow.autopilot.oracle-enrollment-request.v1",
  "schema_version": 1,
  "generatedAt": "2026-07-18T12:00:00.0000000Z",
  "controllerInstanceId": "ap-1f2e3d4c5b6a",
  "note": "Proposal only. Sol owns deployed Oracle configuration and GUID allowlists; the Autopilot never edits playerbots.conf, never restarts worldserver, never silently enrolls a bot.",
  "requests": [
    {
      "jobId": "job-20260718T120000Z-a1b2c3d4",
      "guids": [10],
      "requiredCapabilities": ["oracle.quest_objective"],
      "expiresAt": "2026-07-19T12:00:00.0000000Z",
      "reason": "job job-20260718T120000Z-a1b2c3d4 (QuestLevel) requires oracle capabilities for campaign 'test-league'",
      "requestedAt": "2026-07-18T12:00:00.0000000Z"
    }
  ]
}
```

---

## 4. `autowow.autopilot.enrollment-result.v1` — Sol → Autopilot enrollment response (**proposed, new**)

**Purpose.** Closes the loop opened by contract 3: Sol's receipted answer to enrollment proposals. This is the missing "what receipt announces approval" surface from plan §10 "Remaining server integration requirements". Nothing exists yet on either side; this section is the implementation spec.

**Producer:** Sol. **Consumer:** Autopilot.

**Transport / location.** JSON file at `work\autopilot\oracle-enrollment-result.json`, atomic writes, entries upserted by `jobId` (mirror of contract 3's discipline).

### 4.1 Core semantics (normative)

1. **Absence is pending, never approval.** No result file, no entry for a `jobId`, a malformed file, or an unknown `schema`/`schema_version` all mean *pending*. The Autopilot MUST fail closed and keep the affected jobs blocked/waiting; it never infers approval from the passage of time, from the request's own `expiresAt`, or from capability availability.
2. **Results never confer capability availability.** `status: "approved"` with `allowlistApplied: true` says the GUIDs are enrolled/allowlisted; whether `oracle.quest_objective` is *deployed and authorized* is still solely contract 1's statement. Both are required for live execution; neither implies the other.
3. `allowlistApplied` is a **deployment fact, not a promise**: `true` only after the deployed configuration (e.g. `AutoWow.OracleRuntime.BotGuids`, league membership rows) actually contains the granted GUIDs.
4. `partial` grants MUST enumerate exactly what applies: `guids` and `grantedCapabilities` list the granted subset only; anything not listed is not granted.
5. `expired` marks a request whose `expiresAt` passed without action; it is terminal for that request instance (a fresh proposal restarts the flow).
6. A result whose `(server_build, session_id)` no longer matches the current capabilities manifest identity (contract 6) is stale: the Autopilot MUST treat runtime-scoped grants (`allowlistApplied` against a dead session) as requiring re-verification, not as void — file-level enrollment may survive a restart, but the Autopilot re-checks before any live send.

### 4.2 JSON Schema (draft 2020-12)

Envelope fields are snake_case (server-produced document, matching contracts 1-2); `results[]` entries use camelCase (mirroring the request entries they answer).

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://autowow.local/schemas/autowow.autopilot.enrollment-result.v1.json",
  "title": "autowow.autopilot.enrollment-result.v1",
  "type": "object",
  "required": ["schema", "schema_version", "generated_at", "server_build", "session_id", "results"],
  "additionalProperties": true,
  "properties": {
    "schema": { "const": "autowow.autopilot.enrollment-result.v1" },
    "schema_version": { "const": 1 },
    "generated_at": { "type": "string", "format": "date-time" },
    "server_build": { "type": ["string", "null"],
      "description": "Identity component at decision time; must be comparable with contract 1." },
    "session_id": { "type": ["string", "null"],
      "description": "Identity component at decision time; null when decided during maintenance (no live session)." },
    "results": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["jobId", "guids", "status", "grantedCapabilities",
                     "allowlistApplied", "effectiveAt", "expiresAt", "reason"],
        "additionalProperties": true,
        "properties": {
          "jobId": { "type": "string", "pattern": "^job-\\d{8}T\\d{6}Z-[0-9a-f]{8}$",
            "description": "Correlates to requests[].jobId in contract 3. Upsert key." },
          "guids": { "type": "array", "items": { "type": "integer", "minimum": 1 },
            "description": "Granted GUIDs. For status approved: all requested; partial: the granted subset; denied/expired: MUST be []." },
          "status": { "enum": ["approved", "denied", "partial", "expired"] },
          "grantedCapabilities": {
            "type": "array",
            "items": { "type": "string", "pattern": "^oracle\\.[a-z_]+$" },
            "description": "Granted oracle.* capability ids. Same subset rules as guids; [] for denied/expired."
          },
          "allowlistApplied": { "type": "boolean",
            "description": "True only after the deployed GUID allowlist actually contains the granted GUIDs (fact, not intent)." },
          "effectiveAt": { "type": ["string", "null"], "format": "date-time",
            "description": "When the grant takes effect; null iff status is denied or expired." },
          "expiresAt": { "type": ["string", "null"], "format": "date-time",
            "description": "Grant expiry (independent of the request's expiresAt); null means until revoked or session end." },
          "reason": { "type": "string",
            "description": "Human-readable decision rationale; REQUIRED and non-empty for denied/partial/expired." }
        }
      }
    }
  }
}
```

### 4.3 Field table

| Field | Required | Notes |
|---|---|---|
| `schema`, `schema_version` | REQUIRED | Exactly `autowow.autopilot.enrollment-result.v1` / `1`; anything else → the whole file is unusable (pending) |
| `generated_at` | REQUIRED | Last write time |
| `server_build`, `session_id` | REQUIRED (nullable) | Identity at decision time; enables the staleness rule in 4.1.6 |
| `results[]` | REQUIRED | Upsert by `jobId`; empty array = no decisions yet (everything pending) |
| `results[].jobId`, `guids`, `status`, `grantedCapabilities`, `allowlistApplied`, `effectiveAt`, `expiresAt`, `reason` | REQUIRED | Per-entry; an entry missing any required field is invalid and treated as absent (that job stays pending) — never partially honored |

### 4.4 Correlation keys

Primary: `jobId` (exact, to contract 3 and onward to contract 2 `job_id`). Secondary: `(server_build, session_id)` against contract 1 for staleness (4.1.6).

### 4.5 Versioning / fail-closed

Unknown schema/version or malformed JSON → the Autopilot logs a typed receipt (recommended event: `enrollment_result_invalid`) and treats every request as pending. A valid file with an invalid entry drops only that entry (per 4.3), with a receipt naming the `jobId` and defect.

### 4.6 Worked example

```json
{
  "schema": "autowow.autopilot.enrollment-result.v1",
  "schema_version": 1,
  "generated_at": "2026-07-18T14:30:00.0000000Z",
  "server_build": "phase1-maint-20260718a",
  "session_id": "ws-20260718-143000-9f3a",
  "results": [
    {
      "jobId": "job-20260718T120000Z-a1b2c3d4",
      "guids": [10],
      "status": "approved",
      "grantedCapabilities": ["oracle.quest_objective"],
      "allowlistApplied": true,
      "effectiveAt": "2026-07-18T14:30:00.0000000Z",
      "expiresAt": "2026-07-25T14:30:00.0000000Z",
      "reason": "supervised live quester window; GUID 10 disjoint from director fleets"
    }
  ]
}
```

---

## 5. `autowow.autopilot.roster-snapshot.v1` — read-only roster provider contract (**proposed**)

**Purpose.** A single read-only document describing the character roster the Autopilot may plan over: identity, class/level/spec, professions, gear/bags/money, location, vitals, party/guild — with every unknown fact explicitly unknown. This replaces any temptation to query the DB character tables directly: **the Autopilot performs no direct DB character-table queries for roster state**; it consumes exactly this document. Staleness is judged from `observed_utc` and never guessed away.

**Producer:** today, fixtures only (`source: "fixture"`); future, either a read-only bridge verb — proposed verb name `rostersnapshot`, returning this same document as its JSON response — or a Sol-published periodic file at `work\autopilot\roster-snapshot.json` (`source: "bridge"` in both server-derived cases; `source` records the derivation authority, not the transport). **Consumer:** the Autopilot roster provider (to be implemented; will follow the same provider pattern as contracts 1-2: `unavailable` default, fail closed on unknown schema).

### 5.1 Document shape (authoritative, verbatim for this workflow)

```json
{
  "schema": "autowow.autopilot.roster-snapshot.v1",
  "schema_version": 1,
  "source": "fixture" | "bridge",
  "observed_utc": "<ISO8601 UTC>",
  "server_session_id": "<string|null>",
  "characters": [{
    "guid": <int>, "name": "<string>", "class_id": <int|null>, "class": "<string|null>", "race": "<string|null>",
    "level": <int|null>, "spec": "<string|null>", "roles": ["tank"|"healer"|"dps", ...],
    "professions": [{"skill_id": <int>, "name": "<string>", "value": <int>, "max": <int>}],
    "equipped_item_level": <number|null>, "bag_slots_free": <int|null>, "bag_slots_total": <int|null>,
    "money_copper": <int|null>, "zone": "<string|null>", "map_id": <int|null>,
    "alive": <bool|null>, "ghost": <bool|null>, "in_combat": <bool|null>, "online": <bool|null>,
    "party": {"in_party": <bool|null>, "leader_guid": <int|null>, "raid": <bool|null>},
    "guild": "<string|null>",
    "observed_utc": "<ISO8601 UTC>",
    "unknown_fields": ["<field name>", ...]
  }]
}
```

**Unknown-fact convention (normative):** a fact that is not known is `null` AND its field name appears in that character's `unknown_fields`. A `null` without the corresponding `unknown_fields` entry, or an `unknown_fields` entry over a non-null field, is a producer defect; consumers treat such characters' affected fields as unknown (the conservative reading). Nested facts use dotted names in `unknown_fields` (e.g. `party.leader_guid`). Producers never substitute plausible defaults — a fresh level-1 default, a guessed zone, or a `true` online flag for an unobserved character are all contract violations.

### 5.2 JSON Schema (draft 2020-12)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://autowow.local/schemas/autowow.autopilot.roster-snapshot.v1.json",
  "title": "autowow.autopilot.roster-snapshot.v1",
  "type": "object",
  "required": ["schema", "schema_version", "source", "observed_utc", "server_session_id", "characters"],
  "additionalProperties": true,
  "properties": {
    "schema": { "const": "autowow.autopilot.roster-snapshot.v1" },
    "schema_version": { "const": 1 },
    "source": { "enum": ["fixture", "bridge"],
      "description": "Derivation authority: 'fixture' = authored/recorded test data; 'bridge' = derived from live server surfaces (whatever the transport)." },
    "observed_utc": { "type": "string", "format": "date-time",
      "description": "Collection stamp for the whole document. Staleness is judged from observed_utc, never guessed away." },
    "server_session_id": { "type": ["string", "null"],
      "description": "session_id (contract 6) at collection time; null when unknown — consumers must then fail closed on any session-scoped reasoning." },
    "characters": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["guid", "name", "class_id", "class", "race", "level", "spec", "roles",
                     "professions", "equipped_item_level", "bag_slots_free", "bag_slots_total",
                     "money_copper", "zone", "map_id", "alive", "ghost", "in_combat", "online",
                     "party", "guild", "observed_utc", "unknown_fields"],
        "additionalProperties": true,
        "properties": {
          "guid": { "type": "integer", "minimum": 1 },
          "name": { "type": "string", "minLength": 1 },
          "class_id": { "type": ["integer", "null"] },
          "class": { "type": ["string", "null"] },
          "race": { "type": ["string", "null"] },
          "level": { "type": ["integer", "null"], "minimum": 1 },
          "spec": { "type": ["string", "null"] },
          "roles": { "type": "array", "items": { "enum": ["tank", "healer", "dps"] },
            "uniqueItems": true,
            "description": "Empty array when roles are unknown (and 'roles' listed in unknown_fields)." },
          "professions": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["skill_id", "name", "value", "max"],
              "properties": {
                "skill_id": { "type": "integer", "minimum": 1 },
                "name": { "type": "string" },
                "value": { "type": "integer", "minimum": 0 },
                "max": { "type": "integer", "minimum": 0 }
              }
            },
            "description": "Empty array when professions are unknown (and 'professions' listed in unknown_fields)."
          },
          "equipped_item_level": { "type": ["number", "null"] },
          "bag_slots_free": { "type": ["integer", "null"], "minimum": 0 },
          "bag_slots_total": { "type": ["integer", "null"], "minimum": 0 },
          "money_copper": { "type": ["integer", "null"], "minimum": 0 },
          "zone": { "type": ["string", "null"] },
          "map_id": { "type": ["integer", "null"] },
          "alive": { "type": ["boolean", "null"] },
          "ghost": { "type": ["boolean", "null"] },
          "in_combat": { "type": ["boolean", "null"] },
          "online": { "type": ["boolean", "null"] },
          "party": {
            "type": "object",
            "required": ["in_party", "leader_guid", "raid"],
            "properties": {
              "in_party": { "type": ["boolean", "null"] },
              "leader_guid": { "type": ["integer", "null"] },
              "raid": { "type": ["boolean", "null"] }
            }
          },
          "guild": { "type": ["string", "null"] },
          "observed_utc": { "type": "string", "format": "date-time",
            "description": "Per-character observation stamp (may lag the document stamp when sources differ)." },
          "unknown_fields": { "type": "array", "items": { "type": "string" },
            "description": "Names of every field whose value is null-because-unknown; dotted paths for nested facts." }
        }
      }
    }
  }
}
```

### 5.3 Field table

| Field | Required | Nullable | Notes |
|---|---|---|---|
| `schema`, `schema_version`, `source`, `observed_utc`, `characters` | REQUIRED | no | Envelope |
| `server_session_id` | REQUIRED | yes | Ties the snapshot to contract 6 identity |
| `characters[].guid`, `name` | REQUIRED | no | Minimum identity; a character whose GUID or name is unknown is not listed |
| `characters[].observed_utc`, `unknown_fields` | REQUIRED | no | The honesty machinery; `unknown_fields` may be `[]` |
| `characters[].roles`, `professions` | REQUIRED | no (empty when unknown) | Empty + listed in `unknown_fields` when not known |
| All other `characters[].*` fields | REQUIRED | yes | `null` + `unknown_fields` entry when unknown |

### 5.4 Correlation keys

`guid` correlates to job `characters.eligibleGuids`/`assignedGuids`, ownership leases (`characterGuid`), and Oracle receipts (`bot_guid`). `server_session_id` correlates to contract 1/6. `observed_utc` drives staleness gates (plan §3: planners refuse snapshots older than the job's staleness bound; default 60 s live — fail closed).

### 5.5 Versioning / fail-closed

Unknown `schema`/`schema_version` or malformed JSON → roster unavailable with a typed reason; the planner treats characters as unobservable (jobs block on `character-unavailable` / staleness rather than planning blind). A document whose `observed_utc` exceeds the consumer's staleness bound is *stale*, not invalid: the consumer reports it stale and refuses staleness-sensitive actions.

### 5.6 Worked example

```json
{
  "schema": "autowow.autopilot.roster-snapshot.v1",
  "schema_version": 1,
  "source": "fixture",
  "observed_utc": "2026-07-18T12:00:00.0000000Z",
  "server_session_id": null,
  "characters": [
    {
      "guid": 10, "name": "Northstar", "class_id": 1, "class": "warrior", "race": "human",
      "level": 12, "spec": null, "roles": ["tank", "dps"],
      "professions": [{ "skill_id": 186, "name": "Mining", "value": 75, "max": 150 }],
      "equipped_item_level": 11.4, "bag_slots_free": 9, "bag_slots_total": 36,
      "money_copper": 15230, "zone": "Westfall", "map_id": 0,
      "alive": true, "ghost": false, "in_combat": false, "online": true,
      "party": { "in_party": true, "leader_guid": 10, "raid": false },
      "guild": null,
      "observed_utc": "2026-07-18T11:59:58.0000000Z",
      "unknown_fields": ["spec", "guild"]
    }
  ]
}
```

`spec` and `guild` are `null` and declared unknown — not defaulted. `server_session_id: null` is honest for a fixture: no live session produced it.

---

## 6. Server-session identity and change semantics (**proposed**)

**Purpose.** Define how the Autopilot detects "the server I was talking to is gone" and what it must do about it, so server-side state (Oracle leases, runtime allowlists) is never trusted across a restart or redeploy.

**Producer of the identity:** Sol, via contract 1 fields `server_build` and `session_id` (parsed at `AutopilotLib.ps1:134-135`). **Consumer:** Autopilot.

### 6.1 Identity definition

The server-session identity is the ordered pair:

```
identity = (server_build, session_id)
```

taken from the current capabilities manifest. Comparison is ordinal string equality with `null` as a distinct value. Recommendations to Sol (normative for producers):

- Regenerate `session_id` on **every worldserver start** (a GUID or `ws-<utcstamp>-<4hex>` — any value never reused across starts).
- Bump `server_build` on **every deploy** (binary or module change), independent of restarts.
- Publish the refreshed manifest at startup, before any enrollment result references the new session.

### 6.2 Change detection rules (Autopilot side)

Let `prev` be the last observed identity (persisted in controller state) and `cur` the identity in the freshly read manifest.

1. **Any component change** — `server_build` differs, `session_id` differs, or either transitions between `null` and non-null — is a session change.
2. On a session change the Autopilot MUST, in order:
   a. **Emit a `server_session_changed` receipt** to the controller ledger (`autowow.autopilot.receipt.v1` envelope, `job_id: "controller"`), with data `{ previous: { server_build, session_id }, current: { server_build, session_id }, manifest_path }`.
   b. **Invalidate `leaseRef`** on every job document (`leaseRef` field, `AutopilotLib.ps1:494`): server-side leases are dead — Oracle `IntentLease`s are tick/epoch-scoped in-memory state (`AutoWowOracleContract.h:794-798`; arbiter at `:1392`) and cannot survive the process. Set `leaseRef = null` and receipt the invalidation per job.
   c. **Re-validate all non-terminal jobs.** Jobs in `Queued`/`Validating` re-validate naturally. Jobs in later phases return via the existing legal state machine: transition to `Blocked` with code `stale-state` and detail `server session changed: <prev> -> <cur>`, from which `Blocked -> Validating` is legal (`AutopilotLib.ps1:45`) and the planner's self-healing path re-validates automatically (`:1573-1596`). No phase is silently resumed against the new session.
3. **Ownership FILE leases remain valid.** `work\autopilot\ownership.json` leases (`autowow.autopilot.ownership.v1`, `AutopilotLib.ps1:757-935`) arbitrate controller-vs-controller GUID ownership on the filesystem; they are server-agnostic and expire only by time or release. A worldserver restart neither voids nor extends them.
4. **Null identity = unknown identity.** While either component of `cur` is `null` (e.g. `current.json` during maintenance: `session_id: null`), the Autopilot must not bind new server-side lease references and must treat any session-scoped claim (contract 4 grants with `allowlistApplied`, contract 2 receipts) as unverifiable against the live session — fail closed. The first non-null identity observed after a null period is compared against the last non-null identity; if it differs, rules 2a-2c apply.
5. Enrollment results referencing a dead identity follow contract 4 §4.1.6 (re-verify, do not assume).

### 6.3 Worked example

Manifest read at tick N: `("phase1-maint-20260718a", "ws-20260718-143000-9f3a")`. Manifest read at tick N+1: `("phase1-maint-20260718a", "ws-20260718-160512-02cd")` — same build, new session (worldserver restarted).

Controller ledger gains:

```json
{"schema":"autowow.autopilot.receipt.v1","schema_version":1,"seq":42,"receipt_id":"rcpt-controller-42","timestamp_utc":"2026-07-18T16:05:30.0000000Z","job_id":"controller","event":"server_session_changed","current":{"server_build":"phase1-maint-20260718a","session_id":"ws-20260718-160512-02cd"},"manifest_path":"work\\autopilot\\capabilities.json","previous":{"server_build":"phase1-maint-20260718a","session_id":"ws-20260718-143000-9f3a"}}
```

Every non-terminal job with a `leaseRef` has it nulled and (if past Validating) is Blocked(`stale-state`) pending re-validation; the ownership file is untouched; the next planner tick re-walks Validating against the new manifest.

---

## Appendix A. Shared vocabularies (cited once, used above)

- **OperationCode names** (`OperationCodeName`, `AutoWowOracleContract.h:178-230`; enum `:151-176`): `unknown`, `combat_action`, `navigate`, `quest_acquire`, `quest_accept`, `quest_objective`, `quest_turn_in`, `gather_route`, `gather_source`, `craft`, `bank_withdraw`, `bank_deposit`, `mail_receive`, `mail_send`, `vendor_buy`, `vendor_sell`, `auction_buy`, `auction_sell`, `roster_assignment`, `recover`, `pvp_objective`, `pvp_challenge`, `disengage`.
- **LeaseResource names** (`LeaseResourceName`, `AutoWowOracleContract.h:337-353`; enum `:307-314`): `idle`, `quest_gather`, `transition`, `combat_positioning`, `recovery` (precedence is the enum order and is part of the contract, `:306`).
- **ReceiptStatus names** (`ReceiptStatusName`, `AutoWowOracleContract.h:365-383`; enum `:355-363`): `accepted`, `progressing`, `completed`, `blocked`, `failed`, `rejected`.
- **ReceiptReason names** (`ReceiptReasonName`, `AutoWowOracleContract.h:412-464`; enum `:385-410`): `none`, `planned`, `lease_acquired`, `lease_already_owned`, `lease_renewed`, `lease_released`, `no_eligible_intent`, `invalid_decision`, `invalid_scope`, `invalid_frame`, `stale_frame`, `stale_epoch`, `expired`, `precondition_failed`, `lab_only_persistent_scope`, `bot_lease_conflict`, `squad_owner_conflict`, `item_reservation_conflict`, `capacity_exceeded`, `not_owner`, `executor_blocked`, `executor_failed`, `node_reservation_conflict`.
- **Autopilot capability ids** consumed against contract 1: `oracle.<operation>` (`AutopilotLib.ps1:201-203`), `bridge.<verb>` (`:196-199`), `sql.read` (`:191-194`); known-capability closed list at `:59-76`; per-kind requirements at `:229-245`.
- **Autopilot receipt envelope** (`autowow.autopilot.receipt.v1`, `AutopilotLib.ps1:10, 731-748`): `{schema, schema_version: 1, seq, receipt_id: "rcpt-<jobId>-<seq>", timestamp_utc, job_id, event, ...sorted data keys}` — JSONL, append-only.

## Appendix B. What this document does NOT claim

- No `oracle.*` operation is deployed today. The only real manifest state is the fixture `current.json`: `runtime_enabled: false`, everything undeployed, worldserver stopped for the maintenance build.
- No Oracle receipt export exists; contract 2's producer side is unbuilt.
- Contracts 4, 5, and 6's change semantics are proposed and unimplemented on both sides.
- Nothing herein implies live execution has occurred; the rehearsal evidence (52 green Pester tests, plan §10) proves the consumer-side contracts and gates, and proves nothing about deployment.
