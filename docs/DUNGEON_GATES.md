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
- Iron Clad Door 16397 (guid 30534, -101,-669,7) gates idx3-6. Correction (lane Z): lever 101833 (guid 26206) is
  NOT_SELECTABLE and unusable; the door opens only when spell 6250 hits Defias Cannon 16398 (needs Defias Gunpowder).
  Lane Z2 (dmcannon) rows: loot chest 17155 (guid 26203, -106,-617,14, loot 2882 = Defias Gunpowder 5397 at 100%),
  then the gunpowder holder uses 5397 on the cannon (guid 26205, -108,-660,7). See "Lane Z2" below.
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

## Lane Z (dgates) status, 2026-09-27
Table: `src/Ai/Dungeon/Generic/DungeonGatePolicy.h` (`DungeonGate::Steps`), flag `AutoWow.DungeonNav.Gates` (default 0),
key rows need `AutoWow.DungeonNav.Gates.BypassKeys` (default 0). Encounter indices checked against DungeonEncounter.dbc;
guids/positions/scripts against the world DB and core scripts. doneWhen gained two params (`doneData`, `doneValue`)
and one kind, `escorting` (creature AI `IsEscorted()`), which is what marks a gossip that started an escort.

Shipped (source/DB verified; no in-game run yet):
- Deadmines Sneed idx1: PROXY_KILL Shredder 642 guid 79223 (dead), then KILL_SET Sneed 643 r40 (encounter done).
- Wailing Caverns Mutanus idx7: GOSSIP Disciple 3678 guid 18675 menu 201 option 0 (conditions: data 0..3 DONE) until
  escorting, then ESCORT until encounter done (Mutanus is summoned by the Disciple's SAI).
- Razorfen Kraul Agathelos idx5: KILL_SET Ward Keeper 4625 r15 at (2066,2012,64); ward 21099 SAI opens after.
- Blackfathom Aku'mai idx7: fires 21118/21119/21120/21121 (guids 32930/32932/32933/32931, button, no autoclose)
  each followed by a KILL_SET of its summon group entry (4825/4977/4823/4978, r60 around the fires), then ENTER_AREA
  at portal 21117 (guid 32682) until it opens. Instance script opens it when all fire data are DONE and every summon died.
- Razorfen Downs Tuten'kash idx0: Gong 148917 guid 32045 used twice until GetData(148917) >= 2 (gong is unselectable
  while a wave lives), third use until the gong stays unselectable, then KILL_SET Tuten'kash 7355 r80.
- Gnomeregan Grubbis idx2: GOSSIP Emi 7998 guid 30136 menu 1080 option 0 until escorting, then ESCORT.
- Uldaman Archaedas idx7: Altar of Archaedas 133234 guid 40698 (ritual, 1 participant, spell 10340) until
  GetData(2) >= IN_PROGRESS.
- Sunken Temple Atal'alarion idx0: statues 148830..148835 (guids 27898/27899/28111/28112/28113/28114), row k waits for
  GetData(10) >= k+1 (DATA_STATUES; SAI conditions punish a wrong statue). Required rows, not optional: skipping one on
  timeout would make the next use out of order.

Dropped:
- (Deadmines Iron Clad Door: dropped here, shipped by lane Z2 below.)
- Uldaman Stone Keepers altar 130511 (spell 11568): not asked for; whether Temple Door 124367 blocks the Archaedas route is
  unverified. TODO.
- Zul'Farrak, BRD, Stratholme, Dire Maul rows: not attempted (TODO, survey above still applies).

Needs in-game proof: bot ritual use of 133234; gossip select through the stock packet pair; the Emi/Disciple escort
pacing; BFD summon deaths reaching zero (summons are 300 s out-of-combat despawns: a despawned summon never
decrements the count, the portal then never opens and the ENTER_AREA row retries until the encounter is abandoned).

## Lane Z2 (dmcannon) status, 2026-09-27
Deadmines Iron Clad Door, rows for each of idx3 (Mr. Smite), 4 (Cookie), 5 (Greenskin), 6 (VanCleef); Gilnid idx2 and
earlier are ungated. Facts: lever 101833 guid 26206 NOT_SELECTABLE. Door 16397 guid 30534 (type door, flags 34 =
LOCKED|NODESPAWN) opens only through the cannon's SAI 1639800 (SPELL_HIT 6250: set door GO state, instance data 1 = DONE;
the instance script has no GetData, so the door GO state is the witness). Item 5397 (class 13, max 1) casts 6250 =
OPEN_LOCK on a GO target, 5 yd range, charges -1 (consumed); cannon lock 83 = key item 5397. Chest 17155 lock 57 =
ordinary OPEN/TREASURE; looting it runs event 619 (Defias Overseer 634) and SAI (Taskmaster 4417): expect a fight.
- step 0 LOOT_GO chest 17155 guid 26203: the navigating bot opens it with the NewRpgAction objective-chest path
  (`Player::SendLoot`, autostore only the slot holding 5397, release). done = a party member holds 5397 or the door is open.
- step 1 USE_ITEM_ON_GO cannon 16398 guid 26205, keyItem 5397: the holder (leader first, then followers by guid) walks
  within 4 yd (followers through `AutoWowDungeonWalkAction`) and sends `CMSG_USE_ITEM` with the GO target (UseItemAction
  layout); the core checks range/lock and consumes the key. done = the door is open.
- New doneWhen `unlocked`: door GO `doneData` not READY, or missing on its loaded grid (the instance script despawns a
  door stored open on reload), or (`doneValue` != 0) a party member holds item `doneValue`.
- Key rule changed: a keyItem row is skipped only while no party member holds the key and BypassKeys = 0. This row
  therefore runs with BypassKeys = 0.

Needs in-game proof: chest loot lands 5397 in the leader's bags; the use-item packet fires 6250 on the cannon from 4 yd;
door GO state flips; a follower holder actually walks to the cannon (its AI may re-follow). Risk: if the loot row latched
done and the holder then leaves the party, the cannon row stays key-gated and idx3 falls back to the closed door.
