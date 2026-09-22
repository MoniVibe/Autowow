# Quest recovery v0

The league director has a conservative anti-stuck guard for persistent quest parties. It is intentionally a recovery system, not a substitute for quest logic.

## Trigger

For a party leader whose most recent native plan is an `objective` phase, the director observes two live signals:

- no position movement of at least 8 yards; and
- no XP gain.

Both must remain absent for 120 seconds by default. Combat defers the check. A quest/phase change creates a new observation window, and meaningful movement or XP clears the recovery-attempt counter.

## Escalation

1. The first two detections call the bridge `recover` order.
2. `recover` resets only temporary Playerbots travel/RPG/movement state for the party, disables random movement, and leaves followers in follow mode.
3. The next normal `quest` cycle uses Playerbots' standard questgiver, travel, and RPG-POI actions to make a fresh plan for the same character state.
4. If the same objective remains motionless and earns no XP after the configured recovery attempts, the director safely resets the party once more, pauses every member, and writes a `quest_manual_review_required` receipt. It never switches to random grinding or fabricates quest progress.

The bridge `recover` order itself never teleports a bot, edits character quest records, awards XP/gold/items, or picks a random destination.

The league now has an explicit no-teleport policy. A successful `quest` or `recover` order marks the current party members in an isolated bridge-side policy set. When New-RPG's separate 90-second no-distance-progress fallback is reached, league members hold position and report the failure to the external director instead of calling `TeleportTo(dest)`. Non-league Playerbots retain their historical fallback. The policy is cleared when a league bot is deactivated.

The source/build path is verified in the 2026-07-13 RelWithDebInfo build. A forced stuck-path runtime proof is still pending; the current live parties are making movement and XP progress, so the fallback has not been exercised in this run.

## Operation

The overnight defaults are deliberately cautious:

```powershell
& .\scripts\start-league-simulation-director.ps1 `
  -DurationMinutes 480 `
  -ScoutEverySeconds 20 `
  -NoProgressSeconds 120 `
  -MaxRecoveriesPerQuest 2
```

The parameter name `ScoutEverySeconds` is legacy; the director performs quest cycles only. Inspect the JSONL file in `leagues\results` for `quest_recovery_issued`, `quest_hold_deferred`, or `quest_manual_review_required` events.

To run a controlled native replan for an idle party leader:

```powershell
& .\scripts\autowow-control.ps1 -Action recover -BotGuid 10
& .\scripts\autowow-control.ps1 -Action quest -BotGuid 10
```

## Live proof

On 2026-07-13, Northstar leader Brandreas (guid 10) was idle and out of combat. The bridge returned `reset_members: 5`, followed by a standard Playerbots `quest` response for real quest `4495` in `turnin` phase with destination `playerbots_rpg_poi`. The worldserver remained available afterward and the director restarted with a clean receipt stream.

This proves the reset/replan path. It does not claim that every special quest action type is automated, or that a forced stuck-path runtime proof has been captured; both still need capability-specific handling and proof.
