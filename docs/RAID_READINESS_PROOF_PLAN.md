# AutoWoW raid-readiness proof plan

## Purpose

`scripts/raid-readiness-proof.ps1` is a disjoint, read-only proof lane for an exact 10-, 25-, or 40-bot raid that has already been formed, routed, and placed in one live instance. It extends the proven five-bot dungeon evidence shape to raid scale without claiming that raid formation, routing, encounter completion, or interrupt telemetry exists when the bridge cannot prove it.

The default invocation is a dry run. Live bridge access requires `-Apply`.

## Safety boundary

Live mode sends only these localhost bridge requests:

- one `list` request per sample;
- one `combatlog <guid>` request per exact roster member per sample.

It never sends `party`, `rally`, `deploy`, `route`, `advance`, `engage`, `travel`, `recover`, `fixture init`, quest commands, pause/resume, activation/deactivation, or WSG control. It does not write quest or character tables, create or kill creatures, grant gear or XP, or restart the server. It issues no movement command, so it cannot teleport a member during the encounter.

Fixture staging is deliberately absent. The current control surface exposes `fixture-init` but no safe `fixture-route`, and the current `party` bridge action is a five-player party operation rather than an exact raid constructor. An operator or a separately authorized lane must stage the raid before `-Apply` begins.

## Exact input contract

Every run declares:

- `RaidSize`: exactly `10`, `25`, or `40`;
- `RosterGuid`: exactly that many unique positive GUIDs;
- disjoint, non-empty `TankGuid` and `HealerGuid` subsets; every remaining roster GUID is expected to be DPS;
- one roster `LeaderGuid` (defaults to the first tank if omitted);
- one positive `TargetGuid` and descriptive `TargetName`;
- one positive `ExpectedMapId` and `ExpectedInstanceId`.

At preflight and every live sample, the harness requires every exact member to be online once, expose combat telemetry, have the declared primary role, report the expected map and instance, report the exact raid size, and report the expected leader. Any difference stops observation and yields `FAIL_CLOSED`.

Primary role follows the bridge's role flags in tank, healer, DPS precedence. This accepts a tank whose role mask also includes DPS while still refusing a configured DPS that is observed as a tank or healer.

## Dry-run examples

PowerShell array construction keeps long rosters readable. Replace all sample values with the intended raid contract.

```powershell
# 10: 2 tanks, 3 healers, 5 DPS
./scripts/raid-readiness-proof.ps1 `
  -RaidSize 10 -RosterGuid ([uint32[]](1..10)) `
  -TankGuid 1,2 -HealerGuid 3,4,5 -LeaderGuid 1 `
  -TargetGuid 900 -TargetName 'Raid Boss' `
  -ExpectedMapId 533 -ExpectedInstanceId 42

# 25: 2 tanks, 5 healers, 18 DPS
./scripts/raid-readiness-proof.ps1 `
  -RaidSize 25 -RosterGuid ([uint32[]](1..25)) `
  -TankGuid 1,2 -HealerGuid 3,4,5,6,7 -LeaderGuid 1 `
  -TargetGuid 900 -TargetName 'Raid Boss' `
  -ExpectedMapId 533 -ExpectedInstanceId 42

# 40: 4 tanks, 8 healers, 28 DPS
./scripts/raid-readiness-proof.ps1 `
  -RaidSize 40 -RosterGuid ([uint32[]](1..40)) `
  -TankGuid 1,2,3,4 -HealerGuid 5,6,7,8,9,10,11,12 -LeaderGuid 1 `
  -TargetGuid 900 -TargetName 'Raid Boss' `
  -ExpectedMapId 533 -ExpectedInstanceId 42
```

Add `-Apply` only after the declared raid is already in the declared instance. A live receipt is created under `logs/raid-readiness-*.jsonl`; an existing receipt is never overwritten, and a custom relative `-ReceiptPath` remains constrained beneath `logs`.

## Measurements and default gates

The receipt records compact canonical member evidence rather than interpreting `last_spell` IDs as events.

| Surface | Measurement | Default classification |
|---|---|---|
| Exact roster | GUID set, primary roles, map, instance, group size, leader | Any drift is `FAIL_CLOSED` |
| Tank threat | Exact boss threat-link victim ownership; all-hostile ownership is also reported | `PASS` at 90% or more target-owner samples held by configured tanks |
| Healer tank priority | In-combat healer target selections aimed at configured tanks | `PASS` at 50% or more |
| Non-tank triage | Per-healer opportunities where an alive non-tank is at or below 70% health, and selections of those injured members | `PASS` at 50% or more; `NOT_OBSERVED` if no opportunity occurs |
| Deaths and wipes | Alive-to-dead transitions, unique dead GUIDs, and all-dead samples | `PASS` only with zero transitions and zero wipe samples |
| Encounter duration | Time between first and last sample with combat or target evidence | `MEASURED` when encounter evidence exists |
| Boss health | First health to lowest observed health for the exact target GUID | `PASS` with at least two observations and positive delta |
| Movement/cohesion | Member travel distance, maximum step, and maximum live-member radius around the centroid | `PASS` when at least 80% of samples remain within 60 yards and no step exceeds 100 yards |
| Interrupts | Delta of explicitly exposed cumulative interrupt counters | `MEASURED`, `PARTIAL`, or `UNSUPPORTED`; never inferred from spell IDs |

Thresholds are command parameters so a specific raid's mechanic envelope can be declared in advance. Lowering thresholds changes acceptance policy and should be preserved in the receipt.

`PASS` requires every gated surface to pass. Missing target threat, boss health, position, healer selection, or non-tank injury opportunity produces `PARTIAL`, `UNSUPPORTED`, or `NOT_OBSERVED` rather than a false success. Interrupt counters are opportunistic and do not block an otherwise supported verdict.

## JSONL receipt sequence

Each line uses schema `autowow.raid-readiness.proof.v1`, UTC timestamp, run ID, and event name:

1. `raid_readiness_started` — immutable plan, exact contract, safety boundary, and thresholds;
2. `raid_readiness_preflight` — exact member/role/map/instance evidence;
3. `raid_encounter_observed` — first combat or exact-target observation;
4. `raid_readiness_sample` — target, threat ownership, and canonical per-member evidence;
5. `raid_readiness_guard_failed` or `raid_readiness_error` when applicable;
6. `raid_readiness_verdict` — aggregation, unsupported telemetry, receipt path, and fatal error if any.

## Focused verification

```powershell
$paths = @(
  'scripts/raid-readiness-proof.ps1',
  'scripts/raid-readiness-proof-lib.ps1',
  'scripts/tests/raid-readiness-proof.tests.ps1'
)
foreach ($path in $paths) {
  $tokens = $null; $errors = $null
  [System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $path), [ref]$tokens, [ref]$errors) | Out-Null
  if ($errors) { $errors }
}

Invoke-Pester -Path scripts/tests/raid-readiness-proof.tests.ps1 -Output Detailed
```

The focused tests cover dry-run behavior for all three exact sizes, invalid contracts, forbidden live action dispatch, fail-closed roster drift, aggregation math, death/wipe handling, unsupported interrupt reporting, and parser cleanliness.
