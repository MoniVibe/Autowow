# Cohort Professions + Trainer Visits: Plan

Status: design only (read-only lane, 2026-09-23). Nothing built, nothing run.
Source audited: module repo `/root/autowow-advisor-t1-module` at **main `7d534bbe`**. The checkout is
on branch `combat-c1` (`c49a80ac`, +5 files, combat telemetry only, none of them touched here).
All file:line references are to `7d534bbe`. Core refs are to `/root/p1core`.

Why: the faction KPI is total profession skill per faction plus its trend. The first sample
(`work/soak-20260923/faction-progress.jsonl`, 04:17Z) reads `total_profession_skill: 0` for both
factions (25 characters each, average level 5.8 A / 5.5 H). The cohort has no professions at all,
so nothing can level.

---

## 0. Audit: what exists and what is missing

### Exists (reuse it, do not rebuild)

| Capability | Where | Notes |
|---|---|---|
| Learn from a trainer with **real gold** | `src/Ai/Base/Actions/TrainerAction.cpp:94-157` | `Iterate` + `Learn`: reputation discount, checks the `free money for spells` budget, runs `ModifyMoney(-cost)` and then `learnSpell` or casts the learn spell. Uses no cheat unless `BotCheatMask::gold`. |
| Trainer learns are enabled for masterless non-random bots | `TrainerAction.cpp:36-39` | With `allowLearnTrainerSpells`, anything except a tradeskill trainer under an active real master learns. Event param `"learn"` forces it. |
| Trainer target | `TrainerAction.cpp:77-86` | A masterless bot uses `bot->GetSelectedUnit()`, so the caller must `SetSelection(trainer)` first. |
| Core cap on primary professions | core `Trainer.cpp:147` | A first-rank primary is refused when `GetFreePrimaryProfessionPoints()==0`. |
| Core trainer validity (class, race, spell) | core `Trainer.cpp:211-225` | Does **not** check faction hostility. |
| Trainer lookup by entry | `sObjectMgr->GetTrainer(entry)` (used at `TrainerAction.cpp:27`, `RpgSubActions.cpp:290`) | This is the AC new-trainer system: type, requirement and spell list per creature entry. |
| NewRpg already walks to trainers | `PossibleRpgTargetsValue.cpp:107-119` (the new-rpg target flags include TRAINER_CLASS, TRAINER_PROFESSION and TRAINER); `NewRpgAction.cpp:505-555` (WanderNpc) | On arrival it **only** runs quest interaction (`NewRpgAction.cpp:532-533` calls `NewRpgBaseAction.cpp:982`). It never trains. |
| Travel primitives | `NewRpgBaseAction.cpp:228` `MoveFarTo`; TravelFlight status; `TravelMgr.cpp:2335` indexes TRAINER npcs as `RpgTravelDestination` | Movement exists. There is no "nearest trainer of type X" query. |
| Old-rpg train trigger | `RpgStrategy.cpp:145-147`, `RpgTriggers.cpp:173-186`, `RpgSubActions.cpp:267-300` | Dead for independent bots: `ArmIndependentBot` sets `-rpg` (`AutoWowBridge.cpp:2803-2804`). |
| Train-cost and tradeskill budgets | `BudgetValues.cpp:98` (`TrainCostValue`), `:162` (`NeedMoneyFor::spells`), `:190` (`NeedMoneyFor::tradeskill`) | The money buckets already exist. |
| Opportunistic gathering (herb and ore) | `AiFactory.cpp:606-607` adds `gather` to every non-BG bot; `LootNonCombatStrategy.cpp:20-24` runs `add gathering loot` on a timer; `AddLootAction.cpp:58-73` is skill-gated; `LootAction.cpp:204-208` casts Mining/Herb Gathering | **It stays armed for independent bots.** `ArmIndependentBot` removes only `worker gather` and reports `"gather":true`. It gains nothing today because nobody has the skill. |
| Skinning corpses | `LootAction.cpp:172-185` | Skill-gated as well, so it works as soon as the skill is learned. |
| Mats kept rather than vendored | `ItemUsageValue.cpp:546-635` (`ITEM_USAGE_SKILL` when the bot has the skill) | |
| Recipe-cast helper | `CastCustomSpellAction.cpp:204-331` `CraftRandomItemAction`; `ItemUsageValue.h:68` `SpellGivesSkillUp` | **Bug:** `GetSpellPriority` tests `!= SPELL_EFFECT_CREATE_ITEM` (`:314`), so the skill-up preference never applies, and the sort is by spellId rather than priority (`:271-273`). |
| Class-fit profession pairs | `PlayerbotFactory.h:169` `GetClassProfessionPairs(bot)` | A usable fallback when a bot has no manifest assignment. |
| Oracle craft adapter (exact, bridge-driven) | `AutoWowCraftControl.cpp`, bridge `craft`/`craft-status` (`AutoWowBridge.cpp:2590-2601`) | League and Oracle only (`bot_not_enrolled_in_league`). It is not an autonomous loop. |
| Skill-change hooks in core | `PlayerScript.h:799` `OnPlayerUpdateSkill`, fired from `PlayerUpdates.cpp:741` (UpdateSkill) and `:976` (UpdateSkillPro); `PlayerScript.h:308` `OnPlayerLearnSpell` | The module does not subscribe to either: the `Playerbots.cpp:118-139` hook list has neither. |

