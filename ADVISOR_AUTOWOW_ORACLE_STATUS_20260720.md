# Advisor packet: AutoWoW Oracle / persistent Playerbots

Date: 2026-07-20
Audience: Advisor planning lane
Owner: Sol / AutoWoW orchestrator

## Planning request

Design the next implementation sequence that turns the current AzerothCore Playerbots fork into a believable, persistent player simulation:

- characters quest and level through the world using native quest state;
- characters travel, gather, craft, train, spend talents, maintain gear, and recover from failure;
- characters operate independently but cooperate when a party or raid is useful;
- dungeons, raids, and occasional PvP become measurable progression activities;
- the same generic behavior contracts work across many quests, sources, encounters, and instances rather than one-off scripts;
- disposable micro-scenarios can prove a narrow capability quickly without waiting for a level-1-to-80 campaign;
- every claim is backed by native server state, durable inventory/quest state, combat/loot receipts, or an explicit blocked/inconclusive result.

The immediate planning problem is not “add more intelligence” in the abstract. It is to decide which shared execution and recovery surfaces should be hardened first so that the existing bots stop spending long periods in route/turn-in limbo and begin making durable progress.

## Situation in one paragraph

AutoWoW is a local Windows AzerothCore WotLK 3.3.5a Playerbots server with a custom Playerbot fork and an AutoWow bridge. The project has two overlapping orchestration layers: a server-side Oracle runtime intended to own bounded quest/gather decisions, and an external Autopilot/director intended to plan, lease, observe, recover, and report. The current live server has 10 bots online, all alive, mostly level 3–7. The Oracle runtime config is enabled for 12 GUIDs at a 1-second cadence, but the external director is not running and the published capability manifest is still an interim fail-closed probe that says Oracle operations are undeployed. Live `questobjective` reads show most bots traveling to completed-quest finishers or active objectives; one bot is explicitly blocked by `movement_stuck_no_teleport`. All live gather routes are inactive. The main failure is therefore orchestration and shared route/objective execution, not lack of a larger language model.

## Topology and source truth

### Windows project

- Project root: `D:\Games\wowstuff\AutoWoW`
- Deployment core: `azerothcore-wotlk`
  - branch: `Playerbot`
  - HEAD: `52f58186a53399e603c46c24977fe60fcaad7f9d`
  - dirty count: 0
- Deployment module: `azerothcore-wotlk\modules\mod-playerbots`
  - branch: `master`
  - HEAD: `93aaea3de19243c09ce9ecb25627dc9671715eed`
  - dirty count: 24; preserve this work and do not reset or silently clean it
- Active integration source lane: `work\oracle-integration-r13-r6-r5`
  - detached worktree at module HEAD `93aaea3de19243c09ce9ecb25627dc9671715eed`
  - dirty count: 313; it contains the accumulated Oracle, quest, gathering, dungeon, combat, telemetry, and fixture work
- Prior isolated WSL topology:
  - core: `/root/autowow-quest-giver-core`
  - module mirror: `/root/autowow-oracle-r14-r6-r5-module`
  - test build: `/root/autowow-quest-giver-core/build-tests`
- The module is linked into the isolated core through the existing junction/worktree topology. Advisor must treat the active lane and deployment checkout as separate working states and must not recommend a blind merge/reset.

### Running services

- Bridge: loopback `127.0.0.1:18787`
- Worldserver: WSL process using `/root/p1runtime/worldserver.conf`
- Authserver and MySQL: running locally
- The worldserver process was not restarted during the latest fixture build. Its on-disk build path was rebuilt, so the next deliberate restart would load the new binary; runtime enrollment still fails closed because the micro-scenario allow-list is empty.
- No external `oracle-race-campaign`, `league-simulation-director`, Autopilot, or gathering-orchestrator process is currently running.

### Runtime configuration observed

