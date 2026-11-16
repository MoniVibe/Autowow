# Leatherworking profession catalogue

**Quantified materials:** [Leatherworking shopping lists](MATERIAL_BILLS.md#leatherworking). Costs are proposed; the table states output quantities and marks unimplemented relic cores.

Status: `DESIGN_ONLY_NOT_NATIVE_CATALOGUE` • `native_active: false` • 18 original proposals (8 field, 4 masterwork, 6 legendary)

This catalogue answers the city-crafter need for useful leather and compatible mail-role replacements while keeping weapons with their qualified professions. It is a design artifact, not a native recipe, item, spell, effect, source adapter, or deployment. The proposal follows the workshop direction: a customer funds a purpose and form, an artisan qualifies the recipe and materials, and sequential work produces one credited order.

## Catalogue index

| Tier | ID | Proposed name | Form and priority use | Maker school |
|---|---|---|---|---|
| Field | `leather.field-saltwind-trailharness` | Saltwind Trail Harness | Leather chest; scout, melee, expedition physical stats | Saltwind Harnesser |
| Field | `leather.field-copperleaf-scoutleggings` | Copperleaf Scout Leggings | Leather legs; ranged and scout physical stats | Copperleaf Cartographer |
| Field | `leather.field-raincut-menderwrap` | Raincut Mender Wrap | Leather bracers; healer and caster stats | Raincut Mender |
| Field | `leather.field-cinderstep-trailboots` | Cinderstep Trailboots | Leather feet; melee and scout physical stats | Cinderstep Cobbler |
| Field | `leather.field-wayfinder-softcap` | Wayfinder Softcap | Leather head; caster and healer stats | Wayfinder Milliner |
| Field | `leather.field-hearthhide-utilitybelt` | Hearthhide Utility Belt | Leather waist; repair and expedition utility | Hearthhide Saddler |
| Field | `leather.field-quietstride-handwraps` | Quietstride Handwraps | Leather gloves; rogue and ranged physical stats | Quietstride Glovewright |
| Field | `leather.field-expedition-quiltguards` | Expedition Quiltguards | Leather shoulders; tank, melee, and mail-role stats | Quiltguard Outfitter |
| Masterwork | `leather.masterwork-windrunner-cuirass` | Windrunner Cuirass | Leather chest; mobile combat bridge | Windrunner Patternmaster |
| Masterwork | `leather.masterwork-bogglass-expeditionhood` | Bogglass Expedition Hood | Leather head; caster/healer expedition clarity | Bogglass Visorwright |
| Masterwork | `leather.masterwork-ironbark-warders` | Ironbark Warders | Leather legs/feet; defensive route support | Ironbark Warder |
| Masterwork | `leather.masterwork-tideweave-hauberk` | Tideweave Hauberk | Mail-compatible route-qualified chest/shoulders/bracers | Tideweave Armorer |
| Legendary | `leather.legendary-onyx-scale-warder` | Onyx Scale Warder | Leather scale chest/shoulders; defensive Onyxia theme | Scale-Warder |
| Legendary | `leather.legendary-vashj-tidebinder-harness` | Tidebinder's Harness | Mail-compatible chest/waist; healer support | Tidebinder |
| Legendary | `leather.legendary-sapphiron-rimeguard-mantle` | Sapphiron Rimeguard Mantle | Leather shoulders/feet; frost expedition protection | Rimeguard |
| Legendary | `leather.legendary-yogg-waking-hide` | Waking Hide of the Deep Dream | Leather head; control resistance and clarity | Dreamwake Leatherworker |
| Legendary | `leather.legendary-illidan-feltrail-striders` | Feltrail Striders | Leather legs/feet/gloves; pressure and mobility combat | Feltrail Pursuer |
| Legendary | `leather.legendary-twinloom-resonant-kit` | Twinloom Resonant Kit | Paired leather or route-qualified mail bracers/waists | Twinloom Resonator |

Every row has a matching JSON candidate with the same ID and proposed name. Stat language is a priority within a role and tier budget; it does not grant every listed attribute at maximum.

## Field and routine provisioning

Field gear is deliberately practical and repeatable. It is designed as ordinary combat equipment for adventurers wearing weak or white gear: concrete role-appropriate stat priorities with no required custom proc, recovery, fatigue, camp, supply, or fumble subsystem. None needs a cross-profession collaborator to make its base form. A support finish, buckle, or mail variant can be a separate child commission only after a real qualified route is admitted.

| ID | Slots and roles | Ordinary stat blueprint | Materials / school |
|---|---|---|---|
| `leather.field-saltwind-trailharness` | Chest; scout, melee, expedition | Agility, stamina, and an admitted physical secondary such as attack power or critical strike. | Salt-cured hide, waxed cord; Saltwind Harnesser |
| `leather.field-copperleaf-scoutleggings` | Legs; ranged, scout, gatherer | Agility, stamina, attack power, and critical strike within the physical-role budget. | Copperleaf-tanned hide, dye, thread; Copperleaf Cartographer |
| `leather.field-raincut-menderwrap` | Bracers; healer, support, ranged | Intellect, stamina, spell power or healing priority, and haste. | Oiled lizard hide, linen, resin; Raincut Mender |
| `leather.field-cinderstep-trailboots` | Feet; melee, scout, gatherer | Agility, stamina, attack power, and critical strike within the physical-role budget. | Heat-cured hide, ash-rubber sole; Cinderstep Cobbler |
| `leather.field-wayfinder-softcap` | Head; caster, healer, scout | Intellect, stamina, spell power or healing priority, and haste. | Night-stalker hide, felt, wax; Wayfinder Milliner |
| `leather.field-hearthhide-utilitybelt` | Waist; gatherer, support, tank | Stamina, armor, and defensive rating for a compatible defensive form; strength or agility only if admitted. | Thick hide, brass loops; Hearthhide Saddler |
| `leather.field-quietstride-handwraps` | Gloves; rogue, ranged, gatherer | Agility, stamina, attack power, and critical strike within the physical-role budget. | Supple cat hide, felt pads; Quietstride Glovewright |
| `leather.field-expedition-quiltguards` | Shoulders; tank, melee, mail-role scout | Stamina and compatible defense, plus strength/agility and one physical secondary only for the accepted role. | Marsh hide, wool batting, reed; Quiltguard Outfitter |

Field mechanics are ordinary stat allocations only. They do not require or assert a native proc, spell, recovery system, travel-fatigue system, camp system, supply charge, stealth/fumble rule, or client-visible special effect. The future native-equivalent mapping for each form, recipe, and property bundle remains unqualified.

## Masterwork commissions

Masterworks take longer, use scarcer materials, and ask for a tighter stat/property blueprint. They remain affordable bridges into endgame work rather than legendary substitutes.

| ID | Slots and roles | Stat direction | Signature direction | Materials / school |
|---|---|---|---|---|
| `leather.masterwork-windrunner-cuirass` | Chest; ranged, melee, scout | Agility, stamina, critical/haste choice | Reposition after useful attacks primes one modest mobility or recovery benefit. | Windrunner hide, sinew; Windrunner Patternmaster |
| `leather.masterwork-bogglass-expeditionhood` | Head; healer, caster, scout | Intellect, stamina, healing or spell power | A useful heal or utility spell after a hard route opens a short clarity window. | Bogstrider hide, resin, moss; Bogglass Visorwright |
| `leather.masterwork-ironbark-warders` | Legs, feet; tank, melee, mail-role scout | Stamina and armor-compatible defense | A defensive event or load-bearing route prepares one bounded brace. | Thick hide, bark fiber, wax; Ironbark Warder |
| `leather.masterwork-tideweave-hauberk` | Chest, shoulders, bracers; healer, ranged, mail-role scout | Role primary, stamina, healing/spell-power | Useful healing or rescue work primes a small tide-like recovery response. | Tide-cured hide, scales, kelp cord; Tideweave Armorer |

Mail-role wording means “compatible only if a native mail armor/form route is later qualified.” It is not a claim that current leatherworking templates support mail or these properties.

## Legendary five-stage commissions

Each legendary candidate has exactly five stages in the JSON. They are meaningful production stages, not generic waits:

1. Preparation qualifies the earned core or pattern, purpose, role, form, materials, and funded blueprint.
2. Forging or weaving constructs the accepted armor chassis.
3. Tempering applies the selected primary stat emphasis within the tier and role budget.
4. Signature finishing binds one boss-themed response with explicit trigger and lifecycle rules.
5. Inspection and attunement validates native compatibility, maker lineage, support finish, exact output, and delivery.

| ID | Form, slots, and roles | Boss theme and proposed signature | Maker specialty |
|---|---|---|---|
| `leather.legendary-onyx-scale-warder` | Leather scale chest/shoulders; tank, melee, mail-role scout | Onyxia: choose one bounded scale retaliation or fear-composure response after eligible defense. | Scale-Warder |
| `leather.legendary-vashj-tidebinder-harness` | Mail-compatible chest/waist; healer, ranged, mail-role scout | Vashj: a useful heal or rescue action prepares a bounded tidal pulse for an injured ally. | Tidebinder |
| `leather.legendary-sapphiron-rimeguard-mantle` | Leather shoulders/feet; tank, healer, scout | Sapphiron: one frost barrier or retaliation response when hostile frost breaks the ward. | Rimeguard |
| `leather.legendary-yogg-waking-hide` | Leather head; caster, healer, scout | Yogg-Saron: resisting hostile mental control prepares one brief clarity response. | Dreamwake Leatherworker |
| `leather.legendary-illidan-feltrail-striders` | Leather legs/feet/gloves; melee, rogue, scout | Illidan: sustained eligible pressure prepares one evasive or fel-strike flourish. | Feltrail Pursuer |
| `leather.legendary-twinloom-resonant-kit` | Paired leather or admitted mail bracers/waists; melee, caster, healer | Twin Emperors: two actual group members' eligible martial and magical contributions create bounded resonance. | Twinloom Resonator |

The Twinloom design is a collaborative kit, but its collaboration is explicit and paid: each bearer receives an independently identified item, each piece consumes its own materials and qualification, and any mail collaborator has a real child commission. No legendary weapon is hidden in the kit; weapons remain another profession's work.

## Sourcing, constraints, and release limits

Real ingredient sourcing means credited gatherer output, guild stock, or a purchased supply is reserved against the commission. Hides, cord, thread, resin, scales, cores, and other inputs must reconcile through the craft ledger. A planned order, mail receipt, duplicate stack, or aggregate craft count does not prove a completed item.

Artisan skill, learned native recipe, character level, station, armor family/form, customer role, and material availability remain gates. Maker schools and boss-family mastery are custom design records; they cannot teach a missing native recipe, bypass customer eligibility, or rewrite an old item. Boss theme shapes item form, stat emphasis, and signature vocabulary. Personal earned boss legacies remain a separate uncapped record and do not expand a per-item tier or role budget.

No native IDs are admitted here. Exact item and spell templates, property bundles, damage/healing values, proc rates, cooldowns, durations, ranges, material quantities, client tooltips, save/load behavior, equip/mail persistence, and effect removal all remain unresolved. Any number or timing implied by these proposals is PROPOSED/unratified. The first implementation gate would be one admitted form and one observed lifecycle from real inputs through craft, delivery, equip, save/load, and clean removal; only then should the catalogue expand.

The field entries are future blueprints for ordinary native-equivalent gear. Their immediate native item, recipe, armor-form, and property mapping remains unqualified, so they should not be read as a claim that a current template can already produce these stat bundles.

## JSON companion

The machine-readable companion is [leatherworking.json](leatherworking.json). It records the full stages, dependencies, role/stat priorities, material themes, and provisioning notes for all 18 entries under the required design-only schema.
