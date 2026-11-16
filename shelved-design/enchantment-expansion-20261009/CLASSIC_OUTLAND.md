# Classic Outland legendary enchanting expansion — design only

**Status: DESIGN_ONLY_NOT_NATIVE.** Exactly eight new boss-themed recipe designs; no recipe, custom item, slot route, passive, proc, recharge rate, or encounter award is implemented. Every new item entry is null.

### Shared commission terms

- The customer supplies the eligible host item. The direct bill below is for one finished item and includes exactly one transferable boss relic consumed per craft.
- A separate transferable boss Pattern Folio is consumed once to grant one recipient one exact per-character recipe entitlement. The folio does not recur as a craft input; the relic does not unlock the recipe. Both are custom unimplemented rewards requiring verified encounter participation and victory. No item/drop ID or guild-wide learning is assumed.
- Proposed artisan gate: naturally trained Enchanting 375 and character level 70, exact design entitlement, and approved city station. Native skill, rank, character, reagent, slot, and recipe checks remain authoritative; these designs grant no skill or XP.
- All five stages run at the city workshop for 35 + 75 + 60 + 85 + 45 = **300 active minutes (5 hours)**. Paused or offline time earns no progress. Timers create no item or native skill gain.
- Reusable tools are city fixtures, never consumed: enchanting rod/workbench, elemental containment crucible, pattern-alignment frame, inspection mirror, and calibration weights.
- Every finish has a persistent passive and limited charged signature. Cooldowns begin when a charge is spent. Charge, cooldown, and window state persist by item through relog, equip swap, death, and trade; canceled windows do not refund charges.
- Each recharge batch takes 10 active city minutes and restores exactly five missing charges using only ordinary listed materials. It is accepted only when at least five charges are missing. No relic, core, pattern, boss kill, or encounter token is needed; no batch overfills the item's 30-charge capacity.
- Only the wearer's qualifying direct hostile events count. Self/friendly, environmental, periodic, pet, reflected, triggered, and recursive events are rejected. No effect creates mana or goods. No global boss-legacy, hero, or loadout cap is added.
- A maker's first distinct useful delivered commission grants a proposed design-specific custom mastery and eligibility to teach one learner individually. That record re-awards no boss legacy/attributes and grants no native skill/rank/XP.
- Local design sources: work/server-strategy-20261008/mod-autowow-strategy/docs/BOSS_LEGACIES.md and WORKSHOPS.md. Material names come from outputs/enchantment-expansion-20261009/MATERIAL_REFERENCE.json, which verifies historical names only, not Classic Outland source routes, IDs, recipes, or effects.

## Recipe cards

### Emberwake — enchant2.legend.ragnaros

**Encounter inspiration:** Molten Core — Ragnaros. The encounter script names Lava Burst and Ragnaros's submerge/emerge cycle. Emberwake borrows that build-and-release rhythm; its four-hit meter and fire burst are a new proposal. The custom effect below is a separate proposal.

**Host / roles:** One two-handed melee weapon; replaces one compatible weapon-enchant channel, subject to a future route qualification. Roles: two-handed melee damage; weapon-focused solo play.

**Wearer gate:** The exact finished two-handed weapon must be equipped. Four qualifying owner weapon hits must land on the same hostile target; the effect is item-local.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Ragnaros Emberwake Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Ragnaros Ember Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +15 Fire resistance points on this item. **Signature trigger:** While in combat, the wearer lands four direct melee weapon hits on the same hostile target within 10 seconds. The fourth hit spends one charge and arms an 8-second Emberwake window. **Effect:** The next direct melee special on that same target during the window adds Fire damage equal to 15% of that strike's pre-critical, unmodified weapon damage. The bonus is one direct hit and cannot crit or trigger another enchant.

**Numbers:** passive fire resistance points: 15; qualifying hits: 4; hit window seconds: 10; armed window seconds: 8; bonus fire damage percent of precrit weapon damage: 15. ICD: 45s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 6 × **Eternal Fire** — existing_name_reference; native entry null.
- 1 × **Ragnaros Ember Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Fire** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Emberwake Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Ragnaros](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/EasternKingdoms/BlackrockMountain/MoltenCore/boss_ragnaros.cpp)

### Skydrake's Eye — enchant2.legend.onyxia

**Encounter inspiration:** Onyxia's Lair — Onyxia. The encounter script includes liftoff, flight movement, directional breath, and landing. The proposal translates the aerial sight line into a range-and-aim sequence, not an Onyxia-style flight or breath ability. The custom effect below is a separate proposal.

