# Micro-scenario acceleration V1

Date: 2026-07-20

## Result

The fixture-only acceleration seam is implemented in the isolated Playerbots source lane and builds successfully. It is not enabled in the live runtime. The default configuration remains closed because `AutoWow.MicroScenarioGuids` is empty unless explicitly enrolled.

`1000% speed` is defined as exactly 10 times the fixture's saved movement-rate baseline. Acceleration saves all supported movement rates and restores them through an idempotent cleanup request. It does not change campaign XP, loot, gathering, or persistent bot configuration.

## Control surface

```text
fixture accelerate <guid> 1000
fixture accelerate-off <guid>
fixture kill <guid> entry <entry> spawn <stable-spawn-id>
```

The C++ adapter accepts only an allowlisted online Playerbot. It rejects real players, missing/dead/paused/in-combat fixtures for activation, and mismatched targets. Cleanup remains available while a fixture is paused, dead, or in combat so a failed test can restore its movement rates.

Exact kill requires the same map and instance, exact creature entry, exact stable spawn id, an active acceleration fixture, and a bounded distance. It rejects world bosses. It invokes the native server kill path and explicitly does not write quest status, loot, inventory, or material credit. Therefore this is a test accelerator, not proof that ordinary combat AI could win the encounter without assistance.

## Runner

The reusable runner is [micro-scenario.ps1](scripts/micro-scenario.ps1). It is dry-run by default. Live execution requires both `-Apply` and `-Execute`, records append-only JSONL receipts, pins the manifest hash, and supports idempotent resume.

The accelerated example is [micro-scenario-accelerated-smoke.json](scripts/tests/fixtures/micro-scenario-accelerated-smoke.json). Its order is:

1. read a snapshot;
2. accelerate the fixture to 10x;
3. observe it for a bounded window;
4. run `fixture-accelerate-off` in the explicit cleanup phase.

The example is a plan fixture. It does not run against the live bridge by itself.

## Verification

- PowerShell micro-scenario and catalog tests: **12/12 passed**.
- Isolated WSL `worldserver` and `unit_tests` build: **passed** with `BUILD_TESTING=ON`.
- Focused GoogleTest contract filter: **118/118 passed**, including `FixtureAccelerationControlTest`.
- PowerShell parse checks: **clean**.
- Exact request construction:
  - `fixture accelerate 236 1000`
  - `fixture accelerate-off 236`
  - `fixture kill 236 entry 324 spawn 201104`

The WSL build reports an existing Windows-worktree metadata warning and labels the revision archived/unknown; this is build metadata, not a compilation failure.

## Live safety state

The live bridge was read with `list` only. At the check, the existing runtime reported 10 online bots, including GUID 236 alive and out of combat. No acceleration, kill, quest, inventory, database, or config mutation was issued. The runtime config has the existing `AutoWow.FixtureGuids` list but no `AutoWow.MicroScenarioGuids` enrollment, so the adapter is fail-closed.

The running worldserver process was not restarted. Its build output path was rebuilt during isolated verification, so a future restart of that process would load the newly built binary; the empty runtime allow-list still prevents acceleration commands from succeeding. Treat deployment/restart as a deliberate next step, not as part of this proof.

## Remaining gate

The accelerated quest and harvest manifests remain blocked until a separate fixture-seed adapter can create disposable setup state through native server APIs:

- character/race/class/spec/talents and bounded gear;
- profession and skill state;
- tools, bags, and reagent/material inputs;
- exact quest/giver/objective or gathering source binding;
- lease and cleanup/reset receipts.

Those setup mutations must never be mistaken for quest completion, gathered output, or craft output. After that adapter exists, the next proof should deploy the isolated binary intentionally, enroll one disposable GUID, run one quest and one harvest plan, and require native quest/objective/inventory evidence.
