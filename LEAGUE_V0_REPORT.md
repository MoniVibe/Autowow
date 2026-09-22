# AutoWoW League v0 report

Updated: 2026-07-13 11:24 +03:00

## Current campaign behavior

- Northstar and Ember are persistent five-player Playerbots parties, controlled only through the localhost AutoWow bridge.
- Campaign orders are quest orders. A leader first uses Playerbots' standard questgiver actions and travel data; where the normal travel cache has no route, the bridge invokes Playerbots' own RPG quest-POI action.
- The RPG fallback moves to the game's quest POI, lets normal combat/loot progress objectives, and uses normal questgiver reward/accept actions. It never writes character quest, XP, gold, item, or objective records directly.
- A party with no valid quest work is deliberately idle. The former scout/engage random-grind loop is not used by the campaign director.
- Bootstrap staging is one-time only. Once a leader has recorded quest state, later launches retain the saved world position instead of sending the party back to a starter area.
- The neutral Wayfarer worker remains offline until a real economic-worker loop and professions are implemented. No gathering or treasury production is claimed for it.

## Verified persistent quest proof

Northstar, led by Brandreas (guid 10):

- The party staged in Teldrassil beside the legitimate questgiver for the leader's existing Night Elf chain.
- Quest `3120 – Verdant Sigil` was legitimately rewarded; leader XP increased from 317 to 357.
- The bridge then drove `457 – The Balance of Nature` to its objective POI. Live combat identified a Young Nightsaber and later a Thistle Boar.
- After a safe logout, `acore_characters.character_queststatus` persisted quest 457 as incomplete with `mobcount1 = 1`; quest 3120 appears in the rewarded table.

Ember, led by Saewash (guid 7):

- The party moved from the Valley of Trials to Gornek's real turn-in position at `-600,-4186`.
- Quest `788 – Cutting Teeth` was rewarded through the normal questgiver flow; leader XP increased from 336 to 556.
- The resulting persistent quest set includes active quests `789`, `792`, `1516`, `3084`, and `5441`, with quest 788 in the rewarded table.

The full command and database evidence is in `QUESTING_PROOF_REPORT.md`.

## Stability correction

- A crash discovered during the proof was traced to the bridge's `deactivate` acknowledgement dereferencing a Player object after Playerbots had destroyed it on logout.
- The bridge now saves the GUID before logout and replies without dereferencing the stale pointer. A subsequent activate/deactivate proof kept the bridge available.

## Conservative quest recovery

- The quest director now watches only objective-phase leaders. It requires both no position progress and no XP gain for 120 seconds before acting.
- It first performs up to two native Playerbots state resets/replans, preserving the party's real quest state and forbidding random movement.
- Continued non-progress pauses the complete party and records `quest_manual_review_required`; it does not conceal the problem with random grinding or database changes.
- A live controlled test reset all five Northstar members and immediately returned a normal Playerbots turn-in plan for quest 4495. The worldserver remained stable and the campaign director resumed cleanly.
- Caveat: bridge recovery is non-teleporting, but Playerbots' underlying New-RPG movement currently has a separate 90-second stuck fallback that can `TeleportTo` its destination. It is a real source-level behavior and must be scoped off for league parties before movement fairness is claimed.

## Scope still deliberately unclaimed

- This is a verified early-game persistent questing slice, not yet a hands-off 1–80 campaign.
- Cross-continent and dungeon/raid handoffs, quest-chain recovery when a POI is missing, profession/economy workers, team unlocks, and battleground arbitration still need their own proof loops.
- The parked random-bot account pool remains excluded from campaign control; do not enable random-bot autologin for this simulation.
