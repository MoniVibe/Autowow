# Dungeon Party V0

Status: plan and offline verifier complete; live provisioning/orchestration was not run.

This lane owns only a disposable, local Utgarde Keep normal fixture. The default fixture ID is `awdk1`, the default level is 70, and the default factory gear request is `rare`. The target is map 574, normal difficulty, with the first selected elite pack gated before Ingvar the Plunderer.

The current checkout cannot safely execute the fixture. The new executor therefore fails closed even when `-Execute` is explicitly supplied. It never starts `worldserver`, opens the AutoWow bridge, invokes MySQL, edits a character row, or calls an existing provisioner.

## Commands

Run these from `D:\Games\wowstuff\AutoWoW`:

```powershell
# Dry-run plan; writes a JSONL receipt beneath .\logs unless -ReceiptPath is supplied.
& .\scripts\dungeon-fixture-run.ps1 -Action Plan

# Deterministic alternate fixture identity.
& .\scripts\dungeon-fixture-run.ps1 -Action Plan -FixtureId lane7 -Level 70 -GearQuality rare

# Evaluate a future versioned evidence file without contacting the server.
& .\scripts\dungeon-fixture-run.ps1 -Action Evaluate -EvidencePath .\path\to\evidence.json

# Expose the teardown plan and retain receipts.
& .\scripts\dungeon-fixture-run.ps1 -Action Teardown

# Explicit execution is intentionally blocked by the current gap report.
& .\scripts\dungeon-fixture-run.ps1 -Action Execute -Execute
& .\scripts\dungeon-fixture-run.ps1 -Action Teardown -Execute
```

`-Action Plan` and `-Action Teardown` are dry-run by default. `-Action Execute` requires the second `-Execute` switch and then emits `DNG-EXEC-BLOCKED` plus a JSON gap report. This is an intentional safety result, not a partial live run.

## Fixture contract

The five members have deterministic, length-safe account and character names derived from the fixture ID. The default manifest is:

| Fixture role | Class | Required spec | Spec index | Name |
| --- | --- | --- | ---: | --- |
| tank | Paladin | `prot pve` | 1 | `AWDK1TANK` |
| healer | Priest | `disc pve` | 0 | `AWDK1HEAL` |
| dps-mage | Mage | `fire pve` | 1 | `AWDK1MAGE` |
| dps-rogue | Rogue | `combat pve` | 1 | `AWDK1ROGUE` |
| dps-hunter | Hunter | `bm pve` | 0 | `AWDK1HUNT` |

All members are one faction, level 70, and must be created or repaired only through the legitimate PlayerbotFactory path. The intended generation proof is `PlayerbotFactory.Randomize(false)` with explicit evidence for level, skills, talents, learned spells, and gear. The lane never fabricates combat results and never writes `characters`, `character_inventory`, `item_instance`, `character_talent`, `character_skills`, or `character_spell` rows.

Idempotence is exact-name and exact-fixture scoped:

1. Read the manifest names and ownership first.
2. Reuse only an exact, fixture-owned match.
3. Repair only missing state through the approved factory adapter.
4. Fail on a class, faction, owner, GUID, or name mismatch; never select an arbitrary random bot.

The JSONL receipt records every planned command, including whether it is a mutation, its stage, its exact fixture scope, and why the current surface is or is not supported. Secrets are represented by environment-variable names/placeholders only.

## Stage order and acceptance gates

The state machine is:

```text
readiness PASS
    -> first elite pack
    -> elite-pack gate PASS
    -> Ingvar
    -> completed
```

Ingvar is blocked unless the elite-pack gate is `PASS`. A failed prerequisite is recorded as `BLOCKED`; the evaluator never treats missing evidence as success.

Readiness requires:

- exactly five unique manifest members, same faction, exact class/role/spec/name, and level 70;
- template evidence for `Randomize(false)`, skills, talents, and gear for every member;
- gear `completeness_ratio >= 1`, no missing or invalid slots, and every observed slot marked verified and either occupied or a legitimate class-specific empty slot;
- provenance counters of zero for direct DB mutations, combat-result mutations, teleports, and GM combat commands.

