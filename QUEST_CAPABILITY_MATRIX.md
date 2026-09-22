# Quest Capability Matrix — Playerbots quest execution audit

Date: 2026-07-13 (Asia/Jerusalem)
Scope: `azerothcore-wotlk/modules/mod-playerbots` (reorganized fork) + AutoWow bridge.
Method: read-only source audit. No file, build, server, or database was modified.

This document is **audit-first**. Its purpose is to expose and orchestrate the quest
capability that already exists in Playerbots, not to invent a new AI brain. It is the
input to the Quest Director V1 proposal at the end.

---

## 0. The two quest systems (read this first)

The fork carries **two parallel quest engines**. Confusing them is the single biggest
source of wrong conclusions, so they are separated everywhere below.

| System | Files | Drives | Master required? |
|---|---|---|---|
| **Legacy / master-driven** | `src/Ai/Base/Actions/{AcceptQuest,TalkToQuestGiver,QuestAction,DropQuest,...}` + `src/Ai/Base/Strategy/QuestStrategies.cpp` + classic `src/Mgr/Travel/TravelMgr.cpp` targeting | Chat-command and packet-triggered accept / turn-in / drop, and classic distance-sorted travel targets | Mostly **yes** — short-circuits without an owner/master |
| **New RPG autonomy** | `src/Ai/World/Rpg/Action/NewRpg*.cpp`, `NewRpgInfo.h`, `NewRpgStrategy.cpp` | The **full solo loop**: pick quest → route to a Quest POI → grind/loot at the POI → turn in → recover | **No** — a lone random bot runs the whole loop |

**The New RPG layer is the real autonomous quest engine.** It selects a quest, walks to
the DB Quest-POI point of an unfinished objective, and then *delegates the actual
kill/loot to the grind + loot strategies*. It never itself targets a mob, clicks a
GameObject, or uses a quest item — that delegation boundary is where most gaps below live.

### Bridge control surface today (`src/AutoWow/AutoWowBridge.cpp`, loopback `127.0.0.1:18787`)

`list · destinations · snapshot · activate · deactivate · party · rally · deploy · route · engage · scout · quest · recover · pause · resume · travel`

The **`quest`** order (`QuestParty`, `AutoWowBridge.cpp:664`) already does server-side quest
orchestration for a party leader: it prefers `QUEST_STATUS_COMPLETE` (turn-in) then
`QUEST_STATUS_INCOMPLETE` (objective), resolves a real `TravelMgr` quest destination, and
if none exists **falls back to the New-RPG `new rpg do quest` action rather than random
grind**; if even that is impossible it returns `phase:"blocked"` and idles. **`recover`**
(`RecoverQuestParty`, `:977`) resets transient movement/RPG state with no teleport, quest
edit, or random destination. Neither order writes quest/XP/item/objective tables.

The `quest`/`recover` orders are the **executor**; Quest Director V1 is the **external
closed loop** that decides *when* to issue them and *measures whether they worked*.

---

## 1. Capability matrix (10 categories)

Legend — Coverage: **FULL** = a lone bot completes it autonomously; **PARTIAL** = works
only opportunistically / with a master / same-map; **ABSENT** = no autonomous driver.

### 1. Accept / reward / talk  — **FULL (solo, via New RPG)**

