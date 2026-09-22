# AutoWoW Autopilot V1.2 - persistent player experience (dry-run by default)

Express a long-running intention in a campaign file, start the controller, walk
away, and later understand exactly what the characters accomplished, why they
stopped, and what they need next. Strategic layer only: every tactical action
belongs to the server Oracle and the AutoWow bridge; unavailable server
capabilities are never emulated - jobs wait, honestly, with reasons.

Design: `..\AUTOWOW_AUTOPILOT_V1_PLAN.md` (V1.1 §10, V1.2 §11)
Contracts for Sol: `..\AUTOWOW_ORACLE_INTEGRATION_CONTRACTS_V1.md`
Schemas: `..\AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json`, `schemas\profile.schema.json`, `schemas\campaign.schema.json`

## The hands-free loop

```powershell
# 1. Describe intent (see campaigns\northstar-sample.json):
#    Brandreas quests toward 20 and learns Mining; Kurl stockpiles copper ore.
.\autowow-autopilot.ps1 campaign validate -Path .\campaigns\northstar-sample.json
.\autowow-autopilot.ps1 campaign plan     -Path .\campaigns\northstar-sample.json   # every job, capability, prerequisite, blocker - BEFORE anything runs
.\autowow-autopilot.ps1 campaign apply    -Path .\campaigns\northstar-sample.json   # creates jobs + enrollment proposals; sends nothing

# 2. Start the persistent controller (foreground; Ctrl+C releases everything):
.\autowow-autopilot.ps1 run -DurationMinutes 480 -TickSeconds 30
#    ... or detached: .\autowow-autopilot.ps1 start   /   stop

# 3. Come back later:
.\autowow-autopilot.ps1 status
.\autowow-autopilot.ps1 summary
.\autowow-autopilot.ps1 history -CharacterGuid 10
.\autowow-autopilot.ps1 explain -CharacterGuid 6
.\autowow-autopilot.ps1 blockers

# 4. Lifecycle (V1.3): resume is resume - it never resets clocks. A job whose
#    wall clock expired is refused with a diagnostic; create an honest successor:
.\autowow-autopilot.ps1 retry -JobId <finished-or-expired-job> -Reason 'why'
#    The successor gets a new id, idempotency namespace, clocks, budgets, and
#    ledger, with provenance (supersedesJobId/retryReason); the original job and
#    its ledger are never touched. Short decisive runs: add -StopOnQuiescence.
```

Party-contract refusals from the bridge (`quest_requires_party_leader`,
`party_member_exact_quest_preflight_failed`, `quest_not_in_member_log`,
`quest_not_in_leader_log`, `cross_faction_party_not_allowed`) are planning
preconditions: they block with `party-contract-unmet` receipts, spend zero
recovery budget, and are never re-issued or worked around. The one exception is
`quest_not_in_leader_log`, which re-plans toward `quest <leader> acquire` when
acquisition is permitted. Live sends additionally require a FRESH Sol-published
`source: "deployed-runtime"` capability manifest - interim probes and fixtures
never authorize live mutation.

Blocked domains print `planned, currently unavailable: <capability> (<reason>)`
and unblock automatically the moment a Sol-published capability manifest
evidences the operation - the controller re-validates every tick.

## Safety posture (unchanged, now persistent)

- Dry-run default; a live loop refuses until the supervised-quester prerequisites exist.
- Capability truth ONLY from `autowow.oracle.capabilities.v1` manifests (source
  presence, passing unit tests, compiled-but-undeployed runtimes all count as
  undeployed; unknown schema versions fail closed).
- One gameplay mutator per character (ownership leases; foreign leases never
  superseded); live sends need the quintuple gate (live mode + confirm +
  allowlist + active ownership + capability evidence).
- Profiles/campaigns are strategic preference, never execution authority.
- Success is receipted evidence only; unknown metrics say "unknown", never 0.
- No teleports, no DB writes, no grants, no grinding fallback, no secrets in logs.

## Layout

- `AutopilotLib.ps1` - core: jobs, receipts, state machine + timeouts/recovery,
  capability/ownership/enrollment/oracle-receipt providers, clock, checkpoint,
  session identity, QuestLevel + Gather executors (dry-run).
- `modules\` - `AutopilotScheduler` (one-mutator slots, aging, dependencies,
  explicit idle), `AutopilotRunLoop` (persistent tick loop), `AutopilotCampaign`
  (validate/plan/apply/status/explain/stop), `AutopilotGoals` (deterministic
  dependency DAG), `AutopilotProfiles`, `AutopilotRoster` (read-only facts +
  suitability), `AutopilotSummary` (receipt-backed reporting).
- `profiles\` - quester, gatherer, crafter, dungeon, raider, battleground,
  all-rounder, idle-support.
- `campaigns\` - sample campaign documents.
- `fixtures\` - bridge responses, capability manifests, roster snapshots,
  oracle receipts.
- `tests\` - 8 Pester suites, 187 tests, fully offline, including the
  deterministic eight-hour simulated soak (`soak.tests.ps1`).

## Tests

```powershell
Invoke-Pester -Path .\autopilot\tests\           # everything (~6 min)
Invoke-Pester -Path .\autopilot\tests\soak.tests.ps1 -Output Detailed
```

## Deliberately out of scope

Web dashboard (the CLI is the contract a dashboard would visualize), live-mode
execution, Dungeon/Raid/economy executors (accepted, planned, honestly blocked),
and any writing to the module worktrees, server config, or the running server.
