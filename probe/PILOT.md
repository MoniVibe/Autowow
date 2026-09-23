# Quest probe: design, pilot, consumption proposal

Status: **PILOT BLOCKED ON FIXTURE AVAILABILITY (2026-09-23 05:06 UTC)**. First written as a checkpoint at 04:35 UTC. The pilot was stopped for the orchestrator restart onto
module main 286b5511. Pilot results so far ran on the **old binary** (`/root/autowow-advisor-t1-build`,
module `autowow-advisor-t1-module` 7d09ed77, live run `soak-s5-cohort-r2`). Remaining quests resume on
the new build and will be labelled `soak-s6-cohort-r1`.

## 1. Probe runner design (`probe/probe-quest.ps1`)

PowerShell 5.1, ASCII (parse-checked on 5.1.26100). One bot, one quest, bounded time.

- **Observe** mode: read-only (`fixture-status`, `snapshot`, `questlog`, `questobjective`).
- **Probe** mode: Observe plus recorded SETUP on a fixture bot only:
  `fixture-init <questLevel>` when the bot is more than 3 levels below the quest (server refuses a
  downlevel), and `travel <Destination>` when the quest is not yet in the log and a giver destination
  title is supplied. The bot's normal NewRpg play does the accept, progress and turn-in. The probe never
  grants, completes or rewards anything.
- **Safety gates** (checked before any mutation): the bot is in the live `AutoWow.FixtureGuids`, is not in
  `AutoWow.OracleRuntime.BotGuids`, the cohort (62955-63004) or the scouts. Both keys are read with a
  single-key `grep` and no other config line is read. The bot is online, the faction fits, and the bot
  level passes the NewRpg accept window (`IsQuestWorthDoing`: level <= questLevel + 4), unless the quest
  is already in the log.
- **Phase timeline** (added on coordinator amendment): a sample every 10 s records position, the NewRpg
  directive quest, oracle phase and failure reason, objective count, selected source spawn
  (distance/spawned/in-world), selected target (loaded/alive/distance) and stuck attempts. Phases are
  recorded with entered/exited seconds. The result names the failure **stage** (the stage with the longest
  dwell): select, accept, travel_to_source, find_target, interact_credit, loot, travel_to_finisher or
  turn_in. It also names the pinning evidence: `fixation:directive_<q>` when another quest owns the
  directive, `travel_no_progress(observed a->b yd)` when the travel distance does not shrink, or
  `source_spawned=..,target_alive=..`. The runner consumes ledger `progress` events
  (286b5511) as progress evidence.
- **Verdicts:** REWARDED, REWARDED_CONTAMINATED (teleport/revive contamination during the window),
  COMPLETE_NOT_TURNED_IN, PROGRESSING, STALLED, NOT_ACCEPTED, BLOCKED_PREFLIGHT. Each run writes one JSON
  receipt to `probe/runs/`. The runner reads the live ledger only from the byte offset where the probe
  started, filtered to the bot and quest.

## 2. Fixture availability (the pilot's binding constraint)

Live `AutoWow.FixtureGuids` has 55 GUIDs. Removing 25 (also in the Oracle allowlist) leaves 54 eligible.
Of those, `fixture-status` (read-only, 04:2x UTC) found:

| state | bots | usable for levels 1-15? |
|---|---:|---|
| online, level 80 (8, 24, 26, 41, 45, 60, 68, 83) | 8 | no: `fixture_init` refuses a downlevel, and NewRpg hides quests below level 76 |
| online, level 14 (36, Horde undead priest, Silverpine) | 1 | yes, for quests level >= 10 (Horde/Both) |
| offline (2, 21, 72, 16472-16491, 17392-17411, 19032, 19033) | 45 | no: `activate` requires league enrollment (DB), which is out of scope |
| **total** | **54** | **1** |

Placement gap: `travel` needs a TravelMgr destination title, and `destinations 36` returned an empty
list. `quest` and `quest-acquire` require a league party leader. **There is no verb that places a
non-league fixture bot at an arbitrary quest giver, and none that pins a specific quest.** Needed for a
real probe:
(a) online, low-level fixture bots (league-enroll them, or add a fixture-scoped login verb);
(b) a fixture-scoped `travel-giver <guid> <questId>` that resolves the starter spawn and walks there (or
teleports as recorded setup);
(c) optional: a fixture-only quest-priority lease for the probed quest (the micro-scenario
"priority_policy" already allows this).