| Field | Detail |
|---|---|
| Action(s) / class | Solo: `NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest` → `AcceptQuest` / `TurnInQuest` / `BestRewardIndex` (`NewRpgBaseAction.cpp:283,462,473,506`). Legacy: `AcceptQuestAction`, `TalkToQuestGiverAction::{ProcessQuest,TurnInQuest}`, `QuestAction::AcceptQuest` (`AcceptQuestAction.cpp:33`, `TalkToQuestGiverAction.cpp:19,73`, `QuestAction.cpp:217`). |
| Inputs / state | Questgiver NPC/GO within interaction range; status NONE→acceptable, COMPLETE→rewardable; `CanTakeQuest`/`CanAddQuest`/`CanRewardQuest`. |
| Solo / party / cross-map | Solo path needs no master. Legacy path needs owner/master (`AcceptQuestAction.cpp:35`). Same-map interaction only. |
| Verification quest (1–12) | **783 "A Threat Within"** (Human, Northshire) — pure talk accept + report turn-in, no objective; and the repo-proven **788 "Cutting Teeth" → 789** turn-in chain (Durotar). |
| Failure / stuck signatures | Legacy multi-reward stall: `"Reward pending"` when no item scores usable and `autoPickReward!="yes"` (`TalkToQuestGiverAction.cpp:196`). Range: `"Cannot talk to quest giver"` (`QuestAction.cpp:188`). |
| Recovery present | New-RPG `BestRewardIndex` is **deterministic (seeds index 0)** and never stalls. Walk-to-questgiver + `ForceToWait(5000)` retry. |

### 2. Kill objectives — **FULL (solo); PARTIAL group/cross-map**

| Field | Detail |
|---|---|
| Action(s) / class | Target pick `GrindTargetValue::FindTargetForGrinding` / `::needForQuest` (`GrindTargetValue.cpp:32,155`); POI navigation `NewRpgDoQuestAction::DoIncompleteQuest` (`NewRpgAction.cpp:276`); engagement via combat strategies. |
| Valid-entry whitelist | Yes — `needForQuest` matches `RequiredNpcOrGo[j]` against `target->GetEntry()` and requires `CreatureOrGOCount[j] < RequiredNpcOrGoCount[j]` (`GrindTargetValue.cpp:177`). Entry→quest map in `EntryQuestRelationMapValue` (`QuestValues.cpp:43`). |
| Progress measured? | **Yes** — `DoIncompleteQuest` reads `q_status.CreatureOrGOCount[obj]` vs required, clears objective, re-picks POI (`NewRpgAction.cpp:287`). |
| Solo / party / cross-map | Party-aware assist caps (`GrindTargetValue.cpp:22`). POI filtering is same-map & same-zone only (`NewRpgBaseAction.cpp:904,937`). |
| Verification quest (1–12) | **"Kobold Camp Cleanup" (Northshire)** — kill 10 Kobold Vermin (pure kill/count); repo-proven **457 "The Balance of Nature"** (Teldrassil) kill boars/nightsabers. |
| Failure / stuck | 5 min at POI with zero progression → `lowPriorityQuest.insert`, `questAbandoned++`, idle (`NewRpgAction.cpp:352`). Move-stuck → teleport (`NewRpgBaseAction.cpp:92`). |
| Recovery present | Quest de-prioritized (not dropped); movement teleport-recovery; random-near nudge on path fail. |

### 3. Loot / item-drop objectives — **FULL**

| Field | Detail |
|---|---|
| Action(s) / class | `AddLootAction`/`AddAllLootAction` (`AddLootAction.cpp:17`), `LootAction`+`OpenLootAction`+`StoreLootAction` (`LootAction.cpp:21,73,353`), whitelist `StoreLootAction::IsLootAllowed` (`LootAction.cpp:466`). |
| Valid-item whitelist | Yes — allowed if item `StartQuest` set or any active `quest->RequiredItemId[i]==itemid` (`LootAction.cpp:483`); drop→mob reverse map `DropMapValue`/`ItemDropListValue` (`LootValues.cpp:63,99`); `HaveQuestLootForPlayer` flags drop mobs (`GrindTargetValue.cpp:193`). |
| Progress measured? | Indirect — counted via `q_status.ItemCount` in the objective check (`NewRpgAction.cpp:293`); loot self-caps on `HasItemCount` (`LootAction.cpp:479`). |
| Solo / party / cross-map | Respects group loot method / FFA; guards ninja-loot of flagged chests (`LootAction.cpp:51,143`). Local map only. |
| Verification quest (1–12) | Repo-proven **789 "Sting of the Scorpid"** (Durotar) — kill + loot Scorpid Stinger; **"Beer Basted Boar Ribs"** (Coldridge) collect Chunk of Boar Meat from corpses. |
| Failure / stuck | Same POI-abandon path as kill; corpse released on skip (`LootAction.cpp:459`). |
| Recovery present | Yes (loot retry + POI abandon). |