**Host / roles:** One ranged weapon (bow, gun, or crossbow); replaces one compatible weapon-enchant channel, subject to a future route qualification. Roles: ranged damage; mobile ranged play.

**Wearer gate:** The exact finished ranged weapon must be equipped. The wearer must land the ranged hits personally; pets and triggered damage do not count.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Onyxia Skydrake's Eye Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Onyxia Aerial Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +20 ranged critical-strike rating. **Signature trigger:** The wearer lands three direct ranged weapon hits on the same hostile target from at least 15 yards away within 6 seconds. The third hit spends one charge and opens a 5-second aim window. **Effect:** The next direct ranged weapon shot against that same target during the window gains +8 percentage points of critical-strike chance. The bonus applies to one shot only and cannot trigger itself.

**Numbers:** passive ranged critical strike rating: 20; minimum range yards: 15; qualifying hits: 3; hit window seconds: 6; aim window seconds: 5; bonus critical chance percentage points: 8. ICD: 60s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 6 × **Eternal Air** — existing_name_reference; native entry null.
- 1 × **Onyxia Aerial Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Air** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Skydrake's Eye Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Onyxia](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Kalimdor/OnyxiasLair/boss_onyxia.cpp)

### Chromatic Answer — enchant2.legend.nefarian

**Encounter inspiration:** Blackwing Lair — Nefarian. The encounter script includes class calls and Shadow Flame. Chromatic Answer is a proposed school-response lesson; it does not reproduce a boss ability or grant a school. The custom effect below is a separate proposal.

**Host / roles:** One shoulders item; replaces one compatible shoulder-enchant channel, subject to a future route qualification. Roles: spell damage; healing through hostile spell pressure; school-flexible casters.

**Wearer gate:** The exact finished shoulder item must be equipped. A charge requires an eligible incoming hostile magic spell and the matching outgoing hostile direct spell; the item does not select or change the wearer's spells.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Nefarian Chromatic Answer Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Nefarian Chromatic Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +18 spell power. **Signature trigger:** After the wearer takes direct hostile spell damage of Fire, Frost, Arcane, Nature, or Shadow, the item records that school for 8 seconds. If the wearer then lands a direct hostile spell of the same school, one charge is spent. **Effect:** That matching spell deals 12% more direct damage. Periodic ticks, triggered spells, pets, friendly targets, and a different school do not qualify. The incoming spell only sets the short-lived school mark.

**Numbers:** passive spell power: 18; eligible schools: Fire, Frost, Arcane, Nature, Shadow; school mark window seconds: 8; matching spell bonus damage percent: 12. ICD: 60s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 3 × **Eternal Fire** — existing_name_reference; native entry null.
- 3 × **Eternal Shadow** — existing_name_reference; native entry null.
- 2 × **Eternal Air** — existing_name_reference; native entry null.
- 1 × **Nefarian Chromatic Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Shadow** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Chromatic Answer Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Nefarian](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/EasternKingdoms/BlackrockMountain/BlackwingLair/boss_nefarian.cpp)

### Conjoined Discipline — enchant2.legend.twin_emperors

**Encounter inspiration:** Temple of Ahn'Qiraj — Twin Emperors (one paired encounter family). The encounter script separates the twin bosses' melee and arcane roles. This is one paired-family recipe that rewards a wearer's own martial/arcane alternation. The custom effect below is a separate proposal.

**Host / roles:** One chest item of any armor family; replaces one compatible chest-enchant channel, subject to a future route qualification. Roles: hybrid melee-caster damage; characters whose own kit includes both eligible event types.

**Wearer gate:** The wearer must naturally have both a direct melee special and a direct hostile spell available. The enchant grants no class ability and does not force a hybrid loadout.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Twin Emperors Conjoined Discipline Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Twin Emperors Paired Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +12 melee hit rating and +12 spell hit rating. **Signature trigger:** The wearer lands one direct melee special and one direct hostile damage spell on the same hostile target, in either order within 8 seconds. The paired sequence spends one charge. **Effect:** For the next 8 seconds, the wearer's next direct melee special and next direct hostile damage spell each deal 8% more damage. Each event can benefit once; unused benefit expires without a refund.

