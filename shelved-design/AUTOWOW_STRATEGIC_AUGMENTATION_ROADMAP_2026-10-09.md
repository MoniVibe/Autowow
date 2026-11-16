# AutoWoW strategic augmentation roadmap

Approved direction: make accomplishments change personal capabilities, city production, guild authority and the
world. Develop the native server module while preserving the running world and Galaxy resource priority.

## Feasibility and current evidence

The existing unlinked component contains StrategyPolicy, TreasuryPolicy, BankPolicy, GuildPolicy, MasterworkPolicy
and BossLegacyPolicy. These express eligibility and proposed state changes. StrategyScripts.cpp currently registers
world timing and PvP observation hooks only. They are foundations, not active economy or reward implementations.

Native banks and craft operations provide useful existing physical systems. Durable discovery, contribution,
commission and reward records still need implementation. The old BossLegacyPolicy attribute field excludes
Stamina and is raid-only; it must be replaced or adapted before it can implement the newly selected five-attribute
raid/dungeon policy. Its presence is not proof that the latest rewards work.

The selected core exposes asynchronous database queries and transaction callbacks. Quest reward-item and
before-completion callbacks are present, but neither alone proves every completed quest or dungeon victory.
Encounter completion and participation must be qualified for each pilot. Boss death alone misses surrender,
healing and objective encounters. No client patch is required for records, commissions or server decisions;
novel visible affixes, abilities and assets must use admitted client-known data or a separately scoped client change.

## First playable loop

1. An adventuring group earns a configured dungeon discovery through actual completion and participation.
2. The discovery permanently unlocks one workshop technique for its entitled guild or faction.
3. An adventuring guild places a funded commission for useful gear.
4. Gatherers supply real inputs; representatives reserve existing bank stock and payment.
5. A protected city artisan with the necessary native skill and recipe performs the craft.
6. Exact output is delivered, paid for and equipped through native operations. Contributor credit settles once.

Knowledge unlocks workshop eligibility. It does not invent missing native recipes, profession rank, stock or
character levels. Artisans stay in cities; profession skill advancement and any approved artisan character-level
mechanism are separate from adventuring. Workshop research can add later custom techniques once their native
recipe/property integration is explicitly implemented.

The proof is an earned discovery surviving restart, a real material-consuming craft, reconciled payment/delivery
and useful retained equipment. An order, mail stage or displayed unlock alone is not the complete loop.

## Independent implementation waves

| Wave | Deliverable | Proof needed |
|---|---|---|
| 1 | Durable discovery and knowledge records | Duplicate callbacks and restart cannot award twice; invalid participation refuses |
| 2 | Guild treasury, demand and commission settlement | Reservations and payment reconcile; original donations receive standing once |
| 3 | One city-workshop production chain | Native inputs consumed, skill/output observed, item delivered and equipped |
| 4 | Boss/quest legacies and masterworks | Five-attribute policy, flavored adapters and exact-item properties persist without double application |
| 5 | One region with an upgrade and ownership transition | Old/new owner periods settle correctly; production and spending remain traceable |
| 6 | Hero commands, mercenary contracts and supply campaigns | Autonomous recruitment, duty, wages and return have observed lifecycle evidence |
| 7 | Regional projects, artifacts, doctrines and rival responses | Actual objectives create persistent world consequences and meaningful bot goals |

## Approved workshop time and bulk production

One batch order queues sequential native crafts; ten breastplates require ten independent pieces and ingredients.
Ordinary equipment flows in minutes per batch, masterworks occupy several stages, and legendaries take several
workshop hours through preparation, forging/weaving, tempering, finishing and inspection. Exact recipe timings
remain unadmitted. Only observed eligible city work counts; durable checkpoints resume without outage catch-up.
Timers make a piece ready, not produced. Native craft success, consumed inputs and authoritative settlement advance
the completed quantity. See AUTOWOW_WORKSHOP_PRODUCTION_2026-10-09.md and the component docs/WORKSHOPS.md.

The new pure WorkshopPolicy kernel and regression sources are unlinked/unbuilt. Queue, reservation, scheduler,
native craft adapter and persistent recovery implementation remain pending; the core transaction blocker applies.