### 4. GameObject interaction — **PARTIAL** (giver/loot GOs work; "use GO for credit" ABSENT)

| Field | Detail |
|---|---|
| Works | Quest-giver/taker GOs: `InteractWithNpcOrGameObjectForQuest` handles `TYPEID_GAMEOBJECT` (`NewRpgBaseAction.cpp:388`). Lootable/gatherable quest chests via the loot pipeline (`LootAction.cpp:135`). |
| Absent | **No autonomous action `Use()`s a quest-objective GameObject** (lever / brazier / "click the object"). `UseItemAction::UseGameObject` (`UseItemAction.cpp:45`) is reachable only via a chat `!use` command; the DoQuest/grind engines never click objective GOs, and `GrindTargetValue` only returns `Unit*`. |
| Progress measured? | Only for loot/gather GOs (via ItemCount). Pure "use GO" objectives: no driver → no measurement. |
| Verification quest (1–12) | Any "use/click the object" objective, e.g. a Dun Morogh / Elwynn quest whose objective line is *Use* a world GameObject (bonfire, lever). Expect the bot to route to the POI and idle without clicking → exposes the gap. |
| Failure / stuck | Manifests as POI-reached-but-no-progress → 5-min abandon. |

### 5. Quest-item use (use provided item on target/area) — **ABSENT** (autonomous)

| Field | Detail |
|---|---|
| What exists | `UseItemAction::UseItem` (`UseItemAction.cpp:67`) supports GO/item/unit targets **only when a caller supplies the target**. The only autonomous quest-item action `UseRandomQuestItem` (`:456`) fires **only for items whose `proto->StartQuest` is set** (items that *begin* a quest), used on self. |
| Absent | No logic that binds an active objective → provided quest item → valid target (e.g. "use the Blackjack on the peon", "throw the net on the murloc"). |
| Progress measured? | N/A — no driver. |
| Verification quest (1–12) | **5441 "Lazy Peons"** (Orc, Valley of Trials) — *use Foreman's Blackjack on 4 sleeping peons*. Canonical quest-item-use objective; the engine will route/idle but not wake peons → exposes the gap. |

### 6. Escort / event / scripted quests — **ABSENT**

| Field | Detail |
|---|---|
| Finding | **Zero** `escort` occurrences under `src/`. No follow-escort-NPC, no timed-event, no gossip-triggered event beyond plain accept/turn-in. |
| Active exclusion | `IsQuestCapableDoing` rejects `GetType()!=0 && GetSuggestedPlayers()>=2` (`NewRpgBaseAction.cpp:561`); `OrganizeQuestLog` **drops** such quests (`:602`). Escort/event quests are not attempted and often discarded. |
| Progress measured? | No — persisted `playercount` exists in `character_queststatus` but nothing drives or reads it for escorts. |
| Verification quest (1–12) | **"Escorting Erland"** (Silverpine Forest, ~lvl 11) or any `SPECIAL_FLAGS` escort. Expect the quest to be dropped from the log → exposes the gap. |

### 7. Collect / profession / gathering — **PARTIAL** (opportunistic, not objective-directed)

