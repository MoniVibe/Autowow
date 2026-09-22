# Guild economy pilot

Status: held at the user's 2026-09-23 handoff request. Native source compiled and tests passed, but the new binary was not launched and no trade occurred. This document is a run contract, not a claim that a guild economy has already happened.

## Player loop

Keep the six zone scouts playing as the progression baseline. In parallel, use four existing Horde bots from two *actual* guilds for one small resource exchange. A guild decision may choose an objective; the native controller alone executes gameplay. The first contract transfers exactly one existing, tradable item for a capped copper price through the ordinary AzerothCore trade handlers. Read-only before/after observations and a native receipt must agree before it counts as settled.

The existing same-faction fixture candidates are guild 2, **Ancients of Part Times** (Nathalis 2 and Teleane 8), and guild 3, **Anarchy** (Baeil 26 and Urohke 68). Their saved positions were identical on map 1 at inspection, which is only a preflight hint; live proximity must be checked again. Nathalis held item GUID 603762, one Frostweave Cloth (entry 33470), at the read-only DB inspection. Baeil has Tailoring. These are existing characters and assets, so the trial is capped at that one item and 100 copper. No fixture item or money grant is part of the pilot.

## Proof gates

1. Baseline the exact four live character GUIDs: guild membership, position, alive/combat state, item GUID/owner/count, and both copper balances. Require one new immutable receipt ID. Reject stale or changed candidates.
2. Run one guarded native trade only if both bots are online, same faction, members of different actual guilds, near each other, alive, out of combat, and have the exact item, storage space, and copper. Refuse any retry after a completed or ambiguous attempt.
3. Settle the contract only if the server receipt identifies the exact item in the buyer's bag, absent from the seller, with seller copper +100 and buyer copper -100. Recheck saved character DB after autosave. A successful bridge response alone does not prove settlement.
4. Measure whether the buyer can craft something useful with ordinary recipe/reagent checks. A missing recipe or reagent is a measured blocked state, not a crafted potion or material chain.
5. Compare scout progress and incident time over a bounded run: saved rewarded quests, live level/XP, role XP per hour, copper and material deltas, death and repeated-route incidents. Do not call a short period with no alarm smooth play.

The in-memory contract ledger tracks **attribution of participating character assets**. It is not a physical WoW guild bank. The optional Jev advisor may choose only from controller-supplied strategic options at milestones; its output has no direct gameplay authority. The default comparator is deterministic and offline. A live Jev call requires an available server-side key and records its usage/cost.

## Faction treasury after the first loop

If trade and progression stay healthy, add a faction treasury as a separate, explicit account. A territory can pay at most once per verified control interval. Each credit should name the territory, controller, source, rate, cap, and interval; guild disbursements should debit the treasury and credit actual guild or character wallets through a verified server operation. Copper minted by territory income is a **new game faucet**, so track its total separately from player-to-player trades and tune it against repair, vendor, and other sinks. Losing a territory stops future income; it does not erase earned guild assets. Guild-bank deposits/withdrawals need their own native receipts before the treasury can be described as a real guild bank.

Jev should decide strategic allocation (supply, craft, escort, hold) at contract or territory milestones. Deterministic rules keep bot assignments exclusive and perform immediate movement/combat/trade. Compare Jev against the same deterministic baseline on completed contracts, delivery time, XP/hour by role, stranded materials, copper flow, stuck time, API spend, and CPU cost.
