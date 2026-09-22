# AutoWoW local Playerbots runbook

This is a localhost-only AzerothCore Playerbots server for a clean WoW 3.3.5a build 12340 client.

## First-time order

1. Finish downloading the client and identify its directory.
2. Confirm the client executable reports build `12340`:

   ```powershell
   Get-Item 'D:\path\to\client\Wow.exe' | Select-Object -ExpandProperty VersionInfo
   ```

3. Stage dependencies. Keep the MySQL root password out of files and Git.
4. Run `scripts\build.ps1`.
5. Run `scripts\configure-server.ps1`.
6. Set the password only in the current PowerShell session and start the bundled MySQL instance:

   ```powershell
   $env:MYSQL_ROOT_PASSWORD = '<local MySQL root password>'
   & .\scripts\start-mysql.ps1
   ```

7. Initialize databases and apply core/module updates:

   ```powershell
   & .\scripts\database.ps1
   ```

8. Extract client data into the server data directory:

   ```powershell
   & .\scripts\extract-client-data.ps1 -WowClientDir 'D:\path\to\client'
   ```

9. Make a separate client copy and set its local realm list. This leaves the downloaded/original client untouched:

   ```powershell
   & .\scripts\prepare-client-copy.ps1 -WowClientDir 'D:\path\to\client'
   ```

10. Create one local administrator account using process-only secrets:

   ```powershell
   $env:WOW_ACCOUNT_NAME = 'AUTOWOW'
   $env:WOW_ACCOUNT_PASSWORD = '<local account password>'
   & .\scripts\create-account.ps1
   ```

11. Start the local servers and run the mechanical smoke test:

   ```powershell
   & .\scripts\start-server.ps1
   & .\scripts\smoke-test.ps1
   ```

12. Log in using the prepared client copy, create a character, invite an altbot, and manually verify follow, fight, loot, chat command, restart, and persistence behavior.

## Local automation, quest campaign, and overnight soak

The bridge is localhost-only and has an intentionally narrow command set. Run its bounded control test after the server is ready:

```powershell
& .\scripts\autowow-bridge-test.ps1
```

The legacy supervised random-travel soak remains useful only as a bridge diagnostic; it is not campaign progression and should not be used to measure leveling behavior:

```powershell
& .\scripts\start-autowow-agent.ps1 -DurationMinutes 480 -PollSeconds 20 -TravelEveryMinutes 20
```

The runner has no account, SQL, filesystem, or arbitrary command capability. Stop it safely with:

```powershell
& .\scripts\stop-autowow-agent.ps1
```

The live process record is `autowow-agent.pid.json`; its JSONL receipts and stdout/stderr logs are under `logs`. See `AUTOWOW_BRIDGE.md` for the command contract and `MULTIPLAYER_RUNBOOK.md` for separate-player provisioning and the intentionally deferred LAN decision.

For the persistent league quest campaign, first launch/resume the two parties, then start the quest-only director:

```powershell
& .\scripts\league-simulation.ps1 -Action launch
& .\scripts\start-league-simulation-director.ps1 -DurationMinutes 480 -ScoutEverySeconds 20 -NoProgressSeconds 120 -MaxRecoveriesPerQuest 2
```

The parameter name `ScoutEverySeconds` is retained for compatibility, but the director now issues only `quest` cycles. It records leader quest ID, phase, destination, and interaction attempts in `acore_playerbots.autowow_league_quest_state`, and writes JSONL receipts beneath `leagues\results`. It does not enable `move random` or generic `grind` when a party has no live quest work. Its anti-stuck guard requires both no 8-yard movement and no XP gain for 120 seconds, issues at most two native Playerbots replans per objective, then pauses and reports the party for manual review. See `QUEST_RECOVERY_V0.md` for the exact recovery contract.

To inspect persisted campaign evidence without exposing credentials, use the existing read-only reports `LEAGUE_V0_REPORT.md` and `QUESTING_PROOF_REPORT.md`. The neutral worker remains intentionally offline until the economic-worker loop is implemented.

## Server endpoints

- Auth/realm: `127.0.0.1:3724`
- World: `127.0.0.1:8085`
- MySQL: `127.0.0.1:3306`
- DataDir: `server\data`
- Configs: `server\configs`
- Server logs: `server\logs`
- Automation logs: `logs`

## Conservative first-run Playerbots settings

The configuration script enables Playerbots and random-bot autologin but limits the first run to five random bots and one add-class account pool. Increase those values only after the first smoke test is clean.

## Stop and restart

```powershell
& .\scripts\stop-server.ps1
& .\scripts\stop-mysql.ps1
& .\scripts\start-mysql.ps1
& .\scripts\start-server.ps1
& .\scripts\smoke-test.ps1 -RequireBots
```

## Safety rules

- Do not run `git pull` or update the pinned source repositories during this bootstrap.
- Do not expose ports beyond localhost.
- Do not place `MYSQL_ROOT_PASSWORD` in a script, report, command transcript, or committed config.
- Do not run extraction directly from the original client directory.