| Field | Detail |
|---|---|
| Works | Mechanical gather/skin/mine/fish: `AddGatheringLootAction` (`AddLootAction.cpp:47`), gather-spell open in `OpenLootAction::DoLoot` (Mining 2575 / Herb 2366 / skinning, `LootAction.cpp:118`), strategies `GatherStrategy`/`RevealStrategy`/`UseBobberStrategy` (`LootNonCombatStrategy.cpp:20`). |
| Weak / absent | Not tied to a quest objective — gathering is **opportunistic** (only nodes in loot range while parked/grinding). No autonomous *seeking* of the required nodes. `RevealGatheringItemAction` requires a group and only pings/emotes ("Should we go nearer?", `RevealGatheringItemAction.cpp:20`). |
| Progress measured? | Only if the gathered item is a quest item (generic `q_status.ItemCount`); no gather-specific targeting/counting. |
| Verification quest (1–12) | Corpse-collect works: **"Beer Basted Boar Ribs"**. Node-directed gather (rare ≤12): any "gather N herb/ore" quest — expect no directed node-seeking → exposes the weak spot. |

### 8. Travel & cross-zone handoffs — **PARTIAL** (objective-specific same-map: yes; cross-map: no via TravelStrategy)

| Field | Detail |
|---|---|
| Objective-specific routing | **Yes** — `QuestObjectiveTravelDestination` (`TravelMgr.h:637`, built `TravelMgr.cpp:2063`) routes to the DB **spawn coordinates** of the exact `RequiredNpcOrGo`/`RequiredItemId` entries, gated on remaining count (`isActive:1219`, `getObjectiveStatus:3962`). `SetQuestTarget` (`ChooseTravelTargetAction.cpp:473`) walks the bot's own quest log. Grind/rpg/boss are RNG-gated fallbacks only. |
| POI source | Live DB creature/GO **spawns** of the required entries (`QuestValues.cpp:66,93`) — *not* the `quest_poi` table. Dynamic/scripted/phased/summoned targets → zero points → silently unroutable. |
| Cross-map | **PARTIAL.** Target *selection* is cross-map aware (`maxDistance=0`, map-transfer distance). But `MoveToTravelTargetAction`→`MoveTo(mapId,...)` is **same-map pathing only** — no flight/boat/portal. Flight-master routing (`TravelMgr::GetOptimalFlightDestinations:4406`, `TaxiAction`) is wired **only into the New RPG strategy** (`NewRpgBaseAction.cpp:1055`), not `TravelStrategy`. Only hardcoded bridge: Dark Portal (`TravelAction.cpp:64`). |
| Solo / party | Independent — `SetGroupTarget` merely copies a groupmate's destination if present; `isUseful` has no master requirement (`ChooseTravelTargetAction.cpp:882`). Lone bot routes to its own objectives. |
| Verification quest (1–12) | Same-zone objective route: **788/789** (Durotar) — proven. Cross-zone handoff: any quest whose turn-in is in an adjacent zone/city (e.g. Durotar → Orgrimmar) — expect stall under `TravelStrategy`, success only via New-RPG taxi. |
| Failure / stuck | No-path → `incRetry`, `>5` → `TRAVEL_STATUS_COOLDOWN`; time budget overrun → `TRAVEL_STATUS_EXPIRED`; objective count met → cooldown. Greedy nearest-point selection (acknowledged TODO, `ChooseTravelTargetAction.cpp:38`). |

### 9. Dungeon / group quests — **ABSENT** (objective work)

| Field | Detail |
|---|---|
| What exists | `LfgStrategy` (`LfgStrategy.cpp:11`) — only `lfg join`/`lfg leave`/`give leader in dungeon`. Generic combat assist (tank/heal/dps) works inside instances. |
| Absent | **No in-instance quest-objective logic.** Group/dungeon/elite quests are excluded by `IsQuestCapableDoing` and dropped by `OrganizeQuestLog` (`NewRpgBaseAction.cpp:561,602`). |
| Verification quest (1–12) | The earliest dungeon (Ragefire Chasm) is ~lvl 13, just above the band; below 12 use any elite outdoor group quest. Expect exclusion/drop → exposes the gap. |

