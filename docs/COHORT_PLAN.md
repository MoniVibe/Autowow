# Persistent Bot Cohort Plan (v1)

Status: **design + scripts written, nothing applied.** Source evidence is module checkout
`/root/autowow-advisor-t1-module` at `82f40d59` (paths below are relative to its `src/`).
Live config values were read from `/usr/local/etc/modules/playerbots.conf` and
`/root/p1runtime/worldserver.conf` on 2026-09-23 (read-only).

## 0. Product shape

- 50 persistent characters, level 1 as created: 5 per race, 10 races, 25 per faction.
- They are never re-randomized, never deleted, never logged out by rotation. They level 1->80
  over real time, and later group for dungeons, PvP (realm is PvP: `GameType = 1`) and raids.
- Stable identity comes from `scripts/cohort/cohort-manifest.json`. `id` (for example `A-HU-01`)
  and `seq` are permanent. Names are reserved forever. A retired entry keeps its row
  (`status: retired`) and its id and name are never reassigned.
- Death Knights are excluded (they start at level 55 in Acherus). They are a later unlock.

## 1. How a character plays autonomously outside the random pool

**Account type.** The random pool is defined by the account name. `AssignAccountTypes` selects
`username LIKE 'rndbot%'` (`Bot/RandomPlayerbotMgr.cpp:508`, live `RandomBotAccountPrefix =
"rndbot"`). `IsRandomBot(guid)` is true only when the account is in that list **and** the GUID
is in `currentBots` (`Bot/RandomPlayerbotMgr.cpp:2172-2182`). `currentBots` is loaded only from
`playerbots_random_bots` rows with owner 0 and event `add` (`GetBots`, `:2229-2250`). Cohort
accounts are named `COHORT_<RACE>_<NN>`, so `IsRandomBot` is always false for them. The manifest
validator rejects any `rndbot*` account name.

**Login.** The bridge verb `activate` (`AutoWow/AutoWowBridge.cpp:2719-2741`) calls
`sRandomPlayerbotMgr.AddPlayerBot(guid, 0)` (`:2735`), which is Playerbots' normal async login.
With master account 0, `isRndbot = !masterAccountId` is true (`Bot/PlayerbotMgr.cpp:102`), so
the ownership gate at `:113` passes whatever the account type is. The bot joins the
RandomPlayerbotMgr holder masterless, but this does **not** make it a random bot. `activate` and
`independent` require an unretired `autowow_league_member` row (`IsLeagueMember`,
`AutoWowBridge.cpp:684-689`), so enrollment is part of provisioning.

**Exemption proof (randomize / teleport / revive / rotation).**
- The manager only walks `availableBots = currentBots` (`RandomPlayerbotMgr.cpp:319-320`, loop
  `:420-450`). Every randomize, level-teleport, revive-teleport and logout path lives in
  `ProcessBot(uint32)` (`:1343-1453`: logout `:1364` / `:1443`) and `ProcessBot(Player*)`
  (`:1455-1578`: `Revive` `:1491`, `Randomize` `:1547`, `RandomTeleportForLevel` `:1569`).
  A cohort GUID is never in `currentBots`, so none of these run for it.
- The level-up auto-teleport is gated on `IsRandomBot`
  (`Ai/Base/Actions/AutoMaintenanceOnLevelupAction.cpp:24`).
- `ProcessBot` also calls `IsRandomBot` itself (`:1413`).
- Residual 1: `LogoutAllBots` runs only when `DisabledWithoutRealPlayer = 1`. The live value is
  `0`. Keep it at 0.
- Residual 2: on login, a non-random bot is pushed into the manager's `players` list
  (`:2707-2710`). The only consumers are gated off live: `SyncLevelWithPlayers = 0` and
  `RandomBotJoinBG = 0`.

**Result:** exemption is automatic. **No code change is needed for exemption.**

**`independent`** (`AutoWowBridge.cpp:2763-2813`) makes the bot a solo, autonomous player:
- requires solo and not in combat
- `SetNoTeleport(true)` (`:2783`)
- no master, independent-party flag
- full AI reset
- strategies `+grind,+new rpg,-rpg,-follow,-move random,-travel` (`:2804`)
- forced activity

