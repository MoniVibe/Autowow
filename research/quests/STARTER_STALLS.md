# Starter-zone quest stalls: soak-s5-cohort-r2

Lane `starter-stalls`, module commit `286b5511`, rebased on main `09b79811`. Date: 2026-09-23.
Status: **HELD**. The fixes are behind flags, unit-tested and compile-checked. They have not been run
live, so no flags-on soak has confirmed them yet.

## Evidence base

- **Bridge `questobjective` sweep.** One read-only call per bot for all 50 cohort bots (62955-63004), at about 07:08 WSL time, roughly 1.9 M ms into the run.
  - **Phases (sums to 50):** `travel_to_source` 25, `blocked` 17, `travel_to_finisher` 3, `resolve_objective` 3, `acquire_target` 1, `loot_source` 1.
  - **Blocked reasons (sums to 17):** `movement_stuck_no_teleport` 11, `interaction_rejected` 3 (all q5441), `no_live_candidate` 2, `no_source_spawn` 1 (q9303).
- **Repeated sampling (2-3 samples, 2-3 s apart)** of 8 travelling bots: 62961, 62972, 62982, 62996, 62998, 62991, 62992 and 62994.
  - Each bot's `last_move_to` alternates between two fixed waypoints.
  - Its distance to the source never improves. `stuck_attempts` stays at 0.
- **Ledger fold** (`reduce-s5r2`). Each cohort stall row has `last_ev=accepted` and no later event. Before this lane the ledger could not show silent progress, so it had no data to tell working from parked.
- **World DB (read-only SELECT).** Quest templates, spawns, `conditions` source 17, `smart_scripts` and creature factions.

## Root causes (generic)

1. **Travel ping-pong.** In quest travel, `MoveFarTo(questNoTeleport)` calls `tryPreparedQuestWalk` at every spline end. When that walk is accepted, it resets `nearestMoveFarDis` to the *current* distance and `stuckAttempts` to 0.
   - Two alternating prepared segments therefore look like progress every time, and the stuck detector never fires. The segments come from the TravelMgr re-anchor, whose graph route flips depending on the start point.
   - `TravelToSource` has no wall-clock budget of its own.
   - Observed waypoint pairs:
     - Coldridge: (-6189,391) and (-6315,470)
     - Valley of Trials: (-468,-4283) and (-635,-4298)
     - Mulgore: (-2945,-149) and (-2910,-244), and around (-3000,-840..-900)
2. **Blocked parks the bot for up to 30 minutes.** Outside Oracle management, `BlockQuest` holds with `ForceToWait(3000)` until the `statusDoQuestDuration` (30 min) RPG timeout. No director acts on cohort bots (`oracle.status=not_managed`).
3. **Why accepted quests look neglected (coordinator question A).**
   - **Quest selection:** from IDLE, `RandomChangeStatus` picks DO_QUEST with weight 60 of 150. It then chooses one quest uniformly at random from the quest-log entries that have a POI, excluding `lowPriorityQuest`.
   - **No rotation:** that directive is kept until it is rewarded, until an unsupported blocker is hit, or until the 30-minute timeout. No other rule rotates the bot to another accepted quest.
   - **How quests pile up:** during DO_QUEST, `SearchQuestGiverAndAcceptOrReward` keeps accepting quests (and turning in TALK quests) at any giver the bot passes.
   - **Result:** a bot collects about 3 quests but works only one. When that one is ping-ponging or Blocked, the other quests stay at zero for the whole 31-minute run.
   - This is fixation on one directive, not failure of the neglected quests. It also explains why rewards are mostly TALK quests and nearest objectives.

## Per-quest table