### Missing

1. A NewRpg trainer phase. Nothing sends an independent bot to a trainer on purpose, and nothing learns on arrival.
2. Profession selection for cohort bots. `InitTradeSkills` returns early for non-random bots that are not Oracle-managed (`PlayerbotFactory.cpp:2839-2841`).
3. A filter on what a trainer visit may learn. `Iterate` learns **every** teachable spell. At a profession trainer with a free slot, that picks up whatever profession that trainer teaches, which is wrong.
4. An autonomous crafting loop and reagent buying for independent bots. `rpg craft` and `rpg buy` live only in the dead old-rpg strategy.
5. A real-time skill ledger. The KPI reads `character_skills` (`scripts/cohort/faction-progress.ps1:41`), so it lags the save interval.

### Caveats found in passing (they affect this plan)

- **Oracle-arm cohort bots (25 of 50) already get free class spells.** They take the legacy path `AutoMaintenanceOnLevelupAction.cpp:89-91` (`InitSkills`, `InitClassSpells`, `InitAvailableSpells`) whatever `AutoWow.Independent.AutoMaintenance` is set to. Mode 2 is only used for stock-arm bots (`AutoWowCohortPolicy.h:147-160`). Retiring free spells therefore needs a switch for both arms, or the arm comparison stays confounded.
- A factory randomize or maintenance run on an Oracle-arm cohort bot would call `InitTradeSkills` (`PlayerbotFactory.cpp:2837+`), which grants random professions and `SetRandomSkill` values. That is **free skill that would contaminate the KPI**. Phase 1 must guard it: skip it for cohort guids.

---

## 1. NewRpg trainer phase (`RPG_GO_TRAIN`)

**When it is due** (pure function `AutoWowTrainPolicy::Due(...)`, unit-tested):
- *Class:* the character reached a new even level (class trainers teach on even levels 2-80), or `TrainCostValue` > 0, **and** `free money for spells` >= the cheapest learnable spell. Re-check at most every 10 min. Cooldown resets when the bot levels up.
- *Profession:* (a) an assigned primary or secondary is not learned yet, or (b) skill >= rank cap - 25 (50/125/200/275 for caps 75/150/225/300) and the character meets the level gate for the next rank (Journeyman 10, Expert 20, Artisan 35, Master 50, Grand Master 65), or (c) a cheap recipe that gives skill-ups is available (Phase 4).
- It never preempts combat, an Oracle `DoQuest` directive (`NewRpgInfo.h:132`), or a flight.

**Where it enters:** in the `RPG_IDLE` branch (`NewRpgAction.cpp:344-346`), check `Due` **before** `RandomChangeStatus`. The phase is deterministic, not a random weight. That needs a new variant `GoTrain {trainerSpawnGuid, pos, kind, startedMs}` in `NewRpgInfo.h:91-101`, a new enum value in `PlayerbotAIConfig.h:59` (and the status count print at `RandomPlayerbotMgr.cpp:2944`), and a new trigger and action pair in `NewRpgStrategy.cpp`.

