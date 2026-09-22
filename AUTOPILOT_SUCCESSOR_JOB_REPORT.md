# Autopilot Successor-Job Report (V1.3) — 2026-07-18

Lane: player-facing Autopilot only. **No live gameplay commands, server mutations,
builds, or restarts were performed in this session.** Zero bridge traffic of any
kind; the persistent gatherers Kurl (6) and Pikli (49) were never leased or
addressed. All work is files + tests.

## Changed files

- `autopilot\AutopilotLib.ps1` — successor lifecycle (`Test-AutopilotJobWallClockExpired`,
  `Test-AutopilotJobSuccessorEligible`, `New-AutopilotSuccessorJob`,
  `Invoke-AutopilotResume`, `Suspend-AutopilotJob`); `New-AutopilotJob` gains an
  `IdempotencyNamespace`; party-contract classification table + Executing-branch
  response handling; quest verb separation (`auto` / `acquire` / `execute:<id>`);
  `party-contract-unmet` blocked code + `waiting-for-party-contract` wait state;
  capability-manifest provenance (`Origin`) + `Test-AutopilotCapabilityProviderLiveEligible`
  wired into the live send path.
- `autopilot\autowow-autopilot.ps1` — new `retry` command; `resume` now routes
  through the expiry diagnostic; `pause` uses the lease-releasing suspension path;
  `run -StopOnQuiescence`.
- `autopilot\modules\AutopilotRunLoop.ps1` — `-StopOnQuiescence` decisive-run exit.
- `AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json` — `provenance` object; `party-contract-unmet`.
- `autopilot\tests\v13-lifecycle.tests.ps1` — new suite (15 tests).
- `work\autopilot\capabilities.json` — marked `source: "interim-probe"` (content
  otherwise untouched; no Oracle publication is claimed).
- `autopilot\README.md`, this report.

## Job lifecycle semantics

- **Resume remains resume.** It never resets clocks or budgets. Resuming a job
  whose `jobTimeoutMinutes` wall clock (measured from its original `startedAt`)
  has expired is refused with a `resume_refused_expired` receipt and a diagnostic
  recommending `retry -JobId <id>`. An expired job can never re-enter execution.
- **`retry` creates an honest successor** from a finished job only (terminal
  phase, `Blocked` on job-timeout/phase-timeout/recovery-budget-exhausted,
  cancellation-requested, or wall-clock expired; runnable jobs are refused with
  instructions to cancel first). The successor receives: a new job id; a distinct
  idempotency namespace (`…|ns:supersedes:<originalId>` — it can never collide
  with or dedupe into the original, while double-invoking `retry` still yields
  exactly one successor); fresh creation/start clocks; fresh attempt/recovery
  budgets and empty counters; its own receipt ledger opened with a
  `job_retried_from` receipt. Provenance travels on the successor:
  `provenance.supersedesJobId`, `provenance.retryReason`, `provenance.retriedAt`,
  plus inherited kind/spec/success criteria/policies/mode/profile metadata.
- **The original is historical evidence**: its document and ledger are never
  modified — not even appended to (hash-verified below).

## Party-response classification (planning preconditions, never movement stalls)

| Bridge code | Classification | Controller behavior |
|---|---|---|
| `quest_requires_party_leader` | precondition | `Blocked('party-contract-unmet')`, receipt `party_precondition`: "explicit quest orders require the target character to be an actual party leader" |
| `party_member_exact_quest_preflight_failed` | precondition | same block; "every party member must satisfy the quest contract" |
| `quest_not_in_member_log` | precondition | same block; "companions must obtain it normally (sharing/granting is not permitted)" |
| `quest_not_in_leader_log` | re-plan | when `bridge.quest.acquire` is permitted: `replan_to_acquisition` receipt, `questMode=acquire`, next tick issues the acquisition verb; otherwise the same honest block |
| `cross_faction_party_not_allowed` | precondition | same block; "party members must all share one faction" |

Guarantees, all test-enforced: zero recovery budget consumed; ownership released
on block; the impossible command is never re-issued (Blocked has no self-heal for
this code — an operator or plan change is required); no fabricated parties,
invites, quest sharing/granting, or faction changes exist anywhere in the code.

