# PvP + Group Coordination Science Lane (S2 cells)

Status: RESEARCH ONLY. Written 2026-09-23 ~01:50 IDT during live soak `soak-s1-30-r1` (36 bots online).
No mutations were made: no server, config, bridge-mutation, DB, or git actions. The only
bridge call was one read-only `list`.

Evidence classes:
- **SOURCE**: module `/root/autowow-advisor-t1-module` @ `c2a14f15`, core `/root/autowow-advisor-t1-core`.
- **CONFIG**: `/usr/local/etc/modules/playerbots.conf`, `/root/p1runtime/worldserver.conf`.
- **LIVE**: the bridge `list` at 01:47, plus `/root/autowow-soak/logs/ledger.log` (972 rows, 01:37-01:48).
- **MEASURED**: prior receipts under `D:\Games\wowstuff\AutoWoW\logs`.
- **UNMEASURED**: a hypothesis to check.

Ranking follows the owner's amendments:
1. Live levers (bridge verbs) come first.
2. Config-only changes at restart come second.
3. Code changes are specified for the ~04:00 rebuild window. I did not build them.

---

## 0. Three facts that change the design

1. **This is a PvE realm.** `GameType = 0` (CONFIG worldserver.conf:926).
   - Random bots are forced to `SetPvP(sWorld->IsPvPRealm())`, which here means `false`. This happens at login (`RandomPlayerbotMgr.cpp:2657`) and on every refresh or revive (`:2091`).
   - The stock `pvp` strategy targets only enemies with `IsPvP()` true (`EnemyPlayerValue.cpp:22`).
   - **Result: open-world PvP between bots cannot happen today.** Every bot's strategy list does include `pvp`, `duel`, and `start duel`, but they never fire.
   - Hypothesis (UNMEASURED): this is also why world-PvP fixture run `AWPVP1-20260715-035915` failed with "did not observe an opposing fixture target GUID".
2. **The OraclePvp adapter is a scaffold.**
   - `AutoWowOraclePvpOpenWorldAdapter.h`, `...Policy.h` and `OraclePvpCombatExecutor.{h,cpp}` have unit tests. Nothing outside those files includes them: a grep of `src/` finds zero references.
   - The adapter also blocks itself on purpose (adapter.h:140-141): every PvP/open-world operation "remains planning-visible but blocked until it receives an explicit executor and postcondition contract".
   - It cannot run an experiment in S2.
3. **There is no command server and no scriptable GM console.**
   - `AiPlayerbot.CommandServerPort = 0`, `SOAP.Enabled = 0`, `Ra.Enable = 0`.
   - The worldserver console is the launcher's pty (`/proc/<pid>/fd/0 -> /dev/pts/0`). It is reachable only if the orchestrator owns that terminal.
   - **Whisper commands such as `co +x` and `nc -x` cannot be sent live.** The only live levers are bridge verbs on `127.0.0.1:18787`.

## A. PvP: what is proven and what is scaffold

