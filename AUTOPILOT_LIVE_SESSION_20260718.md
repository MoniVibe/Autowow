# Autopilot Live Session — 2026-07-18 (first live run)

Operator-authorized supervised session against the running local instance. This is
the Autopilot's first live operation; everything below is receipt-backed.
Job ledger: `logs\autopilot\run-20260718T072359Z-*\job-20260718T072359Z-fbc69de8.jsonl`
Captured live responses: `autopilot\fixtures\captured\*.json`

## What was proven live (Autopilot side — all green)

1. **Quintuple live gate held**: the first `sent=true` command required job
   `executionMode=live` + `-ConfirmLiveBridge` + allowlist enrollment (guid 10) +
   an active ownership lease + capability evidence from the interim live-probe
   manifest (`work\autopilot\capabilities.json`, `generated_by: autopilot-live-probe`
   — bridge verbs verified by direct loopback calls; every oracle.* left undeployed).
2. **First live commands** (receipts seq 13, 36, 40): `quest 10` → bridge answered
   `ok:true`; bounded recovery sent `recover 10` (verified effect: the stuck travel
   target cleared to `expired/no destination`); a fresh-attempt re-kick `quest 10`
   was authorized by the recovery (attempt bump) and sent exactly once — no
   duplicate irreversible commands at any point.
3. **Recovery contract executed live and exhausted honestly**: stall → `Recovering`
   → recover order → re-kick → still blocked → second recovery → budget 2/2 spent.
   The job never degraded to grinding, never teleported, and is parked `Suspended`
   with ownership released for resume after the fixed build deploys.
4. **Real wire shapes learned**: `questobjective` (single `objective` object with
   `current_count/required_count/phase/supported/has_lock`), `questlog` (quests[]
   with `objectives_all_done`), `acceptance` (`reward.reward_status`,
   `invariants{...}` all zero, schema `autowow.phase1.acceptance.evidence.v1`) —
   parsers upgraded, dry-run fixture shapes still supported, 187-test tree green.
   Turn-in detection now uses questlog-vanish + acceptance reward postcondition
   (`turnin_probe` receipts) — no inference.

## Server-side findings for Sol (running build, WSL worldserver restarted 2026-07-18 ~10:07 local)

The running worldserver does NOT contain the maintenance fixes. Two-continent
confirmation of the open acquisition gate:

| Probe | Brandreas (10, Teldrassil ~Dolanaar, map 1 @ 9671.7, 2474.7) | Saewash (7, Durotar, map 1 @ 1329.5, -4608.9) |
|---|---|---|
| quest_acquisition | active, **q954** giver entry 3649 (guid 4393), state `issued` | active, **q827** giver entry 3208 (the 07-17 lab quest) |
| movement.action_result | **rejected** (persists across `recover`) | **rejected** |
| `destinations <guid>` | **empty** (even with travel expired) | **empty** |
| questlog | q2520 "Sathrah's Sacrifice" `objectives_all_done=true` (turn-in ready) | **empty** (the no-log signature) |
| `quest <guid>` cycle response | `phase:"blocked", starter_found:false, starter_in_range:false` | — |
| `quest 10 2520` (explicit turn-in) | `phase:"blocked", quest_id:0` — explicit id not latched | — |

Signature match: `QUEST_ACQUIRE_NO_LOG_NO_MOVEMENT_AFTER_ONE_RETRY` (OVERNIGHT_STATUS_20260717 gate 1),
now with two additions: (a) the destination resolver returns an empty set for
league bots even at rest inside a quest hub with a turn-in ready; (b) the explicit
quest-id cycle rejects before latching the id (`quest_id:0` in the response).
Captured JSON for all of the above is under `autopilot\fixtures\captured\`.

## Requests to Sol

1. Deploy the compiled maintenance build to the WSL runtime (this session shows the
   live binary predates it), then publish the real `autowow.oracle.capabilities.v1`
   at `work\autopilot\capabilities.json` (replacing the interim live-probe file —
   same schema, just overwrite it) with true `server_build`/`session_id`.
2. The Autopilot's quester job is parked `Suspended` (`resume -JobId
   job-20260718T072359Z-fbc69de8`, fresh recovery budget on operator resume);
   Brandreas's q2520 turn-in is the ideal first `complete→rewarded` proof — the
   Autopilot's `turnin_probe` path will capture the acceptance evidence automatically.
3. `work\autopilot\oracle-enrollment-request.json` remains pending an
   `enrollment-result` per `AUTOWOW_ORACLE_INTEGRATION_CONTRACTS_V1.md`.

## Session hygiene

No server config, DB, C++, or worktree was touched; commands used only the
documented loopback bridge; all ownership leases released; the WoW client and
worldserver were left exactly as found (bots still online, native AI untouched).
