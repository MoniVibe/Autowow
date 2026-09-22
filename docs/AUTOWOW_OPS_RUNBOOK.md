# AutoWoW operator console

`scripts\autowow-ops.ps1` is the front door for persistent questers and repeatable raid probes.
It composes the specialist scripts and keeps their detailed receipts; it does not replace them.

## Safety

- `Plan` is the default and does not contact the bridge or mutate runtime state.
- `Status`, `Dashboard`, `QuestersStatus`, and `FailureBundle` are read-only runtime actions.
- `Raiders`, `QuestersStart`, `QuestersStop`, and `DevCycle` require `-Apply`.
- The quest director has one canonical PID/state file, so a second background director is refused.
- Quest abandonment detection reads the active WSL log at `logs\phase1-runtime\Playerbots.log`.
- Starting questers activates and forms Northstar and Ember but preserves unrelated live raid and
  dungeon rosters; it does not park the current Probe Lab fleet.

## Common commands

```powershell
# Safe orientation
.\scripts\autowow-ops.ps1
.\scripts\autowow-ops.ps1 -Action Status
.\scripts\autowow-ops.ps1 -Action Dashboard

# Persistent questers (all discoverable league leaders, or supply -LeaderGuid 7,10)
.\scripts\autowow-ops.ps1 -Action QuestersStart -Apply -DurationMinutes 480
.\scripts\autowow-ops.ps1 -Action QuestersStatus
.\scripts\autowow-ops.ps1 -Action QuestersStop -Apply

# Exact concurrent dungeon/raid probes
.\scripts\autowow-ops.ps1 -Action Raiders -Apply -RaidMonitorSeconds 600

# Guarded source sync, focused tests, build, restart, and receipt
.\scripts\autowow-ops.ps1 -Action DevCycle -Apply
```

The dashboard and failure-bundle tools are deliberately read-only. A failure report is evidence,
not an automatic recovery order; fixes remain explicit and testable.
