# Raid Loot Snapshot Runbook

`scripts/raid-loot-snapshot.ps1` records durable inventory and equipment evidence for an exact roster of persistent dungeon or raid bots. It performs one `SELECT` query joining only `character_inventory`, `item_instance`, and `item_template`. It does not use the AutoWoW bridge, contact the worldserver, or write database rows.

## Safety contract

- Supply every character as an exact decimal uint32 GUID in the range `1..4294967295`. Duplicate, malformed, zero, and overflowing values fail closed.
- The script reads `CharacterDatabaseInfo` and `WorldDatabaseInfo` from the existing `worldserver.conf`.
- Passwords are passed to the MySQL client through the process-scoped `MYSQL_PWD` variable, restored immediately afterward, and never included in output or errors. Output contains database names but no host, port, user, or password.
- Generated SQL must begin with `SELECT` and passes a mutation-keyword guard before execution.
- The returned rows are rejected if any character GUID is outside the requested roster.
- A fixture `-QueryExecutor` seam bypasses MySQL completely for Pester tests.

## Capture a baseline

From the AutoWoW root:

```powershell
pwsh -NoProfile -File .\scripts\raid-loot-snapshot.ps1 `
  -RosterGuid 101,102,103,104,105 `
  -WorldServerConfigPath .\server\configs\worldserver.conf `
  -OutputPath .\artifacts\raid-before.json
```

The output directory must already exist. Omit `-OutputPath` to emit JSON to stdout. Pass `-MySqlPath` only when the existing MySQL client is not discoverable.

Each character contains exactly 19 `equipped_slots` records for slots `0..18`, including explicit null-valued empty slots. `carried_items` contains all other `character_inventory` rows, including backpack, bag, and bag-container entries. Item records include item GUID, entry, name, quality, item level, durability, and stack count. Character and roster aggregates report item-GUID count, stack count, equipped/carried counts, total item-level sum, equipped item-level sum, and equipped average item level.

## Compare after a controlled run

After the same persistent roster completes the dungeon or raid and all loot/equipment changes have settled:

```powershell
pwsh -NoProfile -File .\scripts\raid-loot-snapshot.ps1 `
  -RosterGuid 101,102,103,104,105 `
  -WorldServerConfigPath .\server\configs\worldserver.conf `
  -BaselinePath .\artifacts\raid-before.json `
  -OutputPath .\artifacts\raid-after.json
```

The baseline schema and roster must match exactly. `comparison` reports:

- `new_item_guids`: durable item instances present only after the run;
- `removed_item_guids`: item instances no longer held by the roster;
- `equipped_slot_replacements`: slots where one equipped item GUID was replaced by another, with old/new item details and item-level delta;
- `character_item_level_deltas`: equipped item-level sum and average changes per character.

A new GUID proves that the persistent character now holds a new item instance; a before/after replacement proves that persistent equipment changed. To attribute that acquisition specifically to raid loot, keep the interval controlled: do not trade, mail, auction, vendor, administer items, or run unrelated content between snapshots. This inventory-only slice does not identify the drop source by itself.

## Deterministic schema

The schema version is `raid-loot-snapshot-v1`. Top-level fields and nested fields have stable order. Roster GUIDs, characters, item rows, comparison rows, and slots are numerically sorted. `captured_utc` intentionally changes per capture.

## Offline verification

These checks parse the script and run fixture-only tests. They do not invoke MySQL, the bridge, or either server:

```powershell
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path .\scripts\raid-loot-snapshot.ps1),
  [ref]$null,
  [ref]$errors
)
if ($errors.Count) { $errors | Format-List; throw 'PowerShell parse failed.' }

Invoke-Pester -Path .\scripts\tests\raid-loot-snapshot.tests.ps1 -Output Detailed
```