**Numbers:** passive melee hit rating: 12; passive spell hit rating: 12; pairing window seconds: 8; reciprocal window seconds: 8; bonus damage percent per event: 8; eligible event types: direct melee special, direct hostile damage spell. ICD: 75s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 3 × **Eternal Air** — existing_name_reference; native entry null.
- 3 × **Eternal Earth** — existing_name_reference; native entry null.
- 1 × **Twin Emperors Paired Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Air** — existing_name_reference; native entry null.
- 5 × **Eternal Earth** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Conjoined Discipline Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Twin Emperors](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Kalimdor/TempleOfAhnQiraj/boss_twinemperors.cpp)

### Blood Debt — enchant2.legend.hakkar

**Encounter inspiration:** Zul'Gurub — Hakkar. Hakkar's encounter script includes Blood Siphon and Corrupted Blood. This design turns that blood-exchange theme into an explicitly paid, nonlethal strike; it grants no siphon heal. The custom effect below is a separate proposal.

**Host / roles:** One one-handed melee weapon; replaces one compatible weapon-enchant channel, subject to a future route qualification. Roles: melee damage; health-aware solo play.

**Wearer gate:** The exact finished weapon must be equipped. Only an owner-landed direct hit on a hostile target in combat can call the signature.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Hakkar Blood Debt Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Hakkar Blood Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +15 Shadow resistance points. **Signature trigger:** When a direct melee special lands on a hostile target, the wearer may spend 2% of maximum health to empower the hit, but only if paying that cost leaves the wearer at or above 30% maximum health. Otherwise the charge and health are untouched. **Effect:** A paid hit spends one charge and adds Shadow damage equal to 12% of the hit's pre-critical, unmodified weapon damage. It heals nobody. The health cost is never borrowed from allies or paid by pets.

**Numbers:** passive shadow resistance points: 15; health cost percent of max: 2; minimum postcost health percent: 30; bonus shadow damage percent of precrit weapon damage: 12. ICD: 60s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 4 × **Eternal Shadow** — existing_name_reference; native entry null.
- 2 × **Eternal Life** — existing_name_reference; native entry null.
- 1 × **Hakkar Blood Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Shadow** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Blood Debt Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Hakkar](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/EasternKingdoms/ZulGurub/boss_hakkar.cpp)

### Tidal Interchange — enchant2.legend.lady_vashj

**Encounter inspiration:** Serpentshrine Cavern — Lady Vashj. The encounter script lists Static Charge and Forked Lightning. The proposed Tidal-to-Static sequence uses a short reactive window; no tainted-core or add handling is used as a trigger. The custom effect below is a separate proposal.

**Host / roles:** One mail chest item; replaces one compatible chest-enchant channel, subject to a future route qualification. Roles: mail wearers; nature-pressure defense; any role that can answer a direct hostile hit.

**Wearer gate:** The exact finished mail chest must be equipped. The incoming Nature spell and the wearer's answering hit must involve the same hostile target.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Lady Vashj Tidal Interchange Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Lady Vashj Tidal Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +15 Nature resistance points. **Signature trigger:** Taking direct Nature spell damage from a hostile target opens a 6-second Tidal window. If the wearer lands a direct hostile hit on that same target during the window, it becomes Static-marked for 4 seconds. **Effect:** The wearer's next direct hit on the Static-marked target within 4 seconds adds Nature damage equal to 10% of that hit's pre-critical, unmodified base damage, then spends one charge. No second hit means no charge is spent.

**Numbers:** passive nature resistance points: 15; tidal window seconds: 6; static mark window seconds: 4; bonus nature damage percent of precrit base: 10. ICD: 45s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 4 × **Eternal Water** — existing_name_reference; native entry null.
- 2 × **Eternal Air** — existing_name_reference; native entry null.
- 1 × **Lady Vashj Tidal Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Water** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Tidal Interchange Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Lady Vashj](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Outland/CoilfangReservoir/SerpentShrine/boss_lady_vashj.cpp)

### Phoenix Reprise — enchant2.legend.kaelthas

**Encounter inspiration:** Tempest Keep: The Eye — Kael'thas Sunstrider. Kael'thas's encounter script includes a summoned Phoenix and phase transitions. Reprise borrows a brief return-of-fire image; it explicitly grants no resurrection, healing, damage prevention, or extra life. The custom effect below is a separate proposal.

**Host / roles:** One caster-appropriate head item; replaces one compatible head-enchant channel, subject to a future route qualification. Roles: spell damage; casters who can cast a direct hostile spell after surviving a dangerous hit.

