# Guild contribution and territorial campaigns

The user's direction is a strategic economy with real reasons to cooperate and fight. Member contributions feed
workshops; equipped adventurers protect and contest the economy; territory income belongs to its current owner.
City artisans remain producers, not expedition members. This document defines the next gameplay slices; it does
not claim a deployed capture objective, raid system, standing ledger or native bank permission change.

## Guild governance

All members can view and deposit into a shared intake tab, subject to actual native item/bank requirements. Only
designated representatives withdraw, move shared stock, spend guild money or approve outgoing deals. Representatives
fulfil equipment requests for ordinary members through accountable native deliveries. Receiving commissioned gear
does not require granting the recipient unrestricted withdrawal rights.

For society-managed guilds, provision member ranks with native view/deposit rights, zero item withdrawals, zero money
withdrawals and no bank-funded repair right. Representative ranks receive deliberate tab and daily budget limits.
The native guild master retains full rights and is part of the representative governance role. This is not a plan
to demote, replace or seize an existing human guild master. Apply setup only to the configured society guilds, with
current authority and exact guild/rank identities; no new consent registry is needed.

The inspected native implementation permits same-tab rearrangement without a withdrawal check. Native rank settings
alone therefore do not enforce representative-only stock movement. The future integration must guard applicable
bank operations before mutation, including swaps, repairs and deal authorization. The current `GuildPolicy.h`
predicate expresses that restriction but is not connected to an enforcement hook. Outsiders are refused even if
an old representative designation remains. Representatives still need fresh native rights and stock/capacity checks.

## Contribution standing

A confirmed useful donation gives the original contributor guild standing, separate from native faction reputation,
honor, character XP and guild rank. The source policy currently returns eligible units only; weights and awards
remain to implement. One donation of 50 ore against a shortage of 20 can recognize 20 units; the other 30 may remain
bank stock without immediate standing. Give members clear requests so they can supply current workshop shortages.

Count personal stock transferred into the guild once. It may have been naturally gathered, crafted or legitimately
purchased by its owner. Do not credit faction allocations as personal generosity, a representative forwarding guild
stock, repeated deposit/withdraw loops or goods the guild already paid for. Paid commissions can have a separately
defined work-standing event; they cannot also masquerade as unpaid donations of the same goods.

Recognition binds the original contributor, native item/money receipt, source provenance, guild membership at the
event and fulfilled demand version. Deposit requests and pre-transfer callbacks are not receipt proof. Concurrent
donations cannot both consume the same demand. Return, loss or annulled delivery must reconcile the original award;
a guild-funded recipient cannot redonate the item for another award. Ledger evidence persists across relog/restart.

Standing should initially provide public recognition, contributor titles and a queue tie-break between equally
urgent requests. Useful role/level equipment need outranks prestige; newcomers still receive essential equipment.
Standing does not automatically grant representative rank, withdraw permissions or immunity from budget checks.
Set weights from useful item demand and cost rather than raw item count, so cheap filler is not the best strategy.

## Three strategic PvP actions

1. **Depot raid:** attack a configured frontier supply depot and seize a bounded part of its existing, unreserved
   regional stock. Attackers need to finish the objective and extract the shipment. Defenders can interrupt and
   recover it. A raid moves existing supplies; kills do not mint materials and city guild bank contents are not
   automatically exposed. A cooldown and a retained reserve prevent repeatedly emptying the economy.
2. **Territory capture:** contest and hold a configured objective, then establish control through a verified capture
   event. Contesting pauses that objective's new income. Successful ownership changes redirect future regional
   production to the new faction treasury; settle the old owner's completed periods before the transition. Holding
   long enough for income is valuable, while instant alternating captures cannot replay or multiply income.
3. **Supply escort/interception:** guilds allocate a bounded existing shipment to a frontier depot or workshop.
   Defenders earn objective standing/glory for its actual arrival; attackers can contest its extraction. No forced
   arrivals or teleport transport. This comes after the durable ordinary delivery path works.

Objective success, verified defense and supply recovery should matter more than farming individual kills. Glory is
prestige; territory stock is economy. Credit actual eligible participants against objective events, not everyone
nearby or every member of the winning faction. Existing native honor stays separate. No arranged reciprocal farming,
spawned victims, guaranteed fights or rewards from friendly/grey/no-credit kills.

## Ownership and investment

Ownership is by faction; a designated guild can administer a garrison under the faction's budget. Overlords choose
allocations and regional upgrades. Purchased industry belongs to the territory, so capture can take productive land
and losing it has an economic consequence. Improvement costs come from conserved stock. Construction takes time;
capture cannot refund upgrades and also retain their benefits. Configuration must define what happens to unfinished
work and reserved shipments; disputed ownership never authorizes two treasuries to collect the same period.

Begin with one bounded frontier objective per faction and a validated local resource basket. The earlier Redridge
counts were illustrative. Rates must support useful recipes without drowning out gathering, professions or markets.
Capital specializations can later provide industry bonuses with explicit costs, ownership and native recipe/rank
constraints. Capital capture is a later campaign feature, not an inference from a bot crossing a city's boundary.

## Delivery sequence

Mercenary guilds or available adventuring/raiding detachments can supply funded escorts and bodyguards, retaining
their home membership and earned equipment. Skilled city makers can supply individually signed masterwork gear.
See `MERCENARIES_MASTERWORKS.md` for contracts, skill-dependent affixes and exact native implementation boundaries.

First implement durable accounts, representative enforcement and donation receipts. Prove one allocation and one
useful city commission through actual delivery/craft/equip, then contribution standing. Next add a real configured
capture objective, owner transition and income reconciliation. Depot raids follow once reserved stock and extraction
have correct crash/replay recovery; escort gameplay can reuse that shipment path afterward.

Region control, depot loot and PvP glory require new native event integration and meaningful tests; none is wired by
this source slice. Keep the existing server and supported-update work intact. Offline source development remains
the current scope; compilation, release and living gameplay proof are separate gates.

PvE raids also have a separate legacy direction: a full first clear unlocks society industrial knowledge, while
each boss can grant permanent personal veteran power once. See `BOSS_LEGACIES.md` for the deliberately extravagant
end-boss profile, participation and durable uniqueness rules. These are not repeat depot extraction rewards.
