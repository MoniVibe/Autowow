# Mercenary services and notable makers

The user wants guilds to provide well-equipped bodyguards and craftsmen to leave meaningful specialties on their
works. Military glory can grant command/requisition eligibility, while productive artisans become known for the
quality and usefulness of their output. These are new custom society features, not existing native rewards.

## Mercenary guilds and adventuring guild detachments

Use one contract model for either a dedicated mercenary guild or an adventuring/raiding guild offering available
members. The provider retains guild membership and its members' earned gear, professions and progression. A hire
is a temporary duty assignment, not a guild transfer, new character, free equipment package or master takeover.

Representatives advertise available role, level, equipment and deployment terms. A client requests a bodyguard,
escort or expedition detachment; its representative reserves payment in the guild bank. The provider reserves the
actual autonomous bot identities so one fighter cannot serve two contracts or be counted in two simultaneous raids.
Composition, native group capacity, travel, supplies and actual guard availability must qualify before acceptance.

Leader glory can unlock a small retinue allowance, initially proposed as one/two/three slots at deliberately chosen
tiers. Glory is eligibility, not wages. Guild funds pay contracted services, food and repairs from actual stock.
Normal guild escorts need a qualified commander and budget; this need not reserve all useful guards for celebrities.
The supplier can charge more for an experienced, well-equipped healer/tank or difficult deployment. Prices must be
quoted before commitment and linked to exact terms; no item-level number alone proves combat effectiveness.

Persist client/provider guild, commander, reserved fighters, task, fee/cap, start/end and settlement receipts. Recheck
human/master/pause/custody and current group/movement owners before any control change. Do not steal busy guild
members, replace a human master, teleport to an employer or duplicate an assignment after restart. Natural group
admission and travel precede work. Expiry, revocation, unavailable leader and death preserve native recovery, release
unused budget and make surviving fighters available for safe reassignment without deleting or respawning characters.

The gameplay loop is a provider investing in good fighters, a workshop supplying their equipment, and adventurers
paying for reliable protection. Contract receipts can build provider reputation separately from personal PvP glory.
Military guilds can gain income from escorting productive guilds even when no enemy is present. No forced combat,
arranged kills or paid duplicate rewards are needed. Contract persistence/control integration is not implemented yet.

## Craftsmen with a recognizable specialty

Keep artisans in their city workshops. Native recipe learning, ranks, reagents and productive skill gains determine
what they can make. Snapshot the actual profession skill and specialty at the successful craft; do not let a later
skill gain retrospectively improve all earlier works. Their identity and qualified output history become visible
in commission listings, so buyers can choose a maker rather than only the cheapest item entry.

Example specialty directions, with exact amounts and thresholds still to balance:

- Armor specialists: stamina, defense rating or selected defensive ratings appropriate to the armor and role.
- Physical weapon specialists: bounded damage/attack-power or haste profiles appropriate to the weapon.
- Caster specialists across applicable professions: spell-power/intellect or another compatible caster profile.

Use one eligible signature affix first, and a compatible two-affix bundle at a higher trained mastery threshold.
This is a custom bonus to actual crafted gear, not skill, recipe or character-XP grants. A low-level recipe has a
small bonus budget even when made by a master; item tier, role and recipe difficulty constrain power. Specialty
compatibility matters: a defensive smith should not automatically give every item every possible stat.

Haste is a candidate for quickness; directly rewriting weapon swing speed needs separate damage/proc/client
qualification. Use the existing item stat vocabulary and validated effect data. Masterwork identity belongs to the
exact item, survives trade/mail/bank movement and equips with that item. Native binding/level/class/armor constraints
still apply. The same finished item cannot be repeatedly rerolled by relogging, mailing or applying another maker.

## Native seam qualified; catalogue and application remain pending

The inspected core saves item creator, enchantments and random property ID per item instance. Its
`Item::SetItemRandomProperties` uses existing client-known property/suffix entries and writes property enchantment
slots; normal item-template stats are shared and must not be edited for an individual commission. A single native
bundle can contain multiple compatible effects. Calling the setter twice does not append two independent affixes.

The first proposed adapter uses one validated existing property bundle on new unstackable equipment with empty
property slots and no recipe-provided random property. Preserve ordinary enchants, sockets and existing properties.
No candidate native property IDs or effect magnitudes have been admitted. Client data, tooltip/stat calculation,
positive-property/negative-suffix scaling and the selected core's signed16-bit save/load width must all qualify.
Custom new names or new DBC effects may require client data work; a server-only stat label is not proof of a correct
client tooltip. Native creator signatures depend on the template; a persistent commission ledger can retain maker
identity even when the native template does not display it. Do not infer signature support for every recipe.

`Spell::DoCreateItem` invokes `OnPlayerCreateItem` after successful storage/creator handling and before normal craft
skill gain. This is a useful observation seam, but the hook supplies player/item/count rather than recipe spell ID
or proven self-craft provenance. Other creation paths and merged stacks are not automatically masterwork crafts.
An exact successful recipe/cast/output correlation or a qualified context-bearing hook is needed before mutation.

`MasterworkPolicy.h` now proposes eligibility for one exact craft/crafter/item, native validated bundle, current skill
threshold, specialty/recipe match, city workshop and output power budget. It refuses repeat/occupied-property,
stacked/multiple-output, noncraft, unidentified and invalid catalogue cases. It does not select actual effects,
change an item, persist identity or increment skill. Test property IDs/thresholds are synthetic policy fixtures,
not an admitted catalogue or chosen live rates.

## Durable evidence and release sequence

Persist item GUID, creator, recipe, profession skill at craft, specialty, profile/catalogue version, native property
ID and one successful craft event. Reserve one-time application and reconcile exact existing item state after a
crash; retry returns the same outcome rather than another affix. Provenance stays with bank/mail/auction transfers.
Broker and equipment selection must eventually value actual instance effects, not just base template item level.

First prove one actual eligible city craft, one modest native bonus, persistent save/load/mail/equip and its exact
stat effect. Demonstrate unchanged ordinary enchanting/socket behavior and refusal of recipe-provided properties.
Then add the second compatible affix tier and commission/provider reputation. Mercenary duty contracts can reuse
durable banks and safe group admission after their separate custody/control proof. No live craft/guard operation,
bonus, assignment, persistence migration or release occurred in this source-development slice.
