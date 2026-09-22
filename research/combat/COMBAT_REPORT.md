# AutoWoW combat / DPS efficiency: science lane report (2026-09-23)

Scope: how efficiently bots fight, the largest fixable losses, and the S2 measurement design.
Mode: read-only against the live soak `soak-s1-30-r1` (worldserver PID 33653, binary f1ae2cf1,
module main c2a14f15). I made no server, config, bridge, or git changes. The only bridge traffic
was 32 read-only `combatlog` calls spaced at least 11 s apart, plus one read-only `SELECT` against
`acore_characters` for the class roster.

Evidence classes used below:
- **[M] measured**: counted directly from a named artifact.
- **[S] source**: read from code at c2a14f15 (file:line).
- **[H] heuristic**: a model or a reference with an assumption, stated each time.
- **[n=small]**: too few samples to support a claim. Treat these as direction only.

---

## 1. Telemetry inventory

| Surface | What it records | Gate | Where it lands | On now? |
|---|---|---|---|---|
| `AutoWowCombatPerformanceTelemetry` (`src/AutoWow/CombatPerformanceTelemetry.{h,cpp}`) schema `autowow.combat-counters.v1` | Per-bot rolling window: damage done (`UnitScript::OnDamage`, pre-absorb event amount, pet/controlled units attributed to owner), effective healing (`OnHeal`), damage taken, deaths, combat entries/exits, threat samples (1 Hz max), max threat | **None.** It is compiled in and registered in `Playerbots.cpp:629`. There is no config key. | In memory only (max 256 bots). You read it through the bridge `combatlog <guid>` nested `counters`. Nothing is written to disk. | **Yes, ON** [M: every sampled bot returns `available:true`, `next_hook:"server combat event hooks active"`] |
| `AutoWowCombatTelemetry` (`CombatTelemetry.{h,cpp}`) schema `autowow.combat-telemetry.v0` | Instantaneous: alive/death state, in_combat, HP/mana/power, role, victim, ai_target, healer target/intent, threat links, active spell, **last spell id+target+time (s)** | None | Bridge `combatlog <guid>` (world-thread, read-only) | Yes |
| `AutoWowEncounterTelemetry` (`EncounterTelemetry.{h,cpp}`) schema `autowow.encounter-telemetry.v0` | Group encounter snapshot: hostile units within 250 yd (capped at 64), engagement and threat aggregation per member | None | Bridge `encounterlog <guid>` | Yes |
| Quest ledger (`autowow.ledger`) | Quest lifecycle events. Contains no combat fields. | `AutoWow.Ledger.Enable=1`, `AutoWow.Ledger.RunId` | `/root/autowow-soak/logs/ledger.log` | Yes |
| Playerbots PerfMonitor | AI action/trigger timing (CPU cost, not combat outcome) | `AiPlayerbot.PerfMonEnabled` (config) or live console `.playerbots pmon toggle` | Playerbots log | Off (default) |
| `rndbot stats` | Population counts: dead / in combat / moving / resting flag / mounted / engine state / NewRpg status | Live console `.playerbots rndbot stats` | Playerbots log | On demand. **Side effect:** it resets every bot's `rpgStatistic` (`RandomPlayerbotMgr.cpp` in PrintStats) |
| `soak-metrics.sh` | World RSS/CPU/last diff | none | `/root/autowow-soak/metrics-<run>.jsonl` | Yes (running) |
| Bridge `list` snapshots | guid, level, xp, copper, pos, alive, combat, state | none | `work/soak-20260923/snap-soak-s1-30-r1.jsonl` (about every 5 min) | Yes |

**Answer to "can combat telemetry be enabled by config at next restart?"** It is already on and has no key.
Its limits come from the code, not from config:

1. **The window is ephemeral** [S `CombatPerformanceTelemetry.h:27-28`, `.cpp:148-156`]. A window is retired
   after 30 s without a combat event and deleted outright. Nothing keeps the previous window and nothing
   accumulates lifetime totals. A poller has to hit the bot during the fight or within 30 s after it.
   In my live sample, 13 of 32 calls found a live window.
2. **`combat.entries` is structurally always 0 for players** [S+M]. The core fires
   `ScriptMgr::OnUnitEnterCombat` only from `Creature.cpp:2908`, while `OnUnitExitCombat` fires from
   `Unit.cpp:7592` for every unit. Live, all 13 tracked bots show `entries:0` with `exits` of 1-3.
   Any time-in-combat or kills-per-entry metric built on `entries` is therefore wrong.
