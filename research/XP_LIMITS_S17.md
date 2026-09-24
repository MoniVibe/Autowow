# XP limits — soak-s17-full-r1 (what caps cohort XP/hour now)

Read-only analysis, 2026-09-24. Nothing on the server was changed: no config edits, no builds, no restarts.

- **Run:** `soak-s17-full-r1`. Binary f33f4cbf, module main 662115f2 (`/root/autowow-advisor-t1-module`). Worldserver pid 190667.
- **Product profile (all fixes on):**
  - quest scheduler, travel intent, walking V2, safe travel
  - zone progression, transports, portal fallback
  - errands, rest gate, keep-consumables
  - death-loop V2, safe revive, unstick, pull cap
  - tactics (priest), professions
- **Realm rates:** 5x XP (`Rate.XP.Kill/Quest/Explore = 5`). 1.5x player move speed (`Rate.MoveSpeed.Player = 1.5`).
- **RPG weights (playerbots.conf):** WanderRandom 15, WanderNpc 20, GoGrind 15, GoCamp 10, DoQuest 60, TravelFlight 15, Rest 5, OutdoorPvp 10.
- **Cohort:** 50 persistent bots, guids 62955-63004. Level 11.28 at the 07:40 snapshot, 11.90 at 08:36.
- **Server clock:** run started 07:38:45 IDT. Ledger ms 0 = that time.

## Evidence sources and windows

| source | what | window |
|---|---|---|
| `ledger.log`, filtered to the cohort | combat telemetry (cv=1), accepted/rewarded/blocked/deferred/died/errand/zone_move | 07:38:45–08:36:30 |
| `cohort-s17.jsonl` (07:40:19) plus bridge `list` samples | level/xp per bot, used for XP gain | 07:40:19–08:36:23 = **0.936 h/bot = 46.8 bot-h** |
| bridge `list` sampler, 6 s cadence, 381 samples × 50 bots (19,050 observations) | per-bot state, moving flag, hp/mana, position | 08:14:17–08:36:23 = **22.1 min = 17.5 bot-h** |
| `Playerbots.log` | RPG status summaries (fleet-wide, all 133 bots), `[Unstick]`, travel-intent give-ups, `[Errands]` | whole run |
| world DB and `QuestXP.dbc` (read-only SELECTs) | quest XP, `player_xp_for_level` | — |

Reducer outputs (cohort-filtered ledger):
- `work/soak-20260923/reduce-s17/`: ledger-rows.jsonl, ledger-summary.md
- `work/soak-20260923/combat-s17/`: combat-summary.md, combat-cells.jsonl, profile `product-s17`

Scratch scripts are in the session scratchpad: analyze.py, budget.py, split.py, pipeline.py, sampler.py.

**Measured rate:** 330,100 XP over 46.8 bot-h = **7,052 XP/bot-h = 0.655 levels/h/bot**.

The brief's 0.92 levels/h was measured on an earlier, shorter window with a different method. This report computes level plus fractional XP from `player_xp_for_level` over a longer window. The rate fell in the second half of the run, and the S17 profile is not stationary:
- **Full run** (combat reducer): combat 6.9%, dead 3.9%.
- **Sampler window:** combat 4.8%, dead 7.2%. Deaths in the Barrens were rising.

## 1. Time budget per bot-hour

Window: sampler window, 17.5 bot-h. Profile: product-s17. The categories add up to 100%.

| bucket | share | min per bot-hour | how measured |
|---|---:|---:|---|
| in combat | 4.8% | 2.9 | ledger `combat_ms` per minute line |
| dead / corpse run | 7.2% | 4.3 | ledger `dead_ms` (includes ghost run) |
| resting / eating | 2.0% | 1.2 | sampled: not moving, hp < 90% or mana < 80% |
| walking (to quest objectives **and** RPG wander, which cannot be separated per bot) | 33.5% | 20.1 | sampled: moving, or more than 8 yd between samples |
| travel: zone moves | 2.2% | 1.3 | ledger `zone_move.travel_ms` windows |
| town errands (walk to town + walk back; lower bound) | 1.2% | 0.7 | ledger `errand.travel_ms + return_ms` windows |
| idle / deciding | 23.5% | 14.1 | sampled: stationary, full resources, moved within ±3 min |
| stuck (stationary ≥3 min, no XP, no combat) | 25.6% | 15.4 | sampled |
| **total** | **100.0%** | **60.0** | |