The user refined stage meaning: an accepted immutable legendary blueprint selects compatible equipment form,
stat priorities and boss-family signature/support finish. Forging chooses form, tempering stat emphasis, finishing
the themed proc/passive and inspection/attunement the support package. Stages earn work and actual subcomponents;
they do not emit several usable legendary copies. See AUTOWOW_LEGENDARY_WORKSHOP_DESIGNS_2026-10-09.md for
Ragnaros one-/two-handed/tank/caster patterns and thirteen other family directions. Numerical effects, native IDs
and compatibility catalogue remain unadmitted. Current WorkshopPolicy contains no blueprint/effect implementation.

## Approved legendary makers expansion

The explicitly requested six Luna-high passes extended leatherworking, tailoring, enchanting, engineering and
jewelcrafting with90 design concepts plus practical field-gear provisioning. Each profession has8field/4masterwork/
6legendary candidates. Field baselines use ordinary role-appropriate gear/stats/supplies; custom procs and auras
belong to the separately admitted unusual tier. See AUTOWOW_PROFESSION_GEAR_2026-10-09.md and its five catalogues.
None is native-admitted. Immediate bot gearing depends on current recipient demand, actual native recipe/AH/loot
routes, materials, maker skill/training, payment/delivery and retained equipment, not these catalogue counts.

Each configured dungeon/raid finale receives a signature legendary route, with selected complementary penultimate
or memorable encounters. A real recovered core, funded city commission and material-consuming native craft produce
the exact item. The first qualifying design-family craft gives its maker a permanent themed mastery/passive and
named distinction; fulfilled later work builds custom contribution standing. Guilds earn their own workshop
knowledge and project eligibility, while equipped relics can support their actual expedition with local effects.
These scopes settle independently and preserve captured entitlement, item lineage and once-earned provenance.

See AUTOWOW_LEGENDARY_MAKERS_2026-10-09.md/.json for24 initial custom concepts and the expanded competition loop.
No native effect IDs, loot rates, recipe routes, rank/skill changes or first-clear mappings are admitted there.
This expands the selected design; it does not activate items or guild/raid auras. Native binding and equipment
rules remain, and the existing uncapped additive boss-legacy design is unchanged.

Current discovery R10 source is unlinked/unbuilt and its transaction admission remains false. Selected core
BEGIN/COMMIT and reconnect semantics must be fixed and isolated-tested before knowledge, mastery or legendary
settlement can be enabled. Memory-deferred compilation/gameplay testing and Galaxy resource priority remain.

The same discovery model can admit later dungeon techniques and quest-chain milestones through their own
verified event adapters. Region ownership and mercenary duty need separate implementations; they are not inferred
from a kill, proximity, guild name or an existing policy header.

## First slice and constraints

First bounded development slice: discovery eligibility and an immutable knowledge ledger, with no item/stat
mutation. See ../.agents/plans/strategic-discoveries/strategic-discoveries.PLAN.md for the implementation tasks.

Reserve/persist before materializing goods; keep native holdings authoritative after handoff. Capture guild
membership and entitled scope at the earned event. Source receipts remain traceable after aggregation.
Every production consumer checks current authorization, trained skill and available stock.

Existing user-selected personal bonuses remain raid5/10%, dungeon1/2%, additive, all five attributes, all earned
active and no power/loadout cap. Raid consolidation never grants the sum twice. Those are selected designs,
not active effects. PvP glory and contribution standing remain separate from native honor/reputation.

This turn adds plans only. No C++/SQL/configuration/control changes, build, restart, deployment, commit or push.


Material quantities are now visible in [Material quantities for all 90 designs](profession-catalogues-20261009/MATERIAL_BILLS.md). All 90 bills remain proposed; native recipe admission and custom cores are pending.


## Early jewelcrafting: levels 1–60

[Early gems and random socket design](profession-catalogues-20261009/EARLY_GEMS_AND_SOCKET_ROLLS.md) adds 30 proposed cuts across five supply tiers with exact materials. Newly created
qualified crafted/earned gear can roll one additional empty prismatic socket. Rates start at 15% for
common gear and 20% for uncommon gear; role gems start at +1 primary stat/+2 stamina. No roll on
mail, purchase, equip or login. Existing sockets stay intact; higher-tier gems need wearer/host gates.
These are design proposals only: creation/recipe/client/bot adapters and durable once-roll persistence
are not implemented. This extends the earlier socket prohibition only through a future admitted native
creation route; arbitrary gemless items still cannot accept a gem.
