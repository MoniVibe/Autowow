# S89 smallest viable paid cross-guild craft contract — design only

## Decision

Use **one same-faction, one-output commission** between a requesting house treasury and one existing eligible crafter from any guild. The crafter keeps their guild and ordinary background life. They accept only while already online and voluntarily idle; one accepted job temporarily owns that bot until it settles or reaches a finite exit. A guild may still appoint its own dedicated artisan when one is available.

This is a new runtime capability. Current AH code can buy an already-listed finished weapon, and current MailOrders can buy already-owned goods or materials. Neither proves a custom craft commission. A finished-item AH buy remains the first option when a suitable listing exists within the requester's price cap; the contract opens only for unmet custom demand.

## First-contract envelope

The first contract is intentionally narrow:

- Same faction; requester is a real house guild and pays from its real guild bank.
- Exactly one known recipe spell, one deterministic tradeable output, one craft, and one recipient house rep. Reject bind-on-pickup, random/secondary output, cooldown recipes, specializations the crafter lacks, and more than 12 mailed stacks.
- The requesting rep supplies every reagent. Existing AH/COD material purchasing may fill a shortage first. Concrete reagent stacks are reserved before offers; their item IDs, quantities, and eventual mail receipts form the custody manifest.
- Only online existing Playerbots may bid. Eligibility requires the recipe and skill, same faction, free output space, no active contract, and the ordinary independent-AI safety gates: alive, unpaused, out of combat, outside instances/groups/flight/transport, no supply role work, no zone/errand/dungeon/probe ownership, RPG Idle, and no live quest work. No bulk login and no permanent artisan role.
- Candidate acceptance is explicit runtime state, not inferred from profession ownership. Lowest real ask wins; ties use fewest settled commissions in the current fairness window, then oldest last-win time, then lower character GUID. One active contract per crafter prevents a successful specialist from monopolizing all jobs.
- The ask is labor only because the requester owns the materials. It includes both mail postages and a bounded labor floor; any higher ask must be derived from visible market opportunity, never invented gold. Selection compares `reserved material replacement cost + ask` against an actual finished-item AH buyout when one exists. No synthetic buyer or guaranteed demand is created.

## Native execution and custody

1. **Open:** demand dedupe confirms no owned, incoming, already-crafted, AH-purchased, or active-contract output. Allocate a durable 64-bit never-reused `contract_id`; store schema version, requester guild/rep, output item, recipe spell, quantity one, maximum fee, deadlines, and material manifest.
2. **Fund:** reserve the fee plus requester postage against the guild bank. Treasury consumers must see balance minus active reservations. Reserve exact reagent units so supply orders and another contract cannot spend them.
3. **Offer/accept:** rank only eligible idle crafters. Acceptance records the crafter GUID/guild, ask, recipe/skill snapshot, station and mailbox route, and lease deadline. No candidate means a bounded `no_supplier` failure with all reservations released.
4. **Handoff:** the requester rep physically reaches a mailbox through the existing errand route, then the world thread mails real tradeable reagent stacks. Whole-stack excess remains requester property in the custody manifest. Cross-map mail is allowed by the native system; cross-faction mail is rejected. The crafter collects the mail through a real mailbox stop.
5. **Craft:** the crafter reaches the recipe's required world focus/station on a reachable map. Reuse the guarded exact-one-craft checks and require its exact reagent/output postcondition. `CanCastSpell` remains the final station/tool gate. No output observation means failure; never fabricate the item.
6. **Deliver/pay:** the crafter reaches a mailbox and sends the exact output to the requesting rep as COD for the accepted ask. The rep reaches a mailbox, the reserved treasury money tops up its purse, and the core COD take transfers the actual item and sends actual payment to the crafter. Return all unused requester-owned reagent excess in a separate zero-COD mail. Settlement occurs only after the output is in the rep's inventory and the COD payment mail exists.

The low-level guild mail helper does not require mailbox proximity, so the contract runtime must gate every invocation on the corresponding physical mailbox stop. Crafting must likewise pass a real station/focus check. These gates prevent invisible world-wide handoffs.