Notes:
- **Method:** time outside combat, death, errands and zone moves was split per bot using that bot's own sample classes, then summed.
- **In-town errand time:** not logged separately, so it falls inside walking or idle.
- **Walking split (fleet-wide only):** the RPG status mean over 10 summaries covers all 133 bots, including the 83 non-cohort bots:
  - Idle 5.7, Rest 14.9, GoGrind 10.8, GoCamp 3.3
  - MoveRandom 11.2, **MoveNpc 32.8**, **DoQuest 28.5**, **TravelFlight 25.0**, OutdoorPvP 0.8
  - Only about 38% of bot-states are XP-producing (DoQuest + GoGrind + MoveRandom).
- **Stuck bots:**
  - 13 cohort bots were stuck in more than 50% of their samples.
  - 9 bots never moved more than 15 yd in 22 min (they are listed in §2).

## 2. XP sources

Window: 46.8 bot-h, 07:40–08:36.

| source | XP | share | per event |
|---|---:|---:|---|
| kills (modelled: AC `XP::BaseGain` × 5 per kill level delta, not elite, not rested) | 222,414 | 67.4% | 560 kills → **397 XP/kill** |
| quest turn-ins (`QuestXP.dbc` × gray-quest factor × 5) | 85,175 | 25.8% | 31 rewards → **2,839 XP/quest** (about 7 kills) |
| rest (exploration XP, elite ×2, model error) | 22,511 | 6.8% | — |
| **total measured gain** | **330,100** | **100%** | |

Notes:
- **Kill rate:** 12.0 kills/bot-h. Median time to kill (TTK) is 13.4 s, p90 24.0 s.
- **Kill level mix:** the median kill is 2 levels **below** the bot. Of 558 kills with a level-delta sample, 492 (88%) were at or below the bot's level. A same-level kill at L12 is worth 525 XP.
- **Rested XP:** negligible. The cohort's `rest_bonus` sums to 753 XP.
- **Quest mix:** 11 of the 31 rewards are low-value breadcrumbs or quests below the bot's level, worth 225–2,100 XP each (Ride to X, Wharfmaster Dizzywig, Tenaron's Summons…).
- **Zero-XP bots:** 15 of 50 gained **zero XP** in 56 min:
  - 6 parked: Brokkhelm, Heddabrand, Gorvanth, Aelthorian, Lirethiel, Thalindor.
  - 3 L9 bots in Duskwood in a death-loop cluster: Aldermund, Brisenne, Delphyne.
  - 3 L9 bots in Bloodmyst.
  - Kragzul, stuck in Thunder Bluff.
  - 2 others.

**Parked vs mobile:** 9 bots stayed inside 15 yd for the whole sampler window: Brokkhelm, Heddabrand, Olbrek, Morvain, Veyliss, Gorvanth, Aelthorian, Lirethiel, Thalindor.

| group | bots | XP/bot-h |
|---|---:|---:|
| parked | 9 | 911 |
| mobile | 41 | 8,399 |

The mobile bots still average only 14.5 kills/h, 0.73 quests/h, 8.4% combat and 4.8% dead.

### Why the bots are parked (root cause, from code and logs)

- **Log volume:** the cohort logged 5,369 of the fleet's 5,919 `travel intent gives up (intent_replan_exhausted)` lines. The top 8 bots have 345–392 each, one every ~9 s.
- **Destinations are flight masters.** The give-ups point at a handful of coordinates, and each matches a flight master spawn:

  | give-ups | map | position | flight master |
  |---:|---:|---|---|
  | 793 | 530 | 9376,-7165 | Silvermoon |
  | 763 | 0 | -5425,-2930, z=347 | Thelsamar |
  | 404 | 0 | 474,1534 | The Sepulcher |
  | 361 | 1 | -1197,26, z=177 | Thunder Bluff (top of Spirit Rise) |
  | 274 | 0 | -14449,506 | Booty Bay |
  | 260 | 530 | 7595,-6782 | Tranquillien |
  | 191 | 0 | -4821,-1152 | Ironforge |

  Every one of these is on a different floor from the bot or behind a lift or ramp. The bots stand 70–110 yd away with `segments=0`.