```text
AutoWow.OracleRuntime.Enabled = 1
AutoWow.OracleRuntime.BotGuids = "7,10,101,112,121,123,139,144,154,166,236,244"
AutoWow.OracleRuntime.CadenceMs = 1000
AutoWow.OracleRuntime.MaxBots = 12
AutoWow.OracleRuntime.LeaseTtlTicks = 3
AiPlayerbot.AutoDoQuests = 1
AiPlayerbot.EnableNewRpgStrategy = 1
AiPlayerbot.RandomBotGroupNearby = 0
AiPlayerbot.RandomBotJoinBG = 0
AiPlayerbot.RandomBotAutoJoinBG = 0
```

The fast fixture control is not enrolled: `AutoWow.MicroScenarioGuids` is absent/empty in the live runtime, which is the intended default-closed state.

## Intent and success definition

The long-term simulation is two persistent guilds or teams progressing from level 1 toward 80. Their members should behave like imperfect players rather than scripted mannequins:

- choose and complete quests using the actual quest log and objective counters;
- walk or use valid native travel transitions to reach objectives and finishers;
- kill valid objective targets, loot them, and preserve durable item/objective evidence;
- gather resources through exact source selection and native loot/skill credit;
- craft from real profession/reagent state, then equip or bank outputs;
- learn spells, spend talents, and maintain role-appropriate gear;
- form parties when a task benefits from them without requiring every character to have a permanent leader;
- enter dungeons and raids, stage pulls, respect roles and hazards, loot, equip, recover, and continue;
- eventually issue/accept PvP challenges and measure outcomes.

The Oracle is an omniscient planning and test authority, not necessarily an LLM. It may know exact quest targets, source spawns, encounter facts, and safe routes for controlled experiments. That knowledge must be explicit, scoped, leased, and observable. A test kill or seeded item can accelerate setup, but it cannot be reported as proof of normal combat, quest credit, gathering, or crafting.

The campaign gate is durable progression, not movement or XP:

```text
accepted quest -> live objective counter increases -> quest completes -> reward is confirmed
source selected -> native gather/loot/skill event -> durable material delta
recipe selected -> native craft completion -> durable output delta and reagent accounting
encounter engaged -> role/combat/loot receipts -> boss/instance progression and persistence
```

## Current implementation shape

### Native Playerbots layer

The fork contains two different quest engines:

1. Legacy/master-driven actions under `src/Ai/Base/Actions` and classic travel. These often short-circuit without a master.
2. New RPG autonomy under `src/Ai/World/Rpg/Action/NewRpg*.cpp`, `NewRpgInfo.*`, and `NewRpgStrategy.cpp`. This is the real solo loop: select a quest, travel to a quest POI, delegate kill/loot to existing strategies, and turn in or recover.

The New RPG engine is useful but incomplete. It historically selected/randomized targets and delegated too broadly to grind. That allowed XP and nearby kills without objective progress; the engine then marked quests abandoned. Strict objective context, target whitelist, loot binding, finisher binding, and Oracle ownership have been added around this boundary, but the live system still needs end-to-end proof across multiple objective types.

### Oracle runtime

Key surfaces:

- `src/AutoWow/AutoWowOracleRuntime.{h,cpp}`
- `src/AutoWow/AutoWowOracleContract.h`
- `src/AutoWow/OracleQuestExecutor.*`
- `src/AutoWow/OracleGatherExecutor.*`
- `src/AutoWow/OracleGatherRuntimePolicy.h`
- `src/AutoWow/AutoWowOracleOwnershipGate.*`
- `src/AutoWow/QuestAcquisitionPolicy.h`
- `src/AutoWow/QuestFinisherTransitionPolicy.h`
- `src/Ai/World/Rpg/QuestObjectiveContext.h`
- `src/Ai/World/Rpg/Action/NewRpgAction.*`

The runtime is called from `src/Script/Playerbots.cpp::OnUpdate`, runs on the world thread, loads an explicit GUID allow-list, plans one bounded operation per bot, acquires/renews an ownership lease, mirrors authority into the native quest state, dispatches a tagged native action, and records an in-memory receipt ring. It can drive quest objective/finisher work and an exact-source gather executor when a worker/source is explicitly published.

