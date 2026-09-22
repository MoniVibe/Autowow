# Bulk Zone Soak Plan (30 -> 80 -> 200 bots)

Status: PLAN ONLY. Written 2026-09-23 from a read-only investigation. Nothing was started,
stopped, edited, built, or committed. No MySQL session was opened. Every config value quoted
below is a non-secret key read from the live runtime file; no credentials are reproduced.

Evidence classes used in this document:
- **SOURCE** = read in `/root/autowow-advisor-t1-module` @ `8cd116c4` (branch main, WSL Ubuntu-24.04).
- **CONFIG** = read in the live runtime config `/usr/local/etc/modules/playerbots.conf` (mode 0600)
  or `/root/p1runtime/worldserver.conf` (the WSL world launcher points at these:
  `scripts/start-phase1-wsl-worldserver.ps1:13,40`).
- **MEASURED** = a prior recorded run (named).
- **UNMEASURED** = an estimate. Treat it as a hypothesis the ramp must check.

Goal (owner): 2-3 bots per zone per faction across every continent (about 150-250 bots). Each
bot tries its zone's quests headless. Collect one outcome per (bot, quest). Fix failures by
objective family, then re-run idempotently.

Rule for this soak: **level, gear, spells, and the first placement are SETUP (allowed).
Objective credit, loot, turn-ins, and rewards must be earned through normal game actions.**
Any teleport after setup is recorded as an incident. It is never counted as walking proof.

---

## 1. Which bots run the AutoWoW NewRpg improvements?

**Short answer: both populations run the modified NewRpg quest phase machine. The travel
fixes, Oracle lease/dispatch, and forced activity apply only to allowlisted or
independent-armed bots.**

### 1a. Shared by every bot running NewRpg (stock random bots included)
- The phase machine is shared. An unmanaged bot takes the `Ordinary` authority path and reaches
  the same `DoIncompleteQuest` / `DoCompletedQuest` as a managed bot
  (`src/Ai/World/Rpg/Action/NewRpgAction.cpp:686-703`). The dispatch policy returns `Ordinary`
  when no Oracle authority is expected (`src/AutoWow/OracleQuestDispatchPolicy.h:65-68`).
- Ungated improvements on that path:
  - typed `QuestFailureReason` blocking (`src/Ai/World/Rpg/QuestObjectiveContext.h:161-192`,
    `NewRpgAction.cpp:719-741`)
  - source rotation (`NewRpgAction.cpp:862-921`)
  - source-stall policy (`NewRpgAction.cpp:2033-2039`)
  - new `rpg sell` / `rpg train` triggers (`src/Ai/World/Rpg/Strategy/RpgStrategy.cpp`,
    diff vs upstream `93aaea3d`).
- Stock enablement: `AiPlayerbot.EnableNewRpgStrategy = 1` (CONFIG playerbots.conf:1054).
- Unmanaged bots keep the legacy questgiver scan and quest-log maintenance. Managed bots are
  denied it on purpose (`OracleQuestDispatchPolicy.h:85-87`, `NewRpgAction.cpp:683-688`).

### 1b. Gated by `IsAutoWowTravelBot()` = independent flag OR Oracle allowlist
- The gate: `NewRpgBaseAction.cpp:204-208` (`botAI->IsAutoWowIndependentParty() ||
  AutoWowOracleRuntime::IsManagedBot(guid)`).
- What it gates:
  - A stuck bot holds and replans with no teleport. Stock random bots **teleport when stuck
    while roaming** (`NewRpgBaseAction.cpp:426-456`). Quest-path moves hold for everyone
    because `questNoTeleport` is set.
  - Failed-destination cooldown and replan (`NewRpgAction.cpp:444-455, 475-486`;
    `NewRpgBaseAction.cpp:210-224`).
  - Grounded grind and camp destination picks (`NewRpgBaseAction.cpp:1650, 1716`).
  - Destination-progress watching (`NewRpgBaseAction.cpp:349-360`).

