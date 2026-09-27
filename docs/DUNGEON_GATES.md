# Dungeon progression gates (survey 2026-09-27)

Source: read-only survey of AzerothCore core scripts (`/root/autowow-advisor-t1-core/src/server/scripts/`), world DB
(gameobject/creature/gossip/smart_scripts) and probe evidence from S52/S53. Input for lane Z (dungeon interaction table).

Today's navigator knows none of these. Its only interaction rule (`DungeonProgressionInteractionPolicy.h`) fires when the
leader has arrived at a statically spawned, untargetable boss and a button/goober is within 80 yd; closed doors stop the
route. It fired 0 times in S52+S53.

Core note: bot `CMSG_GAMEOBJ_USE` runs `GameObject::Use`, which checks only NOT_SELECTABLE, not the lock, so bots can
open key-locked objects. Rows keep a `keyItem` field; key rows are skipped unless explicitly enabled.

auto = the core opens it after the kills (no bot action).

## Deadmines (36)
- Sneed (idx1) has no static spawn: kill Sneed's Shredder 642 (guid 79223, -289,-513,50); on death it ejects Sneed. The S52 `next=1` stall.
- Doors 13965 (Rhahk'zor), 16400 (Sneed), 16399 (Gilnid) open on boss death: auto.
- Iron Clad Door 16397 (-101,-669,7) gates idx3-6: use lever 101833 (guid 26206, -97,-671,7), no key. Cannon 16398 needs Defias Gunpowder.
- (-60,-860,0) stall = ship-deck pathing, not a gate.

## Wailing Caverns (43)
- Mutanus (idx7): gossip Disciple of Naralex 3678 (guid 18675, -135,125,-78) after the four Fanglords die, then escort.

## Ragefire Chasm (389), Maraudon (349)
- No gates.

## Razorfen Kraul (47)
- Agathelos (idx5) behind ward 21099: kill Death's Head Ward Keepers 4625 (guids 87481/87482, ~2066,2012,64). auto after kills, but the navigator must target them.

## Blackfathom Deeps (48)
- Aku'mai (idx7): use the four Fire of Aku'mai 21118-21121 (~-818,-165,-25), clear each wave, portal 21117 opens.

## Gnomeregan (90)
- Grubbis (idx2): gossip Emi Shortfuse 7998 (guid 30136, -515,-139,-152) + escort; Grubbis spawns in summon group 4 (-495,-91).
- Workshop Door (Workshop Key) = back entrance: skip.

## Razorfen Downs (129)
- Tuten'kash (idx0): Gong 148917 (guid 32045, 2552,857,51), 3 uses, each after its wave is cleared.

## Uldaman (70)
- Ironaya: Keystone 124371 (guid 14393, -235,240,-51) needs Staff of Prehistoria (quest item): probe cannot.
- Stone Keepers: Altar of the Keepers 130511 (105,272,-27), ritual type; kill the four, Temple Door 124367 opens.
- Archaedas (idx7): Altar of Archaedas 133234 (96,269,-52), ritual type.

## Sunken Temple (109)
- Atal'alarion (idx0): statues 148830..148835 in exact entry order (wrong one = poison). The generic rule would pick the nearest statue: hazard.
- Jammal'an: kill balcony trolls 5712-5717, forcefield 149431 opens: auto.
- Avatar of Hakkar: Egg of Hakkar + Hakkari Blood: probe cannot.

## Zul'Farrak (209)
- Gahz'rilla: Gong 141832 (1651,1172,11), optional.
- Nekrum (idx5) + Sezz'ziz (idx6): use a troll cage 141070-141074 (~1886,1297,48) (Executioner's Key drops from 7274 there); 3 waves, wave 3 brings both bosses.
- Ukorz: after the waves, gossip Weegli 7607 (blows End Door 146084). Never talk to Bly first (crew turns hostile).

## Blackrock Depths (230)
- Ring of Law: area trigger 1526 (596,-188,-50, r8).
- The Seven: gossip Doom'rel 9039 (1281,-282,-78).
- Magmus: both Shadowforge Braziers 174744 (1330,-509,-89) and 174745 (1431,-509,-89) (lock wants Shadowforge Torch); throne door opens after Magmus: auto.
- Shadowforge Key / bar doors: probably shortcuts, unverified.

## Stratholme (329)
- Balnazzar: kill Dathrohan 10812 (guid 52149, 3416,-3045,137); he transforms.
- Ramstein: three ziggurat bosses + Thuzadin Acolytes 10399 open Slaughter Square, then abominations 10416/10417: auto.
- Key to the City gates (lock 879, Service Entrance): probe cannot.

## Dire Maul (429)
- East: gossip Ironbark the Redeemed 14241 (-57,-269,-58) opens the Conservatory door (need for Alzzin unverified).
- West: kill guardians 11480/11483 at the five pylons; forcefield drops: auto.
- North: courtyard/inner doors need keys from Fengus and Slip'kik.

## Table shape (one row per step)
```
mapId, encounterIdx, stepOrder, kind{USE_GO,GOSSIP,ESCORT,KILL_SET,ENTER_AREA,PROXY_KILL},
entry, spawnGuid?, x,y,z, radius, gossipOption?, repeat,
doneWhen{goState|creaturesDead|instanceData idx=val|encounterDone},
keyItem(0=none), optional, timeoutMs
```

## Trigger rule
When the navigator selects an unfinished encounter that has steps, travel to the first step whose `doneWhen` is false
(its position replaces the boss spawn as the goal). Act only out of combat with the party gathered; wait for `doneWhen`
or the timeout, then continue. Also: a route stopped at a closed door with a row runs that row. `PROXY_KILL` is the goal
for bosses with no static spawn.

Unverified: whether DM East, BRD and Stratholme gates lie on the main route, and whether plain click-to-open doors
(Deadmines Heavy Doors, Gnomeregan Final Chamber, Stratholme Bastion) block it. A nav probe settles those.