| Capability | Status | Evidence |
|---|---|---|
| WSG exact 10v10 queue + join (bridge `wsg queue/status/leave`, `WarsongFixtureControl.cpp`) | **PROVEN: queue, invite, and in-progress play** | `logs/wsg-10v10-proof-WSG10-20260714-125653-18556e75.json`, fixture `awrole1` (GUIDs 17392-17411, mirrored role comps, L80, rare gear): 57 kills / 57 deaths, 1.84M damage, 664k healing, 10 flag-state changes, **2 captures (Horde 2 - Alliance 0)**, 2 returns |
| WSG match **completed with a winner** | **NOT PROVEN** | The same run ended `FAIL`: `final_state_is_not_terminal:in_progress`, `final_winner_missing`, `exact_instance_roster_was_not_maintained`, after 15 min. All 8 WSG proof receipts are `FAIL`. The other 7 failed before gameplay (harness or preflight errors). |
| AV / WSG via stock random-bot autojoin | **Population only** | `PLAYERBOTS_LOAD_TEST_REPORT.md` (80 bots, AV, 68 on map 30) and `_40_REPORT.md` (40, WSG): PASS for population and memory only, on the **Windows** worldserver. Both reports say this "does not by itself prove a completed battleground". |
| Stock BG tactics (flag carry, AV objectives) | Present, unmeasured here | `BattleGroundTactics.cpp`. `PlayerBotsBGScript::OnBattlegroundStart` picks a **random** per-faction strategy each match (`Playerbots.cpp:563-590`). That is a variance source. |
| Open-world PvP adapter (OraclePvp*) | **Scaffold** | See 0.2 |
| Bridge `engage <leader> player <guid>` | Wired but gated | Requires a league member leader in a group (`AutoWowBridge.cpp:3824-3838`). Target must pass `hostile` + `attackable` (`ExactBossTargetControl.cpp:110-126`). On a PvE realm an unflagged target should fail `attackable` (UNMEASURED). |
| World-PvP 2v2 fixture (`pvp-fixture-world-pvp.ps1`) | FAILED_CLOSED | 108 receipts; 0 accepted engagements |
| BG / PvP ledger events | **None** | Ledger events are only accepted, rewarded, abandoned, blocked, deferred, contaminated (`AutoWowQuestLedger.h:35-43`) |

WSG fixture preconditions (SOURCE `WarsongFixtureControl.cpp:280-440`):
- `RandomBotJoinBG` and `RandomBotAutoJoinBG` must both be 0. Both are 0 now (CONFIG :1267, :1270). If autojoin is on, the fixture refuses with `wsg_random_bg_automation_enabled`. **The stock-autojoin and exact-fixture methods are mutually exclusive in one run.**
- No BG debug or testing mode; WSG template is 10v10.
- All 20 GUIDs enrolled in `autowow_league_member`, online, and playerbots.
- Same level and same bracket, exactly 10 Alliance and 10 Horde.
- No foreign players queued or active in the bracket.

### Experiment P1: WSG bot-vs-bot (RANK 1, runnable live without restart)
Why live: it is bridge verbs only. The BG runs in its own instance (map 489). It does not move any soak bot. The fixture GUIDs are already in `AutoWow.FixtureGuids` (CONFIG :2385).

- **Bots:** `awrole1` 17392-17401 (Alliance) and 17402-17411 (Horde). Mirrored slots: 1 bear tank/flag carrier, 2 healers, 7 DPS per side. **No soak or scout bots are touched.**
- **Preflight (read-only):**
  - Run `fixture status <guid>` for each of the 20.
  - Read-only SQL: `SELECT character_guid FROM <playerbots_db>.autowow_league_member WHERE character_guid BETWEEN 17392 AND 17411 AND retired_at IS NULL`. Expect 20 rows.
- **Run:** use the existing harness (it activates, queues, polls, leaves, and deactivates):
  ```powershell
  pwsh -NoProfile -File D:\Games\wowstuff\AutoWoW\scripts\wsg-10v10-proof.ps1 `
    -FixtureId awrole1 -RosterGuid 17392,17393,17394,17395,17396,17397,17398,17399,17400,17401,17402,17403,17404,17405,17406,17407,17408,17409,17410,17411 `
    -Level 80 -Quality 3 -SkipFixtureInit -MaxWaitMinutes 35 -PollSeconds 2 -Apply
  ```
  The equivalent raw bridge lines:
  - `activate <g>` x20
  - `wsg queue <20 guids>`
  - `wsg status <20 guids>` every 10 s
  - `wsg leave <20 guids>`
  - `deactivate <g>` x20
  
  Raise the harness poll to 10 s for the soak. 2 s means 30 world-thread ops per minute, which is cheap but not needed.
- **Rollback:** `wsg leave <20>` then `deactivate <g>` x20. The harness does this in its cleanup.
- **Duration:** 120 s prep plus the match. Use a max wait of 35 min (the first run was still in progress at 15 min). Repeat serially: about 6 matches in a 4 h S2.
- **Metrics:** all from the `wsg status` JSON (`winner, score, flags, per-player kills/deaths/honorable_kills/damage/healing/flag_captures/flag_returns, battleground_state`).
  - win rate by faction
  - caps per match
  - time queue -> in_progress, and in_progress -> first cap and -> complete
  - deaths per player per minute
  - roster retention: count of `exact_instance_roster_was_not_maintained`
