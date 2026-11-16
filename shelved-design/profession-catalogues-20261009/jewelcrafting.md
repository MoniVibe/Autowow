# Jewelcrafting catalogue: design-only proposal

**Quantified materials:** [Jewelcrafting shopping lists](MATERIAL_BILLS.md#jewelcrafting). Costs are proposed; the table states output quantities and marks unimplemented relic cores.

Status: DESIGN_ONLY_NOT_NATIVE_CATALOGUE

This catalogue turns the approved workshop design into 18 Jewelcrafting concepts: 8 field, 4 masterwork, and 6 legendary. It is a design catalogue, not an installed recipe, item, socket, spell, effect, client route, or native claim. native_active and every candidate's native_ids_admitted are false.

Jewelcrafting gives identity and useful customization to main weapons and armor first. Gems, lenses, and settings are host-bound to an actually admitted native socket or item route. A gemless white item is a replacement-gear candidate; it does not gain a fictional socket. Neck, ring, and paired-resonance work is intentionally bounded so the profession is not mostly trinkets.

## Candidate index

| Tier | Exact design ID | Proposed name | Practical form and priority |
|---|---|---|---|
| field | jewel.field-dawnprism-caster-gem | Dawnprism Caster Gem | Standard gem for an admitted weapon/armor socket; intellect or caster primary, then one haste/critical choice |
| field | jewel.field-hearthstone-tank-gem | Hearthstone Tank Gem | Standard gem for weapon, shield, or armor socket; stamina, defense/avoidance/block |
| field | jewel.field-warcut-melee-gem | Warcut Melee Gem | Standard gem for weapon/armor socket; strength/agility, then one critical/haste choice |
| field | jewel.field-clearview-weapon-lens | Clearview Weapon Lens | Replaceable lens for caster, healer, ranged, or spellblade weapon |
| field | jewel.field-hearthward-armor-setting | Hearthward Armor Setting | Passive chest, legs, shoulders, or shield setting in an existing socket |
| field | jewel.field-edgecut-weapon-setting | Edgecut Weapon Setting | Passive one-handed, two-handed, or ranged weapon setting in an existing socket |
| field | jewel.field-riverglass-neckring-set | Riverglass Neck and Ring Set | Ordinary cut-stone neck plus matching role-stat ring |
| field | jewel.field-twinseed-resonance-charms | Twinseed Resonance Charms | Two ordinary matching role-stat rings from one collection flavor |
| masterwork | jewel.masterwork-astral-casting-lens | Astral Casting Lens | Scarce replaceable lens for an admitted caster/healing main weapon |
| masterwork | jewel.masterwork-bastion-weapon-setting | Bastion Weapon Setting | Defensive tank weapon or shield setting; one bounded counterguard proposal |
| masterwork | jewel.masterwork-warstar-weapon-gem | Warstar Weapon Gem | Scarce melee/ranged main-weapon gem with explicit stat tradeoff |
| masterwork | jewel.masterwork-oathband-neckring-pair | Oathband Neck and Ring Pair | Masterwork neck plus separate matching ring identity |
| legendary | jewel.legendary-onyxia-scalegem | Onyxia Scale Gem | Earned-core gem for an admitted main weapon, armor, or shield socket |
| legendary | jewel.legendary-nefarian-adaptive-lens | Nefarian Adaptive Lens | Earned-core caster/healing weapon lens with one school response |
| legendary | jewel.legendary-ragnaros-hearthsetting | Ragnaros Hearthsetting | Earned-core tank weapon, shield, or two-handed weapon setting |
| legendary | jewel.legendary-chromaggus-prismneck | Chromaggus Prismneck | Earned-core prism necklace with separately identified ring setting |
| legendary | jewel.legendary-twinemperors-paired-resonance | Twin Emperors Paired Resonance | Two earned-core pieces for complementary weapon/armor sockets |
| legendary | jewel.legendary-cthun-eyecut | C'Thun Eye Cut | Earned-core precision gem for an admitted melee/ranged/caster weapon |

## Field goods

Field goods use repeatable ore, stone, lens, setting, and ordinary jewelry inputs. They are funded city commissions and are not raid-gated. The first three are the standard caster, tank, and melee gem priorities. Every field candidate has a plain concrete role-stat baseline; no field candidate depends on a bespoke proc, pair effect, range effect, or identity marker.

The bot should select a field gem or lens only after reading the actual host item, native form, and socket/channel metadata. If a weapon or armor item is white and gemless, the bot should source an admitted replacement item. It should never infer that a setting punches a socket or that a cut gem/lens can attach to an arbitrary item.

## Masterwork goods

Masterworks consume scarcer real stone, lens, and setting materials and require a qualified profession recipe, skill, level, station, role, binding, and client/item route. Their four candidates prioritize the useful weapon path before a neck/ring identity pair. Any proposed focus or counterguard response is one item budget with an explicit trigger and cooldown still awaiting native admission.

## Legendary commissions

Every legendary candidate uses an earned boss core or pattern plus real stone, metal, ore, and a funded commission. Each has exactly five meaningful stages from the workshop source:

1. Preparation: qualify the core/pattern, purpose, form, host/socket route, and complete funded blueprint.
2. Forging or weaving: construct or seat the chosen Jewelcrafting form through an admitted native route.
3. Tempering: apply one role-appropriate stat emphasis within the per-item tier/role budget.
4. Signature finishing: bind one boss-family response with explicit eligible trigger, target, cooldown, and lifecycle.
5. Inspection and attunement: verify exact host/item identity, maker and collaborators, compatibility, effect behavior, and delivery.

The six families are Onyxia, Nefarian, Ragnaros, Chromaggus, Twin Emperors, and C'Thun. Family vocabulary shapes the finish, but does not grant unlimited stats, a generic aura, or an automatic native recipe. Personal boss legacies remain separate and uncapped; a legendary gem or setting still has a finite per-item budget.

## Practical bot gear admission

The bot should evaluate a commission in this order:

1. Identify the role and immediate need. Prefer an admitted main weapon or armor host for caster, tank, or melee work. Treat neck/ring and pair work as secondary identity customization.
2. Read the actual item GUID, form, slot, equip state, native socket metadata, and existing applied stat/effect records. Admit a candidate only when its item_form and slots match an observed route.
3. Reject a gem, lens, or setting for gemless white gear. Recommend a native replacement item with a real socket or an admitted non-socket Jewelcrafting form. Never fabricate sockets or overwrite an existing gem.
4. Check the funded materials ledger. Ordinary field work needs real ordinary inputs and a customer order. Masterwork and legendary work needs its scarce material lot; legendary work additionally needs the earned core/pattern. A planned order, duplicate stack, or mailbox item is not proof of completed craft.
5. Check learned recipe, Jewelcrafting skill, level, station, armor/weapon form, role, binding, and any qualified collaborator. A maker specialty or personal boss legacy cannot teach a missing native route.
6. Apply the candidate's stat priorities as one finite tier/role bundle. Reconcile against already-applied host stats so no stat is silently duplicated. Separate item bonuses from personal boss traits.
7. For the selected component or finish, require at most one admitted effect profile. This component-level rule does not cap compatible enchants, sockets, legendary effects, or earned personal boss legacies on the finished item. When a chosen profile eventually has a native route, record eligible attack, spell, defensive, or heal trigger, hostile/ally target, cooldown, chance, range, and removal behavior. Friendly spam, ambient damage, unrelated periodic effects, and the item's own proc cannot charge it by accident.
8. Persist host item GUID, gem/lens/setting identity, socket identity, accepted blueprint version, maker, collaborators, and contribution records. Equip, unequip, swap, replacement, bank, mail, death, group, and range changes must follow the admitted lifecycle. Removing the host or component removes its effect; it must not leave a permanent aura.
9. Refuse activation while native_ids_admitted is false. Save/load, equip, mail, replacement, and clean effect removal require an observed native/client pilot before any candidate is promoted.

The three best admission gates are therefore:

1. Real host gate: observed compatible native item/socket metadata, with white gear routed to replacement rather than an invented socket.
2. Real craft gate: funded real inputs plus learned recipe, skill, level, station, role, and binding; boss cores are earned craft inputs, not free progress.
3. Real lifecycle gate: exact item/socket/component identity and one bounded effect record with no duplicated stats, aura multiplication, or proc-on-proc loop.

Numbers, rates, native IDs, property bundles, spell IDs, durations, ranges, cooldowns, proc chances, and material quantities remain unqualified proposals. The JSON companion records the same boundary and preserves the exact candidate IDs and names.


## Early jewelcrafting: levels 1–60

[Early gems and random socket design](EARLY_GEMS_AND_SOCKET_ROLLS.md) adds 30 proposed cuts across five supply tiers with exact materials. Newly created
qualified crafted/earned gear can roll one additional empty prismatic socket. Rates start at 15% for
common gear and 20% for uncommon gear; role gems start at +1 primary stat/+2 stamina. No roll on
mail, purchase, equip or login. Existing sockets stay intact; higher-tier gems need wearer/host gates.
These are design proposals only: creation/recipe/client/bot adapters and durable once-roll persistence
are not implemented. This extends the earlier socket prohibition only through a future admitted native
creation route; arbitrary gemless items still cannot accept a gem.