It does not currently provide a complete universal planner for every quest type, every transition, crafting, or all raid mechanics. Its safe behavior is often to block or release rather than guess.

### External planning/control layer

Existing scripts and contracts include:

- `scripts\league-simulation-director.ps1` and `scripts\QuestDirectorLib.ps1`: deterministic quest selection, objective-centric stall detection, engine-abandonment detection, bounded recovery, external backoff.
- `autopilot\autowow-autopilot.ps1` and `autopilot\AutopilotLib.ps1`: strategic jobs, ownership leases, campaign planning, lifecycle successors, capability gating, and fail-closed live execution.
- `AUTOWOW_ORACLE_INTEGRATION_CONTRACTS_V1.md`: the server/Autopilot capability, enrollment, roster, and receipt contracts.
- `work\autopilot\capabilities.json`: current interim probe. It deliberately says `runtime_enabled=false`, `sql_read_deployed=false`, and `quest_objective` undeployed, with `source=interim-probe`. It cannot authorize live sends even though the deployed config contains Oracle runtime settings.

This split is intentional for safety but currently creates a practical gap: the server is configured to run Oracle, while the external controller has no authoritative deployed-runtime manifest and therefore refuses live control.

### Bridge and telemetry

The loopback bridge exposes control and read surfaces including `list`, `snapshot`, `questlog`, `questobjective`, `acceptance`, `destinations`, `professioneconomy`, `gather_route`, `quest`, `recover`, `engage`, `boss`, `combatlog`, `encounterlog`, and fixture controls. The bridge is server-authoritative and world-thread dispatched.

Important observability distinction:

- `list` currently reports the older action/travel summary and can show `action=""` plus `travel=expired` while the Oracle quest state is actively in `travel_to_finisher`, `travel_to_source`, or `acquire_target`.
- `questobjective` exposes the useful live Oracle phase, objective, finisher, movement, route, target, loot, and failure state.
- Oracle receipts currently remain in an in-memory ring of 64 and are not exported as a durable JSONL server source.

### Gathering and crafting

Gathering has a typed exact-source policy and native loot/skill evidence path under `src/Ai/World/Gathering` and `src/AutoWow/OracleGatherExecutor.*`. It requires an explicit worker, exact source identity, source reservation, and native durable credit. No live worker is currently published, so all current `gather_route` states are inactive.

Crafting has catalog/telemetry and a bounded `craft` bridge path in `src/AutoWow/AutoWowCraftControl.*`, but arbitrary profession/reagent/material setup and durable per-bot craft receipts remain incomplete. The campaign records `craft:evidence_unavailable` rather than claiming success.

### Dungeons, raids, and combat

There are generic and encounter-specific policies for combat movement, AOE avoidance, target claims, role triggers, death/corpse recovery, dungeon transitions, loot/equip, and encounter telemetry. The dungeon navigator V0 is intentionally conservative:

- party leader only;
- normal dungeon transitions through real area triggers;
- grounded, safe, bounded walk paths;
- no fabricated cross-map movement;
- no generic support yet for elevators, moving platforms, scripted doors, transports, or raid admission.

Live portability evidence exists for The Nexus and Drak'Tharon Keep. A five-bot Drak'Tharon party crossed the route seam, killed Trollgore, selected Novos, and generated five greed votes for item 35618. This is useful evidence, not proof that all dungeons/raids or all role mechanics are solved. Generic raid behavior, healer/tank priority, interrupt/avoidance attribution, released-corpse recovery, and broad boss coverage still need systematic proof.

### Disposable micro-scenarios

The catalog and runner are under:

- `scripts\micro-scenarios\catalog.json`
- `scripts\micro-scenarios\*.json`
- `scripts\micro-scenario.ps1`
- `scripts\micro-scenario-lib.ps1`
- `scripts\tests\micro-scenario*.tests.ps1`
- `MICRO_SCENARIO_PLAYBOOK.md`