- **Pass criteria:**
  - P1-a: ≥1 match reaches `battleground_state=complete` with `winner != none`. This is the first completed-match proof.
  - P1-b: ≥4 terminal matches in S2.
  - Win-rate claims need ≥20 matches for a mirrored-comp faction-bias test (binomial; 6 matches detects only a 6-0 sweep, p = 0.03). Report "faction bias: insufficient n" until then.
- **Risks:**
  - Adds 20 L80 bots to a live world. The measured memory is +20 bots ≈ the 40-bot WSG load test.
  - **UNMEASURED:** whether `activate` via `sRandomPlayerbotMgr.AddPlayerBot` counts against `MaxRandomBots=36` and makes the manager log soak bots out. Abort if `list` shows fewer than 36 soak bots two minutes after activation.
  - Harness bugs caused 7 of the 8 prior failures. Dry-run it once without `-Apply` first.

### Experiment P1b: stock autojoin WSG/AV (config at restart; alternative to P1, not with it)
- **Keys:**
  - `AiPlayerbot.RandomBotJoinBG = 1`
  - `AiPlayerbot.RandomBotAutoJoinBG = 1`
  - `AiPlayerbot.RandomBotAutoJoinBGWSCount = 1`
  - `AiPlayerbot.RandomBotAutoJoinWSBrackets = 7` (already set; L80 only)
  - For AV: `RandomBotAutoJoinBGAVCount = 1` with `AVBrackets = 3` (already set)
  - Autojoin runs only when all random bots are logged in (`RandomPlayerbotMgr.cpp:1118`).
- **Bots:** every L80 random bot is eligible. Today that is 13 L80s in Northrend (22, 23, 24, 27-33, 38, 39, 41). You need ≥10 per faction for WSG and 40 per faction for AV. **AV cannot run inside an 80-bot S2** because it would consume the whole population. Defer AV to a dedicated step.
- **Questing risk:** BG entry pulls L80 questers out of their zones. Mark all L80s as a BG cell and exclude them from questing metrics. A cleaner option is to keep S2 questers ≤ L79; `RandomBotMaxLevel` is already 60 for new rolls.
- **Metric gap:** no `wsg status` exists for non-roster instances. Code change C1 is needed to measure outcomes.

### Experiment P2: open-world skirmish (NOT runnable live; needs C3 or a whole-world config step)
- **Option A, config at restart:** `GameType = 1` (PvP realm). This flags every random bot on login.
  - It is **world-wide**, so it cannot be a cell. Run it as a separate soak step and compare across runs (S2 vs S2-PvP).
  - It contaminates questing throughput in every non-prohibited zone.
- **Option B, code (C3 `pvpflag` verb):** flag only the cell's bots.
  - The stock `pvp` strategy attacks only `IsPvP()` enemies, so unflagged questers can never become targets. The cell is self-contained.
  - **Zone:** Stranglethorn (zone 33; about L30-45; heavy two-faction quest density). Alternates: Hillsbrad (267) or Ashenvale (331). None are in `PvpProhibitedZoneIds` (CONFIG :1344).
  - **Bots:** 4 Alliance + 4 Horde already in zone 33, picked from the S2 histogram, so no placement is needed. Setup alternative: GM `.tele` / `.character level`, which needs console or SOAP.
  - **Sequence:** `pvpflag <g> on` x8, then observe. Re-flag after any `contaminated rndbot_revive` row, because Refresh resets the flag (`RandomPlayerbotMgr.cpp:2091`). Rollback: `pvpflag <g> off` x8.
  - **Duration:** 2 h.
  - **Metrics:** needs C1 events `pvp_kill` and `death`.
    - engagement rate = PvP kills per flagged bot-hour
    - outcome by (level diff, class) from killer and killed rows
    - questing rewarded per hour for flagged vs unflagged STV bots
  - Stock engage gating for reference (`PossibleTargetsValue.cpp:22-120`): never attack at +5 levels; 25/50/75% chance at a gap of 4/3/2; hash-stable per 2-minute window.
