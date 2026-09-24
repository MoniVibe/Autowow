# Low-mob deaths, cohort, soak-s21-full-r1

Read-only diagnosis, written 2026-09-24. Nothing changed: no config edits, no builds, no DB writes.

## Profile and sources

- **Run:** `soak-s21-full-r1`, binary fe8504f8 (main 5297d894 at build). Module checkout on disk is 79be1606.
- **Profile:** product + `Tactics.Enable` ArmPct 100, `Tactics.Classes = "priest,warlock,hunter"`, `ZoneProgression.HighRoutes`, `Gathering.Detours`, `Trade.Enable`, `Survival.{PullLevelCap,RestGate,SafeRevive,KeepConsumables}`, `DeathLoop.V2`, `QuestScheduler.PreferGearRewards` (all = 1).
- **Window:** the ledger ran 12:13:05 to 13:00:05 (45.2 min). Copied snapshot: 8243 lines.
- **Deaths:** 177 cohort `died` events. The brief says 173 because the ledger kept growing after it was written. The killer was a creature in 176 deaths and the environment in 1.
- **Other sources:**
  - Engage rows: 301 cohort `engage` rows. These cover the tactics classes only.
  - Combat rows: cumulative `combat` cv rows, using the last row per bot.
  - Logs: `Playerbots.log` `[SafeRevive]` lines.
  - DB: read-only SELECTs through `cohort-lib.ps1`. PlayerSaveInterval is 15 min, so gear can be up to 15 min stale.
  - Creature data: `creature_template` rows for all 71 killer entries.
- **Scratch folds:** `an*.py` in the session scratchpad. They are not committed.

## (1) Gear: barely moved since S14

| | S14 (L8-15) | S21 now (L9-24) |
|---|---|---|
| avg equipped ilvl | 3.8 | **5.5** (median 4.95) |
| equipped slots | ~7/19 | **8.2/19** |
| greens | 0 | **26 on 16 of 50 bots**, 0 blue |
| grey / white equipped | 155 / 193 | 147 / 235 |

- **Rogue weapons are all starter or trash items.** Main hands are ilvl 2-5 with 2-5 damage, and off hands are Worn Dirk or Sharp Dirk at 1-2 damage. This holds at L12-21 for all 7 rogues:
  - Vorthak 62982 is L20 and still carries the same Primitive Hand Blade and Worn Dirk he had at L11 in S14.
  - Sabellith 62986 is L21 with Fisherman Knife (grey) and Worn Dirk.
- **Other melee and casters are no better.** Warriors at L15-19 use Worn Greatsword (3-5 damage). Casters use ilvl 2-5 staves and daggers.
- **Quest rewards, AH and trade did not upgrade weapons.** There were 45 `rewarded` events and 40 `trade` events (mail, fees). Errand spending went to repairs, vendoring and training.
- **Consumables:** 33 of 50 bots carry no food or drink (item class 0, subclass 5).

## (2) Multi-mob fights

Engage outcomes for the tactics classes (the only classes with engage telemetry):

| | died (61) | won (238) |
|---|---|---|
| mobs_max >= 2 | **40 (66%)** | 41 (17%) |
| mobs_max >= 4 | **22** | 0 |
| load_max median (centi) | 200 | 80 |
| unchosen (pull_risk -1) | 40 (66%) | 120 (50%) |
| elite_n > 0 | 1 | 0 |

- **Low killers come from packs.** Of the 22 tactics deaths where the killer was 2 or more levels below the bot, **20 were multi-mob.** The killing blow comes from a low trash mob inside a pack, while the damage came from several mobs.
- **Killers by type:** of 176 creature kills, 101 are humanoids (type 7), 45 beasts and 26 undead. By rank, 174 are normal, 1 elite and 1 rare. By class, 154 are warrior-class, 16 paladin-class (a hybrid class) and 6 mage-class, so casters are few.
- **Top killers are social humanoid packs:**
  - Defias Trapper (12)
  - Risen Hungerer (10)
  - Riverpaw Brute (8)
  - Stormsnout (7)
  - Tunnel Rat Kobold (6)
  - Bristleback Hunter / Water Seeker (11)
  - Stonesplinter (9)
- **Shared hotspots:** 32 of 177 deaths had another cohort bot die within 90 s and 60 yd. Hotspots sit 150-300 yd from quest hubs:
  - Westfall around -10500,1200 (Sentinel Hill): 20
  - Loch Modan around -5300,-2700 (Thelsamar): 15
  - Ghostlands around 7550,-6600 (Tranquillien): 14

