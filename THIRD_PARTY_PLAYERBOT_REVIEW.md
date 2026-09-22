# Third-Party Playerbot Review

Reviewed: 2026-07-14

This review separates tools that improve the human observation/control experience from code that improves autonomous bot behavior. No third-party code was copied or installed during the review.

## Decision summary

| Project | Decision | AutoWoW use |
|---|---|---|
| [Lichborne-AC/PlayerbotManager](https://github.com/Lichborne-AC/PlayerbotManager) | Adopt soon, client-side only | Observer and human drop-in roster dashboard: specs, equipment, gear score, strategies, groups, and raid composition |
| [Wishmaster117/MultiBot-Chatless](https://github.com/Wishmaster117/MultiBot-Chatless) | Borrow ideas; do not install in the proof build | UI and structured-telemetry reference for quests, professions, inventory, bank, PvP, strategies, and raid control |
| [Wishmaster117/mod-multibot-bridge](https://github.com/Wishmaster117/mod-multibot-bridge) | Separate compatibility/security lane only | Possible reference for addon-message transport and owner/group-scoped bot visibility |
| [DustinHendrickson/mod-ollama-chat](https://github.com/DustinHendrickson/mod-ollama-chat) | Defer | Optional social/personality layer after autonomous gameplay and server performance are stable |

## PlayerbotManager

PlayerbotManager is a WoW 3.3.5a Lua addon, not a server AI module. Its current README describes spec detection, average item level, GearScore, per-slot inspection, active strategy display/toggling, group views, and raid planning. That aligns well with the observer-client goal and gives a human player a convenient way to inspect the campaign roster.

Why it fits now:

- no server-side C++ integration;
- no second automation protocol;
- useful for checking the exact issues AutoWoW already measures: roles, specs, equipment, and raid composition;
- GPL-3.0 license is present;
- repository activity was current at review time (version 1.4 dated 2026-07-03).

Limit: it does not make bots quest, gather, heal, tank, or fight more intelligently. It is an operator UI. Install it only into the disposable/working client copy, never the clean source client.

## MultiBot Chatless and mod-multibot-bridge

MultiBot Chatless is a large client addon paired with a C++ server bridge. The bridge exposes structured addon messages for roster/state and extensive inspection surfaces including quests, inventory, bank, guild bank, spellbook, skills, reputations, professions, recipes, outfits, trainer data, PvP statistics, and GameObject data. It also exposes allow-listed mutation commands for combat, position, loot, equipment, crafting, and item movement.

Useful lessons for AutoWoW:

- structured addon messages are much cleaner than parsing bot chat;
- the owner/group visibility model is a useful security baseline;
- its profession, recipe, bank, quest, and strategy schemas are good candidates for independently designed read-only AutoWoW endpoints;
- its UI demonstrates what a future observer/control client may need.

Why it should not be installed into the current proof build:

- it would create a second server control plane alongside the existing allow-listed AutoWow bridge;
- its broad mutation surface materially expands security and test scope;
- it does not improve headless autonomy by itself;
- the server bridge had no detected repository license file or GitHub license classification at review time, so copying its C++ is not an acceptable default;
- integration compatibility with the exact `mod-playerbots/Playerbot` snapshot has not been built or tested.

Recommended route: independently implement selected read-only profession/gear/bank/quest surfaces in AutoWow, or test the unmodified bridge and addon in a separate worktree/runtime after a sender-authentication and command-ownership audit.

## mod-ollama-chat

mod-ollama-chat is a C++ AzerothCore module that sends bot/chat context to an Ollama endpoint and adds personalities, chat memory, event chatter, sentiment/context features, and asynchronous responses. This is potentially attractive for making campaign members feel distinct.

Why it is deferred:

- it changes dialogue, not navigation, objective execution, threat, healing, or encounter decisions;
- its own README warns that local LLM use can bog down the server;
- its documented prerequisite is the `liyunfan1223` AzerothCore/Playerbots fork, not the current `mod-playerbots` Playerbot fork, so compatibility requires a deliberate port/build lane;
- the repository LICENSE is AGPL-3.0 even though some descriptive text refers to GPLv3, so any integration must follow the actual license file;
- adding LLM latency and resource consumption would contaminate current performance and raid-readiness measurements.

If adopted later, run it behind a bounded asynchronous queue with hard timeouts, concurrency/rate limits, circuit breaking, and telemetry. Chat generation must never sit on the world-thread decision path or control bot actions directly.

## Recommended order

1. Finish reliable autonomous questing, gathering, PvP, and reusable group/raid combat.
2. Add PlayerbotManager to the observer client's working copy and validate it against the pinned server.
3. Harvest independently designed read-only telemetry endpoints inspired by MultiBot where AutoWow lacks them.
4. Evaluate MultiBot addon/bridge compatibility in a separate runtime only if its richer UI remains valuable.
5. Evaluate mod-ollama-chat as an opt-in social layer after a 100-bot performance baseline is stable.
