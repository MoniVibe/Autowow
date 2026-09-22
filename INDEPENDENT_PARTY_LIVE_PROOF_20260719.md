# AutoWow Independent Party Live Proof — 2026-07-19

## Result

The two persistent five-bot parties now retain group membership without a permanent
master/follower control chain. Party leaders remain roster metadata (`group_leader`); they
are not used as the bots' movement or quest controller.

The independent native loop is:

```text
+grind,+new rpg,-follow,-move random,-travel
```

Explicit quest participants also retain their own objective, travel, combat, loot, and reward
executors. The existing cohesion policy may create a temporary follow preference when a member
is genuinely separated, but native group maintenance cannot recreate permanent follow after a
restart. Leaving a group clears the AutoWow-only mode, preserving ordinary Playerbots behavior.

## Implementation

- `work/oracle-integration-r13-r6-r5/src/Bot/PlayerbotAI.{h,cpp}`
  adds the default-off `autoWowIndependentParty` guard around native group maintenance.
- `work/oracle-integration-r13-r6-r5/src/AutoWow/AutoWowBridge.cpp`
  re-arms independence at every quest dispatch (including resumed persisted groups) and keeps
  explicit quest participants independent.
- `scripts/league-simulation.ps1`
  performs a bounded dispatch/read/retry check after restart; it fails after three attempts if a
  leader still exposes `follow`.

## Verification

- Full core/module build: `[582/582]`, exit code 0.
  Log: `logs/build/independence-core-group-guard-build-20260719.log`
- Focused C++ contracts: **96/96**.
  Log: `logs/build/independence-rearm-tests-20260719.log`
- Operator/launcher PowerShell contracts: **6/6**, including the bounded re-arm check.
  Log: `logs/build/independence-launcher-tests-20260719.log`
- Final live sequence: clean worldserver restart, persisted-party launch, bounded independence
  re-arm, then a 35-second read-only soak.
- Final live state: **0/10 follow**, **10/10 new-rpg**, **10/10 grind**, two intact groups of five,
  8/10 bots moved, and aggregate XP delta `+270` during the soak.
- Worldserver binary SHA-256:
  `2d1737fbd756b287d5336bdfd736ad460b0136e8f2877594bca3b878fa26378f`

This proves independent execution and live activity, not quest completion. Quest acquisition and
objective/reward progress remain separate acceptance gates.