**Wearer gate:** The exact finished head item must be equipped. The wearer must survive the qualifying hostile hit and personally land the follow-up spell.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Kael'thas Phoenix Reprise Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Kael'thas Phoenix Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +18 spell critical-strike rating. **Signature trigger:** A direct hostile hit that leaves the wearer alive at or below 30% maximum health spends one charge and opens a 5-second Phoenix window. **Effect:** The wearer's next direct hostile spell during the window causes a single delayed Fire echo one second later for 30% of the wearer's spell power. It is one bounded hit, not a periodic effect; it cannot crit, recur, restore mana, heal, or prevent death. The echo resolves only if wearer and hostile target remain alive and eligible.

**Numbers:** passive spell critical strike rating: 18; health threshold percent: 30; phoenix window seconds: 5; fire echo total percent of spell power: 30; fire echo delay seconds: 1. ICD: 90s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 4 × **Eternal Fire** — existing_name_reference; native entry null.
- 2 × **Eternal Air** — existing_name_reference; native entry null.
- 1 × **Kael'thas Phoenix Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Fire** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Phoenix Reprise Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Kael'thas Sunstrider](https://www.azerothcore.org/doxygen/d4/d67/boss__kaelthas_8cpp.html)

### Fel Pursuit — enchant2.legend.illidan

**Encounter inspiration:** Black Temple — Illidan Stormrage. Illidan's encounter script includes his demon transformation, Flame Crash, and changing phases. Fel Pursuit turns the theme into voluntary movement; it never teleports, charges, roots, or takes control of the wearer. The custom effect below is a separate proposal.

**Host / roles:** One shoulders item; replaces one compatible shoulder-enchant channel, subject to a future route qualification. Roles: melee damage; voluntary gap-closing; mobile solo play.

**Wearer gate:** The exact finished shoulder item must be equipped. The wearer must move under their own control toward a hostile combat target and land the qualifying melee special personally.

**Unlock:** A per-character entitlement to this design only. It is created only when the recipient learns the matching custom pattern folio; it is not granted by possessing the craft relic, by guild membership, or by a boss legacy attribute reward. **Pattern Folio:** Illidan Fel Pursuit Pattern Folio — transferable until learned, consumed once, and teaches this design only. **Per-craft relic:** Illidan Fel Relic — transferable, earned from the named encounter route, and consumed once per finished item.

**Passive:** +18 Agility. **Signature trigger:** While in combat with a hostile target, the wearer voluntarily moves at least 8 yards toward it and lands a direct melee special on that same target within 3 seconds. One charge is then spent. **Effect:** For 4 seconds, the wearer gains +20% run speed and their next direct melee special against that target deals 10% more damage. Movement is ordinary player-controlled movement; there is no forced relocation or control immunity.

**Numbers:** passive agility: 18; voluntary distance yards: 8; approach to hit window seconds: 3; pursuit duration seconds: 4; run speed percent: 20; next melee special bonus damage percent: 10. ICD: 60s, starts on charge spend. Charges: 30/30 initial/max. At zero, passive remains and signature is inert.

**Consumed bill for one finish:**

- 32 × **Infinite Dust** — existing_name_reference; native entry null.
- 8 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 4 × **Dream Shard** — existing_name_reference; native entry null.
- 2 × **Abyss Crystal** — existing_name_reference; native entry null.
- 4 × **Eternal Shadow** — existing_name_reference; native entry null.
- 2 × **Eternal Fire** — existing_name_reference; native entry null.
- 1 × **Illidan Fel Relic** — custom_unimplemented; native entry null.

**Recharge bill per batch:**

- 5 × **Infinite Dust** — existing_name_reference; native entry null.
- 5 × **Greater Cosmic Essence** — existing_name_reference; native entry null.
- 5 × **Eternal Shadow** — existing_name_reference; native entry null.
- 10 active city minutes restores exactly 5 charges when at least 5 are missing, up to 30; boss relic/core and pattern folio are not required.

**Five stages:** Preparation 35m; Dust substrate 75m; Elemental temper 60m; Relic binding 85m; Inspection and attunement 45m = 300 active city minutes.

**Maker and teaching:** Proposed maker record: Fel Pursuit Signature Mastery, credited once to the artisan after a real, delivered commission using a distinct supplied item and one consumed boss relic. A separate, individually accepted city lesson can grant this exact per-character custom recipe entitlement to one learner. The learner must independently meet native Enchanting skill/rank and character-level requirements; no guild-wide or automatic learning occurs. No native skill/rank/XP or boss legacy/attributes are awarded.

**Encounter source:** [AzerothCore encounter script: Illidan Stormrage](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Outland/BlackTemple/boss_illidan.cpp)
