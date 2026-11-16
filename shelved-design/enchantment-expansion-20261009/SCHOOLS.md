# Enchantment schools — design only

**Status:** `DESIGN_ONLY_NOT_NATIVE`; `native_active: false`; all costs, effects, timings, gates, stations, recipes and rewards are proposals. The eight entries are new school finishes: six field and two masterwork. `source_boss` and `source_encounter` are null; requested boss motifs inspire the designs but claim no drop, recipe provenance, native ID or active effect.

Each runtime band is a separate level-qualified recipe. Wearer and base-item required levels must both fall in that band. The artisan must already meet the listed skill threshold at the actual native rank whose cap permits it; native rank names and character-level requirements remain authoritative. No recipe grants skill, rank, level or armor proficiency. A plate chest is eligible only when the wearer already has native proficiency; otherwise only a separately qualified, wearer-proficient chest family can use that design.

## Runtime bands and gates

| Variant | Wearer and base-item required level | Field skill and rank gate | Masterwork skill and rank gate |
|---|---:|---|---|
| Novice 1-20 | 1–20 | 25; actual native Enchanting rank whose admitted cap permits skill 25; native rank label and character-level requirement remain authoritative | 75; actual native Enchanting rank whose admitted cap permits skill 75; native rank label and character-level requirement remain authoritative |
| Apprentice 21-40 | 21–40 | 100; actual native Enchanting rank whose admitted cap permits skill 100; native rank label and character-level requirement remain authoritative | 150; actual native Enchanting rank whose admitted cap permits skill 150; native rank label and character-level requirement remain authoritative |
| Journeyman 41-60 | 41–60 | 200; actual native Enchanting rank whose admitted cap permits skill 200; native rank label and character-level requirement remain authoritative | 250; actual native Enchanting rank whose admitted cap permits skill 250; native rank label and character-level requirement remain authoritative |
| Artisan 61-70 | 61–70 | 300; actual native Enchanting rank whose admitted cap permits skill 300; native rank label and character-level requirement remain authoritative | 350; actual native Enchanting rank whose admitted cap permits skill 350; native rank label and character-level requirement remain authoritative |
| Master 71-80 | 71–80 | 350; actual native Enchanting rank whose admitted cap permits skill 350; native rank label and character-level requirement remain authoritative | 400; actual native Enchanting rank whose admitted cap permits skill 400; native rank label and character-level requirement remain authoritative |

The 71–80 existing reagent names come from the project material-name reference. Lower-band reagent names are ordinary historical WoW name references outside that 40-name source; every lower reagent has a null native entry and is explicitly unverified pending qualification. These names do not assert a native route or vendor/drop source. Custom patterns remain unimplemented.

## Shared operating rules

Each finish bill covers one compatible real item supplied by the buyer; the base item is not included or destroyed. Reference material names do not establish recipe costs or routes. Lower-band bills replace the high-band bill completely. Recharge uses ordinary named reagents only, with no special refill tokens. A newly completed finish starts at its stated full charge capacity, paid for by the finish bill. Recharge is accepted only at zero charges and restores that full capacity. Charges, ICD deadlines and pending windows persist on the exact item through relog, storage and equip swaps; deadlines continue from saved time, and equipment changes never refill or reset them.

All stages are active work at the listed city Enchanting station, with the eligible artisan, station, recipe, skill, reserved inputs and safe custody observed continuously. No time accrues offline, after logout, during interruption/combat or through restart catch-up; stages make no free interim output. City enchanters never need to adventure or attend a boss encounter. Bots buy only the correct wearer-level variant when the compatible slot, role benefit and complete funded quote are available; they equip only legal useful finishes and recharge only at zero charges with the full bill and fee funded.

Recipe teaching, if later implemented, is one city-teacher credit per distinct useful design/variant after its native rank/skill gate and a funded practice order. Repetition, bulk crafts, splitting, trade, storage and recharge grant no extra teaching or XP. If separately implemented, the completing artisan (maker) may earn custom school mastery once per distinct useful design/variant/work receipt; the item wearer/recipient receives no maker mastery, and design authorship alone earns nothing. Mastery grants no stats, native skill, XP, level, reputation or goods.

