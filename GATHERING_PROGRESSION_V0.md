# Gathering / Profession Progression V0

Status: implementation-ready, read-mostly, and not run against the live realm.

This lane is for the two racing guilds in the current league manifest:

- Northstar Alliance (`northstar`)
- Ember Horde (`ember`)

The lane does not train professions, create characters, move items, post auctions, sell to vendors, deposit to guild banks, or mutate league rows. It plans those actions, observes their evidence, and exposes an explicitly guarded bridge apply path for already-eligible worker characters.

## What was audited

The existing league surfaces are the source of roster and runtime truth:

- `scripts/league-simulation.ps1 -Action roster -AsJson` reads `autowow_league_member` joined to `characters`. Other actions in that script can seed or launch state and were not called.
- `scripts/league-sanity-snapshot.ps1` already reads `character_skills`, talent state, and equipped gear. Its primary skill IDs are reused here.
- `scripts/league-telemetry.ps1` already observes bridge bots, deaths, revives, map changes, and world metrics, but does not observe profession or item/economy flows.
- `scripts/league-ledger.ps1` reads character money and progression state, but does not provide a transaction journal.
- `scripts/autowow-control.ps1` is the loopback bridge client. `deploy` is the existing activation surface for a singleton worker.

The current Playerbots audit found that gathering is an AI behavior for a bot that already has the required skill, not a profession enrollment or training system:

- `modules/mod-playerbots/src/Ai/Base/Strategy/LootNonCombatStrategy.cpp` provides the `gather` strategy and its `add gathering loot` trigger.
- `RevealGatheringItemAction.cpp` only considers nearby mining/herbalism/fishing objects when the bot has the corresponding skill and required rank.
- `LootAction.cpp` casts the normal Mining, Herbalism, Skinning, and Engineering/profession spells after skill gates pass.
- `AutoWowBridge.cpp` already implements league-worker deploy as `+gather,+grind,+move random,-follow` and reports `mode = worker_gather`.
- `SetCraftAction.cpp` supports `craft ?` and normal craft requests. `BankAction.cpp` supports `bank ?`. `MailAction.cpp` supports `mail ?`. The current guild-bank action has no read-only listing command; `gb` with an item is a mutating deposit path and is not used here.
- `BuyAction.cpp`, `SellAction.cpp`, and mail/bank withdrawal paths mutate normal game state when given action arguments. This lane never sends those arguments.

The generated league sanity evidence currently reports Brandreas with Alchemy 5/75 plus Herbalism 5/75 and Saewash with Mining 5/75 plus Engineering 5/75. Wayfarer has no primary profession and must not be credited with gathering output until a future worker eligibility gate is met.

## Roster and profession planner

`[gathering-planner.ps1](scripts/gathering-planner.ps1)` assigns distinct roster candidates to dependency-valid profession pairs. It defaults to the league ceiling of ten worker slots per team. Seven slots are the minimum plan that covers all eleven primary professions; slots eight through ten are redundant capacity.

The first seven slots per side are:

| Slot | Gathering / processing pair | Coverage reason |
|---:|---|---|
| 1 | Herbalism + Alchemy | herbs to consumables |
| 2 | Mining + Engineering | ore to utility |
| 3 | Skinning + Leatherworking | leather to armor |
| 4 | Herbalism + Inscription | herbs to glyph/inscription inputs |
| 5 | Mining + Jewelcrafting | ore to gems |
| 6 | Mining + Blacksmithing | ore to weapons/armor |
| 7 | Tailoring + Enchanting | cloth and disenchant/enchant coverage |

Slots 8–10 add redundant Alchemy, Engineering, and Leatherworking capacity. The planner never treats an arbitrary two-profession declaration as a valid coverage pair: it reports mismatches and invalid current pairs as proposals requiring correction. It also excludes captains and inactive members, keeps each GUID in one slot, and can be restricted to existing `role = worker` rows with `-WorkersOnly`.

League metadata is not skill proof. Pass `-TelemetryPath` with a snapshot from the telemetry script to let the planner mark a worker `live_ready`; without observed `character_skills` rows, an otherwise matching declaration remains `telemetry_required`.

Examples:

```powershell
pwsh -File .\scripts\gathering-planner.ps1 -AsJson -OutputPath .\leagues\results\gathering-plan.json
pwsh -File .\scripts\gathering-planner.ps1 -WorkersOnly -WorkersPerTeam 7 -RosterPath .\fixtures\roster.json -AsJson
pwsh -File .\scripts\gathering-planner.ps1 -RosterPath .\fixtures\roster.json -TelemetryPath .\leagues\results\gathering-snapshot.json -WorkersPerTeam 7 -AsJson
```