### 1c. Gated by the Oracle allowlist only (`IsManagedBot`)
- `IsManagedBot` = `AutoWow.OracleRuntime.Enabled && guid in AutoWow.OracleRuntime.BotGuids`
  (`src/AutoWow/AutoWowOracleRuntime.cpp:1498-1502`, config load at `:794-809`).
- Live values: Enabled=1. BotGuids = 12 GUIDs (7,10,101,112,121,123,139,144,154,166,236,244).
  MaxBots=12. CadenceMs=1000. ZoneTravelAssist=1 (CONFIG playerbots.conf:2370-2374, 2388).
- Hard ceiling: 256 GUIDs (`AutoWowOracleContract.h:94` `kMaxBotLeases = 256`, validated at
  `AutoWowOracleRuntime.cpp:471-476`). An invalid allowlist **disables the whole runtime**
  (`:805-809`).
- What it gates:
  - in-process quest selection, lease, and tagged dispatch (`Runtime::Update`
    `AutoWowOracleRuntime.cpp:2309-2327`, `ProcessConfiguredBot` `:1942+`, called from
    `src/Script/Playerbots.cpp:437`)
  - deferral of unrunnable quests (`:923-935`)
  - Route V2 (`NewRpgAction.cpp:1379-1381, 2108, 3025`)
  - same-zone travel assist (a teleport assist; record it as such)
  - receipt export (JSONL, 32 MiB x 8 files, `AutoWowOracleReceiptStore.h:28-30`,
    dir `logs/autowow-oracle`)
- **No external director is needed.** The runtime runs on the world thread.

### 1d. Gated by the independent flag only (bridge `independent` order)
- **Forced activity** (`src/Bot/PlayerbotAI.cpp:4720-4725`). Without it a bot is subject to
  `BotActiveAlone = 10` (only about 10% of bots are active when no real player is near) plus
  SmartScale (CONFIG playerbots.conf:921-962).
  - **An allowlisted but un-armed bot is still throttled.**
- Per-bot no-teleport policy (`AutoWowBridge.cpp:2780-2784`).
- Arming requires a row in `autowow_league_member` (`AutoWowBridge.cpp:684-689`) and a solo,
  out-of-combat bot. The flag is not persisted across login. It is set per session through the
  bridge.

### 1e. Both populations live in RandomPlayerbotMgr
- The bridge `activate` logs a bot in with `sRandomPlayerbotMgr.AddPlayerBot`
  (`AutoWowBridge.cpp:2734`). `list` iterates `sRandomPlayerbotMgr.GetAllBots()` (`:2662-2680`).
- `RandomPlayerbotMgr::ProcessBot` applies to any bot on an rndbot account
  (`RandomPlayerbotMgr.cpp:2117-2127`). It **auto-revives, repairs, and random-teleports dead
  bots** after 60-300 s (`:1461-1478`, `:1566-1576`, CONFIG :1387-1388). It re-randomizes
  level and gear when `randomize` expires (`:1504-1537`). It teleports idle bots for their
  level (`:1547-1556`). No AutoWow exclusion exists in that function.
- **To verify with a read-only SQL query:** whether the six scouts' accounts are in the random
  account list.

## 2. Getting 2-3 level-appropriate bots per zone per faction

### Recommendation: a hybrid with the least custom work
Stock random bots create and place the population (SETUP). A GUID allowlist promotes them to
managed. No fixture factory is needed.

**Step A: stock population (config only, SETUP).** Proposed values; none are applied.

