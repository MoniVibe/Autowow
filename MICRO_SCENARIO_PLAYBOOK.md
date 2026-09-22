# AutoWoW Micro-Scenario Playbook

This playbook defines small, repeatable fixtures for testing playerbot behavior without waiting for a long campaign to reach the right level, zone, profession, quest, or encounter.

The catalog is in [`scripts/micro-scenarios/catalog.json`](scripts/micro-scenarios/catalog.json). Each entry is a standalone manifest. The manifests are plans and contracts, not permission to mutate the live campaign. The default is dry-run.

## Core rule

Every scenario has four separate moments:

1. **Preparation** creates a disposable fixture and captures a baseline.
2. **Operation** issues the smallest native behavior command under a bounded lease.
3. **Observation** collects server-native receipts and durable read-only state.
4. **Verdict and cleanup** classify the result, release the fixture, and prove that unrelated characters were not changed.

Preparation can make a character convenient to test. Preparation is never evidence that the bot travelled, accepted a quest, gathered a node, crafted an item, or performed a combat role.

## Fixture identity and faction semantics

The fixtures use `wayfarer` / `out-of-roster` semantics. They are not described as a third neutral faction. A character is only neutral if the server has a real, supported neutral-faction model and the live probe confirms it.

The default fixture identity contract is:

- one dedicated character or one explicitly listed fixture roster;
- no Northstar, Ember, race-seed, persistent gather-lane, raid, or campaign ownership;
- no unrelated party or raid membership;
- a fixture lease and an owner recorded in the receipt;
- a teardown that deactivates the fixture and checks persistence.

If a fixture cannot satisfy this contract, the correct result is `blocked`, not a guessed faction or an implicit reuse of a campaign character.

## What may be seeded

The fixture adapter may seed setup state when the server does not expose a safe existing control surface. Examples include:

- level, race, class, specialization, talents, and test gear;
- a profession and its skill value;
- exact reagents, tools, bag capacity, and initial inventory;
- a source or encounter identity, including map, instance, entry, and stable spawn;
- a setup position in a zone;
- a quest/giver fixture definition before the quest is accepted.

Every seed must be recorded as `preparation`. A seeded position is not travel evidence. Seeded inventory is not gathering or crafting evidence. Seeded quest rows, reward rows, objective counters, or output items are forbidden in a proof fixture.

The existing `fixture-init` bridge verb can initialize a bounded level/spec/quality shape. It does not, by itself, prove that arbitrary profession, item, talent, spawn, or quest state was materialized. The manifest must name an adapter for those missing inputs.

## Safe execution sequence

### 1. Select and reserve

Load one manifest and allocate only the GUIDs named by its fixture lease. Check the live bridge roster before any mutation. Reject a fixture if a protected or campaign GUID appears in it.

### 2. Prepare

Use only the manifest's preparation allowlist. Capture:

- `list` and `snapshot` for online, alive, map, instance, combat, and group state;
- `professioneconomy` for profession and recipe state;
- `questlog`, `questobjective`, and `acceptance` for quest state;
- `combatlog` and `encounterlog` for combat baseline;
- read-only SQL snapshots for inventory, skills, quest rows, and durable state;
- the adapter receipt containing the exact fixture inputs and source hash.

Do not start the operation until the baseline is internally consistent. A fixture with an unexpected quest row, output item, material, group, or source is `blocked`.

### 3. Operate natively

Use one bounded operation. The bridge command is a directive, not proof. Examples:

- `deploy` starts the existing worker-gather path;
- `quest-acquire` / `quest` requests normal quest acquisition or an explicit in-log quest;
- `craft <guid> <spell>` issues one craft;
- `engage` or `boss` starts a bounded combat probe;
- `route` stages a native route, but does not turn a setup teleport or position seed into travel evidence.

The operation may be given high priority inside its fixture lease. That prevents unrelated work from winning the scheduler race; it does not grant a quest, create objective credit, or establish success.

### 4. Observe and classify

Poll only the manifest's read surfaces. Stop at the first terminal condition:

- `pass`: every mandatory postcondition is backed by native evidence;
- `fail`: the native operation ran and produced a contradictory or negative result;
- `blocked`: a required adapter, capability, fixture, route, or precondition is absent;
- `inconclusive`: the operation ran, but persistence or attribution evidence did not arrive before the bounded window;
- `timeout`: the operation did not reach a terminal state within its declared budget.

`ok:true`, movement, XP, target selection, an item in a seeded bag, a quest disappearing, or a process staying alive is never sufficient by itself.

### 5. Clean up

End the bounded operation, release the lease, deactivate or log out the fixture, wait for stable persistence where required, and re-read the protected roster. Keep the receipt even for a blocked run. A reset must not silently repair a failed proof by inserting the desired result.

## Accelerated fixture mode

The two accelerated manifests are explicitly fixture-only:

- `quest-progression-accelerated-fixture.json`;
- `harvest-accelerated-fixture.json`.