Each combat stage requires:

- `completed = true` and zero party deaths;
- tank victim ownership at least 90% of sampled hostile events;
- at least one taunt, with every observed taunt confirmed against the intended victim window;
- healer tank-priority ratio at least 60%, positive effective tank healing, and total effective healing no lower than effective tank healing.

Ingvar additionally requires `boss_name = Ingvar the Plunderer`, `completed = true`, and credit for all five expected fixture roles. Effective healing means non-overheal healing; raw cast count alone is not acceptance evidence.

The evidence shape is `autowow.dungeon.fixture.evidence.v1`. The output gate shape is `autowow.dungeon.fixture.acceptance.v1`.

## Audit of existing surfaces

### Account and character provisioners

- `scripts\create-account.ps1:12-20,52-53,84-89,109-116` requires process environment secrets, starts a temporary `worldserver`, sends `.account create` and `.account set gmlevel`, then uses direct auth SQL to ensure GM access and writes `ACCOUNT_REPORT.md`. It provisions one account, not a deterministic five-character party.
- `scripts\provision-local-player.ps1:10-35` is a stop/provision/restart wrapper around the existing account flow. It does not create characters, bind classes/specs, or return fixture ownership.
- `scripts\database.ps1:13-43,90-113` is live DB/bootstrap orchestration. It is not called by this lane.

### PlayerbotFactory, random bots, and commands

- `modules\mod-playerbots\src\Bot\Factory\PlayerbotFactory.h:60-73` exposes the legitimate factory concepts: `Randomize`, skills, talent trees, exact internal `InitTalentsBySpecNo`, and equipment.
- `modules\mod-playerbots\src\Bot\Factory\PlayerbotFactory.cpp:613-742,870,1491-1569,2103,3040` confirms that `Randomize(false)` gives the level, initializes skills/spells/talents/equipment, and saves the bot. The exact spec-index helper exists internally, but is not an external fixture command.
- `modules\mod-playerbots\src\Bot\RandomPlayerbotMgr.cpp:1886-1950,2367-2433` shows random-bot initialization chooses a configured/random level and uses `PlayerbotFactory`; console handling targets online random bots from the configured random account list. It does not create a deterministic class/spec roster.
- `modules\mod-playerbots\src\Bot\PlayerbotMgr.cpp:681-810` shows AddClass `init=rare`/`init=blue` uses `PlayerbotFactory` at the master's level, but the current path does not select the required exact spec. `:875-881,1087-1183` shows AddClass creation requires an active player session and consumes a cached same-faction class character.
- `modules\mod-playerbots\src\Script\PlayerbotCommandScript.cpp:31-54` registers `.playerbots bot` as `Console::No` and `.playerbots rndbot` as `Console::Yes`. This split is why the current console/bridge surfaces cannot perform the complete deterministic flow.
- `modules\mod-playerbots\src\Bot\Factory\AiFactory.cpp:142-229,298-342` maps live talent tabs to roles/spec names. The acceptance contract checks the actual observed spec instead of trusting class alone; Disc is healer and Protection Paladin is tank by this mapping.

The current local config reinforces the gap: `server\configs\modules\playerbots.conf:101,139` has one AddClass account and enables the AddClass command; `:704-705,771,775` fixes random bots at level 80; `:804-814` enables persistence and sets the random gear quality limit; `:1464,1494,1545,1574,1681` contains the desired premade labels; but `:1854-2025` leaves class spec choice probabilistic rather than fixture-deterministic. Existing config was not edited.

### Console, bridge, and dungeon surfaces

