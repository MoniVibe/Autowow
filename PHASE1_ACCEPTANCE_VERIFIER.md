# Phase 1 quest acceptance verifier

`scripts/phase1-acceptance-verifier.ps1` is an offline-first verifier for the Phase 1 objective-lock
acceptance gate. It reads recorded JSONL evidence and, optionally, a saved `questobjective` bridge
response. It writes deterministic JSON and Markdown reports and does not build, restart, configure, or
control AutoWoW.

A report is `PASS` only when all four fixtures have the required objective evidence, the required
release invariants are observed and pass, and the input records have a supported schema. Missing or
unversioned evidence is `INCONCLUSIVE`; it is never treated as success. An explicit wrong entry,
non-zero forbidden action, unsupported objective, or other contradiction is `FAIL`.

## Offline usage

The built-in fixture mode is synthetic test data only:

```powershell
pwsh -NoProfile -File .\scripts\phase1-acceptance-verifier.ps1 `
    -ServerRoot . `
    -OfflineFixture `
    -JsonReportPath .\logs\phase1-acceptance-verifier.json `
    -MarkdownReportPath .\logs\phase1-acceptance-verifier.md
```

With no input path, the verifier still writes reports, but the result is `INCONCLUSIVE`:

```powershell
pwsh -NoProfile -File .\scripts\phase1-acceptance-verifier.ps1 -ServerRoot .
```

For recorded evidence:

```powershell
pwsh -NoProfile -File .\scripts\phase1-acceptance-verifier.ps1 `
    -ServerRoot . `
    -EvidencePath .\logs\phase1-acceptance.jsonl
```

`-JsonReportPath` and `-MarkdownReportPath` are optional. By default they are
`logs\phase1-acceptance-verifier.json` and `logs\phase1-acceptance-verifier.md` under `ServerRoot`.
No wall-clock timestamp is inserted into either report. For byte-for-byte comparisons, keep the input
and output paths unchanged.

## Input contracts and schema guard

The preferred evidence envelope is:

```json
{
  "schema": "autowow.phase1.acceptance.evidence.v1",
  "schema_version": 1,
  "event": "phase1_acceptance_snapshot",
  "scope": "quest",
  "bot_guid": 42,
  "quest_id": 792,
  "objective": {
    "quest_id": 792,
    "objective_family": "npc_or_gameobject",
    "objective_slot": 0,
    "objective_kind": "creature_credit",
    "phase": "verify_reward",
    "failure_reason": "none",
    "supported": true,
    "required_npc_or_go_entry": 3101,
    "required_item_id": 0,
    "current_count": 8,
    "required_count": 8,
    "baseline_count": 0,
    "selected_source_entry": 3101,
    "selected_target_guid": 123,
    "finisher_entry": 3145,
    "finisher_guid": 456,
    "has_lock": true
  },
  "invariants": {
    "unrelated_offensive_target_count": 0,
    "random_grind_fallback_count": 0,
    "teleport_used": false,
    "direct_quest_db_mutation_count": 0,
    "exact_finisher_match": true,
    "reward_postcondition_confirmed": true,
    "objective_context_present": true
  }
}
```

Each JSONL line is an independent object. A run-global invariant record may omit `quest_id`; it is
applied to each fixture only when it contains at least one recognized invariant field. A record with an
unsupported schema is rejected rather than interpreted.

The current Phase 1 source worktree emits the following unversioned bridge shape for
`questobjective <bot-guid>`:

```json
{
  "ok": true,
  "guid": 42,
  "objective": {
    "quest_id": 792,
    "objective_family": "npc_or_gameobject",
    "objective_slot": 0,
    "objective_kind": "creature_credit",
    "phase": "verify_progress",
    "failure_reason": "none",
    "supported": true,
    "required_npc_or_go_entry": 3101,
    "required_item_id": 0,
    "current_count": 4,
    "required_count": 8,
    "baseline_count": 3,
    "selected_source_entry": 3101,
    "selected_target_guid": 123,
    "finisher_entry": 3145,
    "finisher_guid": 456,
    "has_lock": true
  }
}
```

The verifier recognizes this source-confirmed shape, validates all listed objective fields when the
response is `ok=true`, and emits `bridge_schema_unversioned`. A raw bridge capture therefore cannot
produce a release `PASS` by itself; it has no reward postcondition or release-invariant telemetry and
the endpoint currently has no version field. A saved response can be supplied with
`-BridgeSnapshotPath path\to\questobjective.json`; JSONL bridge captures are also accepted.

Expected field aliases are centralized in `$script:Phase1FieldMap` in the verifier. The supported
aliases are:

