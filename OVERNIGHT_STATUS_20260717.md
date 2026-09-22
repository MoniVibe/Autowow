# AutoWoW overnight status — 2026-07-17

## Runtime

- Worldserver PID: `72651` (WSL `Ubuntu-24.04`)
- Loaded worldserver SHA-256: `dd472b70e5b98cae37a87b31a1e3e1f7fd01a13e35e9a4aadfcef15bc9518b03`
- World log: `/root/p1runtime/worldserver-overnight-dd47.log`
- Game/control listeners: `127.0.0.1:8085` and `127.0.0.1:18787`
- Persistent campaign children after maintenance reconciliation:
  - quest director PID `11420`
  - guardian PID `42264`
  - progression observer PID `11124`
  - sanity observer PID `6896`
- Rollback source archive: `/root/p1core/phase1-integration-backup-20260717-093700.tar.gz`

## Build proof

- `unit_tests` and `worldserver` built successfully with `-j1`.
- Focused combined policy run: 57/57 passed (critical-heal recovery, dungeon combat-follow, delayed route revalidation).
- Full suite: 11,888 tests from 167 suites; 6,402 active tests passed; zero failures; remaining cases skipped by design; two disabled tests.
- Full test log: `/root/p1core/build/unit_tests-overnight-final.log`

## Persistent questing

- Northstar completed q2519 through normal accept, travel, objective/turn-in handling.
- Northstar completed q2518 through real item objective progress (`0/1` to `1/1`), walked to the finisher, turned it in, levelled, and accepted q2520.
- q2520 is a talk objective and was explicitly blocked/backed off rather than faked.
- The next selected acquisition (q2541) reports an active travel target but has zero measured movement, reproducing Ember's acquisition defect generically.
- Ember has no active quest; q827 acquisition made zero movement through one retry and entered bounded backoff.
- Independent pre-deployment proof: `logs/autowow-lab/quest-final-bb01-proof-20260717/QUEST_FINAL_BB01_PROOF_REPORT.md`.
- Live director receipt: `leagues/results/persistent-campaign-quest-director-20260716-131112.jsonl`.

## Persistent gathering

- Exact node interactions, loot-open/consume events, real item gains, and profession gains have been proven previously.
- Current workers continue to seek and interact with herb/ore nodes.
- Guardian performs one bounded ordinary `find corpse` recovery per stable death generation/cooldown.
- Emealar has recovered normally after death more than once; recurring world deaths remain a diagnosis target.
- Guardian receipt: `logs/overnight/guardian-20260716-132255.jsonl`.

## Fresh dungeon/raid probes on `dd472b...`

### Nexus — PASS for this bounded run

- Exact five admitted, stayed alive, moved as a convoy, and all participated in combat.
- Grand Magus Telestra was reduced to 5.82% in the last visible sample, then the server logged `convoy_post_combat_route_reset completed_encounter=0` and advanced to encounter 1, confirming the kill.
- Monitor completed PASS and the party continued toward the next encounter.
- Receipt: `logs/autowow-lab/overnight-nexus-party-20260717-140756.jsonl`.
- Summary: `logs/autowow-lab/overnight-nexus-party-20260717-140756-summary.md`.

### Drak'Tharon Keep — PARTIAL / route failure

- Exact five admitted, stayed alive, fought together, and killed Trollgore.
- Server logged `convoy_post_combat_route_reset completed_encounter=0` and selected Novos.
- The group later produced bounded `no_progress` while routing toward Novos.
- Receipt: `logs/autowow-lab/overnight-dtk-party-20260717-140756.jsonl`.
- Summary: `logs/autowow-lab/overnight-dtk-party-20260717-140756-summary.md`.

### Onyxia — FAIL / meaningful combat proof

- Exact ten admitted and traversed the instance as a convoy.
- All ten engaged Onyxia and reduced her to 63.296%.
- Generic add focus, tank pickup, interrupt assignment, and Deep Breath safe movement all fired.
- The raid wiped during sustained whelp/add pressure; eight members released to the graveyard in the first post-wipe sample.
- Receipt: `logs/autowow-lab/overnight-onyxia-raid-20260717-140756.jsonl`.

## Open gates

1. Repair quest acquisition without treating an incomplete direct path as successful travel. Read-only probes found:
   - q827/Margoz: a 20.026-yard partial navmesh segment ending 386.181 yards short was reported `safe=true`.
   - q2541 giver: `safe=false`, `path_not_normal`, ending 1004.006 yards short.
   The first patch is diagnostics-only; later movement must use ordinary intermediate travel routing rather than fake direct-path completion.
2. Explain the DTK route stall between Trollgore and Novos without adding encounter-specific teleport logic.
3. Separate Onyxia's first causal wipe driver (add throughput, threat ownership, healing/mana, or recovery) before changing combat code.
4. Diagnose why Emealar repeatedly dies while gathering, preserving ordinary travel and honest resource credit.

Two read-only Terra evidence lanes were launched for gates 1–3. No further source mutation is authorized until their evidence is reconciled by the Sol orchestrator; only Luna-max workers may implement subsequent code changes.

## Second overnight candidate

- Deployed worldserver PID: `87792`
- Loaded SHA-256: `ca444119543d5ce07916bec54be7a475edd4a5da12b26fee5fd2468c678caf6a`
- Build/test gates:
  - 96/96 focused tests passed.
  - Full suite: 11,902 tests from 168 suites; 6,416 active passes; zero failures.
  - Full log: `/root/p1core/build/unit_tests-overnight-candidate2.log`.
- Generic changes in this candidate:
  - quest-acquisition diagnostics now expose action gates, path type, endpoint distance, and endpoint completeness without changing movement behavior;
  - dungeon shared-regroup retries widen deterministically from 5 to 16 to the existing 128-point cap, with one terminal per unchanged route/member context;
  - raid add waves now hold Burn behind deterministic per-tank capacity claims while preserving boss ownership, healer reservations, and CC claims.
- Persistent campaign children after the second restart:
  - quest director PID `38412`
  - guardian PID `34644`
  - progression observer PID `40128`
  - sanity observer PID `40968`
- Fresh post-fix probes admitted and left bounded:
  - DTK PID `14568`: `logs/autowow-lab/ca44-dtk-party-20260717-150321.jsonl`
  - Onyxia PID `40768`: `logs/autowow-lab/ca44-onyxia-raid-20260717-150321.jsonl`
  - Nexus PID `27008`: `logs/autowow-lab/ca44-nexus-party-20260717-150504.jsonl`

All three exact rosters were alive, admitted to their expected instance maps, and producing monitor samples when handed off. The monitors are time-bounded and will write matching `-summary.md`/`-summary.json` artifacts on completion.