## (3) Zone fit

- **L15-16 deaths (47):**
  - By zone: Westfall 14, Ghostlands 13, Loch Modan 11, Stonetalon 5, other 4.
  - By level gap: 30 at -2 or lower, 12 at -1 or 0, 8 at +1 or higher.
- **The zones are right and there are no elite areas.** The killers are the zones' normal L11-16 mobs.
- **The +1..+35 tail (51 deaths) is a separate issue.** It includes 6 deaths in Searing Gorge (zone 51) at +28..+35, which are High-routes travel spillover.

## (4) Rest at pull start

- **HP rest works.** For died engagements the hp0 median is 100, and only 8 of 61 started below 70% (all of them warlock 62973 chain re-pulls).
- **Priest mana does not.** 14 of 23 priest deaths started below 60% mana (`RestGate.MinManaPct` is 60), and 21 of those 23 were unchosen aggro. The gate only guards proactive pulls, so a low-mana priest walking or detouring gets jumped.

## (5) Repeat deaths

- **Repeat pairs:** there were 136 repeat-death pairs. 78 of them (57%) came within 4 min, 47 within 4 min and 100 yd, and 17 within 2 min and 60 yd.
- **Time dead:** 20% of wall time for rogues, 28% for priests and **43% for mages**.
- **SafeRevive logged only 76 plans for 177 deaths:**
  - 44 `revive_at`, all at threat=0. 13 of these were at deaths=3-4 in the window, which is past `SpiritDeaths`=2: a zero-threat spot overrides the spirit-healer escape.
  - 23 `spirit_healer`.
  - 9 `none`.
- **Threat scoring only counts idle hostiles.** It misses respawns, patrols and runners.
- **Death-loop escapes:** `death_loop` fired 29 times (23 cluster, 6 level_gap) but relocated only 9 times.

## (6) Rogue

- **Rogues take the largest share of deaths.** 7 rogues account for 56 of 177 deaths (32%), and for 39 of the 83 deaths to mobs 2 or more levels below.
- **Rogue damage is the lowest of any class:**

  | class | dmg/s in combat | dmg/fight | taken/fight | fights/death |
  |---|---|---|---|---|
  | rogue | 14.2 | 287 | 187 | 3.5 |
  | warrior | 25.6 | 369 | 111 | 23.1 |

- **The rotation is running; the weapon is the problem.** Rogues make 26.7 casts per combat minute, the highest of any class, so energy and idling are not the issue. The rogue is running on a ~2 DPS weapon.
- **Stealth and openers are unmeasured.** Rogues are not in `Tactics.Classes`, so they emit no engage rows.

## Ranked causes and fixes

| # | cause | evidence | fix | kind |
|---|---|---|---|---|
| 1 | **Weapon/gear stagnation.** Level outruns gear, so every fight is a coin-flip damage race. | ilvl 5.5, starter weapons at L20, rogues 32% of deaths at 14 dmg/s | Weapon-first upgrade errand: at a hub vendor, buy the best class-usable weapon when it beats the equipped DPS by 1.5x or more and gold allows. Auto-equip quest and loot upgrades by DPS or stat score. Audit why `PreferGearRewards` produced 0 weapon upgrades. | code |
| 2 | **Pack and social aggro.** Multiple mobs and runners make the fight unwinnable on this gear; the "low" killer is just the last hit. | 66% of deaths multi-mob vs 17% of wins, 20 of 22 low-gap deaths multi, 22 deaths with 4+ attackers, humanoid killers 57% | Add linked or nearby same-faction mobs within ~12 yd to `pull_risk`, and skip packs of 2+ while gear score is low. Travel.Safe path cost should include mob clusters near hubs. Stun, snare or finish humanoid runners below 20% HP. | code |
| 3 | **Revive back into the kill zone.** | 57% of repeat deaths within 4 min, 13 corpse revives at deaths 3-4, 20-43% of time dead | At deaths >= SpiritDeaths, force spirit healer plus a DeathLoop relocate whatever the spot threat is. Count respawn-imminent and in-combat mobs within 60 yd in threat. Priest: apply the mana rest before travel and detours, not only before pulls. | code, plus config `SafeRevive.SpiritDeaths=1` as a stopgap |

**Also recommended:** add `rogue` (and `mage`) to engage telemetry (`Tactics.Observe`) so the next soak can show opener and stealth behaviour. Mages spend 43% of wall time dead with no engage data.