| Key (CONFIG line) | Live | Soak | Why |
|---|---|---|---|
| `MinRandomBots` / `MaxRandomBots` (:88-89) | 0 / 0 | N / N | population size |
| `RandomBotAccountCount` (:96) | 0 (auto) | 0 | auto-sizes accounts |
| `RandomBotMinLevel` / `MaxLevel` (:704-705) | 80 / 80 | 5 / 80 | **live value would put every bot in Northrend** |
| `RandomBotMaxLevelChance` (:775) | 1.0 | 0.02 | stop pile-up at max level |
| `RandomBotAllianceRatio` / `HordeRatio` (:730-731) | 50/50 | 50/50 | faction balance |
| `AutoTeleportForLevel` (:1256) + `ZoneBracket.*` (59 zones, :1088-1209) | 1 | 1 | first placement by level (SETUP) |
| `RandomBotMaps` (:1221) | 0,1,530,571 | same | all four continents |
| `Min/MaxRandomBotRandomizeTime` (dist :1378-1379) | default 7200 s .. 14 d | > soak length | **a re-roll mid-soak wipes level and gear** |
| `Min/MaxRandomBotTeleportInterval` (dist :1392-1393) | 3600-18000 | > soak length | stops periodic level teleports |
| `Min/MaxRandomBotReviveTime` (:1387-1388) | 60 / 300 | > 1800 | lets the bot corpse-run instead of revive plus random teleport |
| `Min/MaxRandomBotInWorldTime` (dist :1371-1372) | 600-28800 | > soak length | no logout rotation |
| `BotActiveAlone` / `botActiveAloneSmartScale` (:921, :959) | 10 / 1 | 100 / 0 (or arm independent) | **otherwise about 90% of bots sit idle** |
| `RandomBotQuestIds` (:974) | 14 pre-completed chain quests | keep | stock prerequisite setup; list them in the ledger as `setup_precompleted` |

- The initial `Randomize` (level, gear, spells) plus `RandomTeleportForLevel` counts as SETUP.
- A later `randomize`, `teleport`, or `revive` event during the soak is **contamination**.
  Record it as an incident and exclude the bot's open quests from pass/fail.

**Step B: fill zone quotas.** Stock placement is level-weighted, not quota-driven
(`TravelMgr.cpp:4507`, bracket table `:4625+`). It will not guarantee 2-3 bots per zone per
faction.
- Cheapest path: overprovision by about 30% and accept the histogram. Then run a small setup
  pass for empty cells:
  - pick surplus bots of the right faction
  - set their level to the zone bracket midpoint (SETUP; GM `.character level` or
    `fixture init`, which needs `AutoWow.FixtureGuids`, `FixtureFactoryControl.h`)
  - place them at the zone's hub (SETUP; the bridge `travel <guid> random` calls
    `RandomTeleportForLevel`, `AutoWowBridge.cpp:5101`)
- Zone catalog = the 59 `ZoneBracket` zones. Faction-exclusive starting zones only need one
  faction's cells. That gives about 100 cells, so 2-3 bots per cell is 200-250 bots.

**Step C: promote to managed (optional but recommended).**
- Write the chosen random-bot GUIDs into `AutoWow.OracleRuntime.BotGuids` and set `MaxBots`
  to their count (≤ 256). Restart the world.
- This gives managed bots the no-teleport travel, lease-based selection, and receipts.
- Arming them independent (for forced activity and the no-teleport policy) needs
  `autowow_league_member` rows. **That is a DB write.** The alternative is
  `BotActiveAlone=100`, `SmartScale=0` from Step A.
- A/B value: keep half of each zone cell as stock random bots (control) and half as managed
  (treatment). The same ledger then shows what the AutoWoW gates actually add.

**Rejected: AutoWoW fixture-per-bot creation for 200 bots.**
- It needs account and character creation outside the stock manager, league rows, and fixture
  allowlist growth.
- It is more work for the same SETUP result.
- Keep fixtures for filling individual empty cells only.

## 3. Per-quest outcome capture

### Existing telemetry and its cost at scale

