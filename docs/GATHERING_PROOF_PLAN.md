# Gathering Proof Harness

Status: implemented and parse-checked; `-Apply` has not been run.

## Outcome

`scripts/gathering-proof.ps1` is an exact-roster, dry-run-by-default proof harness. An apply run measures persisted gathering profession ranks and class-7 material inventory counts immediately before and after normal Playerbots gathering behavior. The script itself issues only guarded `SELECT` statements and the existing loopback bridge actions `list`, `activate`, `deploy`, and `snapshot`.

It does not start, stop, restart, provision, or reconfigure the worldserver. It does not train a profession, create a character, edit league enrollment, move an item, or issue direct database writes.

## Existing behavior audited

- `scripts/gathering-telemetry.ps1` establishes the current config parsing, MySQL invocation, `SELECT`-only guard, profession IDs, and class-7 material convention. Its snapshot target set is derived from every `autowow_league_member`, so the proof harness does not call that campaign-wide snapshot action. It uses the same conventions with exact fixture GUID predicates instead.
- `scripts/gathering-orchestrator.ps1` establishes dry-run by default and the explicit bridge apply pattern.
- `scripts/autowow-control.ps1` is the existing loopback-only bridge client. The proof harness invokes only its `list`, `activate`, `deploy`, and `snapshot` actions.
- `_phase1_worktree/mod-playerbots/src/Ai/Base/Strategy/LootNonCombatStrategy.cpp` shows that `gather` periodically adds gathering loot.
- `_phase1_worktree/mod-playerbots/src/Ai/Base/Actions/RevealGatheringItemAction.cpp` requires Mining or Herbalism and sufficient skill before revealing eligible distant nodes.
- `_phase1_worktree/mod-playerbots/src/Ai/Base/Actions/LootAction.cpp` uses normal Mining, Herbalism, and Skinning spells and checks required skill rank.
- `_phase1_worktree/mod-playerbots/src/Mgr/Item/LootObjectStack.cpp` rejects missing skills/ranks and requires a mining pick or skinning knife where applicable.
- `_phase1_worktree/mod-playerbots/src/AutoWow/AutoWowBridge.cpp` requires an unretired league enrollment for `activate`/`deploy`; an ungrouped deploy applies `+gather,+grind,+move random,-follow` and reports `mode=worker_gather`, `configured=1`. A grouped character would receive `party_grind`, so the harness rejects grouped targets before deploy.

## Fixture roster contract

`-Apply` never infers targets from the campaign roster. It requires a JSON file supplied with `-FixtureRosterPath`:

```json
{
  "schema": "autowow.gathering-proof.fixture-roster.v1",
  "purpose": "gathering-proof",
  "allow_campaign_characters": false,
  "members": [
    {
      "guid": 123,
      "name": "ExactCharacterName",
      "profession": "Mining"
    }
  ]
}
```

The roster must contain one to ten unique positive GUIDs. `profession` must be `Herbalism`, `Mining`, or `Skinning`. `name` is optional, but when supplied it must match the database character name.

Canonical campaign membership is detected from the `northstar`, `ember`, or `wayfarers` team IDs, plus the current unteamed `wayfarer` affiliation. Other explicitly named fixture teams are not classified as campaign merely because they use a generic affiliation label. A campaign GUID is refused unless the versioned fixture roster also sets `allow_campaign_characters` to `true`. This makes the roster both an exact target allow-list and an explicit campaign override; an arbitrary GUID or default campaign roster can never reach the apply path.

The current bridge also requires each fixture GUID to have an unretired `autowow_league_member` row. This harness observes that enrollment but never creates or edits it.

## Operation

Dry-run performs no SQL query and no bridge call:

```powershell
pwsh -File .\scripts\gathering-proof.ps1
pwsh -File .\scripts\gathering-proof.ps1 -FixtureRosterPath .\fixtures\gathering-proof.json
```

An operator-reviewed apply command is:

```powershell
pwsh -File .\scripts\gathering-proof.ps1 `
  -FixtureRosterPath .\fixtures\gathering-proof.json `
  -DurationSeconds 600 `
  -OutputPath .\logs\gathering-proof-reviewed.json `
  -Apply
```

Apply gates run in this order:

1. Validate the versioned roster and explicit campaign authorization.
2. Resolve only those GUIDs and require existing characters plus unretired bridge enrollment.
3. Collect an exact-GUID baseline with `SELECT` queries for gathering skill rows, class-7 character inventory, and gathering tools.
4. Require a positive expected profession skill and any required mining/skinning tool.
5. List bridge bots; activate only missing exact GUIDs; wait for those GUIDs; reject paused or grouped bots.
6. Deploy each GUID and require `worker_gather` with exactly one configured bot.
7. Observe normal world behavior for the requested duration.
8. Collect the same exact-GUID current snapshot, calculate all three gathering skill deltas and aggregated material inventory deltas, and capture final bridge snapshots.

Per-member `pass` requires the expected profession to remain present and non-regressing, required tools at baseline, at least one positive class-7 inventory delta, and no negative class-7 delta. No positive material delta is `inconclusive`, not success. Missing/regressing skill, missing tool, or negative material movement is `fail` because the run is invalid or confounded.

## Safety properties

- The apply guard fails before SQL or bridge access when `-FixtureRosterPath` is absent.
- SQL must begin with `SELECT`, contain no statement separator, and contain no mutating/administrative keyword.
- Every SQL predicate uses only uint32 GUIDs parsed from the versioned roster; database identifiers accept only letters, digits, and underscores.
- Bridge host is restricted to `127.0.0.1`.
- Allowed bridge actions are hard-coded to `list`, `activate`, `deploy`, and `snapshot`.
- No server lifecycle script, configuration writer, chat action, fixture initializer, profession trainer, or database mutation surface is called.
- Evidence includes query/bridge call counts, baseline/current rows, control receipts, verdicts, and the exact output path.

## Limitations

- `-Apply` was deliberately not run, so there is no live gathering verdict yet.
- SQL observes authoritative persisted rows, but online save timing may delay visibility of a skill or inventory change.
- Class-7 inventory changes are temporal evidence while `worker_gather` is active; the bridge exposes no per-node loot receipt, so the harness cannot prove which exact node produced an item.
- The random gathering route may encounter no eligible node during a short window; that outcome is correctly reported as `inconclusive`.
- Mining and Skinning are gated on the tool item IDs enforced by current Playerbots code, restricted to equipped/backpack/carried-bag locations rather than bank storage. Herbalism has no tool gate.
- `deploy` intentionally changes only the selected bot's non-combat strategy. The current bridge has no safe read/restore surface for the prior strategy, so the harness leaves the explicit fixture in gathering mode and does not pretend to restore it.
- Non-enrolled disposable characters cannot be used because the existing bridge refuses them. This harness will not bypass that restriction or add enrollment with SQL.
- Material counts cover `character_inventory` items whose template class is 7. Mail, auction, guild-bank, vendor, and completed-sale attribution are outside this focused proof.

## Owned files

- `scripts/gathering-proof.ps1`
- `docs/GATHERING_PROOF_PLAN.md`

No existing source, script, config, database row, or server process is modified by this change.
