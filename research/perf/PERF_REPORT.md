# AutoWoW bot decision-time / CPU profile — soak-s1-30-r1

Date: 2026-09-23. Module source: `/root/autowow-advisor-t1-module` @ `c2a14f15`. Binary: `/root/autowow-advisor-t1-build/src/server/apps/worldserver`, **RelWithDebInfo (-O2 -g)**, static linking. Symbols good, frame pointers omitted (so DWARF unwinding was used).

Evidence classes: **MEASURED** = taken from the live process in profile soak-s1-30-r1 (~42 bots online: 36 random bots plus account bots, 12 of them Oracle-allowlisted). **ESTIMATE** = worked out from those measurements.

## Method

- Live process PID 33653 was never stopped or ptrace-attached.
- perf 6.8.0-139 (from `linux-tools-generic`) ran on WSL kernel 6.6.87.2. `perf record -F 99 --call-graph dwarf,16384 -p 33653 -- sleep 90`: 93.8 s wall, 10,308 samples. The raw file is at `/root/autowow-soak/perf/r1.data`.
- Per-thread CPU came from `/proc/33653/task/*/stat` deltas over the same window.
- Call counts came from kernel uprobes counted with `perf stat` for 30 s. The probes were `ActiveQuestObjectiveValue::Calculate` (0x105449a), `Runtime::ProcessConfiguredBot` (0x996a28) and `Runtime::Update` (0x999434). All were removed afterwards.
- Limitation: 58% of main-thread stacks were deeper than the 16 KB DWARF copy, so they have no root frame. Leaf and mid-stack attribution is still exact. No flamegraph SVG was made, because that needs FlameGraph scripts downloaded from outside. `soak-s1-30-r1_mainthread_inclusive_top60.txt` holds the inclusive table instead.

## Thread split (MEASURED, 93.8 s)

| Thread | % of one core |
|---|---|
| main/world thread (TID 33653) | 75.3 |
| map threads x4 (MapUpdate.Threads=4) | 11.9 + 11.7 + 10.7 + 10.1 = 44.4 |
| everything else | ~1.3 |
| **total** | **~121** (the metrics jsonl shows cpu 119) |

Update diff (MEASURED, Server.log): n=27 samples, p50 637 ms, p90 905 ms, max 1200 ms. Over the last 500 diffs the mean is 246 ms and the median is 46 ms, with p95/p99/max of 1243/1376/1437 ms. The distribution is **bimodal**: most world ticks are fast, and one tick per second is huge.

## Root cause: the Oracle tick copies the global quest spawn map

The Oracle runs on the world thread (`PlayerbotsWorldScript::OnUpdate` → `AutoWowOracleRuntime::Update`). Each 1000 ms cadence it runs `ProcessConfiguredBot` for every allowlisted bot (MaxBots=12). Each bot's pass calls `Get()` on the **"active quest objective"** value several times: EnsureQuestDirective (:853, :915), BuildLiveQuestFrame (:1171, twice per pass through :1938/:2236), NativeDispatch → NewRpgDoQuestAction (NewRpgAction.cpp:2005), and :2016.

- That value is built with `checkInterval = 1` (QuestValues.h:272). In `CalculatedValue::Get` (Value.h:75), `checkInterval < 2` means it **recomputes on every Get and never caches**.
- `ActiveQuestObjectiveValue::Calculate` (QuestValues.cpp:359) runs `questGuidpMap questMap = GAI_VALUE(...)`. That is a **deep by-value copy of the entire global quest→relation→entry→vector<GuidPosition> map**: every quest-related creature and GO spawn in the world. The copy is then destroyed at the end of the call.

### Top 5 world-thread hotspots (MEASURED, inclusive % of main-thread samples)