| Source | What it gives | Cost / limit |
|---|---|---|
| Bridge `127.0.0.1:18787` (`AutoWowBridge.cpp:6050-6089`) | `list` (snapshot of every bot), `questlog`, `questobjective`, `snapshot`, `oraclelog`, `acceptance` | Thread per connection. Every request queues onto the world thread (queue cap 10000, **100 ops per world update**, `PlayerbotWorldThreadProcessor.h:111-112`), with a 2 s timeout (`AutoWowBridge.cpp:118, 5733-5751`). `list` serializes all N bots in one world-thread op. |
| Zone-scout collector (`scripts/zone-scout-monitor.ps1`) | `samples.jsonl` and `incidents.jsonl` (stall, no-credit, death, offline) | Per poll: 1 `list` + **2 calls per bot** (`:120-131`). Each call runs `autowow-control.ps1`: new TCP connection, 5 s timeout (`:64-72`). Sequential. **200 bots = 401 round trips per poll.** Each waits at least one world tick. The observed 6-bot cadence was already "5 s plus query time" (Overnight-Report:43). At 200 bots this is roughly 20-60 s per cycle (UNMEASURED) and adds about 80 bridge ops/s of world-thread work. **Does not scale. Retire it for the soak.** |
| `capture-zone-scout-progress.ps1` | saved rewarded quests | Runs `mysql.exe` against `character_queststatus_rewarded`. GUIDs are hard-coded to the 6 scouts (`:38`). Rows lag by up to `PlayerSaveInterval = 900000` ms (15 min). Keep it only as an **end-of-run reconciliation** query, never for polling. |
| Oracle receipt store | per-decision receipts: quest ref, before/after evidence counters, reason, planner mode (`AutoWowOracleReceiptStore.h:44-81`) | Managed bots only. Durable JSONL, rotation 32 MiB x 8 files. At 200 bots at 1 Hz the 256 MiB retention can roll over during a long run (UNMEASURED). Ingest or copy it off between checkpoints. |
| Native reward confirmation | `core_reward_confirmed` in the bridge quest view (`AutoWowBridge.cpp:536-675`) | Pull-only through the bridge, so it has the same cost as above. |
| `BlockQuest` failure reason | typed reason and phase (`NewRpgAction.cpp:719-741`) | Logged at **DEBUG** only (`:723`). Invisible at soak log levels. |

### Minimum change: one ledger row per (bot, quest)
The ledger is push-based (log-derived) and folded offline. This is one small module change. It
is not built.
1. In the existing `PlayerbotsPlayerScript` (`src/Script/Playerbots.cpp:99`), add three core
   hooks. They exist in this core
   (`autowow-advisor-t1-core/.../PlayerScript.h:249, 744, 752`):
   - `OnPlayerQuestAccept` -> `accepted`
   - `OnPlayerCompleteQuest` -> `rewarded`
   - `OnPlayerQuestAbandon` -> `abandoned`

   Emit only for bots: one `LOG_INFO("autowow.ledger", ...)` line with fields
   `run_id, bot_guid, faction, level, quest_id, zone_id, map, event, objective_counters[4], ms`.
2. Raise `BlockQuest` to the same logger as event `blocked`, adding `reason` and `phase`
   (`NewRpgAction.cpp:719`).
3. Add the same line where a quest is set aside or dropped: event `deferred`.
   - runtime deferral: `AutoWowOracleRuntime.cpp:923-935`
   - executor `lowPriorityQuest.insert`: `NewRpgAction.cpp:732`
   - drop-quest path: `NewRpgBaseAction.cpp:1290-1300`
4. Route the `autowow.ledger` logger to its own file appender **on the WSL ext4 filesystem**.
   Do not use `/mnt/d` (see risk 4e).
5. Write an offline reducer (one PowerShell or Python script).
   - Fold events into rows keyed by (run_id, bot_guid, quest_id):
     `accepted_at, first_progress_at, last_counters, rewarded_at | abandoned_at | deferred(reason) | blocked(last reason, count), outcome, stall_signature`.
   - `stall_signature` = `(last reason, phase, zone, 50-yard position bucket)`. Bucket
     identical signatures across bots, then group by objective family (kill / item-drop /
     gameobject / use-item / escort / event / finisher).
   - `outcome` values: `rewarded | open_progressing | blocked | deferred | abandoned | contaminated`.
   - Mark `contaminated` from randombot `revive`/`teleport` events and from travel-assist
     teleports.
   - Re-run idempotency: the key includes `run_id`. The reducer is pure over log files.
     Reconcile against one `character_queststatus_rewarded` query at run end.

