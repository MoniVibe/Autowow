# Managed-bot diagnosis: soak-s3-150-r1 (2026-09-23)

Module repo `/root/autowow-advisor-t1-module`, branch `managed-diag` (from main 3d6a5dbe).
Commits: c0d88d12 (ledger honesty), 9e0c0a08 (fix 1), 82f40d59 (fix 2). Not pushed or merged.
The running worldserver was not rebuilt or relinked: sha256 ed538d06dced75b185e4bfd46acfeac9367879d59339e9477b82b5bf040044b0, checked after every step.

## Evidence used
- The reduced ledger (`work/soak-20260923/reduce-s3`, `reduce-s1`) and the raw `/root/autowow-soak/logs/ledger.log`.
- Oracle receipts for the S3 process (pid 93100): `C:\dev\logs\autowow-oracle\oracle-receipts-oracle-93100-*-0.jsonl`. This is the runtime's ReceiptExport directory, relative to the worldserver's cwd `/mnt/c/dev`.
- Read-only bridge calls, spaced at least 2.1 s apart: `snapshot`, `questlog` and `questobjective` for 3 to 244.
- Live config: `/usr/local/etc/modules/playerbots.conf`. Settings: SliceBots=8, MaxBots=27, LeaseTtlTicks=3, ZoneTravelAssist=1, RouteV2=1, AutoTeleportForLevel=1, Soak.RerollAboveMaxLevel=1.

## What the managed bots were doing (live, around world tick 1700-2100)
There are 27 managed GUIDs. 139, 144, 154 and 166 were offline. 16 only did PvP. The remaining 22 bots fall into these groups:

| state | bots | what the evidence shows |
|---|---|---|
| **A. finisher hang (no frame)** | 3, 6, 9, 11, 12, 14, 15, 19, 35 | The directive is a *completed* quest. The finisher only exists on another map (35 is an exception, see Residual). `questobjective` reports finisher `resolve_finisher`, signed_entry 0, and the snapshot says `runtime_state_not_published`. There are no receipts and no ledger rows at all (9 of these bots have zero ledger rows for the whole run). |
| **B. objective hang (no candidate)** | 7, 10 (whole run); 4, 18 (end of run) | The directive's objective sources are all on another map. Receipts for 7 and 10 are 2096/2096 and 2094/2094 `no_eligible_intent`. 18 sits in Undercity with Durotar quest 784 and phase resolve_objective forever. |
| **C. no directive** | 112, 121, 123, 236, 244 | Every quest in the log was deferred (`oracle_unrunnable` or `inventory_full`). dq=0, the bot stands still, and it accepts no new quests. |
| **D. route-block churn** | 5, 17, 18 (first 760 s), 25, 112 | Same-map but far or cross-zone quests. Each gets `oracle_route_blocked` about every 32 s, rotating through the log with 30/120/600 s backoff, and the bot's position never changes (18 stayed at 1803,68,z=-47 in Undercity for 761 s). |
| progressing | 101 | Webwood Venom, `no_live_candidate` plus ZoneTravelAssist. |

Why the managed ledger rows are 0 rewarded: 29 of the 51 rows are group D, and groups A, B and C emit nothing, so they are not in the 51 rows at all. Raw S3 does contain 2 managed rewards (bots 5 and 25). The reducer scored both rows as contaminated because a zone_travel_assist teleport happened earlier in them.

## Root causes
1. **Location-blind directive selection plus cross-map fail-closed with no exit (bug).** `EnsureQuestDirective` picks the lowest quest id, completed quests first, with no map or zone filter. When the chosen quest has no same-map target, `ResolveQuestBootstrapIdentity` (same-map only) or `ActiveQuestFinisherValue` ("never admits a remote-map spawn") returns nothing. The quest frame is then either unbuildable (A) or empty (B). Neither case calls BlockQuest or deferral, so the directive is kept forever and the bot idles silently. This is the single largest cause: 13 of the 22 active managed bots.
2. **The stock random teleport displaces managed bots from their quest logs.** Managed GUIDs are still in RandomPlayerbotMgr's pool. The level-up `AutoTeleportForLevel` and the scheduled teleport-for-level move them to other zones and continents. Ledger `rndbot_teleport` rows appear for 4, 17 (x4) and 25 during S3. Bot 11 did Teldrassil quests in S1 and was in Dun Morogh in S3. All S3 managed quests were already in the log at start (accepted_at null). This is what feeds causes 1 and 3.
3. **No local acquisition and no cross-zone travel for managed bots (design gap, not fixed).** When nothing in the log is runnable, the bot has no directive. The Oracle's strategy set (`-travel,-move random`) and the managed dispatch policy (`AllowsLegacyQuestMaintenance` is false) leave it inert (group C). Cross-zone routes block without saying why. `ZoneTravelAssist` requires sourceZone == targetZone, so for example Undercity 1497 to Tirisfal 85 is never assisted. The RouteV2 failure code is only logged at DEBUG (`[AutoWow Oracle T1 RouteReceipt]`), and receipts record `route.path_status` as null, so the exact route failure class is **UNKNOWN** from the S3 artifacts.
4. **B3 slicing and B1 const-ref: no evidence against them.** Receipts show a normal lease cycle under SliceBots=8: acquired 438, renewed 10461, released 428, `not_owner` 9. No lease-expiry churn appears. Every failure above has a mechanism that does not involve slicing or the guidp map read, and stock bots rewarded 51 quests through the same guidp map. This is not an A/B proof. If it matters, run S3 again with SliceBots=0.

