# Legendary workshop designs: stages that shape the item

Selected refinement: artisans choose compatible equipment forms, stat priorities and signature effects for each
legendary commission. The five stages are meaningful production steps, not generic waits. This document expands
the approved workshop/legendary design; no new item, effect, native recipe or source adapter is installed here.

## A commissioned design, not a random reward

The buyer supplies a role and purpose: a tank needs a protective weapon, a melee fighter a damage weapon, a caster
a spellcasting focus, or an expedition a resistance-bearing piece. The artisan proposes a blueprint using their
profession, learned techniques and mastery. The representative accepts the quoted design/materials/work/payment.
Bot artisans choose from actual demand and compatible specialty; a human commission can request the same choices.

Each blueprint records legendary family, equipment form, role, stat allocation, signature effect and optional
compatible support finish. For example, the Ragnaros family can have one-handed, two-handed or protective forms.
Weapon form determines its damage/speed/stat envelope; it is not a cosmetic flag on the same finished weapon.
Armor form similarly determines slot, armor family and available finishes. A tailor cannot select a plate chest
without a real qualified collaborator and its own material-consuming contribution.

Critical strike, haste, stamina, defensive ratings, spell power and other compatible priorities are allocations
within that design's tier/role budget. Choosing more haste can mean less critical strike, not receiving every stat
at maximum. Support effects and damage effects also have their own tuned costs. Existing uncapped personal earned
boss legacies remain unchanged; these are the previously selected per-item crafting budgets.

Use a finite set of validated blueprint components and compatible combinations rather than arbitrary stat/spell
numbers. The first native pilot can expose a few useful forms and finishes, expanding once save/load/equip/client
behavior is observed. Exact templates/property bundles/spells, damage, duration, range, proc rate, cooldown and
material quantities remain to qualify. The planner must refuse a combination without an admitted native route.

## How the five stages affect the result

| Stage | Design contribution | Required work |
|---|---|---|
| Preparation | Qualify boss core/pattern, purpose and complete blueprint | Reserve real rare/common inputs and establish component/maker lineage |
| Forging or weaving | Construct selected one-/two-handed weapon or armor form | Qualified profession, city station and recipe-specific work |
| Tempering | Apply selected primary stat emphasis | Specialty technique, its materials and captured craft skill |
| Signature finishing | Bind the selected boss-themed active/passive/proc behavior | Earned compatible knowledge and theme-specific finishing materials |
| Inspection and attunement | Validate compatibility and support finish; seal maker identity | Confirm exact finished output and quoted properties before delivery |

Choices are fixed in a versioned accepted blueprint before work starts, so a player or bot cannot change a nearly
finished sword into a staff without paying/reconciling the different inputs and work. A missing technique or
material holds the relevant stage. Any renegotiation is an explicit revised quote, preserving already consumed
inputs and completed subcomponents; it cannot reset the ledger to recover spent materials.

Early stages produce work records or genuinely crafted subcomponents, not usable legendary equipment. All stages
must qualify before the final exact item is materialized through the admitted native route. Teaching can eventually
let apprentices construct appropriate subcomponents while a master performs signature finishing. Each collaborator
has a real child commission, qualified work/materials and recorded contribution; sharing a project is not free skill.

## Ragnaros: one family, distinct useful creations

| Proposed blueprint | Form and emphasis | Signature | Support finish |
|---|---|---|---|
| Emberheart, Furnace pattern | Two-handed melee; strength and critical strike | Eligible hostile attacks can release a local fire eruption | Personal fire resilience |
| Emberheart, Cinder pattern | One-handed melee; agility/strength as role allows, haste | Eligible attacks can launch a fireball at the struck enemy | A modest compatible fire-resistance aura |
| Emberheart, Hearthguard pattern | One-handed tank weapon or separately designed shield; stamina and defense | Successful defensive events can trigger a fire retaliation | Local fire-resistance protection for nearby allies |
| Emberheart, Living Flame pattern | Caster weapon; spell power and a chosen casting priority | Useful hostile spells can trigger an additional fire projectile | A compatible fire ward rather than a melee-only effect |

These are proposed role variants, not selected effect magnitudes or an assertion that every native recipe supports
every form. The fire theme persists across the family. The artisan chooses a compatible pattern and finish;
each item need not carry eruption, fireball, maximum damage and every aura simultaneously. Mastery makes additional
finishes eligible and can improve a qualified finish tier; it does not let a cheap chassis acquire unlimited stats.

## Other legendary families

