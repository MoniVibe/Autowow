# Faction treasuries, guild banks and deals

Each faction owns its treasury; each guild owns its stock and money. An Overlord allocates faction resources,
while guild authority decides its orders and exchanges. Guild membership does not give a character unrestricted
access to that guild's bank. Existing native ranks, bank tabs, withdrawal limits and inventory rules still apply.

The chosen society rule is member deposits into shared intake, representative-only withdrawals, stock movement and
spending. Native guild-master authority remains; the master is a representative in the managed society model.
`GuildPolicy.h` proposes an additional restriction, without bypassing native permissions. It is not active
enforcement. See `CAMPAIGNS.md` for rank setup, useful contribution standing, depot raids and territorial capture.

## Native source prepared

`BankPolicy.h` names an owner by faction and owner kind, with the actual guild ID for guild accounts. Each balance
names one validated item entry or copper, and distinguishes available stock from outgoing reservations. Capacity
includes both, so cancellation can always return its own reservation without overflowing that account.

`PlanBankTransfer` proposes a one-way grant or transfer between different owners in the same faction. Both sides
must be authorized; source stock and destination room are checked before either proposed balance changes. It can
allocate treasury resources to a workshop guild or transfer surplus between guilds. This is not an item grant API.

`PlanBankDeal` proposes a two-sided exchange between different owners: an item for copper, or one item for another.
It has proposed, reserved, settled and cancelled states. Reservation locks both outgoing legs together. Settlement
rechecks authority, expiry and destination capacity, then transfers both legs together. Cancellation releases only
that deal's holds; other reservations remain untouched. An expired deal cannot settle, but an authorized cancellation
can release it. A changed destination can block settlement without destroying the reserved goods or payment.

One contract has two distinct asset types and four distinct owner/asset balance keys. Same-asset circular deals and
cross-faction deals are refused in this first slice. Multiple goods, partial contract fills and treaty trade are
later features; a physical delivery can still have multiple parcels after a complete accounting settlement.

The functions return proposed copies only. They do not load accounts, authorize a caller, persist transactions,
create items, move money, send mail or invoke native guild banks. A nonzero deal ID is necessary, but only the durable
repository can enforce its uniqueness, own its reservations and reject a replay of an older snapshot.

## One authoritative holding

Reuse native guild banks for physical items and copper. The selected core already stores guild bank item GUIDs,
tabs, rights, money and event logs. Its handlers require bank-object interaction; item movement uses native bank
and inventory validation. Money withdrawal also checks native rank rights and daily allowances. Do not call these
methods as a shortcut around their session, interaction and permission preconditions.

Faction territorial production initially belongs in a custom, unmaterialized treasury. A guild can also own an
unmaterialized allocation there, awaiting physical delivery. These quantities are separate from native bank items:
materialization consumes or irrevocably locks the virtual quantity before one exact physical item is created.
The native holding then becomes authoritative. A display or index of that item is not another spendable balance.

`TreasuryPolicy.h` is an aggregate conservation projection over the same owner records. It must not become a second
independently mutable ledger. Produced, allocated, in-transit, consumed and invested quantities reconcile against
the original event; forwarding through representatives or mail does not create additional production or donation.

The first item-entry balances cover fungible raw materials. Physical gear requires exact item GUID, count, binding,
enchant/random-property and location custody; counting gear by item entry alone cannot qualify a trade. Native
capacity is a slot/stack/template constraint as well as an accounting bound. The adapter must validate both.

## Durable adapter to implement next

Use module-owned prepared statements and asynchronous work, keeping synchronous SQL off map threads. Persist:

- Account identity, asset identity, available/reserved quantities and a version.
- Immutable deal terms: parties, offered/requested assets and quantities, expiry, approvals and a terms hash.
- Deal state/version and two reservations uniquely owned by that deal.
- Unique operation receipts with source provenance and before/after versions.
- Physical delivery intents and acknowledgements, keyed to the original transfer and exact item/money identity.

Reserve, settle or cancel in a single transaction that checks expected account/deal versions, deal-owned holds and
fresh authority. Commit all affected rows or none. The same event retry returns its prior result rather than applying
its amounts again. Approval binds exact terms; an edited price or quantity requires fresh approval. A removed guild
member cannot authorize another withdrawal. Guild disband and unresolved delivery require explicit recovery states.

Ledger settlement is not mailbox receipt or retained equip. A delivery worker must preserve reservations while it
crosses a native operation boundary, confirm that operation's outcome and reconcile after a crash. Do not pretend
an ordinary SQL transaction makes the in-memory native guild state and custom ledger atomically interchangeable.
If native physical stock cannot be safely reserved against ordinary withdrawals, refuse that trade until a proper
reservation integration exists. No bypass of native bank ownership or withdrawal paths is authorized by this policy.

The native `OnGuildMemberDepositMoney` callback in the inspected core is before the money transfer. Its occurrence
is not a successful deposit receipt. Item movement callbacks likewise need their exact outcome qualification before
they can credit an account. Native bank logs, persistent state and the same operation identity must reconcile.

## Productive use

Territory income enters the faction account, distinct from native gathered loot. The Overlord reserves allocations
for guild shortages. A city workshop uses its own input stock, produces native goods and advertises surplus. An
adventuring guild can reserve copper for a weapon commission; a gathering guild can exchange ore for consumables.
Payment, material receipt, craft, delivery and equip remain separate stages of the same goods, counted once.

Contribution standing can later credit useful net fulfilled donations or commissions once, against actual demand.
The eligibility policy now recognizes confirmed original unpaid donations, capped by remaining demand; it does not
award points. Bank deposits and forwarding alone do not grant native reputation, guild rank, XP or standing. Pricing
and regional income rates remain deliberate economic policy; this bank slice introduces no income faucet or reward.

## Evidence and release

The source and prepared native tests are staged outside the active core build graph. Compilation, persistence,
physical bank integration and native trade observation are pending. Do not enable bank mutation through the current
observation-only scripts. Preserve the supported update and live server; Galaxy resource priority remains in force.

Native acceptance: confirmed source stock and authority -> durable reservation -> atomic accounting exchange ->
exact physical handoff -> receiver pickup/bank receipt -> useful craft or retained equip. Test cancellation, changed
capacity, revocation, concurrent orders, restart/replay and native withdrawal contention before enabling mutations.
