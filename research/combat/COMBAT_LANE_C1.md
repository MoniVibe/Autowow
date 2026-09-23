# Combat lane C1: telemetry fixes, lifetime totals, `combat` ledger event, DoT lifetime gate (2026-09-23)

Status: **PROVEN at source/unit level. HELD for runtime:** nothing has run in a worldserver yet. The running
soak binary was not relinked.

- Module repo: WSL `/root/autowow-advisor-t1-module`, branch `combat-c1` off main `7d534bbe`.
- Commits:
  - `c49a80ac` C1
  - `9c529614` C2+C4
  - `7d09ed77` C6
  - `09b79811` C5
- Not pushed and not merged.
- AutoWoW repo (`D:\Games\wowstuff\AutoWoW`): the script changes are **uncommitted**, because commits there were not commissioned:
  - `scripts/combat-reduce.py` is new.
  - `scripts/ledger-reduce.py` has a small edit.
- Worldserver `/root/autowow-advisor-t1-build/src/server/apps/worldserver` sha256 is
  `43c9580c7991d1007fb8e98f567c16abc13daee62886b960f8795fee1bb20c7d`. It was the same before and after every build. I only
  ran `ninja -j4 unit_tests`, and only after `ninja -n` showed no worldserver link step.

## Flags (all default off; OFF = one cached bool per hook, same behaviour as before)

| Key | Default | Effect |
|---|---|---|
| `AutoWow.CombatTelemetry.Enable` | 0 | C1 + C2/C4 lifetime totals + C6 emit |
| `AutoWow.CombatTelemetry.LogIntervalMs` | 60000 | Period of the `combat` event; 0 = never. A final line is also written at logout. Needs `AutoWow.Ledger.Enable=1`. |
| `AutoWow.Combat.DotLifetimeGate` | 0 | C5 behaviour gate |
| `AutoWow.Combat.DotMinTargetHpPct` | 10 | HP floor for trash |
| `AutoWow.Combat.DotMinDurationPct` | 50 | A DoT is skipped if the estimated TTK is below this share of its duration |
| `AutoWow.Combat.DotHighHpPerLevel` | 150 | A target whose absolute HP is at least bot level × this is exempt from the HP floor |

Runtime needs a worldserver.conf change: none. The event goes through the existing `Logger.autowow.ledger` appender to `ledger.log`.

## What is measured (C1, C2, C4, C6)

- **C1:** v1 `combat.entries` was always 0 for players. It now counts entries through `PlayerScript::OnPlayerEnterCombat`. That hook is fired by
  `CombatManager::UpdateOwnerCombatState` on every false→true transition.
- **Lifetime totals per bot**, cumulative since login (schema `cv=1`, fields append-only):
  - time: `wall_ms`, `combat_ms`, `dead_ms`, `starved_ms`
  - activity counts: `fights`, `kills`, `deaths`
  - damage and healing: `dmg`, `taken`, `heal`
  - casts: `casts`, `gcd_casts` (non-triggered casts only)
  - `dot_skips`
  - TTK: `ttk_n`, `ttk_drop`, and `ttk=[[ms,dlvl],...]`, the samples since the previous line.
- **Starved** means one of these holds while in combat:
  - mana < 15 %
  - rage < 10
  - energy < 20

  Runic power and focus are not measured.
