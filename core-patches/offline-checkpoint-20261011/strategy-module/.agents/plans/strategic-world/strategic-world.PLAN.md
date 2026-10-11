# Strategic AutoWoW world — native server development plan

The user's “offline” instruction means developing server source for a later release. Galaxy games retain resource
priority. Keep the supported core/Playerbots update and running S125 session separate; no heavy build or restart.

Discovery preparation adds immutable eligibility, settlement classification and a private async repository with
unrun regression sources and pending DDL. The checked native BEGIN/COMMIT dependency is an explicit default-false
admission gate. No catalogue, completion hook, persistent unlock, DB pool or production consumer is active.
See docs/DISCOVERIES.md and the workspace strategic-discoveries plan for the separately gated database seam.

Workshop preparation adds version-bound timed work, sequential bulk piece proposals and five-stage legendary
routes; docs/WORKSHOPS.md binds the durable queue/native-craft integration contract. No scheduler or persistence
adapter is installed. Prepared regression sources remain uncompiled under the user's memory deferral.

## World direction

Four connected systems give societies decisions beyond individual quests:

1. **Territory economy.** Controlled regions provide a capped baseline resource stream. Faction treasuries and
   Overlords invest in regional industry or supply guild orders. Higher industry tiers increase future production;
   specialized higher-tier baskets require explicit content validation. Gatherers provide additional volume and
   missing inputs. Production events are distinct from native node loot.
2. **City workshops.** Smiths, tailors, leatherworkers and other artisans stay in cities. Each has a useful-output
   queue and a skill-advancement queue, checked against actual learned recipes/trainer ranks/materials. City
   specialization can later accelerate eligible native work inside the trained rank, without granting recipes.
3. **Guild contribution.** A separate society standing ledger recognizes useful delivered inputs and completed
   commissions. It credits the original contributor once, against actual demand, excluding repeated forwarding,
   self-donation loops, returns and bonus copies. It is not native faction reputation or a free character-XP source.
4. **PvP glory.** Seasonal personal/guild prestige from eligible opposition and later verified objectives/defense.
   Begin with standings and cosmetic recognition. Do not make kills a raw-material faucet or overwrite native honor.
   Objective success should eventually matter more than farming vulnerable opponents.

These systems should create campaign goals: equip a struggling adventuring guild, advance a workshop to its next
useful tier, secure an input shortage, or fund a regional upgrade. Start with configured territory ownership;
capture mechanics belong in a later feature built on actual objective events, not guessed ownership from map IDs.

The user has now selected member deposits and representative-only bank withdrawals, with useful donation standing.
`GuildPolicy.h` expresses the access and contribution-quantity rules, without live enforcement or awards. Native
guild-master authority remains part of representative governance. See `docs/CAMPAIGNS.md` for the selected campaign
direction: frontier depot raids, verified territorial capture redirecting future income and later supply escorts.
These objectives are authorized design work, not implemented combat, capture, loot or permission changes.

The user further selected mercenary services from dedicated guilds or adventuring/raiding detachments, plus skill-
and specialty-dependent signature affixes on actual crafted gear. `docs/MERCENARIES_MASTERWORKS.md` binds funded
temporary duty to existing autonomous fighters and outlines notable city makers. `MasterworkPolicy.h` prepares
bounded one-time eligibility using skill at craft and a validated native bundle; no property catalogue, item
mutation, guard contract/control or bonus award is active. Preserve the exact source/release boundaries below.

Personal raid veterans earn one proposed conservative, encounter-flavored first-kill trait per configured boss.
The latest selection adds +5% to all five primary attributes per raid boss/+10% at the configured finale, and
+1% per dungeon boss/+2% at its finale. Final amounts replace ordinary amounts; percentages add, never compound.
All earned traits remain active without loadout/progression caps, building mythical heroes gradually. Complete
raids consolidate existing effects into tidy named legacies without awarding their total twice. The previous
huge attribute/health/output packages are superseded. `BossLegacyPolicy.h` is still an unwired eligibility and
arithmetic scaffold; typed conditions/ratings/schools need a new adapter. See `docs/BOSS_LEGACIES.md` for persistent
earning and consolidation boundaries. Faction workshop knowledge remains separate; no personal reward is active.

## First native slice — implemented source, unbuilt

Separate `mod-autowow-strategy`, with existing WorldScript/PlayerScript hooks, cached default-off observation
options, bounded income-window policy and PvP glory candidate policy. It leaves honor, AI, motion, inventories,
NPCs, factions, professions and quests untouched. No core/Playerbots hook edit is needed.

The native treasury kernel proposes capped deposits, reservations, partial delivery, workshop consumption and
territory investment, with uint64-safe conservation checks. It does not itself commit state or create inventory.