Their pacing value means **1000% of baseline, exactly 10x**. It is a fixture lease, not a server-wide XP, loot, gathering, or campaign-rate change. It must be visible in a capability receipt, scoped to the fixture GUID, and absent from the deployed campaign configuration after cleanup.

The catalog also carries an optional `exact_target_instant_kill` request. It is legal only when all of the following are true:

1. the server reports a named fixture capability for exact-target kill;
2. the adapter binds map, instance, entry, stable spawn, and fixture lease;
3. the operation targets that exact binding rather than a nearest creature;
4. native combat, loot, and quest/gather credit still occur afterward.

The isolated Phase 1 adapter now exposes the bounded fixture-only wires `fixture accelerate <guid> 1000`, `fixture accelerate-off <guid>`, and `fixture kill <guid> entry <entry> spawn <stable-spawn-id>`. It saves and restores the fixture's movement rates, rejects non-allowlisted or real-player GUIDs, and requires the exact map/instance/entry/stable-spawn binding. The adapter is not deployed or enabled by this change: `AutoWow.MicroScenarioGuids` remains empty by default. If the capability or adapter is absent, the accelerated scenario is `blocked`. It must not fall back to arbitrary instant kills or synthetic objective/material credit.

## Scenario procedures

### Gather exact node

Use `gather-exact-node.json` for one named herb or ore source.

Preparation seeds the worker profession, tool, bags, map, and exact source identity. The operation runs the native worker path with `deploy` and observes `gather_route`. A pass requires all of:

- exact entry and stable spawn attribution;
- native gather/loot/skill credit;
- a positive requested-material delta in a stable read-only inventory snapshot;
- coherent profession and skill state.

An inventory increase without an exact source receipt is `inconclusive`, not a gather pass. A material inserted by a fixture adapter remains setup state and cannot be counted.

### Blacksmith one craft

Use `blacksmith-one-craft.json` for one recipe, such as the example Rough Sharpening Stone fixture.

Preparation seeds Blacksmithing, the recipe, reagents, free bag space, and a clean output baseline. The operation sends exactly one `craft` command and polls `craft-status`. A pass requires the native completion receipt, the exact output delta, and reagent accounting from a stable read-only inventory snapshot.

The existing bridge can discover immediately craftable recipes and issue one craft. A fixture adapter is still needed to seed arbitrary reagents and profession state. `craft-status` completion without a durable output delta is `inconclusive`.

### Quest at giver

Use `quest-giver-acquisition.json` for a fresh character near a real quest giver. The q786 values are an example of the known normal-interaction fixture shape; the manifest labels them as fixture inputs, not a claim that every runtime has that route.

Preparation ensures the quest is absent and the character is eligible. The operation uses `quest-acquire` or the normal `quest` directive. A pass requires an acceptance receipt and the exact quest in `questlog`. If the fixture includes a gameobject objective, the exact native use receipt must precede the objective counter delta.

Quest status, rewards, objective counters, and completion rows must never be inserted by the fixture. A high-priority directive is allowed; a direct quest database write is never a valid adapter.

### Combat and role readiness

Use `combat-role-readiness.json` for a bounded tank/healer/damage pull.

Preparation seeds and observes the roster's role state, creates exactly the declared party/raid, and stages it. The operation engages one exact fixture target. A pass requires GUID-attributed native tank, healing/assist, and damage events plus an honest outcome: defeated, survived, wiped, or timed out.

The current bridge gives useful combat and encounter receipts, but a role proof may remain partial when threat, healing, avoidance, or interrupt attribution is absent. That missing attribution belongs in a role observer adapter; it must not be inferred from the character class or from the fact that the group survived.

### Accelerated quest progression

Use `quest-progression-accelerated-fixture.json` only in an isolated fixture runtime. It is a fast test of acquisition, objective execution, and native completion/reward transitions. It is not a way to level the persistent race seeds faster.

Preparation must attest to the 10x pacing lease, exact quest/giver/objective bindings, and a clean quest log. The operation remains normal quest acquisition and native objective execution. If a kill objective uses exact-target instant-kill, native combat/loot/objective receipts remain mandatory. A pass still requires observed quest-log milestones; XP or a missing quest row alone is not enough.

### Accelerated harvesting

Use `harvest-accelerated-fixture.json` only for a disposable worker/source pair. It shortens the test window but does not waive exact source attribution or durable material proof.

Preparation attests to 10x pacing, seeds the worker and exact source, and records a clean inventory. The operation runs one native gather attempt. A pass still requires the exact source receipt, native gather credit, and stable inventory delta. If an NPC guard or creature must be removed, exact-target instant-kill is optional and adapter-gated; it cannot substitute for gather credit.

## Capability matrix

