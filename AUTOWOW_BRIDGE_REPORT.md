# AutoWow bridge v0 report

Generated: 2026-07-12

## Delivered control surface

- Local bridge: `127.0.0.1:18787`, owned by `worldserver.exe`.
- Legacy Playerbots command server: disabled (`AiPlayerbot.CommandServerPort = 0`).
- Commands: `list`, `destinations <guid>`, `snapshot <guid>`, `pause <guid>`, `resume <guid>`, and `travel <guid> <destination>`.
- All game and Playerbots access is deferred from the bridge listener to `PlayerbotWorldThreadProcessor` and executes on the world thread.

## Live proof

Nathalis (GUID `2`) was exercised against the running local server:

1. Snapshot returned `paused: false` at Northrend coordinates `(5633.14, 699.496, 652.042)`.
2. `pause 2` succeeded; the following snapshot returned `paused: true`.
3. `resume 2` succeeded.
4. `travel 2 random` succeeded in `level_appropriate` mode.
5. The subsequent snapshot showed Nathalis at `(8429.82, -351.927, 906.452)`, and Playerbots logged the level-appropriate teleport to Bouldercrag's Refuge.

## Validation

- C++ bridge and `worldserver` compiled and linked with Visual Studio 2022 x64 RelWithDebInfo.
- Installed server started successfully.
- `scripts\smoke-test.ps1` passed with auth port `3724`, world port `8085`, bridge port `18787`, all four schemas, Playerbots tables, and legacy port `8888` disabled.
- `scripts\autowow-control.ps1 -Action list` returned structured snapshots for the five live random bots.

## Current boundary

Named Playerbots travel destinations are resolver-backed and intentionally reject unknown names. The current random-bot population had no eligible named destinations, so `destinations <guid>` returned an empty list. `travel <guid> random` is the tested, working fallback and uses Playerbots' native level-appropriate travel logic.

## Next implementation slice

Build the external agent loop: select a bot, poll `snapshot`, use `destinations` when available, fall back to `travel random`, and apply only the bridge allowlist. Add `follow`, `attack`, and `loot` after that loop has a deterministic receipt and timeout model.
