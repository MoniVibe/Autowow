# Offline AutoWoW checkpoint, 2026-10-11

The owner requested shutdown, a source push and cleanup. World/auth are stopped.
The supported Playerbots source is committed separately to the owner's main branch.
Core tracked.patch applies to 6cf9009f639b10f2dcc8983fd6f514de206137ba; copy the
core-untracked files in addition. The imported upstream targets and original
authors are recorded in the two provenance files. No official upstream is pushed.

The strategic module is an unintegrated source snapshot, outside the supported
CMake graph. Transaction admission remains false. Pending character knowledge
SQL must not be auto-applied. Draft storage is not a working crafting queue.

S128 worldserver built and linked; focused 159 tests passed as a subset of the
full 13,165 executed tests: 7,679 passed, 5,486 skipped, zero failures; three
additional disabled. ELF and dependency checks passed. A consistent four-schema
backup and stopped archive are retained privately. S128 started once, then was
stopped at the owner's request before bot admission. Market working-cash repair
has no native production acceptance yet. S127 had a native two-ore/two-bar city
smelting chain, with the original gathering provenance unknown.

No private credentials, live configs, databases, logs or runtime receipts are
included. Releasing again requires fresh runtime, source, backup and safety
qualification; do not replay completed launchers or admissions.

Committed module: e9d801199667a70f59a3f517b176febb67798c40 (own main).
Core checkpoint: bb599d71c8b21122c700340afaed183f0cab83e4. The verified
core-checkpoint.bundle contains this commit and requires the retained base
6cf9009f639b10f2dcc8983fd6f514de206137ba. It preserves the upstream author
and contribution trailers. Core source was not pushed to official upstream.

Removed only 2,036 obsolete generated files (14.88 GiB allocated) from the
old S83 build: CMake objects and game/scripts static archives. Current lane
build, source, private archives/data, binaries and rollback hashes retained.
VHDX was not compacted. Ubuntu also hosts an unrelated Tri/Polish queue.
