# AutoWow overnight automation report

Generated: 2026-07-12

## Completed before the soak

| Validation | Result |
|---|---|
| Server endpoints | Running on loopback only: auth `3724`, world `8085`, bridge `18787` |
| Legacy unauthenticated Playerbots command port | Disabled (`8888` is not listening) |
| Bridge pause/resume control test | PASS |
| Bridge random-travel control test | PASS; observed bot position changed |
| Background agent start/stop safety test | PASS; receipts recorded and the selected bot was not left paused |
| PowerShell syntax checks | PASS for new runner, controller, provisioning, and account scripts |

The current reproducible bridge proof is in `AUTOWOW_BRIDGE_TEST_REPORT.md`. The background-agent start/stop proof is in the timestamped JSONL file under `logs`.

## Overnight workload

The scheduled local-only agent runs for eight hours with a 20-second snapshot cadence and a 20-minute travel interval. It:

1. picks a living, idle random bot;
2. verifies pause and resume once after selection;
3. records position, life, combat, pause, and travel state as JSONL;
4. sends only `travel random` while that bot is alive, idle, and unpaused;
5. aborts after three consecutive bridge failures; and
6. resumes the bot in its cleanup path if a failure occurs while the probe has it paused.

It never opens network ports, modifies firewall rules, creates accounts, changes server configuration, or accesses the database.

Launch evidence: the eight-hour agent started at `2026-07-12 20:04:58 +03:00`, selected `Nathalis` (guid `2`), completed its pause/resume probe, and remained healthy through subsequent snapshots. Its live receipt stream is `logs\autowow-agent-20260712-200458.jsonl`.

## Intentionally deferred

- LAN/public server exposure: requires an explicit network and firewall decision.
- Player-character creation and live follow/fight/loot verification: requires an interactive 3.3.5a client session.
- Direct `follow`, `attack`, and `loot` bridge commands: not implemented; only tested bridge operations are list, snapshot, pause, resume, and controlled random travel.
- Fresh database smoke query in this PowerShell process: deferred because its root-password secret is not present. The existing smoke report remains the last database proof.