### 10. Abandon / retry / stuck recovery — **FULL (solo, via New RPG); legacy needs master**

| Field | Detail |
|---|---|
| Action(s) / class | RPG stuck-POI abandon `NewRpgDoQuestAction::DoIncompleteQuest` / `DoCompletedQuest` (`NewRpgAction.cpp:352,446`); movement teleport `MoveFarTo` (`NewRpgBaseAction.cpp:92`); log overflow `OrganizeQuestLog` (`:578`); failed-timer `QuestUpdateFailedTimerAction`→`AbandonQuest` (`QuestAction.cpp:441`). Legacy manual: `DropQuestAction` (needs master, `DropQuestAction.cpp:19`), `CleanQuestLogAction` (`:68`). |
| Trigger / inputs | POI reached + `poiStayTime`(~5 min) + no progression; or `stuckAttempts>=5` with <5y movement; or <2 free log slots; or failed-timer packet. |
| Solo / party | Solo recovery via New RPG; legacy drop/clean short-circuits without a master. |
| Verification quest (1–12) | Stage any quest whose objective spawn is dead/instanced, observe `lowPriorityQuest` insertion + idle within ~5 min. |
| Failure / stuck signatures | Log `"[New RPG] {} marked as abandoned quest"`, `"[New RPG] Teleport ... as it stuck when moving far"`. `questAbandoned++`. |
| Recovery present | Yes, but see risks below (`lowPriorityQuest` is permanent/in-memory; no backoff). |

---

## 2. Cross-cutting findings

- **Quest & objective selection is random, not nearest/priority.** New-RPG picks a viable
  quest by `urand` and a POI by `urand` + randomized-weighted centroid
  (`NewRpgBaseAction.cpp:1138`, `NewRpgAction.cpp:315`). Classic travel is distance-greedy
  with an explicit "rewrite to be more intelligent" TODO (`ChooseTravelTargetAction.cpp:38`).
- **`lowPriorityQuest` is a near-permanent, in-memory, per-bot skip set** with no retry
  backoff (TODO at `NewRpgAction.cpp:374`). One transient blockage → the quest is skipped
  for the rest of the session. **This is the primary lever a Director must manage externally.**
- **Reward determinism differs by path.** New-RPG `BestRewardIndex` never stalls; legacy
  `RewardMultipleItem`/`BestRewards` can emit `"Reward pending"` and wait for a master.
- **Quests with no spawned required entry / no `QuestPOIVector` are silently un-doable** and
  loop idle (Cat 4/6 gaps, dynamic spawns).
- **Latent bug (classic path):** `ActiveQuestObjectivesValue::Calculate`
  (`QuestValues.cpp:279`) tests kill objectives against `RequiredItemCount[obj]` (copy-paste
  from the item branch) instead of `RequiredNpcOrGoCount[obj]`; for a pure-kill objective
  that is 0, so the objective is skipped — classic-travel kill-objective targeting is broken.
  The New-RPG path counts correctly (`NewRpgAction.cpp:289`), so the live proof still works.

**Net:** the autonomous quest spine is real and covers the *common* early-level loop —
accept → route to kill/loot objective → measure count → turn in → recover — for solo bots
and, via the bridge `quest` order, for parties. The gaps are **use-GO-for-credit,
use-quest-item-on-target, escort/event, and dungeon/group objectives**, plus **cross-map
travel under the classic strategy** and **directed gathering**.

---

## 3. Non-destructive quest telemetry harness

Delivered: **`scripts/quest-telemetry.ps1`** (read-only). It is the observation half of
Quest Director V1 and can run today against a live server without touching any bot.

What it does each cycle:
1. Bridge **`list`** (read-only) → live per-bot `xp`, `level`, `money`, `position`, `combat`,
   `travel.status/destination`.
