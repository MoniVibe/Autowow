# Field-gear provisioning for Luna professions

Status: design and host-source audit. The October 7 city-crafter report is historical S125, not fresh runtime or today’s roster; its policy/source changes are prepared, not deployed. The October 9 workshop reports are contracts, not native recipe/item/effect/queue/scheduler installs. The matrices, routing, metrics, and actions below are proposed.

The objective is properly equipped adventurers without cheap white or high-level filler. A candidate is admissible only when the recipient’s class, role, weapon proficiency, armor family, class-level gate, native equip/use rules, unique/equipped limits, and source route pass. It must beat the currently equipped actual item for that role by role score (survivability, damage, healing, or utility, including relevant stats and proc compatibility). An empty slot is comparable only after those checks; “higher item level” or “white quality” alone is never a verdict. White gear can be useful when it is the first qualified native upgrade; no rule should claim all white gear is bad.

## Two tracks

**Current field kit.** Fill urgent weapons first, then chest/legs/head/shoulders, then bracers/feet/waist/gloves, then cloak and secondary slots; neck, rings, and trinkets are lower priority. Source candidates from native crafts, auction-house purchases, and quest/loot rewards. The admission record must name source, raw materials, trained skill/recipe rank, native cast or pickup, payment, mail/bank handoff, and equip/use. A missing material, recipe, trainer eligibility, or character-level gate is a visible blocker, never silently substituted with filler.

**Future masterworks/legendaries.** Use a versioned blueprint: family/core, form/slot, role, stat allocation, signature effect, support finish, makers, inputs, payment, and stage ledger. Preparation, forging/weaving, tempering, finishing, and inspection/attunement require qualified work. Native admission, exact properties/effects, save/load/mail/equip, and settlement remain unverified. Separate capacity means qualified artisan assignments and queue priority: one artisan has one active order, so do not start a long project when urgent demand dominates. Shortage, unsafe/busy work, or interruption lawfully holds stage progress; committed progress survives restart without catch-up output. Finished pieces ship partial-as-completed; cancellation preserves produced goods and releases unused inputs.

## Five role-kit matrices

| Role kit | Primary forms and armor | Score emphasis | Hard admission checks |
|---|---|---|---|
| Plate tank | one-handed weapon + shield; chest/legs/head/shoulders | stamina, defense, mitigation, threat | plate proficiency, shield/weapon skill, level, unique limits, native defensive use |
| Melee damage | one- or two-handed weapon; legs/chest/head | role damage stats, haste/crit, usable proc | class/weapon proficiency, two-hand restriction, native attack use |
| Ranged physical | bow/gun/crossbow or qualified ranged weapon; mail/leather body | ranged damage, haste/crit, ammunition compatibility | ranged proficiency, ammo/native attack route, armor family and level |
| Caster damage | staff/focus or caster weapon; cloth/head/shoulders | spell power and chosen casting priority | class spell school, weapon/focus use, native spell/equip path |
| Healer/support | caster weapon/focus; body armor and cloak | healing, mana/survival, compatible support effect | class heal/use rules, group/range lifecycle, no duplicate unique effect |

## Producer and source coverage

| Domain | Producers | Gather/input coverage | First useful output |
|---|---|---|---|
| Plate and weapons | blacksmiths, weapon specialists | miners/smelters; actual ore and bars | tank/melee weapons, chest/legs |
| Mail and leather | leatherworkers | skinning crews; hides and vendor/native components | ranged/melee mail, bracers/feet |
| Cloth | tailors | cloth gatherers and quest/loot cloth | caster/healer chest, head, cloak |
| Arcane finishes | jewelcrafters/enchanters | miners, herbalists, salvaged quest/loot reagents | stat/support finish only when natively admitted |
| Utility and consumables | engineers/alchemists | miners/herbalists; recipe-specific reagents | ranged utility, resistance/encounter consumables |

Artisans remain city specialists and gain profession skill and character work XP through successful native work/training. Training gates cannot bypass character-level gates; work XP counts only after verifying the existing admitted mechanism. They do not adventure to level, self-mine as a new job, receive free progress/materials, or craft as though a catalogue row were native. Gatherers and questers retain their roles. These are coverage targets, not proof every recipe or maker is available.

## Demand, restock, and duplicate control

Build demand from the actual current roster and equipped snapshots; do not assume S125 or an old 100-character roster. For each recipient, retain item-instance GUID, slot, stats, proc/effect profile, source, score, eligibility, and blocker. Aggregate only equivalent demand keys (role, class/armor, form, recipe/source, stat/effect profile). Count each delivery once by exact instance GUID plus shipment and recipient; ten same-template pieces are ten legitimate goods. Mail, bank, trade, equip, or retry never adds a delivery. A batch may partially deliver while reporting quantity, reserved inputs, and blockers. Priority is urgent qualified upgrades, missing core slots, restock buffer, then secondary slots; legendary work never starves urgent work.

Measure before/after for the same recipient and slot under the same exposure, recording the changed item GUID, template, exact stats, proc/effect lifecycle, native craft/pickup, payment settlement, and deaths/wipes per comparable encounter/time. Report source and item identity. Higher level or score is an input change, not proof of fewer deaths; make no causal claim without matched exposure and encounter controls.

## Eight release actions

1. Snapshot the roster, class/role, level, proficiency, equipped item GUIDs/templates/stats/effects, and demand; mark unknowns explicitly.
2. Admit current-track candidates through role, class, armor, weapon, level, unique, and native equip/use gates; reject filler and record the exact blocker.
3. Resolve each admitted candidate’s native recipe/quest/loot source, trained skill/rank, raw materials, payment, cast or pickup, mail/bank, and equip receipt.
4. Split urgent field orders from ordinary bulk and timed legendary capacity; reserve inputs once and allow partial completion without duplicate counts.
5. Route production across the five domains, naming blacksmith and gatherer coverage, shortages, trainer gates, and responsible city artisan.
6. Pilot one or two field-gear chains end to end: material receipt → native craft/pickup → payment → delivery → equip/use, while artisans gain only actual-work progress.
7. After that evidence, admit one bounded legendary family/form/stat/effect/support route with a versioned blueprint and inspect every stage; keep unverified effects blocked.
8. Re-snapshot matched recipient/slot changes and same-exposure outcomes, publish score/stats/procs/deaths and unresolved blockers, then expand recipes or legendary capacity only after native save/load/mail/equip and settlement evidence.


## Early socket demand

[Early gems and random socket design](EARLY_GEMS_AND_SOCKET_ROLLS.md) supplements the equipment queue with 30 starter cuts. Score the real empty socket or
existing gem, wearer/host level and class role. Fund urgent weapons/body pieces before modest gem
optimization. Random sockets are one-time proposed new-item outcomes, never a buy/mail/login reroll.
No native recipe/socket/client adapter is admitted by this design.