The forced activity matters: without it the bot falls under `BotActiveAlone = 10` throttling
(see `BULK_SOAK_PLAN.md` section 1d). The independent flag also enables the AutoWoW travel
fixes: hold and replan when stuck, never teleport (section 1b there).

**Oracle allowlist** (`AutoWow.OracleRuntime.BotGuids`, live `Enabled = 1`, 27 GUIDs,
`MaxBots = 27`, hard cap 256):
- adds in-process quest selection, lease and dispatch, Route V2 and zone-travel assist
- denies the legacy questgiver scan
- enables auto talents, trainer spells and equipment on level-up (section 3)

The list is static config, so changing it needs an edit and a worldserver restart.

**Control mode is a switch, for A/B.** Each manifest entry has `control_arm` (`stock` or
`oracle`). The default split is 13 stock / 12 oracle per faction, interleaved by class.
`cohort-start.ps1 -ControlMode Manifest|Stock|Oracle` applies the arms and checks them against
the live allowlist:
- An oracle-arm GUID that is not listed is skipped.
- A stock-arm GUID that is listed is skipped.
- A bot is never started in the wrong arm.

Current finding to keep in view: Oracle-managed bots complete 0% of quests, stock bots 20%. The
cohort therefore starts safely in `-ControlMode Stock` until the Oracle arm is fixed. After
that, the Oracle-arm GUIDs are added to the allowlist and `MaxBots` is raised in a maintenance
window.

## 2. Character creation

