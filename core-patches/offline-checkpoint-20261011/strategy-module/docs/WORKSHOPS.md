# City workshops, batches and staged commissions

The user approved meaningful production time, batch orders and staged legendary work. Craftsmen remain in city
workshops, supplying adventurers and gatherers. This slice prepares pure source policy and regression sources.
It does not install a scheduler, persist an order, reserve native goods, cast a recipe or grant progression.

## Production experience

A representative submits one order for ten breastplates. The workshop quotes total inputs, price and estimated
work before acceptance. The order enters one artisan's durable FIFO queue. Only one piece is in production at a
time; ten finished pieces require ten separate native crafts. Finished pieces can ship progressively or wait for
a consolidated delivery. One shipment is not another production event. Buyers see completed quantity, remaining
quantity, current stage, missing inputs and an estimate based on actual workshop availability.

Initial balancing direction, not admitted live timing: ordinary equipment takes minutes per batch, masterworks
take tens of minutes through several stages, and legendaries take several workshop hours. Duration is selected
per recipe/design and captured with a version at acceptance. No production timing catalogue is currently admitted.
Basic weapons and armor must continue flowing while exceptional projects occupy workshop capacity. Buyers may
choose another artisan; cancellation or reassignment cannot silently erase consumed work or duplicate reservations.

Legendary stages are preparation, forging/weaving, tempering, specialty finishing and inspection. These are
work stages, not five free craft outputs or five extra native skill awards. A five-stage route can use another
profession's fitting names while retaining the same scheduling contract. Masterwork routes have two to five
stages; routine pieces have one. Intermediate native subcomponent recipes, if later selected, need their own
material-consuming child orders and receipts; the first policy does not fabricate their outputs.

## Time, interruptions and native progression

Only continuously observed eligible workshop activity counts. The runtime must verify the exact autonomous
artisan, city/station presence, alive/safe state, recipe and current skill, reserved inputs, and current control
and motion custody. No wall-clock catch-up on login, server outage, departure, combat, pause or uncertain ownership.
Progress already durably accepted resumes after recovery. No restart restarts a finished stage or awards an item.
The adapter resets its observation epoch after restoration and allocates fresh durable per-order slice identities.

One observed slice credits at most the current stage; excess time is discarded rather than banked into later
stages or pieces. This conservative first policy avoids a delayed callback manufacturing an entire batch.
Paused slices are checkpointed without work credit, so they cannot be replayed later as productive time.
Runtime scheduling must use native EventMap/TaskScheduler or an admitted world scheduler; the policy has no sleep
loop or custom bot tick driver. Timers never mutate goods, skills or XP.

At the final stage the piece becomes ready for one actual native craft. Recheck custody, station, recipe, skill,
reagents, output room and attempt ownership immediately before dispatch. Native reagent consumption and native
output remain authoritative. Native profession gains happen through that craft; a finished timer does not add
skill. Recipe difficulty can stop skill gains. Character-level/rank requirements remain enforced. City-based
artisan career advancement and apprenticeships require separate admitted progression rules; these orders do not
bypass training or raise character level. Masterwork skill is captured at actual craft, not at order submission.

## Durable queue and settlement contract

The first pure kernel returns proposals. Every accepted proposal must be committed before being treated as state.
An implementation needs these durable records and constraints before activation:

- Immutable order: order ID, requester and beneficiary, artisan/workshop, recipe/design, timing profile/version
  and exact durations, quantity, captured entitlement, quoted price, material reservation IDs and delivery terms.
- Queue ownership: one active order/attempt per artisan, FIFO sequence and reservation; no shared order in two
  workshops. Reassignment requires qualified revocation and a new ownership revision, preserving progress/history.
- Cursor: order ID, revision, completed pieces, stage index, credited stage milliseconds and last slice ID.
  Compare-and-swap against the previous revision; an old snapshot cannot overwrite newer work.
- Piece attempt: unique order/unit ordinal, stable attempt identity, ready revision, cast/recipe correlation and
  dispatch status. Reserve before dispatch; further work slices must not alter a revision with an in-flight cast.
- Authoritative completion: exact order/unit, native craft event, item GUID, recipe, maker, actual output quantity,
  consumed reagent identities, captured skill/specialty and committed result. Unique craft event and output identity
  prevent reusing the same gear for another piece/order. One callback replay returns its original settlement.
- Shipment/payment: exact produced item identities and separate delivery/payment receipts. Moving the same goods
  through inventory, mail and bank never counts as additional production, donation or mastery.

Reserve the batch's actual ingredients without duplicating native stock or exposing reserved goods to another
order. Physical escrow/native inventory handoff needs its own atomic/reconciled protocol. Release only unused
reservation on cancellation. Existing completed goods remain goods; work on the current piece follows the quoted
cancellation terms. Unknown native cast outcome stays held until exact input/output reconciliation. Never retry
an uncertain craft based on a timeout, cast-start line, queued SQL save or missing acknowledgement alone.

`WorkshopPolicy.h` accepts validated immutable timing terms and proposes sequential cursor transitions. It admits
one output per piece, checks identities/revisions/ordinal, requires a committed native completion receipt and
never generates a receipt itself. The boolean attestations are adapter contracts, not proof supplied by this header.
The trusted repository must bind the profile version to its immutable duration payload and reject changed terms.
Likewise, global output/event uniqueness and queue exclusion belong to durable settlement, not the local cursor.

No repository/schema implementation is added here. The discovery backend's transaction admission remains false:
selected core BEGIN/COMMIT error handling and reconnect atomicity need repair and isolated tests before any durable
workshop settlement. A filesystem cursor or in-memory cache is not an adequate substitute for native stock custody.

## Native adapter and release sequence

The existing item-created hook lacks sufficient recipe/cast/self-craft context. A qualified context-bearing seam
must correlate exact craft attempt, successful native reagent consumption, item creation and skill progression.
First pilot one ordinary unstackable gear recipe and a two-piece order, then restart/reconcile each stage and
uncertain cast boundary. Observe inputs, two distinct pieces, native skill results, delivery/payment and equip.
Only then admit a five-stage legendary route with real rare inputs and its separate maker/mastery settlement.

This source slice has no live hook, queue, persistence, configuration switch, production timing, teaching action,
legendary recipe or buff. Regression sources cover timing, pause/replay, ten independent pieces, stage sequencing,
excess-time containment, receipt mismatches and invalid/overflow states; C++ compilation/execution is deferred.
