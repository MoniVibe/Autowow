# Runesmithing: a city profession for behavioral inscriptions

Status: architectural design proposal, not source implementation or an installed profession. The user approved the
in-chat direction and a four-Luna/xhigh design swarm. This document carries that intent into a coherent subsystem;
the lane cards supply concrete patterns, costs and progression. All numerical balance and custom outputs are proposed.

## Purpose and success

City artisans create useful equipment patterns for adventurers, gatherers, healers, tanks and mercenaries. A pattern
changes the relationship between ordinary actions: block then counter, endure then shield, alternate schools then
focus, heal then protect. It does not simply duplicate enchanting's stat improvements or jewelcrafting's gems.
Death knights retain their existing class runeforging. No existing profession, spell or armor proficiency is removed
or automatically granted. Artisans can learn from productive city work and other artisans, supplied by adventurers
and other crafts; they do not need to raid to perform a commissioned boss-relic project.

Success ultimately means real reserved components and host -> observed city work -> one applied inscription ->
verified eligible combat sequence -> bounded effect -> persistent cooldown/charge state -> delivery/payment and
useful maker progression. A design catalogue or a successful build alone does not establish that outcome.

## Approach choices

1. **Separate workshop discipline first — recommended.** Its earned Runesmithing career, known patterns, orders and
   specialties belong to the strategic workshop module. Existing native professions provide material components.
   A future trainer/workshop interface exposes it without replacing the user's profession choices. This still needs
   real durable progression and native material/application/effect adapters; nothing is activated by this design.
2. **Reuse/unlock DK runeforging.** This may reduce some surface work, but ties the new economy to class mechanics
   and could overwrite the very identity we want to preserve. Not selected.
3. **New native profession and client skill tab immediately.** This is the full presentation goal, but requires its
   own skill/ability/trainer/spell/client-data compatibility qualification. It is not inferred from a new server table.

For the first approach, the custom career is additional server-tracked workshop progression, not a silent change
to native primary-profession slots. A future native registration policy must preserve existing professions and
validate the two-slot/client behavior before activation. Do not manufacture a native skill-line ID in these cards.

## Grammar and selection

An inscription contains exactly one approved trigger, transformation and release, plus immutable recipe parameters.
The twenty proposed glyphs are a vocabulary, not 288 automatically legal recipes. The first catalogue proposes only
the explicitly reviewed twenty-four recipe tuples. Glyph knowledge is necessary but not sufficient to compose an
arbitrary new combination. Each recipe fixes host, roles, numeric limits, condition provenance and event sequence.

Craft choices are quoted before acceptance: compatible host family and exact item instance, admitted pattern,
material/timing version, allowed role variant and optional learned alphabet. Changing the quote after reservation
requires a new order; it cannot change an already applied item's behavior by editing a catalogue row.

Use a separately implemented rune inscription record/channel rather than assuming an existing enchant or gem slot
is free. One pattern per exact item channel is an item format rule. Distinct compatible enchants, gems and existing
boss legacies remain available; there is no global four-trait or hero earning cap. Death-knight runeforges require
explicit compatibility/removal tests before sharing a host with the custom channel. No native item channel is admitted.

## Progression and supply

Ten field recipes span five equipment bands; two masterworks deepen a role. Six separately costed component recipes
provide five metal blank grades and one binder. Finished inscriptions consume those components, not the components
and their raw ingredients again. Host gear is supplied separately. Bulk jobs remain sequential: each host needs its
own real successful application and receipt; a ten-piece order is not one craft multiplied by a timer.

Runesmithing 1–450 is a proposed custom earned career, distinct from native skill or character XP. Pattern practice,
useful commissions, mastery and teaching require authoritative paid-input useful work. No elapsed timer, endlessly
reapplied finish, bank/mail transfer, self-funded wash trade or authoring a pattern earns a level by itself. Native
supplier skill, rank, recipe and character gates still apply. City-based character advancement needs a separately
admitted earned-work reward mechanism; custom career points do not impersonate native XP.

Ordinary field inscriptions are durable. Recharging every sword is not a required daily chore. Exceptional charged
scripts have their own ordinary refill bill, missing-capacity limit and empty behavior; a refill never consumes a
second boss relic or grants free mastery. Repairs/re-inscriptions quote real input/output custody and replacement
terms. Destruction or cancellation releases only actually unused materials, not a duplicate of consumed stock.

Boss alphabets separate transferable pattern knowledge from a scarce physical relic. One relic is consumed per
legendary commission; five eligible city-work stages take hours. The boss gates acquisition, while the inscription's
signature works in other eligible world/raid combat. Named master runesmiths teach a specialty through their own
distinct useful-work history, not through borrowed guild glory or free learning on login.

## Runtime and recovery contracts

An eventual implementation separates immutable recipe definitions, career/knowledge records, workshop attempts,
item inscriptions and combat sequence state. Craft settlement atomically claims materials, exact host and the
once-only result; an unknown application outcome stays held until actual item/material reconciliation.

Triggers use genuine native events, current host/equip/owner/role and hostile provenance. Sequence state has finite
windows and caps. Damage descendants cannot feed rune, enchant or boss-legacy procs recursively. Healing triggers
require real hostile injury provenance. No forged callbacks, self/friendly damage, probes, factories, dummies or
arranged farming earn rewards. Movement patterns observe ordinary movement and never force it or replace its owner.

Persist charge ownership and item identity across mail/trade/relog; wearer/effect-family cooldown continuity stops
copy swapping from resetting it. Expiry or equipment removal can discard an unfinished sequence but cannot refund
spent charges or clear cooldowns. Offline time does not finish stages or refill items. A compatibility matrix decides
eligible PvE targets and optional future PvP behavior; catalogues alone authorize neither.

## Smallest implementation and acceptance slice

First pilot: one ordinary Guard -> Gather -> Reprisal pattern, one compatible shield/weapon host, one low-tier blank
and binder bill, one city artisan and one autonomous recipient. No new client files, boss drops or charge service
are needed for this slice. Implementation still needs its own reviewed plan, exact source custody and admitted hooks.

Required evidence: input reservation and successful consumption, one exact host inscription, one useful-work career
receipt, a genuine block/parry sequence and a bounded nonrecursive counter effect. Exercise callback replay, foreign
host/owner refusal, unequip/transfer/restart cooldown continuity and uncertain application reconciliation. Compile
and policy evidence remain separate from actual native combat observation. Then expand to the other field roles,
one rare alphabet commission and optional maintenance; a full client profession UI follows independent qualification.

## Primary source context

AzerothCore documents skills in [SkillLine.dbc](https://www.azerothcore.org/wiki/skillline), including profession and
class-skill categories. Its [SkillLineAbility structure](https://www.azerothcore.org/doxygen/d8/d97/structSkillLineAbilityEntry.html)
separately records class/race masks, skill threshold and linked spell. Its [trainer schema](https://www.azerothcore.org/wiki/trainer_spell)
records skill/rank/character requirements and identifies existing Runeforging as skill 776. These establish distinct
data boundaries, not that a new profession is compatible with our current executable or unmodified client. The
workshop-first route is a design recommendation based on those boundaries and the local inactive workshop policy.