**Finding the nearest trainer:** a lazily built static index, created once on first use:
`sObjectMgr->GetAllCreatureData()` → `GetTrainer(entry)` → key `(mapId, type, requirement or skillLine)`, value = spawn positions. For profession trainers, derive the skill line from the trainer's spell list (the first rank's `SkillLineAbility`), not from the `Requirement` column. Filter at query time with `IsTrainerValidForPlayer` and faction friendliness (core does not check faction). Search the same map only. If the nearest match is > `AutoWow.Train.MaxDistance` (default 1500 yd), defer and mark `blocked:far` in the ledger. Cross-continent class trainers (for example the Draenei Exodar case) wait until the bot is on that map, or until TravelFlight or a hearth takes it there. It never teleports.

**On arrival:** `bot->SetSelection(trainer)`, then `botAI->DoSpecificAction("trainer", Event("autowow train", "learn"), true)`. `TrainerAction` needs one change: a filter hook in `Iterate` that skips (i) first-rank primary spells whose skill is not in the bot's plan, and (ii) recipes when `free money for tradeskill` is too low. Order: class spells first, then profession rank-ups, then recipes. Gold stays real (`TrainerAction.cpp:140-149`). There is no grant path.

**Cheap early win (in the same lane):** at the WanderNpc arrival (`NewRpgAction.cpp:532`), if `object` is a trainer and `Due`, run the same learn call. Bots already wander past trainers in camps and towns.

**Retiring AutoMaintenance=2:** once the ledger shows GoTrain keeps class-spell counts at parity with a mode-2 control over a named soak profile, set mode 1 (talents stay automatic). Add `AutoWow.Independent.OracleArmFreeSpells=1` (default, keeps legacy behavior) and flip it to 0 in the same step, so both arms pay.

**Flags (all off by default):** `AutoWow.Independent.GoTrain=0`, `AutoWow.Train.MaxDistance=1500`, `AutoWow.Train.RecheckMs=600000`.

---

## 2. Profession choice (manifest proposal)

Pair archetypes, with skill ids: **MB** Mining 186 + Blacksmithing 164, **ME** Mining 186 + Engineering 202, **MJ** Mining 186 + Jewelcrafting 755, **HA** Herbalism 182 + Alchemy 171, **HI** Herbalism 182 + Inscription 773, **SL** Skinning 393 + Leatherworking 165, **TE** Tailoring 197 + Enchanting 333.
Rule: every 5-character race group gets at least one mining + (BS or Eng), one HA, one SL and one TE. Plate classes lean BS, leather and mail classes lean LW, cloth classes lean TE.

| Race | 01 | 02 | 03 | 04 | 05 |
|---|---|---|---|---|---|
| Human | War MB | Pal ME | Rog SL | Mage TE | Lock HA |
| Dwarf | War MB | Pal HA | Hunt SL | Rog ME | Pri TE |
| Night Elf | War MB | Hunt SL | Rog HA | Pri TE | Dru HI |
| Gnome | War ME | Rog SL | Mage TE | Lock HA | Lock HI |
| Draenei | Pal MB | Hunt SL | Pri TE | Sha HA | Mage MJ |
| Orc | War MB | Hunt SL | Rog HA | Sha ME | Lock TE |
| Undead | War MB | Rog SL | Pri HA | Mage TE | Lock HI |
| Tauren | War MB | Hunt SL | Sha ME | Dru HA | Dru TE |
| Troll | War ME | Hunt SL | Pri TE | Sha HA | Mage HI |
| Blood Elf | Pal MB | Rog SL | Pri HA | Mage TE | Lock MJ |

Each faction totals 25 characters: MB 4 + ME 3 + MJ 1 + HA 5 + HI 2 + SL 5 + TE 5. The two factions are mirror-symmetric, so the KPI compares like with like.
Secondaries: all 50 learn First Aid 129 and Cooking 185. Fishing 356 is *learned* but not *levelled* until Phase 5, because it needs a pole and water-seeking.

Manifest schema v2 (proposal; `manifest_version` goes to 2 and v1 fields are unchanged):

```json
{
  "schema": "autowow.cohort.manifest.v2",
  "manifest_version": 2,
  "profession_plan": {
    "plan_version": 1,
    "pairs": {
      "MB": [186, 164], "ME": [186, 202], "MJ": [186, 755],
      "HA": [182, 171], "HI": [182, 773], "SL": [393, 165], "TE": [197, 333]
    },
    "secondary_default": [129, 185, 356],
    "secondary_levelled": [129, 185]
  },
  "entries": [
    {
      "seq": 1, "id": "A-HU-01", "name": "Aldermund", "class": "Warrior",
      "professions": { "pair": "MB", "primary": [186, 164], "secondary": [129, 185, 356], "plan_version": 1 }
    }
  ]
}
```

