# AutoWoW faction and guild banks — October 9

The native strategic module now has explicitly owned bank accounting proposals. Factions can allocate treasury
stock to guilds; guilds can transfer surplus, reserve purchases or barter with other groups in the same faction.
This is real C++ server source staged for later integration, not a browser economy simulation. Persistence and
native physical banking are still pending; no bank is activated by this change.

## Ownership and deals

`BankPolicy.h` distinguishes faction and guild owners, the guild's actual ID, item entries and copper. Available
stock and outgoing reservations are separate. Transfers require both authorities, matching assets, adequate stock
and destination room. A bilateral deal reserves both outgoing legs before either party receives anything, then
proposes complete settlement or authorized cancellation. Expired deals cannot settle. Cancellation returns its own
holds while preserving other reservations. Destination capacity and authority are checked again at settlement.

This first slice covers fungible materials and copper, one offered asset against one different requested asset.
Physical gear will require exact item GUID/property/location custody. Same-owner, same-asset circular and cross-faction
deals are refused. Treaty trade, multiple goods and partial contract fills are later work. Individual deliveries
are a separate state machine; accounting settlement is not delivery, pickup or retained equip.

For example, faction ore can be allocated to the smith guild; a gatherer guild can barter surplus ore for consumables;
an adventuring guild can reserve its money for workshop output. The policy grants no items or currency. Reservations
and proposals become authoritative only when the future repository validates identities, approvals, event uniqueness
and expected versions and commits all affected rows together.

## Native reuse and remaining adapter

Targeted inspection of the selected core confirmed existing native guild money, item/tab storage, rank rights,
withdrawal limits, native bank-object interaction and persistent event logs. Relevant seams include
`Guild::HandleMemberDepositMoney`, `Guild::HandleMemberWithdrawMoney`, `Guild::SwapItemsWithInventory`,
`Guild::MemberHasTabRights`, the guild bank session handlers and CharacterDatabase bank prepared statements.
These APIs keep their native permissions and actor/service preconditions; they are not administrative transfer
shortcuts. The deposit callback runs before transfer completion, so that hook alone cannot credit an account.

Faction production needs a custom durable treasury. Guild-owned allocations can remain unmaterialized until delivery;
physical goods and copper belong to native holdings once transferred. A ledger index of those holdings is not another
spendable copy. `TreasuryPolicy.h` is now explicitly an aggregate conservation projection of the same owner records.

The next adapter needs account/deal versions, immutable approved terms, unique deal-owned holds, operation receipts
and physical delivery intents/acknowledgements. SQL must use prepared transactions off map threads. A native operation
and a custom ledger cannot be assumed atomic merely because one SQL transaction exists. Crash/replay reconciliation
and reservation protection against ordinary withdrawals are required before physical trades can mutate stock.

Bank forwarding does not by itself award contribution standing, native reputation, guild rank, XP or payment.
Useful fulfilled donations/commissions can later feed the separate standing design once against real demand.

## Changed source and evidence

Actual source is `strategy-module`, outside the active core build graph. Host mirrors are under
`work/server-strategy-20261008/mod-autowow-strategy`; prepared tests are under that task's `tests` directory.

- Added `src/BankPolicy.h` and `tests/BankPolicyTests.cpp`.
- Added `docs/BANKS.md`, covering native ownership, persistence, materialization and delivery boundaries.
- Updated `src/TreasuryPolicy.h`, `README.md` and the strategic native implementation plan.

Root reviewed the exact six-path final source diff. `native-source-receipt-r4.json` pins all eleven component files,
and `r3-native-preimages` preserves changed prior files. The R3-to-R4 diff SHA is
`b8ca79052854db1b1478fab920fffc543074266f324f62108538fc5263a6a37d`.

Official AzerothCore C++ codestyle passed all nine checks on native `src`. Static wiring/source-hash, LF/120-column,
default-off, loader/API and no-live-bank-mutation checks passed. There are 61 prepared bank assertions, separate from
the existing 40 strategy assertions: owner/asset validity, authority, reserved stock, capacity/uint64 boundaries,
barter, complete settlement, refusal/expiry, cancellation, replayed state and corrupted snapshots. Neither native
test suite was compiled or executed. Static checks are not compilation, durable concurrency or native trade proof.

Before and after integration, exact world/auth executable/argv/parent/start/hash, three live configurations,
core/module HEAD/index and all 266 applied supported-update source identities matched. No active guest builder was
found. The new module remains unlinked; existing supported-update source and running S125 were preserved. Galaxy
resource priority remains. No compiler, heavy build/resource baseline, migration, SQL mutation, bot controls,
configuration activation, restart, commit, push or automation change occurred.

Next acceptance is confirmed stock/authority -> durable reservation -> atomic accounting exchange -> exact physical
handoff -> receiver bank/mail receipt -> useful city craft or retained equip. Concurrency, revocation, changed room,
restart/replay and native withdrawal contention must be exercised before enabling mutations.
