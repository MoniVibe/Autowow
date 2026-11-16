# Territorial economy proposal — October 7, 2026

Design only. No gameplay source, configuration, items, skills, money, database or runtime changed. Existing staged critical-gear/city-artisan changes remain separate and undeployed.

The user proposes passive resource income from controlled territories, allocation by Overlords to guilds, city crafting bonuses, and resource-funded economic upgrades. This deliberately extends the earlier earned-only economy with explicit territory production. It does not authorize unrelated progression grants or changes to existing live configuration.

## Recommended first slice

One starting territory per faction with explicitly configured ownership, a 30-minute production period, durable faction stock, and capped stored resources. Capital ownership should initially be configured; contest/capture mechanics are a later feature. Ownership, tier, income schedule and stock must survive restart without replaying income. Bound offline accrual; an outage must not produce unlimited catch-up grants.

Redridge's proposed 200 copper ore /150 tin ore /50 iron ore /200 Peacebloom /100 Briarthorn per hour is an illustrative resource basket, not a validated local resource catalogue or balanced production rate. Monetary copper is a separate resource type. Use exact item IDs and validate each territory basket against the installed content before admitting it. Include other inputs only deliberately. Scale rates against actual guild consumption and useful output, rather than total accumulated stock.

Each production event has one durable territory/period identifier. Faction ownership and tier are checked for that event. Item creation occurs once and is recorded as territory production, never native gathered loot. Transfers have their own durable identities; partial delivery, retry and restart must conserve quantities. No XP is awarded for passive generation or allocation.

The Overlord allocates available stock to actual guild shortages and craft orders. The current Overlord already performs demand scans and order planning; physical stock admission and allocation require new integration. Allocation must be bounded and fair across guilds, including consumables and bags, with emphasis on weapons and critical body armor. No direct free equipment to recipients. Use actual inventories/storage and the established delivery path, preserving mailbox limits, postage and stock reconciliation.

Gathering supplies additional volume, materials outside the passive basket and inputs for upgrades. Passive income is a baseline, not a reason to remove gatherers. Low-tier territories help apprentices advance; they cannot supply every later recipe. Higher tiers or territories require separately defined material baskets.

## Territory upgrades

Tier 2 consumes specified materials from faction stock in one durable transaction and raises future output. Tier 3 can add a deliberately chosen higher-tier basket. Costs and rates remain proposed until balanced against measured income/consumption. Upgrade purchases must refuse insufficient stock and duplicate/replayed requests. Ownership loss ends future income/bonuses; previously distributed goods are not deleted or forcibly reclaimed. Concurrent ownership changes and payments require explicit transaction semantics.

## Capital bonuses

Ironforge's proposed smith bonus is feasible as a custom economy rule. Start with one bounded bonus, then measure it. A bonus skill point should require a successful skill-appropriate craft and stay within the trained rank; it must not unlock recipes or bypass trainer/character-level requirements. Grey-recipe farming must not generate unlimited bonus skill.

An occasional extra output is a separate policy choice from skill acceleration. It must follow successful native production, be explicitly recorded as city bonus output, preserve item binding/stack rules and be counted once. It is not a second native cast and must not independently multiply work XP, payments or order fulfillment. Avoid multiplying every craft's output in the initial trial; a modest capped bonus is easier to balance. Ingredient savings is an alternative, not an additional simultaneous bonus in the first slice.

## Acceptance and reporting

Test ownership/tier changes, timing, duplicate events, restart recovery, capped offline accrual, allocation conservation, partial delivery, upgrade payment atomicity and bonus eligibility. The living acceptance chain is territory production → Overlord allocation → actual guild receipt → eligible city craft → skill/work progression → useful equipment delivery/payment → retained equipment. Resource creation or order counts alone do not establish production benefit.

Report created quantities, held stock, guild allocations, native crafts, bonus outputs, consumption, sales/exports, upgrade costs and next stock balance separately. Reconcile each item: opening stock + production + imports = closing stock + consumption + exports. Internal transfers cancel at faction scope. Current gathering bugs and missing materials still need their own acceptance; passive income must not hide them.

Proceed in stages: durable territorial stock/income and allocation; one resource-funded tier upgrade; then one city bonus after baseline throughput is measured. Both factions participate in the trial, and all features remain opt-in until a qualified release.
