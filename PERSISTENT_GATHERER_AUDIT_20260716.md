# Persistent gatherer audit — 2026-07-16

## Verdict

The lane has real persistent gathering evidence for Kurl (GUID 6), but the latest exact-roster proof is not an all-member pass. Kurl passed the gathering gate. Pikli (GUID 49) gained one class-7 item and one Herbalism point, but also lost two Runecloth; the proof correctly marked that member and the 10-minute run `fail` because the inventory delta is confounded. The live bridge/worldserver was offline for this audit, so no live state was changed or re-queried.

No C++ files were edited and no build, server, MySQL, or bridge mutation was performed.

## Registered roster and status

The authoritative fixed gather manifest is [scripts/gather-lane-manifest.json](scripts/gather-lane-manifest.json). It registers:

| GUID | Name | Declared gather profession | Roster status |
|---:|---|---|---|
| 3 | Emealar | Herbalism | explicit gather cohort |
| 6 | Kurl | Mining | explicit gather cohort |
| 49 | Pikli | Herbalism | explicit gather cohort |

The latest read-only telemetry also contains `role=worker`, `wayfarer`, GUID 17 `Kornzoggoch`, with no declared profession. It is not in the manifest and is now surfaced as `unregistered_worker_rows` by the new status script. This is a roster/data gap, not evidence that GUID 17 gathered.

Existing provision receipts [provision-20260716-091420696.json](logs/gather-lane/provision-20260716-091420696.json) and [provision-20260716-091436573.json](logs/gather-lane/provision-20260716-091436573.json) verify GUIDs 3/6/49 as `wayfarer`, `worker`, `economy-gather-v1`; both report zero character/account/skill/inventory/world writes. They do report one league metadata mutation (`mutations=1`), so this was not a zero-write operation overall.

## Evidence classification

| Question | Finding | Exact evidence |
|---|---|---|
| Professions/skills | Present before observation; learning/training was not proved. Kurl Mining 300→303; Pikli Herbalism 300→301 in the exact proof. | [gather-lane-20260716-092733872-proof.json](logs/gather-lane/gather-lane-20260716-092733872-proof.json), `member_proofs` |
| Actual acquisition | Kurl has strong indirect acquisition evidence: +37 Truesilver/Thorium/Dense Stone units and no negative material delta. Pikli has only +1 Buzzard Meat and a conflicting -2 Runecloth delta. | same proof, `member_proofs[GUID 6]` and `[GUID 49]` |
| Persistence | The harness took a final live snapshot, deactivated the exact workers, waited for normal logout, then read SQL again. It recorded no direct DB writes and no server lifecycle action. | same proof, `attribution`, `safety`, `limitations`; [scripts/gathering-proof.ps1](scripts/gathering-proof.ps1) |
| Travel | The last successful gather status showed Emealar `mmap_walk_pending` toward Lunar Fungal Bloom and Pikli `mmap_walk_started` toward Hellfire Spineleaf; Kurl was `seeking` with no candidate. All three were solo/ungrouped/alive in that status. | [gather-lane-20260716-093946687.jsonl](logs/gather-lane/gather-lane-20260716-093946687.jsonl) |
| Teleport | No teleport command or teleport fallback was used by the gather wrapper; the campaign policy is `real_travel` with teleport disabled. | [scripts/gather-lane.ps1](scripts/gather-lane.ps1), [PERSISTENT_CAMPAIGN_SUPERVISOR.md](PERSISTENT_CAMPAIGN_SUPERVISOR.md) |
| Random movement | No teleport/random cheat is evidenced. However, the existing design documents the worker deploy strategy as `+gather,+grind,+move random,-follow`; this is AI roaming, not a teleport, but it means a stricter “no random roaming” claim is not yet proved. | [GATHERING_PROGRESSION_V0.md](GATHERING_PROGRESSION_V0.md) |
| Death/corpse recovery | Not proved. Pikli was alive after deploy but `alive=false` in the final proof snapshot; the control receipt has no `recover` action and the run ended through deactivation/logout. | same proof, `control_receipts`, `limitations` |
| Economy | Inventory persistence is evidenced. Money/auction/vendor attribution is not: the read-only postdeploy snapshots have no material rows, and the telemetry contract labels vendor flow `inferred_only`. | [gathering-telemetry-postdeploy-20260716-131739-baseline.json](logs/gathering-telemetry-postdeploy-20260716-131739-baseline.json), [gathering-telemetry-postdeploy-20260716-132858-current.json](logs/gathering-telemetry-postdeploy-20260716-132858-current.json) |

The exact proof ran for 600.468 seconds (`2026-07-16T12:27:34.699+03:00` through `12:37:35.167+03:00`). Its safety receipt reports `direct_db_writes=0`, `server_start_stop_restart_actions=0`, `server_config_writes=0`, `chat_commands_sent=0`, and `profession_training_actions=0`; the 14 bridge calls were the proof’s existing list/deploy/snapshot/deactivate lifecycle, not this audit.

After the run, the existing status receipt showed all three gatherers offline while unrelated GUIDs remained online: [gather-lane-20260716-093759790.jsonl](logs/gather-lane/gather-lane-20260716-093759790.jsonl).

## Bounded improvement made

Added [scripts/persistent-gatherer-status.ps1](scripts/persistent-gatherer-status.ps1), a single read-only status surface. It:

- validates the explicit manifest and reports every registered gatherer;
- probes `127.0.0.1:18787` without sending a bridge payload;
- sends only read-only `list`/`snapshot` requests if the endpoint is reachable;
- detects `bridge_disconnected` and `bridge_reconnected` transitions using a small state file;
- folds in the latest gather status, proof, and read-only telemetry;
- flags worker rows outside the manifest; and
- appends compact, secret-free JSONL evidence.

The audit sample is [logs/persistent-gatherer-status.jsonl](logs/persistent-gatherer-status.jsonl). It recorded `bridge_status=offline`, `event=bridge_status`, exact cohort state from the latest local evidence, GUID 17 as unregistered, and safety values `db_writes=0`, `bridge_mutations=0`, `server_start_stop_restart_actions=0`, `secrets_recorded=false`. Its state is [work/persistent-gatherer-status/state.json](work/persistent-gatherer-status/state.json).

Added the offline contract tests in [scripts/tests/persistent-gatherer-status.tests.ps1](scripts/tests/persistent-gatherer-status.tests.ps1).

## Proof run

Focused offline Pester run: **34 passed, 0 failed, 0 skipped**.

```text
Invoke-Pester -Path persistent-gatherer-status, gathering-proof,
  gathering-telemetry, gathering-orchestrator,
  persistent-campaign-supervisor, persistent-campaign-watch -Output Detailed
```

Both new PowerShell files parse with zero parser errors. The live bridge was only TCP-probed and remained unreachable; no bridge mutation was issued.

## Remaining core/C++ gaps

1. The proof currently attributes temporal class-7 inventory deltas, not a per-node acquisition event. Add a read-only node-acquisition receipt/event surface before claiming exact node causality.
2. Existing skill rows prove rank and rank deltas, not how professions were learned. Profession training remains outside this lane.
3. Pikli needs a clean repeat with no unexplained negative inventory delta before being credited as a passing gatherer.
4. Corpse detection/recovery needs an exact-gatherer proof path; the current gather proof has no recovery stage.
5. If “no random” means no random-roam strategy rather than “no random teleport/cheat,” the documented `move random` worker behavior must be replaced or explicitly accepted by the core/orchestrator owner.
6. GUID 17’s worker metadata must be resolved or explicitly excluded by the roster owner; this audit only reports it and does not mutate it.