3. `window_ms` includes up to 30 s of out-of-combat tail. So `damage_done/window_ms` gives
   *sustained* DPS (combat plus short downtime), not active DPS.
4. It has no kill counter, no cast/GCD counter, no rest/drink time, and no per-target TTK.

---

## 2. Current evidence

### 2a. July dungeon captures (5-man, level 80). Profile: `phase1-20260714 dungeon_smoke` (tank DK 2, disc/holy priest 26, fire mage 8, combat rogue 45, BM hunter 24). About 1.24 s sample interval; counters were not yet available.

| Capture | Duration | Outcome [M] |
|---|---|---|
| `keleseth-combat-20260714-083051` | 120 s | Keleseth 133,820 → 0 HP in 77.1 s = **1,736 group HP/s**. PASS, 0 deaths |
| `uk-combat-smoke-shared-20260714-080237` | 120 s | 5 of 7 Dragonflayer trash (35,928 HP each) killed. Per-mob group HP/s **894 – 2,851** (median ≈1,900). 0 deaths |
| `ingvar-combat-20260714-091600` | 179 s | Ingvar went 64,114 → 73,419 (net heal/phase). **Full wipe**, all 5 dead |
| `uk-combat-smoke` | 2×120 s | 4 of 5 dead, tank ownership 0.26 |

- **Versus the in-code reference [H].** The module has its own level-scaled DPS model,
  `EstimatedGroupDpsValue` (`src/Ai/Base/Value/EstimatedLifetimeValue.cpp:26-120`). It gives 3,200/DPS at
  level 80, a 0.3 tank multiplier, a 0.1 healer multiplier and a ×1.05 bonus for 5 players, which is
  **≈11,400 group DPS at gear-score modifier 1.0** (≈8,600 at the 0.75 floor). Observed group HP/s is
  **~1,700–1,900, about 15–22 %** of that. Two caveats: the reference is the module's own heuristic,
  and fixture gear quality was not recorded.
- **Add neglect [M].** In the Keleseth fight the Vrykul Skeletons lost only 31–52 HP/s each over 40–60 s
  while the boss took about 1,700 HP/s. Frost Tombs were burst down (190–770 HP/s). This is the intended
  priority, not a defect.
- **Cast-rate proxy [H].** "New last-spell per sample" was 0.30–0.38 for DPS in clean runs, about 0.25
  casts/s. Against a 1.5 s GCD cap (0.67/s) that suggests GCD usage **≥ ~40 %**. This is a lower bound:
  it misses multiple casts between samples, auto-attack and auto-shot, and it ignores cast times longer
  than the GCD (Fireball).
- **Hunter's Mark share [M, n=small].** Hunter 24 cast Hunter's Mark (53338) in **9 of 32** observed new
  casts in Keleseth and 7 of 29 in UK trash. This fits a known trigger defect (§4 L3).
- **Healer idle wand [M].** Priest 26's most frequent observed spell was Shoot (5019) in all runs
  (49–83 observations). The healer is idle and wanding. That is harmless, but it tells us healer mana
  was not the limit.
- The Nexus review (`logs/autowow-lab/nexus-e476-combat-review-20260717`) found **DPS stuck 124–132 yd
  away and targetless** while the tank and healer fought. That is a target-selection and assist failure,
  and the largest single loss in that run (DPS contribution ≈0).

### 2b. Live soak `soak-s1-30-r1` (36 online bots, not 42: `MinRandomBots=MaxRandomBots=36`)

**World tick [M, `Server.log` "Last 500 diffs summary", 18 windows].** Median 1–34 ms, mean 11–195 ms,
**P95 99–1,147 ms**, P99 547–1,242 ms, max 1,566 ms. The distribution is bimodal: short ticks with
periodic stalls of 0.5–1.2 s.
- **Latency model [H].** AI reaction waits for the next world update. With about 5 % of ticks at ~800 ms
  and 95 % at ~20 ms, the expected wait from a random instant is E[d²]/(2E[d]) ≈ 270 ms. That adds about
  270 ms to every GCD chain (≈15–18 % of a 1.5 s GCD). It also exceeds `ReactDelay=100`, so reaction
  latency is set by the server, not by the bot knob. This is a model; S2 must measure it (§5).