| quest | family (census) | stalled | cause (evidence) | fix |
|---|---|---:|---|---|
| 3361 A Refugee's Quandary | GAMEOBJECT_COLLECT | 9 | Travel ping-pong toward Felix's Box (GO 148499, one spawn at -6374,773, 10 s respawn). 62961 and 62972 alternate (-6189,391) and (-6315,470) and stay 309-427 yd away. **Not contention:** 62971 reached the box (`loot_source`, d=4). | A + B |
| 792 Vile Familiars | KILL | 9 | Valley ping-pong: 62982 alternates (-468,-4283) and (-635,-4298), 241-350 yd from the selected spawn. 37 spawns, so not contention. The July PASS started from a different position. | A (≤2 spawn rotations) + B |
| 5441 Lazy Peons | ITEM_USE_ON_TARGET | 9 | 3 bots Blocked `interaction_rejected` 4-5 yd from a peon. Spell 19938 needs aura 17743 on the target (sleeping; `conditions` 17/19938 type 1, target 1). 9 bots on 14 peons wake them, so later hits never credit. The other bots ping-pong. | C + B + A |
| 233 Coldridge Valley Mail Delivery | TALK_ONLY | 8 | Collateral: complete in log while the directive sits on 3361/170. 62970 and 62974 are Blocked `movement_stuck_no_teleport` at the same (-6315,470) dead end, walking to Talin Keeneye (714 at -6222,689). | B + A (finisher) |
| 170 A New Threat | KILL | 8 | Collateral neglect: the directives sit on 3361. 62973 is actively travelling on 170. 102 trogg spawns, so not contention. | B/A unblock the directive |
| 4402 Galgar's Cactus Apple Surprise | GAMEOBJECT_COLLECT | 6 | 45 GO spawns (60 s respawn). One bot on the directive, travelling through the Valley ping-pong corridor. The others are collateral. Depth UNKNOWN: sampled once. | A + B |
| 789 Sting of the Scorpid | COLLECT_DROP | 5 | Collateral: no bot had it as the directive at sampling time. Valley ping-pong. | A + B |
| 750 The Hunt Continues | COLLECT_DROP | 5 | Mulgore ping-pong: 62991 and 62993 alternate (-2945,-149) and (-2910,-244). 62990 is Blocked `movement_stuck` (-3340,-207). 73 cougar spawns. | A + B |
| 3376 Break Sharptusk! | COLLECT_DROP | 5 | Single spawn (8554 ×1 at -2932,-1276). 62992 and 62994 oscillate about 420 yd away and never close in. There is also latent single-spawn contention. | A → block → B defer |
| 9283 Rescue the Survivors! | KILL → **SPELL_CREDIT** | 4 | Credit is Gift of the Naaru (28880) cast on friendly 16483 (faction 1638, C++ `npc_draenei_survivor`). The kill executor accepts it by `unit_flags`, but the target cannot be attacked. | Census reclassified. Executor: **not fixed** (see below). |
| 9303 Inoculation | ITEM_USE_ON_TARGET | 4 | Wrong target identity. Credit creature 16534 has 0 spawns, so the bot blocks with `no_source_spawn`. The item spell 29528 targets 16518 (`conditions` 17/29528 type 31, target 1, value1 3, value2 16518; 17 spawns). | C |
| 9305 Spare Parts | GAMEOBJECT_COLLECT | 4 | GO 181283 ×13 (180-601 s respawn). 62977 travelling (d=118); the others are collateral. Depth UNKNOWN. | A + B generic |

## Fixes (module branch `starter-stalls`, all default 0)

| flag | label | behaviour when 1 |
|---|---|---|
| `AutoWow.QuestTravelProgressWatch.Enable` | A | Covers source and finisher travel, non-Oracle only. The best distance must improve by at least 10 yd within 150 s of *observed* travel. Gaps of more than 10 s (combat, death, another status) are not charged. On expiry: mark the spawn exhausted and rotate to another resolved spawn (at most 2 travel rotations), otherwise Block with the new reason `travel_no_progress`. |
| `AutoWow.QuestBlockedDefer.Enable` | B | A non-Oracle quest that has been Blocked for 30 s is deferred for 10 min in a per-bot timed book (at most 25 entries). The ledger records `deferred` with reason `blocked_deferred`, and the bot goes back to Idle. DO_QUEST selection skips deferred quests until they expire, after which a fresh DoQuest retries. |
| `AutoWow.QuestItemTargetConditions.Enable` | C | (1) A CAST item-use target must pass the item spell's explicit-target conditions (the same `ConditionMgr` check as `Spell::CheckCast`). (2) If the credit creature has no spawns, positive condition-31 unit entries on the spell's explicit target are added as exact sources. Their spawns come from world data, sorted by spawn key and cached. |
| `AutoWow.Ledger.ProgressSampleMs` | ledger | Takes effect together with `AutoWow.Ledger.Enable`. A per-bot diff of the quest-log `c[4]`/`i[6]` counters every N ms (soak value 60000). Emits the append-only event `progress` = 9: combat is 8, and 10 and 11 are reserved for DeathLoop and SkillUp. `ledger-reduce.py` folds `progress` with `observe_counters`. Progress after a deferral clears `deferred_at`. |