- **Pass criteria:** ≥10 PvP kills; zero PvP kills involving an unflagged bot, which proves containment.

## B. Coordination: solo vs duo vs trio

### What exists
- Bridge `party <leader> <m1..m4>` (`AutoWowBridge.cpp:5506`, handler around :2816):
  - **No league requirement.**
  - Members must be the same faction and online.
  - Creates a real group and sets every member `autoWowIndependentParty` with `+grind,+new rpg,-follow,-move random,-travel`. Each member runs its own quest loop; grouping is social only.
  - Clears no-teleport: `SetNoTeleport(member,false)`.
- Bridge verbs `quest`, `quest-acquire`, `recover`, `independent`, `engage`, and `deploy` all require `autowow_league_member` (e.g. :2764, :3824, :3927, :4360, :4996). Party quest selection (common eligible quest, `party_no_common_eligible_quest` at :4224) and the 45-yard cohesion follow (`QuestPartyCohesionPolicy.h:54`) are therefore league-only.
- `rally` teleports members (:3055-3065). Use it only as setup and log it as contamination.
- `AiPlayerbot.RandomBotGroupNearby = 0` (CONFIG :978).
  - When 1, only bots with grouper type LEADER_2..5 invite (about 20% of bots; `PlayerbotAI.cpp:4634-4653`). MEMBER types join.
  - Stock members **follow** the leader. This is the "follow-group" arm.
  - The key is global, so it is a between-run comparison and not a cell.
- **Group dissolution risk (SOURCE `LeaveGroupAction.cpp` `LeaveFarAwayAction::isUseful`).** A non-leader bot leaves if:
  - its grouper type is SOLO (about 20% of random bots, from a fixed hash)
  - death count > 4
  - level gap to the leader > 4
  - it is on another map or ≥ 2 × `RpgDistance` (400 yd) from the leader
  
  `party` clears no-teleport, so this guard is **active** for bridge parties. Group retention is therefore a primary metric.

### Confounds you must control
- `party` sets `autoWowIndependentParty`. That gives:
  - fast non-combat react delay, clamped to 100-250 ms (`AutoWowIndependentActivityPolicy.h:21`), where stock is 1-3 s outdoors
  - no-teleport stuck handling and grounded grind (`NewRpgBaseAction.cpp:204-208`)
  
  Solo stock controls have neither. A naive solo-vs-party difference measures **react speed plus grouping**, not grouping alone.
  - Fix, live: arm solo controls with `independent <g>`. This needs league rows (see L0 below).
  - Fix, code: C4.
- `inventory_full` is 1058 of 1088 `blocked` rows in S1. Most are scouts 121 and 244, but it will swamp throughput. Report `inventory_full` per arm, and exclude any bot-hour with more than 50% of its blocked rows from `inventory_full`.
- Contamination is live: in 11 minutes, 30 `rndbot_randomize`, 11 `zone_travel_assist`, 7 `rndbot_teleport`. Exclude a bot's bot-hours after any post-order contamination row.

### Live-now pilot (during S1, bridge only; picks exclude scouts 101, 112, 121, 123, 236, 244)
Team is taken from ledger `team` (0 = Alliance, 1 = Horde). Level and zone come from the LIVE `list` at 01:47.

| Pilot cell | Leader, members | Faction | Zone | Levels |
|---|---|---|---|---|
| L-trio-1 | 10; 11, 15 | Alliance | Teldrassil 141 | 9 / 8 / 8 |
| L-duo-1 | 14; 19 | Alliance | Dun Morogh 1 | 9 / 8 |
| L-duo-2 | 17; 25 | Horde | Durotar 14 | 7 / 8 |
| L-solo (control) | 7, 18 | Horde | Tirisfal 85 | 11 / 10 |
| L-solo (control) | 12 | Horde | Eversong 3430 | 10 |

