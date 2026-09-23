# AutoWoW CPU profile S2: after B1+B3 (soak-s2-80-r1)

Date: 2026-09-23.

- Live PID 51955; binary sha256 prefix `22d22a233b3b62af`; module main `3cbccb75`; core `/root/autowow-advisor-t1-core`.
- Run: ~80 bots (32 Oracle, SliceBots=8), GameType=1.
- Bots per map (from coordinator): 571=44, 1=19, 0=13, 530=4.
- Evidence labels: **MEASURED** = taken from this live run. **ESTIMATE** = derived from those measurements.

## Method

- **Sampling:** `perf record -F 99 --call-graph dwarf,8192 -p 51955 -- sleep 60` (10,579 samples). DWARF was kept at 8 KB because memory is tight. The 85 MB `s2.data` was **deleted after summarizing**.
- **Per-thread CPU:** `/proc/51955/task/*/stat` deltas over the same 60 s.
- **Call rates:** uprobes counted with `perf stat` for 10 s, then removed. The uprobes add overhead of their own (about 4M events). Treat the rates as order-of-magnitude.
- **Text reports:** in `research\perf\s2\`. Files are `s2_child_<tid>.txt` (inclusive), `s2_self_<tid>.txt`, `s2_child_all.txt` and `s2_dso.txt`.
- **Not done:** no flamegraph SVG. It needs FlameGraph scripts downloaded from outside.

## Thread split (MEASURED, 60 s)

| Thread | % of one core |
|---|---|
| map worker x4 | 46.2 + 45.8 + 44.8 + 44.4 = 181.2 |
| main/world (51955) | 8.4 |
| network (52077) | 2.2 |
| other | ~0.3 |
| **total** | **~192** (the metrics jsonl shows cpu 169-171 at 1-min granularity) |

- World diff over the last 60 samples: p50 15 ms, p90 26 ms, max 34 ms. In S1 it was 500-1400 ms.
- RSS is 3.3 GB.

## Where the CPU goes now (MEASURED, % of all samples, inclusive, overlapping)

| Frame | % | Owner |
|---|---|---|
| `Map::Update` | 92.7 | core |
| └ `Map::UpdateNonPlayerObjects` | 80.7 | core |
| └└ `Creature::Update` | 68.2 | core, creature sim |
| **1.** `Unit::UpdateSplinePosition` → `Map::CreatureRelocation` → `WorldObject::UpdatePositionData` (full vmap terrain status) | **33.5** | core |
| └ `VMAP::StaticMapTree::GetLocationInfo` / BIH intersect | 15.9 | core |
| **2.** `Unit::ProcessTerrainStatusUpdate` | **9.6** | core |
| **3.** `Creature::UpdateMovementFlags` (GetFloorZ, `DynamicMapTree::getHeight` 6.6) | **8.7** | core |
| **4.** `MotionMaster::UpdateMotion` 7.5, `Unit::_UpdateSpells` 6.9, `AIRelocationNotifier` 6.0, Aura updates 4.0 | ~24 | core |
| **5.** `ScriptMgr::OnCreatureUpdate` 4.5, of which `Creature::GetScriptId` is **3.2 self** (a hash lookup on every creature update) | 4.5 | core hook |
| `Player::Update` | 5.8 | core, bots |
| └ **bot AI** `PlayerbotAI::UpdateAI` | **4.5** | playerbots |
| └└ `Engine::ProcessTriggers` 2.2 (`Trigger::Check` 1.3), value calculates ~1.5 (GrindTarget 0.5, NearestUnits 0.4, HasAvailableLoot 0.4), actions 0.7 | | |
| TempSummon 4.35, MotionTransport 2.9, SmartAI 3.1 | | core |
| Main thread: `DriveOracleQuestRoute` → `TravelNodeMap::getFullPath` | 2.0 | AutoWoW Oracle |
| `ScriptMgr::OnUnitUpdate` → AutoWoW `CombatPerformanceTelemetry::OnUnitUpdate` | 1.0 | AutoWoW |
| `ActiveQuestObjectiveValue::Calculate` | <0.03 | B1 removed it |

Call rates (MEASURED, 10 s uprobe window):

| Function | Rate |
|---|---|
| `Map::Update` | ~2.7k/s, all map instances |
| `Creature::Update` | ~260k/s |
| `WorldObject::UpdatePositionData` | **~137k/s** |
| `PlayerbotAI::UpdateAIInternal` | 183/s, about 2.3 Hz per bot |

**Bottom line:** bot decision-making is now only **about 4.5% of the CPU**. About 75% goes to core creature simulation in the grids that 80 widely spread bots keep active. The largest single part is the terrain query that runs on every spline step.

## Per-bot cost on map threads (ESTIMATE)

- **All-in:** 1.92 cores / 80 ≈ **24 ms of CPU per second per bot**.
- **Bot AI only:** 0.045 × 1.92 / 80 ≈ **1.1 ms/s per bot**, or about 0.47 ms per AI tick.
- **World simulation caused by the bot:** ≈ **19 ms/s per bot**. A creature stays on the update list while any player can see it (`Creature::IsUpdateNeeded`, Creature.cpp:3914). This figure includes a baseline that exists without bots (waypoint mobs, transports).

## Evaluations requested

### (a) AI level-of-detail (fewer ticks for bots with nothing nearby)

Existing knobs:
- `AiPlayerbot.BotActiveAlone` (100) and `botActiveAloneSmartScale` (0).
- `AiPlayerbot.ReactDelay` (100) and `DynamicReactDelay` (1). Stock non-combat delay is 1-3 s; AutoWoW independent bots are clamped to 100-250 ms (`AutoWowIndependentActivityPolicy::NonCombatReactDelay`).
- `AiPlayerbot.IterationsPerTick` (10).
- `PassiveDelay` (10000).

There is no per-bot knob. AutoWoW independent bots **bypass** activity throttling entirely (`ShouldForceActivity`, PlayerbotAI.cpp ~AllowActive).

**Verdict:** LOD targets at most the 4.5% bot-AI share. Even halving AI ticks saves only about 2% of total CPU. Low priority.

### (b) Squad-brain (one planner per 3-5 bots)

- **AI saving alone** (ESTIMATE): 4.5% × ~0.5 ≈ **2%** of CPU.
- **The real lever is co-location.** Creature sim cost scales with the number of distinct visible areas, not with bot count. Squads of 4 sharing one area could cut the world-sim share per bot by roughly **40-60%**, which is about 30-45% of total CPU (ESTIMATE; depends on how much the visibility areas overlap).
- **Recommendation:** worth doing, but justify it as world-sim reduction, not AI reduction.

### (c) B2 objective cache

It now targets **under 0.1%** of CPU, because B1 already removed the copy. **Park it.** Revisit only if Oracle bots grow past about 200.

### (d) Async / off-thread planning

- Engine triggers and values read live Unit, grid and ObjectAccessor state. That is not safe off the map thread.
- The feasible targets are pure computations over snapshots: Oracle Plan over `LiveQuestFrame` (already snapshot-shaped) and `TravelNodeMap` route search. The route search needs an audit for per-node mutable scratch state first.
- **Ceiling:** about 2% (the Oracle route on the main thread) plus a slice of the 4.5% AI. **Not worth it now.**

## Ranked next reductions

### Config / no rebuild (worldserver.conf; takes effect at next restart)

| # | Key | Now → try | Expected (ESTIMATE) | Risk |
|---|---|---|---|---|
| K1 | `MapUpdateInterval` | 10 → **50** | Map ticks now run near the world loop rate. Per-tick creature work (spline, terrain, movement flags, motion: about 55-60% of CPU) scales with tick rate, so expect **−35-50% total CPU**. Bot AI (2.3 Hz) is unaffected. | Coarser creature movement/combat granularity (50 ms). A/B test for one restart. |
| K2 | Bot placement across continents (bridge/spawn policy, not a conf key) | 571=44 / 1=19 / 0=13 / 530=4 → about 20 each | One map updates on one thread, so 571 (55% of bots) is the tick's critical path. Balancing cuts the critical-path time about 2×. Total CPU unchanged. | Changes content distribution. |
| K3 | `Visibility.Distance.Continents` | 100 → 90 | About −19% visible area, so fewer creatures on the update list; roughly −10% CPU. | Bots see less (SightDistance 100 / ReactDistance 150). Medium risk. |
| K4 | AI knobs (`IterationsPerTick`, `ReactDelay`, `BotActiveAlone`/SmartScale) | leave | ≤ 2% | Not worth it. |

I found no live GM or `.playerbots` setting that changes any of the above.

### Needs rebuild

Items marked core need a core-fork change (`/root/autowow-advisor-t1-core`); flag that to the owner before building.

1. **R1 (core): throttle creature terrain refresh on relocation.**
   - Where: `src/server/game/Maps/Map.cpp`, `Map::CreatureRelocation` (~line 834), which calls `creature->UpdatePositionData()` on every spline step. Related: `Unit::UpdateSplineMovement` (Unit.cpp:721-724), where the original 400 ms spline timer was commented out ("update always").
   - Change: refresh only when the cell changed, the 2D distance moved ≥ 1-2 yd since the last full status, or ≥ 250-500 ms have passed. Otherwise keep the cached status.
   - Saving: about 33% → 10%, so **−20-25% CPU**.
   - Risk: medium. Swim/fly flags and area auras could lag by up to the threshold.
   - Test seam: pure predicate `ShouldRefreshTerrain(lastPos, pos, lastMs, nowMs, cellChanged)`, plus a test that a cell change always refreshes.
2. **R2 (core): cache the script id on Creature.**
   - Where: `Creature::GetScriptId` (Creature.cpp:3176), which does a `GetCreatureData` hash lookup on every `ScriptMgr::OnCreatureUpdate`.
   - Change: cache the script id when the creature is created or its entry changes.
   - Saving: **−3%**. Risk: low.
   - Test seam: the cached value equals the recomputed value after `UpdateEntry`.
3. **R3 (module): stop the telemetry per-unit hook.**
   - Where: `src/AutoWow/CombatPerformanceTelemetry.cpp:408`. `OnUnitUpdate` runs for every unit, every tick.
   - Change: move the body into the existing `OnPlayerAfterUpdate`, which only runs for players, and drop the UnitScript hook.
   - Saving: **−1%**. Risk: low.
   - Test seam: the counters are unchanged for a bot player.
4. **R4 (module): cache the Oracle route.**
   - Where: `NewRpgDoQuestAction::DriveOracleQuestRoute` → `TravelNodeMap::getFullPath` on the world thread.
   - Change: cache the route per (bot, target node) until the target changes.
   - Saving: about 2% of CPU, all on the world thread. Risk: low.
   - Test seam: cache key and invalidation.
5. **R5 (module/design): squad co-location (b).** About 30-45%, but it is a gameplay/design change.
6. B2 and async planning: parked (see (c) and (d)).

## Projected capacity (ESTIMATE; assumes linear scaling with bots at today's spread)

| Setup | 200 bots | 400 bots |
|---|---|---|
| Today's config | ~4.8 cores | ~9.6 cores |
| With K1 + R1 | ~2.0-2.5 cores | ~4-5 cores |

- The WSL VM has 24 vCPUs, so total CPU is fine at both counts.
- **The limit is per-map serialization.** Keep bots per continent at about 100 or fewer, or balance them with K2.
- At 400 bots with 55% on 571, map 571 alone would need about 5 ms of CPU per bot-second-tick, so ticks would grow to roughly 60-100 ms (self-throttling, because fewer ticks means less per-tick work). That is acceptable for a bot world but should be verified.
- Memory: 3.3 GB RSS at 80 bots. Expect about 5-7 GB at 400 (ESTIMATE), which matters while memory is tight.

**Next measurement:** A/B `MapUpdateInterval` 10 vs 50 at 80 bots. Repeat this same 60 s recipe and compare total CPU and `UpdatePositionData`/s.
