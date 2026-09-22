# Corpse Rescue Live Proof — 2026-07-16

## Scope

- Runtime artifact SHA-256: `b6860604339294bb4c054426f9b02978abc2ab49075481ba924e6ceeae86865e`
- Corrected deploy receipt: `logs/phase1-dev-cycle/phase1-dev-cycle-20260716-074015525.jsonl`
- Natural Onyxia probe receipt: `logs/probe-lab-live-onyxia-20260716-104750-a8cc89cd.jsonl`
- Playerbots acceptance interval began at `logs/phase1-runtime/Playerbots.log:9238`.
- No direct resurrection, interior teleport, cooldown reset, database character write, or forced encounter completion was used.

## Proven behavior

The first natural tank death progressed through the released-corpse combat-res path:

1. Out-of-combat healers attempted ordinary resurrection and did not cast through invalid geometry (`Playerbots.log:9389-9399`). Their telemetry reports `caster_combat=false combat_res=false`.
2. Nathalis released and the healer resolved his real corpse (`Playerbots.log:9400-9414`).
3. Liguhku entered combat and started Rebirth against that corpse: `spell=rebirth mode=corpse caster_combat=true combat_res=true` (`Playerbots.log:9415`).
4. Nathalis accepted from source GUID 41 as a released ghost (`Playerbots.log:9416`).
5. The monitor subsequently observed Nathalis alive, non-ghost, and back in map 249 instance 14 with all ten raid members alive (`probe receipt line 94`, `2026-07-16T07:56:30.4751268Z`).

Verdict: **PASS — released corpse combat resurrection completed naturally.**

## Telemetry race found

The accept line reports `alive_after=false` even though the next monitor evidence proves the player alive. AzerothCore teleports a released player to the stored resurrection point and schedules `DELAYED_RESURRECT_PLAYER`; the accept action therefore samples too early.

A staged follow-up adds a `[CorpseRescue] completed` event from `PLAYERHOOK_ON_PLAYER_RESURRECT`. The read-only acceptance checker will require `cast_started -> accept -> completed alive=true` after that build is deployed.

## Second-death failure sample

After Rebirth was consumed, Nathalis died and released again (`Playerbots.log:9758-9766`). Once combat ended, every resurrection-capable bot selected the real corpse, but bounded ordinary movement repeatedly returned `started=false` and casts remained blocked by `range+los` (`Playerbots.log:9767` onward).

At `2026-07-16T08:06:37.3243745Z`, the probe showed:

- Nathalis: released ghost on map 1;
- all nine survivors: alive and out of combat in map 249 instance 14;
- several healers near `(-173, -189)`, unable to obtain a pathable endpoint near the corpse.

Verdict: **FAILURE SAMPLE — released corpse selected correctly, but exact-anchor navmesh approach is not robust.**

## Related generic issue

The initial pull began with the tank about 92 yards ahead of the healing line. DungeonNavigator later emitted `blocked=party_cohesion`, but only after combat had begun. Encounter activation therefore needs a role-aware pre-pull readiness gate; corpse rescue also needs deterministic nearby-pathable endpoint sampling.
