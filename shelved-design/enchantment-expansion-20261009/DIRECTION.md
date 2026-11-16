# Enchantment schools, legendary commissions and territorial wards

User requested four Luna agents at xhigh to extrapolate the approved enchantment direction. This is a design
catalogue expansion: no implementation, native recipes, spell IDs, new drops, live items, controls or build changes.
Existing profession/material catalogues and the40-path staged strategic module remain intact.

## What the world should express

Enchanters stay in city workshops. Adventurers bring actual earned relics, gatherers supply consumable reagents,
representatives commission useful services, and named master enchanters teach recognizable schools. Recharge work
creates repeat demand without requiring repeated raid relics. Weapons and body armor receive useful field enchants
before vanity accessories; an enchant does not substitute for a missing or unsuitable base item.

Four explicit gpt-6-luna/xhigh design lanes:

- Schools:8 recipes,6 field and2 masterwork, with genuine novice-to80 variants to qualify.
- Classic/Outland:8 distinct boss-derived legendary enchants.
- Wrath:8 distinct boss-derived legendary enchants.
- World/wards:4 world-boss legendary enchants plus4 guild/territorial wards.

The target is32 NEW designs beside the existing90 profession designs. Every row needs a compatible host, wearer and
artisan gates, exact reference material bill, charge/ICD/empty behavior, replenishment bill, timed stages and bot-use
rules. Boss source and blueprint entitlement are distinct from a consumed per-project relic. No new native IDs are
invented. All numbers are tuning proposals, not admitted encounter balance.

## Shared rules before an eventual implementation

1. Capture immutable recipe/effect/reagent/timing versions before work. Five legendary stages take eligible city
   work over hours; stages do not fabricate materials, duplicate the output or grant five native skill awards.
2. Reserve actual reagents and the exact host before work. Apply one qualified finish to one host instance after
   native completion; repeated mail/bank/inspection does not create another enchant or maker reward.
3. Keep passive and charged signature behavior explicit. Empty charges leave only the stated passive. Recharge
   consumes a stated ordinary bill, restores only missing capacity and grants no free stock, rank or XP.
4. Persist item charge and wearer/effect-family cooldown state. Equip swaps, mail, relog, trade and downtime do not
   restore charges or reset a cooldown. Define transfer/owner change; unknown handoff remains held.
5. Use verified hostile events. Proc descendants cannot recursively trigger another proc; self/friendly damage,
   admin/factory/probe events and arranged exchanges yield no charge, soul or maker progression credit.
6. Treat a boss blueprint as a knowledge entitlement, a relic as scarce physical stock, and the final enchant as
   a commissioned product. Each has its own once-only identity. Councils, adds, resets and raid sizes do not copy it.
   Drop chances/source availability require separate native encounter qualification; this book admits none.
7. Native Enchanting progression still follows actual trained recipes, skill and rank/character requirements.
   Custom mastery, signatures and teaching cannot pretend to be native XP/rank. A city artisan career is a separate
   earned-work progression integration; a missing rank cannot be bypassed by completing a timer.
8. A taught design does not transfer materials, charges or native training. Apprentice needs its own qualified
   workshop, skill, rank and recipe entitlement, then performs its own input-consuming craft. Avoid farmable token
   refunds and self-funded donation/recharge loops as a shortcut to fame.
9. Guild wards belong to a specific authorized site and physical reserve. Capture suspends or redirects future
   service only after ownership reconciliation. No global scouting, free troops, remote guild-bank withdrawals,
   duplicated supply shipments or automatic army victory. NPC campaign points and native stats remain distinct.
10. Existing boss-legacy attributes and flavored first-victory traits remain unchanged and uncapped as requested.
    Individual enchant slot compatibility, trigger potency and cooldowns are mechanic rules, not global hero caps.

## Material and inspiration evidence

MATERIAL_REFERENCE.json is extracted from the preserved prior bill catalogue:40 historically verified item names.
That evidence admits the names only, not new recipe amounts, drops, enchant effects or lower-tier substitutions.
Additional ingredient names are unverified candidates unless separately checked. Custom relics/tools are marked.

Encounter themes can be grounded in primary project scripts, for example [Sapphiron's frost/ice mechanics](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Northrend/Naxxramas/boss_sapphiron.cpp)
and [Azuregos' arcane/frost mechanics](https://www.azerothcore.org/doxygen/d8/dd0/boss__azuregos_8cpp.html).
Those sources are inspiration; they do not contain or authorize the new effects proposed in this book.

## First implementation recommendation

Start with one role-useful Last Stand-style field enchant plus an ordinary material-consuming recharge service.
Prove actual host custody, native application, a genuine hostile trigger, charge debit, durable cooldown and repeat
service. Then one earned boss relic/five-stage legendary commission; finally a local territorial ward. Catalogue
completion is design evidence, not that living chain. No server deployment is commissioned by this design request.
