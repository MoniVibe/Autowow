# AutoWow League ruleset v0

## Purpose

Northstar (Alliance) and Ember (Horde) progress from level one in one persistent local world. The Wayfarers' Exchange is a neutral contract network: it owns no territory and cannot win the season.

This is an implementation ruleset. It deliberately starts with two five-person expeditions so party, progression, persistence, and accounting can be proven before roster growth.

## Roster and activity limits

- Each guild starts with five enrolled level-one adventurers and a roster cap of 20.
- Roster permits can raise a guild cap to 40. A permit is an unlock, not an instantly leveled character.
- Each guild can eventually sponsor at most ten workers. Workers are real persistent Playerbot characters with a valid class and two professions.
- A character is technically Alliance or Horde. A Wayfarer is neutral by league rule, never by client faction.
- No more than 80 bots may be active concurrently. Persistent but parked characters cost database space, not worldserver CPU.

## Economy

- A guild treasury is an auditable campaign ledger. Every grant, contract, unlock, and later in-game transfer has a corresponding entry.
- No free best-in-slot gear, maintenance commands, or consumables are allowed.
- Starting adventurers receive no campaign treasury grant. The first paid unlock must be earned through recorded play or an explicitly recorded charter grant.
- Worker contracts have a sponsor, price, start time, expiry, and outcome. A later production phase will reconcile contracts against actual inventory and gold movement rather than create materials by ledger entry.
- The first live slice deploys expedition leaders with a local grind-and-wander strategy, party members with follow support, and the Wayfarer with gather-and-wander behavior. This is intentionally visible and constrained while contract rewards are not yet automated.
- Northstar starts in Northshire and Ember starts in the Valley of Trials. These are fixed level-one expedition routes, so each team begins in a valid faction zone instead of relying on an empty travel-cache result.
- Each routed expedition leader receives one nearby scout movement action, using the ordinary Playerbots movement system. The party is not teleported after that scout movement.

## PvP and territory

- A challenge names an issuer, target, battleground, squad size, and response deadline.
- An accepted challenge is only resolved from an observed battleground result.
- A declined challenge is a forfeit. It grants a reduced, short-lived boon; it never grants the full reward of a played victory.
- WSG is a 10v10 target. The initial five-person expeditions can train and grow, but they cannot claim a real WSG victory until each side fields ten eligible characters.
- A territory boon is timed and capped. It may grant modest XP or resource efficiency, never permanent power or an uncapped honor faucet.

## Safety and truthfulness

- All campaign control remains localhost-only.
- Character creation, login, grouping, and item handling use Playerbots/core APIs; the campaign scripts do not insert character or inventory rows directly.
- A database row records a league rule or campaign decision. It does not claim that a physical gathering, dungeon clear, battleground win, rescue, or item transfer happened unless telemetry proves it.