The runner is dry-run by default, hash-pins manifests, requires `-Apply -Execute` for live mutation, records JSONL receipts, and supports idempotent resume. Catalog examples cover exact gathering, one craft, quest-giver acquisition, role readiness, accelerated quest progression, and accelerated harvesting.

The isolated fixture acceleration adapter now supports:

```text
fixture accelerate <guid> 1000
fixture accelerate-off <guid>
fixture kill <guid> entry <entry> spawn <stable-spawn-id>
```

`1000` means exactly 10× the saved fixture movement-rate baseline. Exact kill requires map/instance/entry/stable-spawn identity and invokes the native server kill path; it does not inject quest, loot, inventory, or material credit. The adapter is compiled and tested but not deployed/enrolled. The remaining fixture gap is native setup seeding for disposable characters, professions, talents, gear, tools, bags, reagents, quest/giver/source state, and cleanup.

## Current live evidence

Read-only bridge checks on 2026-07-20 found:

- 10 bots online; all alive and out of combat;
- levels 3–7;
- Oracle allow-list contains 12 GUIDs, but only 10 are online;
- no live external director/controller process;
- all `gather_route` states inactive;
- generic `list` shows empty actions and expired/absent legacy travel state;
- detailed `questobjective` shows 7 bots in active `travel_to_finisher`, 2 in active objective work, and 1 blocked.

Representative detailed states:

- Nolliano / GUID 236: directive quest 383, a completed turn-in. Finisher phase is `blocked`, failure is `movement_stuck_no_teleport`; the exact target is known from the database but not live-loaded, and the route executor stopped at the last safe state rather than teleporting.
- Aenstus / GUID 123: quest 8346, creature objective entry 15468, phase `travel_to_source`; exact spawn is known but unloaded, and the safe-incomplete route policy is still progressing toward the valid endpoint.
- Tzigdan / GUID 144: quest 4402, item objective 11583 at 5/6, exact GameObject source live and selected, phase `acquire_target`; this is a useful near-complete objective probe but has a long movement-stall age and needs a decisive recovery/route verdict.

The latest campaign telemetry file, `logs\oracle-race-campaign\run-race-20260720-012412.jsonl`, contains five ticks from approximately 01:24–01:26 UTC. It records 10/10 online, levels unchanged during the run, quest counts from 1–6, and no durable gathering operation. Earlier campaign evidence recorded progression/no-progress and craft-evidence gaps; the telemetry run itself was not a persistent controller.

## What is already strong

| Area | Current evidence | Confidence |
|---|---|---|
| Server/bootstrap | Local WotLK client, AzerothCore Playerbot fork, MySQL/auth/worldserver, loopback bridge | High |
| Native quest talk/turn-in | New RPG and questlog/acceptance surfaces; deterministic reward selection | Medium; live end-to-end reward proof still needs a clean walk |
| Ordinary kill/loot | Entry/count gating, native loot path, objective counters | Medium; historical engine-abandonment signature remains important |
| Objective ownership | Oracle leases, tagged dispatch, strict objective/finisher identity | High in focused tests; live breadth limited |
| Route safety | Grounded path selection, bounded incomplete progress, no-teleport policy, dungeon path safety | High for tested surfaces |
| Dungeon transitions | Nexus and Drak'Tharon V0 evidence; Trollgore kill and loot | Medium; not raid/general-instance complete |
| Gathering policy | Exact-source identity, native loot/skill/evidence policy | High as policy; low live coverage because no worker rotation is active |
| Recovery | Explicit blocked, retry, release, backoff, and no-progress states | High as contracts; live route blockers remain |
| Fixture testing | Catalog, dry-run runner, acceleration adapter, focused build/tests | High for tooling; setup seed adapter remains |
| Autopilot safety | Job lifecycle, ownership, manifest freshness/binary/session binding, fail-closed live gate | High for controller safety; official deployed manifest is not published |

## Weaker points and why they exist

