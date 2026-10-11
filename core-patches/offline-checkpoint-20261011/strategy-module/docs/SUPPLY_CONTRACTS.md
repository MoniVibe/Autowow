# Supply contracts and physical caravans

The first source slice implements conserved contract/payment and shipment-state proposals. It is unlinked and
inactive. Pure C++ tests run independently; no treasury, bank, item, payment, standing, actor, route or world mutation
is enabled. Existing prepared recipes, banks and workshops remain separate from native integration.

## Useful demand and contract terms

A representative requests a specific admitted item/profile and quantity for an actual guild shortage: six swords,
twelve role-appropriate gems or forty healing potions. Each item/profile is one contract row. A compound expedition
order can group several rows without sharing their accounting or counting every mail stage as another commission.
The first policy uses distinct same-faction guild buyer/supplier accounts. Faction allocations enter guild accounts
through the existing treasury/bank path, not through a new implicit resource faucet.

Capture immutable contract ID/version, buyer/supplier guilds, actual item entry, validated quality/role profile,
quantity, unit price and delivery deadline. The native profile must specify recipe/source, wearer/class/role, useful
stats, binding/trade eligibility and destination demand. Item level/quality color alone is insufficient. No default
price is invented. A paid commission is not an unpaid donation and does not automatically award reputation, standing,
guild rank, glory, XP or skill. Future work-standing can have a distinct once-earned receipt and deliberate weights.

## Conserved payment

`PlanSupplyFund` moves total price from available to reserved copper in the buyer's existing balance. The contract
cursor owns exactly that hold. Unrelated reserved funds stay untouched. Valid state requires remaining hold equal to
(requested units minus delivered units) times the captured unit price. Arithmetic/capacity/authority/version checks
refuse malformed, stale, foreign, overflowing or unfunded requests.

Before any physical handoff, `PlanSupplyDispatch` registers one exact held stock/shipment intent and quantity.
This intent is required even for an ordinary bank/mail/local delivery that does not use a caravan. The adapter must
atomically persist the hold/intent before releasing original native goods; a successful cast or queued SQL call is
not that boundary. One contract can have one unresolved shipment at a time. It can dispatch several in sequence.

`PlanSupplyDeliver` requires the exact intent plus verified original native receipt, unique claim, accepted profile,
receiver custody and recorded native received time. It transfers only accepted units times unit price to supplier
copper, leaving unpaid units and other reservations intact. Partial batches are allowed. Payment can settle later
than the deadline when the authoritative receipt proves goods were received on time. Late or future-dated receipts
do not pass; do not substitute the later audit/query time for the native receipt time.

Unique item/stack claims and contract/account compare-and-swap are mandatory durable adapter checks. The pure policy
consumes already verified facts; its local sequence/revision checks cannot replace a persistent global native claim
lookup. No client is allowed to set these booleans. Existing custom repository transaction admission remains false
until checked commit/error/reconnect semantics and isolated failure tests qualify; that blocker keeps all mutations off.

## Cancellation, loss and recovery

Cancellation or expiry closes only unfulfilled demand and returns this contract's remaining copper. It refuses while
physical stock is in transit or a handoff outcome is unknown. Do not refund both money and goods while an earlier
handoff can still complete. Representative authority is rechecked at every action; supplier authority is required for
dispatch, receipt and cargo resolution. Revocation produces a held/recovery case, not an invented successful delivery.

`PlanSupplyResolveCargo` clears the outstanding intent only after the exact original native return or loss has been
durably reconciled. It pays nothing and creates no replacement items. Remaining demand may be supplied again before
the deadline; otherwise close it after cargo resolution. A carrier death, timeout, relog or proximity is not loss proof.
Already delivered goods/payments remain delivered; cancel does not claw back or award them twice.

The native adapter must preserve in-flight intent across disband, actor logout, server restart, changed capacity and
delivery failure. Unknown custody stays held. No background factory/free-maintenance/reset gear or probe fixture may
fulfill a funded society contract as earned supply.

## Physical manifest and journey

`CaravanPolicy.h` binds shipment/contract/version, source/destination owners, exact carrier, configured route/version
and original cargo. Each lot has original physical item/stack GUID, entry, count and a 32-byte metadata fingerprint.
Native evidence must verify the underlying canonical properties and legal ownership/binding transitions; the digest
alone is not proof. Same-template equipment pieces have distinct GUIDs. Duplicate GUIDs, zero quantities, changed
terms, foreign owners and overflow refuse. The first kernel bounds one manifest at 64 physical lots; split larger
shipments into accountable batches. This is a processing bound, not a production or progression cap.

Reserve the existing physical goods against ordinary withdrawal/consumption before starting. Displayed cargo is an
index of those goods, not a second spendable bank or a newly created item stack. Route start needs authoritative
origin evidence, autonomous protected carrier/current motion custody and current party/route authority. A proposed
cursor does not move the actor, install a new goal or seize newer/foreign motion.

Sequential native checkpoint observations advance `Travelling` to `ReadyToUnload`. No clock, interpolation, distance
estimate or teleport contamination can fill missing checkpoints. Halting preserves the checkpoint and stock identity;
resuming rechecks current actor/motion and cargo instead of replaying an old move. Route completion is not delivery.

`Delivered` requires exact physical receiver handoff of the whole frozen manifest. `Returned` requires exact origin
custody; `Lost` requires authoritative loss of that manifest, not a fight/death count. Terminal states cannot replay.
The first caravan kernel does not classify mixed partial-delivered/partial-lost manifests: those remain recovery-held
until a later per-lot disposition adapter qualifies. Contract partial-payment proposals remain useful across separately
acknowledged native lots/shipments. Do not falsely label a mixed shipment fully delivered, returned or lost.

## Next live integration, deliberately separate

1. Repair/admit durable transactions and native bank reservation/representative enforcement; preserve physical truth.
2. Resolve a useful current demand/profile and qualified city maker/source. Protect actors against random factory,
   free maintenance, reroll and human/paused/mastered control paths before assigning autonomous duty.
3. Persist terms, stock holds, shipment intents, account versions and once-only original native receipts.
4. Connect ordinary local/mail/bank receipt and paid settlement. Verify pickup/use or retained equip separately.
5. Configure one ordinary physical supply route and protected carrier; observe actual start/checkpoints/handoff.
6. Connect existing guild escort/mercenary contracts to safe autonomous travel and normal combat. Escorts do not mint
   cargo or guarantee victory. Interception/extraction, standing/glory awards and regional crises require later slices.

Native acceptance: useful demand → reserved real funds → original stock/craft → exact held manifest → observed route
→ real receiver custody → paid settlement → useful consumption/retained equipment. Count source, shipment, mail,
payment and equip as stages of the same goods. Report absent routes/actors/recipes/resources explicitly.

## Evidence

Bounded isolated GCC13/C++17 assertion suites exercise partial conservation, another reservation, transit resolution,
deadline/native receipt timing, overflow, stale/replayed facts, exact manifest/route identity, ordered checkpoints,
halt/resume and whole-manifest native disposition. Existing BankPolicy regressions pass separately. Preserve the
initial missing-header RED and subsequent native-intent/origin-evidence RED; final pure passes are not native tests.

No server build, migrations, scheduler, repository integration, new actor, actual caravan, reward or live supply
contract has run. Galaxy resource priority and the supported-update/current runtime release gates remain in force.