The first command uses the existing roster helper read path. It does not invoke league seed, launch, record, or repair actions.

## Read-only telemetry

`[gathering-telemetry.ps1](scripts/gathering-telemetry.ps1)` has three modes:

- `-Action sql` emits the query contract and mechanically rejects non-`SELECT` SQL.
- `-Action snapshot` runs the contract through the existing local MySQL client and emits JSON containing members, skill ranks, trade-good material rows, money rows, current auction exposure, guild membership/bank state, and guild-bank events.
- `-Action delta -BaselinePath ... -CurrentPath ...` computes skill, material, money, current-auction, and new guild-bank-event deltas without writing to either database.

The snapshot queries are read-only against these existing tables:

- League/roster: `autowow_league_member`, `characters`
- Skills: `character_skills`
- Inventory/materials: `character_inventory`, `item_instance`, `item_template`
- Mail: `mail_items`, `mail`, `item_instance`, `item_template`
- Auctions: `auctionhouse`, `item_instance`, `item_template`
- Guild flow: `guild_member`, `guild`, `guild_bank_item`, `guild_bank_eventlog`

Material rows are separated into `character_storage`, `pending_mail`, and `guild_bank`. Item template buy/sell prices are carried as references, not as proof of a sale or purchase. The current schema has no exact vendor transaction journal, and the auction table is current-state exposure rather than completed-settlement history. Therefore vendor and completed-auction claims are always labelled `inferred_only` and require corroborating before/after money, mail, inventory, or guild-bank evidence.

Examples:

```powershell
pwsh -File .\scripts\gathering-telemetry.ps1 -Action sql -AsJson
pwsh -File .\scripts\gathering-telemetry.ps1 -Action snapshot -OutputPath .\leagues\results\gathering-snapshot.json -AsJson
pwsh -File .\scripts\gathering-telemetry.ps1 -Action delta `
  -BaselinePath .\leagues\results\gathering-before.json `
  -CurrentPath .\leagues\results\gathering-after.json -AsJson
```

## Dry-run orchestration and existing bridge surface

`[gathering-orchestrator.ps1](scripts/gathering-orchestrator.ps1)` is dry-run by default. It produces per-assignment bridge steps and read-only manual chat inspection surfaces:

- inspection: `craft ?`, `mail ?`, and `bank ?`
- proposed worker activation text for an operator: `nc +gather,+grind,+move random,-follow`
- actual automatic activation, when explicitly authorized: existing bridge `list`, conditional `activate`, `deploy`, and `snapshot`

The proposed chat text is not sent by the script. The current guild-bank chat action has no read-only listing surface, so `gb` is deliberately not emitted as an executable telemetry command.

Default operation calls no bridge and performs no database write:

```powershell
pwsh -File .\scripts\gathering-orchestrator.ps1 `
  -PlanPath .\leagues\results\gathering-plan.json -AsJson
```

An operator must deliberately authorize runtime bridge calls, and only assignments already marked `live_ready` are eligible:

```powershell
pwsh -File .\scripts\gathering-orchestrator.ps1 `
  -PlanPath .\leagues\results\gathering-plan-with-skill-proof.json `
  -BridgeScriptPath .\scripts\autowow-control.ps1 `
  -Apply -ConfirmLiveBridge -AsJson
```

`-Apply` without `-ConfirmLiveBridge` fails before any bridge call. Even with both switches, this script does not execute SQL or send chat commands. The bridge's `deploy` result must still report `ok = true`, `mode = worker_gather`, and `configured = 1` for the worker to be considered activated.

## Proposed DB writes (not run)

No DB write is unavoidable for this V0 lane. The normal profession-learning path must be a server/Playerbot API or trainer interaction; directly editing `character_skills`, `character_inventory`, `item_instance`, `auctionhouse`, `mail`, or guild-bank tables is out of bounds.

If a future enrollment adapter proves that a league record is required, the only proposed write is a narrowly scoped transaction for a disposable NPC worker character:

1. Verify the GUID is a newly created disposable NPC worker, has the correct faction, is not in either racing roster, is not a captain/adventurer, and has no existing league membership.
2. Learn the two professions through the normal game/Playerbot API and collect `character_skills` evidence.
3. Optionally insert or update only that worker's `autowow_league_member` metadata (`team_id`, `role = worker`, class plan, and the two declared professions), or append one proposal/event record if the existing league schema provides that event surface.
4. Re-read the worker and reject the proposal if the pair, role, faction, or skill ranks do not match.

