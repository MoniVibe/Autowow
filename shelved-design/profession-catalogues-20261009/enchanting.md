# Enchanting catalogue — design only

**Quantified materials:** [Enchanting shopping lists](MATERIAL_BILLS.md#enchanting). Costs are proposed; the table states output quantities and marks unimplemented relic cores.

This catalogue turns the approved workshop design into proposed enchanting work. A buyer starts with a real,
role-appropriate weapon or armor base; the city enchanter supplies a credited compatible finish. A finish replaces
the compatible native enchant or socket channel, so enchantments do not all stack. These records admit no native IDs,
recipes, casts, properties, client behavior or runtime changes.

## Proposed coverage

There are **proposed counts** of eight field finishes, four masterwork finishes and six legendary signature finishes.
Field work is affordable for weak adventurers and uses common lawful inputs. Masterwork work adds a stronger specialty
and a more involved local finish. Legendary work is a shared finishing commission: the base maker, core owner,
collaborators and city enchanter each have recorded work, material and lineage. Common dust and shards remain available
from lawful stock or disenchanting authorized spares; useful equipped gear is never destroyed and common needs are not
raid-gated.

Field entries below are ordinary stat allocations only. They apply through an admitted compatible native enchant or
socket channel and add no proc, aura, trigger, event hook or new adapter. Masterwork and legendary proc or support-aura
behaviors remain proposed: the exact equipped item is their source; hostile or eligible ally targeting, native group
and range rules, cooldown, removal on unequip/logout/death/removal/group or range change, nonmultiplication of identical
aura families and the no-recursive-proc rule all need native or qualified custom admission and observation. A chest,
legs, head or shoulders row names a qualified native-compatible stat channel only where stated; it does not claim
that an unsupported native cast exists.

## Field finishes

| Exact design ID | Exact proposed name | School / mentorship / style | Ready state | Native blockers |
|---|---|---|---|---|
| enchant.field.warding-spark | Warding Spark | City warding school; mentored by a defensive station keeper; restrained stamina/defense stat style | Design-ready when a buyer supplies a qualified defensive weapon and an admitted compatible channel | Only the ordinary native-compatible stat channel may be used; no new trigger, property or proc adapter is admitted |
| enchant.field.cinder-edge | Cinder Edge | City ember-edge school; mentored by a furnace apprentice-master; economical strength/agility/haste stat style | Design-ready when a real melee weapon has an appropriate damage/stat envelope and an admitted compatible channel | Only the ordinary native-compatible stat channel may be used; no fire proc, event hook or new adapter is admitted |
| enchant.field.mindglass-focus | Mindglass Focus | City mindglass school; mentored by a caster archivist; clear spell-power/haste stat style | Design-ready for a real main-hand caster weapon with an admitted compatible channel; off-hand focus remains unqualified pending native route check | Off-hand focus is explicitly unqualified; use an existing native-compatible ordinary equivalent only if later admitted; no new caster trigger is admitted |
| enchant.field.stormstring | Stormstring Finish | City stormstring school; mentored by a ranger fletcher-enchanter; light agility/haste stat style | Design-ready only if a native-compatible ranged channel is verified | Ranged channel remains unqualified pending native route check; use an existing native-compatible ordinary equivalent only if later admitted; no lightning proc is admitted |
| enchant.field.frostguard-chest | Frostguard Binding | City frostguard school; mentored by a cold-ward keeper; modest stamina/defense stat style | Design-ready only after a qualified armor-family chest route, real chest base and compatible stat channel are accepted | Chest native-compatible stat channel must be qualified; no trigger, aura or new adapter is admitted |
| enchant.field.steadfast-legs | Steadfast Legweave | City legweave school; mentored by an expedition quartermaster; practical stamina/role-stat style | Design-ready only if a native-compatible legs channel is verified | Legs channel remains unqualified pending native route check; use an existing native-compatible ordinary equivalent only if later admitted; no proc is admitted |
| enchant.field.clearsight-helm | Clearsight Helm Finish | City clearsight school; mentored by a surveyor-mage; anti-control stat allocation style | Design-ready only if a native-compatible head channel is verified | Head channel remains unqualified pending native route check; use an existing native-compatible ordinary equivalent only if later admitted; no clarity trigger is admitted |
| enchant.field.prismatic-shoulders | Prismatic Shoulder Seal | City prismatic school; mentored by a school ward examiner; narrow offensive-stat style | Design-ready only if a native-compatible shoulders channel is verified | Shoulders channel remains unqualified pending native route check; use an existing native-compatible ordinary equivalent only if later admitted; no school proc or aura is admitted |

The field line is useful at ordinary progression because the base item carries the weapon damage, armor family and
role stats. A finish is a compatible stat allocation, never a claim that dust on a white item becomes a complete
Northrend kit. Unsupported ranged, off-hand, head, legs and shoulders channels stay unqualified until their native
routes are checked; an existing native-compatible ordinary equivalent may be used later if admitted.

## Masterwork finishes

| Exact design ID | Exact proposed name | School / mentorship / style | Ready state | Native blockers |
|---|---|---|---|---|
| enchant.masterwork.emberward-discipline | Emberward Discipline | Master emberward school; mentored by a city fire-ward master; disciplined tank retaliation with optional local fire support | Ready for a quoted masterwork order after a real one-handed tank weapon and admitted support choice are accepted | No retaliation, aura or native IDs admitted; exact item, group/range, cooldown, removal and nonstacking observation remain blocked |
| enchant.masterwork.arcane-accord | Arcane Accord | Master arcane school; mentored by a city spellwright; focused next-spell preparation style | Ready for a quoted masterwork order after a real caster weapon and role budget are accepted | No native caster proc or ID admitted; hostile spell trigger, duration, cooldown and no-friendly-charge proof remain blocked |
| enchant.masterwork.ironroot-plate | Ironroot Plate Seal | Master ironroot school; mentored by a plate ward master; grounded front-line stance style | Ready for a quoted masterwork order after a qualified plate chest route and real chest are accepted | Chest native/custom route, property, trigger, cooldown and removal remain blocked |
| enchant.masterwork.tideglass-mail | Tideglass Mail Finish | Master tideglass school; mentored by a city healer-enchanter; restrained mail tide support style | Ready for a quoted masterwork order after a real mail shoulder base and qualified shoulders route are accepted | Shoulders native/custom route, heal/hostile trigger, support aura range, cooldown, removal and nonstacking remain blocked |

Masterwork maker credit is earned by actual completed work and useful later commissions. It is separate from any
personal boss legacy and does not grant a universal strong-player buff.

## Legendary signature finishes and shared workshop stages

| Exact design ID | Exact proposed name | School / mentorship / style | Ready state | Native blockers |
|---|---|---|---|---|
| enchant.legendary.emberheart-furnace | Emberheart, Furnace Pattern | Emberheart school; mentored by a credited fire-core finisher; two-handed melee eruption with optional personal fire support | Blueprint-ready once the approved core, real two-handed base, role purpose, stat allocation and collaborators are accepted | Core lineage, weapon property bundle, hostile eruption, support aura lifecycle, cooldown, removal and nonrecursive proof are all blocked |
| enchant.legendary.emberheart-hearthguard | Emberheart, Hearthguard Pattern | Emberheart school; mentored by a credited hearthguard finisher; one-handed defense retaliation with optional local fire support | Blueprint-ready once the approved core, real one-handed tank base, role purpose, stat allocation and collaborators are accepted | Core lineage, defensive event, retaliation, support aura range, cooldown, removal and nonrecursive proof are all blocked |
| enchant.legendary.blackflight-adaptation | Blackflight Adaptation | Blackflight school; mentored by a credited school-adaptation finisher; caster response constrained to the last qualified hostile school | Blueprint-ready once the approved core, real caster weapon, role purpose, school choice, stat allocation and collaborators are accepted | Core lineage, school mapping, hostile trigger, adaptive property, support lifecycle, cooldown and nonrecursive proof are all blocked |
| enchant.legendary.prismatic-resonance | Prismatic Resonance | Prismatic school; mentored by a credited chromatic finisher; shoulders-based narrow school ward | Blueprint-ready once the approved core, real shoulders base, qualified slot route, role purpose, stat allocation and collaborators are accepted | Core lineage, shoulders route, school mapping, ward property, group/range lifecycle, cooldown and nonrecursive proof are all blocked |
| enchant.legendary.tidebinder-current | Tidebinder Current | Tidebinder school; mentored by a credited tide finisher; mail chest current for direct heal or hostile attack | Blueprint-ready once the approved core, real mail chest, qualified chest route, role purpose, stat allocation and collaborators are accepted | Core lineage, chest route, heal/hostile trigger, target rule, support lifecycle, cooldown and nonrecursive proof are all blocked |
| enchant.legendary.sepulchral-conductor | Sepulchral Conductor | Sepulchral school; mentored by a credited frost-shadow finisher; caster/healer weapon ward with local anti-magic support | Blueprint-ready once the approved core, real caster/healer weapon, role purpose, school choice, stat allocation and collaborators are accepted | Core lineage, hostile-school event, ward property, support lifecycle, cooldown and nonrecursive proof are all blocked |

Each legendary design has exactly five meaningful stages:

- **Preparation** — qualify the approved core or pattern, buyer purpose, compatible form, stat priorities, support
  choice and complete the accepted blueprint.
- **Forging or weaving** — the city enchanter prepares a compatible substrate while the qualified weapon or armor
  maker constructs or supplies the real base; this is recorded child work, not a usable final item.
- **Tempering** — apply the accepted primary stat emphasis within the base form's role budget. More of one priority
  means less room for another; it does not grant every stat at maximum.
- **Signature finishing** — the credited legendary enchanter binds one admitted family signature and any separately
  qualified support finish. A fire aura and a fire projectile remain separate effects.
- **Inspection and attunement** — verify exact item identity, maker and collaborators, replacement channel,
  equipped lifecycle and quoted properties before delivery.

The early records are work or genuine crafted subcomponents. Final materialization waits for every stage and an
admitted native or explicitly qualified custom route. The item keeps its exact identity and maker lineage through
bank, mail and trade; later maker skill gains do not mutate old gear.

## Provisioning and release gates

The proposed city contribution is independent: an enchanter supplies lawful dust/shards, technique and station work,
then receives credit for the actual finish. A legendary commission can be shared with a weapon or armor maker and a
core owner, but every collaborator has a real child commission, qualified materials and recorded work. No project
sharing creates free skill.

Common inputs are deliberately ordinary: lawful stock or disenchanting authorized spares can provide dust and shards.
No useful equipped item is destroyed, and no common input is raid-gated. The compatible native enchant or socket
channel is replaced rather than multiplied. Existing personal boss legacies remain separate from maker rewards.

### Proposed counts

- Proposed field finishes: eight.
- Proposed masterwork finishes: four.
- Proposed legendary signature finishes: six.
- Proposed total candidates: eighteen.

### Three best gates before any native pilot

- **Compatibility gate:** admit one exact base form, slot route, stat bundle and replacement channel, with a refusal
  path for an unsupported chest, legs, head or shoulders route.
- **Provenance gate:** settle lawful materials, core lineage, city station, maker/collaborator child work and the
  accepted blueprint before a stage can advance.
- **Lifecycle gate:** observe the exact equipped item's hostile/ally target rules, cooldown, group/range behavior,
  unequip/logout/death/removal cleanup, same-family nonstacking and no recursive proc chain through save/load, mail,
  equip and clean removal.
