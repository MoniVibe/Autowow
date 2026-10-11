# Discoveries and workshop knowledge - initial offline source

`KnowledgePolicy.h` prepares immutable eligible intents for an admitted dungeon-completion or quest-milestone
profile. It does not persist or grant knowledge. No discovery hook or catalogue is registered; no native recipe,
skill, XP, item, money, attribute, profession or movement operation is performed.

## Implemented eligibility source

- Confirmed completion, qualified participation, entitlement and validated catalogue are all required.
- Probe/admin events, zero identities, invalid enum values and mismatched source/scope/family refuse.
- A character scope must equal the contributor. Guild identity is nonzero uint32; faction identity is the internal
  KnowledgeFaction Alliance/Horde, explicitly separate from native TeamId and faction-template IDs.
- The resulting intent retains eventId, contributor identity, source type/family, entitled scope and knowledgeId.
- An intent is a copy of captured facts. Later guild changes cannot rewrite that copy. Repository arbitration is
  still required to refuse resubmitting the same earned event with different scope facts.

The pure policy trusts already-qualified scalar facts supplied by a future adapter. It cannot infer a clear or
membership from an instance map, a guild name, boss loot, a quest item or current character level. Test fixture
numbers are not an admitted catalogue. Eligibility does not mean settlement succeeded.

## Durable settlement contract

Permanent unlock identity is `(scope_kind, scope_id, knowledge_id)`. Its immutable original event/contributor/source
must remain when a later eligible contributor earns the same knowledge. Retrying a settled operation returns its
recorded result, rather than replacing the original provenance.

An event-term key binds `(source_kind, source_family_id, event_id, contributor_guid, knowledge_id)` to one captured
scope. Otherwise moving guilds would let the same captured event grant again elsewhere. Distinct contributors in
one dungeon can have independent entitled scopes; this rule must not confuse them with a conflicting replay.
Knowledge identifiers have one stable scope type in the admitted catalogue.

Grant and receipt commit together. Asynchronous acknowledgement occurs only after committed state is known.
Failure and unknown state cannot enable a workshop; retry queries authoritative records. Concurrent first-earn
requests serialize through unique constraints and transaction locking. No in-memory substitute is selected.
Raw Player pointers never leave callbacks; scalar responses are marshalled back to the proper owning thread.

`KnowledgeRepository.h/.cpp` contains an unbuilt asynchronous backend using the selected core's existing dirty,
unbuilt ModuleDatabasePool extension. Private prepared statements target an explicitly admitted characters schema;
the closed global CharacterDatabase enum is untouched. Initialization refuses zero async/sync connection counts;
zero async would otherwise fall back to blocking SQL. StrategyScripts instantiates no repository and opens no DB.
Initialization also requires transactionApiAdmitted, default false. The selected core transaction path ignores
failed START TRANSACTION and COMMIT results; this private pool instead queues its own checked operation on the
existing native worker. It checks BEGIN, each prepared statement and COMMIT on the same session, explicitly
disables client reconnect and refuses statement-level replay. Body failure attempts checked rollback; uncertain
BEGIN/COMMIT or failed rollback closes the session without calling worker-joining Close on its own thread.
No native Ping is queued for a quarantined session. Owner-thread Close/Initialize is required for re-admission.
The server-wide database behavior and pending supported-update tuple are unchanged.

Each private connection owns a separate description, initialized before the native connection base and
destroyed after it. The selected native socket Open changes host from dot to localhost through its reference;
sharing the pool description could otherwise send a later connection down the TCP branch. The private copy
keeps socket normalization local, preserves sibling/source credentials and survives the pool source description.
Actual native constructor/destructor copy checks and no-query DatabaseWorker execution/joins passed without Open.
This is not positive socket-connect, query/transaction lifetime or real pool-close proof; those remain gates.

Six pure sequencing-test families and C++20 syntax compilation against the selected native headers passed.
Eight separate task/actual-native-queue scenarios passed: success/at-most-once dispatch; false result;
execution exception; quarantine exception; empty work; native cancellation/reset; graceful queued shutdown;
deletion before dispatch. They execute the shared CheckedAsyncOperation and selected core PCQueue/SQLOperation
headers with no real connection. This does not prove DatabaseWorker/pool closure or native DB lifetimes.
Admission remains false until
isolated BEGIN/body/rollback/COMMIT failures, lost acknowledgements, cancellation/close, callback lifetime and
concurrent immutable-key settlement are tested against the real pool and database. Failed or unknown commit
must never grant knowledge; a fresh admitted owner can reconcile the exact immutable receipt terms.
No pool or database was started, no schema applied, and no atomicity/runtime PASS is claimed.

One transaction claims immutable receipt terms, inserts first-earned knowledge without overwriting provenance,
and settles the receipt only when stored scope matches. A successful transaction callback is insufficient:
an aggregate read must confirm the committed receipt and full original source/event/contributor identity.
Null query, missing/pending/inconsistent receipt or shutdown is Failed, never a speculative unlock. An exact retry
returns its recorded Granted classification without awarding anything again. Consumers must not trigger rewards
merely because classification is Granted; downstream commission/delivery needs its own durable receipt.

All methods and nonthrowing callbacks belong to one infrastructure owner thread. Poll dispatches callbacks after
internal iteration; explicit Close reports outstanding requests as Failed. Destruction suppresses external
callbacks. Initialize/Close can block and must never run in map/gameplay callbacks. Failed acknowledgement can
hide a commit; retry the exact captured terms. No Player/Unit pointers may be captured by handlers. The pending
DDL is source only; compilation and isolated transaction/concurrency/restart tests remain deferred. Receipt reads
use COUNT plus plain MAX(column), preserving native column metadata; SQL NULL maps to zero in native Field::Get.
COALESCE with a signed fallback is avoided because widened expression metadata can break raw prepared decoding.

## Native event and production gates

Quest item rewards and before-completion hooks are not universal successful quest receipts. Dungeon death is not
always encounter completion. One precise rewarded quest and one completed dungeon family must each bind native
completed state, instance/save/event identity and entitled participants before enabling ingestion.

The source audit qualified The Nexus/Keristrasza as a later kill-completion pilot: capture eligible native reward
participants through OnPlayerRewardKillRewarder, then require the exact updated OnAfterUpdateEncounterState
final-completion event for map576/entry26723. Map player enumeration, optional boss order and a death alone do not
qualify participants. Bind victim, difficulty, instance save and reset epoch; instanceId alone can be reused.
Capture scalar event-time membership and expire abandoned/reset captures. These hooks are not wired here.

For a separately catalogued nonrepeatable quest, OnPlayerCompleteQuest runs after RewardQuest sets rewarded state.
Verify QUEST_STATUS_REWARDED and capture entitled scalars there. Queued SaveToDB is not a commit acknowledgement.
A stable namespaced character/quest event identity still needs implementation; repeatables are outside this pilot.
Exact source evidence is retained in knowledge-native-seam-sol.md.

Consumers must require durable acknowledged knowledge, currently trained skill, admitted native recipe, reserved
real inputs and an actual city craft. Knowledge does not fill missing stock, teach arbitrary recipes or raise
native profession rank. Workshop output/delivery/payment/equip are separate receipts, counted once per good.

`KnowledgePolicyTests.cpp` and `KnowledgeRepositoryTests.cpp` are source prepared for the later harness. The latter
tests settlement classification only, not a database. Neither has been compiled or run in this memory-deferred turn.
KnowledgeRepositoryLifecycleTests.cpp adds unrun actual-API refusal/shutdown cases that never admit a connection.
The entire component remains unlinked and default-off; existing timing/PvP observers are unchanged.