## Changes (branch managed-diag)
- **c0d88d12 ledger honesty (no behavior change, gated by AutoWow.Ledger.Enable).** Adds a `contaminated` row immediately before these teleports:
  - `rpg_stuck_teleport`: the NewRpgBaseAction MoveFarTo stuck fallback that stock bots use.
  - `corpse_run_teleport`: ReviveFromCorpse inactive-bot teleport.
  - `ghost_graveyard_teleport`: spirit-healer teleport when walking fails.
  - `arena_team_teleport`
  - `bridge_rally_teleport`, `bridge_route_teleport`, `probe_reset_teleport`
  - `dungeon_ground_reattach`: 3 NearTeleportTo sites.

  Rows that already existed: rndbot_teleport, rndbot_revive, rndbot_randomize, setup_reroll, zone_travel_assist. Left unmarked: taxi, transports, the ordinary repop to the graveyard, and meeting-stone summons.
- **9e0c0a08 fix 1, `AutoWow.OracleRuntime.DeferNoCandidatePasses` (default 0 = legacy).** Counts consecutive lease-less passes for alive bots on the same quest (NoFrame / NoCandidate). At N passes it runs the existing unrunnable deferral: low-priority set plus ledger `deferred` with reason `oracle_no_frame` / `oracle_no_candidate`. Quest state is untouched. Suggested N = 5.
- **82f40d59 fix 2, `AutoWow.OracleRuntime.NoRandomTeleport` (default 0).** `RandomPlayerbotMgr::RandomTeleport` returns early for managed bots. With the flag off it does not touch the runtime.

## Verification
- All 13 touched module TUs were compiled with the build's exact flags into `/tmp/mdiag`; the build tree is untouched.
- unit_tests was linked privately against a copy of libmodules.a with the rebuilt members swapped in.
- Result: 12297 tests, 6807 passed, 5486 skipped, 4 failed. All 26 AutoWowOracleRuntimePolicyTest tests pass, including 2 new ones.
- The 4 failures also fail in the pre-change `build/src/test/unit_tests` and are unrelated:
  - QuestObjectiveResolutionSourceContract.ExplicitBridgePersistsDeferredNewRpgDirective
  - QuestFinisherBranchWiringContractTest.OracleOptInControlsStrictAndNoTeleportArguments
  - QuestAcquisitionContractTest.BridgeUsesExactPersistentTravelAndNormalOpcode
  - OracleExecutorTypesTest.RejectsUnknownAndContractBlockedOperations
- A full `ninja unit_tests` was not run, because it would mean 1557 steps (pch plus core R1/R2).
- Neither fix has been proven live. That needs a worldserver rebuild and a soak with both flags on.

## Residual / next
- After fix 1 the group A and B bots will turn into group C (idle with no directive) unless cause 3 is fixed as well. Pass-rate gains need either local acquisition for managed bots when there is no directive, or fix 2 turned on before quests are orphaned.
- To get the route failure class, promote the RouteReceipt log to INFO or add `route_failure` to the ledger (the latter needs a schema bump).
- ZoneTravelAssist is same-zone only. Undercity and other multi-level cities need a cross-zone exit, and that must be logged as contaminated.
- The ledger reducer should decide whether the `bridge_*` and `probe_reset_teleport` reasons count as SETUP (voided) rather than contaminated.
- Bot 35 (quest 11214, Dustwallow, same map) also has an unresolved finisher. The cause is unknown (faction gate?).
- `RandomPlayerbotMgr::RandomTeleport(Player*)` has no callers. It relocates the bot through `UpdatePosition` and never uses the locations it computes. It is dead code.
