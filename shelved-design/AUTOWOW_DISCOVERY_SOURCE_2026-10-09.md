# AutoWoW first discovery slice — source prepared

The offline strategic module now has discovery eligibility, settlement classification and an asynchronous
knowledge-repository draft. It preserves event-time character/guild/faction entitlement and refuses incomplete,
unverified, probe or malformed facts. Unique event receipts and per-scope knowledge preserve first provenance
and are designed to prevent duplicate unlocks or later guild changes from moving an earned discovery.

Actual source is staged at `strategy-module`, outside the build graph. The new schema is only a
pending file at `core-untracked/data/sql/updates/pending_db_characters/2026_10_09_00_autowow_knowledge.sql`.
No database migration, pool instance, native discovery hook, recipe, item, stat or workshop effect is active.

Review found a blocking native dependency: the selected core ignores transaction-start/commit errors and can
reconnect individual statements outside their transaction. Initialization has a separate default-false transaction
admission gate. This must stay false until the core semantics and isolated partial-write/reconnect tests qualify.
The backend is unbuilt preparation, not an atomic persistence PASS. Existing supported-update paths were preserved.

The receipt decoder was corrected to COUNT plus plain MAX columns; signed COALESCE fallbacks can widen native
prepared-result metadata. The source audit also qualified later Nexus/Keristrasza completion/participant hooks and
a post-reward nonrepeatable quest hook. They remain disconnected; no historical clear is fabricated.

Static checks passed for the exact eight reviewed source/test/DDL files: selected C++/SQL style categories,
prepared placeholder counts and disabled-instance inspection. Independent Sol high review passed source only
with the external transaction blocker. Three regression programs are prepared but **uncompiled and unrun**.
R10 serial integration retained R9 preimages; its receipt binds28 component/test files plus one pending migration.
Root reviewed the exact final diff. Live S125 executable/config/HEAD/index and all266 update paths stayed exact.

Evidence:

- `work/server-strategy-20261008/native-source-receipt-r10.json`
- `work/server-strategy-20261008/native-source-r9-to-r10.diff`
- `work/server-strategy-20261008/knowledge-source-checks-r2.json`
- `work/server-strategy-20261008/knowledge-final-review-sol.md/.json`
- `work/server-strategy-20261008/knowledge-native-seam-sol.md` (r1/r2 historical reports retained)
- `.agents/plans/strategic-discoveries/strategic-discoveries.EXECUTION.md`

When memory permits: qualify the transaction dependency, compile the prepared tests and validate the repository
against an isolated database, including duplicate concurrency, scope conflicts, failures and restart. Then wire
one dungeon/quest pilot. The later production chain remains earned discovery → workshop knowledge → funded
commission → real inputs → native city craft → payment/delivery/equip. This source turn performed no builds,
live SQL, configuration/control changes, commits, pushes or restarts. Galaxy work retains resource priority.
