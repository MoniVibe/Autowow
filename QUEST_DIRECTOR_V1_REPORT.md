# Quest Director V1 — report

Date: 2026-07-13 (Asia/Jerusalem)
Inputs: `QUEST_CAPABILITY_MATRIX.md` (capability audit), `scripts/quest-telemetry.ps1` (read-only
telemetry), and the peer diagnostic `logs/quest-progress-diagnostic-20260713-130259.md`.

## 0. Operating constraints honored

- **The running overnight server was NOT stopped, rebuilt, restarted, or reconfigured.** worldserver
  (pid 43908) and the overnight `league-v0-director` (pid 40512, actively issuing to leaders 7/10)
  ran untouched throughout. All dirty work preserved.
- Consequently the two new **C++ bridge endpoints cannot go live this session** (they need a build +
  restart). They are authored, statically reviewed, and wired; live activation is deferred to a
  maintenance window.
- The live proof runs the V1 director in a new **`-ObserveOnly`** mode (issues no `quest`/`recover`
  order) so it cannot conflict with the overnight director already driving 7/10.

## 1. Acceptance status vs the peer baseline

The peer classified the current runs as *"movement/XP activity without quest completion"* and set the
acceptance baseline: **detect abandonment, stop repeating the same quest, prove one real objective
completion.**

| # | Acceptance criterion | Status | Evidence |
|---|---|---|---|
| 1 | Detect abandonment | **MET** | Engine-abandonment detection from `Playerbots.log`; live proof caught `L7 q789` and backed it off. |
| 2 | Stop repeating the same quest | **MET** | Deterministic selection + external backoff ledger + issue-on-change; live proof switched `L7 792→790` and `L10 459→916` after backoff. |
| 3 | Prove one real objective completion | **BLOCKED (mechanism in place)** | Requires a server rebuild: live `questlog` counters + `quest <leader> <quest-id>` targeting. On the current binary the auto-select loops on engine-abandoned quests; V1 routes around them but cannot force a completable quest until the endpoints are live. Exact procedure in §7. |

The single most important empirical finding, reconciling the peer data with the capability audit:
**the parties gain XP and move but never advance an objective counter, and the New-RPG engine then
abandons the quest** (`[New RPG] Saewash marked as abandoned quest 789`, `Brandreas … 459`). That is a
genuine no-completion, not merely save-lagged telemetry — the engine's own abandonment is the ground
truth. Root cause is the capability gap the matrix predicted: the engine routes to the POI and
delegates to grind, which earns XP off nearby non-quest mobs without crediting the quest objective.

## 2. Delivered — read-only `questlog` endpoint (C++, isolated)

New files `src/AutoWow/QuestLogView.{h,cpp}` + 4-line bridge wiring (`questlog <bot-guid>`). Reads live
world-thread `QuestStatusData` (`getQuestStatusMap` + `GetQuestTemplate`) — **no SQL, no mutation, no
quest-table access.** Per active quest it returns: id, title, level, quest_type, suggested_players,
status, `is_complete`, `objectives_all_done`, `capability_class`, `supported`, and per-objective
`{kind (npc|gameobject|item), entry (required target), required, current (live count), done}`.

Capability class (V1 supports only normal talk/kill/loot/turn-in):