| Meaning | Accepted paths |
|---|---|
| Quest id | `quest_id`, `objective.quest_id` |
| Bot guid | `bot_guid`, `guid` |
| Objective identity | `objective_family`, `objective.objective_family`, `objective.family`; `objective_kind`, `objective.objective_kind`, `objective.kind` |
| Required entries/counts | `required_item_id`, `objective.required_item_id`, `objective.required_item`; `required_npc_or_go_entry`, `objective.required_npc_or_go_entry`, `objective.required_entry`; `required_count`, `objective.required_count` |
| Progress | `baseline_count`, `objective.baseline_count`; `current_count`, `objective.current_count` |
| Source/finisher | `selected_source_entry`, `objective.selected_source_entry`; `finisher_entry`, `objective.finisher_entry`, `finisher.entry` |
| Optional resolved source list | `resolved_source_entries`, `allowed_source_entries`, `objective.resolved_source_entries`, `objective.allowed_source_entries` |
| Objective state | `supported`, `objective.supported`; `failure_reason`, `objective.failure_reason`; `has_lock`, `objective.has_lock` |
| Context/reward | `objective_context_present`, `invariants.objective_context_present`; `reward_postcondition_confirmed`, `reward_confirmed`, `reward.turnin_verified`, `invariants.reward_postcondition_confirmed` |
| Release counts | `unrelated_offensive_target_count`, `invariants.unrelated_offensive_target_count`; `random_grind_fallback_count`, `random_grind_fallbacks`, and their `invariants.*` forms; `direct_quest_db_mutation_count` and its `invariants.*` forms |
| Teleport | `teleport_used`, `invariants.teleport_used`; `teleport_count`, `invariants.teleport_count` |
| Explicit finisher invariant | `exact_finisher_match`, `invariants.exact_finisher_match` |

Unknown aliases are ignored. A field that is absent, malformed, or only available from an unsupported
schema is not converted into a default value.

## Phase 1 fixture rules

The verifier checks the exact family/kind where the Phase 1 source contract provides it, the required
objective entry, the selected source allowlist, the exact finisher, support/failure state, and a positive
objective count delta from the recorded baseline. q792 additionally requires `required_count == 8`.
Item fixtures require a positive observed `required_count` but do not hardcode an item count beyond the
facts listed here.

| Fixture | Objective | Required entry | Allowed source entries | Exact finisher |
|---|---|---:|---|---:|
| q459 / 459 | item `3297` | item `3297` | `1988`, `1989` | `1992` |
| q789 / 789 | item `4862` | item `4862` | `3124`, `3281` | `3143` |
| q792 / 792 | creature `3101`, count `8` | creature `3101` | `3101` | `3145` |
| q916 / 916 | item `5166` | item `5166` | `1986` | `2082` |

The bridge payload currently exposes `selected_source_entry`, not the full resolved source vector. The
verifier checks the observed selection against the fixture allowlist; if a versioned evidence record
also supplies a resolved source list, it must match the fixture allowlist exactly. It never fabricates a
missing source list.

## Release invariants

When a recognized field is present, the verifier evaluates it. A release `PASS` requires every invariant
to be observed and passing:

- `unrelated_offensive_target_count == 0`
- `random_grind_fallback_count == 0`
- `teleport_used == false/0` and/or `teleport_count == 0`
- `direct_quest_db_mutation_count == 0`
- exact finisher entry/match
- `reward_postcondition_confirmed == true`
- objective context present (`objective_context_present == true`, or an active `has_lock == true`)
- one observed `current_count > baseline_count` pair

An invariant that is absent is `NOT_OBSERVED` at check level and makes the overall result
`INCONCLUSIVE`. A non-zero/false/mismatched value is an actual `FAIL`. This distinction is important
for existing telemetry such as `quest-telemetry` JSONL, which predates the Phase 1 objective snapshot.

## Live snapshot safety

`-LiveSnapshot` is a dry run unless `-AllowLiveRead` is also supplied:

```powershell
# No socket is opened; this only records the intended read in the report.
pwsh -NoProfile -File .\scripts\phase1-acceptance-verifier.ps1 -ServerRoot . -LiveSnapshot -BotGuid 42

# Explicit opt-in: loopback only, one read-only request.
pwsh -NoProfile -File .\scripts\phase1-acceptance-verifier.ps1 -ServerRoot . `
    -LiveSnapshot -AllowLiveRead -BotGuid 42
```

The opt-in path accepts only `127.0.0.1`, `localhost`, or `::1`, sends exactly
`questobjective 42`, reads one response line, and closes the socket. It does not call
`autowow-control.ps1` and cannot issue `quest`, `recover`, `pause`, `resume`, `travel`, or another
control command. No MySQL path is used.

## Tests and proof boundary

Run the owned offline tests with:

```powershell
Invoke-Pester -Path .\scripts\tests\phase1-acceptance-verifier.tests.ps1
```

The tests use Pester `TestDrive` fixtures and cover synthetic PASS, deterministic output, missing
evidence, explicit source failure, invariant failure, saved unversioned bridge data, unsupported schema,
and live dry-run behavior. They do not prove that the Phase 1 C++ changes are built or live. A live
release claim still requires a separate maintenance-window capture containing versioned evidence and
the required postcondition/invariant telemetry.
