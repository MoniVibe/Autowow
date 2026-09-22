# Quest Director V1 maintenance proof

Generated: 2026-07-13 14:21 +03:00

This is the post-build/live follow-up to `QUEST_DIRECTOR_V1_REPORT.md`. It records the
maintenance window in which the authored C++ quest-log endpoint and Quest Director V1
were installed and exercised against the running two-team simulation.

## Build and restart

- Core SHA: `52f58186a53399e603c46c24977fe60fcaad7f9d`
- Playerbots SHA: `93aaea3de19243c09ce9ecb25627dc9671715eed`
- Configuration: Visual Studio 17 2022, x64, RelWithDebInfo
- Build parallelism: 2
- Build/install: PASS; `QuestLogView.cpp` and `AutoWowBridge.cpp` compiled
- Smoke test: PASS; auth, world, bridge, four schemas, Playerbots tables, and log scan
- Worldserver startup: PASS; `AutoWow bridge listening on 127.0.0.1:18787`

Evidence: `BUILD_REPORT.md`, `SMOKE_TEST_REPORT.md`, and
`logs/cmake-build-20260713-135613.log`.

## Live endpoint proof

The rebuilt bridge returned `ok:true` for both leaders:

- Ember leader 7, Saewash: quest 792 `Vile Familiars`, NPC objective `3/8` initially.
- Northstar leader 10, Brandreas: quest 459 `The Woodland Protector`, item objective `0/8`.

At the end of the run, the live endpoint reported quest 792 as `status:1`,
`is_complete:true`, `objectives_all_done:true`, and `8/8`. This is the first live
objective-completion proof; reward/turn-in completion remains a separate engine gap.

## Corrected issuing proof

Receipt: `logs/quest-director-v1-live-20260713-141031.jsonl`

The six-minute issuing run used one director, roster-discovered leaders, a 60-second
objective/turn-in stall threshold, two recoveries per quest, and a fresh backoff ledger.
It completed with zero `director_error` events:

| Leader | Initial selection | Result | Backoff/switch |
|---|---|---|---|
| 7 Ember | 792, complete at 8/8 | 2 recoveries, no turn-in progress | backed off 792, switched to 789 |
| 10 Northstar | 459, 0/8 | 2 recoveries, XP/movement but no objective progress | backed off 459, switched to 916 |

Aggregate receipt counts:

- 30 quest evaluations
- 4 successful quest commands: `792, 789, 459, 916`
- 7 recoveries
- 2 backoffs
- 17 holds
- 6 combat deferrals
- 0 director errors

The director issued only explicit quest/recover orders. It did not issue generic grind,
random movement, rally, route, or teleport commands. The backoff ledger is
`leagues/results/quest-director-v1-live-20260713-141031-backoff.json`.

## Read-only telemetry

Receipt: `logs/quest-telemetry-live-20260713-140901.jsonl`

- 300 samples over the ten-minute window
- 95 explicit `no_progress` observations across the roster
- Leader 7: level 6→6, XP 55→143, objective sum 4→9
- Leader 10: level 6→6, XP 874→997, objective sum 0→0
- No database mutation was performed by the collector

The telemetry confirms the original failure mode: XP and activity can increase while a
loot objective remains flat. V1 now detects and bounds that state instead of counting
movement/XP as quest progress.

## Script fixes landed during the window

1. Normalized roster JSON parsing under Windows PowerShell, avoiding the `System.Object[]`
   GUID conversion failure.
2. Made bridge result fields optional so `ok:false`/`phase:block` responses are logged,
   not treated as director crashes.
3. Added backoff for semantically blocked/rejected quest commands.
4. Applied the same no-progress timer to `turnin` phases, preventing completed-but-
   unturnable quests from being held forever.

Final verification: all three Quest Director scripts parse, and Pester is `25/25 PASS`.

## Final machine state

- Authserver and worldserver remain running on localhost.
- Ember: 5/5 enrolled guild bots online.
- Northstar: 5/5 enrolled guild bots online.
- AutoWow bridge: 10 bots online.
- At proof close, no diagnostic process remained running; the corrected overnight
  continuation was then started separately (see below).

## Overnight continuation

Started after the proof completed, with no competing issuer:

- Quest Director V1: PID 25216, 480 minutes, 30-second scouting, 120-second
  objective/turn-in stall threshold, two recoveries, 30-minute backoff.
- Read-only telemetry: PID 19352, 480 minutes, 60-second polling.
- Receipts: `logs/quest-director-v1-overnight-20260713-142218.jsonl` and
  `logs/quest-telemetry-overnight-20260713-142218.jsonl`.
- Backoff ledger: `leagues/results/quest-director-v1-overnight-20260713-142218-backoff.json`.

## Remaining limitation and next implementation target

Quest 792 now reaches a real completed state, but the engine does not reliably discover
the turn-in starter from the party's current location. Loot objectives such as 459/916
also remain a capability gap: the parties can move and gain XP without collecting the
required item. The next code target is objective-target enforcement for loot/kill
engagement and a reliable quest turn-in route, followed by a proof that status changes
from complete to rewarded.