Delivery to the server: `scripts/cohort/profession-assign.ps1` renders the manifest into one config value, `AutoWow.Professions.Assignments = "62955:186,164;62956:186,202;..."` (guid:primary list; secondaries come from a global value). That is 50 entries, parsed once. Config was chosen over a DB table because it needs no schema migration and is restart-scoped like the other cohort flags. A guid with no assignment falls back to `PlayerbotFactory::GetClassProfessionPairs` seeded by guid, so the choice is deterministic.

---

## 3. Levelling loop

| Step | Status | Detail |
|---|---|---|
| Learn primaries and secondaries | Missing, Phase 1/3 | Via GoTrain at a profession trainer. Apprentice costs about 10c, so gold is not the limit at level 5. |
| Gather herbs and ore while questing | **Exists** | The `gather` and `loot` strategies stay armed for independent bots. They only pick up nodes that come into loot/grind range on the quest path, with no detour. |
| Skin corpses | **Exists** | `LootAction.cpp:172-185`. |
| Gather detour (node within about 40 yd, not in combat, bags free) | Missing, Phase 5 | Reuse `GatheringCandidatePolicy.h` and `GatheringSafetyPolicy.h` from the `worker gather` lane rather than writing a new scanner. |
| Craft to level with real mats | Missing, Phase 4 | New `autowow craft skillup` action, driven from `RPG_REST` and idle town time: pick recipes where `SpellGivesSkillUp` holds and the reagents are in the bags, most skill-up color first. Fix the `CraftRandomItemAction` priority bug (`CastCustomSpellAction.cpp:271-273, 312-331`) or bypass it. Tool and anvil focus is already gated by `CanCastSpell`. |
| First Aid and Cooking | Missing, Phase 4 | Bandages from cloth drops need only cloth. Cooking needs a fire (Basic Campfire) plus meat drops. Same action, same budget. |
| Buy vendor reagents (thread, vials, flux, salt, parchment) | Missing, Phase 4 | Stock `BuyAction` via `DoSpecificAction("buy", ...)` at a vendor during GoTrain or WanderNpc, capped by `NeedMoneyFor::tradeskill` (`BudgetValues.cpp:190`). A static per-profession list of about 15 item ids. |
| Enchanting mats | Missing, Phase 4 | `DisEnchantRandomItemAction` exists (`CastCustomSpellAction.h:70`). Allow it only on greens the bot would otherwise vendor. |

---

## 4. Metric hooks (real-time KPI)

- New `AutoWowSkillLedger` PlayerScript in its **own** file, with hooks `PLAYERHOOK_ON_UPDATE_SKILL` and `PLAYERHOOK_ON_LEARN_SPELL`. Registration is one line in `AddPlayerbotsScripts` (`Playerbots.cpp:686-690`).
- `OnPlayerUpdateSkill(player, skillId, value, max, step, newValue)` covers gather, craft and fishing skill-ups. Filter to the 14 profession skill ids and to cohort guids.
- `OnPlayerLearnSpell` catches new professions and rank-ups (max 75→150). `UpdateSkill` does **not** fire on the first learn.
- Event: `{"schema":"autowow.skill.ledger.v1","seq":N,"utc":..,"guid":..,"faction":..,"skill":186,"old":12,"new":13,"max":75,"cause":"update|learn|rank"}`. Keep it in a bounded ring buffer (like `AutoWowOracleReceiptStore`) and expose it through a read-only bridge verb `skillledger since=<seq>`.
- `faction-progress.ps1` seeds from the DB once, then folds ledger deltas, so the KPI updates in real time and the DB read becomes the periodic reconciliation check (ledger sum must equal the DB sum after save; a mismatch is reported, not smoothed over).
- Also log GoTrain outcomes to the same ledger (`cause:"train"` with the copper spent, and `blocked:far|gold|bags`). That is the gold-starvation and travel evidence.

---

## 5. Phased plan

Each lane has a disjoint write set, flags default off, and a pure policy header with a unit test in `tests/`.

