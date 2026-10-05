# Combat lab: owner guide

You play a fight. Then the same character fights the same fight on autopilot as a bot. A script compares the
two runs: damage, time to kill, damage taken, opener, spell mix, cooldowns, reaction times, and idle time.

## Before the first session (orchestrator)

1. Stop the world. Run `scripts/lab/provision-lab.ps1` to see the plan, then run it with `-Apply`. It creates up
   to 9 level-1 characters on SHONHAY. It never touches Flacid and never creates accounts.
2. Paste the `AutoWow.Lab.*` block that the script prints into the lab world's `playerbots.conf`.
3. Start a world built from the `combatlab` module branch.

## Characters

Log in as usual: realmlist `127.0.0.1`, account SHONHAY.

| Character  | Class   | Spec (matches the bots) |
|------------|---------|-------------------------|
| Labrogue   | Rogue   | Combat                  |
| Labwarrior | Warrior | Arms                    |
| Labshaman  | Shaman  | Enhancement             |
| Labdruid   | Druid   | Feral (cat)             |
| Labmage    | Mage    | Frost                   |
| Labpriest  | Priest  | Shadow                  |
| Labwarlock | Warlock | Affliction              |
| Labpaladin | Paladin | Retribution             |
| Labhunter  | Hunter  | Beast Mastery (only created if a slot was free; otherwise use Flacid's class for reference) |

There is no death knight, because the cohort has none.

**The first time you log in on each character**, type `.autowow lab kit`. This sets level 75, the bot spec,
spells, and green gear at the cohort's median item level for that class. You only need to do it once.

## Running a scenario

All fights take place at the lab marker on GM Island, where nothing else spawns.

Before you start, turn GM mode off with `.gm off`. While it is on, mobs ignore you.

1. Type `.autowow lab reset`. You are teleported to the marker and get full health and mana with no cooldowns.
   Run this before every fight.
2. Buff yourself as you normally would.
3. Start the fight with a spawn command. The mobs appear around you and attack immediately:

| Fight | Command |
|-------|---------|
| 1v1 melee humanoid | `.autowow lab spawn 27260 1` |
| 1v2 | `.autowow lab spawn 27260 2` |
| 1v3 | `.autowow lab spawn 27260 3` |
| caster humanoid | `.autowow lab spawn 27259 <count>` |
| beast | `.autowow lab spawn 27131 <count>` |
| add mid-fight | type another spawn while mobs are still alive; it joins the same run as a later wave |

The mobs are 27260 Dragonflayer Huscarl, 27259 Dragonflayer Flamebinder, and 27131 Grizzly Bear. Their level
is pinned to 75. To pick another level, add it to the end of the command: `.autowow lab spawn 27260 2 74`.

4. The run stops on its own when every lab mob is dead or when you die. To abort a run, type
   `.autowow lab reset`.

## Bot replay of the same fight

1. On the same character, type `.playerbots bot self`. You should see "Enable player botAI".
2. Type `.autowow lab reset`.
3. Type the same spawn command you used, then take your hands off the keyboard and watch.
4. When it finishes, type `.playerbots bot self` again. You should see "Disable player botAI".

The bot uses your exact character, gear, and talents, so the only difference between the runs is who is
playing.

## Comparing (orchestrator)

The trace is written to `/root/autowow-soak/logs/lab-trace.jsonl` (WSL).

```
python3 scripts/lab/lab-compare.py /root/autowow-soak/logs/lab-trace.jsonl --list
python3 scripts/lab/lab-compare.py /root/autowow-soak/logs/lab-trace.jsonl --name Labrogue --scenario 27260x2@75
python3 scripts/lab/lab-compare.py /root/autowow-soak/logs/lab-trace.jsonl --human <run> --bot <run> --json
```

The `--name` option compares your latest run with the bot's latest run of the same scenario. The report covers:

- DPS, time to kill, damage taken
- GCD busy %
- the first 10 casts
- spell share %
- cooldown use times
- reaction to the first wave and to adds
- low-HP (below 35%) to the first action and to the first defensive
- idle gaps longer than 1.5 s
