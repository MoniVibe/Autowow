# Wrath legendary enchant expansion — design only

**Status:** `DESIGN_ONLY_NOT_NATIVE` · **Native active:** `false` · **Lane:** `wrath`
**Count:** eight distinct boss-derived candidates. The full structured records are in [WRATH.json](WRATH.json).

These are proposed replacement finishes for a real, compatible host item. Each provides one small passive and one narrowly triggered, charged signature; it does not add a blanket attribute package. No native IDs, recipes, custom drops, item properties, effects, blueprints, or live unlocks are claimed. Every boss entitlement, core, enchant, and recharge design remains unimplemented.

The themes are interpretation, not a claim that these effects already exist. The local [boss-legacy policy](../../work/server-strategy-20261008/mod-autowow-strategy/docs/BOSS_LEGACIES.md) expressly uses Sapphiron for frost endurance and Razuvious for martial discipline. Blizzard's encounter pages establish raid context for [Naxxramas](https://worldofwarcraft.blizzard.com/en-us/news/23826539), [Malygos](https://worldofwarcraft.blizzard.com/en-gb/news/23837381), [Ulduar](https://worldofwarcraft.blizzard.com/en-us/news/23897173), [Trial of the Crusader](https://worldofwarcraft.blizzard.com/en-us/news/6873702), and [Icecrown Citadel](https://worldofwarcraft.blizzard.com/en-us/news/24013834). The six remaining theme choices are creative design extrapolations from those encounters.

## Shared gates and operation

- A real level-80 wearer must personally earn the durable, non-tradable blueprint entitlement through eligible participation and victory at the named canonical raid encounter. It is separate from the custom boss relic: one relic is reserved and consumed per completed craft, while the entitlement remains. The relic is a proposed, tradable custody item with no live drop or entry.
- A city artisan must have real native Enchanting 450, an approved city station, valid inputs and any future admitted recipe route. The artisan need not have killed the boss. Host equip rules, character level/rank, native recipe and profession checks remain authoritative; no free skills or XP are granted.
- Each accepted commission has five sequential stages: eligibility and custody review; substrate preparation; host-channel tempering; boss-signature binding; inspection and attunement. The exact stage minutes appear on each card and total four to five active hours. Only continuously observed, eligible artisan time in the city counts. Work pauses on combat, departure, unsafe state, logout, outage, disconnect, or uncertain custody. There is no wall-clock/login catch-up or excess-time carry. Restart resumes only reconciled durable progress; a timer never creates an item or skill gain.
- Each finished item starts with 30 signature charges. One city recharge uses **6 Infinite Dust + 1 Greater Cosmic Essence for 15 active minutes** and restores up to **10 missing charges**, capped at 30. It uses ordinary materials only; no boss relic is needed for a top-up. Empty charges disable only the signature, not that item's passive. Charges and ICD persist on the exact item through relog, death, equip swap, bank, mail, and trade. Equipping another copy does not inherit them.
- All candidates replace one compatible admitted enchant/socket channel; they do not stack into every slot. Static passives apply only while the exact host is equipped. Any live route must first qualify each host/slot, effect, saved item identity, cooldown and removal behavior. Active signature effects end on expiry and the normal unequip/logout/death cleanup.
- The named boss encounter gates blueprint entitlement and its one relic core per craft. After acquisition, each signature works against eligible hostile NPCs in ordinary world combat and other raids using the same boss-inspired event type. Require a genuine combat-credit NPC at least five levels below the wearer or an elite, rare, or boss; exclude PvP, trivial farm targets, encounter adds, summons, pets, friendly/self/environment damage, periodic ticks, reflected or proc/item/legacy damage, probes, dummies and admin fixtures. Unique event identity prevents callback replay; signatures cannot trigger themselves or another proc, and no effect targets friends. Per-item charges and cooldowns regulate each mechanic; they do not cap earned traits or loadouts.
- The lead artisan receives one proposed noncombat signature-mastery credit per distinct useful completed project, keyed by design, wearer, original host-item GUID and project ID. An apprentice with real Enchanting training may receive one noncombat lesson credit after recorded work on that project. Both are separate from native progression, grant no combat stats or XP, and cannot repeat through retry, reshipment, transfer or reapplication.
- These designs issue no boss-legacy +5%/+10% primary attributes and do not alter or duplicate any separate native boss trait. No legacy award is made by an enchant or workshop timer.

## Candidate overview