2. Read-only MySQL (`SELECT`-only, cross-schema, credentials read from `worldserver.conf`
   `CharacterDatabaseInfo`/`WorldDatabaseInfo`, never emitted) joining
   `character_queststatus` (`mobcount1..4`, `itemcount1..6`, `playercount`, `explored`,
   `status`) to `quest_template` (`RequiredNpcOrGo1..4[+Count]`, `RequiredItemId1..6[+Count]`,
   `QuestType`, `Flags`, `LogTitle`) and `quest_template_addon.SpecialFlags`, plus a
   `character_queststatus_rewarded` count.
3. Builds a **per-objective vector** — for each slot: `have` vs `required`, whether the
   target is a creature (`RequiredNpcOrGo>0`) or GameObject (`<0`), and a percent. This is
   both the progress signal **and** the valid-target-entry whitelist.
4. Computes **deltas** vs the previous cycle (xp, rewarded count, per-objective counts,
   distance moved, map change) and applies a **no-progress detector**: if over a window all
   of {xp, objective counts, rewarded count} are flat, the bot is not in combat, and it has
   barely moved, it emits `no_progress` with an explicit reason.
5. Appends JSONL receipts (same idiom as `league-telemetry.ps1`).

**Guardrails enforced in code:** every SQL string is asserted to begin with `SELECT` and to
contain no mutation keyword (`INSERT/UPDATE/DELETE/REPLACE/DROP/ALTER/CREATE/TRUNCATE/GRANT/
SET/CALL`). The harness issues **no** bridge control order (no `pause/resume/travel/quest/
recover`) — only the read-only `list`. It never writes character/quest/XP/item/objective
rows. It does not enable random movement.

**Known limitation (and why it justifies one bridge endpoint):** `character_queststatus` is
flushed to disk on save (periodic + logout), so mid-run objective counters **lag** live
memory. The harness reports this and timestamps DB vs bridge samples separately. The only
clean fix for live objective counters is a **read-only** bridge `questlog <guid>` endpoint
(see §4).

Usage:

```powershell
# Track explicit bots
$env:MYSQL_PWD is NOT used; creds come from worldserver.conf and are never printed.
.\scripts\quest-telemetry.ps1 -BotGuid 7,10 -DurationMinutes 120 -PollSeconds 20

# Track the whole league roster (autowow_league_member)
.\scripts\quest-telemetry.ps1 -DurationMinutes 120
```

---

## 4. Quest Director V1 — proposal

Quest Director V1 is an **external closed loop** (PowerShell, alongside the existing agents)
that orchestrates the *already-present* Playerbots capability. It does **not** add an AI
brain to the server. It reuses the bridge `quest`/`recover` orders as the executor and the
telemetry harness as the sensor.

### Control loop (per party, per cycle)

1. **Choose a party-compatible active quest.** From telemetry, rank the leader's
   `getQuestStatusMap` quests that (a) every alive member can hold/complete, (b) are
   `QuestType==0` and `SuggestedPlayers<2` (mirrors `IsQuestCapableDoing` so we don't pick
   what the engine will drop), (c) are **not** currently in the bot's skip set, preferring
   `COMPLETE` (turn-in) then the objective with the highest partial progress. This replaces
   the engine's `urand` pick with a deterministic, progress-greedy choice **made externally**
   — the engine still executes.
2. **Route specifically to its objective.** Issue the bridge **`quest`** order. That already
   resolves the `QuestObjectiveTravelDestination` (kill/loot spawns of the exact required
   entries) or, if none, the New-RPG POI — never generic grind.
3. **Whitelist valid target entries.** For the chosen objective, telemetry yields the exact
   `RequiredNpcOrGo` entries (creature vs GO by sign) and required counts. V1 uses this as an
   *observation/verification* whitelist (the server-side grind already entry-checks via
   `needForQuest`). A future minimal bridge order could pass the whitelist to constrain
   engagement, but V1 does not need it.
4. **Measure actual objective counters every cycle.** Read `mobcount/itemcount/playercount`
   vs required from telemetry. Compute per-objective delta.