Skipped: live dashboards and bridge polling. Add a sparse monitor only if needed: `list` once
per 60 s for liveness and nothing per bot.

## 4. Scaling risks

| # | Risk | Evidence | Assessment |
|---|---|---|---|
| a | **World update time with 200 all-active bots** | No diff was ever measured. The 40-bot WSG and 80-bot AV tests recorded memory only (`PLAYERBOTS_LOAD_TEST_40_REPORT.md`, `PLAYERBOTS_LOAD_TEST_REPORT.md`, both PASS, 2026-07-12, **Windows worldserver**, not this WSL build). `MapUpdate.Threads = 4` (worldserver.conf:1344), and there are only 4 continent maps, so parallelism is capped. `BotActiveAlone=100` is the real stressor. | **Top risk, unmeasured.** |
| b | Oracle runtime per-tick spike | `Runtime::Update` processes **every** allowlisted bot in one world tick each `CadenceMs` (`AutoWowOracleRuntime.cpp:2320-2327`), with no staggering. Proven at ≤ 12 bots only. | Watch for a 1 Hz sawtooth in diff at 80 and 200. The fix, if needed, is round-robin slicing: process `maxBots/k` bots per tick. |
| c | Memory | MEASURED (Windows build): 3.91 GiB at 40 bots, 4.05 GiB at 80. Data on disk: mmaps 2.1 GB, vmaps 657 MB, maps 295 MB (`/root/p1data`). Grids load lazily (`PreloadAllNonInstancedMapGrids = 0`, :1424). 200 bots spread over about 100 zones load far more grids than 80 bots in one AV instance. | UNMEASURED estimate: 6-9 GiB world RSS at 200. |
| d | WSL 15 GB cap | `free -g` inside WSL: 15 total, 4 swap. No `C:\Users\shonh\.wslconfig` exists (default is 50% of 32 GB host). MySQL runs on **Windows** (`third_party\mysql\bin\mysqld.exe`), not inside WSL. | **No `.wslconfig` change is needed for 30 or 80.** Decide at the 80 checkpoint: if RSS at 80 spread bots exceeds 5 GiB, projected 200 exceeds about 11 GiB and the cap becomes a risk. Report only; do not change it now. |
| e | DB write load and path | World talks to Windows MySQL through a **PowerShell TCP relay** (`scripts/phase1-wsl-mysql-relay.ps1`, 13306 -> 127.0.0.1:3306). `CharacterDatabase.WorkerThreads = 1`, `WorldDatabase = 1` (:135-136), `PlayerbotsDatabase.WorkerThreads = 1` (playerbots.conf:2148). Randombot event values (`SetEventValue`) and player saves (every 15 min) all cross that relay. | Unmeasured. Watch for relay CPU and async-queue lag (worldserver `sql` log warnings). The relay is the likeliest DB chokepoint, not MySQL itself. |
| f | Log I/O on 9P | `LogsDir = /mnt/d/...` (:189). A synchronous appender writing through the WSL-to-Windows filesystem bridge at 200 bots can stall the world thread. | Put the ledger appender and any per-bot logging on ext4. Keep console and file log levels at INFO. |
| g | PowerShell collector | See section 3. | Retire it for the soak. |
| h | Stock randombot interference | revive + random teleport, re-randomize, level teleports (section 1e) | Solved by config (section 2A). Verify that no `randomized` or `teleport for level` lines appear after setup. |
| i | Bridge thread-per-connection | `AutoWowBridge.cpp:6089` detaches one thread per client | Fine at low call rates. Harmful only with the old collector. |

## 5. Ramp plan