| Phase / lane | Scope | Write set | Tests / proof | Est. |
|---|---|---|---|---|
| **P0 Skill ledger** | Section 4: hook, ring buffer, bridge read verb, KPI script fold | new `src/AutoWow/AutoWowSkillLedger.{h,cpp}`, `Playerbots.cpp` (+1 line), bridge verb case, `scripts/cohort/faction-progress.ps1` | Unit: event filter and ring buffer. Live: one manual `.learn` on a scratch bot shows `cause:learn`, and a skill-up equals the DB delta | 0.5-1 d |
| **P1 Train on arrival + assignments** (**build first**) | `AutoWowTrainPolicy.h` (Due, learn filter, pair parse); `TrainerAction::Iterate` filter hook; WanderNpc arrival hook (`NewRpgAction.cpp:532`); `AutoWow.Professions.Assignments` config and parse; guard `InitTradeSkills` for cohort guids | new `src/AutoWow/AutoWowTrainPolicy.h`, `TrainerAction.{h,cpp}`, `NewRpgAction.cpp` (WanderNpc only), `PlayerbotAIConfig.{h,cpp}`, `PlayerbotFactory.cpp:2839` guard, `scripts/cohort/profession-assign.ps1` | Unit: Due matrix, filter rejects off-plan first-rank, pair parse. Live: cohort bot near a trainer learns its assigned Apprentice spells with gold lower by the exact cost, KPI > 0 | 1-1.5 d |
| **P2 Directed GoTrain** | `RPG_GO_TRAIN` status, trainer spawn index, IDLE preemption, far/gold/bags blocks | `NewRpgInfo.{h,cpp}`, `PlayerbotAIConfig.h` enum, `NewRpgStrategy.cpp`, new `NewRpgGoTrainAction`, new `src/AutoWow/AutoWowTrainerIndex.{h,cpp}` | Unit: nearest-trainer selection over a fixture spawn list. Soak: named profile shows class-spell parity with mode 2, then flip AutoMaintenance to 1 and OracleArmFreeSpells to 0 | 2 d |
| **P3 Rank-ups + secondaries** | Level/skill rank gates, First Aid, Cooking, Fishing learn | `AutoWowTrainPolicy.h` only | Unit: rank gate table | 0.5 d |
| **P4 Craft + buy** | `autowow craft skillup`, reagent buy list, disenchant-greens rule, CraftRandomItem priority fix | new `src/AutoWow/AutoWowCraftSkillupAction.{h,cpp}` + policy header, `CastCustomSpellAction.cpp:271-331`, NewRpg REST hook | Unit: recipe pick order, budget cap. Live: a skill-up event whose reagents were consumed from the bags | 2 d |
| **P5 Gather detour + fishing** | Short detours to nodes; fishing at rest near water | NewRpg grind/quest movement hook, reuse Gathering policies | Soak delta of gather skill-ups per bot-hour | 1.5-2 d |

Total: about 8-9 dev-days. P0 and P1 can run in parallel because their write sets are disjoint.

## Risks

- **Gold starvation (levels 1-20).** Class spells cost more per level as bots rise, while recipes plus reagents compete for the same copper. Mitigation: the budget order is spells, then rank-ups, then recipes, then reagents; `blocked:gold` goes to the ledger; recipes are only learned when they are orange or yellow. If `blocked:gold` dominates, that is a finding to report. Topping bots up with gold would be exactly the grant path we are removing.
- **Trainer travel.** Class trainers sit in capitals, sometimes on another continent. Travel is capped by distance, with same-map search only; flights come from the existing TravelFlight. Walk-time goes to the ledger so the cost to questing throughput is measured.
- **Bag space.** Mats marked `ITEM_USAGE_SKILL` are kept, which fills starter bags. Inventory relief sells grays only (`NewRpgBaseAction.cpp:961`). Mitigations: craft down before buying, cap each mat stack at about 40, and buy bags as a Phase 4 budget line.
- **Wrong profession.** A trainer visit without the P1 filter learns an unplanned primary. The filter is mandatory in P1, not an optional polish.
- **KPI contamination.** The free `InitTradeSkills` and `SetRandomSkill` on Oracle-arm bots must be guarded (P1).
- **Arm confound.** Oracle-arm bots get free spells through the legacy path (see caveats). P2 flips both arms together.
