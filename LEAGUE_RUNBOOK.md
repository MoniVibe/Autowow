# AutoWow league runbook

This league is a local benchmark and progression framework for two Playerbots teams. It does not expose any ports or grant network access.

## Roster shape

WoW 3.3.5 permits at most ten characters per account on one realm. A player-led 40-person raid therefore needs one active captain plus 39 bots distributed across four linked roster accounts. A fully autonomous opposing 40-character team likewise needs four roster accounts or a separately managed generated-bot pool.

The server currently permits bots from the captain's own account and linked accounts, with a 40-bot controller ceiling. Account linking is two-way and stored in `acore_playerbots.playerbots_account_links`; it must be created through the Playerbots security-key workflow or a later secret-scoped provisioning tool.

## Modes

### Persistent progression league

Use named characters on linked roster accounts. Gear, inventory, quests, achievements, and raid lockouts remain persistent. Do not use maintenance, autogear, BiS, or free-consumable commands after the agreed starting point.

Score PvE by clear time, attempts, wipes, deaths, loot upgrades, and median gear score. Put competing teams into separate instance groups so neither team's lockout or spawn state affects the other.

### Normalized combat benchmark

Use generated/AddClass or controlled random bots at an agreed level, spec, and gear floor. This is for strategy and performance comparisons, not progression. The recommended PvP event is Alterac Valley because it is designed for 40 Alliance versus 40 Horde players.

## Ergonomic workflow

1. Create the editable local manifest once:

   ```powershell
   .\scripts\league.ps1 -Action initialize
   ```

2. Edit `leagues\autowow-league.json` to choose roster-account names and captains. Keep four roster accounts per team for a 40-member target.
3. Validate it before provisioning anything:

   ```powershell
   .\scripts\league.ps1 -Action validate
   ```

4. Capture a live performance receipt at each test start and finish:

   ```powershell
   .\scripts\league.ps1 -Action snapshot -Label wsg-40-start
   .\scripts\league.ps1 -Action snapshot -Label wsg-40-finish
   ```

5. Only increase population when both manifest guardrails pass. `status` exposes the current online bot count, alive/dead/combat counts, map population, available memory, and worldserver memory:

   ```powershell
   .\scripts\league.ps1 -Action status
   ```

## Deliberately not automated yet

- Creating real player characters on behalf of roster accounts.
- Linking accounts without Playerbots' security-key confirmation.
- Forming raids, assigning squad roles, launching instances, or collecting boss-level telemetry through the AutoWow bridge.
- Awarding loot or gear. Those operations must remain explicit so the progression league stays fair.

The next bridge milestone is a constrained league API: roster inspection, explicit account-link requests, group/raid formation, named scenario start, order receipts, and benchmark summaries. It should never expose arbitrary SQL or account creation over the loopback bridge.