Design: **ABA within each bot.**
- A = solo baseline from the ledger, S1 start (01:36) to T1.
- B = grouped, T1 to T1 + 90 min.
- A' = disbanded, if a disband path exists.

Commands (the orchestrator runs them):
```text
# precheck (read-only): list -> each bot alive, combat=false, group.members=0, same zone as above
party 10 11 15
party 14 19
party 17 25
# every 60 s: the soak-snapshot list sample already running gives group.members and leader_guid
```
PowerShell form: `pwsh -File D:\Games\wowstuff\AutoWoW\scripts\autowow-control.ps1 -Action party -BotGuid 10 -MemberGuid 11,15`.

- **Rollback:** there is **no bridge disband verb**.
  - With the console: `.group disband Brandreas` (leader 10), `.group disband Jogdarkuhr` (14), `.group disband Kornzoggoch` (17).
  - Without it, groups persist in the characters DB. **They must be disbanded before S2**, or verified gone after restart with `list`.
  - Code fix: C2.
- **Metrics:** see the list below. Pilot pass = mechanics only:
  - the three groups form
  - retention ≥ 60 min in ≥ 2 of 3 groups
  - ≥ 1 rewarded row per grouped bot
  
  A throughput claim needs S2 scale.

### S2 coordination cells (A/B, same zones)
- **Arms:**
  - B0 solo stock (control)
  - B0m solo independent (matched control; needs L0 or C4)
  - B1 bridge duo
  - B2 bridge trio
  - B3 party-quest trio (`quest <leader>` shared quest; needs L0 or C4)
  - B4 stock follow-group (`RandomBotGroupNearby=1`; whole-world, separate run)
- **Placement:** each zone cell gets 1 solo, 1 duo, and 1 trio, same faction, level spread ≤ 2, zones with 3-6 bots of one faction.
- **Metrics:** reducer over `ledger.log`, joined with the 60 s `list` samples for group size.
  - rewarded quests per bot-hour (primary)
  - share of rewarded quests that are group or elite. Join the quest id with a read-only SELECT on world `quest_template.QuestInfoID` (1 = Group) and `QuestLevel`.
  - deaths per bot-hour: `list` `alive` false-edges today; C1 `death` event later
  - stall rate = blocked rows per bot-hour, plus distinct `(reason, phase, zone, 50-yd bucket)` signatures
  - travel proxy = accepted -> rewarded duration per quest
  - group retention (minutes grouped / minutes assigned)
  - shared-credit share = rewarded rows where ≥ 2 group members reward the same quest id within 120 s
- **Pass criteria:**
  - Directional: trio or duo reaches ≥ 1.3 × B0m rewarded/bot-hour, or completes ≥ 1 group quest that B0 never completes.
  - Retention ≥ 70%.
  - Deaths per bot-hour not above B0 + 50%.
  - Use Mann-Whitney U on per-bot rates. With about 8 bots per arm, only an effect near 2× is detectable. Report effect size + CI and do not claim significance otherwise.

### L0 (live DB setup, no rebuild; owner go needed)
The league gate queries the DB on every call (`AutoWowBridge.cpp:684-689`), so inserting rows unlocks `independent`, `quest`, `recover`, and `engage` for chosen random bots live.
- Pattern from `scripts/league-simulation.ps1:293`:
  `INSERT IGNORE INTO <playerbots_db>.autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active) VALUES (<g>,NULL,'wayfarers','soak-exp','','','',0);`
- Rollback: `UPDATE ... SET retired_at=NOW() WHERE character_guid IN (...)`.
- Risk: league directors key on `active=1`. Keep `active=0`. No director is running now; only the MySQL relay and `soak-snapshot.ps1` were seen.

## C. Decision latency: where group and PvP decisions wait