- **TTK** is measured from the bot's (or its pet's) first damage on a creature to that creature's death by the bot's (or pet's) killing blow.
  - Each bot tracks up to 16 engagements at once.
  - An engagement older than 2 minutes produces a kill but no TTK sample.
- **Idle GCD:** not measured directly. `gcd_casts` × 1.5 s / combat s is reported as an estimate.
- **Store:** a mutex-guarded map, capped at 2048 bots. It is erased at logout.

**Reducer: `scripts/combat-reduce.py` (`--selftest` OK).**
- It computes deltas between consecutive lines. A counter that drops starts a new session.
- Rates come from summed deltas.
- Output cells: per bot, per class, per level band, per class×band, and total. For each cell it reports:
  - combat DPS and sustained DPS
  - TTK median and p90
  - deaths per hour, plus a join to `died` for deaths by killer (creature entry, level, level delta)
  - combat, dead and downtime shares, which sum to 1
  - starved share, GCD estimate, and dot_skips

**`ledger-reduce.py`:** it did *not* skip unknown events safely. A row was created before the event kind was checked, so `(bot, 0)` rows leaked.
- Fixed: events outside `QUEST_EVENTS` now `continue` before the row is created. The selftest covers `combat`.
- Note: while I worked, another writer added `"progress"` to `QUEST_EVENTS` in the same file. The selftest still passes.

## C5 gate rule

This applies to normal and rare creatures only. The gate is never applied to:
- elite, rare-elite or world-boss rank
- dungeon or world bosses
- players

1. **HP floor.** Skip if HP % < 10, unless HP ≥ bot level × 150. The floor applies to both DoTs and debuffs.
2. **Short TTK (DoTs only).** Skip if the estimated TTK < 50 % of the DoT duration.
3. **Refresh (DoTs only).** Skip if the estimated TTK ≤ the time left on the bot's own running aura.

How TTK is estimated:
- Primary: the target's observed HP loss since the bot engaged it (this includes group damage).
- Floor: the bot's recent DPS.
- This needs `CombatTelemetry.Enable`. Without it the TTK is unknown and only the HP floor applies.

Coverage (the list and the wiring):
- 25 DoTs:
  - warlock: corruption, agony, doom, immolate, unstable affliction, siphon life, seed
  - priest: shadow word: pain, vampiric touch, devouring plague
  - druid: moonfire, insect swarm, rake, rip, lacerate
  - hunter: serpent sting
  - rogue: rupture, garrote
  - warrior: rend
  - death knight: frost fever, blood plague, icy touch, plague strike
  - shaman: flame shock
  - mage: living bomb
- 9 debuffs: hunter's mark, curses of the elements, weakness, exhaustion and tongues, sunder armor, faerie fire and faerie fire (feral), expose armor.
- Two central hooks are enough, so no class header was edited:
  - `BuffTrigger::IsActive`, restricted to `DebuffTrigger` descendants. This includes the overrides that bypassed `DebuffTrigger::IsActive`.
  - `CastDebuffSpellAction::isUseful`
- Siphon life has no trigger or action site in the module.
- Skips are counted in `dot_skips`. Consecutive skips of the same target and spell count once. GCD saved is about `dot_skips` × 1.5 s. Mana saved is not measured.

## Tests

- `unit_tests`: 12327 ran.
  - 6837 passed
  - 5486 skipped
  - 4 failed. These are the known pre-existing oracle and quest source-contract tests:
    - OracleExecutorTypes
    - QuestAcquisitionContract
    - QuestFinisherBranchWiring
    - QuestObjectiveResolutionSource
  - 2 disabled
- New tests:
  - 2 for C1
  - 8 in `CombatLifetime`, covering accumulation, TTK, slot eviction, recent DPS, starvation, skip dedupe, emit interval and format, and TTK overflow
  - 1 ledger format test for `combat`
  - 7 in `DotLifetimeGate`: trash <10 % skipped; boss <10 % cast; elite with big HP cast; high-HP normal target exempt from the floor; short TTK skipped; refresh vs first application; classification and TTK estimate
- Both Python reducers pass `--selftest`.

## Death quick look

Profile: live `/root/autowow-soak/logs/ledger.log`, run `soak-s5-cohort-r2`, ms 97106–2178917 (about 35 min), 46 `died` events in total.

**Cohort bots 62955–63004** (50 bots, levels 3–9):
- 1 death: 62966 in zone 141, killer kind `environment` (a fall or drowning).
- That is about 0.03 deaths per bot-hour.
- The low-level cohort is not dying to overpulls or level gaps.

The other 45 deaths are almost all **repeat deaths at the same spot** (loops), not bad pulls:

| Bot | Level | Deaths | Zone | Killer | Level gap | Pattern |
|---|---|---|---|---|---|---|
| 112 (scout) | 18 | 24 | Burning Steppes (46) | entry 9694 (lvl 54) | **+36** | Deaths every 15–150 s at (-7988,-2371) for 33 min; never rescued |
| 307 | 55 | 9 | 46 | 7043/7048 (lvl 57–58) | +2/+3 | Same cluster (-7980,-1330); ended by `rndbot_revive`+`rndbot_teleport` at 1133 s |
| 7 | 13 | 5 | Barrens (17) | 3382 (lvl 13–14) | 0/+1 | Identical coords (-1572,-3885) each time |
| 8 | 80 | 3 | Icecrown (210) | 29717 (lvl 79–80) | 0/+1 | |

**Recommendations:**
- **Priority: a repeated-death-at-same-spot breaker.**
  - Trigger: N deaths within R yd and T min.
  - Action: spirit-healer resurrect, or relocate / defer the quest.
  - This is code, not config. I have not written it.
- **Rest thresholds (`LowHealth=45`, `LowMana=15`, `BotCheats="taxi,raid"`):** no evidence yet for changing them for the cohort.
  - Revisit with C2 data: `starved_share`, deaths/h per band.
  - Candidates for an A/B: `AiPlayerbot.LowHealth=55` / `LowMana=30`, or the food cheat.
  - Caveat: LowHealth also drives in-combat emergency triggers.
- **Scout 112's +36 level gap is a placement bug in the scout, not pull selection.** `AggroDistance=22` is irrelevant at that gap.

## Residual risks / needs orchestrator

1. **Pre-existing latent data race (not fixed; outside this brief).** The v1 `CombatPerformanceTelemetry` store (`Store()`, an `unordered_map`) is always on. It is written from `OnDamage` and `OnUnitUpdate` on map-update threads, and the live config has `MapUpdate.Threads = 4`, so maps can mutate it concurrently. The file header comment says "world-thread only", but that is not true. The new lifetime store is mutex-guarded. Recommended next step: put a mutex on the v1 store. This could be a crash source in long soaks.
2. **Runtime proof is still needed.** At the next rebuild or restart window, run with `CombatTelemetry.Enable=1`, `LogIntervalMs=60000` and the ledger on, then check:
   - `combat` lines appear
   - `combat.entries` > 0 in bridge `combatlog`
   - `combat-reduce.py` on the real log produces sane DPS and TTK
   - then an A/B of `DotLifetimeGate` measuring `dot_skips`, TTK and DPS
3. `DebuffTrigger` gating uses `dynamic_cast`, but only when the flag is on and an aura is missing.
4. The TTK estimate assumes the target was at full HP when the bot engaged it. If a bot joins a fight late, the estimate is biased toward shorter TTK, which means more skipping. Pulls are mostly fresh targets, where this does not arise.