This proposal must not touch a real roster character, guild bank, mail, auction, inventory, item instance, or `character_skills` row directly. It is not implemented or executed by the scripts in this lane.

## Exact live acceptance gates

Run the gates in order. A failed gate is a stop, not a best-effort continuation.

### Gate 0 — scope and safety

- Confirm the target is the local loopback bridge and the intended disposable worker test set.
- Confirm the plan JSON has `safety.db_writes = 0` and the telemetry snapshot has `read_only = true`.
- Confirm the default orchestrator dry run reports `dry_run = true`, `safety.bridge_calls = 0`, and `safety.chat_commands_sent = 0`.
- No live apply is allowed while either racing guild's real adventurer roster is being used as a disposable test subject.

### Gate 1 — roster eligibility

For each side, the pilot must use one distinct active worker GUID. Before expansion, require:

- `role = worker`, correct Alliance/Horde affiliation, and a valid character row;
- no GUID shared between Northstar and Ember;
- no captain or racing adventurer assigned as a worker;
- one assignment slot per GUID;
- no more than ten worker slots per side;
- the worker is online/listed when the explicit bridge apply is attempted.

### Gate 2 — profession proof

For every planned pair, the telemetry snapshot must contain exactly two positive primary `character_skills` rows for that worker, with names matching the plan. The pair must be one of the seven dependency-valid pairs in the planner output. A league metadata declaration without skill rows is `telemetry_required`, not a pass.

Secondary skills (`Fishing`, `Cooking`, and `First Aid`) may be observed and reported, but do not satisfy a primary pair or coverage slot.

### Gate 3 — one-worker bridge smoke

For one pilot worker per side, under operator supervision:

1. Run the dry-run output review.
2. Run explicit apply with `-Apply -ConfirmLiveBridge`.
3. Require `list` to identify the intended bot or `activate` to return success.
4. Require `deploy` to return `ok = true`, `mode = worker_gather`, and `configured = 1`.
5. Require the following `snapshot` to identify the same GUID and no pause/deactivation state.

Any bridge error, wrong GUID, wrong faction, unexpected party assignment, or non-`worker_gather` mode fails the gate.

### Gate 4 — ten-minute evidence window

Collect a baseline snapshot immediately before deploy and a second snapshot after ten minutes of normal world time. Pass only if, for each pilot worker:

- at least one positive material delta exists in the expected gathering channel or the run is explicitly marked `inconclusive` because no eligible node spawned;
- the positive delta uses the worker's planned gathering profession and item is classified as a trade-good candidate;
- no unexpected negative inventory/mail/guild-bank delta occurs;
- no unapproved auction, vendor, mail, bank, or guild-bank mutation is present;
- skill deltas are non-negative and attributable to the pilot worker;
- money deltas are explained by observed mail, auction, guild-bank, or vendor evidence; otherwise the run fails attribution;
- the worker remains online/healthy, and no non-worker GUID is credited with the output.

`inconclusive` is a repeatable evidence state, not a success claim. Repeat once with a known eligible node before expanding the lane.

### Gate 5 — expansion

Expand from one pilot worker per side to the next assignment only after Gates 0–4 pass for both sides. Re-run the planner and telemetry snapshot after every profession assignment change. Do not exceed ten workers per guild, and do not convert an adventurer or captain into a worker without the disposable-worker proposal and fresh skill proof.

## Tests and changed paths

The new Pester tests use only JSON fixtures and a fake bridge under Pester's test drive; they do not connect to MySQL or the live bridge. Run:

```powershell
Invoke-Pester -Path .\scripts\tests\gathering-planner.tests.ps1, .\scripts\tests\gathering-telemetry.tests.ps1, .\scripts\tests\gathering-orchestrator.tests.ps1 -Output Detailed
```

The intended changed paths are exactly:

- `scripts/gathering-planner.ps1`
- `scripts/gathering-telemetry.ps1`
- `scripts/gathering-orchestrator.ps1`
- `scripts/tests/gathering-planner.tests.ps1`
- `scripts/tests/gathering-telemetry.tests.ps1`
- `scripts/tests/gathering-orchestrator.tests.ps1`
- `GATHERING_PROGRESSION_V0.md`

No C++, existing script, existing config, live realm state, or database row was modified by this implementation.