| # | Frame | Incl. % | Notes |
|---|---|---|---|
| 1 | `ActiveQuestObjectiveValue::Calculate` | 80.8 | Nearly all of it is the map copy |
| 1a | └ questGuidpMap copy-construct | 68.8 | GuidPosition copy ctor, hashtable `_M_assign`, jemalloc |
| 1b | └ questGuidpMap destroy | 15.3 | also includes the finisher copy below |
| 2 | `Runtime::BuildLiveQuestFrame` | 25.1 | caller of #1 |
| 3 | `Runtime::EnsureQuestDirective` | 19.4 | caller of #1 |
| 4 | `NativeDispatch → NewRpgDoQuestAction::Execute` | 17.6 | caller of #1 (DoIncompleteQuest 16.7) |
| 5 | `ActiveQuestFinisherValue::Calculate` | 3.5 | same copy pattern (QuestValues.cpp:715) |

Rows 2-4 are callers and overlap row 1; they are not additive. Travel, pathing and navmesh work on the world thread is about 1% (TravelNodeMap::getFullPath 1.0%, dtNavMeshQuery::findPath 0.25%). On the map threads, the top frames are core creature/movement/vmap work (Creature::Update about 4.6%, malloc/jemalloc about 4%). Bot engine, trigger and value work there is individually below 0.5%.

### Call rates (MEASURED, 30 s uprobes)

- `ActiveQuestObjectiveValue::Calculate`: 1148 calls, **38.3/s**
- `ProcessConfiguredBot`: 324 calls, **10.8/s** (about 11 bots per Oracle tick)
- `Runtime::Update`: 43 calls in about 30 s. World ticks are about 1 s long, so every world tick is an Oracle tick.

### Per-bot cost (ESTIMATE)

- One Calculate ≈ 0.753 × 0.808 / 38.3 ≈ **16 ms**, assuming all calls land on the world thread. Map-thread profiles show them as negligible.
- One Oracle bot ≈ 0.61 core / 10.8 ≈ **56 ms of world-thread CPU per bot per tick** (about 3.5 Calculate calls per bot). That is 5.6% of a core per Oracle bot, all of it serialized. With 12 bots this gives the ~600-900 ms diffs.
- One non-Oracle bot is at most 0.44 core / 42 ≈ **10 ms/s**, spread across the map threads. This upper bound also includes creature AI.

## Ranked reductions

### A. No rebuild: config, testable at the next restart

Oracle config is read once: `LoadConfig` sets `configLoaded_` (AutoWowOracleRuntime.cpp:793-825), and I found no live reload. `.reload config` will not affect it. I also found no live GM or `.playerbots` knob that touches this bottleneck.

| # | Key | Current → proposed | Effect | Risk |
|---|---|---|---|---|
| A1 | `AutoWow.OracleRuntime.CadenceMs` | 1000 → 2000 (or 3000; max 5000) | World-thread Oracle CPU −50% (−67%). The spike **size** stays about the same (~600 ms for 12 bots); it just comes half as often. | Oracle reacts more slowly. Check any lease/timeout counted in `tick_`. |
| A2 | `AutoWow.OracleRuntime.MaxBots` / `BotGuids` | 12 → N | About −56 ms of world-thread CPU per bot per tick removed, linear. Bot 121 (the 1166 inventory_full loop) is **in the allowlist**, so dropping it saves one full bot pass per tick. | Fewer Oracle-driven bots. |
| A3 | `AiPlayerbot.*` knobs (ReactDelay=100, DynamicReactDelay=1, IterationsPerTick=10, RandomBotUpdateInterval=20, BotActiveAlone=100, botActiveAloneSmartScale=0) | **leave as they are for now** | These only affect map-thread bot AI, which is not the bottleneck (about 11% per thread). | Enabling `botActiveAloneSmartScale=1` now would throttle all bots because of the Oracle spike itself (floor 50 ms). Consider it only after B1. |
| A4 | `MapUpdate.Threads` | 4 → keep | MapUpdater parallelizes per map. Bots on 2 continents keep only about 2 threads busy, so more threads will not help. | — |

### B. Needs rebuild (batch for the ~04:00 window)

**B1: stop copying questGuidpMap. Biggest win, low risk.**
- `src/Bot/Engine/Value/Value.h`, `SingleCalculatedValue` (~line 144-167): add `T& RefGet() override { Get(); return this->value; }`. This is required: the inherited `CalculatedValue::RefGet` with checkInterval 1 would **recompute**, which means a full world scan.
- Replace each by-value copy with `questGuidpMap const& questMap = sSharedValueContext.getGlobalValue<questGuidpMap>("quest guidp map")->RefGet();` at:
  - QuestValues.cpp:127, :208, :268, :359, :715
  - Mgr/Travel/TravelMgr.cpp:2059
  - AutoWow/AutoWowBridge.cpp:4005-4006
