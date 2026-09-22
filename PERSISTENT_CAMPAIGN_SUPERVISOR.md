# Persistent campaign supervisor

`scripts\persistent-campaign-supervisor.ps1` is the bounded process supervisor for the eventual
Horde-vs-Alliance 1-80 persistent campaign. It is a one-shot reconcile operation: run it again after
a restart or child expiry and it adopts valid children or starts only missing roles.

The default is dry-run and emits one JSON plan. `-Apply` is required before the supervisor can create
its lock/state/receipt files or start a child process. This lane was not launched during authoring.

```powershell
# Read-only plan/status; safe default
pwsh -File .\scripts\persistent-campaign-supervisor.ps1

# Explicitly reconcile/start the four existing runners
pwsh -File .\scripts\persistent-campaign-supervisor.ps1 -Apply
```

The four managed roles are existing runner scripts:

- `league-simulation-director.ps1` — authoritative Quest Director; the supervisor never sends quest,
  party, gathering, or teleport orders and never starts it with `-ObserveOnly`.
- `autowow-overnight-guardian.ps1` — guardian. It is matched by its exact script path and exact
  `-QuestDirectorPid` association; it is not `league-simulation-monitor.ps1`.
- `league-progression-observer.ps1` — progression observer.
- `league-sanity-observer.ps1` — sanity observer.

The `start-league-*.ps1` paths are recorded as the established launcher references for the director
and observers, but the supervisor starts the underlying runner directly. The overnight guardian has
no dedicated start wrapper and is launched directly. This avoids inheriting launcher stale-PID-file
deletion behavior. Existing PID files are read-only discovery hints, never cleanup authority.

Guardian launches use the existing contract: `-DurationHours` is the safe ceiling-rounded conversion
of `-DurationMinutes`, `-PollSeconds` is passed through, `-QuestDirectorPid` is the adopted or newly
started Quest Director PID, and `-ReleaseLiveMutations` is added only on an explicit `-Apply` start.
The guardian launch does not require `-ServerRoot`.

## Safety and restart behavior

- Discovery uses Windows process command lines. A child is adoptable only when `-File`, the exact
  runner script path, the exact `-ServerRoot` where that role requires it, and role policy flags
  match. The guardian additionally requires the exact `-QuestDirectorPid` and
  `-ReleaseLiveMutations` contract. A PID reuse or stale PID record therefore cannot cause adoption
  of an unrelated process.
- More than one valid process for a role, an observe-only Quest Director, or a contradictory
  teleport/travel flag fails closed. The supervisor does not kill either process.
- The lock is an exclusive file handle at
  `work\persistent-campaign-supervisor\supervisor.lock`. It is non-destructive, released by the OS
  when the process exits, and is never deleted. A stale lock file is not evidence of a live lock;
  only an active exclusive handle blocks a reconcile.
- State is JSON at `work\persistent-campaign-supervisor\state.json`. It contains schema/version,
  current phase, per-role PIDs, receipt paths, per-child lease timestamps, supervisor lease
  timestamps, exact command lines, and policy metadata. Stale PIDs remain recorded as evidence until
  a successful reconcile replaces the state; no process is stopped or removed.
- Supervisor receipts append to
  `leagues\results\persistent-campaign-supervisor.jsonl`. The concise latest status snapshot is
  `logs\persistent-campaign-supervisor-status.json`. Child receipt paths are also recorded in state.
- The policy is `real_travel`, with teleport disabled and the league fallback disabled. This is
  metadata and validation only; it does not alter server configuration. Contradictory child command
  line flags fail closed.

## Phase gates

`-Phase questing` and `-Phase gathering` are open current campaign metadata gates. The supervisor
keeps the same four process roles and does not invoke the gathering bridge/orchestrator itself.

`dungeon`, `raid`, and `pvp` are future gates. They remain locked unless both `-EnableFuturePromotions`
and `-EvidencePath` are supplied. Evidence must be JSON with schema
`autowow.campaign.evidence.v1`, the requested phase, `verdict: PASS`, and true
`no_teleport`, `no_database_writes`, and `no_server_restart` gates. Even when those checks pass,
the result is `eligible_metadata_only`: this bounded supervisor has no future gameplay mutator and
will not issue bridge orders or launch an unimplemented promotion.

## Proof

Offline focused tests:

```powershell
Invoke-Pester -Path .\scripts\tests\persistent-campaign-supervisor.tests.ps1 -Output Detailed
```

The tests parse the script, verify default dry-run creates no files or processes, exercise exact
command-line adoption and fail-closed policy checks, cover current/future phase gates, and assert
that the new supervisor contains no `Stop-Process` or `Remove-Item` cleanup operation. No server,
bridge, database, account, character, teleport, or live child process is used by the test suite.
