# AutoWoW Dungeon and Raid Progress

Date: 2026-07-15

## Live Utgarde Keep result

Status: **PASS — full normal-mode clear with five bots alive**

Roster:

- GUID 2 Nathalis — protection paladin, main tank
- GUID 26 Baeil — holy priest, healer
- GUID 8 Teleane — fire mage, DPS
- GUID 45 Xahran — combat rogue, DPS
- GUID 24 Usta — beast-mastery hunter, DPS

All characters were initialized at level 80 with quality-3 fixture gear. The party was staged to `uk-exterior` before `uk-entry`; this is required so all five enter one shared instance. Directly routing a formed group from separate locations to `uk-entry` can allocate separate instance IDs.

The strict route director completed all 23 waypoints with normal per-member mmap pathing:

```text
passed:              true
waypoints_completed: 23
orders_issued:       60
combat_polls:        34
alive:               5
instance_id:         1
```

Receipt: `logs/phase1-20260714/dungeon-director-20260714-144005.jsonl`

At Ingvar's platform, live combat telemetry observed:

- first-phase Ingvar at 0.98% health while targeting Nathalis;
- second-phase Ingvar restored to 65.08% health, still targeting Nathalis;
- all five players alive after combat ended;
- the last DPS spells targeted runtime creature GUID 60 (Ingvar).

Read-only character-database confirmation after combat:

```sql
SELECT * FROM instance WHERE id = 1;
SELECT guid, instance, permanent
FROM character_instance
WHERE instance = 1
ORDER BY guid;
```

Result:

```text
instance: id=1 map=574 difficulty=0 completedEncounters=7
bindings: 2, 8, 24, 26, 45 -> instance 1
```

`completedEncounters = 7` is the three-bit completed mask for the three Utgarde Keep encounters. This confirms Keleseth, Skarvald/Dalronn, and both phases of Ingvar completed in one shared instance.

## What this proves

- The current fixture gear is sufficient for this five-bot UK normal clear.
- The protection paladin held the active hostile targets observed during the run.
- The holy priest kept the party alive through 34 combat polls and the full Ingvar fight.
- Per-member dungeon movement now traverses the stacked floors and strict upper route without stranding the rogue or hunter.
- Dungeon completion is proven by server persistence, not inferred only from movement or XP.

## Current Onyxia evidence

The proof runtime now links the encounter scripts, supports exact creature-entry engagement, and requires both exact target death and the authoritative Onyxia encounter bit for PASS. The fixed ten-bot roster has completed three honest attempts so far; no attempt is recorded as a kill.

The best pre-triage attempt reached phase three and wiped with Onyxia at 38.12% health after 176.9 seconds. A later cohesive-shelter and normal-interrupt attempt wiped at 50.38% after 214 seconds. The current comparison run used threat-backed generic add assignments and produced:

```text
status:                    FAIL — full wipe
duration:                  211.03 seconds
lowest Onyxia health:      54.9042%
exact boss damage:         45.0958%
tank boss ownership:       7/7 samples (100%)
all-hostile tank ownership: 88.4%
healer tank priority:      62.65%
non-tank triage:           83.33%
movement cohesion:         100%
encounter completion bit:  0 (correctly FAIL)
```

Receipt: `logs/phase1-20260715/raid-readiness-onyxia10-add-assignments.jsonl`

The run improved early air-phase survivability and tank threat, but three simultaneous Lair Guards exposed a general coordination failure: all three successful normal class interrupts targeted one guard while the other casting guards remained uncovered. The same guard was reacquired 112 times, so the 1.5-second target lease also caused needless churn.

A subsequent generic change assigned distinct currently-casting adds to distinct interrupt-capable non-healers, filtered eligibility to spells actually known by the player or active pet, and extended the cooperative target lease to five seconds. Its focused coordination suite passed 26/26 and the full registered CTest target passed 1/1. The exact live comparison still failed:

