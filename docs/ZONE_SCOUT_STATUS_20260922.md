# AutoWoW headless scout handoff

Updated 2026-09-22 12:34 UTC.

## User wrap-up boundary

The user asked to wrap up development and decide next steps. Implementation, builds, deployments, new repair workers and economy experiments are HELD until the user resumes. The realm and read-only collector remain running. Do not ask for an observer login. All workers have finished or frozen their work; no build is running.

Preserve native character assets and quest credit, physical travel between zones, the parked historical Autopilot job and interim manifest. No external API spending or old Quest Director alongside Oracle.

## Current deployed runtime

Project D:/Games/wowstuff/AutoWoW and WSL trees are archived source without Git. Ubuntu-24.04 active module /root/autowow-advisor-t1-module; core /root/autowow-advisor-t1-core; build /root/autowow-advisor-t1-build.
World PID108382, binary /root/autowow-advisor-t1-build/src/server/apps/worldserver, SHA256317d6e7313327b2e6ce4cb5a6795c480042b6f740fd5f4716c09793b7ea73e91. This binary was not changed during wrap-up.
All six saved scouts online: 101Zathis,112Naomini,121Musliwho,123Aenstus,236Nolliano,244Ugrilok. Online does not imply alive or progressing.
World config /root/p1runtime/worldserver.conf; module config /usr/local/etc/modules/playerbots.conf. Never publish full private configs.
1.5x player movement (run10.5/walk3.75), NPCmovement1, PvE XP5 and doubled eligible quest drops/cap100 preserved.
Character autosave interval900000ms; first save is randomized to0.5–1.5x this interval. Live counters and saved DB proof are separate.
MySQL/auth retained. Read fresh work/phase1-wsl-runtime/runtime-processes.json and validate processes before any future stop; wrapper PIDs can be reused after exit. Build only -j2.

## Deployed verification and progress

116/116 native tests in seven suites: work/zone-scouts-20260922/storage-attack-regression-tests.json. Revival10/10, observer18/18. These results predate and do not cover the draft below. Known gcov stale-checksum warnings did not change exit0; do not delete broad trees to chase them.
Native suites: QuestObjectiveResolution, QuestSourceRotationPolicy, AutoWowOracleRuntimePolicyTest, AutoWowOracleZoneTravelAssistPolicyTest, OracleQuestExecutorTest, QuestFinisherBehaviorPolicyTest, ExactQuestAttackRecoveryPolicyTest.

Immutable baseline at11:13:03 UTC: work/zone-scouts-20260922/overnight-progress-baseline.json.
Latest capture12:32:24: seven new saved rewards (Naomini6/35/40/47/54; Musliwho745; Nolliano367).
Levels: Zathis9→9; Naomini9→13; Musliwho6→11; Aenstus6→7; Nolliano8→10; Ugrilok7→7. Naomini46/52 objectives complete but saved rewards not observed. Musliwho748 counters6/6,0/4; conversion quest746 unsupported/deferred.
Normal gray sales recovered Naomini from full bags: naomini-inventory-recovery-proof.json.
Nolliano purchased ordinary six-slot bag4496 in slot19 for475c, occupancy100→72. Saved DB independently confirms new itemGUID867691 and quest items11127x4,2855x2,2859x1 at12:25. See nolliano-bag-purchase-proof.json. Saved copper2105 includes later loot, so its aggregate change is not isolated purchase cost.
Deployed purchase policy requires incomplete item quest, fresh full bags, no safe-gray relief, empty bag slot, real stocked nearby same-zone vendor, ordinary bag>=6slots, price<=500c, reserve>=500c, one attempt/10minute cooldown. No direct item/money grants.

## Frozen draft — do not build or deploy automatically

Sol zone_assist froze three textually complete changes in the active module:
- src/AutoWow/AutoWowOracleRuntime.cpp
- NEW src/AutoWow/AutoWowOracleExactHandoffPolicy.h
- tests/AutoWowOracleRuntimePolicyTest.cpp

DRAFT / UNBUILT / UNTESTED / UNDEPLOYED. The live binary remains the earlier317d build. Preimages, exact diffs, new-header copy and HANDOFF.md are in work/zone-scouts-20260922/lease-handoff-before. The user-visible source ZIP intentionally represents the deployed12:08 files, not this draft.

Diagnosis: a TravelToSource→AcquireTarget phase change releases the prior lease and clears the exact route pin; generic bootstrap can also select a different first spawn than the arrived route. The draft validates the arrived quest/objective/source/map/instance, uses that source for the fresh frame and restores its pin only after a successful new decision. Strict SameIntentIdentity is unchanged. The single focused test includes mismatched bootstrap/source, release/grant failures, changed runtime source/instance/decision and target stability. No test was executed for this draft. Live acquisition, subsequent combat continuity and q916 credit must all be proven after review/build/deploy when authorized again.

