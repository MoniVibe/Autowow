# Autopilot Manifest Gate (V1.4) — 2026-07-18

Task: authoritative deployed-runtime capability-manifest generator + verifier, so
control can be handed to the Autopilot safely the moment the server lane is green.
**Zero bridge mutations and zero bridge reads this session** (probes were OS
process/port queries, read-only file reads, read-only git, and `wsl sha256sum`/
`cat`). The parked successor `job-20260718T180404Z-c1e33600` was not touched:
still `Suspended`, **0 active leases**.

## What was built

- `autopilot\modules\AutopilotManifest.ps1` — observation collectors
  (worldserver SHA-256 of the RUNNING binary via WSL, module git fingerprint,
  session identity from the owning process, deployed-conf parse), Sol-input
  readers (declarations + proof registry, fail-closed), the generator
  (`New-AutopilotDeployedManifest`), and the verifier
  (`Test-AutopilotDeployedManifest`).
- CLI: `manifest generate|verify` with `-OutPath -DeclarationsPath -ProofsPath
  -AsSolPublication -SkipWsl -WorldserverSha256Override -MaxAgeMinutes`.
- `autopilot\tests\manifest.tests.ps1` — 12 offline tests. Full tree: **214/214
  across 10 suites** (all prior certification and the soak included).
- Contracts doc addendum (schemas for the binding fields + the two Sol inputs).

## Binding + refusal semantics (all test-enforced)

- `server_build`/`session_id` are DERIVED from observed facts (binary sha prefix;
  wrapper pid + process start) — they change on any restart or redeploy.
- The generator only downgrades: deployed conf `AutoWow.OracleRuntime.Enabled=0`
  forces `runtime_enabled=false` and per-op `runtime_authorized=false`; a
  'proven' claim with a missing evidence file becomes `unproven`; **the quest
  family cannot be 'proven' without an existing `live-walk-accept-proof`
  evidence file — it is downgraded to `in-progress` with an explicit refusal
  note**. Publication (`source: deployed-runtime`) requires `-AsSolPublication`
  AND established sha + session bindings; anything less is `interim-probe`,
  which the live-send gate already refuses.
- The verifier recomputes everything: source grade, freshness (default 240 min),
  binary sha (or an explicit UNVERIFIED warning when WSL is skipped — never a
  silent pass), session identity, module fingerprint drift, deployed-config
  contradictions, and a proof audit including the quest-family rule.

## Real staged demo (no publication performed)

- Generated a STAGED manifest from live probes: running worldserver
  `worldserver_sha256 = 439b167c44cfdb0ebc91e23bf0c14fca4d968e7d2b29f1dea8883cd1ed0df939`
  (matches neither overnight build `dd47…`/`ca44…` — this is the corrected
  binary), session `40560@20260718T194801`.
- `manifest verify` on the staged file: **FAIL — "not a deployed-runtime
  publication"** (the gate holds even against the generator's own output).
- `manifest verify` on the old interim `work\autopilot\capabilities.json`:
  **FAIL on five grounds**, including a REAL session-identity mismatch — the
  worldserver restarted since that file was written (wrapper 33432→40560).
  Exactly the drift this gate exists to catch.
- Note for Sol: the deployed module conf was not readable at
  `/root/p1runtime/modules/playerbots.conf` or `/root/p1runtime/playerbots.conf`;
  the probe degraded honestly (claims unaffected — downgrade-only). Tell me the
  runtime's module-conf path and I'll add it to the probe list.

## V1.3-final revision (same day): three-state proof vocabulary and hardened refusals

Per-capability proof states are now exactly **`proven-live` | `compiled-only` |
`unproven`** (unknown strings fail closed to `unproven` at generation and FAIL
verification as overclaims). New guarantees, all test-enforced (16 manifest
tests; 218/218 full tree):

- **Quest acquisition is capped at `compiled-only`** until the registry carries a
  `live-walk-accept-proof` entry whose evidence file exists — the live
  walk-to-giver → accepted-into-log receipt. Nothing else lifts it: not build
  success, not movement, not XP, not an `ok:true` command response (those
  evidence kinds are on an explicit insufficient-evidence denylist and downgrade
  any `proven-live` claim to `compiled-only`).
- `max_age_minutes` is embedded in the manifest itself; the verifier honors it
  without needing an operator to remember the right override.
- Alternative source binding: `-TopologyManifestPath` hashes a Sol-supplied
  topology/staging manifest instead of the git worktree (`module_fingerprint.source
  = "topology-manifest"`); the verifier recomputes the file hash and fails on drift
  or a missing topology file.

## Exact evidence Sol must supply after deployment

1. **Declarations** (`autowow.autopilot.capability-declarations.v1`): per-operation
   deployed/native-adapter/runtime-authorized flags + `runtime_enabled` for the
   deployed build. The generator only downgrades these against observed config.
2. **Proof registry** (`autowow.autopilot.capability-proofs.v1`): one entry per
   capability with `status` ∈ proven-live|compiled-only|unproven and, for every
   `proven-live`, an existing evidence file. Specifically for the quest family:
   `evidence_kind: "live-walk-accept-proof"` referencing the receipts of a live
   walk-to-giver ending in an accepted-into-log observation (e.g. the acceptance/
   questlog receipt bundle from the supervised run). Gathering may cite its
   per-node receipt evidence; anything without live receipts stays compiled-only.
3. **Optional topology manifest** for the staged WSL tree if Sol prefers that
   binding over the Windows worktree fingerprint.
4. The **runtime module-conf path** inside WSL (so the config probe can verify
   `AutoWow.OracleRuntime.*` directly rather than degrading).

## Handoff sequence for Sol (when the lane turns green)

```powershell
cd D:\Games\wowstuff\AutoWoW\autopilot
# 1. Author declarations + proofs (proof registry entries for the quest family
#    must reference the finished live walk/accept evidence with
#    evidence_kind "live-walk-accept-proof").
# 2. Publish:
.\autowow-autopilot.ps1 manifest generate -OutPath ..\work\autopilot\capabilities.json `
    -DeclarationsPath <declarations.json> -ProofsPath <proofs.json> -AsSolPublication
# 3. Gate check (also run any time before/after a supervised window):
.\autowow-autopilot.ps1 manifest verify
# 4. Then the parked successor per AUTOPILOT_SUCCESSOR_JOB_REPORT.md:
.\autowow-autopilot.ps1 resume -JobId job-20260718T180404Z-c1e33600
.\autowow-autopilot.ps1 run -DurationMinutes 15 -TickSeconds 10 -StopOnQuiescence -ConfirmLiveBridge
```

A manifest that fails `manifest verify` cannot authorize live sends (the
per-command gate independently requires `deployed-runtime` source + freshness +
identity), so a restart or redeploy between publication and the window fails
closed automatically.