Only hostile NPC events with real player-caused context qualify. Friendly/self-inflicted, periodic, reflected, environmental, pet/proc-generated, probe, factory and administrative events are excluded; custom effects cannot trigger themselves, healing or absorbs cannot recurse, and no forced bodyguard, cleanse, immunity or resurrection is added. Item-local charges/ICDs and same-family duplicate arbitration are permitted; no global hero, legacy, loadout or progression cap is introduced. Runtime use remains blocked on native slot/event, persistence, save/load, equip/removal and bot-use admission.

## Candidate cards

### Last Stand Aegis — Field

**Design ID:** `enchant2.school.last-stand-aegis`
**Host and role:** role-compatible body armor; plate is valid only if the wearer already has native plate proficiency, otherwise use a separately qualified wearer-proficient chest family (slot: chest armor); tank, solo defender, front-line adventurer.
**City/station:** Stormwind or Orgrimmar city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** While this exact chest is equipped in hostile NPC combat, an eligible direct non-periodic hostile hit must move the wearer from above 30% to at or below 30% of maximum health. The trigger is after that hit resolves.
**Effect:** Consume one stored charge and place a personal absorb shield for 5 seconds; it only absorbs later eligible hostile damage.
**Exact numbers:** `{"health_threshold_crossing_percent":30,"absorb_percent_of_wearer_max_health":3,"absorb_hard_cap_by_variant":{"novice_1_20":30,"apprentice_21_40":60,"journeyman_41_60":120,"artisan_61_70":180,"master_71_80":240},"absorb_duration_seconds":5,"first_hit_absorbed":false,"max_charges":24}`; ICD 120s; capacity/charges 24; empty: At zero charges the chest gives no Last Stand shield or substitute passive; only a completed city recharge restores 24 charges.