- **Accounts.** Use the stock console `.account create <NAME> <PASSWORD>`, the same path as
  `scripts/create-account.ps1`.
  - The password comes from `$env:COHORT_ACCOUNT_PASSWORD` and is never printed. Console logs
    are redacted.
  - Accounts get no GM level. There is one account per race: `COHORT_HU_01` ... `COHORT_BE_01`.
    Accounts never mix factions (live `AllowTwoSide.Accounts = 1` would allow it; we don't).
  - `CharactersPerRealm = 10`, so each race account holds its first unlock of 5 more.
- **Characters.** Stock AzerothCore has **no** console or GM command that creates a named
  character, and neither does Playerbots (`.playerbots bot addclass` picks a random cached
  character, per `dungeon-fixture-lib.ps1:397`). The only headless create path in the tree is
  `RandomPlayerbotFactory` (`Bot/Factory/RandomPlayerbotFactory.cpp:58-175, 700-725`):
  `CharacterCreateInfo`, then `Player::Create`, then `SaveToDB(true,false)`, then
  `sCharacterCache->AddCharacterCacheEntry`. That is the same core path as a client
  `CMSG_CHAR_CREATE`, with random race, class and name on `rndbot` accounts.
- **Required code change (C1): console-only command
  `.autowow cohort create <account> <raceId> <classId> <gender> <name>`.** It reuses that exact
  path with explicit arguments. It must:
  - validate the name with `ObjectMgr::CheckPlayerName`, then check uniqueness and reserved
    names
  - enforce `CharactersPerRealm`
  - pick appearance deterministically: the first valid `CharSections`
  - grant nothing: level 1, stock starting gear and spells
  - be console-only, with no bridge exposure (per `LEAGUE_RUNBOOK.md:58`)

  `provision-cohort.ps1 -CharacterCreateCommand` holds the template if the name differs.
  Zero-code fallback: create the 50 characters in the real client (10 accounts x 5). This is
  also the stock path.
- **Names** are deterministic, race-flavoured and invented, to reduce collisions with
  `playerbots_names`. They are unique in the manifest and validated as 2-12 letters, Capitalized,
  with no triple letters. Realm-wide uniqueness is checked at provision time. A collision is a
  **conflict** that aborts `-Apply`. The fix is a manifest edit to a new name. A name that is
  already used is never reused.
- **Headless transport.** SOAP and RA are disabled (`SOAP.Enabled = 0`, `Ra.Enable = 0`), and
  the live WSL world's stdin is not piped. So `-Apply` runs a **one-shot WSL worldserver** and
  feeds the console lines on stdin, as `create-account.ps1` does.
  - The world must be stopped: this is a maintenance window.
  - The MySQL relay must be up.
  - Ready signal: bridge port 18787. Shutdown: stdin EOF, the normal save path.
  - Enrollment (`autowow_league_team` `cohort-alliance` / `cohort-horde`, plus
    `autowow_league_member` rows with role adventurer, affiliation guild, active 0) is plain SQL
    and safe while the world runs.

### Race/class matrix (3.3.5, no DK)

Gnome and Tauren have only 4 legal non-DK classes, so each takes one duplicate. The Tauren
duplicate is Druid, the only way Horde gets a second druid. Paladin (Horde) and Shaman and
Druid (Alliance) are single-race classes, so their counts are capped.

| Race (start zone) | Classes |
|---|---|
| Human (Northshire) | Warrior, Paladin, Rogue, Mage, Warlock |
| Dwarf (Coldridge) | Warrior, Paladin, Hunter, Rogue, Priest |
| Night Elf (Shadowglen) | Warrior, Hunter, Rogue, Priest, Druid |
| Gnome (Coldridge) | Warrior, Rogue, Mage, Warlock, Warlock* |
| Draenei (Ammen Vale) | Paladin, Hunter, Priest, Shaman, Mage |
| Orc (Valley of Trials) | Warrior, Hunter, Rogue, Shaman, Warlock |
| Undead (Deathknell) | Warrior, Rogue, Priest, Mage, Warlock |
| Tauren (Camp Narache) | Warrior, Hunter, Shaman, Druid, Druid* |
| Troll (Valley of Trials) | Warrior, Hunter, Priest, Shaman, Mage |
| Blood Elf (Sunstrider Isle) | Paladin, Rogue, Priest, Mage, Warlock |

\* duplicate class (only 4 non-DK classes are legal for that race).

Faction totals, each summing to 25:
- Alliance: Warrior 4, Rogue 4, Paladin 3, Hunter 3, Priest 3, Mage 3, Warlock 3, Shaman 1,
  Druid 1.
- Horde: Warrior 4, Hunter 3, Rogue 3, Shaman 3, Warlock 3, Priest 3, Mage 3, Druid 2,
  Paladin 1.

## 3. Persistence

| Concern | Mechanism | Status |
|---|---|---|
| Survives restart | Normal character rows. Never in `playerbots_random_bots`, so never deleted or re-rolled by the pool. | automatic |
| Log in on world start | `RandomBotAutologin` only logs in `currentBots`. Cohort bots need `cohort-start.ps1 -Apply` after every world start. | **script, orchestrator must run it** (from revive or the guardian) |
| Relog after crash | Same idempotent call. It only activates offline members and never re-arms online ones. | script |
| No randomize / teleport / rotation logout | Section 1 proof. | automatic |
| Death | No manager revive-teleport (`Revive` is in `ProcessBot`, random-only). The normal Playerbots dead strategy does release, corpse run and spirit healer. | automatic, **unverified live** |
| Leaving the world | Only `cohort-park.ps1 -Apply` or a server stop. | script |

**Required code change (C2): level-up maintenance for the stock arm.** Non-random,
non-Oracle bots get **no** auto talents, trainer spells or quest spells
(`AutoMaintenanceOnLevelupAction.cpp:34-47, 66-77`). They also get no equipment,
consumable, ammo or reagent refresh (`:163-181`). The Oracle arm does get all of these through
`IsManagedBot`. As built, a stock-arm cohort bot would reach 80 with no talents: that breaks the
product and confounds the A/B.

Fix: widen those three gates to `... || botAI->IsAutoWowIndependentParty()` behind a new flag
`AutoWow.Independent.AutoMaintenance` (default 0, flags OFF first). Owner call (product feel):
should talents and spells be free on level-up like random bots, or should bots have to visit
trainers? Trainer visits work through `TrainerAction` with live `AllowLearnTrainerSpells = 1`,
but only the legacy `rpg` strategy has the `rpg train` trigger (`Ai/World/Rpg/Strategy/RpgStrategy.cpp:145`)
and `independent` removes `-rpg`. So an in-world trainer path for the stock arm is **not
proven**.

## 4. Scripts (`scripts/cohort/`)

- `cohort-manifest.json`: 50 entries. Fields: `seq`, `id`, `name`, `faction`, `race`/`race_id`,
  `class`/`class_id`, `gender`, `account`, `map_id`, `start_zone`, `zone_id`, `area_id`,
  `control_arm`, `status`. GUIDs are deliberately absent: the `characters` table is the only
  source of truth, and the scripts resolve GUIDs by name and account on every run.
- `cohort-lib.ps1`: manifest validation (legal race/class, no DK, name rules, no `rndbot`
  accounts, no mixed-faction account, 10 per account), a read-only inventory in which every
  SELECT is guarded, a live allowlist read, and bridge helpers.
- `provision-cohort.ps1`: dry run by default. `-Offline` lists the actions without touching the
  database. `-Apply` runs:
  1. conflict abort
  2. the one-shot console (accounts and characters)
  3. re-inventory
  4. enrollment SQL
  5. final verification, which fails loudly if incomplete

  It is idempotent.
- `cohort-start.ps1` (`-ControlMode`) and `cohort-park.ps1`: dry run by default. They wrap
  `autowow-control.ps1` with `activate`, then wait until online, then `independent` with the
  combat-deferral retry. This is the same sequence as `revive-autowow.ps1`, which is hard-limited
  to six scout GUIDs and so can't be reused directly.

## 5. Per-character progression metrics

Each metric is recorded per `id`, per control arm, and per sampling window. Track each metric
separately and never aggregate a partial list as if it were the total.

| Metric | Source | Exists? |
|---|---|---|
| level/h and xp/h | `characters.level`, `characters.xp` snapshots | yes (SELECT) |
| rewarded quests/h | `character_queststatus_rewarded` count delta | yes (SELECT) |
| deaths/h by killer kind (creature / player / environment / elite) | bridge `combatlog` and CombatTelemetry. Death-by-killer attribution needs a collector. | **partial, needs collector** |
| stuck / loop incidents | AutoWowQuestLedger events, NewRpg stall/replan counters, and the travel status in bridge `list` held unchanged for 10 minutes or more | partial |
| time in combat | fraction of bridge `list` samples with `combat=true` | yes (sampling) |
| online uptime, relog count | bridge `list` presence plus `cohort-start` receipts | yes |

Extend `league-progression-observer.ps1` / `quest-telemetry.ps1`, which both already read
`autowow_league_member`, with a `team_id LIKE 'cohort-%'` roster filter. Don't write a new
observer.

## 6. Slot unlocks (towards 100-200 per faction)

1. Append entries with the next `seq` and ids (for example `A-HU-06`). The ids and names of
   earlier entries are never changed.
2. Accounts fill to 10. After that comes `COHORT_HU_02`, and so on (16-character limit).
3. Run `provision-cohort.ps1` (dry run), then `-Apply` in a maintenance window, then
   `cohort-start.ps1 -Apply`.
4. Keep the class balance rule: each class within one of the faction mean, except the
   single-race classes.
5. The DK unlock later needs Acherus handling (start level 55) and a DK entry in
   `CohortValidClasses`.
6. Capacity:
   - An Oracle-arm scale-up is capped at 256 allowlist GUIDs.
   - The team `roster_cap` is written as 200. The column is tinyint, max 255.
   - Server load is on top of the 150-bot random pool. Measure it before each doubling.

## 7. Orchestrator run order

1. Land C1 (console create command) and C2 (flagged level-up maintenance), then rebuild.
2. In a maintenance window: `$env:COHORT_ACCOUNT_PASSWORD=...;
   .\scripts\cohort\provision-cohort.ps1` (review), then `-Apply`.
3. Start the world as usual, then run `.\scripts\cohort\cohort-start.ps1 -ControlMode Stock`
   (review), then `-Apply`.
4. For the A/B: add the Oracle-arm GUIDs to `AutoWow.OracleRuntime.BotGuids`, raise `MaxBots`,
   restart, and run `cohort-start.ps1 -ControlMode Manifest -Apply`.