The native bank kernel now distinguishes faction and guild owners and item/copper accounts. One-way allocations
and bilateral deals return proposed state only. Two-sided reservation, settlement and cancellation preserve holds,
refuse overspending and recheck expiry and capacity. Native guild bank ownership, permissions and withdrawal limits
remain authoritative for physical stock. See `docs/BANKS.md`; these policies have no persistence or native adapter.

Glory candidates use the native killer/victim and current team/context facts. Exclude self, friendly, duels, arena,
grey targets and native no-credit targets. Shared-account humans are excluded; two native headless sessions may
share an account, but retain pair and daily caps. Native bot-master/arranged-farming qualification is still needed
before actual rewards. No raw Player pointers leave callbacks; no map-thread SQL.

Repeated opponents use an unordered pair, covering reciprocal kills in the same limit. Initial preview weights
10/5/2,10-minute cooldown,50 daily points are explicit proposed defaults. Daily memory is bounded and reset by the
world thread; restart resets previews. This is observation state, not the persistent seasonal leaderboard.

## Next implementation — durable treasury before physical goods

Create a module-owned prepared-statement persistence layer and dated approved migrations, keeping SQL out of map
threads. Persist territory owner/tier/effective period, period receipts, owned accounts, deals and reservations. Use one
transaction to authorize income and stock changes, binding exactly one territory/period event. Ownership and tier
changes settle old periods before taking effect. Cap offline settlement and make skipped periods unreplayable.

Never let virtual treasury stock and newly created physical inventory both remain spendable. The stock model must
state when physical goods are materialized and transferred, and atomically mark that stock as reserved/consumed.
Partial delivery leaves accountable in-transit quantities. Failure/retry needs exactly-once identity, not another
item creation. Use real postage and the established mailbox/service path; delivery is not pickup or craft proof.

Account versions and unique operation identities must commit atomically with deal-owned holds. The aggregate treasury
is a projection of owner records, never another spendable ledger. Native guild bank callbacks require confirmed
outcomes; the money-deposit hook precedes the transfer and cannot itself authorize credit. Physical handoff needs
its own durable intent/acknowledgement and crash reconciliation, preserving native permissions and stock custody.

Admission needs validated native item templates, deliberate regional baskets, actual guild identity and protected
workshops. Rates come from recipe consumption and useful output, not the earlier illustrative Redridge basket.
Broker purchasing remains the emergency bridge for weapons and critical armor until workshops can make them.

## Glory and contribution persistence

Persistent seasons require event identities, unordered pair histories/cooldowns and per-player caps across restart.
Guild credit binds membership at the event, so joining/leaving cannot move already-earned credit. Reserve reward
redemption before applying it; failed redemption must not duplicate a reward. Rankings and unlock conditions are
separate from spendable economy stock. Native honor remains its own system.

Add battleground objectives/territory defense through existing objective hooks only after selecting a concrete
objective. Do not infer capture, assists or participation from a kill near a town. There are no arranged bot matches,
forced PvP, reciprocal farming, spawned opponents or guaranteed victories in this plan.

## Release evidence

Static checks do not replace compilation. The prepared pure-policy tests still need an admitted small test run;
module signatures/load/config and concurrent callbacks need a qualified full build and later native observation.
Follow supported-update and safe cutover gates, then observe original territory event → durable treasury → real
guild receipt → native city craft → actual profession/work progression → useful delivery/payment/retained equip.
For glory, prove the actual hook and eligibility/refusals before persisting points or adding rewards.

## Supply contracts and caravan source slice

The user selected useful supply commissions and physical caravans. SupplyContractPolicy reuses BankPolicy balances,
holds exact buyer copper, registers native handoff intents and pays only verified accepted units. Immutable terms,
deadline/receipt timing and unresolved-cargo cancellation guards preserve ownership and conservation. CaravanPolicy
binds original physical manifest/metadata, route/version and carrier with ordered native observations and explicit
whole-manifest terminal disposition. Mixed disposition and live adapters remain held. See docs/SUPPLY_CONTRACTS.md.

Small isolated GCC13/C++17 contract/caravan/existing-bank regressions passed under separate bounded resource custody.
This qualifies pure proposals only, not persistent payment, movement, guild stock, production, escorts or gameplay.
Next implement checked transaction/native reservations and useful ordinary delivery before one protected native route.

## Territorial NPC militia source slice

The approved hybrid keeps playerbots as heroes and occupations, with NPC squads serving overlord territorial orders.
TerritorialArmyPolicy implements muster/food transfer from exact available accounts, original persistent troop IDs,
captured building/equipment profiles, and attack/defend/patrol/escort proposals. Both engaged squads lock one mode
and one battle outcome; strategic attrition and verified native physical receipts share persistent casualty state.
Capture is a two-sided squad/ownership proposal, not a live income change. Prototype campaign points are not native
WoW stats. See docs/TERRITORIAL_ARMIES.md; durable issuance, unique roster persistence, actual creatures/navigation,
combat and treasury-period reconciliation remain release gates. No live NPC, bank, hero reward or control mutation.