- **`RPG_TRAVEL_FLIGHT` has no timeout and no failure exit:**
  - `NewRpgTravelFlightAction::Execute` just returns `MoveFarTo(data.flightMasterPos)`, at NewRpgAction.cpp:3585.
  - Its status case only leaves on flight arrival, at NewRpgAction.cpp:489.
  - Every other status has a duration: WanderNpc/WanderRandom 5 min, Rest 30 s, DoQuest 30 min.
- **The travel intent gives up, the status stays TravelFlight, and the loop repeats forever.** `[Unstick]` then escalates once:
  - hearth, 43 times;
  - portal, 8 times;
  - then `action=none ok=false` on 96 of 147 triggers, repeating every 10 min with `still_ms≈600000` and `exhausted=70`.
- **Four paths enter TravelFlight:**
  - the random status pick (weight 15);
  - the death-loop relocation;
  - errands (`NewRpgErrands.cpp:711`);
  - zone progression (`NewRpgZoneProgression.cpp:357/632`).

## 3. Quest pipeline (cohort, 169 ledger rows)

- **Final outcomes:**

  | outcome | rows |
  |---|---:|
  | rewarded | 31 |
  | deferred | 81 |
  | open_stalled | 40 |
  | open_progressing | 11 |
  | contaminated | 5 |
  | blocked | 1 |
  | **total** | **169** |

- **Quests accepted during the run:** 70 rows.

  | state | rows |
  |---|---:|
  | rewarded (13%) | 9 |
  | open_stalled | 30 |
  | deferred | 17 |
  | open_progressing | 8 |
  | contaminated | 5 |
  | blocked | 1 |
  | **total** | **70** |

  The other 22 rewards were turn-ins of quests carried over in the persistent quest log.
- **Time per completed quest:**
  - Accept to reward (9 rows): median **1.4 min**, p75 14.9, max 38.8 min.
  - Talk-only rewards take 0.3–3 min.
  - The 3 collect-drop rewards took 4.9, 25.0 and 38.8 min.
  - Pipeline throughput: 46.8 bot-h / 31 rewards = **1.5 bot-h per reward** (0.66 quests/bot-h).
- **Where the pipeline breaks** (blocked events, cohort):

  | reason / phase | events |
  |---|---:|
  | intent_replan_exhausted / travel_to_source | 68 |
  | intent_replan_exhausted / travel_to_finisher | 68 |
  | no_finisher_relation / resolve_finisher | 25 |
  | other | 4 |
  | **total** | **165** |

  - These produce 160 `blocked_deferred` deferrals (600 s holds), plus 11 `death_loop_area`, 5 `sched_rotate` and 2 `inventory_full`.
  - A quest log where every quest is deferred leaves no DoQuest candidate. The bot then falls into random statuses, including the TravelFlight trap.
- **Families that lose the most time.** "Open-min" counts minutes from accept to stop, for quests accepted during the run:

  | family | rows | pass rate | where the time goes |
  |---|---:|---:|---|
  | TALK_ONLY | 64 | 25.0% | 7 deferred rows = 182 open-min. Finishers across zones or on another floor (Ride to X / Dizzywig); `no_finisher_relation` |
  | COLLECT_DROP | 63 | 14.3% | 4 deferred = 105 open-min, 4 progressing = 101, 3 rewarded = 69. Slowest completions; travel_to_source failures around Thelsamar (zone 38 buckets -108,-60 / -107,-59) |
  | KILL | 22 | 18.2% | 2 deferred = 78 open-min. Death-loop areas in zones 40 and 17 |
  | GAMEOBJECT_COLLECT | 18 | 11.1% | 4 deferred = 58 open-min |

## 4. XP/h by class

Same window. Class is confounded with placement: most parked or death-loop bots are paladin, mage, warlock or rogue.