All powers below are custom proposed craft directions. They do not describe installed native loot or boss rewards.

| Inspiration | Forms worth offering | Distinct signature choices | Support direction |
|---|---|---|---|
| Onyxia | Armor, shield, physical weapon | Precise retaliatory strike or resilience after hostile fear | Fear composure and fire protection |
| Nefarian | Caster focus, weapon, head armor | Adaptation to the last hostile magic school | Limited protection against that school |
| Chromaggus | Shoulders, caster focus, weapon | Chosen prismatic school reaction | Multi-school ward with explicit compatible rules |
| Twin Emperors | Complementary martial/caster weapons and armor | Physical or magical pattern that resonates with its paired counterpart | Nearby complementary bearer cooperation |
| C'Thun | Caster focus or precision weapon | Hostile-target eye beam or interrupt-responsive focus | A brief anti-magic shelter |
| Vashj | Mail armor, ranged/caster weapon | Useful attack/heal triggers a lightning or tidal response | A protective tide for nearby allies |
| Kael'thas | Spellblade, staff, cloth armor | Phoenix-themed fire surge from useful casting | Temporary protective embers |
| Illidan | One-handed/paired melee weapons, agile armor | Sustained hostile pressure builds an evasive or fel-strike flourish | Mobility/defensive response, not a universal armor aura |
| Razuvious | Plate legs, shield, martial weapon | Parry or disciplined defensive stance primes a counterattack | Nearby allies benefit from the bearer's defensive discipline |
| Sapphiron | Shoulders, armor, caster focus | Frost retaliation or a protective frozen barrier | Local frost resistance |
| Kel'Thuzad | Caster/healer weapon and cloth armor | Useful casting shapes a frost/shadow response | A short compatible anti-magic ward |
| Yogg-Saron | Head armor or caster focus | Resistance to hostile mental control grants clarity | Nearby allies gain a brief composure effect |
| Lich King | Role-specific weapons and protective armor | Protecting an endangered ally empowers the next compatible action | A defensive unfallen-oath aura |

Boss-family identity determines the signature vocabulary; weapon/armor form and artisan school determine which
members of that vocabulary fit. A defensive school can produce a Sapphiron frost ward, while a weapon school
produces frost retaliation. A different maker leaves a recognizable finishing style without turning every family
into generic stronger armor. Existing legendary catalogue names are family inspirations, not immutable one-slot
constraints; future variant records retain both family and exact accepted blueprint.

## Effect lifecycle and item identity

Each finished item retains exact item GUID, boss family/core lineage, blueprint/version, maker/collaborators,
captured skill/specialty, stat allocation and effect profile. Moving it through mail/bank/trade preserves its
properties. Later maker skill gains do not change old gear; a reforge is a separate priced/material-consuming order.
Do not modify shared item-template data to personalize an existing single item. New form/template variants and
per-instance compatible property/effect adapters require separate native/client admission.

Auras follow the actual equipped bearer and eligible group/range. Unequip, logout, removal, group/range change
and death apply the effect's explicit lifecycle; no permanent aura is left behind by borrowing a weapon. Identical
aura families do not multiply through several bearers; distinct compatible families may coexist. Item bonuses and
personal earned boss traits are separate effects with separate identities and settlement.

Proc triggers must specify attack/spell/defensive/heal eligibility, hostile/ally targets, cooldown and chance.
An item's proc cannot recursively trigger itself or another unlimited proc chain. Friendly/self-arranged events,
ambient damage and unrelated periodic effects cannot charge a signature by accident. Area damage respects native
targeting and hostility. A fire-resistance aura and a triggered fireball are independently observed effects, not
proved by an item's name, catalogue row, equip receipt or higher item level.

## Source and release boundary

Current WorkshopPolicy only schedules credited work and receipt-gated piece completion. It has no stage-specific
blueprint/material/effect adapter. MasterworkPolicy only checks a candidate native-property bundle. This refinement
does not retrofit either header into a functioning item generator. Persistent order/craft transaction admission,
recipe/cast provenance, catalogue/native/client compatibility and observed item/effect lifecycle remain prerequisites.

Next source slice: immutable blueprint eligibility and role/form/stat/theme compatibility, then child-stage input
and settlement contracts. First eventual live proof: one Ragnaros-family form, one stat emphasis, one signature
and one support finish, real inputs/work, exact craft, save/load/mail/equip, observed effect and clean removal.
Then add the other forms/families; no build, native effect, recipe, SQL, configuration or runtime change in this turn.