**71–80 reference finish bill:** 24 × Infinite Dust [historical name reference]; 4 × Dream Shard [historical name reference]; 3 × Eternal Earth [historical name reference]; 1 × Abyss Crystal [historical name reference]; 1 × Last Stand Aegis Master Pattern [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Last Stand Aegis Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Last Stand Aegis Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Last Stand Aegis Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Last Stand Aegis Pattern [custom, unimplemented].

**71–80 full recharge bill:** 24 × Infinite Dust [historical name reference]; 3 × Dream Shard [historical name reference]; 2 × Eternal Earth [historical name reference]; restores 24 charges only from empty in 20 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 24 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 24 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 24 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 24 from empty in 18 active minutes.

**Active stages:** Qualify role-compatible chest and replacement channel — 7 min; Lay the danger threshold and bind the ward — 11 min; Inspect item identity, charge ledger, and removal behavior — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Last Stand Aegis when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider only for a tank/defender whose role and legally equippable role-compatible chest; plate requires existing native proficiency make the threshold shield useful; do not claim automatic survival or cheat-death. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible chest base, wearer-native armor proficiency, and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Spellcatcher Relay — Field

**Design ID:** `enchant2.school.spellcatcher-relay`
**Host and role:** caster or healer main-hand weapon (slot: main-hand weapon); caster damage, mana healer, hostile-spell responder.
**City/station:** Dalaran city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** During pre-damage resolution of one direct, non-periodic hostile NPC spell event against the wearer, reserve one stored charge. The hit must be eligible before it resolves. Commit that charge only if the same event settles with positive damage actually absorbed; if the event cancels or no damage is absorbed, release the reservation without charge loss. Friendly, self, reflected and periodic damage never qualify.
**Effect:** On that same damage event, absorb 40% of its eligible damage up to the variant cap and settle the actual absorbed amount once. Only after absorb settlement, store 25% of that actual amount as an arcane echo. The next direct hostile NPC spell cast by the wearer within 8 seconds deals that echo as bonus arcane damage up to its variant cap; unused echo expires.
**Exact numbers:** `{"incoming_hit_absorb_percent":40,"absorbed_damage_cap_by_variant":{"novice_1_20":20,"apprentice_21_40":40,"journeyman_41_60":80,"artisan_61_70":120,"master_71_80":180},"echo_percent_of_actual_absorb":25,"echo_damage_cap_by_variant":{"novice_1_20":5,"apprentice_21_40":10,"journeyman_41_60":15,"artisan_61_70":20,"master_71_80":25},"echo_window_seconds":8,"max_charges":24,"mana_restored":0,"charge_reserve_point":"before qualifying damage resolves","charge_commit_condition":"positive damage actually absorbed from the same settled event","aborted_or_zero_absorb":"release reservation without consuming charge","echo_stored_after_absorb_settlement":true}`; ICD 30s; capacity/charges 24; empty: At zero charges the weapon neither absorbs nor stores/echoes energy; no mana or other resource is restored.

**71–80 reference finish bill:** 22 × Infinite Dust [historical name reference]; 5 × Greater Cosmic Essence [historical name reference]; 3 × Eternal Water [historical name reference]; 2 × Dream Shard [historical name reference]; 1 × Spellcatcher Relay Matrix [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Spellcatcher Relay Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Spellcatcher Relay Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Spellcatcher Relay Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Spellcatcher Relay Pattern [custom, unimplemented].

**71–80 full recharge bill:** 24 × Infinite Dust [historical name reference]; 4 × Greater Cosmic Essence [historical name reference]; 3 × Eternal Water [historical name reference]; restores 24 charges only from empty in 20 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 24 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 24 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 24 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 24 from empty in 18 active minutes.

**Active stages:** Qualify main-hand weapon and incoming-school event — 7 min; Bind the finite capture matrix — 11 min; Test charge/echo expiry and inspect — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Spellcatcher Relay when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider for mana-using caster or healer roles that face hostile NPC spells; do not purchase on melee-only or non-mana roles unless the recipient explicitly values the defensive route. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible weapon/body-armor base and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Oathbound Bodyguard's Resolve — Field

**Design ID:** `enchant2.school.oathbound-bodyguards-resolve`
**Host and role:** role-compatible body armor; plate is valid only if the wearer already has native plate proficiency, otherwise use a separately qualified wearer-proficient chest family (slot: chest armor); tank, captain, group bodyguard.
**City/station:** Stormwind or Orgrimmar city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** While the wearer has hit the same hostile NPC with a direct attack in the prior 5 seconds, an eligible different group member within 8 yards must cross from above 35% to at or below 35% maximum health due to that NPC's direct non-periodic hostile hit.
**Effect:** Grant that ally a 2.5% maximum-health absorb shield for 5 seconds and the wearer 4% damage reduction for 4 seconds. The hit that crossed the threshold is not undone and damage is never redirected.
**Exact numbers:** `{"ally_health_threshold_crossing_percent":35,"required_same_hostile_player_hit_window_seconds":5,"ally_range_yards":8,"ally_absorb_percent_max_health":2.5,"ally_absorb_cap_by_variant":{"novice_1_20":25,"apprentice_21_40":50,"journeyman_41_60":100,"artisan_61_70":150,"master_71_80":220},"ally_absorb_duration_seconds":5,"wearer_damage_reduction_percent":4,"wearer_reduction_duration_seconds":4,"max_charges":24}`; ICD 90s; capacity/charges 24; empty: At zero charges there is no ally shield or wearer resolve; the enchant has no passive aura or taunt.

**71–80 reference finish bill:** 24 × Infinite Dust [historical name reference]; 4 × Dream Shard [historical name reference]; 3 × Eternal Earth [historical name reference]; 1 × Eternal Life [historical name reference]; 1 × Oathbound Bodyguard's Resolve Pattern [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Oathbound Bodyguard's Resolve Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Oathbound Bodyguard's Resolve Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Oathbound Bodyguard's Resolve Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Oathbound Bodyguard's Resolve Pattern [custom, unimplemented].

**71–80 full recharge bill:** 24 × Infinite Dust [historical name reference]; 3 × Dream Shard [historical name reference]; 2 × Eternal Earth [historical name reference]; restores 24 charges only from empty in 20 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 24 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 24 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 24 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 24 from empty in 18 active minutes.

**Active stages:** Qualify role-compatible chest and group event — 7 min; Weave the oath thread through the resolve pattern — 11 min; Inspect ally target, range, charge and duplicate-family rules — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Oathbound Bodyguard's Resolve when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider only for a tank/captain currently serving a group role and using the compatible body-armor slot; solo bots receive no fabricated group benefit. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible chest base, wearer-native armor proficiency, and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Wayfarer's Wake — Field

**Design ID:** `enchant2.school.wayfarers-wake`
**Host and role:** leather body armor (slot: chest armor); scout, ranged adventurer, mobile support.
**City/station:** Dalaran city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** After an eligible hostile NPC root or snare of at least 2 seconds ends naturally on the wearer during hostile combat, consume one charge.
**Effect:** Grant 6-10% movement speed for 4-6 seconds by variant. It never shortens, cleanses, prevents, or changes the control effect.
**Exact numbers:** `{"minimum_hostile_root_or_snare_duration_seconds":2,"movement_speed_bonus_percent_by_variant":{"novice_1_20":6,"apprentice_21_40":7,"journeyman_41_60":8,"artisan_61_70":9,"master_71_80":10},"duration_seconds_by_variant":{"novice_1_20":4,"apprentice_21_40":4,"journeyman_41_60":5,"artisan_61_70":5,"master_71_80":6},"max_charges":20}`; ICD 45s; capacity/charges 20; empty: At zero charges the wearer gets no movement bonus; no control resistance or passive speed remains.

**71–80 reference finish bill:** 20 × Infinite Dust [historical name reference]; 4 × Greater Cosmic Essence [historical name reference]; 4 × Eternal Air [historical name reference]; 1 × Dream Shard [historical name reference]; 1 × Wayfarer's Wake Windglass [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Wayfarer's Wake Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Wayfarer's Wake Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Wayfarer's Wake Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Wayfarer's Wake Pattern [custom, unimplemented].

**71–80 full recharge bill:** 20 × Infinite Dust [historical name reference]; 3 × Greater Cosmic Essence [historical name reference]; 3 × Eternal Air [historical name reference]; restores 20 charges only from empty in 18 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 20 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 20 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 20 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 20 from empty in 18 active minutes.

**Active stages:** Qualify leather chest and control-state event — 7 min; Bind the wake-thread and verify speed cap — 11 min; Inspect natural-ending and charge persistence — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Wayfarer's Wake when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider for scout or ranged roles on leather chest only when useful movement after hostile control is expected; never claim immunity or obstacle bypass. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible weapon/body-armor base and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Hearthkeeper's Margin — Field

**Design ID:** `enchant2.school.hearthkeepers-margin`
**Host and role:** healer main-hand weapon (slot: main-hand weapon); healer, group support, mana healer.
**City/station:** Dalaran city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** A direct player-cast heal in hostile NPC combat must restore at least 2% of a different group ally's maximum health; that ally must be below 35% when the cast begins and have taken a direct hostile NPC hit in the prior 5 seconds.
**Effect:** Grant only that ally an absorb shield equal to 2% of their maximum health for 5 seconds, capped by the variant amount. Overheal, periodic healing, self-healing, and proc-generated healing do not qualify.
**Exact numbers:** `{"target_health_max_at_cast_start_percent":35,"minimum_effective_heal_percent_of_target_max_health":2,"recent_hostile_damage_window_seconds":5,"target_absorb_percent_max_health":2,"target_absorb_cap_by_variant":{"novice_1_20":20,"apprentice_21_40":40,"journeyman_41_60":80,"artisan_61_70":120,"master_71_80":180},"absorb_duration_seconds":5,"max_charges":24}`; ICD 60s; capacity/charges 24; empty: At zero charges the weapon applies no shield and provides no passive healing or resource bonus.

**71–80 reference finish bill:** 22 × Infinite Dust [historical name reference]; 5 × Greater Cosmic Essence [historical name reference]; 4 × Eternal Life [historical name reference]; 2 × Dream Shard [historical name reference]; 1 × Hearthkeeper's Margin Filament [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Hearthkeeper's Margin Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Hearthkeeper's Margin Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Hearthkeeper's Margin Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Hearthkeeper's Margin Pattern [custom, unimplemented].

**71–80 full recharge bill:** 24 × Infinite Dust [historical name reference]; 4 × Greater Cosmic Essence [historical name reference]; 3 × Eternal Life [historical name reference]; restores 24 charges only from empty in 20 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 24 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 24 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 24 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 24 from empty in 18 active minutes.

**Active stages:** Qualify healer weapon and effective-heal event — 7 min; Weave single-recipient margin ward — 11 min; Inspect absorb cap, ally targeting and no-recursion path — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Hearthkeeper's Margin when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider for healer roles that actively heal injured group allies in hostile NPC combat; avoid if current native gear or another same-family shield makes the finish redundant. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible weapon/body-armor base and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Triune Cadence — Field

**Design ID:** `enchant2.school.triune-cadence`
**Host and role:** caster main-hand weapon (slot: main-hand weapon); caster damage, mana caster.
**City/station:** Stormwind or Orgrimmar city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** Three successful direct player-cast spells against the same hostile NPC within 8 seconds, while the weapon is equipped, consume one charge and prime the next eligible direct hostile spell cast within 6 seconds.
**Effect:** Reduce that next spell's cast time by 8%, capped by the variant seconds; instant spells are unchanged. The sequence must be real casts, not periodic ticks, proc spells, reflections, or friendly actions.
**Exact numbers:** `{"required_successful_direct_casts":3,"same_hostile_target_required":true,"sequence_window_seconds":8,"primed_spell_window_seconds":6,"cast_time_reduction_percent":8,"max_cast_time_reduction_seconds_by_variant":{"novice_1_20":0.15,"apprentice_21_40":0.2,"journeyman_41_60":0.3,"artisan_61_70":0.4,"master_71_80":0.5},"instant_cast_affected":false,"max_charges":24}`; ICD 20s; capacity/charges 24; empty: At zero charges a three-cast sequence primes nothing; no passive haste or spell power remains.

**71–80 reference finish bill:** 24 × Infinite Dust [historical name reference]; 5 × Greater Cosmic Essence [historical name reference]; 4 × Eternal Air [historical name reference]; 2 × Dream Shard [historical name reference]; 1 × Triune Cadence Rune Blank [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 8 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; 1 × Novice Triune Cadence Pattern [custom, unimplemented].
- Apprentice 21-40: 10 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; 1 × Apprentice Triune Cadence Pattern [custom, unimplemented].
- Journeyman 41-60: 12 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; 1 × Journeyman Triune Cadence Pattern [custom, unimplemented].
- Artisan 61-70: 14 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; 1 × Artisan Triune Cadence Pattern [custom, unimplemented].

**71–80 full recharge bill:** 24 × Infinite Dust [historical name reference]; 4 × Greater Cosmic Essence [historical name reference]; 3 × Eternal Air [historical name reference]; restores 24 charges only from empty in 20 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 12 × Strange Dust [unverified name]; 2 × Lesser Magic Essence [unverified name]; 1 × Small Glimmering Shard [unverified name]; restores 24 from empty in 10 active minutes.
- Apprentice 21-40: 14 × Soul Dust [unverified name]; 2 × Lesser Mystic Essence [unverified name]; 1 × Small Glowing Shard [unverified name]; restores 24 from empty in 13 active minutes.
- Journeyman 41-60: 16 × Vision Dust [unverified name]; 3 × Lesser Nether Essence [unverified name]; 1 × Small Radiant Shard [unverified name]; restores 24 from empty in 16 active minutes.
- Artisan 61-70: 18 × Arcane Dust [unverified name]; 3 × Lesser Planar Essence [unverified name]; 2 × Prismatic Shard [unverified name]; restores 24 from empty in 18 active minutes.

**Active stages:** Qualify caster weapon and direct-cast sequence — 7 min; Bind measured cadence into rune blank — 11 min; Inspect cast window, time cap, charge and relog behavior — 7 min (total 25 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Triune Cadence when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider only for mana-caster roles with repeated cast-time spells against hostile NPCs; instant-only rotations do not use the effect. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible weapon/body-armor base and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Captain's Redoubt — Masterwork

**Design ID:** `enchant2.school.captains-redoubt`
**Host and role:** role-compatible body armor; plate is valid only if the wearer already has native plate proficiency, otherwise use a separately qualified wearer-proficient chest family (slot: chest armor); tank captain, group bodyguard, front-line protector.
**City/station:** Stormwind or Orgrimmar city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** A successful native taunt by the wearer against a hostile NPC that has dealt a direct hit to a different group member within the prior 5 seconds, with that ally within 8 yards, consumes one charge.
**Effect:** For 5 seconds grant that ally and wearer damage reduction by variant. The taunt follows its normal native result; this effect does not force target selection, redirect damage, or taunt automatically.
**Exact numbers:** `{"taunted_hostile_must_have_hit_ally_within_seconds":5,"ally_range_yards":8,"ally_and_wearer_damage_reduction_percent_by_variant":{"novice_1_20":{"ally":4,"wearer":3},"apprentice_21_40":{"ally":5,"wearer":4},"journeyman_41_60":{"ally":6,"wearer":5},"artisan_61_70":{"ally":7,"wearer":6},"master_71_80":{"ally":8,"wearer":6}},"duration_seconds":5,"max_charges":24}`; ICD 60s; capacity/charges 24; empty: At zero charges there is no resolve or passive threat bonus; native taunt is unchanged.

**71–80 reference finish bill:** 40 × Infinite Dust [historical name reference]; 8 × Dream Shard [historical name reference]; 6 × Eternal Earth [historical name reference]; 3 × Abyss Crystal [historical name reference]; 1 × Frozen Orb [historical name reference]; 1 × Captain's Redoubt Master Seal [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 16 × Strange Dust [unverified name]; 4 × Lesser Magic Essence [unverified name]; 2 × Small Glimmering Shard [unverified name]; 1 × Novice Captain's Redoubt Pattern [custom, unimplemented].
- Apprentice 21-40: 20 × Soul Dust [unverified name]; 4 × Lesser Mystic Essence [unverified name]; 2 × Small Glowing Shard [unverified name]; 1 × Apprentice Captain's Redoubt Pattern [custom, unimplemented].
- Journeyman 41-60: 24 × Vision Dust [unverified name]; 5 × Lesser Nether Essence [unverified name]; 3 × Small Radiant Shard [unverified name]; 1 × Journeyman Captain's Redoubt Pattern [custom, unimplemented].
- Artisan 61-70: 28 × Arcane Dust [unverified name]; 6 × Lesser Planar Essence [unverified name]; 3 × Prismatic Shard [unverified name]; 1 × Artisan Captain's Redoubt Pattern [custom, unimplemented].

**71–80 full recharge bill:** 40 × Infinite Dust [historical name reference]; 6 × Dream Shard [historical name reference]; 5 × Eternal Earth [historical name reference]; 1 × Abyss Crystal [historical name reference]; restores 24 charges only from empty in 30 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 24 × Strange Dust [unverified name]; 5 × Lesser Magic Essence [unverified name]; 2 × Small Glimmering Shard [unverified name]; restores 24 from empty in 15 active minutes.
- Apprentice 21-40: 28 × Soul Dust [unverified name]; 5 × Lesser Mystic Essence [unverified name]; 2 × Small Glowing Shard [unverified name]; restores 24 from empty in 18 active minutes.
- Journeyman 41-60: 32 × Vision Dust [unverified name]; 6 × Lesser Nether Essence [unverified name]; 3 × Small Radiant Shard [unverified name]; restores 24 from empty in 21 active minutes.
- Artisan 61-70: 36 × Arcane Dust [unverified name]; 7 × Lesser Planar Essence [unverified name]; 3 × Prismatic Shard [unverified name]; restores 24 from empty in 23 active minutes.

**Active stages:** Preparation and qualified role/taunt contract — 15 min; Plate binding and accepted mitigation profile — 25 min; Resolve finishing on exact chest identity — 25 min; Inspect group range, taunt receipt, charge ledger and lifecycle — 15 min (total 80 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Captain's Redoubt when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider only for funded tank/bodyguard duty with a learned legal taunt and compatible legally equippable chest armor; plate requires existing native proficiency; a city artisan never joins the deployment. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible chest base, wearer-native armor proficiency, and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

### Harmonic Succor — Masterwork

**Design ID:** `enchant2.school.harmonic-succor`
**Host and role:** healer main-hand weapon (slot: main-hand weapon); healer, mana healer, multi-target group support.
**City/station:** Dalaran city Enchanting workshop; proposed Enchanting workshop, native route unqualified; the artisan stays in the city for all work.
**Band gates:** wearer and base-item required levels match one row above; minimum skill and actual rank are listed there; native slot/family proficiency must already be learned; no proficiency is granted.
**Trigger:** During hostile NPC combat, three direct player-cast heals each restoring at least 2% of maximum health to three distinct grouped allies who each took a hostile NPC hit in the prior 5 seconds, all completed within 12 seconds, consume one charge and prime the next direct heal for 6 seconds.
**Effect:** That next direct heal has cast time reduced by 8% up to the variant cap and mana cost reduced by 10% of its normal native cost. Instant heals receive the mana-cost reduction only. No mana is restored; effective healing must be real and the primed heal cannot retrigger the sequence.
**Exact numbers:** `{"required_effective_direct_heals":3,"distinct_group_allies":3,"minimum_each_heal_percent_of_target_max_health":2,"recent_hostile_damage_window_seconds":5,"sequence_window_seconds":12,"primed_heal_window_seconds":6,"cast_time_reduction_percent":8,"cast_time_reduction_cap_seconds_by_variant":{"novice_1_20":0.15,"apprentice_21_40":0.25,"journeyman_41_60":0.35,"artisan_61_70":0.45,"master_71_80":0.5},"mana_cost_reduction_percent":10,"mana_restored":0,"max_charges":24}`; ICD 75s; capacity/charges 24; empty: At zero charges no healing sequence primes a discount or speed benefit; no passive healing or mana is granted.

**71–80 reference finish bill:** 40 × Infinite Dust [historical name reference]; 8 × Greater Cosmic Essence [historical name reference]; 6 × Dream Shard [historical name reference]; 6 × Eternal Life [historical name reference]; 3 × Abyss Crystal [historical name reference]; 1 × Harmonic Succor Master Lattice [custom, unimplemented].

**Lower-band finish bills** (all reagents are unverified name references; pattern is custom/unimplemented):

- Novice 1-20: 16 × Strange Dust [unverified name]; 4 × Lesser Magic Essence [unverified name]; 2 × Small Glimmering Shard [unverified name]; 1 × Novice Harmonic Succor Pattern [custom, unimplemented].
- Apprentice 21-40: 20 × Soul Dust [unverified name]; 4 × Lesser Mystic Essence [unverified name]; 2 × Small Glowing Shard [unverified name]; 1 × Apprentice Harmonic Succor Pattern [custom, unimplemented].
- Journeyman 41-60: 24 × Vision Dust [unverified name]; 5 × Lesser Nether Essence [unverified name]; 3 × Small Radiant Shard [unverified name]; 1 × Journeyman Harmonic Succor Pattern [custom, unimplemented].
- Artisan 61-70: 28 × Arcane Dust [unverified name]; 6 × Lesser Planar Essence [unverified name]; 3 × Prismatic Shard [unverified name]; 1 × Artisan Harmonic Succor Pattern [custom, unimplemented].

**71–80 full recharge bill:** 40 × Infinite Dust [historical name reference]; 6 × Greater Cosmic Essence [historical name reference]; 5 × Eternal Life [historical name reference]; 1 × Abyss Crystal [historical name reference]; restores 24 charges only from empty in 30 active city minutes.

**Lower-band full recharge bills:**

- Novice 1-20: 24 × Strange Dust [unverified name]; 5 × Lesser Magic Essence [unverified name]; 2 × Small Glimmering Shard [unverified name]; restores 24 from empty in 15 active minutes.
- Apprentice 21-40: 28 × Soul Dust [unverified name]; 5 × Lesser Mystic Essence [unverified name]; 2 × Small Glowing Shard [unverified name]; restores 24 from empty in 18 active minutes.
- Journeyman 41-60: 32 × Vision Dust [unverified name]; 6 × Lesser Nether Essence [unverified name]; 3 × Small Radiant Shard [unverified name]; restores 24 from empty in 21 active minutes.
- Artisan 61-70: 36 × Arcane Dust [unverified name]; 7 × Lesser Planar Essence [unverified name]; 3 × Prismatic Shard [unverified name]; restores 24 from empty in 23 active minutes.

**Active stages:** Preparation and qualify three target receipts — 15 min; Weave the distinct-ally prayer lattice — 25 min; Tune cast-time and native mana-cost caps — 25 min; Inspect effective-heal, charge, relog and no-recursion evidence — 15 min (total 80 min, active work only).

**Supplier and bot criteria:** Quote exact names, quantities, payment, and delivery before order acceptance; no free replenishment or raid-only artisan reagent. Only consider Harmonic Succor when a funded city supplier offers the correct wearer-level variant, compatible item slot, role use, and complete bill; compare with current legal gear and expected benefit. Consider only for mana-healer roles that make useful direct heals across a group; require funded workshop stock and exact group-role use before commissioning. Equip only the exact qualified variant on a legal compatible real item; do not equip duplicate copies to multiply one recipe family. At a city station, recharge only when all charges are spent, the complete bill and fee are funded, and the order is available; keep persisted charge state through relog and item swaps.

**Dependencies and abuse boundary:** Exact compatible weapon/body-armor base and replacement channel must be admitted; the existing base stays intact and is not destroyed. A real context-bearing hostile-event seam must identify item GUID, wearer, hostile source, group/target state, eligible event type, and charge settlement. Exclude friendly, self-inflicted, periodic, reflected, environmental, training-probe, factory, administrative, pet/proc-generated events and all events generated by this or another custom effect. No self/friendly damage, heal/absorb recursion, forced bodyguard, resurrection, immunity, or effect chaining; use native hostility, group, range, target, school, and removal rules after admission. Only one instance of this recipe family may trigger per wearer at a time; this local duplicate-family arbitration does not cap other distinct designs.

## Source notes

Names/mechanics checked against [the current enchanting catalogue](../profession-catalogues-20261009/enchanting.json) to avoid duplication. High-band reagent names use [MATERIAL_BILLS.json](../profession-catalogues-20261009/MATERIAL_BILLS.json) and [MATERIAL_REFERENCE.json](MATERIAL_REFERENCE.json) as name references only. Timing follows [WORKSHOPS.md](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md); boss-motif and hostile-event constraints follow [BOSS_LEGACIES.md](../../work/server-strategy-20261008/mod-autowow-strategy/docs/BOSS_LEGACIES.md) and the [boss reward catalogue](../AUTOWOW_RAID_BOSS_REWARD_CATALOGUE_2026-10-09.md). Source documents establish proposal vocabulary and operating constraints, not these custom recipes or effects.