**Snapshot duty cycle [M, n=5 snapshots over ~15 min (22:38–22:53Z) per bot; profile `bridge list, soak-s1-30-r1`]:**
- Only 1–2 of 36 bots are in combat at any snapshot. The mean combat share per class is 0–13 %.
- Three deaths were seen (hunter 3, mage 18, scout priest 236). This is a lower bound, because a
  death and release between snapshots is invisible. That is ≈0.33 deaths per bot-hour pooled (3 / ≈9 bot-h); per class it is [n=small].
- The level-80 bots (22-33, 38, 39) earn no XP. Their combat is not XP-gated and they are almost never
  in combat, so they add nothing to the DPS science for questing.

**Combat counters, spot samples [M, n=1–2 per bot; profile `combatlog-sample-20260923.jsonl`, 2 passes,
16 non-scout bots]:** see the table in §2c. It shows sustained window DPS as a ratio to the heuristic reference.

### 2c. Live counter table

Profile: `soak-s1-30-r1 / combatlog spot / 2 passes 22:51–23:01Z / 16 bots`. There were 32 calls,
0 errors, and 13 with an active window. Full output is in `reduce-out-20260923.md`. Window DPS =
damage_done / window_ms. The window includes up to 30 s of idle tail, so these are *sustained*
figures and are biased low. The reference is `GetBasicDps(level)` [H], gs-modifier not applied.

| guid | class | lvl | active windows | sustained DPS | ref [H] | ratio | taken/s |
|---|---|---|---|---|---|---|---|
| 6 | DK | 62 | 1 | 112.0 | 680 | 0.16 | 12.8 |
| 16 | DK | 55 | 2 | 105.4 | 300 | 0.35 | 10.5 |
| 35 | rogue | 40 | 2 | 67.4 | 85 | 0.79 | 8.6 |
| 14 | rogue | 9 | 1 | 8.7 | 14 | 0.62 | 0.7 |
| 25 | rogue (fixture) | 8 | 1 | 22.3 | 13 | 1.71 | 0.4 |
| 12 | paladin | 11 | 1 | 7.1 | 16 | 0.45 | 1.7 |
| 18 | mage | 10 | 2 | 6.1 | 15 | 0.41 | 0.0 |
| 10 | druid (oracle) | 10 | 2 | 5.0 | 15 | 0.33 | 0.8 |
| 15 | priest | 9 | 1 | 3.8 | 14 | 0.27 | 0.0 |

Read [n=small]: the sustained ratio is 0.16–0.79, median ≈0.41, for 8 non-fixture bots. Because the
windows include idle tail, active-combat DPS is higher, and this table cannot separate "slow killing"
from "downtime between pulls". Hook C2 exists to separate them. The two outland/northrend-band DKs sit
at 0.16–0.35. That is the lowest-ratio group and the first target for per-class S2 cells.

---

## 3. Metric definitions (S2 contract)

All metrics are per bot and per (class, spec, level band). Every figure carries its profile name.

| Metric | Definition | Source (today → after hook) |
|---|---|---|
| **Combat share** | Σ in-combat ms / wall ms | today: list/combatlog `in_combat` polling (biased, coarse) → hook H1 `combatMsTotal` |
| **TTK** | Time from first bot damage on creature X to X's death, for kills credited to the bot or its group, bucketed by (mob level − bot level) and mob max HP | hook H2 (kill record) |
| **Effective DPS** | damage_done / in-combat seconds | H1 lifetime totals |
| **Sustained DPS** | damage_done / wall seconds (includes downtime) | H1 |
| **DPS efficiency [H]** | Effective DPS / `GetBasicDps(level)` × gs-modifier. Label it *heuristic reference* every time. A better reference comes later: per-class median of the top quartile in the same level band (self-referencing, no external claims). | reducer |
| **Downtime share** | (wall − combat − travel-on-taxi − rest) / wall, with **rest** split out = time with stand state SIT + food/drink aura | H1 `restMs` |
| **Deaths/h** | deaths / wall hours | counters `deaths` (lifetime under H1) |
| **Damage taken per kill** | damage_taken / kills | H1+H2 |
| **Resource starvation** | Share of combat time with mana < `LowMana` (15 %) or rage/energy/runic < spell cost; plus "entered combat below 50 % HP or mana" rate | H1 `lowResourceCombatMs`, `pullsBelowHalf` |
| **GCD utilization** | Casts-on-GCD × 1.5 s (haste-adjusted if we care) / combat seconds; **idle GCD** = combat time with no cast, no channel and GCD not running | H3 cast counter |
| **Wasted casts** | DoT/debuff casts whose target died before 50 % of the aura duration; mark casts on targets < 20 % HP | H3 + H2 join |

---

