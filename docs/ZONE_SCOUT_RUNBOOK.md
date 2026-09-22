# Zone scout observer

zone-scout-monitor.ps1 is a read-only observer for a fixed roster of persistent bots. It queries autowow-control.ps1 with list, questobjective, and questlog; it never activates, moves, heals, or resets a bot. Keep a separate manifest for the current scout roster:

~~~json
{
  "schema": "autowow.zone-scout.manifest.v1",
  "scouts": [
    { "guid": 101, "name": "Zathis", "home_zone": "Teldrassil" },
    { "guid": 112, "name": "Naomini", "home_zone": "Elwynn Forest" },
    { "guid": 123, "name": "Aenstus", "home_zone": "Eversong Woods" }
  ]
}
~~~

Run the six-scout fixture in a terminal that can remain open:

~~~powershell
& 'D:\Games\wowstuff\AutoWoW\scripts\zone-scout-monitor.ps1' -ManifestPath 'D:\Games\wowstuff\AutoWoW\scripts\fixtures\zone-scouts-20260922.json' -DurationMinutes 120 -PollSeconds 5 -StallSeconds 180 -QuestNoCreditSeconds 600
~~~

DurationMinutes is 1–480. The monitor holds one exclusive file lock at logs\zone-scouts\monitor.lock; a second monitor refuses to start. A terminated process releases the file lock. Each run writes a unique logs\zone-scouts\<run-id> directory containing samples.jsonl, incidents.jsonl, summary.json, and summary.md. logs\zone-scouts\latest.json is atomically replaced and points to the current run with compact status, incident counts, and per-scout state for 15-minute heartbeat review. Older runs are never overwritten.

SUSPECT marks a bounded evidence pattern. It is not an automatic fix or a declaration that every quest is broken. RECOVERED requires XP/level or native objective credit/reward; for quest_no_credit, only native credit/reward can recover it. Changing quests ends that quest context as CONTEXT_CHANGED with outcome unknown. A process session change emits SESSION_RESET and starts a new baseline, not a recovery. OBSERVING means only that no active suspicion met its threshold. Inspect the incident's first failure context and rolling breadcrumbs before changing the game. OFFLINE and DEAD are separate conditions; bridge query failure is recorded as a telemetry error rather than falsely marking a bot offline.

The home zone is only the operator's label. Observed map, zone, area, instance, and movement-speed values are copied from bridge telemetry when present; absent fields remain null. summary.md never treats the home label as an observed zone.

General inactivity considers XP/level, objective credit, and confirmed reward; receipt sequence, money, target, phase, and quest churn are diagnostic only. A same-quest quest_no_credit timer is independent of XP so repeated kills with no quest credit remain visible. Stationary and short oscillating paths are distinguished from productive travel. Combat, casting, eating, and health recovery get at most a 60-second grace; a long respawn wait or repeated failure still becomes suspect. The monitor does not interpret its own successful sampling as player progress.


A one-minute read-only smoke on 2026-09-22 observed existing GUIDs 101, 112, and 123 for 30 samples with no telemetry errors; the run is logs\zone-scouts\20260922-103425-0c325dd1. This validates transport and record publishing only; it does not certify any zone or quest.


For the persistent background session, use scripts\zone-scout-session.ps1 with -Action Start, Status, or Stop. The wrapper owns only the observer process; stopping it leaves bots and worldserver running.