- `modules\mod-playerbots\src\AutoWow\AutoWowBridge.cpp:238-290` exposes identity, pose, vitals, level/xp/money, current target, group count/leader, travel, combat, and action state. It does not expose gear, exact spec, skills/talents, victim ownership, taunts, effective healing, deaths, or boss credit.
- `:448-506` can form a same-faction group from online GUIDs. It does not provision the members or prove fixture ownership.
- `:513-540` implements rally with `TeleportTo`; that is deliberately excluded from natural dungeon-readiness proof.
- `:673-677` implements broad `attack anything`, not a named first-pack or boss-stage command. `:1216-1349` parses the existing bridge verbs, and `:1421` confirms the local bridge port.
- `AUTOWOW_BRIDGE.md:3,23-27,45` documents the loopback-only boundary and explicitly says the bridge has no arbitrary console, SQL, filesystem, account, or network-control operation.
- `modules\mod-playerbots\src\Ai\Dungeon\DungeonStrategyContext.h:51,70` and `modules\mod-playerbots\src\Ai\Dungeon\UK\UKStrategy.cpp:4-41` provide native Utgarde Keep strategy hooks, including Ingvar smash reactions and encounter multipliers. They are AI internals, not a deterministic external entry/target/stage/telemetry API.

### Database schema

The relevant schema is relational and owner-sensitive:

- `data\sql\base\db_characters\characters.sql:23` stores character identity, account, class, level, map, online state, and related state.
- `character_inventory.sql:23`, `item_instance.sql:23`, and their keys link equipment slots to item rows and item owners.
- `character_skills.sql:23`, `character_talent.sql:23`, and `character_spell.sql:23` store the generated skill/talent/spell state.
- `groups.sql:23` and `group_member.sql:23` store group identity, leader, difficulty, and member roles.
- `modules\mod-playerbots\data\sql\playerbots\base\playerbots_random_bots.sql:2`, `playerbots_account_type.sql:2`, and `playerbots_db_store.sql:2` store random-bot event state, AddClass/RNDbot account classification, and bot key/value state.

The new lane emits `SELECT` probes for these relationships only. Hand-written character/account/playerbot `INSERT`, `UPDATE`, or `DELETE` operations are not an acceptable substitute for the factory or an owner-aware teardown adapter.

## Precise gap report

`DNG-EXEC-001` — No five-member owner-aware account/character adapter. The existing account script is live and single-account.

`DNG-EXEC-002` — No external exact-spec operation. AddClass factory init is legitimate but spec selection remains random; `InitTalentsBySpecNo` is internal.

`DNG-EXEC-003` — No deterministic dungeon entry, first-elite-pack target, or Ingvar stage command. Current `engage` is broad and current rally teleports.

`DNG-EXEC-004` — No acceptance telemetry for gear/spec/template state, tank victim/taunt, healer effective heals, deaths, or boss credit.

`DNG-EXEC-005` — No owner-aware disband and teardown. Direct deletion risks orphaning group, item, skill, talent, spell, and playerbot rows.

`DNG-EXEC-006` — Existing config is not a disposable fixture profile: one AddClass account, level-80 random-bot defaults, and probabilistic specs. Editing it is outside this lane.

To make execution safe in a later lane, add a fixture-scoped adapter that returns an exact five-member manifest, invokes the factory with exact spec indices, emits the evidence schema above, provides stage tokens for natural entry/target progression, and tears down only manifest-owned objects. That future work necessarily touches server/module surfaces and is intentionally not included here.

## Files owned by this lane

- `scripts\dungeon-fixture-lib.ps1` — deterministic definition, read-only probes, plan/teardown operations, JSONL receipt writer, gap report, and acceptance evaluator.
- `scripts\dungeon-fixture-run.ps1` — dry-run default entry point, explicit Execute guard, Evaluate action, and teardown exposure.
- `scripts\tests\dungeon-fixture.tests.ps1` — offline Pester, parser, receipt, fail-closed, idempotence, and acceptance-gate coverage.
- `DUNGEON_PARTY_V0.md` — this audit and handoff.

No C++, existing script, or existing config was edited. No live server, bridge, client, or database command was run.
