# Supply chain plan

## Gatherer squad material crews

`AutoWow.Squad.MaterialCrews` is a default-off scheduling split. Off preserves the legacy Alliance and Horde
rosters as one squad per faction. On, the eight optional faction/material-family keys define exclusive Cloth,
Herb, Ore, and Leather crews. The whole layout is validated before activation; empty crews are valid, while a
malformed range, zero GUID, duplicate membership, more than five members in one crew, or more than ten for one
faction rejects
the layout. There is no fallback to the legacy rosters after an invalid opt-in request.

Each active crew owns its party, stint state, hold, benches, anchor cooldowns, and material-family demand. Public
team roster callers see the stable sorted union. A character keeps every learned profession; primary crew
membership only selects the demand family it works. The immutable source index and existing Party ownership are
shared. Stint IDs remain process-global and fail closed at exhaustion instead of wrapping.

The first intended profile uses the existing ten gatherers: three miners plus two herbalists per faction. Cloth
and Leather crews remain empty until qualified members exist. Native survival and effectiveness are runtime proof
gates; the configuration does not invent roles, change skills/specs, alter danger thresholds, or add characters.



### Durability and ownership contract

Crew identity is a fixed, bounded slot: faction first, then the append-only numeric `Kind` value. Configured GUIDs
are sorted before use and a GUID can occupy only one slot, so demand, receipts, and native-party decisions have a
deterministic order. Crew state remains process-local under the existing state version (`kStateVersion = 2`); public
snapshots copy one complete crew state while holding the squad lock. No persisted snapshot or wire schema changed.

Stint IDs are process-global, never reset by config reload, never reused, and fail closed at `uint32` exhaustion.
The immutable source index is published only after the entire layout validates. Runtime decisions use numeric team,
kind, item, spawn, and GUID values; strings are confined to config keys, diagnostics, and receipts.

The old faction roster is migration provenance only. A nonempty old roster must exactly equal the union of at least
two disjoint successor crews. Before a native-group handoff can defer, Party records the validated GUID union in a
bounded pending set under its lock. That pending set alone owns the orphan-cleanup exclusion; permanent squad
membership still excludes formation and recruitment but does not suppress ordinary orphan recovery. The pending
set clears on Party config reload and on every successful or already-complete handoff. No successor stint or party
mutation begins while a partial, foreign, special, controlled, or otherwise unsafe legacy group remains.