**Design notes:**
- The pure policy lives in `src/Ai/World/Rpg/QuestStallRecoveryPolicy.h`. It uses integer yards and ms only, and wrap-safe clocks.
- `QuestFailureReason::TravelNoProgress` is appended, and its name is added to the ledger, bridge and quest-log name tables.
- Every OFF path is an early `if (flag)` guard. The only unconditional change is that `blockedAtMs` is now recorded in `BlockQuest` (a state write that nothing reads while the flags are off).

**Not fixed: q9283 spell-credit executor.** The credit spell is bound in a C++ script, not in DB data (no `smart_scripts` spellhit row, no spell condition). A generic "cast X on Y" executor would need a hand-maintained spell map. That would be a special case, so it is deferred to the owner or orchestrator. Draenei bots do have Gift of the Naaru, so a small data table (quest → spell) is the cheapest path if wanted.

## Census

`census/build_quest_census.py` now counts a required creature as SPELL_CREDIT when it is friendly to every side that can take the quest. "Friendly" means the faction template's group/friend mask includes the side and the enemy mask does not. The friend bit "all players" counts only for non-player-group templates, so 290 (Defias Prisoner) stays KILL.

- 71 quests changed family: KILL → SPELL_CREDIT 68, EXPLORE → SPELL_CREDIT 2, GAMEOBJECT_COLLECT → SPELL_CREDIT 1. Examples: 9283, Garments of *, the Winter Veil emote quests, rescue/free-prisoner quests.
- No other catalog field changed except `botReadyPrediction`: bot-ready fell from 4923 to 4886 of 8122 available.
- Selftest fixtures now include 9283 → SPELL_CREDIT and 170 → KILL. The catalog and `CENSUS_SUMMARY.md` were regenerated.

## Verification

- `tests/QuestStallRecoveryPolicyTest.cpp` (11 tests) and `tests/AutoWowQuestLedgerTest.cpp` (+2 tests) were built standalone against the shared build's `libgtest` and `compile_commands.json`. **Result: 26/26 pass after the rebase.** The object files went to `/tmp/ss` only.
- Compile check (same command lines, run on the worktree): NewRpgAction, NewRpgBaseAction, QuestValues, AutoWowQuestLedger, AutoWowBridge, QuestLogView, PlayerbotAI and PlayerbotAIConfig all compile (rc 0).
- No ninja run and no relink. The running worldserver's sha256 `43c9580c…bb20c7d` was unchanged before and after.
- `ledger-reduce.py --selftest` and `build_quest_census.py --selftest` both pass.

## Needs orchestrator

1. Build the worldserver from `starter-stalls`.
2. Run a cohort soak with `QuestTravelProgressWatch=1`, `QuestBlockedDefer=1`, `QuestItemTargetConditions=1` and `Ledger.ProgressSampleMs=60000`.
3. Expected result:
   - `travel_no_progress` blocks and then `blocked_deferred` rows for 3361, 792, 750 and 3376, followed by other quests progressing.
   - q5441 bots use items only on sleeping peons.
   - q9303 bots travel to owlkin 16518 and get credit.
4. **Risk:** a legitimate detour that increases distance for more than 150 s of travel is misread as a stall. The cost is bounded: a 10-minute deferral, then a retry.