| Boss and raid encounter | Design | Host | Persistent passive | Signature and measured limit |
|---|---|---|---|---|
| Sapphiron — Naxxramas, Frostwyrm Lair | **Sapphiron's Glacial Watch** | Shield | +8 Frost Resistance points | Next hostile hit absorbs 2% max health, capped at 1,000; 6 sec window; 90 sec ICD |
| Instructor Razuvious — Naxxramas, Military Quarter | **Instructor's Reprisal** | One-handed melee weapon | +10 Defense Rating | Next hit against the same eligible hostile NPC gains 50 flat physical damage; 6 sec window; 75 sec ICD |
| Kel'Thuzad — Naxxramas, Frostwyrm Lair | **Kel'Thuzad's Frostbound Chain** | Main-hand caster weapon | +12 Spell Power | Third direct damaging spell after a two-hit chain on the same eligible hostile NPC gains 2.5%, capped at +1,200; 90 sec ICD |
| Malygos — Eye of Eternity | **Malygos's Arcane Coffer** | Chest armor | +8 Arcane Resistance points | Reduce one direct Arcane hit from the same eligible hostile NPC by 4%, capped at 900; 6 sec window; 90 sec ICD |
| Yogg-Saron — Ulduar | **Whisper-Cleared Circlet** | Head armor | +8 Intellect | After 6 sec without a cast, next direct hostile-NPC spell returns 1% max mana, capped at 350; 60 sec ICD |
| Algalon the Observer — Ulduar | **Algalon's Stellar Interval** | Shoulder armor | +12 Haste Rating | +60 Haste Rating for 6 sec on a direct hit after 10 sec of continuous combat; 90 sec ICD |
| Anub'arak — Trial of the Crusader **raid** | **Anub'arak's Buried Carapace** | Shield | +16 Block Value | Next block against the same eligible hostile NPC gains 24 flat Block Value; 8 sec window; 60 sec ICD |
| The Lich King — Icecrown Citadel, Frozen Throne | **Frostmourne's Remembrance** | Two-handed melee weapon | +8 Stamina | After surviving a direct hit from an eligible hostile NPC, self-heal after 2 sec for 1% max health, capped at 600; 120 sec ICD |

All capacities are 30 charges. Each charge is spent only when its complete qualifying event resolves. An active timed effect is self-only and non-stacking. For example, the Sapphiron signature can absorb at most 1,000 damage per trigger and 30,000 over a fully charged item before cooldown/valid-hit limits; the Lich King recovery can return at most 600 health per trigger and 18,000 across all 30 charges. Other effects similarly have one hit/cast per charge and an explicit flat cap or short duration. These are tuning ceilings, not promised native values.

## Material cards and stage plans

Material names below are existing, name-verified references in [MATERIAL_BILLS.json](../../outputs/profession-catalogues-20261009/MATERIAL_BILLS.json), not admitted recipes. JSON deliberately stores their `native_item_entry` as `null` and `kind` as `existing_name_reference`; this design does not mint or assign IDs. Each boss relic is `custom_unimplemented`, quantity one, and has no native entry. Host armor or weapon is supplied separately and is not part of the bill. The complete bill is reserved for one project and consumed once by its future admitted final craft; the five timed stages do not create intermediate outputs or consume the bill repeatedly.

Reusable equipment is listed separately from consumption: **one city Enchanting Station** and the qualified artisan's normal nonconsumable implements. No new tool item is defined.

### Sapphiron's Glacial Watch

**Thematic reference:** Sapphiron's frost command and role guarding Kel'Thuzad inspire a small personal frost defense.
**Proposed effect:** +8 Frost Resistance remains while the shield is equipped. A direct Frost hit from any eligible hostile NPC can spend one charge and arm a self-only absorb: 2% maximum health, capped at 1,000 damage, for the next direct hostile damage event within 6 seconds. It expires unused; 90-second ICD.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Water | 8 |
| Abyss Crystal | 4 |
| Sapphiron Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 30 min; glacial substrate 50; shield tempering 60; frost signature 60; inspection 40. **Total: 240 active minutes.**

### Instructor's Reprisal

**Thematic reference:** Razuvious is the martial-discipline lesson in the local boss-legacy direction.
**Proposed effect:** +10 Defense Rating remains on the one-handed weapon. A successful parry of an eligible hostile NPC's direct melee attack arms one 50-flat-physical-damage bonus for the wearer's next direct weapon hit against that same NPC within 6 seconds; 75-second ICD. The added damage cannot trigger procs.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Earth | 8 |
| Abyss Crystal | 4 |
| Instructor Razuvious Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 30 min; counter-discipline substrate 55; weapon tempering 60; parry signature 65; inspection 40. **Total: 250 active minutes.**

### Kel'Thuzad's Frostbound Chain

**Thematic reference:** Kel'Thuzad's Naxxramas command and sorcery inspire a deliberate spell sequence rather than a generic damage aura.
**Proposed effect:** +12 Spell Power remains on the main-hand caster weapon. Two direct, non-periodic hostile damage spells against the same eligible NPC within 6 seconds prime the next direct damage spell against it. That one cast gains 2.5% damage, capped at +1,200, and must land within the next 6 seconds; 90-second ICD. No periodic ticks or healing qualify.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Shadow | 8 |
| Abyss Crystal | 4 |
| Kel'Thuzad Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 35 min; frostbound substrate 55; caster-channel tempering 65; chain signature 65; inspection 40. **Total: 260 active minutes.**

