# Probe Lab runbook

Probe Lab safely stages and observes multiple exact dungeon/raid rosters through the existing
loopback AutoWoW bridge. It is generalized around named routes, named admission waypoints, and
exact GUID manifests; it contains no boss or interior coordinate logic.

## Safety boundary

- `Plan` is the default and performs no bridge calls or file writes.
- `Launch` remains a dry run unless `-Apply` is explicitly supplied.
- `Monitor` is live but read-only; it rejects `-Apply` and uses only `list` and `combatlog`.
- `scripts\probe-lab-run.ps1` is the one-command entry point; it defaults to dry-run `Plan`.
  Its `LaunchThenMonitor` mode requires `-Apply` and delegates to the existing launcher and monitor.
- The bridge host is fixed to `127.0.0.1`.
- The harness never accepts/stores passwords, touches a database, starts/stops/restarts a server,
  or uses rally/travel/recover/interior teleport operations. Launch applies the ordinary grouped
  `deploy` profile after staging/admission so followers receive standard follow/combat behavior.
- All receipts and summaries are constrained beneath `logs` and existing files are not overwritten.
- Named `route` is an exterior staging route. Optional named `advance` is repeated as an ordinary
  movement request so the server's authoritative trigger can admit members serially. It is never
  evidence of, or a substitute for, teleportation.

## Manifest

Copy `scripts\fixtures\probe-lab.example.json` and replace its illustrative GUIDs, names, fixture
settings, expected destination map, exterior routes, and optional admission waypoints. Every probe
requires a positive `expected_map_id`. Party probes require exactly 5 unique
members. Raid probes require exactly 10, 25, or 40. Every GUID may appear only once across the
whole lab, every leader must be unique and present in its own roster, and route names are safe
tokens without whitespace. Do not add password, secret, credential, or token fields.

Omit `advance_waypoint` where admission is owned by native `DungeonTransition` (for example, The
Nexus). Where a named admission waypoint exists, use the real exterior/portal names (for example,
`voa-exterior` plus `voa-portal`). Optional `failure_policy.fail_on_any_death` defaults to `false`;
set it to `true` only when any individual death should fail that probe.

## Commands

Run from `D:\Games\wowstuff\AutoWoW`:

```powershell
# Default dry-run plan. Safe even when the bridge/server is offline.
& .\scripts\probe-lab.ps1 -ManifestPath .\scripts\fixtures\my-probes.json

# Same safe default through the one-command wrapper.
& .\scripts\probe-lab-run.ps1 -ManifestPath .\scripts\fixtures\my-probes.json

# Restrict every phase to one exact manifest probe.
& .\scripts\probe-lab-run.ps1 -Mode Plan `
    -ManifestPath .\scripts\fixtures\probe-lab-live-dungeons.json `
    -ProbeId dtk-party

# Launch exact GUIDs, exact fixture level/spec/quality, groups, and exterior routes.
& .\scripts\probe-lab.ps1 -Mode Launch -Apply `
    -ManifestPath .\scripts\fixtures\my-probes.json

# Read-only observation with log-derived DungeonNavigator blocked reasons.
& .\scripts\probe-lab.ps1 -Mode Monitor `
    -ManifestPath .\scripts\fixtures\my-probes.json `
    -DurationSeconds 600 -PollSeconds 5 `
    -PlayerbotsLogPath .\logs\phase1-runtime\Playerbots.log

# Apply the exact launch, then begin the read-only monitor in one command.
& .\scripts\probe-lab-run.ps1 -Mode LaunchThenMonitor -Apply `
    -ManifestPath .\scripts\fixtures\probe-lab-live-dungeons.json `
    -ProbeId dtk-party `
    -DurationSeconds 600 -PollSeconds 5 `
    -PlayerbotsLogPath .\logs\phase1-runtime\Playerbots.log
```

Launch fails closed if an exact member is already in a partial/foreign group. If a probe has an
`advance_waypoint`, Launch repeats it until all exact members report `expected_map_id` and one
common nonzero instance or the admission timeout expires. Launch isolates operation failures to
the affected probe: its result and `probe_launch_failed` receipt include the failure code,
operation, detail, optional GUID, and timestamp, while later independent probes continue launching.
After staging (and explicit admission where required), Launch applies the standard grouped `deploy`
profile; it does not relocate anyone inside an instance.
The aggregate Launch status is `LAUNCHED`, `PARTIAL`, or `FAILED`. A global operation that cannot be
attributed to one probe still fails the run. Run probes from an exterior staging position; the
harness does not move a roster directly to an interior location.

Monitor writes one JSONL receipt plus concise JSON and Markdown summaries under `logs`. It preserves
the first observed failure per probe and recognizes `offline`, `split_instance`, `dead_party`,
`wipe`, `cohesion_stall`, `admission_timeout`, `no_progress`, `bridge_error`, and log-derived
`DungeonNavigator blocked=<reason>` evidence. A transient `blocked=party_cohesion` is retained as
telemetry and becomes `cohesion_stall` only after the configured stall timeout. Final current health
is `PASS` only when all exact members are alive and no first failure exists; it is `DEGRADED` when
dead/released members are a minority and the party remains viable; and it is `FAIL` when a first
failure exists or dead members are at least as numerous as living members. Thus one incidental death
remains an observation by default, while `fail_on_any_death` still fails immediately. Aggregate
status is the worst probe status: `FAIL` over `DEGRADED` over `PASS`. A monitor run does not attempt
recovery or mutation.

## Verification

```powershell
Invoke-Pester .\scripts\tests\probe-lab.tests.ps1 -Output Detailed

$paths = @('.\scripts\probe-lab-lib.ps1', '.\scripts\probe-lab.ps1', '.\scripts\probe-lab-run.ps1', '.\scripts\tests\probe-lab.tests.ps1')
foreach ($path in $paths) {
    $tokens = $null; $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $path), [ref]$tokens, [ref]$errors) | Out-Null
    if (@($errors).Count) { $errors; throw "Parse failed: $path" }
}
```
