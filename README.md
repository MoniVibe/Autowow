# AutoWow

**Azeroth that keeps going: persistent bot adventurers, professions, supply chains, and coordinated groups.**

AutoWow is an experimental fork of **Playerbots for AzerothCore**, focused on what happens over a character's whole journey. We want bots to build a life in the world: pursue quests, travel between zones, learn professions, gather materials, make useful goods, maintain their equipment, form parties, and tackle dungeons together.

The fun is watching those systems meet. A gatherer brings back materials, a crafter turns them into bags or potions, and adventurers use those supplies on their next trip. When someone gets stuck, we want to understand why and improve the actual gameplay loop.

This repository contains the native C++ module. The `ops` branch holds companion scripts, runbooks, contracts, and experiment reports.

## How this builds on Playerbots

Playerbots already supplies the foundation: bot characters, combat strategies, questing, travel, group behavior, and extensive dungeon and raid work. AutoWow builds on that work with persistent progression and coordination systems of its own.

| AutoWow focus | What we're adding and exploring |
|---|---|
| Persistent adventurers | Cohorts, quest scheduling, zone progression, and recovery from blocked work |
| Everyday upkeep | Trips for bags, repairs, food, water, ammunition, trainers, and riding/mount purchases |
| Professions and supply | Gathering detours, profession training, crafting, profession-house guilds, material routing, and orders for useful goods |
| Groups with a purpose | Party formation, role-aware dungeon recruitment, group travel, encounter routes, and prerequisite gates |
| Observable decisions | Progress ledgers, combat telemetry, a local control bridge, and bounded executor/ownership policies |

These systems are at different stages of development. Source availability does not mean every combination has passed a live-world test.

## Current snapshot

Source inventory at `599c0346` (September 30, 2026):

| Measure | Count / scope |
|---|---|
| Dedicated AutoWow source/header files | **143** directly under `src/AutoWow/` |
| C++ test source files | **118**, also registered in the module's CMake test configuration |
| AutoWow configuration entries | **528** in `conf/playerbots.conf.dist` |
| New Outland progression ladder | Level **58-70**, opt-in; no Northrend ladder yet |

Those are repository counts, not a test-pass total or a performance benchmark. AutoWow also changes existing Playerbots code outside `src/AutoWow/`.

A recorded **September 22 zone-scout experiment** used six scouts with **5x PvE XP, 1.5x player movement, and doubled eligible quest drops capped at 100**. Its report records seven newly saved quest rewards and level gains for four scouts, alongside stalled quests, deaths, and unsupported objectives. It also records **116/116 native checks across seven named suites** for that earlier deployed build. Those results do not validate today's entire source tree. See the [experiment report on `ops`](https://github.com/MoniVibe/Autowow/blob/ops/docs/ZONE_SCOUT_STATUS_20260922.md).

## What to expect

**Active development.** Many AutoWow systems and recent additions are disabled by default. Read the comments in [the configuration template](conf/playerbots.conf.dist), enable a small set of related features, and validate them on a test realm before expanding.

**Travel and recovery depend on the profile.** Some experiments enforce physical travel and no teleport recovery; other optional systems permit portal fallbacks or treasury-funded purchases. Report the exact flags when comparing results.

**Native gameplay execution.** AutoWow runs inside the Playerbots C++ module. The Oracle-named components are planning, ownership, and executor machinery; optional external advisor tooling lives separately. That tooling does not establish that every bot action uses an LLM.

**Uneven content coverage.** Quest objectives, movement, death recovery, economy behavior, and dungeon coordination still need broader live validation. Implemented routes and policies are not a claim of complete quest, dungeon, raid, or endgame coverage.

## Getting started

You need a compatible **Playerbots-enabled AzerothCore WotLK server**, its databases, and the game data required by that server. This repository is a server module, not a standalone game or a packaged one-click realm.

Use the [preserved upstream README](README_PLAYERBOTS.md) for the foundation and upstream installation references. Its clone commands install upstream Playerbots; use this fork as the module when evaluating AutoWow. Some AutoWow features also require matching core patches described on the [`ops` branch](https://github.com/MoniVibe/Autowow/tree/ops/core-patches).

Start with a small roster and one feature family. Capture the module/core revisions, enabled settings, and observed results. Experimental settings can change progression, money flow, and recovery behavior, so keep backups of your test realm.

## Contribute, experiment, or just follow along

We're interested in people who enjoy bot worlds, server engineering, automation, and the strange stories that emerge when systems interact.

Useful contributions include:

- Reproduce a stuck quest, travel failure, death loop, or inventory problem.
- Improve a specific quest objective, dungeon route, profession recipe path, or recovery policy.
- Test a small roster over time and share both progress and failures.
- Make setup, feature profiles, and experiment reports easier to follow.
- Share clips or stories of bots doing something interesting; observations help choose what to build next.

Open an issue or pull request with **what you expected, what happened, the module/core revisions, the relevant flags, and a short reproduction**. For runtime results, include roster size, duration, rates, and hardware when relevant. Remove credentials and private configuration from logs. Code changes should include relevant checks and their actual results.

## Credits and license

AutoWow owes its foundation to [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), AzerothCore, and the earlier Playerbots projects acknowledged in the [upstream README](README_PLAYERBOTS.md). Their existing capabilities and contributor history remain theirs.

The repository includes the **GNU General Public License v2**; see [LICENSE](LICENSE) and individual source notices.