## Acquisition vs in-log execution

`quest <leader> acquire` (acquisition) and `quest <leader> <quest-id>` (exact
in-log execution/turn-in) are now distinct planner modes with distinct wire
commands and distinct idempotency keys; bare `quest <leader>` remains the native
director cycle (default `auto` mode). Turn-in proof is unchanged and exclusive:
questlog disappearance **plus** positive `acceptance.reward.reward_status`
(or explicit server receipt). XP, movement, or questlog disappearance alone
never count.

## Capability-manifest hygiene

Manifests now carry `source` provenance. Live sends require
`Test-AutopilotCapabilityProviderLiveEligible`: `source == "deployed-runtime"`
AND fresh `generated_at` (default max age 240 min, future-dated rejected) AND
`server_build`/`session_id` present — otherwise the live path throws before any
socket opens. The current `work\autopilot\capabilities.json` is marked
`interim-probe` and therefore **cannot authorize live sends**; this remains true
until Sol publishes a real deployed-runtime manifest.

## Tests

- New `v13-lifecycle.tests.ps1`: **15/15** — expired-resume refusal; unexpired
  resume with untouched clocks; successor provenance/clock/budget/ledger/key;
  byte-for-byte original preservation; double-retry dedupe; runnable-job refusal;
  party-precondition zero-recovery block with actionable receipt and no re-issue;
  classification completeness; `quest_not_in_leader_log` replanning with both
  distinct wires (`quest 10 2520` then `quest 10 acquire`); no-replan-when-not-
  permitted; acquisition dedupe across duplicate ticks; manifest live-eligibility
  accept/reject matrix; end-to-end live refusal on an interim manifest with every
  other gate satisfied; suspension and cancellation lease release.
- Full tree: **202/202 across 9 suites** including the V1.1 certification and the
  eight-hour soak, all green.

## Parked successor (created without any gameplay command)

- Successor id: **`job-20260718T180404Z-c1e33600`** — supersedes
  `job-20260718T072359Z-fbc69de8`, phase **Suspended**, mode live (gated), fresh
  ledger `logs\autopilot\run-20260718T180404Z-7af0\job-20260718T180404Z-c1e33600.jsonl`.
- Original job document and ledger hash-verified **byte-for-byte unchanged**
  (SHA256 `A65F…22D8` / `81A7…75EA`, before == after).
- Ownership registry: **0 active leases**.

## Exact commands for Sol's supervised window (after the acquisition-movement fix deploys)

```powershell
# 1. Sol publishes the real manifest (overwrite the interim file; source MUST be deployed-runtime):
#    work\autopilot\capabilities.json  { "source": "deployed-runtime", "server_build": ..., "session_id": ..., "generated_at": <fresh>, "bridge": { "deployed": true }, ... }

# 2. Wake the parked successor and run a short decisive supervised proof:
cd D:\Games\wowstuff\AutoWoW\autopilot
.\autowow-autopilot.ps1 resume -JobId job-20260718T180404Z-c1e33600
.\autowow-autopilot.ps1 run -DurationMinutes 15 -TickSeconds 10 -StopOnQuiescence -ConfirmLiveBridge

# 3. Watch / conclude:
.\autowow-autopilot.ps1 status
.\autowow-autopilot.ps1 receipts -JobId job-20260718T180404Z-c1e33600 -Tail 30
.\autowow-autopilot.ps1 explain -CharacterGuid 10
```

The run stops on its own at the first of: one proven quest reward
(`turnin_probe` with positive `reward_status` → `Completed`); an explicit
unsupported/party-precondition reason (`Blocked('party-contract-unmet')`, no
self-heal); bounded recovery exhaustion (`recovery-budget-exhausted`); a
lease/capability mismatch (roster-owned block or a live-send refusal); or the
15-minute duration. `-StopOnQuiescence` exits the loop as soon as no job can
make further progress. Expected first contact: q2520 is not yet rewardable and
leader-only preflight rejects it, so the controller should re-plan to
`quest 10 acquire` and pursue acquisition under the fixed movement — with every
step receipted either way.