## 4. Class/rotation audit: ranked suspected losses

Classes present live (DB `characters.class`): warrior 4, paladin 4, hunter 3, rogue 4, priest 3, DK 3,
shaman 4, mage 3, warlock 3, druid 5 (scouts included). Ranked by expected share of realised DPS lost,
multiplied by breadth:

| # | Loss | Evidence | Est. impact | Fix class |
|---|---|---|---|---|
| **L1** | **DPS targetless or out of range in groups.** DPS parked 124–132 yd away with no target while the tank fights | [M] Nexus review; July `ai_target` null 18–36 % of samples even in clean runs | ≈100 % loss in affected pulls; highest severity for dungeons | code (assist target, see RAID/dungeon lane) |
| **L2** | **World-tick stalls of 0.5–1.2 s at P95** add reaction and GCD-chain latency | [M] tick summaries + [H] ≈270 ms/decision model | ≈15 % of GCD capacity, every class (model) | infra/perf (not an AI fix) |
| **L3** | **DoT/mark triggers bypass the lifetime check.** `IsActive() override { return BuffTrigger::IsActive(); }` skips `DebuffTrigger`'s alive check and the `needLifeTime` gate (`GenericTriggers.cpp:279-287`) | [S] Hunter mark `HunterTriggers.h:151-152` (prio 29.5, `GenericHunterStrategy.cpp:62`); Hunter `:159`; Mage `MageTriggers.h:212,219` (living bomb, …); Shaman flame shock `ShamanTriggers.h:224`; Druid moonfire/insect swarm `DruidTriggers.h:96,109`; Warlock `WarlockTriggers.h:198-267` (immolate, all curses) | One wasted GCD per short-lived target. Solo trash lives about 10–20 s, so roughly **5–10 % of GCDs** for hunter/warlock/moonkin/elemental [H]; [M, n=small] 28 % of hunter casts were Mark in Keleseth | code: restore `DebuffTrigger::IsActive()` for these, or gate on `estimated lifetime > duration/2` |
| **L4** | **Rest thresholds with food cheat OFF.** `AiPlayerbot.BotCheats="taxi,raid"`, so non-cheat `UseFoodStrategy` eats only below **45 % HP** and drinks only below **15 % mana** (`UseFoodStrategy.cpp:22-23`). Bots re-pull at 46 % HP / 16 % mana. | [S] + [M] 3 deaths (levels 10–21) in ~15 min × 36 bots ≈ 9 bot-h, i.e. ≈0.33 deaths/bot-h pooled | Each death costs a corpse run of roughly 1–3 min [H]; likely the dominant loss for levels 1–20 (unmeasured until C2) | config A/B (see K1) |
| **L5** | **Grinding default actions sit to eat/drink at any deficit** (`DrinkAction::isUseful` = mana < 100, `EatAction` = HP < 100; prio 4.2/4.1 > `attack anything` 4.0, `GrindingStrategy.cpp:14-15`) with real items. The AI is not blocked (non-cheat path uses the item), but a food cast is about 18–30 s per sit | [S] | Downtime inflation in grind mode only; size unknown (needs H1 `restMs`) | code: threshold such as HP < 70 or mana < 50 |
| **L6** | **Low-level spell gaps**: level 7–12 bots have few spells, so the strategies fall through to melee or wand (e.g. mage 18 last spell 122 Frost Nova; priest 15 Smite) | [M] spot samples | Mostly unavoidable; in scope only if a rotation stalls | observe via H3 |
| **L7** | **Healer DPS restricted in dungeons** (`HealerDPSMapRestriction=1` lists all dungeon maps). The healer wanded (5019) as its top spell while the group was slow | [M] July | Small (+5 % group DPS if the healer nukes) | config (K3, dungeon runs only) |

Not suspected (checked): the Keleseth add priority is correct; warlock `life tap` is gated (29.5 in-combat
emergency, 5.1 normal); `SpellDistance=28.5` is fine.

---

## 5. S2 measurement design

### 5a. Live now (no rebuild, no restart). The orchestrator runs these; I executed none except read-only polling.

