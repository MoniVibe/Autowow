# Jev Shadow-Mode Report: AutoWoW soak `soak-s1-30-r1` (2026-09-23)

**SHADOW. Nothing was applied to the game.** The only bridge verb used was `questlog` (read-only, allowlisted in code, <=1 call/1.1 s, 10 calls total).

## Verdict: BLOCKED on the API key. Jev's merit has NOT been measured.

The key in `C:\dev\SGSurvivors\docs\jevapi.md` is rejected by `https://jevtypesafeai.com/api/v1/decide`:

| Attempt | Result |
|---|---|
| Harness (Python urllib, `Authorization: Bearer <key>`) | 401 "Missing API key. Send 'Authorization: Bearer jv_live_...'" |
| Existing `scripts/jev-guild-advisor.ps1 -UseJev` (Invoke-RestMethod) | fallback, "Jev request failed" (same 401) |
| Same key via `X-API-Key` header or bare `Authorization` | 401 (same message) |
| Fake keys `jv_live_fake123` / `abcdef123` (control) | 401 (same message), so this message means *any* rejected key |
| `jv_live_` + file key | 401 "Invalid or revoked API key." |

What the file holds: 108 chars, single line, `[A-Za-z0-9_.-]`, no BOM, and **no `jv_live_` prefix**. It is either the wrong secret, e.g. from another service, or a revoked key. Owner action: put a current `jv_live_...` key in that file (or point the harness at another path). The key was never printed or logged, and error text is scrubbed.

Because of that: **0 successful Jev calls, $0.00 spent, validity/latency/agreement/persona results = none.** I have not invented any numbers for them.

## What was measured (detector-only DRY run, 20 min, 2026-09-23)

Profile: `JEV_SHADOW_DRY=1 python harness.py 1200`. STALL_N=5 same-quest+reason blocks. Each decision point was sampled for candidate building at most once per 75 s and at most once per bot per 10 min.

- **Decision points detected: 16 in 19.4 min**, of which stall 5 + rewarded 9 + deferred 2. They came from 13 distinct bots. The ledger shows 36 bots active.
- **Fleet rate: ~1.4 decision points per bot-hour.** At 200 bots that is **~275 Jev decisions/hour**. Projected cost = 275 x (cost per call). That comes to ~$0.28/h at $0.001/call and ~$2.75/h at $0.01/call. Persona or narrative use multiplies this by the number of voices per point. *(The per-call cost is unmeasured, and fixing it is the first job once a key works.)*
- **Candidate sets built: 9/9 valid** (3-5 options each, all legal in-log quest ids). Examples: `{keep_trying, clear_bags, switch_746, defer_quest, grind_nearby}` and `{turnin_400, turnin_475, continue_412, pickup_new, grind_nearby}`.
- **Deterministic baseline**, taken from the next ledger event within 15 min (n=9): keep_trying 3, pickup_new 4, continue_quest 1, other (teleport-contaminated) 1.

### Cases where a strategic chooser would clearly matter (evidence, not Jev output)
1. **bot 121 (lvl 12, quest 748 Poison Water): `inventory_full` stall for 19.6+ min**, blocking about once per second. That gives **2654 of the 2705 blocked events in the whole ledger**. The deterministic system kept trying. Any option in the set (`clear_bags`, `switch_746`, `defer_quest`) beats that. **But this is a missing deterministic rule** ("inventory_full, so vendor/destroy, then resume"), not a job for an LLM.
2. **bot 14 (lvl 9): deferred quest 384 (`executor_unsupported`) while holding 2 completed quests (400, 475).** The deterministic system picked up a new quest (317) instead of turning them in. A turn-in-first choice is plausibly better (XP banked, log space). Ambiguous without distances.
3. **bot 30 (lvl 80): `progress_did_not_change` stall on 12624 for 2.8 min** with 2 alternative open quests available. This is a stall-breaking candidate.

## Recommendation (pending a real measurement)
- **Tick-level control: no.** Round-trip latency plus ~275 calls/h at 200 bots, for choices a rule handles, is the wrong trade. The biggest measured waste (case 1) needs a deterministic rule, not a model.
- **Stall-breaking after the deterministic rules run out:** the best efficiency fit. It fires rarely (5 stalls in 20 min across 36 bots) and the allowlist is small. Jev's typed-choice validation fits well here.
- **Personality/narrative (fun):** plausible at milestone moments, after rewards and on zone changes. The persona test has not been run.
- **Milestone strategy (turn-in vs pickup vs continue):** worth measuring. Case 2 shows the baseline making debatable calls.

## To finish the measurement once the key works
`cd jev-shadow; python -u harness.py 3300 *> run.log; python analyze.py`. Caps are in code: 40 calls, $2, one call per 5 s, and 4 persona points x 3 personas (cautious / greedy / reckless explorer). `analyze.py` writes validity, latency P50/P90/max, cost per call, projection, baseline agreement and persona divergence to `summary.json`.

## Files
`harness.py` (detector, candidate builder, Jev client, read-only bridge), `analyze.py` (baseline resolution and stats), `decisions.jsonl`, `dry_candidates.jsonl`, `summary.json`, `run_dry.log`, `*_attempt1*` (the failed 401 attempt, with the key scrubbed).