5. **Detect no-progress states.** If, over a bounded window, the chosen objective's counter,
   the leader's XP, and the rewarded-count are all flat and the party is not in combat →
   `no_progress`.
6. **Retry → reroute → switch → idle, with an explicit reason.** On `no_progress`:
   a. **Retry:** re-issue `quest` once (re-resolves a POI point).
   b. **Reroute:** issue **`recover`** (resets transient RPG/movement state, no teleport/
      random), then `quest` again — this is the sanctioned un-stick.
   c. **Switch:** if still flat after K attempts, mark the quest in the Director's **own**
      external skip ledger (with a timestamp + backoff, unlike the engine's permanent
      in-memory set) and choose the next best quest.
   d. **Idle with reason:** if no party-compatible actionable quest remains, stop issuing
      orders and record `idle{reason}` (e.g. `all_quests_backed_off`, `objective_absent_driver`,
      `cross_map_required`). **Never** fall back to random movement or generic grind.

### The one minimal bridge endpoint V1 needs (read-only)

Everything above works with today's bridge **except** live objective measurement: the
snapshot exposes no quest counters and the DB lags. Proposed **read-only** addition,
isolated to a new source file under `src/AutoWow/` (not editing `AutoWowBridge.cpp` beyond a
one-line dispatch case):

```
questlog <bot-guid>   ->   { ok, guid, quests:[ { id, status, title,
                              objectives:[ { type:"npc"|"go"|"item"|"player",
                                             entry, have, required } ] } ] }
```

It reads live `QuestStatusData` on the world thread (mirror of `NewRpgAction.cpp:287`
counting) and returns counters. It performs **no** mutation. This is the "demonstrably
necessary minimal bridge endpoint" the task allows: it removes DB lag and makes the
no-progress detector accurate. Until it exists, V1 runs on lagged DB counters (usable, just
slower to react). This keeps Director logic isolated from `AutoWowBridge.cpp` internals.

### Explicitly out of scope for V1 (documented gaps, not silent)

Because the engine has **no driver** for them, V1 must *recognize and idle* on — never fake —
these objective types: use-GameObject-for-credit (Cat 4), use-quest-item-on-target (Cat 5),
escort/event/scripted (Cat 6), dungeon/group (Cat 9), directed gathering (Cat 7), and
cross-map objectives under classic travel (Cat 8). V1 detects these from the objective
signature (GO entry with no loot, `SpecialFlags` exploration/event bit, `QuestInfoID` in an
escort/dungeon/raid category, required entry
on another map) and classifies them as `unsupported_objective` with the specific reason, so
the roadmap for a Quest Director V2 (which *would* wire these drivers) is data-driven.

### Constraints honored

- **No direct writes** to character quest / XP / money / item / objective tables — Director
  reads telemetry and issues only `quest`/`recover` (which themselves delegate to Playerbots
  actions and never edit rows).
- **No random-movement fallback** — the terminal state is explicit `idle{reason}`.
- **Isolation from `AutoWowBridge.cpp`** — V1 is external PowerShell; the only proposed C++
  touch is a self-contained read-only `questlog` endpoint whose necessity is demonstrated
  above, added as a new file + one dispatch line.

### Suggested build order (post-audit)

1. Ship the telemetry harness (done) and baseline a multi-hour run on the two proven parties
   (guids 7, 10) to get a real progress-rate and stuck-rate distribution.
2. Add the read-only `questlog` endpoint; switch the no-progress detector to live counters.
3. Implement the Director control loop (choose→quest→measure→recover→switch→idle) as
   `scripts/quest-director.ps1`, driving parties through the FULL categories (1,2,3,10) and
   idling-with-reason on the rest.
4. Fix the classic-path `RequiredItemCount` kill-objective bug (`QuestValues.cpp:279`) — a
   one-line correctness fix independent of the Director.
```