| Capability | Existing bridge verbs and evidence | Adapter still needed | Current catalog stance |
|---|---|---|---|
| Activate a disposable character | `activate`, `list`, `fixture-init`, `fixture-status` | Exact character identity and ownership reservation | Usable for setup |
| Stage in a zone or near a giver | `route`, `advance`, `advance-point`, `snapshot` | A named route/fixture position when the route is absent | Position seed is setup; native movement proof remains separate |
| Claim a neutral faction | No safe generic verb | None until the server has a real neutral-faction model | Forbidden; use wayfarer/out-of-roster |
| Seed profession, talents, gear, tools, and bags | `professioneconomy` and read-only snapshots observe them | Exact loadout/profession/inventory fixture adapter | Partial |
| Gather an exact node | `deploy`, `snapshot`, `professioneconomy`, `gather_route`, read-only inventory/skill proof | Deterministic source spawn and exact fixture loadout | Partial; pass only with source attribution |
| Craft one recipe | `professioneconomy`, `craft`, `craft-status` | Reagent/profession seed and durable inventory snapshot | Operation available; setup/persistence partial |
| Acquire a quest normally | `quest-acquire`, `quest`, `questlog`, `acceptance` | Fresh quest/giver/route fixture when absent | Partial; native acceptance required |
| Execute ordinary kill/loot objectives | `quest`, `questobjective`, combat/loot observations | Exact fixture target/source when not already exposed | Partial to usable per objective |
| Use a gameobject for quest credit | `questobjective` and direct native GO receipts where emitted | Exact GO/giver fixture and route if absent | Partial; receipt plus counter delta required |
| Quest item use, escort, scripted event, dungeon/group objective | No general proof surface | Objective-specific adapter plus native receipts | Blocked until implemented |
| Create a small combat roster | `party`, `raid-create`, `fixture-init`, `route` | Exact role loadout and encounter target seed | Setup available; role proof partial |
| Engage an exact stable-spawn target | `engage`, `boss`, `combatlog`, `encounterlog` | Stable-spawn binding and capability handshake | Entry-only is not exact-spawn proof |
| Prove tank/healer/damage behavior | Combat and encounter logs when fields are present | Role attribution for threat, healing, avoidance, interrupts | Partial until observer fields exist |
| Accelerate a fixture to 10x | Isolated `fixture accelerate <guid> 1000` adapter; not enabled in runtime config | Explicit rate save/restore with `fixture-accelerate-off` cleanup and config isolation | Compiled-only; blocked until deployed fixture seed |
| Exact-target instant-kill | Isolated `fixture kill <guid> entry <entry> spawn <stable-spawn-id>` adapter; not enabled in runtime config | Explicit fixture-only capability and exact target binding | Compiled-only; blocked unless supported; never synthetic credit |
| Prove durable output or quest state | Existing read-only SQL and bridge telemetry harnesses | Generic snapshot adapter if a domain lacks a stable reader | Mandatory for pass |

## Adapter contracts to implement next

These are deliberately separate from the playerbot behavior code:

### `fixture-character-seed`

Input: lease id, exact GUID, level, race, class, spec, talents, skills, professions, gear, bags, inventory, map/instance, and optional setup position.

Output: a signed or hashed receipt listing every seeded field, the fields intentionally left empty, and a read-only baseline. It must reject campaign-owned GUIDs and must never write quest completion or reward state.

### `fixture-source-bind`

Input: map, instance, entry, stable spawn, source type, requested item/objective, and fixture lease.

Output: capability and in-world checks proving that the operation can select exactly that source. It must fail closed when only a nearest-entry search is available.

### `fixture-pacing-10x`

Input: fixture lease and expiry.

Output: a capability receipt showing `pacing_percent_of_baseline=1000` and `pacing_multiplier=10`, with runtime scope and an explicit idempotent `fixture-accelerate-off` cleanup step. The current adapter restores rates on that cleanup wire; the runner must retain the cleanup step even when the operation fails. It must not edit persistent campaign configuration.

### `fixture-exact-kill`

Input: exact target binding and fixture lease.

Output: a capability receipt and native combat/loot events for the same target. It must reject unbound targets and must never increment quest or inventory state directly. This is a test accelerator, not evidence that the bot's normal combat AI could have killed the target without the adapter.

### `role-observer`

Input: fixture roster and pull window.

Output: GUID-attributed threat, healing, damage, avoidance, interrupt, death, and encounter outcome events. It should enrich observation only; it must not steer the playerbots during a proof unless the manifest explicitly tests an existing control loop.

## Evidence ledger

Each run should write one JSONL receipt containing:

- catalog and manifest ids plus manifest hash;
- fixture lease, GUIDs, ownership, and source/target bindings;
- preparation mutations and baseline hashes;
- operation commands and timestamps;
- native response payloads and read-only snapshots;
- every postcondition with `pass`, `fail`, `blocked`, or `inconclusive`;
- cleanup result and protected-roster comparison.

Do not compress a blocked or inconclusive run into a success headline. The purpose of micro-scenarios is to make missing capability visible quickly and locally, so the next adapter can be small and testable.
