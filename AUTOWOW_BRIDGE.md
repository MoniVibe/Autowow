# AutoWow bridge v0

The first AutoWow control surface lives inside the Playerbots module. It binds only to `127.0.0.1:18787` and never exposes MySQL, the game server, or bot controls beyond this machine.

Every request is parsed on a bridge thread, then queued through `PlayerbotWorldThreadProcessor`. Playerbot and world objects are accessed only by that world-thread operation. The request receives a JSON response after the operation completes or a two-second timeout elapses.

## Commands

The transport is one UTF-8 command line per TCP connection.

```text
list
destinations <bot-guid>
snapshot <bot-guid>
pause <bot-guid>
resume <bot-guid>
travel <bot-guid> <destination name>
quest <party-leader-guid>
recover <party-leader-guid>
observe <watch|relocate|status|protect|release> <observer-guid> [leader-guid]
```

`list` returns online random bots. `destinations` returns up to 25 valid Playerbots destinations for that bot. `snapshot` returns the bot's identity, position, vitals, current target, AI action, travel state, combat state, and AutoWow pause state. `travel` uses Playerbots' destination resolver rather than a client-side coordinate guess. Use `travel <bot-guid> random` to send a random bot to a level-appropriate Playerbots location when no named destination is available.

`quest` delegates a party leader's active quest to the native Playerbots RPG quest flow and enables the league no-teleport policy for that party. `recover` resets transient RPG/movement state and enables the same policy without editing quest progress. Both commands are intended for the external league director.

`observe` is a separate, default-closed camera surface. It accepts only GUIDs listed in `AutoWow.ObserverGuids`, refuses Playerbots-controlled characters, and requires the observer to be GM-invisible, ungrouped, and out of combat before relocation. It moves only the observer to the watched leader's live pose; it never joins the bot party.

Use the local test client:

```powershell
.\scripts\autowow-control.ps1 -Action list
.\scripts\autowow-control.ps1 -Action destinations -BotGuid 123
.\scripts\autowow-control.ps1 -Action pause -BotGuid 123
.\scripts\autowow-control.ps1 -Action resume -BotGuid 123
.\scripts\autowow-control.ps1 -Action travel -BotGuid 123 -Destination 'Stormwind'
.\scripts\autowow-control.ps1 -Action travel -BotGuid 123 -Destination random
```

## Safety boundaries

- The legacy Playerbots command socket is disabled in AutoWow configuration because it has no authentication. If explicitly enabled elsewhere, the source now binds it to loopback only.
- `pause` stops movement and interrupts a spell. A paused bot will not make AI decisions, including combat decisions, until resumed.
- An order times out rather than executing late if it remains queued longer than two seconds.
- The bridge deliberately has no arbitrary console command, SQL, filesystem, account, or network-control operation.

## Next autonomy milestones

1. Add an external agent loop that selects one bot, polls snapshots, and issues only the five v0 commands.
2. Add `follow`, `attack`, `loot`, and an explicit task/status model with order IDs and expiry.
3. Add a planner layer for natural-language goals; keep movement and combat execution deterministic and constrained by the bridge's allowlist.