| Wait point | Value | Source |
|---|---|---|
| Base react delay | `ReactDelay=100` ms, `DynamicReactDelay=1` | CONFIG :369, :372 |
| Stock open world, in combat | 5 × base = 500 ms per decision | `PlayerbotAI.cpp:6786-6788` |
| Stock open world, out of combat | 10-30 × base = 1-3 s (resting: 2-20 s) | `:6790-6795` |
| In BG (`FastReactInBG=1`) | combat 250 ms; non-combat 100 ms | `:6770-6783`, CONFIG :1350 |
| AutoWow independent or `party` bots, out of combat | clamped to 100-250 ms | `AutoWowIndependentActivityPolicy.h:21` |
| Enemy-player detection | trigger `enemy player near` checkInterval 3 → **3000 ms** (+ react delay) → 3-6 s to notice a flagged enemy outdoors | `PvpTriggers.h:17`, `Trigger.cpp:14` |
| Oracle managed-bot decisions | `CadenceMs=1000`, lease TTL 3 ticks; all managed bots in one tick | CONFIG :2372-2374 |
| Quest-party cohesion | follower rejoins beyond 45 yd (leader never waits) | `QuestPartyCohesionPolicy.h:51-54` |
| Stock follow | `FollowDistance=1.5`, `MaxWaitForMove=1000` ms | CONFIG :407, :349 |
| Other waits | `PassiveDelay=10000`, `RpgDelay=10000`, `RepeatDelay=2000` ms | CONFIG :375-382 |
| BG strategy | chosen **once** per match at random per faction | `Playerbots.cpp:567-585` |

**Cheap knobs:**
1. `DynamicReactDelay=0`: static 10 × base out of combat, 2.5 × in combat. This is world-wide, so test it as a run, not a cell.
2. Lower `ReactDelay` (e.g. 50).
3. The biggest single PvP wait is the 3 s `enemy player near` interval. Changing it is a code constant (`PvpTriggers.h:17`, `3` → `1`); trigger cost is one grid scan, so check tick time.

No latency instrumentation exists. P1's `wsg status` poll gives time-to-objective only. Per-decision latency needs code (C5).

## D. Code changes for the ~04:00 rebuild (spec only; not built)

| ID | Change | Where | Risk | Test seam |
|---|---|---|---|---|
| C1 | Append ledger events (schema v1 allows appending events; field order is unchanged): `bg_join=6`, `bg_end=7`, `pvp_kill=8`, `death=9`, `group_join=10`, `group_leave=11`. Reuse fields: `quest=0`; `reason` = winner (`win/loss/draw`), killed/killer GUID, or leader GUID; `phase` = BG type (`wsg/av/...`) or kill source (`player/creature`). | `AutoWowQuestLedger.h:35-75` (enum + `EventName`). `Playerbots.cpp:100-119`: add the hook enums `PLAYERHOOK_ON_PLAYER_JOIN_BG` (core PlayerScript.h:101), `..._KILLED_BY_CREATURE` (:42), `..._JUST_DIED` (:32), and the PvP-kill hook (PlayerScript.h:252; confirm the enum name), with overrides. `PlayerBotsBGScript` (`Playerbots.cpp:563`): add `OnBattlegroundEndReward(bg, player, winner)` (AllBattlegroundScript.h:72) and log the chosen `bgStrategies` index at start. Add a GroupScript `OnAddMember/OnRemoveMember` (GroupScript.h:48, :54). | Low. Emit is bot-only and gated on `Ledger.Enable`. The hook list must include the new enums or the hooks never fire. | Extend `tests/AutoWowQuestLedgerTest.cpp`: `EventName` for the new values; golden `FormatLine` row for `bg_end` |
| C2 | Bridge `party-disband <leader>` = rollback for B cells | Parse next to `party` (`AutoWowBridge.cpp:5506`); handler next to `CreateParty` (~:2816): `leader->GetGroup()->Disband()`; clear `SetAutoWowIndependentParty(false)` on members | Low | Parser test with the other bridge wire tests |
| C3 | Bridge `pvpflag <guid> on\|off`, gated to `AutoWow.FixtureGuids` or a new `AutoWow.Experiment.Guids` | New verb; handler calls `bot->SetPvP(on)` (as `FlagAction.cpp:31-35`). Refresh at `RandomPlayerbotMgr.cpp:2091` resets it, so re-flag on revive or exclude the bot. | Medium: a flagged bot can be attacked by other flagged bots only. Verify the PvE-realm flag does not decay out of combat (PvP-desired flag). | Parser test + allowlist gate test |
| C4 | Allow experiment GUIDs past the league gate for control verbs only | New `IsControlEligible(g) = IsLeagueMember(g) \|\| Experiment.Guids`; use it at :2764 (independent), :3824 (engage), :3927/:4360 (quest), :4996 (recover). Leave the economy sites (:2588-2602, :2685) league-only. | Medium: quest-party code may read league team rows; check `team_id` NULL handling | Pure predicate test |
| C5 | Decision-latency probe: record `now - lastEnemySeenMs` at first attack on a player, emitted as `pvp_kill.phase` | `EnemyPlayerValue` / attack action | Low | none (field-only) |
| — | OraclePvp wiring (executor + postcondition contract) | adapter.h:140 | Large; **not for S2** | Existing tests |