Preconditions (owner go needed; these are config and DB mutations):
- Section 2A config applied to a **copy-then-swap** of `playerbots.conf`. Back up first.
- Ledger change (section 3) built, unit-tested, and deployed. The old collector is stopped.
- Measurement kit, all inside WSL, sampled every 60 s:
  - `ps -o rss,pcpu -p <worldserver>`
  - `free -m`
  - `.server info` through the console or SOAP for update-diff mean/median/percentiles
    (verify the output format on this build)
  - `RecordUpdateTimeDiffInterval` 300000 -> 60000 and `MinRecordUpdateTimeDiff` 100 -> 0
    (worldserver.conf:566, 573) for a logged series
  - relay process CPU on Windows
  - ledger event rate

Each step is one named profile: `soak-<N>-<mix>`, where mix = % managed vs stock. Record the
worldserver binary path and module head `8cd116c4` (or its successor) with every run.

| Step | Population | Duration | Measure | Pass to next step |
|---|---|---|---|---|
| **S1: 30** | 10 zone cells (5 per faction, spread over 4 continents) x 3; half managed | 2 h after warm-up (discard the first 15 min of logins) | diff P50/P90/P99; RSS and slope; relay CPU; ledger rows; contamination count; Oracle 1 Hz spike (P99 vs P50) | diff P99 < 100 ms; RSS slope < 100 MiB/h after warm-up; ≥ 1 `rewarded` row per cell; 0 unexplained teleports; the ledger reconciles with the end-of-run SQL rewarded set (≥ 95% match, allowing save lag) |
| **S2: 80** | about 27 cells x 3, all continents; half managed | 4 h | same, plus a memory projection to 200 (RSS per bot beyond the 30-bot baseline) | diff P99 < 150 ms; P50 < 60 ms; projected 200-bot RSS < 11 GiB (otherwise decide on `.wslconfig` with the owner); no world-thread queue-full or relay errors |
| **S3: 200** | about 100 cells x 2-3 | 8 h (overnight), then fix-and-rerun cycles | same; plus per-family failure histogram from the ledger | Soak output = ledger + family histogram. It is not a pass/fail on questing. |

**Abort criteria (any step, act at once):**
- diff P99 > 300 ms for 5 consecutive minutes, or any single diff > 2000 ms. SmartScale's
  200 ms ceiling would idle all non-forced bots.
- WSL `MemAvailable` < 2 GiB, or swap use > 1 GiB, or RSS > 11 GiB.
- Worldserver crash, world-thread `world_thread_queue_full`, or repeated relay disconnects.
- Contamination > 5% of bots: randombot re-randomize, level teleport, or manager revive after
  setup.
- The ledger falls silent while bots are online (logger misrouted).

Abort action: `Park` managed bots through the existing `revive-autowow.ps1 -Action Park -Apply`
pattern, set `MaxRandomBots` back to 0, and keep the logs.

**Fix loop (after S3):** rank failure families by the count of `(stall_signature, family)`
rows. Fix one family per lane. Re-run the same profile with a new `run_id`. The reducer
compares runs on the same (zone, quest) keys.

---

## Top blockers (in order)
1. **Activity throttling.** `BotActiveAlone=10` plus SmartScale leaves about 90% of stock and
   un-armed managed bots idle. The soak needs `BotActiveAlone=100 / SmartScale=0` (maximum
   load) or independent arming (DB rows). The chosen option sets the whole load profile.
2. **No per-quest ledger exists, and the collector cannot scale.** Failure reasons are
   DEBUG-only, and the collector makes 2 bridge calls per bot per poll. The section 3 hook
   change is required first.
3. **Update time and memory at 200 are unmeasured on this WSL build.**
   - The prior load tests were on the Windows worldserver and recorded memory only.
   - The Oracle runtime processes all managed bots in one tick at 1 Hz.
   - DB traffic crosses a PowerShell relay with 1 worker thread per DB.
   - Hence the ramp.

Also required: the randombot re-randomize, level-teleport, and auto-revive timers must be pushed
past the soak window (config only). Otherwise objectives stay earned but zone attribution and
walking evidence are contaminated.
