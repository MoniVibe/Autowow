# AutoWoW Probe Rotation Policy

The probe lab is a mechanic-coverage campaign, not a soak loop. A fixture is run only when it can produce new evidence.

## Promotion rules

| Result | State | Next action |
| --- | --- | --- |
| First run passes | RETIRED | Record the capability and promote the roster to a different instance. |
| First run exposes a new failure | QUARANTINED | Preserve one failure bundle and continue with another eligible instance. |
| A code/config fix changes the relevant build or fix ID | RETEST | Permit exactly one targeted regression run. |
| Retest passes | RETIRED | Promote immediately; do not soak the same fixture. |
| Retest repeats the same failure | QUARANTINED | Stop rerunning it. Diagnose and fix from the captured evidence offline. |

Runtime health monitoring is separate from capability probing. A completed fixture may still have read-only health checks, but those checks must not reset or replay the encounter.

## Current gate (ca44)

| Fixture | Evidence | State | Reason |
| --- | --- | --- | --- |
| Drak'Tharon Keep | `ca44-dtk-party-20260717-150321` | RETIRED | The post-fix regression run passed with all five alive. |
| Nexus | `ca44-nexus-party-20260717-150504` | QUARANTINED | The post-fix run failed `no_progress` with zero leader movement. |
| Onyxia | `ca44-onyxia-raid-20260717-150321` | QUARANTINED | The post-fix run repeated a full wipe. |

## Next breadth

1. Utgarde Keep full clear: runnable now; five-player traversal, gates, gauntlet, multi-boss progression, and ordinary loot.
2. Vault of Archavon, Emalon: runnable now; raid traversal, nova avoidance, overcharge target priority, and tank/healer pressure different from Onyxia. Archavon is already completed evidence and is not the target.
3. Halls of Lightning: next generic route/portal adapter; Ionar dispersion and Loken nova positioning.
4. Violet Hold: next scripted-wave and interaction adapter.
5. Obsidian Sanctum, Sartharion 10: next fresh raid after the generic adapter exists.
6. Oculus: later vehicle and aerial-traversal stress test.

Only the first two are admitted to the current runtime rotation. The remaining candidates require one reusable exterior/portal/ordinary-navigation adapter, not encounter-specific teleports or scripts.

Progress is measured by unique mechanics and durable outcomes: unique bosses killed, full instances completed, loot/equip evidence, interaction types completed, and failures converted into isolated regression fixtures.
