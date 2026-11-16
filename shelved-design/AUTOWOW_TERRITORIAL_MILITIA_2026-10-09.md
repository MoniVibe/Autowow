# Territorial NPC militia — first source slice

Implemented a C++17 campaign model in TerritorialArmyPolicy.h. Playerbots retain hero/profession roles; persistent
NPC troop identities belong to squads controlled through authorized overlord orders. Barracks provide capacity;
verified armory/equipment profiles affect campaign strength. Muster issues weapons/armor and transfers food from
exact available owner accounts; unrelated reservations stay intact. Engaged squads cannot reorder or resupply.

Battle mode locks before resolution. The strategic resolver models simultaneous attrition, wounded survivors and
permanent casualties; physical mode requires full native same-battle roster/objective receipts. Both final squads
and territory/version transition are a single proposed settlement. Draws retain the defending owner. Replays,
changed mode/owner, duplicate troop IDs and unverified or resurrected physical outcomes refuse.

The isolated pilot uses synthetic objectives100/200, route9 and two three-person militias. It is an executable
domain test, not a native encounter or current world state. A later native actor adapter must materialize each
surviving troop exactly once, preserving health, supplies and battle locks. Campaign points are prototype numbers;
no player stats, character XP, hero glory or boss traits are awarded.

Source remains outside the active build graph. No database, actor spawning/navigation/combat, capture income,
Playerbots source/control, supported-update source, live config, population, Git, restart or push is changed.
Checked durable bank/roster transactions, actual NPC templates/routes, physical observation and treasury owner-period
reconciliation are required before a live campaign. Specialist troops, medics, siege and hero leadership follow later.

Evidence and final source integration status are recorded in the task's exclusive army receipts. See
[TERRITORIAL_ARMIES.md](../core-patches/offline-checkpoint-20261011/strategy-module/docs/TERRITORIAL_ARMIES.md).


Final evidence: four isolated GCC13/C++17 executables PASS; fresh final source review PASS_SOURCE_SLICE_ONLY.
Root staged five selected paths (three new) into strategy-module and verified40 native fingerprints,
the exact final diff and retained preimages. All37 prior paths are retained; only README and the strategy plan
changed among them. Existing supported-update266 paths, S125 exe/argv/parents/start/hashes and configs stayed exact.
Receipts: army-pure-fix-green-r1.json, army-final-review-r1.md/.json, army-native-source-r1.json/.diff,
army-root-final-r1.json. Initial missing-header RED and corrupt-ownership-counter assertion RED remain preserved.
No full module/world build or live NPC campaign acceptance is claimed.
