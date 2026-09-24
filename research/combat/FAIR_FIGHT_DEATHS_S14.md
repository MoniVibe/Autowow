# Fair-fight deaths, cohort, soak-s14-full-r1

Read-only diagnosis, written 2026-09-24. Nothing was changed: no config edits, no builds, no DB writes.

## Profile and sources

- **Run:** `soak-s14-full-r1`. Worldserver pid 51886, built from `/root/autowow-advisor-t1-build` with module `/root/autowow-advisor-t1-module` at b64326aa. Config is `/root/p1runtime/worldserver.conf` plus `/usr/local/etc/modules/playerbots.conf`.
- **Flags on:** Tactics.Enable (ArmPct 50, priests only), Survival.PullLevelCap (MaxLevelAbove 2), DeathLoop.Enable plus EscapeViaZoneProgression, Independent.AutoMaintenance 2. BotCheats is `"raid"`, so there is no free food.
- **Population:** the 50 cohort bots, guids 62955-63004, levels 8-15.
- **Ledger window:** 04:27:18 to 04:58:46, 28.7 min. That is 23.33 bot-hours of combat telemetry.
- **Death count:** the ledger has 75 cohort `died` events. The brief said 73; the difference is the extra ~3.7 min of ledger.
- **Reducer:** `python scripts/combat-reduce.py <ledger> --out-dir work/soak-20260923/combat-s14 --profile soak-s14-full-r1`. It covers all 130 bots. The cohort-only tables below came from my own scratch folds over the same ledger.
- **DB:** read-only SELECTs through `cohort-lib.ps1` (characters DB plus `acore_world`). PlayerSaveInterval is 900000, so DB rows can be up to 15 min stale.
- **Other sources:** Playerbots.log `[DeathLoop]` lines, bridge `snapshot` and `combatlog` for 62982 and 62964, and module source.

## 1. Per class (cohort only)

In the table, "starved" is starved_ms as a share of combat_ms.

| class | bots | deaths | deaths/h | fights | kills | fights/death | TTK med s | taken/kill | heal/kill | starved |
|---|---|---|---|---|---|---|---|---|---|---|
| rogue | 7 | 19 | 5.82 | 38 | 22 | 2.0 | 16.9 | 254 | 0 | 5% |
| priest | 6 | 15 | 5.36 | 36 | 18 | 2.4 | 15.6 | 302 | 111 | 59% |
| shaman | 4 | 9 | 4.82 | 32 | 23 | 3.6 | 15.2 | 184 | 52 | 52% |
| mage | 6 | 11 | 3.93 | 35 | 24 | 3.2 | 13.6 | 130 | 0 | 33% |
| warrior | 8 | 11 | 2.95 | 58 | 57 | 5.3 | 12.4 | 113 | 1 | 15% |
| paladin | 4 | 4 | 2.14 | 6 | 2 | 1.5 | 16.4 | 1136 | 812 | 20% |
| warlock | 6 | 6 | 2.14 | 66 | 66 | 11.0 | 11.6 | 24 | 9 | 25% |
| hunter | 6 | 0 | 0 | 5 | 3 | - | 14.3 | 34 | 0 | 0% |
| druid | 3 | 0 | 0 | 20 | 20 | - | 8.8 | 20 | 1 | 22% |
| **total** | **50** | **75** | **3.21** | 296 | 235 | 3.9 | - | 124 | - | - |

