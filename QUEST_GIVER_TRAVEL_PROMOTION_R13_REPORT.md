# Quest-giver travel R13 promotion report

## Authority

- Baseline: `D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots`
- Baseline branch/HEAD: `phase1-objective-lock` at `93aaea3de19243c09ce9ecb25627dc9671715eed`
- Baseline mode: current authoritative working-tree bytes
- Source: isolated `luna/max-autowow-quest-giver-travel` lane

R13 supersedes R12. R12 production compilation is green. The lifecycle scenario crash came from
rendering its snapshot through `PassiveQuestAcquisitionOrderFacts`, which invokes
`PlayerbotAI::HasStrategy` even though the test deliberately creates a lightweight `PlayerbotAI`
without an `Engine`.

`QuestAcquisitionSnapshotJsonForSession` now accepts an optional explicit passive-order-facts input.
Production callers omit it and continue deriving telemetry from the live bot AI exactly as before.
Only `RunNativeExpiryScenario` supplies explicit facts and passes no bot AI into snapshot rendering,
so the test cannot enter the live strategy/Engine path. The scenario additionally checks that these
explicit facts appear in snapshot telemetry.

Three stale source-text checks were repaired:

- the moved `party_arrival_timeout` literal is asserted through executable
  `LifecycleDecisionName(BlockedPartyArrival)` behavior;
- the staged-mover body scrape was removed because focused policy tests already execute direct,
  partial, segmented, bounded-selection, failed-selection, and lifecycle behavior;
- the exact-giver helper scrape was removed because `ClassifyGiverResolution` and giver lookup
  behavior already have direct exhaustive assertions.

The obsolete helper extractors were removed. Production telemetry, exact giver resolution, party
cohesion, no-teleport/no-random behavior, and immutable lifecycle deadlines are unchanged.

## Verification scope

R13 is a standalone exhaustive package against the authoritative current baseline. Verification is
fresh patch application, byte identity, include/static call-graph checks, and registration only. No
configure, compile, test execution, build, client, or server operation was performed.
