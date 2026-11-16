# AutoWoW tailoring profession catalogue — design candidates

**Quantified materials:** [Tailoring shopping lists](MATERIAL_BILLS.md#tailoring). Costs are proposed; the table states output quantities and marks unimplemented relic cores.

Status: DESIGN_ONLY_NOT_NATIVE_CATALOGUE; 18 original tailoring concepts, with native_active false and native_ids_admitted false on every candidate. This is a design handoff for review, not a native catalogue, recipe table, item-template claim, or craftability declaration.

The catalogue follows the city-crafter model: a tailor works from a trained recipe at a qualified city station, satisfies profession and character level gates, consumes real ingredients, and accepts a funded commission. A successful routine craft can support useful gearing and sequential bulk orders, but an order does not teach a recipe, grant skill/XP, create materials, or make an unqualified item craftable. Mastery is a separate earned qualification that can make a finish eligible; it does not erase gates or let tailoring produce plate. Every field piece is independently usable plain cloth armor with a role-appropriate stat allocation; no bespoke proc, action, or fatigue adapter is required for its baseline value.

## Count and coverage

- **8 field**: one cloth piece in each core slot: chest, legs, head, shoulders, bracers, gloves, waist, and boots.
- **4 masterwork**: advanced chest, legs, head, and shoulders for caster DPS, healer, and protective-support demand.
- **6 legendary**: earned boss-core commissions for cloth chest, legs, shoulders, head, cloak, and bracers.
- **No plate** and no unrestricted all-stat design. Caster DPS leans toward spell power, haste, and critical strike; healer designs lean toward healing power, resource regeneration, and haste.
- Cloaks are a collaborative auxiliary route. Bags may be a later utility line, but they do not displace the critical armor coverage above.

Numerical budgets, effect magnitudes, proc rates, durations, cooldowns, ranges, material quantities, and timings are unratified proposals. Per-item role/tier budgets are tuning envelopes and do not cap separately earned boss legacies.

## Candidate index

### Field: routine city production

| Exact ID | Proposed name | Slot and role | Useful bot role / availability note |
|---|---|---|---|
| tailor.field.cinderweave-vest | Cinderweave Adventurer's Vest | Cloth chest; caster DPS | A practical first chest commission for a city caster. Available when the tailor has the trained recipe, level/skill gate, station, real cloth, and funded order; no raid clear or second artisan. |
| tailor.field.mercyweave-leggings | Mercyweave Field Leggings | Cloth legs; healer/support caster | Keeps healer leg provisioning ahead of luxury work. Same ordinary city and material gates; no boss core. |
| tailor.field.prismstitch-hood | Prismstitch Spellhood | Cloth head; caster DPS/spell utility | A selected school flavor gives a bot a clear demand key without requiring a school effect. Routine city availability after training and paid inputs. |
| tailor.field.havenweave-mantle | Havenweave Shoulder Mantle | Cloth shoulders; healer/protective support | Plain healer/support shoulder coverage with defensive and throughput priorities. A city healer can request it from ordinary materials once recipe and level gate are qualified. |
| tailor.field.quickbind-bracers | Quickbind Spell Bracers | Cloth bracers; caster DPS/healer/utility caster | Flexible filler for a missing wrist slot and sequential bulk work. Role and stat emphasis are chosen at quote time; no raid dependency. |
| tailor.field.stitchguard-gloves | Stitchguard Healing Gloves | Cloth gloves; healer/protective support | Healer glove coverage with healing and resource priorities for bots that actively heal injured allies. Routine availability follows learned recipe, station, material, and funding checks. |
| tailor.field.reserveweave-sash | Reserveweave Utility Sash | Cloth waist; caster DPS/healer/expedition utility | Gives the broker a useful waist demand key and plain readiness stats without a charge system or free goods. City production; real webbing and ordinary tailoring supplies required. |
| tailor.field.wayfarer-ward-boots | Wayfarer's Ward Boots | Cloth boots; caster DPS/healer/expedition survival | Covers movement and survival stat demand for properly geared adventurers. Available through ordinary trained tailoring and funded inputs, with no boss route. |

Field designs are the baseline bot supply line. Their primary mechanic is ordinary usable cloth armor with a fixed, role-appropriate stat envelope; specialty colors, school identity, and any future finish remain optional additions. They should be eligible for routine city production and sequential bulk commissions after native recipe/skill/level/station/material checks pass. Their design intent does not claim any current native recipe, item ID, effect, or live availability.

### Masterwork: advanced trained craft

| Exact ID | Proposed name | Slot and role | Useful bot role / availability note |
|---|---|---|---|
| tailor.masterwork.astral-robe | Astral Threadbattle Robe | Cloth chest; caster DPS | A higher-quality caster chest for a bot moving from field gear toward raid preparation. Requires a trained masterwork recipe, advanced skill/level gate, mastery eligibility, real refined cloth, and a funded quote. |
| tailor.masterwork.mercy-legwraps | Mercyweave Legwraps of Reserve | Cloth legs; healer | Advanced healer legs with resource-preserving triage. Available only after its own training and gates; it is not unlocked by merely owning a field pattern. |
| tailor.masterwork.chromatic-hood | Chromatic Focus Hood | Cloth head; caster DPS/spell utility | A controlled selected-school response gives school-conscious bots a meaningful choice. Mastery can qualify the finish but cannot turn it into unrestricted all-school armor. |
| tailor.masterwork.haven-mantle | Havenweave Mantle of Intercession | Cloth shoulders; healer/protective support | Higher-tier reactive protection for group healers. Requires advanced recipe, specialty, materials, station, and funded commission; no raid clear or plate collaborator. |

Masterwork availability is gated by trained recipes/skills/level and separate mastery. It does not grant free profession progress, free materials, free goods, or a boss legacy.

### Legendary: earned, five-stage commissions

Every legendary row below uses exactly five sequential, meaningful stages: preparation, weaving, tempering, signature finishing, and inspection/attunement. Early stages are work records or genuine subcomponents, not usable final gear. A compatible earned boss core and an admitted native route are prerequisites; catalogue presence does not prove loot, effect, or craft availability.

| Exact ID | Proposed name | Slot and role | Signature family | Useful bot role / availability note |
|---|---|---|---|---|
| tailor.legendary.emberheart-chest | Emberheart Cinderweave Chest | Cloth chest; caster DPS/fire expedition | Ragnaros / Emberheart | A late progression fire caster commission with a separate fire response and personal ward. Requires earned core, locked blueprint, legendary recipe, mastery, city station, real inputs, and funded payment. |
| tailor.legendary.tidebinder-legs | Tidebinder's Mercy Leggings | Cloth legs; healer/protective support | Lady Vashj / Tidebinder | A healer commission whose protective tide follows eligible healing. Bot availability begins only after earned core and all native route, training, material, and settlement gates qualify. |
| tailor.legendary.prismatic-shoulders | Prismatic Loom Mantle | Cloth shoulders; caster DPS/healer/spell utility | Chromaggus / Prismatic | School-aware reactive ward with an explicit selected school set. A bot must have a compatible role and qualified school blueprint; it is not blanket resistance. |
| tailor.legendary.waking-mind-hood | Hood of the Waking Mind | Cloth head; caster DPS/healer/mental resilience | Yogg-Saron / Waking Mind | A control-resistance/composure design for dangerous content. Requires the earned family core and fully qualified exact blueprint; no immunity or automatic dispel is implied. |
| tailor.legendary.unfallen-cloak | Cloak of the Unfallen Oath | Cloth cloak; healer/protective support/expedition leader | The Lich King / Unfallen Oath | Auxiliary cloak collaboration: the tailor makes the cloth base and a qualified clasp collaborator owns a real child commission. It must not displace core chest/leg/head/shoulder/wrist/hand/waist/boot provisioning. |
| tailor.legendary.sepulchral-bracers | Sepulchral Conductor Bracers | Cloth bracers; caster DPS/healer/anti-magic support | Kel'Thuzad / Sepulchral Conductor | A bounded frost/shadow ward for useful casting. Requires earned core, legendary training/mastery, role-compatible blueprint, real conductor materials, and funded settlement; no unrestricted stats. |

The legendary rows are intended for funded commissions tied to actual encounter-earned cores. They do not retrofit the current workshop policy into an item generator, admit client or custom-item routes, or assert a live effect lifecycle.

## Maker schools and role boundaries

- **Cinderweaving / Emberweaving** supplies fire-themed caster damage and compatible fire protection.
- **Mercyweaving / Tideweaving** supplies healer throughput, resource preservation, and ally-directed restorative wards.
- **Havenweaving / Stitchguard** supplies protective and reactive woven wards with explicit ally/hostile eligibility.
- **Prismstitch / Chromatic Loom** supplies a selected school or qualified school set; it never means every school at once.
- **Quickbind / Wayweaving / Reserveweaving** supplies movement, readiness, and expedition utility without manufacturing free goods.
- **Dreamweaving** supplies mental resilience and composure responses without immunity.
- **Sepulchral weaving** supplies bounded frost/shadow caster and healer protection.
- **Oathweaving** can coordinate a cloak base with a qualified collaborator; it cannot make plate or bypass a profession gate.

These specialties preserve recognizable maker identity while keeping form, role, school, and stat allocation fixed in the accepted blueprint. A tailor cannot select a plate chest, switch a nearly finished robe into another slot without a revised quote, or max every compatible stat and effect at once.

## Three favorites

1. **tailor.field.mercyweave-leggings — Mercyweave Field Leggings.** Healer legs are a high-value routine gap, and the design gives active triage a modest protective identity without raid dependency or a second artisan.
2. **tailor.masterwork.haven-mantle — Havenweave Mantle of Intercession.** It makes mastery meaningful through a protective/reactive shoulder finish while retaining a clean city recipe, material, and funded-commission path.
3. **tailor.legendary.emberheart-chest — Emberheart Cinderweave Chest.** It translates the workshop note's fire family into a cloth caster chest with separate fire response and ward choices, a clear Emberweaver school, and five auditable work stages.

## Gates before any native catalogue admission

1. **Design gate:** approve the exact ID, name, cloth form, slot, role, stat envelope, school/family, mechanic boundary, and five-stage ledger. All numerical proposals remain unratified.
2. **Recipe and skill gate:** verify a real trained tailoring recipe, profession skill, character level, specialty, and separate mastery qualification. Ordering alone cannot teach any of these.
3. **Supply and funding gate:** verify real cloth, thread, linings, reagents, and any earned boss core; reserve and consume them through a funded commission with provenance.
4. **Station and collaboration gate:** verify city station eligibility. For a cloak or any later collaborator, record a real child commission, qualified work, inputs, and settlement.
5. **Native compatibility gate:** qualify item/property/spell/client routes and exact IDs before any claim of craftability. No plate or unrestricted all-stat route is admitted by this catalogue.
6. **Observation gate:** prove exact craft result, save/load, mail/trade, equip behavior, role and slot usability, effect trigger and lifecycle, and clean removal. A name, item level, receipt, or catalogue row is not proof.
7. **Bulk and bot-availability gate:** after one exact commission is proven, test sequential funded production and demand selection without starving the eight critical field armor slots for cloak, bag, or luxury work.

No live item IDs, effect IDs, recipe IDs, runtime changes, SQL changes, client custom-item route, free XP/skill/materials/goods, or already-craftable catalogue design is claimed here.