## Duplicate, failure, and fairness rules

- One open key per `(requester guild, output item)`; terminal records retain their ID and the open key is released. Demand subtracts owned stock, incoming mail, AH wins, in-progress native casts, and active-contract output.
- Before materials leave, decline/timeout releases material and treasury reservations with no charge. After handoff, interruption pauses briefly; quest work may preempt only before custody is accepted. Death, offline, route failure, mailbox full, station failure, or craft rejection has a fixed retry/deadline budget.
- On terminal failure, the crafter mails all remaining requester-owned materials back at requester expense. If the exact craft consumed reagents but no output was observed, mark `disputed_consumption`; pay no fee, release unspent money, and retain the evidence for review. If output exists but delivery cannot complete, it remains requester-owned contract custody and retries delivery until the deadline. A failed sale never gifts requester-funded output to the crafter: the finite failure route returns it to the requester without COD; if physical recovery is impossible, retain a truthful disputed-custody record without fabricating goods or a refund. No infinite retry and no replacement item.
- A supplier is paid only through successful COD settlement. The fairness window and one-active-job cap distribute work; the price cap and AH comparison protect the buyer. Resource reservations protect other house production from double spending.

## Acceptance criteria for the first playable contract

**PROVEN only when one live commission shows:** unmet demand with no qualifying finished AH purchase; one idle existing cross-guild crafter accepts without reassignment; the requester treasury reserve reconciles; real reagent item GUIDs leave the rep and arrive via mailbox; the crafter reaches the required station; one exact native craft consumes the recorded reagents and creates the recorded output; the output arrives by COD; the guild bank, rep purse, and crafter payment reconcile including postage; unused materials return; the same demand does not create a duplicate; and both bots resume their prior AI after settlement.

Also exercise bounded failures for no supplier, insufficient treasury, mailbox full, route/station unavailable, crafter interruption before custody, exact-craft postcondition failure, and COD timeout. Restart recovery must resume or refund from the durable state without reusing the ID or duplicating mail/craft/payment.

## Integration ownership and write set

Implement serially after the active Smith weapon-line lane closes, because it currently owns `AutoWowSupply.cpp`, `SupplyPolicy.h`, `NewRpgSupply.cpp`, configuration, and supply tests.

- **New contract owner:** `src/AutoWow/CraftContractsPolicy.h`, `src/AutoWow/AutoWowCraftContracts.h/.cpp`, `tests/CraftContractsPolicyTest.cpp` — versioned state machine, candidate ranking, price/fairness, dedupe, reservations, receipts, recovery.
- **Durability:** one new `data/sql/playerbots/custom/..._autowow_craft_contracts_v1.sql` migration for contracts/material custody and a persistent auto-increment 64-bit ID.
- **Exact craft seam:** narrow typed completion API in `AutoWowCraftControl.h/.cpp`; preserve its validation and exact inventory-delta authority.
- **Money/mail owner:** reservation-aware treasury API in `AutoWowGuildsPolicy.h/.cpp`; continue using its native item/COD mail checks and transactions.
- **Supply/market seam after merge:** small demand/reservation hooks in `SupplyPolicy.h` and `AutoWowSupply.cpp`; route contract COD before ordinary MailOrders in `AutoWowTrade.cpp`; add the temporary contractor station/mailbox step in `NewRpgSupply.cpp` without converting the crafter into a supply role.
- **Lifecycle/config/ledger:** `src/Script/Playerbots.cpp`, `AutoWowQuestLedger.h`, and `conf/playerbots.conf.dist` for load/tick, append-only receipt events, enable flag, finite deadlines, price cap, and fairness window.

**Remaining limitation:** this is a prototype design, not a ratified pricing/escrow policy. Supplier asks, fairness windows, deadline values, idle-supplier availability and restart-safe settlement require implementation review and observed acceptance. No custom-commission runtime, durable reservation, contractor travel action, or live receipt exists yet. The initial audit changed no source, build, runtime, database, bot, or Git state; root subsequently corrected the failed-delivery custody rule above to preserve requester-funded property.