| ID | Lever | Exact command | Bots | Duration | Metric source | Rollback |
|---|---|---|---|---|---|---|
| **L-A** (read-only) | Counter poller | `powershell -NoProfile -File D:\Games\wowstuff\AutoWoW\scripts\autowow-control.ps1 -Action combatlog -BotGuid <g>` looped at ≥11 s spacing (template: `C:\Users\shonh\AppData\Local\Temp\claude\C--dev\4673201e-33f1-44d4-a116-94952db0f423\scratchpad\cl.ps1`), output to `research\combat\combatlog-*.jsonl` | Rotate the 13 questing non-scout non-fixture bots: 3,4,6,9,11,12,14,15,16,17,18,19,35 (exclude oracle-managed 7,10; fixtures 24,25,41; scouts 101,112,121,123,236,244) | Whole S1 remainder. One bot is revisited every ~2.5 min, so the 30 s idle retirement loses most fights; that bias is accepted | `combat_reduce.py --combatlog` | none (read-only) |
| **L-B** (read-mostly) | Population duty cycle | Worldserver console (pts/0, PID 33653): `.playerbots rndbot stats` every 10 min | all | S1 remainder | Playerbots.log counts: `combat`, `dead`, `rest`, `moving`, engine state, NewRpg status | Side effect: resets the NewRpg `rpgStatistic` accumulators. Skip it if another lane reads those. |
| **L-C** (flag) | AI CPU cost per action | console `.playerbots pmon toggle`, then `.playerbots pmon tick` after 10 min, then `.playerbots pmon toggle` | all | 10 min | PerfMonitor table in Playerbots.log (links L2 stalls to AI actions) | `.playerbots pmon toggle` again. It adds overhead, so do it once, not during perf-sensitive windows. |

No live strategy toggles are available. Per-bot `co +/-strategy` needs a whispering master player, and
`.playerbots bot` is `Console::No`. So every behaviour A/B is config- or code-at-restart.

### 5b. Config-only A/B knobs (next restart; one knob per arm; the population split must be by level band)

Config is global, so an A/B means **alternating soak segments** (ABAB, ≥45 min each, same bot set,
same level band), not a per-bot split. Each knob:

- **K1: food cheat on.** `AiPlayerbot.BotCheats = "food,taxi,raid"`. The effect: rest triggers move to
  <65 % HP / <65 % mana (`UseFoodStrategy.cpp:15-18`), rest becomes an instant aura with an AI pause
  of 18 s × deficit, and grind-mode sits happen at any deficit. Expect fewer deaths and more rest time.
  Primary metric: deaths/h and XP/h per level band. Guardrail: combat share.
  *Caveat*: this is a "cheat", so it changes the product fairness line. The owner decides whether it
  is shippable. As a science control it is fine.
- **K2: rest thresholds without cheat.** `AiPlayerbot.LowHealth = 55`, `AiPlayerbot.LowMana = 30`.
  **Confound:** `LowHealth` also drives in-combat emergency triggers (e.g. hunter deterrence prio 35,
  healer thresholds). Record it as a two-effect knob, or prefer the code fix C5.
- **K3: reaction delay.** `AiPlayerbot.ReactDelay = 50` vs `100` (`DynamicReactDelay=1` stays).
  The prediction [H] is **no measurable change** while P95 tick ≥ 500 ms (L2), which makes it a clean
  negative control. If this arm moves DPS, the tick model is wrong.
- (dungeon runs only) `AiPlayerbot.HealerDPSMapRestriction = 0`.

### 5c. Code hooks to land at the ~04:00 rebuild window (spec only; not implemented)

**C1: fix `combat.entries` (bug).** `src/AutoWow/CombatPerformanceTelemetry.cpp:408` `OnUnitUpdate`:
keep `bool lastInCombat` per bot in `RollingCounters`. When `unit->IsInCombat()` goes false→true, call
`RecordCombatEntry`. Remove the reliance on `OnUnitEnterCombat`, which is creature-only
(`Creature.cpp:2908`). Test seam: a new `RollingCounters::ObserveCombatFlag(nowMs, bool)` plus a test
in `tests/CombatPerformanceTelemetryTest.cpp` asserting that false→true→false→true gives entries 2 and
exits 2. Risk: negligible (world thread, O(1)).

**C2: lifetime totals plus the last closed window.** Add a `LifetimeTotals` struct (never reset while
online, cleared on `Forget`): `damageDone, damageTaken, healing, deaths, combatEntries, combatMs,
restMs, kills, casts, gcdCasts, lowResourceCombatMs, pullsBelowHalf, lastSeenMs`. Also retain the
`lastClosedWindow` CounterSnapshot when `ResetWindow()` fires. Serialize both under a new
`counters.lifetime` and `counters.last_window`, bump to `autowow.combat-counters.v2` with `version:2`,
and keep v1 fields unchanged for compatibility. `combatMs` accumulates in `OnUnitUpdate` by `diff`
while `IsInCombat()`. `restMs` accumulates while `GetStandState()==UNIT_STAND_STATE_SIT` and the bot has
a food/drink aura (SPELL_AURA_MOD_REGEN / OBS_MOD_POWER), not in combat. Test seam: value-only
accumulators like the existing `RollingCounters` tests. Retrofit: fixed-width saturating counters,
never-reused per-guid store, schema version field.

