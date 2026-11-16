# Runesmithing legendary inscriptions

Design-only proposals for twelve source-themed inscriptions on existing equipment hosts. The structured records in
[`LEGENDS.json`](LEGENDS.json) are authoritative for complete bills, gates, stage times, mechanics, and maintenance.
Every recipe uses one Trigger → Transformation → Release from the shared grammar; each exact triple and host still
needs separate admission. No item, boss drop, blueprint, relic, skill, or effect is installed.

| Source encounter | Proposed inscription on supplied host | Pattern | Signature behavior | Work | Maintenance |
|---|---|---|---|---:|---|
| Ragnaros | Magmawake Inscription on caster main-hand | `spell_alternation → gather_heat → haste_window` | Two direct hostile spells of different schools, including Fire, prime the next Fire cast for a brief self-only spell-haste window. | 155 min | 30 charges; exact five-charge city refill in JSON |
| Sapphiron | Stillwinter Inscription on boots | `steadfast_hold → bind_protection → escape_window` | Hold position within 1 yard under threat for 8 seconds, then trim up to 1 second from one hostile root/slow and gain 10% run speed briefly. | 135 min | Durable |
| Twin Emperors | Concordant Tempo Inscription on caster main-hand | `cadence_combat → resonate_pair → haste_window` | Interrupt a hostile cast, then land a different-school spell on that same NPC for 6% self-only spell haste. | 145 min | Durable |
| Nefarian | Blackwing Riposte Inscription on one-handed weapon | `guard_parry → gather_impact → reprisal_hit` | Parry and interrupt the same hostile NPC, then add bounded damage to the next melee special against it. | 155 min | Durable |
| Kael'thas | Phoenix Aegis Inscription on shield | `guard_block → bind_protection → ward_absorb` | After a block, the same attacker begins a spell; a short ward absorbs part of the wearer's next direct spell damage. | 160 min | 30 charges; exact five-charge city refill in JSON |
| Illidan | Blackened Pursuit Inscription on bracers | `ambush_survive → bind_protection → escape_window` | Survive a new engagement's opener and a second attacker; gain a brief speed window and a capped slow trim. | 160 min | Durable |
| Malygos | Leyline Conductor Inscription on caster main-hand | `spell_alternation → focus_arcana → elemental_burst` | Alternate direct spells including Arcane, then prime the next Arcane cast for a capped single-target burst. | 150 min | Durable |
| Algalon | Last Constellation Inscription on chest armor | `ambush_survive → bind_protection → ward_absorb` | Survive a hostile opener that leaves the wearer below 40% but alive; prepare a bounded ward after the hit resolves. | 170 min | 24 charges; exact five-charge city refill in JSON |
| Azuregos | Blue Dragon's Measure Inscription on caster main-hand | `spell_alternation → resonate_pair → haste_window` | Hold within 1 yard under threat, then cast two different-school spells on one hostile NPC for a short self-only spell-tempo window. | 140 min | Durable |
| Lord Kazzak (classic) | Doomcaller's Mercy Inscription on healer main-hand | `mercy_heal → weave_life → heal_echo` | Heal a critically injured party member; the next heal on that same living target gains one capped echo. | 155 min | 24 charges; exact five-charge city refill in JSON |
| Doomwalker | Earthshaker Countermark Inscription on melee weapon | `pursuit_approach → gather_impact → haste_window` | Voluntarily close on a hostile NPC, land a melee special, then gain brief attack speed; no quake or forced movement. | 155 min | Durable |
| Ysondre | Dreambough Cadence Inscription on healer main-hand | `mercy_heal → weave_life → ward_absorb` | A useful heal to a genuinely injured ally prepares a small, single-target ward against the next hostile hit. | 140 min | Durable |

Each order has five city-work stages totaling at least 120 active minutes. Workshop time advances only under the
city, safety, artisan, input, and custody conditions in [`WORKSHOPS.md`](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md);
offline time and outage time do not count. The existing host is supplied separately and is never consumed as a
recipe ingredient. Native class, character-level, host, and proficiency checks remain in force, and an inscription
grants no class ability. The wearer also needs an approved per-host role and effect-power budget before attachment.

Acquisition is separate from use: only a qualifying participant in the exact real boss encounter may receive the
proposed transferable pattern blueprint and one scarce physical relic as future custom loot. The city artisan
learns the blueprint in town and consumes exactly one relic for each commission; the artisan need not enter the
encounter. Once made, signatures work against eligible genuine hostile NPCs generally, not only the source boss.
Four exceptionally strong inscriptions have capped charge stores and ordinary, relic-free city refill bills;
the other eight are durable and create no refill chore. All charged state and item-family cooldowns persist through
trade, swaps, and relog. No global earned-trait ceiling or four-trait equipment cap is introduced.

Existing material names are historical references from
[`MATERIAL_REFERENCE.json`](../enchantment-expansion-20261009/MATERIAL_REFERENCE.json), not recipe admission.
All recipe ingredient records therefore keep `native_item_entry: null`; every physical relic and every proposed
Rune Blank/Runic Binder remains custom and unimplemented. Each artisan mastery credit requires a completed paid useful order with inputs and result receipted and returned on an approved existing host. Guild or same-account supply is allowed; the order or inscription revision must be materially useful. No-op reapplications, authorship, idle time, relic pickup, and recycling earn nothing.

Design constraints and local evidence: [`BRIEF.md`](BRIEF.md), the prior catalogue's
[`INDEX.md`](../enchantment-expansion-20261009/INDEX.md),
[`BOSS_LEGACIES.md`](../../work/server-strategy-20261008/mod-autowow-strategy/docs/BOSS_LEGACIES.md),
[`WORKSHOPS.md`](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md), and the shared
[`GRAMMAR.json`](GRAMMAR.json). Encounter names supply creative themes; all listed mechanics are explicit custom
proposals rather than claims about native drops or installed encounter abilities.