## Remaining issues / next priorities

1. Zathis101: stillq9160/10 and no XP gain. See ZATHIS_ROUTE_FOLLOWUP.md and draft HANDOFF.md. Earlier LOS-rejection inference was unsupported; no exact-attack rejection logs appeared because its pinned branch was bypassed. Creature selected_source_spawn.grid_loaded/spawn_present fields are hardwired false in that diagnostic; use selected_target.loaded.
2. Ugrilok244: q818 Stalled, q4402 DeadlineExceeded after real routes/death; three blocks caused reentry backoff. q4402 retried12:24. Latest-only per-bot handoff prevents olderq818 receiving a qualifying retry signal. See UGRILOK_PROGRESS_DIAGNOSIS.md. Smallest later repair: bounded per-route/per-quest reentry facts, preserving timers and route safety. Physical cause of failed travel remains UNKNOWN.
3. Aenstus123: q8346 spell-credit marker15468 cannot be executed as a kill. Resolver correctly defers it, XP progresses, new quest/zone progression unproven. Generic questlog supported/kill label is not executor support.
4. Turn-in/death recovery: latest collector reports Naomini repeated_failure and Nolliano dead after earlier collection progress. Do not infer complete lifecycle reliability from earlier quest rewards.

Recommended next milestone: a two-hour unattended adventurer loop with saved quest turn-ins, normal inventory upkeep, death recovery and visible bounded deferrals. Then explicit supported zone plans and physical zone graduation. Economy/AH and RTS should build on this loop.

## Read-only monitoring

Collector PID23388, run20260922-121212-4504c8fc, configured480minutes from12:12 through20:12UTC. logs/zone-scouts/latest.json points to current samples/incidents. Latest12:32: six online, one dead, four suspect, no telemetry errors in this collector run. Earlier logs preserved; rotating an observer does not establish incident recovery.
Collector controls: scripts/zone-scout-session.ps1 -Action Status|Start|Stop. Stop writes operator-paused.json and leaves bots playing; explicit Start clears that marker. Never duplicate collectors or revive a deliberately stopped realm.
CORRECTION: automation_update on12:31 returned that autowow-zone-scouts does not exist; no matching automation.toml exists. Earlier claims of an active15-minute app heartbeat were not verified and are superseded. Current local observation is proven; scheduled review/renewal is not active or proven. Do not silently recreate an absent automation during user wrap-up.
Read-only capture: scripts/capture-zone-scout-progress.ps1; scripts/capture-scout-inventory.ps1. Preserve the immutable baseline and unique history. heartbeat-state.json records implementation held and current evidence for any later resumed review.

To park the six later, use scripts/revive-autowow.ps1 -Action Park -Apply -RosterGuids @(101,112,121,123,236,244). Default roster remains original three. Save/park before linking a future worldserver; validate owned processes before using stop helper. Start with same explicit roster after deployment.

## Artifacts, economy and backups

User report: C:/Users/shonh/Documents/Codex/2026-09-22/azerothcore-first-makes-much-more-sense/outputs/AutoWoW-Overnight-Report.md.
Deployed source archive beside report:29 selected sources/helpers plus manifest and116-test evidence; no configs/credentials/DB/binary. It requires existing AutoWoW tree. SHA55983AC06B3A099DBD1BCE473B3BC4ABDA55B182C14D2D1022B7DEFB75996810.
ECONOMY_AUDIT.md documents normal vendor proof and a bounded separate-staging AH test. No AH module/verified Oracle executor was active at audit; no auction transaction test has run.

Verified world binary backups under /root/autowow-zone-scout-backup-20260922:
- before-zone-scouts13877573cb6da632ff15853870a186d6979e9b3e6db9cd0f2f25763c2db09c0d
- before-maintenance e3a20b15357667d82f8daef1d1c0f81af974b308dd74c0c2ebacb384a9d41de3
- before-final-repair83fcde97d66b06c5ba9a428abbe7f23eac00df4789bf4c91e70a43d0d9f463bf
- before-storage-attack100985e0dd223afc18363e6fb23a1e73564326ce942853e8cfa80c402e684fb9
- before-lease-handoff317d6e7313327b2e6ce4cb5a6795c480042b6f740fd5f4716c09793b7ea73e91
The private pre-movement config backup contains credentials. Earlier ABI cleanup/rebuild was completed; do not repeat it. Never remove broad source/build trees.