| Weak point | Observable symptom | Root cause | Needed class of fix |
|---|---|---|---|
| Finisher/source routing | Bots wait, show expired travel, or become blocked | Target exists in DB but is unloaded; route/ground/path handoff cannot safely complete the last segment; no-teleport policy correctly refuses a shortcut | Generic target loading/route re-probe and finisher/source state machine; preserve safe no-teleport boundary |
| Quest completion priority | Many bots spend time on completed quests while unfinished work waits | Oracle intentionally prioritizes native reward confirmation, but blocked finishers do not yield quickly enough | Bounded finisher budget, explicit yield-to-objective policy, and shared recovery/backoff ledger |
| Apparent idle | `list` says no action while `questobjective` says travel/objective active | Two telemetry surfaces expose different state machines | Make `list` include Oracle phase, lease, objective, route, failure, and last-progress age |
| No persistent progress loop | Levels and objectives remain unchanged after short campaign run | External director is stopped; server runtime only drives configured/eligible native work and cannot replace strategic job ownership | Decide one authoritative scheduler and run it with durable leases, receipts, and restart-safe lifecycle |
| Old grind fallback | XP rises without objective counters; quests are abandoned | New RPG delegates objective execution to generic grind/loot; nearby non-quest targets can win | Strict objective lock must remain authoritative through target selection, combat, loot, and objective counter verification |
| Quest-item use | Bots route but cannot use a quest item on a target | No autonomous binding of active objective → quest item → valid target/area | Typed quest-item executor and native packet/effect/counter receipt |
| GameObject objective use | Bots can talk to or loot some GOs but do not click/use objective GOs | No general objective GameObject `Use()` driver in the New RPG loop | Typed GO interaction executor with stable identity and credit postcondition |
| Escorts/scripted events | Escort/event quests are dropped or excluded | No escort follower, timed event, gossip/script state machine, or playercount driver | General event/escort state machine, only after native event receipts are exposed |
| Cross-map/transitions | Same-map routes work; elevators, doors, transports, and raid admission do not | V0 deliberately excludes unsafe teleport/transport guesses; DB does not encode enough transition semantics | Typed transition catalog and adapters; do not add map-specific waypoint hacks as the first solution |
| Gathering | All live gather routes inactive | Exact worker/source publication and setup are not active; normal bot gathering is opportunistic | Worker scheduler plus fixture/source seed and durable inventory/skill evidence |
| Crafting | `craft:evidence_unavailable` | No complete deployed per-bot craft receipt plus arbitrary reagent/profession seed | Native craft executor, receipt export, inventory delta reader, fixture seed |
| Role combat proof | Bots may survive or kill, but tank/heal/avoid/interrupt attribution is incomplete | Existing class AI and encounter policies are not unified by a generic role/encounter observer | Shared encounter telemetry and role claims before tuning individual bosses |
| Receipt export | Oracle evidence is only an in-memory ring of 64 | Runtime contract was built before durable export surface | Read-only `oraclelog` or JSONL export with bounded retention and session binding |
| Deployment truth | Runtime config says enabled while interim manifest says undeployed | Safety gate intentionally separates compiled/configured from published/deployed/session-bound truth | Generate official manifest after intentional restart; never bypass the gate |
| Dirty topology | Active module lane has hundreds of changes and deployment module has existing dirt | Many lanes accumulated work before a single integration checkpoint | Advisor must prescribe lane extraction, base SHAs, allowed paths, proof, and merge order; no blind checkout/reset |

## Non-negotiable law boundaries

AutoWoW should use the following equivalent causal pipeline for every meaningful operation:

```text
read-only observation
  -> deterministic candidate/objective selection
  -> per-bot ownership lease and preconditions
  -> native Playerbot/world action
  -> authoritative quest/combat/loot/inventory state
  -> durable receipt/telemetry and bounded recovery
```

Rules:

