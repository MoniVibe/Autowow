# Jev Shadow-Mode Report: AutoWoW live soak (2026-09-23)

**SHADOW. Jev decided and nothing was applied to the game.** The only bridge verb used was `questlog` (read-only, allowlisted in code, at most one call per 1.1 s). I sent no mutating command and changed no config.

## Run profile
- **Soak:** `soak-s2-80-r1`, 76 distinct bots in the ledger. The owner restarted the soak from `soak-s1-30-r1` at ~02:35; the ledger was truncated and the harness now follows truncation.
- **Harness:** `python -u harness.py 2700` (45 min). Endpoint `POST https://api.typesafe.ai/v1/systemone`, model `jev-latest`, which answered as `jev-1.13.0`.
- **Detector:** a stall is 5 consecutive `blocked` events for the same quest and reason. The other triggers are `rewarded`, `deferred` and `abandoned`.
- **Sampling:** at most one decision per 60 s, at most one per bot per 10 min, and at most one Jev call per 5 s.
- **Persona test:** every third point with at least 3 options got **one call with 4 questions** (neutral, cautious, greedy, reckless explorer).
- **Baseline:** what the deterministic system did next, taken from the bot's next ledger event within 15 min.

## Headline numbers (measured run: 18 HTTP calls, 33 typed questions)
| Metric | Value |
|---|---|
| Validity (answer is a choice inside the allowlist) | **33/33 (100%)**, 0 HTTP errors |
| Latency per call | **P50 0.42 s, P90 0.75 s, max 0.92 s** |
| Tokens, 1-question call | **~823** (747 in + 76 out) |
| Tokens, 4-question persona call | ~1,559. That is 390 per question, vs 823 as separate calls |
| Total tokens | 18,504 |
| Decision points detected | 39 in 43 min (14 stall, 21 rewarded, 4 deferred) |
| Decision rate | **0.71 per bot-hour** |
| Projection at 200 bots | ~143 decisions/h and **~118k tokens/h** |
| Cost per hour at 200 bots | Pricing unknown. At $1–$10 per million tokens: **~$0.12–$1.18/h**, or $0.0008–$0.008 per decision |
| Agreement with the deterministic baseline | **2/10 comparable = 20%**. The other 8 were 7 "other" (teleport, travel-assist or accepted-elsewhere events) and 1 unobserved |

Budget across all attempts:
- **On the TypeSafe endpoint:** 28 HTTP calls, ~20.2k tokens. These break down as 2 in the s1 partial run, 6 Cloudflare 403s (0 tokens), 2 user-agent probes and 18 measured. All caps held.
- **On the old wrong endpoint:** 6 calls that all failed with 401 ($0).

## What Jev chose vs what the bots did
- **Jev's choices (n=18):** continue an open quest 7, turn in a completed quest 6, switch quest 3, clear bags 1, grind 1, **pick up a new quest 0**.
- **Deterministic (comparable n=10):** pick up a new quest 5, keep trying 2, defer 1, switch 1, turn in 1.
- **The main difference is systematic.** Jev finishes what the bot holds (turn in completed quests, continue open ones). The deterministic system keeps accepting new quests. Jev's style is plausibly better for XP per minute and quest-log hygiene, but it is **not proven**. The option facts carried no distances, and the ledger has no progress events, so the baseline is inferred from the next logged event.

### Clearly better
- **bot 121 (level 12, quest 748), `inventory_full` stall.**
  - Jev chose `clear_bags` at **0.94 confidence**, the highest of the run.
  - The deterministic system kept trying. The stall **persisted 43.4 more minutes**, blocking about once a second; it made up most of the ledger's blocked events in s1 as well.
  - This is a missing deterministic rule, and Jev found it instantly.

### Neutral or unclear
- **bot 5 (level 65) and bot 23 (level 80), `oracle_route_blocked` stalls of 11 and 10 min.**
  - Jev chose to switch to another open quest.
  - The deterministic system resolved each via travel-assist and got a reward ~11 min later.
  - For bot 23 Jev chose the same quest the system switched to (agree).

