# Profession gear: useful kits and notable makers

Six explicitly requested GPT-5.6 Luna high passes produced five profession catalogues and an everyday gearing
plan. Root integrated the designs and corrected field-tier complexity and item-identity accounting. The catalogue
contains90 new concepts:40field,20masterwork,30legendary. These are original proposed blueprints, not admitted
native items, recipes, spells, buffs or a functioning bot supply update.


## Material shopping lists

[Material quantities for all 90 designs](profession-catalogues-20261009/MATERIAL_BILLS.md) now gives the output count, ingredient quantities and supply route for every design.
These are proposed costs, using a level 68–72 field supply reference; existing ingredient names were
verified in the native item catalogue, but native recipe costs and custom relic-core drops are not admitted.

| Proposed output | Direct bill |
|---|---|
| 1 Copperleaf Scout Leggings | 24 Borean Leather + 2 Eternium Thread |
| 1 Cinderweave Adventurer's Vest | 8 Bolt of Frostweave + 2 Eternium Thread |
| 1 Skylance Repeater | 12 Saronite Bar + 4 Cobalt Bar + 4 Handful of Cobalt Bolts + 1 Chalcedony |
| 200 Frostline Cartridges | 2 Saronite Bar + 2 Cobalt Bar + 1 Crystallized Water |

The [combined catalogue with bills](profession-catalogues-20261009/COMBINED_WITH_MATERIALS.json) carries the same quantities for later implementation.

## The practical equipment track

Adventurers need usable weapons and body gear before elaborate artifact commissions. The proposed queue prioritizes
weapons/ammunition, then chest/legs/head/shoulders, then bracers/feet/waist/gloves. Cloaks and jewelry follow those
needs; trinkets are lower priority. Class, level, armor, weapon proficiency, role and actual equipped-item stats
decide whether a candidate is useful. White quality alone is not a rejection; obsolete filler is not a good upgrade
merely because it has a higher item level or fills an empty slot.

The40 field concepts give ordinary stat/gear/supply baselines without requiring newly invented proc/aura systems.
Leatherworkers and tailors each cover eight body slots. Enchanters propose role-specific stat finishes; jewelcrafters
propose gems/settings and secondary jewelry only where a real compatible route exists. Engineers supply practical
ranged/headgear/component/finite-stock routes. A native-equivalent mapping is still required before any field concept
can enter actual production. The first release should use already qualified native recipes/items rather than wait
for every custom blueprint. Blacksmith weapon/plate production remains part of the same shared gear demand.

One artisan has one active order. Urgent work receives queue priority or another qualified available artisan;
there is no imaginary parallel workshop capacity. Long legendary projects defer when urgent demand dominates.
Completed ordinary pieces can ship progressively. Skill/training and character-work progression must use admitted
actual-work mechanisms; a catalogue or donation does not grant free skill, XP or a recipe.

## Five professions, distinct work

| Profession | Field contribution | Masterwork/legendary identity | Catalogue |
|---|---|---|---|
| Leatherworking | Leather and qualified mail body gear | Adaptive hides, defensive scales, tide/frost protection | [Leatherworking](profession-catalogues-20261009/leatherworking.md) |
| Tailoring | Cloth armor for caster/healer roles | Reactive woven wards, protective mantles, triage and spell-school schools | [Tailoring](profession-catalogues-20261009/tailoring.md) |
| Enchanting | Compatible ordinary role-stat finishes | Bound fire, anti-magic, tidal and prismatic signatures | [Enchanting](profession-catalogues-20261009/enchanting.md) |
| Engineering | Ranged/headgear parts, ammunition and finite supplies | Barriers, adaptive visors and controlled elemental ordnance | [Engineering](profession-catalogues-20261009/engineering.md) |
| Jewelcrafting | Compatible socket stats and secondary jewelry | Weapon lenses, elemental settings and paired bearer resonance | [Jewelcrafting](profession-catalogues-20261009/jewelcrafting.md) |

Each catalogue contains8field/4masterwork/6legendary concepts and JSON companions. They record form, slots, roles,
progression band, stat priorities, proposed mechanics, material themes, stages, maker specialties and dependencies.
An alternative form is a separate admitted blueprint variant, not simultaneous occupancy of every listed slot.
The combined JSON is designed for later catalogue work; every native-admission flag remains false.

## What makes the unusual work worth commissioning

Useful ordinary equipment is the foundation. Masterworks and legendaries add selected, theme-compatible specialties
within a tier/role budget. Examples include leather frost mantles, cloth protective oathwork, enchanter fire/ward
finishes, engineer storm repeaters and jewelcrafter paired martial/caster resonance. Exact costs, stats, proc rates,
cooldowns, aura ranges and native/client effects remain unratified. Designer vocabulary does not prove an effect.

Five legendary stages shape the accepted form, stat emphasis, signature and support finish. Real rare cores,
materials, workshop time and qualified native craft produce the exact item. First qualifying maker distinctions
and future teaching belong to their own durable records. Collaborators have paid child commissions and exact
contributions; credit is not multiplied by forwarding the same goods through several professions or mail stages.

Effects respect the exact equipped source and compatible native channels. Enchantments do not overwrite sockets
or an existing finish by accident. Gems do not invent sockets. Engineering gear/devices must pass their actual
profession-use restrictions; ordinary recipients can use only forms that are genuinely eligible. Identical aura
families do not multiply; distinct compatible effects coexist. Triggered damage cannot recursively feed unlimited
proc chains. Personal earned boss legacies remain independently uncapped.

## Turn the plan into retained equipment

See [FIELD_GEAR_PROVISIONING.md](profession-catalogues-20261009/FIELD_GEAR_PROVISIONING.md) for five role kits and
the eight-step proposed release plan. Immediate work is a fresh read-only recipient/slot demand census, qualified
native equivalents and available artisan/material/recipe routes. A city crafter must actually receive inputs,
craft/train successfully, deliver a distinct item, settle payment and have the recipient retain/equip it.

The October7 city-crafter report already records prepared BodyFirst and CityArtisans switches and broker-floor
source work, with historical bounded policy tests. That report said undeployed; this design turn performs no fresh
runtime attestation and cannot claim today's gear quality, roster, bot payments or source-release status.
The workshop policy remains an unlinked source proposal, not persistent queue/native crafting integration.

Before a playable release: qualify source/runtime custody and resources, repair/admit transaction semantics,
compile relevant updates, validate native recipes/effects, then make an archived meaningful release. Pilot one or
two ordinary field chains first and record distinct produced/delivered/equipped GUIDs. Compare matched recipients
and slots, role stats and actual movement/combat exposure; higher item level alone does not prove fewer deaths.
Only then admit the first custom legendary family/form/effect route and expand the catalogue.

This turn changed host design files and canonical/mirror logs only. No C++/SQL/source integration, build, native
tests, configuration, bot controls, items, skills, XP, Git, restart or push occurred. Galaxy resource priority remains.


## Early jewelcrafting: levels 1–60

[Early gems and random socket design](profession-catalogues-20261009/EARLY_GEMS_AND_SOCKET_ROLLS.md) adds 30 proposed cuts across five supply tiers with exact materials. Newly created
qualified crafted/earned gear can roll one additional empty prismatic socket. Rates start at 15% for
common gear and 20% for uncommon gear; role gems start at +1 primary stat/+2 stamina. No roll on
mail, purchase, equip or login. Existing sockets stay intact; higher-tier gems need wearer/host gates.
These are design proposals only: creation/recipe/client/bot adapters and durable once-roll persistence
are not implemented. This extends the earlier socket prohibition only through a future admitted native
creation route; arbitrary gemless items still cannot accept a gem.