- **Pets decide survival.** Warlock, hunter and druid (pet or tank form) get 11 or more fights per death, or have no deaths at all. Rogue, priest, mage and shaman die every 2-3.6 fights.
- **Death-loop bots.** Of the 50 bots, 19 had no fights at all (travel or quest time). The deaths are concentrated: 62982 has 14, 62964 has 12, 62998 has 6 and 62972 has 5. Those 4 bots account for 37 of the 75 deaths.
- **Mob stats.** The killers are normal (rank 0) mobs at L10-13 with 222-300 HP, from `creature_template` and `creature_classlevelstats`.
- **Damage race.** A bot at ~10 in-combat DPS needs a TTK of 20-33 s. Over that time it takes 250-340 damage, which is about the full HP pool (Vorthak's max HP is 235). At full HP the fight is a coin flip. Starting at partial HP is a loss.

**Level gap at death (killer minus bot):** -2: 1, 0: 9, +1: 34, +2: 18, +3: 7, +4: 2, +6: 3, +11: 1. That sums to 75, of which 62 (83%) are at +2 or less, so they are fair fights.

**Zones:** 38 Loch Modan 28, 17 Barrens 25, 40 Westfall 10, 10 Duskwood 4, 130 Silverpine 4, 3525 Bloodmyst 4. Sum 75. Every death had quest=0, so the bots were grinding or defending themselves when they died.

## 2. Gear (DB, equipped slots 0-18 minus shirt and tabard)

- **Quality:** of 348 equipped items, 155 are grey, 193 are white and **0 are green or better.** Average item level is **3.8**. Nothing is broken (0 items at durability 0) and no item is below 25% durability.
- **Main hands are starting weapons.** Item level is 2-5 at character levels 9-15. Examples:
  - Morvain, L15 warrior: Worn Greatsword, 3-5 dmg / 2.9 s, about 1.4 DPS.
  - Vorthak, L11 rogue: Primitive Hand Blade at ~2.1 DPS plus Worn Dirk at ~0.9 DPS.
  - For comparison, a quest reward weapon around L10-12 is about 5-7 DPS.
- **Empty slots across the 50 bots:** head 50, shoulder 50, wrist 23, waist 18, back 12, hands 10, chest 7, feet 7. The head and shoulder gaps are normal at this level. The others are not.
- **Quest history is thin.** Lifetime completed quests are 2-14 per bot, 419 in total. Of the 348 equipped items, 138 are quest rewards, and they are starter-zone ilvl 1-5 pieces. The bots reached L9-15 mostly by grinding, so their level outran their gear. For comparison, a questing player at L10-12 wears roughly ilvl 8-12 with a few greens.
- **No money.** The cohort holds 2.95 g in total. The median is 228 c, and one warrior holds 150 s.
- **Unsold vendor trash.** Most bots have only the 16-slot backpack, and it is 13-16 slots full. Of 524 non-equipped stacks, 256 are grey, and together they would sell for about 49 s. Vendoring is not happening.
- **Consumables.** 33 of 50 bots have no food, drink or potion. The 4 worst-dying bots (62982, 62964, 62998, 62972) have none, and no bandages either.

## 3. Talents and spells: not the cause

- **Talents are spent.** Decoding `character_talent` against Talent.dbc ranks gives 58 points spent against 50 expected from the DB levels. The surplus is DB save lag. No bot is under-spent. AutoMaintenance=2 is working.
- **Spells are learned.** Vorthak, L11, has Backstab, Gouge, Evasion, Sprint, Slice and Dice, Sap, Sinister Strike r2 and Eviscerate r2. Olbrek, L10, has Power Word: Shield, Renew, Mind Blast, Shadow Word: Pain r2, Smite r2 and Lesser Heal r3. Per-class counts in `character_spell` rise with level.
- **Weapon skills are fine for melee.** Vorthak's dagger skill is 55/55 and Zenjira's mace skill is 55/60. Olbrek's mace skill is 5/50, but he is a caster.

## 4. Pulls: single mobs, not packs

- **Priest engagements show 1v1 deaths.** There are 36 cohort priest `engage` events, and 15 of them ended in death. In those 15, mobs_max>1 occurred **0** times, adds>0 occurred 2 times, and there were 0 elites. Level gap was +1 in 13 of them and +2 in 2.
- **The difference between wins and losses is starting state.**
  - The 15 deaths started at median **32% mana**, all 15 below 50%, and 6 below 90% HP. All 15 had gap_rest_ms of 0 or less, meaning no rest.
  - The 18 wins started at median **89% mana**.
- **The pull cap is working.** The PullCap log shows 16,944 over-cap candidates skipped. The killers at +1 and +2 are inside the cap.
- **Fleeing mobs are a minor factor.** Mobs that "Flee For Assist" at 15% HP (smart_scripts action 25: Defias Smuggler 95, Defias Trapper 504, Razormane Hunter, Tunnel Rat) caused 7 deaths. All 7 were L9 bots in Westfall at run start. Those bots were placed in a zone whose bracket starts at 10, and they got 0 kills.

## 5. Last deaths of the top two bots

**62982 Vorthak, orc rogue L11, Barrens.**
- Deaths:
  - 04:55:59, killed by Fleeting Plainstrider (3246) L13 at (-727,-2542).
  - 04:57:37, killed by Greater Plainstrider (3244) L12 at (-620,-2530).
  - 04:58:37, killed by 3244 at L11 at (-647,-2480).
- **Every one was a 1v1 grind with no quest.** Each fatal fight dealt 106-370 damage and took 86-337 damage, against 235 max HP. The last fight lasted 44 s.
- **No food and no money.** Bags hold only greys, and he has 100 c.
- **Death loop.** He triggered `death_loop` 5 times, always with relocate=false. After each spirit-healer rez he died again in about 60-80 s: 04:46:16, 04:47:35, 04:48:37. At L11 that rez also applies Resurrection Sickness (Death.SicknessLevel = 11).
- **Stuck quest.** Quest 1492 is repeatedly `blocked no_finisher_relation`.

**62964 Olbrek, dwarf priest L10, Loch Modan, control arm (arm 0).**
- Deaths:
  - 04:47:52, Mountain Boar (1190) L11, start 100% HP / 37% mana, died after 29 s.
  - 04:48:52, 1190 L11, start 100% HP / 21% mana, died after 33 s.
  - 04:49:44, Forest Lurker (1195) L11, start 72% HP / 30% mana, died after 34 s.
- **All three were single mobs with no rest and no drinks.** starved_ms was about equal to combat_ms.
- **The danger area did not stop him.** All three deaths are inside the same 60-yd danger circle at (-5347,-2850). The DeathLoop count went 5, then 6, then 7, relocate=false. Travel intent keeps failing (`intent_replan_exhausted` for quests 416 and 418), so he stays in place and grinds.

## Top 3 causes, ranked

### 1. Bots pull at low HP or mana and have nothing to recover with
- **The rest gate only covers treatment-arm priests.** `TacticalRuntime::HoldProactivePull` applies only to them. Every other solo bot pulls the next mob right after a fight or right after a rez at 50% HP.
- **Priest deaths:** 15 of 15 started below 50% mana, versus a median of 89% at the start of wins.
- **The priest A/B confirms it.** Across all bots (reducer, L1-20 priests): arm0 has 9.28 deaths/h, arm1 has 1.36 deaths/h.
- **Deaths come back to back.** Of 55 repeat deaths, 26 happened within 90 s of the previous death. The median gap is 93 s.
- **No consumables:** 33 of 50 bots, and all 4 worst bots.

### 2. Gear far below level, because quests are thin and there is no gold
- **Gear:** 0 greens, average ilvl 3.8, starting weapons at L9-15.
- **Result:** an even damage race against L11-12 normal mobs (TTK 12-17 s cohort median; bot takes about its full HP per kill in rogue and priest fights).
- **Pets are the only buffer.** Classes without one die every 2-3.6 fights.

### 3. The death-loop escalation feeds the loop in right-level zones
- **Relocation almost never fires:** 20 escalations, 1 relocation. The zone bracket low (10) is at or near the bot's level, so the overshoot test fails.
- **Spirit-healer rez puts the bot back at the graveyard next to the same mobs** with 50% HP, plus sickness at L11 and up.
- **It makes the next death come sooner.** Median time to next death is 61-64 s after an escalated death (n=13, a small sample) versus 140 s after a normal death (n=42).
- **Danger areas filter only destinations and quest POIs**, not grind targets. So 8 of 18 post-escalation deaths happened inside a live danger circle.

**Separate unfair-fight tail (13 deaths at +3 or more):**
- L9 bots in Westfall at run start.
- Corwick, L13, in Duskwood, killed by +6 to +11 mobs.
- This is a zone-placement problem, not a combat problem.

## Fixes

| # | Fix | Type | Gear grant? |
|---|---|---|---|
| 1a | Widen the rest gate (`HoldProactivePull`, `NeedsRestHealth`, `NeedsRestMana`) from treatment-arm priests to every solo independent bot. Suggested: hold proactive pulls below 70% HP and below 60% mana, and while aura 15007 (sickness) is active. Self-defense stays allowed. | code (small; eligibility predicate plus a class/arm knob) | no |
| 1b | Interim: set `AutoWow.Tactics.ArmPct = 100`. This covers priests only (6 bots, 15 of 75 deaths) and ends the priest A/B test. | config | no |
| 1c | Independent bots sell greys at the nearest vendor when bags are 12/16 or fuller, and buy level-appropriate food and drink with their own gold when out. There is ~49 s of unsold trash in the cohort's bags. Do **not** turn on AltMaintenanceFood or BotCheats `food`: both create free items. | code | no, if bought with real gold |
| 2 | Raise quest throughput and gear value: have the scheduler prefer quests with equippable choice rewards (a weapon first), and buy vendor weapon upgrades with real gold when affordable. Optional stopgap: `AutoWow.Survival.PullLevelCap.MaxLevelAbove = 1` would have removed the +2 pulls (18 deaths) at the cost of fewer targets and slower XP. Attackers still bypass it. | code plus config | no (loot, quests and vendors only) |
| 3 | DeathLoop: (a) make `GrindTargetValue` skip non-attacker targets inside the bot's danger areas; (b) on the 2nd escalation in the same area, relocate even when the zone bracket fits (to the previous or a lower-bracket zone); (c) after a spirit-healer rez, run a forced full rest before any proactive pull. Optionally use corpse runs at L11+ to avoid sickness. | code | no |

**Build order:** 1a first (cheapest, evidence strongest), then 3, then 1c, then 2. None of these needs a free gear grant. Anything that raises gear faster than quests, loot and vendors with earned gold would break the owner rule. Flag any such proposal loudly.

## Residual unknowns

- The DB is up to 15 min stale (PlayerSaveInterval 900000). Gear, money and talents may be slightly behind the live state.
- Adds data comes only from priest `engage` events. Other classes have no add or multi-mob telemetry, so "1v1" is inferred from the priest data and the TTK and damage-taken math.
- The escalated-rez finding rests on n=13 deaths. Re-check it in the next soak.