| class | n | avg L | XP/bot-h | kills/h | quests/h | deaths/h | combat | dead | kill share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| shaman | 4 | 11.8 | 11,429 | 17.4 | 1.07 | 0.53 | 5.8% | 0.1% | 62% |
| warlock | 6 | 12.7 | 8,876 | 15.5 | 1.25 | 0.36 | 5.1% | 0.2% | 63% |
| hunter | 6 | 10.2 | 8,759 | 19.0 | 0.53 | 2.31 | 12.7% | 0.9% | 85% |
| rogue | 7 | 11.4 | 8,395 | 14.5 | 0.46 | 3.05 | 9.8% | 8.1% | 74% |
| warrior | 8 | 11.6 | 7,133 | 13.6 | 0.67 | 2.67 | 6.4% | 2.9% | 69% |
| priest | 6 | 11.5 | 6,529 | 8.2 | 0.53 | 1.78 | 6.5% | 8.9% | 56% |
| druid | 3 | 11.0 | 6,024 | 8.9 | 0.36 | 0.71 | 4.4% | 0.5% | 61% |
| mage | 6 | 10.8 | 4,323 | 5.5 | 0.71 | 2.31 | 3.2% | 0.8% | 56% |
| paladin | 4 | 10.0 | 508 | 0.5 | 0.00 | 1.87 | 5.3% | 11.6% | 47% |

- **Paladin:** 3 of the 4 are parked or death-looping (Brisenne in Duskwood, Heddabrand, Aelthorian). This is not a class-kit verdict.
- **Deaths:** 90 cohort deaths.

  | zone | deaths |
  |---|---:|
  | Barrens (17) | 47 |
  | Loch Modan (38) | 19 |
  | Duskwood (40) | 11 |
  | Bloodmyst (3525) | 7 |
  | Silverpine (130) | 4 |
  | other zones | 2 |
  | **total** | **90** |

  Of the 88 deaths to creatures, 45 (51%) were to mobs 1–3 levels above the bot and 16 to same-level mobs.

## 5. Human reference at 5x, L10–15

These are stated assumptions, not measurements:
- **1x baseline:** an experienced 3.3.5 player at L10–15 levels about 1.2–1.5 times per hour, roughly 12–15k XP/h. About 55–65% comes from quests, 30–40% from kills and ~5% from exploration.
- **At 5x:** the time per action stays the same, so the same play gives about **60–75k XP/h (5–7 levels/h)**.
  - Real 5x players outlevel quests quickly and skip gray ones. A realistic human 5x figure is therefore **about 50k XP/h**.
- **Human time mix:** combat ~40%, walking and travel ~35%, NPC and town ~10%, rest/eat ~8%, dead ~2%, idle ~5%. That gives roughly 80–120 kills/h and 8–12 quests/h.

| | bots, S17 | human, 5x, assumed | ratio |
|---|---:|---:|---:|
| XP/h | 7,052 | ~50,000 | **~14%** |
| combat share | 4.8% (window), 6.9% (run) | ~40% | ~0.15× |
| kills/h | 12.0 | 80–120 | ~0.12× |
| quests/h | 0.66 | 8–12 | ~0.07× |
| idle + stuck | 49.1% | ~5% | ~10× |

**Conclusion:** the bots are not slow at fighting. TTK of 13.4 s is about 1.5–2× a human's. The problem is that they fight for only ~5% of the hour. Half the hour is spent standing still: 23.5% idle and 25.6% stuck.

## 6. Top 3 levers (ranked by expected XP/h × confidence)

### Lever 1 — the TravelFlight / unreachable-flight-master trap (code + config)

**Expected gain:** +1.35k to +2.4k XP/bot-h (**+19% to +34%**).
- **Floor:** the 9 parked bots reach the mobile average. (9×8,399 + 41×8,399) / 50 = 8,399 vs 7,052 today.
- **Ceiling:** all 25.6% stuck time is converted at the current mix. 7,052 / (1 − 0.256) = 9,480.

**Config, zero-risk first step:** set `AiPlayerbot.RpgStatusProbWeight.TravelFlight = 0` in `/usr/local/etc/modules/playerbots.conf`. Zone progression, errands and death loop keep their own flight legs. This removes the random entry, but not the other three.