**C3: casts/GCD.** `PlayerScript::OnPlayerSpellCast(Player*, Spell*, bool skipCheck)` (confirmed at core
`ScriptDefines/PlayerScript.h:331`) increments `casts`,
and `gcdCasts` when `spellInfo->StartRecoveryTime > 0`. Idle-GCD ms = combatMs − Σ(gcd or cast time)
and can be computed in the reducer. The optional ring buffer of the last 32 casts
(spellId, targetGuid, ms) enables the L3 wasted-DoT join.

**C4: kills and TTK.** `PlayerScript::OnPlayerCreatureKill` and `OnPlayerCreatureKilledByPet` (core `PlayerScript.h:258,261`)
record `kills++`. For TTK, keep a small per-bot map creatureGuid →
firstDamageMs, filled in `OnDamage` when the attacker is an attributed bot and the victim is a creature,
capped at 16 and evicted on kill or after 120 s. On kill, push a histogram bucket by
(Δlevel ∈ {≤−3, −2..0, +1..+2, ≥+3}, ttk ms). Emit as `counters.lifetime.ttk_hist`.

**C5 (behaviour, for S3 not S2): L3 lifetime gate.** In the listed trigger headers, replace
`BuffTrigger::IsActive()` with `DebuffTrigger::IsActive()`, which restores the alive check and
`needLifeTime`. For Hunter's Mark keep 0.5 s, but skip when target HP < 20 %. Expected effect:
+5–10 % GCDs to damage spells for hunter, warlock, moonkin, elemental and fire mage [H]. Risk: the
`estimated group dps` heuristic can over-estimate for weak solo bots, so DoTs get skipped on mobs
that would live. Measure wasted-DoT and TTK before and after.

**C6: dump without polling.** Add a periodic (every 60 s) `LOG_INFO("autowow.combat", …)` of each
tracked bot's lifetime counters as one JSON line, gated by a new key
`AutoWow.CombatTelemetry.LogIntervalMs` (default 0 = off). This removes the 1-call-per-10 s bridge
bottleneck and the idle-retirement bias. It needs an `Logger.autowow.combat` / Appender entry in
worldserver.conf, pointing at `/root/autowow-soak/logs/combat.log`.

### 5d. Sample size

Target: ±15 % relative CI on effective DPS per (class, level band). Bot-to-bot CV for effective DPS
within a band is unknown. Assuming CV ≈ 0.5 [H], n ≈ (1.96·0.5/0.15)² ≈ **43 fight-minutes per cell**.
Deaths are Poisson: at ~1 death/h, telling 1.0/h from 0.5/h needs about **20 bot-hours per arm**.
With 13 eligible questing bots across 10 classes, **a single S2 segment cannot fill per-class cells**.
The plan is therefore:

1. S2 primary cells = **level band (1–20, 21–40, 41–60) × role (melee / caster / hunter-pet)**, not class.
2. Per-class results are reported as [n=small] until a later soak raises `MaxRandomBots` (a
   capacity decision, blocked by L2 tick stalls).
3. Each A/B arm runs ≥ 2 × 45 min (ABAB) with the same bots, giving ≈13 bots × 1.5 h ≈ 20 bot-h per arm.

### 5e. Reducer

`research/combat/combat_reduce.py` (stdlib; `--selftest` passes). Current inputs are list snapshots
and combatlog samples. For C2/C6 output, extend `fold_combatlog` to read `counters.lifetime` deltas
between consecutive dumps per bot. The folding rules:
- Per bot: Δ of each lifetime counter between the first and last dump. A counter reset
  (value decreases), meaning a relog or `Forget`, splits the segment.
- Per cell: sum the deltas (sterile sums), then derive the rates. Never average per-bot ratios.
- Emit the profile name `run_id/segment/arm/level-band/role` on every row.
- Exclusions: scouts 101,112,121,123,236,244; oracle-managed 7,10 (and any other guid listed in
  `AutoWow.OracleRuntime.BotGuids`); fixtures in `AutoWow.FixtureGuids`; level-80 idle bots
  (they are reported separately and do not count toward efficiency).
