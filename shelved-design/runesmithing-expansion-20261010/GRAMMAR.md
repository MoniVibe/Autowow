# Runesmithing pattern grammar

**Design only.** Runesmithing is a proposed city artisan career that composes event patterns on compatible host gear. It does not replace death-knight runeforging, ordinary enchanting, gems, native training, or native class, skill, level, and proficiency checks. All glyphs and effects below are proposed names and mechanics; this document does not claim a native skill line, client UI, recipe, database row, or event hook exists.

## Typed pattern

```text
Pattern := exactly_one(Trigger) -> exactly_one(Transformation) -> exactly_one(Release)
```

Each role has a distinct job. A **Trigger** accepts one witnessed native event with its exact actor, target, item, success, and provenance. A **Transformation** turns accepted evidence into a small, expiring, bounded state; it does not produce an effect or resource. A **Release** resolves one bounded effect only after the state threshold is met and the target is still legal. The role and slot are part of every glyph's identity.

The compatibility edges in `GRAMMAR.json` are necessary checks, not an admission list. A syntactically valid triple is inert until one immutable design explicitly admits that exact triple, host, target rules, numbers, and gates. Do not take the Cartesian product of the glyph vocabulary as a catalogue. Each added triple needs its own review for usefulness, host fit, interactions, economy, and abuse. The six examples below explain meanings; they are not gear recipes or admissions.

## Evidence and finite state

Require an authoritative native event identity and provenance. Evidence must name the wearer, exact item GUID, event/cast identity, source, target, result, and relevant native state. Reject missing, replayed, stale, fabricated, self-inflicted, friendly, proc-origin, training-dummy, factory, and arranged-farming evidence. For a healing trigger, the recipient must be a living friendly who has verified recent hostile injury; overheal and self-healing do not qualify. For approach, movement must be voluntary closure toward a legal hostile; knockback, forced movement, teleport, and scripted relocation do not qualify.

An item pattern has finite states: **Dormant**; **Primed** with a bounded counter and expiry; **Claimed** for one unique settlement; **Cooldown**; and, only for an explicitly charged pattern, **Depleted**. Each event contributes at most one unit. Counters have a fixed ceiling and short expiry; expired or excess units are discarded. A successful release clears its priming state. No timer grants work or progression while the artisan or wearer is offline.

When a design uses `resonate_pair`, its admission names exactly two predicates and one discriminator: native event type, spell school, or injured-recipient identity. The two causal records must have different native event IDs; different schools require separate successful cast IDs and different schools, while different recipients require separate qualifying heal IDs and recipient GUIDs. One event cannot fill both halves, and no unrestricted “different anything” wildcard is valid.

For each event, resolve eligibility and release legality first, then make one idempotent settlement keyed by native event ID, item GUID, wearer, and admitted pattern version. That settlement advances the state once, sets item and wearer/effect-family cooldowns, and if applicable debits one charge once; only then may the native effect be emitted. Duplicate callbacks return the prior result. Unknown outcomes stay held for exact reconciliation; they are not replayed as fresh procs. Persist item state, charges, and item cooldown with the item through equip changes and transfers; preserve the wearer's family cooldown across item swaps, relogs, and copies. If identity or provenance cannot be reconciled, fail closed.

Mark every release result with rune-origin provenance. Damage, healing, absorbs, and other proc-created events never count as new evidence for this or another rune pattern. A release cannot recursively trigger itself, another rune, or progression. A failed pre-settlement legality check spends nothing; once claimed, the attempt settles once and cannot be retried to duplicate an outcome or refund an uncertain charge. Proposed caps are ceilings, not tuned live values; an admitted design must give exact magnitudes and intervals.

## Composition and equipment contracts

An inscription is normally durable and has no compulsory recharge chore. A rare strong design may explicitly have a finite charge count, a zero-charge behavior, a visible remaining count, and an ordinary-input refill bill that restores an exact amount of missing capacity. Refill never consumes the rare boss relic again. At zero charges the release is inactive; only an explicitly admitted passive baseline may remain. Neither trade, equip changes, death, relog, nor downtime restores charges.

The Runesmith finish is a separately admitted item channel. Keep a compatible native enchant and socketed gems intact. Reject an item when its native finish, slot, armor/weapon type, class/proficiency gate, or another installed effect conflicts; never silently replace an enchant or gem. For every design declare its exact hosts, conflicts, legal targets, and stacking family. Multiple crafted items can coexist when their admitted families and caps permit it. Item or family limits are per effect channel and do not impose a global hero/legacy cap or a four-trait loadout limit.

Every pattern uses legal native targets and effects. Releases do not force movement, teleport, resurrect, grant resources/items/skills/XP, seize remote goods, or guarantee a battle. Keep damage and healing outputs bounded against verified causal events; do not permit damage-to-heal or heal-to-damage feedback. A bodyguard effect may protect only its admitted wearer or eligible ally. Runesmithing remains useful through choices of event, buildup, outcome, host, and maintenance—not through copying a death-knight spell or granting a class ability.

## Six illustrative readings

Each entry is exactly one typed triple. These are compatible meanings under the proposed edge matrix; a real design must separately admit its exact mechanics and item.

1. `guard_parry -> gather_impact -> reprisal_hit`: a successful hostile parry gathers one bounded impact token, then permits one capped hit against the still-legal original attacker. `guard_block` may be considered for a separate defensive variant, but requires its own exact admission.
2. `steadfast_hold -> bind_protection -> ward_absorb`: holding position under a witnessed hostile threat primes one personal, short-lived absorb. The effect never roots or moves the wearer.
3. `pursuit_approach -> gather_impact -> haste_window`: voluntary approach to a hostile target primes a short self-only haste window. It grants no forced movement, teleport, or extra proc chain.
4. `spell_alternation -> focus_arcana -> elemental_burst`: two distinct successful hostile spells of alternating schools prime one bounded burst using an admitted school and target. The trigger is generic event evidence, not a copied class spell.
5. `mercy_heal -> weave_life -> heal_echo`: a genuine heal to an injured friendly primes one capped echo heal to that still-injured recipient. Echo healing is rune-origin and cannot feed another echo.
6. `ambush_survive -> bind_protection -> escape_window`: surviving a witnessed hostile opener primes a short self-only defensive/escape window. It does not reposition, teleport, or prevent death.

## Source boundary

The names and gameplay behavior in this proposal are fantasy design, not server facts. The local design brief supplies the shared glyph vocabulary, class/proficiency and progression boundaries, and anti-abuse requirements. The workshop policy supplies staged-work and durable-receipt constraints; the enchantment review supplies the separate item/owner-family cooldown, charge, and compatibility precedents. See [BRIEF.md](BRIEF.md), [WORKSHOPS.md](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md), and [ROOT_DESIGN_REVIEW.md](../enchantment-expansion-20261009/ROOT_DESIGN_REVIEW.md).
