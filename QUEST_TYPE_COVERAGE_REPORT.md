# AutoWoW quest-type coverage

Date: 2026-07-15

## Current result

The current Playerbots branch has live end-to-end proof for the common outdoor quest loop and for
three objective families that were missing from the original audit: direct GameObject credit,
quest-item use on a creature, and a native escort event. This is broad enough to begin a player PvP
trial, but it is not evidence that every scripted quest in the game works.

| Capability | Status | Live evidence |
| --- | --- | --- |
| Accept, talk, turn in, choose reward | PASS | q789, q459, q792, and q916 accepted/completed/rewarded through normal NPC interaction |
| Kill/creature credit | PASS | q792 progressed 0/8 to 8/8, then turned in and rewarded |
| Loot/collect item | PASS | q789 and q459 reached 8/8; q916 reached 10/10 and rewarded |
| Direct GameObject use/credit | PASS | q917 rewarded; q786 produced three durable native GO-credit receipts for entries 3189, 3190, and 3192 |
| Use quest item on creature | PASS | q5441 progressed 0/4 to 4/4 with five native item-use packets, then rewarded |
| Escort/event | PASS for a normal escort; PARTIAL globally | q435 followed Erland's native escort script, completed, reached the finisher, and rewarded |
| Directed material gathering/professions | PARTIAL | gathering worker persisted 19 material units without direct inventory writes; profession training and quest coupling remain separate |
| Same-map quest travel | PASS | all listed fixtures walked to sources and finishers with zero quest-path teleports |
| Cross-map/taxi/boat quest travel | PARTIAL | transport selection exists, but no canonical end-to-end cross-continent quest receipt yet |
| Dungeon/group quest orchestration | NOT PROVEN | group PvE clears are proven separately; accepting and coordinating a dungeon quest chain is not yet a release-gated objective type |
| Stall recovery/backoff | PASS | the external director detects objective stagnation/engine abandonment, retries, backs off, and switches rather than grinding blindly |

## Exact receipts

- Kill and normal turn-in: `logs/phase1-20260714/q792-full-lifecycle-live.jsonl`
- Loot/collect and normal turn-in: `logs/phase1-20260713/q789-full-lifecycle-live.jsonl`,
  `logs/phase1-20260714/q789-finisher-live.jsonl`,
  `logs/phase1-20260714/q459-full-lifecycle-live.jsonl`, and
  `logs/phase1-20260714/q459-finisher-no-grind-live.jsonl`
- Autonomous next-quest selection and loot completion: `logs/phase1-20260714/q916-autonomous-lifecycle-live.jsonl`
- Direct GameObject use and reward: `logs/phase1-20260714/q917-gameobject-reward-live.jsonl`
- Three-object GameObject credit: `logs/phase1-20260714/q786-interact-live.jsonl`
- Item on creature and reward: `logs/phase1-20260714/q5441-item-use-live-v2.jsonl`
- Escort and reward: `logs/phase1-20260714/q435-escort-live.jsonl`
- Gathering persistence: `logs/gathering-proof-20260714-141803.json`

All quest acceptance streams report zero unrelated locked-objective pulls, zero random-grind
fallbacks while locked, zero quest-path teleports, and zero direct quest-state database mutations.

## Regression gate

The current PowerShell contract suite for escort behavior, direct GameObject behavior, and director
selection/recovery passes 43/43. During that run, the escort dry-run test exposed that its plan path
was consulting an already-rewarded live fixture character. The fixture now emits its side-effect-free
plan before mutable live-character readiness checks; applied runs still reject a used fixture.

## Honest remaining gaps

1. Prove one cross-map quest using normal taxi/transport transitions.
2. Prove a dungeon/group quest from acceptance through shared credit and reward.
3. Generalize escort support beyond one conventional escort to gossip-started, ambush-heavy, and
   failure/retry variants.
4. Couple profession choice, trainer visits, recipes, and material demand into the campaign economy.

These are follow-on breadth problems. They do not block a first human-versus/with-bots PvP playtest.