- Never write quest status, objective counters, reward rows, inventory output, or material output directly to prove behavior.
- Fixture setup may seed character configuration, tools, reagents, position, source identity, and clean preconditions, but seeded state is never operation evidence.
- Exact-target controls must bind map, instance, entry, stable spawn, fixture lease, and actor.
- No teleport is valid movement evidence. Setup positioning is not travel evidence.
- XP, movement, `ok:true`, process survival, quest disappearance, or an item already present in a seeded bag is not success by itself.
- Unknown facts stay unknown. A blocked or inconclusive run must remain visible.
- One gameplay mutator owns a bot at a time. External Autopilot, Oracle runtime, quest director, gatherer, and observer must use disjoint leases or explicit handoff.
- Production/campaign behavior must not inherit fixture-only 10× pacing or instant-kill controls.
- Prefer generic policies/catalogs/adapters over per-quest or per-boss scripts. A special case requires evidence that the underlying typed capability cannot represent it.

## Ordered priorities for Advisor to evaluate

1. **Unblock one ordinary quest end-to-end.** Fix the shared finisher/source route state machine and prove accepted → objective progress → complete → reward, with no teleport or synthetic state.
2. **Unify scheduler authority.** Decide whether Oracle runtime is the tactical owner and Autopilot is strategic, or whether the older Quest Director remains in the loop. Define lease ownership, handoff, recovery, and restart behavior.
3. **Make telemetry truthful.** Add Oracle phase/lease/failure/progress fields to `list`, and export bounded Oracle receipts so the operator can see why a bot is waiting without querying several endpoints manually.
4. **Build the disposable fixture-seed adapter.** Create native setup-only state for one quest and one gather/craft scenario, with exact cleanup and durable baseline comparison.
5. **Finish shared objective executors.** Implement GameObject-use and quest-item-use before escort/event work; then add an escort/event contract based on native event receipts.
6. **Run a bounded persistent progression trial.** Start a small number of independent questers and gatherers, with objective-centric telemetry, no grind fallback, recovery budgets, and an automatic stop on repeated blockers.
7. **Only then expand dungeon/raid/PvP breadth.** Reuse generic route, role, encounter, loot, and transition policies; use fixtures to probe new mechanics quickly.

## Non-goals for this planning cycle

- Do not add an LLM as the runtime combat/quest brain.
- Do not hand-author every quest, dungeon, or boss unless Advisor proves a missing typed capability.
- Do not make persistent campaign bots 10× fast or give them instant kills.
- Do not claim “1–80 progression” from level changes or XP alone.
- Do not enable the micro-scenario allow-list on campaign GUIDs.
- Do not bypass the deployed-runtime manifest, session binding, ownership leases, or live confirmation gates.
- Do not merge/reset the dirty worktrees as a cleanup convenience.
- Do not prioritize a dashboard, screenshots, or scoreboards ahead of truthful server state and route/objective completion.

## Questions Advisor must answer

1. What is the correct scheduler ownership model: server Oracle runtime for tactical execution plus Autopilot for strategic jobs, or a different split? Give the exact lease/hand-off state machine and identify which existing controller must be disabled.
2. Should the first implementation lane fix finisher/source routing, improve objective execution, or add telemetry/export? Choose one order based on the live evidence, not architectural preference.
3. What generic route abstraction is sufficient for unloaded DB spawns, grid loading, safe incomplete paths, cross-map flight/portal transitions, elevators, doors, and raid admission? Define which transition types belong in the common catalog and which remain explicit adapters.
4. How should a blocked completed quest yield to an unfinished objective without reintroducing quest thrashing or the old random-grind fallback? Specify budgets, backoff keys, and re-entry conditions.
5. What is the smallest native fixture-seed API that can create a disposable character/profession/tool/bag/quest/source state without direct proof writes? Include cleanup and persistence checks.
6. Which evidence must be exported from the server for an external controller to prove a quest, gather, craft, or encounter? Specify a bounded JSONL schema and retention/session rules.
7. What is the minimum generic combat/role observer needed before testing more bosses: threat ownership, healing attribution, interrupts, avoidance, movement safety, deaths/release/resurrection, loot, or all of them? Prioritize.
8. What live performance budget should govern `CadenceMs=1000` for 10–12 bots, then 40–80 bots? Require measured timing and memory evidence, not only unit tests.
9. How should the 313-change active lane be split into mergeable branches without losing the user’s accumulated work? Give base SHA, allowed paths, extraction order, and rollback for each lane.
10. What is the exact acceptance matrix for quest types, gathering, crafting, dungeon transitions, raids, PvP, and persistence, and which categories should remain explicitly blocked until their native evidence exists?