## E. Ready-to-use S2 cell table

| Cell | Bots | Faction | Zone / BG | Config / verb | Metric | Duration |
|---|---|---|---|---|---|---|
| P1 WSG exact | 20 (awrole1 17392-17411) | 10 A / 10 H, mirrored | WSG, L80 | Live: `wsg-10v10-proof.ps1 -FixtureId awrole1 ... -MaxWaitMinutes 35 -Apply` (activate → `wsg queue` → `wsg status` / 10 s → `wsg leave` → deactivate). Needs `RandomBotJoinBG=0`, `AutoJoinBG=0` (current). | winner, score, caps/returns, K/D/dmg/heal per slot, queue→in_progress→first cap→complete times, roster retention | ≤ 37 min per match, ×6 |
| P1b WSG stock (alternative to P1) | all L80 randoms (≥ 10/faction) | mixed | WSG bracket 7 | Restart: `RandomBotJoinBG=1`, `RandomBotAutoJoinBG=1`, `RandomBotAutoJoinBGWSCount=1` | needs C1 `bg_join/bg_end` | 4 h |
| P2 STV skirmish | 8 (4 A + 4 H, L30-45, already in zone 33) | both | Stranglethorn 33 | Needs C3: `pvpflag <g> on`; rollback `off` | C1 `pvp_kill`/`death`: kills per flagged bot-hour, outcome by level gap and class, containment = 0 unflagged victims | 2 h |
| P3 AV | 80 L80 | 40 / 40 | AV | Dedicated step, not S2 | — | — |
| B0 solo stock | ~8 | per zone | same zones as B1-B3 | none | rewarded/bot-h, deaths, stalls, accept→reward time | 4 h |
| B0m solo matched | ~8 | per zone | same | `independent <g>` (needs L0 or C4) | same | 4 h |
| B1 duo | 4 × 2 | same faction, level spread ≤ 2 | same | `party <L> <M>` (live) | same + retention + shared-credit share | 4 h |
| B2 trio | 3 × 3 | same | same | `party <L> <M1> <M2>` (live) | same + group-quest completions | 4 h |
| B3 party-quest trio | 2 × 3 | same | same | `party` then `quest <L>` (needs L0 or C4) | common-quest completion, cohesion follow count | 4 h |
| B4 stock follow-groups | world | all | all | Restart `RandomBotGroupNearby=1` (global → a separate run, not a cell) | groups formed/h, rewarded/bot-h vs S2 | 4 h run |
| L-pilot (now, S1) | 10+11+15, 14+19, 17+25; controls 7, 18, 12 | A / A / H; H | Teldrassil / Dun Morogh / Durotar; Tirisfal / Eversong | `party 10 11 15`, `party 14 19`, `party 17 25`; rollback `.group disband <leader>` (console) or C2 | retention, rewarded/bot-h ABA | 90 min |

Scouts 101, 112, 121, 123, 236 and 244 are in no cell.