Bots 36, 8, 45 and 68 are also random bots that play in the soaks. Probe windows on them share the
ledger with the soak. Their rows stay per-bot, and the probe records contamination.

## 3. Pilot results so far (old binary, run `soak-s5-cohort-r2`, bot 36)

Pilot list (10, in order): 354, 356, 358, 427, 428, 477, 447, 422, 437, 369. Budget: 5 min each, 30 s
polls (these ran before the timeline amendment).

| quest | registry verdict | family | result | detail |
|---:|---|---|---|---|
| 354 Deaths in the Family | FAILING (travel_to_source:oracle_route_blocked) | COLLECT_DROP | STALLED | In log at baseline, counters 0/0/0 held for 5 min. No ledger event for the quest; the directive was elsewhere. Receipt `runs/q354-b36-20260923T043127Z.json` |
| 356 Rear Guard Patrol | FAILING | KILL | BLOCKED_PREFLIGHT | Not in log and `no_giver_destination_supplied`. The placement gap (section 2) blocks it. |
| 358 Graverobbers | FAILING | COLLECT_DROP | INCOMPLETE_RESTART | Started 04:31:28 and stopped at 04:34:5x for the restart. Earlier 1-min smoke observation: COMPLETE_NOT_TURNED_IN (8/8, 5/5, 8/8 done), and the bot did not turn it in. |
| 427 At War With The Scarlet Crusade | FAILING | KILL | INCOMPLETE_RESTART | not started |
| 428 Lost Deathstalkers | UNTESTED | TALK_ONLY | INCOMPLETE_RESTART | not started (complete in the bot's log at baseline) |
| 477 Border Crossings | UNTESTED | TALK_ONLY | INCOMPLETE_RESTART | not started (complete in log) |
| 447 A Recipe For Death | UNTESTED | COLLECT_DROP | INCOMPLETE_RESTART | not started (6/6, 0/6) |
| 422 Arugal's Folly | UNTESTED | GAMEOBJECT_COLLECT | INCOMPLETE_RESTART | not started |
| 437 The Dead Fields | UNTESTED | COLLECT_DROP | INCOMPLETE_RESTART | not started. At 04:3x the live `questobjective` showed 437 as the bot's directive (travel_to_source, source 1983 corpse lootable). |
| 369 A New Plague | UNTESTED | COLLECT_DROP | INCOMPLETE_RESTART | not started |

Sums: 10 quests = STALLED 1 + BLOCKED_PREFLIGHT 1 + INCOMPLETE_RESTART 8.

Gate self-checks (receipts in `runs/`): bot 8 on q354 gave `bot_level_80_hides_quest_level_11`; cohort
bot 62960 gave `bot_is_cohort_or_scout`; Oracle bot 7 gave `bot_not_in_AutoWow.FixtureGuids`; q99999
gave `quest_not_in_census`; bot 36 on q808 gave `bot_level_14_hides_quest_level_9`.

Early read (one bot, no cross-bot claim): bot 36 holds 20 quests, 3 of them complete but not turned in
(358, 428, 477), while its directive sat on 437. This fits the registry's `select:fixation_starved`
signature: one directive works while complete quests wait.

**Resume on the new build (run `soak-s6-cohort-r1`, binary e2cedbc1 / module 72edb12b):** attempted
04:56-05:06 UTC. Only 5 fixture bots were online (2, 41, 45, 68, 83), all level 80. Bot 36 stayed
offline for 30 minutes of polling (every 30 s). Receipts, labelled `run_label: soak-s6-cohort-r1`:
`runs/q358-b36-20260923T045607Z.json` = BLOCKED_PREFLIGHT `fixture_status:fixture_target_not_online`,
and `runs/q358-b2-20260923T045609Z.json` = BLOCKED_PREFLIGHT `bot_level_80_hides_quest_level_8`.
"Re-init" cannot help: `fixture init` only raises level (server `fixture_downlevel_reuse_refused`).
The 8 remaining quests therefore stay **INCOMPLETE_RESTART / BLOCKED (no eligible fixture bot)**.
To resume: `powershell -File probeun-pilot.ps1 -Quests 358,427,428,477,447,422,437,369 -Bot <guid> -RunLabel soak-s6-cohort-r1`
once a fixture bot at level <= 14 (Horde, for this list) is online. This needs section 2 (a).

Original resume plan on the new build: after the bridge is ready, rerun `fixture-status`. If bot 36 (now level 15)
is online, run the 8 remaining quests with the timeline runner and label the receipts
`soak-s6-cohort-r1`. If no low-level fixture bot is online, the pilot stays blocked on section 2 (a).

## 4. Consumption proposal (Phase 3)

Where quests are chosen (running module `autowow-advisor-t1-module`, file:line):

- Accept at a giver: `src/Ai/World/Rpg/Action/NewRpgBaseAction.cpp:1009-1012` (`InteractWithNpcOrGameObjectForQuest`). Giver scoring is at `:1499-1500` (`HasQuestToAcceptOrReward`). Both gate on `IsQuestWorthDoing` (`:1243`) and `IsQuestCapableDoing` (`:1260`).
- Log pruning: `OrganizeQuestLog` `:1277-1310` drops quests that fail the same two predicates when fewer than 2 slots are free (ledger `deferred drop_unworthy_or_failed`).
- Per-bot soft skip already exists: `PlayerbotAI::lowPriorityQuest` (`src/Bot/PlayerbotAI.h:652`). It is skipped by RPG_DO_QUEST selection (`NewRpgBaseAction.cpp:1865`, `:1958`) and by the Oracle selector (`src/AutoWow/AutoWowOracleRuntime.cpp:1170`). It is filled at runtime only, on unrunnable or deferred quests (`NewRpgAction.cpp:743,781`, `AutoWowOracleRuntime.cpp:984,1110`).
- There is no config blacklist. `AiPlayerbot.RandomBotQuestIds` (`PlayerbotAIConfig.cpp:205`) is a factory grant list, not an avoid list.

Proposal (flags OFF first, and a one-guard root-cause placement):

1. `AutoWow.QuestAvoidIds = ""` (comma list, parsed once into an `unordered_set` in PlayerbotAIConfig;
   reuse `AutoWowRandomBotPolicy::GuidListContains`-style parsing). One guard at the top of
   `IsQuestCapableDoing`: `if (avoid.count(quest->GetQuestId())) return false;`. That one predicate
   covers accept (1010), giver scoring (1500) and log pruning (1301), so avoided quests are never taken
   and are dropped first when the log is full.
2. `AutoWow.QuestLowPriorityIds = ""` (FAILING, non-structural: route/respawn/fixation): seed
   `lowPriorityQuest` at AI init, so bots still take them but do not pick them as a directive while other
   work exists. This is a soft tier, and a fix that lands removes the quest from the list.
3. Source of truth: `probe/avoid-quest-ids.txt`. It lists available quests only (never-offered ones are
   pointless) and currently holds 3212 ids: unsupported family, structural flag (holiday, dungeon,
   group-elite, raid, profession, vehicle, PvP, event-gated) or FAILING-structural. See the AVOID section
   of REGISTRY_SUMMARY.md. It goes into the soak profile as `pb|AutoWow.QuestAvoidIds|<list>` through the
   existing `scripts/soak-config.sh apply`, which rewrites named keys only. Low-priority ids come from
   FAILING without `avoid`.
4. Refresh: after each soak's `archive/<run>/ledger.log` is written, run `python probe/build_registry.py`.
   With no arguments it globs every archive plus the live log. The output is byte-identical for identical
   inputs (selftest asserts this), so rerunning is idempotent. A quest leaves the AVOID list automatically
   once it earns a clean reward (PROVEN beats every avoid reason).
5. New events (286b5511, confirmed live in soak-s6-cohort-r1): the registry folds `progress` (event 9) itself, because the v1 reducer ignores
   unknown events. A `progress` event marks a silent row as `silent_after_progress` and counts as other
   activity for fixation. The blocked reason `travel_no_progress` maps to stage travel_to_source/finisher
   by phase and to the waypoint-bounce defect. Both are covered by `--selftest`.

Registry rebuilt 05:06 UTC over 7 logs (S1-S5r2 archives + live S6). It folded 206 `progress` events,
and the verdict totals sum to 9464. The new build also emits deferred reason `blocked_deferred` (84 rows,
7 families). Its stage comes from the row's phase, which is mostly `select` because the deferral fires
at resolve_objective. A later refinement would stage it by the phase of the blocked lines that preceded
it.