### Questionable
- **Stalls that resolved within a minute (bot 236, bot 4; persisted 0 min).**
  - Jev recommended detouring to a turn-in (confidence 0.46 and 0.20).
  - Waiting would have been fine.
  - Lesson: 5 blocks is too twitchy a stall threshold for escalating to Jev. Use time or repeat thresholds of 2 min or more.

## Confidence and probabilities
- **Distribution:** median 0.33 (min 0.11, max 0.94). Only 2 of 18 neutral answers were at 0.7 or above: clear_bags at 0.94 and a turn-in at 0.74.
- **Confidence does not predict agreement with the baseline.** The 2 agreements had 0.33 and 0.12. The disagreements included both high-confidence answers.
- **Usefulness as a gate: yes.** "Act only if confidence ≥ 0.6, else the deterministic rule decides" would have fired on 2 of 18 decisions. Both were plausible improvements.
- **Low confidence is informative.** On post-reward choices between near-equivalent quests (e.g. 0.11 among 5 options) Jev is effectively guessing. Those decisions belong to the deterministic ranking.

## Persona divergence (FUN)
5 of 5 persona points diverged across the personas, each in one call.

| Point | Neutral | Cautious | Greedy | Reckless explorer |
|---|---|---|---|---|
| stall bot236 | turnin_445 | turnin_445 | turnin_445 | keep_trying |
| stall bot23 | switch_12882 | **defer_quest** | switch_12882 | keep_trying |
| reward bot36 | turnin_445 | turnin_445 | turnin_445 | continue_808 |
| defer bot17 | turnin_808 | turnin_808 | turnin_808 | continue_837 |
| reward bot58 | continue_12843 | **turnin_12853** | **turnin_12853** | continue_12843 |

- **Reckless explorer** differs from neutral in 5/5 points, consistently choosing to push on or keep trying.
- **Cautious** differs in 2/5 and **greedy** in 1/5. These two are nearly the same as neutral on these option sets.
- **Personas only show when the option set includes flavourful choices.** With pure "which quest next" options there is little room to express character. Add personality-bearing options (explore a new zone, duel, help a nearby bot, rest at an inn) if fun is the goal.

## Recommendation
- **Tick-level or per-event control: no.** Jev is fast (0.4 s P50) and cheap per call. But at 200 bots it would be ~143 calls/h for choices that are mostly coin-flips (median confidence 0.33), and agreement with a working system is low for no measured gain.
- **Stall-breaking: yes, as a gated escalation.**
  - Trigger on long stalls only: 2 min or more, or a reason class with no deterministic handler.
  - Act only at confidence ≥ 0.6. Otherwise fall back to the deterministic rule.
  - Also mine Jev's high-confidence picks for **missing deterministic rules**. The inventory_full to clear_bags case should become a rule, not a standing LLM call.
- **Milestone strategy (after a reward):** a promising "finish before you pick up" bias. Before trusting it, give Jev distance and XP facts and re-measure against reward rate.
- **Personality and narrative:** a good fit, and cheap using multiple questions per call. Use it at milestones only, with flavour-bearing options. The reckless persona is clearly distinct; cautious and greedy need sharper option sets to separate.

## Integration notes
- The Cloudflare edge returns **403 `error code: 1010`** for the default `Python-urllib` user agent. The harness sends `User-Agent: AutoWoW-JevShadow/1.0`.
- `scripts/jev-guild-advisor.ps1` still targets the old endpoint (`jevtypesafeai.com/api/v1/decide`) and expects `usage.cost_usd`, which does not exist. It will always fall back. It is outside this write set, so I have not changed it.
- The key file holds an `apikey_...` value. It is never printed or logged, errors are scrubbed, and I scanned the outputs for it (clean).

## Files
- `harness.py`: detector, candidate builder, Jev client and read-only bridge.
- `analyze.py`: baseline resolution and stats.
- Measured run outputs: `calls.jsonl`, `decisions.jsonl`, `summary.json`, `run.log`.
- Earlier failed or partial attempts, kept as evidence: `*_attempt1*`, `*_dry*`, `*_s1_partial*`, `*_s2_cf403*`.
