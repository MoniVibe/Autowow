# Oracle Controlled Agent Language v1

This is an AutoWoW project convention inspired by the clarity goals of ASD-STE100. It is **not** a claim of formal ASD-STE100 compliance. ASD-STE100 is a controlled language for technical documentation. This project uses the same useful discipline for machine-checked bot behavior.

## Purpose

Playerbots must act independently, cooperate when useful, and report failure without hiding it. The controlled language gives every behavior the same shape:

```text
WHEN <preconditions>
DO <one action>
VERIFY <observable result>
ON_FAILURE <stable failure code>
THEN <recover, replan, idle, or none>
```

The runtime consumes structured JSON fields. The sentence is a readable rendering of those fields.

## Rules

1. Use one approved verb for one action.
2. Use one target for one action.
3. State preconditions before execution.
4. Define success with an observable signal.
5. Emit a stable failure code when the signal is absent.
6. Do not infer success from movement, time passing, XP, or an `ok:true` transport response.
7. Do not silently change a quest action into generic grinding.
8. Do not require a leader. Cooperation is voluntary and local.
9. A planned contract cannot claim a live execution or recovery loop.
10. A random, generic, or nearby target is not an exact Oracle target.

## Vocabulary

Phases are:

```text
observe, plan, execute, verify, recover
```

Action verbs are:

```text
observe, plan, target, travel, interact, loot, gather, craft,
train, fight, assist, rest, recover, choose_talent, equip, cooperate
```

Contracts have one of three statuses:

```text
native   The deployed Playerbot runtime already performs this behavior.
planned  The contract is defined, but an exact runtime operation or proof is missing.
blocked  The behavior is intentionally unavailable until a stated dependency exists.
```

## Example

The quest objective contract is expressed as:

```text
WHEN quest.active IS true
AND objective.kind IS npc
AND objective.remaining IS greater_than 0
DO target objective.required_entry
VERIFY objective.current INCREASES OR quest.completed IS true
ON_FAILURE quest:objective_no_progress
THEN recover
```

The machine contract requires `questobjective.current` or `questlog` evidence. It does not accept movement, XP, or a successful bridge response as proof of quest progress.

## Doctrines

Doctrine is a priority order, not a separate AI implementation. The initial doctrines are:

- `independent_quester`: quest progress first, then survival, loot, travel, gathering, crafting, and training.
- `independent_gatherer`: survival and material acquisition first, then crafting and training.
- `independent_survivor`: survival and combat first, then quest progress.
- `independent_support`: survival and combat assistance first, with voluntary cooperation.

All four explicitly require no leader. A bot can ask for or provide assistance, but it can continue its own plan when no ally is present.

## Current capability boundary

Quest objective targeting, quest turn-in proof, gathering telemetry, training, and combat are represented as native contracts. Exact crafting is represented as `planned` because the runtime still lacks a deterministic `craft <guid> <recipe>` operation and a positive per-bot craft receipt. The contract therefore refuses to claim craft success.

## Validation

Validate the catalog with:

```powershell
.\scripts\validate-oracle-agent-contracts.ps1
```

The catalog is at:

```text
contracts\oracle-agent-contracts.v1.json
```