### Malygos's Arcane Coffer

**Thematic reference:** Malygos is the blue dragon aspect encountered in the Eye of Eternity; the design turns arcane mastery into a brief containment response.
**Proposed effect:** +8 Arcane Resistance remains on the chest. A direct Arcane hit from any eligible hostile NPC arms a 6-second window in which the next direct Arcane hit from that same NPC is reduced by 4%, capped at 900 damage prevented; 90-second ICD. It grants no immunity or reflection.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Air | 8 |
| Abyss Crystal | 4 |
| Malygos Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 35 min; arcane substrate 60; chest tempering 65; containment signature 65; inspection 45. **Total: 270 active minutes.** Chest route remains unqualified in the source catalogue.

### Whisper-Cleared Circlet

**Thematic reference:** Ulduar's captive Old God and the raid's mind-bending setting inspire clarity through a short pause in casting. The effect is a proposed mana return, not immunity to control.

**Proposed effect:** +8 Intellect remains on the head item. After 6 seconds without a wearer spell cast, the first direct damage spell that hits an eligible hostile NPC returns 1% maximum mana, capped at 350, only if the wearer has mana missing; 60-second ICD. No overflow or transfer.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Shadow | 8 |
| Abyss Crystal | 4 |
| Yogg-Saron Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 40 min; circlet substrate 60; head tempering 70; clarity signature 65; inspection 45. **Total: 280 active minutes.** Head route remains unqualified in the source catalogue.

### Algalon's Stellar Interval

**Thematic reference:** Algalon's role as the Observer inspires one precisely timed response during continuous combat.
**Proposed effect:** +12 Haste Rating remains on the shoulders. The first direct hit on an eligible hostile NPC at least 10 seconds after a durable, continuously observed combat start grants +60 Haste Rating for 6 seconds; 90-second ICD. Changing targets, restarting a pull, relogging, or swapping gear cannot reset the timer.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Air | 8 |
| Abyss Crystal | 4 |
| Algalon Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 40 min; stellar substrate 65; shoulder tempering 70; interval signature 65; inspection 45. **Total: 285 active minutes.** Shoulder route remains unqualified in the source catalogue.

### Anub'arak's Buried Carapace

**Thematic reference:** The Trial's buried nerubian foe inspires controlled carapace and block timing. This card is explicitly for the **Trial of the Crusader raid** boss, never the Anub'arak in the Azjol-Nerub dungeon.
**Proposed effect:** +16 Block Value remains on the shield. A real shield block of an eligible hostile NPC's direct melee attack arms the next successful block against that same NPC within 8 seconds to absorb +24 flat Block Value; 60-second ICD.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Earth | 8 |
| Abyss Crystal | 4 |
| Anub'arak Trial Raid Relic Core — custom, unimplemented | 1 |

**Stages:** raid-version eligibility/custody 40 min; carapace substrate 65; shield tempering 75; block signature 70; inspection 50. **Total: 300 active minutes.**

### Frostmourne's Remembrance

**Thematic reference:** Frostmourne and the Lich King inspire a restrained memory of the wearer's own struggle, with no harvesting of another character's soul.
**Proposed effect:** +8 Stamina remains on the two-handed weapon. If the wearer survives a direct non-periodic hit from an eligible hostile NPC, the same wearer receives 1% maximum health after 2 seconds, capped at 600, only while alive; 120-second ICD. It cannot prevent the hit from being lethal, resurrect anyone, or affect an ally.

| Consumed input | Quantity |
|---|---:|
| Infinite Dust | 40 |
| Greater Cosmic Essence | 10 |
| Dream Shard | 6 |
| Eternal Shadow | 8 |
| Abyss Crystal | 4 |
| Lich King Relic Core — custom, unimplemented | 1 |

**Stages:** eligibility/custody 45 min; remembrance substrate 65; weapon tempering 75; personal-memory signature 70; inspection 50. **Total: 305 active minutes.**

## Release dependencies

The [Enchanting catalogue](../../outputs/profession-catalogues-20261009/enchanting.md) marks several head, shoulder, chest and weapon routes unqualified and all legendary procs as proposed. Before any pilot, each selected host/channel and exact event behavior needs separate admission and observed lifecycle coverage. The [workshop policy](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md) requires durable project reservations, actual native craft receipts and native progression; a completed timer alone cannot output an enchant or award skill. The boss entitlements, relic custody route, per-item signature state, recharge service, teaching record and artisan mastery record all remain design dependencies.