## Required Advisor output

Return a concrete implementation plan, not general recommendations.

### A. Lane table

For every proposed lane, provide:

| Field | Required content |
|---|---|
| Lane ID/name | Short stable identifier |
| Objective | One bounded outcome |
| Base | Exact branch/worktree and base SHA |
| Allowed paths | Exact files/directories that may change |
| Dependencies | Earlier lanes and runtime prerequisites |
| Implementation shape | Types, state machine, adapter, or telemetry contract to add |
| Proof | Exact PowerShell/C++/live commands and expected pass evidence |
| Performance | Timing/memory/scale measurement or explicit reason it is not yet measurable |
| Halt conditions | Compiler error, native evidence absence, route safety failure, lease conflict, dirty-topology conflict, or budget breach |
| Rollback | How to disable/revert without resetting unrelated user work |
| Handoff | Report path, commit/head, hashes, logs, remaining manual step |
| Merge order | Position in the recommended train |

### B. Recommended first train

Choose a small first train, preferably:

1. telemetry truth/Oracle receipt export;
2. generic finisher/source route recovery;
3. one native fixture-seed vertical slice;
4. supervised quest proof;
5. only then persistent quest/gather loop.

Advisor may reorder this, but must explain the evidence-based reason and preserve the gates.

### C. Acceptance gates

Define exact pass/fail criteria for:

- one quest accepted, progressed, completed, and rewarded;
- one exact gather source yielding a durable material;
- one craft with durable output and reagent accounting;
- one dungeon route transition and one boss with loot/equip evidence;
- one failure/recovery cycle that does not teleport or silently reset state;
- a restart with lease cleanup and persistence;
- 10-bot and later 40–80-bot performance.

### D. Explicit uncertainty register

Separate facts verified from source/config, facts verified live, compiled-only claims, and unproven claims. Do not promote any capability because a wire verb or test exists.

## Evidence index

- [Oracle integration contracts](D:/Games/wowstuff/AutoWoW/AUTOWOW_ORACLE_INTEGRATION_CONTRACTS_V1.md)
- [Quest capability matrix](D:/Games/wowstuff/AutoWoW/QUEST_CAPABILITY_MATRIX.md)
- [Quest Director V1 report](D:/Games/wowstuff/AutoWoW/QUEST_DIRECTOR_V1_REPORT.md)
- [Quest type coverage](D:/Games/wowstuff/AutoWoW/QUEST_TYPE_COVERAGE_REPORT.md)
- [Dungeon navigator V0 report](D:/Games/wowstuff/AutoWoW/DUNGEON_NAVIGATOR_V0_REPORT.md)
- [Oracle race campaign report](D:/Games/wowstuff/AutoWoW/ORACLE_RACE_CAMPAIGN_REPORT_20260719.md)
- [Micro-scenario playbook](D:/Games/wowstuff/AutoWoW/MICRO_SCENARIO_PLAYBOOK.md)
- [Micro-scenario acceleration report](D:/Games/wowstuff/AutoWoW/MICRO_SCENARIO_ACCELERATION_V1.md)
- [Autopilot README](D:/Games/wowstuff/AutoWoW/autopilot/README.md)
- [Current interim capability manifest](D:/Games/wowstuff/AutoWoW/work/autopilot/capabilities.json)
- [Latest race telemetry](D:/Games/wowstuff/AutoWoW/logs/oracle-race-campaign/run-race-20260720-012412.jsonl)

## Sol’s instruction to Advisor

Assume the user wants progress that looks like real players, but prefers an honest blocked result to a fake success. Preserve all dirty work. Recommend the smallest reusable change that improves many quests or tasks, and require a live or durable proof for every claim. Keep fixture acceleration and Oracle omniscience available for testing, but never let either one masquerade as normal bot ability.