| Class | Supported | Rule |
|---|---|---|
| turnin | ✅ | status COMPLETE |
| kill | ✅ | creature objective (`RequiredNpcOrGo > 0`) |
| loot | ✅ | item objective (`RequiredItemId > 0`) |
| talk | ✅ | no counted objective (report/explore) |
| item_use | ❌ | source item + creature objective (e.g. "Lazy Peons" / the Blackjack) |
| gameobject | ❌ | GameObject objective (`RequiredNpcOrGo < 0`) — no autonomous "use for credit" driver |
| dungeon_group | ❌ | `QuestType != 0 AND SuggestedPlayers >= 2` (matches the engine's own gate) |

The group gate is an **AND** — normal WotLK quests are `QuestType = 2` with `SuggestedPlayers = 0`, so
an OR would misclassify every quest (a bug caught and fixed during the live smoke test).

## 3. Delivered — `quest <leader-guid> [quest-id]` extension (C++)

`QuestParty` now accepts an optional quest id. When supplied it validates the quest is **actually in
the leader's log** (and not already rewarded) and is **party-compatible** (`SuggestedPlayers <= alive
party members`), then constrains the existing route/RPG selection to that quest only (skipping generic
questgiver discovery and the starter bootstrap). It **never grants, completes, abandons, or edits**
quests — invalid requests return `phase:"blocked"` with `reason` (`quest_not_in_leader_log` /
`quest_not_party_compatible`). The `EnableLeagueNoTeleport` policy already present on the trunk is
preserved.

## 4. Delivered — Quest Director V1 (`scripts/league-simulation-director.ps1`)

Rewritten to the V1 policy; default params/behavior stay backward-compatible (a restart is safe), with
new `-ObserveOnly` and `-Leaders` switches for the proof. Supporting libraries:
`scripts/QuestDirectorLib.ps1` (pure selection/classification/abandonment-parser) and
`scripts/QuestLogSource.ps1` (bridge `questlog` primary + read-only DB fallback).

- **Poll questlog** — primary source is the bridge `questlog` order; when absent (current binary) it
  falls back to **SELECT-only** `character_queststatus`⋈`quest_template` counters (save-lagged; noted
  in every receipt).
- **Deterministic selection** — turn-ins first (lowest id), then supported objectives by greatest
  objective progress ratio, then lowest quest id. No RNG.
- **Issue-on-change only** — issues `quest <leader> <quest-id>` only when the selected
  quest/phase changes; otherwise `quest_hold`.
- **External backoff ledger** — JSON file keyed `"<leader>:<quest>"` with expiry. A blocked objective
  is registered after `MaxRecoveriesPerQuest` failed recoveries and excluded from selection until it
  expires.
- **Engine-abandonment detection** — read-only tail of `server/logs/Playerbots.log` for
  `[New RPG] <name> marked as abandoned quest <id>`; the quest is backed off immediately (definitive
  signal, independent of DB lag).
- **Objective-centric stall** — progress is measured by the **objective counter**, not XP or raw
  movement. A quest that earns XP and moves but never advances its objective still stalls and is
  backed off. (This is the fix for the peer's exact failure mode.)
- **Explicit idle reasons** — `no_active_quest`, `unsupported_only` (+classes), `all_supported_backed_off`.
  It **never** falls back to random movement or generic grinding.

## 5. Offline tests + parse checks

- **Pester: 25/25 passing** (`scripts/tests/quest-director.tests.ps1`) — capability classification
  (incl. the AND group-gate and item_use), progress ratio, deterministic selection (turn-in-first,
  greatest-ratio, lowest-id tie-break, backoff-skip, all three idle reasons, never-grind), and
  engine-abandonment log parsing.
- **Parse checks: clean** for `QuestDirectorLib.ps1`, `QuestLogSource.ps1`,
  `league-simulation-director.ps1`, `quest-director.tests.ps1` (PowerShell AST parser, no errors).
- Two bugs were caught and fixed by the checks/proof: (a) a `$leaders`/`[uint32[]]$Leaders`
  case-insensitive variable collision; (b) a HashSet return being unrolled by PowerShell into
  null/string — both now covered by construction.

## 6. Live proof — 6-minute observe-only run on leaders 7 & 10

`logs/quest-director-v1-final-20260713-133855.jsonl` (86 receipts, exit 0, **0 director errors**).
Leaders: Saewash (7, Ember) and Brandreas (10, Northstar). Source: read-only DB fallback (running
binary predates `questlog`), save-lag noted in every sample.

| Leader | Samples | XP | Movement | Map(s) | Selected → switched | Recoveries | Backoff | Abandonment |
|---|---:|---|---:|---|---|---:|---|---|
| 7 Saewash | 17 | 2359 → 2513 (+154) | ~380 yd | 1 only | 792 (kill) → 790 (loot) | 2 | q792 `no_progress_after_recoveries` | q789 detected from log → backed off |
| 10 Brandreas | 17 | 422 → 527 (+105) | ~392 yd | 1 only | 459 (loot) → 916 (loot) | 2 | q459 `no_progress_after_recoveries` | — |

Exact receipt evidence:
- Abandonment: `quest_abandonment_detected leader_guid=7 quest_id=789 action=backed_off source=playerbots_log`.
- Backoff: `quest_backoff_registered quest_id=792 leader_guid=7 reason=no_progress_after_recoveries`;
  same for `quest_id=459 leader_guid=10`.
- Switch (stop-repeating): `quest_would_issue` sequence `L7 q792` → (backoff) → `L7 q790`; `L10 q459` →
  (backoff) → `L10 q916`.
- Recovery: 4 × `quest_recovery_would_issue` (2 per leader, `idle_seconds=90`).
- **No-teleport**: both leaders remained on map 1 with continuous ~380–390 yd movement across 17
  samples — no discontinuous position jumps. (The trunk's `EnableLeagueNoTeleport` keeps the engine's
  90-s teleport fallback off for league parties; the director itself never teleports.)

Interpretation: the parties are active (XP, movement) but **objective counters never advanced** for any
selected quest, and the engine abandoned q789 — reproducing the peer's finding. V1 correctly detected
this (objective-centric stall + abandonment log), backed off the stuck quests, and deterministically
switched to the next supported quest instead of looping — **acceptance items 1 and 2, demonstrated
live.**

## 7. Item 3 — how to prove one real objective completion (next maintenance window)

Completion cannot be forced on the current binary: `quest <leader>` auto-selects and loops on
engine-abandoned quests, and objective counters are only save-lagged over the DB. The mechanism is in
place; proving it requires the two authored endpoints to be live:

1. **Build & restart** the worldserver with `QuestLogView` + the `quest-id` extension (scheduled, not
   during the overnight run).
2. Run the director **issuing** (drop `-ObserveOnly`) on one leader with live `questlog` counters. It
   will target one supported quest via `quest <leader> <quest-id>`, watch the live objective counter,
   and — on stall/abandonment — back it off and advance to the next supported quest.
3. Acceptance = one `quest_issued` chain where a live objective `current` rises to `required` and the
   quest transitions to turn-in/rewarded (leaves the log), recorded with counters. If every supported
   quest at the party's location abandons, that is itself a valid, logged result ("no completable
   supported objective here") — the director idles with reason rather than grinding, and the party
   should be relocated to a zone with completable objectives.

The likely fix to *reach* completions (separate from the director) is closing the audited engagement
gap: the grind engages non-quest mobs at the POI. Enforcing the questlog target whitelist on
engagement (a future bridge/engine change) would let kill/loot objectives actually credit.

## 8. File manifest

```
QUEST_DIRECTOR_V1_REPORT.md                                     (this report)
azerothcore-wotlk/modules/mod-playerbots/src/AutoWow/
  QuestLogView.h / QuestLogView.cpp                             (new, read-only questlog)
  AutoWowBridge.cpp                                             (questlog wiring + quest-id extension)
scripts/QuestDirectorLib.ps1                                    (pure selector/classifier/abandonment parser)
scripts/QuestLogSource.ps1                                      (questlog + read-only DB fallback)
scripts/league-simulation-director.ps1                         (Quest Director V1)
scripts/tests/quest-director.tests.ps1                         (25 Pester tests)
logs/quest-director-v1-final-20260713-133855.jsonl             (6-min acceptance proof, exit 0)
```

## 9. Honest limits

- Nothing here was built into or run against a rebuilt server; the C++ endpoints are unexercised at
  runtime this session (compile pending the maintenance window). PowerShell is parse-clean and the
  selection/classification/abandonment logic is unit-proven and demonstrated live via the DB fallback.
- The live proof is **observation only** — issuing and completion are deferred (§7) to avoid touching
  the running server and the concurrent overnight director.
- DB-fallback objective counters are save-lagged; the engine-abandonment log is used as the definitive
  no-progress signal precisely because of that lag. The live `questlog` endpoint removes the lag once
  built.
```
