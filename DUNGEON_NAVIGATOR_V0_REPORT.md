# Generic Dungeon Navigator V0 Report

Date: 2026-07-16

## Outcome

The native Playerbots dungeon navigator now admits a complete party through authoritative area
triggers, selects incomplete encounters from `DungeonEncounter`/`InstanceScript` metadata, and
follows persisted Playerbots travel-node walk graphs in bounded, validated segments. It contains
no Utgarde Keep boss names, map-specific boss waypoints, or encounter-specific movement scripts.

The portability run now also covers The Nexus and Drak'Tharon Keep. It exposed and fixed unsafe
cursor consumption, premature encounter completion, missing terminal encounter activation, and two
bounded route-recovery cases without adding map-specific coordinates to the navigator.

## Proven live

- Generic trigger 4745 admitted Nathalis first and then Teleane, Usta, Baeil, and Xahran one at a
  time into the same runtime instance. No member entered a split copy.
- The five-player tank/healer/three-DPS party autonomously cleared Utgarde Keep from the entrance
  through Keleseth, Skarvald/Dalronn, and both phases of Ingvar. All five survived.
- Encounter selection advanced 0 -> 1 -> 2 and ended at `status=all_complete`; completed encounters
  were not revisited.
- The Keleseth-to-Skarvald route composed a 203-point multi-hop walk graph and climbed from roughly
  Z=40 to Z=119. The Skarvald-to-Ingvar route composed another 109 points.
- A party-cohesion gate paused route dispatch when a follower lagged beyond 45 yards; normal follow
  then regrouped the party before progression resumed.
- Keleseth loot voting included item 35572; Skarvald/Dalronn included item 35575; final Ingvar loot
  voting included items 35577 and 35578. Every party member also gained money during the run.
- The persisted travel graph loaded at startup: 3,781 nodes, 15,041 paths, and 1,404,959 path
  points.
- Cached routes are retained until consumed even when a partial direct path temporarily appears;
  graph attachment nodes must be independently reachable from the bot.
- Every dispatched waypoint passed `AutoWowDungeonPath::Probe`; bounded ground-line segments
  retained floor, line-of-sight, slope, and vertical-continuity validation.
- No route, advance, engage, scout, or teleport order was issued during the native traversal
  evidence window.

### Second-dungeon portability evidence

- Two five-player parties entered The Nexus (instance 4) and Drak'Tharon Keep (instance 5) through
  authoritative triggers 4983 and 4998. Followers were admitted serially into each leader's exact
  instance.
- The Nexus party crossed the former route-cursor failure at indices 63 -> 64 -> 65 with all five
  members alive and on the valid floor. A prior observer relocation at the bad state reproduced an
  immediate fall from Z=-35.45 to Z=-1026 and death, proving that the earlier visual terrain drops
  were real rather than a camera artifact.
- Drak'Tharon consumed its 50-point route, emitted
  `completion=terminal_activation_rescan`, automatically engaged and killed Trollgore with all five
  members alive, then selected encounter 1 (Novos) without a manual `engage`.
- Trollgore loot produced five greed votes for item 35618. The archived evidence log is
  `logs/phase1-runtime/Playerbots-dual-probe-trollgore-seam-20260716-032529.log`.
- Combat movement later displaced the Nexus leader from the stored line. The resulting safe,
  incomplete 12-yard path is now consumed only to its actual endpoint; it does not advance the
  stored cursor. This recovery is compiled and unit-tested; a fresh live acceptance run remains.

Evidence is in `logs/phase1-runtime/Playerbots.log`. The final isolated build manifest is
`logs/phase1-20260715/dungeon-navigator-direct-link-build-manifest.json`.

## Verification

- Isolated WSL `unit_tests` and `worldserver` targets built successfully at `-O0`, `-j1`.
- Current worldserver SHA-256: `1c563e232e5c5c86cf49eba44298ace53d26659dbe4d692d39371e96e498a1b5`.
- 56 focused GoogleTests pass across transition, encounter selection/completion/activation,
  path safety, route reconnection, and dungeon death recovery.

## Runtime design

- Party leader only; groupless bots fail closed.
- A generic non-combat transition strategy discovers nearby normal-dungeon area triggers from core
  data, requires the complete party to be alive, stationary, noncombat, normal difficulty, and
  inside the real trigger volume, then activates the leader and followers serially.
- Noncombat and nonraid dungeon only; combat behavior remains owned by normal class/encounter AI.
- Direct safe navmesh path remains the preferred fast path.
- Long routes prefer a complete persisted direct walk link, then compose deterministic multi-hop
  walk-only graph routes. Portal/transport/flight/teleport edges remain excluded.
- The full route is cached per map/instance/encounter/spawn and consumed with a monotonic cursor.
- Lookahead is capped at 32 points and 45 yards; no-progress retries latch after three attempts.
- Safe incomplete paths may provide bounded physical progress to their real endpoint, but never
  cursor arrival. A same-XY ground reattachment may correct at most two upward yards only when the
  next stored point is independently reachable from the corrected height.
- Portal, transport, flight, teleport, and cross-map steps are never executed by V0.

## Remaining boundary

The proven transition executor intentionally supports normal-dungeon area-trigger portals only.
Moving platforms, elevators, explicit gameobject interactions, scripted transports, and raid
admission remain separate typed transitions. Restarting an unbound trash-only instance can place
followers into independent copies if they log in before grouping; deterministic probes therefore
stage parties outside the entrance trigger.

## Next slice

1. Complete the fresh live acceptance of `recovery=partial_progress` in The Nexus and confirm the
   cursor remains unchanged until physical waypoint arrival.
2. Continue Drak'Tharon through Novos and King Dred, verifying encounter-mask advancement and loot.
3. Add typed door/gameobject and elevator/transport transition adapters only for observed metadata
   gaps; do not add map-specific boss waypoints.
4. Repeat in a raid with a correctly sized roster after the second-dungeon portability gate.