**Code** (`src/Ai/World/Rpg/Action/NewRpgAction.cpp`):
- (a) In `NewRpgTravelFlightAction::Execute`, when `MoveFarTo(flightMasterPos)` reports stuck or gives up, remember that flight master as unreachable for this bot (per-bot TTL) and `ChangeToIdle()`.
- (b) Give `RPG_TRAVEL_FLIGHT` a persisted-duration exit while not in flight, like the 5-min wander statuses.
- (c) When `[Unstick]` returns `action=none`, fail the current destination instead of re-planning the same point every ~9 s.
- Zone-progression and errand flight legs should fall back to a walk leg when the flight master is unreachable.

### Lever 2 — RPG status weights: time spent in no-XP statuses (config only)

**Expected gain:** +1.5k to +2.5k XP/bot-h (**+20% to +35%**) on top of lever 1.

**Estimate basis:**
- Fleet-wide, 27.7% of bot-states are MoveNpc + GoCamp + OutdoorPvP (32.8 + 3.3 + 0.8 of 133). Only 38% are DoQuest + GoGrind + MoveRandom.
- Moving that 27.7% into productive statuses raises productive time ×1.73. At 50% realization that is about +36%, and I claim the lower +20–35%.
- **Caveat:** the status counts cover all 133 bots, so the cohort's own mix is unmeasured. Confidence is medium.

**Change** (`playerbots.conf`):
- `WanderNpc 20→2`, `GoCamp 10→0`, `OutdoorPvp 10→0`, `Rest 5→0` (the rest gate already handles real rest).
- `GoGrind 15→30`, `WanderRandom 15→30`.
- Keep `DoQuest 60`.

WanderNpc is locked for 5 min per pick (`statusWanderNpcDuration`). Errands now cover the vendor, repair and training reasons that justified it.

**Verify with:**
- fleet status counts in `Playerbots.log`;
- the combat share in `combat-reduce` (expect >10%);
- kills/h (expect >20).

### Lever 3 — the quest pipeline breaks on travel-intent replan exhaustion (code)

**Expected gain:** +1.5k to +4k XP/bot-h (**+20% to +55%**). Confidence is lower and the change is larger.
- Quests currently deliver 26% of XP at 0.66 rewards/bot-h and 2,839 XP each.
- Doubling completions to about 1.3/h adds about +1.9k. Reaching 2/h adds about +3.8k.

**Evidence:**
- 136 `intent_replan_exhausted` blocks, split evenly between travel_to_source and travel_to_finisher.
- These produce 160 of 178 deferrals.
- Only 9 of the 70 quests accepted during the run were rewarded.
- The same multi-floor towns fail again: Thelsamar, Sepulcher, Thunder Bluff, Silvermoon, Tranquillien.

**Code** (`TravelIntentPolicy` / `MoveFarToIntentV2`, `NewRpgBaseAction.cpp` ~475–520 and ~700):
- Snap the destination to the nearest navmesh poly with a larger vertical search before planning. The failures sit at `best` 70–110 yd with `segments=0`, which matches a z-level mismatch.
- Route multi-floor towns through a known ramp or lift waypoint.
- Before blocking, retry with the next spawn candidate (source) or the next finisher spawn.
- Separately, 25 `no_finisher_relation` events (q1492 Wharfmaster Dizzywig and others) are a data or relation gap. The fix is a quest-avoid entry or a relation fix.

### Honourable mention (not top 3)

- **Death time:** 7.2% of the hour, and rising. The Barrens had 47 of 90 deaths.
  - Tightening the pull cap and death-loop areas for zone 17 is worth at most about +7%.
- **L9 bots in L18+ zones:** Duskwood (3 bots) and Bloodmyst (3 bots) are placement problems that zone progression should graduate downward, or never send them there.

## Residual gaps (named)

- **Walking split:** quest walking vs wander walking cannot be separated per bot. The bridge `list` does not expose RPG status per bot.
  - A per-bot `rpg_status` field in `list`, or a `status` ledger event on each status change, would make this bucket exact.
- **Town-errand in-town time:** not logged (only travel and return). It sits inside walking/idle.
- **Kill XP is modelled, not logged.** No `xp_gain` ledger event exists. The 6.8% remainder includes exploration XP and elite ×2 error.
- **Non-stationary run:** full-run combat/dead is 6.9% / 3.9%, vs 4.8% / 7.2% in the sampler window.