- Saving (ESTIMATE): about 84% of world-thread samples, roughly 0.6 core at 12 Oracle bots. Oracle-bot cost falls from about 56 to about 9 ms per tick, and the Oracle spike from about 600-900 ms to about 100 ms.
- Risk: low. The value lives in a process-lifetime singleton and is read-only after the first compute. The first-compute race already exists today; pre-warming it at startup (a `Get()` in `OnBeforeWorldInitialized`) closes it.
- Test seam: a counting fake `SingleCalculatedValue<int>`. Assert that N× `RefGet()` gives exactly 1 `Calculate`, and that the reference is stable. Add a golden test that `ActiveQuestObjectiveValue` returns the same spec before and after the change.

**B2: memoize "active quest objective" (and "active quest finisher") per bot state.**
- Location: QuestValues.h:272/:283, where checkInterval=1 means no cache.
- Change: cache the spec keyed on (questId, rpgInfo generation, CreatureOrGOCount[4], ItemCount[4], item-count stamp), so `Get()` only recomputes when the key changes. Keep the explicit `Reset()` calls as they are.
- Saving (ESTIMATE): about 3.5 → about 1 compute per bot per tick, so roughly −70% of what is left after B1. It also cuts the map-thread callers (GrindTargetValue.cpp:47, LootAction.cpp:25/113/432, ChooseTravelTargetAction.cpp:52).
- Risk: medium. A missing key field would give a stale spec. The seam is a pure `ObjectiveKey Make(bot, quest)` function, plus a test that any counter change invalidates the cache.

**B3: stagger the Oracle round-robin.**
- Location: `Runtime::Update`, AutoWowOracleRuntime.cpp:2316-2336.
- Change: process `ceil(maxBots × diffMs / cadenceMs)` bots per world tick from a rotating cursor, instead of all of them at the cadence edge.
- Saving: total CPU unchanged, but the world-tick spike becomes about 1/K. This is the step that makes 100+ Oracle bots possible without multi-second diffs.
- Risk: low-medium. Per-bot tick semantics need to move from a global `tick_` to per-bot last-processed time. Test seam: a pure selector `(cursor, count, budget) -> slice` with fairness assertions.

**B4: pass the spec through a pass instead of re-Getting it.**
- Location: the Oracle `ProcessConfiguredBot` → `EnsureQuestDirective` / `BuildLiveQuestFrame` / NativeDispatch chain (:853, :915, :1171, :1938, :2016, :2236).
- Change: compute the spec once per bot pass and pass it along. This is a smaller alternative to B2 that touches only the Oracle.
- Risk: low (only frames that already `Reset()` need a recompute).

**B5: design call, not recommended yet.** Move per-bot Oracle work onto the bot's map-thread `UpdateAI`. It is world-thread only by design (comment at :1951), so this is high risk.

## Projected capacity (ESTIMATE)

- **Today:** 12 Oracle bots ≈ 0.6 core serialized, and each extra Oracle bot adds about 56 ms to the world tick. More than about 18 Oracle bots pushes the tick past 1 s.
- **After B1:** about 9 ms per Oracle bot per tick, so about 100 Oracle bots per 1 s tick. With B1+B2, about 3 ms, so 200+ Oracle bots fit in about 0.6 s/s. B3 is needed to keep p99 diff low.
- **Non-Oracle bots:** at most 10 ms/s each on map threads, bounded per continent. 200 bots is about 1 s/s per continent at worst, which is borderline. Re-profile after B1.

## Re-profile recipe

Use `perf record -F 99 --call-graph dwarf,65528` for full roots, or rebuild with `-fno-omit-frame-pointer` and use `-g`. uprobes: add a `p:aw/<name> <bin>:<file-offset>` line to `/sys/kernel/tracing/uprobe_events` (for this binary, text VA equals the file offset), then run `perf stat -e 'aw:*' -p PID`.