```text
status:                    FAIL — full wipe
duration:                  170.68 seconds
lowest Onyxia health:      55.2873%
exact boss damage:         44.7127%
tank boss ownership:       8/8 samples (100%)
all-hostile tank ownership: 91.25%
healer tank priority:      57.41%
non-tank triage:           71.43%
movement cohesion:         100%
successful assigned interrupts: 1
encounter completion bit:  0 (correctly FAIL)
```

Receipt: `logs/phase1-20260715/raid-readiness-onyxia10-distributed-interrupt.jsonl`

This confirms that target ownership, healing priority, and cohesive movement are no longer the primary failure. The remaining Onyxia blocker is the air-phase add-damage/cast-control envelope: only one observed guard cast was successfully interrupted before attrition cascaded.

The acceptance gate remains unchanged: boss death, authoritative encounter `DONE`, exact shared roster credit, adequate tank/healer behavior, bounded attrition, and no encounter-time teleport.

## Vault of Archavon comparison

Status: **PASS — two different 10-player raid bosses killed with the exact roster and no deaths**

The same fixed role-complete roster entered Vault of Archavon through exterior area trigger 5258, one
member per world tick, and shared map 624 instance 2. Interior staging used ordinary mmap-backed
movement. No character was teleported into the instance or during either boss encounter.

Roster:

- Tanks: GUID 2 Nathalis, GUID 72 Marinor
- Healers: GUID 26 Baeil, GUID 68 Urohke, GUID 41 Liguhku
- DPS: GUID 8 Teleane, GUID 24 Usta, GUID 45 Xahran, GUID 60 Mutel, GUID 83 Bemarnin

Archavon the Stone Watcher (encounter 772, bit 0):

```text
status:                       PASS (authoritative encounter credit)
duration:                     54.04 seconds
deaths:                       0
tank boss ownership:          100%
tank all-hostile ownership:   95.0%
healer tank priority:         87.8%
non-tank triage:              100%
movement cohesion:            100%
completedEncounters after:    1
```

Receipt: `logs/phase1-20260715/raid-readiness-archavon10.jsonl`

The original Archavon wrapper summary says `FAIL` only because its two-second polling missed the single
dead-target frame. The read-only completion gate independently proves target entry 31125 mapped to
encounter 772, bit 0 was set, and exactly the ten expected characters were bound to the completed
instance. The reducer has since been corrected to accept that exact authoritative death evidence.

Emalon the Storm Watcher (encounter 774, bit 1):

```text
status:                       PASS
duration:                     154.39 seconds
deaths:                       0
tank boss ownership:          100%
tank all-hostile ownership:   95.57%
healer tank priority:         77.98%
non-tank triage:              50.0% (threshold 50.0%)
movement cohesion:            100%
maximum member step:          25.565 yards (no teleport event)
completedEncounters after:    3 (bits 0 and 1 set)
```

Receipt: `logs/phase1-20260715/raid-readiness-emalon10.jsonl`

Emalon also exercised general add ownership: while ranged damage continued on the boss, tanks and
melee switched among Tempest Minions. The boss died, encounter bit 1 was set, and all ten exact roster
bindings persisted. This establishes that the generic tank/heal/triage/claim architecture can clear
mechanically different raid bosses; Onyxia remains a narrower air-phase coordination problem rather
than evidence that the entire raid brain is nonfunctional.

## Related live proofs

- WSG 10v10: 57 kills, 57 deaths, 1,838,000 damage, 663,728 healing, ten flag-state changes, two captures, and two returns.
- Gathering: GUID 25 persisted 19 gathered material units with no direct inventory database writes (`logs/gathering-proof-20260714-141803.json`).
- Escort quest q435: completed and rewarded through the native escort path (`logs/phase1-20260714/q435-escort-live.jsonl`).
- Direct GameObject interaction: q917 completed and rewarded through the native interaction path (`logs/phase1-20260714/q917-gameobject-reward-live.jsonl`).
- Item-on-creature interaction: q5441 completed and rewarded through native item targeting and five observed item-use packets (`logs/phase1-20260714/q5441-item-use-live-v2.jsonl`).
